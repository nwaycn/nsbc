#ifndef NSBC_RATING_H
#define NSBC_RATING_H

/* 费率与实时计费（REQ-PBX-BILL-001~005，P1 实时信用控制）
 *
 *  - 费率表：目的码前缀最长匹配，单价=分/60 秒（最小计费单位
 *    bill_inc_s + 最小收费时长 min_bill_s + 接通费 connect_fee）；
 *  - 账户余额（租户/分机/中继客户三口径，粗骨架统一 account key）：
 *    呼叫前 reserve 预占，周期扣费（P1 60s 一次，CDR END 结算多退少补），
 *    余额触阈发催缴/强制拆线事件（事件由组合根执行）；
 *  - 时区/工作日时段费率（P2 先留 band 槽，本批只实现单一费率）。
 * 金额单位统一"微元"（1e-6 元，int64）避免浮点。
 *
 * 并发：billing_t 内部自带 pthread_mutex_t lock，所有 public API 入口
 * 加锁、出口解锁（包括只读迭代）。rate.reload 的 RCU 原子换指针在
 * 外部由 obs_control.c 完成（建新表→bill_destroy 旧表→替换 ctx->billing），
 * 旧表的 destroy 在 uniq 之后。
 */
#include "nsbc_common.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RATE_PREFIX_MAX  24
#define RATE_PLAN_MAX    4096
#define BILL_ACCT_MAX    200000
#define BILL_ACCT_KEY    64
#define BILL_NAME_MAX    48

/* 费率条目：同时承载单费率（rate_tables）和阶梯费率（rate_plan_entries）两种模型。
 *
 * 单费率兼容：tier_min_sec=0, tier_max_sec=0 → 视为全区间单一价格
 * 阶梯费率   ：同一 prefix 多条，按 tier_min_sec 升序排序。calc_stair_charge 按阶梯分段算。
 *
 * trunk_id / time_from / time_to / weekdays：NULL 或 0 表示无此维度限制
 * precedence：同 prefix + trunk + time 条件下优先级（数值大优先） */
typedef struct {
    char     prefix[RATE_PREFIX_MAX];
    char     name[BILL_NAME_MAX];
    int64_t  price_per_min_uc;  /* 微元/分钟 */
    int64_t  connect_fee_uc;    /* 接通费（微元） */
    int      bill_inc_s;        /* 计费进位（1=秒, 6=6秒, 60=分钟） */
    int      min_bill_s;        /* 最小收费秒数 */
    int      initial_s;         /* 首计费块秒数（0=按 bill_inc） */
    int      enabled;

    /* === 阶梯字段（新增，向后兼容） === */
    int64_t  tier_min_sec;      /* 本阶梯起点秒数（0=第一阶梯） */
    int64_t  tier_max_sec;      /* 本阶梯终点秒数（0=无上限） */

    /* === 中继/时段维度（新增，向后兼容 0=不限制） === */
    int64_t  trunk_id;          /* 中继 ID（0=所有中继通用） */
    int64_t  rate_plan_id;      /* 所属费率方案（0=rate_tables 表旧条目） */
    char     time_from[8];      /* "HH:MM" 起始时段（"" = 全时段） */
    char     time_to[8];        /* "HH:MM" 结束时段 */
    char     weekdays[16];      /* "1,2,3,4,5" 或 "" = 每天 */
    int      precedence;        /* 同条件优先级（数值大优先） */
} rate_entry_t;

typedef struct {
    char     prefix[RATE_PREFIX_MAX];
    int64_t  connect_fee_uc;
    int64_t  charge_uc;
    uint32_t billsec_charged;
} rate_quote_t;

typedef struct billing billing_t;
typedef struct nsbc_ctx nsbc_ctx_t;

int  bill_create(billing_t **out);
void bill_destroy(billing_t *b);

/* 费率管理（长前缀优先生效；同前缀覆盖更新） */
int  rate_upsert(billing_t *b, const rate_entry_t *r);
int  rate_delete(billing_t *b, const char *prefix);  /* 按前缀删除，不存在返回 NOENT */
int  rate_lookup(billing_t *b, const char *called, rate_entry_t *out);
/* 报价：按 billsec 计算费用（未达最小收费用 min_bill；首块 initial） */
int  rate_quote(const rate_entry_t *r, uint32_t billsec, rate_quote_t *q);

