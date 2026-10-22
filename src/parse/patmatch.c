#include "patmatch.h"
#include "log.h"

#include <ctype.h>

int pm_init(pm_pattern_t *p, pm_type_t type, const char *pattern,
            uint8_t min_len, uint8_t max_len)
{
    if (!p || !pattern)
        return NSBC_INVAL;

    memset(p, 0, sizeof(*p));
    p->type = type;
    p->min_len = min_len;
    p->max_len = max_len ? max_len : 255;

    size_t n = strlen(pattern);
    if (n >= sizeof(p->pattern))
        return NSBC_INVAL;
    memcpy(p->pattern, pattern, n + 1);

    if (type == PM_REGEX) {
        int ec = 0;
        PCRE2_SIZE eo = 0;
        p->re = pcre2_compile((PCRE2_SPTR)pattern, n,
                              PCRE2_ANCHORED | PCRE2_ENDANCHORED,
                              &ec, &eo, NULL);
        if (!p->re) {
            /* 取出 PCRE2 错误文本 */
            char emsg[256];
            pcre2_get_error_message(ec, (PCRE2_UCHAR *)emsg, sizeof(emsg));
            llog(LOG_ERR, "pm", "pcre2_compile FAIL: pattern='%s' ec=%d eo=%zu msg='%s'",
                 pattern, ec, eo, emsg);
            return NSBC_INVAL;
        }
#ifdef PCRE2_JIT
        pcre2_jit_compile(p->re, PCRE2_JIT_COMPLETE);
#endif
        llog(LOG_INFO, "pm", "pcre2_compile OK: pattern='%s'", pattern);
    }
    return NSBC_OK;
}

void pm_fini(pm_pattern_t *p)
{
    if (p && p->re) {
        pcre2_code_free(p->re);
        p->re = NULL;
    }
}

static int all_digits(const char *s)
{
    for (; *s; ++s)
        if (!isdigit((unsigned char)*s))
            return 0;
    return 1;
}

/* 电话通配: X = 0-9, N = 2-9, Z = 1-9，其它字符字面相等 */
static int match_digit_wild(const char *pat, const char *digits)
{
    size_t i = 0;
    for (; pat[i]; ++i) {
        if (!digits[i])
            return 0;
        char c = pat[i], d = digits[i];
        if (!isdigit((unsigned char)d))
            return 0;
        switch (c) {
        case 'X':
            if (d < '0' || d > '9')
                return 0;
            break;
        case 'N':
            if (d < '2' || d > '9')
                return 0;
            break;
        case 'Z':
            if (d < '1' || d > '9')
                return 0;
            break;
        default:
            if (c != d)
                return 0;
            break;
        }
    }
    return digits[i] == '\0'; /* 长度必须一致 */
}

int pm_match(const pm_pattern_t *p, const char *digits)
{
    char groups[PM_MAX_GROUPS][PM_GROUP_SZ];
    memset(groups, 0, sizeof(groups));
    return pm_match_groups(p, digits, groups);
}

int pm_match_groups(const pm_pattern_t *p, const char *digits,
                    char out_groups[PM_MAX_GROUPS][PM_GROUP_SZ])
{
    if (!p || !digits || !out_groups)
        return NSBC_INVAL;

    /* 非 REGEX 路径：out_groups[0] = 完整匹配串（= digits 原文） */
    size_t len = strlen(digits);
    if (len >= PM_GROUP_SZ) len = PM_GROUP_SZ - 1;

    switch (p->type) {
    case PM_EXACT:
        if (strcmp(p->pattern, digits) != 0) return 0;
        memcpy(out_groups[0], digits, len); out_groups[0][len] = '\0';
        return 1;

    case PM_PREFIX:
        if (!all_digits(digits) || len < strlen(p->pattern)) return 0;
        if (strncmp(p->pattern, digits, strlen(p->pattern)) != 0) return 0;
        memcpy(out_groups[0], digits, len); out_groups[0][len] = '\0';
        return 1;

    case PM_RANGE: {
        if (!all_digits(digits) || len < p->min_len || len > p->max_len) return 0;
        if (p->pattern[0] != '\0') {
            size_t pl = strlen(p->pattern);
            if (len < pl || strncmp(p->pattern, digits, pl) != 0) return 0;
        }
        memcpy(out_groups[0], digits, len); out_groups[0][len] = '\0';
        return 1;
    }

    case PM_DIGIT:
        if (!match_digit_wild(p->pattern, digits)) return 0;
        memcpy(out_groups[0], digits, len); out_groups[0][len] = '\0';
        return 1;

    case PM_REGEX: {
        llog(LOG_INFO, "pm", "pm_match_groups: re=%p digits='%s' pattern='%s'",
             (void*)p->re, digits, p->pattern);
        if (!p->re) {
            llog(LOG_WARN, "pm", "pm_match_groups: PM_REGEX but re=NULL pattern='%s'", p->pattern);
            return 0;
        }
        pcre2_match_data *md = pcre2_match_data_create_from_pattern(p->re, NULL);
        if (!md) {
            llog(LOG_ERR, "pm", "pm_match_groups: pcre2_match_data_create FAIL");
            return 0;
        }
        int rc = pcre2_match(p->re, (PCRE2_SPTR)digits, len, 0, 0, md, NULL);
        if (rc <= 0) {
            if (rc < 0 && rc != PCRE2_ERROR_NOMATCH) {
                char emsg[256];
                pcre2_get_error_message(rc, (PCRE2_UCHAR *)emsg, sizeof(emsg));
                llog(LOG_ERR, "pm", "pm_match_groups: pcre2_match rc=%d msg='%s'", rc, emsg);
            }
            pcre2_match_data_free(md);
            return 0;
        }
        /* rc = 组 0 的索引；最大捕获组数 = min(rc, PM_MAX_GROUPS) */
        int max_g = rc < PM_MAX_GROUPS ? rc : PM_MAX_GROUPS;
        for (int i = 0; i < max_g; ++i) {
            PCRE2_SIZE out = PM_GROUP_SZ;
            pcre2_substring_copy_bynumber(md, i, (PCRE2_UCHAR *)out_groups[i], &out);
            if (out == PM_GROUP_SZ) {
                llog(LOG_WARN, "pm", "pm_match_groups: group[%d] truncated (limit %d)",
                     i, PM_GROUP_SZ);
            }
        }
        pcre2_match_data_free(md);
        llog(LOG_INFO, "pm", "pm_match_groups: rc=%d groups=%d ($0='%s' $1='%s')",
             rc, max_g, out_groups[0], max_g > 1 ? out_groups[1] : "(none)");
        return 1;
    }
    }
    return 0;
}
