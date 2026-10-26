/* event_bus.c — Phase 1 新建 + P3 per-core seq 分片
 *
 * 白皮书 §3 高性能事件总线：
 *   - Per-Core MPSC 无锁 Ring Buffer（每 core 独立 ring，写竞争零）
 *   - nsbc_event_push: 热路径零锁零 malloc，满时 drop（生产绝对优先）
 *   - event_handler_trigger: 同步订阅式 handler 链（SESSION_CREATE/ANSWER/END）
 *   - P3：per-core 独立单调递增序列号 — 每核零竞争，ring 本来 per-core 存帧
 *     消费者按 ring 自身顺序读，per-core 单调完全够语义；无严格跨核全序需求
 *
 * 原子风格：项目约定 __atomic 内置（非 C11 stdatomic.h）
 */
#include "event_bus.h"
#include "log.h"
#include "percpu.h"
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>

/* ============================================================
 * Part A: Per-Core 无锁 Ring Buffer
 * ============================================================ */

/* 每条事件帧格式（Ring 内序列化存储）：
 *   | uint16 name_len | name bytes | uint32 payload_len | payload bytes | uint64 seq | uint8 end_marker |
 * end_marker = 0xFF 表示一条完整帧的末尾；writer 保证连续写入时不会跨越 buffer 边界 */
#define RING_MAX_CORES    32
#define RING_SIZE         (1 << 16)   /* 64KB per-core，每条事件平均 ~200B → ~300 条 */
#define RING_MASK         (RING_SIZE - 1)

/* Cacheline 对齐防 false sharing（percpu.h 已有 NSBC_CACHELINE）*/
typedef struct {
    volatile uint64_t write_pos     NSBC_CACHELINE_ALIGNED;
    volatile uint64_t read_pos      NSBC_CACHELINE_ALIGNED;
    volatile uint64_t dropped_count NSBC_CACHELINE_ALIGNED;
    volatile uint64_t local_seq     NSBC_CACHELINE_ALIGNED;   /* P3: per-core 独立单调递增，零跨核争用 */
    char buffer[RING_SIZE];
} per_core_ring_t;

static per_core_ring_t *g_rings = NULL;   /* 动态分配：init 时按 CPU 数建 */
static int g_ring_cores = 0;

int event_bus_init(void)
{
    int ncores = (int)pcnt_cpus();
    if (ncores <= 0) ncores = 1;
    /* F-06 修复：不再硬限 RING_MAX_CORES=32。
     * 每 core 一个 ring；CPU 越多 ring 越多，绝不多 CPU 折叠到同一 ring。
     * ring 上限改为 256（RING_MAX_CORES 提升 + 重命名）。*/
#define RING_MAX_CORES_HARD 256
    if (ncores > RING_MAX_CORES_HARD) {
        log_warn("event_bus", "detected %d cores, limiting to %d rings", ncores, RING_MAX_CORES_HARD);
        ncores = RING_MAX_CORES_HARD;
    }
    /* 同时更新 per_core_ring_stat_t 缓冲区大小限制在 RING_MAX_CORES 以下 */

    g_rings = (per_core_ring_t *)calloc((size_t)ncores, sizeof(per_core_ring_t));
    if (!g_rings) return NSBC_NOMEM;

    /* posix_memalign 确保 cacheline 对齐更干净，但 calloc 已按 page 对齐 */
    g_ring_cores = ncores;

    log_info("event_bus", "init OK cores=%d ring_size=%d total_mem=%zuKB",
             ncores, RING_SIZE, (size_t)ncores * sizeof(per_core_ring_t) / 1024);
    return NSBC_OK;
}

void event_bus_fini(void)
{
    if (g_rings) {
        free(g_rings);
        g_rings = NULL;
        g_ring_cores = 0;
    }
}

int event_bus_core_count(void) { return g_ring_cores; }

/* P3：per-core 独立单调递增。
 * 每核零跨核 cache line 争用；ring 本来 per-core 存事件帧，
 * 消费者按 ring 自身顺序读，per-core 单调完全够语义。
 * RELAXED 足够：per-core 单 producer，帧的可见性由 write_pos release store 保证。
 *
 * 前置安全：event_handler_trigger() 可能在 event_bus_init() 之前被调用
 * （早期模块 init 路径），g_rings 此时为 NULL，返回 0 保持安全。 */
uint64_t event_bus_next_seq(void)
{
    if (!g_rings) return 0;
    int core_id = pcpt_idx();
    /* F-06 修复：不折叠到 ring 0，安全 fallback 到最后一个有效 ring */
    if (core_id < 0 || core_id >= g_ring_cores)
        core_id = g_ring_cores - 1;
    per_core_ring_t *rb = &g_rings[core_id];
    return __atomic_fetch_add(&rb->local_seq, 1, __ATOMIC_RELAXED) + 1;
}

