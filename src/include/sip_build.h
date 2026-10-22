#ifndef NSBC_SIP_BUILD_H
#define NSBC_SIP_BUILD_H

#include "sip_msg.h"
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 出向 SIP 报文构造（有界缓冲，绝不越界；超长返回 NSBC_NOSPC）。
 * 所有拷贝基于零拷贝令牌 {p,len}，不假设 NUL。 */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int overflow;
} sipb_t;

void sipb_init(sipb_t *b, char *buf, size_t cap);
void sipb_cat(sipb_t *b, const char *s);
void sipb_catn(sipb_t *b, const char *s, size_t n);
void sipb_cat_tok(sipb_t *b, sip_tok_t t);
void sipb_catf(sipb_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* 标准原因短语表（RFC3261 + 491/503 等常用）；未知返回 "Unknown" */
const char *sip_reason_phrase(int status);

/* 拷贝原头的完整原始行（含头名与 CRLF）；用于 Via/From/To/Call-ID/CSeq */
void sipb_copy_hdr(sipb_t *b, const sip_msg_t *m, sip_hdr_type_t t);

/* 由入向请求构造应答：
 *   status      状态码（reason 可传 NULL 用标准短语）
 *   to_tag      非空：To 无 tag 时追加（1xx/2xx UAS 首次应答）
 *   contact     非空：追加 Contact 头
 *   extra       非空：已格式化好的附加头块（每个头自带 CRLF）
 *   body/bodylen SDP 等（自动 Content-Type 由 extra 提供）
 */
int sip_build_response(char *buf, size_t cap, const sip_msg_t *req,
                       int status, const char *reason,
                       const char *to_tag, const char *contact,
                       const char *extra, const char *body,
                       size_t bodylen);

/* 构造 Via 头整行（含 CRLF）。branch 为完整分支参数值（含 z9hG4bK） */
int sip_build_via(char *out, size_t outsz, const char *host, int port,
                  const char *transport, const char *branch, int rport);

typedef struct {
    const char *method;
    const char *ruri;
    const char *from;      /* 完整 "Display <sip:...>;tag=.."（tag 可省） */
    const char *to;
    const char *contact;   /* 可 NULL */
    const char *callid;
    long cseq;
    const char *via_host;
    int via_port;
    const char *transport; /* UDP/TCP/TLS */
    const char *branch;
    int max_forwards;      /* <=0 用 70 */
    /* ---- REQ-IMS P-CSCF/S-CSCF 路由链 + 拓扑隐藏 ----
     * via_maddr：Via 头 maddr 参数（IMS 边缘代理写边缘地址；NULL→不写）；
     * route：Route 头链（IMS P-CSCF→S-CSCF→AS 等代理链；NULL→不写）；
     * record_route：Record-Route（B2BUA 作为 border element 必须写；NULL→不写）；
     * supported：Supported 头默认写 "precondition, gruu, path, outbound, sec-agree"，
     *   NULL→写默认值；非 NULL 覆盖（IMS 强制通告扩展能力）。 */
    const char *via_maddr;
    const char *route;
    const char *record_route;
    /* ---- REQ-IMS 扩展能力通告 ----
     * supported：Supported 头默认 "replaces, precondition, gruu, path, outbound, sec-agree, answermode, eventlist"，
     *   NULL→写默认值；非 NULL 覆盖（IMS 强制 replaces/answermode 等能力通告）。
     * allow：Allow 头默认列出 B2BUA 支持的全部方法
     *   "INVITE, ACK, CANCEL, OPTIONS, BYE, REFER, NOTIFY, MESSAGE, REGISTER, SUBSCRIBE, INFO, PUBLISH"，
     *   NULL→写默认值；非 NULL 覆盖（运营商 IMS 网要求 Allow 头）。
     * allow_events：Allow-Events 头默认 "hold, talk, conference, dialog, park-info"，
     *   NULL→写默认值；非 NULL 覆盖（IMS 场景 hold/resume NOTIFY 需要）。 */
    const char *supported;
    const char *allow;
    const char *allow_events;
    /* ---- topo_hide 接线 ----
     * user_agent：sip_build_request 的 User-Agent 头覆盖；NULL → 默认 "NSBC/2.7"；
     * 由 topo_hide 模块调用 topo_user_agent() 生成后装配（零编译风险透传）。 */
    const char *user_agent; /* 可 NULL → 默认 "NSBC/2.7"；非 NULL 覆盖（topo_hide 接线） */
    const char *extra;     /* 额外头块（自带 CRLF），可 NULL */
    const char *body;
    size_t bodylen;
    const char *content_type; /* body 非空时必填 */
} sip_req_spec_t;

int sip_build_request(char *buf, size_t cap, const sip_req_spec_t *s);

/* CSeq 自增串 */
int sip_cseq_line(char *out, size_t outsz, long n, const char *method);

#ifdef __cplusplus
}
#endif
#endif
