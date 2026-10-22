#ifndef NSBC_RTP_H
#define NSBC_RTP_H

#include "nsbc_common.h"
#include "sip_msg.h" /* sip_tok_t 零拷贝令牌 */

/* ---- 大端读取（RTP/RTCP 全部为网络字节序） ---- */
static inline uint16_t rtp_rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rtp_rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* ---- RTP 固定头解析结果（零拷贝；CSRC/扩展/payload 指向原包） ---- */
typedef struct {
    int      version;
    int      padding;
    int      extension;
    int      cc;
    int      marker;
    int      pt;
    uint16_t seq;
    uint32_t ts;
    uint32_t ssrc;
    const uint8_t *csrc; /* 4*cc 字节 */

    uint16_t ext_profile;
    const uint8_t *ext_data;
    uint16_t ext_words;

    const uint8_t *payload;
    size_t         payload_len; /* 去除填充 */
    size_t         hdr_len;
    uint8_t        pad_len;
} rtp_pkt_t;

nsbc_rc_t rtp_parse(const uint8_t *buf, size_t len, rtp_pkt_t *p);

/* ---- RFC 4733 telephone-event 载荷（>=4 字节） ---- */
typedef struct {
    uint8_t event;
    int     end;
    uint8_t volume;
    uint16_t duration;
} rtp_dtmf_t;

nsbc_rc_t rtp_parse_dtmf(const uint8_t *buf, size_t len, rtp_dtmf_t *d);

/* ---- RTCP compound 迭代 ---- */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} rtcp_iter_t;

typedef struct {
    int      version;
    int      padding;
    int      rc;
    uint8_t  pt;
    uint16_t words; /* length 字段：32-bit 词数 - 1 */
    size_t   total; /* (words+1)*4 */
    const uint8_t *body; /* 公共 4 字节头之后 */
    const uint8_t *raw;
} rtcp_pkt_t;

void     rtcp_iter_init(rtcp_iter_t *it, const uint8_t *buf, size_t len);
int      rtcp_next(rtcp_iter_t *it, rtcp_pkt_t *p); /* 1=取到，0=结束 */

/* SR 发送者信息（RC 由 pkt.rc 给出） */
typedef struct {
    uint32_t ssrc;
    uint64_t ntp;
    uint32_t rtp_ts;
    uint32_t pkt_count;
    uint32_t octet_count;
} rtcp_sr_t;
nsbc_rc_t rtcp_parse_sr(const rtcp_pkt_t *p, rtcp_sr_t *sr);

/* 接收报告块（SR/RR 通用） */
typedef struct {
    uint32_t ssrc;
    uint8_t  fraction;
    uint32_t lost;        /* 24bit 无符号解释 */
    int32_t  lost_signed; /* 24bit 有符号解释 */
    uint32_t ext_seq;
    uint32_t jitter;
    uint32_t lsr;
    uint32_t dlsr;
} rtcp_rb_t;
nsbc_rc_t rtcp_rb_get(const rtcp_pkt_t *p, int idx, rtcp_rb_t *rb);

/* RTCP-XR 块迭代 */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} rtcp_xr_iter_t;
typedef struct {
    uint8_t  bt;
    uint8_t  ts;
    const uint8_t *data;
    size_t         data_len; /* words*4 */
} rtcp_xr_block_t;
void     rtcp_xr_iter_init(rtcp_xr_iter_t *it, const rtcp_pkt_t *p);
int      rtcp_xr_next(rtcp_xr_iter_t *it, rtcp_xr_block_t *b);

/* RFC 5761 RTP/RTCP 复用判别：PT 在 64..95 为 RTCP */
int rtp_mux_is_rtcp(const uint8_t *buf, size_t len);

/* ---- 端口收包统一分类（ICE 场景同一端口可能收到多种报文） ---- */
typedef enum {
    PKT_UNKNOWN = 0,
    PKT_RTP,
    PKT_RTCP,
    PKT_STUN,          /* RFC 5389 */
    PKT_TURN_CHANNEL,  /* RFC 5766 ChannelData */
    PKT_DTLS           /* DTLS（DTLS-SRTP 握手/媒体） */
} pkt_kind_t;
pkt_kind_t rtp_classify(const uint8_t *buf, size_t len);

/* ---- RFC 5285 RTP 头扩展迭代（one-byte/two-byte） ---- */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    int            two_byte;
} rtp_ext_iter_t;
typedef struct {
    uint8_t        id;
    const uint8_t *data;
    size_t         len;
} rtp_ext_elem_t;
void rtp_ext_iter_init(rtp_ext_iter_t *it, const rtp_pkt_t *pkt);
int  rtp_ext_next(rtp_ext_iter_t *it, rtp_ext_elem_t *e);

/* ---- RTCP SDES 条目（CNAME 为 RFC 3550 强制项） ---- */
nsbc_rc_t rtcp_sdes_cname(const rtcp_pkt_t *p, uint32_t *ssrc,
                          sip_tok_t *cname);

/* ---- RTPFB(205)/PSFB(206) 反馈头与常用 FCI ---- */
nsbc_rc_t rtcp_fb_ssrc(const rtcp_pkt_t *p, uint32_t *media_ssrc,
                       uint32_t *src_ssrc);
nsbc_rc_t rtcp_nack_get(const rtcp_pkt_t *p, int idx, uint16_t *pid,
                        uint16_t *blp);
nsbc_rc_t rtcp_fir_get(const rtcp_pkt_t *p, int idx, uint32_t *ssrc,
                       uint8_t *seq);

/* ---- 媒体转发闸门：re-INVITE/UPDATE/前提条件/hold 联动 ---- */
typedef enum {
    RTP_GATE_LOCKED = 0, /* 不转发媒体 */
    RTP_GATE_EARLY,      /* 仅转发早期媒体 */
    RTP_GATE_OPEN        /* 正常转发 */
} rtp_gate_state_t;
typedef struct {
    rtp_gate_state_t state;
} rtp_gate_t;
void rtp_gate_init(rtp_gate_t *g);
/* precond_active 且未满足：锁定（early_media 时放行早期媒体）；
 * 已满足/无前提：dir_open=0（hold/inactive）锁定，否则打开 */
void rtp_gate_update(rtp_gate_t *g, int precond_active, int precond_met,
                     int early_media, int dir_open);
int  rtp_gate_forward(const rtp_gate_t *g);       /* OPEN 才转发 */
int  rtp_gate_forward_early(const rtp_gate_t *g);  /* OPEN/EARLY 转发 */

/* ---- SSRC 学习与重指防护（配合 REQ-MEDIA-SEC-001） ----
 * re-INVITE/UPDATE 改媒体对端后部分 UE 重置 SSRC：
 * 重指给予一次学习新 SSRC 的宽限，此后第三个 SSRC 视为注入丢弃 */
typedef struct {
    uint32_t ssrc;
    int      learned;
    int      grace;
} rtp_ssrc_guard_t;
void rtp_ssrc_guard_init(rtp_ssrc_guard_t *g);
void rtp_ssrc_guard_retarget(rtp_ssrc_guard_t *g);
int  rtp_ssrc_guard_check(rtp_ssrc_guard_t *g, uint32_t ssrc);

#endif /* NSBC_RTP_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
