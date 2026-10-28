#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "log.h"
#include "nsbc_atomic.h"
#include "nsbc_common.h"
#include "realtime.h"
#include "utils.h"   /* mkdir_p() — 通用工具已抽 src/utils/utils.c */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* ---- mkdir_p 已移至 src/utils/utils.c（通用工具模块） ---- */

typedef struct {
    char data[LOG_RECORD_MAX];
    uint16_t len;
} log_slot_t;

/* ---- 全局状态 ---- */
static int g_fd = -1;          /* -1=stderr */
static int g_audit_fd = -1;
static char g_log_path[PATH_MAX];  /* 日志文件路径（dump 用） */
static log_level_t g_level = LOG_INFO;
static uint32_t g_ring_slots = 512;
static pthread_key_t g_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;

struct ring2; /* 前向声明 */
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ring2 *g_rings; /* 已登记 ring 链表 */

static pthread_mutex_t g_wake = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static int g_running;
static pthread_t g_thr;

static nsbc_atomic_t g_dropped;
static nsbc_atomic_t g_written;
static int g_write_errs;        /* 连续 write 错误计数；触发 reopen */
static volatile sig_atomic_t g_reopen_pending;  /* SIGHUP 触发的 reopen 请求 (async-signal-safe) */

/* ---- 自管理 size-based rotate（不依赖外部 logrotate） ----
 * 每次 logger_main drain 完所有 ring 后检查文件大小，
 * 超过阈值就把当前文件 → .1，.1 → .2，...，.keep-1 删掉，然后开新文件。 */
static off_t g_cur_size;          /* 当前日志文件已累计写入字节数 */
static off_t g_rotate_max = 50 * 1024LL * 1024LL;  /* 默认 50MB 触发 rotate */
static int   g_rotate_keep = 5;   /* 保留 .1 ~ .4 + .5（共 5 份历史） */

/* ---- per-thread ring（干净布局） ---- */
typedef struct ring2 {
    log_slot_t *slots;
    uint32_t mask;
    char pad0[NSBC_CACHELINE - sizeof(void *) - sizeof(uint32_t)];
    unsigned long head NSBC_CACHELINE_ALIGNED;
    char pad1[NSBC_CACHELINE - sizeof(unsigned long)];
    unsigned long tail NSBC_CACHELINE_ALIGNED;
    char pad2[NSBC_CACHELINE - sizeof(unsigned long)];
    struct ring2 *next;
    int active;
} ring2_t;

static void ring_push(ring2_t *r, const char *data, uint16_t len)
{
    unsigned long h = r->head;
    unsigned long next = (h + 1) & r->mask;
    if (next == r->tail) {
        sat_inc(g_dropped);
        return;
    }
    r->slots[h].len = len;
    memcpy(r->slots[h].data, data, len);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->head = next;
}

static void key_dtor(void *p) { (void)p; /* detach 在 pthread 退出路径显式做 */ }
static void key_once(void) { pthread_key_create(&g_key, key_dtor); }

int log_thread_attach(void)
{
    pthread_once(&g_key_once, key_once);
    ring2_t *r = (ring2_t *)pthread_getspecific(g_key);
    if (r)
        return NSBC_OK;
    r = (ring2_t *)calloc(1, sizeof(*r));
    if (!r)
        return NSBC_NOMEM;
    uint32_t n = g_ring_slots;
    if (posix_memalign((void **)&r->slots, NSBC_CACHELINE,
                       (size_t)n * sizeof(log_slot_t)) != 0) {
        free(r);
        return NSBC_NOMEM;
    }
    r->mask = n - 1;
    r->active = 1;
    pthread_setspecific(g_key, r);
    pthread_mutex_lock(&g_reg_lock);
    r->next = (ring2_t *)g_rings;
    g_rings = r;
    pthread_mutex_unlock(&g_reg_lock);
    return NSBC_OK;
}

void log_thread_detach(void)
{
    ring2_t *r = (ring2_t *)pthread_getspecific(g_key);
    if (!r)
        return;
    r->active = 0;
    pthread_setspecific(g_key, NULL);
    /* 摘除 + 排空在登记锁内一次完成：日志线程不会并发访问本环
     * （停机路径，宁可短暂等待磁盘） */
    pthread_mutex_lock(&g_reg_lock);
    ring2_t **pp = (ring2_t **)&g_rings;
    while (*pp && *pp != r)
        pp = &(*pp)->next;
    if (*pp)
        *pp = r->next;
    while (r->tail != r->head) {
        log_slot_t *s = &r->slots[r->tail];
        if (g_fd >= 0)
            (void)write(g_fd, s->data, s->len);
        else
            fwrite(s->data, 1, s->len, stderr);
        r->tail = (r->tail + 1) & r->mask;
    }
    pthread_mutex_unlock(&g_reg_lock);
    free(r->slots);
    free(r);
}

