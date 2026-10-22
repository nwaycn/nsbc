/* OpenSSL 全版本兼容层——证书生成 / DTLS-SRTP 终结
 *
 * 只暴露 nsbc_ssl.h 声明的业务 API，版本差异全在本文件内部处理。
 * 所有 SSL_CTX / SSL / EVP_PKEY 在本文件内部持有，对外 void* 包装。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nsbc_ssl.h"
#ifdef NSBC_WITH_OPENSSL
#include "openssl_compat.h"
#endif
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef NSBC_WITH_OPENSSL
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/obj_mac.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

/* ====================================================================
 *  DTLS 自签证书池（lazy-static，进程生命周期内只生成 1 张）
 *  WebRTC DTLS-SRTP 用证书指纹 pinning（跳过 CA 链验证），
 *  一张证书可以无限次复用——每通现生成纯属浪费且可能阻塞。
 * ==================================================================== */
static pthread_once_t g_dtls_once = PTHREAD_ONCE_INIT;
static char   g_dtls_cert_pem[NSBC_SSL_PEM_BUF_SIZE];
static char   g_dtls_key_pem[NSBC_SSL_PEM_BUF_SIZE];
static char   g_dtls_fp[NSBC_SSL_FP_SHA256_SIZE];
static int    g_dtls_ready = 0;

static void nsbc_dtls_cert_pool_init(void)
{
    llog(LOG_INFO, "ssl", "dtls_cert_pool: INIT START (pthread_once)");
    /* 关键：从 /dev/urandom seed 64 字节给 OpenSSL，彻底消除
     * RAND_priv_bytes 等 /dev/random 熵等待（headless 服务器熵常耗尽）。 */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        unsigned char seed[64];
        ssize_t n = read(fd, seed, sizeof(seed));
        close(fd);
        if (n > 0) RAND_seed(seed, (int)n);
    }
    g_dtls_ready = (nsbc_ssl_gen_self_signed(
                       g_dtls_cert_pem, sizeof(g_dtls_cert_pem),
                       g_dtls_key_pem,  sizeof(g_dtls_key_pem),
                       g_dtls_fp,       sizeof(g_dtls_fp)) == NSBC_OK);
    llog(LOG_INFO, "ssl", "dtls_cert_pool: pre-gen %s fp=%s",
         g_dtls_ready ? "OK" : "FAIL", g_dtls_ready ? g_dtls_fp : "(none)");
}
#endif

/* ====================================================================
 *  内部：EC keygen（P-256 Prime256v1）
 *  版本差异巨大，三选一
 * ==================================================================== */
#ifdef NSBC_WITH_OPENSSL
static EVP_PKEY *nsbc_ssl_gen_pkey(void)
{
    /* DTLS-SRTP 对证书算法不挑（RSA/EC 都行）。
     * 优先 EC P-256：keygen 只要 ~1ms——RSA-2048 要 100~400ms，而每通
     * 呼叫每条腿都要现生成一张证书，RSA 会把接续拖慢几百毫秒。
     * 之前怀疑"EC key 在 3.0 上签名失败"是误判（X509_sign 返回签名长度，
     * `!= 1` 把成功当失败），EC 本身没问题；RSA-2048 仅作兜底。 */

#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x10100000L
    /* 1.0.x legacy EC 兜底 */
    EC_KEY *ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!ec) return NULL;
    if (EC_KEY_generate_key(ec) != 1) { EC_KEY_free(ec); return NULL; }
    EVP_PKEY *key = EVP_PKEY_new();
    if (!key) { EC_KEY_free(ec); return NULL; }
    if (EVP_PKEY_assign_EC_KEY(key, ec) != 1) {
        EC_KEY_free(ec); EVP_PKEY_free(key); return NULL;
    }
    return key;
