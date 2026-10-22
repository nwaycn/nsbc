#include "sip_digest.h"
#include "md5.h"
#include "nsbc_id.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* nonce 明文结构：<expiry_ms-hex16>.<rand16hex>
 * 签名：md5(expiry "." rand "." secret) 取前 16 hex
 * 完整 nonce：<exp>.<rand>.<sig16>，共 16+1+16+1+16=50 字符 */

/* ★ 返回 int：NSBC_OK / NSBC_NOMEM（strdup 失败）
 * 旧签名 void digest_init 未检查 strdup 返回值，后续 snprintf("%s", NULL) 是 UB */
int digest_init(digest_cfg_t *d, const char *realm, const char *secret,
                uint64_t ttl_ms, digest_ha1_fn cb, void *ud)
{
    if (!d) return NSBC_INVAL;
    /* strdup：cfg_get_str 返回的指针可能被后续 yaml 覆盖，
     * realm 来自 signaling 模块的固定缓冲区也一起 strdup 保证安全。
     * strdup 失败 → 置 NULL，后续 snprintf("%s", NULL) 在标准下是 UB，
     * 所以这里直接返回错误让调用方处理（通常是放弃 digest auth）。 */
    if (realm) {
        d->realm = (const char *)strdup(realm);
        if (!d->realm) goto nomem;
    } else {
        d->realm = NULL;
    }
    if (secret) {
        d->server_secret = (const char *)strdup(secret);
        if (!d->server_secret) goto nomem_secret;
    } else {
        d->server_secret = NULL;
    }
    d->nonce_ttl_ms = ttl_ms ? ttl_ms : 300000;
    d->ha1_cb = cb;
    d->ha1_ud = ud;
    return NSBC_OK;
nomem_secret:
    free((void *)d->realm); d->realm = NULL;
nomem:
    d->realm = NULL; d->server_secret = NULL;
    llog(LOG_ERR, "dig", "digest_init strdup failed");
    return NSBC_NOMEM;
}

void digest_free(digest_cfg_t *d)
{
    if (!d) return;
    free((void *)d->realm); d->realm = NULL;
    free((void *)d->server_secret); d->server_secret = NULL;
}

/* ★ nonce 长度组成（用宏替代硬编码 50）：
 *   exp 16 hex + '.' + rand 16 hex + '.' + sig 16 hex = 50 字符 */
#define DIGEST_NONCE_EXP_HEX   16
#define DIGEST_NONCE_RAND_HEX  16
#define DIGEST_NONCE_SIG_HEX   16
#define DIGEST_NONCE_LEN       (DIGEST_NONCE_EXP_HEX + 1 + DIGEST_NONCE_RAND_HEX + 1 + DIGEST_NONCE_SIG_HEX)

int digest_make_nonce(const digest_cfg_t *d, uint64_t now_ms, char *out,
                      size_t outsz)
{
    char randhex[DIGEST_NONCE_RAND_HEX + 1];
    if (nsbc_rand_token(randhex, sizeof(randhex), DIGEST_NONCE_RAND_HEX) != NSBC_OK)
        return NSBC_ERR;
    uint64_t exp = now_ms + d->nonce_ttl_ms;
    char base[80], sig33[SIP_RESP_LEN];
    {
        int nb = snprintf(base, sizeof(base),
                          "%0*llx.%s",
                          DIGEST_NONCE_EXP_HEX, (unsigned long long)exp, randhex);
        if (nb < 0 || (size_t)nb >= sizeof(base)) return NSBC_NOSPC;
    }
    nsbc_md5_hex_n(sig33, 3, base, ".", d->server_secret ?
                   d->server_secret : "nsbc");
    int n = snprintf(out, outsz, "%s.%.*s", base, DIGEST_NONCE_SIG_HEX, sig33);
    return (n > 0 && (size_t)n < outsz) ? NSBC_OK : NSBC_NOSPC;
}