/* ---- size-based rotate（自实现，不依赖外部 logrotate） ----
 * 调用点：logger_main drain 完所有 ring + fsync 之后。
 * 此时 g_reg_lock 已释放、ring 空、fsync 完成，最安全。
 *
 * rotate 策略（g_rotate_keep = 5 时）：
 *   nsbcd.log.4 → nsbcd.log.5   (最老的 .4 变成 .5，然后 .5 会被下次覆盖)
 *   nsbcd.log.3 → nsbcd.log.4
 *   nsbcd.log.2 → nsbcd.log.3
 *   nsbcd.log.1 → nsbcd.log.2
 *   nsbcd.log   → nsbcd.log.1
 *   (新建)      → nsbcd.log     ← 新的当前文件
 *
 * 失败回退：如果 open 新文件失败，恢复旧 fd，重置 g_cur_size=0（避免无限重试）。
 */
static void do_rotate(void)
{
    if (!g_log_path[0]) return;  /* stderr 模式 */

    /* 1. 关旧 fd（已经 fsync 过了） */
    int old_fd = g_fd;
    g_fd = -1;

    /* 2. 重命名：.keep-1 删掉，.i → .i+1，当前 → .1 */
    char src[PATH_MAX], dst[PATH_MAX];
    /* 先删最老的那份（存在就删，不存在就忽略） */
    snprintf(dst, sizeof(dst), "%s.%d", g_log_path, g_rotate_keep);
    unlink(dst);  /* ENOENT 正常，不关心错误 */

    /* 依次 .i → .i+1，从最老往前搬 */
    for (int i = g_rotate_keep - 1; i >= 1; i--) {
        snprintf(src, sizeof(src), "%s.%d", g_log_path, i);
        snprintf(dst, sizeof(dst), "%s.%d", g_log_path, i + 1);
        rename(src, dst);  /* ENOENT 正常（第一次 rotate 时还没历史） */
    }
    /* 当前文件 → .1 */
    rename(g_log_path, dst);  /* dst 此时 = g_log_path + ".1" */

    /* 3. 开新文件 */
    int new_fd = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (new_fd < 0) {
        /* 回退：恢复旧 fd，重置 size 计数器避免无限重试 */
        g_fd = old_fd;
        g_cur_size = 0;
        fprintf(stderr,
                "[log-rotate] open new FAIL errno=%d(%s) path=%s — keeping old fd\n",
                errno, strerror(errno), g_log_path);
        return;
    }

    /* 4. 成功：关旧 fd（如果之前有），重置计数器 */
    if (old_fd >= 0)
        close(old_fd);
    g_fd = new_fd;
    g_cur_size = 0;
    g_write_errs = 0;

    fprintf(stderr,
            "[log-rotate] %s → %s.1 (limit %lld MB, keep %d) at %s\n",
            g_log_path, g_log_path,
            (long long)(g_rotate_max / (1024 * 1024)), g_rotate_keep,
            __TIME__);
}

static void __attribute__((used)) sig_handler_hup(int sig) {
    (void)sig;
    g_reopen_pending = 1;  /* sig_atomic_t assignment — async-signal-safe */
}

static void wake_logger(void)
{
    pthread_mutex_lock(&g_wake);
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_wake);
}

