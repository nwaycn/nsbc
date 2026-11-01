/* 实时计费 + CDR 采集接线（REQ-PBX-BILL-001 / REQ-PBX-CDR-001）
 *
 * P0：模块启动时接线 cdr_sink 到 ctx（emit 回调 + ud），让 emit 路径真的有地方进。
 * P1：persist_batch 优先写 DBAL，失败降级 spool.bin 文件；启动时 replay 把遗留
 *     spool push 进 ring 先落 DBAL。
 *
 * Phase 0：从 ctx->cdr_emit 硬编码桥接 → b2b_cdr_hook_add 多槽自注册
 * Phase 6：接 cdr_wal 降级链 — DBAL（stub）失败 → cdr_wal（带 CRC + 文件头 + replay truncate）→ spool.bin
 *
 * 降级链：DBAL dbal_cdr_batch → cdr_wal_append → spool 文件
 * 启动时 replay 顺序：wal（优先，因为比 spool 新且可校验）→ spool（兜底）
 * cdr_wal replay 完会 truncate 文件（已落库不再需要）*/

#include "rating.h"        /* billing_t / bill_create / bill_destroy — 费率引擎 */
#include "app_ctx.h"       /* struct nsbc_ctx 完整定义 — billing_init/fini 访问 ctx->data_dir/billing */
#include "b2bua.h"         /* Phase 0: b2b_cdr_hook_add / b2b_cdr_hook_rm */
#include "cdr.h"           /* cdr_sink_t / cdr_sink_* — billing.c 全部 cdr_sink 声明都在这里 */
#include "cdr_wal.h"       /* Phase 6: cdr_wal 兜底 */
#include "cdr_event.h"
#include "log.h"
#include <time.h>
#include "metric_reg.h"
#include "module.h"
#include "nsbc_atomic.h"
#include "uDS_client.h"    /* G1: uds_push — END settle 后推 billing_settle 给 Rust 侧 */
#include <cjson/cJSON.h>   /* G1: cJSON 构建 billing_settle payload */
#include "cfg.h"           /* Phase 2: cfg_get_bool / cfg_event_fn 热重载回调 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#ifdef __cplusplus
}
#endif

/* ---- 模块全局（billing.c 私有） ---- */
static cdr_sink_t   *g_sink   = NULL;  /* CDR sink（emit + flush 入口）*/
static billing_t    *g_billing = NULL; /* 费率/余额引擎（init 时创建；reload 后是悬挂指针，勿直接读！）*/
static nsbc_ctx_t   *g_ctx    = NULL;  /* 对 nsbc_ctx 的弱引用（RCU reload 后 ctx->billing 才是真相）*/
static cdr_wal_t    *g_wal    = NULL;  /* Phase 6: WAL 兜底（比 spool 强：有 CRC + 文件头 + replay truncate）*/

/* ★ 统一入口：所有读 g_billing 的地方必须调 B() —— RCU reload 后
 * ctx->billing 是唯一真相；g_billing 在 rate.reload 后指向已 destroy 的旧对象 */
static inline billing_t *B(void) { return g_ctx ? g_ctx->billing : g_billing; }

/* persist_batch 回调：先试 DBAL dbal_cdr_batch；失败降级 cdr_wal；再失败才 spool。
 *
 * Phase 6 降级链：
 *   1. DBAL dbal_cdr_batch（当前 stub，返回 NOTSUP）
 *   2. cdr_wal_append 循环（带 CRC + 文件头 + replay truncate，比 spool 安全）
 *   3. spool 文件（cdr_sink_flush 内部自动降级，persist_batch 返回错误即可）
 *
 * 返回 NSBC_OK = 本批全落 cdr_wal，NSBC_ERR/NSBC_NOTSUP = 让 sink 走 spool。*/
static int billing_persist_batch(void *ud, const cdr_event_t *ev, int n)
{
    (void)ud;

    /* Phase 6: DBAL 当前 stub — 直接降级 cdr_wal
     * （未来 DBAL 接好后：先 dbal_cdr_batch，失败才 cdr_wal）*/
    if (g_wal && ev && n > 0) {
        int wal_ok = 1;
        for (int i = 0; i < n; i++) {
            int rc = cdr_wal_append(g_wal, &ev[i]);
            if (rc != NSBC_OK) {
                log_warn("billing", "Phase 6: cdr_wal_append rc=%d at ev[%d]", rc, i);
                wal_ok = 0;
                break;
            }
        }
        if (wal_ok) {
            /* 本批全落 WAL → 返回 OK（不再走 spool）*/
            return NSBC_OK;
        }
        /* WAL 也挂了 → 返回错误让 cdr_sink_flush 降级 spool */
        log_err("billing", "Phase 6: cdr_wal batch failed, spooling %d events", n);
    }
    return NSBC_ERR;
}

