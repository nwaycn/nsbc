/* SDP 安全与兼容派生：
 * - 带宽行 b=AS/CT/IA/TIAS/X-* 查找
 * - DTLS-SRTP（RFC 5763）：a=setup 角色、a=fingerprint 提取
 * - SDES-SRTP（RFC 4568）：a=crypto 存在性
 * - 旧式 hold：c=0.0.0.0 / c=::（RFC 3264 前实现） */

#include "sip_common.h"
#include "sdp.h"

const sdp_bw_t *sdp_bw_find(const sdp_bw_t *b, int n, const char *type)
{
    size_t len = strlen(type);
    for (int i = 0; i < n; ++i)
        if (b[i].type.len == len && sip_ci_eq(b[i].type.p, type, len))
            return &b[i];
    return NULL;
}

static dtls_setup_t setup_of(const sdp_attr_t *a, int n)
{
    const sdp_attr_t *p = sdp_attr_find(a, n, "setup");
    if (!p || p->val.p == NULL)
        return DTLS_SETUP_UNKNOWN;
    const sip_tok_t *v = &p->val;
    if (v->len == 7 && sip_ci_eq(v->p, "actpass", 7))
        return DTLS_SETUP_ACTPASS;
    if (v->len == 6 && sip_ci_eq(v->p, "active", 6))
        return DTLS_SETUP_ACTIVE;
    if (v->len == 7 && sip_ci_eq(v->p, "passive", 7))
        return DTLS_SETUP_PASSIVE;
    if (v->len == 8 && sip_ci_eq(v->p, "holdconn", 8))
        return DTLS_SETUP_HOLDCONN;
    return DTLS_SETUP_UNKNOWN;
}

dtls_setup_t sdp_media_dtls_setup(const sdp_t *s, const sdp_media_t *m)
{
    if (m) {
        dtls_setup_t d = setup_of(m->attrs, m->nattrs);
        if (d != DTLS_SETUP_UNKNOWN)
            return d;
    }
    return setup_of(s->attrs, s->nattrs);
}

/* fingerprint:<hash-algo> SP <fingerprint>；先查媒体段再回落会话段 */
static nsbc_rc_t fp_from(const sdp_attr_t *a, int n, sip_tok_t *algo,
                         sip_tok_t *fp)
{
    const sdp_attr_t *p = sdp_attr_find(a, n, "fingerprint");
    if (!p || p->val.p == NULL)
        return NSBC_ERR;
    const char *v = p->val.p;
    const char *end = v + p->val.len;
    const char *sp = v;
    while (sp < end && !sip_is_sp(*sp))
        ++sp;
    if (sp == v || sp + 1 >= end)
        return NSBC_ERR;
    algo->p = v;
    algo->len = (size_t)(sp - v);
    fp->p = sp + 1;
    fp->len = (size_t)(end - sp - 1);
    if (fp->len == 0)
        return NSBC_ERR;
    return NSBC_OK;
}

nsbc_rc_t sdp_fingerprint_get(const sdp_t *s, const sdp_media_t *m,
                              sip_tok_t *algo, sip_tok_t *fp)
{
    if (m && fp_from(m->attrs, m->nattrs, algo, fp) == NSBC_OK)
        return NSBC_OK;
    return fp_from(s->attrs, s->nattrs, algo, fp);
}

int sdp_media_has_crypto(const sdp_media_t *m)
{
    return sdp_attr_find(m->attrs, m->nattrs, "crypto") != NULL;
}

int sdp_media_is_legacy_hold(const sdp_t *s, const sdp_media_t *m)
{
    const sdp_conn_t *c = sdp_media_effective_conn(s, m);
    if (!c)
        return 0;
    /* IPv4 0.0.0.0；IPv6 未指定地址 :: */
    return (c->addr.len == 7 && memcmp(c->addr.p, "0.0.0.0", 7) == 0) ||
           (c->addr.len == 2 && memcmp(c->addr.p, "::", 2) == 0);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
