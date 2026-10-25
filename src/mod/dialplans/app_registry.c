/* Dialplan Application 注册表 + 13 Application 骨架（REQ-DP-APP-001，P0）
 *
 * 本文件分三部分：
 *   Part A: app_registry 骨架（同 cmd_registry.h 模式）
 *   Part B: dp_rule_t.app_chain 辅助（dp_rule_set_app_chain +
 *           dp_execute_app_chain 执行器）
 *   Part C: 13 Application 函数 + 自动注册（模块 init 时一次性登记）
 *
 * P0 实装：answer（调 b2b_answer_call）、set（调 chan_var_set）、sleep（用
 * reactor 时间片释放）。其余 NOTSUP 占位，骨架已齐全、App 注册表可
 * 接线、dp_rule reload 的 JSON→C 转换与 app_chain 遍历逻辑独立可用。
 *  */
#include "app_registry.h"
#include "app_ctx.h"
#include "b2bua.h"
#include "dialplan.h"
#include "dp_sleep.h"

#include <unistd.h>
#include "ivr.h"           /* ivr_session_start / ivr_profile_find */
#include "log.h"
#include "media_bridge.h"  /* REQ-ORIGINATE-003 &echo as dialplan app: bridge_lookup_relay + rtp_relay_set_echo */
#include "playback_engine.h"
#include "timer_wheel.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ================================================================
 * Part A: app_registry 骨架
 * ================================================================*/

struct app_reg {
    app_desc_t apps[APP_MAX];
    int n;
    pthread_mutex_t lock;
    /* Phase 1 SAFE1: 安全卸载并行字段（每个 app slot 一条）*/
    volatile int refcnt[APP_MAX];
    volatile int unloading[APP_MAX];
};

int app_reg_create(app_reg_t **out)
{
    if (!out) return NSBC_INVAL;
    app_reg_t *r = (app_reg_t *)calloc(1, sizeof(*r));
    if (!r) return NSBC_NOMEM;
    pthread_mutex_init(&r->lock, NULL);
    *out = r;
    return NSBC_OK;
}

void app_reg_destroy(app_reg_t *r)
{
    if (!r) return;
    pthread_mutex_destroy(&r->lock);
    free(r);
}

static app_desc_t *app_find(app_reg_t *r, const char *name)
{
    for (int i = 0; i < r->n; ++i)
        if (strcmp(r->apps[i].name, name) == 0)
            return &r->apps[i];
    return NULL;
}

int app_register(app_reg_t *r, const app_desc_t *d)
{
    if (!r || !d || d->name[0] == '\0' || !d->fn) return NSBC_INVAL;
    pthread_mutex_lock(&r->lock);
    app_desc_t *slot = app_find(r, d->name);
    if (slot) {
        *slot = *d;
        /* Phase 1 SAFE1: 覆盖注册时重置卸载标记 —— 假设覆盖场景无并发执行中 */
        int idx = (int)(slot - r->apps);
        __atomic_store_n(&r->refcnt[idx], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&r->unloading[idx], 0, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&r->lock);
        return NSBC_OK;
    }
    if (r->n >= APP_MAX) {
        pthread_mutex_unlock(&r->lock);
        return NSBC_NOSPC;
    }
    r->apps[r->n] = *d;
    /* Phase 1 SAFE1: 首次注册初始化并行字段 */
    __atomic_store_n(&r->refcnt[r->n], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->unloading[r->n], 0, __ATOMIC_RELAXED);
    ++r->n;
    pthread_mutex_unlock(&r->lock);
    return NSBC_OK;
}

/* Phase 1 SAFE1: 前向声明 —— app_unregister 需要调用 */
int app_registry_unload_wait(app_reg_t *r, const char *name, int timeout_ms);

int app_unregister(app_reg_t *r, const char *name)
{
    if (!r || !name) return NSBC_INVAL;
    /* Phase 1 SAFE1: 复用 unload_wait 的自旋 wait 逻辑（5s 默认超时）*/
    int rc = app_registry_unload_wait(r, name, 5000);
    if (rc == NSBC_BUSY)
        return NSBC_BUSY;  /* unload 超时：仍有 in-flight 执行 */
    if (rc != NSBC_OK)
        return rc;
    return NSBC_OK;
}