static void *logger_main(void *arg)
{
    (void)arg;
    for (;;) {
        int any = 0;
        pthread_mutex_lock(&g_reg_lock);
        ring2_t *r = (ring2_t *)g_rings;
        while (r) {
            for (;;) {
                unsigned long t = r->tail;
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                if (t == r->head)
                    break;
                log_slot_t *s = &r->slots[t];
                if (g_fd >= 0) {
                    ssize_t w = write(g_fd, s->data, s->len);
                    if (w < 0) {
                        if (errno == EINTR) {
                            /* 重试即可，不计入错误 */
                            continue;
                        }
                        g_write_errs++;
                        /* 每秒最多打一条 warn（避免刷屏） */
                        static time_t last_warn;
                        time_t now = time(NULL);
                        if (now != last_warn) {
                            fprintf(stderr,
                                    "[log-flush] write FAIL fd=%d errno=%d(%s) "
                                    "path=%s (consecutive=%d)\n",
                                    g_fd, errno, strerror(errno),
                                    g_log_path[0] ? g_log_path : "<stderr>",
                                    g_write_errs);
                            last_warn = now;
                        }
                        /* 连续错误过多 → 尝试重开（sleep/wake 后 fd 可能失效） */
                        if (g_write_errs >= 100 && g_log_path[0]) {
                            int nfd = open(g_log_path,
                                           O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                                           0640);
                            if (nfd >= 0) {
                                fprintf(stderr,
                                        "[log-flush] reopened fd %d → %d after %d errors\n",
                                        g_fd, nfd, g_write_errs);
                                if (g_fd >= 0) close(g_fd);
                                g_fd = nfd;
                                g_write_errs = 0;
                            } else {
                                /* reopen 也失败 → 退到 stderr */
                                if (g_fd >= 0) close(g_fd);
                                g_fd = -1;
                                g_write_errs = 0;
                                fprintf(stderr,
                                        "[log-flush] reopen FAIL errno=%d(%s), "
                                        "fallback to stderr\n",
                                        errno, strerror(errno));
                            }
                        }
                        /* 跳过这条，继续下一条 */
                    } else {
                        g_write_errs = 0; /* 成功清零 */
                        g_cur_size += w;  /* 累计已写入字节，供 size-rotate 用 */
                    }
                } else {
                    fwrite(s->data, 1, s->len, stderr);
                }
                sat_inc(g_written);
                r->tail = (t + 1) & r->mask;
                any = 1;
            }
            r = r->next;
        }
        pthread_mutex_unlock(&g_reg_lock);
        if (g_fd >= 0) {
            if (fsync(g_fd) < 0 && errno != EINTR) {
                static time_t last_fsync_warn;
                time_t now = time(NULL);
                if (now != last_fsync_warn) {
                    fprintf(stderr, "[log-flush] fsync FAIL fd=%d errno=%d(%s)\n",
                            g_fd, errno, strerror(errno));
                    last_fsync_warn = now;
                }
            }
            /* ---- size-based rotate 检查 ----
             * drain 完所有 ring + fsync 完成后才触发：此时 ring 空、磁盘已落、
             * fd 状态安全。do_rotate 内部 self-close + rename + open，失败回退旧 fd。 */
            if (g_cur_size >= g_rotate_max) {
                do_rotate();
            }
        }
        /* SIGHUP-triggered reopen (logrotate postrotate hook compatibility).
         * Signal handler only sets g_reopen_pending (async-signal-safe).
         * We do the actual open/close here in the logger thread context. */
        if (g_reopen_pending) {
            g_reopen_pending = 0;
            if (g_log_path[0]) {
                int nfd = open(g_log_path,
                               O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                               0640);
                if (nfd >= 0) {
                    int old = g_fd;
                    g_fd = nfd;
                    if (old >= 0)
                        close(old);
                    g_write_errs = 0;
                    fprintf(stderr,
                            "[log-flush] SIGHUP reopen OK fd=%d → %d path=%s\n",
                            old, nfd, g_log_path);
                } else {
                    fprintf(stderr,
                            "[log-flush] SIGHUP reopen FAIL errno=%d(%s) path=%s\n",
                            errno, strerror(errno), g_log_path);
                }
            }
        }
        if (!g_running && !any)
            break;
        pthread_mutex_lock(&g_wake);
        if (!g_running) {
            pthread_mutex_unlock(&g_wake);
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_nsec -= 1000000000L;
            ts.tv_sec += 1;
        }
        pthread_cond_timedwait(&g_cond, &g_wake, &ts);
        pthread_mutex_unlock(&g_wake);
    }
    return NULL;
}

