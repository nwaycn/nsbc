#ifndef NSBC_RESAMPLE_H
#define NSBC_RESAMPLE_H

/* 重采样（REQ-TRANS-002，P1）：8k/16k/24k/48k 线性 PCM 速率变换
 *
 * 转码流水线中夹在 decode 与 encode 之间（如 Opus48k↔G.711 8k）；
 * 整数倍优先线性/多项式插值，非整数比走多相滤波；S16 进 S16 出，
 * 有界缓冲、零内部堆活动（init 后）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct resampler resampler_t;

typedef struct {
    int in_rate;
    int out_rate;
    int channels;     /* 1/2，交织帧 */
    int quality;      /* 0 线性（省 CPU）..3 多相高质量 */
} rs_cfg_t;

int  resampler_open(const rs_cfg_t *cfg, resampler_t **out);
void resampler_close(resampler_t *r);

/* 任意输入长度，内部留尾；out_cap=样本总数（含各声道），
 * 返回实际写出样本数，输入被完全消费（尾样留存到下次）。 */
int  resampler_process(resampler_t *r,
                       const int16_t *in, size_t in_samples,
                       int16_t *out, size_t out_cap,
                       size_t *out_samples);

/* 冲尾（呼叫结束排空，通常 <1 帧） */
int  resampler_flush(resampler_t *r, int16_t *out, size_t out_cap,
                     size_t *out_samples);

/* 速率换算整数比支持判定（不支持返回 NOTSUP，走转码替代链） */
int  resampler_supported(int in_rate, int out_rate, int channels);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_RESAMPLE_H */
