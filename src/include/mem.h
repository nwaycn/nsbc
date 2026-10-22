#ifndef NSBC_MEM_H
#define NSBC_MEM_H

/* 统一共享内存 Arena 与对象池（NSBC详细说明书.md 7.1.5 / 7.1.2）
 *
 * REQ-ARCH-005：按域（信令/媒体/转码/录音/临时包）匿名 mmap 的统一 Arena，
 *               自管切分/free list/边界标记，独立水位统计，glibc malloc
 *               不进关键路径；耗尽与 7.1.2 降级策略联动，严禁越域抢占。
 * REQ-ARCH-006：固定大小类型化 slab + per-thread magazine，快路径无锁，
 *               空/满时批量从共享池搬运；线程退出整体归还；释放按对象头
 *               归属分片直接还共享池。
 *
 * 目标平台：Linux（mmap/madvise/pthread）。Arena 本身是线程安全的，
 * 所有跨线程同步只发生在共享池批量搬运与 Arena 块级操作上。
 */

#include "nsbc_common.h"

/* ------------------------------------------------------------------ */
/* 内存域（7.1.5：物理隔离，防止媒体突发拖垮信令）                      */
/* ------------------------------------------------------------------ */
typedef enum {
    MEM_DOMAIN_SIGNAL = 0, /* SIP 事务/对话/会话等信令对象 */
    MEM_DOMAIN_MEDIA = 1,  /* RTP 转发缓冲 */
    MEM_DOMAIN_TRANSCODE = 2, /* 转码 PCM 缓冲 */
    MEM_DOMAIN_RECORD = 3,   /* 录音缓冲 */
    MEM_DOMAIN_PACKET = 4,   /* 临时报文缓冲 */
    MEM_DOMAIN__COUNT
} mem_domain_t;

const char *mem_domain_name(mem_domain_t d);

/* ------------------------------------------------------------------ */
/* 统计与水位（REQ-ARCH-005：总量/已用/分配次数/失败次数/碎片率）        */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t total_bytes; /* 预留总量（预留不预触，实际驻留以页为准） */
    uint64_t used_bytes;  /* 当前已分配的有效载荷总和 */
    uint64_t peak_bytes;  /* used_bytes 历史峰值 */
    uint64_t nalloc;
    uint64_t nfree;
    uint64_t nfail;     /* 空间不足导致的分配失败次数 */
    uint64_t nbadfree;  /* 双重释放/野指针释放次数 */
} mem_stats_t;

/* ------------------------------------------------------------------ */
/* Arena（REQ-ARCH-005）                                               */
/* ------------------------------------------------------------------ */
typedef struct mem_arena mem_arena_t;

typedef struct {
    mem_domain_t domain;
    size_t bytes;     /* 预留字节数，内部向上取整到页；0 用默认 4MiB */
    int hugepage;     /* 非 0：madvise(MADV_HUGEPAGE)，失败静默回退 */
    int debug;        /* 非 0：guard band/canary/双重释放检测（7.1.5） */
} mem_arena_cfg_t;

nsbc_rc_t mem_arena_create(const mem_arena_cfg_t *cfg, mem_arena_t **out);
void mem_arena_destroy(mem_arena_t *a);

/* 失败返回 NULL（不终止进程），调用方按 7.1.2 走降级路径 */
void *mem_arena_alloc(mem_arena_t *a, size_t n);
void mem_arena_free(mem_arena_t *a, void *p);

/* 调试构建：遍历物理块链，校验 magic 与各已分配块 canary；
 * 生产构建同样可调用（无 guard 时仅校验块链一致性）。
 * NSBC_OK=完好，NSBC_ERR=检出越界写/块链损坏。 */
int mem_arena_check(const mem_arena_t *a);

const mem_stats_t *mem_arena_stats(const mem_arena_t *a);
size_t mem_arena_free_payload(const mem_arena_t *a); /* 空闲块有效载荷总和 */
size_t mem_arena_largest_free(const mem_arena_t *a); /* 最大连续空闲块 */

/* 碎片率 = 1 - 最大空闲块 / 空闲总量（0.0~1.0，0 表示完全不碎） */
double mem_arena_frag_rate(const mem_arena_t *a);

