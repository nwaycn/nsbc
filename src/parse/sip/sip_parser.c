/* 顶层 SIP 解析：分帧 → 首行 → 头索引扫描（含合法折叠）→ CL/body 严格校验
 * 默认严格模式；兼容项经 flags 开启（见 sip_msg.h） */

#include "sip_common.h"

/* 取一行：内容不含终止符；必须以 CRLF 或 LF 结束 */
static nsbc_rc_t get_line(const char *buf, size_t len, size_t *pos,
                          const char **ls, const char **le)
{
    size_t i = *pos;
    if (i >= len)
        return NSBC_ERR;
    size_t start = i;
    while (i < len && buf[i] != '\n')
        ++i;
    if (i >= len)
        return NSBC_ERR;
    *ls = buf + start;
    *le = (i > start && buf[i - 1] == '\r') ? buf + i - 1 : buf + i;
    *pos = i + 1;
    return NSBC_OK;
}

/* 仅允许出现一次的头（重复即可能为注入/歧义，严格拒绝） */
static const sip_hdr_type_t single_hdrs[] = {
    SIP_H_CALL_ID, SIP_H_FROM, SIP_H_TO, SIP_H_CSEQ,
    SIP_H_MAX_FORWARDS, SIP_H_CONTENT_TYPE};

nsbc_rc_t sip_parse_ex(sip_msg_t *m, const char *buf, size_t len,
                       unsigned flags)
{
    memset(m, 0, sizeof(*m));
    m->buf = buf;
    m->buflen = len;
    m->flags = flags;
    for (int i = 0; i < SIP_MAX_HDRS; ++i)
        m->hdrs[i].next_same = -1;

    if (len == 0 || len > SIP_MAX_MSG)
        return NSBC_ERR;
    for (size_t i = 0; i < len; ++i)
        if (buf[i] == '\0')
            return NSBC_ERR;

    size_t pos = 0;
    const char *ls, *le;

    /* 第一行 */
    if (get_line(buf, len, &pos, &ls, &le) != NSBC_OK)
        return NSBC_ERR;
    if ((size_t)(le - ls) == 0 || (size_t)(le - ls) > SIP_MAX_LINE)
        return NSBC_ERR;
    if (sip_parse_firstline(m, ls, le) != NSBC_OK)
        return NSBC_ERR;

    /* 头扫描 */
    int prev = -1;
    for (;;) {
        if (get_line(buf, len, &pos, &ls, &le) != NSBC_OK)
            return NSBC_ERR;
        size_t llen = (size_t)(le - ls);

        if (llen == 0)
            break; /* 空行：头段结束 */

        if (sip_is_sp(*ls)) {
            /* 合法折叠：仅允许接在前一头之后 */
            if (prev < 0)
                return NSBC_ERR;
            if (llen > SIP_MAX_LINE)
                return NSBC_ERR;
            sip_hdr_t *h = &m->hdrs[prev];
            h->value.len = (size_t)(le - h->value.p);
            if (h->value.len > SIP_MAX_LINE)
                return NSBC_ERR;
            continue;
        }

        if (m->nhdrs >= SIP_MAX_HDRS)
            return NSBC_ERR;
        if (llen > SIP_MAX_LINE)
            return NSBC_ERR;

        const char *colon = ls;
        while (colon < le && *colon != ':')
            ++colon;
        if (colon >= le)
            return NSBC_ERR;

        const char *name_end = colon;
        /* 兼容：头名与冒号间空格（RFC 3261 非法，现实设备偶发） */
        if (name_end > ls && sip_is_sp(name_end[-1])) {
            if (!(flags & SIP_F_COMPAT_NAME_SPACE))
                return NSBC_ERR;
            while (name_end > ls && sip_is_sp(name_end[-1]))
                --name_end;
            if (name_end == ls)
                return NSBC_ERR;
        }
        if (name_end == ls)
            return NSBC_ERR;

        sip_hdr_t *h = &m->hdrs[m->nhdrs];
        h->name.p = ls;
        h->name.len = (size_t)(name_end - ls);
        for (size_t i = 0; i < h->name.len; ++i)
            if (!sip_is_tokchar((unsigned char)h->name.p[i]))
                return NSBC_ERR;
        h->type = sip_hdr_type_lookup(h->name.p, h->name.len);
        h->next_same = -1;

        const char *vs = sip_skip_lws_run(colon + 1, le);
        const char *ve = sip_rtrim(vs, le);
        h->value.p = vs;
        h->value.len = (size_t)(ve - vs);

        int idx = m->nhdrs;
        if (!m->by_type[h->type])
            m->by_type[h->type] = h;
        else {
            sip_hdr_t *t = m->by_type[h->type];
            while (t->next_same != -1)
                t = &m->hdrs[t->next_same];
            t->next_same = idx;
        }
        prev = idx;
        ++m->nhdrs;
    }

    /* body = 空行之后全部内容 */
    m->body.p = buf + pos;
    m->body.len = len - pos;

    /* Content-Length 严格校验 */
    sip_hdr_t *cl = m->by_type[SIP_H_CONTENT_LENGTH];
    if (cl) {
        if (cl->next_same != -1)
            return NSBC_ERR; /* 重复 Content-Length */
        long v;
        if (!sip_parse_ulong(cl->value.p, cl->value.len, &v))
            return NSBC_ERR;
        m->cl_seen = 1;
        m->content_length = v;
    } else {
        m->content_length = 0;
    }
    if ((size_t)m->content_length != m->body.len)
        return NSBC_ERR;

    /* 单值头重复拒绝 */
    for (size_t i = 0; i < NSBC_ARRAY_SIZE(single_hdrs); ++i) {
        sip_hdr_t *h = m->by_type[single_hdrs[i]];
        if (h && h->next_same != -1)
            return NSBC_ERR;
    }

    return NSBC_OK;
}

nsbc_rc_t sip_parse(sip_msg_t *m, const char *buf, size_t len)
{
    return sip_parse_ex(m, buf, len, 0);
}

sip_hdr_t *sip_hdr_get(const sip_msg_t *m, sip_hdr_type_t type)
{
    return m->by_type[type];
}

sip_hdr_t *sip_hdr_next(const sip_msg_t *m, const sip_hdr_t *h)
{
    if (h->next_same < 0)
        return NULL;
    /* 经偏移返回，避免在 const 消息上直接取下标 */
    const sip_hdr_t *n = h + (h->next_same - (int)(h - m->hdrs));
    return (sip_hdr_t *)n;
}