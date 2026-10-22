#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sip_build.h"
#include <stdio.h>
#include <string.h>

void sipb_init(sipb_t *b, char *buf, size_t cap)
{
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->overflow = 0;
}

void sipb_catn(sipb_t *b, const char *s, size_t n)
{
    if (b->overflow)
        return;
    if (b->len + n > b->cap) {
        b->overflow = 1;
        return;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}
void sipb_cat(sipb_t *b, const char *s)
{
    sipb_catn(b, s, strlen(s));
}
void sipb_cat_tok(sipb_t *b, sip_tok_t t)
{
    if (t.p && t.len)
        sipb_catn(b, t.p, t.len);
}
void sipb_catf(sipb_t *b, const char *fmt, ...)
{
    if (b->overflow)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->overflow = 1;
        return;
    }
    if ((size_t)n >= b->cap - b->len) {
        b->overflow = 1;
        return;
    }
    b->len += (size_t)n;
}

const char *sip_reason_phrase(int s)
{
    switch (s) {
    case 100: return "Trying";
    case 180: return "Ringing";
    case 181: return "Call Is Being Forwarded";
    case 182: return "Queued";
    case 183: return "Session Progress";
    case 200: return "OK";
    case 202: return "Accepted";
    case 302: return "Moved Temporarily";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 410: return "Gone";
    case 413: return "Request Entity Too Large";
    case 415: return "Unsupported Media Type";
    case 416: return "Unsupported URI Scheme";
    case 420: return "Bad Extension";
    case 421: return "Extension Required";
    case 422: return "Session Interval Too Small";
    case 423: return "Interval Too Brief";
    case 480: return "Temporarily Unavailable";
    case 481: return "Call/Transaction Does Not Exist";
    case 482: return "Loop Detected";
    case 483: return "Too Many Hops";
    case 484: return "Address Incomplete";
    case 485: return "Ambiguous";
    case 486: return "Busy Here";
    case 487: return "Request Terminated";
    case 488: return "Not Acceptable Here";
    case 491: return "Request Pending";
    case 493: return "Undecipherable";
    case 500: return "Server Internal Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Server Time-out";
    case 505: return "Version Not Supported";
    case 600: return "Busy Everywhere";
    case 603: return "Decline";
    case 604: return "Does Not Exist Anywhere";
    case 606: return "Not Acceptable";
    default:  return "Unknown";
    }
}

void sipb_copy_hdr(sipb_t *b, const sip_msg_t *m, sip_hdr_type_t t)
{
    for (sip_hdr_t *h = sip_hdr_get(m, t); h; h = sip_hdr_next(m, h)) {
        /* 解析器 name 令牌不含冒号：name + ": " + value + CRLF */
        sipb_cat_tok(b, h->name);
        sipb_cat(b, ": ");
        sipb_cat_tok(b, h->value);
        sipb_cat(b, "\r\n");
    }
}

/* 从消息头块复制头名到 value 的完整行；To 需要时补 tag */
static void copy_to_with_tag(sipb_t *b, const sip_msg_t *m,
                             const char *to_tag)
{
    sip_hdr_t *h = sip_hdr_get(m, SIP_H_TO);
    if (!h)
        return;
    sipb_cat_tok(b, h->name);
    sipb_cat(b, ": ");
    sipb_cat_tok(b, h->value);
    if (to_tag && *to_tag) {
        sip_hdr_parse_addr(h);
        if (h->u.addr.tag.p == NULL)
            sipb_catf(b, ";tag=%s", to_tag);
    }
    sipb_cat(b, "\r\n");
}

int sip_build_response(char *buf, size_t cap, const sip_msg_t *req,
                       int status, const char *reason,
                       const char *to_tag, const char *contact,
                       const char *extra, const char *body, size_t bodylen)
{
    sipb_t b;
    sipb_init(&b, buf, cap);
    if (!reason)
        reason = sip_reason_phrase(status);
    sipb_catf(&b, "SIP/2.0 %d %s\r\n", status, reason);
    sipb_copy_hdr(&b, req, SIP_H_VIA);
    sipb_copy_hdr(&b, req, SIP_H_FROM);
    copy_to_with_tag(&b, req, to_tag);
    sipb_copy_hdr(&b, req, SIP_H_CALL_ID);
    sipb_copy_hdr(&b, req, SIP_H_CSEQ);
    if (contact)
        sipb_catf(&b, "Contact: %s\r\n", contact);
    /* User-Agent：sip_build_response 没有 sip_req_spec_t 参数，
     * 所以直接写默认值（topo_hide egress 层会覆盖 response 的 UA 头，
     * 这里只作为 response 构建时的默认值）。 */
    sipb_cat(&b, "User-Agent: NSBC/2.7\r\n");
    if (extra)
        sipb_cat(&b, extra);
    if (body && bodylen)
        sipb_catf(&b, "Content-Length: %u\r\n", (unsigned)bodylen);
    else
        sipb_cat(&b, "Content-Length: 0\r\n");
    sipb_cat(&b, "\r\n");
    if (body && bodylen)
        sipb_catn(&b, body, bodylen);
    return b.overflow ? NSBC_NOSPC : (int)b.len;
}

int sip_build_via(char *out, size_t outsz, const char *host, int port,
                  const char *transport, const char *branch, int rport)
{
    int n = snprintf(out, outsz,
                     "Via: SIP/2.0/%s %s:%d;branch=%s%s\r\n",
                     transport ? transport : "UDP", host, port, branch,
                     rport ? ";rport" : "");
    return (n > 0 && (size_t)n < outsz) ? n : NSBC_NOSPC;
}

