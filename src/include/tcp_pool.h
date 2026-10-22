#ifndef NSBC_TCP_POOL_H
#define NSBC_TCP_POOL_H

/* 出站 TCP 连接池（REQ-SIP-TCP-001，P1）
 *
 * 现状：sip_transport 仅复用入向连接，主动外呼硬编码 UDP；
 * b2b_originate 的 TCP/TLS leg 无连接来源。本池按对端地址（IP:port）
 * 复用非阻塞出站连接：连接在途→AGAIN 注册等待；idle keepalive 复用；
 * 容量对齐 5 万并发连接模型（第 9 章）。
 *
 * 分层：net 层；不引 sip（调用方 sip_transport 同/上层装配）。
 * TLS 升级以不透明 tls_ctx 传入（net/tls_io 产物），本文件不依赖 OpenSSL。
 */
#include "net_sock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_POOL_IDLE_DEF_MS   30000u  /* RFC3261 §18 建议空闲回收 */
#define TCP_POOL_CONN_SLOW_MS  5000u   /* connect 超时 */

typedef struct tcp_pool tcp_pool_t;

typedef struct {
    int       max_conns;       /* 硬上限（规划 50000） */
    int       per_peer_max;    /* 同对端最大连接数 */
    uint32_t  idle_ms;         /* 空闲保活时长 */
    uint32_t  connect_ms;      /* 非阻塞连接超时 */
    int       sndbuf_kb;
    int       rcvbuf_kb;
    void     *tls_ctx;         /* 可空：非 NULL 时出 TLS 连接 */
} tcp_pool_cfg_t;

typedef enum {
    TCP_POOL_FREE = 0,   /* 调用方仅发送：失败/关闭由池回收 */
    TCP_POOL_KEEP,       /* 发送后归还保活池 */
    TCP_POOL_DESTROY     /* 发送后直接关闭（对端声明 Connection: close） */
} tcp_pool_release_t;

void tcp_pool_cfg_default(tcp_pool_cfg_t *cfg);
int  tcp_pool_create(const tcp_pool_cfg_t *cfg, tcp_pool_t **out);
void tcp_pool_destroy(tcp_pool_t *p);

/* 取一条到 dst 的已连接 fd：
 *   OK     → *fd 立即可用（池内复用）；
 *   AGAIN  → 连接在途，调用方稍后重试（注册由池内部记账）；
 *   NOSPC  → 达 max_conns；NOENT/INVAL 参数或路由不可达。 */
int  tcp_pool_get(tcp_pool_t *p, const nsbc_sa_t *dst, int *fd);

/* reactor 可写事件回调（连接建立完成）：注册到池的在途 socket 完成 */
int  tcp_pool_on_writable(tcp_pool_t *p, int fd);
/* 对端 RST/出错：从池摘除并关闭 */
void tcp_pool_on_error(tcp_pool_t *p, int fd);

/* 发送结束归还（KEEP 回池；DESTROY 关闭） */
void tcp_pool_put(tcp_pool_t *p, int fd, tcp_pool_release_t how);

/* 周期回收 idle/死连（时间轮驱动；now_ms 单调时钟） */
int  tcp_pool_tick(tcp_pool_t *p, uint64_t now_ms, uint32_t *reaped);

/* OAM 计数 */
typedef struct {
    uint64_t total;
    uint64_t in_use;
    uint64_t connecting;
    uint64_t idle;
} tcp_pool_stat_t;
void tcp_pool_stat(const tcp_pool_t *p, tcp_pool_stat_t *out);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_TCP_POOL_H */
