/* NSBC Data Service — C 侧 UDS Client（REQ-DATA-SVC-002 P0）
 *
 * 传输层：双 AF_UNIX SOCK_DGRAM（Linux 可靠 datagram，无丢包无乱序，
 * 一帧一个 datagram —— 天然消息边界，无需帧重组 / half-frame 处理）。
 *   data_push.sock — 单向 C→Rust，FT_DATA_PUSH（CDR/register/runtime/audit/session）
 *                    fire-and-forget，不等待 ACK（ring buffer 自身有 overflow 处理）
 *   data_cmd.sock  — 双向，FT_HELLO/HELLO_ACK/CMD_REQ/CMD_RSP
 *                    CMD_REQ 阻塞等待匹配 ticket 的 CMD_RSP（16 槽 cond_wait）
 *
 * 帧格式（12B 固定头，与 web/src/uds_server.rs 完全对齐）：
 *   magic(1) + type(1) + total_len(4 NBO) + ticket(4 NBO) + flags(1) + rsvd(1)
 *   datagram 边界 = 帧边界，每次 recv 返回完整一帧。
 *
 * 线程模型（简化了 stream 时代的帧重组逻辑）：
 *   - 后台线程 uds_client_thread：poll(fd_cmd + fd_push + wake_pipe, 200ms)
 *       fd_cmd 可读 → recv → dispatch_frame（CMD_RSP/HELLO_ACK）
 *       fd_push 可读 → recv → 直接忽略（PUSH 不需要 Rust 回 ACK；Rust 端单独处理）
 *       ring 有数据 → 组装 PUSH 帧 → sendto(fd_push)
 *   - 业务线程：uds_send_*() 写 ring + wake；uds_request() 占槽+send(cmd)+cond 等
 *
 * 同步原语：
 *   g.lock      —— ring 生产/消费互斥
 *   g.send_lock —— 对两个 fd 的 send/close 串行（跨线程 fd 竞态收口）
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
    pthread_mutex_t send_lock; /* 对两个 fd 的 send/close 串行 */
    pthread_t       thread;
    int             started;
    int             stop_flag;

    /* 双 datagram fd —— 都用 connect() 到固定路径，简化为 send/recv */
    int             fd_push;   /* C→Rust PUSH 通道，data_push.sock */
    int             fd_cmd;    /* 双向 CMD/HELLO 通道，data_cmd.sock */

    /* 派生的两个路径（init 时从 base path 生成） */
    char            path_push[128];
    char            path_cmd[128];

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

    /* 同步请求 */
    uds_reqslot_t   slots[UDS_REQ_SLOTS];
    volatile uint32_t ticket_seq;

    /* 上次收到帧时间戳（ms） —— heartbeat 超时检测用 */
    uint64_t        last_recv_ms;

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
 * datagram 连接 / 发送原语
 * ======================================================================*/

/* 打开一个 AF_UNIX SOCK_DGRAM 并 connect 到固定路径。
 * datagram connect() 是立即的（同机路径不存在连接握手）。
 * 返回 fd（≥0），失败返回 -1。 */
