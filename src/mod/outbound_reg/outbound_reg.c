/* NSBC → 外部 IMS / SIP 中继 出局注册（REQ-SIP-OUT-001，P1）
 *
 * 状态机（per-trunk）：
 *
 *             ┌────────── init 扫描 TRUNK_REGISTER ─────────┐
 *             v                                              │
 *         IDLE ────── send_register() ──► REGISTERING        │
 *             ▲                                │             │
 *             │               200 OK           │ 401/407     │ 403/6xx/超时
 *             │                                ▼             │
 *             │                          CHALLENGED ──► REGISTERING (带 Authorization)
 *             │                                │             │
 *             │  Expires 到期续注 / 指数退避     │ 200 OK      │
 *             │   (next_send_ms)               ▼             │
 *             │                          REGISTERED          │
 *             │                                │             │
 *             └────────────────────────── FAILED ◄────────────┘
 *                                            │
 *                                   retry_backoff 翻倍 → RETRY_WAIT
 *                                            │
 *                                   next_send_ms 到期 → REGISTERING
 *
 * 定时器：tw 每 trunk 一个 timer；state=REGISTERED 时 Expires-60s；
 *        state=FAILED/RETRY_WAIT 时 retry_backoff_ms（首次 5s，翻倍到 30min 封顶）。
 *
 * Digest 鉴权：收到 401/407 解析 WWW-Authenticate / Proxy-Authenticate → HA1 取
 *        自 trunk_t.ha1_cb；nonce/realm/qop 组装 Authorization。 */
#include "outbound_reg.h"

#include "cfg.h"
#include "nsbc_id.h"     /* nsbc_rand_token */
#include "route_table.h"
#include "sip_build.h"
#include "sip_profile.h" /* sip_profile_find —— 前置校验 profile 存在性（专家 2.6） */
#include "sip_transport.h"
#include "sip_txn.h"
#include "timer_wheel.h"
#include "sip_digest.h"
#include "metric_reg.h"   /* P1-9 Prometheus：metric_counter_inc / metric_gauge_set */
#include "uDS_client.h"   /* P1-10：uds_send_register */
#include "app_ctx.h"
#include "module.h"
#include "b2bua.h"   /* Phase 0: b2b_resp_hook_add / b2b_resp_hook_rm */
#include "log.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <cjson/cJSON.h>

#define OR_MAX_TRUNKS    64
#define OR_DEFAULT_EXPIRES   3600
#define OR_INITIAL_BACKOFF_MS  5000
#define OR_MAX_BACKOFF_MS  (30 * 60 * 1000)
#define OR_CONTACT_PORT    5060

/* ---------- 静态状态 ---------- */

static struct nsbc_ctx *g_ctx = NULL;
static sip_transport_t *g_tr = NULL;  /* 从 outreg_bind_transport 绑定 */
static char g_local_host[128];         /* signaling 传入的 auto-detect IP */
static route_table_t   *g_self_rt = NULL; /* outreg 自己创建的 route_table（ctx->rt 自举兜底） */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static outreg_trunk_t g_trunks[OR_MAX_TRUNKS];
static size_t         g_trunk_cnt = 0;
static outreg_stat_t  g_stat = {0};

/* ---- static forward declarations（GCC 4.8 -Werror 要求先声明后调用） ---- */
static void disarm_ping_timer(outreg_trunk_t *t);
static void push_registration_to_web(outreg_trunk_t *t, const trunk_t *tr);
static void set_health(outreg_trunk_t *t, uint8_t new_state);
static int  arm_timer(outreg_trunk_t *t, uint32_t expire_ms);
static int  disarm_timer(outreg_trunk_t *t);

/* 由 signaling sig_start 末尾调用（level 4，transport 已就绪）。
 * 绑定后立即触发所有 trunk 的首注 REGISTER（init 时 g_tr=NULL 无法发送）。 */
void outreg_bind_transport(sip_transport_t *tr, const char *local_host)
{
    g_tr = tr;
    g_local_host[0] = '\0';
    if (local_host && local_host[0]) {
        snprintf(g_local_host, sizeof(g_local_host), "%s", local_host);
    }
    log_info("outreg", "transport bound local_host=%s tr=%p",
             g_local_host[0] ? g_local_host : "(null)", (void*)tr);

    /* 对每个 trunk 首注（init 时因 g_tr=NULL 没发） */
    for (size_t i = 0; i < g_trunk_cnt; ++i) {
        log_info("outreg", "bind → trigger first register trunk=%s",
                 g_trunks[i].name);
        outreg_send_register(g_trunks[i].name, 0);
    }
    pthread_mutex_lock(&g_lock);
    g_stat.active = (uint32_t)g_trunk_cnt;
    pthread_mutex_unlock(&g_lock);
}

/* ---------- SIP txn client TU 回调 ---------- */
/* 每次 outreg_send_register / outreg_unregister_all 都创建独立 NICT 事务；
 * TU 回调把响应（2xx/3xx/401/407/超时）胶水到 outreg_on_register_response。 */
static void outreg_tu_cb(sip_txn_t *txn, const sip_msg_t *m, txn_event_t ev, void *ud)
{
    (void)txn;
    outreg_trunk_t *t = (outreg_trunk_t *)ud;
    if (t == NULL) return;

    if (ev == TXN_EV_TIMEOUT) {
        log_warn("outreg", "REGISTER txn timeout trunk=%s", t->name);
        outreg_on_register_response(t->call_id, 0, NULL);
        return;
    }

    if (ev == TXN_EV_TRANSPORT_ERR) {
        log_warn("outreg", "REGISTER txn transport_err trunk=%s", t->name);
        outreg_on_register_response(t->call_id, -1, NULL);
        return;
    }

    if (ev != TXN_EV_FINAL || m == NULL) return;

    int status = m->status;
    log_info("outreg", "REGISTER txn final trunk=%s status=%d", t->name, status);

    if (status == 401 || status == 407) {
        /* 把 WWW-Authenticate / Proxy-Authenticate 原始值传给 outreg_on_register_response
         * —— 它内部已经用 digest_parse_challenge 解析。 */
        const char *auth_raw = NULL;
        sip_hdr_t *h = sip_hdr_get(m, SIP_H_WWW_AUTHENTICATE);
        if (h == NULL) h = sip_hdr_get(m, SIP_H_PROXY_AUTHENTICATE);
        if (h && h->value.p && h->value.len > 0) {
            auth_raw = strndup(h->value.p, h->value.len);
        }
        outreg_on_register_response(t->call_id, status, auth_raw);
        if (auth_raw) free((void *)auth_raw);
    } else {
        outreg_on_register_response(t->call_id, status, NULL);
    }
}

/* ---------- helpers ---------- */

/* 从 cfg_retry_sec 计算初始退避时间（文档 §3.4：FS retry-seconds 是固定间隔，NSBC 做指数退避增强）
 *   cfg_retry_sec > 0 → cfg_retry_sec × 1000
 *   cfg_retry_sec == 0 → 直接封顶 OR_MAX_BACKOFF_MS（30min，相当于不重试） */
static uint32_t cfg_retry_initial_ms(const outreg_trunk_t *t)
{
    if (t->cfg_retry_sec == 0) return OR_MAX_BACKOFF_MS;
    uint32_t ms = t->cfg_retry_sec * 1000;
    return ms > OR_MAX_BACKOFF_MS ? OR_MAX_BACKOFF_MS : ms;
}

static uint32_t __attribute__((unused)) now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ctx 没有 local_host 字段 —— 从 cfg 读 sip.extern_ip，fallback "0.0.0.0" */
static const char *get_local_host(nsbc_ctx_t *ctx) {
    (void)ctx;
    return g_local_host[0] ? g_local_host : "0.0.0.0";
}

static void __attribute__((unused)) state_name_stub(void) { (void)0; }

static outreg_trunk_t *find_trunk(const char *name)
{
    pthread_mutex_lock(&g_lock);
    outreg_trunk_t *out = NULL;
    for (size_t i = 0; i < g_trunk_cnt; ++i) {
        if (strcmp(g_trunks[i].name, name) == 0) { out = &g_trunks[i]; break; }
    }
    pthread_mutex_unlock(&g_lock);
    return out;
}

/* ---------- 文件内 forward declarations（GCC 4.8 不允许隐式声明） ---------- */
static int  disarm_timer(outreg_trunk_t *t);
static void gen_call_id(char *out, size_t outsz, const char *local_host);

/* ---------- 号码池解析 helper（P0，用户新增）---------- */
static void parse_caller_pool(const char *pool_str, char pool_out[][64],
                              uint32_t pool_cap, uint32_t *pool_count)
{
    *pool_count = 0;
    if (!pool_str || !pool_str[0]) return;
    char *dup = strdup(pool_str);
    if (!dup) return;
    for (char *tok = dup; tok && *pool_count < pool_cap; ) {
        char *comma = strchr(tok, ',');
        if (comma) *comma = '\0';
        while (*tok == ' ' || *tok == '\t') ++tok;
        if (*tok && strlen(tok) < 64) {
            snprintf(pool_out[*pool_count], 64, "%s", tok);
            (*pool_count)++;
        }
        tok = comma ? comma + 1 : NULL;
    }
    free(dup);
}

/* ---------- 从 trunk_t 构建/刷新 outreg_trunk_t 的 cfg_* 字段 ----------
 * 新 trunk：复制 auth_user/realm → 根据 password 计算 ha1 → 清空运行时字段 → 解析号码池 → 设 OR_IDLE
 * 已有 trunk（reload 增量更新）：只更新 cfg_* 字段 → 密码变了重算 ha1 → 保留 state/call_id/cseq/challenge */
