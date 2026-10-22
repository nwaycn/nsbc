/* URI 令牌化：scheme: [// [user[:passwd]@] host[:port]] [;params] [?headers] */

#include "sip_common.h"

/* 解析 host[:port]，支持 [IPv6] */
static nsbc_rc_t parse_hostport(sip_uri_t *u, const char *p, const char *end)
{
    if (p == end)
        return NSBC_ERR;
    if (*p == '[') {
        const char *q = p + 1;
        while (q < end && *q != ']')
            ++q;
        if (q >= end)
            return NSBC_ERR;
        u->host.p = p;
        u->host.len = (size_t)(q + 1 - p);
        p = q + 1;
        if (p < end) {
            if (*p != ':')
                return NSBC_ERR;
            ++p;
            u->port.p = p;
            u->port.len = (size_t)(end - p);
        }
    } else {
        const char *c = p;
        while (c < end && *c != ':')
            ++c;
        u->host.p = p;
        u->host.len = (size_t)(c - p);
        if (u->host.len == 0)
            return NSBC_ERR;
        if (c < end) {
            ++c;
            u->port.p = c;
            u->port.len = (size_t)(end - c);
        }
    }
    if (u->port.len) {
        long pv;
        if (!sip_parse_ulong(u->port.p, u->port.len, &pv))
            return NSBC_ERR;
    }
    return NSBC_OK;
}

nsbc_rc_t sip_parse_uri(const sip_tok_t *t, sip_uri_t *u)
{
    const char *p = t->p;
    const char *end = p + t->len;
    memset(u, 0, sizeof(*u));

    const char *colon = p;
    while (colon < end && *colon != ':')
        ++colon;
    if (colon == end || colon == p)
        return NSBC_ERR;
    u->scheme.p = p;
    u->scheme.len = (size_t)(colon - p);
    for (size_t i = 0; i < u->scheme.len; ++i)
        if (!sip_is_tokchar((unsigned char)u->scheme.p[i]))
            return NSBC_ERR;
    p = colon + 1;

    /* 定位参数段与 headers 段 */
    const char *semi = end, *question = end;
    for (const char *c = p; c < end; ++c) {
        if (*c == ';' && semi == end)
            semi = c;
        else if (*c == '?' && question == end)
            question = c;
    }
    const char *auth_end = semi < question ? semi : question;

    if (p + 1 < end && p[0] == '/' && p[1] == '/')
        p += 2; /* 容忍非标准 sip:// 写法；标准 SIP URI 无 // */

    /* userinfo（取第一个 '@'） */
    const char *at = NULL;
    for (const char *c = p; c < auth_end; ++c)
        if (*c == '@') {
            at = c;
            break;
        }
    if (at) {
        const char *uc = p;
        while (uc < at && *uc != ':')
            ++uc;
        u->user.p = p;
        u->user.len = (size_t)(uc - p);
        if (u->user.len == 0)
            return NSBC_ERR;
        if (uc < at) {
            u->passwd.p = uc + 1;
            u->passwd.len = (size_t)(at - uc - 1);
        }
        p = at + 1;
    }

    if (parse_hostport(u, p, auth_end) != NSBC_OK)
        return NSBC_ERR;

    /* ;params（到 ? 为止） */
    if (semi < question) {
        const char *pp = semi + 1;
        if (sip_parse_params(u->params, SIP_URI_MAX_PRM, pp, question,
                             &u->nparams) != NSBC_OK)
            return NSBC_ERR;
    }

    if (question < end) {
        u->headers.p = question + 1;
        u->headers.len = (size_t)(end - question - 1);
    }
    return NSBC_OK;
}