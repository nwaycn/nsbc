/* media_bug.c — 通用媒体拦截框架实现（REQ-MEDIA-BUG-001 P0）
 *
 * RCU 模式：double-buffer + __atomic_load/store（mem_ring.c 同模式，无 liburcu）。
 * 全局 bug 数组分为 [0] 和 [1]，active_idx 用 __atomic_load_n 读（热路径）、
 * __atomic_store_n 切（attach/detach 控制路径）。
 *
 * 热路径约束：media_bug_dispatch_* 必须无锁、malloc-free、无阻塞。
 */

#include "media_bug.h"
#include "media_codec.h"
#include "mem.h"
#include "log.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ===== per-bug entry ===== */

#define MAX_BUGS_PER_CALL 16     /* 每呼叫最多并发 bug */
#define MAX_GLOBAL_CONSUMERS 8   /* hep_sink / snapshot / async ring / 未来扩展 */
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

/* ===== per-call 无锁快照（P0：热路径不再持 g_bug_lock 扫全表）=====
 *
 * attach/detach 在控制线程用当前 bug 表重建「本桶 + 本呼叫」的不可变快照，
 * 通过 __atomic_store_n 发布；媒体线程 __atomic_load_n 读取后只读遍历。
 * 旧快照进入 retired 链，等 call destroy（detach_all_for_call）时统一释放，
 * 避免媒体线程仍持有旧指针时被 free 造成 UAF。 */
#define BUG_SNAP_BUCKETS 512
#define BUG_SNAP_MAX     16

typedef struct bug_snap_ent {
    media_bug_cb_t cb;
    void          *ud;
    int            leg_mask;
    media_bug_type_t type;
} bug_snap_ent_t;

typedef struct bug_snap {
    struct bug_snap *next;          /* 同桶其他呼叫的快照 */
    void            *call_key;
    int              n;
    bug_snap_ent_t   ent[BUG_SNAP_MAX];
} bug_snap_t;

static bug_snap_t *volatile g_snap[BUG_SNAP_BUCKETS];
static bug_snap_t *g_snap_retired;   /* 受 g_bug_lock 保护 */

/* ===== PCM tap 懒解码（P1）=====
 * 只有挂了 BUG_PCM_READ/WRITE 的呼叫才需要解码；用 g_pcm_tap_count 做全局开关，
 * 未挂 PCM bug 的呼叫在热路径上只是读一次原子变量。
 * 解码器按 (call, leg, PT) 直接映射缓存，未命中时在控制级锁内创建一次。 */
#define BUG_PCM_SLOTS 256
typedef struct bug_pcm_dec {
    void       *call_key;
    int         leg;
    uint8_t     pt;
    const codec_ops_t *ops;
    codec_sess_t *dec;
    int         failed;      /* 1=init 失败已缓存，避免每包重复 init */
    struct bug_pcm_dec *next;
} bug_pcm_dec_t;

static bug_pcm_dec_t *volatile g_pcm_dec[BUG_PCM_SLOTS];
/* 每槽在读计数：媒体线程在解码临界区内 +1，槽位替换/销毁时等待归零后再释放，
 * 避免媒体线程仍持有解码器指针时被 free（原实现只 delayed 到进程退出 → 每呼叫泄漏）。 */
static uint32_t g_pcm_dec_rc[BUG_PCM_SLOTS];
static bug_pcm_dec_t *g_pcm_dec_retired;   /* 仅等待超时的兜底链，受 g_bug_lock 保护 */
static uint32_t g_pcm_tap_count;

/* 释放单个解码器（调用方持 g_bug_lock）*/
static void pcm_dec_free_one_locked(bug_pcm_dec_t *e)
{
    if (e->dec != NULL && e->ops != NULL && e->ops->fini != NULL)
        e->ops->fini(e->dec);
    free(e);
}

/* 释放一条 PCM 解码器链（调用方持 g_bug_lock）*/
static void pcm_dec_free_chain_locked(bug_pcm_dec_t *p)
{
    while (p != NULL) {
        bug_pcm_dec_t *nx = p->next;
        pcm_dec_free_one_locked(p);
        p = nx;
    }
}

/* 槽位已替换后回收旧解码器：等该槽在读计数归零再释放。
 * 解码临界区很短（一次 codec decode），正常几次 yield 即归零；
 * 极端情况（消费者长时间占用）超时后挂 retired 兜底，由 purge_all 释放。 */
static void pcm_dec_retire_locked(bug_pcm_dec_t *e, int slot)
{
    int spins = 0;
    while (__atomic_load_n(&g_pcm_dec_rc[slot], __ATOMIC_SEQ_CST) > 0) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 };
        nanosleep(&ts, NULL);
        if (++spins >= 5000) break;
    }
    if (__atomic_load_n(&g_pcm_dec_rc[slot], __ATOMIC_SEQ_CST) == 0) {
        pcm_dec_free_one_locked(e);
        return;
    }
    llog(LOG_ERR, "media_bug",
         "pcm decoder retire timeout slot=%d — deferred to purge", slot);
    e->next = NULL;
    {
        bug_pcm_dec_t **pp = &g_pcm_dec_retired;
        while (*pp != NULL) pp = &(*pp)->next;
        *pp = e;
    }
}

/* 呼叫销毁：把该呼叫的 PCM 解码器摘出直接映射表，等在读线程退出后立即释放 */
static void pcm_dec_release_call_locked(void *call_key)
{
    int i;
    for (i = 0; i < BUG_PCM_SLOTS; ++i) {
        bug_pcm_dec_t *e = __atomic_load_n(&g_pcm_dec[i], __ATOMIC_SEQ_CST);
        if (e == NULL || e->call_key != call_key) continue;
        /* SEQ_CST 与读侧 fetch_add 构成全序：确保读侧要么看到 NULL、
         * 要么已被读计数覆盖（详见 pcm_dec_acquire 注释）。 */
        __atomic_store_n(&g_pcm_dec[i], (bug_pcm_dec_t *)NULL, __ATOMIC_SEQ_CST);
        e->next = NULL;
        pcm_dec_retire_locked(e, i);
    }
}

