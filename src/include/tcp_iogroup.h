#ifndef NSBC_TCP_IOGROUP_H
#define NSBC_TCP_IOGROUP_H

/* TCP 独立 I/O 线程组（REQ-SIP-TCP-002 主 / REQ-SIP-006/007）
 *
 * 目的：慢连接（大报文慢速读/对端零窗口/TLS 慢握手）严格隔离在
 * 独立 epoll 线程，绝不拖垮 UDP recvmmsg 快路径（CPS ≥10k 验收）。
 * accept 线程只 accept4 后 handoff 给本组（内部 eventfd/SPMC 派发，
 * least-conns 选线程）；每连接挂慢读超时（无完整帧窗口→断连）。
 *
 * 分层：net 层；收包回调签名在本层中立定义，sip_transport 适配。
 */
#include <stdint.h>
#include "net_sock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_IO_SLOW_READ_DEF_MS  10000u /* 单帧间最大静默 10s */
#define TCP_IO_HANDSHAKE_DEF_MS  5000u  /* TLS 握手窗口（REQ-SIP-TCP-003 P1） */

typedef struct tcp_iogrp tcp_iogrp_t;

/* 中立收包回调（与 sip_transport 的 sip_rx_fn 解耦，组合根适配） */
typedef void (*tcp_io_rx_fn)(void *ud, int fd, const nsbc_sa_t *peer,
                             const char *buf, size_t len);

typedef struct {
    int      nthreads;      /* I/O 线程数（建议与物理核解耦，2~8） */
    int      max_conns;     /* 全组硬上限（规划 50000） */
    uint32_t slow_read_ms;  /* 帧间静默断连阈值 */
    uint32_t handshake_ms;  /* 建连/TLS 握手窗口 */
    int      per_conn_buf_kb;
    tcp_io_rx_fn rx;
    void        *rx_ud;
} tcp_iogrp_cfg_t;

void tcp_iogrp_cfg_default(tcp_iogrp_cfg_t *cfg);
int  tcp_iogrp_create(const tcp_iogrp_cfg_t *cfg, tcp_iogrp_t **out);

/* start 创建 nthreads 个 epoll 线程；stop 停收新 fd 并 drain 在途连接 */
int  tcp_iogrp_start(tcp_iogrp_t *g);
void tcp_iogrp_stop(tcp_iogrp_t *g);
void tcp_iogrp_destroy(tcp_iogrp_t *g);

/* accept 线程移交已 accept 的 fd（含对端地址）：内部负载均衡；
 * 失败时调用方负责 close(fd)。返回 AGAIN=组内队列满（拒收新连）。 */
int  tcp_iogrp_handoff(tcp_iogrp_t *g, int fd, const nsbc_sa_t *peer);

/* 连接错误/发送失败通知（从其他线程，内部转发到属主线程处理） */
void tcp_iogrp_close_fd(tcp_iogrp_t *g, int fd);

typedef struct {
    uint64_t conns_total;
    uint64_t conns_active;
    uint64_t slow_read_kills;
    uint64_t handshake_kills;
    uint64_t handoff_drops;
} tcp_iogrp_stat_t;
void tcp_iogrp_stat(const tcp_iogrp_t *g, tcp_iogrp_stat_t *out);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_TCP_IOGROUP_H */
