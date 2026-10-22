/* RTP 固定头解析（RFC 3550 §5.1/5.2/5.3）：
 * 12 字节固定头 + 4*CC CSRC + 可选扩展头 + payload [+ padding]
 * 严格模式：V 必须为 2；长度与填充计数必须自洽 */

#include "rtp.h"

nsbc_rc_t rtp_parse(const uint8_t *buf, size_t len, rtp_pkt_t *p)
{
    memset(p, 0, sizeof(*p));
    if (len < 12)
        return NSBC_ERR;

    p->version = (buf[0] >> 6) & 0x3;
    p->padding = (buf[0] >> 5) & 0x1;
    p->extension = (buf[0] >> 4) & 0x1;
    p->cc = buf[0] & 0xf;
    p->marker = (buf[1] >> 7) & 0x1;
    p->pt = buf[1] & 0x7f;
    p->seq = rtp_rd16(buf + 2);
    p->ts = rtp_rd32(buf + 4);
    p->ssrc = rtp_rd32(buf + 8);

    if (p->version != 2)
        return NSBC_ERR;

    size_t off = 12;
    size_t csrc_bytes = (size_t)p->cc * 4;
    if (off + csrc_bytes > len)
        return NSBC_ERR;
    p->csrc = buf + off;
    off += csrc_bytes;

    if (p->extension) {
        if (off + 4 > len)
            return NSBC_ERR;
        p->ext_profile = rtp_rd16(buf + off);
        p->ext_words = rtp_rd16(buf + off + 2);
        off += 4;
        size_t edata = (size_t)p->ext_words * 4;
        if (off + edata > len)
            return NSBC_ERR;
        p->ext_data = buf + off;
        off += edata;
    }
    p->hdr_len = off;

    /* 前置检查已保证 off <= len */
    size_t tail = len - off;

    if (p->padding) {
        if (tail == 0)
            return NSBC_ERR;
        p->pad_len = buf[len - 1];
        if (p->pad_len == 0 || (size_t)p->pad_len > tail)
            return NSBC_ERR;
        tail -= p->pad_len;
    }
    p->payload = buf + off;
    p->payload_len = tail;
    return NSBC_OK;
}