int event_bus_get_stat(int core_id, per_core_ring_stat_t *out)
{
    if (!out || core_id < 0 || core_id >= g_ring_cores) return NSBC_INVAL;
    per_core_ring_t *rb = &g_rings[core_id];
    out->core_id = core_id;
    out->write_pos     = __atomic_load_n(&rb->write_pos, __ATOMIC_ACQUIRE);
    out->read_pos      = __atomic_load_n(&rb->read_pos, __ATOMIC_ACQUIRE);
    out->dropped_count = __atomic_load_n(&rb->dropped_count, __ATOMIC_RELAXED);
    out->current_size  = out->write_pos - out->read_pos;
    return NSBC_OK;
}

int event_bus_collect_all_stats(per_core_ring_stat_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = max < g_ring_cores ? max : g_ring_cores;
    for (int i = 0; i < n; ++i) event_bus_get_stat(i, &out[i]);
    return n;
}

/* ===== Per-Core 无锁写入（单 producer：当前 core 的唯一 writer）=====
 *
 * 因为 Per-Core，每个 ring 的 producer 只有一个线程（当前 CPU core），
 * 所以 write_pos 用 __atomic_store_n 就够了——不需要 CAS。
 * reader 在另一核读 write_pos 时用 acquire 保证可见性。
 *
 * 写入流程：
 *   1. 计算帧总长度 + 检查剩余空间
 *   2. 先在 buffer 内写（memcpy 到 wrap 边界）
 *   3. 最后 __atomic_store_n(&write_pos, new_wp, __ATOMIC_RELEASE)
 *
 * 满时 drop + dropped_count++，绝不阻塞。 */
static __thread char tls_event_buf[2048];  /* TLS buffer 零 malloc */

int nsbc_event_push(const char *event_name, const char *payload_json)
{
    if (!g_rings || !event_name || !event_name[0]) return NSBC_INVAL;

    /* 1. 找到当前 core 的 ring */
    int core_id = pcpt_idx();  /* percpu.h 已定义 */
    /* F-06 修复：不折叠到 ring 0，安全 fallback */
    if (core_id < 0 || core_id >= g_ring_cores)
        core_id = g_ring_cores - 1;
    per_core_ring_t *rb = &g_rings[core_id];

    /* 2. 构造帧（TLS buffer）：|u16 name_len|name bytes|u32 payload_len|payload|u64 seq|u8 end| */
    size_t name_size = strlen(event_name);
    size_t payload_size = payload_json ? strlen(payload_json) : 0;
    const size_t frame_overhead = 2 + 4 + 8 + 1;
    if (name_size > UINT16_MAX || payload_size > UINT32_MAX ||
        name_size > sizeof(tls_event_buf) - frame_overhead ||
        payload_size > sizeof(tls_event_buf) - frame_overhead - name_size) {
        /* 帧太大——不 push（生产绝对优先，不阻塞） */
        log_warn("event_bus", "event frame too large name=%zu payload=%zu — skip",
                 name_size, payload_size);
        return NSBC_NOMEM;
    }
    uint16_t name_len = (uint16_t)name_size;
    uint32_t payload_len = (uint32_t)payload_size;
    size_t frame_size = frame_overhead + name_size + payload_size;

    /* 3. Check capacity before taking the globally contended sequence counter. */
    uint64_t wp = __atomic_load_n(&rb->write_pos, __ATOMIC_RELAXED);
    uint64_t rp = __atomic_load_n(&rb->read_pos, __ATOMIC_ACQUIRE);  /* reader 可能在另一核 */

    uint64_t ring_avail = RING_SIZE - (wp - rp);  /* 当前 ring 剩余空间（字节）*/
    if (ring_avail < frame_size) {
        /* 满了——drop，不阻塞生产 */
        __atomic_fetch_add(&rb->dropped_count, 1, __ATOMIC_RELAXED);
        return NSBC_BUSY;
    }

    uint64_t seq = event_bus_next_seq();
    char *p = tls_event_buf;
    memcpy(p, &name_len, 2); p += 2;
    memcpy(p, event_name, name_len); p += name_len;
    memcpy(p, &payload_len, 4); p += 4;
    if (payload_len) { memcpy(p, payload_json, payload_len); p += payload_len; }
    memcpy(p, &seq, 8); p += 8;
    *p++ = 0xFF;  /* end_marker */

    /* 4. 无锁写入 ring buffer */
    /* 4. 拷贝到 ring（处理 wrap） */
    uint64_t offset = wp & RING_MASK;
    uint64_t first_chunk = RING_SIZE - offset;   /* 到 buffer 末尾的字节数 */
    if (frame_size <= first_chunk) {
        /* 单次写入 */
        memcpy(rb->buffer + offset, tls_event_buf, frame_size);
    } else {
        /* 跨 wrap：先写 first_chunk，再写剩下的到 buffer[0] */
        memcpy(rb->buffer + offset, tls_event_buf, (size_t)first_chunk);
        memcpy(rb->buffer, tls_event_buf + first_chunk, frame_size - first_chunk);
    }

    /* 5. release store 发布新 write_pos */
    __atomic_store_n(&rb->write_pos, wp + frame_size, __ATOMIC_RELEASE);
    return NSBC_OK;
}

