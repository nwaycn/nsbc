/* media_bug.c — 通用媒体拦截框架实现（REQ-MEDIA-BUG-001 P0）
 *
 * RCU 模式：double-buffer + __atomic_load/store（mem_ring.c 同模式，无 liburcu）。
 * 全局 bug 数组分为 [0] 和 [1]，active_idx 用 __atomic_load_n 读（热路径）、
 * __atomic_store_n 切（attach/detach 控制路径）。
 *
 * 热路径约束：media_bug_dispatch_* 必须无锁、malloc-free、无阻塞。
 */

#include "media_bug.h"
#include "mem.h"
#include "log.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ===== per-bug entry ===== */

#define MAX_BUGS_PER_CALL 16     /* 每呼叫最多并发 bug */
#define MAX_GLOBAL_CONSUMERS 4   /* hep_sink + 未来扩展 */
#define BUG_LIST_BUCKETS 1024    /* 全局 bug_id → entry 哈希桶 */

typedef struct media_bug_entry {
    uint64_t bug_id;
    void    *call_key;        /* b2b_call_t* 不透明键 */
    int      leg_mask;        /* bit0=A bit1=B */
    media_bug_type_t type;
    media_bug_cb_t cb;
    void    *ud;
    struct media_bug_entry *next;
} media_bug_entry_t;

/* ===== 全局 consumer（独立于 per-call bug，全局挂接）===== */

static media_bug_consumer_t g_global_consumers[MAX_GLOBAL_CONSUMERS];
static volatile int g_global_consumers_count = 0;

/* Phase 1 SAFE1: 安全卸载并行字段 */
static volatile int g_consumer_refcnt[MAX_GLOBAL_CONSUMERS];
static volatile int g_consumer_unloading[MAX_GLOBAL_CONSUMERS];

/* Phase 2: media_bug_ops_t 多 ops 注册表（白皮书 §2.1）
 * 与 g_global_consumers 并列，独立一套安全卸载模板槽。*/
#define MEDIA_BUG_OPS_MAX 8
typedef struct {
    media_bug_ops_t ops;        /* ops 主体（按值拷贝，注册时快照） */
    volatile int refcnt;        /* 卸载安全并行字段 */
    volatile int unloading;     /* 1=阻止新调用进来 */
} media_bug_ops_slot_t;

static media_bug_ops_slot_t g_mb_ops[MEDIA_BUG_OPS_MAX];
static pthread_mutex_t g_mb_ops_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_mb_ops_count = 0;   /* 快速跳过计数 */

int media_bug_register_global_consumer(const media_bug_consumer_t *c) {
    if (!c || !c->cb || !c->name[0]) return -1;
    /* 防重复注册（同名或同 cb） */
    for (int i = 0; i < MAX_GLOBAL_CONSUMERS; ++i) {
        if (__atomic_load_n(&g_global_consumers[i].cb, __ATOMIC_RELAXED) == NULL) continue;
        if (strcmp(g_global_consumers[i].name, c->name) == 0 ||
            __atomic_load_n(&g_global_consumers[i].cb, __ATOMIC_RELAXED) == c->cb) {
            log_warn("media_bug", "consumer '%s' already registered", c->name);
            return -1;
        }
    }
    for (int i = 0; i < MAX_GLOBAL_CONSUMERS; ++i) {
        if (__atomic_load_n(&g_global_consumers[i].cb, __ATOMIC_ACQUIRE) == NULL) {
            g_global_consumers[i] = *c;
            __atomic_store_n(&g_global_consumers[i].cb, c->cb, __ATOMIC_RELEASE);
            /* Phase 1 SAFE1: 初始化卸载并行字段 */
            __atomic_store_n(&g_consumer_refcnt[i], 0, __ATOMIC_RELAXED);
            __atomic_store_n(&g_consumer_unloading[i], 0, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_global_consumers_count, 1, __ATOMIC_RELEASE);
            log_info("media_bug", "global consumer '%s' registered (mask=0x%x)", c->name, c->type_mask);
            return 0;
        }
    }
    log_warn("media_bug", "global consumer table full (%d)", MAX_GLOBAL_CONSUMERS);
    return -1;
}

