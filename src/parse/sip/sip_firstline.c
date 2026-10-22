/* 首行严格解析：
 * 请求： METHOD SP RURI SP SIP/2.0
 * 响应： SIP/2.0 SP 3DIGIT SP reason
 * 仅接受单 SP 分隔（0x20），拒绝 HT/多空格
 * 方法名经 (长度,首字母,尾字母) perfect hash O(1) 判别 */

#include "sip_common.h"

sip_method_t sip_method_lookup(const char *p, size_t len)
{
    if (len == 0)
        return SIP_M_UNKNOWN;
    int f = sip_lower(p[0]);
    int l = sip_lower(p[len - 1]);
    switch (len) {
    case 3:
        if (f == 'a' && l == 'k') return SIP_M_ACK;
        if (f == 'b' && l == 'e') return SIP_M_BYE;
        break;
    case 4:
        if (f == 'i' && l == 'o') return SIP_M_INFO;
        break;
    case 5:
        if (f == 'p' && l == 'k') return SIP_M_PRACK;
        if (f == 'r' && l == 'r') return SIP_M_REFER;
        break;
    case 6:
        if (f == 'i' && l == 'e') return SIP_M_INVITE;
        if (f == 'c' && l == 'l') return SIP_M_CANCEL;
        if (f == 'n' && l == 'y') return SIP_M_NOTIFY;
        if (f == 'u' && l == 'e') return SIP_M_UPDATE;
        break;
    case 7:
        if (f == 'o' && l == 's') return SIP_M_OPTIONS;
        if (f == 'p' && l == 'h') return SIP_M_PUBLISH;
        if (f == 'm' && l == 'e') return SIP_M_MESSAGE;
        break;
    case 8:
        if (f == 'r' && l == 'r') return SIP_M_REGISTER;
        break;
    case 9:
        if (f == 's' && l == 'e') return SIP_M_SUBSCRIBE;
        break;
    }
    return SIP_M_UNKNOWN;
}

/* 在 [p,end) 内找 SP；遇到 HT 即视为非法 */
static const char *find_sp(const char *p, const char *end)
{
    while (p < end) {
        if (*p == ' ')
            return p;
        if (*p == '\t')
            return NULL;
        ++p;
    }
    return NULL;
}

static int version_ok(const char *p, size_t len)
{
    static const char v[] = "SIP/2.0";
    return len == sizeof(v) - 1 && memcmp(p, v, len) == 0;
}

nsbc_rc_t sip_parse_firstline(sip_msg_t *m, const char *p, const char *end)
{
    /* 响应判定：行首为 SIP/ */
    static const char v[] = "SIP/";
    if ((size_t)(end - p) >= sizeof(v) - 1 &&
        memcmp(p, v, sizeof(v) - 1) == 0) {
        m->is_request = 0;
        const char *sp = find_sp(p, end);
        if (!sp || !version_ok(p, (size_t)(sp - p)))
            return NSBC_ERR;
        p = sp + 1;

        const char *sp2 = find_sp(p, end);
        if (!sp2 || (size_t)(sp2 - p) != 3)
            return NSBC_ERR;
        for (const char *c = p; c < sp2; ++c)
            if (!sip_is_digit(*c))
                return NSBC_ERR;
        m->status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
        m->reason.p = sp2 + 1;
        m->reason.len = (size_t)(end - sp2 - 1);
        return NSBC_OK;
    }

    m->is_request = 1;
    const char *sp1 = find_sp(p, end);
    if (!sp1 || sp1 == p)
        return NSBC_ERR;
    const char *mp = p;
    while (mp < sp1) {
        if (!sip_is_tokchar((unsigned char)*mp))
            return NSBC_ERR;
        ++mp;
    }
    m->method_str.p = p;
    m->method_str.len = (size_t)(sp1 - p);
    m->method = sip_method_lookup(p, m->method_str.len);

    p = sp1 + 1;
    const char *sp2 = find_sp(p, end);
    if (!sp2 || sp2 == p)
        return NSBC_ERR;
    m->ruri_raw.p = p;
    m->ruri_raw.len = (size_t)(sp2 - p);

    p = sp2 + 1;
    if (!version_ok(p, (size_t)(end - p)))
        return NSBC_ERR;

    /* R-URI 令牌化 */
    if (sip_parse_uri(&m->ruri_raw, &m->ruri) != NSBC_OK)
        return NSBC_ERR;

    int is_sip = m->ruri.scheme.len == 3 &&
                 sip_ci_eq(m->ruri.scheme.p, "sip", 3);
    int is_sips = m->ruri.scheme.len == 4 &&
                  sip_ci_eq(m->ruri.scheme.p, "sips", 4);
    int is_tel = m->ruri.scheme.len == 3 &&
                 sip_ci_eq(m->ruri.scheme.p, "tel", 3);
    if (!is_sip && !is_sips &&
        !(is_tel && (m->flags & SIP_F_COMPAT_TEL_URI)))
        return NSBC_ERR;
    /* tel: 无 user@host 结构，号码规范化到 user，便于上层统一取号 */
    if (is_tel && m->ruri.user.len == 0) {
        m->ruri.user = m->ruri.host;
        m->ruri.host.p = NULL;
        m->ruri.host.len = 0;
    }
    m->ruri_parsed = 1;
    return NSBC_OK;
}