#define _GNU_SOURCE
#include "reactor.h"
#include "nsbc_common.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* 辅助：owner 线程跑 reactor_wait 循环                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    reactor_t *r;
    volatile int entered;   /* owner 已进入 reactor_wait，task_started=1 */
} owner_ctx_t;

static void *owner_run(void *arg)
{
    owner_ctx_t *oc = (owner_ctx_t *)arg;
    oc->entered = 1;
    while (!reactor_stopping(oc->r)) {
        (void)reactor_wait(oc->r, 100);
    }
    return NULL;
}

static pthread_t spawn_owner(reactor_t *r, owner_ctx_t *oc)
{
    memset(oc, 0, sizeof(*oc));
    oc->r = r;
    pthread_t th;
    assert(pthread_create(&th, NULL, owner_run, oc) == 0);
    /* 等待 reactor_wait 设置 task_started */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 };
    for (int i = 0; i < 200 && !oc->entered; ++i)
        nanosleep(&ts, NULL);
    return th;
}

/* ------------------------------------------------------------------ */
/* 测试 1：owner 直调 — reactor_call 在 owner 线程直接执行             */
/* ------------------------------------------------------------------ */
static int direct_value;
static int direct_task(void *arg)
{
    (void)arg;
    direct_value = 42;
    return 123;
}

static void test_owner_direct_call(void)
{
    reactor_t *r = reactor_create(16);
    assert(r);
    /* 当前线程是 owner，reactor_call 应直接执行 */
    int rc = reactor_call(r, direct_task, NULL);
    assert(rc == 123);
    assert(direct_value == 42);
    reactor_destroy(r);  /* 无 wait 活跃，可直接 destroy */
    puts("reactor: owner direct call PASS");
}

/* ------------------------------------------------------------------ */
/* 测试 2：跨线程同步返回                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    int observed;
} cross_arg_t;

static int cross_task(void *arg)
{
    cross_arg_t *s = (cross_arg_t *)arg;
    s->observed = 999;
    return 77;
}

static void test_cross_thread_sync_return(void)
{
    reactor_t *r = reactor_create(16);
    assert(r);
    owner_ctx_t oc;
    pthread_t owner = spawn_owner(r, &oc);

    cross_arg_t arg = { .observed = 0 };
    int rc = reactor_call(r, cross_task, &arg);
    assert(rc == 77);
    assert(arg.observed == 999);

    reactor_stop(r);
    pthread_join(owner, NULL);
    assert(reactor_destroy(r) == NSBC_OK);
    puts("reactor: cross-thread sync return PASS");
}

/* ------------------------------------------------------------------ */
/* 测试 3：event loop 未启动 — 跨线程提交返回 NSBC_AGAIN               */
/* ------------------------------------------------------------------ */
static int zero_task(void *arg) { (void)arg; return 0; }

static void test_not_started_rejects(void)
{
    reactor_t *r = reactor_create(16);
    assert(r);
    int rc = reactor_call(r, zero_task, NULL);
    assert(rc == NSBC_AGAIN);
    assert(reactor_destroy(r) == NSBC_OK);
    puts("reactor: not-started rejects PASS");
}

/* ------------------------------------------------------------------ */
/* 测试 4：队列满 — 多线程并发提交触发 NOSPC                            */
/* ------------------------------------------------------------------ */
typedef struct {
    reactor_t *r;
    int overflow_seen;     /* 本线程是否看到过 NOSPC */
    int shutdown_seen;     /* 本线程是否看到过 SHUTDOWN */
    long submitted;        /* 本线程成功提交数 */
} flood_arg_t;

static int noop_task(void *arg) { (void)arg; return 0; }

static void *flood_worker(void *arg)
{
    flood_arg_t *fa = (flood_arg_t *)arg;
    while (!fa->overflow_seen && !fa->shutdown_seen) {
        int rc = reactor_call(fa->r, noop_task, NULL);
        if (rc == NSBC_NOSPC) { fa->overflow_seen = 1; break; }
        if (rc == NSBC_SHUTDOWN) { fa->shutdown_seen = 1; break; }
        /* 正常返回 0（noop_task 返回值）*/
        if (rc == 0) fa->submitted++;
    }
    return NULL;
}

