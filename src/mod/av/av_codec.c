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
#include <stdarg.h>
#include <pthread.h>

#ifdef HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/base64.h>
#include <libavutil/samplefmt.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

/* FFmpeg 的 av_log 默认写 stderr，daemon 化后看不到。
 * 桥接到 llog，便于定位 avcodec_open2 等失败的具体原因。 */
static void av_log_to_llog(void *avcl, int level, const char *fmt, va_list vl)
{
    (void)avcl;
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, vl);
    if (n <= 0) return;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    if (n == 0) return;
    int lv = (level <= AV_LOG_ERROR)   ? LOG_ERR  :
             (level <= AV_LOG_WARNING) ? LOG_WARN : LOG_DEBUG;
    llog(lv, "ffmpeg", "%s", buf);
}

static pthread_once_t g_av_log_once = PTHREAD_ONCE_INIT;
static void av_log_bridge_init(void)
{
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(av_log_to_llog);
}
static void av_log_bridge(void)
{
    (void)pthread_once(&g_av_log_once, av_log_bridge_init);
}
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

    /* 视频编码器懒初始化（宽高在首个解码帧后确定） */
    int             vid_ready;
    int             vid_w;
    int             vid_h;
    int             vid_fps;
    struct SwsContext *sws_v;   /* 解码像素格式 → YUV420P */
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
    /* AMR 编码器用 bit_rate 选 mode；显式给最大值，避免不同 FFmpeg
     * 构建对 0 值处理不一致（解码器忽略该字段）。 */
    { CODEC_AMR_NB,  AV_CODEC_ID_AMR_NB,         12200,  0 },
    { CODEC_AMR_WB,  AV_CODEC_ID_AMR_WB,         23850,  0 },
    { CODEC_G723_1,  AV_CODEC_ID_G723_1,         0,      0 },
    { CODEC_G726_16, AV_CODEC_ID_ADPCM_G726,     16000,  0 },
    { CODEC_G726_24, AV_CODEC_ID_ADPCM_G726,     24000,  0 },
    { CODEC_G726_32, AV_CODEC_ID_ADPCM_G726,     32000,  0 },
    { CODEC_G726_40, AV_CODEC_ID_ADPCM_G726,     40000,  0 },
    { CODEC_OPUS,    AV_CODEC_ID_OPUS,           0,      0 },
    { CODEC_H264,    AV_CODEC_ID_H264,           0,      1 },
    { CODEC_H265,    AV_CODEC_ID_HEVC,           0,      1 },
    { CODEC_VP8,     AV_CODEC_ID_VP8,            0,      1 },
    { CODEC_VP9,     AV_CODEC_ID_VP9,            0,      1 },
    /* JPEG：仅用于 snapshot 抓图输出（无 SDP 载荷，不进协商） */
    { CODEC_MJPEG,   AV_CODEC_ID_MJPEG,          0,      1 },
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
    av_log_bridge();
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
                           (sess->id == CODEC_G722 || sess->id == CODEC_AMR_WB)
                               ? 16000 : 8000;
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

    /* Phase 4: 音频重采样 swr（转码管线必需）
     * 全部用新 API：swr_alloc_set_opts2 + AVChannelLayout + SwrContext opaque
     * configure 里 pkg-config --atleast-version 4.6 libswresample 保证版本 */
    if (is_decoder && !p->is_video) {
        enum AVSampleFormat target_fmt   = AV_SAMPLE_FMT_S16;
        int                target_rate   = (sess->id == CODEC_OPUS) ? 48000 :
                                           (sess->id == CODEC_G722 ||
                                            sess->id == CODEC_AMR_WB) ? 16000 : 8000;
        int                target_ch     = 1;

        if (ctx->sample_fmt != target_fmt ||
            ctx->sample_rate != target_rate ||
            ctx->ch_layout.nb_channels != target_ch) {

            /* FFmpeg 6.x swr_alloc_set_opts2: 返回 int，第一参数是 out 指针 &swr */
            SwrContext *swr = NULL;
            int rc = swr_alloc_set_opts2(
                &swr,
                &(AVChannelLayout)AV_CHANNEL_LAYOUT_MONO,
                target_fmt, target_rate,
                &ctx->ch_layout,
                ctx->sample_fmt, ctx->sample_rate,
                0, NULL);
            if (rc >= 0 && swr) {
                swr_init(swr);
                sess->swr = swr;
                sess->swr_out_rate = target_rate;
                llog(LOG_DEBUG, "av",
                     "swr init: codec sfmt=%d->%d rate=%d->%d ch=%d->%d",
                     ctx->sample_fmt, target_fmt,
                     ctx->sample_rate, target_rate,
                     ctx->ch_layout.nb_channels, target_ch);
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
    if (sess->sws_v) sws_freeContext(sess->sws_v);

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
                /* SwrContext 在 FFmpeg 6.x/7.x 不透明，out_sample_rate 直接读不到
                 * — sess->swr_out_rate 在 av_codec_open 里已缓存 */
                out->rate = sess->swr_out_rate;
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

/* ============================================================================
 * 视频编解码（codec_ops_t 的 vid_* 回调）
 *
 * 解码：初始化即打开 AVCodec（无需宽高）。
 * 编码：首个解码帧到达后才知道宽高，故懒初始化；统一转 YUV420P。
 * H.264 优先 libx264，VP9 优先 libvpx-vp9，缺失时回退原生 encoder。
 * ==========================================================================*/
#ifdef HAVE_FFMPEG
static int av_vid_dec_open(struct codec_sess *sess, codec_id_t cid)
{
    const struct ffmpeg_codec_param *p = find_ff_param(cid);
    if (!p || !p->is_video) return NSBC_NOTSUP;

    const AVCodec *codec = avcodec_find_decoder(p->ffmpeg_id);
    if (!codec) {
        llog(LOG_WARN, "av", "video decoder not found id=%d", p->ffmpeg_id);
        return NSBC_NOTSUP;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return NSBC_NOMEM;
    ctx->pkt_timebase = (AVRational){1, 90000};
    ctx->thread_count = 1;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    int rc = avcodec_open2(ctx, codec, NULL);
    if (rc < 0) {
        avcodec_free_context(&ctx);
        llog(LOG_WARN, "av", "video decoder open failed rc=%d id=%d", rc, cid);
        return NSBC_ERR;
    }
    sess->dec_ctx = ctx;
    sess->pkt   = av_packet_alloc();
    sess->frame = av_frame_alloc();
    if (!sess->pkt || !sess->frame) {
        av_packet_free(&sess->pkt);
        av_frame_free(&sess->frame);
        avcodec_free_context(&sess->dec_ctx);
        return NSBC_NOMEM;
    }
    sess->is_decoder = 1;
    return NSBC_OK;
}

static const AVCodec *av_vid_find_encoder(enum AVCodecID fid)
{
    const AVCodec *codec = NULL;
    if (fid == AV_CODEC_ID_H264)
        codec = avcodec_find_encoder_by_name("libx264");
    else if (fid == AV_CODEC_ID_HEVC)
        codec = avcodec_find_encoder_by_name("libx265");
    else if (fid == AV_CODEC_ID_VP9)
        codec = avcodec_find_encoder_by_name("libvpx-vp9");
    else if (fid == AV_CODEC_ID_VP8)
        codec = avcodec_find_encoder_by_name("libvpx");
    if (!codec) codec = avcodec_find_encoder(fid);
    return codec;
}

static int av_vid_enc_ensure(struct codec_sess *s, int w, int h)
{
    if (s->vid_ready && s->vid_w == w && s->vid_h == h) return NSBC_OK;

    av_log_bridge();
    const struct ffmpeg_codec_param *p = find_ff_param(s->id);
    if (!p || !p->is_video) return NSBC_NOTSUP;

    /* 分辨率变化或首帧：重建编码器。
     * 注意 enc_ctx 可能已由 vid_enc_init 以占位尺寸（176x144）打开，
     * 用 enc_ctx 判断而不是 vid_ready，避免首帧重建时泄漏旧上下文。 */
    if (s->enc_ctx || s->enc_pkt || s->enc_frame) {
        avcodec_free_context(&s->enc_ctx);
        av_packet_free(&s->enc_pkt);
        av_frame_free(&s->enc_frame);
        s->vid_ready = 0;
        if (s->sws_v) { sws_freeContext(s->sws_v); s->sws_v = NULL; }
    }

    const AVCodec *codec = av_vid_find_encoder(p->ffmpeg_id);
    if (!codec) {
        llog(LOG_WARN, "av", "video encoder not found id=%d", p->ffmpeg_id);
        return NSBC_NOTSUP;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return NSBC_NOMEM;

    ctx->width       = w;
    ctx->height      = h;
    /* 像素格式统一 YUV420P */
    ctx->pix_fmt     = AV_PIX_FMT_YUV420P;
    ctx->time_base   = (AVRational){1, 90000};
    if (p->ffmpeg_id == AV_CODEC_ID_MJPEG) {
        /* MJPEG 帧内编码：只保留抓图必需字段（宽高 + 像素格式 + 质量）。
         * 部分 FFmpeg 构建对 bit_rate/gop/framerate/低延迟标志的组合会
         * 返回 EINVAL(-22)，这里与 ffmpeg CLI 的 mjpeg 用法对齐。 */
        ctx->color_range = AVCOL_RANGE_JPEG;
        ctx->qmin = 2;   /* 抓图质量：2..6 足够人脸比对 */
        ctx->qmax = 6;
    } else {
        ctx->framerate   = (AVRational){30, 1};
        ctx->bit_rate    = 800000;
        ctx->gop_size    = 30;
        ctx->max_b_frames = 0;
        ctx->thread_count = 1;
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }

    AVDictionary *opts = NULL;
    if (p->ffmpeg_id == AV_CODEC_ID_H264) {
        av_dict_set(&opts, "preset", "ultrafast", 0);
        av_dict_set(&opts, "tune", "zerolatency", 0);
        /* 与 SDP 宣告的 profile-level-id=42e01f 对齐，避免浏览器按
         * constrained baseline 解码时遇到 High profile 码流 */
        av_dict_set(&opts, "profile", "baseline", 0);
        av_dict_set(&opts, "level", "3.1", 0);
        av_dict_set(&opts, "x264-params",
                    "repeat-headers=1:keyint=30:min-keyint=30:scenecut=0", 0);
    } else if (p->ffmpeg_id == AV_CODEC_ID_HEVC) {
        /* H.265 实时编码：ultrafast + zerolatency，IDR 自带 VPS/SPS/PPS，
         * 便于录制直接落 Annex-B（与 H.264 路径一致）。 */
        av_dict_set(&opts, "preset", "ultrafast", 0);
        av_dict_set(&opts, "x265-params",
                    "tune=zerolatency:repeat-headers=1:keyint=30:min-keyint=30"
                    ":scenecut=0:log-level=error", 0);
    } else if (p->ffmpeg_id == AV_CODEC_ID_VP9) {
        av_dict_set(&opts, "deadline", "realtime", 0);
        av_dict_set(&opts, "cpu-used", "8", 0);
        av_dict_set(&opts, "lag-in-frames", "0", 0);
        av_dict_set(&opts, "error-resilient", "1", 0);
    } else if (p->ffmpeg_id == AV_CODEC_ID_VP8) {
        av_dict_set(&opts, "deadline", "realtime", 0);
        av_dict_set(&opts, "cpu-used", "8", 0);
        av_dict_set(&opts, "lag-in-frames", "0", 0);
    }

    /* 打开失败时 ctx 会被释放，提前保存关键参数供日志定位 */
    int vfmt = (int)ctx->pix_fmt;
    int rc = avcodec_open2(ctx, codec, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        avcodec_free_context(&ctx);
        llog(LOG_WARN, "av", "video encoder open failed rc=%d id=%d %dx%d fmt=%d",
             rc, s->id, w, h, vfmt);
        return NSBC_ERR;
    }

    s->enc_ctx   = ctx;
    s->enc_pkt   = av_packet_alloc();
    s->enc_frame = av_frame_alloc();
    if (!s->enc_pkt || !s->enc_frame) {
        av_packet_free(&s->enc_pkt);
        av_frame_free(&s->enc_frame);
        avcodec_free_context(&s->enc_ctx);
        return NSBC_NOMEM;
    }
    s->vid_w = w;
    s->vid_h = h;
    s->vid_ready = 1;
    s->is_decoder = 0;
    return NSBC_OK;
}
#endif /* HAVE_FFMPEG */

#ifdef HAVE_FFMPEG
static int av_vid_dec_init(codec_id_t cid, const char *fmtp, codec_sess_t **out)
{
    (void)fmtp;
    if (!out) return NSBC_INVAL;
    av_log_bridge();
    struct codec_sess *s = (struct codec_sess *)calloc(1, sizeof(*s));
    if (!s) return NSBC_NOMEM;
    s->id = cid;
    int rc = av_vid_dec_open(s, cid);
    if (rc != NSBC_OK) { free(s); return rc; }
    *out = s;
    return NSBC_OK;
}

static int av_vid_enc_init(codec_id_t cid, const char *fmtp, codec_sess_t **out)
{
    (void)fmtp;
    if (!out) return NSBC_INVAL;
    const struct ffmpeg_codec_param *p = find_ff_param(cid);
    if (!p || !p->is_video) return NSBC_NOTSUP;
    if (!av_vid_find_encoder(p->ffmpeg_id)) return NSBC_NOTSUP;

    struct codec_sess *s = (struct codec_sess *)calloc(1, sizeof(*s));
    if (!s) return NSBC_NOMEM;
    s->id = cid;
    s->is_decoder = 0;
    *out = s;
    return NSBC_OK;
}

static void av_vid_frame_free(codec_sess_t *s, video_frame_t *f)
{
    if (!s || !f) return;
#ifdef HAVE_FFMPEG
    struct codec_sess *cs = (struct codec_sess *)s;
    if (cs->frame) av_frame_unref(cs->frame);
#endif
    memset(f, 0, sizeof(*f));
}

static int av_vid_decode(codec_sess_t *s, const uint8_t *au, size_t len,
                         video_frame_t *out)
{
    if (!s || !au || len == 0 || !out) return NSBC_INVAL;
#ifdef HAVE_FFMPEG
    struct codec_sess *cs = (struct codec_sess *)s;
    if (!cs->dec_ctx || !cs->pkt || !cs->frame) return NSBC_INVAL;

    AVPacket *pkt = cs->pkt;
    pkt->data = (uint8_t *)au;
    pkt->size = (int)len;

    int rc = avcodec_send_packet(cs->dec_ctx, pkt);
    if (rc == AVERROR(EAGAIN)) {
        /* 解码器输出队列未取空导致输入被拒：先取一帧腾出空间，再重送
         * 当前 AU，避免整帧输入丢失（VP8/VP9 单包多帧、B 帧会触发）。 */
        (void)avcodec_receive_frame(cs->dec_ctx, cs->frame);
        rc = avcodec_send_packet(cs->dec_ctx, pkt);
    }
    pkt->data = NULL;
    pkt->size = 0;
    if (rc < 0 && rc != AVERROR(EAGAIN)) return NSBC_ERR;

    rc = avcodec_receive_frame(cs->dec_ctx, cs->frame);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return NSBC_AGAIN;
    if (rc < 0) return NSBC_ERR;

    AVFrame *f = cs->frame;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 4; ++i) {
        out->data[i]     = f->data[i];
        out->linesize[i] = f->linesize[i];
    }
    out->width  = f->width;
    out->height = f->height;
    out->format = (int)f->format;
    out->pts    = (uint32_t)f->pts;
#if LIBAVUTIL_VERSION_MAJOR >= 58
    out->keyframe = (f->flags & AV_FRAME_FLAG_KEY) != 0;
#else
    out->keyframe = f->key_frame != 0;
#endif
    if (f->pict_type == AV_PICTURE_TYPE_I) out->keyframe = 1;
    out->priv = f;
    return NSBC_OK;
#else
    (void)s;
    return NSBC_NOTSUP;
#endif
}

static int av_vid_encode(codec_sess_t *s, const video_frame_t *in,
                         uint8_t *out, size_t cap,
                         size_t *out_len, int *keyframe)
{
    if (!s || !in || !out || !out_len) return NSBC_INVAL;
    if (out_len) *out_len = 0;
    if (keyframe) *keyframe = 0;
#ifdef HAVE_FFMPEG
    struct codec_sess *cs = (struct codec_sess *)s;
    int w = in->width & ~1;
    int h = in->height & ~1;
    if (w <= 0 || h <= 0) return NSBC_INVAL;

    int rc = av_vid_enc_ensure(cs, w, h);
    if (rc != NSBC_OK) return rc;

    AVFrame *f = cs->enc_frame;
    enum AVPixelFormat dst_fmt = cs->enc_ctx->pix_fmt;
    if (dst_fmt == AV_PIX_FMT_NONE) dst_fmt = AV_PIX_FMT_YUV420P;
    av_frame_unref(f);
    f->format = dst_fmt;
    f->width  = w;
    f->height = h;
    f->pts    = (int64_t)in->pts;
    if (in->keyframe) f->pict_type = AV_PICTURE_TYPE_I;
    rc = av_frame_get_buffer(f, 0);
    if (rc < 0) return NSBC_ERR;

    /* 解码帧像素格式 → 编码器像素格式（统一 YUV420P） */
    cs->sws_v = sws_getCachedContext(cs->sws_v, in->width, in->height,
                                     (enum AVPixelFormat)in->format,
                                     w, h, dst_fmt,
                                     SWS_BILINEAR, NULL, NULL, NULL);
    if (!cs->sws_v) { av_frame_unref(f); return NSBC_ERR; }
    sws_scale(cs->sws_v, (const uint8_t *const *)in->data,
              in->linesize, 0, in->height,
              f->data, f->linesize);

    rc = avcodec_send_frame(cs->enc_ctx, f);
    av_frame_unref(f);
    if (rc == AVERROR(EAGAIN)) return NSBC_AGAIN;
    if (rc < 0) return NSBC_ERR;

    AVPacket *pkt = cs->enc_pkt;
    rc = avcodec_receive_packet(cs->enc_ctx, pkt);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return NSBC_AGAIN;
    if (rc < 0) return NSBC_ERR;
    if (pkt->size <= 0) { av_packet_unref(pkt); return NSBC_AGAIN; }
    if ((size_t)pkt->size > cap) { av_packet_unref(pkt); return NSBC_NOSPC; }

    memcpy(out, pkt->data, (size_t)pkt->size);
    *out_len = (size_t)pkt->size;
    if (keyframe) *keyframe = (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
    av_packet_unref(pkt);
    return NSBC_OK;
#else
    (void)s; (void)in; (void)out; (void)cap; (void)keyframe;
    return NSBC_NOTSUP;
#endif
}
#endif /* HAVE_FFMPEG */

#define AVCODEC_DECLARE_OPS(cid, cname, cr, ch, pt, ptm, sdpn, aliases_,    \
                            knd, pld, prio, capf)                             \
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
        /* Registry v2 元数据 */                                              \
        ops->api_version   = NSBC_CODEC_API_VERSION;                          \
        ops->sdp_name      = sdpn;                                            \
        ops->aliases       = aliases_;                                        \
        ops->kind          = knd;                                             \
        ops->payload       = pld;                                             \
        ops->priority      = prio;                                            \
        ops->caps          = capf;                                            \
    }

/* 每个 codec 的工厂函数（ops 填充 + 真实/stub 函数对） */
AVCODEC_DECLARE_OPS(CODEC_G722,     avcodec_g722,     16000, 1,  9,  20,
                    "G722", "G.722,G722", CODEC_KIND_AUDIO, RTP_PAYLOAD_SIMPLE,
                    60, CODEC_CAP_DEC | CODEC_CAP_ENC)
AVCODEC_DECLARE_OPS(CODEC_G723_1,   avcodec_g7231,    8000,  1, -1,  30,
                    "G723", "G723.1,G.723.1", CODEC_KIND_AUDIO, RTP_PAYLOAD_SIMPLE,
                    40, CODEC_CAP_DEC | CODEC_CAP_ENC)
AVCODEC_DECLARE_OPS(CODEC_G726_16,  avcodec_g726_16,  8000,  1, -1,  20,
                    "G726-16", "G726_16,G.726-16", CODEC_KIND_AUDIO, RTP_PAYLOAD_SIMPLE,
                    30, CODEC_CAP_DEC | CODEC_CAP_ENC)
AVCODEC_DECLARE_OPS(CODEC_G726_24,  avcodec_g726_24,  8000,  1, -1,  20,
                    "G726-24", "G726_24,G.726-24", CODEC_KIND_AUDIO, RTP_PAYLOAD_SIMPLE,
                    30, CODEC_CAP_DEC | CODEC_CAP_ENC)
AVCODEC_DECLARE_OPS(CODEC_G726_32,  avcodec_g726_32,  8000,  1, -1,  20,
                    "G726-32", "G726_32,G.726-32", CODEC_KIND_AUDIO, RTP_PAYLOAD_SIMPLE,
                    30, CODEC_CAP_DEC | CODEC_CAP_ENC)
AVCODEC_DECLARE_OPS(CODEC_G726_40,  avcodec_g726_40,  8000,  1, -1,  20,
                    "G726-40", "G726_40,G.726-40", CODEC_KIND_AUDIO, RTP_PAYLOAD_SIMPLE,
                    30, CODEC_CAP_DEC | CODEC_CAP_ENC)
AVCODEC_DECLARE_OPS(CODEC_OPUS,     avcodec_opus,     48000, 1, -1,  20,
                    "opus", "OPUS", CODEC_KIND_AUDIO, RTP_PAYLOAD_SIMPLE,
                    80, CODEC_CAP_DEC | CODEC_CAP_ENC | CODEC_CAP_STATEFUL)
AVCODEC_DECLARE_OPS(CODEC_H264,     avcodec_h264,     90000, 0, 96,   0,
                    "H264", "H.264,AVC", CODEC_KIND_VIDEO, RTP_PAYLOAD_H264,
                    90, CODEC_CAP_DEC | CODEC_CAP_ENC | CODEC_CAP_VIDEO)
AVCODEC_DECLARE_OPS(CODEC_VP8,      avcodec_vp8,      90000, 0, -1,   0,
                    "VP8", "VP8", CODEC_KIND_VIDEO, RTP_PAYLOAD_VP8,
                    70, CODEC_CAP_DEC | CODEC_CAP_ENC | CODEC_CAP_VIDEO)
AVCODEC_DECLARE_OPS(CODEC_VP9,      avcodec_vp9,      90000, 0, -1,   0,
                    "VP9", "VP9", CODEC_KIND_VIDEO, RTP_PAYLOAD_VP9,
                    70, CODEC_CAP_DEC | CODEC_CAP_ENC | CODEC_CAP_VIDEO)
/* H.265/HEVC：priority 85 —— 低于 H.264（兼容性优先），高于 VP8/VP9。
 * default_pt=-1：动态 PT，由 SDP 协商。 */
AVCODEC_DECLARE_OPS(CODEC_H265,     avcodec_h265,     90000, 0, -1,   0,
                    "H265", "HEVC,H.265,H265", CODEC_KIND_VIDEO, RTP_PAYLOAD_H265,
                    85, CODEC_CAP_DEC | CODEC_CAP_ENC | CODEC_CAP_VIDEO)
/* JPEG：snapshot 抓图输出专用，不做 SDP 协商（default_pt=-1）。
 * 只使用 vid_encode；vid_decode 存在但无业务调用。 */
AVCODEC_DECLARE_OPS(CODEC_MJPEG,    avcodec_mjpeg,    90000, 0, -1,   0,
                    "MJPEG", "JPEG,MJPG", CODEC_KIND_VIDEO, RTP_PAYLOAD_SIMPLE,
                    10, CODEC_CAP_ENC | CODEC_CAP_VIDEO)

/* ============================================================================
 * AMR-NB / AMR-WB（RFC 4867 octet-aligned）
 *
 * RTP 载荷布局： CMR(1) + ToC(1..N) + 各帧 speech 数据
 *   ToC: F(1) FT(4) Q(1) P(2)；F=1 表示后面还有一个 ToC 字节。
 * FFmpeg 的 AMR 解码器吃「去掉 ToC 的单帧」，编码器输出「不带 ToC 的单帧」，
 * 因此本文件负责 ToC 拆装 + CMR 填充，使 mod_av 可直接参与 AMR 转码。
 * ==========================================================================*/
/* 每帧总字节数（含 1 字节 ToC）；FT=15 只有 ToC，无数据 */
static const int k_amr_nb_frame_bytes[16] = {
    13, 14, 16, 18, 20, 21, 27, 32, 6, 0, 0, 0, 0, 0, 0, 1
};
static const int k_amr_wb_frame_bytes[16] = {
    18, 24, 33, 37, 41, 47, 51, 59, 61, 0, 0, 0, 0, 0, 0, 1
};
/* speech 数据字节数（不含 ToC），用于编码后反推 FT */
static const int k_amr_nb_speech[9] = { 12, 13, 15, 17, 19, 20, 26, 31, 5 };
static const int k_amr_wb_speech[9] = { 17, 23, 32, 36, 40, 46, 50, 58, 60 };

static int amr_sess_open(codec_id_t id, int is_dec, const char *fmtp,
                         codec_sess_t **out)
{
    codec_sess_t *s;
    int rc;
    (void)fmtp;   /* FFmpeg AMR 解码器无需 fmtp；octet-align 由本层保证 */
    if (out == NULL) return NSBC_INVAL;
    s = (codec_sess_t *)calloc(1, sizeof(*s));
    if (s == NULL) return NSBC_NOMEM;
    s->id = id;
    rc = av_codec_open(s, is_dec);
    if (rc != NSBC_OK) { free(s); return rc; }
    *out = s;
    return NSBC_OK;
}

static int amr_unpack_common(const uint8_t *p, size_t len, int wb,
                             codec_frame_t *frames, int max_frames, int *n)
{
    const int *tab = wb ? k_amr_wb_frame_bytes : k_amr_nb_frame_bytes;
    uint8_t toc[16];
    int nf = 0, i;

    if (p == NULL || frames == NULL || n == NULL) return NSBC_INVAL;
    *n = 0;
    if (len < 2) return NSBC_OK;      /* 只有 CMR 或空载荷 */

    i = 1;                            /* 跳过 CMR */
    while (i < (int)len && nf < 16) {
        toc[nf++] = p[i++];
        if (!(toc[nf - 1] & 0x80)) break;   /* F=0：ToC 列表结束 */
    }
    for (int k = 0; k < nf; ++k) {
        int ft = (toc[k] >> 3) & 0x0F;
        int sz = tab[ft];
        int speech = sz > 0 ? sz - 1 : 0;
        if (speech <= 0) continue;          /* FT=15 / 无效 mode */
        if (i + speech > (int)len) {
            llog(LOG_DEBUG, "av", "AMR frame %d truncated (need %d, left %d)",
                 k, speech, (int)len - i);
            break;
        }
        if (*n >= max_frames) break;
        frames[*n].data = p + i;
        frames[*n].len = (size_t)speech;
        frames[*n].samples = wb ? 320 : 160;
        frames[*n].speech = 1;
        (*n)++;
        i += speech;
    }
    return NSBC_OK;
}

static int amr_unpack_nb(codec_sess_t *s, const uint8_t *p, size_t len,
                         codec_frame_t *frames, int max_frames, int *n)
{
    (void)s;
    return amr_unpack_common(p, len, 0, frames, max_frames, n);
}

static int amr_unpack_wb(codec_sess_t *s, const uint8_t *p, size_t len,
                         codec_frame_t *frames, int max_frames, int *n)
{
    (void)s;
    return amr_unpack_common(p, len, 1, frames, max_frames, n);
}

/* 编码：FFmpeg 输出单帧 → 前面补 CMR + ToC */
static int amr_encode_common(codec_sess_t *s, const media_frame_t *in,
                             uint8_t *out, size_t cap, size_t *ol, int wb)
{
    uint8_t tmp[128];
    size_t tlen = 0;
    const int *sp = wb ? k_amr_wb_speech : k_amr_nb_speech;
    int rc, ft = -1;

    if (s == NULL || in == NULL || out == NULL || ol == NULL)
        return NSBC_INVAL;
    *ol = 0;
    rc = av_codec_encode(s, in, tmp, sizeof(tmp), &tlen);
    if (rc != NSBC_OK || tlen == 0) return rc;

    for (int i = 0; i < 9; ++i) {
        if ((size_t)sp[i] == tlen) { ft = i; break; }
    }
    if (ft < 0) {
        /* 非标准长度：取最接近的 mode，保证对端可解析 */
        size_t diff = (size_t)-1;
        for (int i = 0; i < 9; ++i) {
            int d = (int)tlen - sp[i];
            size_t ad = (size_t)(d < 0 ? -d : d);
            if (ad < diff) { diff = ad; ft = i; }
        }
        if (tlen > (size_t)sp[ft]) tlen = (size_t)sp[ft];
    }
    if (cap < tlen + 2) return NSBC_NOSPC;
    out[0] = 0xF0;                                  /* CMR: no mode request */
    out[1] = (uint8_t)(((ft & 0x0F) << 3) | 0x04);  /* F=0 FT=mode Q=1 P=0 */
    memcpy(out + 2, tmp, tlen);
    *ol = tlen + 2;
    return NSBC_OK;
}

static int amr_nb_encode(codec_sess_t *s, const media_frame_t *in,
                         uint8_t *out, size_t cap, size_t *ol)
{
    return amr_encode_common(s, in, out, cap, ol, 0);
}

static int amr_wb_encode(codec_sess_t *s, const media_frame_t *in,
                         uint8_t *out, size_t cap, size_t *ol)
{
    return amr_encode_common(s, in, out, cap, ol, 1);
}

/* fmtp 协商：始终 octet-align=1，保留远端 mode-set */
static int amr_fmtp_negotiate(const char *local, const char *remote,
                              char *out, size_t cap)
{
    char modeset[64] = "";
    (void)local;
    if (out == NULL || cap == 0) return NSBC_INVAL;
    if (remote != NULL) {
        const char *m = strstr(remote, "mode-set=");
        if (m != NULL) {
            size_t n = 0;
            m += 9;
            while (m[n] != '\0' && m[n] != ';' && m[n] != ' ' &&
                   n + 1 < sizeof(modeset)) {
                modeset[n] = m[n];
                ++n;
            }
            modeset[n] = '\0';
        }
        if (strstr(remote, "octet-align=0") != NULL)
            llog(LOG_WARN, "av", "AMR remote wants octet-align=0, forcing 1");
    }
    if (modeset[0])
        snprintf(out, cap, "octet-align=1;mode-set=%s", modeset);
    else
        snprintf(out, cap, "octet-align=1");
    return NSBC_OK;
}

static int amr_nb_dec_init(const char *f, codec_sess_t **o)
{
    return amr_sess_open(CODEC_AMR_NB, 1, f, o);
}
static int amr_nb_enc_init(const char *f, codec_sess_t **o)
{
    return amr_sess_open(CODEC_AMR_NB, 0, f, o);
}
static int amr_wb_dec_init(const char *f, codec_sess_t **o)
{
    return amr_sess_open(CODEC_AMR_WB, 1, f, o);
}
static int amr_wb_enc_init(const char *f, codec_sess_t **o)
{
    return amr_sess_open(CODEC_AMR_WB, 0, f, o);
}
static void amr_fini(codec_sess_t *s) { av_codec_close(s); }
static int amr_nb_decode(codec_sess_t *s, const uint8_t *in, size_t il,
                         media_frame_t *out)
{
    return av_codec_decode(s, in, il, out);
}
static int amr_wb_decode(codec_sess_t *s, const uint8_t *in, size_t il,
                         media_frame_t *out)
{
    return av_codec_decode(s, in, il, out);
}

static void amr_fill_common(codec_ops_t *ops, codec_id_t id, const char *name,
                            const char *sdpn, const char *aliases,
                            int clock, uint16_t prio)
{
    memset(ops, 0, sizeof(*ops));
    ops->id = id;
    snprintf(ops->name, sizeof(ops->name), "%s", name);
    ops->clock_rate = clock;
    ops->channels = 1;
    ops->default_pt = -1;      /* AMR 一律动态 PT */
    ops->ptime_ms = 20;
    ops->api_version = NSBC_CODEC_API_VERSION;
    ops->sdp_name = sdpn;
    ops->aliases = aliases;
    ops->kind = CODEC_KIND_AUDIO;
    ops->payload = RTP_PAYLOAD_AMR;
    ops->priority = prio;
    ops->caps = CODEC_CAP_DEC | CODEC_CAP_ENC | CODEC_CAP_STATEFUL;
    ops->default_fmtp = "octet-align=1";
    ops->fmtp_negotiate = amr_fmtp_negotiate;
    ops->rtp_clock_rate = clock;
    ops->pcm_sample_rate = clock;
    ops->frame_samples = clock / 50;     /* 20ms */
    ops->fini = amr_fini;
}

static void avcodec_amr_nb_fill_ops(codec_ops_t *ops)
{
    amr_fill_common(ops, CODEC_AMR_NB, "amr_nb", "AMR",
                    "AMR_NB,AMR-NB,AMR", 8000, 55);
    ops->init = amr_nb_dec_init;
    ops->encoder_init = amr_nb_enc_init;
    ops->decode = amr_nb_decode;
    ops->encode = amr_nb_encode;
    ops->rtp_unpack = amr_unpack_nb;
}

static void avcodec_amr_wb_fill_ops(codec_ops_t *ops)
{
    amr_fill_common(ops, CODEC_AMR_WB, "amr_wb", "AMR-WB",
                    "AMR_WB,AMRWB,AMR-WB", 16000, 75);
    ops->init = amr_wb_dec_init;
    ops->encoder_init = amr_wb_enc_init;
    ops->decode = amr_wb_decode;
    ops->encode = amr_wb_encode;
    ops->rtp_unpack = amr_unpack_wb;
}

/* 双时钟率收尾：G.722 RTP=8000/PCM=16000；AMR 由 fill 时已填写 */
static void av_fixup_dual_clock(codec_ops_t *slots, int n)
{
    for (int i = 0; i < n; ++i) {
        if (slots[i].id == CODEC_G722) {
            slots[i].rtp_clock_rate = 8000;    /* RFC 3551 */
            slots[i].pcm_sample_rate = 16000;
            slots[i].frame_samples = 320;      /* 20ms @ 16k PCM */
        }
    }
}

/* 视频回调按 codec 绑定（不能放进宏：宏体内无法展开 #ifdef）。
 * 无 FFmpeg 构建不绑定，codec_can_decode/encode 对视频返回 0，
 * 呼叫保持原透传路径。 */
#ifdef HAVE_FFMPEG
#define VIDEO_ATTACH(cid, cname)                                             \
    static int cname##_vid_dec_init(const char *f, codec_sess_t **o)         \
        { return av_vid_dec_init(cid, f, o); }                               \
    static int cname##_vid_enc_init(const char *f, codec_sess_t **o)         \
        { return av_vid_enc_init(cid, f, o); }                               \
    static int cname##_vid_decode(codec_sess_t *s, const uint8_t *au,        \
                                  size_t al, video_frame_t *o)               \
        { return av_vid_decode(s, au, al, o); }                              \
    static int cname##_vid_encode(codec_sess_t *s, const video_frame_t *in,  \
                                  uint8_t *o, size_t cap, size_t *ol, int *kf)\
        { return av_vid_encode(s, in, o, cap, ol, kf); }                     \
    static void cname##_vid_frame_free(codec_sess_t *s, video_frame_t *f)    \
        { av_vid_frame_free(s, f); }                                         \
    static void cname##_attach_vid(codec_ops_t *ops) {                       \
        ops->vid_dec_init   = cname##_vid_dec_init;                          \
        ops->vid_enc_init   = cname##_vid_enc_init;                          \
        ops->vid_decode     = cname##_vid_decode;                            \
        ops->vid_encode     = cname##_vid_encode;                            \
        ops->vid_frame_free = cname##_vid_frame_free;                        \
    }
