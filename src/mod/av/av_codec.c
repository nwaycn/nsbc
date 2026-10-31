/* Phase 3 mod_av: FFmpeg 统一 codec_ops 实现
 *
 * 架构：所有 codec 共用 codec_registry 框架（media_codec.c），
 *   - HAVE_FFMPEG 时 init/fini/decode/encode 为真实 AVCodecContext 调用骨架
 *   - 无 HAVE_FFMPEG 时 stub 返回 NSBC_NOTSUP，但 ops 仍完整注册
 *
 * FFmpeg codec ID 映射：
 *   CODEC_G722     → AV_CODEC_ID_ADPCM_G722
 *   CODEC_G723_1   → AV_CODEC_ID_G723_1
 *   CODEC_G726_16  → AV_CODEC_ID_ADPCM_G726  (bit_rate=16000)
 *   CODEC_G726_24  → AV_CODEC_ID_ADPCM_G726  (bit_rate=24000)
 *   CODEC_G726_32  → AV_CODEC_ID_ADPCM_G726  (bit_rate=32000)
 *   CODEC_G726_40  → AV_CODEC_ID_ADPCM_G726  (bit_rate=40000)
 *   CODEC_OPUS     → AV_CODEC_ID_OPUS
 *   CODEC_H264     → AV_CODEC_ID_H264
 *   CODEC_VP8      → AV_CODEC_ID_VP8
 *   CODEC_VP9      → AV_CODEC_ID_VP9
 *
 * configure 需加（本文件不修改 configure，注释供参考）：
 *   if pkg-config --exists libavcodec libavutil libswresample libswscale; then
 *     AV_CPPFLAGS += $(pkg-config --cflags libavcodec libavutil libswresample libswscale)
 *     AV_LDLIBS   += $(pkg-config --libs   libavcodec libavutil libswresample libswscale)
 *     CFLAGS      += -DHAVE_FFMPEG
 *   fi
 */
#include "media_codec.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#endif

/* ============================================================================
 * codec_sess_t：mod_av 专用 codec 会话结构
 *
 * media_codec.h 里是前向声明 struct codec_sess，
 * 本文件定义其真实布局。每个已注册的 codec_ops_t 都指向本结构。
 * ==========================================================================*/
struct codec_sess {
    codec_id_t  id;          /* codec 标识 */
    int         is_decoder;  /* 1=decode sess, 0=encode sess（FFmpeg 分 encoder/decoder）*/

#ifdef HAVE_FFMPEG
    /* 解码器路径 */
    AVCodecContext *dec_ctx;
    AVPacket       *pkt;
    AVFrame        *frame;
    SwrContext     *swr;      /* 重采样（如 G.722 16k → SBC 中间 8k），Phase 4 启用 */
    int             swr_out_rate; /* swr->out_sample_rate 在 FFmpeg 6.x 是不透明，自己缓存 */

    /* 编码器路径 */
    AVCodecContext *enc_ctx;
    AVPacket       *enc_pkt;
    AVFrame        *enc_frame;
#endif
};

/* ============================================================================
 * FFmpeg codec 参数表（按 codec_id_t 映射到 AV_CODEC_ID_*）
 *
 * 每个 entry 描述一种 FFmpeg 可处理的编解码器。
 * g726_16/24/32/40 都映射到同一个 AV_CODEC_ID_ADPCM_G726，
 * 靠 bit_rate 区分实际编码速率。
 * ==========================================================================*/
#ifdef HAVE_FFMPEG
struct ffmpeg_codec_param {
    codec_id_t   id;
    enum AVCodecID ffmpeg_id;
    int          target_bit_rate; /* G726 系列用；其他 0 = codec 默认 */
    int          is_video;       /* 音频=0, 视频=1 */
};

