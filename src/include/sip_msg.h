#ifndef NSBC_SIP_MSG_H
#define NSBC_SIP_MSG_H

#include "nsbc_common.h"

/* ---- 硬限制（对应 REQ-SIP-005；真实版本可配，PoC 取固定值） ---- */
#define SIP_MAX_MSG      16384 /* 单条报文上限 16KB */
#define SIP_MAX_HDRS     32    /* 单条报文头数量上限 */
#define SIP_MAX_LINE     8192  /* 单行（含折叠展开）上限 */
#define SIP_URI_MAX_PRM  16    /* URI 参数上限 */
#define SIP_HDR_MAX_PRM  16    /* 头值参数上限 */

/* ---- 零拷贝令牌：始终配长度，不依赖 NUL ---- */
typedef struct {
    const char *p;
    size_t      len;
} sip_tok_t;

/* ---- 方法枚举 ---- */
typedef enum {
    SIP_M_UNKNOWN = 0,
    SIP_M_INVITE,
    SIP_M_ACK,
    SIP_M_OPTIONS,
    SIP_M_BYE,
    SIP_M_CANCEL,
    SIP_M_REGISTER,
    SIP_M_PRACK,
    SIP_M_SUBSCRIBE,
    SIP_M_NOTIFY,
    SIP_M_PUBLISH,
    SIP_M_INFO,
    SIP_M_REFER,
    SIP_M_MESSAGE,
    SIP_M_UPDATE
} sip_method_t;

/* ---- 头类型枚举（直达索引槽） ---- */
typedef enum {
    SIP_H_UNKNOWN = 0,
    SIP_H_ACCEPT,
    SIP_H_ACCEPT_CONTACT,
    SIP_H_ACCEPT_ENCODING,
    SIP_H_ACCEPT_LANGUAGE,
    SIP_H_ALLOW,
    SIP_H_ALLOW_EVENTS,
    SIP_H_AUTHORIZATION,
    SIP_H_CALL_ID,
    SIP_H_CONTACT,
    SIP_H_CONTENT_DISPOSITION,
    SIP_H_CONTENT_ENCODING,
    SIP_H_CONTENT_LANGUAGE,
    SIP_H_CONTENT_LENGTH,
    SIP_H_CONTENT_TYPE,
    SIP_H_CSEQ,
    SIP_H_DATE,
    SIP_H_EVENT,
    SIP_H_EXPIRES,
    SIP_H_FROM,
    SIP_H_MAX_FORWARDS,
    SIP_H_MIN_EXPIRES,
    SIP_H_MIN_SE,
    SIP_H_ORGANIZATION,
    SIP_H_P_ASSERTED_IDENTITY,
    SIP_H_P_PREFERRED_IDENTITY,
    SIP_H_P_CALLED_PARTY_ID,
    SIP_H_P_ACCESS_NETWORK_INFO,
    SIP_H_P_EARLY_MEDIA,
    SIP_H_P_CHARGING_FUNCTION_ADDRESSES,
    SIP_H_P_CHARGING_VECTOR,
    SIP_H_PATH,
    SIP_H_PRIORITY,
    SIP_H_PRIVACY,
    SIP_H_PROXY_AUTHENTICATE,
    SIP_H_PROXY_AUTHORIZATION,
    SIP_H_PROXY_REQUIRE,
    SIP_H_REASON,
    SIP_H_RECORD_ROUTE,
    SIP_H_REFER_TO,
    SIP_H_REFERRED_BY,
    SIP_H_REPLACES,
    SIP_H_REPLY_TO,
    SIP_H_REQUIRE,
    SIP_H_RESOURCE_PRIORITY,
    SIP_H_RETRY_AFTER,
    SIP_H_ROUTE,
    SIP_H_RSEQ,
    SIP_H_SERVER,
    SIP_H_SERVICE_ROUTE,
    SIP_H_SESSION_EXPIRES,
    SIP_H_SUBJECT,
    SIP_H_SUBSCRIPTION_STATE,
    SIP_H_SUPPORTED,
    SIP_H_TIMESTAMP,
    SIP_H_TO,
    SIP_H_UNSUPPORTED,
    SIP_H_USER_AGENT,
    SIP_H_VIA,
    SIP_H_WARNING,
    SIP_H_WWW_AUTHENTICATE,
    SIP_H__COUNT
} sip_hdr_type_t;

/* ---- 参数（name=value 或裸 name） ---- */
typedef struct {
    sip_tok_t name;
    sip_tok_t val; /* val.p==NULL 表示无值参数 */
} sip_param_t;

/* ---- URI（sip:/sips:/通用形式） ---- */
typedef struct {
    sip_tok_t    scheme;
    sip_tok_t    user;
    sip_tok_t    passwd;
    sip_tok_t    host;
    sip_tok_t    port;
    sip_param_t  params[SIP_URI_MAX_PRM];
    int          nparams;
    sip_tok_t    headers; /* ? 之后 */
} sip_uri_t;

/* ---- Via 深解析结果 ---- */
typedef struct {
    sip_tok_t proto; /* SIP */
    sip_tok_t ver;   /* 2.0 */
    sip_tok_t transport;
    sip_tok_t host;
    sip_tok_t port;
    sip_tok_t branch;
    sip_tok_t rport;    /* p!=NULL 且 len==0：rport 无值形式 */
    sip_tok_t received;
} sip_via_t;

