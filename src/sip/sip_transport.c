#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sip_transport.h"
#include "sip_build.h"
#include "net_mmsg.h"
#include "net_sock.h"
#include "tcp_framer.h"
#include "tls_io.h"
#include "log.h"
#include "sip_profile.h"   /* sip_profile_at / sip_profile_count */
#include "ws_listener.h"
#include "security.h"     /* ban_is_banned / security_check_ip — reactor 层前置黑名单检查 */

/* reactor 线程内本地接收端口传递（b2bua.c 写入，sip_tr_send 读取，
 * 同一 epoll 线程内同步执行）。定义在此文件：sip_transport 是核心公共模块，
 * 保证其他模块（b2bua.c）可以 extern 引用。 */
__thread uint16_t g_rx_local_port = 0;

/* 从 AF_UNSPEC nsbc_sa_t padding 提取 ws_conn* — signaling.c 定义 */
void *sig_ws_bridge_conn_from_sa(const nsbc_sa_t *sa);

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <alloca.h>
#include <sys/socket.h>
#include <arpa/inet.h>   /* inet_addr / INADDR_ANY */

/* ------------------------------------------------------------------ */
/* UDP CL 预筛（RFC3261 §18.3 / REQ-SIP-017）                          */
/* ------------------------------------------------------------------ */
udp_cl_rc_t sip_udp_cl_check(const char *buf, size_t len, size_t max_msg)
{
    if (len > max_msg)
        return UDP_CL_DROP;
    /* 定位头/体分隔 */
    const char *sep = NULL;
    for (size_t i = 0; i + 3 < len; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
            buf[i + 3] == '\n') {
            sep = buf + i;
            break;
        }
        if (buf[i] == '\n' && buf[i + 1] == '\n') {
            sep = buf + i;
            break;
        }
    }
    if (!sep)
        return UDP_CL_OK; /* 无头体分隔：无 body 口径 */
    /* 头区大小写不敏感扫 Content-Length（仅一行，折叠不在此处理） */
    int cl_seen = 0;
    long cl = 0;
    const char *p = buf;
    while (p < sep) {
        const char *eol = memchr(p, '\n', (size_t)(sep - p));
        if (!eol)
            eol = sep;
        size_t ll = (size_t)(eol - p);
        /* 跳折叠行（以 SP/HT 开头） */
        if (ll > 0 && (p[0] == ' ' || p[0] == '\t')) {
            p = (eol < sep) ? eol + 1 : eol;
            continue;
        }
        size_t cl0 = sizeof("Content-Length") - 1;
        if (ll >= cl0 && !strncasecmp(p, "Content-Length", cl0)) {
            const char *q = p + cl0;
            while (q < eol && (*q == ' ' || *q == '\t'))
                ++q;
            if (q >= eol || *q != ':') {
                p = (eol < sep) ? eol + 1 : eol;
                continue;
            }
            ++q;
            while (q < eol && (*q == ' ' || *q == '\t'))
                ++q;
            if (cl_seen)
                return UDP_CL_DROP; /* 重复 CL */
            char tmp[16];
            size_t k = 0;
            while (q < eol && *q >= '0' && *q <= '9' && k < sizeof(tmp) - 1)
                tmp[k++] = *q++;
            while (q < eol && (*q == ' ' || *q == '\t' || *q == '\r'))
                ++q;
            if (k == 0 || q != eol)
                return UDP_CL_DROP; /* 非数字 */
            tmp[k] = '\0';
            cl = atol(tmp);
            cl_seen = 1;
        }
        p = (eol < sep) ? eol + 1 : eol;
    }
    if (!cl_seen)
        return UDP_CL_OK; /* 无 CL：数据报边界即消息边界（body 空口径） */
    /* body 起点：CRLFCRLF 之后 */
    size_t body_off;
    if (sep[0] == '\r')
        body_off = (size_t)(sep - buf) + 4;
    else
        body_off = (size_t)(sep - buf) + 2;
    if (body_off > len)
        return UDP_CL_DROP;
    size_t body_len = len - body_off;
    if (body_len > (size_t)cl)
        return UDP_CL_DROP;   /* CL 后有多余字节：MUST 丢弃 */
    if (body_len < (size_t)cl)
        return UDP_CL_BAD400; /* 截断：SHOULD 400 */
    return UDP_CL_OK;
}

/* ------------------------------------------------------------------ */
/* 连接表（TCP/TLS）                                                    */
/* ------------------------------------------------------------------ */
typedef struct tr_conn {
    int fd;
    int in_use;
    int is_tls;
    tls_conn_t *tls;
    int tls_ok;
    nsbc_sa_t peer;
    tcp_framer_t fr;
    uint8_t fbuf[1]; /* 柔性：实际分配 max_msg*2 */
    /* ---- (proto, peer) → conn* 哈希表链 ----
     * 单桶链地址法，conn_index 锁保护整个 idx_next 链表操作。 */
    struct tr_conn *idx_next;
    /* ---- 发送缓冲（TCP/TLS 通用）---- */
    uint8_t *sbuf;
    size_t   sbuf_cap;
    size_t   sbuf_head;
    size_t   sbuf_tail;
    int      sbuf_want_epollout;
    pthread_mutex_t sbuf_lock;
} tr_conn_t;

/* ------------------------------------------------------------------ */
/* (proto, peer) → conn* 哈希表（链地址法，64K 桶 — 取代 O(n) fd 空间扫描） */
/* ------------------------------------------------------------------ */
#define CONN_IDX_BITS 16          /* 65536 桶 */
#define CONN_IDX_CAP  (1 << CONN_IDX_BITS)

static inline uint64_t conn_key_hash(int is_tls, const nsbc_sa_t *peer)
{
    /* FNV-1a over 4-bit proto + family + port + 16B addr */
    uint64_t h = 14695981039346656037ULL;
    uint8_t proto_b = (uint8_t)(is_tls ? 2 : 1);
    const uint8_t *p = (const uint8_t *)&peer->ss;
    size_t n = (size_t)peer->len;
    h ^= proto_b; h *= 1099511628211ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i]; h *= 1099511628211ULL;
    }
    return h;
}

typedef struct conn_index {
    pthread_mutex_t lock;
    tr_conn_t **slots;     // [CONN_IDX_CAP] 链头
    size_t n;
} conn_index_t;

/* 多 profile 多端口监听：数组上限。当前两个 profile（internal+external）
 * 各一对 UDP/TCP = 4；预留到 16 足够未来扩展 */
#define SIP_TR_FD_MAX 16

struct sip_transport {
    sip_tr_cfg_t cfg;
    /* 多 fd 数组：启动时按 global cfg 创建第一组，reload profiles 后用
     * sip_tr_add_listeners() 追加。内部去重——同一端口不重复监听。 */
    int udp_fds[SIP_TR_FD_MAX];
    int udp_fd_count;
    int tcp_lfds[SIP_TR_FD_MAX];
    int tcp_count;
    int tls_lfds[SIP_TR_FD_MAX];
    int tls_count;
    net_rx_t rx;
    net_dgram_t dg[NET_BATCH];
    tr_conn_t **conns;
    int conns_cap;
    int conns_n;
    pthread_mutex_t clock; /* worker 外呼发送与 reactor 线程互斥 */
    conn_index_t idx;      /* (proto, peer) → conn* 哈希表 */
    /* per-IP 连接计数：4096 桶，hash(ip) % BUCKETS —— 防单 IP SYN flood 打满全局 conn_max */
#define IP_BUCKETS 4096
    uint32_t ip_conn_count[IP_BUCKETS];
    pthread_mutex_t ip_conn_lock;
};

/* reactor 分帧回调上下文 —— __thread：多 reactor 线程各自独立，互不覆盖 */
static __thread tr_conn_t *s_cur_conn;