static const struct ffmpeg_codec_param FF_PARAMS[] = {
    { CODEC_G722,    AV_CODEC_ID_ADPCM_G722,     0,      0 },
    { CODEC_G723_1,  AV_CODEC_ID_G723_1,         0,      0 },
    { CODEC_G726_16, AV_CODEC_ID_ADPCM_G726,     16000,  0 },
    { CODEC_G726_24, AV_CODEC_ID_ADPCM_G726,     24000,  0 },
    { CODEC_G726_32, AV_CODEC_ID_ADPCM_G726,     32000,  0 },
    { CODEC_G726_40, AV_CODEC_ID_ADPCM_G726,     40000,  0 },
    { CODEC_OPUS,    AV_CODEC_ID_OPUS,           0,      0 },
    { CODEC_H264,    AV_CODEC_ID_H264,           0,      1 },
    { CODEC_VP8,     AV_CODEC_ID_VP8,            0,      1 },
    { CODEC_VP9,     AV_CODEC_ID_VP9,            0,      1 },
};

static const struct ffmpeg_codec_param *find_ff_param(codec_id_t id)
{
    for (size_t i = 0; i < sizeof(FF_PARAMS) / sizeof(FF_PARAMS[0]); i++)
        if (FF_PARAMS[i].id == id) return &FF_PARAMS[i];
    return NULL;
}
#endif /* HAVE_FFMPEG */

/* ============================================================================
 * 通用 init：decoder / encoder 二选一（FFmpeg 分两个不同的 AVCodec）
 *
 * decoder 路径：
 *   avcodec_find_decoder → avcodec_alloc_context3 → avcodec_parameters_to_context
 *   → avcodec_open2
 *
 * encoder 路径：
 *   avcodec_find_encoder → avcodec_alloc_context3 → 设置 bit_rate/sample_fmt/
 *   sample_rate/channels → avcodec_open2
 * ==========================================================================*/
