/* G.729 (ITU-T G.729 Annex B) —— 帧级编解码器（bcg729 库）
 *
 * 编码：每 10ms = 80 样本 PCM → 10 字节码流（+ annexB SID 舒适噪声帧 2 字节）
 * 解码：每 10 字节码流 → 80 样本 PCM
 *
 * bcg729 是 Belledonne Communications 的开源实现，FreeSWITCH/Asterisk 都用它。
 * 用 NSBC_WITH_BCG729=1 编译开关启用；否则退化为 stub（注册 ops 但 init 返回 NOTSUP），
 * 保证整个工程编译不受 bcg729 是否存在的影响。
 *
 * codec_sess_t 内部包含：
 *   - bcg729EncoderChannelContextStruct *enc_ctx（bcg729 编码器上下文）
 *   - bcg729DecoderChannelContextStruct *dec_ctx（bcg729 解码器上下文）
 *   - int16_t pcm_buf[800]（编码侧跨包帧缓冲，最多缓存 10 帧样本=100ms）
 *   - int pcm_fill（已缓存样本数）
 *   - uint8_t dec_state（BFI/SID 跟踪状态，解码侧用）
 */
#include "media_codec.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * HAVE_BCG729 守卫
 *
 * 若构建系统链接了 bcg729（pkg-config bcg729 或 vendored src），
 *   make NSBC_WITH_BCG729=1 时 CPPFLAGS 里有 -DHAVE_BCG729
 * 否则走下方 stub 实现：codec_ops_t 注册成功但 init 返回 NSBC_NOTSUP，
 * relay_recv_one 遇 NOTSUP 会 log WARN 后原样转发，不丢包。
 * ==========================================================================*/
#ifdef HAVE_BCG729
#  include <bcg729/encoder.h>
#  include <bcg729/decoder.h>
#endif

/* bcg729 帧参数：固定 10ms / 8k / mono / 10 byte */
#define G729_FRAME_SAMPLES  80   /* 10ms @ 8kHz */
#define G729_FRAME_BYTES    10   /* 标准 G.729 码流帧大小 */
#define G729_SID_BYTES      2    /* Annex B SID 舒适噪声帧 */
#define G729_PCM_BUF_FRAMES 10   /* 跨包缓冲：最多 10 帧 = 100ms */

/* decode 写入 out->pcm 的安全上限 — 与 transcode_sess.pcm_buf[1500] 对齐，
 * 一次 RTP 包最多 15 帧 G.729 = 1200 样本，1500 足够 */
#define TRANSCODE_PCM_MAX   1500

/* ============================================================================
 * codec_sess 定义
 * ==========================================================================*/
struct codec_sess {
    codec_id_t  id;

#ifdef HAVE_BCG729
    /* bcg729 上下文 */
    void       *enc_ctx;
    void       *dec_ctx;

    /* 编码侧跨包帧缓冲：PCM 样本攒够 80 才出一帧
     * RTP 包可能不是 10ms 对齐的，这里缓存剩余样本等下一个包 */
    int16_t     pcm_buf[G729_FRAME_SAMPLES * G729_PCM_BUF_FRAMES];
    int         pcm_fill;
#endif
};

/* ============================================================================
 * HAVE_BCG729 路径：真实 bcg729 调用
 *
 * bcg729 v1.1.1 真实 API（来自 3rd/bcg729/include/bcg729/）：
 *
 *   encoder.h:
 *     initBcg729EncoderChannel(uint8_t enableVAD)           // 0=关闭 VAD/DTX（永远输出 10B）
 *     closeBcg729EncoderChannel(ctx)
 *     bcg729Encoder(ctx, inputFrame[80], bitStream[], bitStreamLength*) // 输出长度可变 0/2/10
 *     bcg729GetRFC3389Payload(ctx, payload[11])
 *
 *   decoder.h:
 *     initBcg729DecoderChannel(void)                         // 无参数！
 *     closeBcg729DecoderChannel(ctx)
 *     bcg729Decoder(ctx, bitStream[], bitStreamLen,
 *                   frameErasureFlag, SIDFrameFlag,
 *                   rfc3389PayloadFlag, signal[80])          // 7 参数！
 * ==========================================================================*/
#ifdef HAVE_BCG729

