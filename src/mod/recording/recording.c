/* 通话录音接线（REQ-REC-001~004，P1）：rec_mgr 生命周期 + ctx 挂载
 *
 * 职责：
 *  - init 读 cfg → rec_mgr_create → ctx->rec_mgr 挂载；fini 销毁
 *  - 对外暴露 recording_apply_dp_action：callctrl 在 dp_execute
 *    命中 rule 后调用，根据 dp_result.rec_action 启停会话
 *  - 维护 per-callid sess 注册表（简单哈希表，P1 用固定桶数）：
 *    stop 时从表中摘除；录音 stop 输出 final_path 回填 cdr.rec_path
 */
#include "module.h"
#include "recorder.h"
#include "recording.h"
#include "app_ctx.h"   /* struct nsbc_ctx 完整定义 */
#include "cfg.h"
#include "log.h"

/* === Phase 0 自注册依赖 === */
#include "media_bridge.h"   /* bridge_add_rec_feed_hook */
#include "app_registry.h"   /* app_register / app_desc_t */
#include "dp_sleep.h"       /* dp_sleep_app_reg() 全局注册表 getter */
#include "b2bua.h"          /* b2b_call_t (bridge 回调 call_key) */

/* sig_bridge_instance() 定义在 signaling.c（core 层），无独立 header ——
 * extern 声明让 dlopen 模块能 link（主二进制 -export-dynamic 已加）*/
extern bridge_mgr_t *sig_bridge_instance(struct nsbc_ctx *ctx);

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>   /* unlink() */
#include <limits.h>
#include <stddef.h>
#define REC_BUCKET_BITS 10
#define REC_BUCKET_CNT  (1 << REC_BUCKET_BITS)

struct rec_bucket {
    char          callid[REC_CALLID_MAX + 8];  /* callid:leg 复合 key */
    rec_sess_t   *sess;
    char          tpl[128];                    /* start 时缓存的 rec_path_template */
    char          caller[64];                  /* start 时缓存的 caller */
    char          callee[64];                  /* start 时缓存的 callee */
    struct rec_bucket *next;
};

static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rec_bucket *g_reg[REC_BUCKET_CNT]; /* 全局 per-callid:leg 注册表 */
static void *g_rec_bridge_hook = NULL;
static struct nsbc_ctx *g_ctx = NULL;

/* forward declarations（文件后半段定义，前半段要用）*/
static void reg_del(const char *callid, int leg);

/* 复合 key：callid:leg（分腿独立录制，P0 之前单 key 会撞分腿 open） */
static void make_key(char *out, size_t outsz, const char *callid, int leg)
{
    snprintf(out, outsz, "%s:%d", callid, leg);
}

static inline uint32_t callid_hash(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) + (uint8_t)*s++;
    return h & (REC_BUCKET_CNT - 1);
}

static void reg_put(const char *callid, int leg, rec_sess_t *s,
                    const char *tpl, const char *caller, const char *callee)
{
    char key[REC_CALLID_MAX + 8];
    make_key(key, sizeof(key), callid, leg);
    pthread_mutex_lock(&g_reg_lock);
    uint32_t b = callid_hash(key);
    struct rec_bucket *node = calloc(1, sizeof(*node));
    if (!node) { pthread_mutex_unlock(&g_reg_lock); return; }
    snprintf(node->callid, sizeof(node->callid), "%s", key);
    node->sess = s;
    if (tpl && tpl[0])     snprintf(node->tpl, sizeof(node->tpl), "%s", tpl);
    if (caller && caller[0]) snprintf(node->caller, sizeof(node->caller), "%s", caller);
    if (callee && callee[0]) snprintf(node->callee, sizeof(node->callee), "%s", callee);
    node->next = g_reg[b];
    g_reg[b] = node;
    pthread_mutex_unlock(&g_reg_lock);
}