VIDEO_ATTACH(CODEC_H264, avcodec_h264)
VIDEO_ATTACH(CODEC_VP8,  avcodec_vp8)
VIDEO_ATTACH(CODEC_VP9,  avcodec_vp9)
VIDEO_ATTACH(CODEC_H265, avcodec_h265)
VIDEO_ATTACH(CODEC_MJPEG, avcodec_mjpeg)
#endif /* HAVE_FFMPEG */

#ifdef HAVE_FFMPEG
/* ============================================================================
 * H.265 SDP 参数集（sprop-vps / sprop-sps / sprop-pps）
 *
 * RFC 7798 允许在 fmtp 中带外宣告参数集。SBC 自己编码 H.265（H.264→H.265
 * 方向）时，对端在收到首个 IDR 前需要这些参数集初始化解码器；部分终端即使
 * 支持带内参数集也会优先读 fmtp。
 *
 * 参数集与编码器配置强相关，而 SDP 生成时编码器尚未启动，因此这里在 codec
 * 注册阶段用与转码一致的参数编一帧，从码流中提取 VPS/SPS/PPS 并 base64 缓存。
 * 只执行一次；libx265 不可用时返回 NULL，SDP 退化为不带 sprop（仅带内）。
 * ==========================================================================*/
static char g_hevc_sprop[768];