static int av_codec_open(struct codec_sess *sess, int is_decoder)
{
#ifdef HAVE_FFMPEG
    const struct ffmpeg_codec_param *p = find_ff_param(sess->id);
    if (!p) return NSBC_NOTSUP;

    const AVCodec *codec;
    if (is_decoder)
        codec = avcodec_find_decoder(p->ffmpeg_id);
    else
        codec = avcodec_find_encoder(p->ffmpeg_id);

    if (!codec) {
        llog(LOG_WARN, "av", "FFmpeg codec not found for id=%d decoder=%d", p->ffmpeg_id, is_decoder);
        return NSBC_NOTSUP;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return NSBC_NOMEM;

    ctx->bit_rate = p->target_bit_rate;

    if (p->is_video) {
        /* 视频：默认 176x144 QCIF，Phase 4 由 fmtp 协商覆盖 */
        ctx->width  = 176;
        ctx->height = 144;
        ctx->time_base = (AVRational){1, 90000};
    } else {
        /* 音频：通用 PCM S16 interleaved */
        ctx->sample_fmt  = AV_SAMPLE_FMT_S16;
        ctx->sample_rate = (sess->id == CODEC_OPUS) ? 48000 :
                           (sess->id == CODEC_G722) ? 16000 : 8000;
#if LIBAVCODEC_VERSION_MAJOR >= 59
        /* FFmpeg 5.1+: channels → ch_layout */
        av_channel_layout_default(&ctx->ch_layout, 1);
#else
        ctx->channels = 1;
#endif
    }

    /* 打开 codec */
    int rc = avcodec_open2(ctx, codec, NULL);
    if (rc < 0) {
        avcodec_free_context(&ctx);
        llog(LOG_WARN, "av", "avcodec_open2 failed rc=%d id=%d", rc, sess->id);
        return NSBC_ERR;
    }

    /* Phase 4: 音频 decoder 输出 → S16 interleaved 的 swr 准备
     * 如果 decoder 输出本来就是 AV_SAMPLE_FMT_S16 + 目标 sample_rate/channels 匹配，
     * 跳过 swr（零开销）。否则创建 swr_context 做：sample_fmt + sample_rate + channels 三件套重采样。
     * encoder 方向不需要 swr（输入假设 transcode pipeline 已处理好）。*/
    if (is_decoder && !p->is_video) {
        enum AVSampleFormat target_fmt   = AV_SAMPLE_FMT_S16;
        int                target_rate   = (sess->id == CODEC_OPUS) ? 48000 :
                                           (sess->id == CODEC_G722) ? 16000 : 8000;
        int                target_ch     = 1;

        if (ctx->sample_fmt != target_fmt ||
            ctx->sample_rate != target_rate ||
#ifdef LIBSWRESAMPLE_VERSION_MAJOR
            ctx->ch_layout.nb_channels != target_ch) {
#else
            ctx->channels != target_ch) {
#endif

#if defined(LIBSWRESAMPLE_VERSION_MAJOR) && LIBSWRESAMPLE_VERSION_MINOR >= 6
            /* libswresample 4.6+ (FFmpeg 6.1+): swr_alloc_set_opts2 + AVChannelLayout* */
            SwrContext *swr = swr_alloc_set_opts2(
                NULL,
                /* out */
                &(AVChannelLayout)AV_CHANNEL_LAYOUT_MONO,
                target_fmt,
                target_rate,
                /* in */
                &ctx->ch_layout,
                ctx->sample_fmt,
                ctx->sample_rate,
                0, NULL);
#else
            /* 旧版 libswresample (<4.6): swr_alloc_set_opts + channel mask */
            uint64_t in_mask =
#if LIBAVCODEC_VERSION_MAJOR >= 59
                ctx->ch_layout.u.mask;
#else
                ctx->channel_layout;
#endif
            SwrContext *swr = swr_alloc_set_opts(
                NULL,
                AV_CH_LAYOUT_MONO,
                target_fmt,
                target_rate,
                in_mask,
                ctx->sample_fmt,
                ctx->sample_rate,
                0, NULL);
#endif
            if (swr) {
                swr_init(swr);
                sess->swr = swr;
#if defined(LIBSWRESAMPLE_VERSION_MAJOR) && LIBSWRESAMPLE_VERSION_MINOR >= 6
                sess->swr_out_rate = target_rate; /* swr opaque in new libswresample, cache locally */
#endif
                llog(LOG_DEBUG, "av",
                     "swr init: codec sample_fmt=%d→%d rate=%d→%d ch=%d→%d",
                     ctx->sample_fmt, target_fmt,
                     ctx->sample_rate, target_rate,
#ifdef LIBSWRESAMPLE_VERSION_MAJOR
                     ctx->ch_layout.nb_channels, target_ch);
#else
                     ctx->channels, target_ch);
#endif
            }
        }
    }

    if (is_decoder) {
        sess->dec_ctx = ctx;
        sess->pkt     = av_packet_alloc();
        sess->frame   = av_frame_alloc();
        if (!sess->pkt || !sess->frame) {
            av_packet_free(&sess->pkt);
            av_frame_free(&sess->frame);
            avcodec_free_context(&sess->dec_ctx);
            return NSBC_NOMEM;
        }
    } else {
        sess->enc_ctx   = ctx;
        sess->enc_pkt   = av_packet_alloc();
        sess->enc_frame = av_frame_alloc();
        if (!sess->enc_pkt || !sess->enc_frame) {
            av_packet_free(&sess->enc_pkt);
            av_frame_free(&sess->enc_frame);
            avcodec_free_context(&sess->enc_ctx);
            return NSBC_NOMEM;
        }
    }
    sess->is_decoder = is_decoder;
    return NSBC_OK;
#else
    (void)sess; (void)is_decoder;
    return NSBC_NOTSUP;
#endif /* HAVE_FFMPEG */
}

static void av_codec_close(struct codec_sess *sess)
{
    if (!sess) return;
#ifdef HAVE_FFMPEG
    if (sess->swr) swr_free(&sess->swr);

    if (sess->is_decoder) {
        av_packet_free(&sess->pkt);
        av_frame_free(&sess->frame);
        avcodec_free_context(&sess->dec_ctx);
    } else {
        av_packet_free(&sess->enc_pkt);
        av_frame_free(&sess->enc_frame);
        avcodec_free_context(&sess->enc_ctx);
    }
#endif
    free(sess);
}

