/* Route/Record-Route 深解析： ["display"] <uri>;params 或 uri;params
 * PoC：仅解析第一个逗号分隔段（真实栈按 name-addr 列表逐段解析） */

#include "sip_common.h"

nsbc_rc_t sip_hdr_parse_route(sip_hdr_t *h)
{
    if (h->parsed)
        return NSBC_OK;
    if (h->type != SIP_H_ROUTE && h->type != SIP_H_RECORD_ROUTE)
        return NSBC_INVAL;

    const char *p = h->value.p;
    const char *end = p + h->value.len;
    sip_route_t *r = &h->u.route;
    memset(r, 0, sizeof(*r));

    /* 取第一段（逗号不在 <> 内的粗略处理：PoC 语料均为单段尖括号形式） */
    const char *seg_end = end;
    int in = 0;
    for (const char *c = p; c < end; ++c) {
        if (*c == '<')
            in = 1;
        else if (*c == '>')
            in = 0;
        else if (*c == ',' && !in) {
            seg_end = c;
            break;
        }
    }

    const char *lt = NULL, *gt = NULL;
    for (const char *c = p; c < seg_end; ++c) {
        if (*c == '<' && !lt)
            lt = c;
        else if (*c == '>' && lt) {
            gt = c;
            break;
        }
    }
    const char *ub, *ue;
    if (lt && gt) {
        ub = lt + 1;
        ue = gt;
    } else {
        const char *semi = seg_end;
        for (const char *c = p; c < seg_end; ++c)
            if (*c == ';') {
                semi = c;
                break;
            }
        ub = p;
        ue = semi;
    }
    if (ue == ub)
        return NSBC_ERR;
    sip_tok_t ut;
    ut.p = ub;
    ut.len = (size_t)(ue - ub);
    if (sip_parse_uri(&ut, &r->uri) != NSBC_OK)
        return NSBC_ERR;

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
