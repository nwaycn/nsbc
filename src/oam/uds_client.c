/* NSBC Data Service — C 侧 UDS Client（REQ-DATA-SVC-002 P1）
 *
 * 传输层：双 AF_UNIX SOCK_DGRAM（Linux 可靠 datagram，无丢包无乱序，
 * 一帧一个 datagram —— 天然消息边界，无需帧重组 / half-frame 处理）。
 *   data_push.sock — 单向 C→Rust，FT_DATA_PUSH（CDR/register/runtime/audit/session）
 *                    fire-and-forget，不等待 ACK（shard ring buffer 自身有 overflow 处理）
 *   data_cmd.sock  — 双向，FT_HELLO/HELLO_ACK/CMD_REQ/CMD_RSP
 *                    CMD_REQ 阻塞等待匹配 ticket 的 CMD_RSP（16 槽 cond_wait）
 *
 * 帧格式（12B 固定头，与 web/src/uds_server.rs 完全对齐）：
 *   magic(1) + type(1) + total_len(4 NBO) + ticket(4 NBO) + flags(1) + rsvd(1)
 *   datagram 边界 = 帧边界，每次 recv 返回完整一帧。
 *
 * 线程模型（P1 自适应多核心 — 见 struct uds_state_t 注释）：
 *   - N 个 shard consumer 线程（N = sysconf(_SC_NPROCESSORS_ONLN), 上限 8）：
 *       每个 shard 独立 lock-free SPSC ring + wake pipe + send_lock
 *       poll(wake_pipe, 200ms 超时) → 从 ring 取最多 32 条 → sendmmsg 批量发送
 *       consumer 线程核亲和性绑 CPU（软设置，失败不阻塞）
 *   - 1 个 CMD poll 线程（独立，只管 CMD/HELLO/HEARTBEAT）：
 *       poll(fd_cmd + wake_pipe, 500ms 超时) → recv → dispatch_frame
 *       重连负责：reconnect → send_hello → 继续 poll
 *   - 业务线程：uds_send_*() atomic round-robin 选 shard → shard_put（零锁） → shard_wake
 *              uds_request() 占槽 + cmd_lock 保护分片 send + cond 等 RSP
 *
 * 同步原语：
 *   shard.head/tail —— lock-free SPSC（生产 atomic head release，消费 atomic tail release）
 *   g.next_shard    —— atomic round-robin（生产侧选 shard）
 *   g.conn_lock     —— fd open/close/reconnect 串行（所有 consumer + CMD 线程共用）
 *   g.cmd_lock      —— CMD 通道 send 串行（分片大 payload 时保护 fd_cmd）
 *   shard.send_lock —— 每个 shard flush 期间保护 fd_push（拆旧 g.send_lock 成每 shard 独立）
 *   slot->mtx/cv    —— 每个等待槽独立（16 槽并发上限）
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "uDS_client.h"
#include "log.h"
#include "nsbc_common.h"
#include "metric_reg.h"
#include "cmd_registry.h"    /* uds_handle_core_cmd → cmd_execute */
#include "obs.h"             /* obs_control_get_cmd_reg() 拿 CCMD 注册表 */
#include <cjson/cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>       /* CPU_SET / sched_setaffinity */
#include <stdarg.h>     /* va_start / va_end */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ========================================================================
 * 自适应多核心架构（REQ-DATA-SVC-002 P1）
 *
 * 检测 sysconf(_SC_NPROCESSORS_ONLN) 决定 shard 数：
 *   1 核   → 1 shard  （和旧代码一样，无开销）
 *   2 核   → 2 shards
 *   4 核   → 4 shards
 *   8+ 核  → 8 shards（上限防止过度分片）
 *
 * 每个 shard 独立 lock-free SPSC ring + consumer thread + wake_pipe。
 * 生产者（worker/billing 线程）按 atomic round-robin 选 shard → 零锁竞争。
 * 消费者（每个 shard 一个 consumer 线程）核亲和性绑 CPU，flush 用
 * sendmmsg batch 32 帧 → 一次 syscall 顶 32 次。
 *
 * CMD 通道保持单线程 poll（CMD 频率 ~100/s 不是热点，不走 shard）。
 * ======================================================================*/

/* shard 数量上限 + 每 shard ring 大小 */
#define UDS_MAX_SHARDS       8
#define UDS_MIN_SHARD_RING   4096   /* 每 shard ring 槽数（幂 2 便于 & mask） */
#define UDS_BATCH_SEND       32     /* sendmmsg 批量 */

/* ring buffer 槽：每个槽存一条待推 JSON */
typedef struct {
    char json[8192];    /* 单条事件 JSON 上限 8KB */
    char kind[16];      /* "cdr" / "register" / "runtime" / "audit" */
} uds_slot_t;

