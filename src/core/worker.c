#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "worker.h"
#include "log.h"
#include "metric_reg.h"

#include <errno.h>
#include <limits.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WORKER_WAIT_BUCKETS 7
#define WORKER_SUBMIT_CLOSING (1UL << (sizeof(unsigned long) * CHAR_BIT - 1))
#define WORKER_SUBMIT_COUNT_MASK (WORKER_SUBMIT_CLOSING - 1)

typedef struct {
    unsigned id;
    pthread_t thr;
    int started;           /* thread 已创建并在运行 */
    nsbc_atomic_t slot_ready;  /* F-02 修复：原子，submitter 无锁读验证 slot 存活 */
    /* ★ P0-2.3 RACE-FIX: q_refcnt. 保护 w->q 生命周期。
     * submit 侧 fetch_add 在 double-check slot_ready 后、mem_mpsc_push 前；
     * push 完 fetch_sub。scale-down 在 mem_mpsc_destroy 前 while(refcnt>0) yield。
     * 否则 submit double-check(slot_ready=1) → break → mem_mpsc_push 之间
     * scale-down 可能 destroy q 并置 NULL → submit 访问已 free queue → UAF。 */
    nsbc_atomic_t q_refcnt;
    sem_t ready;
    int ready_initialized;
    mem_mag_t *mag;
    mem_rcu_reader_t *rd;
    mem_mpsc_t *q;
    worker_pool_t *pool;
    nsbc_atomic_t draining;  /* P3: scale-down 时置 1，worker drain queue 后 self-exit */
} worker_t;

struct worker_pool {
    unsigned max_workers;
    nsbc_atomic_t n_active;  /* P3: atomic，submitter 无锁读 */
    worker_t *w;             /* 预分配 max_workers 槽位，永不 realloc */
    mem_rcu_t *rcu;
    int own_rcu;
    nsbc_atomic_t rr;
    nsbc_atomic_t submit_state;
    pthread_mutex_t submit_wait_lock;
    pthread_cond_t submit_wait_cond;
    int submit_wait_initialized;
    pthread_mutex_t resize_lock;   /* P3: 保护 scale-up/down 并发 */
    int resize_lock_initialized;
    int metrics_registered;
    int running;
    uint64_t active;
    uint64_t submitted;
    uint64_t completed;
    uint64_t rejected;
    uint64_t wait_buckets[WORKER_WAIT_BUCKETS];
    uint64_t wait_sum_ns;
};