/* Phase 0: b2b_cdr_hook_add 回调 —— b2bua call_end 三阶段（START/ANSWER/END）
 * 多槽链里 billing 自己注册的入口。内部直接调 cdr_sink_emit(ud, ev)，
 * ud 就是 billing_init 时传的 g_sink。比旧 ctx->cdr_emit 路径更强：
 * 收到完整三阶段 CDR（START 记开始时间、ANSWER 记应答时间、END 记挂断原因）。
 *
 * Phase 1 新增：在 CDR 事件里接 reserve/settle 两阶段计费：
 *   ANSWER → rate_lookup_stair 拿到费率 → 算保守 reserve 金额 → bill_reserve
 *   END    → calc_stair_charge 算实际费用 → bill_settle（多退少补）
 *   账户 key 推导：出向=分机付费，入向=中继付费，域内=主叫付费。*/

/* ---- account key 推导 ----
 * 格式: "tenant:<tid>:extension:<num>" 或 "tenant:<tid>:trunk:<name>" */
static void billing_make_key(const cdr_event_t *ev, char *out, size_t out_sz)
{
    const char *tid = (ev->tenant[0] == '\0') ? "1" : ev->tenant;
    const char *what, *who;
    switch (ev->dir) {
    case CDR_DIR_OUTBOUND:
        what = "extension"; who = ev->from; break;
    case CDR_DIR_INBOUND:
        what = "trunk";
        who = (ev->trunk[0] != '\0') ? ev->trunk : ev->to;
        break;
    case CDR_DIR_INTERNAL:
    default:
        what = "extension"; who = ev->from; break;
    }
    snprintf(out, out_sz, "tenant:%s:%s:%s", tid, what, who);
}

