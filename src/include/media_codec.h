#ifndef NSBC_MEDIA_CODEC_H
#define NSBC_MEDIA_CODEC_H

/* 编解码注册表与转码会话（REQ-TRANS-001/004/005）
 *
 * Registry v2（2026-11-22）：
 *   - codec_id_t 改为 uint16_t，内置 codec 保留稳定 ID（1..19），
 *     动态模块（AMR-WB/AV1/...）由 codec_register() 分配 ID，无需改核心枚举。
 *   - codec_ops_t 携带 SDP 元数据（sdp_name/aliases/kind/payload/clock_rate/
 *     default_pt/ptime/fmtp/caps），核心只做通用调度，不再硬编码 codec 名称。
 *   - 名称查找大小写不敏感，支持别名与 SDP 名（如 H264 / H.264）。
 *   - 动态加载 ABI 带版本号；注册/注销带引用计数，呼叫引用期间不卸载。
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
#define NSBC_CODEC_API_VERSION 2

/* codec_id_t：0=CODEC_NONE；1..31 内置稳定 ID；>=64 动态模块 */
typedef uint16_t codec_id_t;

enum {
    CODEC_NONE = 0,
    CODEC_PCMU,       /* 1  G.711 µ-law 8k */
    CODEC_PCMA,       /* 2  G.711 A-law 8k */
    CODEC_G722,       /* 3  16k 宽带（RFC3551 时钟 8000 历史问题） */
    CODEC_G729,       /* 4  专利许可 */
    CODEC_G726_16,    /* 5  16 kbps ADPCM（mod_av FFmpeg）*/
    CODEC_G726_24,    /* 6  24 kbps ADPCM（mod_av FFmpeg）*/
    CODEC_G726_32,    /* 7  32 kbps ADPCM（mod_av FFmpeg）*/
    CODEC_G726_40,    /* 8  40 kbps ADPCM（mod_av FFmpeg）*/
    CODEC_ILBC,       /* 9 */
    CODEC_AMR_NB,     /* 10 专利许可 */
    CODEC_AMR_WB,     /* 11 */
    CODEC_G723_1,     /* 12 6.3 kbps ACELP（mod_av FFmpeg）*/
    CODEC_OPUS,       /* 13 48k 宽带 */
    CODEC_TELEPHONE_EVENT, /* 14 RFC 4733 */
    CODEC_CN,              /* 15 RFC 3389 舒适噪声 */
    CODEC_T38,             /* 16 传真（非音频，走 fax 子系统）*/
    CODEC_H264,      /* 17 H.264/AVC（mod_av FFmpeg）*/
    CODEC_VP8,       /* 18 VP8（mod_av FFmpeg）*/
    CODEC_VP9,       /* 19 VP9（mod_av FFmpeg）*/

    CODEC_ID_BUILTIN_MAX = 32,  /* 内置 ID 上限（不含） */
    CODEC_ID_DYNAMIC_MIN = 64,  /* 动态模块分配起点 */
    CODEC__MAX = 256            /* 注册表槽位容量（obs_control 遍历用） */
};

/* codec 类别 */
enum { CODEC_KIND_AUDIO = 1, CODEC_KIND_VIDEO = 2 };

/* RTP 载荷规则；模块可自定义 depacketize/packetize 覆盖 */
enum {
    RTP_PAYLOAD_AUTO   = 0,   /* 按 sdp_name 由核心选择 */
    RTP_PAYLOAD_SIMPLE = 1,   /* 1 codec 帧 = 1 payload（G.711/G.729）*/
    RTP_PAYLOAD_AMR    = 2,   /* RFC 4867（CMR + ToC）*/
    RTP_PAYLOAD_H264   = 3,   /* RFC 6184 */
    RTP_PAYLOAD_VP8    = 4,   /* RFC 7741 */
    RTP_PAYLOAD_VP9    = 5,   /* RFC 9628 */
    RTP_PAYLOAD_CUSTOM = 255  /* 模块自带 unpack/pack */
};