static inline int snap_bucket_of(void *call_key)
{
    return (int)(((uintptr_t)call_key >> 4) & (BUG_SNAP_BUCKETS - 1));
}

/* ===== 全局 consumer（独立于 per-call bug，全局挂接）===== */

static media_bug_consumer_t g_global_consumers[MAX_GLOBAL_CONSUMERS];
static volatile int g_global_consumers_count = 0;

/* 每消费者/每 ops 的观测计数（P2-9 统一输出；热路径 relaxed 原子加）*/
static volatile uint64_t g_consumer_frames[MAX_GLOBAL_CONSUMERS];
static volatile uint64_t g_consumer_bytes[MAX_GLOBAL_CONSUMERS];
static volatile uint64_t g_consumer_dropped[MAX_GLOBAL_CONSUMERS];
static volatile uint64_t g_consumer_latency_max_us[MAX_GLOBAL_CONSUMERS];

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
static volatile uint64_t g_ops_frames[MEDIA_BUG_OPS_MAX];
static volatile uint64_t g_ops_bytes[MEDIA_BUG_OPS_MAX];

int media_bug_register_global_consumer(const media_bug_consumer_t *c) {
    if (!c || !c->cb || !c->name[0]) return -1;
    /* 防重复注册（同名或同 cb） */
    for (int i = 0; i < MAX_GLOBAL_CONSUMERS; ++i) {
        if (__atomic_load_n(&g_global_consumers[i].cb, __ATOMIC_RELAXED) == NULL) continue;
        if (strcmp(g_global_consumers[i].name, c->name) == 0 ||
            (__atomic_load_n(&g_global_consumers[i].cb, __ATOMIC_RELAXED) == c->cb &&
             __atomic_load_n(&g_global_consumers[i].ud, __ATOMIC_RELAXED) == c->ud)) {
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
            __atomic_store_n(&g_consumer_frames[i], 0, __ATOMIC_RELAXED);
            __atomic_store_n(&g_consumer_bytes[i], 0, __ATOMIC_RELAXED);
            __atomic_store_n(&g_consumer_dropped[i], 0, __ATOMIC_RELAXED);
            __atomic_store_n(&g_consumer_latency_max_us[i], 0, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_global_consumers_count, 1, __ATOMIC_RELEASE);
            /* 全局消费者也可能消费 PCM_READ/WRITE，与 per-call bug 共享
             * 同一个懒解码开关，否则 VAD 等 consumer 收不到 PCM tap。 */
            if (c->type_mask & (BUG_PCM_READ | BUG_PCM_WRITE))
                __atomic_fetch_add(&g_pcm_tap_count, 1, __ATOMIC_RELAXED);
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

    /* 先快照 type_mask：下面清空 cb 后槽位可能被新消费者复用，
     * 再读会误减 g_pcm_tap_count。 */
    int slot_mask = __atomic_load_n(&g_global_consumers[slot].type_mask,
                                    __ATOMIC_RELAXED);

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
    if (slot_mask & (BUG_PCM_READ | BUG_PCM_WRITE))
        __atomic_fetch_sub(&g_pcm_tap_count, 1, __ATOMIC_RELAXED);
    /* 计数随槽位一起清零，避免新消费者读到上一个的统计 */
    __atomic_store_n(&g_consumer_frames[slot], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_consumer_bytes[slot], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_consumer_dropped[slot], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_consumer_latency_max_us[slot], 0, __ATOMIC_RELAXED);
}

void media_bug_consumer_note_drop(media_bug_cb_t cb, void *ud, uint64_t n)
{
    if (cb == NULL || n == 0) return;
    for (int i = 0; i < MAX_GLOBAL_CONSUMERS; ++i) {
        if (__atomic_load_n(&g_global_consumers[i].cb, __ATOMIC_ACQUIRE) == cb &&
            __atomic_load_n(&g_global_consumers[i].ud, __ATOMIC_ACQUIRE) == ud) {
            __atomic_fetch_add(&g_consumer_dropped[i], n, __ATOMIC_RELAXED);
            return;
        }
    }
}

/* ===== 全局 bug 注册表（按 bug_id 哈希）===== */

static media_bug_entry_t *g_bug_table[BUG_LIST_BUCKETS];
static pthread_mutex_t    g_bug_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t   g_next_bug_id = 1;
static _Atomic uint32_t   g_bug_count = 0;   /* 当前 per-call bug 总数 —— 无 bug 场景热路径 O(1) 快速跳过 */

static inline int bucket_of(uint64_t id) {
    return (int)(id & (BUG_LIST_BUCKETS - 1));
}

/* 释放一条快照链 */
static void snap_free_chain(bug_snap_t *p)
{
    while (p != NULL) {
        bug_snap_t *nx = p->next;
        free(p);
        p = nx;
    }
}

/* 用当前 bug 表重建 call_key 的快照，并原子发布新桶链。
 * 调用方必须持有 g_bug_lock（控制路径，非热路径）。 */
static int snap_rebuild_locked(void *call_key)
{
    int bkt = snap_bucket_of(call_key);
    bug_snap_t *old = g_snap[bkt];
    bug_snap_t *head = NULL;
    bug_snap_t **tail = &head;
    bug_snap_t *node;
    int i;

    /* 克隆同桶其他呼叫的节点（链不可变，读侧无锁）*/
    for (bug_snap_t *p = old; p != NULL; p = p->next) {
        bug_snap_t *c;
        if (p->call_key == call_key) continue;
        c = (bug_snap_t *)malloc(sizeof(*c));
        if (c == NULL) { snap_free_chain(head); return NSBC_NOMEM; }
        *c = *p;
        c->next = NULL;
        *tail = c;
        tail = &c->next;
    }

    node = (bug_snap_t *)calloc(1, sizeof(*node));
    if (node == NULL) { snap_free_chain(head); return NSBC_NOMEM; }
    node->call_key = call_key;
    for (i = 0; i < BUG_LIST_BUCKETS && node->n < BUG_SNAP_MAX; ++i) {
        media_bug_entry_t *e;
        for (e = g_bug_table[i]; e != NULL; e = e->next) {
            if (e->call_key != call_key) continue;
            node->ent[node->n].cb = e->cb;
            node->ent[node->n].ud = e->ud;
            node->ent[node->n].leg_mask = e->leg_mask;
            node->ent[node->n].type = e->type;
            node->n++;
            if (node->n >= BUG_SNAP_MAX) break;
        }
    }
    if (node->n > 0)
        *tail = node;
    else
        free(node);

    __atomic_store_n(&g_snap[bkt], head, __ATOMIC_RELEASE);

    /* 旧链整体退役：媒体线程可能仍持有旧指针，延迟释放防 UAF */
    while (old != NULL) {
        bug_snap_t *nx = old->next;
        old->next = g_snap_retired;
        g_snap_retired = old;
        old = nx;
    }
    return NSBC_OK;
}

/* 释放 retired 链中属于该呼叫的快照（call destroy 时已无媒体线程在用）*/
static void snap_free_retired_for_locked(void *call_key)
{
    bug_snap_t **pp = &g_snap_retired;
    while (*pp != NULL) {
        if ((*pp)->call_key == call_key) {
            bug_snap_t *v = *pp;
            *pp = v->next;
            free(v);
        } else {
            pp = &(*pp)->next;
        }
    }
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
    if (e->type & (BUG_PCM_READ | BUG_PCM_WRITE))
        __atomic_fetch_add(&g_pcm_tap_count, 1, __ATOMIC_RELAXED);
    if (snap_rebuild_locked(call_key) != NSBC_OK)
        llog(LOG_ERR, "media_bug", "snapshot rebuild failed (call=%p) — "
             "hot path may miss bugs until next attach", call_key);
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
            void *call_key = victim->call_key;
            *pp = victim->next;
            __atomic_fetch_sub(&g_bug_count, 1, __ATOMIC_RELAXED);
            if (victim->type & (BUG_PCM_READ | BUG_PCM_WRITE))
                __atomic_fetch_sub(&g_pcm_tap_count, 1, __ATOMIC_RELAXED);
            free(victim);
            if (snap_rebuild_locked(call_key) != NSBC_OK)
                llog(LOG_ERR, "media_bug",
                     "snapshot rebuild failed on detach (call=%p)", call_key);
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
    for (int i = 0; i < BUG_SNAP_BUCKETS; ++i) {
        snap_free_chain(g_snap[i]);
        __atomic_store_n(&g_snap[i], (bug_snap_t *)NULL, __ATOMIC_RELEASE);
    }
    snap_free_chain(g_snap_retired);
    g_snap_retired = NULL;
    for (int i = 0; i < BUG_PCM_SLOTS; ++i) {
        bug_pcm_dec_t *p = g_pcm_dec[i];
        __atomic_store_n(&g_pcm_dec[i], (bug_pcm_dec_t *)NULL, __ATOMIC_RELEASE);
        pcm_dec_free_chain_locked(p);
        __atomic_store_n(&g_pcm_dec_rc[i], 0, __ATOMIC_RELAXED);
    }
    pcm_dec_free_chain_locked(g_pcm_dec_retired);
    g_pcm_dec_retired = NULL;
    __atomic_store_n(&g_pcm_tap_count, 0, __ATOMIC_RELAXED);
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
                if (victim->type & (BUG_PCM_READ | BUG_PCM_WRITE))
                    __atomic_fetch_sub(&g_pcm_tap_count, 1, __ATOMIC_RELAXED);
                free(victim);
                continue; /* 继续查同桶 */
            }
            pp = &(*pp)->next;
        }
    }
    /* 清空该呼叫的快照并回收延迟释放链（呼叫已销毁，无媒体线程再用）*/
    if (snap_rebuild_locked(call_key) != NSBC_OK)
        llog(LOG_ERR, "media_bug", "snapshot rebuild failed on destroy (call=%p)",
             call_key);
    snap_free_retired_for_locked(call_key);
    pcm_dec_release_call_locked(call_key);
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

