/* Codec 描述符注册（G.711/G.729/未来 H.264/G.722/Opus...）。
 *
 * 架构：media_bridge 内核只提供 codec_registry（全局注册表），
 * 不硬编码注册任何 codec。所有 codec 实现都在本模块，
 * init 时自注册到 registry。bridge_confirm 查 registry，
 * 没找到就跳过转码。
 *
 * 新增 codec 只需在本模块加一个 .c 实现文件（编译系统用
 * find src/mod/codecs/*.c 自动收集），在文件里写
 *     NSBC_CODEC_REGISTRAR(my_register)
 * 即可，不需要改本文件。 */
#include "module.h"
#include "media_codec.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

/* ELF 段边界（GNU ld 在存在该段引用时自动生成）。
 * 每个 codec 源文件通过 NSBC_CODEC_REGISTRAR 在该段内放一个函数指针。 */
extern const codec_registrar_fn __start_nsbc_codecs[];
extern const codec_registrar_fn __stop_nsbc_codecs[];

/* 旁路载荷（RFC 4733 telephone-event / RFC 3389 CN）：
 * 只有身份与能力，不参与 PCM 转码；用于 PT 识别与转发，
 * 避免协商阶段因 CODEC_NONE 被丢弃。 */
static void event_fill(codec_ops_t *ops, codec_id_t id, const char *name,
                       const char *sdpn, int pt, const char *aliases,
                       const char *def_fmtp)
{
    memset(ops, 0, sizeof(*ops));
    ops->id = id;
    snprintf(ops->name, sizeof(ops->name), "%s", name);
    ops->clock_rate = 8000;
    ops->channels = 1;
    ops->default_pt = pt;
    ops->ptime_ms = 0;
    ops->api_version = NSBC_CODEC_API_VERSION;
    ops->sdp_name = sdpn;
    ops->aliases = aliases;
    ops->kind = CODEC_KIND_AUDIO;
    ops->payload = RTP_PAYLOAD_SIMPLE;
    ops->priority = 10;
    ops->caps = CODEC_CAP_EVENT;
    ops->default_fmtp = def_fmtp;
    ops->rtp_clock_rate = 8000;
    ops->pcm_sample_rate = 8000;
}

static int event_codecs_register(void)
{
    static codec_ops_t te_ops;
    static codec_ops_t cn_ops;
    int rc1, rc2;

    event_fill(&te_ops, CODEC_TELEPHONE_EVENT, "telephone-event",
               "telephone-event", -1, "TELEPHONE-EVENT,DTMF",
               "0-16");
    rc1 = codec_registry_register(&te_ops);

    event_fill(&cn_ops, CODEC_CN, "CN", "CN", 13, "COMFORT-NOISE",
               NULL);
    rc2 = codec_registry_register(&cn_ops);

    return (rc1 == NSBC_OK && rc2 == NSBC_OK) ? NSBC_OK : NSBC_ERR;
}

static int codecs_init(struct nsbc_ctx *ctx)
{
    (void)ctx;

    /* 遍历内置 codec 自注册段：G.711 / G.729 / 未来新增 codec 自动生效 */
    int ok = 0, total = 0;
    for (const codec_registrar_fn *p = &__start_nsbc_codecs[0];
         p < &__stop_nsbc_codecs[0]; ++p) {
        if (*p == NULL) continue;
        total++;
        int rc = (*p)();
        if (rc == NSBC_OK) {
            ok++;
        } else {
            llog(LOG_WARN, "codec", "codec registrar %p failed rc=%d",
                 (const void *)*p, rc);
        }
    }
    llog(LOG_INFO, "codec", "builtin codecs: %d/%d registered", ok, total);

    int rc3 = event_codecs_register();
    if (rc3 == NSBC_OK)
        llog(LOG_INFO, "codec",
             "event codecs registered: telephone-event + CN(PT=13)");
    else
        llog(LOG_WARN, "codec", "event_codecs_register rc=%d", rc3);

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