int digest_check_nonce(const digest_cfg_t *d, const char *nonce, size_t nlen,
                       uint64_t now_ms)
{
    /* ★ 用宏替代硬编码 nlen != 50 */
    if (nlen != DIGEST_NONCE_LEN ||
        nonce[DIGEST_NONCE_EXP_HEX] != '.' ||
        nonce[DIGEST_NONCE_EXP_HEX + 1 + DIGEST_NONCE_RAND_HEX] != '.')
        return NSBC_AUTH;
    char base[40];
    memcpy(base, nonce, DIGEST_NONCE_EXP_HEX + 1 + DIGEST_NONCE_RAND_HEX);
    base[DIGEST_NONCE_EXP_HEX + 1 + DIGEST_NONCE_RAND_HEX] = '\0';
    char expect33[SIP_RESP_LEN];
    nsbc_md5_hex_n(expect33, 3, base, ".", d->server_secret ?
                   d->server_secret : "nsbc");
    if (strncmp(expect33,
                nonce + DIGEST_NONCE_EXP_HEX + 1 + DIGEST_NONCE_RAND_HEX + 1,
                DIGEST_NONCE_SIG_HEX) != 0)
        return NSBC_AUTH;
    char ex[DIGEST_NONCE_EXP_HEX + 1];
    memcpy(ex, nonce, DIGEST_NONCE_EXP_HEX);
    ex[DIGEST_NONCE_EXP_HEX] = '\0';
    uint64_t exp = (uint64_t)strtoull(ex, NULL, 16);
    return now_ms <= exp ? NSBC_OK : NSBC_EXPIRED;
}

int digest_make_challenge(const digest_cfg_t *d, int stale, char *out,
                          size_t outsz)
{
    char nonce[SIP_NONCE_LEN];
    /* 质询生成处用墙钟单调时间均可；签名只与内容相关，时效判定由
     * 调用方传 now_ms，这里用 0 基准由校验方解释绝对毫秒。 */
    if (digest_make_nonce(d, nsbc_now_ms(), nonce, sizeof(nonce)) != NSBC_OK)
        return NSBC_ERR;
    int n = snprintf(
        out, outsz,
        "Digest realm=\"%s\", nonce=\"%s\", algorithm=MD5, qop=\"auth\"%s",
        d->realm, nonce, stale ? ", stale=true" : "");
    return (n > 0 && (size_t)n < outsz) ? NSBC_OK : NSBC_NOSPC;
}

static void tok_copy(char *dst, size_t dstsz, sip_tok_t t)
{
    size_t n = t.len < dstsz - 1 ? t.len : dstsz - 1;
    memcpy(dst, t.p, n);
    dst[n] = '\0';
}

