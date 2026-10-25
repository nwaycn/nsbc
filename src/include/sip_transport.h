#ifndef NSBC_SIP_TRANSPORT_H
#define NSBC_SIP_TRANSPORT_H

/* SIP 传输调度层（REQ-SIP-006/017、RFC3261 §18）
 *
 * UDP：SO_REUSEPORT + recvmmsg 批量收（NET_BATCH=32），sendmmsg/GSO
 *      回退发送；epoll 边沿驱动 reactor 回调。
 * TCP：listen + accept4，每连接独立 tcp_framer_t（CL 定界，无 CL/chunked
 *      立即断连，超 max_msg 断连），CRLF keepalive 跳过。
 * TLS：每连接 tls_io 非阻塞握手（TLS1.3 优先），解密后复用同一分帧器。
 *
 * REQ-SIP-017 UDP Content-Length 处置（入向预筛，sip_udp_cl_check）：
 *   多余字节      → DROP（静默丢弃，绝不回错）
 *   报文截断      → BAD400（SHOULD 回 400 Bad Request）
 *   重复/非数字 CL → DROP
 *   无 CL         → 以数据报末尾为界（body 为空口径由解析器保证）
 */
#include "net_sock.h"
#include "reactor.h"
#include "reactor_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SIP_TR_UDP = 0, SIP_TR_TCP, SIP_TR_TLS, SIP_TR_WS, SIP_TR_WSS } sip_tr_proto_t;

typedef struct sip_transport sip_transport_t;

typedef void (*sip_rx_fn)(void *ud, sip_tr_proto_t proto,
                          const nsbc_sa_t *src, const nsbc_sa_t *local,
                          const char *buf, size_t len);

typedef struct {
    reactor_t *re;        /* reactor[0]（main 线程跑） */
    reactor_mgr_t *mgr;  /* P3: reactor manager（多 reactor UDP SO_REUSEPORT） */
    const char *listen_ip;
    uint16_t udp_port;
    uint16_t tcp_port;
    uint16_t tls_port;
    int enable_udp;
    int enable_tcp;
    int enable_tls;
    int reuseport;
    int rcvbuf_kb;
    int sndbuf_kb;
    size_t max_msg;
    int tcp_conn_max;
    int tcp_conn_per_ip;       /* 每 IP 并发连接上限（防止单 IP SYN flood）；0=默认 50 */
    int tcp_idle_timeout_ms;   /* TCP/TLS idle 超时断链（毫秒）；0=默认 30000 */
    void *tls_ctx; /* tls_ctx_server_new 产物；enable_tls 时必填 */
    sip_rx_fn rx;
    void *rx_ud;
} sip_tr_cfg_t;

int sip_tr_create(const sip_tr_cfg_t *cfg, sip_transport_t **out);
void sip_tr_destroy(sip_transport_t *t);

/* 后置绑定 rx 用户数据：解 B2BUA 与 transport 的构造循环依赖
 * （b2b 构造需 tr，tr 收包需 b2b）。必须在 reactor 跑起来前设置。 */
void sip_tr_set_rx_ud(sip_transport_t *t, void *ud);

/* TLS ctx 热替换（SIGHUP reload 证书续期用）*/
void sip_tr_set_tls_ctx(sip_transport_t *t, void *ctx);

/* 发送。UDP 始终可用；TCP/TLS 仅能复用入向已建立连接
 * （主动外呼 TCP 连接管理器为 P1，P0 返回 NSBC_NOTSUP）。 */
int sip_tr_send(sip_transport_t *t, sip_tr_proto_t proto,
                const char *buf, size_t len, const nsbc_sa_t *dst);

typedef enum {
    UDP_CL_OK = 0,
    UDP_CL_DROP,   /* 多余字节/重复 CL/非数字：静默丢弃 */
    UDP_CL_BAD400  /* 截断：应答 400 */
} udp_cl_rc_t;
udp_cl_rc_t sip_udp_cl_check(const char *buf, size_t len, size_t max_msg);

/* 监听 fd（metrics/诊断用）——返回第一个 UDP fd；多端口场景用
 * sip_tr_udp_fd_count + sip_tr_udp_fd_at 遍历 */
int sip_tr_udp_fd(const sip_transport_t *t);
int sip_tr_udp_fd_count(const sip_transport_t *t);
int sip_tr_udp_fd_at(const sip_transport_t *t, int idx);

/* 动态添加一组 UDP/TCP 监听（profile reload 后追加端口用）。
 * 内部去重——已监听的端口直接跳过。
 * 返回：NSBC_OK / NSBC_ERR（socket 创建失败） */