/* 单个 SPSC shard：生产侧 atomic head（release store），消费侧 atomic tail */
typedef struct {
    _Atomic uint32_t head;          /* 写侧：下一个写位置 */
    _Atomic uint32_t tail;          /* 读侧：下一个读位置 */
    uint32_t         size;          /* ring 总槽数（幂 2） */
    uint32_t         mask;          /* size-1 掩码 */
    uds_slot_t      *slots;         /* malloc 出来的 slot array（cache 对齐） */

    /* wake pipe（生产者写一端，consumer 读一端） */
    int              wake_rd;
    int              wake_wr;

    pthread_mutex_t  send_lock;     /* 本 shard 的 send/close 串行（拆 g.send_lock 成每 shard 独立） */
    pthread_t        thread;        /* consumer thread */
    int              running;       /* 1=已启动 */

    /* flush batch 临时 buffer（heap 分配，避免栈溢出 — frames[32][262KB] ≈ 8.3MB） */
    uds_slot_t      *batch;         /* UDS_BATCH_SEND × uds_slot_t ≈ 256KB */
    uint8_t         *frames;        /* UDS_BATCH_SEND × NSBC_MAX_FRAME ≈ 8MB */
    struct iovec    *iov;           /* UDS_BATCH_SEND × struct iovec */
    struct mmsghdr  *mh;            /* UDS_BATCH_SEND × struct mmsghdr */

    /* drop 计数（暴露到 metrics） */
    _Atomic uint32_t dropped;
} uds_shard_t;

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
    /* ---- shards 数组（自适应多核心） ---- */
    uds_shard_t      shards[UDS_MAX_SHARDS];
    int              n_shards;        /* 实际启用的 shard 数 */
    _Atomic uint32_t next_shard;       /* round-robin 计数器 */

    /* ---- 全局连接锁：保护 fd open/close/reconnect ---- */
    pthread_mutex_t  conn_lock;

    /* ---- CMD 通道保持单线程 ---- */
    pthread_mutex_t  cmd_lock;         /* CMD 发送串行（分片大 payload 时的 send 互斥） */
    pthread_t        cmd_thread;       /* CMD poll 线程 */
    int              cmd_started;

    /* 双 datagram fd —— 共享，所有 shard 都 connect() 到同一 push 路径 */
    int              fd_push;   /* C→Rust PUSH 通道，data_push.sock */
    int              fd_cmd;    /* 双向 CMD/HELLO 通道，data_cmd.sock */

    /* 派生的两个 server 路径（init 时从 base path 生成） */
    char             path_push[128];
    char             path_cmd[128];

    /* 自身 bind 的两个源路径（让服务端 recv_from 拿到可寻址 peer） */
    char             path_bind_push[128];
    char             path_bind_cmd[128];

    /* CMD poll 线程的 wake pipe */
    int              wake_cmd_rd;
    int              wake_cmd_wr;

    int              started;          /* 所有线程已启动 */
    volatile int     stop_flag;        /* 全通道停止标志 */

    /* 重连退避（CMD 通道用） */
    int              backoff_ms;

    /* 同步请求 */
    uds_reqslot_t    slots[UDS_REQ_SLOTS];
    volatile uint32_t ticket_seq;

    /* 上次收到帧时间戳（ms） —— heartbeat 超时检测用 */
    uint64_t         last_recv_ms;

    /* 总溢出丢弃计数（所有 shard 之和，暴露到 metrics） */
    volatile uint32_t ring_dropped;

    /* ---- 分片重组状态（收到 Rust 发来的大 payload 时累积） ----
     * 单槽重组：同时只允许一个 ticket 在重组（datagram 天然有序同 ticket 必连）。 */
    uint32_t         frag_ticket;     /* 当前重组 ticket，0=无分片在收 */
    char            *frag_buf;        /* malloc 重组 buffer */
    size_t           frag_len;        /* 已累积长度 */
    size_t           frag_total;      /* 预期总长度（最后一片到达后才知道） */
    uint64_t         frag_last_ms;    /* 最后一片到达时间戳（超时清理用） */
} uds_state_t;

static uds_state_t g = {0};

/* ---- 前向声明 ---- */
static int dgram_send_locked(int fd, const uint8_t *buf, size_t len);

/* ========================================================================
 * 帧编码：12B 固定头 + payload（NBO JSON）
 * ======================================================================*/

size_t uds_encode_frame(uint8_t frame_type, uint32_t ticket, uint8_t flags,
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
    *p++ = flags;   /* flags: bit0=more_fragment, bit1=compressed */
    *p++ = 0;       /* rsvd */
    memcpy(p, payload, payload_len);
    return frame_sz;
}

/* 分片发送：payload > MAX_PAYLOAD 时拆成多片独立 datagram。
 * 同 ticket，前 N-1 片 UDS_FLG_MORE_FRAG，最后一片 flags=0。
 * send_lock 必须由调用者持有。fd 必须 >= 0（不校验，上层保证）。
 * 返回 0=全部发送成功，-1=任一片发送失败（调用者 mark_broken）。 */
int uds_send_fragmented_locked(int fd, uint8_t frame_type, uint32_t ticket,
                               const char *payload, size_t payload_len)
{
    if (!payload || payload_len == 0) return -1;
    const size_t chunk = (size_t)NSBC_MAX_PAYLOAD;
    const uint8_t frame[NSBC_MAX_FRAME];

    if (payload_len <= chunk) {
        /* 单片发送，无分片 */
        size_t fsz = uds_encode_frame(frame_type, ticket, 0,
                                       payload, payload_len,
                                       (uint8_t *)frame, sizeof(frame));
        if (fsz == 0) return -1;
        return dgram_send_locked(fd, frame, fsz);
    }

    /* 多片发送 */
    size_t sent = 0;
    while (sent < payload_len) {
        size_t remaining = payload_len - sent;
        size_t this_chunk = remaining > chunk ? chunk : remaining;
        uint8_t flags = (this_chunk == chunk && remaining > chunk)
                          ? UDS_FLG_MORE_FRAG : 0;
        size_t fsz = uds_encode_frame(frame_type, ticket, flags,
                                       payload + sent, this_chunk,
                                       (uint8_t *)frame, sizeof(frame));
        if (fsz == 0) return -1;
        if (dgram_send_locked(fd, frame, fsz) != 0) return -1;
        sent += this_chunk;
    }
    return 0;
}

/* ========================================================================
 * datagram 连接 / 发送原语
 * ======================================================================*/

/* 打开一个 AF_UNIX SOCK_DGRAM：先 bind 自己的源路径（让服务端 recv_from
 * 拿到可寻址的 peer 路径，tokio 1.54 send_to 要求 AsRef<Path>），再 connect
 * 到服务端固定路径。datagram connect() 是立即的（同机路径不存在握手）。
 * 返回 fd（≥0），失败返回 -1。 */
static int uds_open_dgram(const char *server_path, const char *bind_path)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    /* datagram 也可以设 SO_RCVBUF/SO_SNDBUF 让 kernel queue 更大 */
    int rcvbuf = 1 << 20; /* 1 MB */
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &rcvbuf, sizeof(rcvbuf));

    /* 先 bind 源路径 —— 必须先 unlink 防止旧文件残留 */
    unlink(bind_path);
    struct sockaddr_un bd;
    memset(&bd, 0, sizeof(bd));
    bd.sun_family = AF_UNIX;
    snprintf(bd.sun_path, sizeof(bd.sun_path), "%s", bind_path);
    if (bind(fd, (struct sockaddr *)&bd, sizeof(bd)) != 0) {
        close(fd);
        return -1;
    }

    /* 再 connect 到服务端固定目标路径 */
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", server_path);
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