/* 返回完整 bucket（含 sess + tpl/caller/callee），stop 路径用 */
static struct rec_bucket *reg_get_bucket(const char *callid, int leg)
{
    char key[REC_CALLID_MAX + 8];
    make_key(key, sizeof(key), callid, leg);
    uint32_t b = callid_hash(key);
    struct rec_bucket *n = g_reg[b];
    while (n) {
        if (strcmp(n->callid, key) == 0) return n;
        n = n->next;
    }
    return NULL;
}

static rec_sess_t *reg_get(const char *callid, int leg)
{
    pthread_mutex_lock(&g_reg_lock);
    struct rec_bucket *n = reg_get_bucket(callid, leg);
    pthread_mutex_unlock(&g_reg_lock);
    return n ? n->sess : NULL;
}

/* 公共 API：按 callid+leg 反查 rec_sess_t*（bridge.rec_feed_fn wrapper 用）。
 * 直接调内部 static reg_get；零编译风险：纯 wrapper，不改内部逻辑。 */
rec_sess_t *recording_find_by_callid(const char *callid, int leg)
{
    if (!callid || !callid[0]) return NULL;
    return reg_get(callid, leg);
}

/* Phase 0: bridge rec_feed 回调 —— 从 signaling.c rec_feed_wrapper 搬过来，
 * 内部查 recording 自己的 reg_put/reg_get 表（signaling 不再硬编码桥接 cfg.rec_feed_fn）。
 * bridge_add_rec_feed_hook 注册后，媒体线程每次 payload 自动调这里。
 *
 * ★ 性能关键：recording_open 时已经用 __atomic_store_n 把 sess 挂到 call->rec[leg]，
 * 这里直接 atomic_load 取 —— 零锁、零哈希表、零 strcmp。reg_get 仅在 callctrl 启动/停止路径用。 */
static void recording_rec_feed(void *ud, void *call_key, const void *payload,
                               size_t len, int leg, int pt)
{
    (void)ud;
    b2b_call_t *c = (b2b_call_t *)call_key;
    if (!c || !payload || len == 0) return;
    if (leg < 0 || leg > 1) return;
    /* 直接从 call 上 atomic 取 sess —— 无锁查表 */
    rec_sess_t *sess = (rec_sess_t *)__atomic_load_n((void **)&c->rec[leg], __ATOMIC_ACQUIRE);
    if (!sess) return;
    rec_feed_raw(sess, leg, payload, len, pt);
}

/* Phase 0: dialplan app_record 实现 —— 从 app_registry.c stub 搬到 recording 模块。
 * app_ctx_t 里有 call_key（b2b_call_t*），leg，vars 等。data="start"/"stop"（默认 start）。
 * 直接调 rec_open → reg_put / rec_stop → reg_del，不走 dp_result.rec_action 路径（DP 路径
 * 是 resolve_cb 里按规则批量启停，这里是 app 显式触发单 leg 启停）。 */