/* 能力标记 */
#define CODEC_CAP_DEC       (1u << 0)
#define CODEC_CAP_ENC       (1u << 1)
#define CODEC_CAP_STATEFUL  (1u << 2)   /* 编解码器有跨帧状态（G.729/AMR）*/
#define CODEC_CAP_VIDEO     (1u << 3)
#define CODEC_CAP_FEC       (1u << 4)

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

/* 音频 RTP 载荷内的单个 codec 帧（AMR ToC / 多帧 G.729） */
typedef struct {
    const uint8_t *data;
    size_t         len;
    int            samples;   /* 该帧解码后的每声道 PCM 采样数 */
    int            speech;    /* 0=静音/CNG 帧（AMR SID）*/
} codec_frame_t;

/* 视频帧：核心不解释像素格式，由模块填 data/linesize/format；
 * priv 由模块持有（AVFrame* 等），有效期为 decode→encode 之间。 */
typedef struct {
    uint8_t *data[4];
    int      linesize[4];
    int      width;
    int      height;
    int      format;     /* 模块私有像素格式 */
    int      keyframe;
    uint32_t pts;        /* 90 kHz */
    void    *priv;
} video_frame_t;

typedef struct codec_sess codec_sess_t;

/* 编解码 ops（v2）。所有实现必须无锁、零 malloc（init 后）、有界写入。
 *
 * decoder/encoder init 分离：
 *   init         → 创建 decoder sess
 *   encoder_init → 创建 encoder sess（NULL 时回退 init）
 * 视频模块使用 vid_dec_init / vid_enc_init + vid_decode / vid_encode。
 */
typedef struct codec_ops {
    /* --- v1 兼容字段（保持原语义）--- */
    codec_id_t id;
    char       name[CODEC_NAME_MAX];
    int        clock_rate;
    int        channels;
    int        default_pt;     /* 静态 PT；动态编码填 -1 */
    int        ptime_ms;       /* 建议帧长 */
    int      (*init)(const char *fmtp, codec_sess_t **out);
    int      (*encoder_init)(const char *fmtp, codec_sess_t **out);
    void     (*fini)(codec_sess_t *s);
    int      (*decode)(codec_sess_t *s, const uint8_t *in, size_t in_len,
                       media_frame_t *out);
    int      (*encode)(codec_sess_t *s, const media_frame_t *in,
                       uint8_t *out, size_t cap, size_t *out_len);

    /* --- v2 元数据 --- */
    uint32_t    api_version;      /* 必须等于 NSBC_CODEC_API_VERSION */
    const char *sdp_name;         /* SDP rtpmap 名，如 "AMR-WB"/"H264" */
    const char *aliases;          /* 逗号分隔别名，如 "AMR_WB,AMRWB" */
    uint8_t     kind;             /* CODEC_KIND_AUDIO/VIDEO */
    uint8_t     payload;          /* RTP_PAYLOAD_* */
    uint16_t    priority;         /* 规划器偏好，越大越优先 */
    uint32_t    caps;             /* CODEC_CAP_* */
    const char *default_fmtp;     /* 如 AMR-WB "octet-align=1" */

    /* --- v2 扩展回调（可空）--- */
    int  (*fmtp_negotiate)(const char *local, const char *remote,
                           char *out, size_t cap);
    /* 音频非 1:1 载荷（AMR/AMR-WB）；NULL 时按 RTP_PAYLOAD_SIMPLE 处理 */
    int  (*rtp_unpack)(codec_sess_t *s, const uint8_t *payload, size_t len,
                       codec_frame_t *frames, int max_frames, int *n_frames);
    int  (*rtp_pack)(codec_sess_t *s, const codec_frame_t *frames, int n_frames,
                     uint8_t *out, size_t cap, size_t *out_len);

    /* 视频编解码（kind==VIDEO 时使用） */
    int  (*vid_dec_init)(const char *fmtp, codec_sess_t **out);
    int  (*vid_enc_init)(const char *fmtp, codec_sess_t **out);
    int  (*vid_decode)(codec_sess_t *s, const uint8_t *au, size_t len,
                       video_frame_t *out);
    int  (*vid_encode)(codec_sess_t *s, const video_frame_t *in,
                       uint8_t *out, size_t cap, size_t *out_len,
                       int *keyframe);
    void (*vid_frame_free)(codec_sess_t *s, video_frame_t *f);
} codec_ops_t;