/* ---- From/To/Contact 等地址类深解析 ---- */
typedef struct {
    sip_tok_t display;
    sip_uri_t uri;
    sip_tok_t tag;
    int       star; /* Contact: * */
} sip_addr_t;

/* ---- Route/Record-Route 深解析 ---- */
typedef struct {
    sip_uri_t uri;
} sip_route_t;

/* ---- CSeq 深解析 ---- */
typedef struct {
    long         number;
    sip_method_t method;
    sip_tok_t    method_str;
} sip_cseq_t;

/* ---- Digest 凭证/质询通用形式：scheme + 参数集 ---- */
typedef struct {
    sip_tok_t   scheme;
    sip_param_t params[SIP_HDR_MAX_PRM];
    int         nparams;
} sip_auth_t;

/* ---- 单个头 ---- */
typedef struct sip_hdr {
    sip_hdr_type_t type;
    sip_tok_t      name;  /* 原始头名 */
    sip_tok_t      value; /* 值（折叠时含跨行原始跨度） */
    int            next_same;
    int            parsed; /* 深解析完成标志 */
    union {
        sip_via_t   via;
        sip_addr_t  addr;
        sip_route_t route;
        sip_cseq_t  cseq;
        sip_auth_t  auth;
        long        maxfwd;
    } u;
} sip_hdr_t;

/* ---- 解析 flags（默认严格；兼容项按需开启） ---- */
#define SIP_F_COMPAT_NAME_SPACE 0x01 /* 容忍 "Via :" 头名与冒号间空格 */
#define SIP_F_COMPAT_TEL_URI    0x02 /* 容忍 R-URI 为 tel:（RFC 3966） */
#define SIP_F_COMPAT_ALL        0x03

/* ---- 消息对象 ---- */
typedef struct {
    int           is_request;
    unsigned      flags;
    sip_method_t  method;
    sip_tok_t     method_str; /* 未知方法时保留原文 */
    sip_tok_t     ruri_raw;
    sip_uri_t     ruri;
    int           ruri_parsed;
    int           status;
    sip_tok_t     reason;

    sip_hdr_t     hdrs[SIP_MAX_HDRS];
    int           nhdrs;
    sip_hdr_t     *by_type[SIP_H__COUNT];

    sip_tok_t     body;
    int           cl_seen;
    long          content_length;

    const char    *buf;
    size_t         buflen;
} sip_msg_t;

/* ---- 公共 API ---- */
nsbc_rc_t sip_parse(sip_msg_t *m, const char *buf, size_t len);
nsbc_rc_t sip_parse_ex(sip_msg_t *m, const char *buf, size_t len,
                       unsigned flags);

sip_hdr_t *sip_hdr_get(const sip_msg_t *m, sip_hdr_type_t type);
sip_hdr_t *sip_hdr_next(const sip_msg_t *m, const sip_hdr_t *h);

nsbc_rc_t sip_parse_uri(const sip_tok_t *t, sip_uri_t *u);
const sip_param_t *sip_param_find(const sip_param_t *ps, int n,
                                  const char *name);

nsbc_rc_t sip_hdr_parse_via(sip_hdr_t *h);
nsbc_rc_t sip_hdr_parse_addr(sip_hdr_t *h);
nsbc_rc_t sip_hdr_parse_route(sip_hdr_t *h);
nsbc_rc_t sip_hdr_parse_cseq(sip_hdr_t *h);
nsbc_rc_t sip_hdr_parse_maxfwd(sip_hdr_t *h);
nsbc_rc_t sip_hdr_parse_auth(sip_hdr_t *h);

/* Require/Supported/Proxy-Require 等逗号分隔选项标签中是否含 tag
 * （选项标签大小写敏感；折叠行内 CR/LF 按 LWS 处理） */
int sip_hdr_has_option(const sip_hdr_t *h, const char *tag);

/* 是否处于对话内：From/To 原始值中均带非空 tag（不触发深解析） */
int sip_msg_in_dialog(const sip_msg_t *m);

/* ---- 会话更新（re-INVITE/UPDATE）glare 策略（RFC 3264/6141） ---- */
typedef struct {
    int pending; /* 本侧存在未决会话更新 Offer */
} sip_upd_guard_t;
void sip_upd_guard_init(sip_upd_guard_t *g);
/* 开始一次更新：1 成功；0 与未决更新冲突（应回 491 Request Pending） */
int  sip_upd_begin(sip_upd_guard_t *g);
void sip_upd_end(sip_upd_guard_t *g);
/* glare 退避：Call-ID 较小方等 0~1s，另一方等 2.1~4s；
 * r 为注入随机值（0..0xFFFF），返回毫秒，便于无系统依赖测试 */
long sip_glare_backoff_ms(int local_callid_lower, unsigned r);

/* 头名（含 compact）查表；方法名查表（O(1) perfect hash） */
sip_hdr_type_t sip_hdr_type_lookup(const char *name, size_t len);
sip_method_t sip_method_lookup(const char *p, size_t len);

#endif /* NSBC_SIP_MSG_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