int app_execute(app_reg_t *r, const char *name, app_ctx_t *c,
                const char *data, app_rc_t *rc_out)
{
    if (!r || !name || !c) return NSBC_INVAL;
    pthread_mutex_lock(&r->lock);
    int idx = -1;
    for (int i = 0; i < r->n; ++i) {
        if (strcmp(r->apps[i].name, name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        pthread_mutex_unlock(&r->lock);
        return NSBC_NOENT;
    }
    /* Phase 1 SAFE1: 检查卸载标记 —— unloading 中的 app 拒绝新调用 */
    if (__atomic_load_n(&r->unloading[idx], __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&r->lock);
        return NSBC_BUSY;
    }
    app_fn_t fn = r->apps[idx].fn;
    void *ud = r->apps[idx].ud;
    /* Phase 1 SAFE1: 锁内先递增 refcnt —— 保证卸载等待能看到这次执行 */
    __atomic_fetch_add(&r->refcnt[idx], 1, __ATOMIC_ACQ_REL);
    pthread_mutex_unlock(&r->lock);

    app_rc_t rc = fn(c, data ? data : "", ud);

    /* Phase 1 SAFE1: 执行完递减 refcnt */
    __atomic_fetch_sub(&r->refcnt[idx], 1, __ATOMIC_ACQ_REL);
    if (rc_out) *rc_out = rc;
    return NSBC_OK;
}

int app_help(app_reg_t *r, const char *prefix,
             int (*io_printf)(void *, const char *, ...), void *ud)
{
    if (!r || !io_printf) return NSBC_INVAL;
    int n = 0;
    for (int i = 0; i < r->n; ++i) {
        if (prefix && prefix[0] &&
            strncmp(r->apps[i].name, prefix, strlen(prefix)) != 0)
            continue;
        io_printf(ud, "%-24s %s\n", r->apps[i].name, r->apps[i].help);
        ++n;
    }
    return n;
}

int app_count(app_reg_t *r)
{
    return r ? r->n : 0;
}

/* Phase 1 SAFE1: app_registry_find_inc —— 卸载安全 find + refcnt++
 * 给需要长持有 app 指针的场景用（如 reload 期间保持旧表）。
 * 返回 slot 索引（-1 表示未找到或正在 unloading）；
 * 调用方用完必须调 app_registry_dec。 */
int app_registry_find_inc(app_reg_t *r, const char *name, int *out_slot)
{
    if (!r || !name || !out_slot) return -1;
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < r->n; ++i) {
        if (strcmp(r->apps[i].name, name) == 0) {
            if (__atomic_load_n(&r->unloading[i], __ATOMIC_ACQUIRE)) {
                pthread_mutex_unlock(&r->lock);
                return -1;
            }
            __atomic_fetch_add(&r->refcnt[i], 1, __ATOMIC_ACQ_REL);
            *out_slot = i;
            pthread_mutex_unlock(&r->lock);
            return i;
        }
    }
    pthread_mutex_unlock(&r->lock);
    return -1;
}

/* Phase 1 SAFE1: app_registry_dec —— 配对 find_inc 使用 */
void app_registry_dec(app_reg_t *r, int slot)
{
    if (!r || slot < 0 || slot >= APP_MAX) return;
    __atomic_fetch_sub(&r->refcnt[slot], 1, __ATOMIC_ACQ_REL);
}

/* Phase 1 SAFE1: app_registry_unload_wait —— 置 unloading=true → 自旋 wait refcnt→0 → swap 删除
 * 注意 swap 删除 apps[i] ↔ apps[n-1] 时，refcnt/unloading 也跟着一起 swap，
 * 保证 index 始终对应同一 app。 */
int app_registry_unload_wait(app_reg_t *r, const char *name, int timeout_ms)
{
    if (!r || !name) return NSBC_INVAL;

    /* 1. 持锁找 app，置 unloading */
    pthread_mutex_lock(&r->lock);
    int idx = -1;
    for (int i = 0; i < r->n; ++i) {
        if (strcmp(r->apps[i].name, name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        pthread_mutex_unlock(&r->lock);
        return NSBC_NOENT;
    }
    __atomic_store_n(&r->unloading[idx], 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&r->lock);

    /* 2. 自旋等待 refcnt→0 */
    int spins = 0;
    int deadline = timeout_ms > 0 ? timeout_ms : 5000;
    while (__atomic_load_n(&r->refcnt[idx], __ATOMIC_ACQUIRE) > 0) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (++spins >= deadline) {
            llog(LOG_ERR, "app", "unload timeout '%s' refcnt=%d — fail-safe leak",
                 name, __atomic_load_n(&r->refcnt[idx], __ATOMIC_ACQUIRE));
            return NSBC_BUSY;
        }
    }

    /* 3. 持锁重新按 name 定位（swap 可能改变 index），然后 swap 删除 */
    pthread_mutex_lock(&r->lock);
    idx = -1;
    for (int i = 0; i < r->n; ++i) {
        if (strcmp(r->apps[i].name, name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx >= 0) {
        int last = r->n - 1;
        if (idx != last) {
            r->apps[idx] = r->apps[last];
            /* Phase 1 SAFE1: swap 并行字段，保证 index→app 身份一致 */
            int tmp_refcnt = r->refcnt[idx];
            int tmp_unld   = r->unloading[idx];
            r->refcnt[idx]   = r->refcnt[last];
            r->unloading[idx] = r->unloading[last];
            r->refcnt[last]   = tmp_refcnt;
            r->unloading[last] = tmp_unld;
        }
        --r->n;
    }
    pthread_mutex_unlock(&r->lock);
    return NSBC_OK;
}

/* ================================================================
 * Part B: dp_rule_t.app_chain 辅助
 * ================================================================*/

int dp_rule_set_app_chain(dp_rule_t *rule,
                          const dp_app_action_t *chain, size_t n)
{
    if (!rule) return NSBC_INVAL;
    /* 清空旧 chain（reload 时旧表由 RCU 宽限期后统一回收，
     * 这里调用方已保证指针归属——P0 简化：不 free，依赖 RCU） */
    rule->app_chain = (struct dp_app_action *)chain;
    rule->app_chain_n = n;
    return NSBC_OK;
}

/* 执行整条 app_chain：逐 app 调用，遇到 FAILED/HANGUP/HANDOFF_SILENT
 * 立即停并返回该 rc；stopped_at 输出停在 index；全部跑完返回 OK。 */
app_rc_t dp_execute_app_chain(app_reg_t *reg, app_ctx_t *ctx,
                              const dp_rule_t *rule, size_t *stopped_at)
{
    if (!reg || !ctx || !rule || rule->app_chain_n == 0)
        return APP_RC_OK;

    app_rc_t rc = APP_RC_OK;
    for (size_t i = 0; i < rule->app_chain_n; ++i) {
        const dp_app_action_t *a = &rule->app_chain[i];
        int rc2 = app_execute(reg, a->app, ctx, a->data, &rc);
        if (rc2 != NSBC_OK) {
            rc = APP_RC_FAILED;
            if (stopped_at) *stopped_at = i;
            llog(LOG_WARN, "app", "app_chain[%zu] unknown app '%s'",
                 i, a->app);
            return rc;
        }
        if (rc == APP_RC_PENDING) {
            /* ⏳ sleep / 非阻塞挂起：
             *   a->app 是 "sleep" → a->data 是毫秒字符串 "2000"
             *   这里统一挂 timer（dp_execute_app_chain 持有 rule 指针） */
            if (strcmp(a->app, "sleep") == 0) {
                int ms = a->data[0] ? atoi(a->data) : 0;
                if (ms > 0) {
                    nsbc_ctx_t *c = nsbc_get_ctx();
                    if (c && c->tw) {
                        int s_rc = dp_sleep_add(c->tw, ctx->call_key,
                                                rule, i + 1, ms);
                        if (s_rc != NSBC_OK) {
                            llog(LOG_WARN, "dp", "sleep add failed rc=%d", s_rc);
                            /* timer 挂不上 → 退化为 OK 继续跑下一 app */
                            rc = APP_RC_OK;
                            continue;
                        }
                    } else {
                        llog(LOG_WARN, "dp", "no timer_wheel — skip sleep");
                        rc = APP_RC_OK;
                        continue;
                    }
                } else {
                    /* ms=0 → 直接跳过 sleep，继续下一 app */
                    rc = APP_RC_OK;
                    continue;
                }
            }
            if (stopped_at) *stopped_at = i + 1;
            return APP_RC_PENDING;
        }
        if (rc == APP_RC_FAILED || rc == APP_RC_HANGUP ||
            rc == APP_RC_HANDOFF_SILENT) {
            if (stopped_at) *stopped_at = i;
            return rc;
        }
    }
    return APP_RC_OK;
}

/* ================================================================
 * Part C: 13 Application 骨架
 *
 * P0 实装：answer / set / sleep
 * 其余 NOTSUP 占位，提供函数签名 + 参数白名单校验 + 执行结果
 * APP_RC_FAILED（应用未就绪，Dialplan 执行器自然走下一 prio）。
 * ================================================================*/

/* ---- 公共：校验 call_key 存活 ---- */
static int cv_call_alive(const app_ctx_t *c)
{
    if (!c || !c->call_key) return 0;
    /* call_key 是不透明 b2b_call_t*——我们调 b2b_leg_state 验证：
     * 已终止 leg 返回 LEG_TERMINATED/LEG_TERMINATING */
    leg_state_t la = b2b_leg_state((b2b_call_t *)c->call_key, 0);
    leg_state_t lb = b2b_leg_state((b2b_call_t *)c->call_key, 1);
    return !(la == LEG_TERMINATED || la == LEG_TERMINATING ||
             lb == LEG_TERMINATED || lb == LEG_TERMINATING);
}

/* ---- 1. answer（REQ-DP-APP-001，P0 实装）---- */
static app_rc_t app_answer(app_ctx_t *c, const char *data, void *ud)
{
    (void)data; (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;

    /* §8：若未 PRE_ANSWERED 也未 ANSWERED → 自动隐式 pre_answer */
    char pre_answered[16] = "";
    int pre_done = c->vars &&
                   chan_var_get(c->vars, c->leg, "pre_answered",
                                pre_answered, sizeof(pre_answered)) == NSBC_OK
                   && strcmp(pre_answered, "true") == 0;
    if (!pre_done) {
        b2b_pre_answer_call((b2b_call_t *)c->call_key);
        if (c->vars) chan_var_set(c->vars, c->leg, "pre_answered", "true");
    }

    int rc = b2b_answer_call((b2b_call_t *)c->call_key, c->leg, NULL, 0);
    if (rc == NSBC_OK) return APP_RC_OK;
    if (rc == NSBC_NOENT) return APP_RC_OK;  /* 已接通 → answer 幂等 */
    return APP_RC_FAILED;
}

/* ---- 2. bridge（REQ-DP-APP-001，P1 实装，P0 占位 NOTSUP）---- */
static app_rc_t app_bridge(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    /* data 白名单：sofia/gateway/<gw>/<ext> 或 user/<ext> 或 sip:<uri>
     * P0 只做格式校验 */
    const char *d = data ? data : "";
    size_t dl = strlen(d);
    if (dl == 0 || dl > 256) return APP_RC_FAILED;
    return APP_RC_FAILED;  /* NOTSUP → APP_RC_FAILED（执行器走下一 prio） */
}

/* ---- §6 pre_answer（Early Media 原语）----
 * 向 A 腿发 183 Session Progress（带 hold SDP），不发 200 OK。
 * data 可为空；暂不支持自定义 hold SDP。 */
static app_rc_t app_pre_answer(app_ctx_t *c, const char *data, void *ud)
{
    (void)data; (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    int rc = b2b_pre_answer_call((b2b_call_t *)c->call_key);
    if (rc == NSBC_OK) {
        /* 标记通道变量供后续 early media 动作感知 */
        if (c->vars) chan_var_set(c->vars, c->leg, "pre_answered", "true");
        return APP_RC_OK;
    }
    if (rc == NSBC_NOENT) return APP_RC_OK;  /* 已经 pre_answered → 幂等 */
    return APP_RC_FAILED;
}

/* ---- 3. playback（P1 实装，接 pb_engine）----
 * data：WAV 文件路径，支持管道符拼接 "a.wav|b.wav|c.wav"
 * leg 参数：默认向当前执行腿（通常 A 腿）放音；可指定 both。
 * loop：0=播完 EOF 自动 stop，1=循环（MoH） */
static app_rc_t app_playback(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    if (!data || data[0] == '\0' || strlen(data) > 256) return APP_RC_FAILED;

    /* 停掉可能在播的（同一个 call_key 只能有一个 pb_ctx） */
    pb_stop_if_playing(c->call_key);

    int side = c->leg;  /* 默认当前 leg */
    int loop = 0;

    /* 管道符拼接 → 拆成数组调 pb_start_chain */
    if (strchr(data, '|')) {
        char buf[512];
        strncpy(buf, data, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        const char *list[8];
        int n = 0;
        char *p = buf;
        while (p && n < 8) {
            list[n++] = p;
            p = strchr(p, '|');
            if (p) { *p = '\0'; p++; }
        }
        int rc = pb_start_chain(c->call_key, list, n, side, NULL);
        return rc == NSBC_OK ? APP_RC_OK : APP_RC_FAILED;
    }

    /* 单文件 */
    int rc = pb_start(c->call_key, data, side, loop, NULL);
    return rc == NSBC_OK ? APP_RC_OK : APP_RC_FAILED;
}

/* ---- post_playback（接通后放音，与 playback 语义相同但意图不同）----
 * 区别：pre_playback 在 pre_answer 后执行（Early Media 阶段），
 * post_playback 在 answer 后执行（正式接通阶段）。底层调用相同 pb_start，
 * 但 Dialplan 可见性区分让状态机/CDR 能区分阶段。 */
static app_rc_t app_post_playback(app_ctx_t *c, const char *data, void *ud)
{
    /* 与 playback 完全相同——pb_start 不区分阶段 */
    return app_playback(c, data, ud);
}

/* ---- endless_playback（P1 循环播放，uuid_break 可打断）---- */
static app_rc_t app_endless_playback(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    if (!data || data[0] == '\0' || strlen(data) > 256) return APP_RC_FAILED;
    pb_stop_if_playing(c->call_key);
    int rc = pb_start(c->call_key, data, c->leg, /*loop=*/1, NULL);
    return rc == NSBC_OK ? APP_RC_OK : APP_RC_FAILED;
}

/* ---- §8 uuid_break（双模：Dialplan app + ESL api）----
 * 打断当前 Leg 的 playback / sleep / 非阻塞动作。
 * 底层：pb_stop_if_playing + dp_sleep_cancel + esl_event_fire(PAYBACK_STOP)
 * 返回 APP_RC_HANGUP → Dialplan 执行器终止 app_chain。 */
static app_rc_t app_uuid_break(app_ctx_t *c, const char *data, void *ud)
{
    (void)data; (void)ud;
    if (!c || !c->call_key) return APP_RC_FAILED;

    /* 1. 停 playback */
    int pb_rc = pb_stop_if_playing(c->call_key);

    /* 2. 取消 sleep */
    int sl_rc = dp_sleep_cancel(c->call_key);

    /* 3. fire ESL 事件（供外部感知打断） */
    if (c->call_key) {
        nsbc_ctx_t *ctx = nsbc_get_ctx();
        if (ctx && ctx->esl_event_fire) {
            ctx->esl_event_fire("PLAYBACK_STOP", "{\"reason\":\"uuid_break\"}",
                                strlen("{\"reason\":\"uuid_break\"}"));
        }
    }

    llog(LOG_INFO, "app", "uuid_break: call_key=%p pb=%d sleep=%d",
         c->call_key, pb_rc, sl_rc);

    /* uuid_break 本质是"终止整条 app_chain 执行"——返回 HANGUP 语义最贴切
     * （区别于 FAILED 让执行器走下一 prio）。但需求语义是"暂停放音继续通话"，
     * 所以返回 OK 让后续动作继续跑（可再 answer / bridge 等） */
    return APP_RC_OK;
}

/* ---- §8 read（DTMF 收号，P0 占位）----
 * data 格式简化："variable=agent_id,timeout=10000,digits=3"
 * P1 实装：挂 DTMF 事件钩子 + timer_wheel timeout → APP_RC_PENDING →
 *          digit 到达回调 resume → 写 chan_var。
 * P0：返回 FAILED 让执行器跳过（DTMF 检测钩子 P1 接） */
static app_rc_t app_read(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud; (void)data;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    /* P0 占位——DTMF 收号依赖 dtmf_rx_on_pcm 钩子（媒体层 RTP 回调喂入），
     * 需要 media_bridge → dtmf_rx 桥接 + event collector 回调 resume。
     * 当前返回 FAILED 让执行器跳过，P1 实装时改 APP_RC_PENDING。 */
    llog(LOG_WARN, "app", "read NOTSUP (P0 placeholder — DTMF collector P1)");
    return APP_RC_FAILED;
}

/* ---- §13 ivr（REQ-PBX-IVR-001 P0）----
 *
 * data = "<menu_name>" —— IVR 菜单名（ivr_menus 表里的 name 字段）。
 *
 * 执行链路：
 *   1. ivr_profile_find(name, tenant="1") — 从全局 profile 注册表找菜单
 *   2. ivr_session_start(profile, entry_node, call_key, &g_ivr_ops, &sess)
 *      — 创建 session，首节点自动播 prompt 开始收号
 *   3. call->ivr_sess = sess — **关键！** media_bridge relay_recv_one 在收到
 *      telephone-event RTP 时调 ivr_on_dtmf(call->ivr_sess, digit)
 *   4. 返回 APP_RC_HANDOFF_SILENT — 挂起 dialplan app_chain，IVR 状态机
 *      接管后续所有 DTMF 交互；终态动作（TRANSFER/HANGUP）由 ivr_ops
 *      回调触发，回调内部调 b2b_transfer_call / b2b_hangup_call。 */

/* ivr_ops_t 全局回调：ivr_session_start 需要这个指针。
 * play → pb_start，transfer → b2b_transfer_call（P1 实装）。
 * 注意：ivr_ops_t 没有 hangup 字段——终态 HANGUP 动作由 media_bridge
 * relay_recv_one 消费 ivr_on_dtmf 返回的 ivr_action_t.act 来处理。 */
static int ivr_ops_play(void *ud, void *call_key,
                        const char *clip, int loop)
{
    (void)ud;
    if (!clip || !call_key) return NSBC_INVAL;
    pb_stop_if_playing(call_key);
    return pb_start(call_key, clip, 0, loop, NULL);
}
static int ivr_ops_transfer(void *ud, void *call_key, const char *target)
{
    (void)ud;
    llog(LOG_INFO, "ivr", "★ivr_ops_transfer call=%p target=%s (P1 接 b2b_transfer_call)",
         call_key, target ? target : "");
    return NSBC_OK;
}
static ivr_ops_t g_ivr_ops = {
    .play     = ivr_ops_play,
    .transfer = ivr_ops_transfer,
    .http_get = NULL,
    .ud       = NULL,
};

static app_rc_t app_ivr(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    if (!data || data[0] == '\0') {
        llog(LOG_WARN, "app", "ivr: data is empty (need menu name)");
        return APP_RC_FAILED;
    }

    /* 1. 从全局 profile 注册表找菜单（tenant 默认 "1"） */
    const char *tenant = "1";
    if (c->vars) {
        char tbuf[32] = "";
        if (chan_var_get(c->vars, c->leg, "tenant_id", tbuf, sizeof(tbuf)) == NSBC_OK && tbuf[0])
            tenant = tbuf;
    }
    const ivr_profile_t *p = ivr_profile_find(data, tenant);
    if (!p) {
        llog(LOG_WARN, "app", "ivr profile not found: name=%s tenant=%s", data, tenant);
        return APP_RC_FAILED;
    }

    /* 2. 创建 IVR session —— entry_node 硬编码默认 "main"
     * （前端 default nodes_json 首节点名 + reload JSON 默认 entry_node="main"）。
     * P1 可在 data 里加 ":entry" 后缀让调用方指定入口节点。 */
    static const char *default_entry = "main";
    ivr_sess_t *sess = NULL;
    int rc = ivr_session_start(p, default_entry, c->call_key, &g_ivr_ops, &sess);
    if (rc != NSBC_OK || !sess) {
        llog(LOG_WARN, "app", "ivr_session_start FAIL rc=%d (entry='%s')", rc, default_entry);
        return APP_RC_FAILED;
    }

    /* 3. ★ 关键：存到 call->ivr_sess —— media_bridge relay_recv_one 靠这个喂 DTMF */
    b2b_call_t *call = (b2b_call_t *)c->call_key;
    call->ivr_sess = sess;

    llog(LOG_INFO, "ivr", "★IVR START call=%p menu=%s entry=%s tenant=%s",
         call, data, default_entry, tenant);

    /* 4. 挂起 dialplan app_chain —— IVR 状态机接管，后续 DTMF 走 media_bridge */
    return APP_RC_HANDOFF_SILENT;
}

/* ---- 4. record（P1 接 recording）---- */
static app_rc_t app_record(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    /* data: /path/to/file.wav 或无参（默认 path） */
    if (!data || strlen(data) > 256) return APP_RC_FAILED;
    return APP_RC_FAILED;
}

/* ---- 5. transfer（P1 接 b2b_transfer_call）---- */
static app_rc_t app_transfer(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    /* data: <dest> [XML default] */
    if (!data || data[0] == '\0' || strlen(data) > 256) return APP_RC_FAILED;
    return APP_RC_FAILED;
}

/* ---- 6. set（REQ-DP-APP-001 + REQ-CHAN-VAR-001，P0 实装）----
 *
 * data 格式："var_name=value"（首个 '=' 切分；值可含 '=' 本身）；
 * 支持 export 变体："var_name" 不带值 → set 空串（或 unset 语义按约定）。
 * ${var} 替换先做（chan_var_substitute 已支持嵌套）。 */
static app_rc_t app_set(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!cv_call_alive(c) || !c->vars) return APP_RC_FAILED;
    const char *d = data ? data : "";
    const char *eq = strchr(d, '=');
    char *key;
    const char *val;
    if (eq) {
        size_t klen = (size_t)(eq - d);
        if (klen == 0) return APP_RC_FAILED;
        key = (char *)alloca(klen + 1);
        memcpy(key, d, klen);
        key[klen] = '\0';
        val = eq + 1;
    } else {
        /* 纯 "var_name" → set 空串 */
        if (d[0] == '\0') return APP_RC_FAILED;
        key = (char *)alloca(strlen(d) + 1);
        strncpy(key, d, strlen(d)); key[strlen(d)] = '\0';
        val = "";
    }
    /* ${} 替换（值里的变量） */
    char vbuf[512];
    int rc = chan_var_substitute(c->vars, c->leg, val, vbuf, sizeof(vbuf));
    const char *final_val = (rc == NSBC_OK) ? vbuf : val;
    int s = chan_var_set(c->vars, c->leg, key, final_val);
    if (s == NSBC_OK) return APP_RC_OK;
    if (s == NSBC_PERM_DENIED) return APP_RC_OK;  /* nsbc_* 只读 → 静默忽略 */
    return APP_RC_FAILED;
}

/* ---- 7. conference（P1 接 mixer）---- */
static app_rc_t app_conference(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    /* data: <room>@<profile> */
    if (!data || strlen(data) > 128) return APP_RC_FAILED;
    return APP_RC_FAILED;
}

/* ---- 8. play_and_get_digits（P1 接 IVR + DigitMap）---- */
static app_rc_t app_play_and_get_digits(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    if (!data || strlen(data) > 256) return APP_RC_FAILED;
    return APP_RC_FAILED;
}

/* ---- 9. sleep（REQ-DP-APP-001，P0 实装：时间片释放 + 定时器回调）----
 *
 * Dialplan 执行线程不能被阻塞（reactor 单线程循环）；sleep 语义 =
 * 挂起当前 rule 执行、登记 timer_wheel 条目，到时回调继续。
 * P0 简化：我们无法在同步返回中释放线程（返回 APP_RC_HANDOFF_SILENT
 * 让执行器停掉；定时器回调是 P1 框架接线）——暂按 NOTSUP 走
 * APP_RC_FAILED。
 * 实际 sleep 要真正实现需扩展 dp_execute 为 async state machine。 */
/* ---- §7 sleep（非阻塞！timer_wheel 异步定时）----
 * 严禁系统 sleep。返回 APP_RC_PENDING → dp_execute_app_chain 挂 timer →
 * 到期回调 dp_sleep_resume_cb 从 resume_idx 继续。 */
static app_rc_t app_sleep(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    const char *d = data ? data : "0";
    int ms = atoi(d);
    if (ms <= 0 || ms > 600000) return APP_RC_FAILED;  /* 10min 封顶 */
    /* timer 挂载由 dp_execute_app_chain 统一做——这里只返回 PENDING */
    llog(LOG_INFO, "app", "sleep pending: call_key=%p ms=%d", c->call_key, ms);
    return APP_RC_PENDING;
}

/* ---- 10. park（P1 接 ccf_park + 媒体 MoH）---- */
static app_rc_t app_park(app_ctx_t *c, const char *data, void *ud)
{
    (void)data; (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    /* park 后通话挂起、进入 hold 状态（REQ-SDP-002 inactive）、
     * 播放等待音（player）、分配 orbit 号 uuid_pickup 可取回。
     * P0 占位：返回 HANDOFF_SILENT 让执行器停（同 P1 park 停止语义），
     * 实际 hold + MoH 在 P1 接 media bridge。 */
    return APP_RC_HANDOFF_SILENT;
}

/* ---- 11. att_xfer（P1 接 ccf_transfer CONSULTED）---- */
static app_rc_t app_att_xfer(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    if (!data || strlen(data) > 256) return APP_RC_FAILED;
    return APP_RC_FAILED;
}

/* ---- 12. gentones（P2）---- */
static app_rc_t app_gentones(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;
    if (!data || strlen(data) > 256) return APP_RC_FAILED;
    return APP_RC_FAILED;
}

/* ---- 13. echo（REQ-ORIGINATE-003 作为 dialplan app）----
 *
 * 把 media bridge 的 relay 切到 RTP 环回模式：
 * relay_recv_one 收到 side=N 的包不再转发到 side=other，
 * 而是发回 side=N 自己（两侧各自 side→side），
 * 让通话方听到自己的回声。
 *
 * 调用时机：dialplan 在 A 腿 answer 后执行 app_chain 时。
 * 要求：两侧 leg 都是 CONFIRMED（relay 已建好）。
 *        单侧 inbound 还没 answer 时执行 → relay 不存在 → 返回 FAILED。
 * data 参数：可空；传入时长秒（如 "30"）会在 N 秒后自动关闭 echo 恢复正常转发。
 *           当前实现忽略 data（全程 echo 直到通话挂断），P1 实装 auto-timeout。 */
static app_rc_t app_echo(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!cv_call_alive(c)) return APP_RC_FAILED;

    nsbc_ctx_t *ctx = nsbc_get_ctx();
    if (!ctx || !ctx->bridge) {
        llog(LOG_WARN, "app", "echo: no bridge_mgr (media bridge not loaded)");
        return APP_RC_FAILED;
    }

    bridge_mgr_t *m = (bridge_mgr_t *)ctx->bridge;
    rtp_relay_t *relay = NULL;
    nsbc_sa_t peer_unused[2];
    /* REQ-ORIGINATE-003 race 修复：inbound 来电 answer 后 WebRTC DTLS
     * async build relay，加重试循环（每 50ms，最多 2s）。 */
    int lrc = NSBC_NOENT;
    for (int retry = 0; retry < 40; retry++) {
        lrc = bridge_lookup_relay(m, c->call_key, &relay, peer_unused);
        if (lrc == NSBC_OK && relay) break;
        usleep(50000);
    }
    if (lrc != NSBC_OK || !relay) {
        llog(LOG_WARN, "app",
             "echo: relay lookup rc=%d (not yet built or bypass mode) call_key=%p",
             lrc, c->call_key);
        return APP_RC_FAILED;
    }

    rtp_relay_set_echo(relay, 1);
    llog(LOG_INFO, "app", "echo: RTP loopback ON call_key=%p data=%s",
         c->call_key, data ? data : "(no auto-timeout)");
    return APP_RC_OK;
}

/* ================================================================
 * 模块 init：自动注册内建 Application
 *
 * 注册点在 Dialplan 模块 init（mod/dialplans/dialplan.c 或新
 * 模块）调用 app_builtin_register(reg)。本函数不暴露，内部
 * 仅在本文件。
 * ================================================================*/

static const app_desc_t builtin_apps[] = {
    { "answer",             "应答来电（发送 200 OK）",                          app_answer,             NULL },
    { "pre_answer",         "Early Media 预应答（发 183，不发 200 OK）",         app_pre_answer,         NULL },
    { "bridge",             "桥接当前通话到另一端点 sofia/gateway/<gw>/<ext>", app_bridge,            NULL },
    { "playback",           "播放单个音频文件（支持 a.wav|b.wav 管道符拼接）", app_playback,          NULL },
    { "post_playback",      "接通后放音（语义同 playback，阶段区分用）",        app_post_playback,     NULL },
    { "record",             "录制当前通话音频",                                 app_record,            NULL },
    { "transfer",           "转移当前通话（REFER 或 302）",                     app_transfer,          NULL },
    { "set",                "设置通道变量 var_name=value（支持 ${} 替换）",     app_set,               NULL },
    { "conference",         "将通话加入会议 <room>@<profile>",                  app_conference,        NULL },
    { "play_and_get_digits","播放提示音并收集 DTMF",                            app_play_and_get_digits,NULL },
    { "sleep",              "暂停 Dialplan 执行（毫秒）",                       app_sleep,             NULL },
    { "park",               "保持当前通话等待取回（hold + MoH）",               app_park,              NULL },
    { "att_xfer",           "出席/咨询转移",                                    app_att_xfer,          NULL },
    { "gentones",           "生成特定音调序列",                                 app_gentones,          NULL },
    { "endless_playback",   "循环播放音频（uuid_break 可中断）",                app_endless_playback,  NULL },
    { "uuid_break",         "打断当前 call 的 playback/sleep（双模：DP app + ESL api）", app_uuid_break, NULL },
    { "read",               "DTMF 收号（P0 占位，P1 实装）",                     app_read,              NULL },
    { "ivr",                "启动 IVR 菜单 session；data=<menu_name>",          app_ivr,               NULL },
    { "echo",               "RTP 环回（两侧各自 self→self，听自己回声）",        app_echo,              NULL },
};

/* 对外暴露：Dialplan 模块 init 时调此函数一次性注册内建 apps。 */
void app_builtin_register(app_reg_t *reg)
{
    if (!reg) return;
    for (size_t i = 0; i < sizeof(builtin_apps)/sizeof(builtin_apps[0]); ++i) {
        int rc = app_register(reg, &builtin_apps[i]);
        if (rc != NSBC_OK)
            llog(LOG_WARN, "app", "builtin app '%s' register rc=%d",
                 builtin_apps[i].name, rc);
    }
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