static int uds_open_dgram(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    /* datagram 也可以设 SO_RCVBUF/SO_SNDBUF 让 kernel queue 更大 */
    int rcvbuf = 1 << 20; /* 1 MB */
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", sock_path);
    if (connect(fd, (struct sockaddr *)&un, sizeof(un)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* send_lock 内：datagram 不需要循环发（内核保证一次性递送）。 */
static int dgram_send_locked(int fd, const uint8_t *buf, size_t len)
{
    ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) {
        n = send(fd, buf, len, MSG_NOSIGNAL);
    }
    if ((size_t)n != len) return -1;
    return 0;
}

/* 标记所有连接断开（send_lock 内调用） */
static void mark_broken_locked(void)
{
    if (g.fd_push >= 0) { close(g.fd_push); g.fd_push = -1; }
    if (g.fd_cmd  >= 0) { close(g.fd_cmd);  g.fd_cmd  = -1; }
}

/* 全量重建两个 datagram 连接。成功返回 0，失败返回 -1。 */
static int uds_reconnect_both(void)
{
    int p = uds_open_dgram(g.path_push);
    if (p < 0) return -1;
    int c = uds_open_dgram(g.path_cmd);
    if (c < 0) { close(p); return -1; }
    mark_broken_locked();
    g.fd_push = p;
    g.fd_cmd  = c;
    g.backoff_ms = UDS_RECONNECT_BASE_MS;
    g.last_recv_ms = 0;
    return 0;
}

/* CMD 通道发 HELLO */
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
    if (fsz == 0) return;
    pthread_mutex_lock(&g.send_lock);
    if (g.fd_cmd >= 0 && dgram_send_locked(g.fd_cmd, frame, fsz) != 0)
        mark_broken_locked();
    pthread_mutex_unlock(&g.send_lock);
}

/* ========================================================================
 * 收帧分发（仅后台线程，datagram 天然完整帧 —— 无需重组）
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

/* 校验 magic + total_len，然后按 frame_type 分发。
 * datagram 天然一个 recv 就是一帧 —— 不需要 half-frame / memmove 错位扫描。 */
static void dispatch_frame(const uint8_t *frame, size_t len)
{
    if (len < NSBC_FRAME_HDR_LEN) return;
    if (frame[0] != NSBC_FRAME_MAGIC) return;

    uint32_t total =
        ((uint32_t)frame[2] << 24) | ((uint32_t)frame[3] << 16) |
        ((uint32_t)frame[4] << 8) | (uint32_t)frame[5];
    if (total != len) return; /* datagram 帧完整性自检 —— 丢包/错位不应发生 */

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

/* datagram 收帧 —— 天然一帧一 recv，无重组负担。
 * recv 返回 0 或 errno=ECONNREFUSED 表示对端路径不存在（Rust 端还没 bind 好）。
 * 其他错误返回 -1 → 上层 reconnect。 */
static int recv_cmd_once(void)
{
    uint8_t frame[NSBC_MAX_FRAME];
    ssize_t n = recv(g.fd_cmd, frame, sizeof(frame), 0);
    if (n > 0) {
        g.last_recv_ms = nsbc_now_ms();
        dispatch_frame(frame, (size_t)n);
        return 0;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    if (n < 0 && errno == EINTR)
        return 0;
    /* ECONNREFUSED / ENOENT —— Rust 端还没起来，不是永久错误 */
    if (n < 0 && (errno == ECONNREFUSED || errno == ENOENT))
        return -1; /* 触发上层重连 */
    return -1;
}

/* ========================================================================
 * 后台线程：reconnect → poll(fd_cmd + fd_push + wake) → 收帧 / ring → PUSH
 * ======================================================================*/

/* ring → PUSH（push 通道，fire-and-forget，不回 ACK） */
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
    if (!root) return;
    cJSON_AddStringToObject(root, "kind", slot.kind);
    cJSON *parsed = cJSON_Parse(slot.json);
    if (parsed)
        cJSON_AddItemToObject(root, "payload", parsed);
    else
        cJSON_AddStringToObject(root, "payload", slot.json);
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return;

    size_t n = strlen(json_str);
    if (n > NSBC_MAX_PAYLOAD - NSBC_FRAME_HDR_LEN) {
        /* 单条事件超过 datagram 安全上限 —— 直接丢弃（大 payload 不应出现在 CDR） */
        llog(LOG_WARN, "uds", "push frame too large (%zuB), dropped", n);
        cJSON_free(json_str);
        return;
    }

    uint8_t frame[NSBC_FRAME_HDR_LEN + NSBC_MAX_PAYLOAD];
    size_t fsz = uds_encode_frame(FT_DATA_PUSH, 0, json_str, n,
                                   frame, sizeof(frame));
    cJSON_free(json_str);
    if (fsz == 0) return;

    pthread_mutex_lock(&g.send_lock);
    if (g.fd_push >= 0 && dgram_send_locked(g.fd_push, frame, fsz) != 0)
        mark_broken_locked();
    pthread_mutex_unlock(&g.send_lock);
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
        if (g.stop_flag) break;

        /* ---- 未连接：重连两个 datagram ---- */
        if (g.fd_push < 0 || g.fd_cmd < 0) {
            pthread_mutex_lock(&g.send_lock);
            int rc = uds_reconnect_both();
            pthread_mutex_unlock(&g.send_lock);
            if (rc != 0) {
                if (g.backoff_ms > UDS_RECONNECT_MAX_MS)
                    g.backoff_ms = UDS_RECONNECT_MAX_MS;
                llog(LOG_INFO, "uds",
                     "connect %s+%s failed, retry in %dms errno=%d",
                     g.path_push, g.path_cmd, g.backoff_ms, errno);
                usleep((useconds_t)g.backoff_ms * 1000);
                g.backoff_ms *= 2;
                continue;
            }
            g.backoff_ms = UDS_RECONNECT_BASE_MS;
            llog(LOG_NOTICE, "uds", "connected to %s + %s, sending hello",
                 g.path_push, g.path_cmd);
            send_hello();
        }

        /* ---- poll 三源：fd_cmd/POLLIN + fd_push/POLLIN + wake_pipe/POLLIN ----
         * fd_push 理论上不会收到数据（我们只发不收），但留着以防 Rust 端
         * 反向发 PUSH 回来（现在不会，但未来扩展安全）。 */
        int fdp = g.fd_push, fdc = g.fd_cmd;
        struct pollfd pfd[3];
        int nfds = 0;
        if (fdc >= 0) { pfd[nfds].fd = fdc; pfd[nfds].events = POLLIN; nfds++; }
        if (fdp >= 0) { pfd[nfds].fd = fdp; pfd[nfds].events = POLLIN; nfds++; }
        pfd[nfds].fd = g.wake_rd;
        pfd[nfds].events = POLLIN;
        nfds++;

        int pr = poll(pfd, nfds, 200);
        if (g.stop_flag) break;
        if (pr < 0) {
            if (errno == EINTR) continue;
            pthread_mutex_lock(&g.send_lock);
            mark_broken_locked();
            pthread_mutex_unlock(&g.send_lock);
            continue;
        }

        /* fd_cmd 可读 —— 收 RSP / HELLO_ACK / HEARTBEAT */
        if (fdc >= 0 && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (recv_cmd_once() != 0) {
                pthread_mutex_lock(&g.send_lock);
                mark_broken_locked();
                pthread_mutex_unlock(&g.send_lock);
                llog(LOG_INFO, "uds", "datagram path reset by peer, reconnecting");
                continue;
            }
        }
        /* wake pipe —— ring 有新消息 or stop */
        if (pfd[nfds-1].revents & POLLIN)
            drain_wake_pipe();

        /* ---- ring → PUSH ---- */
        flush_ring_one();
    }

    /* stop：尽力 flush 剩余（push 通道断了则丢弃） */
    pthread_mutex_lock(&g.lock);
    int remain = (int)g.count;
    pthread_mutex_unlock(&g.lock);
    while (remain-- > 0 && g.fd_push >= 0)
        flush_ring_one();

    pthread_mutex_lock(&g.send_lock);
    mark_broken_locked();
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

/* G1: 通用推送 API — 任意 kind（"cdr"/"billing_settle"/...），内部 ring 聚合异步发送。
 * 现有 uds_send_xxx 全部调这里，保持向后兼容。 */
int uds_push(const char *kind, const char *json)
{
    if (!kind || !json || !kind[0] || !g.started)
        return NSBC_ERR;
    return ring_put(kind, json);
}

int uds_send_cdr(const char *json)       { return uds_push("cdr",      json); }
int uds_send_register(const char *json)  { return uds_push("register", json); }
int uds_send_runtime(const char *json)   { return uds_push("runtime",  json); }
int uds_send_audit(const char *json)     { return uds_push("audit",    json); }
int uds_send_session(const char *json)   { return uds_push("session",  json); }
int uds_send_vad(const char *json)       { return uds_push("vad_event",json); }

int uds_flush_now(void)
{
    wake_thread();
    return NSBC_OK;
}

/* ========================================================================
 * 同步命令请求（S2）：占槽 → send(cmd) → cond 等待 RSP
 * ======================================================================*/

int uds_request(const char *cmd_json, int timeout_ms,
                char *resp, size_t resp_cap)
{
    if (!cmd_json || !resp || resp_cap == 0)
        return NSBC_INVAL;
    resp[0] = '\0';
    if (!g.started || g.fd_cmd < 0)
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

    uint8_t frame[NSBC_FRAME_HDR_LEN + 2048];
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
    int sfailed = (g.fd_cmd < 0 || dgram_send_locked(g.fd_cmd, frame, fsz) != 0);
    pthread_mutex_unlock(&g.send_lock);

    int rc = NSBC_ERR;
    if (!sfailed) {
        /* 3. 等 RSP */
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
        slot->in_use = 0;
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
    uds_derive_paths(p, g.path_push, sizeof(g.path_push),
                         g.path_cmd,  sizeof(g.path_cmd));

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
    g.fd_push = -1;
    g.fd_cmd  = -1;
    g.ticket_seq = 0;

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
    llog(LOG_NOTICE, "uds",
         "client thread started, push=%s cmd=%s",
         g.path_push, g.path_cmd);
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
    if (g.fd_push >= 0) { close(g.fd_push); g.fd_push = -1; }
    if (g.fd_cmd  >= 0) { close(g.fd_cmd);  g.fd_cmd  = -1; }
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