#else
    ERR_clear_error();
    /* 1.1+ 优先 EC P-256：~1ms keygen，接续无感 */
    {
        EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
        if (pctx) {
            EVP_PKEY *key = NULL;
            if (EVP_PKEY_keygen_init(pctx) > 0
                && EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) > 0
                && EVP_PKEY_keygen(pctx, &key) > 0) {
                EVP_PKEY_CTX_free(pctx);
                return key;
            }
            EVP_PKEY_CTX_free(pctx);
        }
    }
    ERR_clear_error();
    /* EC 不行再 RSA-2048 兜底（default provider 必有） */
    {
        EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
        if (pctx) {
            EVP_PKEY *key = NULL;
            if (EVP_PKEY_keygen_init(pctx) > 0
                && EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) > 0
                && EVP_PKEY_keygen(pctx, &key) > 0) {
                EVP_PKEY_CTX_free(pctx);
                return key;
            }
            EVP_PKEY_CTX_free(pctx);
        }
    }
    return NULL;
#endif
}
#endif

/* ====================================================================
 *  自签 X509 证书生成
 * ==================================================================== */
int nsbc_ssl_gen_self_signed(char *cert_pem, size_t cert_sz,
                             char *key_pem, size_t key_sz,
                             char *fp_sha256, size_t fp_sz)
{
#ifdef NSBC_WITH_OPENSSL
    if (!cert_pem || !key_pem || !fp_sha256 ||
        cert_sz < 64 || key_sz < 64 || fp_sz < 32)
        return NSBC_INVAL;

    /* 每个 OpenSSL 操作前清空 error queue，失败后立即 dump */
    ERR_clear_error();
    EVP_PKEY *key = nsbc_ssl_gen_pkey();
    if (!key) {
        llog(LOG_ERR, "ssl", "gen_self_signed: pkey gen FAIL (EC+RSA)");
        ERR_print_errors_fp(stderr);
        return NSBC_ERR;
    }

    ERR_clear_error();
    X509 *x = X509_new();
    if (!x) {
        llog(LOG_ERR, "ssl", "gen_self_signed: X509_new FAIL");
        ERR_print_errors_fp(stderr);
        EVP_PKEY_free(key); return NSBC_ERR;
    }

    X509_set_version(x, 2);
    /* 序列号：ASN1_INTEGER_new + set（1.0/1.1/3.0 统一） */
    ASN1_INTEGER *ser = ASN1_INTEGER_new();
    ASN1_INTEGER_set(ser, 1);
    X509_set_serialNumber(x, ser);
    ASN1_INTEGER_free(ser);

    X509_NAME *subj = X509_NAME_new();
    X509_NAME_add_entry_by_txt(subj, "CN", MBSTRING_ASC,
                               (const unsigned char *)"nsbc-media", -1, -1, 0);
    X509_set_issuer_name(x, subj);
    X509_set_subject_name(x, subj);
    X509_NAME_free(subj);

    ASN1_TIME *nb = X509_getm_notBefore(x);
    ASN1_TIME *na = X509_getm_notAfter(x);
    X509_gmtime_adj(nb, 0);                      /* 现在 */
    X509_gmtime_adj(na, 60*60*24*365*2);         /* 2 年 */

    ERR_clear_error();
    if (X509_set_pubkey(x, key) != 1) {
        llog(LOG_ERR, "ssl", "gen_self_signed: X509_set_pubkey FAIL");
        ERR_print_errors_fp(stderr);
        X509_free(x); EVP_PKEY_free(key); return NSBC_ERR;
    }

    ERR_clear_error();
    /* 签名：全版本统一用 X509_sign + EVP_sha256()。
     * 返回值语义：成功返回签名长度（RSA-2048=256，ECDSA-P256≈70），
     * 失败返回 0 —— 绝不能和 1 比较！之前 `!= 1` 把每次成功都判成
     * "X509_sign FAIL"，DTLS 永远建不起来、通通降级 BYPASS，根因就是它
     * （旁证：FAIL 后错误队列是空的，ERR_print_errors_fp 什么都不打）。 */
#pragma GCC diagnostic push
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    if (X509_sign(x, key, EVP_sha256()) <= 0) {
        llog(LOG_ERR, "ssl", "gen_self_signed: X509_sign FAIL");
        ERR_print_errors_fp(stderr);
        X509_free(x); EVP_PKEY_free(key); return NSBC_ERR;
    }
#pragma GCC diagnostic pop

    /* PEM 编码：用 BIO_mem + BIO_get_mem_data（OpenSSL 3.0 安全，避免 BUF_MEM 内部字段差异） */
    BIO *cb = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(cb, x);
    const unsigned char *cert_data = NULL;
    long cert_len = BIO_get_mem_data(cb, (char **)&cert_data);
    size_t cl = (cert_len > 0 && (size_t)cert_len < cert_sz - 1) ? (size_t)cert_len : cert_sz - 1;
    memcpy(cert_pem, cert_data, cl);
    cert_pem[cl] = '\0';
    BIO_free(cb);

    BIO *kb = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(kb, key, NULL, NULL, 0, NULL, NULL);
    const unsigned char *key_data = NULL;
    long key_len = BIO_get_mem_data(kb, (char **)&key_data);
    size_t kl = (key_len > 0 && (size_t)key_len < key_sz - 1) ? (size_t)key_len : key_sz - 1;
    memcpy(key_pem, key_data, kl);
    key_pem[kl] = '\0';
    BIO_free(kb);

    /* SHA-256 指纹（hex + 冒号） */
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    X509_digest(x, EVP_sha256(), md, &md_len);
    static const char hx[] = "0123456789abcdef";
    char *out = fp_sha256;
    for (unsigned i = 0; i < md_len; i++) {
        *out++ = hx[(md[i] >> 4) & 0xF];
        *out++ = hx[md[i] & 0xF];
        if (i + 1 < md_len) *out++ = ':';
    }
    *out = '\0';

    X509_free(x);
    EVP_PKEY_free(key);
    return NSBC_OK;
#else
    (void)cert_pem; (void)cert_sz; (void)key_pem; (void)key_sz;
    (void)fp_sha256; (void)fp_sz;
    return NSBC_NOTSUP;
#endif
}