static void apply_trunk_config(outreg_trunk_t *t, const trunk_t *tr, int is_new)
{
    /* 先保存旧值，判断 password 是否变化（用于 ha1 重算） */
    char old_ha1[33] = {0};
    int old_ha1_set = t->ha1_set;
    if (old_ha1_set) memcpy(old_ha1, t->ha1, 32);

    /* 拷贝 auth_user / realm（如果变了也可能影响 ha1） */
    if (tr->auth_user[0]) snprintf(t->auth_user, sizeof(t->auth_user), "%s", tr->auth_user);
    if (tr->realm[0])     snprintf(t->realm,     sizeof(t->realm),     "%s",     tr->realm);

    /* FreeSWITCH 兼容 cfg_ 字段 */
    t->cfg_expire_sec = tr->expire_seconds ? tr->expire_seconds : OR_DEFAULT_EXPIRES;
    t->cfg_retry_sec  = tr->retry_seconds;  /* 0=不重试，FS 语义 */
    t->cfg_ping_sec   = tr->ping_seconds;
    if (tr->from_user[0])         snprintf(t->cfg_from_user, sizeof(t->cfg_from_user), "%s", tr->from_user);
    if (tr->from_domain[0])       snprintf(t->cfg_from_domain, sizeof(t->cfg_from_domain), "%s", tr->from_domain);
    if (tr->proxy[0])             snprintf(t->cfg_proxy, sizeof(t->cfg_proxy), "%s", tr->proxy);
    if (tr->register_proxy[0])    snprintf(t->cfg_register_proxy, sizeof(t->cfg_register_proxy), "%s", tr->register_proxy);
    if (tr->contact_params[0])    snprintf(t->cfg_contact_params, sizeof(t->cfg_contact_params), "%s", tr->contact_params);
    t->cfg_caller_id_in_from  = tr->caller_id_in_from;
    t->cfg_extension_in_contact = tr->extension_in_contact;
    if (tr->inbound_extension[0]) snprintf(t->cfg_inbound_ext, sizeof(t->cfg_inbound_ext), "%s", tr->inbound_extension);
    t->cfg_rfc5626 = tr->rfc5626_enabled;
    if (tr->reg_id[0])            snprintf(t->cfg_reg_id, sizeof(t->cfg_reg_id), "%s", tr->reg_id);
    if (tr->peer_ip[0])           snprintf(t->cfg_peer_ip, sizeof(t->cfg_peer_ip), "%s", tr->peer_ip);
    /* 用户新增：profile_name、号码池、per_number_max */
    if (tr->caller_id_pool[0])    snprintf(t->cfg_caller_pool, sizeof(t->cfg_caller_pool), "%s", tr->caller_id_pool);
    t->cfg_per_num_max = tr->per_number_max_channels;

    /* ha1 计算：从 trunk_t.password 重算（Web reload 明文传入，安全走 UDS） */
    if (tr->password[0]) {
        digest_make_ha1(t->auth_user, t->realm, tr->password, t->ha1);
        t->ha1_set = 1;
    } else {
        t->ha1_set = 0;
        memset(t->ha1, 0, sizeof(t->ha1));
    }
    /* 专家 2.6：ha1_set=0 打 warn 日志（password 未配置 —— 401/407 后会 FAILED，但仍发 REGISTER 等运营商挑战） */
    if (!t->ha1_set) {
        log_warn("outreg", "trunk=%s no password configured (ha1_set=0), 401/407 challenge will fail",
                 tr->name);
    }

    /* profile_name 绑定（专家 2.7：profile 校验前置依赖这个字段） */
    if (tr->profile_name[0]) snprintf(t->profile_name, sizeof(t->profile_name), "%s", tr->profile_name);

    /* 号码池解析（每次 reload 都重新解析，per_num_active 清零；在呼的呼叫持有旧指针不受影响） */
    memset(t->pool_caller, 0, sizeof(t->pool_caller));
    memset(t->per_num_active, 0, sizeof(t->per_num_active));
    t->total_active = 0;
    parse_caller_pool(tr->caller_id_pool, t->pool_caller, GW_POOL_MAX, &t->pool_count);
    if (tr->caller_id_pool[0] && t->pool_count >= GW_POOL_MAX)
        log_warn("outreg", "trunk=%s caller_id_pool exceeded GW_POOL_MAX=%d, truncated",
                 tr->name, GW_POOL_MAX);

    /* 新 trunk：清空运行时字段，等 outreg_reload_from_route_table 外层设置 state=OR_IDLE + 发首注 */
    if (is_new) {
        t->cseq = 0;
        t->expires = 0;
        t->retry_backoff_ms = 0;
        t->next_send_ms = 0;
        t->state = OR_IDLE;
        t->unregistering = 0;
        t->nc_counter = 0;
        t->auth_hdr[0] = '\0';
        t->ch_realm[0] = '\0';
        t->ch_nonce[0] = '\0';
        t->ch_opaque[0] = '\0';
        t->ch_qop[0] = '\0';
        t->health_state = GW_HEALTH_HEALTHY;
        t->ping_fail_count = 0;
        t->last_ping_ms = 0;
    } else {
        /* 重载已有 trunk：密码变了记日志 */
        if ((old_ha1_set && t->ha1_set && memcmp(old_ha1, t->ha1, 32) != 0)
            || (old_ha1_set != t->ha1_set)) {
            log_info("outreg", "trunk=%s credentials changed (ha1 old=%d new=%d) → will use new auth on next REGISTER",
                     t->name, old_ha1_set, t->ha1_set);
            /* clear 两套 challenge cache，401/407 后重新取新 nonce */
            t->ch_realm[0] = '\0';
            t->ch_nonce[0] = '\0';
            t->ch_opaque[0] = '\0';
            t->ch_qop[0] = '\0';
            t->nc_counter = 0;
            t->p_realm[0] = '\0';
            t->p_nonce[0] = '\0';
            t->p_opaque[0] = '\0';
            t->p_qop[0] = '\0';
            t->p_nc_counter = 0;
        }
    }
}

/* ---------- 外部入口：Web reload 后刷新 outreg_trunk_t[]（文档 §3.1）----------
 * list=NULL 或 count=0 表示"新表没有任何 register trunk → 全部清理"
 *
 * 设计说明：
 *   - 全程持有 g_lock 做增量 diff + 数组 compact（锁内只操作 g_trunks[] 内存）
 *   - 新 trunk 的首注 REGISTER 在锁外统一触发（避免死锁 send_register → find_trunk）
 *   - 删除 trunk：**不发 Expires=0 unreg**——因为 trunk_table_reload_json 已经原子换了
 *     route_table，被删 trunk 的 host/port 我们拿不到了（rt_trunk_get 查不到）。
 *     对端注册自然过期（默认 cfg_expire_sec，最长 3600s）就是安全清除。
 *     旧 REGISTER 响应回来时 trunk 已从数组移除 → find_trunk 返回 NULL → on_register_response
 *     里"trunk not found, stale response"日志丢弃（安全兜底，文档专家 2.6 预期行为）。 */
int outreg_reload_from_route_table(const trunk_t *list, size_t count)
{
    pthread_mutex_lock(&g_lock);

    /* 1. 构造 old_set：used[i] 标记该旧 trunk 是否在新 list 中存在 */
    size_t old_cnt = g_trunk_cnt;
    uint8_t used[OR_MAX_TRUNKS];
    memset(used, 0, sizeof(used));

    /* 2. 遍历新 list：同名→更新 cfg_*；新→创建 outreg_trunk_t 并校验 */
    for (size_t i = 0; i < count && g_trunk_cnt < OR_MAX_TRUNKS; ++i) {
        const trunk_t *tr = &list[i];
        if (!tr || !tr->name[0] || tr->kind != TRUNK_REGISTER) continue;

        /* 查找是否已存在 */
        outreg_trunk_t *existing = NULL;
        size_t old_idx = (size_t)-1;
        for (size_t j = 0; j < old_cnt; ++j) {
            if (!used[j] && strcmp(g_trunks[j].name, tr->name) == 0) {
                existing = &g_trunks[j];
                old_idx = j;
                break;
            }
        }

        if (existing) {
            used[old_idx] = 1;
            if (!tr->enabled) {
                /* 专家 2.3：enable=0 → OR_DISABLED + disarm + compact 删除
                 * 原 state==OR_REGISTERED 时设 unregistering=1 → 锁外发 Expires=0 注销 */
                log_info("outreg", "reload trunk=%s disabled → OR_DISABLED (will be removed)", tr->name);
                if (existing->state == OR_REGISTERED || existing->state == OR_RETRY_WAIT
                    || existing->state == OR_CHALLENGED) {
                    existing->unregistering = 1;
                    log_info("outreg", "reload trunk=%s disabled → schedule Expires=0 unregister", tr->name);
                }
                disarm_timer(existing);
                disarm_ping_timer(existing);  /* P1-9：OR_DISABLED 停止健康探测 */
                existing->state = OR_DISABLED;
                apply_trunk_config(existing, tr, /*is_new=*/0);
                /* P1-10 UDS push：OR_DISABLED → Web registrations UPSERT（expires=0） */
                push_registration_to_web(existing, tr);
            } else {
                /* enabled=1 → 更新 cfg_*，保留状态机
                 * 但清零失败诊断计数——Web reload 通常意味着用户已修正配置，
                 * 让新配置从干净状态开始重试 */
                apply_trunk_config(existing, tr, /*is_new=*/0);
                existing->reg_fail_count = 0;
                existing->first_fail_ms  = 0;
                if (existing->state == OR_DISABLED || existing->state == OR_INVALID) {
                    existing->state = OR_IDLE;
                    existing->retry_backoff_ms = 0;
                    existing->expires = existing->cfg_expire_sec;
                }
            }
        } else {
            /* 全新 trunk → 创建 outreg_trunk_t → apply_trunk_config → 前置校验 → 标记 OR_IDLE/OR_INVALID */
            outreg_trunk_t *t = &g_trunks[g_trunk_cnt++];
            memset(t, 0, sizeof(*t));
            snprintf(t->name, sizeof(t->name), "%s", tr->name);
            gen_call_id(t->call_id, sizeof(t->call_id),
                        g_local_host[0] ? g_local_host : "localhost");
            apply_trunk_config(t, tr, /*is_new=*/1);

            /* === 专家 2.6 / 2.7：前置校验 === */
            int failed = 0;
            if (g_tr == NULL) {
                log_warn("outreg", "trunk=%s transport not bound → OR_INVALID (wait for outreg_bind_transport)",
                         tr->name);
                t->state = OR_INVALID; failed = 1;
            }
            if (t->auth_user[0] == 0) {
                log_warn("outreg", "trunk=%s no auth_user → OR_INVALID", tr->name);
                t->state = OR_INVALID; failed = 1;
            }
            if (tr->profile_name[0]) {
                sip_profile_t *prof = sip_profile_find(tr->profile_name);
                if (prof == NULL) {
                    log_warn("outreg", "trunk=%s profile=%s not found → OR_INVALID (reload sequence: sip_profiles first)",
                             tr->name, tr->profile_name);
                    t->state = OR_INVALID; failed = 1;
                } else {
                    log_info("outreg", "trunk=%s profile=%s bound OK", tr->name, tr->profile_name);
                }
            }

            if (!failed) {
                t->state = OR_IDLE;
                t->expires = t->cfg_expire_sec;
                log_info("outreg", "reload added trunk=%s callid=%s cfg_expire=%u cfg_retry=%u ha1_set=%d profile=%s pool_count=%u",
                         t->name, t->call_id, t->cfg_expire_sec, t->cfg_retry_sec,
                         t->ha1_set,
                         t->profile_name[0] ? t->profile_name : "(none)",
                         t->pool_count);
            } else {
                log_info("outreg", "reload trunk=%s OR_INVALID (validation failed)", tr->name);
                /* P1-10 UDS push：OR_INVALID → Web registrations UPSERT（expires=0） */
                push_registration_to_web(t, tr);
            }
        }
    }

    /* 3. 删除分支：compact —— used[i]==0 的 trunk 从 g_trunks 里物理移除
     *    不发 unreg（route_table 已换表拿不到 host/port），对端注册自然过期。
     *    disarm_timer 确保不再发续注。 */
    size_t dst = 0;
    for (size_t src = 0; src < g_trunk_cnt; ++src) {
        int drop = 0;
        if (src >= old_cnt) {
            /* src 在 old_cnt..g_trunk_cnt-1 范围：新创建的 trunk，只有 OR_INVALID 才 drop
             * （已在上一轮被标记为 OR_INVALID，不发 REGISTER）——保留在数组里，
             * 让下次 reload 时 cfg 变更可能自动变为有效。不 drop。 */
        } else {
            /* src 在旧范围内：used[src]==0 表示新 list 里没有这个 trunk */
            if (!used[src]) {
                outreg_trunk_t *t = &g_trunks[src];
                log_info("outreg", "reload removed trunk=%s → disarm, let REGISTER expire naturally (cfg_expire=%us)",
                         t->name, t->cfg_expire_sec);
                disarm_timer(t);
                t->state = OR_IDLE; /* 等过期 */
                /* P1-10 UDS push：删除 trunk → Web registrations UPSERT（expires=0 立即过期）
                 * 注意：此时 route_table 已换表，rt_trunk_get 查不到 → tr=NULL 走 fallback */
                push_registration_to_web(t, NULL);
                drop = 1;
            }
        }
        if (drop) continue; /* 跳过，不搬到 dst */
        if (dst != src) g_trunks[dst] = g_trunks[src];
        dst++;
    }
    g_trunk_cnt = dst;

    /* 4. g_stat 同步 */
    g_stat.total = (uint32_t)g_trunk_cnt;
    g_stat.active = g_stat.total;

    pthread_mutex_unlock(&g_lock);

    /* 5. 锁外统一触发首注 OR Expires=0 注销（避免死锁）：
     *    - OR_IDLE → 首注 REGISTER（新 trunk 或 OR_DISABLED→enabled=1 恢复）
     *    - OR_DISABLED + unregistering=1 → Expires=0 REGISTER（enabled=0 注销）
     *    OR_INVALID 不触发任何动作。 */
    for (size_t i = 0; i < g_trunk_cnt; ++i) {
        outreg_trunk_t *t = &g_trunks[i];
        if (t->state == OR_IDLE && g_tr != NULL) {
            log_info("outreg", "reload → send first REGISTER trunk=%s", t->name);
            outreg_send_register(t->name, 0);
        } else if (t->state == OR_DISABLED && t->unregistering) {
            log_info("outreg", "reload → send Expires=0 unregister trunk=%s", t->name);
            outreg_send_register(t->name, 0); /* send_register guard 允许 OR_DISABLED + unregistering */
        }
    }

    log_info("outreg", "reload done: %zu trunks active", g_trunk_cnt);
    return NSBC_OK;
}

