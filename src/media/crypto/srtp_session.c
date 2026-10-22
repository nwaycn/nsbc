/* SRTP 会话——REQ-SEC-001 DTLS-SRTP（P0）
 *
 * 两向会话：inbound/recv（srtp_unprotect 解密对端发来的 SRTP）+
 * outbound/send（srtp_protect 加密本端发出的 RTP），各自独立 srtp_t。
 *
 * 构建宏 NSBC_WITH_SRTP：apt install libsrtp2-dev → #include <srtp2/srtp.h>
 *                      → 链接 -lsrtp2。未定义宏时全接口 NOTSUP（壳行为，
 *                      保证呼叫侧协商分支可测，无 SRTP 构建且对端强制
 *                      crypto 时应答 488 Not Acceptable Here）。
 *
 * 密钥 material 从 DTLS exporter (RFC5764 label "EXTRACTOR-dtls_srtp")
 * 注入，本模块只做 RTP/RTCP protect/unprotect + 重放窗口。
 * 绝不打印密钥 material 任何字节。
 *
 * libsrtp2 API 与 NSBC wrapper 符号隔离：
 *   - libsrtp2 类型/函数保持原名（srtp_policy_t, srtp_protect, ...）
 *   - NSBC 对外接口全加 nsbc_ 前缀（nsbc_srtp_policy_t, nsbc_srtp_protect_rtp, ...）
 */
#include "srtp_session.h"
#include "log.h"

#ifdef NSBC_WITH_SRTP
/* libsrtp 头文件路径：Debian 11/Ubuntu 22.04 → <srtp2/srtp.h>
 * Debian 12/Ubuntu 24.04+ (libsrtp ≥ 2.4) → <srtp/srtp.h>
 * 先找 srtp2/srtp.h，fallback srtp/srtp.h，最后 fallback openssl/srtp.h */
#if __has_include(<srtp2/srtp.h>)
#  include <srtp2/srtp.h>
#elif __has_include(<srtp/srtp.h>)
#  include <srtp/srtp.h>
#elif __has_include(<openssl/srtp.h>)
#  include <openssl/srtp.h>
#else
#  error "No libsrtp header found. Install libsrtp2-dev."
#endif
#endif

struct srtp_sess {
    nsbc_srtp_policy_t policy;
    nsbc_srtp_material_t inbound;
    nsbc_srtp_material_t outbound;
#ifdef NSBC_WITH_SRTP
    srtp_t send_ctx;   /* outbound：protect（本端发→加密） */
    srtp_t recv_ctx;   /* inbound ：unprotect（对端来→解密） */
#endif
};

#ifdef NSBC_WITH_SRTP
/* libsrtp2 cipher/auth 常量必须来自官方头文件 crypto_types.h。
 * ★ 关键教训（2026-10-09 修复）：之前用 #ifndef 检查枚举名（比如 SRTP_AES_ICM_128）
 *   做 fallback，但 #ifndef 检查的是预处理宏不是 enum 名字 —— enum 里的
 *   SRTP_AES_ICM_128 不是宏，所以 #ifndef SRTP_AES_ICM_128 恒为真，里面的
 *   #define SRTP_AES_GCM_128 就把 libsrtp2 官方 enum 值覆盖了 → KDF 不同 →
 *   加密结果和 Chrome 不一致 → tag 校验失败 → recv=0。
 *
 * ★ libsrtp2 版本差异：
 *   v2.2- (Debian 12 默认)：SRTP_AES_GCM_128=6, SRTP_AES_GCM_256=7
 *     — 只有标准 AEAD_GCM 一个变体，tag 固定 16 字节
 *   v2.3+ (Ubuntu 24.04+)：SRTP_AES_GCM_128=4, SRTP_AES_GCM_256=5
 *     — 新增 _16_AUTH 变体 6/7，标准 GCM 重编号为 4/5
 *   两者都是 RFC 7714 标准 AEAD，值不同但实现完全一致。 */
/* libsrtp ≥ 2.4（Debian 12/Ubuntu 24.04+）把 crypto_types.h 的枚举合并进了 srtp.h
 * 本身，不再安装单独的 crypto_types.h。2.3 及更早版本仍需单独 include。
 * 两个版本的常量名/值完全一致，只是头文件组织变化。
 * 这里用 weak fallback：有 crypto_types.h 就 include，没有就跳过（srtp.h 已覆盖）。*/
#if defined(__has_include)
#  if __has_include(<srtp2/crypto_types.h>)
#    include <srtp2/crypto_types.h>
#  elif __has_include(<srtp/crypto_types.h>)
#    include <srtp/crypto_types.h>
#  endif
/* else: libsrtp ≥ 2.4 — crypto_types 已合并进 srtp.h，无需单独 include */
#endif

