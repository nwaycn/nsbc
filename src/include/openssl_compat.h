/* ----------------------------------------------------------------------------
 * NSBC OpenSSL 版本兼容层
 * ----------------------------------------------------------------------------
 * 目标矩阵：
 *   CentOS 7.6  ->  OpenSSL 1.0.2k (fips)
 *   CentOS 8     ->  OpenSSL 1.1.1k
 *   Debian 11/12 ->  OpenSSL 1.1.1n / 3.0.x
 *   Debian 13    ->  OpenSSL 3.5.x
 *
 * 只做"缺什么补什么"：新版本有就直接用，旧版本缺的用宏/内联包装。
 * 不引入任何运行期分发（没有 dlsym 也没有运行时版本探测）。
 *
 * 使用方式：NSBC_WITH_OPENSSL 分支内 include "openssl_compat.h"，
 * 在 openssl 头文件之后 include（本文件自己会 include 所需 openssl 头）。
 * ---------------------------------------------------------------------------- */
#ifndef NSBC_OPENSSL_COMPAT_H
#define NSBC_OPENSSL_COMPAT_H

#ifndef NSBC_WITH_OPENSSL
#  error "openssl_compat.h must be included inside #ifdef NSBC_WITH_OPENSSL"
#endif

#include <openssl/opensslv.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/crypto.h>   /* OPENSSL_malloc / OPENSSL_free */
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/bio.h>

/* ---------- 版本号速记 ----------
 *  1.0.2  = 0x10002000L
 *  1.1.0  = 0x10100000L  <- 大量 new/free API 从此版引入
 *  1.1.1  = 0x10101000L
 *  3.0.0  = 0x30000000L
 *  3.5.0  = 0x30500000L
 */

/* ====================================================================
 *  1. EVP_MD_CTX new/free
 *     1.0.x 有 EVP_MD_CTX_create/destroy -> 直接宏映射
 *     1.1.0+ 原生有 EVP_MD_CTX_new/free
 * ==================================================================== */
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x10100000L
#  ifndef EVP_MD_CTX_new
#    define EVP_MD_CTX_new()       EVP_MD_CTX_create()
#  endif
#  ifndef EVP_MD_CTX_free
#    define EVP_MD_CTX_free(p)     EVP_MD_CTX_destroy((p))
#  endif
#endif

/* ====================================================================
 *  2. EVP_CIPHER_CTX new/free
 *     重点！OpenSSL 1.0.x 上根本没有 EVP_CIPHER_CTX_create/destroy！
 *     1.0.x 的 EVP_CIPHER_CTX 是栈分配 + EVP_CIPHER_CTX_init/_cleanup。
 *     所以这里必须自己写堆分配包装（OPENSSL_malloc + init）。
 *     1.1.0+ 原生有 EVP_CIPHER_CTX_new/free，直接用。
 * ==================================================================== */
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x10100000L

static inline EVP_CIPHER_CTX *nsbc_evp_cipher_ctx_new(void)
{
    EVP_CIPHER_CTX *c = (EVP_CIPHER_CTX *)OPENSSL_malloc(sizeof(EVP_CIPHER_CTX));
    if (!c) return NULL;
    EVP_CIPHER_CTX_init(c);
    return c;
}

static inline void nsbc_evp_cipher_ctx_free(EVP_CIPHER_CTX *c)
{
    if (!c) return;
    EVP_CIPHER_CTX_cleanup(c);
    OPENSSL_free(c);
}

#  ifndef EVP_CIPHER_CTX_new
#    define EVP_CIPHER_CTX_new()       nsbc_evp_cipher_ctx_new()
#  endif
#  ifndef EVP_CIPHER_CTX_free
#    define EVP_CIPHER_CTX_free(p)     nsbc_evp_cipher_ctx_free((p))
#  endif
#endif

