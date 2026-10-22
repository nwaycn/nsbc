/* Codec 描述符注册（G.711/G.729/未来 H.264/G.722/Opus...）。
 *
 * 架构：media_bridge 内核只提供 codec_registry（全局注册表），
 * 不硬编码注册任何 codec。所有 codec 实现都在本模块，
 * init 时自注册到 registry。bridge_confirm 查 registry，
 * 没找到就跳过转码。新增 codec 只需在本模块加实现 + 调 register()。
 *
 * PCM（G.711 a/µ-law）也从这里注册，不再内联在 bridge_mgr_create。 */
#include "module.h"
#include "media_codec.h"
#include "log.h"

/* codec 实现的注册函数（同模块内 .c 文件提供） */
int g711_register(void);
int g729_register(void);

static int codecs_init(struct nsbc_ctx *ctx)
{
    (void)ctx;

    /* G.711 内置编解码（PCMA PT=8 + PCMU PT=0） */
    int rc1 = g711_register();
    if (rc1 == NSBC_OK)
        llog(LOG_INFO, "codec", "G.711 codec registered: PCMA(PT=8) + PCMU(PT=0)");
    else
        llog(LOG_WARN, "codec", "g711_register failed rc=%d", rc1);

    /* G.729 Annex B —— 有 bcg729 时真实编解码，无则 stub 返回 NOTSUP */
    int rc2 = g729_register();
#ifdef HAVE_BCG729
    if (rc2 == NSBC_OK)
        llog(LOG_INFO, "codec", "G.729 codec registered (bcg729 real): G729(PT=18, Annex B)");
    else
        llog(LOG_WARN, "codec", "g729_register failed rc=%d", rc2);
#else
    if (rc2 == NSBC_OK)
        llog(LOG_INFO, "codec", "G.729 codec registered (stub, NSBC_WITH_BCG729=1 to enable)");
    else
        llog(LOG_WARN, "codec", "g729_register failed rc=%d (stub path)", rc2);
#endif

    return NSBC_OK;  /* 模块本身 OK，个别 codec 失败不影响模块加载 */
}

NSBC_MOD0_CAT(codecs, 1, MOD_L_FINAL, 0, MOD_CAT_CODEC, codecs_init, NULL);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
