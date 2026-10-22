#ifndef NSBC_WORKER_H
#define NSBC_WORKER_H

/* worker 池（core 层）
 *
 * 每个 worker 线程独占：mem magazine（快路径无锁）、RCU 读者、
 * MPSC 任务队列（REQ-ARCH-008）。net 收包线程按稳定键哈希
 * （branch/Call-ID）投递，同一事务始终落在同一 worker，无跨线程共享写。
 * 队列满 → NSBC_NOSPC：上层按 REQ-HA-003/REQ-SIP-006 回 503（背压）。
 */

#include <pthread.h>
#include "mem.h"
#include "nsbc_atomic.h"
#include "nsbc_common.h"

typedef struct worker_job {
    void (*fn)(void *arg);
    void *arg;
} worker_job_t; /* 内嵌于报文/事务对象，排队零 malloc */

typedef struct worker_pool worker_pool_t;

int wpool_create(worker_pool_t **out, unsigned n, uint32_t queue_cap,
                 mem_rcu_t *rcu);
void wpool_destroy(worker_pool_t *p);

/* hash_key 选队列；同一 Call-ID/branch 必须传相同哈希 */
int wpool_submit(worker_pool_t *p, uint64_t hash_key, worker_job_t *job);
int wpool_submit_any(worker_pool_t *p, worker_job_t *job);

unsigned wpool_size(const worker_pool_t *p);
mem_rcu_t *wpool_rcu(worker_pool_t *p);

/* RCU 宽限期推进（由 core 100ms tick 调用；REQ-ARCH-001） */
int wpool_rcu_advance(worker_pool_t *p);

#endif /* NSBC_WORKER_H */
