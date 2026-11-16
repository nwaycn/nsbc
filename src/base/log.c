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

/* ★ 关键顺序：先前向声明 + 声明指针变量，再 reg_lock() wrapper，再完整 typedef
 * C 里指针变量声明不依赖完整 struct 定义（只需要前向声明），
 * 但访问成员 r->next / r->active 必须在完整 typedef 之后。 */
struct ring2;                                       /* 前向声明 */
static struct ring2 *g_rings;                       /* 指针变量 — 合法（不访问成员）*/
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_reg_lock_robust = 0;

/* ====== g_reg_lock wrapper — EOWNERDEAD 自动恢复 ======
 *
 * robust mutex 语义（glibc 2.4+）：
 *   若持锁线程 crash / SIGKILL，内核不释放 mutex；
 *   下一个 lock 线程收到 EOWNERDEAD → consistent() → 后续 lock 正常。
 *
 * ★ orphan 标记移到 logger_main drain 内（那里 typedef 完整，
 *   可以安全访问 r->active / r->next；reg_lock 只做最小化处理避免不完整类型错误）。
 * 正常进程 crash 残留普通 mutex → 永远卡死；
 * robust mutex → 最多丢少量 ring 日志（标记为 orphan 跳过），进程继续活。 */
static int reg_lock(void)
{
    int rc = pthread_mutex_lock(&g_reg_lock);
    if (rc == EOWNERDEAD) {
        fprintf(stderr,
                "[log] ★ g_reg_lock EOWNERDEAD (owner crashed) — "
                "will mark rings orphan in logger drain\n");
        pthread_mutex_consistent(&g_reg_lock);  /* 必须！否则下次 lock 还是 EOWNERDEAD */
    }
    return rc;  /* 0=正常；EOWNERDEAD=已处理完（下一轮会是 0）*/
}
static void reg_unlock(void) { pthread_mutex_unlock(&g_reg_lock); }

static pthread_mutex_t g_wake = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static int g_running;
static pthread_t g_thr;

static nsbc_atomic_t g_dropped;
static nsbc_atomic_t g_written;
static int g_write_errs;        /* 连续 write 错误计数；触发 reopen */
static volatile sig_atomic_t g_reopen_pending;  /* SIGHUP 触发的 reopen 请求 (async-signal-safe) */

/* ---- per-thread ring（完整定义 — 在 reg_lock() 之后声明，之后所有函数都能用 ring2_t 访问成员）---- */
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
    reg_lock();
    r->next = (ring2_t *)g_rings;
    g_rings = r;
    reg_unlock();
    return NSBC_OK;
}

void log_thread_detach(void)
{
    ring2_t *r = (ring2_t *)pthread_getspecific(g_key);
    if (!r)
        return;
    r->active = 0;
    pthread_setspecific(g_key, NULL);
    reg_lock();
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
    reg_unlock();
    free(r->slots);
    free(r);
}

/* ---- size-based rotate（参考 zlog / systemd-journald 设计）----
 *
 * rotate 必须在 g_reg_lock 内执行（见 logger_main），保证 drain 完后
 * 没有并发 write(g_fd) 期间做 fd 切换。
 *
 * 锁内操作序列（绝对不能变序）：
 *   fsync(old_fd)  →  close(old_fd)  →  rename 链  →  open(new_fd)  →  g_fd=new_fd
 *
 * 关键设计：
 *   1. 先 fsync 再 close：close 后 inode 虽然还活着但已经不在目录项里，
 *      close 前 fsync 是最后一次"在正确的文件上"落盘的机会
 *   2. close 再 rename：rename 时 fd 已经关了，避免 rename 后 inode 消失
 *      但 fd 还能 write 造成的"黑洞日志"
 *   3. open 带 O_DSYNC：让 write 自带同步语义（内核保证数据块写盘成功
 *      才返回），替代 logger_main 里每轮独立 fsync —— 内核可以合并
 *      多次 write 的 sync 操作，温和得多
 *   4. open 带 O_NOATIME：不让内核更新 access time，减少 metadata write
 *
 * rate limit：每小时最多 rotate 一次，防止 open 持续失败时高频 syscall
 * 失败策略：不重置 g_cur_size —— 下次 drain 继续尝试（open 失败常见原因
 *   是磁盘临时满 / 权限瞬时丢失，重置 size 会无限推迟下次机会）
 */
static off_t g_cur_size;
static off_t g_rotate_max = 50 * 1024LL * 1024LL;
static int   g_rotate_keep = 5;
static time_t g_last_rotate_time;
#define NSBC_ROTATE_MIN_INTERVAL_SEC 3600

/* open flags（所有 log open 统一复用） —— O_DSYNC 替代每轮 fsync */
#define LOG_OPEN_FLAGS (O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC \
                       | O_DSYNC | O_NOATIME)
