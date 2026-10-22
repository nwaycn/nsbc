/* 自适应抖动缓冲——粗骨架（REQ-TRANS-003，P1）
 *
 * 接线时：环形定槽（max_depth_ms/ptime 个 jb_pkt_t），seq 回绕用
 * uint16 差值判定；自适应用到达间隔 EWMA 方差调 cur_depth_ms；
 * SSRC 切换（发言方）由调用方 jb_reset，不在本模块做混源。
 */
#include "jitter_buf.h"

struct jitter_buf {
    jb_cfg_t cfg;
    jb_stat_t st;
    uint16_t next_seq;
    int      primed;
};

int jb_create(const jb_cfg_t *cfg, jitter_buf_t **out)
{
    jitter_buf_t *jb;
    if (cfg == NULL || out == NULL || cfg->clock_rate <= 0 ||
        cfg->ptime_ms <= 0)
        return NSBC_INVAL;
    jb = (jitter_buf_t *)calloc(1, sizeof(*jb));
    if (jb == NULL)
        return NSBC_NOMEM;
    jb->cfg = *cfg;
    if (jb->cfg.init_depth_ms <= 0)
        jb->cfg.init_depth_ms = JB_DEFAULT_DEPTH_MS;
    if (jb->cfg.max_depth_ms <= 0 ||
        jb->cfg.max_depth_ms > JB_MAX_DEPTH_MS)
        jb->cfg.max_depth_ms = JB_MAX_DEPTH_MS;
    *out = jb;
    return NSBC_OK;
}

void jb_destroy(jitter_buf_t *jb)
{
    free(jb);
}

void jb_reset(jitter_buf_t *jb)
{
    if (jb == NULL)
        return;
    memset(&jb->st, 0, sizeof(jb->st));
    jb->primed = 0;
}

jb_rc_t jb_push(jitter_buf_t *jb, const jb_pkt_t *pkt)
{
    int16_t diff;
    if (jb == NULL || pkt == NULL)
        return JB_OVERFLOW;
    if (!jb->primed) {
        jb->next_seq = pkt->seq;
        jb->primed = 1;
    }
    diff = (int16_t)(pkt->seq - jb->next_seq);
    if (diff < 0) {
        ++jb->st.drops_late;
        return JB_DROP_OLD;
    }
    if (diff == 0 && jb->primed) {
        ++jb->st.drops_dup;
        return JB_DROP_DUP;
    }
    /* TODO(P1)：槽位环形落位、超深扩容/收缩、统计 EWMA */
    ++jb->st.pushed;
    return JB_OK;
}

jb_rc_t jb_pop(jitter_buf_t *jb, uint64_t now_ms, jb_pkt_t *out)
{
    if (jb == NULL || out == NULL)
        return JB_OVERFLOW;
    (void)now_ms;
    /* TODO(P1)：到播放点→拷贝并推进 next_seq；缺包→plc_events++ NEED_MORE */
    return JB_NEED_MORE;
}

void jb_stat(const jitter_buf_t *jb, jb_stat_t *out)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (jb != NULL)
        *out = jb->st;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
