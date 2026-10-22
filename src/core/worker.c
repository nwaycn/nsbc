#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "worker.h"
#include "log.h"

#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    unsigned id;
    pthread_t thr;
    mem_mag_t *mag;
    mem_rcu_reader_t *rd;
    mem_mpsc_t *q;
    worker_pool_t *pool;
    uint64_t done;
} worker_t;

struct worker_pool {
    unsigned n;
    worker_t *w;
    mem_rcu_t *rcu;
    int own_rcu;
    nsbc_atomic_t rr;
    volatile int running;
};

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    log_thread_attach();
    uint32_t spins = 0;
    while (w->pool->running || mem_mpsc_size(w->q) > 0) {
        void *msg = NULL;
        if (mem_mpsc_pop(w->q, &msg) == NSBC_OK) {
            worker_job_t *job = (worker_job_t *)msg;
            job->fn(job->arg);
            ++w->done;
            spins = 0;
        } else {
            if (++spins < 100) {
                sched_yield();
            } else {
                struct timespec ts = {0, 200 * 1000}; /* 200us */
                nanosleep(&ts, NULL);
            }
        }
    }
    mem_rcu_thread_leave(w->rd);
    mem_mag_destroy(w->mag);
    log_thread_detach();
    return NULL;
}

int wpool_create(worker_pool_t **out, unsigned n, uint32_t queue_cap,
                 mem_rcu_t *rcu)
{
    if (n == 0)
        n = 1;
    worker_pool_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NSBC_NOMEM;
    p->n = n;
    p->rcu = rcu;
    if (!p->rcu) {
        p->rcu = mem_rcu_create();
        p->own_rcu = 1;
    }
    if (!p->rcu) {
        free(p);
        return NSBC_NOMEM;
    }
    p->w = calloc(n, sizeof(worker_t));
    if (!p->w) {
        if (p->own_rcu)
            mem_rcu_destroy(p->rcu);
        free(p);
        return NSBC_NOMEM;
    }
    p->running = 1;
    for (unsigned i = 0; i < n; ++i) {
        worker_t *w = &p->w[i];
        w->id = i;
        w->pool = p;
        w->q = mem_mpsc_create(queue_cap ? queue_cap : 4096, NULL);
        w->mag = mem_mag_create();
        w->rd = mem_rcu_thread_join(p->rcu);
        if (!w->q || !w->mag || !w->rd) {
            /* fail-fast：缺任一资源都不允许半启动 */
            p->running = 0;
            wpool_destroy(p);
            return NSBC_NOMEM;
        }
        if (pthread_create(&w->thr, NULL, worker_main, w) != 0) {
            p->running = 0;
            wpool_destroy(p);
            return NSBC_ERR;
        }
    }
    *out = p;
    return NSBC_OK;
}

void wpool_destroy(worker_pool_t *p)
{
    if (!p)
        return;
    p->running = 0;
    for (unsigned i = 0; i < p->n; ++i) {
        if (p->w[i].thr) {
            pthread_join(p->w[i].thr, NULL);
        } else {
            /* 启动失败路径：线程未跑，资源未在线程出口释放 */
            if (p->w[i].rd)
                mem_rcu_thread_leave(p->w[i].rd);
            if (p->w[i].mag)
                mem_mag_destroy(p->w[i].mag);
        }
        if (p->w[i].q)
            mem_mpsc_destroy(p->w[i].q);
    }
    free(p->w);
    if (p->own_rcu)
        mem_rcu_destroy(p->rcu);
    free(p);
}

int wpool_submit(worker_pool_t *p, uint64_t hash_key, worker_job_t *job)
{
    unsigned idx = (unsigned)(hash_key % p->n);
    return mem_mpsc_push(p->w[idx].q, job) == NSBC_OK ? NSBC_OK
                                                      : NSBC_NOSPC;
}

int wpool_submit_any(worker_pool_t *p, worker_job_t *job)
{
    unsigned long i = __atomic_fetch_add(&p->rr, 1, __ATOMIC_RELAXED);
    for (unsigned k = 0; k < p->n; ++k) {
        unsigned idx = (unsigned)((i + k) % p->n);
        if (mem_mpsc_push(p->w[idx].q, job) == NSBC_OK)
            return NSBC_OK;
    }
    return NSBC_NOSPC;
}

unsigned wpool_size(const worker_pool_t *p) { return p->n; }
mem_rcu_t *wpool_rcu(worker_pool_t *p) { return p->rcu; }

int wpool_rcu_advance(worker_pool_t *p)
{
    return mem_rcu_advance(p->rcu);
}
