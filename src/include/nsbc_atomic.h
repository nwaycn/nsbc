#ifndef NSBC_ATOMIC_H
#define NSBC_ATOMIC_H

/* REQ-ENV-003 / REQ-AICODE-004：GCC 4.8 兼容原子子集
 *
 * 核心代码禁止 stdatomic.h / C11 _Atomic（4.8 支持不完整）；
 * 跨线程字段一律经本封装的 __atomic/__sync 内置函数访问，
 * 禁止把普通字段指针强转后冒充原子访问。
 * 内存序语义：默认 acq_rel；RCU 读锁宣告必须 seq_cst（见 sat_rmb_seqcst）。
 */

#include <stdint.h>

#define NSBC_CACHELINE 64
#define NSBC_CACHELINE_ALIGNED __attribute__((aligned(NSBC_CACHELINE)))

typedef volatile unsigned long nsbc_atomic_t;

#define sat_init(v, i) ((v) = (unsigned long)(i))

#define sat_read(v) __atomic_load_n(&(v), __ATOMIC_ACQUIRE)
#define sat_read_relaxed(v) __atomic_load_n(&(v), __ATOMIC_RELAXED)
#define sat_write(v, n) __atomic_store_n(&(v), (unsigned long)(n), __ATOMIC_RELEASE)

#define sat_add(v, n) __atomic_add_fetch(&(v), (unsigned long)(n), __ATOMIC_ACQ_REL)
#define sat_sub(v, n) __atomic_sub_fetch(&(v), (unsigned long)(n), __ATOMIC_ACQ_REL)
#define sat_inc(v) sat_add(v, 1)
#define sat_dec(v) sat_sub(v, 1)

/* 返回 CAS 前的旧值；成功时 *p 已被更新为 n */
static inline unsigned long sat_cas(nsbc_atomic_t *p, unsigned long o,
                                    unsigned long n)
{
    unsigned long old = o;
    __atomic_compare_exchange_n(p, &old, n, 0, __ATOMIC_ACQ_REL,
                                __ATOMIC_ACQUIRE);
    return old;
}

#define sat_cas_ok(p, o, n) (sat_cas((p), (o), (n)) == (unsigned long)(o))

/* 发布/订阅：RCU 式无锁读多写少指针 */
#define sat_load_ptr(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define sat_store_ptr(p, n)                                                    \
    __atomic_store_n((p), (n), __ATOMIC_RELEASE)

/* RCU 读侧临界区宣告（REQ-AICODE-004：必须 seq_cst） */
static inline void sat_rmb_seqcst(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

/* 顺序锁序列号：奇数=写进行中 */
#define sat_seq_begin(v) sat_add((v), 1)
#define sat_seq_end(v) sat_add((v), 1)

#endif /* NSBC_ATOMIC_H */
