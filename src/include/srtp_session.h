#ifndef NSBC_SRTP_SESSION_H
#define NSBC_SRTP_SESSION_H

/* SRTP 媒体加密（REQ-SEC-001，P0 DTLS-SRTP；P1 SDES 可选）
 *
 * 底座：libsrtp（BSD，见 NSBC推荐的开源库.md）；构建宏 NSBC_WITH_SRTP。
 * 密钥协商不在本模块：DTLS-SRTP 的指纹在 SDP a=fingerprint（见
 * sdp_sec），DTLS 握手在 net/tls，keying material 导出后注入本模块；
 * 本模块只做 RTP/RTCP 报文的 protect/unprotect 与重放/ROH 策略。
 * 未带构建宏：全部接口 NSBC_NOTSUP，呼叫侧按 offer/answer 协商结果
 * 决定是否拒绝（无 SRTP 构建且对端强制 crypto 时应答 488）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SRTP_KEY_MAX     64   /* AES-256 master key 32B + salt 14B 余量 */
#define SRTP_TAG_MAX    16
#define SRTP_LABEL_MAX  24

typedef enum {
    SRTP_CIPHER_AESCM128 = 0,  /* AES-CM-128 + HMAC-SHA1（RFC3711 基线） */
    SRTP_CIPHER_AEAD_AESGCM128,/* RFC7714 GCM（AEAD，免独立 auth tag） */
    SRTP_CIPHER_AEAD_AESGCM256
} nsbc_srtp_cipher_t;

typedef struct {
    nsbc_srtp_cipher_t cipher;
    uint8_t  key[SRTP_KEY_MAX];
    int      keylen;
    uint64_t kdr;              /* RFC3711 §3.2.1 key 推导率，见校验 API */
    int      auth_tag_len;     /* 80bit 默认；GCM 忽略 */
    int      replay_win;       /* 重放窗口（默认 64） */
    int      roc_disabled;     /* 仅 ROC 单包模式（短连接） */
} nsbc_srtp_policy_t;

typedef struct {
    uint8_t  key[SRTP_KEY_MAX];
    int      keylen;
    char     label[SRTP_LABEL_MAX]; /* "EXTRACTOR-dtls_srtp" 派生槽标记 */
} nsbc_srtp_material_t;

typedef struct srtp_sess nsbc_srtp_sess_t;

int  nsbc_srtp_global_init(void);   /* 进程一次（srtp_init 包装） */
void nsbc_srtp_global_shutdown(void);

int  nsbc_srtp_session_create(const nsbc_srtp_policy_t *policy,
                         const nsbc_srtp_material_t *inbound,
                         const nsbc_srtp_material_t *outbound,
                         nsbc_srtp_sess_t **out);
void nsbc_srtp_session_destroy(nsbc_srtp_sess_t *s);

/* 就地加解密：buf 需有尾部 tag 余量；in/out len 为同一缓冲的长度，
 * *len 出写实际长度（protect 变长增加 tag，unprotect 减少）。 */
int  nsbc_srtp_protect_rtp(nsbc_srtp_sess_t *s, uint8_t *buf, size_t *len,
                      size_t cap);
int  nsbc_srtp_unprotect_rtp(nsbc_srtp_sess_t *s, uint8_t *buf, size_t *len);
int  nsbc_srtp_protect_rtcp(nsbc_srtp_sess_t *s, uint8_t *buf, size_t *len,
                       size_t cap);
int  nsbc_srtp_unprotect_rtcp(nsbc_srtp_sess_t *s, uint8_t *buf, size_t *len);

/* KDR 合法性（RFC3711 §3.2.1 冻结口径，勿改）：
 *  合法集合 = {0} ∪ {2^k | 0 ≤ k ≤ 24}；其余 INVAL。 */
int  nsbc_srtp_kdr_valid(uint64_t kdr);
const char *nsbc_srtp_cipher_name(nsbc_srtp_cipher_t c);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_SRTP_SESSION_H */
