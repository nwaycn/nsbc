/* HA 状态复制 TCP 传输层（P1，REQ-HA-002 / REQ-HA-004）
 *
 * 线程模型（稳定性 P0-1 硬约束：业务线程永不阻塞）：
 *   业务线程（registrar/b2bua）→ repl_publish() → ring_used++
 *       → ops.send = repl_ops_send() [本文件]
 *         → transport SPSC send_q 入队（memcpy 4KB + 2 次原子）
 *         → 立即返回（队列满 = NSBC_NOSPC → 业务转全量/告警）
 *   transport TCP 线程：独立 pthread，poll() 100ms 周期
 *     → drain send_q → HaReplV1 16B 头 + payload → TCP send
 *     → recv 对端帧 → decode → repl_ingest() → ops.apply
 *     → ACK 帧回推：payload_len 字段填 applied_seq
 *
 * socket 全部非阻塞，SO_RCVTIMEO/SO_SNDTIMEO 1s 兜底。
 * 连接建立：ACTIVE listen(port) + accept；STANDBY connect(peer) 指数退避。
 * 角色切换：ha.c 心跳线程 role 变更后调 ha_repl_trans_notify_role() 重连。
 */
#include "ha_repl_transport.h"
#include "cfg.h"
#include "ha.h"
#include "log.h"
#include "net_sock.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TRANS_POLL_MS          100
#define TRANS_CONNECT_RETRY_MAX_MS  5000

struct ha_repl_trans {
    int enabled;
    pthread_t thr;
    int stop;           /* __atomic_load/acquire, transport 线程检查 */

    int listen_fd;      /* ACTIVE: listen on port */
    int peer_fd;        /* connected peer fd (both roles, -1 表示未连) */

    ha_repl_t *repl;    /* transport owns */

    /* SPSC send queue: business thread push / transport thread pop */
    repl_rec_t send_q[REPL_RING_CAP];
    uint64_t send_head;
    uint64_t send_tail;

    uint64_t last_sent_seq;  /* 已 send 到 TCP 栈的最高 seq */
    uint64_t last_ack_seq;

    int connected;
    int is_active;
    nsbc_sa_t peer_addr;
    uint16_t port;
    int backoff_ms;      /* STANDBY connect 指数退避 */

    int have_peer;
};

/* === HaReplV1 帧编解码 === */

static int frame_encode(const repl_rec_t *r, char *out, uint32_t outsz)
{
    ha_repl_frame_hdr_t h;
    uint32_t hdr_sz = sizeof(h);
    uint32_t total = hdr_sz + r->len;
    if (total > outsz)
        return -1;
    memset(&h, 0, sizeof(h));
    h.magic       = htons(HA_REPL_FRAME_MAGIC);
    h.ver         = htons(HA_REPL_FRAME_VER);
    h.seq         = htobe64(r->seq);
    h.type        = (uint8_t)r->type;
    h.reserved    = 0;
    h.payload_ver = htons(r->ver);
    h.payload_len = htonl(r->len);
    memcpy(out, &h, hdr_sz);
    if (r->len > 0)
        memcpy(out + hdr_sz, r->payload, r->len);
    return (int)total;
}

static int frame_decode_hdr(const char *buf, ha_repl_frame_hdr_t *h)
{
    if (buf == NULL || h == NULL) return -1;
    memcpy(h, buf, sizeof(*h));
    h->magic       = ntohs(h->magic);
    h->ver         = ntohs(h->ver);
    h->seq         = be64toh(h->seq);
    h->payload_ver = ntohs(h->payload_ver);
    h->payload_len = ntohl(h->payload_len);
    if (h->magic != HA_REPL_FRAME_MAGIC) return -1;
    if (h->ver > HA_REPL_FRAME_VER) return -1; /* 版本不匹配 */
    if (h->payload_len > REPL_PAYLOAD_MAX) return -1;
    return 0;
}

/* === repl_ops 回调（ha_replica 调用） === */

