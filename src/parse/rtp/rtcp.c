/* RTCP 解析（RFC 3550/3611/4585/5761）：
 * compound 包逐包迭代；严格校验版本与 length 字段；
 * SR/RR 报告块与 RTCP-XR 块可独立取；RFC 5761 复用判别 */

#include "rtp.h"

void rtcp_iter_init(rtcp_iter_t *it, const uint8_t *buf, size_t len)
{
    it->p = buf;
    it->end = buf + len;
}

int rtcp_next(rtcp_iter_t *it, rtcp_pkt_t *p)
{
    if (it->p + 4 > it->end)
        return 0;
    const uint8_t *b = it->p;

    memset(p, 0, sizeof(*p));
    p->version = (b[0] >> 6) & 0x3;
    p->padding = (b[0] >> 5) & 0x1;
    p->rc = b[0] & 0x1f;
    p->pt = b[1];
    p->words = rtp_rd16(b + 2);
    p->total = ((size_t)p->words + 1) * 4;
    p->raw = b;
    p->body = b + 4;

    if (p->version != 2)
        return 0; /* 严格模式：版本非法，迭代中止 */
    if (b + p->total > it->end)
        return 0; /* length 字段越过缓冲 */
    if (p->padding) {
        uint8_t n = b[p->total - 1];
        /* 公共头 4 字节不计入填充可覆盖范围 */
        if (n == 0 || (size_t)n + 4 > p->total)
            return 0;
    }

    it->p = b + p->total;
    return 1;
}

/* SR：body[0..4) SSRC + 20 字节 sender info */
nsbc_rc_t rtcp_parse_sr(const rtcp_pkt_t *p, rtcp_sr_t *sr)
{
    memset(sr, 0, sizeof(*sr));
    if (p->pt != 200)
        return NSBC_INVAL;
    if (p->total < 4 + 4 + 20)
        return NSBC_ERR;
    sr->ssrc = rtp_rd32(p->body);
    const uint8_t *si = p->body + 4;
    uint64_t hi = rtp_rd32(si);
    uint64_t lo = rtp_rd32(si + 4);
    sr->ntp = (hi << 32) | lo;
    sr->rtp_ts = rtp_rd32(si + 8);
    sr->pkt_count = rtp_rd32(si + 12);
    sr->octet_count = rtp_rd32(si + 16);
    return NSBC_OK;
}

/* 报告块定位：SR 在 sender info 之后（body+24），RR 在 SSRC 之后（body+4） */
nsbc_rc_t rtcp_rb_get(const rtcp_pkt_t *p, int idx, rtcp_rb_t *rb)
{
    memset(rb, 0, sizeof(*rb));
    if (p->pt != 200 && p->pt != 201)
        return NSBC_INVAL;
    if (idx < 0 || idx >= p->rc)
        return NSBC_INVAL;

    size_t base = (p->pt == 200) ? 24 : 4;
    size_t off = base + (size_t)idx * 24;
    if (off + 24 > p->total)
        return NSBC_ERR;
    const uint8_t *q = p->body + off;
    rb->ssrc = rtp_rd32(q);
    rb->fraction = q[4];
    uint32_t raw = ((uint32_t)q[5] << 16) | ((uint32_t)q[6] << 8) | q[7];
    rb->lost = raw & 0x7fffffu;
    rb->lost_signed =
        (raw & 0x800000u) ? (int32_t)(raw | 0xff000000u) : (int32_t)raw;
    rb->ext_seq = rtp_rd32(q + 8);
    rb->jitter = rtp_rd32(q + 12);
    rb->lsr = rtp_rd32(q + 16);
    rb->dlsr = rtp_rd32(q + 20);
    return NSBC_OK;
}

/* SDES：SSRC + item*(type 1B/len 1B/data)，type 0 结束并填充。
 * CNAME（type 1）为强制项，缺失视为畸形。 */
