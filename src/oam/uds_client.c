/* NSBC Data Service — C 侧 UDS Client（REQ-DATA-SVC-002 + S2 命令面）
 *
 * 连接 data.sock（Web 端 uds_server.rs 为 SERVER，本端 CLIENT）：
 *   - PUSH：CDR/注册/运行时状态事件（ring 聚合，后台线程发）
 *   - HELLO：连接建立时上报版本/主机/PID → Web 触发全量配置推送
 *   - CMD_REQ/CMD_RSP：uds_request() 同步查询（S6 注册鉴权兜底用）
 *
 * 帧格式（12B 固定头，与 web/src/uds_server.rs 完全对齐）：
 *   magic(1) + type(1) + total_len(4 NBO) + ticket(4 NBO) + flags(1) + rsvd(1)
 *
 * 线程模型：
 *   - 后台线程 uds_client_thread：poll(socket + wake_pipe, 200ms)
 *       socket 可读 → 帧重组 → RSP 按票投递等待槽 / HELLO_ACK 记日志
 *       ring 有数据 → 组装 PUSH 帧 → send
 *   - 业务线程：uds_send_*() 写 ring；uds_request() 占槽+send+cond 等待
 *
 * 同步原语：
 *   g.lock      —— ring 生产/消费互斥
 *   g.send_lock —— 对 g.fd 的 send/close/赋值全串行（跨线程 fd 竞态收口）
 *   slot->mtx/cv —— 每个等待槽独立（16 槽并发上限）
 *   wake_pipe   —— ring_put/stop 唤醒后台线程立即处理（替代 cond 轮询）
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "uDS_client.h"
#include "log.h"
#include "nsbc_common.h"
#include "metric_reg.h"
#include <cjson/cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ========================================================================
 * 全局状态（单进程单实例）
 * ======================================================================*/

/* ring buffer 槽：每个槽存一条待推 JSON */
typedef struct {
    char json[8192];    /* 单条事件 JSON 上限 8KB */
    char kind[16];      /* "cdr" / "register" / "runtime" / "audit" */
} uds_slot_t;

#define UDS_RING_SIZE 1024  /* 聚合窗 1024 条，足够 1000 erlang 峰值 */

/* 同步请求等待槽（S2） */
typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    int      in_use;    /* 1=某业务线程占用等待中 */
    int      done;      /* 1=RSP 已填入 */
    uint32_t ticket;
    char     resp[8192];
    size_t   resp_len;
} uds_reqslot_t;

typedef struct {
    pthread_mutex_t lock;      /* ring 互斥 */
    pthread_mutex_t send_lock; /* fd send/close 串行 */
    pthread_t       thread;
    int             started;
    int             stop_flag;

    int             fd;       /* 仅在 send_lock 内改；主线程 poll 前读快照 */
    char            path[128];

    /* wake pipe（业务线程 → 后台线程立即处理） */
    int             wake_rd;
    int             wake_wr;

    /* ring buffer */
    uds_slot_t      ring[UDS_RING_SIZE];
    unsigned        head;    /* 写侧：下一个写位置 */
    unsigned        tail;    /* 读侧：下一个读位置 */
    unsigned        count;   /* 当前 ring 内有效条数（<= UDS_RING_SIZE） */

    /* 重连退避 */
    int             backoff_ms;

    /* 收帧重组（仅后台线程访问） */
    uint8_t         rbuf[NSBC_FRAME_HDR_LEN + 65536];
    size_t          rhave;

    /* 同步请求 */
    uds_reqslot_t   slots[UDS_REQ_SLOTS];
    volatile uint32_t ticket_seq;

    /* 溢出丢弃计数（ring_put full 时 +1，暴露到 control.sock stats） */
    volatile uint32_t ring_dropped;
} uds_state_t;

static uds_state_t g = {0};

/* ========================================================================
 * 帧编码：12B 固定头 + payload（NBO JSON）
 * ======================================================================*/