/* ====================================================================
 *  nsbc_dtls 不透明句柄（内部：SSL_CTX + SSL + BIO）
 * ==================================================================== */
struct nsbc_dtls {
    int fd;
#ifdef NSBC_WITH_OPENSSL
    SSL_CTX *ctx;
    SSL *ssl;
    BIO *bio;
    int established;
#endif
};

/* ★ REQ-MEDIA-SEC-005：DTLS-SRTP 专用 verify callback。
 * DTLS-SRTP 场景下两端都用自签名证书，OpenSSL 默认 X509_verify_cert
 * 走 CA 链校验 → 自签名永远过不了 → 握手 abort。
 * 这里永远返回 1 接受任意证书；证书指纹校验走独立路径
 * dtls_verify_peer_fp()（dtls_srtp.c 里，对比 set_peer_fp 注入的期望 fp）。 */
static int nsbc_verify_always_ok(int preverify_ok, X509_STORE_CTX *ctx)
{
    (void)preverify_ok;
    (void)ctx;
    return 1;
}

int nsbc_ssl_dtls_server_new(int fd, nsbc_dtls_hdl_t *out,
                              const char *srtp_profiles,
                              char *fp_sha256, size_t fp_sz)
{
#ifdef NSBC_WITH_OPENSSL
    llog(LOG_INFO, "ssl", "dtls_server_new: ENTER fd=%d", fd);
    if (!out) return NSBC_INVAL;
    nsbc_openssl_init();  /* 幂等，确保 3.0 provider 加载 */
    llog(LOG_INFO, "ssl", "dtls_server_new: openssl_init OK");
    nsbc_dtls_hdl_t h = calloc(1, sizeof(*h));
    if (!h) return NSBC_NOMEM;
    h->fd = fd;

    /* 确保非阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* 用预生成 DTLS 证书池（pthread_once，只生成 1 张进程复用）。
     * 彻底消除每通呼叫都调 RAND_priv_bytes → 等 /dev/random 熵 →
     * 阻塞 SIP 线程几十秒的死锁假象。 */
    pthread_once(&g_dtls_once, nsbc_dtls_cert_pool_init);
    if (!g_dtls_ready) {
        llog(LOG_ERR, "ssl", "dtls_server_new: cert pool NOT ready");
        free(h); return NSBC_ERR;
    }
    if (fp_sha256 && fp_sz > 0) {
        size_t copylen = fp_sz - 1;
        if (copylen > sizeof(g_dtls_fp) - 1) copylen = sizeof(g_dtls_fp) - 1;
        memcpy(fp_sha256, g_dtls_fp, copylen);
        fp_sha256[copylen] = '\0';
    }
    llog(LOG_INFO, "ssl", "dtls_server_new: using pre-gen cert fp=%s", g_dtls_fp);

    h->ctx = SSL_CTX_new(DTLS_server_method());
    if (!h->ctx) {
        llog(LOG_ERR, "ssl", "dtls_server_new: SSL_CTX_new FAIL");
        free(h); return NSBC_ERR;
    }
    /* BIO_mem 载入预生成证书 + 私钥（进程复用同一张） */
    BIO *cb = BIO_new_mem_buf(g_dtls_cert_pem, -1);
    BIO *kb = BIO_new_mem_buf(g_dtls_key_pem, -1);
    X509 *x = PEM_read_bio_X509(cb, NULL, NULL, NULL);
    EVP_PKEY *k = PEM_read_bio_PrivateKey(kb, NULL, NULL, NULL);
    BIO_free(cb); BIO_free(kb);
    SSL_CTX_use_certificate(h->ctx, x);
    SSL_CTX_use_PrivateKey(h->ctx, k);
    X509_free(x); EVP_PKEY_free(k);

    /* ★ REQ-MEDIA-SEC-005：DTLS Server 必须主动请求对端证书。
     * SSL_VERIFY_PEER → 服务器发 CertificateRequest → 浏览器（DTLS Client）
     * 才会发送自己的自签名证书。挂 nsbc_verify_always_ok 绕开默认 CA 链校验
     * （自签名过不了 X509_verify_cert），fp 校验走独立路径 dtls_verify_peer_fp()。 */
    SSL_CTX_set_verify(h->ctx,
        SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
        nsbc_verify_always_ok);
    SSL_CTX_set_mode(h->ctx, SSL_MODE_RELEASE_BUFFERS);
    SSL_CTX_set_options(h->ctx, SSL_OP_NO_QUERY_MTU);

    /* SRTP profiles —— 用 SSL_CTX_set_tlsext_use_srtp（OpenSSL 全版本通用：
     *   1.0.2: 只支持 CM_SHA1_80/32，不支持 AEAD；
     *   1.1.x+: 原生支持全部；3.0+ 同样。
     * profiles 参数是 colon-delimited 字符串如 "SRTP_AES128_CM_SHA1_80:SRTP_AEAD_AES_128_GCM"。
     * 策略：先试调用方传的完整列表，失败则 fallback 到 CM_SHA1_80 only。 */
    if (srtp_profiles && srtp_profiles[0]) {
        if (SSL_CTX_set_tlsext_use_srtp(h->ctx, srtp_profiles) != 0) {
            llog(LOG_WARN, "ssl", "SSL_CTX_set_tlsext_use_srtp FAIL '%s', "
                 "trying CM_SHA1_80 fallback...", srtp_profiles);
            ERR_print_errors_fp(stderr);
            static const char *fallback = "SRTP_AES128_CM_SHA1_80";
            if (SSL_CTX_set_tlsext_use_srtp(h->ctx, fallback) != 0) {
                llog(LOG_ERR, "ssl", "SSL_CTX_set_tlsext_use_srtp fallback "
                     "also FAIL — DTLS-SRTP disabled");
                ERR_print_errors_fp(stderr);
                SSL_CTX_free(h->ctx);
                free(h);
                return NSBC_ERR;
            }
            srtp_profiles = fallback;
        }
    }

    h->ssl = SSL_new(h->ctx);
    if (!h->ssl) {
        llog(LOG_ERR, "ssl", "dtls_server_new: SSL_new FAIL");
        SSL_CTX_free(h->ctx); free(h); return NSBC_ERR;
    }
    llog(LOG_DEBUG, "ssl", "dtls_server_new: SSL_new OK");
    if (srtp_profiles && srtp_profiles[0])
        SSL_set_tlsext_use_srtp(h->ssl, srtp_profiles);
    SSL_set_mtu(h->ssl, 1460);

    /* BIO_dgram 包装已有 fd（BIO_NOCLOSE：fd 仍由 reactor 管理） */
    h->bio = BIO_new_dgram(fd, BIO_NOCLOSE);
    if (!h->bio) {
        llog(LOG_ERR, "ssl", "dtls_server_new: BIO_new_dgram FAIL");
        SSL_free(h->ssl); SSL_CTX_free(h->ctx); free(h); return NSBC_ERR;
    }
    llog(LOG_DEBUG, "ssl", "dtls_server_new: BIO_new_dgram OK");
    BIO_set_nbio(h->bio, 1);
    SSL_set_bio(h->ssl, h->bio, h->bio);

    *out = h;
    llog(LOG_DEBUG, "ssl", "dtls_server_new: SUCCESS fd=%d", fd);
    return NSBC_OK;
#else
    (void)fd; (void)out; (void)srtp_profiles;
    return NSBC_NOTSUP;
#endif
}