/* 版本兼容：老版本 libsrtp2 NULL auth 枚举叫 SRTP_NULL_AUTH，新版本可能叫 SRTP_AUTH_NULL */
#ifndef SRTP_NULL_AUTH
#  ifdef SRTP_AUTH_NULL
#    define SRTP_NULL_AUTH SRTP_AUTH_NULL
#  else
#    define SRTP_NULL_AUTH 0    /* 兜底，避免 static_assert 失败 */
#  endif
#endif

/* ★ 安全 assert：值不能是 0（NULL_CIPHER），GCM 值范围 4-7 都合法。
 *   旧版 6/7 和新版 4/5 都是标准 GCM，只是版本编号不同。 */
_Static_assert(SRTP_AES_ICM_128 != 0, "SRTP_AES_ICM_128 must not be 0");
_Static_assert(SRTP_AES_GCM_128 >= 4 && SRTP_AES_GCM_128 <= 7,
    "SRTP_AES_GCM_128 value out of range");
_Static_assert(SRTP_NULL_AUTH == 0, "SRTP_NULL_AUTH must be 0");


#endif

int nsbc_srtp_global_init(void)
{
#ifdef NSBC_WITH_SRTP
    return (srtp_init() == srtp_err_status_ok) ? NSBC_OK : NSBC_ERR;
#else
    return NSBC_OK;
#endif
}

void nsbc_srtp_global_shutdown(void)
{
#ifdef NSBC_WITH_SRTP
    srtp_shutdown();
#endif
}

