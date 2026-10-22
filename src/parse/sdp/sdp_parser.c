/* SDP 顶层解析（RFC 4566 + RFC 3264 关注点）：
 * 行扫描 → 会话级强制字段(v/o/s/t) → m= 分段 → c=/a= 归入当前作用域
 * 严格模式：强制字段必须存在；媒体段必须有有效 c=（port 0 除外） */

#include "sip_common.h"
#include "sdp.h"
#include "sdp_internal.h"
#include "log.h"
/* 拆空白字段：返回字段数（上限 cap），字段为零拷贝令牌 */
static int split_fields(sip_tok_t *out, int cap, const char *p,
                        const char *end)
{
    int n = 0;
    while (p < end) {
        p = sip_skip_lws_run(p, end);
        if (p >= end)
            break;
        if (n >= cap)
            return -1;
        const char *b = p;
        while (p < end && !sip_is_sp(*p))
            ++p;
        out[n].p = b;
        out[n].len = (size_t)(p - b);
        ++n;
    }
    return n;
}

/* 解析 a= 行内容为 name[:val] */
static nsbc_rc_t parse_attr(const char *p, const char *end, sdp_attr_t *a)
{
    const char *colon = p;
    while (colon < end && *colon != ':')
        ++colon;
    a->name.p = p;
    a->name.len = (size_t)(colon - p);
    if (a->name.len == 0)
        return NSBC_ERR;
    for (size_t i = 0; i < a->name.len; ++i)
        if (!sip_is_tokchar((unsigned char)a->name.p[i]))
            return NSBC_ERR;
    a->val.p = NULL;
    a->val.len = 0;
    if (colon < end) {
        a->val.p = colon + 1;
        a->val.len = (size_t)(end - colon - 1);
    }
    return NSBC_OK;
}

/* 解析 m= 行 */
static nsbc_rc_t parse_mline(sdp_media_t *m, const char *p,
                             const char *end)
{
    sip_tok_t f[2 + SDP_MAX_FMT];
    int nf = split_fields(f, (int)NSBC_ARRAY_SIZE(f), p, end);
    if (nf < 4)
        return NSBC_ERR;
    memset(m, 0, sizeof(*m));
    m->port_count = 1;

    static const struct {
        const char *name;
        sdp_mtype_t t;
    } mt[] = {
        {"audio", SDP_M_AUDIO},       {"video", SDP_M_VIDEO},
        {"text", SDP_M_TEXT},         {"application", SDP_M_APPLICATION},
        {"message", SDP_M_MESSAGE},   {"image", SDP_M_IMAGE},
    };
    m->type = SDP_M_UNKNOWN;
    for (size_t i = 0; i < NSBC_ARRAY_SIZE(mt); ++i) {
        size_t n = strlen(mt[i].name);
        if (f[0].len == n && sip_ci_eq(f[0].p, mt[i].name, n)) {
            m->type = mt[i].t;
            break;
        }
    }

    /* port[/count] */
    const char *q = f[1].p;
    const char *qe = q + f[1].len;
    const char *sl = q;
    while (sl < qe && *sl != '/')
        ++sl;
    long port;
    if (!sip_parse_ulong(q, (size_t)(sl - q), &port))
        return NSBC_ERR;
    m->port = (int)port;
    m->rejected = (m->port == 0);
    if (sl < qe) {
        long pc;
        if (!sip_parse_ulong(sl + 1, (size_t)(qe - sl - 1), &pc) || pc == 0)
            return NSBC_ERR;
        m->port_count = (int)pc;
    }

    m->proto = f[2];

    int np = 0;
    for (int i = 3; i < nf; ++i) {
        if (np >= SDP_MAX_FMT)
            return NSBC_ERR;
        m->fmt[np].raw = f[i];
        long v;
        m->fmt[np].pt =
            sip_parse_ulong(f[i].p, f[i].len, &v) ? (int)v : -1;
        ++np;
    }
    m->nfmt = np;
    return NSBC_OK;
}

/* o= 六字段 */
static nsbc_rc_t parse_oline(sdp_t *s, const char *p, const char *end)
{
    sip_tok_t f[6];
    if (split_fields(f, 6, p, end) != 6)
        return NSBC_ERR;
    s->sess_id = f[1];
    s->sess_ver = f[2];
    s->origin_addr = f[5];
    return NSBC_OK;
}

