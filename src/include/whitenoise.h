#ifndef NSBC_WHITENOISE_H
#define NSBC_WHITENOISE_H

/* 舒适噪声（REQ-WN-001/002，P1）
 *
 * CN payload RFC 3389（PT 13 动态协商）：CN 包携带按频带的噪声能级
 * （-127..0 dBov）与可选谱参数；接收端在语音间隙按能级再生平稳噪声
 * 填充（静音抑制后避免完全死寂、降低用户挂断误判）。
 * 与 PLC（media_dsp）配合：刚丢包走 PLC，持续静音期切 CN；
 * 与 mixer 配合：会议背景噪声按说话人 VAD 混合（mixer 归一化）。
 */
#include "nsbc_common.h"
#include "media_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CN_BANDS_MAX 4  /* RFC3389：G.711 单带，宽带最多带表扩展 */

typedef struct {
    int8_t level_dbov[CN_BANDS_MAX]; /* 每带能级；-127=静默 */
    int    nbands;
    uint8_t payload_type;
} cn_desc_t;

typedef struct cn_gen cn_gen_t;  /* 接收端噪声再生器 */
typedef struct cn_est cn_est_t;  /* 发送端能级估计器 */

int  cn_gen_create(uint8_t payload_type, int rate, cn_gen_t **out);
void cn_gen_destroy(cn_gen_t *g);
/* 收到 CN 包：更新能级（payload 1B 起始为整体 dBov，余为带差） */
int  cn_gen_on_packet(cn_gen_t *g, const uint8_t *p, size_t len);
/* 按 ptime 产出一帧噪声（S16；随机相位白噪声×带增益） */
int  cn_gen_fill(cn_gen_t *g, media_frame_t *frm);

int  cn_est_create(int rate, cn_est_t **out);
void cn_est_destroy(cn_est_t *e);
/* 发送端喂静音段 PCM：估计能级（EWMA），能级变化超 3dB 才出 CN 包 */
void cn_est_on_silence(cn_est_t *e, const media_frame_t *frm);
int  cn_est_emit(cn_est_t *e, uint8_t *out, size_t cap, size_t *out_len,
                 int *changed);

/* dBov↔线性增益换算（生成幅度用） */
float cn_dbov_to_gain(int dbov);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_WHITENOISE_H */
