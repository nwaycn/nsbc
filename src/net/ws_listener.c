/* NSBC WebSocket Listener — RFC 6455 (最小子集) + SIP (RFC 7118)
 *
 * Phase 2 架构（epoll + 固定 worker 线程池 + 非阻塞状态机）：
 *   ┌─────────────┐  accept()   ┌──────────────┐  eventfd  ┌──────────────┐
 *   │ Main Thread │ ──────────► │ Worker 0     │ ◄───────── │ Worker 1..N  │
 *   └─────────────┘             │ epoll + state│            │  (一样)       │
 *                                └──────────────┘            └──────────────┘
 *
 * 每个 worker: 一个 epoll fd + 一个 eventfd（接收主线程 handoff）
 * 每个 conn:   一个状态机（HANDSHAKE → HTTP_READ → READ_FRAME_* → CLOSED）
 * 非阻塞读:    每次 epoll EPOLLIN 触发时，状态机推进到能推进的最远位置
 * 非阻塞写:    ws_conn_send() → enqueue 到 send_queue → 标记 EPOLLOUT
 *              worker 在 EPOLLOUT 时 flush send_queue
 * TLS (Phase 2): SSL_accept/read/write 映射 WANT_READ/WANT_WRITE → epoll EPOLLIN/EPOLLOUT
 *
 * 外部接口不变：
 *   ws_conn_t   — 前 3 字段 fd/closed/next 原位（registrar 指针安全）
 *   ws_conn_alive(conn) — 链表遍历 + closed 判断
 *   ws_conn_send(conn,buf,len) — enqueue + EPOLLOUT 唤醒
 *   ws_listener_init/start/stop/shutdown — 原样
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ws_listener.h"
#include "log.h"
#include "nsbc_common.h"
#include "tls_io.h"

#ifdef NSBC_WITH_OPENSSL
#include "openssl_compat.h"
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

/* ==== RFC 6455 常量 ==== */
#define WS_GUID         "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_OP_TEXT      0x1
#define WS_OP_BINARY    0x2
#define WS_OP_CLOSE     0x8
#define WS_OP_PING      0x9
#define WS_OP_PONG      0xA
#define WS_MAX_PAYLOAD  (64 * 1024)          /* 64KB — 防内存 DoS */
#define WS_READ_BUF     16384                 /* 16KB per-conn ring read buf */
/* 读缓冲上限：最大 WS 扩展头 14B + WS_MAX_PAYLOAD，另留 512B 余量，
 * 保证 64KB 帧能一次性完整落入缓冲（否则水平触发 EPOLLIN 永久空转）。*/
#define WS_READ_BUF_MAX (WS_MAX_PAYLOAD + 14 + 512)
#define WS_SEND_QUEUE_CAP  8                  /* 每 conn 发送队列容量 */

/* P2-2：TLS 握手（非阻塞）超时与并发上限 */
#define WS_TLS_HS_TIMEOUT_MS  5000
#define WS_TLS_HS_MAX         256

/* ==== WS 连接状态机 ==== */
typedef enum {
    CS_HANDSHAKE = 0,   /* HTTP upgrade 解析中 */
    CS_READ_FRAME,      /* WS frame 头部 + payload 读取中 */
    CS_PENDING_TXN,     /* 有发送队列等 flush */
    CS_CLOSING,         /* CLOSE 帧已发/已收，等收尾 */
    CS_TLS_HANDSHAKE    /* P2-2：TLS 握手进行中（非阻塞，由 epoll 驱动） */
} ws_conn_state_t;

/* ==== 发送队列项 ==== */
typedef struct ws_send_item {
    uint8_t  *data;      /* 完整 WS frame (header + payload) */
    size_t     len;
    size_t     sent;     /* 已发送字节 */
} ws_send_item_t;

/* ==== WS 连接上下文 — ⚠️ 前 3 字段 fd/closed/next 保持原位 ==== */
typedef struct ws_conn {
    /* ── 外部可见（registrar 存指针用） ────────────────── */
    int            fd;          /* TCP fd（TLS 时仍是裸 fd，非阻塞） */
    int            closed;      /* 1=已关闭（ws_conn_alive 判死） */
    struct ws_conn *next;

    /* ── 内部私有（状态机 + 发送队列） ──────────────────── */
    int            worker_idx;   /* 所属 worker 下标 */
    int            close_notified; /* REQ-CALL-HANGUP-001：关闭回调只发一次 */
    ws_conn_state_t state;

    /* 统一 per-conn 读缓冲 — 动态分配（按需扩到 WS_READ_BUF_MAX）
     * 默认 accept 时只 malloc WS_READ_BUF + 128 ≈ 16KB，
     * 大 payload（>16KB）触发 realloc 到 WS_READ_BUF_MAX ≈ 64KB。
     * 几百 WS 连接场景省几十 MB 固定内存。*/
    uint8_t        *read_buf;
    size_t         read_buf_cap;   /* 当前分配容量（动态）*/
    size_t         read_pos;       /* 已消费位置 */
    size_t         read_len;       /* 缓冲里有效数据长度 */

    /* WS frame header 解析状态 */
    uint8_t        ws_opcode;
    uint8_t        ws_mask_key[4];
    int            ws_has_mask;
    uint64_t       ws_plen;      /* payload 期望长度 */
    int            ws_hdr_done;  /* header 是否完整 */

    /* 发送队列 */
    pthread_mutex_t send_mtx;
    ws_send_item_t send_q[WS_SEND_QUEUE_CAP];
    int            send_q_head;
    int            send_q_tail;
    int            send_q_count;
    int            send_q_flushing;
    volatile int   send_pending;

    /* TLS (Phase 2) */
    void          *ssl;
    void          *rbio;
    int            tls_want;
    uint64_t       tls_deadline_ms; /* P2-2：握手截止时间（ms）；0=非握手态 */
    int            hs_counted;      /* P2-2：1=已计入 tls_hs_active，防重复扣减 */

    /* WebSocket ping/pong (RFC 6455 §5.5.2) — idle conn detection
     * Proxy idle timeout (Nginx/HAProxy 60-300s) silently drops WSS TCP
     * when no SIP traffic. Session Timer is a TODO skeleton; ws ping
     * every 30s keeps the conn alive and detects dead proxies quickly. */
    uint64_t       last_rx_ms;     /* last frame received (ms since epoch) */
    uint64_t       last_ping_ms;   /* last ping sent */
    int            pongs_missed;   /* consecutive pong misses, close at 3 */
} ws_conn_t;

/* compact: read_pos > 0 时把剩余数据移到 buf 开头 */
static void ws_compact_read_buf(ws_conn_t *c)
{
    if (c->read_pos > 0) {
        size_t remain = c->read_len - c->read_pos;
        if (remain > 0)
            memmove(c->read_buf, c->read_buf + c->read_pos, remain);
        c->read_len = remain;
        c->read_pos = 0;
    }
}

/* ==== WebSocket ping/pong helpers (RFC 6455 §5.5.2) ==== */

/* ws_now_ms — monotonic wall clock in ms (CLOCK_REALTIME, fine for idle checks).
 * Avoids gettimeofday every frame — called once per worker loop pass (500ms). */
static uint64_t ws_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

/* RFC 6455 ping frame: FIN=1 opcode=0x9 RSV=0 mask=0 payload=0 → 2 bytes total.
 * Returns len (2) or 0 if send queue full. */
static size_t ws_build_ping_frame(uint8_t *out, size_t out_sz)
{
    if (out_sz < 2) return 0;
    out[0] = 0x89;  /* FIN + PING opcode (0x9) */
    out[1] = 0x00;  /* mask=0, payload len=0 */
    return 2;
}

/* ws_build_pong_frame intentionally omitted — PING handler (L621+) builds
 * pong inline there because it needs to echo the exact payload bytes. */

