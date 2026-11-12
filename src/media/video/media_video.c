/* 视频媒体——粗骨架（REQ-VID-001/002，P3）
 *
 * 视频走 bypass/透明中继主路径（不进转码引擎）；SSRC 出向重写、
 * latching、rtcp-mux/BUNDLE 处理与音频共用 media_bridge 策略。
 * 端口：视频/屏幕辅流（H.239 式 content）为独立 m 段，端口表各分配。
 */
#include "media_video.h"
#include <arpa/inet.h>   /* htonl */

struct video_fwd {
    video_stream_t s;
};

int video_fwd_create(const video_stream_t *s, video_fwd_t **out)
{
    video_fwd_t *v;
    if (s == NULL || out == NULL || s->clock_rate != 90000)
        return NSBC_INVAL;
    v = (video_fwd_t *)calloc(1, sizeof(*v));
    if (v == NULL)
        return NSBC_NOMEM;
    v->s = *s;
    *out = v;
    return NSBC_OK;
}

void video_fwd_destroy(video_fwd_t *v)
{
    free(v);
}

int video_is_keyframe(const uint8_t *payload, size_t len)
{
    unsigned nal;
    if (payload == NULL || len == 0)
        return 0;
    nal = payload[0] & 0x1f;
    if (nal == H264_NAL_IDR)
        return 1;
    if (nal == H264_NAL_FU_A && len >= 2) {
        unsigned inner = payload[1] & 0x1f;
        int start = (payload[1] & 0x80) != 0; /* FU 指示位 Start */
        return start && inner == H264_NAL_IDR;
    }
    return 0;
}

int video_fwd_on_packet(video_fwd_t *v, const rtp_pkt_t *pkt, uint8_t *out, size_t cap, size_t *out_len)
{
    if (v == NULL || pkt == NULL || out == NULL || out_len == NULL)
        return NSBC_INVAL;
    if (pkt->payload == NULL || cap < pkt->payload_len)
        return NSBC_NOSPC;
    /* 透明中继：载荷原样（SSRC/seq 由外层重写策略统一处理）；
     * TODO(P3)：SVC 按 TemporalID 过滤给受限带宽接收者。 */
    memcpy(out, pkt->payload, pkt->payload_len);
    *out_len = pkt->payload_len;
    return NSBC_OK;
}

int video_build_pli(uint32_t media_ssrc, uint32_t src_ssrc, uint8_t *out, size_t cap, size_t *out_len)
{
    /* RFC 4585 §6.1: PLI = Picture Loss Indication
     *   0                   1                   2                   3
     *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * |V=2|P| FMT=1   | PT=206(PSFB) |         length=2              |
     * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * |                        Sender SSRC                            |
     * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * |                         Media SSRC                            |
     * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * 总 12 字节，无 FCI（Feedback Control Information）。 */
    if (out == NULL || out_len == NULL)
        return NSBC_INVAL;
    if (cap < 12)
        return NSBC_NOSPC;

    out[0] = 0x81;                 /* V=2, P=0, FMT=1 (PLI) */
    out[1] = 206;                  /* PT=PSFB */
    out[2] = 0x00;                 /* length high */
    out[3] = 0x02;                 /* length low (32-bit words - 1) */

    uint32_t s = htonl(src_ssrc);      /* Sender SSRC (nsbcd) */
    uint32_t m = htonl(media_ssrc);    /* Media SSRC (Chrome) */
    memcpy(out + 4, &s, 4);
    memcpy(out + 8, &m, 4);

    *out_len = 12;
    return NSBC_OK;
}

int video_build_fir(uint32_t media_ssrc, uint32_t src_ssrc, uint8_t seq, uint8_t *out, size_t cap, size_t *out_len)
{
    /* TODO(P3)：RFC5104 FIR FCI=SSRC(4)+Seq nr(1)+R(3)，8B */
    (void)media_ssrc;
    (void)src_ssrc;
    (void)seq;
    (void)cap;
    if (out == NULL || out_len == NULL)
        return NSBC_INVAL;
    *out_len = 0;
    return NSBC_NOTSUP;
}

struct bfcp_sess {
    int floors;
};

int bfcp_create(int floor_count, bfcp_sess_t **out)
{
    bfcp_sess_t *b;
    if (floor_count <= 0 || out == NULL)
        return NSBC_INVAL;
    b = (bfcp_sess_t *)calloc(1, sizeof(*b));
    if (b == NULL)
        return NSBC_NOMEM;
    b->floors = floor_count;
    *out = b;
    return NSBC_OK;
}

void bfcp_destroy(bfcp_sess_t *b)
{
    free(b);
}

int bfcp_on_packet(bfcp_sess_t *b, const uint8_t *buf, size_t len)
{
    if (b == NULL || buf == NULL || len == 0)
        return NSBC_INVAL;
    /* TODO(P3)：RFC8855 FloorRequest/Release/Query 透传+floor 状态镜像，
     * 与视频 content 流联动（谁拿 floor 谁发辅流）。 */
    return NSBC_NOTSUP;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
