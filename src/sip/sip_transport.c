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
#include "nsbc_atomic.h"  /* sat_add / sat_sub — tr_conn_t refcount */
#include "obs_sub.h"       /* obs_sub_publish_log — siptrace 同时推送给 UDS 订阅者 */
#include "sip_profile.h"   /* sip_profile_at / sip_profile_count */
#include "ws_listener.h"
#include "security.h"     /* ban_is_banned / security_check_ip — reactor 层前置黑名单检查 */

/* reactor 线程内本地接收端口传递（b2bua.c 写入，sip_tr_send 读取，
 * 同一 epoll 线程内同步执行）。定义在此文件：sip_transport 是核心公共模块，
 * 保证其他模块（b2bua.c）可以 extern 引用。 */
__thread uint16_t g_rx_local_port = 0;

/* ------------------------------------------------------------------ */
/* SIP trace 全局开关（类似 FreeSWITCH sofia global siptrace）
 *
 * volatile int — 读端只做一次缓存行读取，接近零开销；
 * 写端由 UDS 命令 "sofia global siptrace on/off" 触发。 */
static volatile int g_sip_trace = 0;

void sip_tr_set_trace(int on) { g_sip_trace = on ? 1 : 0; }
int  sip_tr_get_trace(void)   { return g_sip_trace; }

/* 前向声明 — sip_tr_rx_trace 在下面调用 sip_trace_dump */
static void sip_trace_dump(const char *tag, int dir,
                           const char *peer_ip, uint16_t peer_port,
                           const char *buf, size_t len);

/* WS/WSS RX trace hook — signaling.c:sig_ws_bridge_rx 调
 * proto = SIP_TR_WS 或 SIP_TR_WSS */
void sip_tr_rx_trace(int proto, const char *peer_ip, uint16_t peer_port,
                     const char *buf, size_t len)
{
    if (!g_sip_trace) return;
    const char *ptag = (proto == SIP_TR_WSS) ? "WSS" : "WS";
    sip_trace_dump(ptag, 0, peer_ip, peer_port, buf, len);
}

/* SIP trace — 原始文本 dump，双通道输出：
 *   1) llog(LOG_DEBUG "siptrace") → 日志文件（tail -f 看）
 *   2) obs_sub_publish_log         → UDS 订阅推送（client 发 "sub log" 即可实时收）
 * 长包截断到 4KB。关闭 g_sip_trace 后零开销。 */
static void sip_trace_dump(const char *tag, int dir,
                           const char *peer_ip, uint16_t peer_port,
                           const char *buf, size_t len)
{
    if (!g_sip_trace) return;
    const size_t MAX_SHOW = 4096;
    const size_t show = len > MAX_SHOW ? MAX_SHOW : len;
    const char *d = dir > 0 ? "TX->" : "<-RX";

    /* 双通道，互不影响：
     *   1) llog(LOG_DEBUG "siptrace") → 日志文件（默认 INFO 级别不显示）
     *   2) obs_sub_publish_trace       → 独立 topic "trace"
     *      （客户端需显式 sub trace，不污染 log topic） */
    llog(LOG_DEBUG, "siptrace",
         "%s %s %s:%u (%zuB%s)\n%.*s",
         d, tag, peer_ip ? peer_ip : "-", (unsigned)peer_port,
         len, len > show ? " truncated" : "",
         (int)show, buf);
    obs_sub_publish_trace(
         "siptrace %s %s %s:%u (%zuB%s)\n%.*s\n",
         d, tag, peer_ip ? peer_ip : "-", (unsigned)peer_port,
         len, len > show ? " truncated" : "",
         (int)show, buf);
}

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
    int dead;                  /* 置 1 = conn_close 已执行独占清理，禁止新路径访问 */
    nsbc_atomic_t refcount;    /* 引用计数；free(c) 延迟到 refcount==0 */
    int is_tls;
    tls_conn_t *tls;
    int tls_ok;
    nsbc_sa_t peer;
    reactor_t *home_reactor;  /* P3: 路由归属的 reactor —— accept 时选定，所有 FD 操作必须经此 reactor */
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
    /* P3: per-reactor UDP 接收缓冲——原来 t->rx/t->dg 是 per-transport 共享字段，
     * 多 reactor UDP SO_REUSEPORT 后 N 个 reactor 线程同时跑 on_udp_event 并发写 → data race。
     * 修复：按 reactor 数量分配 N 组 rx + dg 数组；on_udp_event 里通过 fd 反查 reactor index
     * 找到对应的组使用。单 reactor（cfg->mgr==NULL）时 n_rx=1，退化成旧行为。 */
    int n_rx;  /* 数组维度（reactor 数量） */
    net_rx_t *per_reactor_rx;                 /* [n_rx] */
    net_dgram_t (*per_reactor_dg)[NET_BATCH]; /* [n_rx][NET_BATCH] */
    tr_conn_t **conns;
    int conns_cap;
    int conns_n;
    pthread_mutex_t conn_lock; /* P3: 保护 conns[]/conns_n/conns_cap —— conn_accept( reactor[0] )
                                * 与 conn_close（c->home_reactor 可能 != reactor[0]）在多 reactor 下并发访问 */
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