static int repl_ops_send(void *ud, const repl_rec_t *r)
{
    ha_repl_trans_t *t = ud;
    if (t == NULL || r == NULL)
        return NSBC_INVAL;
    uint64_t head = __atomic_load_n(&t->send_head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&t->send_tail, __ATOMIC_ACQUIRE);
    /* SPSC: 单生产者（business thread）+ 单消费者（transport thread），
     * 没有 ABA 问题 —— 槽位覆盖前消费者一定已经读完 */
    if (head - tail >= REPL_RING_CAP) {
        llog(LOG_WARN, "ha-repl",
             "transport SPSC ring FULL head=%llu tail=%llu seq=%llu → NOSPC",
             (unsigned long long)head, (unsigned long long)tail,
             (unsigned long long)r->seq);
        return NSBC_NOSPC;
    }
    uint64_t slot = head % REPL_RING_CAP;
    memcpy(&t->send_q[slot], r, sizeof(*r));
    __atomic_store_n(&t->send_head, head + 1, __ATOMIC_RELEASE);
    return NSBC_OK;
}

static int repl_ops_on_gap(void *ud, uint64_t expect, uint64_t got)
{
    /* P1 skeleton: gap 重传通过 GAP_REQ 帧触发主端重发；当前骨架日志即可 */
    llog(LOG_INFO, "ha-repl", "repl gap expect=%llu got=%llu → request resync",
         (unsigned long long)expect, (unsigned long long)got);
    (void)ud;
    return NSBC_OK;
}

/* === TCP 辅助 === */

static int tcp_set_nonblock_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) return -1;
    (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    return 0;
}

static int tcp_connect_blocking(const nsbc_sa_t *peer, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    tcp_set_nonblock_cloexec(fd);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (tv.tv_sec == 0 && tv.tv_usec == 0) { tv.tv_sec = 1; tv.tv_usec = 0; }
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int rc = connect(fd, (const struct sockaddr *)&peer->ss, peer->len);
    if (rc < 0 && errno == EINPROGRESS) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        rc = poll(&pfd, 1, timeout_ms);
        if (rc <= 0) { close(fd); return -1; }
        int sockerr = 0; socklen_t sl = sizeof(sockerr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &sl);
        if (sockerr != 0) { close(fd); return -1; }
    } else if (rc < 0) {
        close(fd); return -1;
    }
    /* 禁用 Nagle：复制帧要低延迟 */
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

/* === transport TCP 线程主循环 === */

