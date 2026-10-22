/* =============================================================================
 * Phase 3+4: VAD — 能量阈值法 + hangover hysteresis
 *
 * Phase 4 升级：
 *   ✓ hangover hysteresis：静音→语音需连续 preroll_frames 帧超阈值确认
 *                           语音→静音需连续 hangover_frames 帧低于阈值才切回
 *   ✓ spectral_flatness 启发式估计：按能量在帧内的分布估算
 *     （能量方差大 → 语音 → SF 低；能量平稳 → 白噪声 → SF 高）
 *     真 RFFT spectral_flatness 留给 WebRTC VAD vendored 版本
 *   ✓ 状态机输出：每次 vad_process 反映稳定态 is_speech（不随单帧抖动）
 *
 * Phase 5 升级路径：
 *   - WebRTC VAD vendored C（G.729 Annex B 已证明项目接受 vendored C）
 *   - 真 256/512 点 RFFT → spectral_flatness = 10*log10(geom_mean/arith_mean)
 * ============================================================================= */

#include "vad.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct vad_state {
    int    sample_rate;
    int    channels;
    double energy_db_threshold;  /* dBFS */

    /* Phase 4: hysteresis 状态机 */
    int    in_speech;            /* 当前稳定态：1=语音, 0=静音 */
    int    preroll_counter;      /* 静音态时：连续能量超阈值的帧计数 */
    int    hangover_counter;     /* 语音态时：连续能量低于阈值的帧计数 */

    /* 每帧 20ms 假设：默认 hangover 200ms = 10 帧，preroll 40ms = 2 帧 */
    int    preroll_frames;
    int    hangover_frames;
};

/* ------------------------------------------------------------------ */

int vad_create(vad_state_t **out, int sample_rate, int channels)
{
    if (!out || sample_rate <= 0 || channels <= 0)
        return NSBC_INVAL;

    vad_state_t *s = (vad_state_t *)calloc(1, sizeof(*s));
    if (!s) return NSBC_NOMEM;

    s->sample_rate = sample_rate;
    s->channels    = channels;
    /* 默认阈值 -30 dBFS：典型对话语音峰值 -15~-25 dBFS，
     * 静音底噪 -45~-35 dBFS，-30 是分水岭 */
    s->energy_db_threshold = -30.0;

    /* Phase 4: 默认 20ms/帧假设（调用方每 20ms 调一次）*/
    s->preroll_frames  = 2;    /* 40ms 连续超阈值 → 确认语音 */
    s->hangover_frames = 10;   /* 200ms 连续低于阈值 → 确认静音 */
    s->in_speech       = 0;

    *out = s;
    return NSBC_OK;
}

void vad_destroy(vad_state_t *s)
{
    free(s);
}

void vad_set_threshold(vad_state_t *s, double energy_db_threshold)
{
    if (!s) return;
    s->energy_db_threshold = energy_db_threshold;
}

/* ------------------------------------------------------------------ */
/* Phase 4: 启发式 spectral_flatness 估计
 *
 * 真 spectral_flatness = 10*log10(几何平均(FFT bin 能量) / 算术平均(FFT bin 能量))
 * 值范围 -inf 到 0：纯净谐波(纯音) → 很低；白噪声 → 接近 0
 * 归一化到 0-1：SF_norm = exp(SF_true / 10)
 *   - 纯白噪声：SF_true ≈ -3dB → SF_norm ≈ 0.5
 *   - 纯净语音（谐波丰富）：SF_true ≈ -15dB → SF_norm ≈ 0.03
 *
 * 启发式估计（零 FFT）：
 *   把 PCM 帧分成 N 个子块，计算每个子块的能量 mean_i
 *   SF_est = 几何平均(mean_i) / 算术平均(mean_i)
 *   能量分布越均匀（白噪声/平稳噪声）→ 接近 1.0
 *   能量分布越不均匀（语音：浊音能量集中、清音分散）→ 更低
 * ------------------------------------------------------------------ */
#define VAD_SF_SUBBLOCKS 8   /* 把一帧 PCM 分成 8 个子块 */

static double estimate_spectral_flatness(const int16_t *pcm, size_t samples)
{
    if (samples < VAD_SF_SUBBLOCKS) return 0.5;

    size_t sub_len = samples / VAD_SF_SUBBLOCKS;
    double means[VAD_SF_SUBBLOCKS];

    for (int i = 0; i < VAD_SF_SUBBLOCKS; i++) {
        double sum_sq = 0.0;
        size_t start = i * sub_len;
        for (size_t j = 0; j < sub_len; j++) {
            double v = (double)pcm[start + j];
            sum_sq += v * v;
        }
        means[i] = sqrt(sum_sq / (double)sub_len) + 1e-12;  /* +eps 防 log(0) */
    }

    /* 几何平均 / 算术平均 */
    double geom_log = 0.0, arith = 0.0;
    for (int i = 0; i < VAD_SF_SUBBLOCKS; i++) {
        geom_log += log(means[i]);
        arith += means[i];
    }
    geom_log /= VAD_SF_SUBBLOCKS;
    arith    /= VAD_SF_SUBBLOCKS;

    if (arith < 1e-12) return 0.5;

    double sf = exp(geom_log) / arith;  /* 0-1 范围（白噪声=1.0，纯谐波≈0.01）*/

    /* 钳位 */
    if (sf < 0.01) sf = 0.01;
    if (sf > 1.0)  sf = 1.0;

    return sf;
}

/* ------------------------------------------------------------------ */

int vad_process(vad_state_t *s, const int16_t *pcm, size_t samples,
                vad_result_t *out)
{
    if (!s || !pcm || !out || samples == 0)
        return NSBC_INVAL;

    /* ---- 帧内能量 ---- */
    double sum_sq = 0.0;
    for (size_t i = 0; i < samples; i++) {
        double v = (double)pcm[i];
        sum_sq += v * v;
    }
    double mean_sq = sum_sq / (double)samples;

    /* E_dBFS = 10 * log10( mean(pcm^2) / FS^2 )
     * FS = 32768 (S16 full scale), silent floor ≈ -96 dBFS */
    const double FS = 32768.0;
    double dbfs = 10.0 * log10(mean_sq / (FS * FS));
    if (dbfs < -96.0) dbfs = -96.0;

    /* ---- spectral_flatness 启发式 ---- */
    out->spectral_flatness = estimate_spectral_flatness(pcm, samples);

    /* ---- Phase 4: hysteresis 状态机 ---- */
    int frame_is_speech = (dbfs >= s->energy_db_threshold) ? 1 : 0;

    if (s->in_speech) {
        /* 当前语音态 */
        if (!frame_is_speech) {
            s->hangover_counter++;
            if (s->hangover_counter >= s->hangover_frames) {
                s->in_speech = 0;
                s->hangover_counter = 0;
                s->preroll_counter  = 0;
            }
        } else {
            s->hangover_counter = 0;   /* 有语音 → hangover 归零 */
        }
    } else {
        /* 当前静音态 */
        if (frame_is_speech) {
            s->preroll_counter++;
            if (s->preroll_counter >= s->preroll_frames) {
                s->in_speech = 1;
                s->preroll_counter  = 0;
                s->hangover_counter = 0;
            }
        } else {
            s->preroll_counter = 0;   /* 有静音 → preroll 归零 */
        }
    }

    out->energy_db = dbfs;
    out->is_speech = s->in_speech;   /* ★ 输出稳定态，不是单帧判定 */

    return NSBC_OK;
}