static int conn_idx(sip_transport_t *t, int fd)
{
    if (fd < 0 || fd >= t->conns_cap)
        return -1;
    tr_conn_t *c = t->conns[fd];
    return (c && c->in_use) ? fd : -1;
}

static void conn_close(sip_transport_t *t, int fd)
{
    int i = conn_idx(t, fd);
    if (i < 0)
        return;
    tr_conn_t *c = t->conns[i];
    reactor_del(t->cfg.re, fd);
    /* ---- 从 (proto, peer) 哈希表摘除 ---- */
    if (t->idx.slots) {
        pthread_mutex_lock(&t->idx.lock);
        uint64_t h = conn_key_hash(c->is_tls, &c->peer);
        size_t bucket = h & (CONN_IDX_CAP - 1);
        tr_conn_t **pp = &t->idx.slots[bucket];
        while (*pp) {
            if (*pp == c) {
                *pp = (*pp)->idx_next;
                break;
            }
            pp = &(*pp)->idx_next;
        }
        --t->idx.n;
        pthread_mutex_unlock(&t->idx.lock);
    }
#ifdef NSBC_WITH_OPENSSL
    if (c->tls)
        tls_conn_free(c->tls);
#endif
    close(fd);
    free(c->sbuf);             /* 发送缓冲（可能还有残留数据，对端已断，直接丢） */
    pthread_mutex_destroy(&c->sbuf_lock);
    c->in_use = 0;
    /* per-IP 计数减一（必须在 free(c) 之前，否则 use-after-free） */
    {
        uint32_t bucket = 0;
        int fam = nsbc_sa_family(&c->peer);
        if (fam == AF_INET) {
            uint32_t ip = ((struct sockaddr_in *)&c->peer.ss)->sin_addr.s_addr;
            bucket = (ip ^ (ip >> 8) ^ (ip >> 16) ^ (ip >> 24)) % IP_BUCKETS;
        } else {
            const uint8_t *p = ((struct sockaddr_in6 *)&c->peer.ss)->sin6_addr.s6_addr;
            uint32_t h = 0;
            for (int k = 0; k < 16; k++) h ^= ((uint32_t)p[k]) << ((k % 4) * 8);
            bucket = h % IP_BUCKETS;
        }
        pthread_mutex_lock(&t->ip_conn_lock);
        if (t->ip_conn_count[bucket] > 0)
            t->ip_conn_count[bucket]--;
        pthread_mutex_unlock(&t->ip_conn_lock);
    }
    free(c);
    t->conns[i] = NULL;
    --t->conns_n;
}

/* ------------------------------------------------------------------ */
/* TCP/TLS 发送队列（解决 EAGAIN 部分写入时的字节流损坏炸弹）           */
/*                                                                      */
/* 单生产者（sip_tr_send 线程）单消费者（reactor EPOLLOUT 线程）         */
/*                                                                      */
/* 队列为空时：生产者入队 → 注册 EPOLLOUT → reactor 收到 → flush 消费    */
/* 队列非空时：生产者入队 → 不重复注册 EPOLLOUT → reactor 继续 flush     */
/* ------------------------------------------------------------------ */

/** 生产者入队 —— 把一段字节追加到 conn 的发送缓冲尾部
 *  @return 0=成功，-1=内存不足或 conn 已关闭 */
static int sip_tcp_enqueue(sip_transport_t *t, tr_conn_t *c,
                           const uint8_t *data, size_t len)
{
    if (!c || !c->in_use || !data || len == 0) return -1;

    pthread_mutex_lock(&c->sbuf_lock);
    size_t used = c->sbuf_tail - c->sbuf_head;
    size_t free_room = c->sbuf_cap - used;

    /* 空间不够 → 扩容（先压缩再判断，避免 realloc 无意义触发） */
    if (len > free_room) {
        if (c->sbuf_head > 0) {
            memmove(c->sbuf, c->sbuf + c->sbuf_head, used);
            c->sbuf_tail = used;
            c->sbuf_head = 0;
            free_room = c->sbuf_cap - used;
        }
        if (len > free_room) {
            size_t new_cap = c->sbuf_cap;
            while (new_cap - used < len)
                new_cap *= 2;
            uint8_t *nb = realloc(c->sbuf, new_cap);
            if (!nb) {
                pthread_mutex_unlock(&c->sbuf_lock);
                return -1;
            }
            c->sbuf = nb;
            c->sbuf_cap = new_cap;
        }
    }

    memcpy(c->sbuf + c->sbuf_tail, data, len);
    c->sbuf_tail += len;

    /* 队列先前为空 → 向 reactor 注册 EPOLLOUT（如果还没注册） */
    if (!c->sbuf_want_epollout) {
        c->sbuf_want_epollout = 1;
        int want = c->is_tls ? (EPOLLIN | EPOLLOUT | EPOLLRDHUP)
                             : (EPOLLIN | EPOLLOUT | EPOLLRDHUP);
        reactor_mod(t->cfg.re, c->fd, want);
    }

    pthread_mutex_unlock(&c->sbuf_lock);
    return 0;
}

/** 消费者 flush —— reactor EPOLLOUT 触发时调用
 *  @return 0=全部发完 / EAGAIN 留待下次，-1=致命错误（对端断开）*/
static int sip_tcp_flush(sip_transport_t *t, tr_conn_t *c)
{
    for (;;) {
        pthread_mutex_lock(&c->sbuf_lock);
        size_t used = c->sbuf_tail - c->sbuf_head;
        if (used == 0) {
            /* 队列空 → 关掉 EPOLLOUT，保留 EPOLLIN */
            c->sbuf_want_epollout = 0;
            pthread_mutex_unlock(&c->sbuf_lock);
            reactor_mod(t->cfg.re, c->fd,
                        c->is_tls ? (EPOLLIN | EPOLLRDHUP)
                                  : (EPOLLIN | EPOLLRDHUP));
            return 0;
        }
        uint8_t *p = c->sbuf + c->sbuf_head;
        pthread_mutex_unlock(&c->sbuf_lock);

        ssize_t n;
#ifdef NSBC_WITH_OPENSSL
        if (c->is_tls && c->tls) {
            int want = 0;
            n = tls_write(c->tls, p, used, &want);
            if (n == NSBC_AGAIN) {
                /* TLS 层需要等 WANT_READ/WRITE — reactor 对应关注位 */
                uint32_t want_ev = (want == TLS_WANT_WRITE) ? EPOLLOUT : EPOLLIN;
                reactor_mod(t->cfg.re, c->fd, want_ev | EPOLLRDHUP);
                return 0; /* 留待下次 */
            }
            if (n <= 0)
                return -1; /* TLS 写失败 → conn_close 处理 */
        } else
#endif
        {
            n = send(c->fd, p, used, MSG_NOSIGNAL);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0; /* 留待下次 EPOLLOUT */
            if (n == 0) return -1;
            if (n < 0) return -1;
        }

        /* 成功发出 n 字节 → 推进 head，满队列清零 */
        pthread_mutex_lock(&c->sbuf_lock);
        c->sbuf_head += (size_t)n;
        if (c->sbuf_head == c->sbuf_tail)
            c->sbuf_head = c->sbuf_tail = 0;
        pthread_mutex_unlock(&c->sbuf_lock);
    }
}

