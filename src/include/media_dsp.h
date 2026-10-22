#ifndef NSBC_MEDIA_DSP_H
#define NSBC_MEDIA_DSP_H

/* 内置 DSP（REQ-TRANS-005 VAD、REQ-TRANS-006 PLC/AGC/ALC）
 *
 * 规格口径：DSP 内置、不做可插拔模块（7.8 目录基线）。
 * 全部处理 S16 PCM 帧（media_frame_t）；单会话单 ctx、无内部锁、
 * init 后零 malloc；能量阈值/噪声估计在 ctx 内滚动。
 */
#include "nsbc_common.h"
#include "media_codec.h" /* media_frame_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int vad_enable;
    int vad_mode;        /* 0..3，越大越激进（拒噪），对齐 G.729 VAD 习惯 */
    int plc_enable;      /* 丢包补偿（JB NEED_MORE 时由调用方喂） */
    int agc_enable;      /* 自动增益 */
    int target_level_db; /* AGC 目标（典型 -18dBov） */
    int alc_enable;      /* 自动响度补偿（长时窗） */
} dsp_cfg_t;

typedef struct dsp_ctx dsp_ctx_t;

int  dsp_open(const dsp_cfg_t *cfg, int rate, int channels, dsp_ctx_t **out);
void dsp_close(dsp_ctx_t *d);

/* 正常帧：依次 AGC/ALC→（VAD 判定不改样值，只出标志） */
int  dsp_process(dsp_ctx_t *d, media_frame_t *frm, int *is_speech);

/* PLC：丢包时生成补偿帧（前一基音周期延拓/衰减噪声），
 * 连续丢帧计数超限（典型 4 帧）后输出静音/CN，返回 1=仍在补偿 */
int  dsp_plc_generate(dsp_ctx_t *d, media_frame_t *out, int missing_run);

/* INBAND DTMF 能量检测挂点（Goertzel），实现见 dtmf 批调用 */
int  dsp_dtmf_scan(dsp_ctx_t *d, const media_frame_t *frm, char *digit_out);

/* 重置（SSRC 切换/新发言方时调用，清噪声基底与历史样） */
void dsp_reset(dsp_ctx_t *d);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_MEDIA_DSP_H */
