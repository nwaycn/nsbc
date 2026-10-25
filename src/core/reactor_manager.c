
#include "reactor_manager.h"
#include "log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

struct reactor_mgr {
    unsigned n;            /* reactor 总数 */
    reactor_t **reactors;  /* [0..n-1] */
    int max_events;

    /* P3: 多 reactor 线程支持 */
    pthread_t *threads;    /* [1..n-1] 的 pthread_t，threads[0] 对应 reactors[1] */
    int started;           /* 0=未启动线程（boot 失败路径可 destroy），1=已启动 */
};

/* reactor_thread_fn 的线程参数 —— 小栈 struct，pthread_create 直接传栈上副本 */
typedef struct {
    reactor_mgr_t *mgr;
    unsigned rid;    /* reactor id ∈ [1..n-1]（0 留给 main） */
} reactor_thread_arg_t;

/* reactor[1..N-1] 的线程入口：
 *   1. CPU 绑核（reactor[i] → CPU core i）——容器配额不足时 log_warn 继续
 *   2. loop reactor_wait(r, 1000) until reactor_stopping(r)
 *   3. drain 任务后退出线程——reactor_destroy 由 reactor_mgr_destroy 在
 *      main 线程统一执行（必须在 modmgr_stop_all 之后，避免 rtp_relay_stop
 *      的 reactor_call 触达已 free 内存，F-03 修复） */
static void *reactor_thread_fn(void *arg)
{
    reactor_thread_arg_t *ta = (reactor_thread_arg_t *)arg;
    if (!ta || !ta->mgr) return NULL;

    /* 拷贝到局部变量后立刻 free —— pthread_create 只传了指针值，
     * ta 是我们 malloc 的，线程入口负责释放。 */
    reactor_mgr_t *mgr = ta->mgr;
    unsigned rid = ta->rid;
    free(ta);

    /* C1: CPU 亲和性绑核 —— reactor[rid] 绑 CPU (rid - 1)。
     * reactor[0] 留给 main 线程（不绑核，保持调度灵活性）。
     * 绑核失败（容器 CPU 配额不足 / CPU_SET 越界）log_warn 后继续，不阻塞。 */
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        unsigned cpu = rid - 1;  /* reactor[1] → CPU 0, reactor[2] → CPU 1 ... */
        CPU_SET(cpu, &cpuset);
        int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
        if (rc == 0) {
            log_info("core", "reactor[%u] pinned to CPU %u", rid, cpu);
        } else {
            log_warn("core", "reactor[%u] pin to CPU %u failed rc=%d (ignored)",
                     rid, cpu, rc);
        }
    }

    reactor_t *r = mgr->reactors[rid];
    if (!r) return NULL;

    /* reactor_wait 入口设置 task_started=1 和 wait_active=1。
     * 超时 1000ms 无 ticker 时间轮耦合——无超时事件时保持低 CPU 占用。 */
    while (!reactor_stopping(r)) {
        int wait_rc = reactor_wait(r, 1000);
        if (wait_rc < 0) {
            /* reactor_wait 返回错误（非 EINTR 可忽略场景），
             * 只有 reactor_stopping 会正常退出循环 */
            log_warn("core", "reactor_thread: reactor_wait rc=%d (will stop)",
                     wait_rc);
            break;
        }
    }

    /* reactor_wait 已退出 → wait_active=0，stop 标志已置。
     * drain 可能的剩余任务（reactor_stop 唤醒后会再跑一次 reactor_run_tasks）。
     * 此时 reactor_wait(0) 会跑一次任务队列 drain。 */
    (void)reactor_wait(r, 0);

    /* F-03 修复：线程只 drain 后退出，不调用 reactor_destroy。
     * reactor_destroy 统一由 reactor_mgr_destroy 在 main 线程执行，
     * 此时 modmgr_stop_all 已完成，rtp_relay_stop 的 reactor_call
     * 不会再触达这些 reactor。 */
    log_info("core", "reactor[%u] thread exited (drained, reactor kept for manager destroy)", rid);
    return NULL;
}

reactor_mgr_t *reactor_mgr_create(unsigned n, int max_events)
{
    if (n == 0) n = 1;
    if (n > 64) {
        log_warn("core", "core.reactors=%u exceeds 64 cap, clamping to 64", n);
        n = 64;
    }

    reactor_mgr_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->n = n;
    m->max_events = max_events > 0 ? max_events : 256;
    m->started = 0;

    m->reactors = calloc(n, sizeof(reactor_t *));
    if (!m->reactors) { free(m); return NULL; }

    /* 预创建所有 reactor 对象（main 线程 = owner）。
     * 此时无线程运行——reactor_add 由 main 线程在模块 init 时逐个调用，
     * slot table 只有一个线程访问，无并发竞争（slot_ensure 的 realloc 安全）。 */
    for (unsigned i = 0; i < n; ++i) {
        m->reactors[i] = reactor_create(m->max_events);
        if (!m->reactors[i]) {
            for (unsigned j = 0; j < i; ++j) reactor_destroy(m->reactors[j]);
            free(m->reactors);
            free(m);
            return NULL;
        }
    }
    log_info("core", "reactor_mgr create n=%u max_events=%d (threads pending)",
             n, m->max_events);
    return m;
}