size_t uds_encode_frame(uint8_t frame_type, uint32_t ticket,
                        const char *payload, size_t payload_len,
                        uint8_t *out, size_t out_sz)
{
    if (!payload || payload_len == 0 || !out) return 0;
    if (payload_len > (size_t)NSBC_MAX_PAYLOAD) return 0;
    size_t frame_sz = NSBC_FRAME_HDR_LEN + payload_len;
    if (frame_sz > out_sz) return 0;

    uint8_t *p = out;
    *p++ = NSBC_FRAME_MAGIC;
    *p++ = frame_type;
    uint32_t total_len = (uint32_t)frame_sz;
    *p++ = (uint8_t)((total_len >> 24) & 0xff);
    *p++ = (uint8_t)((total_len >> 16) & 0xff);
    *p++ = (uint8_t)((total_len >> 8) & 0xff);
    *p++ = (uint8_t)(total_len & 0xff);
    *p++ = (uint8_t)((ticket >> 24) & 0xff);
    *p++ = (uint8_t)((ticket >> 16) & 0xff);
    *p++ = (uint8_t)((ticket >> 8) & 0xff);
    *p++ = (uint8_t)(ticket & 0xff);
    *p++ = 0;   /* flags */
    *p++ = 0;   /* rsvd */
    memcpy(p, payload, payload_len);
    return frame_sz;
}

/* ========================================================================
 * 连接 / 重连 / 发送原语
 * ======================================================================*/

static int uds_connect_nonblocking(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", g.path);
    int rc = connect(fd, (struct sockaddr *)&un, sizeof(un));
    if (rc == 0) return fd;      /* 同步连接成功（同机 Unix socket 常见） */
    if (errno == EINPROGRESS) return fd;
    close(fd);
    return -1;
}

/* EINPROGRESS 收尾：poll POLLOUT + SO_ERROR 确认连接真正建立 */
static int uds_wait_connected(int fd)
{
    struct pollfd p = { fd, POLLOUT, 0 };
    if (poll(&p, 1, 500) <= 0)
        return -1;
    if (!(p.revents & POLLOUT))
        return -1;
    int err = 0;
    socklen_t el = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0)
        return -1;
    return 0;
}

/* send_lock 内：整帧写出。失败返回 -1（连接由调用方标记断开） */
static int send_all_locked(const uint8_t *buf, size_t len)
{
    size_t w = 0;
    while (w < len) {
        ssize_t wr = send(g.fd, buf + w, len - w, MSG_NOSIGNAL);
        if (wr <= 0) {
            if (wr < 0 && (errno == EINTR)) continue;
            return -1;
        }
        w += (size_t)wr;
    }
    return 0;
}

/* 标记连接断开（send_lock 内调用）：close+置 -1，等待槽靠超时自然回收 */
static void mark_broken_locked(void)
{
    if (g.fd >= 0) {
        close(g.fd);
        g.fd = -1;
    }
    g.rhave = 0;
}

/* 连接建立点：发 FT_HELLO（Web 端收到后回 HELLO_ACK 并触发全量推送） */
static void send_hello(void)
{
    char host[128] = "unknown";
    gethostname(host, sizeof(host) - 1);
    char payload[256];
    int n = snprintf(payload, sizeof(payload),
                     "{\"version\":1,\"hostname\":\"%s\",\"pid\":%d}",
                     host, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(payload))
        return;
    uint8_t frame[NSBC_FRAME_HDR_LEN + 256];
    size_t fsz = uds_encode_frame(FT_HELLO, 0, payload, (size_t)n,
                                  frame, sizeof(frame));
    if (fsz == 0)
        return;
    pthread_mutex_lock(&g.send_lock);
    if (g.fd >= 0 && send_all_locked(frame, fsz) != 0)
        mark_broken_locked();
    pthread_mutex_unlock(&g.send_lock);
}

/* ========================================================================
 * 收帧分发（仅后台线程）
 * ======================================================================*/