#define LOG_OPEN_MODE  0640

/* 内部：在 g_reg_lock 内调用。old_fd 必须 >= 0（已 fsync + 要被 close）。
 * 返回 1 表示 rotate 成功（已切换 g_fd），0 表示失败（old_fd 已 close）。*/
static int do_rotate_locked(int old_fd)
{
    if (!g_log_path[0] || g_rotate_max <= 0) return 0;

    /* rate limit：避免 open 持续失败时每次 drain 都调用 rename/unlink */
    time_t now = time(NULL);
    if (g_last_rotate_time > 0 && now - g_last_rotate_time < NSBC_ROTATE_MIN_INTERVAL_SEC)
        return 0;

    /* ---- 阶段 1: fsync + close 旧 fd（必须在 rename 之前！）---- */
    if (old_fd >= 0) {
        (void)fsync(old_fd);
        close(old_fd);     /* ★ 先关再 rename — 避免 rename 后 fd write 黑洞 */
    }

    /* ---- 阶段 2: rename 链 ---- */
    char dst[PATH_MAX];
    snprintf(dst, sizeof(dst), "%s.%d", g_log_path, g_rotate_keep);
    unlink(dst);
    for (int i = g_rotate_keep - 1; i >= 1; i--) {
        char src[PATH_MAX];
        snprintf(src, sizeof(src), "%s.%d", g_log_path, i);
        snprintf(dst, sizeof(dst), "%s.%d", g_log_path, i + 1);
        rename(src, dst);  /* ENOENT 正常 */
    }
    snprintf(dst, sizeof(dst), "%s.1", g_log_path);
    rename(g_log_path, dst);

    /* ---- 阶段 3: open 新 fd ---- */
    int new_fd = open(g_log_path, LOG_OPEN_FLAGS, LOG_OPEN_MODE);
    if (new_fd < 0) {
        g_fd = -1;
        fprintf(stderr,
                "[log-rotate] open NEW FAIL errno=%d(%s) path=%s — "
                "fallback stderr, WILL retry next cycle\n",
                errno, strerror(errno), g_log_path);
        g_last_rotate_time = now;
        return 0;
    }

    g_fd = new_fd;
    g_cur_size = 0;
    g_write_errs = 0;
    g_last_rotate_time = now;
    fprintf(stderr,
            "[log-rotate] %s → %s.1 (limit %lld MB, keep %d)\n",
            g_log_path, g_log_path,
            (long long)(g_rotate_max / (1024 * 1024)), g_rotate_keep);
    return 1;
}