/* ------------------------------------------------------------------ */
/* slab 对象池（REQ-ARCH-006）                                         */
/* ------------------------------------------------------------------ */
typedef struct mem_slab mem_slab_t;

/* 构造回调：每个对象生命周期内只执行一次（首次取出时）；对象复用时
 * 不再调用，需要重置的字段由取出方自行初始化（7.1.5）。 */
typedef void (*mem_ctor_fn)(void *obj, void *ud);

typedef struct {
    const char *name;    /* 类型名：sip_msg/transaction/dialog/... */
    size_t obj_size;     /* 单对象有效大小 */
    int capacity;        /* 对象总数（按峰值预留，见容量公式） */
    int batch;           /* magazine 批量搬运大小，0 用默认 8 */
    mem_arena_t *arena;  /* 归属域 Arena（物理隔离） */
    mem_ctor_fn ctor;    /* 可为 NULL */
    void *ud;
} mem_slab_cfg_t;

nsbc_rc_t mem_slab_create(const mem_slab_cfg_t *cfg, mem_slab_t **out);
void mem_slab_destroy(mem_slab_t *s);

/* 不经 magazine 的共享路径（取一次分片锁，用于管理面/批量取空后） */
void *mem_slab_alloc_direct(mem_slab_t *s);

/* 按对象头归属直接还共享池，不要求归还到原分配线程（7.1.5） */
void mem_obj_release(void *obj);

typedef struct {
    uint64_t nalloc;
    uint64_t nrelease;
    uint64_t nfail;
    int nfree;
    int capacity;
} mem_slab_stat_t;
void mem_slab_get_stat(const mem_slab_t *s, mem_slab_stat_t *out);

/* 容量公式（7.1.5）：
 *   capacity = burst_factor × CPS × 平均事务生命周期秒数
 * burst_factor 取 32~64（突发系数），结果由压测校准。 */
int mem_recommended_capacity(int cps, double avg_lifetime_s, int burst_factor);

/* ------------------------------------------------------------------ */
/* per-thread magazine（REQ-ARCH-006）                                 */
/* ------------------------------------------------------------------ */
typedef struct mem_mag mem_mag_t;

/* 每个工作线程创建一个；destroy 时本地缓存整体归还共享池，不泄漏 */
mem_mag_t *mem_mag_create(void);
void mem_mag_destroy(mem_mag_t *m);

void *mem_mag_alloc(mem_mag_t *m, mem_slab_t *s); /* 命中本地：无锁无原子 */

/* 释放到本线程对应缓存；若该 slab 未挂载到本 magazine（如跨线程
 * 收到的对象），自动按对象头归属直接还共享池。 */
void mem_mag_free(mem_mag_t *m, void *obj);

/* ------------------------------------------------------------------ */
/* 内存池水位与降级策略（7.1.2）                                       */
/* ------------------------------------------------------------------ */
typedef enum {
    MEM_LEVEL_NORMAL = 0, /* 水位 < 70% */
    MEM_LEVEL_WARN = 1,   /* 70%~85% */
    MEM_LEVEL_CRIT = 2    /* >= 85%（耗尽在即/已耗尽） */
} mem_level_t;

mem_level_t mem_level_for_ratio(double used_ratio);

/* 增强业务位：耗尽时先降级增强业务，保证核心信令与基础通话 */
#define MEM_FEAT_RECORD    0x01u
#define MEM_FEAT_TRANSCODE 0x02u

typedef struct mem_policy {
    uint32_t enabled;  /* 系统启用的增强能力 */
    uint32_t active;   /* 当前允许新呼叫使用的能力 */
    int degraded;      /* 1=增强业务已降级 */
    mem_level_t level[MEM_DOMAIN__COUNT];
} mem_policy_t;

void mem_policy_init(mem_policy_t *p, uint32_t enabled_features);

/* 用某域实际水位驱动状态机：媒体/转码/录音域进入 CRIT 即降级增强
 * 业务；回落 NORMAL（<70%）后平滑解除。返回该域最新级别。
 * 注意：信令域有独立 Arena，媒体域 CRIT 不影响信令分配。 */
mem_level_t mem_policy_observe(mem_policy_t *p, mem_domain_t d,
                               double used_ratio);
