/* 重采样——粗骨架（REQ-TRANS-002，P1） */
#include "resample.h"

struct resampler {
    rs_cfg_t cfg;
};

int resampler_open(const rs_cfg_t *cfg, resampler_t **out)
{
    resampler_t *r;
    if (cfg == NULL || out == NULL || cfg->in_rate <= 0 ||
        cfg->out_rate <= 0 || (cfg->channels != 1 && cfg->channels != 2))
        return NSBC_INVAL;
    if (!resampler_supported(cfg->in_rate, cfg->out_rate, cfg->channels))
        return NSBC_NOTSUP;
    r = (resampler_t *)calloc(1, sizeof(*r));
    if (r == NULL)
        return NSBC_NOMEM;
    r->cfg = *cfg;
    *out = r;
    return NSBC_OK;
}

void resampler_close(resampler_t *r)
{
    free(r);
}

int resampler_process(resampler_t *r, const int16_t *in, size_t in_samples, int16_t *out, size_t out_cap, size_t *out_samples)
{
    if (r == NULL || in == NULL || out == NULL || out_samples == NULL)
        return NSBC_INVAL;
    (void)in_samples;
    (void)out_cap;
    /* TODO(P1)：同速率直通拷贝；整数比插值；非整数多相滤波，留尾 */
    *out_samples = 0;
    return NSBC_NOTSUP;
}

int resampler_flush(resampler_t *r, int16_t *out, size_t out_cap, size_t *out_samples)
{
    if (r == NULL || out == NULL || out_samples == NULL)
        return NSBC_INVAL;
    (void)out_cap;
    *out_samples = 0;
    return NSBC_OK;
}

int resampler_supported(int in_rate, int out_rate, int channels)
{
    /* 粗骨架：先声明常规电话速率与单/双声道；非整比实现随 P1 补 */
    static const int rates[] = {8000, 12000, 16000, 24000, 48000};
    size_t i;
    int in_ok = 0, out_ok = 0;
    if (channels != 1 && channels != 2)
        return 0;
    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        if (rates[i] == in_rate)
            in_ok = 1;
        if (rates[i] == out_rate)
            out_ok = 1;
    }
    return in_ok && out_ok;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