/* ====================================================================
 *  3. TLS_server_method
 *     1.1.0+ 原生有 TLS_server_method()
 *     1.0.x 没有，最接近的是 SSLv23_server_method()（支持 SSLv3 + TLS1.x，
 *     然后用 SSL_CTX_set_options 禁低版本达到"最低 TLS 1.2"的效果）。
 * ==================================================================== */
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x10100000L
#  define TLS_server_method()   SSLv23_server_method()
#endif

/* ====================================================================
 *  4. SSL_CTX_set_min_proto_version
 *     1.1.0+ 有原生 API；1.0.x 没有，用 SSL_CTX_set_options 禁低版本
 *     来模拟"最低 TLS 1.2"的效果。
 * ==================================================================== */
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x10100000L

/* TLS1_3_VERSION / TLS1_2_VERSION 在 1.0.x 上可能没定义 — 就地补充 */
#  ifndef TLS1_2_VERSION
#    define TLS1_2_VERSION 0x0303
#  endif
#  ifndef TLS1_3_VERSION
#    define TLS1_3_VERSION 0x0304   /* 1.0.x 上永远达不到 */
#  endif

static inline int nsbc_ssl_min_proto_version(SSL_CTX *ctx, int ver)
{
    if (ver < TLS1_2_VERSION) {
        /* 最低要求比 TLS 1.2 还低 — 那就只禁 SSLv3 */
        SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv3);
        return 1;
    }
    /* 最低 TLS 1.2 或更高：禁 SSLv3 + TLS1.0 + TLS1.1 */
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1);
#  ifdef SSL_OP_NO_TLSv1_1
    SSL_CTX_set_options(ctx, SSL_OP_NO_TLSv1_1);
#  endif
    /* 1.0.x 根本没 TLS 1.3，任何 >= TLS1_2 的要求到此为止 */
    return 1;
}

#  define SSL_CTX_set_min_proto_version(ctx, ver) \
      nsbc_ssl_min_proto_version((ctx), (ver))
#endif

/* ====================================================================
 *  5. OpenSSL 初始化
 *     1.0.x：必须 SSL_library_init / SSL_load_error_strings
 *     1.1.0+：OpenSSL_init_ssl 自动处理（幂等）
 * ==================================================================== */
static inline void nsbc_openssl_init(void)
{
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
#elif defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x30000000L
    /* 1.1.x：小写 OpenSSL_init_ssl */
    OpenSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS
                     | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
#else
    /* 3.0+：全大写 OPENSSL_init_ssl + 显式加载 default + legacy provider
     * （某些发行版拆包导致 legacy 不在默认链上，EC_KEY_* 等 legacy API 需要它）
     * 注：OSSL_PROVIDER_load 可能卡磁盘 I/O（NFS 根或容器缺 provider 目录），
     * 但只在首次调用时触发。 */
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS
                     | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
# include <openssl/provider.h>
    OSSL_PROVIDER_load(NULL, "default");
    OSSL_PROVIDER_load(NULL, "legacy");
#endif
}

/* ====================================================================
 *  6. DTLS_method / DTLS_server_method
 *     1.0.x 只有 DTLSv1_server_method()；1.1+ 有 DTLS_server_method()
 * ==================================================================== */
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x10100000L
#  define DTLS_server_method()   DTLSv1_server_method()
#endif

/* ====================================================================
 *  7. SSL_CTX_set_srtp_profiles / SSL_set_dtls_srtp_profiles
 *     1.0.x 用 SSL_CTX_set_tlsext_use_srtp()（ctx 级，只改 ctx 一次）
 *     1.1.0+ 原生有 SSL_CTX_set_srtp_profiles() / SSL_set_dtls_srtp_profiles()
 *     两者同时存在；3.0+ 同样支持。
 *     注意：1.1.0+ 上必须两套都设（ctx 级+ssl 级），否则 DTLS 握手会因
 *     SRTP 套件不匹配失败。
 * ==================================================================== */
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x10100000L