static int frame_cb(void *ud, const uint8_t *msg, size_t len)
{
    sip_transport_t *t = (sip_transport_t *)ud;
    if (!s_cur_conn)
        return 0;
    if (len > s_cur_conn->fr.max_msg)
        return 1;
    /* TCP/TLS reactor 层前置封禁检查（同 UDP） */
    if (security_is_blocked(&s_cur_conn->peer)) {
        static unsigned blk_tcp = 0;
        if (blk_tcp++ % 1024 == 0) {
            char dip[INET6_ADDRSTRLEN] = {0};
            nsbc_sa_fmt(&s_cur_conn->peer, dip, sizeof(dip));
            llog(LOG_NOTICE, "sip",
                 "TCP/TLS reactor silent-drop: blocked ip=%s (dropped=%u)",
                 dip, blk_tcp);
        }
        return 0;
    }
    t->cfg.rx(t->cfg.rx_ud,
              s_cur_conn->is_tls ? SIP_TR_TLS : SIP_TR_TCP,
              &s_cur_conn->peer, NULL, (const char *)msg, len);
    return 0;
}

static int feed_framer(sip_transport_t *t, tr_conn_t *c)
{
    frame_rc_t frc = FRAME_NEED_MORE;
    int nm = 0;
    s_cur_conn = c;
    tcp_framer_feed(&c->fr, NULL, 0, frame_cb, t, &frc, &nm);
    s_cur_conn = NULL;
    if (frc == FRAME_OVERFLOW || frc == FRAME_PROTO) {
        conn_close(t, c->fd);
        return -1;
    }
    return 0;
}

static void on_conn_event(int fd, uint32_t ev, void *arg)
{
    sip_transport_t *t = (sip_transport_t *)arg;
    if (ev & (EPOLLHUP | EPOLLERR)) {
        conn_close(t, fd);
        return;
    }
    int i = conn_idx(t, fd);
    if (i < 0)
        return;
    tr_conn_t *c = t->conns[i];

#ifdef NSBC_WITH_OPENSSL
    if (c->is_tls && c->tls && !c->tls_ok) {
        /* 非阻塞握手：WANT_READ/WRITE 切换 epoll 关注位 */
        for (;;) {
            tls_hs_state_t hs = tls_handshake_step(c->tls);
            if (hs == TLS_HS_OK) {
                c->tls_ok = 1;
                reactor_mod(t->cfg.re, fd, EPOLLIN | EPOLLRDHUP);
                break;
            }
            if (hs == TLS_HS_WANT_READ) {
                reactor_mod(t->cfg.re, fd, EPOLLIN | EPOLLRDHUP);
                return;
            }
            if (hs == TLS_HS_WANT_WRITE) {
                reactor_mod(t->cfg.re, fd,
                            EPOLLIN | EPOLLOUT | EPOLLRDHUP);
                return;
            }
            conn_close(t, fd);
            return;
        }
    }
#endif

    /* ---- EPOLLOUT：发送队列 flush（必须在 TLS 握手 OK 之后） ---- */
    if ((ev & EPOLLOUT) && c->sbuf_want_epollout) {
        int rc = sip_tcp_flush(t, c);
        if (rc < 0) {
            conn_close(t, fd);
            return;
        }
    }

    /* ---- EPOLLIN：数据接收 ---- */
    for (;;) {
        size_t off = c->fr.end;
        size_t room = c->fr.cap - off;
        if (room == 0) {
            if (feed_framer(t, c) < 0)
                return;
            off = c->fr.end;
            room = c->fr.cap - off;
            if (room == 0) {
                conn_close(t, fd);
                return;
            }
        }
        ssize_t r;
#ifdef NSBC_WITH_OPENSSL
        if (c->is_tls && c->tls) {
            int want = 0;
            r = tls_read(c->tls, c->fbuf + off, room, &want);
            if (r == NSBC_AGAIN) {
                reactor_mod(t->cfg.re, fd,
                            (want == TLS_WANT_WRITE ? EPOLLOUT : EPOLLIN) |
                                EPOLLRDHUP);
                break;
            }
            if (r == 0) {
                conn_close(t, fd);
                return;
            }
            if (r < 0) {
                conn_close(t, fd);
                return;
            }
        } else
#endif
        {
            r = recv(fd, c->fbuf + off, room, 0);
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (r == 0) {
                conn_close(t, fd);
                return;
            }
            if (r < 0) {
                conn_close(t, fd);
                return;
            }
        }
        c->fr.end = off + (size_t)r;
        if (feed_framer(t, c) < 0)
            return;
    }
}

static tr_conn_t *conn_accept(sip_transport_t *t, int lfd, int is_tls)
{
    nsbc_sa_t peer;
    int fd = net_tcp_accept(lfd, &peer);
    if (fd < 0)
        return NULL;
    if (t->conns_n >= t->cfg.tcp_conn_max) {
        close(fd);
        llog(LOG_WARN, "sip", "tcp conn limit %d, reject",
             t->cfg.tcp_conn_max);
        return NULL;
    }
    /* per-IP 连接计数（防单 IP SYN flood 打满全局 conn_max） */
    {
        int per_ip_max = t->cfg.tcp_conn_per_ip > 0 ? t->cfg.tcp_conn_per_ip : 50;
        /* IP hash：IPv4 直接取 4 字节 XOR；IPv6 取前 16 字节 XOR */
        uint32_t bucket = 0;
        int fam = nsbc_sa_family(&peer);
        if (fam == AF_INET) {
            uint32_t ip = ((struct sockaddr_in *)&peer.ss)->sin_addr.s_addr;
            bucket = (ip ^ (ip >> 8) ^ (ip >> 16) ^ (ip >> 24)) % IP_BUCKETS;
        } else {
            const uint8_t *p = ((struct sockaddr_in6 *)&peer.ss)->sin6_addr.s6_addr;
            uint32_t h = 0;
            for (int k = 0; k < 16; k++) h ^= ((uint32_t)p[k]) << ((k % 4) * 8);
            bucket = h % IP_BUCKETS;
        }
        pthread_mutex_lock(&t->ip_conn_lock);
        if (t->ip_conn_count[bucket] >= (uint32_t)per_ip_max) {
            pthread_mutex_unlock(&t->ip_conn_lock);
            close(fd);
            llog(LOG_WARN, "sip", "tcp per-IP conn limit %d, reject bucket=%u",
                 per_ip_max, bucket);
            return NULL;
        }
        t->ip_conn_count[bucket]++;
        pthread_mutex_unlock(&t->ip_conn_lock);
    }
    if (fd >= t->conns_cap) {
        int cap = t->conns_cap ? t->conns_cap * 2 : 256;
        while (cap <= fd)
            cap *= 2;
        tr_conn_t **nv = realloc(t->conns, cap * sizeof(*nv));
        if (!nv) {
            close(fd);
            return NULL;
        }
        memset(nv + t->conns_cap, 0,
               (cap - t->conns_cap) * sizeof(*nv));
        t->conns = nv;
        t->conns_cap = cap;
    }
    size_t fcap = t->cfg.max_msg * 2 + 16;
    tr_conn_t *c = calloc(1, sizeof(*c) + fcap - 1);
    if (!c) {
        close(fd);
        return NULL;
    }
    c->fd = fd;
    c->in_use = 1;
    c->is_tls = is_tls;
    c->peer = peer;
    /* TCP socket 选项：防半开连接 / idle 占 fd
     *   SO_KEEPALIVE=1（保活探测，空闲 2h 后 OS 自动探测）
     *   TCP_USER_TIMEOUT=idle_timeout_ms（强制断链，默认 30s）
     * TCP_USER_TIMEOUT 是 Linux 专有，但内核 <2.6.37 时 setsockopt 失败
     * 会 silent ignore — 不影响功能。 */
    {
        int keep = 1;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keep, sizeof(keep));
    }
    {
        int uto = t->cfg.tcp_idle_timeout_ms > 0
                  ? t->cfg.tcp_idle_timeout_ms : 30000;
        /* 毫秒单位（Linux 4.x+）；老内核 silent ignore */
#if defined(TCP_USER_TIMEOUT)
        setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &uto, sizeof(uto));