int sip_cseq_line(char *out, size_t outsz, long n, const char *method)
{
    int r = snprintf(out, outsz, "CSeq: %ld %s\r\n", n, method);
    return (r > 0 && (size_t)r < outsz) ? r : NSBC_NOSPC;
}

int sip_build_request(char *buf, size_t cap, const sip_req_spec_t *s)
{
    sipb_t b;
    sipb_init(&b, buf, cap);
    sipb_catf(&b, "%s %s SIP/2.0\r\n", s->method, s->ruri);
    /* Via：RFC 3261 §20.42；IMS 边缘代理加 maddr 参数 */
    if (s->via_maddr && s->via_maddr[0]) {
        sipb_catf(&b, "Via: SIP/2.0/%s %s:%d;branch=%s;rport;maddr=%s\r\n",
                  s->transport ? s->transport : "UDP", s->via_host, s->via_port,
                  s->branch, s->via_maddr);
    } else {
        sipb_catf(&b, "Via: SIP/2.0/%s %s:%d;branch=%s;rport\r\n",
                  s->transport ? s->transport : "UDP", s->via_host, s->via_port,
                  s->branch);
    }
    sipb_catf(&b, "Max-Forwards: %d\r\n",
              s->max_forwards > 0 ? s->max_forwards : 70);
    /* ---- REQ-IMS：Route/Record-Route 在 Max-Forwards 之后、From 之前 ----
     * RFC 3261 §16.4：Route 头链在 Route 集合处理时消费；
     * RFC 3261 §16.6：Record-Route 配对 Route，让后续请求沿相同路径；
     * IMS P-CSCF→S-CSCF→AS 路由链必须由 B2BUA 作为 border element 写。
     * NULL 表示本呼叫不需要（域内分机直连），不写头。 */
    if (s->route && s->route[0])
        sipb_catf(&b, "Route: %s\r\n", s->route);
    if (s->record_route && s->record_route[0])
        sipb_catf(&b, "Record-Route: %s\r\n", s->record_route);
    /* ---- REQ-IMS：Supported 默认头通告扩展能力 ----
     * RFC 3261 §20.37 + IMS 强制 precondition/gruu/path/outbound/sec-agree；
     * replaces 是运营商呼叫转移 REFER 必需能力；answermode/eventlist 补充 IMS 兼容。
     * NULL→写默认值（IMS 兼容基础），非 NULL→覆盖（特殊场景只通告部分）。 */
    if (s->supported && s->supported[0]) {
        sipb_catf(&b, "Supported: %s\r\n", s->supported);
    } else {
        sipb_cat(&b, "Supported: replaces, precondition, gruu, path, outbound, sec-agree, answermode, eventlist\r\n");
    }
    /* ---- REQ-IMS：Allow 头通告本 B2BUA 支持的方法 ----
     * RFC 3261 §20.5 — 运营商 IMS/SSF 网关要求 Allow 头做兼容性判断；
     * B2BUA 作为边界 element 声明两端各自的方法集，这里列全常见方法。
     * 默认仅 INVITE 方法写 Allow（REGISTER/OPTIONS/BYE 等非 INVITE 不默认带），
     * 若调用方显式设 spec.allow 非 NULL 则无论什么方法都写（覆盖）。 */
    if (s->allow && s->allow[0]) {
        sipb_catf(&b, "Allow: %s\r\n", s->allow);
    } else if (strcmp(s->method, "INVITE") == 0) {
        sipb_cat(&b, "Allow: INVITE, ACK, CANCEL, OPTIONS, BYE, REFER, NOTIFY, MESSAGE, REGISTER, SUBSCRIBE, INFO, PUBLISH\r\n");
    }
    /* ---- REQ-IMS：Allow-Events 头 ----
     * RFC 3265 §4.4 — 告诉对端本端支持哪些 SIP Event Packages；
     * hold/resume NOTIFY（RFC 3264）、conference（RFC 4575）、dialog（RFC 4235）等。
     * 默认仅 INVITE 方法写 Allow-Events（REGISTER 等非 INVITE 不默认带），
     * 若调用方显式设 spec.allow_events 非 NULL 则无论什么方法都写（覆盖）。 */
    if (s->allow_events && s->allow_events[0]) {
        sipb_catf(&b, "Allow-Events: %s\r\n", s->allow_events);
    } else if (strcmp(s->method, "INVITE") == 0) {
        sipb_cat(&b, "Allow-Events: hold, talk, conference, dialog, park-info\r\n");
    }
    sipb_catf(&b, "From: %s\r\n", s->from);
    sipb_catf(&b, "To: %s\r\n", s->to);
    sipb_catf(&b, "Call-ID: %s\r\n", s->callid);
    sipb_catf(&b, "CSeq: %ld %s\r\n", s->cseq, s->method);
    if (s->contact)
        sipb_catf(&b, "Contact: %s\r\n", s->contact);
    /* topo_hide 接线：user_agent 可 NULL→默认 "NSBC/2.7" */
    if (s->user_agent)
        sipb_catf(&b, "User-Agent: %s\r\n", s->user_agent);
    else
        sipb_cat(&b, "User-Agent: NSBC/2.7\r\n");
    if (s->extra)
        sipb_cat(&b, s->extra);
    if (s->body && s->bodylen) {
        sipb_catf(&b, "Content-Type: %s\r\n",
                  s->content_type ? s->content_type : "application/sdp");
        sipb_catf(&b, "Content-Length: %u\r\n", (unsigned)s->bodylen);
    } else {
        sipb_cat(&b, "Content-Length: 0\r\n");
    }
    sipb_cat(&b, "\r\n");
    if (s->body && s->bodylen)
        sipb_catn(&b, s->body, s->bodylen);
    return b.overflow ? NSBC_NOSPC : (int)b.len;
}
