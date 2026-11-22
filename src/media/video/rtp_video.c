/* 视频 RTP 打包 / 解包实现（RFC 6184 / RFC 7741） */
#include "rtp_video.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * 通用
 * ==========================================================================*/
static int au_reserve(uint8_t **buf, size_t *cap, size_t need)
{
    if (*buf && *cap >= need + 64) return NSBC_OK;
    size_t nc = need + 64;   /* 尾部填充：FFmpeg 解码器要求输入有 padding */
    if (nc < 4096) nc = 4096;
    /* 倍增扩容：FU-A 分片逐包 append，避免大关键帧 O(n²) 重拷贝 */
    if (*buf && nc < *cap * 2) nc = *cap * 2;
    if (nc > RTP_VIDEO_AU_MAX + 64) nc = RTP_VIDEO_AU_MAX + 64;
    uint8_t *nb = (uint8_t *)realloc(*buf, nc);
    if (!nb) return NSBC_NOMEM;
    *buf = nb;
    *cap = nc;
    return NSBC_OK;
}

/* 在 AU 尾部补零（avcodec 可能按 AV_INPUT_BUFFER_PADDING_SIZE 读取） */
static void au_zero_tail(uint8_t *buf, size_t cap, size_t len)
{
    if (!buf) return;
    size_t n = cap - len;
    if (n > 64) n = 64;
    if (n) memset(buf + len, 0, n);
}

static int au_append(uint8_t **buf, size_t *len, size_t *cap,
                     const void *data, size_t n)
{
    if (*len + n > RTP_VIDEO_AU_MAX) return NSBC_NOSPC;
    int rc = au_reserve(buf, cap, *len + n);
    if (rc != NSBC_OK) return rc;
    memcpy(*buf + *len, data, n);
    *len += n;
    return NSBC_OK;
}

static const uint8_t k_start_code[4] = { 0x00, 0x00, 0x00, 0x01 };

/* ============================================================================
 * H.264 解包
 * ==========================================================================*/
int rtp_h264_depack_init(rtp_h264_depack_t *d)
{
    if (!d) return NSBC_INVAL;
    memset(d, 0, sizeof(*d));
    return NSBC_OK;
}

void rtp_h264_depack_free(rtp_h264_depack_t *d)
{
    if (!d) return;
    free(d->au);
    memset(d, 0, sizeof(*d));
}

/* 追加一个完整 NAL（带 Annex-B 起始码） */
static int h264_append_nal(rtp_h264_depack_t *d, const uint8_t *nal, size_t len)
{
    int rc = au_append(&d->au, &d->len, &d->cap, k_start_code, 4);
    if (rc != NSBC_OK) return rc;
    return au_append(&d->au, &d->len, &d->cap, nal, len);
}

int rtp_h264_depack_feed(rtp_h264_depack_t *d,
                         const uint8_t *payload, size_t len,
                         int marker, uint32_t ts,
                         const uint8_t **au, size_t *au_len,
                         uint32_t *au_ts, int *key)
{
    if (!d || !payload || len < 1) return NSBC_INVAL;

    /* 时间戳切换视为上一个 AU 结束（丢包时 marker 可能丢失） */
    if (d->have_ts && ts != d->ts && d->len > 0) {
        d->len = 0;
        d->in_fu = 0;
    }

    int type = payload[0] & 0x1F;

    if (type >= 1 && type <= 23) {
        /* single NAL unit */
        int rc = h264_append_nal(d, payload, len);
        if (rc != NSBC_OK) { d->len = 0; return rc; }
        d->in_fu = 0;
    } else if (type == 24) {
        /* STAP-A: 2 字节长度前缀的 NAL 序列 */
        size_t off = 1;
        while (off + 2 <= len) {
            size_t nl = ((size_t)payload[off] << 8) | payload[off + 1];
            off += 2;
            if (nl == 0 || off + nl > len) break;
            int rc = h264_append_nal(d, payload + off, nl);
            if (rc != NSBC_OK) { d->len = 0; return rc; }
            off += nl;
        }
        d->in_fu = 0;
    } else if (type == 28) {
        /* FU-A: indicator + fu_header */
        if (len < 3) return 0;
        uint8_t fu = payload[1];
        int start = (fu & 0x80) != 0;
        int end   = (fu & 0x40) != 0;
        int nt    = fu & 0x1F;
        if (start) {
            uint8_t nh = (uint8_t)((payload[0] & 0xE0) | (uint8_t)nt);
            int rc = au_append(&d->au, &d->len, &d->cap, k_start_code, 4);
            if (rc == NSBC_OK)
                rc = au_append(&d->au, &d->len, &d->cap, &nh, 1);
            if (rc == NSBC_OK)
                rc = au_append(&d->au, &d->len, &d->cap, payload + 2, len - 2);
            if (rc != NSBC_OK) { d->len = 0; return rc; }
            d->in_fu = 1;
        } else if (d->in_fu) {
            int rc = au_append(&d->au, &d->len, &d->cap, payload + 2, len - 2);
            if (rc != NSBC_OK) { d->len = 0; d->in_fu = 0; return rc; }
        }
        if (end) d->in_fu = 0;
    } else {
        /* 其他类型（STAP-B/MTAP/FU-B）不支持，忽略 */
        return 0;
    }

    d->ts = ts;
    d->have_ts = 1;

    if (!marker) return 0;

    if (d->len == 0) return 0;
    au_zero_tail(d->au, d->cap, d->len);
    *au     = d->au;
    *au_len = d->len;
    *au_ts  = d->ts;
    *key    = rtp_video_is_keyframe(RTP_PAYLOAD_H264, d->au, d->len);
    d->len  = 0;
    d->in_fu = 0;
    return 1;
}