static void dispatch_rsp(uint32_t ticket, const uint8_t *payload, size_t len)
{
    for (int i = 0; i < UDS_REQ_SLOTS; ++i) {
        uds_reqslot_t *s = &g.slots[i];
        pthread_mutex_lock(&s->mtx);
        if (s->in_use && s->ticket == ticket) {
            size_t n = len < sizeof(s->resp) ? len : sizeof(s->resp) - 1;
            memcpy(s->resp, payload, n);
            s->resp[n] = '\0';
            s->resp_len = n;
            s->done = 1;
            pthread_cond_signal(&s->cv);
            pthread_mutex_unlock(&s->mtx);
            return;
        }
        pthread_mutex_unlock(&s->mtx);
    }
    /* 无匹配槽：迟到 RSP（请求方已超时离开），丢弃 */
    llog(LOG_DEBUG, "uds", "rsp ticket=%u unmatched, dropped", ticket);
}

static void dispatch_frame(const uint8_t *frame, size_t len)
{
    if (len < NSBC_FRAME_HDR_LEN || frame[0] != NSBC_FRAME_MAGIC)
        return;
    uint8_t ftype = frame[1];
    uint32_t ticket = ((uint32_t)frame[6] << 24) | ((uint32_t)frame[7] << 16) |
                      ((uint32_t)frame[8] << 8) | (uint32_t)frame[9];
    const uint8_t *payload = frame + NSBC_FRAME_HDR_LEN;
    size_t plen = len - NSBC_FRAME_HDR_LEN;

    switch (ftype) {
    case FT_CMD_RSP:
        dispatch_rsp(ticket, payload, plen);
        break;
    case FT_HELLO_ACK:
        llog(LOG_NOTICE, "uds", "web hello_ack received (link up)");
        break;
    case FT_ACK:
    case FT_HEARTBEAT:
        break; /* 当前无状态依赖，静默 */
    default:
        llog(LOG_DEBUG, "uds", "ignore frame type=0x%02x len=%zu", ftype, len);
        break;
    }
}

/* 非阻塞收帧重组。返回 0=正常（读尽或消费完）；-1=连接断开/协议错乱 */
static int recv_and_dispatch(void)
{
    for (;;) {
        /* 消费 rbuf 中所有完整帧 */
        while (g.rhave >= NSBC_FRAME_HDR_LEN) {
            uint32_t total =
                ((uint32_t)g.rbuf[2] << 24) | ((uint32_t)g.rbuf[3] << 16) |
                ((uint32_t)g.rbuf[4] << 8) | (uint32_t)g.rbuf[5];
            if (total < NSBC_FRAME_HDR_LEN || total > sizeof(g.rbuf)) {
                llog(LOG_WARN, "uds", "frame len %u invalid, reset link", total);
                return -1;
            }
            if (g.rhave < total)
                break; /* 半帧，继续收 */
            dispatch_frame(g.rbuf, total);
            memmove(g.rbuf, g.rbuf + total, g.rhave - total);
            g.rhave -= total;
        }
        if (g.rhave == sizeof(g.rbuf))
            return -1; /* 头声明不足一帧却塞满缓冲：错乱 */

        ssize_t r = recv(g.fd, g.rbuf + g.rhave, sizeof(g.rbuf) - g.rhave, 0);
        if (r > 0) {
            g.rhave += (size_t)r;
            continue;
        }
        if (r == 0)
            return -1; /* 对端关闭 */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;  /* 读尽 */
        if (errno == EINTR)
            continue;
        return -1;
    }
}

/* ========================================================================
 * 后台线程：connect → poll(socket+wake) → 收帧分发 / ring → PUSH
 * ======================================================================*/