static app_rc_t app_record_exec(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!c || !c->call_key) return APP_RC_FAILED;
    b2b_call_t *call = (b2b_call_t *)c->call_key;
    int leg = c->leg;
    if (leg < 0 || leg > 1) return APP_RC_FAILED;

    /* 解析 action："start"(默认) / "stop" */
    int action = 1;
    if (data) {
        while (*data == ' ' || *data == '\t') ++data;
        if (strncasecmp(data, "stop", 4) == 0) action = 2;
        else if (strncasecmp(data, "start", 5) == 0) action = 1;
    }

    if (action == 1) {
        rec_sess_t *sess = NULL;
        char caller[64], callee[64];
        snprintf(caller, sizeof(caller), "%s", call->leg[0].from[0] ? call->leg[0].from : "");
        snprintf(callee, sizeof(callee), "%s", call->leg[1].to[0]   ? call->leg[1].to   : "");

        /* 从 rec_mgr 拿 base_dir 构造 rec_open_t（新版 rec_open 签名）*/
        rec_mgr_t *mgr = g_ctx ? (rec_mgr_t *)g_ctx->rec_mgr : NULL;
        const rec_mgr_cfg_t *mcfg = mgr ? rec_mgr_get_cfg(mgr) : NULL;
        const char *base = mcfg ? mcfg->base_dir : "/tmp/nsbc/recordings";

        rec_open_t req;
        memset(&req, 0, sizeof(req));
        req.mode = REC_MODE_SINGLE;
        req.leg_mask = (1 << leg);
        snprintf(req.callid, sizeof(req.callid), "%s", call->leg[0].callid);
        snprintf(req.reason, sizeof(req.reason), "app_record");
        snprintf(req.caller, sizeof(req.caller), "%s", caller);
        snprintf(req.callee, sizeof(req.callee), "%s", callee);
        snprintf(req.rec_path_template, sizeof(req.rec_path_template),
                 "{date}/{caller}-{callee}-{callid}.wav");

        int rc = rec_open(mgr, &req, &sess);
        if (rc != NSBC_OK || !sess) {
            llog(LOG_WARN, "rec", "app_record start failed callid=%s leg=%d rc=%d",
                 call->leg[0].callid, leg, rc);
            return APP_RC_FAILED;
        }
        reg_put(call->leg[0].callid, leg, sess, base, caller, callee);
        __atomic_store_n((void **)&call->rec[leg], sess, __ATOMIC_RELEASE);
        llog(LOG_INFO, "rec", "app_record START callid=%s leg=%d sess=%p",
             call->leg[0].callid, leg, (void *)sess);
    } else {
        rec_sess_t *sess = recording_find_by_callid(call->leg[0].callid, leg);
        if (sess) {
            char final_path[512]; uint64_t bytes = 0;
            reg_del(call->leg[0].callid, leg);
            rec_stop(sess, final_path, sizeof(final_path), &bytes);
            __atomic_store_n((void **)&call->rec[leg], NULL, __ATOMIC_RELEASE);
            llog(LOG_INFO, "rec", "app_record STOP callid=%s leg=%d path=%s bytes=%llu",
                 call->leg[0].callid, leg, final_path, (unsigned long long)bytes);
        }
    }
    return APP_RC_OK;
}

static app_rc_t app_record_session_exec(app_ctx_t *c, const char *data, void *ud)
{
    (void)data; (void)ud;
    if (!c || !c->call_key) return APP_RC_FAILED;
    b2b_call_t *call = (b2b_call_t *)c->call_key;
    app_ctx_t sub = *c;
    sub.leg = 0; app_record_exec(&sub, "start", NULL);
    sub.leg = 1; app_record_exec(&sub, "start", NULL);
    llog(LOG_INFO, "rec", "app_record_session callid=%s both legs", call->leg[0].callid);
    return APP_RC_OK;
}

/* bridge_add_rec_feed_hook 返回的 hook 节点指针（fini 时传 bridge_remove_rec_feed_hook）*/