int mem_policy_allow(const mem_policy_t *p, uint32_t feat);

/* ------------------------------------------------------------------ */
/* 无锁环形队列（REQ-ARCH-008，7.1.5）                                 */
/* ------------------------------------------------------------------ */
/* 共同约定：
 * - 固定容量（容量向上取整为 2 的幂），槽位存放 void*；
 * - 满则返回 NSBC_ERR（背压：丢弃/断连策略由上层决定），严禁无界增长；
 * - 队列只搬运指针，不拥有对象所有权语义（对象归属/释放由调用方约定，
 *   典型为配合引用计数与 RCU 回收）。
 */

/* ---- SPSC：单生产者单消费者，读侧/写侧计数器缓存行隔离 ---- */
typedef struct mem_spsc mem_spsc_t;

/* arena 非 NULL 时槽位从该域 Arena 预留；否则按缓存行对齐 malloc */
mem_spsc_t *mem_spsc_create(uint32_t capacity, mem_arena_t *arena);
void mem_spsc_destroy(mem_spsc_t *q);
int mem_spsc_push(mem_spsc_t *q, void *msg);  /* NSBC_OK / NSBC_ERR(满) */
int mem_spsc_pop(mem_spsc_t *q, void **out);  /* NSBC_OK / NSBC_ERR(空) */
uint32_t mem_spsc_size(const mem_spsc_t *q);

/* ---- MPSC：多生产者单消费者（Vyukov 有界队列） ---- */
/* 每槽位 sequence + 生产者 CAS 抢入队位置；消费者唯一（API 契约：
 * 同时只能有一个线程 pop），出队无 CAS。 */
typedef struct mem_mpsc mem_mpsc_t;

mem_mpsc_t *mem_mpsc_create(uint32_t capacity, mem_arena_t *arena);
void mem_mpsc_destroy(mem_mpsc_t *q);
int mem_mpsc_push(mem_mpsc_t *q, void *msg);
int mem_mpsc_pop(mem_mpsc_t *q, void **out);
uint32_t mem_mpsc_size(const mem_mpsc_t *q); /* 近似值（多生产者） */

/* ------------------------------------------------------------------ */
/* RCU 宽限期回收（7.1.5：引用计数 + RCU，读侧无锁）                    */
/* ------------------------------------------------------------------ */
typedef struct mem_rcu mem_rcu_t;
typedef struct mem_rcu_reader mem_rcu_reader_t;
typedef void (*mem_rcu_free_fn)(void *ptr);

mem_rcu_t *mem_rcu_create(void);
/* 销毁：强制释放所有尚未度过宽限期的对象（仅用于停机/所有读者已退出） */
void mem_rcu_destroy(mem_rcu_t *r);

/* 每个使用读侧临界区的线程显式注册/注销（注册稀少） */
mem_rcu_reader_t *mem_rcu_thread_join(mem_rcu_t *r);
void mem_rcu_thread_leave(mem_rcu_reader_t *rd);

/* 读侧临界区：仅一次线程本地原子宣告，无全局锁、无 CAS；
 * 临界区内读取到的指针在 unlock 之前不会被回收。 */
void mem_rcu_read_lock(mem_rcu_reader_t *rd);
void mem_rcu_read_unlock(mem_rcu_reader_t *rd);

/* 写者：对象从读者可见结构中摘除（release 发布）后调用 retire 延迟
 * 释放；内部自动尝试推进宽限期，推进不动则保留（写者承担背压）。
 * 也可显式调用 mem_rcu_advance 驱动（如周期性回收线程）。 */
void mem_rcu_retire(mem_rcu_t *r, void *ptr, mem_rcu_free_fn fn);
int mem_rcu_advance(mem_rcu_t *r); /* NSBC_OK=推进了一个宽限期 */
uint64_t mem_rcu_pending(const mem_rcu_t *r);
uint64_t mem_rcu_reclaimed(const mem_rcu_t *r);

/* 便利封装：延迟归还到对象所属 slab（配合 mem_obj_release） */
void mem_rcu_retire_obj(mem_rcu_t *r, void *obj);

#endif /* NSBC_MEM_H */