dig_auth_rc_t digest_verify(const digest_cfg_t *d, const sip_msg_t *m,
                            const sip_auth_t *a, uint64_t now_ms)
{
    if (!a || a->scheme.len == 0)
        return DIG_AUTH_BAD_FORMAT;
    char scheme[16];
    tok_copy(scheme, sizeof(scheme), a->scheme);
    if (strcasecmp(scheme, "Digest"))
        return DIG_AUTH_NOTSUP;

    char user[128], realm[128], nonce[80], uri[256], qop[16], nc[16],
        cnonce[128], resp[40], algo[16];
    user[0] = realm[0] = nonce[0] = uri[0] = qop[0] = nc[0] = cnonce[0] =
        resp[0] = algo[0] = '\0';
    for (int i = 0; i < a->nparams; ++i) {
        const sip_param_t *p = &a->params[i];
        char name[32];
        tok_copy(name, sizeof(name), p->name);
        char *dst = NULL;
        size_t cap = 0;
        if (!strcasecmp(name, "username")) { dst = user; cap = sizeof(user); }
        else if (!strcasecmp(name, "realm")) { dst = realm; cap = sizeof(realm); }
        else if (!strcasecmp(name, "nonce")) { dst = nonce; cap = sizeof(nonce); }
        else if (!strcasecmp(name, "uri")) { dst = uri; cap = sizeof(uri); }
        else if (!strcasecmp(name, "qop")) { dst = qop; cap = sizeof(qop); }
        else if (!strcasecmp(name, "nc")) { dst = nc; cap = sizeof(nc); }
        else if (!strcasecmp(name, "cnonce")) { dst = cnonce; cap = sizeof(cnonce); }
        else if (!strcasecmp(name, "response")) { dst = resp; cap = sizeof(resp); }
        else if (!strcasecmp(name, "algorithm")) { dst = algo; cap = sizeof(algo); }
        if (dst && p->val.p)
            tok_copy(dst, cap, p->val);
    }
    if (algo[0] && strcasecmp(algo, "MD5"))
        return DIG_AUTH_NOTSUP;
    if (!user[0] || !realm[0] || !nonce[0] || !uri[0] || !resp[0])
        return DIG_AUTH_BAD_FORMAT;

    int nrc = digest_check_nonce(d, nonce, strlen(nonce), now_ms);
    if (nrc == NSBC_AUTH)
        return DIG_AUTH_BAD_NONCE;
    if (nrc == NSBC_EXPIRED)
        return DIG_AUTH_STALE_NONCE;

    const char *ha1 = d->ha1_cb
        ? d->ha1_cb(realm, strlen(realm), user, strlen(user), d->ha1_ud)
        : NULL;
    /* ⚠️ 移除 static ha1_fallback（多 worker 并发写竞态，P0 线程安全 Bug）。
     * 生产环境必须通过 ha1_cb 提供每个用户的 HA1；没有 ha1_cb 且无匹配用户 → 拒绝认证。 */
    if (!ha1) {
        llog(LOG_WARN, "dig",
             "key=fail2ban reason=user_unknown user=%s realm=%s",
             user, realm);
        return DIG_AUTH_USER_UNKNOWN;
    }

    char method[16];
    size_t ml = m->method_str.len < sizeof(method) - 1
        ? m->method_str.len : sizeof(method) - 1;
    memcpy(method, m->method_str.p, ml);
    method[ml] = '\0';

    char ha2[33];
    nsbc_md5_hex_n(ha2, 3, method, ":", uri);
    llog(LOG_DEBUG, "dig", "verify: user=%s realm=%s qop=%s nc=%s cnonce=%s",
         user, realm, qop[0]?qop:"(none)", nc[0]?nc:"(none)", cnonce[0]?cnonce:"(none)");
    /* ★ HA1 脱敏：等价于明文密码的 MD5，生产环境即使 DEBUG 也不能全量打印 */
    char ha1_mask[8] = {0};
    if (ha1) snprintf(ha1_mask, sizeof(ha1_mask), "%.4s***", ha1);
    llog(LOG_DEBUG, "dig", "verify: ha1=%s ha2=%s client_resp=%s",
         ha1_mask, ha2, resp);
    char expect[33];
    if (qop[0]) {
        if (strcasecmp(qop, "auth") || !nc[0] || !cnonce[0])
            return DIG_AUTH_BAD_FORMAT;
        /* H(HA1:nonce:nc:cnonce:qop:HA2) */
        nsbc_md5_t c;
        nsbc_md5_init(&c);
        char sep = ':';
        const char *parts[6] = {ha1, nonce, nc, cnonce, qop, ha2};
        for (int i = 0; i < 6; ++i) {
            if (i)
                nsbc_md5_update(&c, &sep, 1);
            nsbc_md5_update(&c, parts[i], strlen(parts[i]));
        }
        uint8_t dg[16];
        nsbc_md5_final(&c, dg);
        static const char hx[] = "0123456789abcdef";
        for (int i = 0; i < 16; ++i) {
            expect[i * 2] = hx[dg[i] >> 4];
            expect[i * 2 + 1] = hx[dg[i] & 15];
        }
        expect[32] = '\0';
    } else {
        /* 兼容无 qop 旧终端：H(HA1:nonce:HA2) */
        nsbc_md5_hex_n(expect, 5, ha1, ":", nonce, ":", ha2);
    }
    llog(LOG_DEBUG, "dig", "verify: server_expect=%s match=%d", expect, strcasecmp(expect, resp) == 0);
    return strcasecmp(expect, resp) == 0 ? DIG_AUTH_OK
                                         : DIG_AUTH_MISMATCH;
}

void digest_make_ha1(const char *user, const char *realm, const char *pass,
                     char out33[SIP_RESP_LEN])
{
    /* H(user:realm:pass)：通过逐段喂入插入冒号 */
    nsbc_md5_t c;
    nsbc_md5_init(&c);
    char sep = ':';
    nsbc_md5_update(&c, user, strlen(user));
    nsbc_md5_update(&c, &sep, 1);
    nsbc_md5_update(&c, realm, strlen(realm));
    nsbc_md5_update(&c, &sep, 1);
    nsbc_md5_update(&c, pass, strlen(pass));
    uint8_t dg[16];
    nsbc_md5_final(&c, dg);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        out33[i * 2] = hx[dg[i] >> 4];
        out33[i * 2 + 1] = hx[dg[i] & 15];
    }
    out33[32] = '\0';
}

int digest_client_response(const char *user, const char *realm,
                           const char *pass, const char *method,
                           const char *uri, const char *nonce,
                           const char *nc, const char *cnonce,
                           char resp33[SIP_RESP_LEN])
{
    char ha1[33];
    digest_make_ha1(user, realm, pass, ha1);
    return digest_client_response_ha1(ha1, method, uri, nonce, nc, cnonce,
                                      "auth", resp33);
}

/* ---------- UAC 侧：parse challenge + build Authorization ---------- */

/* 找 hdr 中 key=value 对。value 可有引号。返回值：找到返回 1，dst 为值；
 * 找不到返回 0。outsz 不够返回 -1。
 * ★ 边界检查：key 前面必须是字符串起始、逗号、空格或 \t，
 *   避免 xrealm= / notrealm= 这类前缀匹配。 */
