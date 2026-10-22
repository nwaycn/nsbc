#ifndef NSBC_VAD_H
#define NSBC_VAD_H

/* =============================================================================
 * Phase 3: VAD（Voice Activity Detection）
 * 目标：AI Dialog 场景下 on_speech_start 自动触发 media_bug attach + TTS 启动
 * 输入：PCM S16 interleaved（8k/16k 单声道）
 * 输出：vad_result_t { is_speech, energy_db, spectral_flatness }
 *
 * Phase 3 骨架：
 *   - 能量阈值法（E = 10*log10(mean(pcm^2) / 32768^2) dBFS）
 *   - spectral_flatness stub（返回 0.0，Phase 4 补真 FFT）
 *   - media_bug attach 钩子先不写（Phase 4 在 media_bug.c / ai_dialog.c 接线）
 *
 * Phase 4 补全清单：
 *   1. spectral_flatness：补 RFFT（长度 256/512 取前 N bin，
 *      SF = 10*log10(mean(几何)) - 10*log10(mean(算术))，归一化到 0-1）
 *   2. 升级到 WebRTC VAD C 移植（G.729 Annex B 已证明项目接受 vendored C）
 *   3. hangover / pre-roll：静音→语音的 hysteresis（避免抖）
 *   4. b2b_call.c 接线：b2b_call_create 时 vad_create，destroy 时 vad_destroy；
 *      媒体线程每 20ms 调 vad_process，on_speech_start 触发一次事件
 *   5. ai_dialog.c 接线：订阅 on_speech_start，attach media_bug + 启动 TTS
 * ============================================================================= */

#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  is_speech;            /* 1=有语音, 0=静音 */
    double energy_db;          /* dBFS 能量（FS = full scale = 32768） */
    double spectral_flatness;  /* 频谱平坦度（0-1，越高越噪声/白噪声=1.0）*/
} vad_result_t;

typedef struct vad_state vad_state_t;

/* 创建/销毁（per-call 创建，b2b_call_destroy 时释放）*/
int  vad_create(vad_state_t **out, int sample_rate, int channels);
void vad_destroy(vad_state_t *s);

/* 处理一帧 PCM → 返回 vad_result_t
 * samples = 单声道样本数（interleaved 多声道：调用方自己拆出目标声道）*/
int  vad_process(vad_state_t *s, const int16_t *pcm, size_t samples,
                vad_result_t *out);

/* 阈值设置（默认 -30 dBFS 判定静音/语音）*/
void vad_set_threshold(vad_state_t *s, double energy_db_threshold);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_VAD_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