reactor_t *reactor_mgr_get(reactor_mgr_t *m, unsigned id)
{
    if (!m || id >= m->n) return NULL;
    return m->reactors[id];
}

reactor_t *reactor_mgr_by_id(reactor_mgr_t *m, unsigned id)
{
    return reactor_mgr_get(m, id);
}

unsigned reactor_mgr_count(const reactor_mgr_t *m)
{
    return m ? m->n : 0;
}

/* P3: 真正多 reactor 路由。
 *   hash_key % n → reactor id。
 *   单 reactor（n=1）时恒返回 0，与 P2 行为一致。 */
unsigned reactor_mgr_route(const reactor_mgr_t *m, uint64_t hash_key)
{
    if (!m || m->n == 0) return 0;
    return (unsigned)(hash_key % (uint64_t)m->n);
}

int reactor_mgr_start_threads(reactor_mgr_t *m)
{
    if (!m) return NSBC_INVAL;
    if (m->started) return NSBC_OK; /* 幂等 */

    unsigned need_threads = m->n > 1 ? m->n - 1 : 0;
    if (need_threads == 0) {
        log_info("core", "reactor_mgr: n=1, no extra threads needed");
        m->started = 1;
        return NSBC_OK;
    }

    m->threads = calloc(need_threads, sizeof(pthread_t));
    if (!m->threads) return NSBC_NOMEM;

    /* reactor[1..n-1] 各自一个 pthread 跑 reactor_thread_fn。
     * reactor[0] 留给 main 线程（ticker 时间轮耦合）。 */
    for (unsigned i = 0; i < need_threads; ++i) {
        reactor_thread_arg_t *ta = calloc(1, sizeof(*ta));
        if (!ta) {
            log_warn("core", "reactor_mgr: arg alloc[%u] failed — rolling back", i);
            /* F-03/F-05 修复：线程只 drain 不自 destroy，
             * reactor_mgr_destroy 会统一 destroy 所有 reactors[0..n-1]，
             * 不会再出现双重 free。 */
            for (unsigned j = 0; j < i; ++j) reactor_stop(m->reactors[j + 1]);
            for (unsigned j = 0; j < i; ++j) pthread_join(m->threads[j], NULL);
            free(m->threads);
            m->threads = NULL;
            return NSBC_NOMEM;
        }
        ta->mgr = m;
        ta->rid = i + 1;
        int rc = pthread_create(&m->threads[i], NULL, reactor_thread_fn, ta);
        if (rc != 0) {
            log_warn("core", "reactor_mgr: pthread_create[%u] failed rc=%d — rolling back", i, rc);
            free(ta);
            /* 已创建的线程 stop+join；未创建的 reactor 保持原状。
             * reactor_mgr_destroy 统一 destroy 所有——无双重 free。 */
            for (unsigned j = 0; j < i; ++j) reactor_stop(m->reactors[j + 1]);
            for (unsigned j = 0; j < i; ++j) pthread_join(m->threads[j], NULL);
            free(m->threads);
            m->threads = NULL;
            return NSBC_ERR;
        }
        log_info("core", "reactor_mgr: reactor[%u] thread started", i + 1);
    }

    m->started = 1;
    log_info("core", "reactor_mgr: all %u reactor threads started", need_threads);
    return NSBC_OK;
}

void reactor_mgr_stop_all(reactor_mgr_t *m)
{
    if (!m) return;
    for (unsigned i = 0; i < m->n; ++i)
        reactor_stop(m->reactors[i]);
}

void reactor_mgr_join(reactor_mgr_t *m)
{
    if (!m || !m->threads) return;
    unsigned need_threads = m->n > 1 ? m->n - 1 : 0;
    for (unsigned i = 0; i < need_threads; ++i) {
        pthread_join(m->threads[i], NULL);
        log_info("core", "reactor_mgr: reactor[%u] thread joined", i + 1);
    }
    free(m->threads);
    m->threads = NULL;
}

int reactor_mgr_destroy(reactor_mgr_t *m)
{
    if (!m) return NSBC_OK;
    int rc = NSBC_OK;

    /* F-03 修复：统一在 main 线程 destroy 所有 reactors[0..n-1]。
     * reactor_thread_fn 不再自 destroy——线程只 drain 后退出，
     * main 等 modmgr_stop_all + reactor_mgr_join 完成后统一释放。
     * 这样 rtp_relay_stop 的 reactor_call 绝不会触达已 free reactor。 */
    for (unsigned i = 0; i < m->n; ++i) {
        if (m->reactors[i]) {
            int drc = reactor_destroy(m->reactors[i]);
            if (drc != NSBC_OK) {
                log_warn("core", "reactor_mgr_destroy reactor[%u] rc=%d", i, drc);
                rc = drc;
            }
            m->reactors[i] = NULL;
        }
    }

    free(m->reactors);
    free(m);
    return rc;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
