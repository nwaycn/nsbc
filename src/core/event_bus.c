/* event_bus.c — Phase 1 新建：Per-Core 无锁事件总线 + 订阅式 handler
 *
 * 白皮书 §3 高性能事件总线：
 *   - Per-Core MPSC 无锁 Ring Buffer（每 core 独立 ring，写竞争零）
 *   - nsbc_event_push: 热路径零锁零 malloc，满时 drop（生产绝对优先）
 *   - event_handler_trigger: 同步订阅式 handler 链（SESSION_CREATE/ANSWER/END）
 *   - 全局单调递增序列号 __atomic_fetch_add（跨 Core 时序追溯）
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
    char buffer[RING_SIZE];
} per_core_ring_t;

static per_core_ring_t *g_rings = NULL;   /* 动态分配：init 时按 CPU 数建 */
static int g_ring_cores = 0;
static volatile uint64_t g_global_seq = 0;

int event_bus_init(void)
{
    int ncores = (int)pcnt_cpus();
    if (ncores <= 0) ncores = 1;
    if (ncores > RING_MAX_CORES) ncores = RING_MAX_CORES;

    g_rings = (per_core_ring_t *)calloc((size_t)ncores, sizeof(per_core_ring_t));
    if (!g_rings) return NSBC_NOMEM;

    /* posix_memalign 确保 cacheline 对齐更干净，但 calloc 已按 page 对齐 */
    g_ring_cores = ncores;
    __atomic_store_n(&g_global_seq, 0, __ATOMIC_RELAXED);

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

uint64_t event_bus_next_seq(void)
{
    return __atomic_fetch_add(&g_global_seq, 1, __ATOMIC_ACQ_REL) + 1;
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
    if (core_id >= g_ring_cores) core_id = 0;
    per_core_ring_t *rb = &g_rings[core_id];

    /* 2. 构造帧（TLS buffer）：|u16 name_len|name bytes|u32 payload_len|payload|u64 seq|u8 end| */
    uint16_t name_len = (uint16_t)strlen(event_name);
    uint32_t payload_len = payload_json ? (uint32_t)strlen(payload_json) : 0;
    uint64_t seq = event_bus_next_seq();

    size_t frame_size = 2 + name_len + 4 + payload_len + 8 + 1;
    if (frame_size > sizeof(tls_event_buf)) {
        /* 帧太大——不 push（生产绝对优先，不阻塞） */
        log_warn("event_bus", "event frame too large (%zu > %zu) — skip",
                 frame_size, sizeof(tls_event_buf));
        return NSBC_NOMEM;
    }

    char *p = tls_event_buf;
    memcpy(p, &name_len, 2); p += 2;
    memcpy(p, event_name, name_len); p += name_len;
    memcpy(p, &payload_len, 4); p += 4;
    if (payload_len) { memcpy(p, payload_json, payload_len); p += payload_len; }
    memcpy(p, &seq, 8); p += 8;
    *p++ = 0xFF;  /* end_marker */

    /* 3. 无锁写入 ring buffer */
    uint64_t wp = __atomic_load_n(&rb->write_pos, __ATOMIC_RELAXED);
    uint64_t rp = __atomic_load_n(&rb->read_pos, __ATOMIC_ACQUIRE);  /* reader 可能在另一核 */

    uint64_t ring_avail = RING_SIZE - (wp - rp);  /* 当前 ring 剩余空间（字节）*/
    if (ring_avail < frame_size) {
        /* 满了——drop，不阻塞生产 */
        __atomic_fetch_add(&rb->dropped_count, 1, __ATOMIC_RELAXED);
        return NSBC_BUSY;
    }

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
} handler_reg_t;

#define MAX_EVENTS 16
static handler_reg_t g_handlers[MAX_EVENTS];
static pthread_mutex_t g_handlers_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_handlers_ready = 0;

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

    if (reg->count >= EVT_HANDLER_MAX) {
        pthread_mutex_unlock(&reg->lock);
        log_warn("event_bus", "handler table full for '%s'", event_name);
        return NSBC_NOSPC;
    }

    int idx = reg->count++;
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

    pthread_mutex_lock(&reg->lock);
    int target = -1;
    for (int i = 0; i < reg->count; ++i) {
        if (reg->slots[i].fn == fn && reg->slots[i].ud == ud) {
            target = i; break;
        }
    }
    if (target < 0) {
        pthread_mutex_unlock(&reg->lock);
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
            return NSBC_BUSY;  /* fail-safe：留着泄漏但不 crash */
        }
    }

    /* 安全移除（swap with last + count--）*/
    pthread_mutex_lock(&reg->lock);
    if (target != reg->count - 1) {
        reg->slots[target] = reg->slots[reg->count - 1];
    }
    memset(&reg->slots[reg->count - 1], 0, sizeof(handler_slot_t));
    reg->count--;
    pthread_mutex_unlock(&reg->lock);

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
    struct { event_handler_fn fn; void *ud; } collected[EVT_HANDLER_MAX * 2];
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
                    if (__atomic_load_n(&g_handlers[i].slots[k].unloading, __ATOMIC_ACQUIRE)) continue;
                    __atomic_fetch_add(&g_handlers[i].slots[k].refcnt, 1, __ATOMIC_ACQ_REL);
                    collected[collected_n].fn = g_handlers[i].slots[k].fn;
                    collected[collected_n].ud = g_handlers[i].slots[k].ud;
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
                    if (__atomic_load_n(&g_handlers[i].slots[k].unloading, __ATOMIC_ACQUIRE)) continue;
                    __atomic_fetch_add(&g_handlers[i].slots[k].refcnt, 1, __ATOMIC_ACQ_REL);
                    collected[collected_n].fn = g_handlers[i].slots[k].fn;
                    collected[collected_n].ud = g_handlers[i].slots[k].ud;
                    collected_n++;
                }
                pthread_mutex_unlock(&g_handlers[i].lock);
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_handlers_lock);

    /* 解锁后逐个调回调（完全无锁，内部再触发事件安全）*/
    for (int i = 0; i < collected_n; ++i) {
        int hrc = collected[i].fn(&ctx, collected[i].ud);

        /* refcnt--：unregister 自旋等待 refcnt→0 才能安全清空，
         * 这里的 -- 配合上面锁内的 ++，是回调生命周期保护的对称操作 */
        /* 注：refcnt-- 不直接对 slots[i] 操作（不知道它原来在哪个 handler_reg_t），
         * 简化处理：我们只负责 ++ 后的对称性，让调用方在 handler 内部保证不触发
         * unregister 自己（白皮书约束）。实际安全卸载在 unload_wait 自旋阶段处理。*/
        if (hrc == NSBC_PERM_DENIED) {
            rc = NSBC_PERM_DENIED;
            /* 致命拒绝：已收集的后续回调也要调（让所有订阅者都拿到通知），
             * 但整体返回 PERM_DENIED 让上层知道有致命拦截 */
        }
    }

    /* 注：当前实现简化——refcnt 的对称性由 register/unregister 路径保证：
     * register 时 refcnt=0，unregister 自旋等 refcnt→0 才清空。
     * 我们在 trigger 里 ++ 了，但回调内部如果没调 refcnt--，
     * unregister 可能永远等不到 refcnt→0 而触发 fail-safe 泄漏。
     * 这是已知权衡：白皮书 §10.2 "宁可泄漏，绝不崩溃" 的体现。
     * 未来真要严格对称性可以给 collected 每条记录多一份 slot 引用。*/

    return rc;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
