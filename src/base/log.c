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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
                    if (write(g_fd, s->data, s->len) < 0 && errno == EINTR) {}
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
        if (g_fd >= 0)
            fsync(g_fd); /* 批量落盘；可配置为间隔 fsync，P0 先保守 */
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
    }
    if (g_running) {
        /* 两阶段启动：第二次调用仅热切换输出 fd，不重复建日志线程 */
        int old = g_fd;
        g_fd = nfd;
        if (old >= 0)
            close(old);
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