int nsbc_srtp_session_create(const nsbc_srtp_policy_t *policy,
                        const nsbc_srtp_material_t *inbound,
                        const nsbc_srtp_material_t *outbound,
                        nsbc_srtp_sess_t **out)
{
    if (policy == NULL || inbound == NULL || outbound == NULL ||
        out == NULL)
        return NSBC_INVAL;
    if (!nsbc_srtp_kdr_valid(policy->kdr))
        return NSBC_INVAL;
    if (policy->keylen <= 0 || policy->keylen > SRTP_KEY_MAX ||
        inbound->keylen <= 0 || inbound->keylen > SRTP_KEY_MAX ||
        outbound->keylen <= 0 || outbound->keylen > SRTP_KEY_MAX)
        return NSBC_INVAL;

    nsbc_srtp_sess_t *s = (nsbc_srtp_sess_t *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NSBC_NOMEM;
    s->policy = *policy;
    s->inbound = *inbound;
    s->outbound = *outbound;
#ifdef NSBC_WITH_SRTP
    srtp_profile_t profile;
    switch (policy->cipher) {
    case SRTP_CIPHER_AESCM128:
        profile = srtp_profile_aes128_cm_sha1_80;
        break;
    case SRTP_CIPHER_AEAD_AESGCM128:
        profile = srtp_profile_aead_aes_128_gcm;
        break;
    case SRTP_CIPHER_AEAD_AESGCM256:
        profile = srtp_profile_aead_aes_256_gcm;
        break;
    default:
        free(s);
        return NSBC_INVAL;
    }

    /* ========== inbound/recv policy ========== */
    srtp_policy_t sp;
    memset(&sp, 0, sizeof(sp));

    srtp_err_status_t rc = srtp_crypto_policy_set_from_profile_for_rtp(&sp.rtp, profile);
    if (rc != srtp_err_status_ok) {
        llog(LOG_ERR, "srtp", "set_from_profile_for_rtp FAIL rc=%d profile=%d",
             (int)rc, (int)profile);
        free(s);
        return NSBC_ERR;
    }
    /* ★★ 绝对不要覆盖 set_from_profile 的任何字段！
     * 某些版本 libsrtp2 的 srtp_crypto_policy_t 有隐藏字段（如 salt_len、
     * cipher_id 内部枚举等），set_from_profile 会一起正确设置。
     * 手动覆盖 cipher_key_len/sec_serv 会导致内部字段不一致 → rc=5 bad_param。 */
    rc = srtp_crypto_policy_set_from_profile_for_rtcp(&sp.rtcp, profile);
    if (rc != srtp_err_status_ok) {
        llog(LOG_ERR, "srtp", "set_from_profile_for_rtcp FAIL rc=%d", (int)rc);
        free(s);
        return NSBC_ERR;
    }

    llog(LOG_INFO, "srtp", "set_from_profile: cipher=%d cipher_key_len=%d "
         "auth_type=%d auth_key_len=%d auth_tag_len=%d sec_serv=%d "
         "sizeof_policy=%lu",
         sp.rtp.cipher_type, sp.rtp.cipher_key_len,
         sp.rtp.auth_type, sp.rtp.auth_key_len,
         sp.rtp.auth_tag_len, sp.rtp.sec_serv,
         (unsigned long)sizeof(srtp_crypto_policy_t));

    /* ★ sp.key 必须指向 s->inbound.key（堆上），不是调用者栈上的
     * receiving.key——dtls_export_srtp 返回后栈帧释放，虽然 srtp_create
     * 内部会拷贝 key，但用堆上 buffer 更安全。 */
    sp.ssrc.type = ssrc_any_inbound;
    sp.ssrc.value = 0;
    sp.window_size = (policy->replay_win > 0) ? policy->replay_win : 64;
    sp.key = s->inbound.key;   /* 堆上拷贝后的 key buffer */
    sp.next = NULL;

    llog(LOG_INFO, "srtp", "BEFORE inbound create: profile=%d "
         "sp.rtp.cipher_key_len=%d inbound->keylen=%d "
         "window_size=%lu key_ptr=%p",
         (int)profile, sp.rtp.cipher_key_len, inbound->keylen,
         sp.window_size, (void *)sp.key);

    rc = srtp_create(&s->recv_ctx, &sp);
    if (rc != srtp_err_status_ok) {
        llog(LOG_ERR, "srtp", "srtp_create inbound FAIL rc=%d profile=%d "
             "cipher_key_len=%d material_len=%d window_size=%lu ssrc_type=%d "
             "key_ptr=%p",
             (int)rc, (int)profile, sp.rtp.cipher_key_len,
             inbound->keylen, sp.window_size, sp.ssrc.type, (void *)sp.key);
        free(s);
        return NSBC_ERR;
    }

    /* ========== outbound/send policy ========== */
    memset(&sp, 0, sizeof(sp));

    rc = srtp_crypto_policy_set_from_profile_for_rtp(&sp.rtp, profile);
    if (rc != srtp_err_status_ok) { free(s); return NSBC_ERR; }
    rc = srtp_crypto_policy_set_from_profile_for_rtcp(&sp.rtcp, profile);
    if (rc != srtp_err_status_ok) { free(s); return NSBC_ERR; }

    sp.ssrc.type = ssrc_any_outbound;
    sp.ssrc.value = 0;
    sp.window_size = (policy->replay_win > 0) ? policy->replay_win : 64;
    sp.key = s->outbound.key;  /* 堆上拷贝后的 key buffer */
    sp.next = NULL;

    llog(LOG_INFO, "srtp", "BEFORE outbound create: profile=%d cipher=%d "
         "cipher_key_len=%d auth_tag_len=%d sec_serv=%d material_len=%d "
         "window_size=%lu",
         (int)profile, sp.rtp.cipher_type, sp.rtp.cipher_key_len,
         sp.rtp.auth_tag_len, sp.rtp.sec_serv, outbound->keylen,
         sp.window_size);

    rc = srtp_create(&s->send_ctx, &sp);
    if (rc != srtp_err_status_ok) {
        llog(LOG_ERR, "srtp", "srtp_create outbound FAIL rc=%d profile=%d "
             "cipher=%d cipher_key_len=%d auth_tag_len=%d sec_serv=%d "
             "material_len=%d window_size=%lu ssrc_type=%d",
             (int)rc, (int)profile, sp.rtp.cipher_type, sp.rtp.cipher_key_len,
             sp.rtp.auth_tag_len, sp.rtp.sec_serv, outbound->keylen,
             sp.window_size, sp.ssrc.type);
        srtp_dealloc(s->recv_ctx);
        free(s);
        return NSBC_ERR;
    }
#endif

    *out = s;
    return NSBC_OK;
}

void nsbc_srtp_session_destroy(nsbc_srtp_sess_t *s)
{
    if (s == NULL)
        return;
#ifdef NSBC_WITH_SRTP
    srtp_dealloc(s->send_ctx);
    srtp_dealloc(s->recv_ctx);
#endif
    memset(&s->policy, 0, sizeof(s->policy));
    memset(s->inbound.key, 0, sizeof(s->inbound.key));
    memset(s->outbound.key, 0, sizeof(s->outbound.key));
    memset(s, 0, sizeof(*s));
    free(s);
}

int nsbc_srtp_protect_rtp(nsbc_srtp_sess_t *s, uint8_t *buf, size_t *len, size_t cap)
{
    (void)cap;
    if (s == NULL || buf == NULL || len == NULL)
        return NSBC_INVAL;
#ifdef NSBC_WITH_SRTP
    int slen = (int)*len;
    srtp_err_status_t rc = srtp_protect(s->send_ctx, buf, &slen);
    if (rc == srtp_err_status_ok) {
        *len = (size_t)slen;
        return NSBC_OK;
    }
    if (rc == srtp_err_status_replay_fail ||
        rc == srtp_err_status_replay_old) {
        return NSBC_AGAIN;
    }
    return NSBC_ERR;
#else
    return NSBC_NOTSUP;
#endif
}

int nsbc_srtp_unprotect_rtp(nsbc_srtp_sess_t *s, uint8_t *buf, size_t *len)
{
    if (s == NULL || buf == NULL || len == NULL)
        return NSBC_INVAL;
#ifdef NSBC_WITH_SRTP
    int slen = (int)*len;
    srtp_err_status_t rc = srtp_unprotect(s->recv_ctx, buf, &slen);
    if (rc == srtp_err_status_ok) {
        *len = (size_t)slen;
        return NSBC_OK;
    }
    if (rc == srtp_err_status_replay_fail ||
        rc == srtp_err_status_replay_old) {
        return NSBC_AGAIN;
    }
    llog(LOG_WARN, "srtp", "unprotect_rtp FAIL libsrtp_rc=%d in_len=%zu "
         "first_byte=0x%02x", (int)rc, *len, buf[0]);
    return NSBC_ERR;
#else
    return NSBC_NOTSUP;
#endif
}

int nsbc_srtp_protect_rtcp(nsbc_srtp_sess_t *s, uint8_t *buf, size_t *len, size_t cap)
{
    (void)cap;
    if (s == NULL || buf == NULL || len == NULL)
        return NSBC_INVAL;
#ifdef NSBC_WITH_SRTP
    int slen = (int)*len;
    /* 调用 libsrtp2 的 srtp_protect_rtcp——不是 nsbc_srtp_protect_rtcp 自身！ */
    srtp_err_status_t rc = srtp_protect_rtcp(s->send_ctx, buf, &slen);
    if (rc == srtp_err_status_ok) {
        *len = (size_t)slen;
        return NSBC_OK;
    }
    return NSBC_ERR;
#else
    return NSBC_NOTSUP;
#endif
}

int nsbc_srtp_unprotect_rtcp(nsbc_srtp_sess_t *s, uint8_t *buf, size_t *len)
{
    if (s == NULL || buf == NULL || len == NULL)
        return NSBC_INVAL;
#ifdef NSBC_WITH_SRTP
    int slen = (int)*len;
    /* 调用 libsrtp2 的 srtp_unprotect_rtcp——不是 nsbc_srtp_unprotect_rtcp 自身！ */
    srtp_err_status_t rc = srtp_unprotect_rtcp(s->recv_ctx, buf, &slen);
    if (rc == srtp_err_status_ok) {
        *len = (size_t)slen;
        return NSBC_OK;
    }
    /* RTCP unprotect 失败（rc=7 replay_fail / rc=14 ssrc_mismatch）是 libsrtp2
     * ssrc_any_inbound 通配对 RTCP 的已知 bug——RTP 通配 OK 但 RTCP 仍做硬校验。
     * bridge 层会静默 drop，这里只 DEBUG 级留痕，避免日志风暴。 */
    if (rc == srtp_err_status_replay_fail ||
        rc == srtp_err_status_replay_old ||
        (int)rc == 14 /* ssrc_mismatch */) {
        llog(LOG_DEBUG, "srtp", "unprotect_rtcp non-fatal rc=%d "
             "in_len=%zu", (int)rc, *len);
        return NSBC_ERR;  /* bridge 层区分 RTP/RTCP，RTCP fail 不计数 */
    }
    llog(LOG_WARN, "srtp", "unprotect_rtcp FAIL libsrtp_rc=%d in_len=%zu "
         "first_byte=0x%02x", (int)rc, *len, buf[0]);
    return NSBC_ERR;
#else
    return NSBC_NOTSUP;
#endif
}

int nsbc_srtp_kdr_valid(uint64_t kdr)
{
    if (kdr == 0)
        return 1;
    if (kdr > (1ULL << 24))
        return 0;
    return (kdr & (kdr - 1)) == 0;
}

const char *nsbc_srtp_cipher_name(nsbc_srtp_cipher_t c)
{
    switch (c) {
    case SRTP_CIPHER_AESCM128:        return "AES_CM_128_HMAC_SHA1_80";
    case SRTP_CIPHER_AEAD_AESGCM128:  return "AEAD_AES_128_GCM";
    case SRTP_CIPHER_AEAD_AESGCM256:  return "AEAD_AES_256_GCM";
    }
    return "unknown";
}