/* ---- 当前时间 HH:MM ---- */
static void billing_now_hhmm(char *buf, size_t sz)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, sz, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static void billing_cdr_cb(void *ud, const cdr_event_t *ev)
{
    cdr_sink_t *sink = (cdr_sink_t *)ud;
    if (!sink || !ev) return;
    cdr_sink_emit(sink, ev);  /* 原有 CDR 落库 —— 始终先做 */

    /* Phase 2: billing.enabled 开关 —— 关闭时跳过所有 reserve/settle/pre-auth */
    billing_t *bl = B();
    if (!bl || !bill_is_enabled(bl)) return;

    char acct_key[128];
    billing_make_key(ev, acct_key, sizeof(acct_key));

    switch (ev->phase) {

    /* ---- ANSWER: 接通 → reserve ----
     * 查费率（阶梯），按保守金额预占。失败不阻断通话（pre-paid 可以在 INVITE 入口拦截，
     * 这里是 post-hoc，只记日志）。*/
    case CDR_PHASE_ANSWER: {
        if (!ev->to[0]) break;
        rate_entry_t tiers[8];
        int n_tiers = 0;
        char hhmm[8];
        billing_now_hhmm(hhmm, sizeof(hhmm));

        /* Phase 1.5 L1: trunk_name → trunk_id（没映射时传 0 表示"不限"兜底）*/
        int64_t tr_id = 0;
        if (ev->trunk[0] && bl)
            tr_id = billing_lookup_trunk_id(bl, ev->trunk);

        int rc = rate_lookup_stair(bl, ev->to, tr_id, hhmm, NULL,
                                   tiers, &n_tiers, 8);
        if (rc != NSBC_OK || n_tiers == 0) {
            log_warn("billing", "ANSWER %s rate_lookup_stair rc=%d — no reserve",
                     ev->call_id, rc);
            break;
        }

        /* 保守 reserve = 第一阶梯 max(min_bill_s, 120秒) 的费率 + 接通费 × 2 */
        uint32_t reserve_sec = (uint32_t)tiers[0].min_bill_s;
        if (reserve_sec < 120) reserve_sec = 120;
        int64_t reserve = calc_stair_charge(tiers, n_tiers, reserve_sec);
        if (reserve < 0) reserve = 0;
        /* 再保守一点：× 2 + 接通费 */
        reserve = reserve * 2 + tiers[0].connect_fee_uc;

        uint64_t resv_id = 0;
        rc = bill_reserve(bl, acct_key, reserve, ev->call_id, &resv_id);
        if (rc == NSBC_OK) {
            /* Phase 2 L4: 成功后补填 called/trunk_id —— tick reserve bump 精确费率查找需要 */
            bill_resv_fill(bl, resv_id, ev->to, tr_id);

            log_info("billing", "ANSWER %s reserved=%lld uc acct=%s resv_id=%llu",
                     ev->call_id, (long long)reserve, acct_key, (unsigned long long)resv_id);
        } else {
            log_warn("billing", "ANSWER %s bill_reserve rc=%d reserve=%lld uc acct=%s",
                     ev->call_id, rc, (long long)reserve, acct_key);
        }
        break;
    }

    /* ---- END: 挂断 → settle ----
     * 用实际 billsec 算阶梯费用，多退少补。
     * 未应答（billsec=0）只收接通费（如果费率有 connect_fee）。*/
    case CDR_PHASE_END: {
        if (!ev->to[0]) break;
        rate_entry_t tiers[8];
        int n_tiers = 0;
        char hhmm[8];
        billing_now_hhmm(hhmm, sizeof(hhmm));

        /* Phase 1.5 L1: 同 ANSWER — 用 trunk_id 做精确费率匹配 */
        int64_t tr_id = 0;
        if (ev->trunk[0] && bl)
            tr_id = billing_lookup_trunk_id(bl, ev->trunk);

        int rc = rate_lookup_stair(bl, ev->to, tr_id, hhmm, NULL,
                                   tiers, &n_tiers, 8);
        if (rc != NSBC_OK || n_tiers == 0) {
            log_warn("billing", "END %s no rate match — no settle", ev->call_id);
            /* 尝试释放所有 reservation（用 call_id 匹配 resv 的 ai 字段？）*/
            break;
        }

        /* 实际费用 */
        int64_t charge = calc_stair_charge(tiers, n_tiers, ev->billsec);

        /* settle：按 call_id + acct_key 精确匹配 reservation —— 同账户多通话并发不会错乱 */
        rc = bill_settle(bl, acct_key, ev->call_id, charge);
        if (rc == NSBC_OK) {
            log_info("billing", "END %s billsec=%u charge=%lld uc acct=%s settle=OK",
                     ev->call_id, ev->billsec, (long long)charge, acct_key);

            /* G1: 推 billing_settle → Rust 侧 uds_server 收到后调 settle_billing() 写 DB 流水
             * payload 契约与 web/src/uds_server.rs dispatch_push("billing_settle") 对齐 */
            {
                cJSON *p = cJSON_CreateObject();
                cJSON_AddStringToObject(p, "account_key", acct_key);
                cJSON_AddStringToObject(p, "call_id",     ev->call_id);
                cJSON_AddNumberToObject(p, "charge_uc",   (double)(-charge));  /* 负数 = 扣费 */
                cJSON_AddNumberToObject(p, "billsec",     (double)ev->billsec);
                if (ev->tenant[0])  cJSON_AddStringToObject(p, "tenant",  ev->tenant);
                if (ev->trunk[0])   cJSON_AddStringToObject(p, "trunk",   ev->trunk);
                char *json = cJSON_PrintUnformatted(p);
                cJSON_Delete(p);
                if (json) {
                    (void)uds_push("billing_settle", json);  /* 非阻塞，内部 ring 满了丢最旧 */
                    free(json);
                }
            }
        } else {
            log_warn("billing", "END %s bill_settle rc=%d charge=%lld uc acct=%s",
                     ev->call_id, rc, (long long)charge, acct_key);
        }

        /* 如果账户还有低额阈值，发告警 */
        int64_t bal = 0, resv = 0;
        if (account_balance(bl, acct_key, &bal, &resv) == NSBC_OK) {
            if (bal < 0) {
                log_warn("billing", "END %s account OVERDRAWN bal=%lld uc acct=%s",
                         ev->call_id, (long long)bal, acct_key);
                /* Phase 2: 超过 credit_limit 时 kill 同账户其他通话 */
            }
        }
        break;
    }

    case CDR_PHASE_START:
        /* 暂不做 pre-auth — Phase 2 在 INVITE 入口接 b2b_guard_hook_add */
        break;
    }
}