/* 已构造好的 frame 派发：全局 consumers + ops 注册表 + per-call 快照 */
static inline void dispatch_frame(media_bug_frame_t *frame);

/* ---- PCM tap：从原始 RTP 解出 PCM 后再派发 BUG_PCM_READ ---- */
static inline int pcm_slot_of(void *call_key, int leg)
{
    return (int)((((uintptr_t)call_key >> 4) ^ (uintptr_t)(leg * 0x9E37u)) &
                 (BUG_PCM_SLOTS - 1));
}

/* 仅解析 RTP 固定头 + CSRC + 扩展头；返回 0 成功 */
static int bug_rtp_peek(const uint8_t *d, size_t n, uint8_t *pt,
                        const uint8_t **payload, size_t *plen)
{
    size_t hl;
    if (d == NULL || n < 12 || (d[0] >> 6) != 2) return -1;
    hl = 12 + 4u * (size_t)(d[0] & 0x0F);
    if (d[0] & 0x10) {
        size_t ext;
        if (hl + 4 > n) return -1;
        ext = 4 + 4u * (((size_t)d[hl + 2] << 8) | d[hl + 3]);
        if (hl + ext > n) return -1;
        hl += ext;
    }
    if (hl > n) return -1;
    *pt = (uint8_t)(d[1] & 0x7F);
    *payload = d + hl;
    *plen = n - hl;
    return 0;
}

