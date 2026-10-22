/* 通用参数表：';' 分隔的 name[=val]，零拷贝，结果写入调用方数组 */

#include "sip_common.h"

/* 值允许字符：token 字符 + '[', ']', ':'（host:port/IPv6） */
static int is_valchar(unsigned char c)
{
    return sip_is_tokchar(c) || c == '[' || c == ']' || c == ':';
}

nsbc_rc_t sip_parse_params(sip_param_t *out, int cap, const char *p,
                           const char *end, int *n_out)
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
        if (p == name)
            return NSBC_ERR;
        out[n].name.p = name;
        out[n].name.len = (size_t)(p - name);
        out[n].val.p = NULL;
        out[n].val.len = 0;
        if (p < end && *p == '=') {
            ++p;
            const char *v = p;
            if (v < end && *v == '"') {
                /* 引号串：含内部任意字符直到闭合引号 */
                ++v;
                const char *q = v;
                while (q < end && *q != '"')
                    ++q;
                if (q >= end)
                    return NSBC_ERR;
                out[n].val.p = v;
                out[n].val.len = (size_t)(q - v);
                p = q + 1;
            } else {
                while (p < end && is_valchar((unsigned char)*p))
                    ++p;
                if (p == v)
                    return NSBC_ERR;
                out[n].val.p = v;
                out[n].val.len = (size_t)(p - v);
            }
        }
        p = sip_skip_lws_run(p, end);
        if (p < end) {
            if (*p != ';')
                return NSBC_ERR;
            ++p;
        }
        ++n;
    }
    if (n_out)
        *n_out = n;
    return NSBC_OK;
}

const sip_param_t *sip_param_find(const sip_param_t *ps, int n,
                                  const char *name)
{
    size_t len = strlen(name);
    for (int i = 0; i < n; ++i)
        if (ps[i].name.len == len && sip_ci_eq(ps[i].name.p, name, len))
            return &ps[i];
    return NULL;
}