/* RFC 6455 §11.8 default ping interval recommended 20-30s.
 * We use 30s — comfortably below Nginx/HAProxy default idle timeout (60s). */
#define WS_PING_INTERVAL_MS   30000ULL   /* send ping if idle >= this */
#define WS_PING_TIMEOUT_MS    15000ULL   /* wait this long for pong before counting miss */
#define WS_PING_MAX_MISSED    3          /* close after N consecutive misses (~90s total) */

/* ==== atomic 替代（GCC builtin — 不用 C11 stdatomic）==== */
#define WS_ATOMIC_STORE(p, v)     __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define WS_ATOMIC_LOAD(p)         __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define WS_ATOMIC_FETCH_ADD(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define WS_ATOMIC_FETCH_SUB(p, v) __atomic_fetch_sub((p), (v), __ATOMIC_SEQ_CST)

/* ==== Worker 线程 ==== */
typedef struct {
    pthread_t    tid;
    int          epfd;          /* 本 worker 的 epoll fd */
    int          wakeup_fd;     /* eventfd — 主线程 handoff 新 fd 过来 */
    int          idx;
    volatile int running;       /* 非严格原子 — 仅 worker 自己写 */
} ws_worker_t;

/* ==== Listener 全局状态 ==== */
typedef struct {
    pthread_t    accept_tid;
    int          accept_started;
    volatile int stop_flag;    /* __atomic_* 操作保护 */
    int          listen_fd;
    uint16_t     port;
    int          tls_enabled;
    int          enabled;
    void        *tls_ctx;
    ws_rx_fn     rx_fn;
    void        *rx_ud;
    ws_close_fn  close_fn;      /* REQ-CALL-HANGUP-001：conn 关闭 → 终止绑定呼叫 */
    void        *close_ud;
    pthread_mutex_t conn_mtx;
    ws_conn_t   *conn_head;
    int          conn_count;    /* __atomic_* 操作保护 */

    int          worker_count;
    ws_worker_t *workers;

    /* P2-2：当前处于 TLS 握手中的连接数（__atomic_* 维护） */
    volatile int tls_hs_active;
} ws_state_t;

static ws_state_t g_ws = { .listen_fd = -1, .stop_flag = 0 };

/* P2-2：释放一个握手并发槽（幂等，任意关闭路径都可调用） */
static void ws_hs_release(ws_conn_t *c)
{
    if (c->hs_counted) {
        c->hs_counted = 0;
        __atomic_fetch_sub(&g_ws.tls_hs_active, 1, __ATOMIC_RELAXED);
    }
}

/* epoll 事件批量数组（一次 epoll_wait 最多取这么多） */
#define WS_EPOLL_BATCH  256

/* ==== OpenSSL 工具（仅 Phase 1 plain WS 用不到；TLS 启用时才用）==== */

#ifdef NSBC_WITH_OPENSSL
static int base64_encode(const uint8_t *data, size_t len, char *out, size_t out_cap)
{
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *bmem = BIO_new(BIO_s_mem());
    if (!b64 || !bmem) return -1;
    BIO *b = BIO_push(b64, bmem);
    BIO_set_flags(b, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b, data, (int)len);
    BIO_flush(b);
    BUF_MEM *bptr = NULL;
    BIO_get_mem_ptr(b, &bptr);
    if ((size_t)bptr->length >= out_cap) {
        BIO_free_all(b);
        return -1;
    }
    memcpy(out, bptr->data, bptr->length);
    out[bptr->length] = '\0';
    BIO_free_all(b);
    return 0;
}
static void ws_compute_accept(const char *key, char out[64])
{
    char combo[256];
    snprintf(combo, sizeof(combo), "%s%s", key, WS_GUID);
    uint8_t hash[20];
    SHA1((const uint8_t *)combo, strlen(combo), hash);
    base64_encode(hash, 20, out, 64);
}
#endif

/* ==== Socket 读写（非阻塞 + TLS Phase 2 入口）==== */

/* 非阻塞读：TLS 走 SSL_read 并映射 WANT_READ/WANT_WRITE；非 TLS 直接 recv
 * 返回: n>0 读了 n 字节, 0=EOF, -1=错误 (errno 保留), -2=TLS WANT_READ, -3=TLS WANT_WRITE */
static ssize_t ws_nb_read(ws_conn_t *c, uint8_t *buf, size_t cap)
{
    if (c->ssl) {
#ifdef NSBC_WITH_OPENSSL
        int n = SSL_read((SSL *)c->ssl, buf, (int)cap);
        if (n > 0) { c->tls_want = 0; return n; }
        int err = SSL_get_error((SSL *)c->ssl, n);
        if (err == SSL_ERROR_WANT_READ)  { c->tls_want = 1; return -2; }
        if (err == SSL_ERROR_WANT_WRITE) { c->tls_want = 2; return -3; }
        return -1;   /* 真正错误 / EOF */
#endif
        return -1;
    }
    /* 非 TLS — 裸 recv，必须非阻塞 */
    ssize_t n = recv(c->fd, buf, cap, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;  /* 相当于 WANT_READ */
        if (errno == EINTR) return -2;   /* 重来 */
        return -1;
    }
    return n;
}

/* 非阻塞写：返回 n>=0 写了 n 字节, -1=错误, -2=EAGAIN, -3=TLS WANT_WRITE */
static ssize_t ws_nb_write(ws_conn_t *c, const uint8_t *buf, size_t cap)
{
    if (c->ssl) {
#ifdef NSBC_WITH_OPENSSL
        int n = SSL_write((SSL *)c->ssl, buf, (int)cap);
        if (n > 0) { c->tls_want = 0; return n; }
        int err = SSL_get_error((SSL *)c->ssl, n);
        if (err == SSL_ERROR_WANT_WRITE) { c->tls_want = 2; return -3; }
        if (err == SSL_ERROR_WANT_READ)  { c->tls_want = 1; return -2; }
        return -1;
#endif
        return -1;
    }
    ssize_t n = send(c->fd, buf, cap, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        if (errno == EINTR) return -2;
        return -1;
    }
    return n;
}

/* ==== HTTP header 查找工具 ==== */

static const char *http_find_header(const char *hdrs, size_t hdrs_len,
                                    const char *name, size_t *val_len_out)
{
    size_t name_len = strlen(name);
    const char *p = hdrs;
    const char *end = hdrs + hdrs_len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) break;
        size_t ll = (size_t)(eol - p);
        if (ll >= name_len + 2 &&
            !strncasecmp(p, name, name_len) &&
            p[name_len] == ':') {
            const char *val = p + name_len + 1;
            while (val < eol && (*val == ' ' || *val == '\t')) val++;
            size_t vlen = (size_t)(eol - val);
            if (vlen > 0 && val[vlen - 1] == '\r') vlen--;
            *val_len_out = vlen;
            return val;
        }
        p = eol + 1;
    }
    return NULL;
}

/* ==== WS frame header 纯缓冲解析 ====
 * 从任意 buf 解析 WS header，填充 out 字段。
 * 返回: 0=header 完整（out 全填好）, 1=header 还不完整（需要更多数据）, -1=error
 * hdr_len_out = 完整 header 的总字节数（2+扩展长度+4 mask）
 * 当 return 0 时，buf 至少 hdr_len_out 字节 */