static int find_quoted_param(const char *hdr, size_t hlen,
                             const char *key, char *dst, size_t dstsz)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "%s=", key);
    if (n < 0 || (size_t)n >= sizeof(needle)) return 0;
    const char *p = memmem(hdr, hlen, needle, (size_t)n);
    if (!p) return 0;
    /* token 边界：p 之前必须是分隔符或字符串起始 */
    if (p > hdr) {
        char prev = p[-1];
        if (prev != ',' && prev != ' ' && prev != '\t') {
            /* 不是 token 边界，跳过这次匹配 */
            return 0;
        }
    }
    p += n;
    size_t remaining = hlen - (size_t)(p - hdr);
    if (*p == '"') {
        ++p; --remaining;
        const char *end = p;
        while (*end && remaining > 0 && *end != '"') { ++end; --remaining; }
        size_t vlen = (size_t)(end - p);
        if (vlen >= dstsz) return -1;
        memcpy(dst, p, vlen);
        dst[vlen] = '\0';
        return 1;
    } else {
        /* 无引号：逗号或字符串结尾 */
        const char *end = p;
        while (*end && *end != ',' && *end != ' ' && *end != '\t') ++end;
        size_t vlen = (size_t)(end - p);
        if (vlen >= dstsz) return -1;
        memcpy(dst, p, vlen);
        dst[vlen] = '\0';
        return 1;
    }
}

static int find_flag_param(const char *hdr, size_t hlen,
                           const char *key, int *out)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "%s=true", key);
    if (n < 0 || (size_t)n >= sizeof(needle)) return 0;
    /* ★ 边界检查 + 用 memmem */
    const char *p = memmem(hdr, hlen, needle, (size_t)n);
    if (!p) { *out = 0; return 0; }
    if (p > hdr) {
        char prev = p[-1];
        if (prev != ',' && prev != ' ' && prev != '\t') {
            *out = 0; return 0;
        }
    }
    *out = 1; return 1;
}

int digest_parse_challenge(const char *hdr_value, digest_challenge_t *out)
{
    if (!hdr_value || !out) return NSBC_INVAL;
    memset(out, 0, sizeof(*out));

    /* 跳过 scheme："Digest realm=..." 或 "Digest"（可能大小写混合） */
    const char *s = hdr_value;
    while (*s == ' ' || *s == '\t') ++s;
    if (strncasecmp(s, "Digest", 6) != 0) {
        /* 可能是 Proxy-Authenticate 用了其他 scheme */
        return NSBC_NOTSUP;
    }
    s += 6;
    if (*s == ':') ++s;
    while (*s == ' ' || *s == '\t') ++s;

    size_t hlen = strlen(s);
    int found = 0;

    if (find_quoted_param(s, hlen, "realm", out->realm, sizeof(out->realm)) > 0) ++found;
    find_quoted_param(s, hlen, "nonce", out->nonce, sizeof(out->nonce));
    find_quoted_param(s, hlen, "opaque", out->opaque, sizeof(out->opaque));
    find_quoted_param(s, hlen, "qop", out->qop, sizeof(out->qop));
    find_quoted_param(s, hlen, "algorithm", out->algorithm, sizeof(out->algorithm));
    find_flag_param(s, hlen, "stale", &out->stale);

    if (!found || !out->nonce[0]) {
        /* realm 或 nonce 缺失 → 格式无效 */
        return NSBC_INVAL;
    }
    /* qop 如果是 auth,auth-int 形式，优先选 auth（auth-int 要求 entity-body
     * hashing，我们不带 body 所以不支持） */
    if (out->qop[0]) {
        if (strchr(out->qop, ',')) {
            /* 遍历 qop 列表找 "auth"；找不到取第一个 */
            char list[64];
            int nl = snprintf(list, sizeof(list), "%s", out->qop);
            if (nl < 0 || (size_t)nl >= sizeof(list)) {
                /* 长到溢出 → 只用第一个 token（逗号之前） */
                list[0] = '\0';
                for (size_t i = 0; i < sizeof(list)-1 && out->qop[i]
                         && out->qop[i] != ','; ++i)
                    list[i] = out->qop[i];
                list[list[0] ? strlen(list) : 0] = '\0';
            }
            char *tok = list;
            char *best = NULL;
            while (tok) {
                char *comma = strchr(tok, ',');
                if (comma) *comma = '\0';
                while (*tok == ' ') ++tok;
                if (*tok) {
                    if (strcasecmp(tok, "auth") == 0) {
                        best = tok;
                        break;
                    } else if (!best) {
                        best = tok;
                    }
                }
                tok = comma ? comma + 1 : NULL;
            }
            if (best) {
                int nq = snprintf(out->qop, sizeof(out->qop), "%s", best);
                if (nq < 0 || (size_t)nq >= sizeof(out->qop)) {
                    /* best 超长（极端）：直接置空让 caller 按无 qop 处理 */
                    out->qop[0] = '\0';
                }
            }
        }
    }
    if (!out->algorithm[0]) {
        strcpy(out->algorithm, "MD5");
    } else if (strcasecmp(out->algorithm, "MD5") != 0) {
        /* MD5-sess / SHA-256 等暂不支持 */
        return NSBC_NOTSUP;
    }
    return NSBC_OK;
}