/* 解析 b= 行：<bwtype>:<bandwidth> */
static nsbc_rc_t parse_bw(const char *p, const char *end, sdp_bw_t *b)
{
    const char *colon = p;
    while (colon < end && *colon != ':')
        ++colon;
    if (colon == p || colon + 1 >= end)
        return NSBC_ERR;
    b->type.p = p;
    b->type.len = (size_t)(colon - p);
    for (size_t i = 0; i < b->type.len; ++i)
        if (!sip_is_tokchar((unsigned char)b->type.p[i]))
            return NSBC_ERR;
    long v;
    if (!sip_parse_ulong(colon + 1, (size_t)(end - colon - 1), &v))
        return NSBC_ERR;
    b->val = v;
    return NSBC_OK;
}

/* a= 应用到作用域：方向 / rtcp / mid */
static nsbc_rc_t apply_attr(sdp_t *s, sdp_media_t *m, const sdp_attr_t *a)
{
    sdp_dir_t d = sdp_dir_from_flag(a->name.p, a->name.len);
    if (d != SDP_DIR_INHERIT) {
        if (m)
            m->dir = d;
        else
            s->dir = d;
        return NSBC_OK;
    }
    if (!m)
        return NSBC_OK; /* 其余派生语义仅处理媒体段 */

    if (a->name.len == 4 && sip_ci_eq(a->name.p, "rtcp", 4)) {
        if (a->val.p == NULL)
            return NSBC_ERR;
        if (sdp_parse_rtcp_attr(a, &m->rtcp_port, &m->rtcp_conn,
                                &m->has_rtcp) != NSBC_OK)
            return NSBC_ERR;
    } else if (a->name.len == 3 && sip_ci_eq(a->name.p, "mid", 3)) {
        if (a->val.p == NULL || a->val.len == 0)
            return NSBC_ERR;
        m->mid = a->val;
    }
    return NSBC_OK;
}