static int billing_init(struct nsbc_ctx *ctx)
{
    cdr_ops_t ops = { 0 };
    char spool_path[512];
    char wal_path[512];
    int rc;

    if (ctx == NULL) return NSBC_INVAL;
    g_ctx = ctx;  /* ★ 记住 ctx — RCU reload 后 ctx->billing 才是唯一真相 */

    /* Phase 6: 先构 WAL 路径（比 spool 新，replay 优先）*/
    if (ctx->data_dir && ctx->data_dir[0]) {
        snprintf(spool_path, sizeof(spool_path), "%s/nsbc-cdr-spool.bin", ctx->data_dir);
        snprintf(wal_path,   sizeof(wal_path),   "%s/nsbc-cdr-wal.bin",     ctx->data_dir);
    } else {
        snprintf(spool_path, sizeof(spool_path), "./nsbc-cdr-spool.bin");
        snprintf(wal_path,   sizeof(wal_path),   "./nsbc-cdr-wal.bin");
    }

    /* Phase 6: 创建 cdr_wal（带 CRC + 文件头 + replay truncate）*/
    rc = cdr_wal_create(wal_path, &g_wal);
    if (rc == NSBC_OK) {
        cdr_wal_set_global(g_wal);  /* Phase 6: 全局入口供 OAM show cdr_wal */
        /* 先 replay WAL（优先，CRC 校验 + 完 truncate）*/
        int n = cdr_wal_replay(g_wal);
        if (n > 0)
            log_info("billing", "Phase 6: WAL replay ok events=%d", n);
        else
            log_debug("billing", "Phase 6: WAL empty or truncated, no replay needed");
    } else {
        log_warn("billing", "Phase 6: cdr_wal_create rc=%d, falling back to spool only", rc);
    }

    ops.persist_batch = billing_persist_batch;
    ops.ud = NULL;
    ops.workers = ctx->wpool;

    rc = cdr_sink_create(&ops, &g_sink);
    if (rc != NSBC_OK) return rc;

    cdr_sink_set_spool_path(g_sink, spool_path);

    /* Phase 6: replay spool 兜底（WAL 之后）*/
    cdr_sink_replay_spool(g_sink);

    /* Phase 0: 多槽自注册 —— billing 不再硬编码 ctx->cdr_emit 桥接，
     * 改为 b2b_cdr_hook_add(billing_cdr_cb, g_sink) 自注册到 b2bua.c call_end
     * 三阶段 CDR 链。signaling.c 的 ctx->cdr_emit END 路径保留作 fallback。 */
    int arc = b2b_cdr_hook_add(billing_cdr_cb, g_sink);
    log_info("billing", "Phase 0: b2b_cdr_hook_add billing arc=%d", arc);

    /* OAM cdr.recent / cdr.flush / show cdr 从 ctx->cdr_ud 取 sink
     * （obs_control.c 强转）。Phase 0 多槽重构时该挂接丢失，导致
     * Web poller 的 cdr.recent 兜底拉取恒为空（cdrs=0）、write-through
     * 短暂断链期间 CDR 静默丢失且无日志可查。fini 中已有对应置 NULL。 */
    ctx->cdr_ud = g_sink;

    /* 创建费率/账户管理器并挂接（OAM show rates/balances 依赖） */
    rc = bill_create(&g_billing);
    if (rc != NSBC_OK) {
        log_warn("billing", "bill_create failed rc=%d (OAM show rates/balances unavailable)", rc);
    } else {
        ctx->billing = g_billing;

        /* Phase 2: 读 billing.enabled 全局开关（默认 false）*/
        int enabled = 0;
        if (ctx->cfg) {
            cfg_get_bool(ctx->cfg, "billing.enabled", &enabled, NULL);
        }
        bill_set_enabled(g_billing, enabled);
    }

    /* CDR hook 始终注册 —— billing_cdr_cb 内部已有 enabled guard
     * （CDR 落库始终要做，只是 reserve/settle/pre-auth 在开关关闭时跳过）。*/
    log_info("billing", "init OK (cdr_sink=%p billing=%p enabled=%d cdr_arc=%d)",
             g_sink, g_billing, g_billing ? bill_is_enabled(g_billing) : 0, arc);
    return NSBC_OK;
}