/* 在控制级锁内懒创建解码器（每 call/leg/PT 一次）。
 * 返回 0 表示槽位已有可用节点；失败返回 -1。 */
static int pcm_dec_create(void *call_key, int leg, uint8_t pt,
                          const codec_ops_t *ops)
{
    int slot = pcm_slot_of(call_key, leg);
    bug_pcm_dec_t *e;
    bug_pcm_dec_t *ne;

    pthread_mutex_lock(&g_bug_lock);
    e = __atomic_load_n(&g_pcm_dec[slot], __ATOMIC_SEQ_CST);
    if (e != NULL && e->call_key == call_key && e->leg == leg && e->pt == pt) {
        pthread_mutex_unlock(&g_bug_lock);
        return 0;   /* 并发创建：已有，回到读保护环重取 */
    }
    ne = (bug_pcm_dec_t *)calloc(1, sizeof(*ne));
    if (ne == NULL) {
        pthread_mutex_unlock(&g_bug_lock);
        return -1;
    }
    ne->call_key = call_key;
    ne->leg = leg;
    ne->pt = pt;
    ne->ops = ops;
    /* init 失败也要登记（failed=1）：否则该 codec 每个 RTP 包都会
     * calloc + 拿全局锁重试（如未编译 bcg729 时的 G.729 桩）。 */
    if (ops->init == NULL || ops->init(NULL, &ne->dec) != NSBC_OK) {
        ne->dec = NULL;
        ne->failed = 1;
        llog(LOG_WARN, "media_bug",
             "pcm decoder init failed (codec=%s), tap disabled for this flow",
             ops->name);
    }
    /* 先摘旧节点再发布新节点：等待在读线程退出期间新读者走锁路径，
     * 不会把新节点的读者算进旧节点的等待。 */
    __atomic_store_n(&g_pcm_dec[slot], (bug_pcm_dec_t *)NULL, __ATOMIC_SEQ_CST);
    if (e != NULL) {
        e->next = NULL;
        pcm_dec_retire_locked(e, slot);
    }
    __atomic_store_n(&g_pcm_dec[slot], ne, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&g_bug_lock);
    return 0;
}

/* 取解码器并进入该槽读保护。成功返回时持有读计数，调用方必须
 * pcm_dec_release(slot)；返回 NULL 表示无可用解码器。 */
static bug_pcm_dec_t *pcm_dec_acquire(void *call_key, int leg, uint8_t pt,
                                      const codec_ops_t *ops, int *slot_out)
{
    int slot = pcm_slot_of(call_key, leg);
    int attempt;

    for (attempt = 0; attempt < 2; ++attempt) {
        bug_pcm_dec_t *e;
        /* fetch_add 之后的 load 若命中旧节点，其 retire 会等待本次计数归零。
         * fetch_add / load 均为 SEQ_CST，与 retire 侧「置 NULL + 读计数」
         * 构成全序，保证要么读到 NULL、要么计数已被覆盖（不会 UAF）。 */
        __atomic_add_fetch(&g_pcm_dec_rc[slot], 1, __ATOMIC_SEQ_CST);
        e = __atomic_load_n(&g_pcm_dec[slot], __ATOMIC_SEQ_CST);
        if (e != NULL && e->call_key == call_key && e->leg == leg && e->pt == pt) {
            *slot_out = slot;
            return e;
        }
        __atomic_sub_fetch(&g_pcm_dec_rc[slot], 1, __ATOMIC_SEQ_CST);
        if (attempt > 0) break;      /* 创建后仍不匹配（并发销毁），本包放弃 */
        if (pcm_dec_create(call_key, leg, pt, ops) != 0) break;
    }
    return NULL;
}

static inline void pcm_dec_release(int slot)
{
    __atomic_sub_fetch(&g_pcm_dec_rc[slot], 1, __ATOMIC_SEQ_CST);
}

static void pcm_tap_try(void *call_key, int leg, media_bug_frame_t *raw)
{
    uint8_t pt;
    const uint8_t *payload;
    size_t plen;
    const codec_ops_t *ops;
    bug_pcm_dec_t *e;
    media_frame_t mf;
    media_bug_frame_t out;
    int slot = -1;
    int rc;
    /* 4096 样本：G.722（16kHz）单包 2048 字节即 4096 样本，是静态 PT 音频
     * codec 的最大膨胀比，据此可保证 decode 输出不越界。 */
    int16_t pcm[4096];

    if (__atomic_load_n(&g_pcm_tap_count, __ATOMIC_RELAXED) == 0) return;
    if (bug_rtp_peek(raw->data, raw->data_len, &pt, &payload, &plen) != 0)
        return;
    ops = codec_find_by_pt_kind(pt, CODEC_KIND_AUDIO);
    if (ops == NULL || ops->decode == NULL || ops->init == NULL) return;
    if (ops->caps & CODEC_CAP_EVENT) return;
    if (plen > sizeof(pcm) / sizeof(pcm[0]) / 2) return;  /* 见 pcm[] 容量说明 */

    e = pcm_dec_acquire(call_key, leg, pt, ops, &slot);
    if (e == NULL) return;
    if (e->dec == NULL) {          /* init 失败已缓存 */
        pcm_dec_release(slot);
        return;
    }

    memset(&mf, 0, sizeof(mf));
    mf.pcm = pcm;
    mf.rate = codec_pcm_sample_rate(ops);
    mf.channels = 1;
    rc = ops->decode(e->dec, payload, plen, &mf);
    /* decode 已完成，解码器会话不再被使用，尽早退出读保护（缩短 retire 等待）*/
    pcm_dec_release(slot);
    if (rc != NSBC_OK || mf.samples == 0) return;
    if (mf.samples > sizeof(pcm) / sizeof(pcm[0])) return;

    memset(&out, 0, sizeof(out));
    out.call_key = call_key;
    out.leg = leg;
    out.type = BUG_PCM_READ;
    out.pcm = pcm;
    out.samples = mf.samples;
    out.rate = mf.rate > 0 ? mf.rate : 8000;
    out.channels = 1;
    out.codec_id = (int)ops->id;
    out.data = (uint8_t *)(uintptr_t)pcm;
    out.data_len = mf.samples * sizeof(int16_t);
    out.timestamp_ns = raw->timestamp_ns;
    dispatch_frame(&out);
}