/* 从 Annex-B 码流取下一个 NAL（支持 3/4 字节起始码） */
static int hevc_annexb_next(const uint8_t *p, size_t len, size_t *off,
                            const uint8_t **nal, size_t *nal_len)
{
    size_t i = *off;
    while (i + 3 <= len) {
        size_t sc = 0;
        if (p[i] == 0 && p[i + 1] == 0) {
            if (p[i + 2] == 1) sc = 3;
            else if (i + 4 <= len && p[i + 2] == 0 && p[i + 3] == 1) sc = 4;
        }
        if (sc == 0) { ++i; continue; }

        size_t ns = i + sc;
        size_t j  = ns;
        while (j + 3 <= len) {
            if (p[j] == 0 && p[j + 1] == 0 &&
                (p[j + 2] == 1 ||
                 (j + 4 <= len && p[j + 2] == 0 && p[j + 3] == 1)))
                break;
            ++j;
        }
        if (j + 3 > len) j = len;

        *nal     = p + ns;
        *nal_len = j - ns;
        *off     = j;
        return *nal_len > 0;
    }
    return 0;
}

static void hevc_sprop_build_once(void)
{
    /* codec 模块 reload 会重新走一遍注册，这里保证真正只编一次 */
    static int done;
    if (done) return;
    done = 1;

    const AVCodec *codec = avcodec_find_encoder_by_name("libx265");
    if (!codec) {
        llog(LOG_INFO, "av", "HEVC sprop: libx265 not available");
        return;
    }

    AVCodecContext *ctx  = avcodec_alloc_context3(codec);
    AVDictionary   *opts = NULL;
    AVFrame        *frm  = NULL;
    AVPacket       *pkt  = NULL;
    uint8_t vps[256], sps[512], pps[256];
    size_t vps_len = 0, sps_len = 0, pps_len = 0;
    if (!ctx) return;

    ctx->width       = 320;
    ctx->height      = 240;
    ctx->pix_fmt     = AV_PIX_FMT_YUV420P;
    ctx->time_base   = (AVRational){1, 30};
    ctx->framerate   = (AVRational){30, 1};
    ctx->bit_rate    = 200000;
    ctx->gop_size    = 30;
    ctx->max_b_frames = 0;

    /* 与 av_vid_enc_ensure 的 H.265 参数保持一致，保证 sprop 对得上实际码流 */
    av_dict_set(&opts, "preset", "ultrafast", 0);
    av_dict_set(&opts, "x265-params",
                "tune=zerolatency:repeat-headers=1:keyint=30:min-keyint=30"
                ":scenecut=0:log-level=error", 0);

    if (avcodec_open2(ctx, codec, &opts) < 0) {
        av_dict_free(&opts);
        avcodec_free_context(&ctx);
        llog(LOG_WARN, "av", "HEVC sprop: encoder open failed");
        return;
    }
    av_dict_free(&opts);

    frm = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frm || !pkt) goto out;

    frm->format = ctx->pix_fmt;
    frm->width  = ctx->width;
    frm->height = ctx->height;
    frm->pts    = 0;
    if (av_frame_get_buffer(frm, 0) < 0) goto out;

    /* 中性画面：有限范围黑（Y=16，Cb/Cr=128），一帧足够产出参数集 */
    for (int y = 0; y < ctx->height; ++y)
        memset(frm->data[0] + (size_t)y * frm->linesize[0], 16,
               (size_t)ctx->width);
    for (int p = 1; p < 3; ++p) {
        int hh = ctx->height / 2;
        for (int y = 0; y < hh; ++y)
            memset(frm->data[p] + (size_t)y * frm->linesize[p], 128,
                   (size_t)(ctx->width / 2));
    }

    if (avcodec_send_frame(ctx, frm) < 0) goto out;
    if (avcodec_receive_packet(ctx, pkt) < 0) {
        /* 部分 x265 构建会缓存首帧：flush 后再取 */
        if (avcodec_send_frame(ctx, NULL) < 0) goto out;
        if (avcodec_receive_packet(ctx, pkt) < 0) goto out;
    }

    size_t off = 0;
    const uint8_t *nal = NULL;
    size_t nl = 0;
    while (hevc_annexb_next(pkt->data, (size_t)pkt->size, &off, &nal, &nl)) {
        if (nl < 2) continue;
        int t = (nal[0] >> 1) & 0x3F;
        if (t == 32 && nl <= sizeof(vps)) { memcpy(vps, nal, nl); vps_len = nl; }
        else if (t == 33 && nl <= sizeof(sps)) { memcpy(sps, nal, nl); sps_len = nl; }
        else if (t == 34 && nl <= sizeof(pps)) { memcpy(pps, nal, nl); pps_len = nl; }
    }