nsbc_rc_t sdp_parse(sdp_t *s, const char *buf, size_t len)
{
    memset(s, 0, sizeof(*s));
    s->buf = buf;
    s->buflen = len;
    s->dir = SDP_DIR_INHERIT;

    if (len == 0)
        return NSBC_ERR;

    int saw_v = 0, saw_o = 0, saw_s = 0, saw_t = 0;
    sdp_media_t *cur = NULL;
    size_t pos = 0;
    int line_no = 0;  /* ★ 精确行号，诊断时打出来 */

    while (pos < len) {
        size_t start = pos;
        while (pos < len && buf[pos] != '\n')
            ++pos;
        if (pos >= len) {
            llog(LOG_WARN, "sdp",
                 "sdp_parse FAIL: last line missing \\n (pos=%zu len=%zu)",
                 pos, len);
            return NSBC_ERR; /* 最后一行必须有终止符 */
        }
        const char *ls = buf + start;
        const char *le =
            (pos > start && buf[pos - 1] == '\r') ? buf + pos - 1
                                                 : buf + pos;
        ++pos;
        ++line_no;  /* ★ */
        size_t llen = (size_t)(le - ls);
        if (llen == 0 || llen > SDP_MAX_LINE) {
            llog(LOG_WARN, "sdp",
                 "sdp_parse FAIL line #%d: line len=%zu 0 or > SDP_MAX_LINE=%d",
                 line_no, llen, SDP_MAX_LINE);
            return NSBC_ERR;
        }
        if (le - ls < 2 || ls[1] != '=') {
            llog(LOG_WARN, "sdp",
                 "sdp_parse FAIL line #%d: missing '=' after type char '%c'",
                 line_no, ls[0]);
            return NSBC_ERR;
        }
        char type = ls[0];
        const char *vp = ls + 2;
        const char *ve = le;

        switch (type) {
        case 'v':
            if (cur || saw_v || llen != 3 || memcmp(vp, "0", 1) != 0)
                return NSBC_ERR;
            saw_v = 1;
            break;
        case 'o':
            if (cur || saw_o)
                return NSBC_ERR;
            if (parse_oline(s, vp, ve) != NSBC_OK)
                return NSBC_ERR;
            saw_o = 1;
            break;
        case 's':
            if (cur || saw_s || vp == ve)
                return NSBC_ERR;
            saw_s = 1;
            break;
        case 't':
            if (cur || saw_t)
                return NSBC_ERR;
            {
                sip_tok_t tf[2];
                if (split_fields(tf, 2, vp, ve) != 2)
                    return NSBC_ERR;
                long t0, t1;
                if (!sip_parse_ulong(tf[0].p, tf[0].len, &t0) ||
                    !sip_parse_ulong(tf[1].p, tf[1].len, &t1))
                    return NSBC_ERR;
            }
            saw_t = 1;
            break;
        case 'r':
        case 'z':
            if (cur)
                return NSBC_ERR; /* 仅会话级 */
            break;
        case 'i':
        case 'u':
        case 'e':
        case 'p':
        case 'k':
            break; /* 接受，PoC 不存储 */
        case 'b': {
            sdp_bw_t *dst = cur ? cur->bws : s->bws;
            int *n = cur ? &cur->nbws : &s->nbws;
            if (*n >= 8)
                return NSBC_ERR;
            if (parse_bw(vp, ve, &dst[*n]) != NSBC_OK)
                return NSBC_ERR;
            ++(*n);
            break;
        }
        case 'c': {
            sdp_conn_t *dst = cur ? &cur->conn : &s->conn;
            int *flag = cur ? &cur->has_conn : &s->has_conn;
            if (*flag)
                return NSBC_ERR; /* 同作用域重复 c= */
            sip_tok_t cv;
            cv.p = vp;
            cv.len = (size_t)(ve - vp);
            if (sdp_parse_conn(&cv, dst) != NSBC_OK)
                return NSBC_ERR;
            *flag = 1;
            break;
        }
        case 'a': {
            sdp_attr_t a;
            if (parse_attr(vp, ve, &a) != NSBC_OK) {
                llog(LOG_WARN, "sdp",
                     "sdp_parse FAIL line #%d type=a: parse_attr ERR", line_no);
                return NSBC_ERR;
            }
            if (apply_attr(s, cur, &a) != NSBC_OK) {
                char aname[64] = {0};
                size_t nl = a.name.len < sizeof(aname) - 1 ? a.name.len : sizeof(aname) - 1;
                memcpy(aname, a.name.p, nl);
                llog(LOG_WARN, "sdp",
                     "sdp_parse FAIL line #%d type=a apply_attr ERR name=%s "
                     "(sdp_attr.c rtcp/mid parse failed?)", line_no, aname);
                return NSBC_ERR;
            }
            sdp_attr_t *dst =
                cur ? cur->attrs : s->attrs;
            int *n = cur ? &cur->nattrs : &s->nattrs;
            if (*n >= SDP_MAX_ATTRS) {
                llog(LOG_WARN, "sdp",
                     "sdp_parse FAIL line #%d type=a: attr overflow n=%d >= SDP_MAX_ATTRS=%d "
                     "(need bump)", line_no, *n, SDP_MAX_ATTRS);
                return NSBC_ERR;
            }
            dst[*n] = a;
            ++(*n);
            break;
        }
        case 'm': {
            if (s->nmedia >= SDP_MAX_MEDIA) {
                llog(LOG_WARN, "sdp",
                     "sdp_parse FAIL line #%d type=m: media overflow n=%d >= SDP_MAX_MEDIA=%d",
                     line_no, s->nmedia, SDP_MAX_MEDIA);
                return NSBC_ERR;
            }
            cur = &s->media[s->nmedia];
            if (parse_mline(cur, vp, ve) != NSBC_OK) {
                llog(LOG_WARN, "sdp",
                     "sdp_parse FAIL line #%d type=m: parse_mline ERR (port/fmt?)",
                     line_no);
                return NSBC_ERR;
            }
            ++s->nmedia;
            break;
        }
        default:
            llog(LOG_WARN, "sdp",
                 "sdp_parse FAIL line #%d: unknown SDP type char '%c' "
                 "(len=%zu -- non-standard SDP line!)",
                 line_no, type, llen);
            return NSBC_ERR; /* 严格模式：未知行类型拒绝 */
        }
    }

    /* 强制字段（无媒体段也要求完整会话描述） */
    if (!saw_v || !saw_o || !saw_s || !saw_t) {
        llog(LOG_WARN, "sdp",
             "sdp_parse FAIL: missing mandatory field v=%d o=%d s=%d t=%d",
             saw_v, saw_o, saw_s, saw_t);
        return NSBC_ERR;
    }

    /* 每个非拒绝媒体段必须有可达 c= */
    for (int i = 0; i < s->nmedia; ++i) {
        sdp_media_t *m = &s->media[i];
        if (!m->rejected && !sdp_media_effective_conn(s, m)) {
            llog(LOG_WARN, "sdp",
                 "sdp_parse FAIL media[%d]: rejected=%d has_conn=%d session.has_conn=%d "
                 "(非拒绝媒体段无可达 c=)",
                 i, m->rejected, m->has_conn, s->has_conn);
            return NSBC_ERR;
        }
    }
    return NSBC_OK;
}