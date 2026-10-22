/* From/To/Contact/Refer-To 等地址类深解析：
 * ["display"] <uri>[;params]   或   uri[;params]   或   * */

#include "sip_common.h"

/* 找 '<'，跳过引号串内部 */
static const char *find_lt(const char *p, const char *end)
{
    int q = 0;
    while (p < end) {
        if (*p == '"')
            q = !q;
        else if (*p == '<' && !q)
            return p;
        ++p;
    }
    return NULL;
}

nsbc_rc_t sip_hdr_parse_addr(sip_hdr_t *h)
{
    if (h->parsed)
        return NSBC_OK;
    if (h->type != SIP_H_FROM && h->type != SIP_H_TO &&
        h->type != SIP_H_CONTACT && h->type != SIP_H_REFER_TO &&
        h->type != SIP_H_REPLY_TO && h->type != SIP_H_P_ASSERTED_IDENTITY &&
        h->type != SIP_H_P_PREFERRED_IDENTITY)
        return NSBC_INVAL;

    const char *p = h->value.p;
    const char *end = p + h->value.len;
    sip_addr_t *a = &h->u.addr;
    memset(a, 0, sizeof(*a));

    if (end - p == 1 && *p == '*') {
        a->star = 1;
        h->parsed = 1;
        return NSBC_OK;
    }

    const char *lt = find_lt(p, end);
    const char *uri_b, *uri_e, *prm_b;

    if (lt) {
        const char *gt = lt + 1;
        while (gt < end && *gt != '>')
            ++gt;
        if (gt >= end)
            return NSBC_ERR;
        a->display.p = p;
        a->display.len = (size_t)(sip_rtrim(p, lt) - p);
        uri_b = lt + 1;
        uri_e = gt;
        prm_b = gt + 1;
    } else {
        const char *semi = end;
        for (const char *c = p; c < end; ++c)
            if (*c == ';') {
                semi = c;
                break;
            }
        uri_b = p;
        uri_e = semi;
        prm_b = semi;
    }
    if (uri_e == uri_b)
        return NSBC_ERR;

    sip_tok_t ut;
    ut.p = uri_b;
    ut.len = (size_t)(uri_e - uri_b);
    if (sip_parse_uri(&ut, &a->uri) != NSBC_OK)
        return NSBC_ERR;

    if (prm_b < end) {
        sip_param_t prm[SIP_HDR_MAX_PRM];
        int nprm = 0;
        /* sip_parse_params 期望输入以第一个参数的 name 开头，不含前导 ';' */
        while (prm_b < end && (*prm_b == ';' || *prm_b == ' ' || *prm_b == '\t'))
            ++prm_b;
        if (sip_parse_params(prm, SIP_HDR_MAX_PRM, prm_b, end, &nprm) != NSBC_OK)
            return NSBC_ERR;
        const sip_param_t *t = sip_param_find(prm, nprm, "tag");
        if (t)
            a->tag = t->val;
    }

    h->parsed = 1;
    return NSBC_OK;
}