/* ---------- FSM 事件处理 ---------- */

/* 三个失败分支（403/5xx/6xx/超时、challenge 解析失败、ha1 缺失）的统一入口：
 *   disarm → 指数退避 → arm retry timer → 打诊断日志 → push Web
 * 内部自己持/释 g_lock；调用方在锁外调用（避免重入死锁）。
 * reason 传入一个简短字符串用于日志（"status=403" "challenge_parse_fail" "no_credentials" 等）。 */
static void enter_retry_wait(outreg_trunk_t *t, int status, const char *reason)
{
    if (t == NULL) return;

    struct timespec _ts;
    clock_gettime(CLOCK_MONOTONIC, &_ts);
    uint64_t now_ms = (uint64_t)_ts.tv_sec * 1000 + (uint64_t)_ts.tv_nsec / 1000000;

    pthread_mutex_lock(&g_lock);
    disarm_timer(t);
    disarm_ping_timer(t);
    t->state = OR_FAILED;

    /* 失败计数 + 首次失败时间戳 */
    if (t->first_fail_ms == 0) t->first_fail_ms = now_ms;
    t->reg_fail_count += 1;

    /* 指数退避：初始值优先 cfg_retry_sec × 1000（兜底 OR_INITIAL_BACKOFF_MS），
     *          翻倍直到 OR_MAX_BACKOFF_MS（30min）封顶；
     * 地板 OR_INITIAL_BACKOFF_MS 防止 cfg_retry_sec=1/2/3 太小导致疯狂重试；
     * cfg_retry_sec==0 时 cfg_retry_initial_ms 返回 OR_MAX_BACKOFF_MS 表示"基本不重试" */
    uint32_t base;
    if (t->retry_backoff_ms == 0) {
        base = cfg_retry_initial_ms(t);
        if (base < OR_INITIAL_BACKOFF_MS) base = OR_INITIAL_BACKOFF_MS;
    } else {
        base = t->retry_backoff_ms * 2;
    }
    if (base > OR_MAX_BACKOFF_MS) base = OR_MAX_BACKOFF_MS;
    t->retry_backoff_ms = base;
    t->state = OR_RETRY_WAIT;

    /* P1-10 UDS push：FAILED → Web registrations UPSERT（expires=0 立即过期） */
    {
        const trunk_t *tr = g_ctx && g_ctx->rt
            ? rt_trunk_get((route_table_t *)g_ctx->rt, t->name) : NULL;
        push_registration_to_web(t, tr);
    }
    arm_timer(t, t->retry_backoff_ms);

    uint64_t since_first = (now_ms >= t->first_fail_ms) ? (now_ms - t->first_fail_ms) : 0;
    log_warn("outreg",
             "REGISTER failed trunk=%s status=%d reason=%s fail#=%u since_first=%llums retry_in=%ums",
             t->name, status, reason ? reason : "?", t->reg_fail_count,
             (unsigned long long)since_first, t->retry_backoff_ms);
    pthread_mutex_unlock(&g_lock);
}

static void on_timer_fire(uint64_t now_ms_arg, void *ud)
{
    outreg_trunk_t *t = (outreg_trunk_t *)ud;
    (void)now_ms_arg;
    /* timer callback 在 tw 线程；g_lock 不在持锁上下文——trunk.name 只读，
     * 直接打；不要读 state/count 等可变字段（避免 data race） */
    log_info("outreg", "timer fire: retrying trunk=%s → outreg_send_register",
             t && t->name[0] ? t->name : "(null)");
    outreg_send_register(t->name, 1);
}

static int arm_timer(outreg_trunk_t *t, uint32_t expire_ms)
{
    if (g_ctx == NULL || g_ctx->tw == NULL) {
        log_warn("outreg", "arm_timer: ctx_or_tw_null ctx=%p tw=%p trunk=%s",
                 (void*)g_ctx, g_ctx ? (void*)g_ctx->tw : NULL,
                 t ? t->name : "(null)");
        return NSBC_ERR;
    }
    /* tw_add 需要**绝对到期时间**（CLOCK_MONOTONIC ms），不是相对间隔 */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t abs_expire = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000
                        + (uint64_t)expire_ms;
    tw_node_init(&t->timer);
    int rc = tw_add(g_ctx->tw, &t->timer, abs_expire, on_timer_fire, t);
    if (rc != NSBC_OK) {
        log_warn("outreg", "arm_timer: tw_add failed rc=%d trunk=%s expire_ms=%u abs=%llu",
                 rc, t ? t->name : "(null)", expire_ms, (unsigned long long)abs_expire);
        return rc;
    }
    log_info("outreg", "timer armed trunk=%s expire_ms=%u abs=%llu cb=on_timer_fire",
             t ? t->name : "(null)", expire_ms, (unsigned long long)abs_expire);
    return NSBC_OK;
}

static int disarm_timer(outreg_trunk_t *t)
{
    if (g_ctx == NULL || g_ctx->tw == NULL) return NSBC_OK;
    tw_del(g_ctx->tw, &t->timer);
    return NSBC_OK;
}

/* ---------- OPTIONS 健康探测 timer（P1-9，独立于注册 timer）----------
 *
 * 与 arm_timer/disarm_timer 同样的模式，但：
 *   - 回调是 on_ping_fire → 调 outreg_send_options
 *   - arm 条件：cfg_ping_sec > 0 且 state=OR_REGISTERED（只有已注册 trunk 才发 OPTIONS）
 *   - 周期 = cfg_ping_sec × 1000（毫秒）
 *   - ping_timer 与 timer（注册 timer）完全独立，互不干扰 */
static void on_ping_fire(uint64_t now_ms_arg, void *ud)
{
    (void)now_ms_arg;
    outreg_trunk_t *t = (outreg_trunk_t *)ud;
    /* 锁外发，调用完 arm 下一轮 */
    int rc = outreg_send_options(t->name);
    if (rc == NSBC_OK) {
        /* 发成功 → arm 下一轮；发失败（OR_INVALID/cfg_ping_sec=0）不再 arm */
        pthread_mutex_lock(&g_lock);
        if (g_ctx && g_ctx->tw && t->cfg_ping_sec > 0
            && (t->state == OR_REGISTERED || t->state == OR_REGISTERING)) {
            tw_node_init(&t->ping_timer);
            uint64_t abs = nsbc_now_ms() + (uint64_t)t->cfg_ping_sec * 1000ULL;
            tw_add(g_ctx->tw, &t->ping_timer, abs, on_ping_fire, t);
        }
        pthread_mutex_unlock(&g_lock);
    }
}

/* arm_ping_timer —— 只在 state=OR_REGISTERED 时 arm
 * （在 on_register_response 收到 200 OK 成功注册后调用，reload 时 state=OR_REGISTERED 也 arm） */
static void arm_ping_timer(outreg_trunk_t *t)
{
    if (g_ctx == NULL || g_ctx->tw == NULL) return;
    if (t->cfg_ping_sec == 0) return; /* 探测关闭 */
    if (t->state != OR_REGISTERED) return; /* 只探测已注册 trunk */
    tw_node_init(&t->ping_timer);
    uint64_t abs = nsbc_now_ms() + (uint64_t)t->cfg_ping_sec * 1000ULL;
    int rc = tw_add(g_ctx->tw, &t->ping_timer, abs, on_ping_fire, t);
    if (rc != NSBC_OK) {
        log_warn("outreg", "arm_ping_timer: tw_add failed rc=%d trunk=%s", rc, t->name);
        return;
    }
    log_info("outreg", "PING armed trunk=%s cfg_ping=%us", t->name, t->cfg_ping_sec);
}

