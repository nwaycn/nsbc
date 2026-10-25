#define _GNU_SOURCE
#include "metric_reg.h"
#include "worker.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int entered;
    int release;
} blocking_job_t;

typedef struct {
    worker_pool_t *pool;
} destroy_arg_t;

static void blocking_job(void *arg)
{
    blocking_job_t *state = (blocking_job_t *)arg;
    pthread_mutex_lock(&state->lock);
    state->entered = 1;
    pthread_cond_broadcast(&state->cond);
    while (!state->release)
        pthread_cond_wait(&state->cond, &state->lock);
    pthread_mutex_unlock(&state->lock);
}

static void count_job(void *arg)
{
    unsigned *count = (unsigned *)arg;
    __atomic_add_fetch(count, 1, __ATOMIC_RELAXED);
}

static void *destroy_pool(void *arg)
{
    destroy_arg_t *destroy = (destroy_arg_t *)arg;
    wpool_destroy(destroy->pool);
    return NULL;
}

static void wait_for_job(blocking_job_t *state)
{
    pthread_mutex_lock(&state->lock);
    while (!state->entered)
        pthread_cond_wait(&state->cond, &state->lock);
    pthread_mutex_unlock(&state->lock);
}

int main(void)
{
    worker_pool_t *pool = NULL;
    assert(wpool_create(&pool, 1, 1, 8, NULL) == NSBC_OK);

    blocking_job_t blocked;
    memset(&blocked, 0, sizeof(blocked));
    assert(pthread_mutex_init(&blocked.lock, NULL) == 0);
    assert(pthread_cond_init(&blocked.cond, NULL) == 0);

    worker_job_t first = { .fn = blocking_job, .arg = &blocked };
    assert(wpool_submit(pool, 0, &first) == NSBC_OK);
    wait_for_job(&blocked);

    unsigned accepted_count = 0;
    unsigned executed_count = 0;
    worker_job_t queued[8] = { 0 };
    for (; accepted_count < 8; ++accepted_count) {
        queued[accepted_count].fn = count_job;
        queued[accepted_count].arg = &executed_count;
        assert(wpool_submit_any(pool, &queued[accepted_count]) == NSBC_OK);
    }

    worker_job_t overflow = { .fn = count_job, .arg = &executed_count };
    assert(wpool_submit_any(pool, &overflow) == NSBC_NOSPC);

    destroy_arg_t destroy = { .pool = pool };
    pthread_t destroy_thread;
    assert(pthread_create(&destroy_thread, NULL, destroy_pool, &destroy) == 0);

    int shutdown_seen = 0;
    struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
    for (unsigned attempt = 0; attempt < 5000; ++attempt) {
        int rc = wpool_submit_any(pool, &overflow);
        if (rc == NSBC_SHUTDOWN) {
            shutdown_seen = 1;
            break;
        } else {
            assert(rc == NSBC_NOSPC);
        }
        nanosleep(&pause, NULL);
    }
    assert(shutdown_seen);

    pthread_mutex_lock(&blocked.lock);
    blocked.release = 1;
    pthread_cond_broadcast(&blocked.cond);
    pthread_mutex_unlock(&blocked.lock);
    assert(pthread_join(destroy_thread, NULL) == 0);

    char metrics[4096];
    assert(metric_render_all(metrics, sizeof(metrics)) >= 0);
    assert(strstr(metrics, "nsbc_worker_threads") == NULL);
    assert(accepted_count == 8);
    assert(__atomic_load_n(&executed_count, __ATOMIC_RELAXED) == accepted_count);

    pthread_cond_destroy(&blocked.cond);
    pthread_mutex_destroy(&blocked.lock);
    puts("worker lifecycle: PASS");
    return 0;
}