static void test_queue_full(void)
{
    reactor_t *r = reactor_create(16);
    assert(r);
    owner_ctx_t oc;
    pthread_t owner = spawn_owner(r, &oc);

    /* 起 8 个并发提交线程，让队列真正堆积 */
    const int NTHREADS = 8;
    pthread_t workers[NTHREADS];
    flood_arg_t args[NTHREADS];
    for (int i = 0; i < NTHREADS; ++i) {
        args[i].r = r;
        args[i].overflow_seen = 0;
        args[i].shutdown_seen = 0;
        args[i].submitted = 0;
        pthread_create(&workers[i], NULL, flood_worker, &args[i]);
    }

    /* 等至少一个线程看到 NOSPC 或全部线程都在跑 */
    int any_overflow = 0;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
    for (int i = 0; i < 500 && !any_overflow; ++i) {
        for (int j = 0; j < NTHREADS; ++j)
            if (args[j].overflow_seen) any_overflow = 1;
        if (!any_overflow) nanosleep(&ts, NULL);
    }

    reactor_stop(r);
    for (int i = 0; i < NTHREADS; ++i)
        pthread_join(workers[i], NULL);
    pthread_join(owner, NULL);

    assert(any_overflow);  /* 必须至少一个线程看到队列满 */
    assert(reactor_destroy(r) == NSBC_OK);
    puts("reactor: queue full overflow PASS");
}

/* ------------------------------------------------------------------ */
/* 测试 5：stop 后拒绝新提交 — 返回 NSBC_SHUTDOWN                      */
/* ------------------------------------------------------------------ */
static void test_stop_rejects(void)
{
    reactor_t *r = reactor_create(16);
    assert(r);
    owner_ctx_t oc;
    pthread_t owner = spawn_owner(r, &oc);

    reactor_stop(r);

    /* task_closing 可见 */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
    nanosleep(&ts, NULL);

    int rc = reactor_call(r, zero_task, NULL);
    assert(rc == NSBC_SHUTDOWN);

    pthread_join(owner, NULL);
    assert(reactor_destroy(r) == NSBC_OK);
    puts("reactor: stop rejects PASS");
}

/* ------------------------------------------------------------------ */
/* 测试 6：destroy 状态检查                                             */
/*   - 非 owner 线程 → NSBC_PERM_DENIED                                 */
/*   - wait 活跃时 → NSBC_BUSY                                          */
/*   - 正常 stop+drain 后 → NSBC_OK                                    */
/* ------------------------------------------------------------------ */
static void *non_owner_destroy_try(void *arg)
{
    reactor_t *r = (reactor_t *)arg;
    int rc = reactor_destroy(r);
    assert(rc == NSBC_PERM_DENIED);
    return NULL;
}

static void test_destroy_checks(void)
{
    reactor_t *r = reactor_create(16);
    assert(r);

    /* 起 owner 线程跑 reactor_wait */
    owner_ctx_t oc;
    pthread_t owner = spawn_owner(r, &oc);

    /* 非 owner 线程调 destroy → PERM_DENIED */
    pthread_t naughty;
    pthread_create(&naughty, NULL, non_owner_destroy_try, r);
    pthread_join(naughty, NULL);

    /* stop + join owner（reactor_wait 会 drain 队列后退出）*/
    reactor_stop(r);
    pthread_join(owner, NULL);

    assert(reactor_destroy(r) == NSBC_OK);
    puts("reactor: destroy checks PASS");
}

/* ------------------------------------------------------------------ */
/* 测试 7：stop + drain 已接受命令                                      */
/* ------------------------------------------------------------------ */
static int drain_counter;
static int drain_task(void *arg)
{
    (void)arg;
    ++drain_counter;
    return 0;
}

static void test_stop_drains(void)
{
    reactor_t *r = reactor_create(16);
    assert(r);
    drain_counter = 0;
    owner_ctx_t oc;
    pthread_t owner = spawn_owner(r, &oc);

    /* 提交一批任务让 owner 处理完成 */
    for (int i = 0; i < 50; ++i)
        reactor_call(r, drain_task, NULL);

    /* 所有任务应已同步执行 */
    assert(drain_counter == 50);

    reactor_stop(r);
    pthread_join(owner, NULL);

    assert(reactor_destroy(r) == NSBC_OK);
    puts("reactor: stop+drain PASS");
}

/* ------------------------------------------------------------------ */
/* 主入口                                                               */
/* ------------------------------------------------------------------ */
int main(void)
{
    test_owner_direct_call();
    test_not_started_rejects();
    test_destroy_checks();
    test_cross_thread_sync_return();
    test_stop_rejects();
    test_stop_drains();
    test_queue_full();
    puts("ALL REACTOR TESTS PASSED");
    return 0;
}