static int ws_frame_hdr_parse_buf(const uint8_t *buf, size_t buflen,
                                  int *opcode_out, uint64_t *plen_out,
                                  uint8_t mask_key_out[4], int *has_mask_out,
                                  size_t *hdr_len_out)
{
    if (buflen < 2) return 1;  /* 还需要更多 */

    uint8_t opcode = buf[0] & 0x0F;
    uint8_t mask   = buf[1] & 0x80;
    uint64_t plen  = buf[1] & 0x7F;
    size_t hdr_total;

    if (plen < 126) {
        hdr_total = 2 + (mask ? 4 : 0);
    } else if (plen == 126) {
        if (buflen < 4) return 1;
        plen = ((uint64_t)buf[2] << 8) | buf[3];
        hdr_total = 4 + (mask ? 4 : 0);
    } else {
        if (buflen < 10) return 1;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | buf[2 + i];
        hdr_total = 10 + (mask ? 4 : 0);
    }

    if (buflen < hdr_total) return 1;  /* header 还没完整 */

    if (plen > WS_MAX_PAYLOAD) {
        llog(LOG_WARN, "ws", "frame too large (%lu), closing", plen);
        return -1;
    }

    if (mask) memcpy(mask_key_out, buf + hdr_total - 4, 4);
    *has_mask_out = mask ? 1 : 0;
    *opcode_out = opcode;
    *plen_out = plen;
    *hdr_len_out = hdr_total;
    return 0;
}

/* ==== HTTP upgrade 处理 — 从 read_buf 提取的 headers 里解析 Upgrade ====
 * 返回: 0=ok (响应已入 send queue), -1=error, -2=还没完整 headers (不该到这) */
