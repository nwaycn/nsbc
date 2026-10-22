/* Phase 3 mod_av: FFmpeg 商业 codec 统一注册器
 *
 * 分工（白皮书 §4.8）：
 *   mod_codecs — 内置轻量 codec，无 heavy 依赖（G.711/G.729 Annex B）
 *   mod_av     — 所有商业音视频 codec，依赖 FFmpeg
 *
 * 两者都注册到同一个 codec_registry，互不干扰。bridge_confirm 按 codec_id
 * 查 registry → 自动路由到对应的 transcode_open，bridge 层完全不关心
 * codec 实现来源。
 *
 * configure 自动检测 FFmpeg 存在性（本文件注释，不改 configure）：
 *   if pkg-config --exists libavcodec libavutil libswresample libswscale; then
 *     AV_CPPFLAGS += $(pkg-config --cflags libavcodec libavutil libswresample libswscale)
 *     AV_LDLIBS   += $(pkg-config --libs   libavcodec libavutil libswresample libswscale)
 *     CFLAGS      += -DHAVE_FFMPEG
 *   fi
 *
 * 无 HAVE_FFMPEG 时：所有 ops 仍注册，但 init/decode/encode 为 NULL
 * → transcode_open 检测到 ops->init==NULL 返回 NSBC_NOTSUP
 * → bridge 层原样透传 RTP 包，不触发转码。
 */
#include "module.h"
#include "media_codec.h"
#include "log.h"

/* av_codec.c 里提供：把 10 个 codec_ops_t 注册进 registry */
int av_register_all_codecs(void);

static int av_init(struct nsbc_ctx *ctx)
{
    (void)ctx;

    int rc = av_register_all_codecs();

#ifdef HAVE_FFMPEG
    if (rc == NSBC_OK)
        llog(LOG_INFO, "av",
             "HAVE_FFMPEG: 10 codecs registered (G722/G723.1/G726_16/24/32/40/Opus/H264/VP8/VP9)");
    else
        llog(LOG_WARN, "av", "av_register_all_codecs failed rc=%d", rc);
#else
    if (rc == NSBC_OK)
        llog(LOG_INFO, "av",
             "stub mode: 10 codecs registered (no FFmpeg — build with -DHAVE_FFMPEG for real)");
    else
        llog(LOG_WARN, "av", "av_register_all_codecs failed rc=%d (stub path)", rc);
#endif

    return NSBC_OK; /* 模块级 OK；个别 codec 失败由 registry 返回，不阻断模块加载 */
}

static void av_fini(struct nsbc_ctx *ctx)
{
    (void)ctx;

    codec_registry_unregister(CODEC_G722);
    codec_registry_unregister(CODEC_G723_1);
    codec_registry_unregister(CODEC_G726_16);
    codec_registry_unregister(CODEC_G726_24);
    codec_registry_unregister(CODEC_G726_32);
    codec_registry_unregister(CODEC_G726_40);
    codec_registry_unregister(CODEC_OPUS);
    codec_registry_unregister(CODEC_H264);
    codec_registry_unregister(CODEC_VP8);
    codec_registry_unregister(CODEC_VP9);

#ifdef HAVE_FFMPEG
    llog(LOG_INFO, "av", "all 10 FFmpeg-backed codecs unregistered");
#else
    llog(LOG_INFO, "av", "all 10 stub codecs unregistered");
#endif
}

NSBC_MOD0_CAT(av, 1, MOD_L_FINAL, 0, MOD_CAT_CODEC, av_init, av_fini);
