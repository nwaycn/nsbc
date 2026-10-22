#ifndef NSBC_FRAUD_H
#define NSBC_FRAUD_H

/* 防盗打（REQ-FRAUD，P0 规则门禁 / P1 行为异常）
 *
 * 检查点：outbound 路由解析后、实际占用中继前（被拒不产生中继话务）。
 *  - P0 规则：目的码黑名单/白名单、账号并发上限、CPS 上限、日消费
 *    上限（与 billing 余额联动，阈值独立）、时间段禁打；
 *  - P1 异常：ASR（应答率）骤降、短时通话激增（试号特征）、单账号
 *    目的码发散度（单位时间不同被叫号个数）超阈；
 * 命中动作：拒绝（403/503 口径由呼叫侧映射）+ 事件上报告警；
 * 紧急呼叫必须在调用方提前豁免（见 emergency.h，本模块不判紧急号）。
 * 分层：mod（4）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAUD_KEY_MAX      64   /* 账号键：tenant:ext 或 tenant:trunk */
#define FRAUD_PREFIX_MAX   24
#define FRAUD_RULE_CAP     4096
#define FRAUD_SUBJ_CAP     20000
#define FRAUD_WIN_MS       3600000 /* 滑窗 1h（粗骨架单窗不滑） */

typedef enum {
    FRAUD_ACT_ALLOW = 0,
    FRAUD_ACT_DENY,
    FRAUD_ACT_WATCH
} fraud_action_t;

typedef enum {
    FRAUD_R_PREFIX_DENY = 0,   /* 目的码前缀黑名单 */
    FRAUD_R_PREFIX_ALLOW,      /* 白名单（非名单即拒，优先级最高） */
    FRAUD_R_MAX_CONCURRENT,    /* 并发上限 */
    FRAUD_R_MAX_CPS,           /* 每秒起呼上限 */
    FRAUD_R_DAILY_SPEND,       /* 日消费微元上限 */
    FRAUD_R_TIME_BAN,          /* 禁打时段（本地小时 0..23 位掩码） */
    FRAUD_R_ASR_DROP,          /* P1：应答率下限（%） */
    FRAUD_R_SHORT_BURST        /* P1：短通话（<10s）次数窗内上限 */
} fraud_rule_kind_t;

typedef struct {
    fraud_rule_kind_t kind;
    char     prefix[FRAUD_PREFIX_MAX]; /* 前缀类规则用 */
    int64_t  num;                       /* 上限/阈值/掩码 */
    int      enabled;
} fraud_rule_t;

typedef struct {
    char key[FRAUD_KEY_MAX];
    int  concurrent;
    int  calls_in_win;
    int  answered_in_win;
    int  short_in_win;
    uint32_t win_start_s;
    int64_t  spend_today_uc;
    uint32_t day_epoch;
} fraud_counters_t;

typedef struct fraud_guard fraud_guard_t;

int  fraud_create(fraud_guard_t **out);
void fraud_destroy(fraud_guard_t *g);

int  fraud_rule_add(fraud_guard_t *g, const char *key,
                    const fraud_rule_t *r);
int  fraud_rule_remove(fraud_guard_t *g, const char *key,
                       fraud_rule_kind_t kind, const char *prefix);

/* 起呼检查：now_s 本地 UNIX 秒（时段判定）；命中 DENY 返 PERM_DENIED，
 * WATCH 返 OK（调用方可记观察日志），全通过返 OK。 */
int  fraud_check(fraud_guard_t *g, const char *key, const char *called,
                 uint64_t now_ms, uint32_t now_s, int *hit_action);

/* 呼叫结束回写：answered=是否接起，billsec=计费秒，spend_uc=本呼叫
 * 消费（0=不计费）。调用顺序：check(OK) → … → call_end。 */
void fraud_call_begin(fraud_guard_t *g, const char *key, uint64_t now_ms);
void fraud_call_end(fraud_guard_t *g, const char *key, int answered,
                    uint32_t billsec, int64_t spend_uc,
                    uint64_t now_ms, uint32_t today_epoch);

int  fraud_counters(fraud_guard_t *g, const char *key,
                    fraud_counters_t *out);

const char *fraud_action_name(fraud_action_t a);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_FRAUD_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