static int ws_http_process(ws_conn_t *c, const uint8_t *hdrs, size_t hdrs_len)
{
    size_t vlen = 0;
    const char *upg = http_find_header((const char *)hdrs, hdrs_len,
                                       "Upgrade", &vlen);
    if (!upg || strncasecmp(upg, "websocket", 9)) return -1;

    const char *ws_key = http_find_header((const char *)hdrs, hdrs_len,
                                          "Sec-WebSocket-Key", &vlen);
    if (!ws_key || vlen == 0 || vlen > 64) return -1;
    char key_buf[65];
    memcpy(key_buf, ws_key, vlen);
    key_buf[vlen] = '\0';

#ifdef NSBC_WITH_OPENSSL
    const char *ws_proto = http_find_header((const char *)hdrs, hdrs_len,
                                            "Sec-WebSocket-Protocol", &vlen);
    char proto_buf[128];
    int proto_found = 0;
    if (ws_proto && vlen > 0 && vlen < sizeof(proto_buf)) {
        memcpy(proto_buf, ws_proto, vlen);
        proto_buf[vlen] = '\0';
        const char *p = proto_buf;
        while ((p = strstr(p, "sip"))) {
            int before_ok = (p == proto_buf) || (p[-1] == ',' || p[-1] == ' ');
            int after_ok  = (p[3] == '\0') || (p[3] == ',' || p[3] == ' ');
            if (before_ok && after_ok) { proto_found = 1; break; }
            p += 3;
        }
    }

    char accept[64];
    ws_compute_accept(key_buf, accept);
    char resp[1024];
    if (proto_found)
        snprintf(resp, sizeof(resp),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n"
            "Sec-WebSocket-Protocol: sip\r\n\r\n", accept);
    else
        snprintf(resp, sizeof(resp),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n", accept);

    /* 响应走 send queue */
    size_t rlen = strlen(resp);
    ws_send_item_t item;
    item.data = (uint8_t *)malloc(rlen);
    if (!item.data) return -1;
    memcpy(item.data, resp, rlen);
    item.len = rlen;
    item.sent = 0;
    pthread_mutex_lock(&c->send_mtx);
    if (c->send_q_count >= WS_SEND_QUEUE_CAP) {
        pthread_mutex_unlock(&c->send_mtx);
        free(item.data);
        return -1;
    }
    c->send_q[c->send_q_tail] = item;
    c->send_q_tail = (c->send_q_tail + 1) % WS_SEND_QUEUE_CAP;
    c->send_q_count++;
    pthread_mutex_unlock(&c->send_mtx);
    /* 直接 epoll_ctl MOD 加 EPOLLOUT（线程安全）*/
    if (c->fd >= 0) {
        struct epoll_event ee;
        ee.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
        ee.data.ptr = c;
        (void)epoll_ctl(g_ws.workers[c->worker_idx].epfd, EPOLL_CTL_MOD, c->fd, &ee);
    }

    llog(LOG_INFO, "ws", "handshake ok (key=%s, proto=%s)",
         key_buf, proto_found ? "sip" : "(none)");
    return 0;
#else
    (void)c;
    return -1;
#endif
}

/* ==== Worker: flush send queue ==== */
/* 返回: 0=flush complete (队空), -1=error (fd 关闭), 1=partial (继续等 EPOLLOUT) */
static int ws_flush_send_queue(ws_conn_t *c)
{
    pthread_mutex_lock(&c->send_mtx);
    if (c->send_q_count == 0) { pthread_mutex_unlock(&c->send_mtx); return 0; }
    ws_send_item_t *item = &c->send_q[c->send_q_head];
    pthread_mutex_unlock(&c->send_mtx);

    while (item->sent < item->len) {
        ssize_t n = ws_nb_write(c, item->data + item->sent, item->len - item->sent);
        if (n < 0) {
            if (n == -2 || n == -3) return 1;  /* EAGAIN / TLS WANT_WRITE — 下次 EPOLLOUT 再来 */
            return -1;  /* 真错误 */
        }
        item->sent += (size_t)n;
    }
    /* 这个 item 发完了，出队 */
    pthread_mutex_lock(&c->send_mtx);
    free(item->data);
    c->send_q_head = (c->send_q_head + 1) % WS_SEND_QUEUE_CAP;
    c->send_q_count--;
    pthread_mutex_unlock(&c->send_mtx);
    /* 继续 flush 下一个 */
    return ws_flush_send_queue(c);
}

/* ==== 状态机主循环（worker 每次 EPOLLIN/EPOLLOUT 触发调用）==== */
/* 返回 0=继续, -1=应关闭 */
/* P2-2：非阻塞 TLS 握手推进。
 * 返回 1=握手完成（可继续处理本次 events）；0=等待下次 epoll 事件；-1=失败需关闭。*/
static int ws_tls_handshake_step(ws_conn_t *c, int events)
{
    (void)events;
    if (!c->ssl) return -1;
#ifdef NSBC_WITH_OPENSSL
    int rc = SSL_do_handshake((SSL *)c->ssl);
    if (rc == 1) {
        c->tls_deadline_ms = 0;
        ws_hs_release(c);
        c->state = CS_HANDSHAKE;
        c->tls_want = 0;
        return 1;
    }
    int err = SSL_get_error((SSL *)c->ssl, rc);
    if (err == SSL_ERROR_WANT_READ)  { c->tls_want = 1; return 0; }
    if (err == SSL_ERROR_WANT_WRITE) { c->tls_want = 2; return 0; }

    {
        unsigned long e = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(e, err_buf, sizeof(err_buf));
        /* P0-3：TLS 失败直接断开，绝不降级明文 */
        llog(LOG_WARN, "ws", "TLS handshake fail: err=%d %s — dropping peer (no plain fallback)",
             err, err_buf);
    }
    return -1;
#else
    return -1;
#endif
}

static int ws_conn_step(ws_conn_t *c, int events)
{
    /* 先处理发送（EPOLLOUT）*/
    if (events & EPOLLOUT) {
        int rc = ws_flush_send_queue(c);
        if (rc < 0) return -1;
    }

    /* 读相关事件 — 统一先把 socket 数据读进 read_buf 尾部。
     * EPOLLOUT 也进来：TLS 读返回 WANT_WRITE 后需要借可写事件重试 SSL_read。 */
    if (events & (EPOLLIN | EPOLLPRI | EPOLLOUT)) {
        /* compact 一下腾出空间 */
        ws_compact_read_buf(c);

        /* 容量不够 → realloc 到 WS_READ_BUF_MAX（上限）。
         * 上限必须能容纳「最大扩展头 + WS_MAX_PAYLOAD」，否则 64KB 帧永远
         * 装不进缓冲，水平触发 EPOLLIN 会持续空转 100% CPU。*/
        size_t room = (c->read_buf_cap > c->read_len + 256)
                    ? c->read_buf_cap - c->read_len - 256 : 0;
        if (room == 0 && c->read_buf_cap < WS_READ_BUF_MAX) {
            size_t new_cap = WS_READ_BUF_MAX;
            uint8_t *nb = (uint8_t *)realloc(c->read_buf, new_cap);
            if (!nb) return -1;
            c->read_buf = nb;
            c->read_buf_cap = new_cap;
            room = c->read_buf_cap - c->read_len - 256;
        }
        if (room == 0) {
            /* 缓冲已达上限仍无空间 — 帧超过上限，断开避免空转 */
            return -1;
        }
        {
            ssize_t n = ws_nb_read(c, c->read_buf + c->read_len, room);
            if (n == -1) return -1;
            if (n == 0) return -1;         /* 对端有序关闭（否则水平触发空转） */
            if (n == -2) goto done_read;  /* EAGAIN / TLS WANT_READ */
            if (n == -3) {                /* TLS WANT_WRITE — 等 EPOLLOUT 后重试 */
                c->tls_want = 2;
                goto done_read;
            }
            /* 仅正值累加；任何负值都已在上面分流，防止 size_t 下溢 */
            if (n > 0) c->read_len += (size_t)n;
        }

        /* ── 状态 1: HTTP HANDSHAKE ──────────────────────── */
        if (c->state == CS_HANDSHAKE) {
            /* 在 read_buf[read_pos : read_len] 里找 HTTP 头结束符 */
            size_t avail = c->read_len - c->read_pos;
            const uint8_t *p = c->read_buf + c->read_pos;
            int term = 0;
            size_t hdr_end = 0;

            if (avail >= 4) {
                /* 搜索 \r\n\r\n */
                for (size_t i = 0; i + 3 < avail; i++) {
                    if (p[i]=='\r' && p[i+1]=='\n' &&
                        p[i+2]=='\r' && p[i+3]=='\n') {
                        term = 1;
                        hdr_end = c->read_pos + i + 4;
                        break;
                    }
                }
                /* 也兼容 \n\n */
                if (!term && avail >= 2) {
                    for (size_t i = 0; i + 1 < avail; i++) {
                        if (p[i]=='\n' && p[i+1]=='\n') {
                            term = 1;
                            hdr_end = c->read_pos + i + 2;
                            break;
                        }
                    }
                }
            }

            if (term) {
                /* 提取完整 HTTP headers 块 */
                int rc = ws_http_process(c, c->read_buf + c->read_pos,
                                          hdr_end - c->read_pos);
                if (rc != 0) return -1;

                /* 消费 HTTP headers + terminator（保留后面的数据——可能是第一个 WS frame！）*/
                c->read_pos = hdr_end;
                ws_compact_read_buf(c);

                /* 切到 READ_FRAME，header 状态清零 */
                c->state = CS_READ_FRAME;
                c->ws_hdr_done = 0;
                c->ws_opcode = 0;
                c->ws_plen = 0;
                c->ws_has_mask = 0;
                memset(c->ws_mask_key, 0, 4);
                /* 继续在下面处理 READ_FRAME（数据已在 read_buf 里！）*/
                /* 不 return 0 了——让它 fall through 到 READ_FRAME */
            } else {
                /* Slowloris guard — 缓冲快满还没 terminator → drop */
                if (c->read_buf_cap - c->read_pos < 512) return -1;
                return 0;  /* 还没读完 headers，等下次 EPOLLIN */
            }
        }

        /* ── 状态 2: READ_FRAME（header + payload 全在 read_buf 里）── */
        if (c->state == CS_READ_FRAME) {
            /* 循环处理 read_buf 里所有能解析的 WS frame */
            for (;;) {
                ws_compact_read_buf(c);
                size_t avail = c->read_len - c->read_pos;
                if (avail < 2) goto done_read;  /* 连 2B 基础头都不够 */

                int opcode;
                uint64_t plen;
                uint8_t mask_key[4];
                int has_mask;
                size_t hdr_len;

                int rc = ws_frame_hdr_parse_buf(
                    c->read_buf + c->read_pos, avail,
                    &opcode, &plen, mask_key, &has_mask, &hdr_len);

                if (rc == 1) goto done_read;  /* header 还没完整 */
                if (rc == -1) return -1;      /* 错误（frame 太大等）*/

                /* header 完整 — 检查 payload 是否也在 read_buf 里 */
                size_t total_frame = hdr_len + (size_t)plen;
                if (avail < total_frame) goto done_read;  /* payload 还没到齐 */

                /* 完整 frame 在 read_buf[read_pos : read_pos + total_frame] */
                uint8_t *frame_data = c->read_buf + c->read_pos + hdr_len;
                size_t frame_plen = (size_t)plen;

                /* 解 mask（就地解，因为只是消费 read_buf 里的数据）*/
                if (has_mask && frame_plen > 0) {
                    for (size_t i = 0; i < frame_plen; i++)
                        frame_data[i] ^= mask_key[i & 3];
                }

                /* CLOSE frame */
                if (opcode == WS_OP_CLOSE) {
                    return -1;
                }

                /* ANY frame received = connection alive — reset idle timer + ping miss counter
                 * (covers PING from remote, PONG to our ping, TEXT/BINARY SIP, etc.) */
                c->last_rx_ms = ws_now_ms();
                c->pongs_missed = 0;
                c->last_ping_ms = 0;   /* 已有流量，作废本轮 ping 等待，避免误计 miss */

                /* PING — 回 PONG (already reset pongs_missed above) */
                if (opcode == WS_OP_PING) {
                    uint8_t pong[10];
                    size_t phdr = 0;
                    pong[phdr++] = 0x80 | WS_OP_PONG;
                    pong[phdr++] = 0x00;
                    if (frame_plen > 0) {
                        if (frame_plen <= 125) {
                            pong[1] = (uint8_t)frame_plen;
                        } else if (frame_plen <= 0xFFFF) {
                            pong[1] = 126;
                            pong[phdr++] = (uint8_t)(frame_plen >> 8);
                            pong[phdr++] = (uint8_t)(frame_plen & 0xff);
                        } else {
                            /* >64KB：必须写 8 字节扩展长度，否则 length 字段为 0 */
                            pong[1] = 127;
                            for (int k = 7; k >= 0; --k)
                                pong[phdr++] = (uint8_t)((uint64_t)frame_plen >> (k * 8));
                        }
                    }
                    ws_send_item_t item;
                    size_t pong_total = phdr + frame_plen;
                    item.data = (uint8_t *)malloc(pong_total);
                    if (item.data) {
                        memcpy(item.data, pong, phdr);
                        if (frame_plen > 0) memcpy(item.data + phdr, frame_data, frame_plen);
                        item.len = pong_total;
                        item.sent = 0;
                        pthread_mutex_lock(&c->send_mtx);
                        if (c->send_q_count < WS_SEND_QUEUE_CAP) {
                            c->send_q[c->send_q_tail] = item;
                            c->send_q_tail = (c->send_q_tail + 1) % WS_SEND_QUEUE_CAP;
                            c->send_q_count++;
                        } else {
                            free(item.data);
                        }
                        pthread_mutex_unlock(&c->send_mtx);
                    }
                    /* 消费这个 frame */
                    c->read_pos += total_frame;
                    continue;
                }

                /* PONG — 忽略 */
                if (opcode == WS_OP_PONG) {
                    c->read_pos += total_frame;
                    continue;
                }

                /* TEXT / BINARY — SIP over WS 通常 TEXT */
                if (g_ws.rx_fn && frame_plen > 0) {
                    g_ws.rx_fn(g_ws.rx_ud, (const char *)frame_data, frame_plen, c);
                }

                /* 消费这个 frame，继续 read_buf 里可能有的下一个 */
                c->read_pos += total_frame;
            }
        }
    }

done_read:
    return 0;
}

/* ==== Worker epoll 事件掩码计算 ==== */
static int ws_calc_epoll_events(ws_conn_t *c)
{
    /* P2-2：握手态只关心读，或 SSL_do_handshake 要求可写时关心写 */
    if (c->state == CS_TLS_HANDSHAKE) {
        uint32_t hev = EPOLLRDHUP;
        hev |= (c->tls_want == 2) ? EPOLLOUT : EPOLLIN;
        return (int)hev;
    }

    uint32_t ev = 0;
    pthread_mutex_lock(&c->send_mtx);
    int need_flush = (c->send_q_count > 0);
    pthread_mutex_unlock(&c->send_mtx);

    /* EPOLLIN: 只要不是 CLOSING 就需要读 */
    if (c->state != CS_CLOSING) ev |= EPOLLIN;
    /* EPOLLOUT: 有待发送，或 TLS 读需要可写后才能重试（WANT_WRITE） */
    if (need_flush || c->tls_want == 2) ev |= EPOLLOUT;
    /* 加上 EPOLLERR / EPOLLHUP 总是需要 */
    ev |= EPOLLRDHUP;
    return ev;
}

/* ==== Worker: conn 的 epoll_ctl 更新 ==== */
static int ws_epoll_update(int epfd, ws_conn_t *c, uint32_t events)
{
    struct epoll_event ee;
    ee.events = events;
    ee.data.ptr = c;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ee);
}