static void __attribute__((used)) sig_handler_hup(int sig) {
    (void)sig;
    g_reopen_pending = 1;  /* sig_atomic_t — async-signal-safe */
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
        struct timespec wd_t0;
        clock_gettime(CLOCK_MONOTONIC, &wd_t0);

        reg_lock();  /* robust mutex */

        int wd_n_rings = 0;
        ring2_t *r = (ring2_t *)g_rings;
        while (r) {
            wd_n_rings++;
            if (!r->active) {
                r = r->next;
                continue;
            }
            for (;;) {
                unsigned long t = r->tail;
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                if (t == r->head) break;
                log_slot_t *s = &r->slots[t];

                if (g_fd >= 0) {
                    ssize_t w = write(g_fd, s->data, s->len);
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        g_write_errs++;
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
                        if (g_write_errs >= 100 && g_log_path[0]) {
                            int old_fd = g_fd;
                            close(old_fd);
                            g_fd = open(g_log_path, LOG_OPEN_FLAGS, LOG_OPEN_MODE);
                            if (g_fd >= 0) {
                                fprintf(stderr,
                                        "[log-flush] reopened fd %d → %d after %d errors\n",
                                        old_fd, g_fd, g_write_errs);
                                g_write_errs = 0;
                            } else {
                                g_fd = -1;
                                g_write_errs = 0;
                                fprintf(stderr,
                                        "[log-flush] reopen FAIL errno=%d(%s), "
                                        "fallback stderr — will retry next cycle\n",
                                        errno, strerror(errno));
                            }
                        }
                    } else {
                        g_write_errs = 0;
                        g_cur_size += w;
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

        /* ====== drain 完成 — 锁内做 fd 管理 ====== */
        if (g_fd >= 0 && g_rotate_max > 0 && g_cur_size >= g_rotate_max) {
            int old_fd = g_fd;
            do_rotate_locked(old_fd);
        }

        if (g_reopen_pending) {
            g_reopen_pending = 0;
            if (g_log_path[0]) {
                int old = g_fd;
                int nfd = open(g_log_path, LOG_OPEN_FLAGS, LOG_OPEN_MODE);
                if (nfd >= 0) {
                    g_fd = nfd;
                    if (old >= 0) close(old);
                    g_write_errs = 0;
                    g_cur_size = 0;
                    fprintf(stderr,
                            "[log-flush] SIGHUP reopen OK fd=%d → %d path=%s\n",
                            old, nfd, g_log_path);
                } else {
                    fprintf(stderr,
                            "[log-flush] SIGHUP reopen FAIL errno=%d(%s) path=%s "
                            "(keep old fd=%d)\n",
                            errno, strerror(errno), g_log_path, old);
                }
            }
        }

        reg_unlock();

        /* watchdog */
        {
            struct timespec wd_t1;
            clock_gettime(CLOCK_MONOTONIC, &wd_t1);
            int64_t wd_ns = (int64_t)(wd_t1.tv_sec - wd_t0.tv_sec) * 1000000000LL
                          + (int64_t)wd_t1.tv_nsec - (int64_t)wd_t0.tv_nsec;
            uint64_t wd_ms = wd_ns > 0 ? (uint64_t)wd_ns / 1000000ULL : 0;
            if (wd_ms > 500 && any) {
                static uint32_t wd_count;
                if (wd_count++ % 10 == 0) {
                    fprintf(stderr,
                            "[log-watchdog] ★ drain+fd-mgmt took %lu ms "
                            "n_rings=%d (disk stall?)\n",
                            (unsigned long)wd_ms, wd_n_rings);
                }
            }
        }

        if (!g_running && !any) break;
        pthread_mutex_lock(&g_wake);
        if (!g_running) {
            pthread_mutex_unlock(&g_wake);
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
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
        mkdir_p(path);
        nfd = open(path, LOG_OPEN_FLAGS, LOG_OPEN_MODE);
        if (nfd < 0) {
            fprintf(stderr, "log_init: open(%s) failed errno=%d (%s)\n",
                    path, errno, strerror(errno));
            return NSBC_ERR;
        }
        struct stat st;
        g_cur_size = (fstat(nfd, &st) == 0) ? st.st_size : 0;
    } else {
        g_cur_size = 0;
    }
    if (g_running) {
        int old = g_fd;
        reg_lock();
        if (old >= 0) close(old);
        reg_unlock();
        g_fd = nfd;
        return NSBC_OK;
    }
    g_fd = nfd;
    g_running = 1;
    {
        pthread_condattr_t cattr;
        pthread_condattr_init(&cattr);
        pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
        pthread_cond_init(&g_cond, &cattr);
        pthread_condattr_destroy(&cattr);
    }
    /* ★ ROBUST MUTEX: g_reg_lock 在创建 logger 线程之前初始化成
     *   robust + errorcheck 属性。crash 后下次 lock 收到 EOWNERDEAD →
     *   reg_lock() 调 pthread_mutex_consistent() → 进程继续活。 */
    {
        pthread_mutexattr_t mattr;
        pthread_mutexattr_init(&mattr);
        pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
        pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_ERRORCHECK);
        pthread_mutex_init(&g_reg_lock, &mattr);
        pthread_mutexattr_destroy(&mattr);
        g_reg_lock_robust = 1;
    }
    pthread_once(&g_key_once, key_once);
    if (pthread_create(&g_thr, NULL, logger_main, NULL) != 0) {
        g_running = 0;
        if (g_fd >= 0)
            close(g_fd);
        return NSBC_ERR;
    }
    atexit(log_stop);
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
        return NSBC_OK;
    mkdir_p(path);
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

void log_set_rotate(long long max_bytes, int keep)
{
    if (max_bytes <= 0) {
        g_rotate_max = -1;
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
    ssize_t w = write(fd, buf, (size_t)n);
    (void)w;
}

uint64_t log_dropped(void) { return sat_read(g_dropped); }
uint64_t log_written(void) { return sat_read(g_written); }

static off_t tail_find_start(int fd, int nlines)
{
    if (fd < 0) return 0;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size <= 0) return 0;
    off_t end = size;
    if (lseek(fd, end - 1, SEEK_SET) >= 0) {
        char c = '\0';
        if (read(fd, &c, 1) == 1 && c == '\n')
            end--;
    }
    if (end <= 0) return 0;
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
                    return pos + i + 1;
                }
            }
        }
    }
    return 0;
}

size_t log_ring_tail(char *out_buf, size_t out_sz, int max_lines)
{
    if (max_lines <= 0) max_lines = 200;
    if (max_lines > 5000) max_lines = 5000;
    if (!out_buf || out_sz == 0) return 0;

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

    size_t total = 0;
    int lines = 0;
    char *p = out_buf;
    size_t rem = out_sz - 1;

    reg_lock();
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
    reg_unlock();
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
