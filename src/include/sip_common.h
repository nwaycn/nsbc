#ifndef NSBC_SIP_COMMON_H
#define NSBC_SIP_COMMON_H

/* parse/sip 模块内部公共小工具：所有操作均基于 [p,end)，不依赖 NUL */

#include "sip_msg.h"

static inline int sip_is_sp(char c) { return c == ' ' || c == '\t'; }

/* RFC 3261 token = 1*(%x21/23-27/2A/2B/2D/2E/30-39/41-5A/5E-7E) */
static inline int sip_is_tokchar(unsigned char c)
{
    static const unsigned char m[256] = {
        ['!'] = 1, ['#'] = 1, ['$'] = 1, ['%'] = 1, ['&'] = 1, ['\''] = 1,
        ['*'] = 1, ['+'] = 1, ['-'] = 1, ['.'] = 1, ['0'] = 1, ['1'] = 1,
        ['2'] = 1, ['3'] = 1, ['4'] = 1, ['5'] = 1, ['6'] = 1, ['7'] = 1,
        ['8'] = 1, ['9'] = 1,
        ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1,
        ['E'] = 1, ['F'] = 1, ['G'] = 1, ['H'] = 1, ['I'] = 1, ['J'] = 1,
        ['K'] = 1, ['L'] = 1, ['M'] = 1, ['N'] = 1, ['O'] = 1, ['P'] = 1,
        ['Q'] = 1, ['R'] = 1, ['S'] = 1, ['T'] = 1, ['U'] = 1, ['V'] = 1,
        ['W'] = 1, ['X'] = 1, ['Y'] = 1, ['Z'] = 1,
        ['^'] = 1, ['_'] = 1, ['`'] = 1,
        ['a'] = 1, ['b'] = 1, ['c'] = 1, ['d'] = 1, ['e'] = 1, ['f'] = 1,
        ['g'] = 1, ['h'] = 1, ['i'] = 1, ['j'] = 1, ['k'] = 1, ['l'] = 1,
        ['m'] = 1, ['n'] = 1, ['o'] = 1, ['p'] = 1, ['q'] = 1, ['r'] = 1,
        ['s'] = 1, ['t'] = 1, ['u'] = 1, ['v'] = 1, ['w'] = 1, ['x'] = 1,
        ['y'] = 1, ['z'] = 1, ['~'] = 1};
    return m[c];
}

static inline int sip_is_digit(char c) { return c >= '0' && c <= '9'; }

static inline int sip_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static inline int sip_ci_eq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (sip_lower(a[i]) != sip_lower(b[i]))
            return 0;
    return 1;
}

/* 跳过 SP/HT，返回不越过 end 的新指针 */
static inline const char *sip_skip_lws_run(const char *p, const char *end)
{
    while (p < end && sip_is_sp(*p))
        ++p;
    return p;
}

/* 回退尾部 SP/HT */
static inline const char *sip_rtrim(const char *p, const char *end)
{
    while (end > p && sip_is_sp(end[-1]))
        --end;
    return end;
}

/* 严格无符号整数：全为数字、非空、无多余字符 */
static inline int sip_parse_ulong(const char *p, size_t len, long *out)
{
    if (len == 0)
        return 0;
    long v = 0;
    for (size_t i = 0; i < len; ++i) {
        if (!sip_is_digit(p[i]))
            return 0;
        v = v * 10 + (p[i] - '0');
        if (v < 0)
            return 0;
    }
    *out = v;
    return 1;
}

/* sip_params.c */
nsbc_rc_t sip_parse_params(sip_param_t *out, int cap, const char *p,
                           const char *end, int *n_out);

/* sip_firstline.c */
nsbc_rc_t sip_parse_firstline(sip_msg_t *m, const char *p, const char *end);

#endif /* NSBC_SIP_COMMON_H */