/* 通用 dispatch：构造 frame 后交给 dispatch_frame */
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

    /* PCM tap 生产：仅当存在 PCM_READ 消费者时，从原始 RTP 懒解码一次 */
    if (type == BUG_RAW_RTP)
        pcm_tap_try(call_key, leg, &frame);

    dispatch_frame(&frame);
}

/* 帧载荷字节数（统一计数用；PCM 按 S16 交织）*/
static inline uint64_t frame_payload_bytes(const media_bug_frame_t *f)
{
    if (f->data && f->data_len) return (uint64_t)f->data_len;
    if (f->pcm && f->samples) {
        int ch = f->channels > 0 ? f->channels : 1;
        return (uint64_t)f->samples * (uint64_t)ch * 2u;
    }
    return 0;
}

static inline void stat_latency(volatile uint64_t *max_us,
                                uint64_t now_ns_v, uint64_t ts_ns)
{
    if (ts_ns == 0 || now_ns_v <= ts_ns) return;
    uint64_t us = (now_ns_v - ts_ns) / 1000u;
    uint64_t old = __atomic_load_n(max_us, __ATOMIC_RELAXED);
    if (us > old) __atomic_store_n(max_us, us, __ATOMIC_RELAXED);
}

static inline void dispatch_frame(media_bug_frame_t *frame)
{
    media_bug_type_t type = frame->type;
    int leg = frame->leg;
    void *call_key = frame->call_key;
    uint64_t fbytes = frame_payload_bytes(frame);
    uint64_t t_ns = 0;

    /* 1) 全局 consumers（hep_sink / snapshot / async ring 等）
     * 热路径零锁读 + refcnt 卸载安全。
     * 注意：槽位可能因注销产生空洞，必须遍历全部槽位而不是 count 前 N 个。 */
    int gc_count = __atomic_load_n(&g_global_consumers_count, __ATOMIC_RELAXED);
    if (gc_count > 0) {
        if (frame->timestamp_ns) t_ns = now_ns();
        for (int i = 0; i < MAX_GLOBAL_CONSUMERS; ++i) {
            media_bug_cb_t cb0 = __atomic_load_n(&g_global_consumers[i].cb,
                                                 __ATOMIC_RELAXED);
            if (cb0 == NULL) continue;
            /* 先加引用再复查 unloading：先检查后加引用存在窗口，
             * 卸载方看到 refcnt=0 清空 cb/ud 后回调会收到 NULL ud */
            __atomic_fetch_add(&g_consumer_refcnt[i], 1, __ATOMIC_ACQ_REL);
            if (__atomic_load_n(&g_consumer_unloading[i], __ATOMIC_ACQUIRE)) {
                __atomic_fetch_sub(&g_consumer_refcnt[i], 1, __ATOMIC_ACQ_REL);
                continue;
            }
            media_bug_cb_t cb = __atomic_load_n(&g_global_consumers[i].cb,
                                                __ATOMIC_ACQUIRE);
            if (!cb || !(__atomic_load_n(&g_global_consumers[i].type_mask,
                                          __ATOMIC_RELAXED) & (int)type)) {
                __atomic_fetch_sub(&g_consumer_refcnt[i], 1, __ATOMIC_ACQ_REL);
                continue;
            }
            void *ud = __atomic_load_n(&g_global_consumers[i].ud, __ATOMIC_ACQUIRE);
            cb(frame, ud);
            __atomic_fetch_add(&g_consumer_frames[i], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_consumer_bytes[i], fbytes, __ATOMIC_RELAXED);
            if (t_ns) stat_latency(&g_consumer_latency_max_us[i], t_ns,
                                   frame->timestamp_ns);
            __atomic_fetch_sub(&g_consumer_refcnt[i], 1, __ATOMIC_ACQ_REL);
        }
    }

    /* 1.5) Phase 2 ops 注册表（record/hep/ai_stream 等）——零锁 + __atomic refcnt */
    int ops_count = __atomic_load_n(&g_mb_ops_count, __ATOMIC_RELAXED);
    if (ops_count > 0) {
        for (int i = 0; i < MEDIA_BUG_OPS_MAX; ++i) {
            if (g_mb_ops[i].ops.name == NULL) continue;
            /* 同 consumers：先加引用再复查，避免与 unregister 的 memset/清零竞争 */
            __atomic_fetch_add(&g_mb_ops[i].refcnt, 1, __ATOMIC_ACQ_REL);
            if (__atomic_load_n(&g_mb_ops[i].unloading, __ATOMIC_ACQUIRE)) {
                __atomic_fetch_sub(&g_mb_ops[i].refcnt, 1, __ATOMIC_ACQ_REL);
                continue;
            }
            void (*cb)(media_bug_frame_t *, void *) =
                __atomic_load_n((void *volatile *)&g_mb_ops[i].ops.frame_cb, __ATOMIC_ACQUIRE);
            if (!cb) {
                __atomic_fetch_sub(&g_mb_ops[i].refcnt, 1, __ATOMIC_ACQ_REL);
                continue;
            }
            void *ud = __atomic_load_n((void *volatile *)&g_mb_ops[i].ops.ud, __ATOMIC_ACQUIRE);
            cb(frame, ud);
            __atomic_fetch_add(&g_ops_frames[i], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_ops_bytes[i], fbytes, __ATOMIC_RELAXED);
            __atomic_fetch_sub(&g_mb_ops[i].refcnt, 1, __ATOMIC_ACQ_REL);
        }
    }

    /* 2) per-call bugs——无锁快照遍历（attach/detach 由控制线程重建并发布）*/
    {
        int bkt = snap_bucket_of(call_key);
        bug_snap_t *snap = __atomic_load_n(&g_snap[bkt], __ATOMIC_ACQUIRE);
        for (; snap != NULL; snap = snap->next) {
            if (snap->call_key != call_key) continue;
            for (int i = 0; i < snap->n; ++i) {
                if ((snap->ent[i].leg_mask & (1 << leg)) &&
                    (snap->ent[i].type & type) && snap->ent[i].cb)
                    snap->ent[i].cb(frame, snap->ent[i].ud);
            }
            break;
        }
    }
}

