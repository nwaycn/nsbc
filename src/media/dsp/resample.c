/* PCM 重采样（REQ-TRANS-002）
 *
 * 实现：Q32 定点相位 + 线性插值，支持任意输入/输出采样率（4k..192k）
 * 与 1/2 声道交织数据。流式处理：跨包保留上一帧样本与相位，无累积漂移。
 * init 后零堆分配，输出有界。
 */
#include "resample.h"
#include <stdlib.h>
#include <string.h>

#define RS_ONE ((uint64_t)1 << 32)

struct resampler {
    rs_cfg_t cfg;
    uint64_t step;      /* 每个输出样本前进的输入相位 */
    uint64_t phase;     /* 当前相位 [0, RS_ONE) */
    int16_t  prev[2];   /* 上一输入帧（每声道）*/
    int      primed;
};

static int rs_rate_ok(int r)
{
    return r >= 4000 && r <= 192000;
}

int resampler_supported(int in_rate, int out_rate, int channels)
{
    if (channels != 1 && channels != 2) return 0;
    if (!rs_rate_ok(in_rate) || !rs_rate_ok(out_rate)) return 0;
    return 1;
}

int resampler_open(const rs_cfg_t *cfg, resampler_t **out)
{
    resampler_t *r;
    if (cfg == NULL || out == NULL) return NSBC_INVAL;
    if (!resampler_supported(cfg->in_rate, cfg->out_rate, cfg->channels))
        return NSBC_NOTSUP;
    r = (resampler_t *)calloc(1, sizeof(*r));
    if (r == NULL) return NSBC_NOMEM;
    r->cfg = *cfg;
    r->step = (((uint64_t)cfg->in_rate) << 32) / (uint64_t)cfg->out_rate;
    if (r->step == 0) r->step = 1;
    *out = r;
    return NSBC_OK;
}

void resampler_close(resampler_t *r)
{
    free(r);
}

int resampler_process(resampler_t *r,
                      const int16_t *in, size_t in_samples,
                      int16_t *out, size_t out_cap,
                      size_t *out_samples)
{
    size_t i, o = 0;
    int ch, c;
    uint64_t ph;

    if (r == NULL || in == NULL || out == NULL || out_samples == NULL)
        return NSBC_INVAL;
    *out_samples = 0;
    if (in_samples == 0) return NSBC_OK;

    ch = r->cfg.channels;

    /* 同速率：直通拷贝 */
    if (r->cfg.in_rate == r->cfg.out_rate) {
        size_t n = in_samples;
        if (n > out_cap) n = out_cap;
        memcpy(out, in, n * sizeof(int16_t));
        *out_samples = n;
        return NSBC_OK;
    }

    ph = r->phase;
    for (i = 0; i + (size_t)ch <= in_samples; i += (size_t)ch) {
        if (!r->primed) {
            for (c = 0; c < ch; ++c)
                r->prev[c] = in[i + (size_t)c];
            r->primed = 1;
            continue;
        }
        /* 在当前 [prev, cur) 区间内按相位产生输出 */
        while (ph < RS_ONE) {
            if (o + (size_t)ch > out_cap) {
                r->phase = ph;
                *out_samples = o;
                return NSBC_NOMEM;
            }
            for (c = 0; c < ch; ++c) {
                int32_t a = r->prev[c];
                int32_t b = in[i + (size_t)c];
                int64_t diff = (int64_t)(b - a) * (int64_t)ph;
                int32_t v = a + (int32_t)(diff >> 32);
                if (v > 32767) v = 32767;
                else if (v < -32768) v = -32768;
                out[o + (size_t)c] = (int16_t)v;
            }
            o += (size_t)ch;
            ph += r->step;
        }
        ph -= RS_ONE;
        for (c = 0; c < ch; ++c)
            r->prev[c] = in[i + (size_t)c];
    }

    r->phase = ph;
    *out_samples = o;
    return NSBC_OK;
}

int resampler_flush(resampler_t *r, int16_t *out, size_t out_cap,
                    size_t *out_samples)
{
    (void)out_cap;
    if (r == NULL || out == NULL || out_samples == NULL)
        return NSBC_INVAL;
    /* 线性插值无内部延迟队列，尾样已随上一个包输出 */
    *out_samples = 0;
    return NSBC_OK;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