int log_init(const char *path, log_level_t level, uint32_t ring_slots)
{
    if (ring_slots == 0 || (ring_slots & (ring_slots - 1)))
        return NSBC_INVAL;
    g_ring_slots = ring_slots;
    g_level = level;
    if (path && *path) {
        snprintf(g_log_path, sizeof(g_log_path), "%s", path);
    } else {
        g_log_path[0] = '\0';
    }
    int nfd = -1;
    if (path && *path) {
        mkdir_p(path);  /* 自动创建父目录（mkdir -p 语义） */
        nfd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
        if (nfd < 0) {
            fprintf(stderr, "log_init: open(%s) failed errno=%d (%s)\n",
                    path, errno, strerror(errno));
            return NSBC_ERR;
        }
        /* 初始化 size 计数器：从当前文件已有大小开始（重启后续写） */
        struct stat st;
        g_cur_size = (fstat(nfd, &st) == 0) ? st.st_size : 0;
    } else {
        g_cur_size = 0;
    }
    if (g_running) {
        /* 两阶段启动：第二次调用仅热切换输出 fd，不重复建日志线程
         * ★ P0 RACE-FIX: close(old) 必须在 g_reg_lock 内。
         * logger_main 只在持 g_reg_lock 时 write(g_fd)，因此 close 争用
         * 同一把锁就不会和 write 并发 — 不会 close 一个正在被 write 的 fd。 */
        int old = g_fd;
        g_fd = nfd;
        if (old >= 0) {
            pthread_mutex_lock(&g_reg_lock);
            close(old);
            pthread_mutex_unlock(&g_reg_lock);
        }
        return NSBC_OK;
    }
    g_fd = nfd;
    g_running = 1;
    pthread_once(&g_key_once, key_once);
    if (pthread_create(&g_thr, NULL, logger_main, NULL) != 0) {
        g_running = 0;
        if (g_fd >= 0)
            close(g_fd);
        return NSBC_ERR;
    }
    /* 全局退出钩子：确保任何 exit()/main return 都能 flush ring buffer，
     * 防止 early return 丢失关键日志。atexit 本身是幂等的（log_stop 检查
     * g_running），所以正常停机路径的显式 log_stop() 再调一次也安全。 */
    atexit(log_stop);
    /* SIGHUP → reopen log file（logrotate postrotate 兼容）。
     * handler 只设 volatile sig_atomic_t flag，实际 reopen 在 logger 线程里做，
     * 因为 signal handler 上下文不能安全地用 pthread/heap 操作。 */
    struct sigaction sa = { 0 };
    sa.sa_handler = sig_handler_hup;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);
    return NSBC_OK;
}

int log_audit_open(const char *path)
{
    if (!path || !*path)
        return NSBC_OK;  /* 没配审计路径，跳过 */
    mkdir_p(path);  /* 自动创建父目录 */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd < 0) {
        fprintf(stderr, "log_audit_open: open(%s) failed errno=%d (%s)\n",
                path, errno, strerror(errno));
        return NSBC_ERR;
    }
    g_audit_fd = fd;
    return NSBC_OK;
}

void log_set_level(log_level_t level) { g_level = level; }
log_level_t log_get_level(void) { return g_level; }

/// Configure size-based log rotation.
/// @param max_bytes  trigger size in bytes; 0 or negative → disable size-rotate
///                   (SIGHUP reopen still works). Suggested: 50*1024*1024 (50MB) or larger.
/// @param keep       number of historical copies to keep (1..9). Default 5.
void log_set_rotate(long long max_bytes, int keep)
{
    if (max_bytes <= 0) {
        g_rotate_max = -1;  /* 禁用 */
        return;
    }
    g_rotate_max = (off_t)max_bytes;
    if (keep < 1) keep = 1;
    if (keep > 9) keep = 9;
    g_rotate_keep = keep;
}

void log_stop(void)
{
    if (!g_running)
        return;
    g_running = 0;
    wake_logger();
    pthread_join(g_thr, NULL);
    if (g_fd >= 0)
        close(g_fd);
    g_fd = -1;
    if (g_audit_fd >= 0)
        close(g_audit_fd);
    g_audit_fd = -1;
}

static const char level_chars[] = "EACEWNID";

void log_emit(log_level_t lvl, const char *mod, const char *callid,
              const char *file, int line,
              const char *fmt, ...)
{
    if ((int)lvl > (int)g_level)
        return;
    pthread_once(&g_key_once, key_once);
    ring2_t *r = (ring2_t *)pthread_getspecific(g_key);
    if (!r) {
        if (log_thread_attach() != NSBC_OK)
            return;
        r = (ring2_t *)pthread_getspecific(g_key);
    }
    char buf[LOG_RECORD_MAX];
    char ts[32];
    nsbc_wall_iso8601(ts, sizeof(ts));
    /* 取 file basename（去掉路径前缀）— 同时处理 / 和 \ */
    const char *base = file ? strrchr(file, '/') : NULL;
    {
        const char *bs = file ? strrchr(file, '\\') : NULL;
        if (bs && (!base || bs > base)) base = bs;
    }
    if (base) base++; else base = file;
    int n = snprintf(buf, sizeof(buf), "%s %c %-6s [%s] %s:%d ", ts,
                     level_chars[(unsigned)lvl <= 7 ? lvl : 7],
                     mod ? mod : "-", callid && *callid ? callid : "-",
                     base ? base : "?", line);
    if (n < 0 || n >= (int)sizeof(buf))
        return;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
    va_end(ap);
    if (m < 0)
        return;
    if ((size_t)(n + m) >= sizeof(buf) - 1)
        m = (int)sizeof(buf) - 1 - n;
    n += m;
    if (n < (int)sizeof(buf) - 1) {
        buf[n++] = '\n';
    }
    ring_push(r, buf, (uint16_t)n);
    /* ERR 及以上立即唤醒；其余依赖 5ms 定时聚合 */
    if (lvl <= LOG_ERR)
        wake_logger();
}

