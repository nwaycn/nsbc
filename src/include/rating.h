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

typedef struct {
    char     prefix[RATE_PREFIX_MAX];
    char     name[BILL_NAME_MAX];
    int64_t  price_per_min_uc;  /* 微元/分钟 */
    int64_t  connect_fee_uc;    /* 接通费（微元） */
    int      bill_inc_s;        /* 计费进位（1/6 秒） */
    int      min_bill_s;        /* 最小收费秒数 */
    int      initial_s;         /* 首计费块秒数（0=按 bill_inc） */
    int      enabled;
} rate_entry_t;

typedef struct {
    char     prefix[RATE_PREFIX_MAX];
    int64_t  connect_fee_uc;
    int64_t  charge_uc;
    uint32_t billsec_charged;
} rate_quote_t;

typedef struct billing billing_t;

int  bill_create(billing_t **out);
void bill_destroy(billing_t *b);

/* 费率管理（长前缀优先生效；同前缀覆盖更新） */
int  rate_upsert(billing_t *b, const rate_entry_t *r);
int  rate_delete(billing_t *b, const char *prefix);  /* 按前缀删除，不存在返回 NOENT */
int  rate_lookup(billing_t *b, const char *called, rate_entry_t *out);
/* 报价：按 billsec 计算费用（未达最小收费用 min_bill；首块 initial） */
int  rate_quote(const rate_entry_t *r, uint32_t billsec, rate_quote_t *q);

/* 账户（balance 余额 / reserved 预占 / credit_limit 透支额度） */
int  account_open(billing_t *b, const char *key, int64_t balance_uc,
                  int64_t credit_limit_uc);
int  account_balance(billing_t *b, const char *key,
                     int64_t *balance, int64_t *reserved);
int  account_topup(billing_t *b, const char *key, int64_t delta_uc);

/* 呼叫前预占（不足返回 PERM_DENIED）；resv_id 出参供结算/撤销 */
int  bill_reserve(billing_t *b, const char *key, int64_t amount_uc,
                  uint64_t *resv_id);
/* 撤销预占（未接/失败，全额释放） */
int  bill_release(billing_t *b, uint64_t resv_id);
/* 结算：预占转实收，差额多退少补（余额不足时吃透支额度） */
int  bill_settle(billing_t *b, uint64_t resv_id, int64_t actual_uc);

/* 低额阈值与告警回调（阈值在账户上，粗骨架全局一个阈值） */
typedef void (*bill_low_cb)(void *ud, const char *key, int64_t balance);
void bill_set_low_water(billing_t *b, int64_t threshold_uc,
                        bill_low_cb cb, void *ud);

/* OAM 只读迭代（REQ-OAM-009：show rates / show balances）。
 * 回调返回 NSBC_OK 继续，NSBC_ERR 提前终止；遍历顺序为内部定槽顺序，
 * 不保证前缀有序。 */
typedef int (*rate_iter_cb)(const rate_entry_t *r, void *ud);
typedef int (*acct_iter_cb)(const char *key, int64_t balance,
                            int64_t reserved, int64_t credit_limit, void *ud);
int bill_foreach_rate(billing_t *b, rate_iter_cb cb, void *ud);
int bill_foreach_account(billing_t *b, acct_iter_cb cb, void *ud);

/* 当前费率条目数（只读快照，无锁语义：仅用于 show 命令的容量预估） */
int bill_rate_count(billing_t *b);
int bill_account_count(billing_t *b);

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