static void *ha_repl_trans_thread(void *arg)
{
    ha_repl_trans_t *t = arg;
    char rbuf[REPL_PAYLOAD_MAX + 128];

    while (!__atomic_load_n(&t->stop, __ATOMIC_ACQUIRE)) {
        if (t->is_active && t->listen_fd >= 0 && t->peer_fd < 0) {
            /* ACTIVE: 等备端 connect */
            struct pollfd pfd = { t->listen_fd, POLLIN, 0 };
            int rc = poll(&pfd, 1, TRANS_POLL_MS);
            if (rc > 0 && (pfd.revents & POLLIN)) {
                nsbc_sa_t peer; memset(&peer, 0, sizeof(peer));
                int afd = net_tcp_accept(t->listen_fd, &peer);
                if (afd >= 0) {
                    tcp_set_nonblock_cloexec(afd);
                    t->peer_fd = afd;
                    t->connected = 1;
                    t->backoff_ms = 100;
                    llog(LOG_NOTICE, "ha-repl",
                         "ACTIVE accepted peer repl tcp port=%u", t->port);
                    /* 关闭 listen —— 同一时间只接一个备端 */
                    close(t->listen_fd);
                    t->listen_fd = -1;
                }
            }
        } else if (!t->is_active && t->have_peer && t->peer_fd < 0) {
            /* STANDBY: 指数退避 connect 主端 */
            if (t->backoff_ms < TRANS_CONNECT_RETRY_MAX_MS) {
                t->backoff_ms = t->backoff_ms * 2;
                if (t->backoff_ms > TRANS_CONNECT_RETRY_MAX_MS)
                    t->backoff_ms = TRANS_CONNECT_RETRY_MAX_MS;
            }
            uint64_t wait = t->backoff_ms;
            /* 用 poll 做 sleep，响应 stop */
            int waited = 0;
            while (waited < (int)wait &&
                   !__atomic_load_n(&t->stop, __ATOMIC_ACQUIRE)) {
                struct pollfd pfd = { 0, 0, 0 };
                int rc = poll(&pfd, 1, 50);
                (void)rc; waited += 50;
            }
            if (__atomic_load_n(&t->stop, __ATOMIC_ACQUIRE)) break;
            int fd = tcp_connect_blocking(&t->peer_addr, 1000);
            if (fd >= 0) {
                t->peer_fd = fd;
                t->connected = 1;
                t->backoff_ms = 100;
                llog(LOG_NOTICE, "ha-repl", "STANDBY repl tcp connected port=%u",
                     t->port);
                /* 全量对齐：通知 ha_replica 立即触发 REPL_FULL（骨架中置位） */
            } else {
                llog(LOG_DEBUG, "ha-repl",
                     "STANDBY repl connect failed errno=%d → retry %dms",
                     errno, t->backoff_ms);
            }
        }

        /* 收发：peer_fd >= 0 时才有数据通路 */
        if (t->peer_fd >= 0) {
            struct pollfd pfds[2];
            int np = 0;
            pfds[np].fd = t->peer_fd;
            pfds[np].events = POLLIN | POLLOUT;
            np++;
            int rc = poll(pfds, np, TRANS_POLL_MS);
            if (rc < 0 && errno != EINTR) {
                llog(LOG_WARN, "ha-repl", "poll errno=%d → reconnect", errno);
                goto reconnect;
            }
            /* === 发 === */
            if (rc > 0 && (pfds[0].revents & POLLOUT)) {
                uint64_t head = __atomic_load_n(&t->send_head, __ATOMIC_ACQUIRE);
                uint64_t tail = __atomic_load_n(&t->send_tail, __ATOMIC_ACQUIRE);
                while (tail < head) {
                    uint64_t slot = tail % REPL_RING_CAP;
                    char frame[REPL_PAYLOAD_MAX + 128];
                    int flen = frame_encode(&t->send_q[slot], frame, sizeof(frame));
                    if (flen <= 0) break;
                    ssize_t s = send(t->peer_fd, frame, (size_t)flen, MSG_NOSIGNAL);
                    if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    if (s < 0) {
                        llog(LOG_WARN, "ha-repl", "repl send errno=%d → reconnect",
                             errno);
                        goto reconnect;
                    }
                    tail++;
                    __atomic_store_n(&t->send_tail, tail, __ATOMIC_RELEASE);
                }
            }
            /* === 收 === */
            if (rc > 0 && (pfds[0].revents & POLLIN)) {
                ssize_t r = recv(t->peer_fd, rbuf, sizeof(rbuf), 0);
                if (r == 0) {
                    /* 对端关 */
                    llog(LOG_WARN, "ha-repl", "peer closed repl tcp → reconnect");
                    goto reconnect;
                }
                if (r < 0 && errno == EAGAIN) continue;
                if (r < 0) {
                    llog(LOG_WARN, "ha-repl", "repl recv errno=%d → reconnect",
                         errno);
                    goto reconnect;
                }
                /* decode + dispatch */
                if ((size_t)r >= sizeof(ha_repl_frame_hdr_t)) {
                    ha_repl_frame_hdr_t hdr;
                    if (frame_decode_hdr(rbuf, &hdr) == 0) {
                        if (hdr.type == HA_REPL_FRAME_ACK_TYPE) {
                            /* ACK 帧：payload_len 填 applied_seq（跨协议约定） */
                            uint64_t ack = hdr.payload_len;
                            if (ack > hdr.seq) ack = hdr.seq;
                            repl_ack(t->repl, ack);
                            t->last_ack_seq = ack;
                        } else {
                            repl_rec_t rec;
                            memset(&rec, 0, sizeof(rec));
                            rec.seq = hdr.seq;
                            rec.type = (repl_type_t)hdr.type;
                            rec.ver = hdr.payload_ver;
                            rec.len = (uint16_t)hdr.payload_len;
                            if (rec.len > 0 && (size_t)r >= sizeof(hdr) + rec.len)
                                memcpy(rec.payload, rbuf + sizeof(hdr), rec.len);
                            (void)repl_ingest(t->repl, &rec);
                            /* 每收一帧发 ACK —— 简化 P1 骨架 */
                            char ackbuf[sizeof(ha_repl_frame_hdr_t)];
                            ha_repl_frame_hdr_t ah;
                            memset(&ah, 0, sizeof(ah));
                            ah.magic = htons(HA_REPL_FRAME_MAGIC);
                            ah.ver   = htons(HA_REPL_FRAME_VER);
                            ah.type  = HA_REPL_FRAME_ACK_TYPE;
                            ah.payload_len = htonl((uint32_t)repl_applied_seq(t->repl));
                            memcpy(ackbuf, &ah, sizeof(ah));
                            (void)send(t->peer_fd, ackbuf, sizeof(ah), MSG_NOSIGNAL);
                        }
                    }
                }
            }
            if (rc > 0 && (pfds[0].revents & (POLLERR | POLLHUP))) {
                llog(LOG_WARN, "ha-repl", "peer POLLERR/HUP → reconnect");
                goto reconnect;
            }
        }
        continue;

reconnect:
        if (t->peer_fd >= 0) { close(t->peer_fd); t->peer_fd = -1; }
        t->connected = 0;
        if (t->is_active && t->listen_fd < 0) {
            /* ACTIVE 重新 listen 等备端 */
            int lfd = net_tcp_listen(NULL, t->port, 16, 0);
            if (lfd >= 0) {
                tcp_set_nonblock_cloexec(lfd);
                t->listen_fd = lfd;
            } else {
                llog(LOG_WARN, "ha-repl",
                     "ACTIVE relisten errno=%d; degrade to solo-active", errno);
            }
        }
    }
    /* transport 线程退出前 close socket */
    if (t->peer_fd >= 0) close(t->peer_fd);
    if (t->listen_fd >= 0) close(t->listen_fd);
    return NULL;
}

