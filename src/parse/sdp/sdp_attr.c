/* 属性相关：查找、方向、rtpmap/fmtp/rtcp 值解析、静态 PT 表 */

#include "sip_common.h"
#include "sdp.h"
#include "sdp_internal.h"

/* ---- 属性查找（CI，按已知长度） ---- */
const sdp_attr_t *sdp_attr_find(const sdp_attr_t *a, int n,
                                const char *name)
{
    size_t len = strlen(name);
    for (int i = 0; i < n; ++i)
        if (a[i].name.len == len && sip_ci_eq(a[i].name.p, name, len))
            return &a[i];
    return NULL;
}

/* ---- 方向 flag ---- */
sdp_dir_t sdp_dir_from_flag(const char *p, size_t len)
{
    static const char *names[] = {
        "sendrecv", "sendonly", "recvonly", "inactive"};
    sdp_dir_t dirs[] = {
        SDP_DIR_SENDRECV, SDP_DIR_SENDONLY, SDP_DIR_RECVONLY,
        SDP_DIR_INACTIVE};
    for (size_t i = 0; i < 4; ++i) {
        size_t n = strlen(names[i]);
        if (n == len && sip_ci_eq(p, names[i], n))
            return dirs[i];
    }
    return SDP_DIR_INHERIT;
}

/* ---- c= 解析：nettype SP addrtype SP addr[/ttl[/count]] ---- */
nsbc_rc_t sdp_parse_conn(const sip_tok_t *v, sdp_conn_t *c)
{
    const char *p = v->p;
    const char *end = p + v->len;
    memset(c, 0, sizeof(*c));
    c->count = 1;

    const char *sp1 = NULL, *sp2 = NULL;
    for (const char *q = p; q < end; ++q) {
        if (*q == ' ') {
            if (!sp1)
                sp1 = q;
            else if (!sp2) {
                sp2 = q;
                break;
            }
        }
    }
    if (!sp1 || !sp2)
        return NSBC_ERR;
    c->nettype.p = p;
    c->nettype.len = (size_t)(sp1 - p);
    c->addrtype.p = sp1 + 1;
    c->addrtype.len = (size_t)(sp2 - sp1 - 1);
    const char *a = sp2 + 1;
    if (a == end)
        return NSBC_ERR;

    const char *sl = end;
    for (const char *q = a; q < end; ++q)
        if (*q == '/') {
            sl = q;
            break;
        }
    c->addr.p = a;
    c->addr.len = (size_t)(sl - a);
    if (c->addr.len == 0)
        return NSBC_ERR;

    if (sl < end) {
        const char *sl2 = end;
        for (const char *q = sl + 1; q < end; ++q)
            if (*q == '/') {
                sl2 = q;
                break;
            }
        c->ttl.p = sl + 1;
        c->ttl.len = (size_t)(sl2 - sl - 1);
        long ttl;
        if (!sip_parse_ulong(c->ttl.p, c->ttl.len, &ttl))
            return NSBC_ERR;
        if (sl2 < end) {
            sip_tok_t ct;
            ct.p = sl2 + 1;
            ct.len = (size_t)(end - sl2 - 1);
            long cnt;
            if (!sip_parse_ulong(ct.p, ct.len, &cnt) || cnt == 0)
                return NSBC_ERR;
            c->count = (int)cnt;
        }
    }
    return NSBC_OK;
}

/* ---- rtpmap 值：PT SP enc/rate[/channels] ---- */
nsbc_rc_t sdp_parse_rtpmap_val(const sip_tok_t *v, int *pt,
                               sip_tok_t *enc, sip_tok_t *rate,
                               sip_tok_t *channels)
{
    const char *p = v->p;
    const char *end = p + v->len;
    const char *sp = p;
    while (sp < end && !sip_is_sp(*sp))
        ++sp;
    if (sp == p)
        return NSBC_ERR;
    long ppt;
    if (!sip_parse_ulong(p, (size_t)(sp - p), &ppt))
        return NSBC_ERR;
    *pt = (int)ppt;

    const char *e = sip_skip_lws_run(sp, end);
    const char *sl1 = e;
    while (sl1 < end && *sl1 != '/')
        ++sl1;
    if (sl1 == e)
        return NSBC_ERR;
    enc->p = e;
    enc->len = (size_t)(sl1 - e);

    const char *sl2 = end;
    for (const char *q = sl1 + 1; q < end; ++q)
        if (*q == '/') {
            sl2 = q;
            break;
        }
    rate->p = sl1 + 1;
    rate->len = (size_t)(sl2 - sl1 - 1);
    if (rate->len == 0)
        return NSBC_ERR;
    long r;
    if (!sip_parse_ulong(rate->p, rate->len, &r))
        return NSBC_ERR;

    channels->p = NULL;
    channels->len = 0;
    if (sl2 < end) {
        channels->p = sl2 + 1;
        channels->len = (size_t)(end - sl2 - 1);
        if (channels->len == 0)
            return NSBC_ERR;
    }
    return NSBC_OK;
}

/* ---- a=rtcp：port [nettype addrtype addr] ---- */
nsbc_rc_t sdp_parse_rtcp_attr(const sdp_attr_t *a, int *port,
                              sdp_conn_t *c, int *has_conn)
{
    const char *p = a->val.p;
    const char *end = p + a->val.len;
    const char *sp = p;
    while (sp < end && !sip_is_sp(*sp))
        ++sp;
    if (sp == p)
        return NSBC_ERR;
    long pv;
    if (!sip_parse_ulong(p, (size_t)(sp - p), &pv))
        return NSBC_ERR;
    *port = (int)pv;
    *has_conn = 0;
    if (sp < end) {
        sip_tok_t cv;
        cv.p = sp + 1;
        cv.len = (size_t)(end - sp - 1);
        if (sdp_parse_conn(&cv, c) != NSBC_OK)
            return NSBC_ERR;
        *has_conn = 1;
    }
    return NSBC_OK;
}

/* ---- RFC 3551 静态 PT 表（无 rtpmap 时回退） ---- */
typedef struct {
    int pt;
    const char *enc;
    const char *rate;
} static_pt_t;

static const static_pt_t static_pt_tab[] = {
    {0, "PCMU", "8000"},   {1, "1016", "8000"},
    {2, "G721", "8000"},   {3, "GSM", "8000"},
    {4, "G723", "8000"},   {5, "DVI4", "8000"},
    {6, "DVI4", "16000"},  {7, "LPC", "8000"},
    {8, "PCMA", "8000"},   {9, "G722", "8000"},
    {10, "L16", "44100"},  {11, "L16", "44100"},
    {12, "QCELP", "8000"}, {13, "CN", "8000"},
    {14, "MPA", "90000"},  {15, "G728", "8000"},
    {16, "DVI4", "11025"}, {17, "DVI4", "22050"},
    {18, "G729", "8000"},
};

nsbc_rc_t sdp_static_pt_lookup(int pt, sip_tok_t *enc, sip_tok_t *rate)
{
    for (size_t i = 0; i < NSBC_ARRAY_SIZE(static_pt_tab); ++i) {
        if (static_pt_tab[i].pt != pt)
            continue;
        enc->p = static_pt_tab[i].enc;
        enc->len = strlen(static_pt_tab[i].enc);
        rate->p = static_pt_tab[i].rate;
        rate->len = strlen(static_pt_tab[i].rate);
        return NSBC_OK;
    }
    return NSBC_ERR;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