/* 从 ring 取一条并发送 PUSH 帧（每轮一条，保持响应性） */
static void flush_ring_one(void)
{
    uds_slot_t slot;
    pthread_mutex_lock(&g.lock);
    if (g.count == 0) {
        pthread_mutex_unlock(&g.lock);
        return;
    }
    slot = g.ring[g.tail]; /* 拷贝出锁（slot 8KB 栈拷贝） */
    g.tail = (g.tail + 1) % UDS_RING_SIZE;
    g.count--;
    pthread_mutex_unlock(&g.lock);

    /* cJSON 包装 {"kind":"..","payload":{...}}（Web uds_server.rs 口径） */
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return;
    cJSON_AddStringToObject(root, "kind", slot.kind);
    cJSON *parsed = cJSON_Parse(slot.json);
    if (parsed)
        cJSON_AddItemToObject(root, "payload", parsed);
    else
        cJSON_AddStringToObject(root, "payload", slot.json);
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str)
        return;

    uint8_t frame[NSBC_FRAME_HDR_LEN + 16384];
    size_t n = strlen(json_str);
    size_t fsz = (n <= 16384)
                     ? uds_encode_frame(FT_DATA_PUSH, 0, json_str, n,
                                        frame, sizeof(frame))
                     : 0;
    cJSON_free(json_str);
    if (fsz > 0) {
        pthread_mutex_lock(&g.send_lock);
        if (g.fd >= 0 && send_all_locked(frame, fsz) != 0)
            mark_broken_locked();
        pthread_mutex_unlock(&g.send_lock);
    } else {
        llog(LOG_WARN, "uds", "push frame too large (%zuB), dropped", n);
    }
}

static void drain_wake_pipe(void)
{
    char buf[64];
    while (read(g.wake_rd, buf, sizeof(buf)) > 0)
        ;
}

static void *uds_client_thread(void *ud)
{
    (void)ud;
    g.backoff_ms = UDS_RECONNECT_BASE_MS;

    for (;;) {
        if (g.stop_flag)
            break;

        /* ---- 未连接：connect（成功点发 HELLO）---- */
        if (g.fd < 0) {
            int fd = uds_connect_nonblocking();
            if (fd < 0 || uds_wait_connected(fd) != 0) {
                if (fd >= 0)
                    close(fd);
                if (g.backoff_ms > UDS_RECONNECT_MAX_MS)
                    g.backoff_ms = UDS_RECONNECT_MAX_MS;
                llog(LOG_WARN, "uds",
                     "connect %s failed, retry in %dms errno=%d",
                     g.path, g.backoff_ms, errno);
                usleep((useconds_t)g.backoff_ms * 1000);
                g.backoff_ms *= 2;
                continue;
            }
            g.rhave = 0;
            pthread_mutex_lock(&g.send_lock);
            g.fd = fd;
            pthread_mutex_unlock(&g.send_lock);
            g.backoff_ms = UDS_RECONNECT_BASE_MS;
            llog(LOG_NOTICE, "uds", "connected to %s, sending hello", g.path);
            send_hello();
        }

        /* ---- poll 双源：socket 收帧 + wake pipe 立即处理 ---- */
        int sfd = g.fd;
        struct pollfd pfd[2];
        pfd[0].fd = sfd;       /* sfd<0 时 poll 忽略该项 */
        pfd[0].events = POLLIN;
        pfd[1].fd = g.wake_rd;
        pfd[1].events = POLLIN;
        int pr = poll(pfd, 2, 200);
        if (g.stop_flag)
            break;
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            pthread_mutex_lock(&g.send_lock);
            mark_broken_locked();
            pthread_mutex_unlock(&g.send_lock);
            continue;
        }

        if (sfd >= 0 && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (recv_and_dispatch() != 0) {
                pthread_mutex_lock(&g.send_lock);
                mark_broken_locked();
                pthread_mutex_unlock(&g.send_lock);
                llog(LOG_WARN, "uds", "link reset by peer, reconnecting");
                continue;
            }
        }
        if (pfd[1].revents & POLLIN)
            drain_wake_pipe();

        /* ---- ring → PUSH ---- */
        flush_ring_one();
    }

    /* stop：尽力 flush 剩余（连接已断则丢弃） */
    pthread_mutex_lock(&g.lock);
    int remain = (int)g.count;
    pthread_mutex_unlock(&g.lock);
    while (remain-- > 0 && g.fd >= 0)
        flush_ring_one();

    pthread_mutex_lock(&g.send_lock);
    if (g.fd >= 0) {
        close(g.fd);
        g.fd = -1;
    }
    pthread_mutex_unlock(&g.send_lock);
    return NULL;
}