/* ==== 关闭一个 conn（helper — worker 和其他地方都可能调用）====
 * ⚠️ 不 free(c) — registrar 可能持有指针，留 shutdown 统一释放 */
static void ws_close_conn(int epfd, ws_conn_t *c)
{
    /* 防重入：同一 conn 只清理/通知一次（shutdown 路径不经过这里） */
    if (c->close_notified)
        return;
    c->close_notified = 1;

    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    ws_hs_release(c);   /* P2-2：任何关闭路径都要归还握手并发槽 */
    if (c->ssl) {
#ifdef NSBC_WITH_OPENSSL
        SSL_free((SSL *)c->ssl);   /* SSL_set_bio 后 SSL_free 连带 free BIO */
#endif
        c->ssl = NULL;
        c->rbio = NULL;
    }
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    if (c->read_buf) { free(c->read_buf); c->read_buf = NULL; c->read_buf_cap = 0; }
    pthread_mutex_lock(&c->send_mtx);
    while (c->send_q_count > 0) {
        ws_send_item_t *item = &c->send_q[c->send_q_head];
        free(item->data);
        c->send_q_head = (c->send_q_head + 1) % WS_SEND_QUEUE_CAP;
        c->send_q_count--;
    }
    pthread_mutex_unlock(&c->send_mtx);
    WS_ATOMIC_FETCH_SUB(&g_ws.conn_count, 1);

    /* 通知上层连接已死（REQ-CALL-HANGUP-001）。此处已不持任何锁，
     * 回调内可安全调用 b2b_terminate_*（仅做非阻塞发送）。 */
    if (g_ws.close_fn)
        g_ws.close_fn(g_ws.close_ud, c);
}

/* ==== Worker 主循环 ==== */
static void *ws_worker_thread(void *ud)
{
    ws_worker_t *w = (ws_worker_t *)ud;
    w->running = 1;
    llog(LOG_INFO, "ws", "worker[%d] started (epfd=%d)", w->idx, w->epfd);

    struct epoll_event events[WS_EPOLL_BATCH];

    while (WS_ATOMIC_LOAD(&g_ws.stop_flag) == 0) {
        int n = epoll_wait(w->epfd, events, WS_EPOLL_BATCH, 500);  /* 500ms 超时 */
        if (n < 0) {
            if (errno == EINTR) continue;
            if (WS_ATOMIC_LOAD(&g_ws.stop_flag)) break;
            llog(LOG_WARN, "ws", "worker[%d] epoll_wait errno=%d", w->idx, errno);
            continue;
        }

        for (int i = 0; i < n; i++) {
            struct epoll_event *ev = &events[i];

            /* eventfd 唤醒 — 主线程 handoff 了新 fd（已被主线程直接 epoll_ctl ADD 进来）*/
            if (ev->data.fd == w->wakeup_fd) {
                uint64_t kick;
                (void)read(w->wakeup_fd, &kick, sizeof(kick));
                continue;
            }

            ws_conn_t *c = (ws_conn_t *)ev->data.ptr;

            /* EPOLLERR / EPOLLHUP — 关 */
            if (ev->events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                c->closed = 1;
                ws_close_conn(w->epfd, c);
                continue;    /* ← Bug 1 fix: continue 不跳出 worker 循环 */
            }

            /* P2-2：TLS 握手态由 epoll 驱动推进，不进入 WS 协议状态机 */
            if (c->state == CS_TLS_HANDSHAKE) {
                int hs = ws_tls_handshake_step(c, ev->events);
                if (hs < 0) {
                    c->closed = 1;
                    ws_close_conn(w->epfd, c);
                    continue;
                }
                if (hs == 0) {
                    ws_epoll_update(w->epfd, c, (uint32_t)ws_calc_epoll_events(c));
                    continue;
                }
                /* hs == 1：交给下面的 ws_conn_step 处理可能已到达的 HTTP 请求 */
            }

            /* 状态机推进 */
            int step_rc = ws_conn_step(c, ev->events);
            if (step_rc < 0) {
                c->closed = 1;
                ws_close_conn(w->epfd, c);
                continue;    /* ← Bug 1 fix */
            }

            /* 更新 epoll 事件掩码 */
            uint32_t new_ev = ws_calc_epoll_events(c);
            ws_epoll_update(w->epfd, c, new_ev);
        }

        /* ==== Idle pass (ping detection) — runs every 500ms ====
         * Iterate all active conns on this worker's round-robin slice.
         * If a conn has been idle (no RX) >= WS_PING_INTERVAL_MS,
         * send a ping frame. If ping sent and no pong after
         * WS_PING_TIMEOUT_MS, increment pongs_missed. On >=3 missed
         * (~90s total dead), close the conn.
         *
         * Why? Session Timer (RFC 4028) is a TODO skeleton in sip_sess_timer.c —
         * after answer zero SIP traffic is exchanged → middle proxy
         * (Nginx/HAProxy idle timeout 60-300s) silently drops WSS TCP →
         * A leg BYE later finds B leg WSS dead → B leg never gets BYE.
         * Ping every 30s keeps proxy table entries alive and detects
         * dead proxies/peers quickly. */
        {
            uint64_t now = ws_now_ms();
            pthread_mutex_lock(&g_ws.conn_mtx);
            ws_conn_t *c = g_ws.conn_head;
            while (c) {
                ws_conn_t *next = c->next;  /* snapshot — c may close below */
                if (!c->closed && c->worker_idx == w->idx) {
                    /* P2-2：握手超时兜底（替代原 SO_RCVTIMEO 的 Slowloris 保护） */
                    if (c->state == CS_TLS_HANDSHAKE &&
                        c->tls_deadline_ms > 0 && now > c->tls_deadline_ms) {
                        llog(LOG_WARN, "ws", "worker[%d] TLS handshake timeout fd=%d",
                             w->idx, c->fd);
                        c->closed = 1;
                        pthread_mutex_unlock(&g_ws.conn_mtx);
                        ws_close_conn(w->epfd, c);
                        pthread_mutex_lock(&g_ws.conn_mtx);
                        c = next;
                        continue;
                    }

                    uint64_t idle = now - c->last_rx_ms;

                    /* 1) Already waiting for pong — check timeout */
                    if (c->last_ping_ms > 0) {
                        uint64_t ping_age = now - c->last_ping_ms;
                        if (ping_age >= WS_PING_TIMEOUT_MS) {
                            c->pongs_missed++;
                            /* 置 0 让下一轮 idle pass 重发 ping（15s 一次），
                             * 否则单个 ping 丢失会连续 3 个周期无探测机会。 */
                            c->last_ping_ms = 0;
                            llog(LOG_DEBUG, "ws",
                                 "worker[%d] pong miss #%d fd=%d",
                                 w->idx, c->pongs_missed, c->fd);
                            if (c->pongs_missed >= WS_PING_MAX_MISSED) {
                                llog(LOG_WARN, "ws",
                                     "worker[%d] pong miss MAX -> close fd=%d",
                                     w->idx, c->fd);
                                c->closed = 1;
                                pthread_mutex_unlock(&g_ws.conn_mtx);
                                ws_close_conn(w->epfd, c);
                                pthread_mutex_lock(&g_ws.conn_mtx);
                            }
                        }
                    }

                    /* 2) Idle long enough — send ping（last_ping_ms==0 表示当前不在等 pong，
                     *    否则每 500ms 会重复灌一个 ping 帧） */
                    if (c->pongs_missed < WS_PING_MAX_MISSED &&
                        c->last_ping_ms == 0 &&
                        idle >= WS_PING_INTERVAL_MS) {
                        uint8_t ping_frame[2];
                        size_t plen = ws_build_ping_frame(ping_frame, sizeof(ping_frame));
                        if (plen > 0) {
                            ws_send_item_t item = {
                                .data = (uint8_t *)malloc(plen),
                                .len  = plen,
                                .sent = 0
                            };
                            if (item.data) {
                                memcpy(item.data, ping_frame, plen);
                                pthread_mutex_lock(&c->send_mtx);
                                if (c->send_q_count < WS_SEND_QUEUE_CAP) {
                                    c->send_q[c->send_q_tail] = item;
                                    c->send_q_tail = (c->send_q_tail + 1) % WS_SEND_QUEUE_CAP;
                                    c->send_q_count++;
                                    c->send_pending = 1;
                                    c->last_ping_ms = now;
                                } else {
                                    free(item.data);
                                }
                                pthread_mutex_unlock(&c->send_mtx);
                                /* ★ 关键：idle pass 不是 epoll 事件回调，仅入队不会触发
                                 * EPOLLOUT；worker 的 epoll_wait 超时分支也不 flush 队列，
                                 * 结果 ping 永远发不出去 → 对端不回 pong → 误判断线。
                                 * 与 ws_conn_send 一致，直接 epoll_ctl MOD 唤醒本 worker。 */
                                if (c->fd >= 0) {
                                    struct epoll_event ee;
                                    ee.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                    ee.data.ptr = c;
                                    (void)epoll_ctl(w->epfd, EPOLL_CTL_MOD, c->fd, &ee);
                                }
                            }
                        }
                    }
                }
                c = next;
            }
            pthread_mutex_unlock(&g_ws.conn_mtx);
        }
    }

    llog(LOG_INFO, "ws", "worker[%d] exiting", w->idx);
    w->running = 0;
    return NULL;
}

