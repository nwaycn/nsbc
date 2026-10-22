#ifndef NSBC_MEDIA_CODEC_H
#define NSBC_MEDIA_CODEC_H

/* 编解码注册表与转码会话（REQ-TRANS-001/004/005，P1 执行；
 * 描述符/协商属 P0 配套；G.711 为内置基线，见 g711.h）
 *
 * 现状：media_bridge 唯一转码执行点返 NSBC_NOTSUP；src/mod/codecs
 * 为空描述符；本头统一编解码 ops 注册表，商业 codec（G.729/AMR）经
 * 描述符登记，调用方只认 codec_id，不感知实现来源（静态/dlopen）。
 *
 * 中间统一表示：解码出 PCM S16 interleaved（声道数在 frame 中），
 * 编码消费同一格式；转码=decode→（重采样）→encode 两段流水线。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CODEC_NAME_MAX  24
#define CODEC_PT_DYN_MIN 96   /* RFC 3551 动态 PT 起点 */

typedef enum {
    CODEC_NONE = 0,
    CODEC_PCMU,       /* 0  G.711 µ-law 8k */
    CODEC_PCMA,       /* 8  G.711 A-law 8k */
    CODEC_G722,       /* 9  16k 宽带（RFC3551 时钟 8000 历史问题） */
    CODEC_G729,       /* 18 专利许可 */
    CODEC_G726_16,    /* — 16 kbps ADPCM（mod_av FFmpeg）*/
    CODEC_G726_24,    /* — 24 kbps ADPCM（mod_av FFmpeg）*/
    CODEC_G726_32,    /* 2/4 */
    CODEC_G726_40,    /* — 40 kbps ADPCM（mod_av FFmpeg）*/
    CODEC_ILBC,
    CODEC_AMR_NB,     /* 专利许可 */
    CODEC_AMR_WB,
    CODEC_G723_1,     /* — 6.3 kbps ACELP（mod_av FFmpeg）*/
    CODEC_OPUS,       /* 48k 宽带 */
    CODEC_TELEPHONE_EVENT, /* RFC 4733 */
    CODEC_CN,              /* RFC 3389 舒适噪声 */
    CODEC_T38,             /* 传真（非音频，走 fax 子系统）*/
    /* === Phase 3 mod_av 视频 codec === */
    CODEC_H264,      /* — H.264/AVC（mod_av FFmpeg）*/
    CODEC_VP8,       /* — VP8（mod_av FFmpeg）*/
    CODEC_VP9,       /* — VP9（mod_av FFmpeg）*/
    CODEC__MAX
} codec_id_t;

/* PT 静态值表（mod_av 注册时用；动态 codec 填 -1）
 * 音频：G.722=9, G.726 系列无静态 PT（动态 96+）, Opus 无静态 PT
 * 视频：H.264=96（常用默认）, VP8/VP9 动态
 * G.723.1 无静态 PT */

typedef enum {
    CODEC_FRAMING_RTP = 0,
    CODEC_FRAMING_FILE
} codec_framing_t;

/* PCM 统一中间帧（单调时钟时间戳由调用方持有） */
typedef struct {
    int16_t *pcm;       /* S16 native endian interleaved */
    size_t   samples;   /* 每声道采样点数 */
    int      rate;      /* 采样率 8000/16000/48000 */
    int      channels;  /* 1 单 / 2 交织 */
    uint32_t rtp_ts;
} media_frame_t;

typedef struct codec_sess codec_sess_t;

/* 编解码 ops：decode 入参为 RTP 载荷（去头），encode 产出载荷；
 * 所有实现必须无锁、零 malloc（init 后）、有界写入。
 *
 * decoder/encoder init 分离（mod_av 用，mod_codecs 的 G.711/G.729 encoder_init 保持 NULL）：
 *   init → 创建 decoder sess（绑 dec_ctx）
 *   encoder_init → 创建 encoder sess（绑 enc_ctx）
 * transcode_open 先 dops->init（dec sess）再 eops->encoder_init 或 eops->init（enc sess）。
 * 两个 init 独立 calloc 各自 sess，互不干扰。*/
typedef struct {
    codec_id_t id;
    char       name[CODEC_NAME_MAX];
    int        clock_rate;
    int        channels;
    int        default_pt;     /* 静态 PT；动态编码填 -1 */
    int        ptime_ms;       /* 建议帧长 */
    int      (*init)(const char *fmtp, codec_sess_t **out);           /* decoder sess init */
    int      (*encoder_init)(const char *fmtp, codec_sess_t **out);    /* encoder sess init（NULL=走 init）*/
    void     (*fini)(codec_sess_t *s);
    int      (*decode)(codec_sess_t *s, const uint8_t *in, size_t in_len,
                       media_frame_t *out);
    int      (*encode)(codec_sess_t *s, const media_frame_t *in,
                       uint8_t *out, size_t cap, size_t *out_len);
} codec_ops_t;

/* 全局注册表（启动期登记，运行期只读，无需加锁） */
int  codec_registry_register(const codec_ops_t *ops);
const codec_ops_t *codec_registry_find(codec_id_t id);
const codec_ops_t *codec_registry_find_pt(int pt, int clock_rate);
const codec_ops_t *codec_registry_find_name(const char *name, size_t len);
unsigned codec_registry_count(void);

/* Phase 5 CLI: 遍历所有已注册 codec — 回调非 0 提前终止，返回遍历数 */
int codec_registry_foreach(int (*cb)(const codec_ops_t *, void *), void *ud);

/* Phase 1 SAFE1: 安全卸载扩展 API（不改现有公开签名）
 *   codec_registry_find_inc / codec_registry_dec —— 卸载安全 find，调用者配对使用
 *   codec_registry_unload_wait / codec_registry_unregister —— 卸载等待 + 清理 */
const codec_ops_t *codec_registry_find_inc(codec_id_t id, int *out_slot);
void codec_registry_dec(int slot);
int  codec_registry_unload_wait(codec_id_t id, int timeout_ms);
int  codec_registry_unregister(codec_id_t id);

/* 转码会话：src→dst（需要时内部级联重采样，见 resample.h） */
typedef struct transcode_sess transcode_sess_t;
int  transcode_open(codec_id_t src, codec_id_t dst, transcode_sess_t **out);
void transcode_close(transcode_sess_t *t);
/* 一帧 RTP 载荷进、一帧载荷出（无足够数据时 out_len=0/AGAIN） */
int  transcode_process(transcode_sess_t *t,
                       const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t cap, size_t *out_len);
/* accessor：media_bridge.c 里 cnode 持有 transcode_sess_t* 但不能访问内部字段 */
codec_id_t transcode_dst_codec_id(const transcode_sess_t *t);

/* SDP fmtp 协商裁剪（如 AMR octet-align、Opus maxaveragebitrate），
 * 交集结果写 dst（有界）；无对应 codec 返回 NOTSUP */
int  codec_fmtp_negotiate(codec_id_t id, const char *offer, const char *answer,
                          char *dst, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_MEDIA_CODEC_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