/* per-call 快照由控制线程在 attach/detach 时重建并原子发布，
 * 媒体线程只做 __atomic_load + 只读遍历，全程无锁。 */

void media_bug_dispatch_pcm(void *call_key, int side,
                            const int16_t *pcm, size_t samples, int rate,
                            int channels, int codec_id, int is_write)
{
    media_bug_frame_t frame;
    uint32_t nb = __atomic_load_n(&g_bug_count, __ATOMIC_RELAXED);
    uint32_t gc = __atomic_load_n(&g_global_consumers_count, __ATOMIC_RELAXED);
    uint32_t ops = __atomic_load_n(&g_mb_ops_count, __ATOMIC_RELAXED);
    if (pcm == NULL || samples == 0 || channels <= 0) return;
    if (__builtin_expect(gc == 0 && ops == 0 && nb == 0, 1)) return;

    memset(&frame, 0, sizeof(frame));
    frame.call_key = call_key;
    frame.leg = side;
    frame.type = is_write ? BUG_PCM_WRITE : BUG_PCM_READ;
    frame.pcm = pcm;
    frame.samples = samples;
    frame.rate = rate > 0 ? rate : 8000;
    frame.channels = channels;
    frame.codec_id = codec_id;
    frame.data = (uint8_t *)(uintptr_t)pcm;
    frame.data_len = samples * (size_t)channels * sizeof(int16_t);
    frame.timestamp_ns = now_ns();
    dispatch_frame(&frame);
}

void media_bug_dispatch_video_au(void *call_key, int side,
                                 const uint8_t *au, size_t len,
                                 int codec_id, int keyframe, uint32_t ts,
                                 uint32_t ssrc)
{
    media_bug_frame_t frame;
    uint32_t nb = __atomic_load_n(&g_bug_count, __ATOMIC_RELAXED);
    uint32_t gc = __atomic_load_n(&g_global_consumers_count, __ATOMIC_RELAXED);
    uint32_t ops = __atomic_load_n(&g_mb_ops_count, __ATOMIC_RELAXED);
    if (au == NULL || len == 0) return;
    if (__builtin_expect(gc == 0 && ops == 0 && nb == 0, 1)) return;

    memset(&frame, 0, sizeof(frame));
    frame.call_key = call_key;
    frame.leg = side;
    frame.type = BUG_VIDEO_AU;
    frame.data = (uint8_t *)au;      /* consumer 只读 */
    frame.data_len = len;
    frame.codec_id = codec_id;
    frame.keyframe = keyframe ? 1 : 0;
    frame.ts = ts;
    frame.ssrc = ssrc;
    frame.timestamp_ns = now_ns();
    dispatch_frame(&frame);
}

/* 供媒体路径低成本判断是否需要准备视频 AU 元数据（关键帧解析等） */
int media_bug_video_active(void)
{
    if (__atomic_load_n(&g_bug_count, __ATOMIC_RELAXED) != 0) return 1;
    if (__atomic_load_n(&g_mb_ops_count, __ATOMIC_RELAXED) != 0) return 1;
    /* 只有订阅了 BUG_VIDEO_AU 的全局消费者才需要解析关键帧；
     * 仅挂音频 PCM 的消费者（VAD 常驻）不应让每个视频 AU 多扫一遍 NAL。 */
    for (int i = 0; i < MAX_GLOBAL_CONSUMERS; ++i) {
        media_bug_cb_t cb =
            __atomic_load_n(&g_global_consumers[i].cb, __ATOMIC_ACQUIRE);
        if (cb == NULL) continue;
        if (__atomic_load_n(&g_global_consumers[i].type_mask,
                            __ATOMIC_RELAXED) & BUG_VIDEO_AU)
            return 1;
    }
    return 0;
}

/* 生命周期：通知 ops 注册表（refcnt 保护，与 frame 派发同一套卸载安全）*/
static void notify_ops_answer(void *call_key)
{
    int n = __atomic_load_n(&g_mb_ops_count, __ATOMIC_RELAXED);
    if (n <= 0) return;
    /* 遍历全部槽位：注销可能在中间留下空洞 */
    for (int i = 0; i < MEDIA_BUG_OPS_MAX; ++i) {
        if (g_mb_ops[i].ops.name == NULL) continue;
        __atomic_fetch_add(&g_mb_ops[i].refcnt, 1, __ATOMIC_ACQ_REL);
        if (__atomic_load_n(&g_mb_ops[i].unloading, __ATOMIC_ACQUIRE)) {
            __atomic_fetch_sub(&g_mb_ops[i].refcnt, 1, __ATOMIC_ACQ_REL);
            continue;
        }
        {
            int (*cb)(b2b_call_t *, void *) =
                __atomic_load_n((void *volatile *)&g_mb_ops[i].ops.on_answer,
                                __ATOMIC_ACQUIRE);
            void *ud = __atomic_load_n((void *volatile *)&g_mb_ops[i].ops.ud,
                                       __ATOMIC_ACQUIRE);
            if (cb) cb((b2b_call_t *)call_key, ud);
        }
        __atomic_fetch_sub(&g_mb_ops[i].refcnt, 1, __ATOMIC_ACQ_REL);
    }
}