/* ==== accept 主线程 ==== */
static void *ws_accept_thread(void *ud)
{
    (void)ud;
    llog(LOG_INFO, "ws", "accept thread started, %s :%u, workers=%d",
         g_ws.tls_enabled ? "WSS" : "WS", g_ws.port, g_ws.worker_count);

    int worker_round_robin = 0;

    for (;;) {
        if (WS_ATOMIC_LOAD(&g_ws.stop_flag)) break;
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int fd = accept4(g_ws.listen_fd, (struct sockaddr *)&cli, &cli_len,
                         SOCK_CLOEXEC | SOCK_NONBLOCK);  /* P2-2：一律非阻塞 */
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (WS_ATOMIC_LOAD(&g_ws.stop_flag)) break;
            llog(LOG_WARN, "ws", "accept errno=%d", errno);
            continue;
        }

        /* 停机中 — accept 返回后立刻丢弃，不再进入耗时的 TLS 握手 */
        if (WS_ATOMIC_LOAD(&g_ws.stop_flag)) { close(fd); break; }

        /* P2-2：TLS 握手改为 worker 内非阻塞状态机（CS_TLS_HANDSHAKE），
         * accept 线程只创建 SSL 对象并挂 epoll，绝不做阻塞 SSL_accept。
         * ssl/rbio 类型：OpenSSL 开时用具体类型获编译期检查，关时用 void* 占位（ws_conn_t 已用 void* 存）*/
#ifdef NSBC_WITH_OPENSSL
        SSL *ssl = NULL;
        BIO *rbio = NULL;
#else
        void *ssl = NULL;
        void *rbio = NULL;
#endif
        void *tls_ctx = __atomic_load_n(&g_ws.tls_ctx, __ATOMIC_ACQUIRE);
        if (g_ws.tls_enabled && tls_ctx) {
#ifdef NSBC_WITH_OPENSSL
            SSL_CTX *ssl_ctx = (SSL_CTX *)tls_ctx_get_ssl(tls_ctx);
            if (ssl_ctx) {
                /* 握手洪水保护：并发握手数达上限直接拒绝（快速失败，不排队） */
                int active = __atomic_load_n(&g_ws.tls_hs_active, __ATOMIC_RELAXED);
                if (active >= WS_TLS_HS_MAX) {
                    llog(LOG_WARN, "ws", "tls handshake slots exhausted (%d) — dropping peer", active);
                    close(fd);
                    continue;
                }
                ssl = SSL_new(ssl_ctx);
                if (!ssl) {
                    close(fd);
                    continue;
                }
                SSL_set_accept_state(ssl);
                rbio = BIO_new_socket(fd, BIO_NOCLOSE);
                if (!rbio) {
                    SSL_free(ssl);
                    close(fd);
                    continue;
                }
                SSL_set_bio(ssl, rbio, rbio);
            }
#endif
        }

        /* 停机中：accept 返回后若已 stop 直接丢弃（不再有阻塞握手等待） */
        if (WS_ATOMIC_LOAD(&g_ws.stop_flag)) {
            if (ssl) {
#ifdef NSBC_WITH_OPENSSL
                SSL_free(ssl);   /* 连带 free BIO */
#endif
            }
            close(fd);
            break;
        }

        /* worker 未就绪（启动失败）— 丢弃 peer，避免后面 % 0 */
        if (g_ws.worker_count <= 0 || !g_ws.workers) {
            if (ssl) {
#ifdef NSBC_WITH_OPENSSL
                SSL_free(ssl);
#endif
            }
            close(fd);
            continue;
        }

        /* fd 已由 accept4 设为非阻塞（TLS 握手交给 worker 状态机推进） */

        /* 分配 conn */
        ws_conn_t *c = (ws_conn_t *)calloc(1, sizeof(*c));
        if (!c) {
            if (ssl) {
#ifdef NSBC_WITH_OPENSSL
                SSL_free(ssl);
#endif
            }
            close(fd);
            continue;
        }
        c->fd = fd;
        c->ssl = ssl;
        c->rbio = rbio;
        /* P2-2：TLS 连接先进握手态，由 worker 非阻塞推进；明文直接进 HTTP upgrade */
        c->state = ssl ? CS_TLS_HANDSHAKE : CS_HANDSHAKE;
        if (ssl) {
            c->tls_deadline_ms = ws_now_ms() + WS_TLS_HS_TIMEOUT_MS;
            __atomic_fetch_add(&g_ws.tls_hs_active, 1, __ATOMIC_RELAXED);
            c->hs_counted = 1;
        }
        pthread_mutex_init(&c->send_mtx, NULL);

        /* Ping/pong init — conn just created, baseline last_rx = now. */
        c->last_rx_ms   = ws_now_ms();
        c->last_ping_ms = 0;
        c->pongs_missed = 0;

        /* ★ P4 fix: 动态 read_buf —— 默认只 malloc WS_READ_BUF + 128B ≈ 16KB，
         * 大 payload 触发 realloc 到 WS_READ_BUF_MAX ≈ 64KB。
         * 几百 WS 连接省 (64-16)KB × 1000 = ~48MB 固定内存。 */
        c->read_buf_cap = WS_READ_BUF + 128;
        c->read_buf = (uint8_t *)malloc(c->read_buf_cap);
        if (!c->read_buf) {
            ws_hs_release(c);
            pthread_mutex_destroy(&c->send_mtx);
            if (ssl) {
#ifdef NSBC_WITH_OPENSSL
                SSL_free(ssl);
#endif
            }
            close(fd);
            free(c);
            continue;
        }
        c->read_pos = 0;
        c->read_len = 0;

        /* 选 worker (round robin) */
        c->worker_idx = worker_round_robin % g_ws.worker_count;
        worker_round_robin++;
        ws_worker_t *w = &g_ws.workers[c->worker_idx];

        /* 挂入全局链表 */
        pthread_mutex_lock(&g_ws.conn_mtx);
        c->next = g_ws.conn_head;
        g_ws.conn_head = c;
        WS_ATOMIC_FETCH_ADD(&g_ws.conn_count, 1);
        pthread_mutex_unlock(&g_ws.conn_mtx);

        /* epoll_ctl ADD 到 worker 的 epfd（TLS 握手需要读写双事件） */
        struct epoll_event ee;
        ee.events = (uint32_t)(EPOLLIN | EPOLLRDHUP | (ssl ? EPOLLOUT : 0));
        ee.data.ptr = c;
        if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, fd, &ee) != 0) {
            llog(LOG_WARN, "ws", "epoll_ctl ADD fd=%d errno=%d", fd, errno);
            pthread_mutex_lock(&g_ws.conn_mtx);
            ws_conn_t **pp = &g_ws.conn_head;
            while (*pp) {
                if (*pp == c) { *pp = c->next; WS_ATOMIC_FETCH_SUB(&g_ws.conn_count, 1); break; }
                pp = &(*pp)->next;
            }
            pthread_mutex_unlock(&g_ws.conn_mtx);
            ws_hs_release(c);
            if (ssl) {
#ifdef NSBC_WITH_OPENSSL
                SSL_free(ssl);
#endif
            }
            pthread_mutex_destroy(&c->send_mtx);
            free(c);
            close(fd);
            continue;
        }

        /* 唤醒 worker */
        uint64_t kick = 1;
        write(w->wakeup_fd, &kick, sizeof(kick));
    }

    llog(LOG_INFO, "ws", "accept thread stopped");
    return NULL;
}