int digest_client_response_ha1(const char *ha132,
                               const char *method, const char *uri,
                               const char *nonce,
                               const char *nc, const char *cnonce,
                               const char *qop,
                               char resp33[SIP_RESP_LEN])
{
    if (!ha132 || !method || !uri || !nonce || !cnonce || !resp33)
        return NSBC_INVAL;

    /* HA2 = H(method:uri) */
    char ha2[33];
    nsbc_md5_hex_n(ha2, 3, method, ":", uri);

    nsbc_md5_t c;
    nsbc_md5_init(&c);
    char sep = ':';
    const char *parts[6] = {ha132, nonce};
    int nparts = 2;
    if (qop && qop[0]) {
        if (!nc || !*nc) return NSBC_INVAL;
        parts[nparts++] = nc;
        parts[nparts++] = cnonce;
        parts[nparts++] = qop;
    }
    parts[nparts++] = ha2;

    for (int i = 0; i < nparts; ++i) {
        if (i) nsbc_md5_update(&c, &sep, 1);
        nsbc_md5_update(&c, parts[i], strlen(parts[i]));
    }
    uint8_t dg[16];
    nsbc_md5_final(&c, dg);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        resp33[i * 2] = hx[dg[i] >> 4];
        resp33[i * 2 + 1] = hx[dg[i] & 15];
    }
    resp33[32] = '\0';
    return NSBC_OK;
}

int digest_client_build_auth(const char *username, const char *realm,
                             const char *nonce, const char *opaque,
                             const char *qop,
                             const char *nc, const char *cnonce,
                             const char *method, const char *uri,
                             const char *ha1_or_pass, int is_ha1,
                             char *out, size_t outsz)
{
    if (!username || !realm || !nonce || !method || !uri
        || !ha1_or_pass || !out || outsz == 0) return NSBC_INVAL;

    char ha1[33];
    if (is_ha1) {
        if (strlen(ha1_or_pass) != 32) return NSBC_INVAL;
        memcpy(ha1, ha1_or_pass, 32);
        ha1[32] = '\0';
    } else {
        digest_make_ha1(username, realm, ha1_or_pass, ha1);
    }

    char resp33[SIP_RESP_LEN];
    int rc = digest_client_response_ha1(ha1, method, uri, nonce,
                                         nc, cnonce, qop, resp33);
    if (rc != NSBC_OK) return rc;

    /* Authorization: Digest username="u", realm="r", nonce="n", uri="u",
     *   [qop=auth,] nc=00000001, cnonce="c", response="resp",
     *   algorithm=MD5[, opaque="o"]
     * 返回值：成功 NSBC_OK，截断 NSBC_NOSPC。snprintf 返回的是字符数，不能当 rc */
    int n;
    if (qop && qop[0]) {
        if (opaque && opaque[0]) {
            n = snprintf(out, outsz,
                "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
                "uri=\"%s\", qop=%s, nc=%s, cnonce=\"%s\", response=\"%s\", "
                "algorithm=MD5, opaque=\"%s\"",
                username, realm, nonce, uri, qop, nc, cnonce, resp33, opaque);
        } else {
            n = snprintf(out, outsz,
                "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
                "uri=\"%s\", qop=%s, nc=%s, cnonce=\"%s\", response=\"%s\", "
                "algorithm=MD5",
                username, realm, nonce, uri, qop, nc, cnonce, resp33);
        }
    } else {
        if (opaque && opaque[0]) {
            n = snprintf(out, outsz,
                "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
                "uri=\"%s\", response=\"%s\", algorithm=MD5, opaque=\"%s\"",
                username, realm, nonce, uri, resp33, opaque);
        } else {
            n = snprintf(out, outsz,
                "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
                "uri=\"%s\", response=\"%s\", algorithm=MD5",
                username, realm, nonce, uri, resp33);
        }
    }
    if (n < 0 || (size_t)n >= outsz) return NSBC_NOSPC;
    return NSBC_OK;
}
