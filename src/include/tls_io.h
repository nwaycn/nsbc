#ifndef NSBC_TLS_IO_H
#define NSBC_TLS_IO_H

/* SIP TLS（P0：SIP TLS；REQ-SIP-TCP-003 异步握手/会话复用为 P1）
 *
 * 密码底座：OpenSSL（Apache-2.0，见 NSBC推荐的开源库.md）。
 * 无 STARTTLS（RFC 3261 无此机制）：TLS 走独立端口/SIPS（RFC 5630）。
 * 构建时未带 NSBC_WITH_OPENSSL：全部接口返回 NSBC_NOTSUP，
 * 生产 P0 构建必须启用 OpenSSL。
 *
 * 握手状态机：socket 全程 NONBLOCK，SSL_ERROR_WANT_READ/WRITE 驱动
 * EPOLLIN/OUT 切换，握手线程不被慢客户端拖住（P1 REQ-SIP-TCP-003 增强为
 * 独立握手线程组+Session Ticket，P0 在 I/O 线程内完成非阻塞重试）。
 */

#include <stddef.h>
#include <stdint.h>
#include "nsbc_common.h"

typedef struct tls_ctx tls_ctx_t;
typedef struct tls_conn tls_conn_t;

typedef enum {
    TLS_HS_INIT = 0,
    TLS_HS_WANT_READ,
    TLS_HS_WANT_WRITE,
    TLS_HS_OK,
    TLS_HS_FAIL
} tls_hs_state_t;

typedef struct {
    const char *cert_pem;
    const char *key_pem;
    const char *ca_pem;       /* 可选：校验对端（双向 TLS/mTLS） */
    int require_client_cert;
    int min_tls12;           /* 允许 TLS1.2 回退（RFC 5246），默认优先 1.3 */
} tls_cfg_t;

tls_ctx_t *tls_ctx_server_new(const tls_cfg_t *cfg);
void tls_ctx_free(tls_ctx_t *ctx);

/* 取出内部 SSL_CTX* — 供 ws_listener 等需要裸 OpenSSL API 的模块用 */
void *tls_ctx_get_ssl(tls_ctx_t *ctx);

tls_conn_t *tls_conn_accept(tls_ctx_t *ctx, int fd);
tls_hs_state_t tls_handshake_step(tls_conn_t *c);
void tls_conn_free(tls_conn_t *c);
int tls_conn_fd(const tls_conn_t *c);

/* 语义同 read/write；NSBC_AGAIN=WANT_READ/WRITE（按返回的 want_out） */
int tls_read(tls_conn_t *c, void *buf, size_t len, int *want_out);
int tls_write(tls_conn_t *c, const void *buf, size_t len, int *want_out);

#define TLS_WANT_READ 1
#define TLS_WANT_WRITE 2

#endif /* NSBC_TLS_IO_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