/* === 公共 API === */

int ha_repl_trans_create(const ha_repl_trans_cfg_t *cfg, ha_repl_trans_t **out)
{
    if (cfg == NULL || out == NULL)
        return NSBC_INVAL;
    if (!cfg->enabled || !cfg->have_peer) {
        /* 关闭或单机 —— transport 创建但 transport 线程不会连对端，
         * repl_publish 永远不被调（因为 ACTIVE 检查），开销只一个 pthread_create。
         * 更激进的降级：直接返回 NSBC_NOENT 让 ha.c 跳过集成。 */
        *out = NULL;
        return NSBC_NOENT;
    }
    ha_repl_trans_t *t = calloc(1, sizeof(*t));
    if (t == NULL)
        return NSBC_NOMEM;
    t->enabled = cfg->enabled;
    t->is_active = cfg->is_active;
    t->port = cfg->port;
    t->have_peer = cfg->have_peer;
    t->peer_addr = cfg->peer_addr;
    t->peer_fd = -1;
    t->listen_fd = -1;
    t->backoff_ms = 100;
    t->last_sent_seq = 0;
    t->last_ack_seq = 0;
    __atomic_store_n(&t->send_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&t->send_tail, 0, __ATOMIC_RELEASE);

    /* ha_replica ops 装配 —— send 入队 transport SPSC;
     * apply 走 ha_repl_apply_proxy 多路分发（registrar/b2bua 各自 subscribe_apply） */
    repl_ops_t ops = {
        .send   = repl_ops_send,
        .apply  = ha_repl_apply_proxy,   /* ha.h 内声明 — proxy 遍历订阅链 */
        .on_gap = repl_ops_on_gap,
        .ud     = t,
    };
    if (repl_create(&ops, cfg->is_active, &t->repl) != NSBC_OK) {
        free(t);
        return NSBC_NOMEM;
    }

    /* ACTIVE: 立即 listen；STANDBY: 线程里指数退避 connect */
    if (t->is_active) {
        int lfd = net_tcp_listen(NULL, t->port, 16, 0);
        if (lfd < 0) {
            llog(LOG_WARN, "ha-repl",
                 "listen tcp/%u errno=%d; degrade to solo-active",
                 t->port, errno);
            repl_destroy(t->repl);
            free(t);
            *out = NULL;
            return NSBC_OK; /* 降级，不拖死整机 */
        }
        tcp_set_nonblock_cloexec(lfd);
        t->listen_fd = lfd;
    }

    __atomic_store_n(&t->stop, 0, __ATOMIC_RELEASE);
    if (pthread_create(&t->thr, NULL, ha_repl_trans_thread, t) != 0) {
        llog(LOG_WARN, "ha-repl",
             "transport thread create errno=%d; degrade", errno);
        if (t->listen_fd >= 0) close(t->listen_fd);
        repl_destroy(t->repl);
        free(t);
        *out = NULL;
        return NSBC_OK;
    }
    llog(LOG_NOTICE, "ha-repl",
         "transport enabled role=%s port=%u",
         t->is_active ? "ACTIVE" : "STANDBY", t->port);
    *out = t;
    return NSBC_OK;
}