static int worker_submit_enter(worker_pool_t *p)
{
    unsigned long state =
        __atomic_load_n(&p->submit_state, __ATOMIC_ACQUIRE);
    for (;;) {
        if (state & WORKER_SUBMIT_CLOSING)
            return NSBC_SHUTDOWN;
        if ((state & WORKER_SUBMIT_COUNT_MASK) == WORKER_SUBMIT_COUNT_MASK)
            return NSBC_BUSY;
        if (__atomic_compare_exchange_n(&p->submit_state, &state, state + 1,
                                        0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return NSBC_OK;
    }
}

static void worker_submit_leave(worker_pool_t *p)
{
    unsigned long old =
        __atomic_fetch_sub(&p->submit_state, 1, __ATOMIC_RELEASE);
    if ((old & WORKER_SUBMIT_CLOSING) &&
        (old & WORKER_SUBMIT_COUNT_MASK) == 1) {
        int rc = pthread_mutex_lock(&p->submit_wait_lock);
        if (rc != 0) {
            log_warn("worker", "submit shutdown lock failed rc=%d", rc);
            abort();
        }
        rc = pthread_cond_broadcast(&p->submit_wait_cond);
        if (rc != 0) {
            log_warn("worker", "submit shutdown signal failed rc=%d", rc);
            abort();
        }
        pthread_mutex_unlock(&p->submit_wait_lock);
    }
}

static void worker_close_submit_gate(worker_pool_t *p)
{
    __atomic_fetch_or(&p->submit_state, WORKER_SUBMIT_CLOSING,
                      __ATOMIC_ACQ_REL);
    int rc = pthread_mutex_lock(&p->submit_wait_lock);
    if (rc != 0) {
        log_warn("worker", "submit shutdown lock failed rc=%d", rc);
        abort();
    }
    while ((__atomic_load_n(&p->submit_state, __ATOMIC_ACQUIRE) &
            WORKER_SUBMIT_COUNT_MASK) != 0) {
        rc = pthread_cond_wait(&p->submit_wait_cond, &p->submit_wait_lock);
        if (rc != 0) {
            log_warn("worker", "submit shutdown wait failed rc=%d", rc);
            abort();
        }
    }
    pthread_mutex_unlock(&p->submit_wait_lock);
}

static uint64_t worker_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int render_worker_metrics(char *buf, size_t cap, void *ud)
{
    worker_pool_t *p = (worker_pool_t *)ud;
    uint64_t active = __atomic_load_n(&p->active, __ATOMIC_RELAXED);
    uint64_t submitted = __atomic_load_n(&p->submitted, __ATOMIC_RELAXED);
    uint64_t completed = __atomic_load_n(&p->completed, __ATOMIC_RELAXED);
    uint64_t rejected = __atomic_load_n(&p->rejected, __ATOMIC_RELAXED);
    uint64_t wait_sum = __atomic_load_n(&p->wait_sum_ns, __ATOMIC_RELAXED);
    unsigned n_active = __atomic_load_n(&p->n_active, __ATOMIC_RELAXED);

    /* 只扫 n_active 槽位——max_workers 可能更大但未初始化 */
    uint64_t queued = 0;
    for (unsigned i = 0; i < n_active && i < p->max_workers; ++i) {
        if (p->w[i].slot_ready)
            queued += mem_mpsc_size(p->w[i].q);
    }

    int n = snprintf(buf, cap,
                     "# TYPE nsbc_worker_active gauge\n"
                     "# TYPE nsbc_worker_queued gauge\n"
                     "# TYPE nsbc_worker_submitted_total counter\n"
                     "# TYPE nsbc_worker_completed_total counter\n"
                     "# TYPE nsbc_worker_rejected_total counter\n"
                     "# TYPE nsbc_worker_queue_wait_nanoseconds_total counter\n"
                     "# TYPE nsbc_worker_queue_wait_seconds histogram\n"
                     "# TYPE nsbc_worker_threads_active gauge\n"
                     "# TYPE nsbc_worker_threads_configured gauge\n"
                     "nsbc_worker_threads_active %u\n"
                     "nsbc_worker_threads_configured %u\n"
                     "nsbc_worker_active %llu\n"
                     "nsbc_worker_queued %llu\n"
                     "nsbc_worker_submitted_total %llu\n"
                     "nsbc_worker_completed_total %llu\n"
                     "nsbc_worker_rejected_total %llu\n"
                     "nsbc_worker_queue_wait_nanoseconds_total %llu\n",
                     n_active, p->max_workers, (unsigned long long)active,
                     (unsigned long long)queued, (unsigned long long)submitted,
                     (unsigned long long)completed, (unsigned long long)rejected,
                     (unsigned long long)wait_sum);
    if (n < 0 || (size_t)n >= cap)
        return n;

    static const char *bounds_seconds[WORKER_WAIT_BUCKETS - 1] = {
        "0.00001", "0.0001", "0.001", "0.01", "0.1", "1"
    };
    uint64_t cumulative = 0;
    for (unsigned i = 0; i < WORKER_WAIT_BUCKETS; ++i) {
        cumulative += __atomic_load_n(&p->wait_buckets[i], __ATOMIC_RELAXED);
        int written;
        if (i < WORKER_WAIT_BUCKETS - 1) {
            written = snprintf(buf + n, cap - (size_t)n,
                               "nsbc_worker_queue_wait_seconds_bucket{le=\"%s\"} %llu\n",
                               bounds_seconds[i], (unsigned long long)cumulative);
        } else {
            written = snprintf(buf + n, cap - (size_t)n,
                               "nsbc_worker_queue_wait_seconds_bucket{le=\"+Inf\"} %llu\n",
                               (unsigned long long)cumulative);
        }
        if (written < 0 || (size_t)written >= cap - (size_t)n)
            return (int)cap;
        n += written;
    }
    int tail = snprintf(buf + n, cap - (size_t)n,
                        "nsbc_worker_queue_wait_seconds_count %llu\n"
                        "nsbc_worker_queue_wait_seconds_sum %.9f\n",
                        (unsigned long long)cumulative,
                        (double)wait_sum / 1000000000.0);
    if (tail < 0)
        return tail;
    return (size_t)tail >= cap - (size_t)n ? (int)cap : n + tail;
}

/* P3: worker 线程主循环。
 * 与原版区别：
 *   - draining=1 时继续 drain 自己 queue 直到空 → self-exit
 *   - running=0（shutdown）且 queue 空时退出
 *   - 线程退出前 self-free 自己 slot 资源（mag/rd/q），不 destroy pool 结构 */
static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    log_thread_attach();
    for (;;) {
        /* P3: draining 或 shutdown 且 queue 空 → 退出 */
        int draining = __atomic_load_n(&w->draining, __ATOMIC_ACQUIRE);
        if (draining && mem_mpsc_size(w->q) == 0)
            break;
        if (!__atomic_load_n(&w->pool->running, __ATOMIC_ACQUIRE) &&
            mem_mpsc_size(w->q) == 0)
            break;

        while (sem_wait(&w->ready) != 0) {
            if (errno == EINTR)
                continue;
            log_warn("worker", "sem_wait failed worker=%u errno=%d", w->id,
                     errno);
            goto done;
        }

        void *msg = NULL;
        if (mem_mpsc_pop(w->q, &msg) == NSBC_OK) {
            worker_job_t *job = (worker_job_t *)msg;
            uint64_t now = worker_now_ns();
            uint64_t wait_ns = now >= job->enqueued_ns
                             ? now - job->enqueued_ns : 0;
            unsigned bucket = wait_ns <= 10000ULL ? 0
                            : wait_ns <= 100000ULL ? 1
                            : wait_ns <= 1000000ULL ? 2
                            : wait_ns <= 10000000ULL ? 3
                            : wait_ns <= 100000000ULL ? 4
                            : wait_ns <= 1000000000ULL ? 5 : 6;
            __atomic_fetch_add(&w->pool->wait_buckets[bucket], 1,
                               __ATOMIC_RELAXED);
            __atomic_fetch_add(&w->pool->wait_sum_ns, wait_ns,
                               __ATOMIC_RELAXED);
            __atomic_add_fetch(&w->pool->active, 1, __ATOMIC_RELAXED);
            job->fn(job->arg);
            __atomic_sub_fetch(&w->pool->active, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&w->pool->completed, 1, __ATOMIC_RELAXED);
            continue;
        }
    }

done:
    /* P3: worker 线程退出 — 只做 thread-local 清理，不改 pool 共享状态。
     * pool 共享字段（started/slot_ready/mag/rd/q/sem）由调用方在持有 resize_lock
     * 的情况下 join 后统一清理（wpool_resize scale-down / wpool_destroy）。
     *
     * mem_rcu_thread_leave 必须留在本线程做（RCU reader 是 thread-local 状态，
     * 其他线程调无意义甚至 UB）。其他 pool 资源（mag/q/sem）可以在任何线程释放。 */
    if (w->rd) mem_rcu_thread_leave(w->rd);
    log_thread_detach();
    return NULL;
}

/* P3: 初始化 worker slot 资源（sem/q/mag/rd）+ 启动线程。
 * 必须在 p->resize_lock 保护下调用。
 * 失败时只清理本 slot 资源（不炸整个池）——wpool_create 或 wpool_resize
 * 上层负责决定整体失败还是 rollback。 */
static int worker_slot_init(worker_pool_t *p, unsigned idx, uint32_t queue_cap)
{
    worker_t *w = &p->w[idx];
    if (w->slot_ready)
        return NSBC_OK;

    w->id = idx;
    w->pool = p;
    __atomic_store_n(&w->draining, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&w->q_refcnt, 0, __ATOMIC_RELAXED);

    if (sem_init(&w->ready, 0, 0) != 0)
        return NSBC_ERR;
    w->ready_initialized = 1;

    w->q = mem_mpsc_create(queue_cap ? queue_cap : 4096, NULL);
    w->mag = mem_mag_create();
    w->rd = mem_rcu_thread_join(p->rcu);
    if (!w->q || !w->mag || !w->rd) {
        /* 只清理自己 slot 的半成品资源，不调 wpool_destroy */
        if (w->rd) { mem_rcu_thread_leave(w->rd); w->rd = NULL; }
        if (w->mag) { mem_mag_destroy(w->mag); w->mag = NULL; }
        if (w->q)   { mem_mpsc_destroy(w->q);   w->q = NULL; }
        if (w->ready_initialized) { sem_destroy(&w->ready); w->ready_initialized = 0; }
        return NSBC_NOMEM;
    }
    __atomic_store_n(&w->slot_ready, 1, __ATOMIC_RELEASE);

    if (pthread_create(&w->thr, NULL, worker_main, w) != 0) {
        /* pthread_create 失败——slot 资源已 init，线程未启动。
         * 清理 slot 资源后返回错误，调用方决定 rollback 或整体失败。 */
        if (w->rd) { mem_rcu_thread_leave(w->rd); w->rd = NULL; }
        if (w->mag) { mem_mag_destroy(w->mag); w->mag = NULL; }
        if (w->q)   { mem_mpsc_destroy(w->q);   w->q = NULL; }
        if (w->ready_initialized) { sem_destroy(&w->ready); w->ready_initialized = 0; }
        __atomic_store_n(&w->slot_ready, 0, __ATOMIC_RELEASE);
        return NSBC_ERR;
    }
    w->started = 1;
    return NSBC_OK;
}

int wpool_create(worker_pool_t **out, unsigned min_workers, unsigned max_workers,
                 uint32_t queue_cap, mem_rcu_t *rcu)
{
    if (max_workers == 0)
        max_workers = 1;
    if (min_workers == 0)
        min_workers = 1;
    if (min_workers > max_workers)
        min_workers = max_workers;
    if (max_workers > 64) {
        log_warn("worker", "max_workers=%u exceeds 64 cap, clamping to 64",
                 max_workers);
        max_workers = 64;
    }

    worker_pool_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NSBC_NOMEM;

    if (pthread_mutex_init(&p->submit_wait_lock, NULL) != 0) {
        free(p);
        return NSBC_ERR;
    }
    if (pthread_cond_init(&p->submit_wait_cond, NULL) != 0) {
        pthread_mutex_destroy(&p->submit_wait_lock);
        free(p);
        return NSBC_ERR;
    }
    p->submit_wait_initialized = 1;

    if (pthread_mutex_init(&p->resize_lock, NULL) != 0) {
        pthread_cond_destroy(&p->submit_wait_cond);
        pthread_mutex_destroy(&p->submit_wait_lock);
        free(p);
        return NSBC_ERR;
    }
    p->resize_lock_initialized = 1;

    p->max_workers = max_workers;
    __atomic_store_n(&p->n_active, min_workers, __ATOMIC_RELEASE);
    p->rcu = rcu;
    if (!p->rcu) {
        p->rcu = mem_rcu_create();
        p->own_rcu = 1;
    }
    if (!p->rcu) {
        pthread_mutex_destroy(&p->resize_lock);
        pthread_cond_destroy(&p->submit_wait_cond);
        pthread_mutex_destroy(&p->submit_wait_lock);
        free(p);
        return NSBC_NOMEM;
    }

    /* P3: 预分配 max_workers 槽位（永不 realloc）——submitter 无锁并发读 */
    p->w = calloc(max_workers, sizeof(worker_t));
    if (!p->w) {
        if (p->own_rcu) mem_rcu_destroy(p->rcu);
        pthread_mutex_destroy(&p->resize_lock);
        pthread_cond_destroy(&p->submit_wait_cond);
        pthread_mutex_destroy(&p->submit_wait_lock);
        free(p);
        return NSBC_NOMEM;
    }

    __atomic_store_n(&p->running, 1, __ATOMIC_RELEASE);

    /* 只初始化前 min_workers 个槽 */
    for (unsigned i = 0; i < min_workers; ++i) {
        if (worker_slot_init(p, i, queue_cap) != NSBC_OK) {
            log_warn("worker", "slot_init[%u] failed during create", i);
            __atomic_store_n(&p->running, 0, __ATOMIC_RELEASE);
            wpool_destroy(p);
            return NSBC_ERR;
        }
    }

    int mrc = metric_source_register("nsbc_worker_threads",
                                     "Worker pool queue and execution metrics",
                                     render_worker_metrics, p);
    if (mrc != NSBC_OK)
        log_warn("worker", "worker metrics registration failed rc=%d", mrc);
    else
        p->metrics_registered = 1;

    *out = p;
    log_info("worker", "pool created min=%u max=%u queue_cap=%u",
             min_workers, max_workers, queue_cap ? queue_cap : 4096);
    return NSBC_OK;
}

/* P3: 热更活跃 worker 数。scale-up/down 逻辑见 worker.h 注释。 */
int wpool_resize(worker_pool_t *p, unsigned new_n)
{
    if (!p || new_n == 0)
        return NSBC_INVAL;
    if (new_n > p->max_workers) {
        log_warn("worker", "resize new_n=%u exceeds max=%u, capping",
                 new_n, p->max_workers);
        new_n = p->max_workers;
    }

    pthread_mutex_lock(&p->resize_lock);
    unsigned old_n = __atomic_load_n(&p->n_active, __ATOMIC_RELAXED);
    if (new_n == old_n) {
        pthread_mutex_unlock(&p->resize_lock);
        return NSBC_OK;  /* 无变化 */
    }

    if (new_n > old_n) {
        /* scale-up：初始化 [old_n, new_n) 槽 + 启动线程
         * 部分失败 rollback：已启动的 worker 标 draining + 等退出
         * （不更新 n_active —— 保持 old_n） */
        unsigned fail_at = UINT_MAX;
        for (unsigned i = old_n; i < new_n; ++i) {
            if (worker_slot_init(p, i, 0) != NSBC_OK) {
                log_warn("worker", "resize scale-up slot_init[%u] failed, rolling back", i);
                fail_at = i;
                break;
            }
            log_info("worker", "resize: worker[%u] started", i);
        }
        if (fail_at != UINT_MAX) {
            /* rollback：让 [old_n, fail_at) 已启动的 worker draining
             * + drain queue 后 self-exit + join 清理 slot 资源。
             * 不更新 n_active —— 保持 old_n。
             *
             * F-02 修复：同 scale-down——slot_ready 必须先于 destroy q。 */
            for (unsigned i = old_n; i < fail_at; ++i) {
                worker_t *w = &p->w[i];
                __atomic_store_n(&w->slot_ready, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&w->draining, 1, __ATOMIC_RELEASE);
                if (w->started && w->ready_initialized)
                    sem_post(&w->ready);
            }
            for (unsigned i = old_n; i < fail_at; ++i) {
                worker_t *w = &p->w[i];
                if (w->started) {
                    pthread_join(w->thr, NULL);
                    w->started = 0;
                }
                if (w->mag) { mem_mag_destroy(w->mag); w->mag = NULL; }
                if (w->q)   { mem_mpsc_destroy(w->q);   w->q = NULL; }
                if (w->ready_initialized) { sem_destroy(&w->ready); w->ready_initialized = 0; }
                if (w->rd)  { w->rd = NULL; }
            }
            pthread_mutex_unlock(&p->resize_lock);
            return NSBC_ERR;
        }
        __atomic_store_n(&p->n_active, new_n, __ATOMIC_RELEASE);
        log_info("worker", "resize: n_active %u → %u (scale-up)", old_n, new_n);
    } else {
        /* scale-down：先 atomic dec n_active（阻止新 submit 路由到 [new_n, old_n)），
         * 然后对被踢出的 worker 设 slot_ready=0 + draining=1 + sem_post。
         *
         * F-02 修复关键点：slot_ready 必须在 destroy q 之前置 0。
         * 顺序：n_active=0 → slot_ready=0 → draining=1 → join → destroy q。
         * 这样 submitter 读到旧 n_active 后也会检查 slot_ready==0，
         * 不会向即将被 free 的 queue push。 */
        __atomic_store_n(&p->n_active, new_n, __ATOMIC_RELEASE);
        for (unsigned i = new_n; i < old_n; ++i) {
            worker_t *w = &p->w[i];
            if (__atomic_load_n(&w->slot_ready, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&w->slot_ready, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&w->draining, 1, __ATOMIC_RELEASE);
                sem_post(&w->ready);
                log_info("worker", "resize: worker[%u] marked draining", i);
            }
        }
        /* 等 draining worker drain 完 queue 后 self-exit */
        for (unsigned i = new_n; i < old_n; ++i) {
            worker_t *w = &p->w[i];
            if (w->started) {
                pthread_join(w->thr, NULL);
                w->started = 0;
            }
            /* 清理 slot 资源（slot_ready 已在上一步置 0，submitter 不会再用 q） */
            if (w->mag) { mem_mag_destroy(w->mag); w->mag = NULL; }
            /* ★ P0-2.3 RACE-FIX: 等所有 in-flight submitter 释放 q_refcnt 再 destroy */
            {
                unsigned spins = 0;
                while (__atomic_load_n(&w->q_refcnt, __ATOMIC_ACQUIRE) != 0) {
                    sched_yield();
                    if (++spins > 100000) {
                        log_warn("worker", "resize: worker[%u] q_refcnt stuck=%u, force destroy",
                                 i, (unsigned)__atomic_load_n(&w->q_refcnt, __ATOMIC_RELAXED));
                        break;
                    }
                }
            }
            if (w->q)   { mem_mpsc_destroy(w->q);   w->q = NULL; }
            if (w->ready_initialized) { sem_destroy(&w->ready); w->ready_initialized = 0; }
            if (w->rd)  { w->rd = NULL; }
            log_info("worker", "resize: worker[%u] joined + cleaned up", i);
        }
        log_info("worker", "resize: n_active %u → %u (scale-down)", old_n, new_n);
    }

    pthread_mutex_unlock(&p->resize_lock);
    return NSBC_OK;
}

void wpool_destroy(worker_pool_t *p)
{
    if (!p)
        return;

    /* P3: 拿 resize_lock 防 ticker 线程的并发 wpool_resize（shutdown 期间 ticker 可能还在跑
     * ~1s 周期检查 —— resize 会访问正在 destroy 的 pool 资源）。
     * 后续代码里的 join 会释放锁之前启动的所有线程。 */
    if (p->resize_lock_initialized)
        pthread_mutex_lock(&p->resize_lock);

    worker_close_submit_gate(p);

    if (p->metrics_registered) {
        int rc = metric_source_unregister("nsbc_worker_threads",
                                          render_worker_metrics, p);
        if (rc != NSBC_OK)
            log_warn("worker", "worker metrics unregister failed rc=%d", rc);
    }

    __atomic_store_n(&p->running, 0, __ATOMIC_RELEASE);

    /* 唤醒所有 worker（包括 scale-down 中已 draining 的）让它们退出 */
    for (unsigned i = 0; i < p->max_workers; ++i) {
        if (p->w[i].ready_initialized && p->w[i].started) {
            sem_post(&p->w[i].ready);
        }
    }

    /* join 所有已启动的 worker 线程（包括 scale-down 中 draining 尚未 exit 的）。
     * 注：worker_main 退出时只做 thread-local RCU leave（mem_rcu_thread_leave），
     * pool 共享资源（mag/q/sem）必须由调用方在持有 resize_lock 后 join 清理——
     * 这避免了 worker_main 在无锁状态下改 pool 共享字段的 race。 */
    for (unsigned i = 0; i < p->max_workers; ++i) {
        worker_t *w = &p->w[i];
        if (w->started) {
            pthread_join(w->thr, NULL);
            w->started = 0;
        }
        /* join 后清理 slot 资源 */
        if (w->mag) { mem_mag_destroy(w->mag); w->mag = NULL; }
        /* ★ P0-2.3 RACE-FIX: 同 wpool_resize — 等 q_refcnt 归零 */
        {
            unsigned spins = 0;
            while (__atomic_load_n(&w->q_refcnt, __ATOMIC_ACQUIRE) != 0) {
                sched_yield();
                if (++spins > 100000) break;
            }
        }
        if (w->q)   { mem_mpsc_destroy(w->q);   w->q = NULL; }
        if (w->ready_initialized) { sem_destroy(&w->ready); w->ready_initialized = 0; }
        if (w->rd)  { w->rd = NULL; }
        __atomic_store_n(&w->slot_ready, 0, __ATOMIC_RELEASE);
    }

    free(p->w);
    if (p->own_rcu)
        mem_rcu_destroy(p->rcu);
    /* destroy resize_lock 前先 unlock（POSIX: pthread_mutex_destroy 不能在持有态） */
    if (p->resize_lock_initialized) {
        pthread_mutex_unlock(&p->resize_lock);
        pthread_mutex_destroy(&p->resize_lock);
    }
    if (p->submit_wait_initialized) {
        pthread_cond_destroy(&p->submit_wait_cond);
        pthread_mutex_destroy(&p->submit_wait_lock);
    }
    free(p);
}

int wpool_submit(worker_pool_t *p, uint64_t hash_key, worker_job_t *job)
{
    if (!p || !job || !job->fn)
        return NSBC_INVAL;
    int rc = worker_submit_enter(p);
    if (rc != NSBC_OK) {
        __atomic_add_fetch(&p->rejected, 1, __ATOMIC_RELAXED);
        return rc;
    }
    /* F-02 修复：读取 n_active 后必须 double-check slot 存活。
     * resize 线程会先 atomic_store(n_active=new_n, RELEASE)
     * + atomic_store(slot_ready=0, RELEASE)，然后才 destroy q。
     * 这里用 ACQUIRE 读 slot_ready → 若 slot 已死，重新读取 n_active。 */
    unsigned n_active, idx;
    for (;;) {
        n_active = __atomic_load_n(&p->n_active, __ATOMIC_ACQUIRE);
        if (n_active == 0) {
            worker_submit_leave(p);
            return NSBC_INVAL;
        }
        idx = (unsigned)(hash_key % (uint64_t)n_active);
        if (__atomic_load_n(&p->w[idx].slot_ready, __ATOMIC_ACQUIRE))
            break;  /* slot 存活，安全 */
        /* slot 已死（concurrent scale-down/up）— 重试 */
    }

    /* ★ P0-2.3 RACE-FIX: double-check 后 fetch_add q_refcnt，
     * 阻止 scale-down 在 mem_mpsc_push 前 destroy q。 */
    __atomic_fetch_add(&p->w[idx].q_refcnt, 1, __ATOMIC_ACQUIRE);
    job->enqueued_ns = worker_now_ns();
    if (mem_mpsc_push(p->w[idx].q, job) != NSBC_OK) {
        __atomic_fetch_sub(&p->w[idx].q_refcnt, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&p->rejected, 1, __ATOMIC_RELAXED);
        worker_submit_leave(p);
        return NSBC_NOSPC;
    }
    __atomic_fetch_sub(&p->w[idx].q_refcnt, 1, __ATOMIC_RELEASE);

    __atomic_add_fetch(&p->submitted, 1, __ATOMIC_RELAXED);
    if (sem_post(&p->w[idx].ready) != 0)
        log_warn("worker", "sem_post after submit failed worker=%u errno=%d",
                 idx, errno);
    worker_submit_leave(p);
    return NSBC_OK;
}

int wpool_submit_any(worker_pool_t *p, worker_job_t *job)
{
    if (!p || !job || !job->fn)
        return NSBC_INVAL;
    int rc = worker_submit_enter(p);
    if (rc != NSBC_OK) {
        __atomic_add_fetch(&p->rejected, 1, __ATOMIC_RELAXED);
        return rc;
    }
    /* F-02 修复：同 wpool_submit — 用 ACQUIRE 读 n_active，
     * 遍历候选时检查 slot_ready，跳过已死 slot。 */
    unsigned n_active = __atomic_load_n(&p->n_active, __ATOMIC_ACQUIRE);
    if (n_active == 0) n_active = 1;

    unsigned long i = __atomic_fetch_add(&p->rr, 1, __ATOMIC_RELAXED);
    job->enqueued_ns = worker_now_ns();
    for (unsigned k = 0; k < n_active; ++k) {
        unsigned idx = (unsigned)((i + k) % n_active);
        /* slot 死了（被 concurrent resize tear down）→ 跳过 */
        if (!__atomic_load_n(&p->w[idx].slot_ready, __ATOMIC_ACQUIRE))
            continue;
        /* ★ P0-2.3 RACE-FIX: 同 wpool_submit — 引用保护 q 生命周期 */
        __atomic_fetch_add(&p->w[idx].q_refcnt, 1, __ATOMIC_ACQUIRE);
        if (mem_mpsc_push(p->w[idx].q, job) == NSBC_OK) {
            __atomic_fetch_sub(&p->w[idx].q_refcnt, 1, __ATOMIC_RELEASE);
            __atomic_add_fetch(&p->submitted, 1, __ATOMIC_RELAXED);
            if (sem_post(&p->w[idx].ready) != 0)
                log_warn("worker", "sem_post after submit failed worker=%u errno=%d",
                         idx, errno);
            worker_submit_leave(p);
            return NSBC_OK;
        }
        __atomic_fetch_sub(&p->w[idx].q_refcnt, 1, __ATOMIC_RELEASE);
        /* push 失败（queue 满）→ 换下一个 slot 重试 */
    }
    __atomic_add_fetch(&p->rejected, 1, __ATOMIC_RELAXED);
    worker_submit_leave(p);
    return NSBC_NOSPC;
}

unsigned wpool_size(const worker_pool_t *p)
{
    return p ? __atomic_load_n(&p->n_active, __ATOMIC_RELAXED) : 0;
}

unsigned wpool_max_size(const worker_pool_t *p)
{
    return p ? p->max_workers : 0;
}

mem_rcu_t *wpool_rcu(worker_pool_t *p) { return p->rcu; }

int wpool_rcu_advance(worker_pool_t *p)
{
    return mem_rcu_advance(p->rcu);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