static void notify_ops_destroy(void *call_key)
{
    int n = __atomic_load_n(&g_mb_ops_count, __ATOMIC_RELAXED);
    if (n <= 0) return;
    for (int i = 0; i < MEDIA_BUG_OPS_MAX; ++i) {
        if (g_mb_ops[i].ops.name == NULL) continue;
        __atomic_fetch_add(&g_mb_ops[i].refcnt, 1, __ATOMIC_ACQ_REL);
        if (__atomic_load_n(&g_mb_ops[i].unloading, __ATOMIC_ACQUIRE)) {
            __atomic_fetch_sub(&g_mb_ops[i].refcnt, 1, __ATOMIC_ACQ_REL);
            continue;
        }
        {
            void (*cb)(b2b_call_t *, void *) =
                __atomic_load_n((void *volatile *)&g_mb_ops[i].ops.on_destroy,
                                __ATOMIC_ACQUIRE);
            void *ud = __atomic_load_n((void *volatile *)&g_mb_ops[i].ops.ud,
                                       __ATOMIC_ACQUIRE);
            if (cb) cb((b2b_call_t *)call_key, ud);
        }
        __atomic_fetch_sub(&g_mb_ops[i].refcnt, 1, __ATOMIC_ACQ_REL);
    }
}

void media_bug_notify_answer(void *call_key)
{
    if (!call_key) return;
    notify_ops_answer(call_key);
}

void media_bug_notify_destroy(void *call_key)
{
    if (!call_key) return;
    notify_ops_destroy(call_key);
    media_bug_detach_all_for_call(call_key);
}

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
    if (out == NULL || outsz == 0) return 0;
    int off = 0;
#define MB_APPEND(...) do { \
        if (off >= 0 && (size_t)off < outsz) { \
            int _n = snprintf(out + off, outsz - (size_t)off, __VA_ARGS__); \
            if (_n > 0) off += _n; \
        } \
    } while (0)

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

    MB_APPEND("media_bug: global_consumers=%d ops=%d per_call_bugs=%d next_bug_id=%lu\n",
              gc, ops_c, nb, (unsigned long)next);

    /* P2-9：全局消费者统一输出（含帧/字节/丢帧/最大延迟）*/
    for (int i = 0; i < MAX_GLOBAL_CONSUMERS; ++i) {
        media_bug_cb_t cb = __atomic_load_n(&g_global_consumers[i].cb,
                                            __ATOMIC_ACQUIRE);
        if (cb == NULL) continue;
        MB_APPEND("  consumer[%d] name=%s mask=0x%x frames=%llu bytes=%llu "
                  "dropped=%llu latency_max_us=%llu\n",
                  i, g_global_consumers[i].name,
                  __atomic_load_n(&g_global_consumers[i].type_mask, __ATOMIC_RELAXED),
                  (unsigned long long)__atomic_load_n(&g_consumer_frames[i], __ATOMIC_RELAXED),
                  (unsigned long long)__atomic_load_n(&g_consumer_bytes[i], __ATOMIC_RELAXED),
                  (unsigned long long)__atomic_load_n(&g_consumer_dropped[i], __ATOMIC_RELAXED),
                  (unsigned long long)__atomic_load_n(&g_consumer_latency_max_us[i], __ATOMIC_RELAXED));
    }

    /* ops 注册表 */
    for (int i = 0; i < MEDIA_BUG_OPS_MAX; ++i) {
        const char *n = g_mb_ops[i].ops.name;
        if (n == NULL) continue;
        MB_APPEND("  ops[%d] name=%s refcnt=%d unloading=%d frames=%llu bytes=%llu\n",
                  i, n,
                  __atomic_load_n(&g_mb_ops[i].refcnt, __ATOMIC_RELAXED),
                  __atomic_load_n(&g_mb_ops[i].unloading, __ATOMIC_RELAXED),
                  (unsigned long long)__atomic_load_n(&g_ops_frames[i], __ATOMIC_RELAXED),
                  (unsigned long long)__atomic_load_n(&g_ops_bytes[i], __ATOMIC_RELAXED));
    }

    /* per-call bug 明细（最多 16 条，避免刷屏）*/
    pthread_mutex_lock(&g_bug_lock);
    int shown = 0;
    for (int i = 0; i < BUG_LIST_BUCKETS && shown < 16; ++i) {
        for (media_bug_entry_t *e = g_bug_table[i]; e && shown < 16; e = e->next) {
            MB_APPEND("  bug[%d] id=%lu call=%p leg_mask=%d type=0x%x\n",
                      shown++, (unsigned long)e->bug_id, e->call_key,
                      e->leg_mask, (int)e->type);
        }
    }
    pthread_mutex_unlock(&g_bug_lock);

#undef MB_APPEND
    return off;
}

/* P2-10：调试期一致性校验。
 * 目的不是功能测试，而是尽早暴露“注册/卸载/快照发布”过程中的结构性错误：
 *   - 消费者槽计数与实际 cb 数不符、槽内缺 name、refcnt 为负、重名；
 *   - ops 槽 name/refcnt 不一致、重名；
 *   - per-call bug 的 id/cb/leg_mask 非法；
 *   - 快照链长度超界或 call_key 为空。 */