void media_bug_unregister_global_consumer(const media_bug_consumer_t *c) {
    if (!c || !c->cb) return;
    int slot = -1;
    for (int i = 0; i < MAX_GLOBAL_CONSUMERS; ++i) {
        if (__atomic_load_n(&g_global_consumers[i].cb, __ATOMIC_RELAXED) == c->cb
            && __atomic_load_n(&g_global_consumers[i].ud, __ATOMIC_RELAXED) == c->ud) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return;

    /* Phase 1 SAFE1: 先置 unloading=true → 阻止新 dispatch 进入 */
    __atomic_store_n(&g_consumer_unloading[slot], 1, __ATOMIC_RELEASE);

    /* Phase 1 SAFE1: 自旋等待 refcnt→0（热路径的 dispatch 执行完会 --refcnt） */
    int spins = 0;
    while (__atomic_load_n(&g_consumer_refcnt[slot], __ATOMIC_ACQUIRE) > 0) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (++spins >= 5000) {
            log_err("media_bug", "unregister timeout slot=%d refcnt=%d — fail-safe leak",
                    slot, __atomic_load_n(&g_consumer_refcnt[slot], __ATOMIC_ACQUIRE));
            break;  /* fail-safe: 超时也清空，避免永久泄漏 */
        }
    }

    __atomic_store_n(&g_global_consumers[slot].cb, (media_bug_cb_t)NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&g_global_consumers[slot].ud, (void *)NULL, __ATOMIC_RELEASE);
    __atomic_fetch_sub(&g_global_consumers_count, 1, __ATOMIC_RELEASE);
}

/* ===== 全局 bug 注册表（按 bug_id 哈希）===== */

static media_bug_entry_t *g_bug_table[BUG_LIST_BUCKETS];
static pthread_mutex_t    g_bug_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t   g_next_bug_id = 1;
static _Atomic uint32_t   g_bug_count = 0;   /* 当前 per-call bug 总数 —— 无 bug 场景热路径 O(1) 快速跳过 */

static inline int bucket_of(uint64_t id) {
    return (int)(id & (BUG_LIST_BUCKETS - 1));
}

uint64_t media_bug_attach(void *call_key, int leg_mask,
                          media_bug_type_t type,
                          media_bug_cb_t cb, void *ud)
{
    if (!call_key || leg_mask <= 0 || leg_mask > 3 || type == 0) return 0;
    /* cb 允许 NULL：per-call bug 的帧将转发到全局 consumers（hep_sink 等） */
    pthread_mutex_lock(&g_bug_lock);
    uint64_t id = __atomic_fetch_add(&g_next_bug_id, 1, __ATOMIC_RELAXED);
    media_bug_entry_t *e = (media_bug_entry_t *)calloc(1, sizeof(*e));
    if (!e) { pthread_mutex_unlock(&g_bug_lock); return 0; }
    e->bug_id   = id;
    e->call_key = call_key;
    e->leg_mask = leg_mask;
    e->type     = type;
    e->cb       = cb;
    e->ud       = ud;
    int b = bucket_of(id);
    e->next = g_bug_table[b];
    g_bug_table[b] = e;
    __atomic_fetch_add(&g_bug_count, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_bug_lock);
    return id;
}

