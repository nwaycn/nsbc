/* 舒适噪声——粗骨架（REQ-WN-001/002，P1）
 *
 * 接线：
 *  - 发送 VAD 判静且持续≥帧窗后停发音频改周期性发 CN（500ms 刷新）；
 *  - 接收无语音帧时 gen_fill 填噪，PLC 与 CN 的切换由 bridge 按
 *    丢包/静音来源裁决；CN 不进 jitter_buf 正常播放序列（旁路）；
 *  - 会议 mixer 背景噪声能量参与归一化，避免多路静音呼吸效应。
 */
#include "whitenoise.h"

struct cn_gen {
    uint8_t pt;
    int     rate;
    cn_desc_t desc;
};

struct cn_est {
    int    rate;
    int8_t cur_dbov;
    int    last_sent_dbov;
};

int cn_gen_create(uint8_t payload_type, int rate, cn_gen_t **out)
{
    cn_gen_t *g;
    if (out == NULL)
        return NSBC_INVAL;
    g = (cn_gen_t *)calloc(1, sizeof(*g));
    if (g == NULL)
        return NSBC_NOMEM;
    g->pt = payload_type;
    g->rate = rate;
    g->desc.payload_type = payload_type;
    *out = g;
    return NSBC_OK;
}

void cn_gen_destroy(cn_gen_t *g)
{
    free(g);
}

int cn_gen_on_packet(cn_gen_t *g, const uint8_t *p, size_t len)
{
    if (g == NULL || p == NULL || len == 0)
        return NSBC_INVAL;
    /* TODO(P1)：首字节整体 dBov；后续字节为各带相对差（有符号），
     * 宽带表（RFC3389 Appendix II 频带边界）映射。 */
    g->desc.nbands = (int)len;
    if (g->desc.nbands > CN_BANDS_MAX)
        g->desc.nbands = CN_BANDS_MAX;
    memcpy(g->desc.level_dbov, p, (size_t)g->desc.nbands);
    return NSBC_OK;
}

int cn_gen_fill(cn_gen_t *g, media_frame_t *frm)
{
    if (g == NULL || frm == NULL || frm->pcm == NULL)
        return NSBC_INVAL;
    /* TODO(P1)：简单 PRNG 均匀白噪声按 dBov 增益缩放（单带基线），
     * 多带时 IIR 分带整形；保证幅度不超过 CN 能级上限。 */
    return NSBC_NOTSUP;
}

int cn_est_create(int rate, cn_est_t **out)
{
    cn_est_t *e;
    if (out == NULL)
        return NSBC_INVAL;
    e = (cn_est_t *)calloc(1, sizeof(*e));
    if (e == NULL)
        return NSBC_NOMEM;
    e->rate = rate;
    e->cur_dbov = -127;
    e->last_sent_dbov = -127;
    *out = e;
    return NSBC_OK;
}

void cn_est_destroy(cn_est_t *e)
{
    free(e);
}

void cn_est_on_silence(cn_est_t *e, const media_frame_t *frm)
{
    if (e == NULL || frm == NULL || frm->samples == 0)
        return;
    /* TODO(P1)：RMS→dBov，EWMA 平滑（防止噪声包抖） */
}

int cn_est_emit(cn_est_t *e, uint8_t *out, size_t cap, size_t *out_len, int *changed)
{
    if (e == NULL || out == NULL || out_len == NULL || changed == NULL)
        return NSBC_INVAL;
    *changed = 0;
    *out_len = 0;
    /* TODO：变化≥3dB 或距上次 500ms → changed=1，输出 1B dBov */
    if (cap < 1)
        return NSBC_NOSPC;
    return NSBC_NOTSUP;
}

float cn_dbov_to_gain(int dbov)
{
    float x;
    /* 10^(dBov/20)；dbov≤-127 视为 0。粗骨架用迭代展开近似幂，
     * 接线时用已有的快查表或 expf（非关键路径允许 libm）。 */
    if (dbov <= -127)
        return 0.0f;
    x = (float)dbov / 20.0f;
    (void)x;
    return 0.0f; /* TODO */
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
