#ifndef NSBC_SDP_H
#define NSBC_SDP_H

#include "sip_msg.h" /* 复用 sip_tok_t 零拷贝令牌 */

/* ---- 硬限制 ---- */
#define SDP_MAX_LINE   4096
#define SDP_MAX_MEDIA  8
#define SDP_MAX_FMT    32     /* P0 升: WebRTC video 段 15 PT, Chrome 25+ extmap 加 PT */
#define SDP_MAX_ATTRS  256    /* P0 升: 原 48 不够 — Chrome WebRTC offer 单 media 段 20+ attr */

typedef enum {
    SDP_M_UNKNOWN = 0,
    SDP_M_AUDIO,
    SDP_M_VIDEO,
    SDP_M_TEXT,
    SDP_M_APPLICATION,
    SDP_M_MESSAGE,
    SDP_M_IMAGE
} sdp_mtype_t;

/* 媒体方向：媒体段未声明时为 INHERIT（默认最终回落到 sendrecv） */
typedef enum {
    SDP_DIR_INHERIT = 0,
    SDP_DIR_SENDRECV,
    SDP_DIR_SENDONLY,
    SDP_DIR_RECVONLY,
    SDP_DIR_INACTIVE
} sdp_dir_t;

/* c= 连接数据 */
typedef struct {
    sip_tok_t nettype;  /* IN */
    sip_tok_t addrtype; /* IP4/IP6 */
    sip_tok_t addr;
    sip_tok_t ttl;      /* 多播 TTL，单播为空 */
    int       count;    /* /count，默认 1 */
} sdp_conn_t;

/* m= 的格式项：原文 + 数字 PT（非数字 PT，如 webrtc-datachannel 时为 -1） */
typedef struct {
    sip_tok_t raw;
    int       pt;
} sdp_fmt_t;

/* a= 属性：name[:value]，flag 属性 val.p==NULL */
typedef struct {
    sip_tok_t name;
    sip_tok_t val;
} sdp_attr_t;

/* b= 带宽：<bwtype>:<bandwidth>（AS/CT/TIAS 及 X- 扩展） */
typedef struct {
    sip_tok_t type;
    long      val; /* TIAS 单位为 b/s；AS 单位为 kb/s */
} sdp_bw_t;

/* DTLS-SRTP a=setup（RFC 4145/5763） */
typedef enum {
    DTLS_SETUP_UNKNOWN = 0,
    DTLS_SETUP_ACTIVE,
    DTLS_SETUP_PASSIVE,
    DTLS_SETUP_ACTPASS,
    DTLS_SETUP_HOLDCONN
} dtls_setup_t;

/* RFC 3312 前提条件条目 */
typedef enum {
    SDP_PC_CURR = 1,
    SDP_PC_DES,
    SDP_PC_CONF
} sdp_pc_kind_t;
typedef struct {
    sdp_pc_kind_t kind;
    sip_tok_t ptype;       /* 前提类型 qos */
    sip_tok_t strength;    /* 仅 des：mandatory/optional/none/failure/unknown */
    sip_tok_t status_type; /* e2e/local/remote */
    sip_tok_t direction;   /* none/send/recv/sendrecv */
} sdp_precond_t;

/* 协商后的编解码（每媒体段构建一次） */
typedef struct {
    int       pt;
    sip_tok_t enc;
    sip_tok_t rate;
    sip_tok_t channels;
    sip_tok_t fmtp;
    int       telephone_event;
} sdp_codec_t;

typedef struct {
    sdp_mtype_t type;
    int         port;
    int         port_count;
    sip_tok_t   proto;
    sdp_fmt_t   fmt[SDP_MAX_FMT];
    int         nfmt;
    int         rejected; /* port 0：媒体被拒绝/停用 */

    sdp_conn_t  conn;
    int         has_conn;
    sdp_dir_t   dir;

    int         rtcp_port;
    sdp_conn_t  rtcp_conn;
    int         has_rtcp;

    sip_tok_t   mid;

    sdp_bw_t    bws[8];
    int         nbws;

    sdp_attr_t  attrs[SDP_MAX_ATTRS];
    int         nattrs;

    sdp_codec_t codecs[SDP_MAX_FMT];
    int         ncodecs;
    int         codecs_built;

    sdp_precond_t pcs[12];
    int            npcs;
    int            pc_built;
} sdp_media_t;

typedef struct {
    sdp_conn_t  conn;
    int         has_conn;
    sdp_dir_t   dir;

    sip_tok_t   sess_id;
    sip_tok_t   sess_ver;
    sip_tok_t   origin_addr;

    sdp_bw_t    bws[8];
    int         nbws;

    sdp_attr_t  attrs[SDP_MAX_ATTRS];
    int         nattrs;

    sdp_media_t media[SDP_MAX_MEDIA];
    int         nmedia;

    const char *buf;
    size_t      buflen;
} sdp_t;

/* ---- API ---- */
nsbc_rc_t sdp_parse(sdp_t *s, const char *buf, size_t len);

/* lazy 构建编解码列表，幂等（已构建直接返回） */
nsbc_rc_t sdp_media_build_codecs(sdp_t *s, sdp_media_t *m);

sdp_dir_t  sdp_media_effective_dir(const sdp_t *s, const sdp_media_t *m);
const sdp_conn_t *sdp_media_effective_conn(const sdp_t *s,
                                           const sdp_media_t *m);
int        sdp_media_is_t38(const sdp_media_t *m);

/* 带宽行查找（AS/CT/TIAS/IA/X-*），未找到返回 NULL */
const sdp_bw_t *sdp_bw_find(const sdp_bw_t *b, int n, const char *type);

/* DTLS-SRTP（RFC 5763）：媒体段 setup 优先回落会话级 */
dtls_setup_t sdp_media_dtls_setup(const sdp_t *s, const sdp_media_t *m);
nsbc_rc_t sdp_fingerprint_get(const sdp_t *s, const sdp_media_t *m,
                              sip_tok_t *algo, sip_tok_t *fp);
/* SDES-SRTP（RFC 4568）a=crypto 是否存在 */
int        sdp_media_has_crypto(const sdp_media_t *m);

/* RFC 3312 前提条件：lazy 构建（畸形属性返回 ERR）；
 * active=提出 qos 前提；met=mandatory des 均被 curr 满足；
 * confirmed=应答中存在非 none 的 conf 条目 */
nsbc_rc_t sdp_media_build_preconds(sdp_t *s, sdp_media_t *m);
int        sdp_media_preconditions_active(const sdp_media_t *m);
int        sdp_media_preconditions_met(const sdp_media_t *m);
int        sdp_media_preconditions_confirmed(const sdp_media_t *m);

/* 旧式 hold：c=0.0.0.0（IPv4）/ c=::（IPv6），RFC 3264 前设备 */
int        sdp_media_is_legacy_hold(const sdp_t *s, const sdp_media_t *m);

const sdp_attr_t *sdp_attr_find(const sdp_attr_t *a, int n,
                                const char *name);

/* RFC 3551 静态 PT 回退；命中返回 OK 并填充令牌 */
nsbc_rc_t sdp_static_pt_lookup(int pt, sip_tok_t *enc, sip_tok_t *rate);

#endif /* NSBC_SDP_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
