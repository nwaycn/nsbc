/* 端口收包统一分类：RTP/RTCP/STUN/TURN ChannelData/DTLS
 * 这些报文在 ICE / DTLS-SRTP / TURN 场景下可能到达同一端口。
 * 依据：RFC 3550（V 位）、RFC 5389（magic cookie）、
 * RFC 5766（channel 号 0x4000~0x7FFF）、RFC 6347（DTLS record） */

#include "rtp.h"

pkt_kind_t rtp_classify(const uint8_t *b, size_t len)
{
    if (len < 1)
        return PKT_UNKNOWN;

    /* STUN：≥20B，前 16 位高 2 位为 00，偏移 4 为固定 magic cookie。
     * STUN 消息类型合法范围 0x0000..0x01FF 且保留位为 0。 */
    if (len >= 20 && (b[0] & 0xc0) == 0 &&
        rtp_rd32(b + 4) == 0x2112a442u)
        return PKT_STUN;

    /* DTLS record：content type 20..63，major version 在 byte1=0xFE */
    if (len >= 3 && b[0] >= 20 && b[0] <= 63 && b[1] == 0xfe)
        return PKT_DTLS;

    /* TURN ChannelData：channel 号 0x4000..0x7FFF；
     * 长度字段应与报文吻合（容忍外层封装余量，仅做范围校验） */
    if (len >= 4) {
        uint16_t ch = rtp_rd16(b);
        if (ch >= 0x4000 && ch <= 0x7fff)
            return PKT_TURN_CHANNEL;
    }

    /* RTP/RTCP：版本位必须为 2；PT 64..95 按 RFC 5761 归为 RTCP */
    if (((b[0] >> 6) & 0x3) == 2 && len >= 2) {
        int pt = b[1] & 0x7f;
        if (pt >= 64 && pt <= 95)
            return PKT_RTCP;
        return PKT_RTP;
    }
    return PKT_UNKNOWN;
}