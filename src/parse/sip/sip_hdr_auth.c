/* RFC 2617/7616 Digest 深解析：
 * scheme 1*SP name=val *("," SP name=val)
 * 值为 quoted-string 或 token；逗号在引号外才是分隔符
 * 同时覆盖 Authorization/Proxy-Authorization（凭证）与
 * WWW/Proxy-Authenticate（质询） */

#include "sip_common.h"

/* 逗号分隔的 auth 参数 */
static nsbc_rc_t parse_auth_params(sip_param_t *out, int cap,
                                   const char *p, const char *end, int *n_out)
{
    int n = 0;
    while (p < end) {
        p = sip_skip_lws_run(p, end);
        if (p >= end)
            break;
        if (n >= cap)
            return NSBC_ERR;

        const char *name = p;
        while (p < end && sip_is_tokchar((unsigned char)*p))
            ++p;
        if (p == name || p >= end || *p != '=')
            return NSBC_ERR;
        out[n].name.p = name;
        out[n].name.len = (size_t)(p - name);
        ++p; /* 跳过 '=' */

        if (p < end && *p == '"') {
            const char *v = ++p;
            while (p < end && *p != '"')
                ++p;
            if (p >= end)
                return NSBC_ERR;
            out[n].val.p = v;
            out[n].val.len = (size_t)(p - v);
            ++p; /* 跳过闭合引号 */
        } else {
            const char *v = p;
            while (p < end && sip_is_tokchar((unsigned char)*p))
                ++p;
            if (p == v)
                return NSBC_ERR;
            out[n].val.p = v;
            out[n].val.len = (size_t)(p - v);
        }

        p = sip_skip_lws_run(p, end);
        if (p < end) {
            if (*p != ',')
                return NSBC_ERR;
            ++p;
        }
        ++n;
    }
    *n_out = n;
    return NSBC_OK;
}

nsbc_rc_t sip_hdr_parse_auth(sip_hdr_t *h)
{
    if (h->parsed)
        return NSBC_OK;
    if (h->type != SIP_H_AUTHORIZATION &&
        h->type != SIP_H_PROXY_AUTHORIZATION &&
        h->type != SIP_H_WWW_AUTHENTICATE &&
        h->type != SIP_H_PROXY_AUTHENTICATE)
        return NSBC_INVAL;

    const char *p = h->value.p;
    const char *end = p + h->value.len;
    sip_auth_t *a = &h->u.auth;
    memset(a, 0, sizeof(*a));

    const char *sp = p;
    while (sp < end && !sip_is_sp(*sp))
        ++sp;
    if (sp == p)
        return NSBC_ERR;
    a->scheme.p = p;
    a->scheme.len = (size_t)(sp - p);
    for (size_t i = 0; i < a->scheme.len; ++i)
        if (!sip_is_tokchar((unsigned char)a->scheme.p[i]))
            return NSBC_ERR;

    const char *pb = sip_skip_lws_run(sp, end);
    if (pb < end &&
        parse_auth_params(a->params, SIP_HDR_MAX_PRM, pb, end,
                          &a->nparams) != NSBC_OK)
        return NSBC_ERR;

    /* scheme 后必须有参数（Digest 至少含 realm 等） */
    if (pb < end && a->nparams == 0)
        return NSBC_ERR;

    h->parsed = 1;
    return NSBC_OK;
}