void log_audit(const char *actor, const char *action, const char *fmt, ...)
{
    char buf[LOG_RECORD_MAX];
    char ts[32];
    nsbc_wall_iso8601(ts, sizeof(ts));
    int n = snprintf(buf, sizeof(buf), "%s AUDIT actor=%s action=%s ", ts,
                     actor ? actor : "-", action ? action : "-");
    if (n < 0 || n >= (int)sizeof(buf))
        return;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
    va_end(ap);
    if (m < 0)
        return;
    if ((size_t)(n + m) >= sizeof(buf) - 1)
        m = (int)sizeof(buf) - 1 - n;
    n += m;
    if (n < (int)sizeof(buf) - 1)
        buf[n++] = '\n';
    int fd = g_audit_fd >= 0 ? g_audit_fd : (g_fd >= 0 ? g_fd : 2);
    /* O_APPEND 保证单条 write 原子追加；审计不允许丢弃，失败仅计数 */
    ssize_t w = write(fd, buf, (size_t)n);
    (void)w;
}

uint64_t log_dropped(void) { return sat_read(g_dropped); }
uint64_t log_written(void) { return sat_read(g_written); }

/* ---- 从 fd 的末尾倒推，找到最后 nlines 行的起始偏移 ---- */
static off_t tail_find_start(int fd, int nlines)
{
    if (fd < 0) return 0;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size <= 0) return 0;

    /* 先跳过文件末尾的 '\n'（tail 不算空尾巴） */
    off_t end = size;
    if (lseek(fd, end - 1, SEEK_SET) >= 0) {
        char c = '\0';
        if (read(fd, &c, 1) == 1 && c == '\n')
            end--;
    }

    if (end <= 0) return 0;

    /* 从 end 向前找第 nlines 个 '\n' */
    const int BS = 4096;
    char buf[BS];
    off_t pos = end;
    int found = 0;

    while (pos > 0) {
        off_t chunk_sz = (pos > BS) ? BS : pos;
        pos -= chunk_sz;
        if (lseek(fd, pos, SEEK_SET) < 0) return 0;
        ssize_t n = read(fd, buf, (size_t)chunk_sz);
        if (n <= 0) break;
        for (ssize_t i = n - 1; i >= 0; --i) {
            if (buf[i] == '\n') {
                found++;
                if (found >= nlines) {
                    /* 返回 '\n' 之后的位置 */
                    return pos + i + 1;
                }
            }
        }
    }
    return 0;  /* 总行数不足 nlines，从头返回 */
}

size_t log_ring_tail(char *out_buf, size_t out_sz, int max_lines)
{
    if (max_lines <= 0) max_lines = 200;
    if (max_lines > 5000) max_lines = 5000;
    if (!out_buf || out_sz == 0) return 0;

    /* 优先从磁盘文件 tail 最后 N 行（logger 线程已 fsync） */
    if (g_log_path[0]) {
        int fd = open(g_log_path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            size_t total = 0;
            size_t cap = out_sz - 1;
            off_t start = tail_find_start(fd, max_lines);
            if (lseek(fd, start, SEEK_SET) >= 0) {
                char tmp[8192];
                while (total < cap) {
                    ssize_t n = read(fd, tmp,
                                     (size_t)(cap - total) < sizeof(tmp)
                                         ? (cap - total) : sizeof(tmp));
                    if (n <= 0) break;
                    memcpy(out_buf + total, tmp, (size_t)n);
                    total += (size_t)n;
                }
                out_buf[total] = '\0';
            }
            close(fd);
            if (total > 0) return total;
        }
    }

    /* 回退：从 ring buffer 读（logger 可能还没来得及 drain） */
    size_t total = 0;
    int lines = 0;
    char *p = out_buf;
    size_t rem = out_sz - 1;

    pthread_mutex_lock(&g_reg_lock);
    for (ring2_t *r = (ring2_t *)g_rings; r; r = r->next) {
        if (!r->active) continue;
        unsigned long cursor = r->tail;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        while (cursor != r->head && lines < max_lines) {
            log_slot_t *s = &r->slots[cursor];
            uint16_t len = s->len;
            if (rem < (size_t)len) goto done;
            memcpy(p, s->data, len);
            p += len;
            rem -= len;
            total += len;
            lines++;
            cursor = (cursor + 1) & r->mask;
        }
    }
done:
    pthread_mutex_unlock(&g_reg_lock);
    *p = '\0';
    return total;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