/* 全量重建两个 datagram 连接（先 bind 自身源路径，再 connect 服务端）。
 * 成功返回 0，失败返回 -1。 */
static int uds_reconnect_both(void)
{
    int p = uds_open_dgram(g.path_push, g.path_bind_push);
    if (p < 0) return -1;
    int c = uds_open_dgram(g.path_cmd, g.path_bind_cmd);
    if (c < 0) { close(p); unlink(g.path_bind_push); return -1; }
    mark_broken_locked();
    g.fd_push = p;
    g.fd_cmd  = c;
    g.backoff_ms = UDS_RECONNECT_BASE_MS;
    g.last_recv_ms = 0;
    return 0;
}

/* CMD 通道发 HELLO（用 cmd_lock 保护 fd_cmd 发送） */
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
    size_t fsz = uds_encode_frame(FT_HELLO, 0, 0, payload, (size_t)n,
                                  frame, sizeof(frame));
    if (fsz == 0) return;
    pthread_mutex_lock(&g.cmd_lock);
    if (g.fd_cmd >= 0 && dgram_send_locked(g.fd_cmd, frame, fsz) != 0) {
        pthread_mutex_lock(&g.conn_lock);
        mark_broken_locked();
        pthread_mutex_unlock(&g.conn_lock);
    }
    pthread_mutex_unlock(&g.cmd_lock);
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

/* ========================================================================
 * 分片重组：清理 / 超时 / 累积
 * ======================================================================*/

static void frag_reset(void)
{
    if (g.frag_buf) { free(g.frag_buf); g.frag_buf = NULL; }
    g.frag_ticket = 0;
    g.frag_len = 0;
    g.frag_total = 0;
    g.frag_last_ms = 0;
}

/* 每次 recv / poll 时检查：超过 3s 没收到后续片 → 丢弃 */
static void frag_check_timeout(void)
{
    if (g.frag_ticket == 0) return;
    uint64_t now = nsbc_now_ms();
    if (now - g.frag_last_ms > UDS_FRAG_TIMEOUT_MS) {
        llog(LOG_WARN, "uds", "fragment reassembly timeout ticket=%u (got %zuB), dropped",
             g.frag_ticket, g.frag_len);
        frag_reset();
    }
}

/* 累积一片 payload 到重组 buffer。返回 0=成功，-1=超限/失败。 */
static int frag_accumulate(uint32_t ticket, const uint8_t *payload, size_t plen)
{
    /* 新 ticket 或第一片：重置 */
    if (g.frag_ticket != ticket) frag_reset();

    size_t new_len = g.frag_len + plen;
    if (new_len == 0 || new_len > UDS_FRAG_MAX_TOTAL) {
        llog(LOG_WARN, "uds", "fragment too large (%zu > %u), dropped",
             new_len, UDS_FRAG_MAX_TOTAL);
        frag_reset();
        return -1;
    }

    char *nb = (char *)realloc(g.frag_buf, new_len);
    if (!nb) { frag_reset(); return -1; }
    memcpy(nb + g.frag_len, payload, plen);
    g.frag_buf = nb;
    g.frag_len = new_len;
    g.frag_ticket = ticket;
    g.frag_last_ms = nsbc_now_ms();
    return 0;
}

/* ========================================================================
 * Rust→C CMD_REQ 处理（extensions.reload 等大命令走 data_cmd.sock 二进制帧）
 * ======================================================================*/

/* cmd_registry io_printf 适配器：把输出写到本地 buffer，然后由调用者发回 RSP */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} uds_io_ctx_t;

static int uds_io_printf(void *io_ud, const char *fmt, ...)
{
    uds_io_ctx_t *io = (uds_io_ctx_t *)io_ud;
    if (!io || !fmt) return -1;
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    size_t need = io->len + (size_t)n + 1;
    if (need > io->cap) {
        size_t newcap = need < 4096 ? 4096 : need;
        char *nb = (char *)realloc(io->buf, newcap);
        if (!nb) return -1;
        io->buf = nb;
        io->cap = newcap;
    }
    memcpy(io->buf + io->len, tmp, (size_t)n);
    io->len += (size_t)n;
    io->buf[io->len] = '\0';
    return n;
}

/* 前向声明 — uds_handle_core_cmd 在下方定义，worker 线程需要先调 */
static void uds_handle_core_cmd(uint32_t ticket, const char *line, size_t len);

/* CMD_REQ 异步 worker —— 让 poll thread 立即回去 poll 不被 43s 的 extensions.reload 阻塞 */
typedef struct _cmd_job { uint32_t ticket; char *payload; size_t len; } _cmd_job_t;
static void *uds_handle_core_cmd_worker(void *arg) {
    _cmd_job_t *j = arg;
    uds_handle_core_cmd(j->ticket, j->payload, j->len);
    free(j->payload);
    free(j);
    return NULL;
}

/* 处理 Rust 发来的 CMD_REQ payload（行协议："extensions.reload <json>"），
 * 调 cmd_registry → 把结果作为 FT_CMD_RSP 发回。
 * send_lock 必须由调用者持有。 */