/* ------------------------------------------------------------------ */
/* conn 生命周期管理 —— dead 标记 + refcount 延迟释放                    */
/*                                                                      */
/* conn_close() 在 home_reactor 线程被调用（EPOLLHUP / recv=0 /         */
/* feed_framer FRAME_OVERFLOW/PROTO），它执行独占清理：                  */
/*   dead=1 → conns[]=NULL → reactor_del → 摘 idx → close(fd)          */
/* 然后把 refcount 从 1 减到 0（若无并发持有者则立即 free）。           */
/*                                                                      */
/* 跨 reactor 的使用者（如 sip_tr_send TCP 路径）必须：                 */
/*   idx.lock 内 sat_add(c->refcount, 1) → 解锁 → 用完 → conn_release */
/* 确保 conn_close 的独占清理不会 free 还在被用的指针。                 */
/* ------------------------------------------------------------------ */

/** 释放一个 conn 引用 —— refcount--，归零时 free 所有资源 */
static void conn_release(tr_conn_t *c)
{
    if (!c) return;
    if (sat_sub(c->refcount, 1) == 0) {
        free(c->sbuf);
        pthread_mutex_destroy(&c->sbuf_lock);
        free(c);
    }
}

static void conn_close(sip_transport_t *t, int fd)
{
    /* P3: conn_lock 保护 conns[]/conns_n —— conn_accept(reactor[0]) 与
     * conn_close(c->home_reactor 可能 != reactor[0]) 多 reactor 并发访问。
     * 立即把 conns[i] = NULL —— 防止并发 on_conn_event 通过 conn_idx 再看到 */
    pthread_mutex_lock(&t->conn_lock);
    int i = conn_idx(t, fd);
    if (i < 0) { pthread_mutex_unlock(&t->conn_lock); return; }
    tr_conn_t *c = t->conns[i];
    t->conns[i] = NULL;
    --t->conns_n;
    pthread_mutex_unlock(&t->conn_lock);

    /* dead=1 先置 —— 任何跨 reactor 路径（sip_tr_send 查 idx 时）能看到，
     * 不再把这个 conn 当作有效目标 */
    c->dead = 1;

    /* reactor_del 必须在 conn_lock 外——reactor 回调里可能再调 conn_close → 死锁 */
    reactor_del(c->home_reactor, fd);
    /* ---- 从 (proto, peer) 哈希表摘除 —— 阻止后续 sip_tr_send 查到 ---- */
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
    /* 释放初始 refcount=1；如无人并发持有 → 立即 free */
    conn_release(c);
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
    (void)t;  /* 回调签名要求，实现只操作 conn */
    /* dead=1 表示 conn_close 已执行独占清理（close fd / 清 sbuf 等），
     * 跨 reactor 路径持有的 conn 引用可能已经被清理，必须拒绝入队 */
    if (!c || c->dead || !data || len == 0) return -1;

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
        reactor_mod(c->home_reactor, c->fd, want);
    }

    pthread_mutex_unlock(&c->sbuf_lock);
    return 0;
}

/** 消费者 flush —— reactor EPOLLOUT 触发时调用
 *  @return 0=全部发完 / EAGAIN 留待下次，-1=致命错误（对端断开）*/
