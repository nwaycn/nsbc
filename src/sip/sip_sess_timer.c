/* RFC 4028 会话定时器——粗骨架（REQ-SIP-014，P0）
 *
 * 纯协商/判定逻辑（不碰 transport，可在单测中直接验证）；
 * TODO(P0 接线)：b2bua INVITE 协商分支挂 offer/negotiate/arm，
 * 时间轮在 interval*500ms（refresher）与 interval+grace（被刷新方）
 * 各 arm 一个回调；刷新失败/超时走 b2b_terminate 发 BYE。
 */
#include "sip_sess_timer.h"

void sess_timer_cfg_default(sess_timer_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    cfg->min_se_s = SESS_TIMER_MIN_SE_S;
    cfg->def_se_s = SESS_TIMER_DEF_SE_S;
    cfg->max_se_s = SESS_TIMER_DEF_SE_S;
}

void sess_timer_init(sess_timer_t *st)
{
    if (st == NULL)
        return;
    memset(st, 0, sizeof(*st));
    st->interval_s = 0;
    st->refresher = SIP_REF_NONE;
}

int sess_timer_uac_offer(const sess_timer_cfg_t *cfg, sess_timer_nego_t *out)
{
    if (cfg == NULL || out == NULL)
        return NSBC_INVAL;
    memset(out, 0, sizeof(*out));
    out->interval_s = cfg->def_se_s;
    out->min_se_s = cfg->min_se_s;
    out->refresher = SIP_REF_UAC; /* UAC 提议自己刷新（RFC4028 §5.1） */
    return NSBC_OK;
}

int sess_timer_uas_negotiate(const sess_timer_cfg_t *cfg,
                             const sess_timer_nego_t *in,
                             sess_timer_nego_t *out)
{
    int v;
    if (cfg == NULL || in == NULL || out == NULL)
        return NSBC_INVAL;
    v = in->interval_s;
    if (v < cfg->min_se_s)
        v = cfg->min_se_s;               /* 低于本地下限→抬高（422 由调用方发） */
    if (v > cfg->max_se_s)
        v = cfg->max_se_s;
    out->interval_s = v;
    out->min_se_s = cfg->min_se_s;
    /* 提议未带 refresher 参数时，UAS 强制承担刷新（RFC4028 §5.1） */
    out->refresher = (in->refresher == SIP_REF_NONE) ? SIP_REF_UAS
                                                     : in->refresher;
    return NSBC_OK;
}

int sess_timer_arm(sess_timer_t *st, const sess_timer_nego_t *n,
                   uint64_t now_ms)
{
    if (st == NULL || n == NULL)
        return NSBC_INVAL;
    /* interval_s 下限检查已在协商阶段由调用方完成 */
    st->active = 1;
    st->interval_s = n->interval_s;
    st->refresher = n->refresher;
    st->armed_ms = now_ms;
    st->refresh_sent = 0;
    return NSBC_OK;
}

int sess_timer_due_refresh(sess_timer_t *st, uint64_t now_ms)
{
    uint64_t half;
    if (st == NULL || !st->active || st->refresher == SIP_REF_NONE)
        return 0;
    if (st->refresh_sent)
        return 0;
    half = (uint64_t)st->interval_s * 500u; /* 过半刷新 */
    if (now_ms - st->armed_ms < half)
        return 0;
    st->refresh_sent = 1;
    return 1;
}

int sess_timer_expired(const sess_timer_t *st, uint64_t now_ms)
{
    uint64_t deadline;
    if (st == NULL || !st->active)
        return 0;
    deadline = st->armed_ms + (uint64_t)st->interval_s * 1000u
             + SESS_TIMER_GRACE_MS;
    return now_ms >= deadline;
}

void sess_timer_reset(sess_timer_t *st, uint64_t now_ms)
{
    if (st == NULL || !st->active)
        return;
    st->armed_ms = now_ms;
    st->refresh_sent = 0;
}

void sess_timer_refresh_acked(sess_timer_t *st, uint64_t now_ms)
{
    if (st == NULL || !st->active)
        return;
    st->armed_ms = now_ms;
    st->refresh_sent = 0;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