static void uds_handle_core_cmd(uint32_t ticket, const char *line, size_t len)
{
    cmd_reg_t *reg = obs_control_get_cmd_reg();
    if (!reg) {
        llog(LOG_WARN, "uds", "handle CMD_REQ but cmd_registry not ready");
        return;
    }

    /* 切分命令行：argv[0]=命令名，argv[1]=剩余全部（大 JSON payload 不按空格切） */
    /* 找第一个空格 */
    const char *sp = memchr(line, ' ', len);
    char argv0[CMD_NAME_MAX] = {0};
    const char *argv1 = "";
    size_t argv1_len = 0;

    if (sp) {
        size_t name_len = (size_t)(sp - line);
        if (name_len >= sizeof(argv0)) name_len = sizeof(argv0) - 1;
        memcpy(argv0, line, name_len);
        argv0[name_len] = '\0';
        argv1 = sp + 1;
        argv1_len = len - (size_t)(sp + 1 - line);
    } else {
        /* 无参数命令 */
        size_t name_len = len < sizeof(argv0) - 1 ? len : sizeof(argv0) - 1;
        memcpy(argv0, line, name_len);
        argv0[name_len] = '\0';
    }

    if (!argv0[0]) return;

    /* 检查命令是否已注册 */
    if (!cmd_registered(reg, argv0)) {
        llog(LOG_WARN, "uds", "unknown CMD_REQ command: %s", argv0);
        return;
    }

    /* 构造 ctx + io buffer，执行命令 */
    uds_io_ctx_t io = {.buf = NULL, .len = 0, .cap = 0};
    cmd_ctx_t ctx = {
        .uid = NULL,
        .authed_level = CMD_L2_ADMIN,   /* UDS CMD_REQ 隐含 L2 权限（同机 Rust 端调用） */
        .l2_confirmed = 1,
        .io_printf = uds_io_printf,
        .io_ud = &io,
    };

    /* Rust 端 periodic push 每 60s 推全量命令（N 条/次），NOTICE 级会刷屏。
     * 高频 CMD_REQ 详情降 DEBUG；CMD_REQ 本身的成功/失败由各 cmd handler 自打。 */
    llog(LOG_DEBUG, "uds-cmd", "CMD_REQ argv0='%s' argv1_len=%zu ticket=%u",
         argv0, argv1_len, ticket);

    /* 把 argv1 拷贝到临时 buffer（cmd_execute 可能原地修改 argv[1]） */
    char *argv1_copy = NULL;
    if (argv1_len > 0) {
        argv1_copy = (char *)malloc(argv1_len + 1);
        if (!argv1_copy) goto cleanup;
        memcpy(argv1_copy, argv1, argv1_len);
        argv1_copy[argv1_len] = '\0';
    }

    char *argv[2] = { argv0, argv1_copy };
    int argc = argv1_copy ? 2 : 1;
    int rc = cmd_execute(reg, &ctx, argc, argv);
    (void)rc;

cleanup:
    /* 把命令结果作为 FT_CMD_RSP 发回 Rust 端
     * conn_lock 保护 fd_cmd 不被 reconnect 并发重建，
     * cmd_lock 保护 send 分片（uds_send_fragmented_locked 假设调用者持锁）。 */
    if (io.buf && io.len > 0) {
        pthread_mutex_lock(&g.conn_lock);
        int fd = g.fd_cmd;
        pthread_mutex_unlock(&g.conn_lock);
        if (fd >= 0) {
            pthread_mutex_lock(&g.cmd_lock);
            if (uds_send_fragmented_locked(fd, FT_CMD_RSP, ticket,
                                            io.buf, io.len) != 0) {
                llog(LOG_WARN, "uds", "failed to send CMD_RSP ticket=%u", ticket);
            }
            pthread_mutex_unlock(&g.cmd_lock);
        }
    }
    free(argv1_copy);
    free(io.buf);
}

/* 校验 magic + total_len + flags，然后按 frame_type 分发。
 * 支持分片重组（flags.bit0=UDS_FLG_MORE_FRAG）。
 * 支持 Rust→C CMD_REQ（extensions.reload 等大命令走 data_cmd.sock）。 */
static void dispatch_frame(const uint8_t *frame, size_t len)
{
    if (len < NSBC_FRAME_HDR_LEN) return;
    if (frame[0] != NSBC_FRAME_MAGIC) return;

    uint32_t total =
        ((uint32_t)frame[2] << 24) | ((uint32_t)frame[3] << 16) |
        ((uint32_t)frame[4] << 8) | (uint32_t)frame[5];
    if (total != len) return; /* datagram 帧完整性自检 */

    uint8_t ftype = frame[1];
    uint32_t ticket = ((uint32_t)frame[6] << 24) | ((uint32_t)frame[7] << 16) |
                      ((uint32_t)frame[8] << 8) | (uint32_t)frame[9];
    uint8_t flags = frame[10];
    const uint8_t *payload = frame + NSBC_FRAME_HDR_LEN;
    size_t plen = len - NSBC_FRAME_HDR_LEN;

    /* 分片处理：先累积，最后一片再 dispatch */
    int is_last = !(flags & UDS_FLG_MORE_FRAG);
    if (!is_last) {
        /* 中间分片：累积后直接返回，等后续片 */
        frag_accumulate(ticket, payload, plen);
        return;
    }

    /* 最后一片 —— 可能是单片（前面无累积）也可能是末尾片 */
    char *full_payload = NULL;
    size_t full_len = 0;

    if (g.frag_ticket == ticket && g.frag_len > 0) {
        /* 之前有累积，现在是最后一片 —— 拼完整 */
        size_t new_len = g.frag_len + plen;
        char *buf = (char *)realloc(g.frag_buf, new_len + 1);
        if (!buf) { frag_reset(); return; }
        memcpy(buf + g.frag_len, payload, plen);
        buf[new_len] = '\0';
        full_payload = buf;
        full_len = new_len;
        g.frag_buf = NULL; frag_reset(); /* ownership 转移给 full_payload */
    } else {
        /* 单片（无前序累积） */
        full_len = plen;
        full_payload = (char *)malloc(plen + 1);
        if (!full_payload) return;
        memcpy(full_payload, payload, plen);
        full_payload[plen] = '\0';
    }

    switch (ftype) {
    case FT_CMD_RSP:
        dispatch_rsp(ticket, (const uint8_t *)full_payload, full_len);
        break;
    case FT_CMD_REQ: {
        /* Rust→C 命令请求
         * cmd_execute 可能耗时几十秒（extensions.reload 90K entries ≈ 43s）。
         * 绝不能在 poll thread 里同步执行 —— 会阻塞整个 poll loop，
         * 占满一颗核 + 让后续 CMD_REQ 排队。
         * 方案：pthread_create detached 执行 —— poll thread 立即回去 poll。 */
        _cmd_job_t *j = calloc(1, sizeof(*j));
        j->ticket  = ticket;
        j->payload = full_payload;       /* 所有权转移给 worker */
        j->len     = full_len;
        pthread_t tid;
        if (pthread_create(&tid, NULL, uds_handle_core_cmd_worker, j) == 0)
            pthread_detach(tid);
        else {
            uds_handle_core_cmd(ticket, full_payload, full_len); /* fallback 同步 */
            free(j);
        }
        full_payload = NULL;   /* 防止下面 free — worker 接管 */
        break;
    }
    case FT_HELLO_ACK:
        llog(LOG_NOTICE, "uds", "web hello_ack received (link up)");
        break;
    case FT_ACK:
    case FT_HEARTBEAT:
        break; /* 静默 */
    default:
        llog(LOG_DEBUG, "uds", "ignore frame type=0x%02x len=%zu", ftype, len);
        break;
    }
    free(full_payload);
}