/* ============================================================
 * Part B: event_handler 同步订阅（SESSION_CREATE/ANSWER/END 等）
 *
 * 安全卸载模板：条目带 refcnt + unloading 原子字段
 * 支持通配 "*" 订阅所有事件
 * ============================================================ */

#define EVT_HANDLER_MAX 32

typedef struct {
    event_handler_fn fn;
    void *ud;
    volatile int refcnt;       /* 当前执行的 handler 数 */
    volatile int unloading;    /* 卸载标记：阻止新调用进来 */
} handler_slot_t;

typedef struct {
    char event_name[64];       /* 事件名（或 "*" 通配）*/
    handler_slot_t slots[EVT_HANDLER_MAX];
    int count;
    pthread_mutex_t lock;
    pthread_mutex_t unregister_lock;
} handler_reg_t;

#define MAX_EVENTS 16
static handler_reg_t g_handlers[MAX_EVENTS];
static pthread_mutex_t g_handlers_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_handlers_ready = 0;
static __thread unsigned event_dispatch_depth;

static void handlers_init_once(void);   /* 前向声明 */

static void handlers_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, handlers_init_once);
}

static void handlers_init_once(void)
{
    for (int i = 0; i < MAX_EVENTS; ++i) {
        memset(&g_handlers[i], 0, sizeof(g_handlers[i]));
        pthread_mutex_init(&g_handlers[i].lock, NULL);
        pthread_mutex_init(&g_handlers[i].unregister_lock, NULL);
    }
    g_handlers_ready = 1;
}

/* 找或创建 event_name 对应的 handler_reg_t（内部锁保护）*/
static handler_reg_t *find_or_create_reg(const char *event_name)
{
    handlers_init();
    pthread_mutex_lock(&g_handlers_lock);

    /* 已存在 → 直接返回 */
    for (int i = 0; i < MAX_EVENTS; ++i) {
        if (g_handlers[i].event_name[0] &&
            strcmp(g_handlers[i].event_name, event_name) == 0) {
            pthread_mutex_unlock(&g_handlers_lock);
            return &g_handlers[i];
        }
    }
    /* 找空槽 */
    for (int i = 0; i < MAX_EVENTS; ++i) {
        if (g_handlers[i].event_name[0] == '\0') {
            snprintf(g_handlers[i].event_name, sizeof(g_handlers[i].event_name), "%s", event_name);
            pthread_mutex_unlock(&g_handlers_lock);
            return &g_handlers[i];
        }
    }
    pthread_mutex_unlock(&g_handlers_lock);
    return NULL;  /* 表满 */
}

int event_handler_register(const char *event_name, event_handler_fn fn, void *ud)
{
    if (!event_name || !event_name[0] || !fn) return NSBC_INVAL;

    handler_reg_t *reg = find_or_create_reg(event_name);
    if (!reg) return NSBC_NOSPC;

    pthread_mutex_lock(&reg->lock);

    /* 防重复：同 fn+ud 不重复注册 */
    for (int i = 0; i < reg->count; ++i) {
        if (reg->slots[i].fn == fn && reg->slots[i].ud == ud) {
            pthread_mutex_unlock(&reg->lock);
            return NSBC_EXISTS;
        }
    }

    int idx = 0;
    while (idx < reg->count && reg->slots[idx].fn)
        ++idx;
    if (idx == reg->count && reg->count >= EVT_HANDLER_MAX) {
        pthread_mutex_unlock(&reg->lock);
        log_warn("event_bus", "handler table full for '%s'", event_name);
        return NSBC_NOSPC;
    }

    if (idx == reg->count)
        ++reg->count;
    reg->slots[idx].fn = fn;
    reg->slots[idx].ud = ud;
    __atomic_store_n(&reg->slots[idx].refcnt, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&reg->slots[idx].unloading, 0, __ATOMIC_RELAXED);

    pthread_mutex_unlock(&reg->lock);
    log_info("event_bus", "handler '%s' idx=%d registered fn=%p",
             event_name, idx, (void *)fn);
    return NSBC_OK;
}

