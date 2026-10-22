#include "digitmap.h"

void dm_cfg_default(dm_cfg_t *cfg)
{
    if (!cfg)
        return;
    cfg->first_timeout_ms = 16000;
    cfg->inter_timeout_ms = 4000;
    cfg->max_len = DM_MAX_DIGITS;
}

void dm_reset(dm_t *dm)
{
    if (!dm)
        return;
    memset(dm, 0, sizeof(*dm));
}

void dm_pickup(dm_t *dm, uint64_t now_ms)
{
    if (!dm)
        return;
    dm_reset(dm);
    dm->started = 1;
    dm->first_ms = now_ms;
    dm->last_ms = now_ms;
}

static dm_state_t check_complete(dm_t *dm, const dm_cfg_t *cfg,
                                 const dp_ctx_t *ctx)
{
    if (ctx && dp_find_immediate(ctx, dm->buf))
        return DM_READY;
    if (dm->len >= cfg->max_len)
        return DM_READY; /* 自动匹配位 */
    return DM_COLLECTING;
}

dm_state_t dm_feed(dm_t *dm, const dm_cfg_t *cfg, const dp_ctx_t *ctx,
                   char digit, uint64_t now_ms)
{
    if (!dm || !cfg)
        return DM_OVERFLOW;

    if (digit == '#') {
        if (!dm->started || dm->len == 0)
            return DM_COLLECTING;
        return DM_READY;
    }

    if (digit < '0' || digit > '9')
        return DM_COLLECTING; /* 忽略非法字符 */

    if (!dm->started) {
        dm->started = 1;
        dm->first_ms = now_ms; /* 未显式 pickup 时以首位时间兜底 */
    }
    if (dm->len >= DM_MAX_DIGITS || dm->len >= cfg->max_len)
        return DM_OVERFLOW;

    dm->buf[dm->len++] = digit;
    dm->buf[dm->len] = '\0';
    dm->last_ms = now_ms;

    return check_complete(dm, cfg, ctx);
}

dm_state_t dm_tick(const dm_t *dm, const dm_cfg_t *cfg, uint64_t now_ms)
{
    if (!dm || !cfg || !dm->started)
        return DM_COLLECTING;

    uint64_t base = dm->len == 0 ? dm->first_ms : dm->last_ms;
    uint32_t to = dm->len == 0 ? cfg->first_timeout_ms : cfg->inter_timeout_ms;

    if (now_ms - base >= to)
        return DM_TIMEOUT;
    return DM_COLLECTING;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
