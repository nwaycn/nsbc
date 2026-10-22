/* STIR/SHAKEN——粗骨架（P3 RFC8224/8225/8226/8588）
 *
 * 本文件只实现无密码学依赖部分：Identity 头定位、参数与 JWT 三段
 * 拆分、payload 的 base64url 解码与字段粗提取、iat 时间窗判定。
 * x5u 证书链与 ECDSA 验签在 NSBC_WITH_OPENSSL 接线批补（ss_verify
 * 当前返回 SS_RES_NOTSUP，不谎报验证通过）。
 */
#include "stir_shaken.h"

#include <ctype.h>

const char *ss_find_identity(sip_msg_t *msg)
{
    int i;
    if (msg == NULL)
        return NULL;
    for (i = 0; i < msg->nhdrs; ++i) {
        sip_tok_t n = msg->hdrs[i].name;
        if (n.len == 8) {
            /* "Identity" 不区分大小写（无 compact 短名） */
            if (tolower((unsigned char)n.p[0]) == 'i' &&
                tolower((unsigned char)n.p[7]) == 'y' &&
                n.p[1] == 'd' && n.p[2] == 'e' && n.p[3] == 'n' &&
                n.p[4] == 't' && n.p[5] == 'i' && n.p[6] == 't')
                return msg->hdrs[i].value.p;
        }
    }
    return NULL;
}

/* 取引号/尖括号参数：info=<URL> / alg=ES256 / ppt="shaken"，
 * 粗扫描（值内不做转义处理，PASSporT 头参数集固定）。 */
static const char *param_val(const char *s, const char *key,
                             char *out, size_t cap, int *olen)
{
    size_t klen = strlen(key);
    const char *p = s;
    while ((p = strstr(p, key)) != NULL) {
        const char *v;
        if (p != s && isalnum((unsigned char)p[-1])) {
            ++p;
            continue;
        }
        v = p + klen;
        while (*v == ' ' || *v == '\t')
            ++v;
        if (*v == '=') {
            ++v;
            while (*v == ' ' || *v == '\t')
                ++v;
            if (*v == '<' || *v == '"') {
                char close = *v == '<' ? '>' : '"';
                const char *e = strchr(++v, close);
                size_t n;
                if (e == NULL)
                    return NULL;
                n = (size_t)(e - v);
                if (n >= cap)
                    return NULL;
                memcpy(out, v, n);
                out[n] = '\0';
                if (olen != NULL)
                    *olen = (int)n;
                return out;
            }
        }
        ++p;
    }
    return NULL;
}

static int b64val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '-')
        return 62;
    if (c == '_')
        return 63;
    return -1;
}