/* === 阶梯/多维费率 ===
 *
 * rate_lookup_stair 按 called + trunk_id + now() 查匹配的所有阶梯条目
 * (同一 prefix 可能多条 tier)，输出排序后的数组。n_tiers_out 为输出条目数。
 * 返回 NSBC_OK = 找到（至少一条），NSBC_NOENT = 无匹配（默认费率 plan_id=0 兜底）。
 *
 * calc_stair_charge 按匹配到的 tiers 数组计算费用：逐阶梯分段、按各自 bill_inc_s
 * 取整、首阶梯用 min_bill_s 兜底。返回微元（int64）。 */
int  rate_lookup_stair(billing_t *b, const char *called, int64_t trunk_id,
                       const char *time_from_cache, const char *weekdays_cache,
                       rate_entry_t *tiers_out, int *n_tiers_out,
                       int max_tiers);
int64_t calc_stair_charge(rate_entry_t *tiers, int n_tiers, uint32_t billsec);

/* rate_plan_entries.reload / rate_plans.reload — C 侧直接接收 JSON 数组 */
int  rate_plan_entries_reload_json(billing_t *b, const char *json);
int  rate_plans_reload_json(billing_t *b, const char *json);

/* 时段匹配辅助（给定 HH:MM 和 weekdays，返回 true = 命中） */
int  time_slot_match(const char *time_from, const char *time_to,
                     const char *weekdays, const char *now_hhmm, int now_wd);

/* 账户（balance 余额 / reserved 预占 / credit_limit 透支额度） */
int  account_open(billing_t *b, const char *key, int64_t balance_uc,
                  int64_t credit_limit_uc);
int  account_balance(billing_t *b, const char *key,
                     int64_t *balance, int64_t *reserved);
int  account_topup(billing_t *b, const char *key, int64_t delta_uc);

/* 呼叫前预占（不足返回 PERM_DENIED）。
 * call_id: SIP Call-ID 字符串，供 END 结算时精确匹配 reservation（避免同账户并发通话错乱）；
 *          可以传 NULL 或 ""，此时 reservation 不绑定 call_id（向后兼容旧调用点）。
 * resv_id 出参：供未接/失败时 bill_release(resv_id) 释放。*/
int  bill_reserve(billing_t *b, const char *key, int64_t amount_uc,
                  const char *call_id, uint64_t *resv_id);
/* 撤销预占（未接/失败，全额释放）。按 resv_id 匹配。 */
int  bill_release(billing_t *b, uint64_t resv_id);
/* 结算：预占转实收，差额多退少补。
 * 优先按 call_id + key 精确匹配 reservation；call_id 为 NULL 时按 key 找第一个。 */
int  bill_settle(billing_t *b, const char *key, const char *call_id,
                 int64_t actual_uc);

/* 低额阈值与告警回调（阈值在账户上，粗骨架全局一个阈值） */
typedef void (*bill_low_cb)(void *ud, const char *key, int64_t balance);
void bill_set_low_water(billing_t *b, int64_t threshold_uc,
                        bill_low_cb cb, void *ud);

/* OAM 只读迭代（REQ-OAM-009：show rates / show balances / show resv）。
 * 回调返回 NSBC_OK 继续，NSBC_ERR 提前终止；遍历顺序为内部定槽顺序，
 * 不保证前缀有序。 */
typedef int (*rate_iter_cb)(const rate_entry_t *r, void *ud);
typedef int (*acct_iter_cb)(const char *key, int64_t balance,
                            int64_t reserved, int64_t credit_limit, void *ud);
/* resv_iter_cb — show resv 迭代。
 * 参数: used=1 活跃, call_id, resv_id, account_key(acct->key),
 *       billsec(实时秒), reserved_uc, called, trunk_id */
typedef int (*resv_iter_cb)(int used, uint64_t resv_id,
                            const char *call_id, const char *account_key,
                            int64_t billsec, int64_t reserved_uc,
                            const char *called, int64_t trunk_id,
                            void *ud);
int bill_foreach_rate(billing_t *b, rate_iter_cb cb, void *ud);
int bill_foreach_account(billing_t *b, acct_iter_cb cb, void *ud);
int bill_foreach_reservation(billing_t *b, resv_iter_cb cb, void *ud);

/* 当前条目数（只读快照，无锁语义：仅用于 show 命令的容量预估） */
int bill_rate_count(billing_t *b);
int bill_account_count(billing_t *b);
int bill_reservation_count(billing_t *b);

/* accounts.reload <json> — OAM 全量账户同步（REQ-OAM-009 L1）。
 * JSON 数组元素：{account_key|"key", balance_uc|"balance",
 *                 credit_limit_uc|"credit_limit"}。
 * upsert-only：存在则覆盖 balance/credit，不存在则新建；**不删除**
 *              JSON 里没有的账户（resv[] 活跃 reservation 可能要结算）。
 * resv[] 表不动（在途 reserve/settle 继续生效）。
 * 复用 ivr.h 公开的 jstr/jint/jarray_at 手写 JSON 工具。*/