void ha_repl_trans_destroy(ha_repl_trans_t *t)
{
    if (t == NULL) return;
    __atomic_store_n(&t->stop, 1, __ATOMIC_RELEASE);
    pthread_join(t->thr, NULL);
    if (t->repl) repl_destroy(t->repl);
    free(t);
}

ha_repl_t *ha_repl_trans_repl(ha_repl_trans_t *t)
{
    return t ? t->repl : NULL;
}

void ha_repl_trans_set_apply_ops(ha_repl_trans_t *t,
                                 int (*apply)(void *ud, const repl_rec_t *r),
                                 int (*on_gap)(void *ud, uint64_t expect, uint64_t got),
                                 void *ud)
{
    if (t == NULL || t->repl == NULL) return;
    /* 通过 repl_set_ops 覆盖 ha_repl 内部 ops.apply/on_gap。
     * 不 race：apply/on_gap 只在 transport recv 线程调，本函数在业务模块 init 调
     * （此时 transport 线程尚未启动或尚未开始 recv）。 */
    repl_set_ops(t->repl, apply, on_gap, ud);
    llog(LOG_NOTICE, "ha-repl", "set apply ops apply=%p on_gap=%p",
         (void *)apply, (void *)on_gap);
}

int ha_repl_trans_connected(ha_repl_trans_t *t)
{
    return t ? t->connected : 0;
}

uint64_t ha_repl_trans_last_ack(ha_repl_trans_t *t)
{
    return t ? t->last_ack_seq : 0;
}

void ha_repl_trans_notify_role(ha_repl_trans_t *t, ha_role_t role)
{
    if (t == NULL) return;
    int new_active = (role == HA_ROLE_ACTIVE);
    if (new_active == t->is_active) return;
    t->is_active = new_active;
    /* 角色切换：transport 线程下次 poll 周期起走新分支
     * （ACTIVE=accept listen；STANDBY=connect 对端） */
    llog(LOG_NOTICE, "ha-repl",
         "notify_role → %s listen=%d peer_fd=%d",
         new_active ? "ACTIVE" : "STANDBY",
         t->listen_fd, t->peer_fd);
    /* ACTIVE 切换回来时若 listen_fd 已关，重新 open */
    if (new_active && t->listen_fd < 0 && t->enabled) {
        int lfd = net_tcp_listen(NULL, t->port, 16, 0);
        if (lfd >= 0) {
            tcp_set_nonblock_cloexec(lfd);
            t->listen_fd = lfd;
        } else {
            llog(LOG_WARN, "ha-repl",
                 "ACTIVE notify: relisten errno=%d", errno);
        }
    }
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