out:
    if (vps_len && sps_len && pps_len) {
        /* 兼容各版本 av_base64_encode 签名（老版本返回 char*，新版本返回 int），
         * 不使用返回值，改为校验输出缓冲是否被写满。 */
        char b64v[512] = {0}, b64s[1024] = {0}, b64p[512] = {0};
        (void)av_base64_encode(b64v, sizeof(b64v), vps, (int)vps_len);
        (void)av_base64_encode(b64s, sizeof(b64s), sps, (int)sps_len);
        (void)av_base64_encode(b64p, sizeof(b64p), pps, (int)pps_len);
        if (b64v[0] && b64s[0] && b64p[0]) {
            int sn = snprintf(g_hevc_sprop, sizeof(g_hevc_sprop),
                              "sprop-max-don-diff=0;sprop-vps=%s;sprop-sps=%s;sprop-pps=%s",
                              b64v, b64s, b64p);
            if (sn <= 0 || (size_t)sn >= sizeof(g_hevc_sprop)) {
                /* 截断的 base64 会让对端解出坏参数集，宁可不宣告 */
                g_hevc_sprop[0] = '\0';
                llog(LOG_WARN, "av", "HEVC sprop too long, fall back to no sprop");
            } else {
                llog(LOG_INFO, "av",
                     "HEVC sprop ready: vps=%zuB sps=%zuB pps=%zuB",
                     vps_len, sps_len, pps_len);
            }
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&frm);
    avcodec_free_context(&ctx);
}
#endif /* HAVE_FFMPEG */

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
    static codec_ops_t g_ops_slots[20];   /* 12 个 AV codec（含 AMR NB/WB）+ MJPEG + 余量 */
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
    REG_CODEC(avcodec_amr_nb_fill_ops);
    REG_CODEC(avcodec_amr_wb_fill_ops);
    REG_CODEC(avcodec_g7231_fill_ops);
    REG_CODEC(avcodec_g726_16_fill_ops);
    REG_CODEC(avcodec_g726_24_fill_ops);
    REG_CODEC(avcodec_g726_32_fill_ops);
    REG_CODEC(avcodec_g726_40_fill_ops);
    REG_CODEC(avcodec_opus_fill_ops);
    REG_CODEC(avcodec_h264_fill_ops);
    REG_CODEC(avcodec_vp8_fill_ops);
    REG_CODEC(avcodec_vp9_fill_ops);
    REG_CODEC(avcodec_h265_fill_ops);
    REG_CODEC(avcodec_mjpeg_fill_ops);