nsbc_rc_t rtcp_sdes_cname(const rtcp_pkt_t *p, uint32_t *ssrc,
                          sip_tok_t *cname)
{
    if (p->pt != 202)
        return NSBC_INVAL;
    if (p->total < 4)
        return NSBC_ERR;
    const uint8_t *b = p->body;
    *ssrc = rtp_rd32(b);
    const uint8_t *q = b + 4;
    const uint8_t *end = p->raw + p->total;

    while (q < end) {
        uint8_t t = q[0];
        if (t == 0)
            break;
        if (q + 2 > end)
            return NSBC_ERR;
        uint8_t len = q[1];
        if (q + 2 + len > end)
            return NSBC_ERR;
        if (t == 1) {
            cname->p = (const char *)q + 2;
            cname->len = len;
            return NSBC_OK;
        }
        q += 2 + len;
    }
    return NSBC_ERR;
}

/* RTPFB(205)/PSFB(206) 公共头：SSRC of media sender + SSRC of media source */
nsbc_rc_t rtcp_fb_ssrc(const rtcp_pkt_t *p, uint32_t *media_ssrc,
                       uint32_t *src_ssrc)
{
    if (p->pt != 205 && p->pt != 206)
        return NSBC_INVAL;
    if (p->total < 8)
        return NSBC_ERR;
    *media_ssrc = rtp_rd32(p->body);
    *src_ssrc = rtp_rd32(p->body + 4);
    return NSBC_OK;
}

/* NACK：RTPFB FMT=1；FCI 每项 PID(16)+BLP(16) */
nsbc_rc_t rtcp_nack_get(const rtcp_pkt_t *p, int idx, uint16_t *pid,
                        uint16_t *blp)
{
    if (p->pt != 205 || p->rc != 1)
        return NSBC_INVAL;
    if (p->total < 8 || (size_t)(idx + 1) * 4 > p->total - 8)
        return NSBC_INVAL;
    const uint8_t *q = p->body + 8 + (size_t)idx * 4;
    *pid = rtp_rd16(q);
    *blp = rtp_rd16(q + 2);
    return NSBC_OK;
}

/* FIR：PSFB FMT=4；FCI 每项 SSRC(32)+Seq nr(8)+reserved(24)，8 字节 */
nsbc_rc_t rtcp_fir_get(const rtcp_pkt_t *p, int idx, uint32_t *ssrc,
                       uint8_t *seq)
{
    if (p->pt != 206 || p->rc != 4)
        return NSBC_INVAL;
    if (p->total < 8 || (size_t)(idx + 1) * 8 > p->total - 8)
        return NSBC_INVAL;
    const uint8_t *q = p->body + 8 + (size_t)idx * 8;
    *ssrc = rtp_rd32(q);
    *seq = q[4];
    return NSBC_OK;
}

/* RTCP-XR（PT 207）：SSRC + 若干 block（BT/TS 各 1B + length 2B + data） */
void rtcp_xr_iter_init(rtcp_xr_iter_t *it, const rtcp_pkt_t *p)
{
    if (p->total >= 8) {
        it->p = p->body + 4; /* 跳过 SSRC */
        it->end = p->raw + p->total;
    } else {
        it->p = it->end = p->raw;
    }
}

int rtcp_xr_next(rtcp_xr_iter_t *it, rtcp_xr_block_t *b)
{
    if (it->p + 4 > it->end)
        return 0;
    memset(b, 0, sizeof(*b));
    b->bt = it->p[0];
    b->ts = it->p[1];
    uint16_t words = rtp_rd16(it->p + 2);
    size_t dlen = (size_t)words * 4;
    if (it->p + 4 + dlen > it->end)
        return 0;
    b->data = it->p + 4;
    b->data_len = dlen;
    it->p += 4 + dlen;
    return 1;
}

/* RFC 5761：V=2 且 PT 在 64..95 为 RTCP；RTP PT 在 0..63/96..127 */
int rtp_mux_is_rtcp(const uint8_t *buf, size_t len)
{
    if (len < 2)
        return 0;
    if (((buf[0] >> 6) & 0x3) != 2)
        return 0; /* 先验版本位：非 RTP 报文不按 PT 归类 */
    int pt = buf[1] & 0x7f;
    return pt >= 64 && pt <= 95;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