static void disarm_ping_timer(outreg_trunk_t *t)
{
    if (g_ctx == NULL || g_ctx->tw == NULL) return;
    tw_del(g_ctx->tw, &t->ping_timer);
}

/* ---------- REGISTER 构建与发送 ---------- */

static void gen_call_id(char *out, size_t outsz, const char *local_host)
{
    static uint32_t g_counter = 0;
    uint32_t n = __atomic_add_fetch(&g_counter, 1, __ATOMIC_ACQ_REL);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(out, outsz, "nsbc-reg-%u-%lx@%s", n,
             (unsigned long)ts.tv_nsec,
             local_host ? local_host : "localhost");
}

/* ---------- 独立 Digest 鉴权头构建（P1-11）----------
 *
 * 通用函数：根据 hdr_type 选择对应的 challenge 缓存 + nc 计数器，
 * 计算完整 Digest 鉴权头值（不含 "Authorization: " 前缀）。
 *
 * REGISTER 401 → OUTREG_HDR_AUTH（WWW-Authenticate 缓存）
 * INVITE 407   → OUTREG_HDR_PROXY（Proxy-Authenticate 缓存）
 * INVITE 两轮  → 先 OUTREG_HDR_PROXY，后同时 OUTREG_HDR_PROXY + OUTREG_HDR_AUTH
 *
 * 注意：nc_counter 或 p_nc_counter 会自增 +1（RFC 3261：每次发新报文必须递增）。 */
int outreg_build_auth_hdr(outreg_trunk_t *t,
                          const char *method,
                          const char *uri,
                          int hdr_type,
                          char *out_buf, size_t out_sz)
{
    if (!t || !t->ha1_set) { out_buf ? out_buf[0] = '\0' : (void)0; return NSBC_OK; }

    /* 选缓存：OUTREG_HDR_AUTH → ch_* / nc_counter；OUTREG_HDR_PROXY → p_* / p_nc_counter */
    const char *crealm, *cnonce_src, *copaque, *cqop;
    uint32_t *nc_ptr;
    if (hdr_type == OUTREG_HDR_PROXY) {
        crealm = t->p_realm[0]   ? t->p_realm   : t->realm;
        cnonce_src = t->p_nonce;
        copaque = t->p_opaque[0] ? t->p_opaque : NULL;
        cqop = t->p_qop[0]       ? t->p_qop    : "auth";
        nc_ptr = &t->p_nc_counter;
    } else {
        crealm = t->ch_realm[0]  ? t->ch_realm  : t->realm;
        cnonce_src = t->ch_nonce;
        copaque = t->ch_opaque[0] ? t->ch_opaque : NULL;
        cqop = t->ch_qop[0]      ? t->ch_qop    : "auth";
        nc_ptr = &t->nc_counter;
    }

    /* 没有对应 challenge nonce → 跳过鉴权（out_buf 空） */
    if (cnonce_src[0] == '\0') {
        if (out_buf) out_buf[0] = '\0';
        return NSBC_OK;
    }

    /* nc 自增 + 格式化 */
    *nc_ptr += 1;
    char nc[16];
    snprintf(nc, sizeof(nc), "%08x", (unsigned)*nc_ptr);

    char cnonce[16];
    nsbc_rand_token(cnonce, sizeof(cnonce), 8);

    int rc = digest_client_build_auth(
        t->auth_user, crealm, cnonce_src, copaque,
        cqop, nc, cnonce, method, uri,
        t->ha1, 1 /* is_ha1=1 */,
        out_buf, out_sz);

    if (rc != NSBC_OK) {
        if (out_buf) out_buf[0] = '\0';
        log_warn("outreg", "build_auth_hdr failed trunk=%s method=%s hdr_type=%d rc=%d",
                 t->name, method ? method : "?", hdr_type, rc);
        return rc;
    }
    log_debug("outreg", "auth_hdr built trunk=%s method=%s hdr_type=%d nc=%s realm=%s",
              t->name, method ? method : "?", hdr_type, nc, crealm);
    return NSBC_OK;
}

