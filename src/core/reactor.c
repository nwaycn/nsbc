#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reactor.h"
#include "nsbc_atomic.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    reactor_cb_fn cb;
    void *arg;
} slot_t;

typedef struct reactor_call {
    struct reactor_call *next;
    reactor_task_fn fn;
    void *arg;
    pthread_mutex_t lock;
    pthread_cond_t done_cond;
    int result;
    int done;
} reactor_call_t;

struct reactor {
    int epfd;
    int wake_fd;
    int max_events;
    struct epoll_event *events;
    slot_t *slots;     /* fd → 回调（fd 直查，容量=fd 高水位） */
    int slots_cap;
    nsbc_atomic_t stop;
    pthread_t owner;
    pthread_mutex_t task_lock;
    reactor_call_t *task_head;
    reactor_call_t *task_tail;
    uint32_t task_count;
    int task_queue_initialized;
    int task_started;
    int task_closing;
    int wait_active;
    uint64_t task_submitted;
    uint64_t task_completed;
    uint64_t task_rejected;
};

static void reactor_wake(reactor_t *r)
{
    uint64_t one = 1;
    ssize_t n;
    do {
        n = write(r->wake_fd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EAGAIN)
        fprintf(stderr, "reactor: wake eventfd write failed errno=%d\n",
                errno);
}

static void reactor_drain_wake_fd(reactor_t *r)
{
    uint64_t value;
    for (;;) {
        ssize_t n = read(r->wake_fd, &value, sizeof(value));
        if (n == (ssize_t)sizeof(value))
            continue;
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return;
        if (n == 0)
            return;
        fprintf(stderr, "reactor: wake eventfd read failed errno=%d\n",
                errno);
        return;
    }
}

static unsigned reactor_run_tasks(reactor_t *r, unsigned budget)
{
    unsigned done = 0;
    while (done < budget) {
        pthread_mutex_lock(&r->task_lock);
        reactor_call_t *task = r->task_head;
        if (task) {
            r->task_head = task->next;
            if (!r->task_head)
                r->task_tail = NULL;
            --r->task_count;
        }
        pthread_mutex_unlock(&r->task_lock);
        if (!task)
            break;

        int result = task->fn(task->arg);
        pthread_mutex_lock(&task->lock);
        task->result = result;
        task->done = 1;
        int signal_rc = pthread_cond_signal(&task->done_cond);
        if (signal_rc != 0) {
            fprintf(stderr, "reactor: command completion signal failed rc=%d\n",
                    signal_rc);
            abort();
        }
        pthread_mutex_unlock(&task->lock);

        pthread_mutex_lock(&r->task_lock);
        ++r->task_completed;
        int pending = r->task_count != 0;
        pthread_mutex_unlock(&r->task_lock);
        ++done;

        if (reactor_stopping(r))
            budget = UINT_MAX;
        else if (pending && done >= budget)
            reactor_wake(r);
    }
    return done;
}

reactor_t *reactor_create(int max_events)
{
    reactor_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->max_events = max_events > 0 ? max_events : 256;
    r->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (r->epfd < 0) {
        free(r);
        return NULL;
    }
    r->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (r->wake_fd < 0) {
        close(r->epfd);
        free(r);
        return NULL;
    }
    struct epoll_event wake_event;
    memset(&wake_event, 0, sizeof(wake_event));
    wake_event.events = EPOLLIN;
    wake_event.data.fd = r->wake_fd;
    if (epoll_ctl(r->epfd, EPOLL_CTL_ADD, r->wake_fd, &wake_event) < 0) {
        close(r->wake_fd);
        close(r->epfd);
        free(r);
        return NULL;
    }
    r->events = calloc((size_t)r->max_events, sizeof(struct epoll_event));
    r->slots_cap = 1024;
    r->slots = calloc((size_t)r->slots_cap, sizeof(slot_t));
    if (!r->events || !r->slots) {
        close(r->wake_fd);
        close(r->epfd);
        free(r->events);
        free(r->slots);
        free(r);
        return NULL;
    }
    if (pthread_mutex_init(&r->task_lock, NULL) != 0) {
        close(r->wake_fd);
        close(r->epfd);
        free(r->events);
        free(r->slots);
        free(r);
        return NULL;
    }
    r->task_queue_initialized = 1;
    r->owner = pthread_self();
    return r;
}

int reactor_destroy(reactor_t *r)
{
    if (!r)
        return NSBC_OK;
#ifdef NSBC_REACTOR_OWNER_CHECK
    /* P3: 多 reactor 场景下 reactor[1..N-1] 线程退出时自 destroy，
     * 但 reactor owner 是 main 线程（预创建时设定）。生产构建关闭此检查。
     * wait_active=0 + 无 pending commands 保证安全。 */
    if (!pthread_equal(pthread_self(), r->owner)) {
        fprintf(stderr, "reactor: destroy must run on the owner thread (debug check)\n");
        return NSBC_PERM_DENIED;
    }
#endif
    if (r->wait_active) {
        fprintf(stderr, "reactor: destroy refused while reactor_wait is active\n");
        return NSBC_BUSY;
    }
    if (r->task_queue_initialized) {
        pthread_mutex_lock(&r->task_lock);
        r->task_closing = 1;
        uint32_t pending = r->task_count;
        if (pending != 0) {
            pthread_mutex_unlock(&r->task_lock);
            fprintf(stderr, "reactor: destroy refused with %u queued commands\n",
                    pending);
            return NSBC_BUSY;
        }
        pthread_mutex_unlock(&r->task_lock);
        pthread_mutex_destroy(&r->task_lock);
    }
    close(r->wake_fd);
    close(r->epfd);
    free(r->events);
    free(r->slots);
    free(r);
    return NSBC_OK;
}

static int slot_ensure(reactor_t *r, int fd)
{
    if (!r || fd < 0)
        return NSBC_INVAL;
    if (fd < r->slots_cap)
        return NSBC_OK;
    int nc = r->slots_cap;
    while (nc <= fd) {
        if (nc > INT_MAX / 2)
            return NSBC_NOSPC;
        nc *= 2;
    }
    if ((size_t)nc > SIZE_MAX / sizeof(slot_t))
        return NSBC_NOMEM;
    slot_t *ns = realloc(r->slots, (size_t)nc * sizeof(slot_t));
    if (!ns)
        return NSBC_NOMEM;
    memset(ns + r->slots_cap, 0,
           (size_t)(nc - r->slots_cap) * sizeof(slot_t));
    r->slots = ns;
    r->slots_cap = nc;
    return NSBC_OK;
}

int reactor_add(reactor_t *r, int fd, uint32_t events, reactor_cb_fn cb,
                void *arg)
{
    if (!r || !cb || fd == r->wake_fd)
        return NSBC_INVAL;
#ifdef NSBC_REACTOR_OWNER_CHECK
    if (!pthread_equal(pthread_self(), r->owner)) {
        fprintf(stderr,
                "reactor: add must run on owner thread (debug check)\n");
        return NSBC_PERM_DENIED;
    }
#endif
    int rc = slot_ensure(r, fd);
    if (rc != NSBC_OK)
        return rc;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(r->epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
        return NSBC_ERR;
    r->slots[fd].cb = cb;
    r->slots[fd].arg = arg;
    return NSBC_OK;
}

int reactor_mod(reactor_t *r, int fd, uint32_t events)
{
    if (!r || fd < 0 || fd == r->wake_fd)
        return NSBC_INVAL;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(r->epfd, EPOLL_CTL_MOD, fd, &ev) < 0 ? NSBC_ERR
                                                           : NSBC_OK;
}

int reactor_del(reactor_t *r, int fd)
{
    if (!r || fd < 0 || fd == r->wake_fd)
        return NSBC_INVAL;
#ifdef NSBC_REACTOR_OWNER_CHECK
    if (!pthread_equal(pthread_self(), r->owner)) {
        fprintf(stderr,
                "reactor: del must run on owner thread (debug check)\n");
        return NSBC_PERM_DENIED;
    }
#endif
    if (epoll_ctl(r->epfd, EPOLL_CTL_DEL, fd, NULL) < 0)
        return NSBC_ERR;
    if (fd >= 0 && fd < r->slots_cap) {
        r->slots[fd].cb = NULL;
        r->slots[fd].arg = NULL;
    }
    return NSBC_OK;
}

int reactor_call(reactor_t *r, reactor_task_fn fn, void *arg)
{
    if (!r || !fn)
        return NSBC_INVAL;
    if (pthread_equal(pthread_self(), r->owner))
        return fn(arg);

    reactor_call_t task;
    memset(&task, 0, sizeof(task));
    task.fn = fn;
    task.arg = arg;
    if (pthread_mutex_init(&task.lock, NULL) != 0)
        return NSBC_ERR;
    if (pthread_cond_init(&task.done_cond, NULL) != 0) {
        pthread_mutex_destroy(&task.lock);
        return NSBC_ERR;
    }

    pthread_mutex_lock(&r->task_lock);
    if (r->task_closing) {
        ++r->task_rejected;
        pthread_mutex_unlock(&r->task_lock);
        pthread_cond_destroy(&task.done_cond);
        pthread_mutex_destroy(&task.lock);
        return NSBC_SHUTDOWN;
    }
    if (!r->task_started) {
        ++r->task_rejected;
        pthread_mutex_unlock(&r->task_lock);
        pthread_cond_destroy(&task.done_cond);
        pthread_mutex_destroy(&task.lock);
        return NSBC_AGAIN;
    }
    if (r->task_count >= REACTOR_TASK_QUEUE_CAP) {
        ++r->task_rejected;
        pthread_mutex_unlock(&r->task_lock);
        pthread_cond_destroy(&task.done_cond);
        pthread_mutex_destroy(&task.lock);
        return NSBC_NOSPC;
    }
    if (r->task_tail)
        r->task_tail->next = &task;
    else
        r->task_head = &task;
    r->task_tail = &task;
    ++r->task_count;
    ++r->task_submitted;
    pthread_mutex_unlock(&r->task_lock);
    reactor_wake(r);

    pthread_mutex_lock(&task.lock);
    while (!task.done) {
        int wait_rc = pthread_cond_wait(&task.done_cond, &task.lock);
        if (wait_rc != 0) {
            fprintf(stderr, "reactor: command wait failed rc=%d\n", wait_rc);
            abort();
        }
    }
    int result = task.result;
    pthread_mutex_unlock(&task.lock);
    pthread_cond_destroy(&task.done_cond);
    pthread_mutex_destroy(&task.lock);
    return result;
}

void reactor_get_task_stats(const reactor_t *r, reactor_task_stats_t *out)
{
    if (!r || !out)
        return;
    pthread_mutex_lock((pthread_mutex_t *)&r->task_lock);
    out->queued = r->task_count;
    out->submitted = r->task_submitted;
    out->completed = r->task_completed;
    out->rejected = r->task_rejected;
    pthread_mutex_unlock((pthread_mutex_t *)&r->task_lock);
}

void reactor_stop(reactor_t *r)
{
    if (!r)
        return;
    pthread_mutex_lock(&r->task_lock);
    r->task_closing = 1;
    sat_write(r->stop, 1);
    pthread_mutex_unlock(&r->task_lock);
    reactor_wake(r);
}

int reactor_stopping(const reactor_t *r)
{
    return r && sat_read(((reactor_t *)r)->stop) != 0;
}

int reactor_wait(reactor_t *r, int timeout_ms)
{
    if (!r)
        return NSBC_INVAL;
#ifdef NSBC_REACTOR_OWNER_CHECK
    /* P3: 多 reactor 场景下 reactor[1..N-1] 的 reactor 在线程启动前预创建（owner=main），
     * 然后各 pthread 调 reactor_wait。owner 检查在 debug 构建开启，生产构建关闭。
     * wait_active 标志已防护多线程并发 reactor_wait。 */
    if (!pthread_equal(pthread_self(), r->owner))
        return NSBC_PERM_DENIED;
#endif
    if (r->wait_active)
        return NSBC_BUSY;
    r->wait_active = 1;
    pthread_mutex_lock(&r->task_lock);
    r->task_started = 1;
    pthread_mutex_unlock(&r->task_lock);
    if (reactor_stopping(r)) {
        (void)reactor_run_tasks(r, UINT_MAX);
        r->wait_active = 0;
        return 0;
    }
    int n;
    do {
        n = epoll_wait(r->epfd, r->events, r->max_events, timeout_ms);
    } while (n < 0 && errno == EINTR && !reactor_stopping(r));
    if (n < 0) {
        if (reactor_stopping(r)) {
            (void)reactor_run_tasks(r, UINT_MAX);
            r->wait_active = 0;
            return 0;
        }
        pthread_mutex_lock(&r->task_lock);
        r->task_closing = 1;
        sat_write(r->stop, 1);
        pthread_mutex_unlock(&r->task_lock);
        (void)reactor_run_tasks(r, UINT_MAX);
        r->wait_active = 0;
        return NSBC_ERR;
    }
    if (reactor_stopping(r)) {
        (void)reactor_run_tasks(r, UINT_MAX);
        r->wait_active = 0;
        return 0;
    }
    /* Reactor 慢处理 watchdog：
     * epoll_wait 返回后，若单个 fd callback 链总耗时 > 500ms → 打限频 WARN。
     * 慢速连接 / TLS 握手卡住 / 大包解析阻塞都会触发 — 帮运维定位 reactor 线程瓶颈。
     * 用 CLOCK_MONOTONIC 不受 NTP 跳变影响。 */
    struct timespec t0, t1;
    int have_t0 = clock_gettime(CLOCK_MONOTONIC, &t0) == 0;
    for (int i = 0; i < n; ++i) {
        if (reactor_stopping(r)) {
            (void)reactor_run_tasks(r, UINT_MAX);
            break;
        }
        int fd = r->events[i].data.fd;
        if (fd == r->wake_fd) {
            reactor_drain_wake_fd(r);
            (void)reactor_run_tasks(r, (unsigned)r->max_events);
            continue;
        }
        if (fd >= r->slots_cap || !r->slots[fd].cb)
            continue;
        r->slots[fd].cb(fd, r->events[i].events, r->slots[fd].arg);
    }
    if (reactor_stopping(r))
        (void)reactor_run_tasks(r, UINT_MAX);
    else
        (void)reactor_run_tasks(r, (unsigned)r->max_events);
    if (have_t0 && clock_gettime(CLOCK_MONOTONIC, &t1) == 0) {
        int64_t elapsed_ns = (int64_t)(t1.tv_sec - t0.tv_sec) * 1000000000LL
                           + (int64_t)t1.tv_nsec - (int64_t)t0.tv_nsec;
        uint64_t elapsed_ms = elapsed_ns > 0 ? (uint64_t)elapsed_ns / 1000000ULL : 0;
        if (elapsed_ms > 500) {
            static uint32_t wd_n = 0;
            if (wd_n++ % 10 == 0)
                fprintf(stderr, "reactor_warn: slow batch %lu ms n=%d (wd_n=%u)\n",
                        (unsigned long)elapsed_ms, n, wd_n);
        }
    }
    r->wait_active = 0;
    return n;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