int media_bug_selftest(char *out, size_t cap)
{
    int off = 0, errs = 0;
    if (out != NULL && cap > 0) out[0] = '\0';
#define ST_FAIL(...) do { \
        ++errs; \
        if (out != NULL && off >= 0 && (size_t)off < cap) { \
            int _n = snprintf(out + off, cap - (size_t)off, "FAIL: " __VA_ARGS__); \
            if (_n > 0) off += _n; \
        } \
    } while (0)

    /* 1) 全局消费者槽 */
    int live = 0;
    int cnt = __atomic_load_n(&g_global_consumers_count, __ATOMIC_RELAXED);
    for (int i = 0; i < MAX_GLOBAL_CONSUMERS; ++i) {
        media_bug_cb_t cb = __atomic_load_n(&g_global_consumers[i].cb,
                                            __ATOMIC_ACQUIRE);
        int rc = __atomic_load_n(&g_consumer_refcnt[i], __ATOMIC_RELAXED);
        if (cb == NULL) {
            if (rc != 0)
                ST_FAIL("consumer slot %d empty but refcnt=%d\n", i, rc);
            continue;
        }
        ++live;
        if (g_global_consumers[i].name[0] == '\0')
            ST_FAIL("consumer slot %d registered without name\n", i);
        if (rc < 0)
            ST_FAIL("consumer '%s' slot %d refcnt=%d < 0\n",
                    g_global_consumers[i].name, i, rc);
        for (int j = i + 1; j < MAX_GLOBAL_CONSUMERS; ++j) {
            if (__atomic_load_n(&g_global_consumers[j].cb, __ATOMIC_RELAXED) != NULL &&
                strcmp(g_global_consumers[i].name, g_global_consumers[j].name) == 0)
                ST_FAIL("duplicate consumer name '%s' (slot %d/%d)\n",
                        g_global_consumers[i].name, i, j);
        }
    }
    if (cnt != live)
        ST_FAIL("global_consumers_count=%d but live slots=%d\n", cnt, live);

    /* 2) ops 槽 */
    int ops_live = 0;
    int ocnt = __atomic_load_n(&g_mb_ops_count, __ATOMIC_RELAXED);
    for (int i = 0; i < MEDIA_BUG_OPS_MAX; ++i) {
        const char *n = g_mb_ops[i].ops.name;
        int rc = __atomic_load_n(&g_mb_ops[i].refcnt, __ATOMIC_RELAXED);
        if (n == NULL) {
            if (rc != 0)
                ST_FAIL("ops slot %d empty but refcnt=%d\n", i, rc);
            continue;
        }
        ++ops_live;
        if (n[0] == '\0')
            ST_FAIL("ops slot %d registered without name\n", i);
        if (rc < 0)
            ST_FAIL("ops '%s' slot %d refcnt=%d < 0\n", n, i, rc);
        for (int j = i + 1; j < MEDIA_BUG_OPS_MAX; ++j)
            if (g_mb_ops[j].ops.name != NULL && strcmp(n, g_mb_ops[j].ops.name) == 0)
                ST_FAIL("duplicate ops name '%s' (slot %d/%d)\n", n, i, j);
    }
    if (ocnt != ops_live)
        ST_FAIL("mb_ops_count=%d but live slots=%d\n", ocnt, ops_live);

    /* 3) per-call bug 表 + 快照链（控制级锁保护）*/
    pthread_mutex_lock(&g_bug_lock);
    int nb = 0;
    for (int i = 0; i < BUG_LIST_BUCKETS; ++i) {
        for (media_bug_entry_t *e = g_bug_table[i]; e != NULL; e = e->next) {
            ++nb;
            if (e->bug_id == 0) ST_FAIL("bug entry with id 0 in bucket %d\n", i);
            if (e->cb == NULL) ST_FAIL("bug id=%lu has NULL cb\n",
                                       (unsigned long)e->bug_id);
            if (e->leg_mask <= 0 || e->leg_mask > 3)
                ST_FAIL("bug id=%lu bad leg_mask=%d\n",
                        (unsigned long)e->bug_id, e->leg_mask);
            if (e->call_key == NULL)
                ST_FAIL("bug id=%lu has NULL call_key\n",
                        (unsigned long)e->bug_id);
        }
    }
    for (int i = 0; i < BUG_SNAP_BUCKETS; ++i) {
        for (bug_snap_t *s = g_snap[i]; s != NULL; s = s->next) {
            if (s->call_key == NULL) ST_FAIL("snap bucket %d has NULL call_key\n", i);
            if (s->n <= 0 || s->n > BUG_SNAP_MAX)
                ST_FAIL("snap bucket %d n=%d out of range\n", i, s->n);
        }
    }
    pthread_mutex_unlock(&g_bug_lock);

    int bug_count_now = (int)__atomic_load_n(&g_bug_count, __ATOMIC_RELAXED);
    if (bug_count_now != nb)
        ST_FAIL("g_bug_count=%d but table entries=%d\n", bug_count_now, nb);

    if (out != NULL && off >= 0 && (size_t)off < cap) {
        int _n = snprintf(out + off, cap - (size_t)off,
                          "media_bug selftest: %d issue(s), consumers=%d ops=%d bugs=%d\n",
                          errs, live, ops_live, nb);
        if (_n > 0) off += _n;
    }
#undef ST_FAIL
    return errs;
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
            __atomic_store_n(&g_ops_frames[i], 0, __ATOMIC_RELAXED);
            __atomic_store_n(&g_ops_bytes[i], 0, __ATOMIC_RELAXED);
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
            __atomic_store_n(&g_ops_frames[i], 0, __ATOMIC_RELAXED);
            __atomic_store_n(&g_ops_bytes[i], 0, __ATOMIC_RELAXED);
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