static void billing_fini(struct nsbc_ctx *ctx)
{
    /* Phase 0: 先卸载 hook（rec_feed 回调不再进来）*/
    if (g_sink) {
        b2b_cdr_hook_rm(billing_cdr_cb, g_sink);
        log_info("billing", "Phase 0: b2b_cdr_hook_rm billing");
        (void)cdr_sink_flush(g_sink);
        cdr_sink_destroy(g_sink);
        g_sink = NULL;
    }
    /* Phase 6: 先 flush WAL 残留（w->batch.count）再 destroy */
    if (g_wal) {
        cdr_wal_destroy(g_wal);
        g_wal = NULL;
    }
    /* ctx->billing 是唯一真相 —— rate.reload 在 obs_control.c 里直接写 ctx->billing = newb
     * 然后 bill_destroy(旧对象)；B() 辅助函数优先返回 ctx->billing，g_billing 在 reload 后
     * 成为悬挂指针但再也不会被任何热路径读到（所有读都走 B() / ctx->billing）。*/
    if (ctx != NULL && ctx->billing != NULL) {
        bill_destroy(ctx->billing);
        ctx->billing = NULL;
    }
    g_billing = NULL;  /* 清悬挂指针；不 destroy（已由 ctx->billing 覆盖） */
    g_ctx = NULL;      /* ★ 清 ctx 弱引用 — 模块卸载后不再读 */
    if (ctx != NULL) {
        ctx->cdr_emit = NULL;
        ctx->cdr_ud = NULL;
    }
}

NSBC_MOD0_CAT(billing, 1, MOD_L_FINAL, 0, MOD_CAT_BILLING, billing_init, billing_fini);

/* =========================================================================
 * Phase 2 L4+L5: billing_tick — 供 main.c ticker_main 周期性调用
 *
 * 职责：
 *   1. 从 YAML 读 billing.enabled（每 ~3s，够快）→ 热重载生效
 *   2. 调 billing_tick_internal → 遍历 resv[] + accts[] → OVERDRAWN 门闩
 *   3. 释放 billing 锁后，对 kill_list 里的每个 call_id 调 b2b_terminate_callid
 *
 * 锁序安全：billing_tick_internal 返回时 billing 锁已释放；
 *           b2b_terminate_callid 内部自己管理 shard_lock + call_lock，不碰 billing 锁。
 * ========================================================================= */

/* tick 节流：上次 tick 时刻 —— 每 3s 真正跑一次 */
static uint64_t g_last_tick_ms = 0;
#define BILL_TICK_INTERVAL_MS 3000

int billing_tick(nsbc_ctx_t *ctx)
{
    if (!ctx) return NSBC_INVAL;

    /* 节流 */
    uint64_t now = nsbc_now_ms();
    if (g_last_tick_ms && (now - g_last_tick_ms) < BILL_TICK_INTERVAL_MS)
        return NSBC_OK;
    g_last_tick_ms = now;

    /* ★ 唯一真相：ctx->billing —— rate.reload RCU 换的就是这个
     * g_billing 在 reload 后指向已 destroy 的旧对象，严禁在此处读！*/
    billing_t *bl = ctx->billing;

    /* 从 cfg 同步 enabled —— 热重载（control reload / config.set billing.enabled=true）后自动生效
     * cfg_get_bool 只从内存 cfg 树读，极快，每 3s 一次零压力。 */
    if (ctx->cfg && bl) {
        int enabled = 0;
        if (cfg_get_bool(ctx->cfg, "billing.enabled", &enabled, NULL) == NSBC_OK)
            bill_set_enabled(bl, enabled);
    }

    if (!bl || !bill_is_enabled(bl))
        return NSBC_OK;

    /* 1. 遍历 resv + acct → OVERDRAWN kill_list */
    char kill_list[256][40];
    int kill_sz = 0;
    int rc = billing_tick_internal(bl, kill_list, &kill_sz);
    if (rc != NSBC_OK) return rc;
    if (kill_sz == 0) return NSBC_OK;

    /* 2. 释放 billing 锁后调 b2b_terminate_callid —— 锁序安全
     *    cause_code=41 (NORMAL_CLEARING)；CDR END 后正常走 settle → billing_settle push → DB 落库。*/
    b2b_core_t *co = (b2b_core_t *)ctx->b2b;
    if (!co) {
        log_warn("billing", "tick: no b2b core, can't kill %d OVERDRAWN calls", kill_sz);
        return NSBC_OK;
    }

    for (int i = 0; i < kill_sz; ++i) {
        int krc = b2b_terminate_callid(co, kill_list[i], strlen(kill_list[i]), 41);
        if (krc == NSBC_OK) {
            llog(LOG_INFO, "billing", "OVERDRAWN killed callid=%s", kill_list[i]);
        } else {
            llog(LOG_WARN, "billing", "OVERDRAWN kill failed callid=%s rc=%d",
                 kill_list[i], krc);
        }
    }
    return NSBC_OK;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
