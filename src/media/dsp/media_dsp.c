/* 内置 DSP——粗骨架（REQ-TRANS-005/006，P1）
 *
 * TODO（细节轮）：VAD 能量+过零率+噪声基底 EWMA（或 WebRTC 前级
 * 简化版）；PLC 基音周期延拓；AGC 峰值/RMS 增益限幅防削顶；
 * DTMF Goertzel 697/770/852/941 × 1209/1336/1477 双频判决。
 */
#include "media_dsp.h"

struct dsp_ctx {
    dsp_cfg_t cfg;
    int       rate;
    int       channels;
    int       speech;
    int       plc_run;
};

int dsp_open(const dsp_cfg_t *cfg, int rate, int channels, dsp_ctx_t **out)
{
    dsp_ctx_t *d;
    if (cfg == NULL || out == NULL ||
        (channels != 1 && channels != 2))
        return NSBC_INVAL;
    d = (dsp_ctx_t *)calloc(1, sizeof(*d));
    if (d == NULL)
        return NSBC_NOMEM;
    d->cfg = *cfg;
    d->rate = rate;
    d->channels = channels;
    *out = d;
    return NSBC_OK;
}

void dsp_close(dsp_ctx_t *d)
{
    free(d);
}

int dsp_process(dsp_ctx_t *d, media_frame_t *frm, int *is_speech)
{
    if (d == NULL || frm == NULL)
        return NSBC_INVAL;
    /* TODO：AGC/ALC 原位缩放；VAD 能量判决回填 */
    d->speech = 1;
    if (is_speech != NULL)
        *is_speech = d->speech;
    return NSBC_OK;
}

int dsp_plc_generate(dsp_ctx_t *d, media_frame_t *out, int missing_run)
{
    if (d == NULL || out == NULL)
        return NSBC_INVAL;
    (void)missing_run;
    /* TODO：前 4 丢包基音补偿；其后 CN/静音，返回 0 让调用方停发 */
    return NSBC_NOTSUP;
}

int dsp_dtmf_scan(dsp_ctx_t *d, const media_frame_t *frm, char *digit_out)
{
    if (d == NULL || frm == NULL || digit_out == NULL)
        return NSBC_INVAL;
    *digit_out = '\0';
    /* TODO(P0 随 DTMF-001 接线)：Goertzel 双频能量+持续 20ms 确认 */
    return NSBC_NOTSUP;
}

void dsp_reset(dsp_ctx_t *d)
{
    if (d == NULL)
        return;
    d->speech = 0;
    d->plc_run = 0;
}