/* ============================================================================
 * decode 骨架：avcodec_send_packet → avcodec_receive_frame
 *   → av_samples_get_buffer_size → memcpy S16 interleaved 到 out->pcm
 * ==========================================================================*/
static int av_codec_decode(struct codec_sess *sess,
                           const uint8_t *in, size_t in_len,
                           media_frame_t *out)
{
    if (!sess || !in || !out || !out->pcm) return NSBC_INVAL;
#ifdef HAVE_FFMPEG
    if (!sess->dec_ctx) return NSBC_INVAL;

    AVPacket *pkt = sess->pkt;
    /* FFmpeg 6.x: av_init_packet deprecated, pkt is already zeroed by av_packet_alloc */
    pkt->data   = (uint8_t *)in;
    pkt->size   = (int)in_len;

    int rc = avcodec_send_packet(sess->dec_ctx, pkt);
    if (rc == AVERROR(EAGAIN)) return NSBC_AGAIN;
    if (rc < 0) return NSBC_ERR;

    /* 清空 pkt，复用 */
    pkt->data = NULL;
    pkt->size = 0;

    out->samples  = 0;
#if LIBAVCODEC_VERSION_MAJOR >= 59
    out->channels = sess->dec_ctx->ch_layout.nb_channels;
#else
    out->channels = sess->dec_ctx->channels;
#endif
    out->rate     = sess->dec_ctx->sample_rate;
    if (sess->id == CODEC_OPUS) out->rate = 48000; /* Opus 强制 48k 输出 */

    AVFrame *frame = sess->frame;
    while (1) {
        rc = avcodec_receive_frame(sess->dec_ctx, frame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) return NSBC_ERR;

        /* 音频：planar → interleaved + 格式/sample_rate/channels 重采样
         *
         * 两条路径：
         *   A. sess->swr != NULL — av_codec_open 里检测到 decoder 输出不匹配 S16+mono+8k
         *      （G.722 16k planar S16, Opus 48k planar float, H.264 视频无 swr）
         *      → swr_convert 真重采样到 interleaved S16 mono 8k（或 codec 默认 rate）
         *   B. sess->swr == NULL — decoder 输出已经匹配（G.723.1 8k planar S16? 视 codec）
         *      → 直接 memcpy interleaved S16 输出
         *
         * 视频（is_video）跳过 swr，直接拷贝 frame->data[0] 里的原始 YUV */
        if (sess->swr) {
            /* swr 路径：重采样到 interleaved S16 mono，每样本占 sizeof(int16_t) * 2 = 4B 预留 */
            uint8_t swr_out[4096];  /* stack buffer，无需 malloc */
            /* FFmpeg 6.x: swr_convert out 类型是 uint8_t * const *，不能直接传数组地址 */
            uint8_t *swr_out_ptr = swr_out;
            int converted = swr_convert(sess->swr,
                &swr_out_ptr, sizeof(swr_out) / sizeof(int16_t),
                (const uint8_t **)frame->data, frame->nb_samples);
            if (converted > 0) {
                int16_t *dst = (int16_t *)(out->pcm + out->samples);
                memcpy(dst, swr_out, converted * sizeof(int16_t));
                out->samples += converted;
#if defined(LIBSWRESAMPLE_VERSION_MAJOR) && LIBSWRESAMPLE_VERSION_MINOR >= 6
                /* 新版 libswresample opaque swr, use cached value */
                out->rate = sess->swr_out_rate;
#else
                out->rate = sess->swr->out_sample_rate;
#endif
            }
        } else if (frame->format == AV_SAMPLE_FMT_S16) {
            /* 非重采样路径：codec 输出已是 interleaved S16 + 匹配 sample_rate/channels */
#if LIBAVCODEC_VERSION_MAJOR >= 59
            int frame_ch = frame->ch_layout.nb_channels;
#else
            int frame_ch = frame->channels;
#endif
            size_t frame_bytes = av_samples_get_buffer_size(
                NULL, frame_ch, frame->nb_samples,
                AV_SAMPLE_FMT_S16, 1);
            if (frame_bytes > (out->samples * sizeof(int16_t) * 2)) {
                size_t cap = 1500 * sizeof(int16_t) * 2; /* 与 transcode 缓冲对齐 */
                if (frame_bytes > cap) frame_bytes = cap;
                memcpy(out->pcm + out->samples, frame->data[0], frame_bytes);
                out->samples += frame->nb_samples;
            } else {
                out->samples += frame->nb_samples; /* skip cap check */
            }
        } else {
            /* 无 swr 但输出格式未知 — 警告跳过（Phase 7 日志）*/
            llog(LOG_WARN, "av",
                 "av_codec_decode: unknown sample format %d, skip frame", frame->format);
        }
        av_frame_unref(frame);
    }
    return NSBC_OK;
#else
    (void)in_len;
    return NSBC_NOTSUP;
#endif /* HAVE_FFMPEG */
}