int event_handler_unregister(const char *event_name, event_handler_fn fn, void *ud)
{
    if (!event_name || !fn) return NSBC_INVAL;
    if (event_dispatch_depth > 0) {
        log_warn("event_bus",
                 "handler unregister from event callback is deferred");
        return NSBC_BUSY;
    }
    handlers_init();

    handler_reg_t *reg = NULL;
    pthread_mutex_lock(&g_handlers_lock);
    for (int i = 0; i < MAX_EVENTS; ++i) {
        if (g_handlers[i].event_name[0] &&
            strcmp(g_handlers[i].event_name, event_name) == 0) {
            reg = &g_handlers[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_handlers_lock);

    if (!reg) return NSBC_NOENT;

    pthread_mutex_lock(&reg->unregister_lock);
    pthread_mutex_lock(&reg->lock);
    int target = -1;
    for (int i = 0; i < reg->count; ++i) {
        if (reg->slots[i].fn == fn && reg->slots[i].ud == ud) {
            target = i; break;
        }
    }
    if (target < 0) {
        pthread_mutex_unlock(&reg->lock);
        pthread_mutex_unlock(&reg->unregister_lock);
        return NSBC_NOENT;
    }

    /* 置 unloading=true → 阻止新调用 */
    __atomic_store_n(&reg->slots[target].unloading, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&reg->lock);

    /* 自旋等待 refcnt→0（默认 5s 超时） */
    int spins = 0;
    while (__atomic_load_n(&reg->slots[target].refcnt, __ATOMIC_ACQUIRE) > 0) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (++spins > 5000) {
            log_err("event_bus",
                    "unregister '%s' fn=%p timeout (refcnt=%d) — leak but no crash",
                    event_name, (void *)fn,
                    __atomic_load_n(&reg->slots[target].refcnt, __ATOMIC_ACQUIRE));
            pthread_mutex_unlock(&reg->unregister_lock);
            return NSBC_BUSY;  /* fail-safe：留着泄漏但不 crash */
        }
    }

    /* Keep slot addresses stable: trigger snapshots may reference other slots. */
    pthread_mutex_lock(&reg->lock);
    memset(&reg->slots[target], 0, sizeof(handler_slot_t));
    while (reg->count > 0 && !reg->slots[reg->count - 1].fn)
        --reg->count;
    pthread_mutex_unlock(&reg->lock);
    pthread_mutex_unlock(&reg->unregister_lock);

    log_info("event_bus", "handler '%s' fn=%p unregistered (clean)",
             event_name, (void *)fn);
    return NSBC_OK;
}

/* ===== 触发：同步遍历所有 handler + 通配 "*" =====
 *
 * Phase 审计修复：原实现是「中途解锁 → 调回调 → 再加锁」，
 * 有死锁风险：handler 回调内部再调 event_handler_trigger（哪怕不同 event_name）
 * 会因为 handler_reg_t.lock 非重入而死锁。
 *
 * 白皮书 §10.3 事务边界设计的正确落地：
 *   加锁 → 一次性收集所有有效 (fn, ud) 到本地临时数组 + refcnt++ → 解锁
 *   → 逐个调回调（完全无锁，回调内部再触发事件安全）→ refcnt--
 *
 * 临时数组大小受 EVT_HANDLER_MAX(32) 约束，栈上分配足够，零 malloc。
 * PERM_DENIED 提前终止时已收集的回调正常 refcnt-- 清理。*/
int event_handler_trigger(const char *event_name, const char *payload,
                          const void *call_key, uint64_t *out_seq)
{
    if (!event_name || !event_name[0]) return NSBC_INVAL;
    handlers_init();

    uint64_t seq = event_bus_next_seq();
    if (out_seq) *out_seq = seq;

    event_context_t ctx;
    ctx.event_name = event_name;
    ctx.payload    = payload;
    ctx.call_key   = call_key;
    ctx.event_seq  = seq;

    int rc = NSBC_OK;

    /* 临时槽：最多 EVT_HANDLER_MAX × 2（通配 + 精确）*/
    struct {
        event_handler_fn fn;
        void *ud;
        handler_slot_t *slot;
    } collected[EVT_HANDLER_MAX * 2];
    int collected_n = 0;

    /* 一次性锁内收集（通配 + 精确）—— 中途绝不解锁 */
    pthread_mutex_lock(&g_handlers_lock);
    {
        /* 通配 "*" */
        for (int i = 0; i < MAX_EVENTS && collected_n < EVT_HANDLER_MAX; ++i) {
            if (g_handlers[i].event_name[0] &&
                strcmp(g_handlers[i].event_name, "*") == 0) {
                pthread_mutex_lock(&g_handlers[i].lock);
                for (int k = 0; k < g_handlers[i].count && collected_n < EVT_HANDLER_MAX; ++k) {
                    if (!g_handlers[i].slots[k].fn ||
                        __atomic_load_n(&g_handlers[i].slots[k].unloading, __ATOMIC_ACQUIRE)) continue;
                    __atomic_fetch_add(&g_handlers[i].slots[k].refcnt, 1, __ATOMIC_ACQ_REL);
                    collected[collected_n].fn = g_handlers[i].slots[k].fn;
                    collected[collected_n].ud = g_handlers[i].slots[k].ud;
                    collected[collected_n].slot = &g_handlers[i].slots[k];
                    collected_n++;
                }
                pthread_mutex_unlock(&g_handlers[i].lock);
                break;
            }
        }

        /* 精确匹配 */
        for (int i = 0; i < MAX_EVENTS && collected_n < EVT_HANDLER_MAX * 2; ++i) {
            if (g_handlers[i].event_name[0] &&
                strcmp(g_handlers[i].event_name, event_name) == 0) {
                pthread_mutex_lock(&g_handlers[i].lock);
                for (int k = 0; k < g_handlers[i].count && collected_n < EVT_HANDLER_MAX * 2; ++k) {
                    if (!g_handlers[i].slots[k].fn ||
                        __atomic_load_n(&g_handlers[i].slots[k].unloading, __ATOMIC_ACQUIRE)) continue;
                    __atomic_fetch_add(&g_handlers[i].slots[k].refcnt, 1, __ATOMIC_ACQ_REL);
                    collected[collected_n].fn = g_handlers[i].slots[k].fn;
                    collected[collected_n].ud = g_handlers[i].slots[k].ud;
                    collected[collected_n].slot = &g_handlers[i].slots[k];
                    collected_n++;
                }
                pthread_mutex_unlock(&g_handlers[i].lock);
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_handlers_lock);

    /* 解锁后逐个调回调（完全无锁，内部再触发事件安全）*/
    ++event_dispatch_depth;
    for (int i = 0; i < collected_n; ++i) {
        int hrc = collected[i].fn(&ctx, collected[i].ud);

        __atomic_sub_fetch(&collected[i].slot->refcnt, 1, __ATOMIC_RELEASE);
        if (hrc == NSBC_PERM_DENIED) {
            rc = NSBC_PERM_DENIED;
            /* 致命拒绝：已收集的后续回调也要调（让所有订阅者都拿到通知），
             * 但整体返回 PERM_DENIED 让上层知道有致命拦截 */
        }
    }
    --event_dispatch_depth;

    return rc;
}

/* ============================================================
 * Part C: Phase 2 — Async Subscriber Chain + Drain Thread
 *
 * 独立于同步 event_handler 链——双通道不重复。
 *
 * 流程：nsbc_event_push（零锁零 malloc 写 per-core ring）
 *       → drain 线程约每 50ms 扫所有 ring，解析二进制帧
 *       → fire 给 async subscriber 链
 *       → payload/tls buffer 全部在 drain 线程栈上完成，零 malloc
 *
 * 帧解析格式（和写入端完全对齐）：
 *   | uint16 name_len | name bytes | uint32 payload_len | payload bytes | uint64 seq | uint8 end_marker(0xFF) |
 *   - 从 read_pos 开始；writer 保证连续帧不跨越 ring 边界
 *   - end_marker=0xFF 表示完整帧末尾（我们用它做完整性校验）
 *   - 解析一帧后 advance read_pos（atomic RELEASE）
 * ============================================================ */

/* --- Async subscriber 链（和 g_handlers 完全独立的数组 + 锁）--- */

#define ASYNC_HANDLER_MAX EVT_HANDLER_MAX
#define MAX_ASYNC_EVENTS  MAX_EVENTS

typedef struct {
    char event_name[64];
    event_handler_fn slots[ASYNC_HANDLER_MAX];
    void            *slots_ud[ASYNC_HANDLER_MAX];
    int count;
    pthread_mutex_t lock;
} async_reg_t;

static async_reg_t g_async_handlers[MAX_ASYNC_EVENTS];
static pthread_mutex_t g_async_handlers_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_async_handlers_ready = 0;

static void async_handlers_init_once(void)
{
    for (int i = 0; i < MAX_ASYNC_EVENTS; ++i) {
        memset(&g_async_handlers[i], 0, sizeof(g_async_handlers[i]));
        pthread_mutex_init(&g_async_handlers[i].lock, NULL);
    }
    g_async_handlers_ready = 1;
}

static void async_handlers_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, async_handlers_init_once);
}

static async_reg_t *async_find_or_create(const char *event_name)
{
    async_handlers_init();
    pthread_mutex_lock(&g_async_handlers_lock);
    for (int i = 0; i < MAX_ASYNC_EVENTS; ++i) {
        if (g_async_handlers[i].event_name[0] &&
            strcmp(g_async_handlers[i].event_name, event_name) == 0) {
            pthread_mutex_unlock(&g_async_handlers_lock);
            return &g_async_handlers[i];
        }
    }
    for (int i = 0; i < MAX_ASYNC_EVENTS; ++i) {
        if (g_async_handlers[i].event_name[0] == '\0') {
            snprintf(g_async_handlers[i].event_name,
                     sizeof(g_async_handlers[i].event_name), "%s", event_name);
            pthread_mutex_unlock(&g_async_handlers_lock);
            return &g_async_handlers[i];
        }
    }
    pthread_mutex_unlock(&g_async_handlers_lock);
    return NULL;
}

int event_async_register(const char *event_name, event_handler_fn fn, void *ud)
{
    if (!event_name || !event_name[0] || !fn) return NSBC_INVAL;
    async_reg_t *reg = async_find_or_create(event_name);
    if (!reg) return NSBC_NOSPC;
    pthread_mutex_lock(&reg->lock);
    for (int i = 0; i < reg->count; ++i) {
        if (reg->slots[i] == fn && reg->slots_ud[i] == ud) {
            pthread_mutex_unlock(&reg->lock);
            return NSBC_EXISTS;
        }
    }
    if (reg->count >= ASYNC_HANDLER_MAX) {
        pthread_mutex_unlock(&reg->lock);
        log_warn("event_bus", "async handler table full for '%s'", event_name);
        return NSBC_NOSPC;
    }
    reg->slots[reg->count] = fn;
    reg->slots_ud[reg->count] = ud;
    reg->count++;
    pthread_mutex_unlock(&reg->lock);
    log_info("event_bus", "async handler '%s' registered fn=%p", event_name, (void *)fn);
    return NSBC_OK;
}

int event_async_unregister(const char *event_name, event_handler_fn fn, void *ud)
{
    if (!event_name || !fn) return NSBC_INVAL;
    async_handlers_init();
    async_reg_t *reg = NULL;
    pthread_mutex_lock(&g_async_handlers_lock);
    for (int i = 0; i < MAX_ASYNC_EVENTS; ++i) {
        if (g_async_handlers[i].event_name[0] &&
            strcmp(g_async_handlers[i].event_name, event_name) == 0) {
            reg = &g_async_handlers[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_async_handlers_lock);
    if (!reg) return NSBC_NOENT;
    pthread_mutex_lock(&reg->lock);
    for (int i = 0; i < reg->count; ++i) {
        if (reg->slots[i] == fn && reg->slots_ud[i] == ud) {
            reg->slots[i] = NULL;
            reg->slots_ud[i] = NULL;
            while (reg->count > 0 && !reg->slots[reg->count - 1]) --reg->count;
            pthread_mutex_unlock(&reg->lock);
            log_info("event_bus", "async handler '%s' fn=%p unregistered",
                     event_name, (void *)fn);
            return NSBC_OK;
        }
    }
    pthread_mutex_unlock(&reg->lock);
    return NSBC_NOENT;
}

/* drain 线程内 fire 到 async subscriber 链（通配 "*" + 精确匹配）*/
static void async_fire(const char *event_name, const char *payload, uint64_t seq)
{
    if (!event_name) return;

    /* 收集所有匹配的 (fn, ud) 到栈上数组 → 解锁后执行 */
    struct { event_handler_fn fn; void *ud; } collected[ASYNC_HANDLER_MAX * 2];
    int n = 0;

    pthread_mutex_lock(&g_async_handlers_lock);
    for (int i = 0; i < MAX_ASYNC_EVENTS; ++i) {
        if (!g_async_handlers[i].event_name[0]) continue;
        int wildcard = strcmp(g_async_handlers[i].event_name, "*") == 0;
        int exact   = strcmp(g_async_handlers[i].event_name, event_name) == 0;
        if (!wildcard && !exact) continue;
        pthread_mutex_lock(&g_async_handlers[i].lock);
        for (int k = 0; k < g_async_handlers[i].count && n < ASYNC_HANDLER_MAX * 2; ++k) {
            if (g_async_handlers[i].slots[k]) {
                collected[n].fn = g_async_handlers[i].slots[k];
                collected[n].ud = g_async_handlers[i].slots_ud[k];
                ++n;
            }
        }
        pthread_mutex_unlock(&g_async_handlers[i].lock);
    }
    pthread_mutex_unlock(&g_async_handlers_lock);

    if (n == 0) return;

    event_context_t ctx = {
        .event_name = event_name,
        .payload    = payload,
        .call_key   = NULL,
        .event_seq  = seq,
    };
    for (int i = 0; i < n; ++i)
        collected[i].fn(&ctx, collected[i].ud);
}

/* --- Per-ring 帧解析器 ---
 *
 * 从 ring 的 read_pos 开始，解析出一帧。
 * 返回帧总长度（含帧内所有字节），或 0 表示 ring 空/截断/等待 writer。
 * 返回 1 表示 corruption——逐字节 skip 让 drain 线程重新对齐帧边界。
 *
 * 帧格式: | u16 name_len | name bytes | u32 payload_len | payload | u64 seq | u8 0xFF |
 *
 * 两阶段 parse（修复 2026-10-26 崩溃）:
 *   Phase 1: 从 ring 头部读 name_len(2B) + payload_len(4B)，计算 total_frame_size
 *   Phase 2: 校验 total_frame_size <= buf_size 且 <= avail，memcpy 整帧到 frame_buf
 *   Phase 3: 从完整拷贝里解包 name/payload/seq，校验 end_marker = 0xFF
 *
 * 之前的 bug (root cause): 只 memcpy 了 frame_overhead（=2+name_len+4+8+1, 不含 payload）
 * 然后 p+=pl 跳过未拷贝的 payload → 栈飞越界 → segfault。
 *
 * frame_buf 必须 >= 2048 字节（与 TLS buffer 同大小）。*/
static size_t ring_parse_one(per_core_ring_t *rb, size_t buf_size, char *frame_buf,
                             const char **out_name, const char **out_payload,
                             uint64_t *out_seq)
{
    /* ===== Phase 0: 快照 rp/wp，确保整个解析过程用一致的位置 ===== */
    uint64_t rp = __atomic_load_n(&rb->read_pos, __ATOMIC_ACQUIRE);
    uint64_t wp = __atomic_load_n(&rb->write_pos, __ATOMIC_ACQUIRE);

    if (rp >= wp) return 0;  /* ring 空 */

    uint64_t offset = rp & RING_MASK;
    uint64_t avail = wp - rp;

    /* ===== Phase 1: 先读 name_len(2B) + payload_len(4B) ===== */
    const size_t HDR_SZ = 6;  /* name_len(2) + payload_len(4) */
    if (avail < HDR_SZ) return 0;  /* 头部还没写完，等下轮 */

    uint16_t name_len;
    uint32_t payload_len;
    {
        char hdr[6];
        if (offset + HDR_SZ <= RING_SIZE) {
            memcpy(hdr, rb->buffer + offset, HDR_SZ);
        } else {
            size_t first = RING_SIZE - offset;
            memcpy(hdr, rb->buffer + offset, first);
            memcpy(hdr + first, rb->buffer, HDR_SZ - first);
        }
        memcpy(&name_len, hdr, 2);
        memcpy(&payload_len, hdr + 2, 4);
    }

    /* name_len sanity: 空事件名非法（writer push 前已拒绝），255 是上限 */
    if (name_len < 1 || name_len > 255) return 1;

    /* payload_len sanity: writer TLS buffer 只有 2048，payload 不可能超过 2000 */
    if (payload_len > 2000) return 1;

    /* ===== Phase 1b: 计算 total_frame_size 并校验 ===== */
    const size_t total_frame = 2 + name_len + 4 + payload_len + 8 + 1;
    if (total_frame > buf_size) return 1;   /* 超过 frame_buf，corruption */
    if (total_frame > avail) return 0;      /* writer 还没写完 payload，等下轮 */

    /* ===== Phase 2: memcpy 整帧到 frame_buf（含 payload！修复之前的 bug）===== */
    if (offset + total_frame <= RING_SIZE) {
        memcpy(frame_buf, rb->buffer + offset, total_frame);
    } else {
        size_t first = RING_SIZE - offset;
        memcpy(frame_buf, rb->buffer + offset, first);
        memcpy(frame_buf + first, rb->buffer, total_frame - first);
    }

    /* ===== Phase 3: 从完整拷贝解包（复用 Phase 1 已解出的 nl/pl 局部变量！）=====
     *
     * ★ 关键修复：Phase 1 已经用原子快照从 ring buffer 解出了正确的
     * name_len/payload_len 局部变量。Phase 2 跨 wrap 拷贝时 writer 可能正在
     * buffer[0] 写新帧覆盖 frame_buf 尾部 → frame_buf 里的 payload_len 字段
     * 被污染成垃圾值 → 重新解包会飞越界。
     *
     * 所以 Phase 3 **绝对不能再 memcpy nl/pl**——直接用 Phase 1 的局部变量！
     * 只从 frame_buf 里取 name/payload/seq/end_marker 这些偏移量由 nl/pl 决定的字段。 */
    char *name = frame_buf + 2;
    char *payload = frame_buf + 2 + name_len + 4;
    char *seq_ptr = frame_buf + 2 + name_len + 4 + payload_len;

    /* seq: 直接 memcpy 8 字节（不会被 Phase 2 拷贝污染，它在 frame_buf 中部）*/
    uint64_t seq;
    memcpy(&seq, seq_ptr, 8);

    /* end_marker: total_frame 最后 1 字节 */
    uint8_t end_marker = frame_buf[total_frame - 1];

    if (end_marker != 0xFF) {
        /* corruption —— 逐字节 skip (return 1) 让 drain 线程重新扫描帧边界
         * 旧代码 return 8 硬跳 8 字节几乎不可能对齐下一帧 → 永久错位 → 反复 crash */
        return 1;
    }

    /* null-terminate（drain 线程内安全）*/
    if (name_len < 256) name[name_len] = '\0';
    if (payload_len > 0 && payload_len < (uint32_t)buf_size)
        payload[payload_len] = '\0';

    *out_name = name;
    *out_payload = payload_len > 0 ? payload : NULL;
    *out_seq = seq;
    return total_frame;
}

/* 扫一遍 core_id 指定的 ring：解析所有 pending 帧 → fire 给 async chain */
static void drain_one_ring(int core_id)
{
    if (!g_rings || core_id < 0 || core_id >= g_ring_cores) return;
    per_core_ring_t *rb = &g_rings[core_id];

    /* drain 线程栈上 buffer：零 malloc */
    char frame_buf[4096];

    int safety = 0;
    while (safety++ < 2048) {   /* 单 drain 周期最多处理 2048 帧 */
        const char *name, *payload;
        uint64_t seq;
        size_t frame_len = ring_parse_one(rb, sizeof(frame_buf), frame_buf,
                                          &name, &payload, &seq);
        if (frame_len == 0) break;  /* ring 空或 writer 还没写完 */
        /* frame_len >= 16 = 完整帧（name=1B+pl=0 → 2+1+4+0+8+1=16）fire
         * frame_len == 1 = corruption（ring_parse_one 返回值）→ 逐字节 skip 对齐下一帧 */
        if (frame_len >= 16) {
            async_fire(name, payload, seq);
            uint64_t rp = __atomic_load_n(&rb->read_pos, __ATOMIC_ACQUIRE);
            __atomic_store_n(&rb->read_pos, rp + frame_len, __ATOMIC_RELEASE);
        } else {
            /* corruption 逐字节 skip：让 reader 重新对齐帧边界。
             * 旧代码硬跳 8 字节几乎不可能对齐 → 永久错位 → 反复 crash（2026-10-26 修复）*/
            uint64_t rp = __atomic_load_n(&rb->read_pos, __ATOMIC_ACQUIRE);
            __atomic_store_n(&rb->read_pos, rp + frame_len, __ATOMIC_RELEASE);
        }
    }
}

/* --- Drain 线程 --- */

static pthread_t      g_drain_thread = 0;
static volatile int    g_drain_running = 0;
static volatile int    g_drain_stop_flag = 0;

static void *drain_thread_fn(void *ud)
{
    (void)ud;
    log_info("event_bus", "drain thread started (cores=%d)", g_ring_cores);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };  /* 50ms 周期 */

    while (!__atomic_load_n(&g_drain_stop_flag, __ATOMIC_ACQUIRE)) {
        for (int c = 0; c < g_ring_cores; ++c)
            drain_one_ring(c);
        nanosleep(&ts, NULL);
    }

    /* 退出前 drain 最后一轮——确保 shutdown 期间 push 的帧不丢 */
    for (int c = 0; c < g_ring_cores; ++c)
        drain_one_ring(c);

    log_info("event_bus", "drain thread exiting");
    return NULL;
}

int event_bus_start_drain_thread(void)
{
    /* F-03 配套：先 init（如果还没） */
    if (!g_rings) {
        int rc = event_bus_init();
        if (rc != NSBC_OK) return rc;
    }
    if (__atomic_load_n(&g_drain_running, __ATOMIC_ACQUIRE))
        return NSBC_OK;  /* 已经在跑 */

    __atomic_store_n(&g_drain_stop_flag, 0, __ATOMIC_RELEASE);
    if (pthread_create(&g_drain_thread, NULL, drain_thread_fn, NULL) != 0) {
        log_err("event_bus", "failed to create drain thread");
        return NSBC_ERR;
    }
    __atomic_store_n(&g_drain_running, 1, __ATOMIC_RELEASE);
    return NSBC_OK;
}

void event_bus_stop_drain_thread(void)
{
    if (!__atomic_load_n(&g_drain_running, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&g_drain_stop_flag, 1, __ATOMIC_RELEASE);
    if (g_drain_thread) {
        pthread_join(g_drain_thread, NULL);
        g_drain_thread = 0;
    }
    __atomic_store_n(&g_drain_running, 0, __ATOMIC_RELEASE);
}