int outreg_send_register(const char *trunk_name, int is_retry)
{
    outreg_trunk_t *t = find_trunk(trunk_name);
    if (t == NULL) {
        log_warn("outreg", "send_register: trunk '%s' not found in outreg array (trunk removed?)",
                 trunk_name ? trunk_name : "(null)");
        return NSBC_ERR;
    }
    if (g_ctx == NULL) {
        log_warn("outreg", "send_register: ctx is NULL");
        return NSBC_ERR;
    }

    pthread_mutex_lock(&g_lock);
    /* OR_DISABLED 也允许（仅用于 unregistering=1 场景 → enabled=0 注销）
     * OR_INVALID / OR_REGISTERING 仍拒绝（REGISTERING 已有事务在路上） */
    if (!t->unregistering && t->state != OR_IDLE && t->state != OR_RETRY_WAIT
        && t->state != OR_CHALLENGED && t->state != OR_REGISTERED) {
        log_warn("outreg", "send_register: trunk=%s state=%d (unreg=%d) — skip",
                 t->name, t->state, t->unregistering);
        pthread_mutex_unlock(&g_lock);
        return NSBC_ERR; /* 非法状态 */
    }

    const trunk_t *tr = NULL;
    if (g_ctx->rt) tr = rt_trunk_get((route_table_t *)g_ctx->rt, trunk_name);
    if (tr == NULL) {
        log_warn("outreg", "send_register: trunk '%s' not found in route_table (reload cleared?)",
                 trunk_name);
        pthread_mutex_unlock(&g_lock);
        return NSBC_NOTSUP;
    }
    if (tr->kind != TRUNK_REGISTER) {
        log_warn("outreg", "send_register: trunk '%s' kind=%d != TRUNK_REGISTER(1) — skip",
                 trunk_name, tr->kind);
        pthread_mutex_unlock(&g_lock);
        return NSBC_NOTSUP;
    }

    /* 更新 cseq / state （call_id 在 outreg_init 创建 trunk 时已生成，
     * RFC 3261 §10.2 同一 AOR 所有 REGISTER 共用同一 Call-ID） */
    t->cseq += 1;
    t->state = OR_REGISTERING;

    /* 组装 Contact：trunk_t.auth_user @ local_host:port */
    snprintf(t->contact, sizeof(t->contact),
             "sip:%s@%s:%d", tr->auth_user,
             get_local_host(g_ctx) ? get_local_host(g_ctx) : "0.0.0.0",
             OR_CONTACT_PORT);

    /* 期望过期时间：服务端下发 expires 优先，否则 cfg_expire_sec，再兜底 3600
     * —— 注销路径（unregistering=1）强制 Expires=0 */
    uint32_t want_expires;
    if (t->unregistering) {
        want_expires = 0;
    } else if (t->expires > 0) {
        want_expires = t->expires;               /* 服务端上次下发 */
    } else if (t->cfg_expire_sec > 0) {
        want_expires = t->cfg_expire_sec;        /* DB 配置 */
    } else {
        want_expires = OR_DEFAULT_EXPIRES;       /* 硬编码兜底 3600 */
    }

    /* Request-URI：默认端口 5060 省略（RFC 3261 §19.1），非默认端口才显式带
     * Digest Authorization 的 uri 参数必须与 Request-URI 完全一致（HA2 = MD5(method + ":" + uri)） */
    const char *local_host = get_local_host(g_ctx) ? get_local_host(g_ctx) : "0.0.0.0";
    uint16_t    dst_port   = tr->port ? tr->port : 5060;
    char ruri[256];
    if (dst_port == 5060) {
        snprintf(ruri, sizeof(ruri), "sip:%s", tr->host);
    } else {
        snprintf(ruri, sizeof(ruri), "sip:%s:%u", tr->host, dst_port);
    }

    /* Digest Authorization 头构建（锁内！因为要写 t->nc_counter + t->auth_hdr）：
     * 每次 send_register nc 递增 + 重算 response。条件：ha1_set=1 且有 challenge 缓存。
     * 专家 2.1：REGISTER 只用 WWW-Authenticate 缓存 → OUTREG_HDR_AUTH。 */
    outreg_build_auth_hdr(t, "REGISTER", ruri, OUTREG_HDR_AUTH,
                          t->auth_hdr, sizeof(t->auth_hdr));
    const char *auth_hdr = t->auth_hdr[0] ? t->auth_hdr : NULL;

    log_info("outreg", "REGISTERing trunk=%s cseq=%u callid=%s retry=%d expires=%u auth=%s",
             t->name, t->cseq, t->call_id, is_retry, want_expires,
             auth_hdr ? "yes" : "no");

    pthread_mutex_unlock(&g_lock);

    /* From / To AOR：auth_user@对端域（tr->realm 优先，fallback tr->host）
     *
     * RFC 3261 §10.2：REGISTER 的 From/To 头必须指向"正在注册的 AOR"——
     * 也就是我们要在对端注册哪个用户名 + 在哪个域下。
     * 用 local_host（我们自己的 IP）会导致：对端去查 domain="我们的 IP" 下的用户 → 403。
     *
     * FreeSWITCH 等注册服务器还会用 From 头的 host 作为 challenge realm，
     * 所以 realm 必须填对，否则收到的 Digest challenge realm 就是错的。
     *
     * 覆盖链：
     *   From user    = tr->from_user[0] ? tr->from_user : tr->auth_user
     *   From/To host = tr->realm[0]     ? tr->realm     : tr->host */
    const char *from_user = tr->from_user[0] ? tr->from_user : tr->auth_user;
    const char *aor_host  = tr->realm[0]     ? tr->realm     : tr->host;
    char from[256], to[256];
    snprintf(from, sizeof(from), "sip:%s@%s", from_user, aor_host);
    snprintf(to,   sizeof(to),   "sip:%s@%s", from_user, aor_host);

    /* transport + txn_mgr 检查 */
    if (g_tr == NULL || g_ctx->txn_mgr == NULL) {
        log_warn("outreg", "REGISTER skipped: sip_transport or txn_mgr not bound");
        pthread_mutex_lock(&g_lock);
        t->state = OR_FAILED;
        pthread_mutex_unlock(&g_lock);
        return NSBC_NOTSUP;
    }

    /* 构建完整 SIP REGISTER 报文（复用 send_uac 内部那套参数） */
    char buf[16 * 1024];
    char tag[64], from_line[256], branch[64];
    char extra_block[2048];
    size_t eo = 0;

    gen_from_tag(t->call_id, (long)t->cseq, tag, sizeof(tag));
    build_from_line(from, tag, from_line, sizeof(from_line));
    gen_branch(t->call_id, (long)t->cseq, branch, sizeof(branch));

    if (auth_hdr && auth_hdr[0]) {
        int w = snprintf(extra_block + eo, sizeof(extra_block) - eo,
                         "Authorization: %s\r\n", auth_hdr);
        if (w > 0) eo += (size_t)w;
    }
    {
        int w = snprintf(extra_block + eo, sizeof(extra_block) - eo,
                         "Expires: %lu\r\n", (unsigned long)want_expires);
        if (w > 0) eo += (size_t)w;
    }

    sip_req_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.method       = "REGISTER";
    spec.ruri         = ruri;
    spec.from         = from_line;
    spec.to           = to;
    spec.contact      = t->contact;
    spec.callid       = t->call_id;
    spec.cseq         = (long)t->cseq;
    spec.via_host     = local_host;
    spec.via_port     = OR_CONTACT_PORT;
    spec.transport    = "UDP";
    spec.branch       = branch;
    spec.max_forwards = 70;
    spec.extra        = eo > 0 ? extra_block : NULL;

    int rc = sip_build_request(buf, sizeof(buf), &spec);
    if (rc < 0) {
        log_warn("outreg", "REGISTER build failed trunk=%s rc=%d", t->name, rc);
        pthread_mutex_lock(&g_lock);
        t->state = OR_FAILED;
        pthread_mutex_unlock(&g_lock);
        return rc;
    }
    size_t buflen = (size_t)rc;

    nsbc_sa_t dst;
    nsbc_sa_clear(&dst);
    rc = nsbc_sa_from(&dst, tr->host, dst_port, 0);
    if (rc != NSBC_OK) {
        log_warn("outreg", "REGISTER dst resolve failed trunk=%s", t->name);
        pthread_mutex_lock(&g_lock);
        t->state = OR_FAILED;
        pthread_mutex_unlock(&g_lock);
        return rc;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;

    sip_txn_t *txn = txn_client_new((txn_mgr_t *)g_ctx->txn_mgr, TXN_NICT,
                                    branch, strlen(branch), SIP_M_REGISTER,
                                    &dst, 0 /* UDP=unreliable */,
                                    buf, buflen,
                                    outreg_tu_cb, t, now_ms);
    if (txn == NULL) {
        log_warn("outreg", "REGISTER txn create failed trunk=%s", t->name);
        pthread_mutex_lock(&g_lock);
        t->state = OR_FAILED;
        pthread_mutex_unlock(&g_lock);
        return NSBC_ERR;
    }

    /* txn NICT 自带 Timer F(64×T1≈32s) 超时驱动 TXN_EV_TIMEOUT，不需要自己 arm */
    log_info("outreg", "REGISTER sent trunk=%s txn=%p", t->name, (void*)txn);
    return NSBC_OK;
}

/* ---------- 响应处理 ---------- */

int outreg_on_register_response(const char *call_id, int status,
                                const char *auth_hdr)
{
    pthread_mutex_lock(&g_lock);
    outreg_trunk_t *t = NULL;
    for (size_t i = 0; i < g_trunk_cnt; ++i) {
        if (strcmp(g_trunks[i].call_id, call_id ? call_id : "") == 0) {
            t = &g_trunks[i]; break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (t == NULL) {
        /* 专家 2.6：reload 删除 trunk 后旧 REGISTER 响应回来 → 安全丢弃 */
        log_info("outreg", "stale REGISTER response (trunk removed/replaced): call_id=%s status=%d",
                 call_id ? call_id : "?", status);
        return NSBC_OK;
    }

    if (status == 200) {
        /* 注销 200：清 unregistering → OR_IDLE，不 arm timer
         * 正常注册 200：停 30s 超时定时器 → state=REGISTERED → arm 续注 */
        pthread_mutex_lock(&g_lock);
        if (t->unregistering) {
            t->unregistering = 0;
            disarm_timer(t);
            /* 200 OK 成功：清零所有失败诊断计数 */
            t->reg_fail_count = 0;
            t->first_fail_ms = 0;
            if (t->state == OR_DISABLED) {
                /* enabled=0 触发的注销：成功后保持 OR_DISABLED（等待 compact 删除） */
                log_info("outreg", "unregistered trunk=%s (expires=0 200 OK, OR_DISABLED)", t->name);
            } else {
                t->state = OR_IDLE;
                log_info("outreg", "unregistered trunk=%s (expires=0 200 OK)", t->name);
            }
        } else {
            disarm_timer(t);
            t->state = OR_REGISTERED;
            /* 200 OK 成功：清零所有失败诊断计数 */
            t->reg_fail_count = 0;
            t->first_fail_ms = 0;
            /* 续注时间：服务端下发 expires 优先，否则 cfg_expire_sec，再兜底 3600 */
            uint32_t renew_src = t->expires ? t->expires
                                : (t->cfg_expire_sec ? t->cfg_expire_sec : OR_DEFAULT_EXPIRES);
            uint32_t renew_ms = renew_src;
            if (renew_ms > 60) renew_ms = (renew_ms - 60) * 1000; /* 提前 60s 续注 */
            else renew_ms = 300000;
            arm_timer(t, renew_ms);
            t->retry_backoff_ms = 0;
            log_info("outreg", "REGISTERed trunk=%s expires=%u renew_ms=%u cfg_expire=%u",
                     t->name, t->expires, renew_ms, t->cfg_expire_sec);
            /* P1-9 OPTIONS 健康探测：state=REGISTERED 才 arm ping_timer
             * （cfg_ping_sec=0 / 已关闭 由 arm_ping_timer 内部 guard） */
            arm_ping_timer(t);
            /* P1-10 UDS push：REGISTERED → Web registrations UPSERT */
            {
                const trunk_t *tr = g_ctx && g_ctx->rt
                    ? rt_trunk_get((route_table_t *)g_ctx->rt, t->name) : NULL;
                push_registration_to_web(t, tr);
            }
        }
        pthread_mutex_unlock(&g_lock);
        return NSBC_OK;
    }

    if (status == 401 || status == 407) {
        /* Digest challenge → 解析 → 缓存 → state=CHALLENGED → 立刻重发带 Authorization */
        log_info("outreg", "REGISTER challenged trunk=%s status=%d", t->name, status);

        digest_challenge_t ch;
        int rc = auth_hdr ? digest_parse_challenge(auth_hdr, &ch) : NSBC_INVAL;
        if (rc != NSBC_OK) {
            /* challenge 解析失败（畸形/丢失 auth_hdr）——走重试路径 */
            log_warn("outreg", "trunk=%s challenge parse failed rc=%d → retry",
                     t->name, rc);
            enter_retry_wait(t, status, "challenge_parse_fail");
            return NSBC_OK;
        }

        if (!t->ha1_set) {
            /* ha1 缺失=配置问题，但也走重试（下次 reload 补密码后会自动恢复） */
            log_warn("outreg",
                     "trunk=%s challenged but no credentials (ha1 not set) → retry "
                     "(reload trunk with password to recover)",
                     t->name);
            enter_retry_wait(t, status, "no_credentials");
            return NSBC_OK;
        }

        pthread_mutex_lock(&g_lock);
        disarm_timer(t);

        /* 专家 2.1（修订）：两套独立 challenge 缓存，但 REGISTER 场景下
         * 收到 407 时必须**同时存两套**——
         *   p_* 套 → INVITE 407 两轮挑战用 Proxy-Authorization 头
         *   ch_* 套 → REGISTER 重发时用 Authorization 头（send_register 永远用 OUTREG_HDR_AUTH）
         * 如果对 REGISTER 回 407 只存 p_*，send_register 从 ch_* 读 nonce 为空
         * → Authorization 头为空 → 重发 REGISTER 还是裸的 → 对端永远 407
         *
         * 401 场景不变：只存 ch_*（WWW-Authenticate → Authorization 头），
         * p_* 保持不变（INVITE 已有的 Proxy-Authenticate 缓存不受干扰）。 */
        if (status == 407) {
            snprintf(t->p_realm, sizeof(t->p_realm), "%s", ch.realm);
            snprintf(t->p_nonce, sizeof(t->p_nonce), "%s", ch.nonce);
            snprintf(t->p_opaque, sizeof(t->p_opaque), "%s", ch.opaque);
            snprintf(t->p_qop, sizeof(t->p_qop), "%s", ch.qop);
            if (ch.stale) t->p_nc_counter = 0;
            /* 关键：REGISTER 重发要用 Authorization 头 → 同时存到 ch_* 套 */
            snprintf(t->ch_realm, sizeof(t->ch_realm), "%s", ch.realm);
            snprintf(t->ch_nonce, sizeof(t->ch_nonce), "%s", ch.nonce);
            snprintf(t->ch_opaque, sizeof(t->ch_opaque), "%s", ch.opaque);
            snprintf(t->ch_qop, sizeof(t->ch_qop), "%s", ch.qop);
            if (ch.stale) t->nc_counter = 0;
            log_info("outreg", "407 challenge (Proxy+WWW both) cached trunk=%s realm=%s nonce_len=%zu qop=%s",
                     t->name, t->p_realm, strlen(t->p_nonce), t->p_qop);
        } else {
            /* 401 Unauthorized（WWW-Authenticate） → 只存 ch_* 套，不碰 p_* */
            snprintf(t->ch_realm, sizeof(t->ch_realm), "%s", ch.realm);
            snprintf(t->ch_nonce, sizeof(t->ch_nonce), "%s", ch.nonce);
            snprintf(t->ch_opaque, sizeof(t->ch_opaque), "%s", ch.opaque);
            snprintf(t->ch_qop, sizeof(t->ch_qop), "%s", ch.qop);
            if (ch.stale) t->nc_counter = 0;
            log_info("outreg", "401 challenge (WWW) cached trunk=%s realm=%s nonce_len=%zu qop=%s",
                     t->name, t->ch_realm, strlen(t->ch_nonce), t->ch_qop);
        }
        t->state = OR_CHALLENGED;
        pthread_mutex_unlock(&g_lock);

        /* send_register 会检测到 challenge nonce + ha1_set 自动 build Authorization 头 */
        outreg_send_register(t->name, 0);
        return NSBC_OK;
    }

    /* 403 / 5xx / 6xx / 超时(status=0) / transport_err(status=-1)
     * → FAILED → 指数退避 → RETRY_WAIT → arm retry timer
     * 注意：401/407 challenge 不计入失败计数——那是正常 Digest 流程 */
    enter_retry_wait(t, status, "server_rejected_or_timeout");
    return NSBC_OK;
}

/* ---------- Phase 0: b2b_resp_hook 自注册回调 ----------
 *
 * 从 signaling.c 硬编码桥接搬过来：b2bua.c 收到无主响应旁路
 * （REGISTER 的 200/401/407 等不经过本端事务层的响应）时，
 * 通过 b2b_resp_hook 多槽链分发给所有注册者。outreg 在这里
 * 自注册自己的回调，不再依赖 signaling 硬编码接线。
 *
 * b2b_resp_hook_fn 返回 void，outreg_on_register_response 返回 int —
 * wrapper 忽略返回值（outreg 内部已记录状态机转换）。 */
static void outreg_on_register_response_via_b2b(
    const char *call_id, int status, const char *www_auth_hdr, void *ud)
{
    (void)ud;
    if (call_id == NULL || call_id[0] == '\0') return;
    log_info("outreg", "Phase 0: resp_hook callid=%s status=%d www=%s",
             call_id, status, www_auth_hdr ? www_auth_hdr : "(none)");
    outreg_on_register_response(call_id, status, www_auth_hdr);
}

/* ---------- init / fini ---------- */

/* b2b_resp_hook_add 返回的 hook 节点函数指针（fini 时传 b2b_resp_hook_rm）*/
static b2b_resp_hook_fn g_resp_hook_fn = NULL;

int outreg_init(nsbc_ctx_t *ctx)
{
    if (ctx == NULL) return NSBC_INVAL;

    g_ctx = ctx;

    /* 1. route_table 自举：如果 ctx->rt 没被上层创建，outreg 自己 rt_create
     *    （route_table 也被 dialplan/b2b 用；谁先创建谁赢，后续模块共享） */
    if (ctx->rt == NULL) {
        int rc = rt_create(&g_self_rt);
        if (rc != NSBC_OK) {
            log_warn("outreg", "rt_create failed → no route_table");
        } else {
            ctx->rt = g_self_rt;
        }
    }

    /* 2. transport 绑定留给 signaling sig_start 末尾 outreg_bind_transport()，
     *    此处 level 3 时 ctx->sip_tr 必然是 NULL */

    pthread_mutex_lock(&g_lock);
    g_trunk_cnt = 0;
    memset(g_trunks, 0, sizeof(g_trunks));

    /* 3. 加载 trunk：cfg 键 "outbound.register_trunks" 逗号分隔 trunk 名。
     *    对每个 trunk：先 rt_trunk_get(ctx->rt, name)；找不到则从兜底 cfg 键
     *    "outbound.trunk.<name>.host/port/auth_user/realm/expires" 读并自建 trunk_t
     *    再 rt_trunk_add（保证 route_table 也同步，后续模块可共享）。 */
    const char *names = NULL;
    (void)cfg_get_str(ctx->cfg, "outbound.register_trunks", &names, NULL);
    if (names && names[0]) {
        char *dup = strdup(names);
        if (dup) {
            for (char *tok = dup; tok && g_trunk_cnt < OR_MAX_TRUNKS; ) {
                char *comma = strchr(tok, ',');
                if (comma) *comma = '\0';
                while (*tok == ' ' || *tok == '\t') ++tok;
                if (*tok) {
                    const trunk_t *tr = NULL;
                    if (ctx->rt) tr = rt_trunk_get((route_table_t *)ctx->rt, tok);

                    if (tr == NULL && ctx->rt) {
                        /* 兜底：从 cfg 读 "outbound.trunk.<name>.*" 自建 trunk_t */
                        trunk_t local;
                        memset(&local, 0, sizeof(local));
                        snprintf(local.name, sizeof(local.name), "%s", tok);
                        local.kind = TRUNK_REGISTER;
                        const char *host = NULL;
                        long port = 5060;
                        const char *auth_user = NULL;
                        const char *realm = NULL;
                        char key[256];
                        snprintf(key, sizeof(key), "outbound.trunk.%s.host", tok);
                        if (cfg_get_str(ctx->cfg, key, &host, NULL) != NSBC_OK || !host || !host[0]) {
                            log_warn("outreg", "trunk=%s: no host → skip", tok);
                            tok = comma ? comma + 1 : NULL;
                            continue;
                        }
                        snprintf(local.host, sizeof(local.host), "%s", host);
                        snprintf(key, sizeof(key), "outbound.trunk.%s.port", tok);
                        cfg_get_int(ctx->cfg, key, &port, NULL);
                        local.port = (uint16_t)(port > 0 && port < 65536 ? port : 5060);
                        snprintf(key, sizeof(key), "outbound.trunk.%s.auth_user", tok);
                        cfg_get_str(ctx->cfg, key, &auth_user, NULL);
                        if (auth_user && auth_user[0])
                            snprintf(local.auth_user, sizeof(local.auth_user), "%s", auth_user);
                        snprintf(key, sizeof(key), "outbound.trunk.%s.realm", tok);
                        cfg_get_str(ctx->cfg, key, &realm, NULL);
                        if (realm && realm[0])
                            snprintf(local.realm, sizeof(local.realm), "%s", realm);
                        rt_trunk_add((route_table_t *)ctx->rt, &local);
                        tr = rt_trunk_get((route_table_t *)ctx->rt, tok);
                        if (tr)
                            log_info("outreg", "trunk=%s self-loaded host=%s:%u",
                                     tok, tr->host, tr->port);
                    }

                    if (tr && tr->kind == TRUNK_REGISTER) {
                        outreg_trunk_t *t = &g_trunks[g_trunk_cnt++];
                        snprintf(t->name, sizeof(t->name), "%s", tr->name);
                        t->cseq = 0;
                        t->expires = OR_DEFAULT_EXPIRES;
                        /* cfg 路径兜底：给 cfg_* 字段补默认值
                         * （Web reload 是主路径；纯 cfg 部署时靠这些默认值） */
                        t->cfg_expire_sec = tr->expire_seconds ? tr->expire_seconds : OR_DEFAULT_EXPIRES;
                        t->cfg_retry_sec  = tr->retry_seconds ? tr->retry_seconds : 5;
                        t->state = OR_IDLE;
                        /* RFC 3261 §10.2：同一 AOR 的所有 REGISTER 必须复用同一个
                         * Call-ID（对端用它关联续注）。在 init 时生成一次，后续
                         * send_register / 401/407 重发 / 续注 都不再生成新的。 */
                        const char *cid_host = get_local_host(ctx) ? get_local_host(ctx) : tr->host;
                        gen_call_id(t->call_id, sizeof(t->call_id), cid_host);

                        /* 凭证加载：优先 cfg "outbound.trunk.<name>.ha1"（预计算 32 hex）→
                         * 再 cfg "outbound.trunk.<name>.password"（明文→digest_make_ha1）→
                         * 都没有 → ha1_set=0（401/407 后直接 FAILED）。 */
                        snprintf(t->auth_user, sizeof(t->auth_user), "%s", tr->auth_user);
                        snprintf(t->realm, sizeof(t->realm), "%s", tr->realm);
                        {
                            char key[256];
                            const char *precomputed_ha1 = NULL;
                            snprintf(key, sizeof(key), "outbound.trunk.%s.ha1", tr->name);
                            if (cfg_get_str(ctx->cfg, key, &precomputed_ha1, NULL) == NSBC_OK
                                && precomputed_ha1 && strlen(precomputed_ha1) == 32) {
                                memcpy(t->ha1, precomputed_ha1, 32);
                                t->ha1[32] = '\0';
                                t->ha1_set = 1;
                                log_info("outreg", "trunk=%s ha1 loaded from cfg ha1 key", tr->name);
                            } else {
                                const char *pwd = NULL;
                                snprintf(key, sizeof(key), "outbound.trunk.%s.password", tr->name);
                                if (cfg_get_str(ctx->cfg, key, &pwd, NULL) == NSBC_OK
                                    && pwd && pwd[0]) {
                                    digest_make_ha1(t->auth_user, t->realm, pwd, t->ha1);
                                    t->ha1_set = 1;
                                    log_info("outreg", "trunk=%s ha1 computed from cfg password key", tr->name);
                                } else {
                                    t->ha1_set = 0;
                                    log_warn("outreg", "trunk=%s NO credentials (no ha1/password cfg) → "
                                             "401/407 will fail", tr->name);
                                }
                            }
                        }

                        g_stat.total++;
                        log_info("outreg", "load trunk=%s host=%s:%u callid=%s expires=%u ha1_set=%d",
                                 tr->name, tr->host, tr->port, t->call_id, t->expires, t->ha1_set);
                    } else if (tr && tr->kind != TRUNK_REGISTER) {
                        log_warn("outreg", "skip trunk=%s kind!=REGISTER (kind=%d)",
                                 tr->name, tr->kind);
                    } else {
                        log_warn("outreg", "trunk=%s not found in route_table (no cfg fallback)", tok);
                    }
                }
                tok = comma ? comma + 1 : NULL;
            }
            free(dup);
        }
    } else {
        log_info("outreg", "outbound.register_trunks not set → no trunk loaded");
    }

    /* 对每个 trunk 的首注 REGISTER 留给 outreg_bind_transport()
     * —— 此处 g_tr=NULL，发了也返回 NOTSUP */
    pthread_mutex_unlock(&g_lock);

    /* Phase 0: b2b_resp_hook_add 自注册 —— outreg 从此不再依赖
     * signaling.c 硬编码桥接。b2bua 无主响应旁路（REGISTER 的
     * 200/401/407）统一通过多槽链分发给 outreg 等所有注册者。*/
    {
        int arc = b2b_resp_hook_add(outreg_on_register_response_via_b2b, NULL);
        if (arc == NSBC_OK) {
            g_resp_hook_fn = outreg_on_register_response_via_b2b;
            log_info("outreg", "Phase 0: b2b_resp_hook_add OK");
        } else {
            log_warn("outreg", "Phase 0: b2b_resp_hook_add failed arc=%d (HOOK 链可能满)", arc);
        }
    }

    return NSBC_OK;
}

void outreg_fini(nsbc_ctx_t *ctx)
{
    (void)ctx;

    /* Phase 0: 先卸载 hook（阻止新响应再进来），再清理内部状态 */
    if (g_resp_hook_fn) {
        b2b_resp_hook_rm(g_resp_hook_fn, NULL);
        log_info("outreg", "Phase 0: b2b_resp_hook_rm done");
        g_resp_hook_fn = NULL;
    }

    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_trunk_cnt; ++i) {
        disarm_timer(&g_trunks[i]);
    }
    g_trunk_cnt = 0;
    memset(&g_trunks, 0, sizeof(g_trunks));
    memset(&g_stat, 0, sizeof(g_stat));
    g_ctx = NULL;
    g_tr  = NULL;
    if (g_self_rt) {
        rt_destroy(g_self_rt);
        g_self_rt = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}

/* 主动注销（发 Expires=0 REGISTER，复用同一 call_id/cseq/auth）：
 * RFC 3261 §10.2.2：REGISTER expires=0 向服务器表明"我要注销"。
 * stop=1 则定时器停 + 置 OR_IDLE（正常模块关闭流程）；
 * stop=0 则状态保持（对端应答前不影响续注计时器——通常不应发生，保留做 API 灵活性）。 */
int outreg_unregister_all(int stop)
{
    if (g_ctx == NULL || g_tr == NULL) return NSBC_NOTSUP;

    pthread_mutex_lock(&g_lock);
    int sent = 0;
    for (size_t i = 0; i < g_trunk_cnt; ++i) {
        outreg_trunk_t *t = &g_trunks[i];
        const trunk_t *tr = g_ctx->rt
            ? rt_trunk_get((route_table_t *)g_ctx->rt, t->name) : NULL;
        if (tr == NULL || tr->kind != TRUNK_REGISTER) continue;

        if (stop) disarm_timer(t);
        /* 不在 OR_REGISTERING 里发——与挂起 REGISTER 对同一个 call_id */
        if (t->state == OR_REGISTERING) {
            log_debug("outreg", "unreg skip trunk=%s (state=REGISTERING)", t->name);
            continue;
        }

        /* 锁内构建所有参数 + 拷到栈变量 → unlock 后再调 send_uac
         * （send_uac 期间 timer 线程不碰这些字段） */
        const char *local_host = get_local_host(g_ctx) ? get_local_host(g_ctx) : "0.0.0.0";
        uint16_t dst_port = tr->port ? tr->port : 5060;
        char ruri[256], from[256], to[256];
        if (dst_port == 5060) {
            snprintf(ruri, sizeof(ruri), "sip:%s", tr->host);
        } else {
            snprintf(ruri, sizeof(ruri), "sip:%s:%u", tr->host, dst_port);
        }
        const char *from_user = tr->from_user[0] ? tr->from_user : tr->auth_user;
        const char *aor_host  = tr->realm[0]     ? tr->realm     : tr->host;
        snprintf(from, sizeof(from), "sip:%s@%s", from_user, aor_host);
        snprintf(to,   sizeof(to),   "sip:%s@%s", from_user, aor_host);

        t->cseq += 1;
        outreg_build_auth_hdr(t, "REGISTER", ruri, OUTREG_HDR_AUTH,
                              t->auth_hdr, sizeof(t->auth_hdr));

        /* 栈变量拷贝（锁外安全用） */
        char  call_id[64], contact[256], auth[768];
        memcpy(call_id, t->call_id, sizeof(call_id));
        memcpy(contact, t->contact, sizeof(contact));
        memcpy(auth, t->auth_hdr, sizeof(auth));
        long  cseq_shot = (long)t->cseq;
        const char *auth_hdr = auth[0] ? auth : NULL;

        /* 标记"注销中"——200 OK 回来会走 OR_IDLE 分支，不复 arm 续注 timer */
        t->unregistering = 1;
        if (stop) t->state = OR_REGISTERING;

        pthread_mutex_unlock(&g_lock);

        int rc = sip_transport_send_uac(g_tr,
                                        "REGISTER", ruri,
                                        from, NULL,
                                        to, call_id, cseq_shot,
                                        contact, 0L /* expires=0 */,
                                        auth_hdr,
                                        local_host, OR_CONTACT_PORT,
                                        tr->host, dst_port,
                                        NULL, NULL, 0, NULL);
        pthread_mutex_lock(&g_lock);
        if (rc == NSBC_OK) {
            sent++;
            log_info("outreg", "unreg sent trunk=%s callid=%s cseq=%ld auth=%s",
                     t->name, call_id, cseq_shot, auth_hdr ? "yes" : "no");
            if (stop) {
                /* send 失败也应该清 timer */
                disarm_timer(t);
                t->state = OR_IDLE;
                t->unregistering = 0;
            }
        } else {
            log_warn("outreg", "unreg send failed trunk=%s rc=%d → unregistering=0",
                     t->name, rc);
            t->unregistering = 0;
            if (stop) {
                disarm_timer(t);
                t->state = OR_IDLE;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    log_info("outreg", "unregister_all sent=%d stop=%d", sent, stop);
    return NSBC_OK;
}

void outreg_stat(outreg_stat_t *out)
{
    if (out == NULL) return;
    pthread_mutex_lock(&g_lock);
    out->total = g_stat.total;
    out->active = g_stat.active;
    out->registered = 0;
    out->failed = 0;
    for (size_t i = 0; i < g_trunk_cnt; ++i) {
        if (g_trunks[i].state == OR_REGISTERED) out->registered++;
        if (g_trunks[i].state == OR_FAILED || g_trunks[i].state == OR_RETRY_WAIT) out->failed++;
    }
    pthread_mutex_unlock(&g_lock);
}

/* ---------- OPTIONS 健康探测实现（P1-9，文档 §3.2）---------- */

/* Prometheus 指标名（TODO：metric_reg 简化 API 未实现，等 obs_metrics 扩展后接入）：
 *   nsbc_gateway_health_state{trunk="xxx"} gauge   0=HEALTHY 1=DEGRADED 2=UNREACHABLE
 *   nsbc_gateway_ping_fail_total{trunk="xxx"} counter */

/* 按 gw_health_t 整数编码映射到 Prometheus gauge 值 */
static const char *health_str(uint8_t h)
{
    switch (h) {
        case GW_HEALTH_HEALTHY:     return "HEALTHY";
        case GW_HEALTH_DEGRADED:    return "DEGRADED";
        case GW_HEALTH_UNREACHABLE: return "UNREACHABLE";
        default:                    return "UNKNOWN";
    }
}

/* 更新 health_state + Prometheus，仅在状态真正变化时触发（避免噪声） */
static void set_health(outreg_trunk_t *t, uint8_t new_state)
{
    if (t->health_state == new_state) return;
    uint8_t old = t->health_state;
    t->health_state = new_state;
    log_info("outreg", "health_state changed trunk=%s %s → %s (fail_count=%u)",
             t->name, health_str(old), health_str(new_state), t->ping_fail_count);
    /* Prometheus gauge 更新：按 trunk label 设置值
     * TODO：当前 metric_reg.h 只有 metric_source_register 注册 callback 模式，
     *       metric_gauge_set 简化 API 不存在——等 obs_metrics 扩展后再接入。 */
    /* metric_gauge_set(...); */
}

int outreg_send_options(const char *trunk_name)
{
    if (!trunk_name || !*trunk_name) return NSBC_ERR;

    pthread_mutex_lock(&g_lock);
    outreg_trunk_t *t = NULL;
    for (size_t i = 0; i < g_trunk_cnt; ++i) {
        if (strcmp(g_trunks[i].name, trunk_name) == 0) { t = &g_trunks[i]; break; }
    }
    if (t == NULL) { pthread_mutex_unlock(&g_lock); return NSBC_ERR; }

    if (t->cfg_ping_sec == 0 || t->state == OR_DISABLED || t->state == OR_INVALID) {
        pthread_mutex_unlock(&g_lock);
        return NSBC_ERR;
    }
    if (g_tr == NULL || g_ctx->txn_mgr == NULL) {
        pthread_mutex_unlock(&g_lock);
        return NSBC_ERR;
    }
    const trunk_t *tr = NULL;
    if (g_ctx->rt) tr = rt_trunk_get((route_table_t *)g_ctx->rt, trunk_name);
    if (tr == NULL) { pthread_mutex_unlock(&g_lock); return NSBC_ERR; }

    /* 独立 Call-ID（专家 1.3：OPTIONS 与 REGISTER 完全独立） */
    if (t->ping_call_id[0] == '\0') {
        gen_call_id(t->ping_call_id, sizeof(t->ping_call_id),
                    g_local_host[0] ? g_local_host : "localhost");
    }
    t->ping_cseq += 1;

    /* Request-URI / From / To：复用 send_register 里同一套 host 拼接
     * OPTIONS 不需要 Contact / Expires / Digest —— 就是纯探测
     * 但 From/To host 必须是对端域（tr->realm / tr->host），否则 FS 可能拒 */
    const char *local_host = get_local_host(g_ctx) ? get_local_host(g_ctx) : "0.0.0.0";
    uint16_t    dst_port   = tr->port ? tr->port : 5060;
    char ruri[256];
    if (dst_port == 5060) {
        snprintf(ruri, sizeof(ruri), "sip:%s", tr->host);
    } else {
        snprintf(ruri, sizeof(ruri), "sip:%s:%u", tr->host, dst_port);
    }

    const char *from_user = tr->from_user[0] ? tr->from_user : tr->auth_user;
    const char *aor_host  = tr->realm[0]     ? tr->realm     : tr->host;
    char from[256], to[256];
    snprintf(from, sizeof(from), "sip:%s@%s", from_user, aor_host);
    snprintf(to,   sizeof(to),   "sip:%s@%s", from_user, aor_host);

    t->last_ping_ms = nsbc_now_ms();
    uint32_t p_cseq = t->ping_cseq;
    char p_cid[64];
    memcpy(p_cid, t->ping_call_id, sizeof(p_cid));

    pthread_mutex_unlock(&g_lock);

    /* ---- 通过 sip_transport_send_uac 发 OPTIONS（复用 txn/transport 层，自动构建 Via/branch/From-tag）
     * OPTIONS 不需要 Contact/Expires/Digest auth —— 纯探测 */
    log_debug("outreg", "OPTIONS sending trunk=%s callid=%s cseq=%u ruri=%s",
              trunk_name, p_cid, p_cseq, ruri);

    int rc = sip_transport_send_uac(g_tr,
                                    "OPTIONS", ruri,
                                    from, NULL,            /* from_tag=NULL：send_uac 内部自动生成 */
                                    to, p_cid, (long)p_cseq,
                                    NULL, 0L,              /* contact, expires */
                                    NULL,                  /* auth_hdr：纯探测不带 auth */
                                    local_host, 5060,      /* via_host, via_port */
                                    tr->host, dst_port,    /* dst */
                                    NULL, NULL, 0, NULL);  /* extra, body, content_type */
    if (rc != NSBC_OK) {
        log_warn("outreg", "OPTIONS send failed trunk=%s rc=%d", trunk_name, rc);
        return rc;
    }

    log_info("outreg", "OPTIONS sent trunk=%s callid=%s cseq=%u", trunk_name, p_cid, p_cseq);
    return NSBC_OK;
}

/* OPTIONS 响应回调（文档 §3.2）：
 *   status=2xx → ping_fail_count=0, health_state=HEALTHY
 *   status=403  → 运营商显式拒绝 → 直接 UNREACHABLE
 *   超时 / 非 2xx → ping_fail_count++
 *                    ping_fail_count>=3  → DEGRADED
 *                    ping_fail_count>=10 → UNREACHABLE */
void outreg_on_options_response(const char *ping_call_id, int status, uint64_t rtt_ms)
{
    if (!ping_call_id || !*ping_call_id) return;

    pthread_mutex_lock(&g_lock);
    outreg_trunk_t *t = NULL;
    for (size_t i = 0; i < g_trunk_cnt; ++i) {
        if (strcmp(g_trunks[i].ping_call_id, ping_call_id) == 0) { t = &g_trunks[i]; break; }
    }
    if (t == NULL) { pthread_mutex_unlock(&g_lock); return; }

    log_info("outreg", "OPTIONS rx trunk=%s callid=%s status=%d rtt_ms=%lu",
             t->name, ping_call_id, status, (unsigned long)rtt_ms);

    /* Prometheus：ping 失败计数（TODO：metric_counter_inc 简化 API 不存在） */
    if (status < 200 || status >= 300) {
        /* metric_counter_inc(METRIC_FAIL, 1, "trunk", t->name); */
    }

    if (status >= 200 && status < 300) {
        /* 2xx 成功 —— 清零连续失败计数 + 回到 HEALTHY */
        if (t->ping_fail_count > 0) {
            log_info("outreg", "OPTIONS success trunk=%s ping_fail_count reset %u→0 (rtt=%lums)",
                     t->name, t->ping_fail_count, (unsigned long)rtt_ms);
        }
        t->ping_fail_count = 0;
        set_health(t, GW_HEALTH_HEALTHY);
    } else if (status == 403) {
        /* 运营商显式拒绝 OPTIONS —— 不是超时，但明确表示"不响应" → 直接 UNREACHABLE
         * （很多运营商 OPTIONS 返回 403 Forbidden 表示不接受该探测，但注册还可能有效。
         * 这里做成可配置：硬编码 403 仍视为失败，实际行为看运营商兼容性。） */
        log_warn("outreg", "OPTIONS 403 trunk=%s → UNREACHABLE (carrier explicit reject)", t->name);
        t->ping_fail_count++;
        set_health(t, GW_HEALTH_UNREACHABLE);
    } else {
        /* 其他非 2xx —— 累加 ping_fail_count */
        t->ping_fail_count++;
        uint32_t fc = t->ping_fail_count;
        log_info("outreg", "OPTIONS failed trunk=%s status=%d ping_fail_count=%u",
                 t->name, status, fc);
        if (fc >= 10) {
            set_health(t, GW_HEALTH_UNREACHABLE);
        } else if (fc >= 3) {
            set_health(t, GW_HEALTH_DEGRADED);
        } else {
            /* ping_fail_count 1-2 —— 不降级，等下次 */
        }
    }

    pthread_mutex_unlock(&g_lock);
}

/* ---------- 呼入 trunk 四层匹配 P2-14 层 1：register 型反向关联 ----------
 *
 * To-user / PAI → outreg_trunk_t.auth_user；仅 state==OR_REGISTERED 才算命中
 * （未注册不表示此 trunk —— 可能 trunk 配置但 auth_user 相同，不应误匹配）。
 * 锁内遍历（g_trunks[] 规模 < 64，无竞争）。 */
int outreg_find_by_auth_user(const char *auth_user, char *out_name, size_t out_len)
{
    if (!auth_user || !*auth_user || !out_name || out_len == 0) return 0;

    int found = 0;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_trunk_cnt; ++i) {
        outreg_trunk_t *t = &g_trunks[i];
        if (t->state == OR_REGISTERED
            && t->auth_user[0]
            && strcmp(t->auth_user, auth_user) == 0) {
            snprintf(out_name, out_len, "%s", t->name);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

/* 路由层调用：查询某 trunk 健康状态
 * 未知 trunk（已删除或 cfg_ping_sec=0）→ HEALTHY（不阻断选路，降级为兼容模式） */
uint8_t outreg_get_health(const char *trunk_name)
{
    if (!trunk_name || !*trunk_name) return GW_HEALTH_HEALTHY;
    pthread_mutex_lock(&g_lock);
    uint8_t h = GW_HEALTH_HEALTHY;
    for (size_t i = 0; i < g_trunk_cnt; ++i) {
        if (strcmp(g_trunks[i].name, trunk_name) == 0) {
            h = g_trunks[i].health_state;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return h;
}

/* ---------- arm/disarm ping_timer 的挂点 ----------
 * outreg_on_register_response 收到 200 OK 成功注册后 arm；
 * OR_DISABLED/OR_INVALID/OR_FAILED 时 disarm。
 *
 * (在上面 on_register_response 和 outreg_reload_from_route_table 里手动调用 arm_ping_timer/disarm_ping_timer) */

/* ---------- P1-10：注册状态 UDS 推送到 Web registrations 表 ----------
 *
 * 设计：outreg_trunk_t 状态变化时（OR_REGISTERED / OR_FAILED / OR_DISABLED / OR_INVALID）
 *      调此 helper → 组装 JSON payload → uds_send_register → Web 端 upsert_registration 落库。
 *
 * Web 端 handler (uds_server.rs line 312-328) 期望的 payload JSON 字段：
 *   user            string  "trunk:<name>"
 *   contact         string  "sip:<auth_user>@<host>:<port>"
 *   instance_id     string  "outreg:<name>"
 *   agent           string  固定 "NSBC-GW"
 *   expires_sec     u32     min(t->expires, t->cfg_expire_sec) —— 剩余有效秒数
 *   received_ip     ""      outreg 场景下为空（C 侧 registrar.c 检测 NAT，outreg 不带 NAT 元数据）
 *   received_port   0       同上
 *   nat_detected    0       同上
 *   public_contact  ""      同上
 *   transport       ""      同上
 *
 * 唯一键 = user + instance_id（Web 端 registrations 表约束），重复推送 UPSERT。 */
static const char *or_state_str(uint8_t s)
{
    switch (s) {
        case OR_IDLE:           return "IDLE";
        case OR_REGISTERING:    return "REGISTERING";
        case OR_CHALLENGED:     return "CHALLENGED";
        case OR_REGISTERED:     return "REGISTERED";
        case OR_RETRY_WAIT:     return "RETRY_WAIT";
        case OR_FAILED:         return "FAILED";
        case OR_DISABLED:       return "DISABLED";
        case OR_INVALID:        return "INVALID";
        default:                return "UNKNOWN";
    }
}

static void push_registration_to_web(outreg_trunk_t *t, const trunk_t *tr)
{
    if (t == NULL) return;

    /* 从 g_ctx->rt 取最新 trunk_t（host/port），若拿不到就只用 cfg_* 字段 */
    char contact[256] = {0};
    char auth_user[96] = {0};
    char host[128] = {0};
    uint16_t port = 5060;

    if (tr) {
        snprintf(auth_user, sizeof(auth_user), "%s", tr->auth_user);
        snprintf(host, sizeof(host), "%s", tr->host);
        port = tr->port ? tr->port : 5060;
    } else {
        snprintf(auth_user, sizeof(auth_user), "%s", t->auth_user);
        /* tr 拿不到时 host 也没法拿，contact 里 host 位置留 "(unknown)" —— Web 仍能显示 trunk 状态 */
        snprintf(host, sizeof(host), "(unknown)");
    }
    if (host[0])
        snprintf(contact, sizeof(contact), "sip:%s@%s:%u", auth_user, host, port);

    /* 剩余有效秒数：REGISTERED → expires 或 cfg_expire_sec；FAILED/DISABLED/INVALID → 0（立即过期） */
    uint32_t expires_sec = 0;
    if (t->state == OR_REGISTERED) {
        expires_sec = t->expires ? t->expires : (t->cfg_expire_sec ? t->cfg_expire_sec : OR_DEFAULT_EXPIRES);
    }

    /* 组装 JSON payload（Web 端 handler 用 "user" / "contact" / "instance_id" / "agent" / "expires_sec" / ... 字段名） */
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    char user_key[256];
    snprintf(user_key, sizeof(user_key), "trunk:%s", t->name);
    cJSON_AddStringToObject(root, "user", user_key);
    cJSON_AddStringToObject(root, "contact", contact);

    char inst_key[256];
    snprintf(inst_key, sizeof(inst_key), "outreg:%s", t->name);
    cJSON_AddStringToObject(root, "instance_id", inst_key);
    cJSON_AddStringToObject(root, "agent", "NSBC-GW");
    cJSON_AddNumberToObject(root, "expires_sec", (double)expires_sec);

    /* NAT 元数据（outreg 场景下全部留空/0 —— registrar.c 推送分机注册时才填） */
    cJSON_AddStringToObject(root, "received_ip", "");
    cJSON_AddNumberToObject(root, "received_port", 0);
    cJSON_AddNumberToObject(root, "nat_detected", 0);
    cJSON_AddStringToObject(root, "public_contact", "");
    cJSON_AddStringToObject(root, "transport", "");

    /* detail 字段（JSON string，供前端展示） */
    char detail[512];
    snprintf(detail, sizeof(detail),
             "{\"trunk\":\"%s\",\"state\":\"%s\",\"health\":\"%s\",\"profile\":\"%s\"}",
             t->name, or_state_str(t->state), health_str(t->health_state),
             t->profile_name[0] ? t->profile_name : "(none)");
    cJSON_AddStringToObject(root, "detail", detail);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    int rc = uds_send_register(json);
    free(json);

    if (rc != NSBC_OK) {
        log_warn("outreg", "UDS push register failed trunk=%s state=%s rc=%d",
                 t->name, or_state_str(t->state), rc);
    } else {
        log_debug("outreg", "UDS push register OK trunk=%s state=%s expires=%u contact=%s",
                  t->name, or_state_str(t->state), expires_sec, contact);
    }
}

/* ---------- module descriptor ---------- */

NSBC_MOD0_CAT(outreg, 1, MOD_L_WORKER_READY, 0, MOD_CAT_OUTBOUND, outreg_init, outreg_fini);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
