#ifndef NSBC_SSL_H
#define NSBC_SSL_H

/* OpenSSL 全版本兼容层——证书生成、DTLS-SRTP 终结
 *
 * 封装 OpenSSL 版本差异（1.0.2k → 3.5.x），上层（dtls_srtp.c/tls_io.c）
 * 只需调本层 API。版本宏在 openssl_compat.h，本文件只暴露业务 API。
 *
 * 本层所有函数返回 NSBC_OK/NSBC_ERR/NSBC_NOMEM/NSBC_NOTSUP，
 * 不在头文件泄漏 OpenSSL 具体类型（SSL_CTX / SSL 用 void* 包装）。
 */
#include "nsbc_common.h"
#include "srtp_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 自签 ECDSA P-256 证书 + 私钥 ----
 * 在 cert_pem / key_pem（内存缓冲，PEM 格式）中返回；fp_sha256 输出
 * SHA-256 指纹（小写 hex，冒号分隔），给 SDP a=fingerprint:sha-256 用。
 * cert_pem/key_pem 长度不超 PEM_BUF_SIZE（6KB 足够 P-256 证书）。 */
#define NSBC_SSL_PEM_BUF_SIZE 6144
/* SHA-256 指纹（冒号分隔 hex）：32B → 64 hex + 31 ':' + NUL = 96 */
#define NSBC_SSL_FP_SHA256_SIZE 96

int nsbc_ssl_gen_self_signed(char *cert_pem, size_t cert_sz,
                             char *key_pem, size_t key_sz,
                             char *fp_sha256, size_t fp_sz);

/* ---- DTLS Server 上下文（非阻塞 UDP dgram BIO） ----
 * 创建 DTLS server 侧 SSL_CTX + SSL，SRTP profiles 预置 SRTP_AEAD_AES_128_GCM。
 * fd 绑定到 BIO_dgram(BIO_NOCLOSE)，非阻塞。
 * 返回不透明 handle（dtls_hdl_t），dtls_on_read 内部解析。 */
typedef struct nsbc_dtls *nsbc_dtls_hdl_t;

int nsbc_ssl_dtls_server_new(int fd, nsbc_dtls_hdl_t *out,
                              const char *srtp_profiles /* "SRTP_AEAD_AES_128_GCM" */,
                              char *fp_sha256 /* 输出 SHA-256 指纹，可为 NULL */,
                              size_t fp_sz);
void nsbc_ssl_dtls_destroy(nsbc_dtls_hdl_t hdl);

/* DTLS 非阻塞握手推进：内部 while(SSL_accept != 1) { 处理 WANT_READ/WRITE }
 * 返回 NSBC_OK 握手完成；NSBC_AGAIN 需再调（有新 DTLS 数据时）；NSBC_ERR 致命。 */
int nsbc_ssl_dtls_handshake(nsbc_dtls_hdl_t hdl);

/* 外部把一个 dgram 包喂给 DTLS BIO——relay_recv_one 统一 recvfrom 后，
 * 按首字节判定是 DTLS record 时调本函数。内部：
 *   BIO_ctrl(BIO_CTRL_DGRAM_SET_DATA, len, buf) 把数据塞进 BIO 的 dgram 队列
 * 之后 nsbc_ssl_dtls_handshake 能推进握手。
 * 返回 NSBC_OK；NSBC_INVAL 如果 hdl/buf 空；NSBC_ERR 如果握手已完成（不该喂了）。 */
int nsbc_ssl_dtls_feed(nsbc_dtls_hdl_t hdl, const uint8_t *buf, size_t len);

/* 握手完成查询（供 dtls_server_state 接口用） */
int nsbc_ssl_dtls_established(nsbc_dtls_hdl_t hdl);

/* RFC5764 keying material 导出 → nsbc_srtp_material_t 双向
 * label 默认 "EXTRACTOR-dtls_srtp"；sending=本端发出 SRTP material；
 * receiving=本端收到 SRTP material。
 * material->label 会被覆写为导出槽标记（"EXTRACTOR-dtls_srtp:0007:SND"）。 */
int nsbc_ssl_dtls_export_srtp(nsbc_dtls_hdl_t hdl,
                               nsbc_srtp_material_t *sending,
                               nsbc_srtp_material_t *receiving);

/* 握手完成后拿对端证书 SHA-256 指纹（小写 hex + 冒号，95 字符 + NUL）。
 * established 之前或对端未发证书返回 NSBC_ERR。
 * outsz 必须 >= NSBC_SSL_FP_SHA256_SIZE (96)。 */
int nsbc_ssl_dtls_peer_fp(nsbc_dtls_hdl_t hdl, char *out, size_t outsz);

/* 从 nsbc_dtls_hdl_t 内部取 SSL*（dtls_srtp.c 调试打印 SSL_state 用） */
void *nsbc_ssl_dtls_get_ssl(nsbc_dtls_hdl_t hdl);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_SSL_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