/* ========================================================================
 * 推送 API（业务线程调，加锁写 ring + wake）
 * ======================================================================*/

static void wake_thread(void)
{
    char c = 1;
    ssize_t r = write(g.wake_wr, &c, 1);
    (void)r; /* pipe 满则依赖 200ms poll 超时兜底 */
}

static int ring_put(const char *kind, const char *json)
{
    if (!kind || !json) return NSBC_INVAL;
    pthread_mutex_lock(&g.lock);
    if (g.count >= UDS_RING_SIZE) {
        llog(LOG_WARN, "uds", "ring full, dropping oldest");
        g.tail = (g.tail + 1) % UDS_RING_SIZE;
        g.count--;
        __atomic_add_fetch(&g.ring_dropped, 1, __ATOMIC_RELAXED);
    }
    uds_slot_t *slot = &g.ring[g.head];
    snprintf(slot->kind, sizeof(slot->kind), "%s", kind);
    snprintf(slot->json, sizeof(slot->json), "%s", json);
    g.head = (g.head + 1) % UDS_RING_SIZE;
    g.count++;
    pthread_mutex_unlock(&g.lock);
    wake_thread();
    return NSBC_OK;
}

int uds_send_cdr(const char *json)       { return ring_put("cdr",      json); }
int uds_send_register(const char *json)  { return ring_put("register", json); }
int uds_send_runtime(const char *json)   { return ring_put("runtime",  json); }
int uds_send_audit(const char *json)     { return ring_put("audit",    json); }
int uds_send_session(const char *json)   { return ring_put("session",  json); }
int uds_send_vad(const char *json)       { return ring_put("vad_event",json); }

int uds_flush_now(void)
{
    wake_thread();
    return NSBC_OK;
}

/* ========================================================================
 * 同步命令请求（S2）：占槽 → send → cond 等待 RSP
 * ======================================================================*/

int uds_request(const char *cmd_json, int timeout_ms,
                char *resp, size_t resp_cap)
{
    if (!cmd_json || !resp || resp_cap == 0)
        return NSBC_INVAL;
    resp[0] = '\0';
    if (!g.started || g.fd < 0)
        return NSBC_ERR;
    if (timeout_ms <= 0 || timeout_ms > UDS_REQ_TIMEOUT_MS)
        timeout_ms = UDS_REQ_TIMEOUT_MS;

    /* 1. 找空闲槽（trylock 语义：短临界区，不等待其它请求线程） */
    uds_reqslot_t *slot = NULL;
    for (int i = 0; i < UDS_REQ_SLOTS && !slot; ++i) {
        pthread_mutex_lock(&g.slots[i].mtx);
        if (!g.slots[i].in_use) {
            g.slots[i].in_use = 1;
            slot = &g.slots[i];
        }
        pthread_mutex_unlock(&g.slots[i].mtx);
    }
    if (!slot) {
        llog(LOG_WARN, "uds", "request slots full (%d)", UDS_REQ_SLOTS);
        return NSBC_ERR;
    }

    uint32_t ticket = __sync_add_and_fetch(&g.ticket_seq, 1);
    slot->ticket = ticket;
    slot->done = 0;
    slot->resp_len = 0;

    uint8_t frame[2048];
    size_t plen = strlen(cmd_json);
    size_t fsz = (plen > 0 && plen <= sizeof(frame) - NSBC_FRAME_HDR_LEN)
                     ? uds_encode_frame(FT_CMD_REQ, ticket, cmd_json, plen,
                                        frame, sizeof(frame))
                     : 0;
    if (fsz == 0) {
        pthread_mutex_lock(&slot->mtx);
        slot->in_use = 0;
        pthread_mutex_unlock(&slot->mtx);
        return NSBC_ERR;
    }

    /* 2. 发送（send_lock 与 PUSH/HELLO 串行） */
    pthread_mutex_lock(&g.send_lock);
    int sfailed = (g.fd < 0 || send_all_locked(frame, fsz) != 0);
    pthread_mutex_unlock(&g.send_lock);

    int rc = NSBC_ERR;
    if (!sfailed) {
        /* 3. 等 RSP（CLOCK_REALTIME 绝对超时，与 cond_timedwait 时钟一致） */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_mutex_lock(&slot->mtx);
        while (!slot->done) {
            if (pthread_cond_timedwait(&slot->cv, &slot->mtx, &ts) == ETIMEDOUT)
                break;
        }
        if (slot->done) {
            size_t n = slot->resp_len;
            if (n >= resp_cap)
                n = resp_cap - 1;
            memcpy(resp, slot->resp, n);
            resp[n] = '\0';
            rc = NSBC_OK;
        }
        slot->in_use = 0; /* 释放槽（含超时路径；迟到 RSP 自然落空） */
        pthread_mutex_unlock(&slot->mtx);
        return rc;
    }

    pthread_mutex_lock(&slot->mtx);
    slot->in_use = 0;
    pthread_mutex_unlock(&slot->mtx);
    return NSBC_ERR;
}

