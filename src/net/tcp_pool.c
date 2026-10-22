/* 出站 TCP 连接池——粗骨架（REQ-SIP-TCP-001，P1）
 *
 * TODO 接线：sip_transport.sip_tr_send 的 TCP 分支当前只查入向表，
 * 外呼路径改走 tcp_pool_get；b2b_originate leg proto 从配置/路由取，
 * 不再硬编码 UDP。实现时连接表用分片哈希（对端 nsbc_sa_t 键）+
 * 每连接时间轮两定时器（connect 超时/idle 回收）。
 */
#include "tcp_pool.h"

struct tcp_pool {
    tcp_pool_cfg_t cfg;
    tcp_pool_stat_t st;
    unsigned inited;
};

void tcp_pool_cfg_default(tcp_pool_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_conns = 50000;
    cfg->per_peer_max = 8;
    cfg->idle_ms = TCP_POOL_IDLE_DEF_MS;
    cfg->connect_ms = TCP_POOL_CONN_SLOW_MS;
}

int tcp_pool_create(const tcp_pool_cfg_t *cfg, tcp_pool_t **out)
{
    tcp_pool_t *p;
    if (cfg == NULL || out == NULL)
        return NSBC_INVAL;
    p = (tcp_pool_t *)calloc(1, sizeof(*p));
    if (p == NULL)
        return NSBC_NOMEM;
    p->cfg = *cfg;
    p->inited = 1u;
    *out = p;
    return NSBC_OK;
}

void tcp_pool_destroy(tcp_pool_t *p)
{
    /* TODO(P1)：遍历分片关闭全部 fd（shutdown 后 drain 在途写） */
    free(p);
}

int tcp_pool_get(tcp_pool_t *p, const nsbc_sa_t *dst, int *fd)
{
    if (p == NULL || dst == NULL || fd == NULL)
        return NSBC_INVAL;
    *fd = -1;
    /* TODO(P1)：查池→idle 命中复用；无则 socket()+nonblock connect()，
     * EINPROGRESS 入在途表并返回 AGAIN，注册 reactor 可写事件。 */
    return NSBC_NOTSUP;
}

int tcp_pool_on_writable(tcp_pool_t *p, int fd)
{
    if (p == NULL || fd < 0)
        return NSBC_INVAL;
    /* TODO(P1)：SO_ERROR 取连接结果，0→入 idle 表唤醒等待者，
     * 其余→摘除关闭并通知等待方按 503/失败重路由。 */
    return NSBC_NOTSUP;
}

void tcp_pool_on_error(tcp_pool_t *p, int fd)
{
    if (p == NULL || fd < 0)
        return;
    /* TODO(P1)：分片定位、摘除、close(fd)、统计 */
}

void tcp_pool_put(tcp_pool_t *p, int fd, tcp_pool_release_t how)
{
    if (p == NULL || fd < 0)
        return;
    (void)how;
    /* TODO(P1)：KEEP→刷在途写后回 idle 表并 arm idle 定时；
     * DESTROY/FREE 按语义关闭；TLS 连接先走 tls close notify。 */
}

int tcp_pool_tick(tcp_pool_t *p, uint64_t now_ms, uint32_t *reaped)
{
    if (p == NULL)
        return NSBC_INVAL;
    (void)now_ms;
    if (reaped != NULL)
        *reaped = 0u;
    return NSBC_OK;
}

void tcp_pool_stat(const tcp_pool_t *p, tcp_pool_stat_t *out)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (p != NULL)
        *out = p->st;
}