/* ============================================================================
 * encode 骨架：把 media_frame_t PCM 喂进 AVFrame → avcodec_send_frame
 *   → avcodec_receive_packet → memcpy 到 out 缓冲
 * ==========================================================================*/
static int av_codec_encode(struct codec_sess *sess,
                           const media_frame_t *in,
                           uint8_t *out, size_t cap, size_t *out_len)
{
    if (!sess || !in || !in->pcm || !out || !out_len) return NSBC_INVAL;
#ifdef HAVE_FFMPEG
    if (!sess->enc_ctx) return NSBC_INVAL;

    AVFrame *frame = sess->enc_frame;
    memset(frame, 0, sizeof(*frame));
    frame->nb_samples     = in->samples;
    frame->sample_rate    = in->rate;
    frame->format         = sess->enc_ctx->sample_fmt;
#if LIBAVCODEC_VERSION_MAJOR >= 59
    frame->ch_layout      = sess->enc_ctx->ch_layout;
#else
    frame->channel_layout = sess->enc_ctx->channel_layout;
    frame->channels       = sess->enc_ctx->channels;
#endif

    size_t samples_size = in->samples * in->channels * sizeof(int16_t);
    int rc = avcodec_fill_audio_frame(frame, in->channels,
                                      AV_SAMPLE_FMT_S16,
                                      (const uint8_t *)in->pcm, samples_size, 0);
    if (rc < 0) return NSBC_ERR;

    rc = avcodec_send_frame(sess->enc_ctx, frame);
    if (rc == AVERROR(EAGAIN)) { av_frame_unref(frame); return NSBC_AGAIN; }
    if (rc < 0) { av_frame_unref(frame); return NSBC_ERR; }
    av_frame_unref(frame);

    size_t written = 0;
    AVPacket *pkt = sess->enc_pkt;
    while (written < cap) {
        rc = avcodec_receive_packet(sess->enc_ctx, pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) return NSBC_ERR;

        if (pkt->size > (int)(cap - written)) {
            memcpy(out + written, pkt->data, cap - written);
            written += cap - written;
        } else {
            memcpy(out + written, pkt->data, pkt->size);
            written += pkt->size;
        }
        av_packet_unref(pkt);
    }
    *out_len = written;
    return NSBC_OK;
#else
    (void)cap;
    return NSBC_NOTSUP;
#endif /* HAVE_FFMPEG */
}

/* ============================================================================
 * init / fini 包装：包装成 codec_ops_t 需要的签名
 *
 * 注意：transcode_open 会分别调用 codec_ops.init（decoder 路径）和
 * codec_ops.encode 所属的 encoder sess init（encoder 路径）—— 每次
 * init 只开一个方向的 codec（decoder 或 encoder）。
 * 所以 AVCODEC_DECLARE_OPS 宏生成两套独立 init：
 *   - cname##_init  → av_codec_open(s, 1)  // decoder
 *   - cname##_enc_init → av_codec_open(s, 0)  // encoder
 * codec_ops_t 只有一个 init 函数指针；decode ops 绑 decoder init，
 * encode ops 绑 encoder init（宏内部处理）。
 * ==========================================================================*/