void nsbc_ssl_dtls_destroy(nsbc_dtls_hdl_t h)
{
#ifdef NSBC_WITH_OPENSSL
    if (!h) return;
    /* SSL_set_bio 后 BIO_free_all 同时清 SSL 的 bio —— 只需要顺序
     * SSL_free(先) 再 SSL_CTX_free(后)。BIO_dgram 的 fd 由 BIO_NOCLOSE
     * 保证不 close；我们也不 free BIO 以免 double-free。 */
    if (h->ssl) SSL_free(h->ssl);
    if (h->ctx) SSL_CTX_free(h->ctx);
    free(h);
#else
    (void)h;
#endif
}

int nsbc_ssl_dtls_handshake(nsbc_dtls_hdl_t h)
{
#ifdef NSBC_WITH_OPENSSL
    if (!h || !h->ssl) return NSBC_INVAL;
    if (h->established) return NSBC_OK;
    int rc = SSL_accept(h->ssl);
    if (rc == 1) {
        h->established = 1;
        return NSBC_OK;
    }
    int err = SSL_get_error(h->ssl, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return NSBC_AGAIN;
    return NSBC_ERR;
#else
    (void)h;
    return NSBC_NOTSUP;
#endif
}

int nsbc_ssl_dtls_feed(nsbc_dtls_hdl_t hdl, const uint8_t *buf, size_t len)
{
#ifdef NSBC_WITH_OPENSSL
    if (!hdl || !hdl->bio || !buf || len == 0) return NSBC_INVAL;
    if (hdl->established) return NSBC_OK;

    /* BIO_dgram 绑定真实 fd——DTLS 数据通过 SSL_read 内部 recvfrom 获取，
     * 不需要也没有 BIO_CTRL_DGRAM_SET_DATA 这样的 ctrl。
     * feed 模型只有在 BIO_dgram_pair（OpenSSL 1.1.1+）或 BIO_s_mem + 自定义
     * transport 时才有意义——当前实现用 BIO_new_dgram(fd)，SSL_accept 直接
     * 从 fd 读 UDP，feed 在这里是空操作。 */
    (void)buf; (void)len;
    return NSBC_OK;
#else
    (void)hdl; (void)buf; (void)len;
    return NSBC_NOTSUP;
#endif
}

int nsbc_ssl_dtls_established(nsbc_dtls_hdl_t h)
{
#ifdef NSBC_WITH_OPENSSL
    return h && h->established ? 1 : 0;
#else
    (void)h; return 0;
#endif
}

int nsbc_ssl_dtls_peer_fp(nsbc_dtls_hdl_t hdl, char *out, size_t outsz)
{
#ifdef NSBC_WITH_OPENSSL
    if (!hdl || !hdl->ssl || !out || outsz < NSBC_SSL_FP_SHA256_SIZE)
        return NSBC_INVAL;
    if (!hdl->established)
        return NSBC_ERR;

    X509 *peer = NSBC_SSL_PEER_CERT(hdl->ssl);
    if (!peer) {
        llog(LOG_ERR, "ssl", "peer cert unavailable fd=%d", hdl->fd);
        return NSBC_ERR;
    }
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    if (!X509_digest(peer, EVP_sha256(), md, &md_len)) {
        llog(LOG_ERR, "ssl", "X509_digest SHA-256 FAIL fd=%d", hdl->fd);
        NSBC_SSL_PEER_CERT_FREE(peer);
        return NSBC_ERR;
    }
    NSBC_SSL_PEER_CERT_FREE(peer);

    /* 小写 hex + 冒号（与 gen_self_signed 中本地证书同格式）
     * outsz 在入口已断言 >= NSBC_SSL_FP_SHA256_SIZE (96)，
     * 调用方保证空间充足，不需要额外边界条件。 */
    static const char hx[] = "0123456789abcdef";
    char *p = out;
    for (unsigned i = 0; i < md_len; ++i) {
        *p++ = hx[(md[i] >> 4) & 0xF];
        *p++ = hx[md[i] & 0xF];
        if (i + 1 < md_len) *p++ = ':';
    }
    *p = '\0';
    return NSBC_OK;
#else
    (void)hdl; (void)out; (void)outsz;
    return NSBC_NOTSUP;
#endif
}

int nsbc_ssl_dtls_export_srtp(nsbc_dtls_hdl_t hdl,
                               nsbc_srtp_material_t *sending,
                               nsbc_srtp_material_t *receiving)
{
#ifdef NSBC_WITH_OPENSSL
    if (!hdl || !hdl->ssl || !hdl->established) return NSBC_INVAL;
    if (!sending || !receiving) return NSBC_INVAL;

    /* 探测协商到的 SRTP profile：
     * 1.1.1+ 有 SSL_get_selected_srtp_profile() → 拿 profile id
     * 1.0.x 无此 API → 默认 AEAD_AES_128_GCM（但 1.0.x + 现代浏览器
     *   握手阶段就会因 profile 不匹配失败，不会走到这里） */
    int profile_id = 0x0007; /* 默认 SRTP_AEAD_AES_128_GCM */
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10101000L
    SRTP_PROTECTION_PROFILE *sel = SSL_get_selected_srtp_profile(hdl->ssl);
    if (sel) profile_id = sel->id;
#endif

    /* RFC 5764 §4.2 exporter 输出是交错格式（key 和 salt 分开排列）：
     *
     *   bytes[0..key_len-1]              = client_write_key
     *   bytes[key_len..2*key_len-1]      = server_write_key
     *   bytes[2*key_len..2*key_len+salt_len-1]        = client_write_salt
     *   bytes[2*key_len+salt_len..2*key_len+2*salt_len-1] = server_write_salt
     *
     * 总输出 = 2*key_len + 2*salt_len
     *
     * libsrtp2 的 srtp_policy_t.key 期望 master key = key || salt 拼接。
     * 所以我们需要把交错格式重新拼接成连续的 key||salt：
     *   receiving (client_write) = client_write_key || client_write_salt
     *   sending   (server_write) = server_write_key || server_write_salt
     *
     * 之前 Bug：直接按 keylen 连续切半（memcpy raw[0..28] 和 raw[28..56]），
     * 完全忽略了 key/salt 交错排列的 RFC 格式 → 生成的 master key 是
     * client_key+server_key前12字节这种垃圾组合 → SRTP 解密/加密必然错。 */
    int key_len, salt_len, keylen;
    switch (profile_id) {
    case 0x0007:  /* SRTP_AEAD_AES_128_GCM */
        key_len = 16; salt_len = 12; keylen = 28; break;
    case 0x0008:  /* SRTP_AEAD_AES_256_GCM */
        key_len = 32; salt_len = 12; keylen = 44; break;
    case 0x0001:  /* SRTP_AES128_CM_SHA1_80 */
    case 0x0002:  /* SRTP_AES128_CM_SHA1_32 */
        key_len = 16; salt_len = 14; keylen = 30; break;
    default:
        llog(LOG_ERR, "dtls", "Unknown SRTP profile id=0x%04x", profile_id);
        return NSBC_ERR;
    }
    int total = 2 * key_len + 2 * salt_len;
    uint8_t raw[96] = {0};  /* 最多 2*32+2*14=92 */
    if (total > (int)sizeof(raw)) {
        llog(LOG_ERR, "dtls", "SRTP exporter buffer too small total=%d", total);
        return NSBC_ERR;
    }

    int rc = SSL_export_keying_material(hdl->ssl,
                                     raw, (size_t)total,
                                     "EXTRACTOR-dtls_srtp", 19,
                                     NULL, 0, 0);  /* use_context=0 是 RFC 5764 要求 */
    if (rc != 1) { llog(LOG_ERR, "dtls", "SSL_export_keying_material FAIL total=%d", total); return NSBC_ERR; }

    /* 按 RFC 5764 交错格式拼接成连续 master key (key||salt) */
    uint8_t *ck = raw;                          /* client_write_key */
    uint8_t *sk = raw + key_len;                /* server_write_key */
    uint8_t *cs = raw + 2 * key_len;            /* client_write_salt */
    uint8_t *ss = raw + 2 * key_len + salt_len; /* server_write_salt */

    /* receiving (我们收) = client_write_key || client_write_salt */
    memcpy(receiving->key, ck, key_len);
    memcpy(receiving->key + key_len, cs, salt_len);
    receiving->keylen = keylen;

    /* sending (我们发) = server_write_key || server_write_salt */
    memcpy(sending->key, sk, key_len);
    memcpy(sending->key + key_len, ss, salt_len);
    sending->keylen = keylen;

    memcpy(sending->label,   "EXTRACTOR-dtls_srtp", 19);
    memcpy(receiving->label, "EXTRACTOR-dtls_srtp", 19);

    /* ★ 诊断：打印完整 keylen 字节 hex，和 Wireshark/Chrome 对比 */
    {
        char snd_hex[128] = {0}, rcv_hex[128] = {0};
        for (int i = 0; i < keylen && i * 3 < 127; ++i) {
            snd_hex[i*3]   = "0123456789abcdef"[(sending->key[i] >> 4) & 0xF];
            snd_hex[i*3+1] = "0123456789abcdef"[sending->key[i] & 0xF];
            snd_hex[i*3+2] = (i + 1 < keylen && (i + 1) * 3 < 127) ? ' ' : '\0';
            rcv_hex[i*3]   = "0123456789abcdef"[(receiving->key[i] >> 4) & 0xF];
            rcv_hex[i*3+1] = "0123456789abcdef"[receiving->key[i] & 0xF];
            rcv_hex[i*3+2] = (i + 1 < keylen && (i + 1) * 3 < 127) ? ' ' : '\0';
        }
        llog(LOG_DEBUG, "dtls",
             "SRTP keys: sending=[%s] receiving=[%s] profile=0x%04x keylen=%d",
             snd_hex, rcv_hex, profile_id, keylen);
    }

    return NSBC_OK;
#else
    (void)hdl; (void)sending; (void)receiving;
    return NSBC_NOTSUP;
#endif
}

void *nsbc_ssl_dtls_get_ssl(nsbc_dtls_hdl_t h)
{
#ifdef NSBC_WITH_OPENSSL
    return h ? h->ssl : NULL;
#else
    (void)h; return NULL;
#endif
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