/* ==== 公开 API ==== */

int ws_listener_init(uint16_t port, int tls_enabled, int enabled,
                     void *tls_ctx, ws_rx_fn rx, void *rx_ud)
{
    g_ws.port = port;
    g_ws.tls_enabled = tls_enabled;
    g_ws.enabled = enabled;
    g_ws.tls_ctx = tls_ctx;
    g_ws.rx_fn = rx;
    g_ws.rx_ud = rx_ud;
    g_ws.listen_fd = -1;
    g_ws.conn_head = NULL;
    WS_ATOMIC_STORE(&g_ws.conn_count, 0);
    g_ws.worker_count = 0;
    g_ws.workers = NULL;
    pthread_mutex_init(&g_ws.conn_mtx, NULL);
    return NSBC_OK;
}

void ws_listener_set_close_cb(ws_close_fn cb, void *ud)
{
    g_ws.close_fn = cb;
    g_ws.close_ud = ud;
}

/* TLS 证书热重载：nsbc-web 更新 cert.pem 后 kill SIGHUP → nsbcd reload → 调此函数 */
void ws_listener_set_tls_ctx(void *tls_ctx)
{
    __atomic_store_n(&g_ws.tls_ctx, tls_ctx, __ATOMIC_RELEASE);
    llog(LOG_INFO, "ws", "tls_ctx updated for hot reload");
}

/* CPU 核数探测（linux 可用 /proc/cpuinfo 或 sysconf）*/
static int cpu_count(void)
{
#ifdef __linux__
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (int)n;
#endif
    return 4;  /* fallback */
}

