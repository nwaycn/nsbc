/* Via 深解析： SIP/2.0/UDP host[:port] ;branch=..;rport[=..];received=.. */

#include "sip_common.h"

nsbc_rc_t sip_hdr_parse_via(sip_hdr_t *h)
{
    if (h->parsed)
        return NSBC_OK;
    if (h->type != SIP_H_VIA)
        return NSBC_INVAL;

    const char *p = h->value.p;
    const char *end = p + h->value.len;
    sip_via_t *v = &h->u.via;
    memset(v, 0, sizeof(*v));

    const char *sl1 = NULL, *sl2 = NULL;
    for (const char *c = p; c < end; ++c) {
        if (*c == '/') {
            if (!sl1)
                sl1 = c;
            else if (!sl2) {
                sl2 = c;
                break;
            }
        }
    }
    if (!sl1 || !sl2 || sl1 == p)
        return NSBC_ERR;
    v->proto.p = p;
    v->proto.len = (size_t)(sl1 - p);
    v->ver.p = sl1 + 1;
    v->ver.len = (size_t)(sl2 - sl1 - 1);

    const char *q = sl2 + 1;
    while (q < end && !sip_is_sp(*q))
        ++q;
    if (q == sl2 + 1 || q >= end)
        return NSBC_ERR;
    v->transport.p = sl2 + 1;
    v->transport.len = (size_t)(q - sl2 - 1);

    q = sip_skip_lws_run(q, end);
    const char *semi = end;
    for (const char *c = q; c < end; ++c)
        if (*c == ';') {
            semi = c;
            break;
        }

    /* host[:port] */
    const char *hp = q;
    if (hp < semi && *hp == '[') {
        const char *cb = hp + 1;
        while (cb < semi && *cb != ']')
            ++cb;
        if (cb >= semi)
            return NSBC_ERR;
        v->host.p = hp;
        v->host.len = (size_t)(cb + 1 - hp);
        hp = cb + 1;
        if (hp < semi) {
            if (*hp != ':')
                return NSBC_ERR;
            v->port.p = hp + 1;
            v->port.len = (size_t)(semi - hp - 1);
        }
    } else {
        const char *cp = hp;
        while (cp < semi && *cp != ':')
            ++cp;
        v->host.p = hp;
        v->host.len = (size_t)(cp - hp);
        if (v->host.len == 0)
            return NSBC_ERR;
        if (cp < semi) {
            v->port.p = cp + 1;
            v->port.len = (size_t)(semi - cp - 1);
        }
    }
    if (v->port.len) {
        for (size_t i = 0; i < v->port.len; ++i)
            if (!sip_is_digit(v->port.p[i]))
                return NSBC_ERR;
    }

    sip_param_t prm[SIP_HDR_MAX_PRM];
    int nprm = 0;
    if (semi < end &&
        sip_parse_params(prm, SIP_HDR_MAX_PRM, semi + 1, end, &nprm) !=
            NSBC_OK)
        return NSBC_ERR;

    const sip_param_t *b = sip_param_find(prm, nprm, "branch");
    if (b)
        v->branch = b->val;
    const sip_param_t *rp = sip_param_find(prm, nprm, "rport");
    if (rp) {
        /* p!=NULL 表示存在；val 有值时指向值，否则 len==0 无值形式 */
        v->rport.p = rp->val.p ? rp->val.p : rp->name.p;
        v->rport.len = rp->val.len;
    }
    const sip_param_t *rc = sip_param_find(prm, nprm, "received");
    if (rc)
        v->received = rc->val;

    h->parsed = 1;
    return NSBC_OK;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