/* ============================================================================
 * VP9 解包
 * ==========================================================================*/
int rtp_vp9_depack_init(rtp_vp9_depack_t *d)
{
    if (!d) return NSBC_INVAL;
    memset(d, 0, sizeof(*d));
    return NSBC_OK;
}

void rtp_vp9_depack_free(rtp_vp9_depack_t *d)
{
    if (!d) return;
    free(d->au);
    memset(d, 0, sizeof(*d));
}

int rtp_vp9_depack_feed(rtp_vp9_depack_t *d,
                        const uint8_t *payload, size_t len,
                        int marker, uint32_t ts,
                        const uint8_t **frame, size_t *frame_len,
                        uint32_t *frame_ts, int *key)
{
    if (!d || !payload || len < 1) return NSBC_INVAL;

    if (d->have_ts && ts != d->ts && d->len > 0)
        d->len = 0;

    uint8_t desc = payload[0];
    int b = (desc & 0x08) != 0;   /* 帧起始 */
    int e = (desc & 0x04) != 0;   /* 帧结束 */
    size_t off = 1;
    if (desc & 0x80)                       /* I: picture id 存在 */
        off += (desc & 0x40) ? 2 : 1;      /* P 决定 7/15 bit */
    if (desc & 0x20) {                     /* L: 可选帧大小 */
        off += 1;
        if (desc & 0x10) off += 1;         /* F + L = 2 字节 */
    }
    if (off >= len) return 0;
    if (b) d->len = 0;                     /* 新帧起始，丢弃残留 */

    int rc = au_append(&d->au, &d->len, &d->cap, payload + off, len - off);
    if (rc != NSBC_OK) { d->len = 0; return rc; }

    d->ts = ts;
    d->have_ts = 1;

    if (!e) return 0;
    if (d->len == 0) return 0;

    au_zero_tail(d->au, d->cap, d->len);
    *frame     = d->au;
    *frame_len = d->len;
    *frame_ts  = d->ts;
    *key       = rtp_video_is_keyframe(RTP_PAYLOAD_VP9, d->au, d->len);
    d->len = 0;
    (void)marker;
    return 1;
}

/* ============================================================================
 * VP8 解包（RFC 7741）
 * ==========================================================================*/
int rtp_vp8_depack_init(rtp_vp8_depack_t *d)
{
    if (!d) return NSBC_INVAL;
    memset(d, 0, sizeof(*d));
    return NSBC_OK;
}

void rtp_vp8_depack_free(rtp_vp8_depack_t *d)
{
    if (!d) return;
    free(d->au);
    memset(d, 0, sizeof(*d));
}