/* 简单 fmtp 解析：抽 aptime/bitrate/ptime 协商值
 * 返回协商后的 ptime_ms（ms），找不到或不合法返回 0（用 codec 默认）。
 * 暂未接入 transcode pipeline，标记 unused 避免 -Werror 误报。 */
__attribute__((unused)) static int parse_fmtp_ptime(const char *fmtp)
{
    if (!fmtp || !fmtp[0]) return 0;
    /* 找 aptime=xx 或 ptime=xx */
    const char *p = strcasestr(fmtp, "aptime=");
    if (!p) p = strcasestr(fmtp, "ptime=");
    if (!p) p = strcasestr(fmtp, "pt=");
    if (!p) return 0;

    p += strchr(p, '=') - p + 1;
    while (*p == ' ') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (!end || v <= 0 || v > 1000) return 0;
    return (int)v;
}

#define AVCODEC_DECLARE_OPS(cid, cname, cr, ch, pt, ptm)                      \
    /* decoder sess init */                                                   \
    static int cname##_init(const char *fmtp, codec_sess_t **out) {            \
        (void)fmtp;                                                           \
        if (!out) return NSBC_INVAL;                                          \
        codec_sess_t *s = (codec_sess_t *)calloc(1, sizeof(*s));             \
        if (!s) return NSBC_NOMEM;                                            \
        s->id = cid;                                                          \
        int rc = av_codec_open(s, /*is_decoder=*/1);                          \
        if (rc != NSBC_OK) { free(s); return rc; }                            \
        *out = s;                                                             \
        return NSBC_OK;                                                       \
    }                                                                         \
    /* encoder sess init */                                                   \
    static int cname##_enc_init(const char *fmtp, codec_sess_t **out) {        \
        (void)fmtp;                                                           \
        if (!out) return NSBC_INVAL;                                          \
        codec_sess_t *s = (codec_sess_t *)calloc(1, sizeof(*s));             \
        if (!s) return NSBC_NOMEM;                                            \
        s->id = cid;                                                          \
        int rc = av_codec_open(s, /*is_decoder=*/0);                          \
        if (rc != NSBC_OK) { free(s); return rc; }                            \
        *out = s;                                                             \
        return NSBC_OK;                                                       \
    }                                                                         \
    static void cname##_fini(codec_sess_t *s) { av_codec_close(s); }          \
    static int cname##_decode(codec_sess_t *s,                                \
                              const uint8_t *in, size_t il,                   \
                              media_frame_t *out)                             \
        { return av_codec_decode(s, in, il, out); }                          \
    static int cname##_encode(codec_sess_t *s,                                \
                              const media_frame_t *in,                        \
                              uint8_t *out, size_t cap, size_t *ol)           \
        { return av_codec_encode(s, in, out, cap, ol); }                     \
    static void cname##_fill_ops(codec_ops_t *ops) {                          \
        memset(ops, 0, sizeof(*ops));                                         \
        ops->id          = cid;                                               \
        snprintf(ops->name, sizeof(ops->name), #cname);                       \
        ops->clock_rate  = cr;                                                \
        ops->channels    = ch;                                                \
        ops->default_pt  = pt;                                                \
        ops->ptime_ms    = ptm;                                               \
        ops->init          = cname##_init;                                    \
        ops->encoder_init  = cname##_enc_init;                                \
        ops->fini          = cname##_fini;                                    \
        ops->decode        = cname##_decode;                                  \
        ops->encode        = cname##_encode;                                  \
    }