int sip_tr_add_listeners(sip_transport_t *t, const char *listen_ip,
                         uint16_t udp_port, uint16_t tcp_port, uint16_t tls_port);

/* 从所有 enabled sip_profile_t 收集不同的 UDP/TCP 端口，
 * 按需追加到 transport 监听 fd 列表（内部已去重）。
 * 在 profiles.reload 成功后调用，确保新端口被监听。 */
int sip_tr_sync_from_profiles(sip_transport_t *t);

/* ---- SIP trace 全局开关（类似 FreeSWITCH sofia global siptrace） ----
 * 打开后 sip_tr_send（TX）和 on_udp_event（RX）会把完整 SIP 包转成
 * hex+ascii 格式打 LOG_DEBUG "siptrace" 日志；关闭后为零开销（volatile 读）。 */
void sip_tr_set_trace(int on);
int  sip_tr_get_trace(void);

/* WebRTC/WS RX trace hook — signaling.c:sig_ws_bridge_rx 调
 * sip_tr_send(TX) 和 on_udp_event/frame_cb(RX) 已内建 trace，
 * WS/WSS RX 入口在 signaling.c 不走 sip_transport，需要手动通知 */
void sip_tr_rx_trace(int proto, const char *peer_ip, uint16_t peer_port,
                     const char *buf, size_t len);

/* ---- UAC 出站请求（P1；sip_transport_send_uac) ----
 *
 * 主动外发 SIP 请求（REGISTER / INVITE 等）。当前仅支持 UDP
 * （TCP/TLS 主动连接管理器为 P1，先返回 NSBC_NOTSUP）。
 * 报文构造走 sip_build_request（无 malloc，有界缓冲）。
 *
 * 参数：
 *   t          sip_transport_t（sig_rt->tr）
 *   method     "REGISTER" / "INVITE" ...（必须；不用 SIP_M_* enum，保持字符串透明）
 *   uri        Request-URI："sip:host:port"
 *   from       From 头："sip:user@host"（不带 tag，本函数自动生成 tag）
 *   from_tag   可 NULL；非 NULL 直接用；NULL 则本函数生成 "nsbc-<callid[:8]>-<seq>"
 *   to         To 头（可与 from 同）
 *   call_id    Call-ID（必须）
 *   cseq       CSeq 序列号（从 1 起）
 *   contact    Contact 头值（可 NULL；REGISTER 必填）
 *   expires    Expires 秒（0=不发 Expires 头；REGISTER 通常 3600）
 *   auth_hdr   Authorization 头值（Digest 完整值，含 "Authorization: " 前缀；NULL=不发）
 *   via_host   Via/Contact 广告 host（通常 ctx->local_host）
 *   via_port   Via/Contact 广告端口
 *   dst_host   对端 host（DNS / IP 均可）
 *   dst_port   对端端口（通常 5060）
 *   extra      额外头块（自带 CRLF，可 NULL；例如 Route/Proxy-Authorization）
 *   body       可选 body（可 NULL；INVITE 的 SDP；长度 bodylen）
 *   bodylen    body 长度
 *   content_type body 非空时填（"application/sdp"）
 *
 * 返回：
 *   NSBC_OK       已发出（UDP 不保证送达）
 *   NSBC_NOTSUP   TCP/TLS 主动连接未实现；或 t=NULL / udp_fd<0
 *   NSBC_INVAL    参数缺失（method/uri/from/to/call_id 任一 NULL 或空）
 *   NSBC_NOSPC    构建缓冲不足（16KB 足够绝大多数请求）
 *   NSBC_AGAIN    send 瞬时返回 EAGAIN（调用方决定是否重试） */
#define SIP_UAC_BUF_CAP (16 * 1024)
int sip_transport_send_uac(sip_transport_t *t,
                           const char *method, const char *uri,
                           const char *from, const char *from_tag,
                           const char *to, const char *call_id, long cseq,
                           const char *contact, long expires,
                           const char *auth_hdr,
                           const char *via_host, int via_port,
                           const char *dst_host, uint16_t dst_port,
                           const char *extra,
                           const char *body, size_t bodylen,
                           const char *content_type);

/* ---- 公共辅助函数（UAC 出站请求需要） ---- */
void gen_from_tag(const char *call_id, long cseq, char *out, size_t outsz);
void gen_branch(const char *call_id, long cseq, char *out, size_t outsz);
void build_from_line(const char *sip_uri, const char *tag,
                     char *out, size_t outsz);

#ifdef __cplusplus
}
#endif
#endif

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
