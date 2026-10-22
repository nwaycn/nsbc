#ifndef NSBC_CHAN_VAR_H
#define NSBC_CHAN_VAR_H

/* 通道变量引擎（REQ-CHAN-VAR-001，P0）
 *
 * FreeSWITCH 级 Channel Variables：按 call_session 生命周期挂载，
 * 两条 Leg 共享 session 级变量 + 各自 leg 级变量。键在 Arena 分配，
 * 零 glibc malloc 进入快路径；哈希表 64 桶 + 写时 mutex 保护
 * （信令 P99 路径读多写少，P1 升级 RCU 读无锁）。
 *
 * 命名空间约定：
 *   nsbc_*       —— 系统只读（如 nsbc_rtp_loss / nsbc_codec），set 返回 PERM_DENIED
 *   sip_h_*      —— 自动映射为出向 INVITE SIP 头域（sip_h_X-Foo → X-Foo）
 *   nolocal_*    —— 不写入 CDR（显式标记）
 *   *:           —— leg 级前缀（"0:var" / "1:var"），跨 leg 不可见
 *
 * Dialplan 联动：set / unset / export（跨 leg 提升）条件表达式
 * ${var} / ${var:-default} / ${1}（正则捕获组）在 P1 补充；
 * P0 提供基础 get/set + 条件求值器骨架。
 */

#include "mem.h"
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- flags ---- */
#define CV_F_SESSION    0    /* session 级（默认） */
#define CV_F_LEG_A      1    /* leg A 级 */
#define CV_F_LEG_B      2    /* leg B 级 */
#define CV_F_LEG_MASK   3

/* ---- 默认容量 ---- */
#define CV_BUCKETS        64
#define CV_KEY_MAX        128    /* 含 leg 前缀最大 127 字节 */
#define CV_VAL_MAX        512
#define CV_REASON_MAX     64

typedef struct chan_var_entry {
    char key[CV_KEY_MAX];     /* 完整键（含 "0:"/"1:" leg 前缀，或纯 session） */
    char val[CV_VAL_MAX];
    int  flags;
    struct chan_var_entry *next;
} chan_var_entry_t;

typedef struct chan_var {
    pthread_mutex_t lock;
    mem_arena_t *arena;
    chan_var_entry_t *buckets[CV_BUCKETS];
    uint32_t count;
} chan_var_t;

/* ---- 生命周期 ---- */
int  chan_var_create(mem_arena_t *arena, chan_var_t **out);
void chan_var_destroy(chan_var_t *v);

/* ---- 读写 API ----
 *
 * leg=-1 表示 session 级；leg=0 或 1 表示 leg 级。
 * nsbc_* 系统变量 set 时返回 PERM_DENIED（只读保护）。
 * key/value 为空或超上限返回 INVAL。
 * value 最大 CV_VAL_MAX-1 字节；超出静默截断（P0 口径，不报错）。 */
int  chan_var_set(chan_var_t *v, int leg, const char *key, const char *value);
int  chan_var_get(chan_var_t *v, int leg, const char *key,
                  char *out, size_t out_cap);  /* NOENT=不存在 */
int  chan_var_unset(chan_var_t *v, int leg, const char *key);

/* 条件表达式求值器（P0 骨架，P1 完整 Dialplan 支持）：
 *   "${var_name}"          → 变量值（缺省空串）
 *   "${var_name:-default}" → 变量不存在时返回 default
 *   "${var:-}"             → 变量不存在时返回空串
 * 支持嵌套："${a:-${b:-foo}}" 递归求值。
 * 求值失败返回 NULL 串 + INVAL（调用方按字面处理）。 */
int  chan_var_substitute(chan_var_t *v, int leg, const char *expr,
                         char *out, size_t out_cap);

/* ---- 批量导出（RECD/事件）：逐 entry 写 fmt */
typedef int (*chan_var_cb)(const char *key, const char *val, int leg, void *ud);
int  chan_var_foreach(chan_var_t *v, int flags, chan_var_cb cb, void *ud);

/* ---- 安全校验 ----
 * key：长度 1..CV_KEY_MAX-1、仅可见 ASCII、不以 'nsbc_' 开头且不在已知
 *   禁止列表里（nsbc_codec/nsbc_rtp_loss 等，只读）。
 * value：长度 1..CV_VAL_MAX-1、仅可见 ASCII（同 obs_control 白名单）。 */
int  chan_var_key_valid(const char *key, size_t len, int *readonly_out);
int  chan_var_val_valid(const char *val, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_CHAN_VAR_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