/* 每个 codec 的工厂函数（ops 填充 + 真实/stub 函数对） */
AVCODEC_DECLARE_OPS(CODEC_G722,     avcodec_g722,     16000, 1,  9,  20)
AVCODEC_DECLARE_OPS(CODEC_G723_1,   avcodec_g7231,    8000,  1, -1,  30)
AVCODEC_DECLARE_OPS(CODEC_G726_16,  avcodec_g726_16,  8000,  1, -1,  20)
AVCODEC_DECLARE_OPS(CODEC_G726_24,  avcodec_g726_24,  8000,  1, -1,  20)
AVCODEC_DECLARE_OPS(CODEC_G726_32,  avcodec_g726_32,  8000,  1, -1,  20)
AVCODEC_DECLARE_OPS(CODEC_G726_40,  avcodec_g726_40,  8000,  1, -1,  20)
AVCODEC_DECLARE_OPS(CODEC_OPUS,     avcodec_opus,     48000, 1, -1,  20)
AVCODEC_DECLARE_OPS(CODEC_H264,     avcodec_h264,     90000, 0, 96,   0)
AVCODEC_DECLARE_OPS(CODEC_VP8,      avcodec_vp8,      90000, 0, -1,   0)
AVCODEC_DECLARE_OPS(CODEC_VP9,      avcodec_vp9,      90000, 0, -1,   0)

/* ============================================================================
 * av_register_all_codecs：把全部 ops 注册进 codec_registry
 *
 * 成功全部注册 → 返回 NSBC_OK（个别重复/失败由 registry 返回的 rc 决定）
 * ==========================================================================*/
int av_register_all_codecs(void)
{
    /* 静态槽位数组 —— registry 只存指针不做 copy，
     * 每个 codec 必须有独立的、跨函数有效的存储。
     * 之前用单个栈变量 `codec_ops_t ops`，循环内被 fill_ops 覆盖后
     * codec_registry_register(&ops) 存的栈地址在函数返回后悬空，
     * 导致 show codecs 显示 NAME/RATE/PTIME 等全是垃圾值。 */
    static codec_ops_t g_ops_slots[16];   /* 16 > 10 个 AV codec，留点余量 */
    int slot = 0;
    int total = 0, ok = 0;

#define REG_CODEC(fn)                                                         \
    do {                                                                      \
        if (slot >= (int)(sizeof(g_ops_slots)/sizeof(g_ops_slots[0]))) {       \
            llog(LOG_WARN, "av", "ops slot exhausted, skip %s", #fn);         \
            break;                                                            \
        }                                                                     \
        codec_ops_t *ops = &g_ops_slots[slot++];                              \
        fn(ops);                                                              \
        int rc = codec_registry_register(ops);                                \
        total++;                                                              \
        if (rc == NSBC_OK) {                                                  \
            ok++;                                                             \
            llog(LOG_INFO, "av", "registered ops id=%d name=%s PT=%d",        \
                 ops->id, ops->name, ops->default_pt);                        \
        } else {                                                              \
            llog(LOG_WARN, "av", "register failed id=%d name=%s rc=%d",       \
                 ops->id, ops->name, rc);                                     \
        }                                                                     \
    } while (0)

    REG_CODEC(avcodec_g722_fill_ops);
    REG_CODEC(avcodec_g7231_fill_ops);
    REG_CODEC(avcodec_g726_16_fill_ops);
    REG_CODEC(avcodec_g726_24_fill_ops);
    REG_CODEC(avcodec_g726_32_fill_ops);
    REG_CODEC(avcodec_g726_40_fill_ops);
    REG_CODEC(avcodec_opus_fill_ops);
    REG_CODEC(avcodec_h264_fill_ops);
    REG_CODEC(avcodec_vp8_fill_ops);
    REG_CODEC(avcodec_vp9_fill_ops);

#undef REG_CODEC

    llog(LOG_INFO, "av", "av_register_all_codecs: %d/%d registered (HAVE_FFMPEG=%d)",
         ok, total,
#ifdef HAVE_FFMPEG
         1
#else
         0
#endif
    );
    return (ok == total) ? NSBC_OK : NSBC_ERR;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