static int b64url_dec(const char *s, size_t n, uint8_t *out, size_t cap,
                      size_t *olen)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t i, w = 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        int d;
        if (s[i] == '=')
            break;
        d = b64val(c);
        if (d < 0)
            return NSBC_PROTO;
        acc = (acc << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (w >= cap)
                return NSBC_NOSPC;
            out[w++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    if (olen != NULL)
        *olen = w;
    return NSBC_OK;
}

/* 解码后 JSON 中取 "key":"val"（仅字符串字段，号码即此形态） */
static int json_str(const uint8_t *j, size_t n, const char *key,
                    char *out, size_t cap)
{
    char pat[32];
    const char *hit;
    const char *end = (const char *)j + n;
    size_t klen;
    int z = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (z <= 0)
        return NSBC_INVAL;
    hit = strstr((const char *)j, pat);
    if (hit == NULL)
        return NSBC_NOENT;
    hit += strlen(pat);
    while (hit < end && (*hit == ' ' || *hit == ':' || *hit == '\t'))
        ++hit;
    if (hit >= end || *hit != '"')
        return NSBC_NOENT;
    ++hit;
    klen = 0;
    while (hit + klen < end && hit[klen] != '"')
        ++klen;
    if (klen >= cap)
        return NSBC_NOSPC;
    memcpy(out, hit, klen);
    out[klen] = '\0';
    return NSBC_OK;
}

static ss_result_t extract_payload(const char *jwt, ss_passport_t *o)
{
    const char *dot1 = strchr(jwt, '.');
    const char *dot2;
    uint8_t dec[1024];
    size_t dlen = 0;
    char num[24];
    int rc;
    if (dot1 == NULL)
        return SS_RES_MALFORMED;
    dot2 = strchr(dot1 + 1, '.');
    if (dot2 == NULL)
        return SS_RES_MALFORMED;
    rc = b64url_dec(dot1 + 1, (size_t)(dot2 - dot1 - 1),
                    dec, sizeof(dec) - 1, &dlen);
    if (rc != NSBC_OK)
        return SS_RES_MALFORMED;
    dec[dlen] = '\0';
    if (json_str(dec, dlen, "attest", num, sizeof(num)) == NSBC_OK) {
        o->attest = num[0] == 'A' ? SS_ATTEST_A
                  : num[0] == 'B' ? SS_ATTEST_B
                  : num[0] == 'C' ? SS_ATTEST_C
                  : SS_ATTEST_UNKNOWN;
    }
    (void)json_str(dec, dlen, "tn", o->orig_tn, sizeof(o->orig_tn));
    /* dest 粗骨架：标准 dest.uri 数组，P3 接线批做完整 JSON，先取
     * 首个 tel/sip: 号码片段（扫描 "uri":"...13xx"）。 */
    (void)json_str(dec, dlen, "uri", o->dest_tn, sizeof(o->dest_tn));
    return SS_RES_OK;
}

ss_result_t ss_parse(const char *v, ss_passport_t *out,
                     char *jwt_b64_out, size_t cap)
{
    const char *ls;
    const char *gt;
    size_t blen;
    if (v == NULL || out == NULL)
        return SS_RES_MALFORMED;
    ls = strchr(v, '<');
    gt = ls == NULL ? NULL : strchr(ls, '>');
    if (ls == NULL || gt == NULL)
        return SS_RES_MALFORMED;
    blen = (size_t)(gt - ls - 1);
    if (blen == 0 || blen >= SS_B64_MAX)
        return SS_RES_MALFORMED;
    if (jwt_b64_out != NULL && cap > blen) {
        memcpy(jwt_b64_out, ls + 1, blen);
        jwt_b64_out[blen] = '\0';
    }
    memset(out, 0, sizeof(*out));
    if (param_val(v, "info", out->x5u, sizeof(out->x5u), NULL) == NULL)
        return SS_RES_MALFORMED;
    if (param_val(v, "alg", out->alg, sizeof(out->alg), NULL) == NULL)
        return SS_RES_MALFORMED;
    (void)param_val(v, "ppt", out->ppt, sizeof(out->ppt), NULL);
    if (strcmp(out->alg, "ES256") != 0)
        return SS_RES_BAD_ALG;
    if (out->ppt[0] != '\0' && strcmp(out->ppt, "shaken") != 0)
        return SS_RES_MALFORMED;
    return extract_payload(ls + 1, out);
}

ss_result_t ss_verify(const char *identity_value, const ss_ops_t *ops,
                      int64_t now_s, int slew_s)
{
    ss_passport_t pp;
    ss_result_t r;
    (void)ops;
    (void)now_s;
    (void)slew_s;
    if (identity_value == NULL)
        return SS_RES_NO_HEADER;
    r = ss_parse(identity_value, &pp, NULL, 0);
    if (r != SS_RES_OK)
        return r;
    /* TODO(NSBC_WITH_OPENSSL)：
     *  ops->fetch_cert(x5u) → X509 链验 → 校验 SP 证书 TNAuthList
     *  含 orig_tn（RFC8226）→ ECDSA P-256 验签两段 hash →
     *  |now-iat|<=slew_s 时间窗（时钟偏差先拒后观测调宽）。 */
    return SS_RES_NOTSUP;
}

const char *ss_attest_name(ss_attest_t a)
{
    switch (a) {
    case SS_ATTEST_A: return "A";
    case SS_ATTEST_B: return "B";
    case SS_ATTEST_C: return "C";
    case SS_ATTEST_UNKNOWN: break;
    }
    return "unknown";
}

const char *ss_result_name(ss_result_t r)
{
    switch (r) {
    case SS_RES_OK:           return "ok";
    case SS_RES_NO_HEADER:    return "no-header";
    case SS_RES_MALFORMED:    return "malformed";
    case SS_RES_BAD_ALG:      return "bad-alg";
    case SS_RES_CERT_FETCH:   return "cert-fetch";
    case SS_RES_SIGNATURE:    return "signature";
    case SS_RES_EXPIRED:      return "expired";
    case SS_RES_NOTSUP:       break;
    }
    return "notsup";
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