int ws_listener_start(void)
{
    if (!g_ws.enabled) return NSBC_OK;
    if (g_ws.accept_started) return NSBC_OK;

#ifdef NSBC_WITH_OPENSSL
    if (g_ws.tls_enabled && !g_ws.tls_ctx) {
        llog(LOG_ERR, "ws", "tls enabled but tls_ctx is NULL");
        return NSBC_ERR;
    }
#else
    if (g_ws.tls_enabled) {
        llog(LOG_WARN, "ws", "tls requested but NSBC_WITH_OPENSSL not set, fallback to plain WS");
        g_ws.tls_enabled = 0;
    }
#endif

    /* 1. 创建 listen fd */
    g_ws.listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_ws.listen_fd < 0) {
        llog(LOG_ERR, "ws", "socket() errno=%d", errno);
        return NSBC_ERR;
    }
    int one = 1;
    setsockopt(g_ws.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    srv.sin_port = htons(g_ws.port);

    if (bind(g_ws.listen_fd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        llog(LOG_ERR, "ws", "bind :%u errno=%d (in use?)", g_ws.port, errno);
        close(g_ws.listen_fd);
        g_ws.listen_fd = -1;
        return NSBC_ERR;
    }
    if (listen(g_ws.listen_fd, 1024) < 0) {  /* backlog 改 1024（大并发）*/
        llog(LOG_ERR, "ws", "listen() errno=%d", errno);
        close(g_ws.listen_fd);
        g_ws.listen_fd = -1;
        return NSBC_ERR;
    }

    /* 2. 创建 worker 线程池 */
    g_ws.worker_count = cpu_count();
    if (g_ws.worker_count < 2) g_ws.worker_count = 2;
    if (g_ws.worker_count > 16) g_ws.worker_count = 16;
    g_ws.workers = (ws_worker_t *)calloc((size_t)g_ws.worker_count, sizeof(ws_worker_t));
    if (!g_ws.workers) {
        close(g_ws.listen_fd);
        g_ws.listen_fd = -1;
        return NSBC_ERR;
    }

    for (int i = 0; i < g_ws.worker_count; i++) {
        g_ws.workers[i].idx = i;
        g_ws.workers[i].epfd = epoll_create1(EPOLL_CLOEXEC);
        if (g_ws.workers[i].epfd < 0) {
            llog(LOG_ERR, "ws", "epoll_create1[%d] errno=%d", i, errno);
            return NSBC_ERR;
        }
        g_ws.workers[i].wakeup_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (g_ws.workers[i].wakeup_fd < 0) {
            llog(LOG_ERR, "ws", "eventfd[%d] errno=%d", i, errno);
            return NSBC_ERR;
        }
        /* wakeup_fd 自己也要加进 epoll（用于主线程 handoff 唤醒）*/
        struct epoll_event we;
        we.events = EPOLLIN;
        we.data.fd = g_ws.workers[i].wakeup_fd;
        epoll_ctl(g_ws.workers[i].epfd, EPOLL_CTL_ADD,
                  g_ws.workers[i].wakeup_fd, &we);

        int rc = pthread_create(&g_ws.workers[i].tid, NULL, ws_worker_thread,
                                &g_ws.workers[i]);
        if (rc != 0) {
            llog(LOG_ERR, "ws", "pthread_create worker[%d] rc=%d", i, rc);
            return NSBC_ERR;
        }
    }

    /* 3. accept 线程 */
    WS_ATOMIC_STORE(&g_ws.stop_flag, 0);
    int rc = pthread_create(&g_ws.accept_tid, NULL, ws_accept_thread, NULL);
    if (rc != 0) {
        llog(LOG_ERR, "ws", "pthread_create accept rc=%d", rc);
        close(g_ws.listen_fd);
        g_ws.listen_fd = -1;
        return NSBC_ERR;
    }
    g_ws.accept_started = 1;
    llog(LOG_NOTICE, "ws", "listener bound %s :%u (workers=%d)",
         g_ws.tls_enabled ? "WSS" : "WS", g_ws.port, g_ws.worker_count);
    return NSBC_OK;
}

void ws_listener_stop(void)
{
    if (!g_ws.accept_started) return;

    WS_ATOMIC_STORE(&g_ws.stop_flag, 1);

    /* ---- 先关监听 fd 并 join accept 线程 ----
     * P2-2 后 accept 线程不再阻塞在 TLS 握手，join 应在毫秒级返回；
     * 但仍须先停 accept 再释放 workers，否则 accept 可能向已释放的
     * workers 数组投递连接（worker_round_robin % 0 或 UAF）。*/
    uint64_t stop_t0 = ws_now_ms();
    if (g_ws.listen_fd >= 0) {
        shutdown(g_ws.listen_fd, SHUT_RDWR);
        close(g_ws.listen_fd);
        g_ws.listen_fd = -1;
    }
    pthread_join(g_ws.accept_tid, NULL);
    g_ws.accept_started = 0;
    llog(LOG_INFO, "ws", "accept thread joined in %llu ms",
         (unsigned long long)(ws_now_ms() - stop_t0));

    /* ---- accept 已停，安全停止并释放 worker ---- */
    if (g_ws.workers) {
        for (int i = 0; i < g_ws.worker_count; i++) {
            uint64_t kick = 1;
            write(g_ws.workers[i].wakeup_fd, &kick, sizeof(kick));
        }
        for (int i = 0; i < g_ws.worker_count; i++) {
            pthread_join(g_ws.workers[i].tid, NULL);
            close(g_ws.workers[i].epfd);
            close(g_ws.workers[i].wakeup_fd);
        }
        free(g_ws.workers);
        g_ws.workers = NULL;
        g_ws.worker_count = 0;
    }

    /* ---- 关闭所有活跃连接（标记 closed + shutdown fd）---- */
    pthread_mutex_lock(&g_ws.conn_mtx);
    ws_conn_t *c = g_ws.conn_head;
    while (c) {
        ws_conn_t *next = c->next;
        c->closed = 1;
        ws_hs_release(c);   /* P2-2：停机归还握手槽，避免下次 init 计数残留 */
        if (c->fd >= 0) shutdown(c->fd, SHUT_RDWR);
        c = next;
    }
    pthread_mutex_unlock(&g_ws.conn_mtx);
}

void ws_listener_shutdown(void)
{
    ws_listener_stop();

    /* 全部 free（包括 closed 的 conn — shutdown 时没人持有 registrar 指针了）*/
    pthread_mutex_lock(&g_ws.conn_mtx);
    ws_conn_t *c = g_ws.conn_head;
    while (c) {
        ws_conn_t *next = c->next;
        if (c->read_buf) { free(c->read_buf); c->read_buf = NULL; }
        ws_hs_release(c);   /* P2-2 */
        if (c->ssl) {
#ifdef NSBC_WITH_OPENSSL
            SSL_free((SSL *)c->ssl);   /* 连带 free BIO — 不要 double free */
#endif
        }
        if (c->fd >= 0) close(c->fd);
        pthread_mutex_destroy(&c->send_mtx);
        free(c);
        c = next;
    }
    g_ws.conn_head = NULL;
    WS_ATOMIC_STORE(&g_ws.conn_count, 0);
    pthread_mutex_unlock(&g_ws.conn_mtx);
    pthread_mutex_destroy(&g_ws.conn_mtx);
}

/* ==== ws_conn_send — 公开（signaling/b2bua 调用）==== */
int ws_conn_send(void *conn, const char *buf, size_t len)
{
    ws_conn_t *c = (ws_conn_t *)conn;
    if (!c || c->closed) return -1;

    /* 构建 WS frame header (unmasked, 服务端发客户端) */
    uint8_t frame_hdr[10];
    size_t hdr_len = 0;
    frame_hdr[hdr_len++] = 0x80 | WS_OP_TEXT;   /* FIN=1, opcode=TEXT (RFC 7118 §8.1) */
    frame_hdr[hdr_len++] = 0x00;                 /* MASK=0, len placeholder */
    uint64_t plen = len;
    if (plen <= 125) {
        frame_hdr[1] = (uint8_t)plen;
    } else if (plen <= 0xFFFF) {
        frame_hdr[1] = 126;
        frame_hdr[hdr_len++] = (uint8_t)(plen >> 8);
        frame_hdr[hdr_len++] = (uint8_t)(plen & 0xff);
    } else {
        frame_hdr[1] = 127;
        for (int i = 7; i >= 0; i--)
            frame_hdr[hdr_len++] = (uint8_t)((plen >> (i * 8)) & 0xff);
    }

    /* 合并 header + payload 为一个 send item */
    size_t total = hdr_len + len;
    uint8_t *frame = (uint8_t *)malloc(total);
    if (!frame) return -1;
    memcpy(frame, frame_hdr, hdr_len);
    if (len > 0) memcpy(frame + hdr_len, buf, len);

    /* 进发送队列 */
    pthread_mutex_lock(&c->send_mtx);
    if (c->send_q_count >= WS_SEND_QUEUE_CAP) {
        pthread_mutex_unlock(&c->send_mtx);
        free(frame);
        return -1;   /* 队列满 — 反压 */
    }
    ws_send_item_t *item = &c->send_q[c->send_q_tail];
    item->data = frame;
    item->len = total;
    item->sent = 0;
    c->send_q_tail = (c->send_q_tail + 1) % WS_SEND_QUEUE_CAP;
    c->send_q_count++;
    pthread_mutex_unlock(&c->send_mtx);

    /* 直接 epoll_ctl MOD 加 EPOLLOUT — epoll_ctl 线程安全，任何线程都能调
     * 这样 worker 的 epoll_wait 立刻返回该 conn 的 EPOLLOUT 事件，flush send queue
     * （之前的 send_pending+kick eventfd 方案有 gap：conn 无 epoll 事件时 worker 不会检查）*/
    if (c->worker_idx >= 0 && g_ws.workers && c->fd >= 0) {
        ws_worker_t *w = &g_ws.workers[c->worker_idx];
        struct epoll_event ee;
        ee.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
        ee.data.ptr = c;
        /* ENOENT = conn 刚关了，正常忽略 */
        (void)epoll_ctl(w->epfd, EPOLL_CTL_MOD, c->fd, &ee);
    }
    return 0;
}

/* ==== ws_conn_alive — 公开（registrar 用）==== */
int ws_conn_alive(void *conn)
{
    if (!conn) return 0;
    ws_conn_t *c = (ws_conn_t *)conn;
    pthread_mutex_lock(&g_ws.conn_mtx);
    int alive = 0;
    for (ws_conn_t *p = g_ws.conn_head; p; p = p->next) {
        if (p == c) { alive = !p->closed; break; }
    }
    pthread_mutex_unlock(&g_ws.conn_mtx);
    return alive;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