int rtp_vp8_depack_feed(rtp_vp8_depack_t *d,
                        const uint8_t *payload, size_t len,
                        int marker, uint32_t ts,
                        const uint8_t **frame, size_t *frame_len,
                        uint32_t *frame_ts, int *key)
{
    if (!d || !payload || len < 1) return NSBC_INVAL;

    if (d->have_ts && ts != d->ts && d->len > 0)
        d->len = 0;

    /* |X|R|N|S|R|PID|：S=1 表示一个分区/帧的起始 */
    uint8_t desc = payload[0];
    int start = (desc & 0x10) != 0;
    size_t off = 1;
    if (desc & 0x80) {                     /* X: 扩展控制字节 */
        if (len < 2) return 0;
        uint8_t ext = payload[1];
        off = 2;
        if (ext & 0x80) {                  /* I: PictureID，最高位=2 字节 */
            if (off >= len) return 0;
            off += (payload[off] & 0x80) ? 2 : 1;
        }
        if (ext & 0x40) off += 1;          /* L: TL0PICIDX */
        if (ext & 0x30) off += 1;          /* T/K: TID/KEYIDX 共 1 字节 */
    }
    if (off >= len) return 0;
    if (start) d->len = 0;                 /* 新帧起始，丢弃残留 */

    int rc = au_append(&d->au, &d->len, &d->cap, payload + off, len - off);
    if (rc != NSBC_OK) { d->len = 0; return rc; }

    d->ts = ts;
    d->have_ts = 1;

    /* VP8 没有 B/E 位，以 marker（或 ts 切换）作为帧尾 */
    if (!marker) return 0;
    if (d->len == 0) return 0;

    au_zero_tail(d->au, d->cap, d->len);
    *frame     = d->au;
    *frame_len = d->len;
    *frame_ts  = d->ts;
    *key       = rtp_video_is_keyframe(RTP_PAYLOAD_VP8, d->au, d->len);
    d->len = 0;
    return 1;
}

/* ============================================================================
 * H.264 打包
 * ==========================================================================*/
typedef struct { size_t off; size_t len; } nal_span_t;

static int h264_annexb_spans(const uint8_t *au, size_t len,
                             nal_span_t *spans, int max_spans, int *out_n)
{
    int n = 0;
    size_t i = 0;
    int found_sc = 0;

    /* 先找是否存在 Annex-B 起始码 */
    for (i = 0; i + 3 <= len; ++i) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            found_sc = 1;
            break;
        }
    }

    if (!found_sc) {
        /* AVCC（4 字节长度前缀）或裸 NAL */
        size_t p = 0;
        int avcc = 0;
        if (len >= 5) {
            size_t l0 = ((size_t)au[0] << 24) | ((size_t)au[1] << 16) |
                        ((size_t)au[2] << 8) | au[3];
            if (l0 > 0 && l0 + 4 <= len) avcc = 1;
        }
        if (avcc) {
            while (p + 4 <= len && n < max_spans) {
                size_t nl = ((size_t)au[p] << 24) | ((size_t)au[p + 1] << 16) |
                            ((size_t)au[p + 2] << 8) | au[p + 3];
                p += 4;
                if (nl == 0 || p + nl > len) break;
                spans[n].off = p;
                spans[n].len = nl;
                ++n;
                p += nl;
            }
            *out_n = n;
            return n > 0 ? NSBC_OK : NSBC_ERR;
        }
        spans[0].off = 0;
        spans[0].len = len;
        *out_n = 1;
        return NSBC_OK;
    }

    /* Annex-B: 3/4 字节起始码切分 */
    size_t nal_start = 0;
    int have = 0;
    i = 0;
    while (i + 3 <= len) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            if (have) {
                size_t e = i;
                while (e > nal_start && au[e - 1] == 0) --e;  /* 去掉尾随 0 */
                if (e > nal_start && n < max_spans) {
                    spans[n].off = nal_start;
                    spans[n].len = e - nal_start;
                    ++n;
                }
            }
            i += 3;
            nal_start = i;
            have = 1;
            continue;
        }
        ++i;
    }
    if (have) {
        size_t e = len;
        while (e > nal_start && au[e - 1] == 0) --e;
        if (e > nal_start && n < max_spans) {
            spans[n].off = nal_start;
            spans[n].len = e - nal_start;
            ++n;
        }
    }
    *out_n = n;
    return n > 0 ? NSBC_OK : NSBC_ERR;
}