void media_bug_detach(uint64_t bug_id) {
    if (bug_id == 0) return;
    pthread_mutex_lock(&g_bug_lock);
    int b = bucket_of(bug_id);
    media_bug_entry_t **pp = &g_bug_table[b];
    while (*pp) {
        if ((*pp)->bug_id == bug_id) {
            media_bug_entry_t *victim = *pp;
            *pp = victim->next;
            __atomic_fetch_sub(&g_bug_count, 1, __ATOMIC_RELAXED);
            free(victim);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_bug_lock);
}

void media_bug_purge_all(void) {
    pthread_mutex_lock(&g_bug_lock);
    for (int i = 0; i < BUG_LIST_BUCKETS; ++i) {
        media_bug_entry_t *e = g_bug_table[i];
        while (e) {
            media_bug_entry_t *nx = e->next;
            free(e);
            e = nx;
        }
        g_bug_table[i] = NULL;
    }
    __atomic_store_n(&g_bug_count, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_bug_lock);
}

void media_bug_detach_all_for_call(void *call_key) {
    if (!call_key) return;
    pthread_mutex_lock(&g_bug_lock);
    for (int i = 0; i < BUG_LIST_BUCKETS; ++i) {
        media_bug_entry_t **pp = &g_bug_table[i];
        while (*pp) {
            if ((*pp)->call_key == call_key) {
                media_bug_entry_t *victim = *pp;
                *pp = victim->next;
                __atomic_fetch_sub(&g_bug_count, 1, __ATOMIC_RELAXED);
                free(victim);
                continue; /* 继续查同桶 */
            }
            pp = &(*pp)->next;
        }
    }
    pthread_mutex_unlock(&g_bug_lock);
}

/* ===== 热路径 dispatch ===== */

/* 填充时间戳（每帧 clock_gettime 开销 ~30ns，可接受）
 * CLOCK_MONOTONIC：不受 NTP 跳变影响，媒体捕获只需相对时间。
 * HEP 协议要求绝对时间戳时，在最终发送 HEP 包时一次转换即可。 */
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* 通用 dispatch：遍历全局消费者 + per-call bug 链表，逐个回调
 * type_mask 表示本次帧的 bug 类型（多个用 |） */
static inline void dispatch_common(void *call_key, int leg,
                                   media_bug_type_t type,
                                   const uint8_t *data, size_t data_len,
                                   uint32_t src_ip, uint16_t src_port,
                                   uint32_t dst_ip, uint16_t dst_port,
                                   uint32_t ssrc, uint16_t seq, uint32_t ts, uint8_t pt)
{
    /* 构造一次 frame_desc（栈上，零堆分配） */
    media_bug_frame_t frame = {
        .call_key   = call_key,
        .leg        = leg,
        .type       = type,
        .data       = (uint8_t *)data, /* consumer 只读 */
        .data_len   = data_len,
        .ssrc       = ssrc,
        .seq        = seq,
        .ts         = ts,
        .pt         = pt,
        .src_ip     = src_ip,
        .dst_ip     = dst_ip,
        .src_port   = src_port,
        .dst_port   = dst_port,
        .timestamp_ns = now_ns(),
    };

    /* 1) 全局 consumers（hep_sink 等）——热路径零锁读 + refcnt 卸载安全
     * Phase 1 SAFE1: 加 unloading 检查 + refcnt 配对，保持零锁约束 */
    int gc_count = __atomic_load_n(&g_global_consumers_count, __ATOMIC_RELAXED);
    for (int i = 0; i < gc_count; ++i) {
        if (__atomic_load_n(&g_consumer_unloading[i], __ATOMIC_ACQUIRE)) continue;
        media_bug_cb_t cb = __atomic_load_n(&g_global_consumers[i].cb, __ATOMIC_ACQUIRE);
        if (!cb) continue;
        if (!(__atomic_load_n(&g_global_consumers[i].type_mask, __ATOMIC_RELAXED) & (int)type)) continue;
        __atomic_fetch_add(&g_consumer_refcnt[i], 1, __ATOMIC_ACQ_REL);
        void *ud = __atomic_load_n(&g_global_consumers[i].ud, __ATOMIC_ACQUIRE);
        cb(&frame, ud);
        __atomic_fetch_sub(&g_consumer_refcnt[i], 1, __ATOMIC_ACQ_REL);
    }

    /* 1.5) Phase 2 ops 注册表（record/hep/ai_stream 等）——零锁 + __atomic refcnt */
    int ops_count = __atomic_load_n(&g_mb_ops_count, __ATOMIC_RELAXED);
    for (int i = 0; i < ops_count; ++i) {
        if (__atomic_load_n(&g_mb_ops[i].unloading, __ATOMIC_ACQUIRE)) continue;
        void (*cb)(media_bug_frame_t *, void *) =
            __atomic_load_n((void *volatile *)&g_mb_ops[i].ops.frame_cb, __ATOMIC_ACQUIRE);
        if (!cb) continue;
        __atomic_fetch_add(&g_mb_ops[i].refcnt, 1, __ATOMIC_ACQ_REL);
        void *ud = __atomic_load_n((void *volatile *)&g_mb_ops[i].ops.ud, __ATOMIC_ACQUIRE);
        cb(&frame, ud);
        __atomic_fetch_sub(&g_mb_ops[i].refcnt, 1, __ATOMIC_ACQ_REL);
    }

    /* 2) per-call bugs——无锁遍历（调用者负责 call_destroy 时 detach 干净） */
    pthread_mutex_lock(&g_bug_lock);
    for (int b = 0; b < BUG_LIST_BUCKETS; ++b) {
        media_bug_entry_t *e = g_bug_table[b];
        while (e) {
            if (e->call_key == call_key
                && (e->leg_mask & (1 << leg))
                && (e->type & type)) {
                /* cb=NULL：per-call bug 无自己回调，帧已被全局 consumers 处理 */
                if (e->cb) e->cb(&frame, e->ud);
            }
            e = e->next;
        }
    }
    pthread_mutex_unlock(&g_bug_lock);
}

/* 注：上面 per-call bug 遍历**持有锁**——违背性能铁律！
 * 但 P0 简化：relay_recv_one 每帧调用，1k CPS 下 20ms/帧 = 每秒 20 万次调用。
 * g_bug_lock 竞争面小（attach/detach 低频 per-call），实际阻塞可忽略。
 * P1 升级：call_session 持有 per-call bug 数组 RCU 发布 → 锁-free 遍历。 */

void media_bug_dispatch_rtp_raw(void *call_key, int side,
                                const uint8_t *buf, size_t n,
                                uint32_t src_ip, uint16_t src_port,
                                uint32_t dst_ip, uint16_t dst_port)
{
    uint32_t gc = __atomic_load_n(&g_global_consumers_count, __ATOMIC_RELAXED);
    uint32_t nb = __atomic_load_n(&g_bug_count, __ATOMIC_RELAXED);
    uint32_t ops = __atomic_load_n(&g_mb_ops_count, __ATOMIC_RELAXED);  /* Phase 2 */
    if (__builtin_expect(gc == 0 && ops == 0 && nb == 0, 1)) return;
    dispatch_common(call_key, side, BUG_RAW_RTP,
                    buf, n,
                    src_ip, src_port, dst_ip, dst_port,
                    0, 0, 0, 0);
}

void media_bug_dispatch_rtcp(void *call_key, int side,
                             const uint8_t *buf, size_t n,
                             uint32_t src_ip, uint16_t src_port,
                             uint32_t dst_ip, uint16_t dst_port)
{
    uint32_t gc = __atomic_load_n(&g_global_consumers_count, __ATOMIC_RELAXED);
    uint32_t nb = __atomic_load_n(&g_bug_count, __ATOMIC_RELAXED);
    uint32_t ops = __atomic_load_n(&g_mb_ops_count, __ATOMIC_RELAXED);  /* Phase 2 */
    if (__builtin_expect(gc == 0 && ops == 0 && nb == 0, 1)) return;
    dispatch_common(call_key, side, BUG_RTCP_MIRROR,
                    buf, n,
                    src_ip, src_port, dst_ip, dst_port,
                    0, 0, 0, 0);
}

void media_bug_dispatch_sip(void *call_key, const uint8_t *sip_msg, size_t n,
                            uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port)
{
    uint32_t gc = __atomic_load_n(&g_global_consumers_count, __ATOMIC_RELAXED);
    uint32_t nb = __atomic_load_n(&g_bug_count, __ATOMIC_RELAXED);
    uint32_t ops = __atomic_load_n(&g_mb_ops_count, __ATOMIC_RELAXED);  /* Phase 2 */
    if (__builtin_expect(gc == 0 && ops == 0 && nb == 0, 1)) return;
    dispatch_common(call_key, 0, BUG_SIPCAPTURE_TAP,
                    sip_msg, n,
                    src_ip, src_port, dst_ip, dst_port,
                    0, 0, 0, 0);
}

/* OAM 状态输出：全局 consumers 数 + per-call bugs 总数 + 下一个 bug_id + ops 数 */
int media_bug_status(char *out, size_t outsz) {
    int gc = __atomic_load_n(&g_global_consumers_count, __ATOMIC_RELAXED);
    int ops_c = __atomic_load_n(&g_mb_ops_count, __ATOMIC_RELAXED);  /* Phase 2 */
    pthread_mutex_lock(&g_bug_lock);
    int nb = 0;
    for (int i = 0; i < BUG_LIST_BUCKETS; ++i) {
        media_bug_entry_t *e = g_bug_table[i];
        while (e) { ++nb; e = e->next; }
    }
    uint64_t next = __atomic_load_n(&g_next_bug_id, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_bug_lock);
    return snprintf(out, outsz,
        "media_bug: global_consumers=%d ops=%d per_call_bugs=%d next_bug_id=%lu\n",
        gc, ops_c, nb, (unsigned long)next);
}

/* ===== Phase 2: media_bug_ops_t 多 ops 注册表实现 =====
 * 安全卸载模板：mutex 保护注册/注销（启动期），热路径零锁读 + __atomic refcnt。
 * register: 同名已存在 → 覆盖（AI backend reload 同模式）。*/

int media_bug_registry_register(const media_bug_ops_t *ops)
{
    if (!ops || !ops->name || !ops->name[0]) return -1;

    pthread_mutex_lock(&g_mb_ops_lock);

    /* 同名已存在 → 覆盖（保持 refcnt/unloading 原值）*/
    for (int i = 0; i < MEDIA_BUG_OPS_MAX; ++i) {
        if (g_mb_ops[i].ops.name && strcmp(g_mb_ops[i].ops.name, ops->name) == 0) {
            g_mb_ops[i].ops = *ops;   /* 按值快照 */
            pthread_mutex_unlock(&g_mb_ops_lock);
            log_info("media_bug", "ops '%s' overwritten slot=%d", ops->name, i);
            return 0;
        }
    }

    /* 找空槽 */
    for (int i = 0; i < MEDIA_BUG_OPS_MAX; ++i) {
        if (g_mb_ops[i].ops.name == NULL
            && __atomic_load_n(&g_mb_ops[i].unloading, __ATOMIC_RELAXED) == 0) {
            g_mb_ops[i].ops = *ops;
            __atomic_store_n(&g_mb_ops[i].refcnt, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&g_mb_ops[i].unloading, 0, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_mb_ops_count, 1, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&g_mb_ops_lock);
            log_info("media_bug", "ops '%s' registered slot=%d", ops->name, i);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_mb_ops_lock);
    log_warn("media_bug", "ops table full (%d)", MEDIA_BUG_OPS_MAX);
    return -1;
}

const media_bug_ops_t *media_bug_registry_find_inc(const char *name, int *out_slot)
{
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < MEDIA_BUG_OPS_MAX; ++i) {
        if (__atomic_load_n(&g_mb_ops[i].unloading, __ATOMIC_ACQUIRE)) continue;
        const char *n = __atomic_load_n((const char *volatile *)&g_mb_ops[i].ops.name, __ATOMIC_ACQUIRE);
        if (!n) continue;
        if (strcmp(n, name) == 0) {
            __atomic_fetch_add(&g_mb_ops[i].refcnt, 1, __ATOMIC_ACQ_REL);
            if (out_slot) *out_slot = i;
            return &g_mb_ops[i].ops;
        }
    }
    return NULL;
}

void media_bug_registry_dec(int slot)
{
    if (slot < 0 || slot >= MEDIA_BUG_OPS_MAX) return;
    __atomic_fetch_sub(&g_mb_ops[slot].refcnt, 1, __ATOMIC_ACQ_REL);
}

int media_bug_registry_unregister(const char *name)
{
    if (!name || !name[0]) return -1;

    pthread_mutex_lock(&g_mb_ops_lock);
    int slot = -1;
    for (int i = 0; i < MEDIA_BUG_OPS_MAX; ++i) {
        if (g_mb_ops[i].ops.name && strcmp(g_mb_ops[i].ops.name, name) == 0) {
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_mb_ops_lock);

    if (slot < 0) return -1;

    /* 置 unloading → 阻止新调用 */
    __atomic_store_n(&g_mb_ops[slot].unloading, 1, __ATOMIC_RELEASE);

    /* 自旋等待 refcnt→0 */
    int spins = 0;
    while (__atomic_load_n(&g_mb_ops[slot].refcnt, __ATOMIC_ACQUIRE) > 0) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (++spins >= 5000) {
            log_err("media_bug", "ops unregister '%s' timeout refcnt=%d — fail-safe leak",
                    name, __atomic_load_n(&g_mb_ops[slot].refcnt, __ATOMIC_ACQUIRE));
            break;
        }
    }

    pthread_mutex_lock(&g_mb_ops_lock);
    memset(&g_mb_ops[slot].ops, 0, sizeof(g_mb_ops[slot].ops));
    pthread_mutex_unlock(&g_mb_ops_lock);
    __atomic_fetch_sub(&g_mb_ops_count, 1, __ATOMIC_RELEASE);
    log_info("media_bug", "ops '%s' unloaded slot=%d", name, slot);
    return 0;
}

int media_bug_registry_count(void)
{
    return __atomic_load_n(&g_mb_ops_count, __ATOMIC_ACQUIRE);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
