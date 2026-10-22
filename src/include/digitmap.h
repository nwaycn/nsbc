#ifndef NSBC_DIGITMAP_H
#define NSBC_DIGITMAP_H

#include "dialplan.h"

#define DM_MAX_DIGITS 32

/* 对应 REQ-PBX-DP-002 */
typedef enum {
    DM_COLLECTING = 0, /* 继续收号 */
    DM_READY,          /* 收齐：立即匹配 / #结束 / 达到最大长度 */
    DM_TIMEOUT,        /* 首位或位间超时，buf 中为已收号码 */
    DM_OVERFLOW        /* 超过最大长度 */
} dm_state_t;

typedef struct {
    char buf[DM_MAX_DIGITS + 1];
    uint8_t len;
    uint64_t first_ms;
    uint64_t last_ms;
    uint8_t started;
} dm_t;

typedef struct {
    uint32_t first_timeout_ms; /* 摘机后首位超时 */
    uint32_t inter_timeout_ms; /* 位间超时 */
    uint8_t max_len;
} dm_cfg_t;

void dm_cfg_default(dm_cfg_t *cfg);
void dm_reset(dm_t *dm);

/* 摘机：开始首位计时 */
void dm_pickup(dm_t *dm, uint64_t now_ms);

/* 输入一个字符：'0'-'9' 收号，'#' 立即结束。时钟必须为单调时钟 */
dm_state_t dm_feed(dm_t *dm, const dm_cfg_t *cfg, const dp_ctx_t *ctx,
                   char digit, uint64_t now_ms);

/* 周期 tick，检测超时 */
dm_state_t dm_tick(const dm_t *dm, const dm_cfg_t *cfg, uint64_t now_ms);

#endif /* NSBC_DIGITMAP_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
