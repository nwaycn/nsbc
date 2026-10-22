/* NSBC WebSocket Listener — RFC 6455 (最小子集) + SIP bridge (RFC 7118)
 *
 * Phase 1 功能：
 *   - HTTP Upgrade handshake (Sec-WebSocket-Key → Sec-WebSocket-Accept)
 *   - WS frame 收/发（TEXT opcode=0x1, CLOSE opcode=0x8）
 *   - 客户端 masked 帧解 mask；服务端 unmasked 发
 *   - WSS 支持：复用 net_sock / tls_io 现有 TLS 能力
 *   - PATH 匹配：只接受 /ws 或 /sip 或 /（三种都接受，Phase 1 简单处理）
 *
 * 线程模型：accept pthread + 每连接 pthread（和 STUN server 同模式）
 *
 * 上层桥接：收到完整 TEXT 帧（SIP 消息）后 → 调用 ws_rx_fn 回调
 * signaling.c 里把这个回调绑定到 b2b_on_rx(SIP_TR_WS, ...)
 */
#ifndef NSBC_WS_LISTENER_H
#define NSBC_WS_LISTENER_H

#include "nsbc_common.h"
#include "sip_transport.h"   /* sip_tr_proto_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ws_rx_fn)(void *ud, const char *buf, size_t len,
                         void *conn);   /* conn=ws_conn_t* 发送时用 */

int  ws_listener_init(uint16_t port, int tls_enabled, int enabled,
                      void *tls_ctx,   /* tls_ctx_server_new 产物；tls_enabled 时必填 */
                      ws_rx_fn rx, void *rx_ud);
int  ws_listener_start(void);
void ws_listener_stop(void);
void ws_listener_shutdown(void);

/* TLS ctx 热替换（SIGHUP reload 证书续期用）*/
void ws_listener_set_tls_ctx(void *tls_ctx);

/* 给 b2b_on_rx 的发送路径：把 SIP 响应/请求发回对应 WS 连接 */
int  ws_conn_send(void *conn, const char *buf, size_t len);

/* 校验 conn 指针有效（挂链且未关闭）— registrar loc_binding_t.ws_conn
 * 的防 UAF 检查：conn 线程退出后对象惰性保留（closed=1 不 free），
 * 指针永远可安全解引用，本函数判断的是"可否继续用它发送"。 */
int  ws_conn_alive(void *conn);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_WS_LISTENER_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