static inline int nsbc_ssl_ctx_set_srtp_profiles(SSL_CTX *ctx, const char *profiles)
{
    /* 1.0.x：SSL_CTX_set_tlsext_use_srtp 返回 0 成功、1 失败（注意与新 API 相反） */
    return (SSL_CTX_set_tlsext_use_srtp(ctx, profiles) == 0) ? 1 : 0;
}
#  define SSL_CTX_set_srtp_profiles(ctx, prof)  nsbc_ssl_ctx_set_srtp_profiles((ctx), (prof))
/* 1.0.x 没有 SSL_set_dtls_srtp_profiles——ctx 级设了就够；宏返回 void（无副作用） */
#  define SSL_set_dtls_srtp_profiles(ssl, prof)  (void)(ssl)

#endif

/* ====================================================================
 *  7.5 BIO_CTRL_DGRAM_SET_DATA（nsbc_ssl_dtls_feed 用）
 *     将外部收到的 UDP datagram 塞回 BIO_dgram 的读队列。
 *     **此 ctrl 从未成为公开 API**——即使在 OpenSSL 1.1.1/3.0 也没
 *     有在公共头文件里定义。nsbc_ssl_dtls_feed 现为空操作，DTLS 数据
 *     由 BIO_dgram + SSL_accept 直接从 fd 读取（dtls_server_on_read 路径）。
 *     NSBC_OPENSSL_HAS_DTLS_FEED 恒为 0。
 * ==================================================================== */
#  define NSBC_OPENSSL_HAS_DTLS_FEED 0

/* ====================================================================
 *  8. X509_getm_notBefore / X509_getm_notAfter
 *     1.1.0+ 有原生 X509_getm_notBefore/notAfter（返回 const ASN1_TIME*）
 *     1.0.x 只有 X509_get_notBefore/notAfter（返回 ASN1_TIME*）
 * ==================================================================== */
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x10100000L
#  define X509_getm_notBefore(x)   X509_get_notBefore(x)
#  define X509_getm_notAfter(x)    X509_get_notAfter(x)
#endif

/* ====================================================================
 *  8.5 SSL_get_peer_certificate vs SSL_get1_peer_certificate
 *     1.1.1+ : SSL_get1_peer_certificate — 返回引用计数副本，必须 X509_free
 *     1.0.x / 1.1.0 : SSL_get_peer_certificate — 返回 SSL 内部持有的证书
 *                    指针（不增加引用计数），free 会 double-free 或释放
 *                    SSL 内部对象导致后续崩溃。
 *     两个宏配合使用：
 *       X509 *peer = NSBC_SSL_PEER_CERT(ssl);
 *       ...
 *       NSBC_SSL_PEER_CERT_FREE(peer);  // 1.0.x 上是空操作
 * ==================================================================== */
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10101000L
#  define NSBC_SSL_PEER_CERT(ssl)       SSL_get1_peer_certificate((ssl))
#  define NSBC_SSL_PEER_CERT_FREE(x)     X509_free((x))
#else
#  define NSBC_SSL_PEER_CERT(ssl)       SSL_get_peer_certificate((ssl))
#  define NSBC_SSL_PEER_CERT_FREE(x)    ((void)(x))   /* legacy 不 free */
#endif

/* ====================================================================
 *  9. SSL_EXPORT_KEYING_MATERIAL 宏（SSL_export_keying_material 别名）
 *     所有 OpenSSL 版本（1.0.1+）都有 SSL_export_keying_material
 *     —— 此处仅补一个更简洁的别名
 * ==================================================================== */
#ifndef SSL_EXPORT_KEYING_MATERIAL
#  define SSL_EXPORT_KEYING_MATERIAL(ssl, out, outlen, label, laballen, ctx, ctxlen, use_context) \
      SSL_export_keying_material((ssl), (out), (outlen), (label), (laballen), (ctx), (ctxlen), (use_context))
#endif

#endif /* NSBC_OPENSSL_COMPAT_H */
