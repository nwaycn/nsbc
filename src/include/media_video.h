#ifndef NSBC_MEDIA_VIDEO_H
#define NSBC_MEDIA_VIDEO_H

/* 视频媒体（REQ-VID-001/002，P3）
 *
 * 引用口径（项目记忆，防错）：
 *   H.264 RTP 载荷 RFC 6184（FU-A/STAP-A 分帧）；
 *   SVC 是 H.264 Annex G，RTP 载荷仍 RFC 6190；
 *   视频属性 a=orient 出处 RFC 8866 §6.8；
 *   BFCP 协议本体 RFC 8855（Obsoletes 4582），SDP 映射 RFC 4583。
 * P3：本批仅粗骨架；视频转发为 RTP 透明中继（SSRC 重写规则与音频
 * 同族 REQ-MEDIA-SEC-003），不做视频转码。
 */
#include "nsbc_common.h"
#include "rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VID_NONE = 0,
    VID_H264,
    VID_H264_SVC,   /* Annex G（RTP RFC6190） */
    VID_H265,
    VID_VP8,
    VID_VP9,
    VID_AV1
} video_codec_t;

typedef struct {
    video_codec_t codec;
    int  clock_rate;    /* RTP 90000 */
    int  width;
    int  height;
    int  framerate;
    int  payload_type;
    char orient[8];     /* sendonly/recvonly/sendrecv/inactive 非本字段；
                         * orient: portrait/landscape/seascape */
} video_stream_t;

/* H.264 分帧识别（RFC6184）：NAL 类型（FU-A=28/FU-B=29/STAP-A=24…） */
typedef enum {
    H264_NAL_SLICE = 1,
    H264_NAL_IDR = 5,
    H264_NAL_SEI = 6,
    H264_NAL_SPS = 7,
    H264_NAL_PPS = 8,
    H264_NAL_STAP_A = 24,
    H264_NAL_FU_A = 28,
    H264_NAL_FU_B = 29
} h264_nal_type_t;

typedef struct video_fwd video_fwd_t;

int  video_fwd_create(const video_stream_t *s, video_fwd_t **out);
void video_fwd_destroy(video_fwd_t *v);

/* 关键帧判定（IDR/FU-A 起始片含 IDR NRI），用于转发/录制标记 */
int  video_is_keyframe(const uint8_t *payload, size_t len);
/* 透转发每包处理：目前仅 SSRC/序号策略，无转码；预留 SVC 层过滤槽 */
int  video_fwd_on_packet(video_fwd_t *v, const rtp_pkt_t *pkt,
                         uint8_t *out, size_t cap, size_t *out_len);

/* 关键帧请求：FIR（RFC5104，pt=206 FMT=4）/PLI（RFC4585 FMT=1）；
 * 首包丢失/转码入场时由组合根触发，渲染到 RTCP 反馈包。 */
int  video_build_pli(uint32_t media_ssrc, uint32_t src_ssrc,
                     uint8_t *out, size_t cap, size_t *out_len);
int  video_build_fir(uint32_t media_ssrc, uint32_t src_ssrc,
                     uint8_t seq, uint8_t *out, size_t cap,
                     size_t *out_len);

/* BFCP 会话（RFC8855/4583）：floor control 独立 m=application 段，
 * 粗骨架仅建/销与透传，不解析 BFCP 原语。 */
typedef struct bfcp_sess bfcp_sess_t;
int  bfcp_create(int floor_count, bfcp_sess_t **out);
void bfcp_destroy(bfcp_sess_t *b);
int  bfcp_on_packet(bfcp_sess_t *b, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_MEDIA_VIDEO_H */
