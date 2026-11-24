#ifndef NSBC_VIDEO_XCODE_H
#define NSBC_VIDEO_XCODE_H

/* 视频转码会话 video_xcode（REQ-TRANS-VIDEO-002）
 *
 * 单方向流水线：源腿 RTP 载荷 → 解包(AU) → FFmpeg 解码 → 重编码
 *               → 打包为对侧腿 RTP 载荷（回调发送）。
 *
 * 只支持 H.264 / H.265 / VP8 / VP9（其余组合返回 NULL，呼叫保持原透传）。
 * 每方向一个实例，仅媒体线程访问；无锁、无动态扩容。
 */
#include "nsbc_common.h"
#include "media_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct video_xcode video_xcode_t;

/* 每产出一个 RTP 包（含 12 字节头）回调一次 */
typedef void (*video_xcode_emit_fn)(void *ud, const uint8_t *pkt, size_t len);

/* 每个重编码成功的帧回调一次（H.264 Annex-B，含起始码/SPS/PPS）。
 * 供录制旁路直接取 AU，免去再打包成 RTP 又解包的开销。
 * key=1 表示该帧为 IDR；ts 为输出 90kHz RTP 时间戳。
 * tap 实例（video_xcode_create_tap）回调的是源编码 AU，格式见该函数说明。 */
typedef void (*video_xcode_au_fn)(void *ud, const uint8_t *au, size_t len,
                                  uint32_t ts, int key);

/* 创建会话；src/dst 必须可解/可编。失败返回 NULL（保持透传）。 */
video_xcode_t *video_xcode_create(codec_id_t src, codec_id_t dst,
                                  uint8_t src_pt, uint8_t dst_pt,
                                  uint32_t out_ssrc);

/* 观察模式（同编码也建）：只解包，不经解码/重编码，收齐完整帧即回调
 * au sink，交付源编码 AU（H.264/H.265 为 Annex-B，VP8/VP9 为裸帧）。
 * 供录像（H.264 腿）与抓图（全部编码）使用，几乎不占 CPU。 */
video_xcode_t *video_xcode_create_tap(codec_id_t src, uint8_t src_pt);

void video_xcode_destroy(video_xcode_t *vx);

/* 输入一个源腿 RTP 载荷；收齐完整帧时内部解码/重编码并多次回调 emit。
 * 返回 0=已消费；<0=内部错误（调用方丢弃即可）。 */
int video_xcode_input(video_xcode_t *vx,
                      const uint8_t *payload, size_t len,
                      int marker, uint32_t rtp_ts,
                      video_xcode_emit_fn emit, void *ud);

/* 取走一次“需要上游关键帧”的请求（PLI/FIR 由调用方发送）。 */
int video_xcode_take_pli(video_xcode_t *vx);

/* 注册重编码 AU 旁路（录制用）。fn=NULL 取消。仅媒体线程调用。 */
void video_xcode_set_au_sink(video_xcode_t *vx, video_xcode_au_fn fn, void *ud);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_VIDEO_XCODE_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
