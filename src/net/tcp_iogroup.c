/* TCP 独立 I/O 线程组——粗骨架（REQ-SIP-TCP-002，P0 架构项）
 *
 * 实现要点（接线批补）：
 *  - 每线程独立 epoll fd + eventfd 唤醒（handoff 队列）；
 *  - 每连接挂 tcp_framer（CL 定界），解析出完整帧才回调 rx；
 *  - 慢读判定：epoll_wait 超时粒度 200ms 扫连接 deadline 堆/时间轮；
 *  - TLS 连接握手期适用 handshake_ms，建立后切 slow_read_ms；
 *  - 线程内零 malloc 快路径：连接对象走 mem 域 slab（调用方注入）。
 * 本批仅轮廓，不创建线程。
 */
#include "tcp_iogroup.h"

struct tcp_iogrp {
    tcp_iogrp_cfg_t  cfg;
    tcp_iogrp_stat_t st;
    int              running;
};

void tcp_iogrp_cfg_default(tcp_iogrp_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->nthreads = 2;
    cfg->max_conns = 50000;
    cfg->slow_read_ms = TCP_IO_SLOW_READ_DEF_MS;
    cfg->handshake_ms = TCP_IO_HANDSHAKE_DEF_MS;
    cfg->per_conn_buf_kb = 32; /* 第 9 章：32KB/连接 → 5 万连 1.6GB */
}

int tcp_iogrp_create(const tcp_iogrp_cfg_t *cfg, tcp_iogrp_t **out)
{
    tcp_iogrp_t *g;
    if (cfg == NULL || out == NULL || cfg->nthreads <= 0 || cfg->rx == NULL)
        return NSBC_INVAL;
    g = (tcp_iogrp_t *)calloc(1, sizeof(*g));
    if (g == NULL)
        return NSBC_NOMEM;
    g->cfg = *cfg;
    *out = g;
    return NSBC_OK;
}

int tcp_iogrp_start(tcp_iogrp_t *g)
{
    if (g == NULL)
        return NSBC_INVAL;
    if (g->running)
        return NSBC_EXISTS;
    /* TODO(P0 接线)：建 nthreads 个 epoll+eventfd，pthread_create，
     * 线程名 nbc-tcpio-N；归属 worker 框架的 log_thread_attach。 */
    return NSBC_NOTSUP;
}

void tcp_iogrp_stop(tcp_iogrp_t *g)
{
    if (g == NULL || !g->running)
        return;
    /* TODO：置停→关监听 handoff→每连接发 BYE 不适用（传输层只 shutdown）→
     * join→残余 fd close 统计。 */
    g->running = 0;
}

void tcp_iogrp_destroy(tcp_iogrp_t *g)
{
    if (g == NULL)
        return;
    tcp_iogrp_stop(g);
    free(g);
}

int tcp_iogrp_handoff(tcp_iogrp_t *g, int fd, const nsbc_sa_t *peer)
{
    if (g == NULL || fd < 0 || peer == NULL)
        return NSBC_INVAL;
    if (!g->running)
        return NSBC_SHUTDOWN;
    /* TODO：选 least-conns 线程→入 SPSC handoff 环→write eventfd；
     * 环满（慢连接风暴）返 AGAIN，accept 线程限频关新连并计 drop。 */
    return NSBC_NOTSUP;
}

void tcp_iogrp_close_fd(tcp_iogrp_t *g, int fd)
{
    if (g == NULL || fd < 0)
        return;
    /* TODO：fd→属主线程表查询，跨线程时入该线程关闭队列并唤醒 */
}

void tcp_iogrp_stat(const tcp_iogrp_t *g, tcp_iogrp_stat_t *out)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (g != NULL)
        *out = g->st;
}
