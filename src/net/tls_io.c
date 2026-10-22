#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "tls_io.h"

#ifdef NSBC_WITH_OPENSSL
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "openssl_compat.h"   /* 版本兼容层（自动 include err.h/ssl.h/evp.h）*/

struct tls_ctx {
    SSL_CTX *ssl;
};

struct tls_conn {
    SSL *ssl;
    int fd;
};

tls_ctx_t *tls_ctx_server_new(const tls_cfg_t *cfg)
{
    FILE *cert_fp = NULL, *key_fp = NULL;
    tls_ctx_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;

    /* ---------- 前置检查：文件可读 + errno ---------- */
    fprintf(stderr, "[tls] cert=%s  key=%s\n",
            cfg->cert_pem ? cfg->cert_pem : "(null)",
            cfg->key_pem  ? cfg->key_pem  : "(null)");
    if (!cfg->cert_pem || !cfg->cert_pem[0]) {
        fprintf(stderr, "[tls] cert_pem 为空\n");
        goto fail;
    }
    cert_fp = fopen(cfg->cert_pem, "rb");
    if (!cert_fp) {
        fprintf(stderr, "[tls] 证书读失败: %s (errno=%d: %s)\n",
                cfg->cert_pem, errno, strerror(errno));
        goto fail;
    }
    fclose(cert_fp); cert_fp = NULL;

    if (!cfg->key_pem || !cfg->key_pem[0]) {
        fprintf(stderr, "[tls] key_pem 为空\n");
        goto fail;
    }
    key_fp = fopen(cfg->key_pem, "rb");
    if (!key_fp) {
        fprintf(stderr, "[tls] 私钥读失败: %s (errno=%d: %s)\n",
                cfg->key_pem, errno, strerror(errno));
        goto fail;
    }
    fclose(key_fp); key_fp = NULL;

    /* ---------- OpenSSL 全局初始化（1.0.x 必须；1.1+ 幂等安全）---------- */
    nsbc_openssl_init();

    /* ---------- OpenSSL TLS 上下文 ---------- */
    c->ssl = SSL_CTX_new(TLS_server_method());
    if (!c->ssl) {
        fprintf(stderr, "[tls] SSL_CTX_new 失败\n");
        ERR_print_errors_fp(stderr);
        goto fail;
    }
    /* 优先 TLS 1.3（RFC 8446），按 REQ-SIP-TCP-003 允许 1.2 回退 */
    SSL_CTX_set_min_proto_version(c->ssl,
        cfg->min_tls12 ? TLS1_2_VERSION : TLS1_3_VERSION);

    /* ---------- Cipher Suite：现代浏览器兼容 ----------
     * 优先级：ECDHE-GCM (PFS+AEAD) > ECDHE-CHACHA20 > RSA-GCM > ECDHE-CBC > RSA-CBC
     * 排除所有 EXPORT/LOW/aNULL/eNULL/MD5/3DES 等不安全套件
     * OpenSSL 1.0.2 默认 cipher list 不包含 ECDHE-GCM 优先级靠前，必须显式设
     * FIPS 模式（CentOS 7 默认）会自动剔除 CHACHA20，不影响 GCM 优先
     */
    SSL_CTX_set_cipher_list(c->ssl,
        "ECDHE-ECDSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:"
        "ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-CHACHA20-POLY1305:"
        "ECDHE-ECDSA-AES128-SHA:"
        "ECDHE-RSA-AES128-SHA:"
        "ECDHE-ECDSA-AES256-SHA:"
        "ECDHE-RSA-AES256-SHA:"
        "AES128-GCM-SHA256:"
        "AES256-GCM-SHA384:"
        "AES128-SHA:"
        "AES256-SHA");

    /* ---------- Forward Secrecy：自动选 ECDHE 曲线 ---------- */
    SSL_CTX_set_ecdh_auto(c->ssl, 1);   /* 1.0.2 原生支持 */

    /* ---------- 安全选项 ----------
     * SSL_OP_NO_COMPRESSION：防 CRIME 攻击
     * SSL_OP_SINGLE_DH_USE：每次 DH 握手重新生成临时密钥
     */
    SSL_CTX_set_options(c->ssl, SSL_OP_NO_COMPRESSION | SSL_OP_SINGLE_DH_USE);

    if (SSL_CTX_use_certificate_chain_file(c->ssl, cfg->cert_pem) != 1) {
        fprintf(stderr, "[tls] SSL_CTX_use_certificate_chain_file 失败\n");
        ERR_print_errors_fp(stderr);
        goto fail_ssl;
    }
    if (SSL_CTX_use_PrivateKey_file(c->ssl, cfg->key_pem,
                                    SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "[tls] SSL_CTX_use_PrivateKey_file 失败\n");
        ERR_print_errors_fp(stderr);
        goto fail_ssl;
    }
    /* 证书-私钥配对校验：防止证书/私钥来自不同对 */
    if (!SSL_CTX_check_private_key(c->ssl)) {
        fprintf(stderr, "[tls] 证书与私钥不匹配（check_private_key 失败）\n");
        ERR_print_errors_fp(stderr);
        goto fail_ssl;
    }

    if (cfg->ca_pem) {
        SSL_CTX_load_verify_locations(c->ssl, cfg->ca_pem, NULL);
        SSL_CTX_set_verify(c->ssl,
            cfg->require_client_cert
                ? (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT)
                : SSL_VERIFY_PEER,
            NULL);
    }
    fprintf(stderr, "[tls] OK cert=%s key=%s min_tls=%d\n",
            cfg->cert_pem, cfg->key_pem,
            cfg->min_tls12 ? 12 : 13);
    return c;
fail_ssl:
    SSL_CTX_free(c->ssl);
fail:
    if (cert_fp) fclose(cert_fp);
    if (key_fp)  fclose(key_fp);
    free(c);
    return NULL;
}