static void reg_del(const char *callid, int leg)
{
    char key[REC_CALLID_MAX + 8];
    make_key(key, sizeof(key), callid, leg);
    pthread_mutex_lock(&g_reg_lock);
    uint32_t b = callid_hash(key);
    struct rec_bucket **pp = &g_reg[b];
    while (*pp) {
        if (strcmp((*pp)->callid, key) == 0) {
            struct rec_bucket *dead = *pp;
            *pp = dead->next;
            free(dead);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_reg_lock);
}

/* strip base_dir 前缀 → rec_path_out 永远是录音根下的相对路径
 * 不管 YAML 里 recording.path 写 "./recordings"、"../recordings" 还是绝对路径，
 * CDR 里存的 rec_path 都应该是 "20261011/caller-callee-callid.wav" 这种格式。
 * 这样前端拼 /recordings/ + rec_path → URL，后端 root.join(rec_path) → 文件，
 * 两端都不需要知道 base_dir 叫什么名字。 */
static const char *strip_base_dir(const char *path, const char *base_dir)
{
    if (!base_dir || !base_dir[0] || !path) return path;
    size_t bd_len = strlen(base_dir);
    if (strncmp(path, base_dir, bd_len) == 0) {
        const char *p = path + bd_len;
        if (*p == '/') p++;   /* 去掉 base_dir 后的那个 '/' */
        return p;
    }
    return path;
}

/* ---------- dp action handler ---------- */

/* dp_result.rec_action 取值：
 *   0 = 不动作（默认）
 *   1 = start recording（dialplan 触发开启）
 *   2 = stop recording（dialplan 触发停止，常见于 hangup_any 兜底） */
int recording_apply_dp_action(rec_mgr_t *mgr, uint8_t rec_action,
                              const char *callid, const char *reason,
                              const char *rec_path_template,
                              const char *caller, const char *callee,
                              char *rec_path_out, size_t path_cap)
{
    if (rec_path_out && path_cap > 0) rec_path_out[0] = '\0';
    if (mgr == NULL || callid == NULL || callid[0] == '\0')
        return NSBC_INVAL;
    if (rec_action == 0) return NSBC_OK;

    if (rec_action == 1) { /* start — 分腿独立录制 + 透传路径模板 */
        for (int leg = 0; leg < 2; ++leg) {
            if (reg_get(callid, leg) != NULL) continue;
            rec_open_t req = { 0 };
            snprintf(req.callid, sizeof(req.callid), "%s", callid);
            snprintf(req.reason, sizeof(req.reason),
                     reason ? reason : "dialplan");
            req.mode = REC_MODE_SINGLE;
            req.leg_mask = (leg == 0) ? 1 : 2;
            /* REQ-REC-007：路径模板变量透传 */
            if (rec_path_template && rec_path_template[0])
                snprintf(req.rec_path_template, sizeof(req.rec_path_template), "%s", rec_path_template);
            if (caller && caller[0])
                snprintf(req.caller, sizeof(req.caller), "%s", caller);
            if (callee && callee[0])
                snprintf(req.callee, sizeof(req.callee), "%s", callee);

            rec_sess_t *s = NULL;
            int rc = rec_open(mgr, &req, &s);
            if (rc != NSBC_OK) {
                llog(LOG_WARN, "rec", "recording_apply start leg=%d rc=%d callid=%s", leg, rc, callid);
            } else {
                reg_put(callid, leg, s, rec_path_template, caller, callee);
                llog(LOG_INFO, "rec", "recording START leg=%d (%s) callid=%s reason=%s",
                         leg, leg == 0 ? "A" : "B", callid, req.reason);
            }
        }
        return NSBC_OK;
    }

    if (rec_action == 2) { /* stop — 双腿停 → 合并成 stereo mix → 删 mono 中间文件 */
        int rc_all = NSBC_OK;
        char a_path[REC_PATH_MAX] = {0}, b_path[REC_PATH_MAX] = {0};
        const rec_mgr_cfg_t *mcfg = rec_mgr_get_cfg(mgr);
        const char *bd = mcfg ? mcfg->base_dir : "";

        /* REQ-REC-007：先从 bucket 拿模板/caller/callee（rec_stop 会 free sess）
         * 入参可能为 NULL（hangup/call_destroy 兜底路径没传）—— fallback 用 bucket 缓存 */
        char sess_tpl[128] = {0}, sess_caller[64] = {0}, sess_callee[64] = {0};
        pthread_mutex_lock(&g_reg_lock);
        struct rec_bucket *b0 = reg_get_bucket(callid, 0);
        if (b0) {
            snprintf(sess_tpl, sizeof(sess_tpl), "%s", b0->tpl);
            snprintf(sess_caller, sizeof(sess_caller), "%s", b0->caller);
            snprintf(sess_callee, sizeof(sess_callee), "%s", b0->callee);
        }
        pthread_mutex_unlock(&g_reg_lock);

        for (int leg = 0; leg < 2; ++leg) {
            rec_sess_t *s = reg_get(callid, leg);
            if (s == NULL) continue;
            char final_path[REC_PATH_MAX];
            uint64_t bytes = 0;
            int rc = rec_stop(s, final_path, sizeof(final_path), &bytes);
            reg_del(callid, leg);
            if (rc != NSBC_OK) rc_all = rc;
            if (final_path[0]) {
                if (leg == 0) snprintf(a_path, sizeof(a_path), "%s", final_path);
                else          snprintf(b_path, sizeof(b_path), "%s", final_path);
            }
            llog(LOG_INFO, "rec", "recording STOP leg=%d callid=%s rc=%d path=%s bytes=%llu",
                     leg, callid, rc, final_path, (unsigned long long)bytes);
        }

        /* 模板参数 fallback：入参有就用入参，没有就用 bucket 里 start 时缓存的 */
        const char *tpl = (rec_path_template && rec_path_template[0]) ? rec_path_template : sess_tpl;
        const char *cal = (caller && caller[0]) ? caller : sess_caller;
        const char *cl  = (callee && callee[0]) ? callee : sess_callee;
        llog(LOG_INFO, "rec", "recording MERGE tpl='%s' caller='%s' callee='%s'",
             tpl ? tpl : "", cal ? cal : "", cl ? cl : "");

        /* REQ-REC-008：合并两条 mono → stereo mix（A=左 B=右） */
        if (a_path[0] && b_path[0]) {
            /* 目录用 a_path 的目录（rec_open 已确保存在） */
            char mix_dir[REC_PATH_MAX];
            snprintf(mix_dir, sizeof(mix_dir), "%s", a_path);
            char *sl = strrchr(mix_dir, '/');
            if (sl) *sl = '\0';

            /* 模板只生成文件名部分（不传 base_dir，函数会自动加 date 前缀，
             * 但如果模板自己带了目录路径我们就只取最后一段文件名） */
            char mix_name[REC_PATH_MAX];
            resolve_rec_path_template(mix_name, sizeof(mix_name),
                                      tpl, cal, cl,
                                      callid, "");  // base_dir="" → 走默认 "./"
            /* 如果 mix_name 带了目录路径（模板里写了 path/name），提取最后的文件名 */
            char *last_sl = strrchr(mix_name, '/');
            if (last_sl) {
                /* 确保路径分隔符和系统一致，用最后一段作文件名 */
                char *fname = last_sl + 1;
                if (fname[0]) snprintf(mix_name, sizeof(mix_name), "%s", fname);
            }

            char mix_path[REC_PATH_MAX];
            snprintf(mix_path, sizeof(mix_path), "%s/%s", mix_dir, mix_name);

            if (merge_stereo_wav(a_path, b_path, mix_path) == NSBC_OK) {
                llog(LOG_INFO, "rec", "recording MERGE callid=%s mix=%s", callid, mix_path);
                unlink(a_path);
                unlink(b_path);
                if (rec_path_out && path_cap > 0)
                    snprintf(rec_path_out, path_cap, "%s", strip_base_dir(mix_path, bd));
            } else {
                llog(LOG_WARN, "rec", "recording MERGE failed callid=%s — mono files kept", callid);
                if (rec_path_out && b_path[0] && path_cap > 0)
                    snprintf(rec_path_out, path_cap, "%s", strip_base_dir(b_path, bd));
            }
        } else if (a_path[0]) {
            if (rec_path_out && path_cap > 0)
                snprintf(rec_path_out, path_cap, "%s", strip_base_dir(a_path, bd));
        }

        return rc_all;
    }

    return NSBC_INVAL;
}

/* media 快路径喂帧 — callctrl/bridge 的 PCM 回调里调用。
 * leg=-1 表示上层已合并好双腿（MIXED 模式）。分腿独立录制后本函数保留兼容性，
 * 实际媒体桥 raw 路径用 rec_feed_raw + call->rec[leg] 直接分发。 */
int recording_feed_frame(const char *callid, int leg, const media_frame_t *frm)
{
    int l = (leg < 0) ? 0 : leg;
    rec_sess_t *s = reg_get(callid, l);
    if (s == NULL && leg < 0) s = reg_get(callid, 1); /* fallback */
    if (s == NULL) return NSBC_OK;
    return rec_feed(s, leg, frm);
}

/* ---------- module init / fini ---------- */

static int recording_init(struct nsbc_ctx *ctx)
{
    if (ctx == NULL) return NSBC_INVAL;

    rec_mgr_cfg_t mc = { 0 };
    snprintf(mc.base_dir, sizeof(mc.base_dir), "/var/lib/nsbc/recordings");
    mc.fmt = REC_FMT_WAV_PCM;
    mc.rate = 8000;
    mc.channels = 1;
    mc.max_concurrent = 128;
    mc.encrypt_rest = 0;
    mc.max_bytes = 512ULL * 1024 * 1024;

    if (ctx->cfg) {
        const char *s;
        int v;
        long lv;
        (void)cfg_get_str(ctx->cfg, "recording.path", &s, NULL);
        if (s && s[0]) {
            /* recording.path 相对 nsbcd 二进制所在目录解析（对齐 nsbc-web）
             * 绝对路径原样返回；相对路径拼 exe_dir。 */
            if (s[0] == '/') {
                snprintf(mc.base_dir, sizeof(mc.base_dir), "%s", s);
            } else {
                char exe_dir[PATH_MAX] = "";
                char resolved[PATH_MAX];
                ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
                if (len > 0) {
                    exe_dir[len] = '\0';
                    char *sl = strrchr(exe_dir, '/');
                    if (sl) *sl = '\0';
                    if (exe_dir[0]) {
                        snprintf(resolved, sizeof(resolved), "%s/%s", exe_dir, s);
                        snprintf(mc.base_dir, sizeof(mc.base_dir), "%s", resolved);
                    } else {
                        snprintf(mc.base_dir, sizeof(mc.base_dir), "%s", s);
                    }
                } else {
                    snprintf(mc.base_dir, sizeof(mc.base_dir), "%s", s);
                }
            }
        }

        (void)cfg_get_bool(ctx->cfg, "recording.encrypt", &v, NULL);
        mc.encrypt_rest = v ? 1 : 0;

        (void)cfg_get_int(ctx->cfg, "recording.max_concurrent", &lv, NULL);
        if (lv > 0 && lv < 4096) mc.max_concurrent = (int)lv;

        (void)cfg_get_int(ctx->cfg, "recording.max_mb_per_file", &lv, NULL);
        if (lv > 0) mc.max_bytes = (uint64_t)lv * 1024 * 1024;

        (void)cfg_get_str(ctx->cfg, "recording.format", &s, NULL);
        if (s && strcmp(s, "opus") == 0) mc.fmt = REC_FMT_OGG_OPUS;
        else mc.fmt = REC_FMT_WAV_PCM;
    }

    rec_mgr_t *mgr = NULL;
    int rc = rec_mgr_create(&mc, &mgr);
    if (rc != NSBC_OK) {
        llog(LOG_WARN, "rec", "recording_init rec_mgr_create failed rc=%d — recording disabled", rc);
        return NSBC_OK;
    }
    ctx->rec_mgr = mgr;
    llog(LOG_INFO, "rec", "recording init OK base=%s fmt=%s concurrent=%d",
             mc.base_dir, mc.fmt == REC_FMT_WAV_PCM ? "wav" : "opus", mc.max_concurrent);

    /* Phase 0: 自注册 bridge rec_feed hook（替代 signaling.c cfg.rec_feed_fn 硬编码）*/
    g_ctx = ctx;
    bridge_mgr_t *br = sig_bridge_instance(ctx);
    if (br) {
        g_rec_bridge_hook = bridge_add_rec_feed_hook(br, recording_rec_feed, NULL);
        if (g_rec_bridge_hook)
            llog(LOG_INFO, "rec", "Phase 0: bridge rec_feed_hook registered bridge=%p", (void *)br);
        else
            llog(LOG_WARN, "rec", "Phase 0: bridge_add_rec_feed_hook returned NULL");
    } else {
        llog(LOG_WARN, "rec", "Phase 0: sig_bridge_instance() null — bridge hook skipped (OAM restart?)");
    }

    /* Phase 0: 自注册 dialplan apps — recording 模块自己实装 record/record_session，
     * app_registry.c builtin_apps[] 里移除 stub 占位 */
    app_reg_t *reg = dp_sleep_app_reg();
    if (reg) {
        app_desc_t rec_desc = {
            .name = "record",
            .help = "启动/停止当前 leg 录音（data=start|stop，默认 start）",
            .fn   = app_record_exec,
            .ud   = NULL,
        };
        app_desc_t rs_desc = {
            .name = "record_session",
            .help = "启动整条通话全生命周期录音（A+B 双 leg）",
            .fn   = app_record_session_exec,
            .ud   = NULL,
        };
        int arc1 = app_register(reg, &rec_desc);
        int arc2 = app_register(reg, &rs_desc);
        llog(LOG_INFO, "rec", "Phase 0: app_register record=%d record_session=%d", arc1, arc2);
    } else {
        llog(LOG_WARN, "rec", "Phase 0: dp_sleep_app_reg() null — apps not registered");
    }

    return NSBC_OK;
}

static void recording_fini(struct nsbc_ctx *ctx)
{
    /* Phase 0: 先清理自注册（rec_mgr_destroy 之前，因为 bridge hook 回调还需要 rec_mgr）*/
    if (g_rec_bridge_hook && ctx) {
        bridge_mgr_t *br = sig_bridge_instance(ctx);
        if (br) {
            bridge_remove_rec_feed_hook(br, g_rec_bridge_hook);
            llog(LOG_INFO, "rec", "Phase 0: bridge rec_feed_hook removed");
        }
        g_rec_bridge_hook = NULL;
    }
    app_reg_t *reg = dp_sleep_app_reg();
    if (reg) {
        app_unregister(reg, "record");
        app_unregister(reg, "record_session");
        llog(LOG_INFO, "rec", "Phase 0: apps unregistered");
    }

    /* 兜底：所有未 stop 的 sess 强制 stop（掉电恢复前 flush 落盘） */
    if (ctx) {
        for (size_t b = 0; b < REC_BUCKET_CNT; ++b) {
            struct rec_bucket *n = g_reg[b];
            while (n) {
                struct rec_bucket *next = n->next;
                rec_stop(n->sess, NULL, 0, NULL);
                free(n);
                n = next;
            }
            g_reg[b] = NULL;
        }
        if (ctx->rec_mgr) {
            rec_mgr_destroy((rec_mgr_t *)ctx->rec_mgr);
            ctx->rec_mgr = NULL;
        }
    }
    g_ctx = NULL;
}

/* ---------- dp_rule_t.rec_action 字符串解析 ----------
 *
 * 支持以下 dialplan 规则属性语法（trim 前后空白 + case-insensitive）：
 *   recording=start       → 1
 *   recording=stop        → 2
 *   recording             → 1（只有键名，没值默认 start）
 *   其他 / 空             → 0 */
uint8_t recording_parse_action(const char *s)
{
    if (s == NULL) return 0;
    /* 去前后空白 */
    while (*s == ' ' || *s == '\t') ++s;
    const char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) --end;
    size_t len = (size_t)(end - s);
    if (len == 0) return 0;

    /* 直接是 recording / recording=start / recording=stop 形式 */
    if ((len >= 9 && strncasecmp(s, "recording", 9) == 0)) {
        if (len == 9) return 1;                               /* recording → start */
        if (s[9] == '=' || s[9] == ':') {
            const char *v = s + 10;
            if (strncasecmp(v, "start", 5) == 0) return 1;
            if (strncasecmp(v, "stop", 4) == 0) return 2;
        }
        return 0;
    }
    return 0;
}

NSBC_MOD0_CAT(recording, 1, MOD_L_FINAL, 0, MOD_CAT_MEDIA_BUG, recording_init, recording_fini);