static int g729_init(const char *fmtp, codec_sess_t **out)
{
    (void)fmtp; /* P0 不解析 fmtp=annexb；VAD/DTX 默认关 */
    if (out == NULL) return NSBC_INVAL;
    codec_sess_t *s = (codec_sess_t *)calloc(1, sizeof(*s));
    if (s == NULL) return NSBC_NOMEM;
    s->id = CODEC_G729;
    s->pcm_fill = 0;

    /* 关闭 VAD/DTX → 编码器永远输出 10B，不产生 0/2B 帧（RTP payload 长度稳定） */
    s->enc_ctx = (void *)initBcg729EncoderChannel(0 /* enableVAD=0 */);
    s->dec_ctx = (void *)initBcg729DecoderChannel(/* void */);

    if (!s->enc_ctx || !s->dec_ctx) {
        if (s->enc_ctx) closeBcg729EncoderChannel((bcg729EncoderChannelContextStruct *)s->enc_ctx);
        if (s->dec_ctx) closeBcg729DecoderChannel((bcg729DecoderChannelContextStruct *)s->dec_ctx);
        free(s);
        return NSBC_NOMEM;
    }
    *out = s;
    return NSBC_OK;
}

static void g729_fini(codec_sess_t *s)
{
    if (s == NULL) return;
    if (s->enc_ctx) closeBcg729EncoderChannel((bcg729EncoderChannelContextStruct *)s->enc_ctx);
    if (s->dec_ctx) closeBcg729DecoderChannel((bcg729DecoderChannelContextStruct *)s->dec_ctx);
    free(s);
}

/* 从 RTP payload 解出多个 G.729 帧 → 16bit PCM 样本
 *
 * decoder API（7 参数）：
 *   bcg729Decoder(ctx, bitStream[], bitStreamLen,
 *                frameErasureFlag, SIDFrameFlag,
 *                rfc3389PayloadFlag, signal[80])
 *
 * bitStreamLen 正常帧 = 10B，Annex B SID 帧 = 2B。
 * frameErasureFlag=0（正常帧），SIDFrameFlag=0，rfc3389PayloadFlag=0
 */
static int g729_decode(codec_sess_t *s, const uint8_t *in, size_t in_len,
                        media_frame_t *out)
{
    if (s == NULL || in == NULL || out == NULL || out->pcm == NULL) return NSBC_INVAL;

    size_t consumed = 0;
    out->samples = 0;
    out->rate = 8000;
    out->channels = 1;

    /* 每轮处理一帧：先猜是 10B 正常帧；如果剩余 < 10B 则按 SID（2B）处理 */
    while (consumed < in_len) {
        uint8_t bitStream[10];
        uint8_t bitStreamLen;  /* 传给 bcg729Decoder 的实际输入长度 */
        uint8_t SIDFrameFlag = 0;

        if (in_len - consumed >= G729_FRAME_BYTES) {
            /* 正常语音帧：10B */
            memcpy(bitStream, in + consumed, G729_FRAME_BYTES);
            bitStreamLen = G729_FRAME_BYTES;
            consumed += G729_FRAME_BYTES;
        } else if (in_len - consumed >= G729_SID_BYTES) {
            /* Annex B SID 帧：2B，SIDFrameFlag=1 */
            memcpy(bitStream, in + consumed, G729_SID_BYTES);
            memset(bitStream + G729_SID_BYTES, 0, G729_FRAME_BYTES - G729_SID_BYTES);
            bitStreamLen = G729_SID_BYTES;
            SIDFrameFlag = 1;
            consumed += G729_SID_BYTES;
        } else {
            /* 剩余 0-1B：垃圾，丢弃 */
            break;
        }

        int16_t frame_out[G729_FRAME_SAMPLES];

        /* 真实 bcg729 v1.1.1 7 参数解码 API */
        bcg729Decoder(
            (bcg729DecoderChannelContextStruct *)s->dec_ctx,
            bitStream,              /* 码流字节 */
            bitStreamLen,           /* 实际长度（10 或 2） */
            0,                      /* frameErasureFlag：0=正常 */
            SIDFrameFlag,           /* 0=正常帧, 1=SID 帧 */
            0,                      /* rfc3389PayloadFlag：0=普通 G.729 SID */
            frame_out               /* 输出 80 样本 PCM */
        );

        /* 写入 out->pcm（确保不越界 — TRANSCODE_PCM_MAX 和调用方缓冲对齐） */
        size_t cap = TRANSCODE_PCM_MAX - (size_t)out->samples;
        size_t copy = G729_FRAME_SAMPLES < cap ? G729_FRAME_SAMPLES : cap;
        if (copy == 0) break;
        memcpy(out->pcm + out->samples, frame_out, copy * sizeof(int16_t));
        out->samples += (int)copy;
    }
    return NSBC_OK;
}