#else
        (void)uto;
#endif
    }
    tcp_framer_init(&c->fr, c->fbuf, fcap, t->cfg.max_msg);
    /* 发送缓冲初始 32KB，最大单 SIP 消息 ~6KB × 多帧缓冲足够；
     * 不够时自动扩容（sip_tcp_enqueue 里 realloc）。 */
    c->sbuf_cap = 32 * 1024;
    c->sbuf = malloc(c->sbuf_cap);
    if (!c->sbuf) {
        free(c);
        t->conns[fd] = NULL;
        --t->conns_n;
        close(fd);
        return NULL;
    }
    c->sbuf_head = c->sbuf_tail = 0;
    c->sbuf_want_epollout = 0;
    pthread_mutex_init(&c->sbuf_lock, NULL);
    /* ---- 插入 (proto, peer) 哈希表 ---- */
    c->idx_next = NULL;
    if (t->idx.slots) {
        pthread_mutex_lock(&t->idx.lock);
        uint64_t h = conn_key_hash(is_tls, &peer);
        size_t bucket = h & (CONN_IDX_CAP - 1);
        c->idx_next = t->idx.slots[bucket];
        t->idx.slots[bucket] = c;
        ++t->idx.n;
        pthread_mutex_unlock(&t->idx.lock);
    }
    t->conns[fd] = c;
    ++t->conns_n;
#ifdef NSBC_WITH_OPENSSL
    if (is_tls) {
        void *tls_ctx = __atomic_load_n(&t->cfg.tls_ctx, __ATOMIC_ACQUIRE);
        if (tls_ctx) {
            c->tls = tls_conn_accept((tls_ctx_t *)tls_ctx, fd);
            if (!c->tls) {
                /* 从哈希表摘除（已在前面插入） */
                if (t->idx.slots) {
                    pthread_mutex_lock(&t->idx.lock);
                    uint64_t h = conn_key_hash(is_tls, &peer);
                    size_t b = h & (CONN_IDX_CAP - 1);
                    tr_conn_t **pp = &t->idx.slots[b];
                    while (*pp && *pp != c) pp = &(*pp)->idx_next;
                    if (*pp) { *pp = c->idx_next; --t->idx.n; }
                    pthread_mutex_unlock(&t->idx.lock);
                }
                free(c->sbuf); pthread_mutex_destroy(&c->sbuf_lock);
                free(c);
                t->conns[fd] = NULL;
                --t->conns_n;
                close(fd);
                return NULL;
            }
        }
    }
#else
    if (is_tls) {
        llog(LOG_ERR, "sip", "TLS listener requires NSBC_WITH_OPENSSL");
        /* 从哈希表摘除 */
        if (t->idx.slots) {
            pthread_mutex_lock(&t->idx.lock);
            uint64_t h = conn_key_hash(is_tls, &peer);
            size_t b = h & (CONN_IDX_CAP - 1);
            tr_conn_t **pp = &t->idx.slots[b];
            while (*pp && *pp != c) pp = &(*pp)->idx_next;
            if (*pp) { *pp = c->idx_next; --t->idx.n; }
            pthread_mutex_unlock(&t->idx.lock);
        }
        free(c->sbuf); pthread_mutex_destroy(&c->sbuf_lock);
        free(c);
        t->conns[fd] = NULL;
        --t->conns_n;
        close(fd);
        return NULL;
    }
#endif
    reactor_add(t->cfg.re, fd,
                is_tls ? (EPOLLIN | EPOLLOUT | EPOLLRDHUP)
                       : (EPOLLIN | EPOLLRDHUP),
                on_conn_event, t);
    return c;
}

static void on_tcp_accept(int fd, uint32_t ev, void *arg)
{
    (void)ev;
    sip_transport_t *t = (sip_transport_t *)arg;
    /* 判断 fd 是否属于 TLS listener：遍历 tls_lfds 数组 */
    int is_tls = 0;
    for (int i = 0; i < t->tls_count; ++i) {
        if (t->tls_lfds[i] == fd) { is_tls = 1; break; }
    }
    for (;;) {
        if (!conn_accept(t, fd, is_tls))
            break;
    }
}