int rtp_h264_pack(const uint8_t *au, size_t len, size_t mtu,
                  rtp_video_pkt_cb cb, void *ud)
{
    if (!au || len == 0 || !cb) return NSBC_INVAL;
    if (mtu < 64) mtu = RTP_VIDEO_MTU_DEFAULT;

    nal_span_t spans[128];
    int n = 0;
    int rc = h264_annexb_spans(au, len, spans, 128, &n);
    if (rc != NSBC_OK) return rc;

    uint8_t buf[RTP_VIDEO_MTU_DEFAULT + 16];
    size_t pkt_mtu = mtu > sizeof(buf) ? sizeof(buf) : mtu;

    for (int i = 0; i < n; ++i) {
        const uint8_t *nal = au + spans[i].off;
        size_t nl = spans[i].len;
        int last_nal = (i == n - 1);

        if (nl <= pkt_mtu - 1) {
            cb(ud, nal, nl, last_nal);
            continue;
        }
        /* FU-A 分片 */
        uint8_t indicator = (uint8_t)((nal[0] & 0xE0) | 28);
        uint8_t nt = (uint8_t)(nal[0] & 0x1F);
        size_t off = 1;
        size_t chunk = pkt_mtu - 2;
        while (off < nl) {
            size_t take = nl - off;
            if (take > chunk) take = chunk;
            int start = (off == 1);
            int end = (off + take >= nl);
            buf[0] = indicator;
            buf[1] = (uint8_t)((start ? 0x80 : 0) | (end ? 0x40 : 0) | nt);
            memcpy(buf + 2, nal + off, take);
            cb(ud, buf, take + 2, last_nal && end);
            off += take;
        }
    }
    return NSBC_OK;
}

/* ============================================================================
 * VP9 打包
 * ==========================================================================*/
int rtp_vp9_pack(const uint8_t *frame, size_t len, size_t mtu,
                 rtp_video_pkt_cb cb, void *ud)
{
    if (!frame || len == 0 || !cb) return NSBC_INVAL;
    if (mtu < 64) mtu = RTP_VIDEO_MTU_DEFAULT;

    uint8_t buf[RTP_VIDEO_MTU_DEFAULT + 16];
    size_t pkt_mtu = mtu > sizeof(buf) ? sizeof(buf) : mtu;
    size_t chunk = pkt_mtu - 1;

    if (len <= chunk) {
        buf[0] = 0x0C;   /* B=1, E=1 */
        memcpy(buf + 1, frame, len);
        cb(ud, buf, len + 1, 1);
        return NSBC_OK;
    }

    size_t off = 0;
    while (off < len) {
        size_t take = len - off;
        if (take > chunk) take = chunk;
        int start = (off == 0);
        int end = (off + take >= len);
        buf[0] = (uint8_t)((start ? 0x08 : 0) | (end ? 0x04 : 0));
        memcpy(buf + 1, frame + off, take);
        cb(ud, buf, take + 1, end);
        off += take;
    }
    return NSBC_OK;
}

/* ============================================================================
 * VP8 打包
 * ==========================================================================*/
int rtp_vp8_pack(const uint8_t *frame, size_t len, size_t mtu,
                 rtp_video_pkt_cb cb, void *ud)
{
    if (!frame || len == 0 || !cb) return NSBC_INVAL;
    if (mtu < 64) mtu = RTP_VIDEO_MTU_DEFAULT;

    uint8_t buf[RTP_VIDEO_MTU_DEFAULT + 16];
    size_t pkt_mtu = mtu > sizeof(buf) ? sizeof(buf) : mtu;
    size_t chunk = pkt_mtu - 1;

    if (len <= chunk) {
        buf[0] = 0x10;   /* X=0, N=0, S=1, PID=0 */
        memcpy(buf + 1, frame, len);
        cb(ud, buf, len + 1, 1);
        return NSBC_OK;
    }

    size_t off = 0;
    while (off < len) {
        size_t take = len - off;
        if (take > chunk) take = chunk;
        int end = (off + take >= len);
        /* 首包带 S=1，后续包描述符全 0；帧尾靠 marker 标记 */
        buf[0] = (uint8_t)(off == 0 ? 0x10 : 0x00);
        memcpy(buf + 1, frame + off, take);
        cb(ud, buf, take + 1, end);
        off += take;
    }
    return NSBC_OK;
}

/* ============================================================================
 * 关键帧判定
 * ==========================================================================*/
int rtp_video_is_keyframe(int rtp_payload, const uint8_t *au, size_t len)
{
    if (!au || len < 1) return 0;

    if (rtp_payload == RTP_PAYLOAD_H264) {
        size_t i = 0;
        while (i + 3 <= len) {
            if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
                size_t n = i + 3;
                if (n < len) {
                    int t = au[n] & 0x1F;
                    if (t == 5 || t == 7) return 1;   /* IDR / SPS */
                }
                i = n;
                continue;
            }
            ++i;
        }
        return 0;
    }

    if (rtp_payload == RTP_PAYLOAD_VP9) {
        /* 未压缩头 bit2 = frame_type，0 表示 KEY_FRAME */
        return (au[0] & 0x04) == 0;
    }

    if (rtp_payload == RTP_PAYLOAD_VP8) {
        /* 帧头 bit0 = P，0 表示关键帧 */
        return (au[0] & 0x01) == 0;
    }

    return 0;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
