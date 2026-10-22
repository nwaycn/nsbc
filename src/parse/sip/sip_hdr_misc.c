/* CSeq 与 Max-Forwards 深解析（严格校验取值范围，畸形拒绝）；
 * 另含选项标签查找、对话内判定、glare 退避策略 */

#include "sip_common.h"

/* CSeq：DIGIT+ SP METHOD；序号 1..10 位且 <= 2^31-1 */
nsbc_rc_t sip_hdr_parse_cseq(sip_hdr_t *h)
{
    if (h->parsed)
        return NSBC_OK;
    if (h->type != SIP_H_CSEQ)
        return NSBC_INVAL;

    const char *p = h->value.p;
    const char *end = p + h->value.len;
    sip_cseq_t *c = &h->u.cseq;
    memset(c, 0, sizeof(*c));

    const char *sp = p;
    while (sp < end && !sip_is_sp(*sp))
        ++sp;
    if (sp == p || sp >= end)
        return NSBC_ERR;

    size_t dlen = (size_t)(sp - p);
    long num;
    if (dlen > 10 || !sip_parse_ulong(p, dlen, &num) ||
        num > 2147483647L)
        return NSBC_ERR;
    c->number = num;

    const char *mp = sip_skip_lws_run(sp + 1, end);
    const char *me = end;
    if (mp >= me)
        return NSBC_ERR;
    /* 方法必须为纯 token，尾部不允许垃圾 */
    for (const char *q = mp; q < me; ++q)
        if (!sip_is_tokchar((unsigned char)*q))
            return NSBC_ERR;
    c->method_str.p = mp;
    c->method_str.len = (size_t)(me - mp);
    c->method = sip_method_lookup(mp, c->method_str.len);

    h->parsed = 1;
    return NSBC_OK;
}

/* Max-Forwards：0..255 纯数字 */
nsbc_rc_t sip_hdr_parse_maxfwd(sip_hdr_t *h)
{
    if (h->parsed)
        return NSBC_OK;
    if (h->type != SIP_H_MAX_FORWARDS)
        return NSBC_INVAL;

    long v;
    if (h->value.len == 0 || h->value.len > 3 ||
        !sip_parse_ulong(h->value.p, h->value.len, &v) || v > 255)
        return NSBC_ERR;
    h->u.maxfwd = v;
    h->parsed = 1;
    return NSBC_OK;
}

/* ---- 逗号分隔选项标签查找（大小写敏感） ---- */
static int pc_is_lws(char c)
{
    return sip_is_sp(c) || c == '\r' || c == '\n';
}

int sip_hdr_has_option(const sip_hdr_t *h, const char *tag)
{
    if (!h || h->value.p == NULL)
        return 0;
    size_t tl = strlen(tag);
    const char *p = h->value.p;
    const char *end = p + h->value.len;

    while (p < end) {
        while (p < end && pc_is_lws(*p))
            ++p;
        const char *s = p;
        while (p < end && *p != ',' && !pc_is_lws(*p))
            ++p;
        if ((size_t)(p - s) == tl && memcmp(s, tag, tl) == 0)
            return 1;
        while (p < end && pc_is_lws(*p))
            ++p;
        if (p >= end)
            break;
        if (*p != ',')
            return 0; /* 值中存在非法字符 */
        ++p;
    }
    return 0;
}

/* ---- 在 From/To 值中直接取 tag（不经深解析，保持 const） ---- */
static int has_tag(const sip_hdr_t *h)
{
    const char *p = h->value.p;
    const char *end = p + h->value.len;
    const char *s = p;
    for (const char *q = p; q < end; ++q)
        if (*q == '>') {
            s = q; /* 参数区从 '>' 之后开始 */
            break;
        }
    for (const char *q = s; q + 5 <= end; ++q) {
        if (*q != ';')
            continue;
        const char *v = q + 1;
        if (sip_ci_eq(v, "tag=", 4) && v[4] != ';' && v[4] != ' ')
            return 1; /* 非空值 */
    }
    return 0;
}

int sip_msg_in_dialog(const sip_msg_t *m)
{
    const sip_hdr_t *f = m->by_type[SIP_H_FROM];
    const sip_hdr_t *t = m->by_type[SIP_H_TO];
    return f && t && has_tag(f) && has_tag(t);
}

/* ---- glare 策略 ---- */
void sip_upd_guard_init(sip_upd_guard_t *g) { g->pending = 0; }

int sip_upd_begin(sip_upd_guard_t *g)
{
    if (g->pending)
        return 0;
    g->pending = 1;
    return 1;
}

void sip_upd_end(sip_upd_guard_t *g) { g->pending = 0; }

long sip_glare_backoff_ms(int local_callid_lower, unsigned r)
{
    double f = (double)(r & 0xffff) / 65535.0;
    if (local_callid_lower)
        return (long)(f * 1000.0);          /* 0~1s */
    return 2100 + (long)(f * 1900.0);      /* 2.1~4s */
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
