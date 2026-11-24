#ifndef NSBC_RTP_VIDEO_H
#define NSBC_RTP_VIDEO_H

/* 视频 RTP 打包 / 解包（REQ-TRANS-VIDEO-001）
 *
 * 只做 RFC 载荷层，不碰 RTP 头（seq/ts/ssrc/PT 由 video_xcode 组装）。
 *   H.264 : RFC 6184 —— single NAL / STAP-A / FU-A，AU 用 Annex-B 起始码拼接
 *   H.265 : RFC 7798 —— single NAL / AP / FU，2 字节 NAL 头，AU 用 Annex-B 拼接
 *   VP8   : RFC 7741 —— X/S/PID 描述符，S=1 起始，marker/ts 判帧尾
 *   VP9   : RFC 7741/9628 —— descriptor(B/E) + 可选 picture id，无 descriptor 复帧
 *
 * 解包器内部按需扩容（上限 RTP_VIDEO_AU_MAX），单线程使用
 * （每呼叫每方向一个实例），无锁。
 */
#include "nsbc_common.h"
#include "media_codec.h"   /* RTP_PAYLOAD_H264 / H265 / VP8 / VP9 */

#ifdef __cplusplus
extern "C" {
#endif

#define RTP_VIDEO_MTU_DEFAULT 1200
#define RTP_VIDEO_AU_MAX      (512 * 1024)

/* ---------------------------------------------------------------- H.264 */
typedef struct {
    uint8_t *au;
    size_t   len;
    size_t   cap;
    uint32_t ts;
    int      have_ts;
    int      in_fu;
} rtp_h264_depack_t;

int  rtp_h264_depack_init(rtp_h264_depack_t *d);
void rtp_h264_depack_free(rtp_h264_depack_t *d);

/* 返回 1=产出完整 access unit；0=尚未收齐；<0=参数/内存错误。
 * 输出指针仅在下次 feed 前有效。key=1 表示含 IDR/SPS。 */
int  rtp_h264_depack_feed(rtp_h264_depack_t *d,
                          const uint8_t *payload, size_t len,
                          int marker, uint32_t ts,
                          const uint8_t **au, size_t *au_len,
                          uint32_t *au_ts, int *key);

/* ------------------------------------------------------------------ VP8 */
typedef struct {
    uint8_t *au;
    size_t   len;
    size_t   cap;
    uint32_t ts;
    int      have_ts;
} rtp_vp8_depack_t;

int  rtp_vp8_depack_init(rtp_vp8_depack_t *d);
void rtp_vp8_depack_free(rtp_vp8_depack_t *d);

/* 返回 1=产出完整帧；0=尚未收齐；<0=参数/内存错误。 */
int  rtp_vp8_depack_feed(rtp_vp8_depack_t *d,
                         const uint8_t *payload, size_t len,
                         int marker, uint32_t ts,
                         const uint8_t **frame, size_t *frame_len,
                         uint32_t *frame_ts, int *key);

/* ------------------------------------------------------------------ VP9 */
typedef struct {
    uint8_t *au;
    size_t   len;
    size_t   cap;
    uint32_t ts;
    int      have_ts;
} rtp_vp9_depack_t;

int  rtp_vp9_depack_init(rtp_vp9_depack_t *d);
void rtp_vp9_depack_free(rtp_vp9_depack_t *d);

int  rtp_vp9_depack_feed(rtp_vp9_depack_t *d,
                         const uint8_t *payload, size_t len,
                         int marker, uint32_t ts,
                         const uint8_t **frame, size_t *frame_len,
                         uint32_t *frame_ts, int *key);

/* --------------------------------------------------------------- H.265 */
/* RFC 7798 —— 2 字节 NAL 头 / AP(48) / FU(49)；NAL 重建后仍用 Annex-B 起始码拼接。
 * 与 H.264 解包器相同的“每方向一个实例、单线程、按需扩容”约定。 */
typedef struct {
    uint8_t *au;
    size_t   len;
    size_t   cap;
    uint32_t ts;
    int      have_ts;
    int      in_fu;
} rtp_h265_depack_t;

int  rtp_h265_depack_init(rtp_h265_depack_t *d);
void rtp_h265_depack_free(rtp_h265_depack_t *d);

/* 返回 1=产出完整 access unit；0=尚未收齐；<0=参数/内存错误。 */
int  rtp_h265_depack_feed(rtp_h265_depack_t *d,
                          const uint8_t *payload, size_t len,
                          int marker, uint32_t ts,
                          const uint8_t **au, size_t *au_len,
                          uint32_t *au_ts, int *key);

/* --------------------------------------------------------------- 打包器 */
/* 每产出一个 RTP 载荷回调一次；marker=1 表示该 AU 的最后一个载荷。 */
typedef void (*rtp_video_pkt_cb)(void *ud, const uint8_t *payload,
                                 size_t len, int marker);

/* Annex-B AU → H.264 RTP 载荷序列 */
int rtp_h264_pack(const uint8_t *au, size_t len, size_t mtu,
                  rtp_video_pkt_cb cb, void *ud);
/* Annex-B AU → H.265 RTP 载荷序列（RFC 7798） */
int rtp_h265_pack(const uint8_t *au, size_t len, size_t mtu,
                  rtp_video_pkt_cb cb, void *ud);
/* VP8 原始帧 → RTP 载荷序列 */
int rtp_vp8_pack(const uint8_t *frame, size_t len, size_t mtu,
                 rtp_video_pkt_cb cb, void *ud);
/* VP9 原始帧 → RTP 载荷序列 */
int rtp_vp9_pack(const uint8_t *frame, size_t len, size_t mtu,
                 rtp_video_pkt_cb cb, void *ud);

/* 关键帧判定：payload 用 RTP_PAYLOAD_H264 / H265 / VP8 / VP9 */
int rtp_video_is_keyframe(int rtp_payload, const uint8_t *au, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_RTP_VIDEO_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
