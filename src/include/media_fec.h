#ifndef NSBC_MEDIA_FEC_H
#define NSBC_MEDIA_FEC_H

/* 媒体抗丢包：RED（RFC 2198）+ ULP FEC（RFC 5109）+ FlexFEC（RFC 8627）
 * 对应 REQ-MEDIA-FEC（P1/P2，WebRTC 场景主用）
 *
 * 口径（防标准错引，项目记忆）：
 *   RED=RFC 2198（载荷冗余封装，音频）；
 *   ULP FEC=RFC 5109（音频 XOR 恢复，WebRTC 必选 RFC 8854）；
 *   FlexFEC=RFC 8627（视频可配置掩码 FEC，RFC 6682 Raptor 喷泉码与
 *   实时音频无关，勿混用）。
 * 重传（NACK/RFC 4585）由媒体快路径按 rtcp_nack_get 处理，不在本文件。
 */
#include "nsbc_common.h"
#include "rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FEC_SCHEME_NONE = 0,
    FEC_SCHEME_RED,       /* RFC2198 冗余帧携带（音频常用 1 级） */
    FEC_SCHEME_ULP,       /* RFC5109 XOR */
    FEC_SCHEME_FLEX       /* RFC8627 掩码（视频） */
} fec_scheme_t;

typedef struct {
    fec_scheme_t scheme;
    int  red_level;       /* RED 冗余级数（典型 1~2，时延换保护） */
    int  group_size;      /* ULP/FlexFEC 一组包数 */
    int  payload_type;    /* RED/FEC 流 PT（SDP 协商） */
    int  repair_pt;       /* FlexFEC repair 流 PT（ssrc 独立） */
} fec_cfg_t;

typedef struct fec_enc fec_enc_t;
typedef struct fec_dec fec_dec_t;

/* 编码侧：随主包产出 RED 冗余包或组末 FEC repair 包 */
int  fec_enc_create(const fec_cfg_t *cfg, fec_enc_t **out);
void fec_enc_destroy(fec_enc_t *e);
/* 输入主 RTP 载荷；out=本次需随发的冗余/repair（need=0 无） */
int  fec_enc_on_packet(fec_enc_t *e, const uint8_t *payload, size_t len,
                       uint16_t seq, uint32_t ts,
                       uint8_t *out, size_t cap, size_t *out_len, int *need);

/* 解码侧：RED 解封装（多块按时间戳还原）；FEC 组尝试恢复缺失包 */
int  fec_dec_create(const fec_cfg_t *cfg, fec_dec_t **out);
void fec_dec_destroy(fec_dec_t *d);
/* 收到 RED 载荷：还原最后一块为主包（*primary），冗余块内部缓存 */
int  fec_dec_on_red(fec_dec_t *d, const uint8_t *buf, size_t len,
                    const uint8_t **primary, size_t *primary_len,
                    uint32_t *ts_out);
/* 收到 FEC repair：组齐且存在缺包时返恢复包（need=1） */
int  fec_dec_on_repair(fec_dec_t *d, const uint8_t *buf, size_t len,
                       uint8_t *restored, size_t cap,
                       size_t *out_len, uint16_t *restored_seq, int *need);

/* SDP a=fmtp 协商（useinbandfec/RED PT 交集）骨架 */
int  fec_sdp_negotiate(fec_scheme_t want, const char *offer_fmtp,
                       const char *answer_fmtp, fec_cfg_t *out);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_MEDIA_FEC_H */