#ifdef HAVE_FFMPEG
    /* 视频回调在注册后绑定：registry 只保存 ops 指针，直接补字段即可 */
    if (slot >= 5) {
        avcodec_h264_attach_vid(&g_ops_slots[slot - 5]);
        avcodec_vp8_attach_vid(&g_ops_slots[slot - 4]);
        avcodec_vp9_attach_vid(&g_ops_slots[slot - 3]);
        avcodec_h265_attach_vid(&g_ops_slots[slot - 2]);
        avcodec_mjpeg_attach_vid(&g_ops_slots[slot - 1]);
    }
#endif

    /* H.264/H.265 默认 fmtp 归入 codec 元数据：SDP 重写 / answer 生成统一从
     * ops->default_fmtp 读取，核心不再硬编码 packetization-mode。
     * H.265 的 sprop-vps/sps/pps 由 libx265 编一帧生成（见 hevc_sprop_build_once），
     * 失败时只声明 sprop-max-don-diff=0（与本端 AP 解包假设一致）。 */
    for (int i = 0; i < slot; ++i) {
        if (g_ops_slots[i].id == CODEC_H264 && g_ops_slots[i].default_fmtp == NULL)
            g_ops_slots[i].default_fmtp =
                "packetization-mode=1;profile-level-id=42e01f";
        if (g_ops_slots[i].id == CODEC_H265 && g_ops_slots[i].default_fmtp == NULL) {
#ifdef HAVE_FFMPEG
            hevc_sprop_build_once();
            g_ops_slots[i].default_fmtp = g_hevc_sprop[0]
                ? g_hevc_sprop : "sprop-max-don-diff=0";
#else
            g_ops_slots[i].default_fmtp = "sprop-max-don-diff=0";
#endif
        }
    }

    /* G.722 双时钟率：RTP 8000 / PCM 16000（SDP 写 8000）*/
    av_fixup_dual_clock(g_ops_slots, slot);

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