/* ========================================================================
 * 生命周期
 * ======================================================================*/

/* OBS 自埋点：UDS ring overflow render —— ring_put full 时丢弃的条数 */
static int r_uds_ring(char *b, size_t cap, void *ud)
{
    (void)ud;
    uint32_t dropped = __atomic_load_n(&g.ring_dropped, __ATOMIC_RELAXED);
    return snprintf(b, cap, "nsbc_uds_ring_dropped %u\n", dropped);
}

int uds_client_init(const char *sock_path)
{
    const char *p = sock_path ? sock_path : uds_default_sock_path();
    if (strlen(p) >= sizeof(g.path)) return NSBC_INVAL;
    snprintf(g.path, sizeof(g.path), "%s", p);
    pthread_mutex_init(&g.lock, NULL);
    pthread_mutex_init(&g.send_lock, NULL);

    int wfd[2];
    if (pipe2(wfd, O_CLOEXEC | O_NONBLOCK) != 0) {
        pthread_mutex_destroy(&g.lock);
        pthread_mutex_destroy(&g.send_lock);
        return NSBC_ERR;
    }
    g.wake_rd = wfd[0];
    g.wake_wr = wfd[1];

    for (int i = 0; i < UDS_REQ_SLOTS; ++i) {
        pthread_mutex_init(&g.slots[i].mtx, NULL);
        pthread_cond_init(&g.slots[i].cv, NULL);
    }
    g.fd = -1;
    g.ticket_seq = 0;

    /* 注册 UDS ring overflow metric（暴露到 control.sock stats + /metrics） */
    metric_source_register("nsbc_uds_ring_dropped",
                           "UDS ring buffer overflow drops (oldest evicted)",
                           r_uds_ring, NULL);
    return NSBC_OK;
}

int uds_client_start(void)
{
    if (g.started) return NSBC_OK;
    g.stop_flag = 0;
    int rc = pthread_create(&g.thread, NULL, uds_client_thread, NULL);
    if (rc != 0) return NSBC_ERR;
    g.started = 1;
    llog(LOG_NOTICE, "uds", "client thread started, target %s", g.path);
    return NSBC_OK;
}

void uds_client_stop(void)
{
    if (!g.started) return;
    pthread_mutex_lock(&g.lock);
    g.stop_flag = 1;
    pthread_mutex_unlock(&g.lock);
    wake_thread();
    pthread_join(g.thread, NULL);
    g.started = 0;
    llog(LOG_NOTICE, "uds", "client thread stopped");
}

void uds_client_shutdown(void)
{
    uds_client_stop();
    pthread_mutex_destroy(&g.lock);
    pthread_mutex_destroy(&g.send_lock);
    for (int i = 0; i < UDS_REQ_SLOTS; ++i) {
        pthread_mutex_destroy(&g.slots[i].mtx);
        pthread_cond_destroy(&g.slots[i].cv);
    }
    if (g.wake_rd >= 0) { close(g.wake_rd); g.wake_rd = -1; }
    if (g.wake_wr >= 0) { close(g.wake_wr); g.wake_wr = -1; }
    if (g.fd >= 0) { close(g.fd); g.fd = -1; }
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