/* ============================================================================
 * 注册表
 * ==========================================================================*/
/* 启动期登记内置 codec（ops->id 必须已填写且在 [1, CODEC_ID_BUILTIN_MAX)）*/
int  codec_registry_register(const codec_ops_t *ops);

/* 动态注册：ops->id 为 0 时由注册表分配 [CODEC_ID_DYNAMIC_MIN, CODEC__MAX)，
 * 并回写到 ops->id（ops 必须是可写且生命周期覆盖模块加载期）。
 * 返回分配的 codec_id，失败返回 CODEC_NONE。 */
codec_id_t codec_register(codec_ops_t *ops);
int  codec_unregister(codec_id_t id);

const codec_ops_t *codec_registry_find(codec_id_t id);
const codec_ops_t *codec_registry_find_pt(int pt, int clock_rate);
/* 名称/别名/SDP 名查找，大小写不敏感；name 可不以 '\0' 结尾 */
const codec_ops_t *codec_registry_find_name(const char *name, size_t len);
/* 按类别 + 名称查找（避免 audio/video 同名歧义）*/
const codec_ops_t *codec_find_by_name_kind(const char *name, size_t len, int kind);
/* 按静态 PT + 类别查找 */
const codec_ops_t *codec_find_by_pt_kind(int pt, int kind);
unsigned codec_registry_count(void);
int  codec_registry_foreach(int (*cb)(const codec_ops_t *, void *), void *ud);

/* 能力查询：能否作为源（解码）/目的（编码）*/
int  codec_can_decode(codec_id_t id);
int  codec_can_encode(codec_id_t id);
int  codec_kind_of(codec_id_t id);

/* 卸载安全：find_inc 成功后必须 codec_registry_dec */
const codec_ops_t *codec_registry_find_inc(codec_id_t id, int *out_slot);
void codec_registry_dec(int slot);
int  codec_registry_unload_wait(codec_id_t id, int timeout_ms);
int  codec_registry_unregister(codec_id_t id);

/* ============================================================================
 * 转码会话：src→dst（内部按需级联重采样，见 resample.h）
 * ==========================================================================*/
typedef struct transcode_sess transcode_sess_t;

/* 同采样率版本（沿用旧行为）*/
int  transcode_open(codec_id_t src, codec_id_t dst, transcode_sess_t **out);
/* 显式采样率/声道版本（AMR-WB 16k ↔ PCMA 8k 等）*/
int  transcode_open2(codec_id_t src, codec_id_t dst,
                     int src_rate, int dst_rate, int channels,
                     transcode_sess_t **out);
void transcode_close(transcode_sess_t *t);
/* 一帧 RTP 载荷进、一帧载荷出（无足够数据时 out_len=0/AGAIN） */
int  transcode_process(transcode_sess_t *t,
                       const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t cap, size_t *out_len);
codec_id_t transcode_src_codec_id(const transcode_sess_t *t);
codec_id_t transcode_dst_codec_id(const transcode_sess_t *t);
/* 协商出的目标腿 PT（0xFF=未设置）；会话创建后只写一次，媒体线程只读 */
void       transcode_set_dst_pt(transcode_sess_t *t, uint8_t pt);
uint8_t    transcode_get_dst_pt(const transcode_sess_t *t);

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