void *tls_ctx_get_ssl(tls_ctx_t *ctx)
{
    if (!ctx) return NULL;
    return ctx->ssl;
}

void tls_ctx_free(tls_ctx_t *ctx)
{
    if (!ctx)
        return;
    SSL_CTX_free(ctx->ssl);
    free(ctx);
}

tls_conn_t *tls_conn_accept(tls_ctx_t *ctx, int fd)
{
    tls_conn_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->ssl = SSL_new(ctx->ssl);
    c->fd = fd;
    SSL_set_fd(c->ssl, fd);
    return c;
}

tls_hs_state_t tls_handshake_step(tls_conn_t *c)
{
    int r = SSL_accept(c->ssl);
    if (r == 1)
        return TLS_HS_OK;
    int e = SSL_get_error(c->ssl, r);
    if (e == SSL_ERROR_WANT_READ)
        return TLS_HS_WANT_READ;
    if (e == SSL_ERROR_WANT_WRITE)
        return TLS_HS_WANT_WRITE;
    return TLS_HS_FAIL;
}

void tls_conn_free(tls_conn_t *c)
{
    if (!c)
        return;
    SSL_shutdown(c->ssl);
    SSL_free(c->ssl);
    free(c);
}

int tls_conn_fd(const tls_conn_t *c) { return c->fd; }

int tls_read(tls_conn_t *c, void *buf, size_t len, int *want_out)
{
    int r = SSL_read(c->ssl, buf, (int)len);
    if (r > 0)
        return r;
    int e = SSL_get_error(c->ssl, r);
    if (e == SSL_ERROR_WANT_READ) {
        if (want_out) *want_out = TLS_WANT_READ;
        return NSBC_AGAIN;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
        if (want_out) *want_out = TLS_WANT_WRITE;
        return NSBC_AGAIN;
    }
    if (e == SSL_ERROR_ZERO_RETURN)
        return NSBC_EOF;
    return NSBC_ERR;
}

int tls_write(tls_conn_t *c, const void *buf, size_t len, int *want_out)
{
    int r = SSL_write(c->ssl, buf, (int)len);
    if (r > 0)
        return r;
    int e = SSL_get_error(c->ssl, r);
    if (e == SSL_ERROR_WANT_WRITE) {
        if (want_out) *want_out = TLS_WANT_WRITE;
        return NSBC_AGAIN;
    }
    if (e == SSL_ERROR_WANT_READ) {
        if (want_out) *want_out = TLS_WANT_READ;
        return NSBC_AGAIN;
    }
    return NSBC_ERR;
}

#else /* ---------- 未启用 OpenSSL：显式 NOTSUP，不伪造 TLS ---------- */

struct tls_ctx { int dummy; };
struct tls_conn { int dummy; };

tls_ctx_t *tls_ctx_server_new(const tls_cfg_t *cfg)
{
    (void)cfg;
    return NULL;
}
void tls_ctx_free(tls_ctx_t *ctx) { (void)ctx; }
void *tls_ctx_get_ssl(tls_ctx_t *ctx) { (void)ctx; return NULL; }
tls_conn_t *tls_conn_accept(tls_ctx_t *ctx, int fd)
{
    (void)ctx;
    (void)fd;
    return NULL;
}
tls_hs_state_t tls_handshake_step(tls_conn_t *c)
{
    (void)c;
    return TLS_HS_FAIL;
}
void tls_conn_free(tls_conn_t *c) { (void)c; }
int tls_conn_fd(const tls_conn_t *c)
{
    (void)c;
    return -1;
}
int tls_read(tls_conn_t *c, void *b, size_t l, int *w)
{
    (void)c; (void)b; (void)l; (void)w;
    return NSBC_NOTSUP;
}
int tls_write(tls_conn_t *c, const void *b, size_t l, int *w)
{
    (void)c; (void)b; (void)l; (void)w;
    return NSBC_NOTSUP;
}
#endif