/* datagram 收帧 —— 天然一帧一 recv，分片帧在 dispatch_frame 里累积/完成。
 * recvmsg 替代 recv：用 SCM_CREDENTIALS 拿 peer uid，非 root 发来的 datagram
 * 直接丢弃（安全修复 P0-5）。Linux ≥ 2.6.8 支持，Ubuntu 22.04 肯定可用。
 * 内核不支持时（CMSG_FIRSTHDR 返回 NULL）退回到"仅本机可达"信任模型。 */
static int recv_cmd_once(void)
{
    uint8_t frame[NSBC_MAX_FRAME];

    /* 安全修复 P0-5：用 recvmsg 拿 peer uid */
    struct iovec iov = { .iov_base = frame, .iov_len = sizeof(frame) };
    char ctrl[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = ctrl,
        .msg_controllen = sizeof(ctrl),
    };
    ssize_t n = recvmsg(g.fd_cmd, &msg, 0);

    /* 内核不支持 SCM_CREDENTIALS 时跳过校验 */
    if (n > 0) {
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        int uid_ok = 1;  /* 无 cmsg = 内核不支持 = 信任（退避） */
        uid_t peer_uid = (uid_t)-1;
        while (cmsg) {
            if (cmsg->cmsg_level == SOL_SOCKET &&
                cmsg->cmsg_type  == SCM_CREDENTIALS) {
                struct ucred *uc = (struct ucred *)CMSG_DATA(cmsg);
                peer_uid = uc->uid;
                if (peer_uid != 0) {
                    uid_ok = 0;
                    llog(LOG_WARN, "uds",
                         "drop datagram from non-root uid=%u "
                         "(CMD_REQ requires peer uid=0)",
                         (unsigned)peer_uid);
                }
                break;  /* 只取第一个 SCM_CREDENTIALS */
            }
            cmsg = CMSG_NXTHDR(&msg, cmsg);
        }
        if (!uid_ok) {
            frag_check_timeout();
            return 0;  /* 丢弃这帧，不触发上层重连 */
        }
    }

    frag_check_timeout();  /* 每次收前先检查上一个分片是否超时 */
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
 * shard lock-free SPSC ring 原语（生产 = 任意 worker 线程，消费 = 本 shard consumer 线程）
 *
 * SPSC 安全性：
 *   producer 只写 head（atomic store release → consumer 能看到 slots 里刚写完的数据）
 *   consumer 只写 tail（atomic store release → producer 能看到 slots 已被释放）
 *   同一时刻只有一个线程写 head，只有一个线程写 tail → 零锁竞争
 *
 * ring full：丢最旧（tail 前进一格 + dropped++）——和旧代码策略一致
 * ring empty：head == tail
 * ring full（留 1 空位区分）：(head + 1) & mask == tail
 * ======================================================================*/

static inline uint32_t next_pow2(uint32_t v)
{
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    v++;
    return v;
}

static int shard_init(uds_shard_t *s, uint32_t ring_size, int idx)
{
    (void)idx;  /* 未来可用于 shard 编号诊断日志，当前实现不需要 */
    memset(s, 0, sizeof(*s));
    s->size = next_pow2(ring_size);
    if (s->size < 256) s->size = 256;
    s->mask = s->size - 1;
    /* cache 对齐分配：每 slot 8KB，ring 4096 → 32MB 对齐到 64B */
    size_t alloc_sz = s->size * sizeof(uds_slot_t) + 64;
    s->slots = (uds_slot_t *)aligned_alloc(64, alloc_sz);
    if (!s->slots) return -1;
    memset(s->slots, 0, s->size * sizeof(uds_slot_t));

    int wfd[2];
    if (pipe2(wfd, O_CLOEXEC | O_NONBLOCK) != 0) {
        free(s->slots); s->slots = NULL;
        return -1;
    }
    s->wake_rd = wfd[0];
    s->wake_wr = wfd[1];
    pthread_mutex_init(&s->send_lock, NULL);

    /* 分配 flush batch 临时 buffer（heap，避免 8MB 栈溢出） */
    s->batch = (uds_slot_t *)calloc(UDS_BATCH_SEND, sizeof(uds_slot_t));
    s->frames = (uint8_t *)calloc(UDS_BATCH_SEND, NSBC_MAX_FRAME);
    s->iov = (struct iovec *)calloc(UDS_BATCH_SEND, sizeof(struct iovec));
    s->mh = (struct mmsghdr *)calloc(UDS_BATCH_SEND, sizeof(struct mmsghdr));
    if (!s->batch || !s->frames || !s->iov || !s->mh) {
        free(s->batch); free(s->frames); free(s->iov); free(s->mh);
        s->batch = NULL; s->frames = NULL; s->iov = NULL; s->mh = NULL;
        pthread_mutex_destroy(&s->send_lock);
        close(s->wake_rd); close(s->wake_wr);
        free(s->slots); s->slots = NULL;
        return -1;
    }
    return 0;
}

static void shard_destroy(uds_shard_t *s)
{
    if (s->wake_rd >= 0) { close(s->wake_rd); s->wake_rd = -1; }
    if (s->wake_wr >= 0) { close(s->wake_wr); s->wake_wr = -1; }
    pthread_mutex_destroy(&s->send_lock);
    /* 释放 batch 临时 buffer */
    free(s->batch);  s->batch  = NULL;
    free(s->frames); s->frames = NULL;
    free(s->iov);    s->iov    = NULL;
    free(s->mh);     s->mh     = NULL;
    free(s->slots);
    s->slots = NULL;
}

/* 生产侧：SPSC，零锁
 * 返回 0=ok，-1=参数错 */
static inline int shard_put(uds_shard_t *s, const char *kind, const char *json)
{
    uint32_t head = atomic_load_explicit(&s->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);
    uint32_t next_head = (head + 1) & s->mask;

    if (next_head == tail) {
        /* ring full —— 丢最旧 */
        uint32_t new_tail = (tail + 1) & s->mask;
        atomic_store_explicit(&s->tail, new_tail, memory_order_release);
        atomic_fetch_add_explicit(&s->dropped, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g.ring_dropped, 1, memory_order_relaxed);
    }

    uds_slot_t *slot = &s->slots[head];
    snprintf(slot->kind, sizeof(slot->kind), "%s", kind);
    snprintf(slot->json, sizeof(slot->json), "%s", json);
    atomic_store_explicit(&s->head, next_head, memory_order_release);
    return 0;
}

/* 生产侧：wake 本 shard 的 consumer pipe
 * pipe 满不阻塞（200ms poll 超时兜底） */
static inline void shard_wake(const uds_shard_t *s)
{
    char c = 1;
    (void)write(s->wake_wr, &c, 1);
}

/* 消费侧：从 ring 取最多 max 条，写入 out 数组。返回取出条数 */
static int shard_try_pop(uds_shard_t *s, uds_slot_t *out, int max)
{
    int got = 0;
    uint32_t tail = atomic_load_explicit(&s->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&s->head, memory_order_acquire);

    while (tail != head && got < max) {
        out[got] = s->slots[tail];  /* 拷贝出 ring（8KB slot 栈拷贝） */
        tail = (tail + 1) & s->mask;
        got++;
    }

    if (got > 0) {
        atomic_store_explicit(&s->tail, tail, memory_order_release);
    }
    return got;
}

/* 消费侧：sendmmsg 批量发送
 * 一次 syscall 发 UDS_BATCH_SEND 帧。每条帧是独立 datagram。 */
static int shard_flush_batch(uds_shard_t *s, int *out_fatal)
{
    /* 用 shard 自己预分配的 heap buffer，避免 8MB 栈溢出 */
    uds_slot_t *batch = s->batch;      /* UDS_BATCH_SEND × 8KB ≈ 256KB */
    uint8_t    *frames = s->frames;    /* UDS_BATCH_SEND × NSBC_MAX_FRAME ≈ 8MB */
    struct iovec *iov = s->iov;
    struct mmsghdr *mh = s->mh;

    int n = shard_try_pop(s, batch, UDS_BATCH_SEND);
    if (n == 0) return 0;

    int sent = 0;

    for (int i = 0; i < n; i++) {
        /* cJSON 包装 —— 这个 CPU 是 consumer 自己的，开销不影响 worker */
        cJSON *root = cJSON_CreateObject();
        if (!root) break;
        cJSON_AddStringToObject(root, "kind", batch[i].kind);
        cJSON *parsed = cJSON_Parse(batch[i].json);
        if (parsed)
            cJSON_AddItemToObject(root, "payload", parsed);
        else
            cJSON_AddStringToObject(root, "payload", batch[i].json);
        char *json_str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!json_str) break;

        size_t jlen = strlen(json_str);
        if (jlen > NSBC_MAX_PAYLOAD) {
            llog(LOG_WARN, "uds", "push frame too large (%zuB), dropped", jlen);
            cJSON_free(json_str);
            break;
        }

        /* frames[i] — 每次从 s->frames 基址偏移 i×NSBC_MAX_FRAME */
        uint8_t *frm = frames + (size_t)i * NSBC_MAX_FRAME;
        size_t fsz = uds_encode_frame(FT_DATA_PUSH, 0, 0,
                                        json_str, jlen,
                                        frm, NSBC_MAX_FRAME);
        cJSON_free(json_str);
        if (fsz == 0) break;

        iov[i].iov_base = frm;
        iov[i].iov_len  = fsz;
        memset(&mh[i], 0, sizeof(mh[i]));
        mh[i].msg_hdr.msg_iov = &iov[i];
        mh[i].msg_hdr.msg_iovlen = 1;
        sent++;
    }

    if (sent == 0) return n;  /* 全部被丢弃（cJSON/编码失败） */

    pthread_mutex_lock(&s->send_lock);
    if (g.fd_push < 0) {
        pthread_mutex_unlock(&s->send_lock);
        return n;  /* fd 已断，丢剩余 */
    }

    int sent_total = 0;
    /* glibc 在 _GNU_SOURCE 下已声明 sendmmsg —— 直接调
     * 返回 int：成功 = 发送的消息数 >=0，失败 = -1 */
    sent_total = sendmmsg(g.fd_push, mh, sent, 0);
    if (sent_total == -1) {
        /* sendmmsg 系统调用失败 —— 回退逐帧 send（兼容内核版本 < 3.0） */
        for (int i = 0; i < sent; i++) {
            uint8_t *frm = frames + (size_t)i * NSBC_MAX_FRAME;
            if (dgram_send_locked(g.fd_push, frm, iov[i].iov_len) != 0) {
                *out_fatal = 1;
                pthread_mutex_unlock(&s->send_lock);
                return n;
            }
        }
    } else {
        /* sendmmsg 可能部分成功 —— 失败帧按单帧 send 补 */
        for (int i = sent_total; i < sent; i++) {
            uint8_t *frm = frames + (size_t)i * NSBC_MAX_FRAME;
            if (dgram_send_locked(g.fd_push, frm, iov[i].iov_len) != 0) {
                *out_fatal = 1;
                pthread_mutex_unlock(&s->send_lock);
                return n;
            }
        }
    }
    pthread_mutex_unlock(&s->send_lock);
    return n;
}