/* 把 media_frame_t 里的 PCM 样本编码成 G.729 码流
 *
 * encoder API（4 参数）：
 *   bcg729Encoder(ctx, inputFrame[80], bitStream[], bitStreamLength*)
 *
 * enableVAD=0（init 时设）→ 输出永远是 10B 正常帧，bitStreamLength 恒等于 10。
 * 所以 encode 比 decode 简单：攒够 80 样本 → 编 10B → 追加到 out。
 */
static int g729_encode(codec_sess_t *s, const media_frame_t *in,
                        uint8_t *out, size_t cap, size_t *out_len)
{
    if (s == NULL || in == NULL || in->pcm == NULL || out == NULL || out_len == NULL)
        return NSBC_INVAL;

    size_t written = 0;
    int samples = in->samples;
    const int16_t *pcm = in->pcm;

    /* 先把新 PCM 样本追加到 sess 的帧缓冲 */
    int space_in_buf = (int)(sizeof(s->pcm_buf) / sizeof(s->pcm_buf[0])) - s->pcm_fill;
    if (samples > space_in_buf)
        samples = space_in_buf;
    if (samples > 0) {
        memcpy(s->pcm_buf + s->pcm_fill, pcm, samples * sizeof(int16_t));
        s->pcm_fill += samples;
        /* VAD=0 不需要保留 VAD/DTX 状态残留——直接 advance 指针 */
    }

    /* 只要缓冲里有 >= 80 样本，就编码出一帧 */
    while (s->pcm_fill >= G729_FRAME_SAMPLES && written + G729_FRAME_BYTES <= cap) {
        uint8_t frame[10];
        uint8_t bitStreamLength = 0;
        bcg729Encoder(
            (bcg729EncoderChannelContextStruct *)s->enc_ctx,
            s->pcm_buf,               /* 输入 80 样本 PCM */
            frame,                    /* 输出码流（VAD=0 时永远写 10B） */
            &bitStreamLength          /* 实际输出长度（预期=10） */
        );
        /* VAD=0 时 bitStreamLength 必为 10；万一 VAD 开了或别的情况，安全起见 */
        size_t frame_len = bitStreamLength > 0 ? bitStreamLength : G729_FRAME_BYTES;
        if (frame_len > cap - written) frame_len = cap - written;
        if (frame_len > 0) {
            memcpy(out + written, frame, frame_len);
            written += frame_len;
        }
        /* 左移剩余样本 */
        s->pcm_fill -= G729_FRAME_SAMPLES;
        if (s->pcm_fill > 0)
            memmove(s->pcm_buf, s->pcm_buf + G729_FRAME_SAMPLES,
                    s->pcm_fill * sizeof(int16_t));
    }

    *out_len = written;
    return NSBC_OK;
}

#endif /* HAVE_BCG729 */

/* ============================================================================
 * Stub 实现：HAVE_BCG729 未定义时编译路径
 *
 * codec_ops_t 能注册到 registry，但 init 返回 NSBC_NOTSUP，
 * bridge_confirm 里 transcode_open(CODEC_PCMU, CODEC_G729) 会失败，
 * 日志显示 "transcode_open(PCMU→G729) rc=-9"，relay 层不触发转码分支，
 * 原始 RTP 包直接透传，上层行为完全正常。
 * ==========================================================================*/
#ifndef HAVE_BCG729

static int g729_init(const char *fmtp, codec_sess_t **out)
{
    (void)fmtp;
    (void)out;
    return NSBC_NOTSUP; /* bcg729 未链接 */
}

static void g729_fini(codec_sess_t *s)
{
    (void)s;
}

static int g729_decode(codec_sess_t *s, const uint8_t *in, size_t in_len,
                        media_frame_t *out)
{
    (void)s; (void)in; (void)in_len; (void)out;
    return NSBC_NOTSUP;
}

static int g729_encode(codec_sess_t *s, const media_frame_t *in,
                        uint8_t *out, size_t cap, size_t *out_len)
{
    (void)s; (void)in; (void)out; (void)cap; (void)out_len;
    return NSBC_NOTSUP;
}

#endif /* !HAVE_BCG729 */

/* ============================================================================
 * codec_ops_t 描述符 + g729_register()
 * ==========================================================================*/
static const codec_ops_t G729_OPS = {
    .id          = CODEC_G729,
    .name        = "G729",
    .clock_rate  = 8000,
    .channels    = 1,
    .default_pt  = 18,       /* RFC 3551 静态 PT */
    .ptime_ms    = 10,       /* G.729 标准帧长 */
    .init        = g729_init,
    .fini        = g729_fini,
    .decode      = g729_decode,
    .encode      = g729_encode,
};

int g729_register(void)
{
    return codec_registry_register(&G729_OPS);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