int account_reload_json(billing_t *b, const char *json);

/* INVITE pre-auth — 呼叫前快速检查余额够不够 reserve。
 *
 * 在 INVITE 入口（b2bua.c）security_check_fence 之后调用。
 * 根据 caller/callee 粗略推方向：caller 以 extension 开头 → 分机付费（出向/内部），
 * 否则 callee 是分机 → 中继付费（入向）。
 * reserve 金额 = rate_lookup_stair 第一阶梯 max(min_bill, 120s) × 2 + connect_fee。
 *
 * 返回 NSBC_OK = 允许（余额够 + 找到费率），
 *        NSBC_NOENT = 账户不存在或没匹配费率（不拦截，allow pass —— 让 ANSWER reserve 再拒），
 *        NSBC_PERM_DENIED = 余额不够 reserve（拒接，静默丢）。
 *
 * tenant: 可选（默认 NULL = "1"）；src_ip: 可选（用于未来 trunk 解析）。*/
int billing_pre_auth(billing_t *b, const char *caller, const char *callee,
                     const char *tenant, const char *src_ip);

/* === Phase 1.5 L1: trunk_name → trunk_id 映射 ===
 *
 * CDR 里 ev->trunk 是字符串（如 "china_unicom"），但 rate_plan_entries.trunk_id 是 int64。
 * 这两个函数让 billing 能把 trunk name 转换成 trunk_id 做精确匹配。*/
int64_t billing_lookup_trunk_id(billing_t *b, const char *trunk_name);
int     trunk_map_reload_json(billing_t *b, const char *json);

/* === Phase 2: billing.enabled 开关 ===
 * 单一字段 int，单字对齐自然原子，读写无需额外锁保护。
 * bill_is_enabled — 业务侧所有计费入口统一查询此 API（billing_cdr_cb / billing_pre_auth / billing_tick）。
 * bill_set_enabled — 热更新唯一写入点（cfg reload / config.set 回调）。*/
int  bill_is_enabled(billing_t *b);
void bill_set_enabled(billing_t *b, int enabled);

/* === Phase 2 L4: bill_reserve 后补填 called/trunk_id ===
 * bill_reserve 签名不变（向后兼容），内部填 answer_ms。
 * 调用方（billing.c CDR ANSWER）成功后调此函数补填精确费率查找所需字段。
 * 不补填也能跑（tick 保守模式跳过 reserve bump，只做 OVERDRAWN 检查）。*/
int  bill_resv_fill(billing_t *b, uint64_t resv_id,
                    const char *called, int64_t trunk_id);

/* === Phase 2 L4v2+L4v3 + L5: 精确费率 reserve 双向维护 + OVERDRAWN kill ===
 * 由 main.c ticker_main 每 ~3s 调一次（billing_tick 内部 3s 节流）。
 * 内部遍历 resv[] + accts[]：
 *   L4v2: tick_lookup_tiers_locked（已持 wrlock，内联最长前缀+trunk_id+时段过滤）
 *         + calc_stair_charge(billsec) → cumulative > resv->amount
 *         → a->reserved += delta, resv->amount = cumulative
 *   L4v3: cumulative < resv->amount 且差值 > 安全 guard
 *         → a->reserved -= refund, resv->amount = cumulative
 *         guard = connect_fee + price_per_min_uc × 120s / 60（防 bill_inc_s 抖动）
 *         费率不变时 cumulative 单调递增 → 只追加不收缩
 *   保守模式：resv 无 called/trunk_id → 跳过 reserve 维护
 *   L5:   OVERDRAWN 门闩（边沿检测 + 0.8 滞回防抖动）
 *         触发时收集同账户所有活跃 call_id
 *         L4v3 收缩减小 reserved → available 增大 → 门闩复位自动生效
 *   不变式：balance 永远只在 reserve/settle 里变。L4v2+L4v3 只动 reserved。
 * kill_list_out: 输出需 kill 的 call_id 数组（由调用者分配，最多 256 条）
 * kill_list_sz:  输出实际条目数
 * 外层（billing.c）释放 billing 锁后调 b2b_terminate_callid —— 锁序安全 */
int  billing_tick_internal(billing_t *b,
                           char kill_list_out[][40], int *kill_list_sz);

/* billing_tick — billing.c 模块对外入口。
 * main.c ticker_main 周期性调用（内部 3s 节流 + cfg 读 enabled）。*/
int  billing_tick(nsbc_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_RATING_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