/* consumer 线程主循环：poll(wake_pipe) → batch flush
 * 200ms poll 超时兜底（即使没 wake 也 flush，避免 ring 积压） */
static void *shard_consumer_thread(void *arg)
{
    uds_shard_t *s = (uds_shard_t *)arg;
    (void)s;

    while (!g.stop_flag) {
        /* poll wake pipe */
        struct pollfd pfd = { .fd = s->wake_rd, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (g.stop_flag) break;
        if (pr > 0) {
            char buf[64];
            while (read(s->wake_rd, buf, sizeof(buf)) > 0)
                ;
        }

        int fatal = 0;
        shard_flush_batch(s, &fatal);
        if (fatal) {
            /* send 永久失败 —— 标记 fd 断了，交给 CMD 线程做 reconnect */
            pthread_mutex_lock(&g.conn_lock);
            mark_broken_locked();
            pthread_mutex_unlock(&g.conn_lock);
        }
    }

    /* stop：尽力 flush 剩余 */
    int fatal = 0;
    while (!fatal)
        shard_flush_batch(s, &fatal);
    return NULL;
}

/* ========================================================================
 * CMD poll 线程（独立，只管 CMD/HELLO/HEARTBEAT 的收和回）
 * 旧 uds_client_thread 拆出来的 CMD 通道循环
 * ======================================================================*/

static void *cmd_poll_thread(void *ud)
{
    (void)ud;
    g.backoff_ms = UDS_RECONNECT_BASE_MS;

    for (;;) {
        if (g.stop_flag) break;

        /* ---- 未连接：重连两个 datagram ---- */
        if (g.fd_push < 0 || g.fd_cmd < 0) {
            pthread_mutex_lock(&g.conn_lock);
            int rc = uds_reconnect_both();
            pthread_mutex_unlock(&g.conn_lock);
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

        /* poll: fd_cmd (POLLIN) + wake_cmd (POLLIN)
         * 只 poll fd_cmd —— PUSH 通道的 flush 由每个 shard consumer 线程独立处理 */
        int fdc = g.fd_cmd;
        struct pollfd pfd[2];
        int nfds = 0;
        if (fdc >= 0) { pfd[nfds].fd = fdc; pfd[nfds].events = POLLIN; nfds++; }
        pfd[nfds].fd = g.wake_cmd_rd;
        pfd[nfds].events = POLLIN;
        nfds++;

        int pr = poll(pfd, nfds, 500);
        if (g.stop_flag) break;
        if (pr < 0) {
            if (errno == EINTR) continue;
            pthread_mutex_lock(&g.conn_lock);
            mark_broken_locked();
            pthread_mutex_unlock(&g.conn_lock);
            continue;
        }

        /* fd_cmd 可读 —— 收 RSP / HELLO_ACK / HEARTBEAT */
        if (fdc >= 0 && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (recv_cmd_once() != 0) {
                pthread_mutex_lock(&g.conn_lock);
                mark_broken_locked();
                pthread_mutex_unlock(&g.conn_lock);
                llog(LOG_INFO, "uds", "datagram path reset by peer, reconnecting");
                continue;
            }
        }
        /* wake pipe —— stop 或其它 */
        if (pfd[nfds-1].revents & POLLIN) {
            char buf[64];
            while (read(g.wake_cmd_rd, buf, sizeof(buf)) > 0)
                ;
        }
    }

    pthread_mutex_lock(&g.conn_lock);
    mark_broken_locked();
    pthread_mutex_unlock(&g.conn_lock);
    return NULL;
}

/* ========================================================================
 * 推送 API（业务线程调，atomic round-robin 选 shard → 零锁）
 * ======================================================================*/

/* 选 shard：atomic round-robin（多生产零锁竞争）
 * 如果只有 1 个 shard，每次都选 0 → 零开销 */
static inline int pick_shard(void)
{
    int n = g.n_shards;
    if (n <= 1) return 0;
    uint32_t next = atomic_fetch_add_explicit(&g.next_shard, 1, memory_order_relaxed);
    return (int)(next % (uint32_t)n);
}

static void shard_wake_all(void)
{
    for (int i = 0; i < g.n_shards; i++)
        shard_wake(&g.shards[i]);
}

static int uds_push_internal(const char *kind, const char *json)
{
    if (!kind || !json || !kind[0] || !g.started)
        return NSBC_ERR;
    int idx = pick_shard();
    shard_put(&g.shards[idx], kind, json);
    shard_wake(&g.shards[idx]);
    return NSBC_OK;
}

/* G1: 通用推送 API — 任意 kind（"cdr"/"billing_settle"/...），内部 shard 聚合异步发送。
 * 现有 uds_send_xxx 全部调这里，保持向后兼容。 */
int uds_push(const char *kind, const char *json)
{
    return uds_push_internal(kind, json);
}

int uds_send_cdr(const char *json)       { return uds_push_internal("cdr",      json); }
int uds_send_register(const char *json)  { return uds_push_internal("register", json); }
int uds_send_runtime(const char *json)   { return uds_push_internal("runtime",  json); }
int uds_send_audit(const char *json)     { return uds_push_internal("audit",    json); }
int uds_send_session(const char *json)   { return uds_push_internal("session",  json); }
int uds_send_vad(const char *json)       { return uds_push_internal("vad_event",json); }

int uds_flush_now(void)
{
    shard_wake_all();
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

    size_t plen = strlen(cmd_json);
    if (plen == 0) {
        pthread_mutex_lock(&slot->mtx); slot->in_use = 0; pthread_mutex_unlock(&slot->mtx);
        return NSBC_ERR;
    }

    /* 2. 发送：cmd_lock 保护 fd_cmd 分片发送 */
    pthread_mutex_lock(&g.cmd_lock);
    int sfailed = (g.fd_cmd < 0 ||
                   uds_send_fragmented_locked(g.fd_cmd, FT_CMD_REQ, ticket,
                                               cmd_json, plen) != 0);
    pthread_mutex_unlock(&g.cmd_lock);

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

/* OBS 自埋点：UDS ring overflow render —— 所有 shard dropped 之和 */
static int r_uds_ring(char *b, size_t cap, void *ud)
{
    (void)ud;
    uint32_t total = 0;
    for (int i = 0; i < g.n_shards; i++)
        total += atomic_load_explicit(&g.shards[i].dropped, memory_order_relaxed);
    if (total == 0) total = g.ring_dropped;  /* fallback 到全局累加器 */
    return snprintf(b, cap, "nsbc_uds_ring_dropped %u\n", total);
}

/* 核亲和性尝试（软设置，失败不阻塞） */
static void try_set_cpu_affinity(pthread_t t, int cpu)
{
#ifdef __linux__
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    /* 软亲和：sched_setaffinity 失败不影响线程运行 */
    pthread_setaffinity_np(t, sizeof(mask), &mask);
#endif
}

/* 探测在线 CPU 数：最少 1，最多 UDS_MAX_SHARDS=8 */
static int detect_ncpu(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > UDS_MAX_SHARDS) n = UDS_MAX_SHARDS;
    return (int)n;
}

int uds_client_init(const char *sock_path)
{
    const char *p = sock_path ? sock_path : uds_default_sock_path();
    uds_derive_paths(p, g.path_push, sizeof(g.path_push),
                         g.path_cmd,  sizeof(g.path_cmd));

    /* 派生自身 bind 路径（客户端源路径） */
    snprintf(g.path_bind_push, sizeof(g.path_bind_push),
             "/tmp/nsbc_client_%d_push.sock", (int)getpid());
    snprintf(g.path_bind_cmd, sizeof(g.path_bind_cmd),
             "/tmp/nsbc_client_%d_cmd.sock", (int)getpid());

    /* 探测核数 + 初始化 shards */
    g.n_shards = detect_ncpu();
    /* 1 核：单 shard，ring 更大（8192）；多核：每 shard 4096，总容量 = n_shards*4096 */
    uint32_t ring_per_shard = (g.n_shards <= 1) ? 8192U : UDS_MIN_SHARD_RING;

    for (int i = 0; i < g.n_shards; i++) {
        if (shard_init(&g.shards[i], ring_per_shard, i) != 0) {
            llog(LOG_ERR, "uds", "shard %d init failed", i);
            for (int j = 0; j < i; j++) shard_destroy(&g.shards[j]);
            return NSBC_ERR;
        }
    }
    atomic_store_explicit(&g.next_shard, 0, memory_order_relaxed);

    /* CMD 通道锁 + wake pipe */
    pthread_mutex_init(&g.conn_lock, NULL);
    pthread_mutex_init(&g.cmd_lock, NULL);
    int wfd[2];
    if (pipe2(wfd, O_CLOEXEC | O_NONBLOCK) != 0) {
        pthread_mutex_destroy(&g.conn_lock);
        pthread_mutex_destroy(&g.cmd_lock);
        for (int i = 0; i < g.n_shards; i++) shard_destroy(&g.shards[i]);
        return NSBC_ERR;
    }
    g.wake_cmd_rd = wfd[0];
    g.wake_cmd_wr = wfd[1];

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

    /* 先启动 N 个 shard consumer 线程 */
    for (int i = 0; i < g.n_shards; i++) {
        g.shards[i].running = 1;
        int rc = pthread_create(&g.shards[i].thread, NULL,
                                 shard_consumer_thread, &g.shards[i]);
        if (rc != 0) {
            g.shards[i].running = 0;
            /* 已启动的线程先 stop */
            g.stop_flag = 1;
            char c = 1;
            if (g.wake_cmd_wr >= 0) write(g.wake_cmd_wr, &c, 1);
            for (int j = 0; j < i; j++) {
                shard_wake(&g.shards[j]);
                pthread_join(g.shards[j].thread, NULL);
                g.shards[j].running = 0;
            }
            return NSBC_ERR;
        }
        try_set_cpu_affinity(g.shards[i].thread, i % detect_ncpu());
    }

    /* 再启动 CMD poll 线程 */
    g.cmd_started = 1;
    int rc = pthread_create(&g.cmd_thread, NULL, cmd_poll_thread, NULL);
    if (rc != 0) {
        g.cmd_started = 0;
        g.stop_flag = 1;
        char c = 1;
        if (g.wake_cmd_wr >= 0) write(g.wake_cmd_wr, &c, 1);
        for (int j = 0; j < g.n_shards; j++) {
            shard_wake(&g.shards[j]);
            pthread_join(g.shards[j].thread, NULL);
            g.shards[j].running = 0;
        }
        return NSBC_ERR;
    }

    g.started = 1;
    llog(LOG_NOTICE, "uds",
         "adaptive %d-core mode: %d shards × ring=%u, push=%s cmd=%s",
         detect_ncpu(), g.n_shards,
         (unsigned)(g.n_shards <= 1 ? 8192U : UDS_MIN_SHARD_RING),
         g.path_push, g.path_cmd);
    return NSBC_OK;
}

void uds_client_stop(void)
{
    if (!g.started) return;
    g.stop_flag = 1;

    /* 唤醒 CMD 线程 */
    char c = 1;
    if (g.wake_cmd_wr >= 0) (void)write(g.wake_cmd_wr, &c, 1);
    if (g.cmd_started) {
        pthread_join(g.cmd_thread, NULL);
        g.cmd_started = 0;
    }

    /* 唤醒所有 shard consumer 线程 */
    for (int i = 0; i < g.n_shards; i++) {
        if (g.shards[i].running) {
            shard_wake(&g.shards[i]);
            pthread_join(g.shards[i].thread, NULL);
            g.shards[i].running = 0;
        }
    }

    g.started = 0;
    llog(LOG_NOTICE, "uds", "all %d shards + cmd thread stopped", g.n_shards);
}

void uds_client_shutdown(void)
{
    uds_client_stop();
    frag_reset();  /* 清理分片重组 buffer */

    /* 销毁 shards */
    for (int i = 0; i < g.n_shards; i++)
        shard_destroy(&g.shards[i]);
    g.n_shards = 0;

    pthread_mutex_destroy(&g.conn_lock);
    pthread_mutex_destroy(&g.cmd_lock);

    for (int i = 0; i < UDS_REQ_SLOTS; ++i) {
        pthread_mutex_destroy(&g.slots[i].mtx);
        pthread_cond_destroy(&g.slots[i].cv);
    }
    if (g.wake_cmd_rd >= 0) { close(g.wake_cmd_rd); g.wake_cmd_rd = -1; }
    if (g.wake_cmd_wr >= 0) { close(g.wake_cmd_wr); g.wake_cmd_wr = -1; }

    pthread_mutex_lock(&g.conn_lock);
    if (g.fd_push >= 0) { close(g.fd_push); g.fd_push = -1; }
    if (g.fd_cmd  >= 0) { close(g.fd_cmd);  g.fd_cmd  = -1; }
    pthread_mutex_unlock(&g.conn_lock);

    /* 清理自身 bind 的源路径文件 */
    unlink(g.path_bind_push);
    unlink(g.path_bind_cmd);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