static int sip_tcp_flush(sip_transport_t *t, tr_conn_t *c)
{
    (void)t;  /* 回调签名要求，实现只操作 conn */
    for (;;) {
        pthread_mutex_lock(&c->sbuf_lock);
        size_t used = c->sbuf_tail - c->sbuf_head;
        if (used == 0) {
            /* 队列空 → 关掉 EPOLLOUT，保留 EPOLLIN */
            c->sbuf_want_epollout = 0;
            pthread_mutex_unlock(&c->sbuf_lock);
            reactor_mod(c->home_reactor, c->fd,
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
                reactor_mod(c->home_reactor, c->fd, want_ev | EPOLLRDHUP);
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
    /* siptrace RX — TCP/TLS（与 UDP on_udp_event 对称） */
    if (g_sip_trace) {
        char dip[INET6_ADDRSTRLEN] = {0};
        uint16_t dport = nsbc_sa_port(&s_cur_conn->peer);
        nsbc_sa_fmt(&s_cur_conn->peer, dip, sizeof(dip));
        sip_trace_dump(s_cur_conn->is_tls ? "TLS" : "TCP", 0, dip, dport,
                       (const char *)msg, len);
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
                reactor_mod(c->home_reactor, fd, EPOLLIN | EPOLLRDHUP);
                break;
            }
            if (hs == TLS_HS_WANT_READ) {
                reactor_mod(c->home_reactor, fd, EPOLLIN | EPOLLRDHUP);
                return;
            }
            if (hs == TLS_HS_WANT_WRITE) {
                reactor_mod(c->home_reactor, fd,
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
                reactor_mod(c->home_reactor, fd,
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

/* F-04 修复：TCP accept 在 reactor[0] 线程，但新连接的 home_reactor
 * 可能是 [1..N]。reactor_add 跨 owner 线程不安全（并发读写 slot 表）。
 * 改为 reactor_call 在 owner 线程执行 reactor_add。 */
typedef struct {
    reactor_t *r;
    int fd;
    uint32_t events;
    reactor_cb_fn cb;
    void *ud;
    int result;
} tcp_accept_add_args_t;

static int tcp_accept_add_fn(void *arg)
{
    tcp_accept_add_args_t *a = (tcp_accept_add_args_t *)arg;
    a->result = reactor_add(a->r, a->fd, a->events, a->cb, a->ud);
    return NSBC_OK;  /* task 本身成功，结果在 a->result 中 */
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
    /* P3: conn_lock 保护 conns_cap realloc（与 conn_close 并发） */
    pthread_mutex_lock(&t->conn_lock);
    if (fd >= t->conns_cap) {
        int cap = t->conns_cap ? t->conns_cap * 2 : 256;
        while (cap <= fd)
            cap *= 2;
        tr_conn_t **nv = realloc(t->conns, cap * sizeof(*nv));
        if (!nv) {
            pthread_mutex_unlock(&t->conn_lock);
            close(fd);
            return NULL;
        }
        memset(nv + t->conns_cap, 0,
               (cap - t->conns_cap) * sizeof(*nv));
        t->conns = nv;
        t->conns_cap = cap;
    }
    pthread_mutex_unlock(&t->conn_lock);
    size_t fcap = t->cfg.max_msg * 2 + 16;
    tr_conn_t *c = calloc(1, sizeof(*c) + fcap - 1);
    if (!c) {
        close(fd);
        return NULL;
    }
    c->fd = fd;
    c->in_use = 1;
    c->dead = 0;
    c->refcount = 1;          /* idx 表 + reactor 回调各持一份 — 接受即 1 */
    c->is_tls = is_tls;
    c->peer = peer;
    /* P3: TCP/TLS 连接按 (proto, peer) hash 路由到 reactor。
     * 保证同一 peer 的连接落到同一 reactor（减少跨 reactor 同步）。
     * cfg->mgr == NULL 时退回 reactor[0]（t->cfg.re）。 */
    if (t->cfg.mgr) {
        uint64_t h = conn_key_hash(is_tls, &peer);
        unsigned rid = reactor_mgr_route(t->cfg.mgr, h);
        c->home_reactor = reactor_mgr_get(t->cfg.mgr, rid);
    }
    if (!c->home_reactor)
        c->home_reactor = t->cfg.re;
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
    /* P3: conn_lock 保护 conns[] 写入（与 conn_close 并发） */
    pthread_mutex_lock(&t->conn_lock);
    t->conns[fd] = c;
    ++t->conns_n;
    pthread_mutex_unlock(&t->conn_lock);
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
                pthread_mutex_lock(&t->conn_lock);
                t->conns[fd] = NULL;
                --t->conns_n;
                pthread_mutex_unlock(&t->conn_lock);
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
    /* F-04 修复：通过 reactor_call 在 home_reactor owner 线程执行 reactor_add。
     * 跨线程直接调 reactor_add 会并发写 owner-only 的 callback slot 表。 */
    {
        tcp_accept_add_args_t aa;
        aa.r = c->home_reactor;
        aa.fd = fd;
        aa.events = is_tls ? (EPOLLIN | EPOLLOUT | EPOLLRDHUP)
                           : (EPOLLIN | EPOLLRDHUP);
        aa.cb = on_conn_event;
        aa.ud = t;
        aa.result = NSBC_OK;
        int rc = reactor_call(c->home_reactor, tcp_accept_add_fn, &aa);
        if (rc != NSBC_OK || aa.result != NSBC_OK) {
            llog(LOG_ERR, "sip",
                 "conn_accept: reactor_add failed rc=%d add_rc=%d fd=%d",
                 rc, aa.result, fd);
            /* 回滚：从 conns[] 和哈希表摘除，关闭 fd，释放 conn */
            if (t->idx.slots) {
                pthread_mutex_lock(&t->idx.lock);
                uint64_t h = conn_key_hash(is_tls, &peer);
                size_t b = h & (CONN_IDX_CAP - 1);
                tr_conn_t **pp = &t->idx.slots[b];
                while (*pp && *pp != c) pp = &(*pp)->idx_next;
                if (*pp) { *pp = c->idx_next; --t->idx.n; }
                pthread_mutex_unlock(&t->idx.lock);
            }
#ifdef NSBC_WITH_OPENSSL
            if (c->tls) tls_conn_free(c->tls);
#endif
            free(c->sbuf); pthread_mutex_destroy(&c->sbuf_lock);
            pthread_mutex_lock(&t->conn_lock);
            t->conns[fd] = NULL;
            --t->conns_n;
            pthread_mutex_unlock(&t->conn_lock);
            free(c);
            close(fd);
            return NULL;
        }
    }
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
    /* P3: 通过 fd 反查 reactor index —— udp_fds[i] 绑定 reactor[i]（UDP 创建循环 i 次）。
     * 找到后用对应的 per-reactor rx/dg 组（修复多 reactor 并发写 t->rx/t->dg 的 data race）。 */
    int rx_idx = -1;
    for (int i = 0; i < t->udp_fd_count; ++i) {
        if (t->udp_fds[i] == fd) { rx_idx = i; break; }
    }
    if (rx_idx < 0) {
        /* 真 ghost：udp_fds[] 完全找不到这个 fd
         * （不是 add_listeners 的问题，是 close race / fd 复用）
         *
         * 修复：降级 LOG_DEBUG + reactor_del 摘掉僵尸 fd
         * （注意：跨 reactor callback 里 reactor_del 会触发 reactor_mgr 遍历，
         *   在 reactor callback 上下文里调 reactor_del 对 owner reactor 安全） */
        static __thread uint64_t ghost_count = 0;
        ghost_count++;
        if (ghost_count % 500 == 1) {
            llog(LOG_DEBUG, "sip", "on_udp_event: ghost fd=%d (no udp_fds match) "
                 "reactor_del, total_ghost=%lu", fd, ghost_count);
        }
        if (t->cfg.mgr) {
            unsigned n = reactor_mgr_count(t->cfg.mgr);
            for (unsigned k = 0; k < n; ++k) {
                reactor_t *rk = reactor_mgr_get(t->cfg.mgr, k);
                if (rk) reactor_del(rk, fd);
            }
        } else if (t->cfg.re) {
            reactor_del(t->cfg.re, fd);
        }
        /* 注意：不 close(fd) —— 无法确认是不是合法 listener。reactor_del
         * 已经阻止 reactor 继续触发 EPOLLIN，不会再空转。close 可能杀掉
         * 合法 UDP listener（虽然 udp_fds[] 找不到） */
        return;
    }

    /* REQ-FIX: sip_tr_add_listeners 加 UDP fd 时只增 udp_fd_count 不增 n_rx
     * 这些 fd 全部加到 reactor[0] 上（L1270: reactor_add(t->cfg.re, fd, ...)），
     * 所以 udp_fds[rx_idx] 有效但 rx_idx >= n_rx（n_rx=reactor 数量）。
     *
     * 修复：对这些动态添加的 listener，用 rx_idx % n_rx 复用已有的 per_reactor_rx/dg 槽位
     * （因为 add_listeners 的 fd 都加在 reactor[0] 上，rx_idx % n_rx 也会落到 reactor[0] 槽位）
     *
     * 这比扩展 per_reactor 数组安全——不会 realloc 已在 reactor 回调里使用的数组。 */
    int effective_rx_idx = (rx_idx >= t->n_rx) ? (rx_idx % t->n_rx) : rx_idx;
    net_rx_t *rx = &t->per_reactor_rx[effective_rx_idx];
    net_dgram_t *dg = t->per_reactor_dg[effective_rx_idx];
    /* 先用 fd 拿监听端口，后面填到每个 dgram 的 local.port 上 */
    uint16_t bind_port = 0;
    {
        struct sockaddr_in sin; socklen_t sl = sizeof(sin);
        if (getsockname(fd, (struct sockaddr *)&sin, &sl) == 0)
            bind_port = ntohs(sin.sin_port);
    }
    for (;;) {
        net_rx_prepare(rx, dg);
        int n = net_rx_recv(fd, rx, dg);
        if (n <= 0)
            break;
        for (int i = 0; i < n; ++i) {
            net_dgram_t *d = &dg[i];
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
                if (g_sip_trace) {
                    char dip[INET6_ADDRSTRLEN] = {0};
                    uint16_t dport = nsbc_sa_port(&d->peer);
                    nsbc_sa_fmt(&d->peer, dip, sizeof(dip));
                    sip_trace_dump("UDP", 0, dip, dport,
                                   (const char *)d->buf, d->len);
                }
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
            /* SIP trace RX（开关 g_sip_trace=1 时才打；blocked 路径上面已打） */
            if (g_sip_trace) {
                char dip[INET6_ADDRSTRLEN] = {0};
                uint16_t dport = nsbc_sa_port(&d->peer);
                nsbc_sa_fmt(&d->peer, dip, sizeof(dip));
                sip_trace_dump("UDP", 0, dip, dport,
                               (const char *)d->buf, d->len);
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
    pthread_mutex_init(&t->conn_lock, NULL);
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

    /* P3: per-reactor UDP 接收缓冲分配——原来单组 t->rx/t->dg，多 reactor 并发写 race。
     * 按 reactor 数量分配 N 组 rx + 二维 dg 数组，每组独立 NET_BATCH 个 net_dgram_t。 */
    t->n_rx = cfg->mgr ? reactor_mgr_count(cfg->mgr) : 1;
    if (t->n_rx == 0) t->n_rx = 1;
    if (t->n_rx > SIP_TR_FD_MAX) {
        llog(LOG_WARN, "sip", "n_rx=%u exceeds SIP_TR_FD_MAX=%d, capping",
             t->n_rx, SIP_TR_FD_MAX);
        t->n_rx = SIP_TR_FD_MAX;
    }
    t->per_reactor_rx = calloc((size_t)t->n_rx, sizeof(net_rx_t));
    t->per_reactor_dg = calloc((size_t)t->n_rx, sizeof(net_dgram_t[NET_BATCH]));
    if (!t->per_reactor_rx || !t->per_reactor_dg) {
        sip_tr_destroy(t);
        return NSBC_NOMEM;
    }
    /* 初始化每组 dg[i].buf —— reactor 内部各线程独立使用自己的组 */
    for (int r = 0; r < t->n_rx; ++r) {
        for (int i = 0; i < NET_BATCH; ++i) {
            t->per_reactor_dg[r][i].cap = cfg->max_msg;
            t->per_reactor_dg[r][i].buf = malloc(cfg->max_msg);
            if (!t->per_reactor_dg[r][i].buf) {
                sip_tr_destroy(t);
                return NSBC_NOMEM;
            }
        }
    }
    llog(LOG_INFO, "sip", "per-reactor rx/dg allocated n_rx=%d", t->n_rx);

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
        /* P3: 多 reactor 时 SO_REUSEPORT 必须强制开启（否则同端口绑多 socket 失败） */
        unsigned n_udp = 1;
        if (cfg->mgr) {
            n_udp = reactor_mgr_count(cfg->mgr);
            if (!cfg->reuseport) {
                llog(LOG_WARN, "sip",
                     "multi-reactor requires SO_REUSEPORT, forcing on");
                uc.reuseport = 1;
            }
            if (n_udp > (unsigned)SIP_TR_FD_MAX) {
                llog(LOG_WARN, "sip", "reactor count %u exceeds SIP_TR_FD_MAX=%d, capping",
                     n_udp, SIP_TR_FD_MAX);
                n_udp = SIP_TR_FD_MAX;
            }
        }

        for (unsigned i = 0; i < n_udp; ++i) {
            int fd = net_udp_open(cfg->listen_ip, cfg->udp_port, &uc);
            if (fd < 0) {
                llog(LOG_CRIT, "sip", "udp listen %s:%u [reactor %u] failed",
                     cfg->listen_ip, cfg->udp_port, i);
                /* rollback：关闭已创建的 UDP fds */
                for (int j = 0; j < t->udp_fd_count; ++j)
                    if (t->udp_fds[j] >= 0) close(t->udp_fds[j]);
                sip_tr_destroy(t);
                return NSBC_ERR;
            }
            t->udp_fds[t->udp_fd_count++] = fd;
            reactor_t *r = cfg->mgr ? reactor_mgr_get(cfg->mgr, i) : cfg->re;
            reactor_add(r, fd, EPOLLIN, on_udp_event, t);
            llog(LOG_INFO, "sip", "udp fd=%d bound reactor[%u]", fd, i);
        }
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
    /* P3: per-reactor rx/dg 清理 */
    if (t->per_reactor_dg) {
        for (int r = 0; r < t->n_rx; ++r) {
            for (int i = 0; i < NET_BATCH; ++i)
                free(t->per_reactor_dg[r][i].buf);
        }
        free(t->per_reactor_dg);
    }
    free(t->per_reactor_rx);
    pthread_mutex_destroy(&t->conn_lock);
    pthread_mutex_destroy(&t->clock);
    pthread_mutex_destroy(&t->ip_conn_lock);
    free((void *)t->cfg.listen_ip);   /* strdup 来的，sip_tr_create 里分配 */
    free(t);
}

int sip_tr_send(sip_transport_t *t, sip_tr_proto_t proto,
                const char *buf, size_t len, const nsbc_sa_t *dst)
{
    /* SIP trace TX（开关 g_sip_trace=1 时才打） */
    if (g_sip_trace && dst) {
        char dip[INET6_ADDRSTRLEN] = {0};
        uint16_t dport = nsbc_sa_port(dst);
        nsbc_sa_fmt(dst, dip, sizeof(dip));
        const char *ptag = "?";
        if (proto == SIP_TR_UDP) ptag = "UDP";
        else if (proto == SIP_TR_TCP) ptag = "TCP";
        else if (proto == SIP_TR_TLS) ptag = "TLS";
        else if (proto == SIP_TR_WS) ptag = "WS";
        else if (proto == SIP_TR_WSS) ptag = "WSS";
        sip_trace_dump(ptag, 1, dip, dport, buf, len);
    }
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
        /* dead=1 的 conn 可能还没从 idx 表摘掉（conn_close 先 dead=1 再摘链），
         * 必须跳过；同时 sat_add 持引用，保护解锁后 conn 不会被 free */
        if (!c->dead && c->in_use &&
            (c->is_tls == want_tls) && nsbc_sa_eq(&c->peer, dst)) {
            found = c;
            sat_add(c->refcount, 1);  /* 持有跨 reactor 引用 */
            break;
        }
    }
    pthread_mutex_unlock(&t->idx.lock);
    if (!found)
        return NSBC_NOTSUP;
    /* 入队发送 —— 解决 EAGAIN 部分写入时的字节流损坏炸弹。
     * sip_tcp_enqueue 返回 0 表示数据已安全落发送缓冲，
     * 稍后 reactor EPOLLOUT 会驱动 flush。
     * conn_release 释放上面的引用；enqueue 内部还会再查 dead 做最后防线。 */
    int rc = sip_tcp_enqueue(t, found, (const uint8_t *)buf, len);
    conn_release(found);
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

    /* 每次 sip_tr_sync_from_profiles 会对每个 combo 调用本函数一次，
     * 连续打印 LOG_INFO 是噪音。只在确实新增了 fd 时打 NOTICE，否则 DEBUG 级。 */
    if (added > 0)
        llog(LOG_NOTICE, "sip",
             "add_listeners done: added=%d udp=%d tcp=%d tls=%d",
             added, t->udp_fd_count, t->tcp_count, t->tls_count);
    else
        llog(LOG_DEBUG, "sip",
             "add_listeners done (dup): added=0 udp=%d tcp=%d tls=%d",
             t->udp_fd_count, t->tcp_count, t->tls_count);
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
