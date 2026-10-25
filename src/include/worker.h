#ifndef NSBC_WORKER_H
#define NSBC_WORKER_H

/* worker 池（core 层）
 *
 * 每个 worker 线程独占：mem magazine（快路径无锁）、RCU 读者、
 * MPSC 任务队列（REQ-ARCH-008）。调用方可按稳定键哈希
 * （branch/Call-ID）投递，同一键始终落在同一 worker。
 * 队列满 → NSBC_NOSPC：上层按 REQ-HA-003/REQ-SIP-006 回 503（背压）。
 * 当前用于有界、可脱离呼叫状态机的 CDR 持久化工作；SIP/媒体状态机本身不投递到 Worker。
 *
 * P3 按需扩缩：
 *   - worker_t 数组预分配 max_workers 槽位（永不 realloc——submitter 无锁并发读）
 *   - n_active（atomic）表示当前活跃 worker 数，在 [min, max] 间变化
 *   - scale-up：初始化新槽 + pthread_create + atomic inc n_active
 *   - scale-down：atomic dec n_active（阻止新 submit 路由）+ draining=1 + sem_post，
 *                 worker drain 自己 queue 后 self-exit
 *   - resize 通过 core.workers 配置热更新（CGF_HOT）触发 wpool_resize()
 */

#include <pthread.h>
#include "mem.h"
#include "nsbc_atomic.h"
#include "nsbc_common.h"

typedef struct worker_job {
    void (*fn)(void *arg);
    void *arg;
    uint64_t enqueued_ns; /* set by the pool before queue insertion */
} worker_job_t; /* 内嵌于报文/事务对象，排队零 malloc */

typedef struct worker_pool worker_pool_t;

/* 创建 worker 池。
 *   min_workers —— 初始活跃 worker 数（启动时立即创建）
 *   max_workers —— 预分配槽位上限（wpool_resize 不能超过此值）
 *   queue_cap   —— 每 worker MPSC queue 容量（0 = 默认 4096）
 *   rcu         —— 外部 RCU（可 NULL，内部自建） */
int wpool_create(worker_pool_t **out, unsigned min_workers, unsigned max_workers,
                 uint32_t queue_cap, mem_rcu_t *rcu);

/* 热更活跃 worker 数。new_n ∈ [1, max_workers]。
 * scale-up：创建新 worker 槽 → 启动线程 → atomic inc n_active
 * scale-down：atomic dec n_active（阻止新 submit 路由）→ draining=1 + sem_post
 *             被踢出的 worker drain 自己 queue 后 self-exit，wpool_resize 不 join 线程
 *             （线程 exit 时自清理资源）。
 * resize 会短暂改变 hash 路由归属（同一 hash_key 可能到不同 worker）
 * ——对 CDR 等无严格顺序依赖的任务安全。 */
int wpool_resize(worker_pool_t *p, unsigned new_n);

/* Stops accepting jobs, drains accepted work and joins workers. The owner must
 * quiesce all submitter threads before destroying the pool. */
void wpool_destroy(worker_pool_t *p);

/* hash_key 选队列；同一 Call-ID/branch 必须传相同哈希
 * P3 按需扩缩：用 atomic_load(&n_active) 算 idx，resize 期间可能短暂
 * 改变路由归属——对 CDR 等无严格顺序依赖的任务安全。
 * Returns NSBC_SHUTDOWN once pool shutdown has begun, NSBC_NOSPC when queues
 * are full, or NSBC_BUSY if the active-submitter counter is exhausted. */
int wpool_submit(worker_pool_t *p, uint64_t hash_key, worker_job_t *job);
int wpool_submit_any(worker_pool_t *p, worker_job_t *job);

unsigned wpool_size(const worker_pool_t *p);       /* 当前活跃 worker 数 */
unsigned wpool_max_size(const worker_pool_t *p);  /* 预分配槽位上限 */
mem_rcu_t *wpool_rcu(worker_pool_t *p);

/* RCU 宽限期推进（由 core 100ms tick 调用；REQ-ARCH-001） */
int wpool_rcu_advance(worker_pool_t *p);

#endif /* NSBC_WORKER_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