/* ------------------------------------------------------------------ */
/* UDP 接收                                                             */
/* ------------------------------------------------------------------ */
static void on_udp_event(int fd, uint32_t ev, void *arg)
{
    (void)ev;
    sip_transport_t *t = (sip_transport_t *)arg;
    /* 先用 fd 拿监听端口，后面填到每个 dgram 的 local.port 上 */
    uint16_t bind_port = 0;
    {
        struct sockaddr_in sin; socklen_t sl = sizeof(sin);
        if (getsockname(fd, (struct sockaddr *)&sin, &sl) == 0)
            bind_port = ntohs(sin.sin_port);
    }
    for (;;) {
        net_rx_prepare(&t->rx, t->dg);
        int n = net_rx_recv(fd, &t->rx, t->dg);
        if (n <= 0)
            break;
        for (int i = 0; i < n; ++i) {
            net_dgram_t *d = &t->dg[i];
            /* 补上监听端口（IP 来自 IP_PKTINFO，端口来自 getsockname） */
            if (bind_port > 0 && d->local.len > 0 &&
                d->local.ss.ss_family == AF_INET) {
                ((struct sockaddr_in *)&d->local.ss)->sin_port = htons(bind_port);
            }
            udp_cl_rc_t cr = sip_udp_cl_check((const char *)d->buf, d->len,
                                              t->cfg.max_msg);
            if (cr == UDP_CL_DROP)
                continue;
            if (cr == UDP_CL_BAD400) {
                /* SHOULD 400：截断报文解析不可靠，P0 记录并丢弃 */
                llog(LOG_INFO, "sip", "truncated UDP datagram %zuB, drop",
                     d->len);
                continue;
            }
            /* reactor 层前置封禁检查：auto-ban 表 + 静态 CIDR 黑名单。
             * 只查 IP，不查 UA/号码前缀（需要 parse 后的头，放 b2bua 层）。
             * 省掉被封 IP 的 sip_parse_ex + b2b_on_rx 调用栈开销。 */
            if (security_is_blocked(&d->peer)) {
                static unsigned blk = 0;
                if (blk++ % 1024 == 0) {
                    char dip[INET6_ADDRSTRLEN] = {0};
                    nsbc_sa_fmt(&d->peer, dip, sizeof(dip));
                    llog(LOG_NOTICE, "sip",
                         "UDP reactor silent-drop: blocked ip=%s (dropped=%u)",
                         dip, blk);
                }
                continue;
            }
            t->cfg.rx(t->cfg.rx_ud, SIP_TR_UDP, &d->peer, &d->local,
                      (const char *)d->buf, d->len);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 创建/销毁/发送                                                       */
/* ------------------------------------------------------------------ */
int sip_tr_create(const sip_tr_cfg_t *cfg, sip_transport_t **out)
{
    sip_transport_t *t = calloc(1, sizeof(*t));
    if (!t)
        return NSBC_NOMEM;
    t->cfg = *cfg;
    /* listen_ip 必须独立持有：cfg 来自调用者栈（signaling.c::sig_start 局部数组），
     * sig_start 返回后栈帧即释放；sync_from_profiles / add_listeners 在 reload 时
     * 用到 t->cfg.listen_ip，如果不 strdup 就是野指针 → bind 失败打印乱码地址。 */
    if (cfg->listen_ip && cfg->listen_ip[0]) {
        t->cfg.listen_ip = strdup(cfg->listen_ip);
        if (!t->cfg.listen_ip) {
            free(t);
            return NSBC_NOMEM;
        }
    }
    pthread_mutex_init(&t->clock, NULL);
    /* per-IP 连接计数表 */
    pthread_mutex_init(&t->ip_conn_lock, NULL);
    memset(t->ip_conn_count, 0, sizeof(t->ip_conn_count));
    /* ---- 初始化 (proto, peer) 哈希表 ---- */
    pthread_mutex_init(&t->idx.lock, NULL);
    t->idx.slots = calloc(CONN_IDX_CAP, sizeof(tr_conn_t *));
    if (!t->idx.slots) {
        sip_tr_destroy(t);
        return NSBC_NOMEM;
    }
    t->idx.n = 0;

    for (int i = 0; i < NET_BATCH; ++i) {
        t->dg[i].cap = cfg->max_msg;
        t->dg[i].buf = malloc(cfg->max_msg);
        if (!t->dg[i].buf) {
            sip_tr_destroy(t);
            return NSBC_NOMEM;
        }
    }

    /* 多 fd 数组初始化：全部 -1（sip_tr_destroy / add_listeners 用） */
    t->udp_fd_count = t->tcp_count = t->tls_count = 0;
    for (int i = 0; i < SIP_TR_FD_MAX; ++i) {
        t->udp_fds[i] = -1;
        t->tcp_lfds[i] = -1;
        t->tls_lfds[i] = -1;
    }

    /* dualstack=1: SIP socket 保持 IPv6 通配监听双栈。
     * dualstack=0 时收到完整 SIP 包触发 libc segfault（sip parser 处理
     * 假设 sockaddr_in6 结构）。relay socket 单独用 dualstack=0。 */
    net_udp_cfg_t uc = {cfg->reuseport, cfg->rcvbuf_kb * 1024,
                        cfg->sndbuf_kb * 1024, 1, 1};
    if (cfg->enable_udp) {
        int fd = net_udp_open(cfg->listen_ip, cfg->udp_port, &uc);
        if (fd < 0) {
            llog(LOG_CRIT, "sip", "udp listen %s:%u failed",
                 cfg->listen_ip, cfg->udp_port);
            sip_tr_destroy(t);
            return NSBC_ERR;
        }
        t->udp_fds[0] = fd;
        t->udp_fd_count = 1;
        reactor_add(t->cfg.re, fd, EPOLLIN, on_udp_event, t);
    }
    if (cfg->enable_tcp) {
        int fd = net_tcp_listen(cfg->listen_ip, cfg->tcp_port, 4096,
                                 cfg->reuseport);
        if (fd < 0) {
            llog(LOG_CRIT, "sip", "tcp listen %s:%u failed",
                 cfg->listen_ip, cfg->tcp_port);
            sip_tr_destroy(t);
            return NSBC_ERR;
        }
        t->tcp_lfds[0] = fd;
        t->tcp_count = 1;
        reactor_add(t->cfg.re, fd, EPOLLIN, on_tcp_accept, t);
    }
    if (cfg->enable_tls) {
        int fd = net_tcp_listen(cfg->listen_ip, cfg->tls_port, 4096,
                                 cfg->reuseport);
        if (fd < 0) {
            llog(LOG_CRIT, "sip", "tls listen %s:%u failed",
                 cfg->listen_ip, cfg->tls_port);
            sip_tr_destroy(t);
            return NSBC_ERR;
        }
        t->tls_lfds[0] = fd;
        t->tls_count = 1;
        reactor_add(t->cfg.re, fd, EPOLLIN, on_tcp_accept, t);
    }
    *out = t;
    return NSBC_OK;
}

void sip_tr_set_rx_ud(sip_transport_t *t, void *ud)
{
    if (t)
        t->cfg.rx_ud = ud;
}

/* TLS ctx 热替换（SIGHUP reload 证书续期用）*/
void sip_tr_set_tls_ctx(sip_transport_t *t, void *ctx)
{
    if (t)
        __atomic_store_n(&t->cfg.tls_ctx, ctx, __ATOMIC_RELEASE);
}

void sip_tr_destroy(sip_transport_t *t)
{
    if (!t)
        return;
    /* 关闭所有多端口监听 fd */
    for (int i = 0; i < t->udp_fd_count; ++i) {
        if (t->udp_fds[i] >= 0) close(t->udp_fds[i]);
    }
    for (int i = 0; i < t->tcp_count; ++i) {
        if (t->tcp_lfds[i] >= 0) close(t->tcp_lfds[i]);
    }
    for (int i = 0; i < t->tls_count; ++i) {
        if (t->tls_lfds[i] >= 0) close(t->tls_lfds[i]);
    }
    t->udp_fd_count = t->tcp_count = t->tls_count = 0;
    if (t->conns) {
        for (int i = 0; i < t->conns_cap; ++i) {
            if (t->conns[i]) {
                tr_conn_t *c = t->conns[i];
#ifdef NSBC_WITH_OPENSSL
                if (c->tls) tls_conn_free(c->tls);
#endif
                close(c->fd);
                free(c->sbuf);
                pthread_mutex_destroy(&c->sbuf_lock);
                free(c);
            }
        }
        free(t->conns);
    }
    /* ---- 释放哈希表 ---- */
    free(t->idx.slots);
    pthread_mutex_destroy(&t->idx.lock);
    for (int i = 0; i < NET_BATCH; ++i)
        free(t->dg[i].buf);
    pthread_mutex_destroy(&t->clock);
    pthread_mutex_destroy(&t->ip_conn_lock);
    free((void *)t->cfg.listen_ip);   /* strdup 来的，sip_tr_create 里分配 */
    free(t);
}

int sip_tr_send(sip_transport_t *t, sip_tr_proto_t proto,
                const char *buf, size_t len, const nsbc_sa_t *dst)
{
    if (proto == SIP_TR_UDP) {
        int udp = -1;
        uint16_t use_port = g_rx_local_port;
        /* 诊断：打印所有 UDP fd 端口 + 本次尝试匹配的目标端口 */
        char fdlist[256] = {0};
        for (int i = 0; i < t->udp_fd_count && i < 8; ++i) {
            struct sockaddr_in sin; socklen_t sl = sizeof(sin);
            uint16_t p = 0;
            if (t->udp_fds[i] >= 0 &&
                getsockname(t->udp_fds[i], (struct sockaddr *)&sin, &sl) == 0)
                p = ntohs(sin.sin_port);
            char tmp[32]; snprintf(tmp, sizeof(tmp), "[%d]fd=%d:port=%u ",
                                   i, t->udp_fds[i], p);
            strncat(fdlist, tmp, sizeof(fdlist) - strlen(fdlist) - 1);
        }
        llog(LOG_INFO, "sip-tr",
             "UDP send: g_rx_local_port=%u fd_count=%d fds=%s",
             use_port, t->udp_fd_count, fdlist);
        if (use_port > 0 && t->udp_fd_count > 1) {
            for (int i = 0; i < t->udp_fd_count; ++i) {
                struct sockaddr_in sin; socklen_t sl = sizeof(sin);
                if (t->udp_fds[i] >= 0 &&
                    getsockname(t->udp_fds[i], (struct sockaddr *)&sin, &sl) == 0 &&
                    ntohs(sin.sin_port) == use_port) {
                    udp = t->udp_fds[i];
                    llog(LOG_INFO, "sip-tr", "UDP send match: i=%d fd=%d port=%u", i, udp, use_port);
                    break;
                }
            }
        }
        if (udp < 0 && t->udp_fd_count > 0) {
            udp = t->udp_fds[0];
            llog(LOG_INFO, "sip-tr", "UDP send fallback udp_fds[0] fd=%d", udp);
        }
        if (udp < 0) {
            llog(LOG_WARN, "sip-tr", "UDP send: no udp fds!");
            return NSBC_NOTSUP;
        }
        net_tx_dgram_t dg = {buf, len, *dst};
        int n = net_tx_batch(udp, &dg, 1);
        return n == 1 ? NSBC_OK : NSBC_AGAIN;
    }
    /* ---- SIP over WebSocket (RFC 7118) ---- */
    if (proto == SIP_TR_WS || proto == SIP_TR_WSS) {
        void *conn = sig_ws_bridge_conn_from_sa(dst);
        llog(LOG_INFO, "sip-tr", "WS send proto=%d conn=%p dst_fam=%d",
             (int)proto, conn, dst ? dst->ss.ss_family : -1);
        if (!conn) {
            llog(LOG_WARN, "ws", "send: no ws_conn in dst sa");
            return NSBC_NOTSUP;
        }
        int rc = ws_conn_send(conn, buf, len);
        llog(LOG_INFO, "sip-tr", "WS send rc=%d", rc);
        if (rc < 0) return NSBC_ERR;
        return NSBC_OK;
    }
    /* TCP/TLS：仅复用入向连接（P0；主动外呼连接管理 P1） */
    int want_tls = (proto == SIP_TR_TLS);
    pthread_mutex_lock(&t->idx.lock);
    tr_conn_t *found = NULL;
    uint64_t h = conn_key_hash(want_tls, dst);
    size_t bucket = h & (CONN_IDX_CAP - 1);
    for (tr_conn_t *c = t->idx.slots[bucket]; c; c = c->idx_next) {
        if (c->in_use && (c->is_tls == want_tls) && nsbc_sa_eq(&c->peer, dst)) {
            found = c;
            break;
        }
    }
    pthread_mutex_unlock(&t->idx.lock);
    if (!found)
        return NSBC_NOTSUP;
    /* 入队发送 —— 解决 EAGAIN 部分写入时的字节流损坏炸弹。
     * sip_tcp_enqueue 返回 0 表示数据已安全落发送缓冲，
     * 稍后 reactor EPOLLOUT 会驱动 flush。 */
    int rc = sip_tcp_enqueue(t, found, (const uint8_t *)buf, len);
    if (rc < 0)
        return NSBC_ERR;
    return NSBC_OK;
}

int sip_tr_udp_fd(const sip_transport_t *t)
{
    /* 向后兼容：返回第一个 UDP fd；多端口场景用 sip_tr_udp_fd_at(i) */
    return (t && t->udp_fd_count > 0) ? t->udp_fds[0] : -1;
}

int sip_tr_udp_fd_count(const sip_transport_t *t)
{
    return t ? t->udp_fd_count : 0;
}

int sip_tr_udp_fd_at(const sip_transport_t *t, int idx)
{
    if (!t || idx < 0 || idx >= t->udp_fd_count) return -1;
    return t->udp_fds[idx];
}

/* 动态添加一组 UDP/TCP 监听（profile reload 后追加端口用）。
 * 内部按 (ip, port) 组合去重——同一 IP:port 已存在直接跳过。 */
int sip_tr_add_listeners(sip_transport_t *t, const char *listen_ip,
                         uint16_t udp_port, uint16_t tcp_port, uint16_t tls_port)
{
    if (!t || !listen_ip || !listen_ip[0])
        return NSBC_INVAL;

    net_udp_cfg_t uc = {t->cfg.reuseport, t->cfg.rcvbuf_kb * 1024,
                        t->cfg.sndbuf_kb * 1024, 1, 1};

    int added = 0;

    /* ---- UDP：按 (ip, port) 去重（getsockname 查 bind 结果） ---- */
    if (udp_port > 0) {
        int dup = 0;
        for (int i = 0; i < t->udp_fd_count; ++i) {
            struct sockaddr_in sin; socklen_t sl = sizeof(sin);
            if (getsockname(t->udp_fds[i], (struct sockaddr *)&sin, &sl) == 0 &&
                ntohs(sin.sin_port) == udp_port) {
                uint32_t bound_ip = sin.sin_addr.s_addr;
                if (strcmp(listen_ip, "0.0.0.0") == 0 ||
                    ntohl(bound_ip) == INADDR_ANY ||
                    inet_addr(listen_ip) == bound_ip) {
                    dup = 1; break;
                }
            }
        }
        if (!dup && t->udp_fd_count < SIP_TR_FD_MAX) {
            int fd = net_udp_open(listen_ip, udp_port, &uc);
            if (fd < 0) {
                llog(LOG_WARN, "sip", "add udp listen %s:%u failed",
                     listen_ip, udp_port);
                return NSBC_ERR;
            }
            t->udp_fds[t->udp_fd_count++] = fd;
            reactor_add(t->cfg.re, fd, EPOLLIN, on_udp_event, t);
            llog(LOG_NOTICE, "sip", "added udp listener %s:%u (total=%d)",
                 listen_ip, udp_port, t->udp_fd_count);
            added++;
        }
    }

    /* ---- TCP：按 (ip, port) 去重 ---- */
    if (tcp_port > 0) {
        int dup = 0;
        for (int i = 0; i < t->tcp_count; ++i) {
            struct sockaddr_in sin; socklen_t sl = sizeof(sin);
            if (getsockname(t->tcp_lfds[i], (struct sockaddr *)&sin, &sl) == 0 &&
                ntohs(sin.sin_port) == tcp_port) {
                uint32_t bound_ip = sin.sin_addr.s_addr;
                if (strcmp(listen_ip, "0.0.0.0") == 0 ||
                    ntohl(bound_ip) == INADDR_ANY ||
                    inet_addr(listen_ip) == bound_ip) {
                    dup = 1; break;
                }
            }
        }
        if (!dup && t->tcp_count < SIP_TR_FD_MAX) {
            int fd = net_tcp_listen(listen_ip, tcp_port, 4096, t->cfg.reuseport);
            if (fd < 0) {
                llog(LOG_WARN, "sip", "add tcp listen %s:%u failed",
                     listen_ip, tcp_port);
                return NSBC_ERR;
            }
            t->tcp_lfds[t->tcp_count++] = fd;
            reactor_add(t->cfg.re, fd, EPOLLIN, on_tcp_accept, t);
            llog(LOG_NOTICE, "sip", "added tcp listener %s:%u (total=%d)",
                 listen_ip, tcp_port, t->tcp_count);
            added++;
        }
    }

    /* ---- TLS：按 (ip, port) 去重（复用 on_tcp_accept handler，
     * handler 内部遍历 tls_lfds[] 判断 is_tls） ---- */
    if (tls_port > 0) {
        int dup = 0;
        for (int i = 0; i < t->tls_count; ++i) {
            struct sockaddr_in sin; socklen_t sl = sizeof(sin);
            if (getsockname(t->tls_lfds[i], (struct sockaddr *)&sin, &sl) == 0 &&
                ntohs(sin.sin_port) == tls_port) {
                uint32_t bound_ip = sin.sin_addr.s_addr;
                if (strcmp(listen_ip, "0.0.0.0") == 0 ||
                    ntohl(bound_ip) == INADDR_ANY ||
                    inet_addr(listen_ip) == bound_ip) {
                    dup = 1; break;
                }
            }
        }
        if (!dup && t->tls_count < SIP_TR_FD_MAX) {
            int fd = net_tcp_listen(listen_ip, tls_port, 4096, t->cfg.reuseport);
            if (fd < 0) {
                llog(LOG_WARN, "sip", "add tls listen %s:%u failed",
                     listen_ip, tls_port);
                return NSBC_ERR;
            }
            t->tls_lfds[t->tls_count++] = fd;
            reactor_add(t->cfg.re, fd, EPOLLIN, on_tcp_accept, t);
            llog(LOG_NOTICE, "sip", "added tls listener %s:%u (total=%d)",
                 listen_ip, tls_port, t->tls_count);
            added++;
        }
    }

    llog(LOG_INFO, "sip", "add_listeners done: added=%d udp=%d tcp=%d tls=%d",
         added, t->udp_fd_count, t->tcp_count, t->tls_count);
    return NSBC_OK;
}

/* 从所有 enabled+OK profile 收集不同的 (sip_ip, UDP/TCP port) 组合，
 * 按需追加到 transport 监听 fd 列表（内部 (ip,port) 去重）。
 * profiles.reload 成功后调用。
 *
 * ⚠️ 关键：每个 profile 有独立的 sip_ip 字段（DB 里存的绑定 IP），
 * 必须用 profile->sip_ip 来开 socket，不能统一用 t->cfg.listen_ip——
 * 那样 external profile 绑到 "10.0.0.163:5080" 会被错误地绑到 "0.0.0.0:5080"。 */
int sip_tr_sync_from_profiles(sip_transport_t *t)
{
    if (!t) return NSBC_INVAL;

    unsigned np = sip_profile_count();
    if (np == 0) {
        llog(LOG_INFO, "sip", "sync_from_profiles: no profiles loaded");
        return NSBC_OK;
    }

    /* 收集所有不同的 (sip_ip, port) 组合 —— 三种协议分开 */
    struct { char ip[64]; uint16_t port; } udp_combo[SIP_TR_FD_MAX * 2];
    struct { char ip[64]; uint16_t port; } tcp_combo[SIP_TR_FD_MAX * 2];
    struct { char ip[64]; uint16_t port; } tls_combo[SIP_TR_FD_MAX * 2];
    int udp_n = 0, tcp_n = 0, tls_n = 0;

    for (unsigned i = 0; i < np; ++i) {
        sip_profile_t *p = sip_profile_at(i);
        if (!p || !p->enabled || p->state != SIP_PROFILE_OK)
            continue;

        const char *lip = (p->sip_ip[0] && p->sip_ip[0] != '\0')
                          ? p->sip_ip : "0.0.0.0";

        if (p->enable_udp && p->sip_udp_port > 0) {
            int dup = 0;
            for (int k = 0; k < udp_n; ++k)
                if (udp_combo[k].port == p->sip_udp_port &&
                    strcmp(udp_combo[k].ip, lip) == 0) { dup = 1; break; }
            if (!dup && udp_n < SIP_TR_FD_MAX * 2) {
                snprintf(udp_combo[udp_n].ip, sizeof(udp_combo[udp_n].ip),
                         "%s", lip);
                udp_combo[udp_n].port = p->sip_udp_port;
                udp_n++;
            }
        }
        if (p->enable_tcp && p->sip_tcp_port > 0) {
            int dup = 0;
            for (int k = 0; k < tcp_n; ++k)
                if (tcp_combo[k].port == p->sip_tcp_port &&
                    strcmp(tcp_combo[k].ip, lip) == 0) { dup = 1; break; }
            if (!dup && tcp_n < SIP_TR_FD_MAX * 2) {
                snprintf(tcp_combo[tcp_n].ip, sizeof(tcp_combo[tcp_n].ip),
                         "%s", lip);
                tcp_combo[tcp_n].port = p->sip_tcp_port;
                tcp_n++;
            }
        }
        if (p->enable_tls && p->sip_tls_port > 0) {
            int dup = 0;
            for (int k = 0; k < tls_n; ++k)
                if (tls_combo[k].port == p->sip_tls_port &&
                    strcmp(tls_combo[k].ip, lip) == 0) { dup = 1; break; }
            if (!dup && tls_n < SIP_TR_FD_MAX * 2) {
                snprintf(tls_combo[tls_n].ip, sizeof(tls_combo[tls_n].ip),
                         "%s", lip);
                tls_combo[tls_n].port = p->sip_tls_port;
                tls_n++;
            }
        }
    }

    llog(LOG_INFO, "sip", "sync_from_profiles: target %d udp %d tcp %d tls unique (ip,port) combos",
         udp_n, tcp_n, tls_n);

    /* ---- 差集：关闭不在目标集合里的旧 fd ---- */
    int closed = 0;

    /* UDP 差集 */
    for (int i = t->udp_fd_count - 1; i >= 0; --i) {
        int fd = t->udp_fds[i];
        if (fd < 0) continue;
        struct sockaddr_in sin; socklen_t sl = sizeof(sin);
        if (getsockname(fd, (struct sockaddr *)&sin, &sl) != 0) continue;
        uint16_t cur_port = ntohs(sin.sin_port);
        int keep = 0;
        for (int k = 0; k < udp_n; ++k)
            if (udp_combo[k].port == cur_port) { keep = 1; break; }
        if (!keep) {
            llog(LOG_NOTICE, "sip", "sync_from_profiles: close stale udp fd=%d port=%u",
                 fd, cur_port);
            reactor_del(t->cfg.re, fd);
            close(fd);
            t->udp_fds[i] = -1;
            for (int j = i; j < t->udp_fd_count - 1; ++j)
                t->udp_fds[j] = t->udp_fds[j + 1];
            t->udp_fds[t->udp_fd_count - 1] = -1;
            t->udp_fd_count--;
            closed++;
        }
    }

    /* TCP 差集 */
    for (int i = t->tcp_count - 1; i >= 0; --i) {
        int fd = t->tcp_lfds[i];
        if (fd < 0) continue;
        struct sockaddr_in sin; socklen_t sl = sizeof(sin);
        if (getsockname(fd, (struct sockaddr *)&sin, &sl) != 0) continue;
        uint16_t cur_port = ntohs(sin.sin_port);
        int keep = 0;
        for (int k = 0; k < tcp_n; ++k)
            if (tcp_combo[k].port == cur_port) { keep = 1; break; }
        if (!keep) {
            llog(LOG_NOTICE, "sip", "sync_from_profiles: close stale tcp fd=%d port=%u",
                 fd, cur_port);
            reactor_del(t->cfg.re, fd);
            close(fd);
            t->tcp_lfds[i] = -1;
            for (int j = i; j < t->tcp_count - 1; ++j)
                t->tcp_lfds[j] = t->tcp_lfds[j + 1];
            t->tcp_lfds[t->tcp_count - 1] = -1;
            t->tcp_count--;
            closed++;
        }
    }

    /* TLS 差集 */
    for (int i = t->tls_count - 1; i >= 0; --i) {
        int fd = t->tls_lfds[i];
        if (fd < 0) continue;
        struct sockaddr_in sin; socklen_t sl = sizeof(sin);
        if (getsockname(fd, (struct sockaddr *)&sin, &sl) != 0) continue;
        uint16_t cur_port = ntohs(sin.sin_port);
        int keep = 0;
        for (int k = 0; k < tls_n; ++k)
            if (tls_combo[k].port == cur_port) { keep = 1; break; }
        if (!keep) {
            llog(LOG_NOTICE, "sip", "sync_from_profiles: close stale tls fd=%d port=%u",
                 fd, cur_port);
            reactor_del(t->cfg.re, fd);
            close(fd);
            t->tls_lfds[i] = -1;
            for (int j = i; j < t->tls_count - 1; ++j)
                t->tls_lfds[j] = t->tls_lfds[j + 1];
            t->tls_lfds[t->tls_count - 1] = -1;
            t->tls_count--;
            closed++;
        }
    }

    if (closed)
        llog(LOG_INFO, "sip", "sync_from_profiles: closed %d stale listeners", closed);

    /* ---- 追加目标里新增的 fd ---- */
    int rc = NSBC_OK;
    for (int i = 0; i < udp_n; ++i) {
        int sub = sip_tr_add_listeners(t, udp_combo[i].ip, udp_combo[i].port, 0, 0);
        if (sub != NSBC_OK) rc = sub;
    }
    for (int i = 0; i < tcp_n; ++i) {
        int sub = sip_tr_add_listeners(t, tcp_combo[i].ip, 0, tcp_combo[i].port, 0);
        if (sub != NSBC_OK) rc = sub;
    }
    for (int i = 0; i < tls_n; ++i) {
        int sub = sip_tr_add_listeners(t, tls_combo[i].ip, 0, 0, tls_combo[i].port);
        if (sub != NSBC_OK) rc = sub;
    }
    return rc;
}

/* ================================================================== */
/* UAC 出站请求（P1）                                                  */
/* ================================================================== */

/* 生成本地 From tag："nsbc-<callid_[:8]>-<seq>" */
void gen_from_tag(const char *call_id, long cseq, char *out, size_t outsz)
{
    if (call_id == NULL || call_id[0] == '\0') {
        snprintf(out, outsz, "nsbc-%ld", cseq);
        return;
    }
    /* 找 call_id @ 前半段（SIP Call-ID 通常 "<uuid>@host"） */
    const char *at = strchr(call_id, '@');
    size_t prefix = at ? (size_t)(at - call_id) : strlen(call_id);
    if (prefix > 8) prefix = 8;
    snprintf(out, outsz, "nsbc-%.*s-%ld", (int)prefix, call_id, cseq);
}

/* 生成本地 Via branch："z9hG4bK<callid[:8]><seq>m<monotonic>" */
void gen_branch(const char *call_id, long cseq, char *out, size_t outsz)
{
    static uint32_t g_branch_counter = 0;
    uint32_t n = __atomic_add_fetch(&g_branch_counter, 1, __ATOMIC_RELAXED);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t monotonic = (uint32_t)(ts.tv_sec ^ ts.tv_nsec);
    if (call_id && call_id[0]) {
        const char *at = strchr(call_id, '@');
        size_t prefix = at ? (size_t)(at - call_id) : strlen(call_id);
        if (prefix > 8) prefix = 8;
        snprintf(out, outsz, "z9hG4bK%.*s%ldm%ux%u",
                 (int)prefix, call_id, cseq, monotonic, n);
    } else {
        snprintf(out, outsz, "z9hG4bK%ldm%ux%u", cseq, monotonic, n);
    }
}

/* 构造 Via 头值（不含头名，sip_build_request 会自己加 Via:）。
 * 这里我们只传 via_host/via_port，branch 在上层 gen_branch 后填入 */
void build_from_line(const char *sip_uri, const char *tag,
                     char *out, size_t outsz)
{
    if (tag && tag[0])
        snprintf(out, outsz, "<%s>;tag=%s", sip_uri, tag);
    else
        snprintf(out, outsz, "<%s>", sip_uri);
}

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
                           const char *content_type)
{
    /* 参数校验 */
    if (t == NULL || method == NULL || method[0] == '\0' ||
        uri == NULL || uri[0] == '\0' ||
        from == NULL || from[0] == '\0' ||
        to == NULL || to[0] == '\0' ||
        call_id == NULL || call_id[0] == '\0' ||
        via_host == NULL || via_host[0] == '\0' ||
        dst_host == NULL || dst_host[0] == '\0')
      if (!t) return NSBC_INVAL;
    if (t->udp_fd_count <= 0) return NSBC_NOTSUP; /* TCP/TLS 主动连接 P1 */

    /* 本地缓冲（栈上 16KB — 绝大多数 SIP 请求够） */
    char *buf = (char *)alloca(SIP_UAC_BUF_CAP);
    if (buf == NULL) return NSBC_NOMEM;

    /* 1. 本地 From tag */
    char tag[64] = {0};
    if (from_tag && from_tag[0]) {
        snprintf(tag, sizeof(tag), "%s", from_tag);
    } else {
        gen_from_tag(call_id, cseq, tag, sizeof(tag));
    }

    /* 2. From 完整行 "<sip:user@host>;tag=..." 供 sip_req_spec.from 使用 */
    char from_line[256] = {0};
    build_from_line(from, tag, from_line, sizeof(from_line));

    /* 3. Via branch */
    char branch[64] = {0};
    gen_branch(call_id, cseq, branch, sizeof(branch));

    /* 4. Via 整行（sip_build_request 会生成 "Via: SIP/2.0/UDP host:port;branch=...;rport"）
     *    —— sip_req_spec_t 里只需要 via_host/via_port/transport/branch */

    /* 5. 拼接 extra / auth_hdr / expires（这些都是额外头块，带 CRLF） */
    /*    sip_req_spec.extra 的语义是"已格式化好的附加头块（每个头自带 CRLF）"
     *    我们把 auth_hdr + expires + 用户 extra 串起来 */
    char extra_block[4096] = {0};
    size_t eo = 0;
    if (auth_hdr && auth_hdr[0]) {
        int w = snprintf(extra_block + eo, sizeof(extra_block) - eo,
                         "Authorization: %s\r\n", auth_hdr);
        if (w > 0) eo += (size_t)w;
    }
    if (expires > 0) {
        int w = snprintf(extra_block + eo, sizeof(extra_block) - eo,
                         "Expires: %ld\r\n", expires);
        if (w > 0) eo += (size_t)w;
    }
    if (extra && extra[0]) {
        size_t el = strlen(extra);
        if (eo + el < sizeof(extra_block)) {
            memcpy(extra_block + eo, extra, el);
            eo += el;
            /* 确保 extra 末尾有 CRLF */
            if (eo >= 2 && !(extra_block[eo - 2] == '\r' && extra_block[eo - 1] == '\n')) {
                if (eo + 2 < sizeof(extra_block)) {
                    extra_block[eo++] = '\r';
                    extra_block[eo++] = '\n';
                }
            }
        }
    }

    /* 6. 构造 sip_req_spec */
    sip_req_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.method       = method;
    spec.ruri         = uri;
    spec.from         = from_line;           /* 已带 <sip:...>;tag=... 格式 */
    spec.to           = to;                  /* 直接 "sip:user@host" 或 "<sip:...>" */
    spec.contact      = contact;
    spec.callid       = call_id;
    spec.cseq         = cseq;
    spec.via_host     = via_host;
    spec.via_port     = via_port;
    spec.transport    = "UDP";
    spec.branch       = branch;
    spec.max_forwards = 70;
    spec.extra        = eo > 0 ? extra_block : NULL;
    spec.body         = body;
    spec.bodylen      = bodylen;
    spec.content_type = content_type;

    /* 7. 构建报文（sip_build_request 成功返回正=报文长度，失败返回负 NSBC_*） */
    int rc = sip_build_request(buf, SIP_UAC_BUF_CAP, &spec);
    if (rc < 0) {
        log_warn("tr", "send_uac build failed rc=%d method=%s uri=%s", rc, method, uri);
        return rc;
    }

    /* 8. 目标地址（host:port → nsbc_sa_t，UDP 走 IPv4 优先） */
    nsbc_sa_t dst;
    nsbc_sa_clear(&dst);
    rc = nsbc_sa_from(&dst, dst_host, dst_port, 0); /* family=0 自动选 */
    if (rc != NSBC_OK) {
        log_warn("tr", "send_uac dst resolve failed host=%s port=%u", dst_host, dst_port);
        return rc;
    }

    /* 9. 发！（UDP；TCP/TLS 为 P1 NOTSUP） */
    size_t built = strlen(buf); /* sip_build_request 构建的 C 字符串（含 \r\n 结尾） */
    rc = sip_tr_send(t, SIP_TR_UDP, buf, built, &dst);
    if (rc != NSBC_OK) {
        log_warn("tr", "send_uac failed rc=%d method=%s uri=%s", rc, method, uri);
    } else {
        log_info("tr", "send_uac OK %s %s → %s:%u len=%zu",
                 method, uri, dst_host, dst_port, built);
    }
    return rc;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
