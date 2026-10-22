/* =============================================================================
 * Phase 3: Cachedb backend registry + 内置 mem backend 骨架
 *
 * Phase 3 骨架完整度：~50%
 *   ✓ 全局注册表（静态数组 + nsbc_atomic_t 序列号做简单读写同步）
 *   ✓ cachedb_backend_register / cachedb_backend_find
 *   ✓ cachedb_mem_register 内置 mem backend（简单哈希表 + ttl stub）
 *   ✗ mem backend ttl 到期清理（Phase 4 接 timer_wheel）
 *   ✗ mem backend get/set 的真正实现（Phase 4 补）
 *   ✗ redis/lmdb/sqlite backend 动态模块（Phase 4）
 *   ✗ cachedb.c 接线（Phase 4）
 *
 * GCC __atomic_* 风格：
 *   - g_seq 使用 nsbc_atomic_t + sat_seq_begin/sat_seq_end 做顺序锁
 *   - register（写）: seq_begin → 操作数组 → seq_end
 *   - find（读）    : seq_begin → 读数组 → seq_end（验证序列号没被改写）
 *
 * Phase 4 补全清单（同 cachedb_backend.h 注释）
 * ============================================================================= */

/* =============================================================================
 * Phase 3+4: Cachedb backend registry + 内置 mem backend 真哈希表
 *
 * Phase 4 升级：mem backend 替换 stub 为生产级开放寻址哈希表
 *   - FNV-1a 64-bit 哈希 + 线性探测
 *   - Tombstone 删除（removed 标记保持探测链不中断）
 *   - 负载因子 0.75 触发 2x 扩容 + rehash
 *   - Lazy TTL 过期清理（get 时发现过期 → 删；每次 write 后随机采样清理）
 *   - pthread_rwlock_t 读写锁保护（get=rdlock，set/del=wrlock）
 * ============================================================================= */

#include "cachedb_backend.h"
#include "log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* 全局注册表                                                          */

static const cachedb_backend_ops_t *g_backends[CACHEDB_BACKEND_MAX];
static nsbc_atomic_t g_seq;          /* 顺序锁序列号：奇数=写进行中 */
static pthread_rwlock_t g_reg_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/* ------------------------------------------------------------------ */

int cachedb_backend_register(const cachedb_backend_ops_t *ops)
{
    if (!ops || !ops->name) return NSBC_INVAL;
    if (!ops->open || !ops->close || !ops->get || !ops->set)
        return NSBC_INVAL;

    pthread_rwlock_wrlock(&g_reg_rwlock);

    /* 先扫：同名则覆盖（后注册的生效），否则找空位 */
    int slot = -1;
    for (int i = 0; i < CACHEDB_BACKEND_MAX; i++) {
        if (g_backends[i] && strcmp(g_backends[i]->name, ops->name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < CACHEDB_BACKEND_MAX; i++) {
            if (!g_backends[i]) { slot = i; break; }
        }
    }
    if (slot < 0) {
        pthread_rwlock_unlock(&g_reg_rwlock);
        return NSBC_NOSPC;
    }

    g_backends[slot] = ops;

    pthread_rwlock_unlock(&g_reg_rwlock);
    return NSBC_OK;
}

const cachedb_backend_ops_t *cachedb_backend_find(const char *name)
{
    if (!name) return NULL;

    pthread_rwlock_rdlock(&g_reg_rwlock);

    const cachedb_backend_ops_t *found = NULL;
    for (int i = 0; i < CACHEDB_BACKEND_MAX; i++) {
        if (g_backends[i] && strcmp(g_backends[i]->name, name) == 0) {
            found = g_backends[i];
            break;
        }
    }

    pthread_rwlock_unlock(&g_reg_rwlock);
    return found;
}

/* ================================================================== */
/* Phase 4: 内置 mem backend — 开放寻址哈希表 + TTL
 *
 * 每个 handle 是独立的 mem_t 实例（支持多 backend 并行）。
 * 默认缓存键值对在 mem 堆上，TTL 到期自动 lazy 清理。
 * ================================================================== */

/* entry 状态 */
#define MEM_EMPTY    0   /* 从未占用 */
#define MEM_ACTIVE   1   /* 活跃条目 */
#define MEM_REMOVED  2   /* Tombstone（线性探测链不能断） */

typedef struct {
    char     *key;
    uint8_t  *val;
    size_t    val_len;
    int64_t   expires_at_ms;   /* 0 = 永不过期 */
    uint8_t   state;           /* MEM_EMPTY / MEM_ACTIVE / MEM_REMOVED */
} mem_entry_t;

typedef struct mem_t {
    mem_entry_t *buckets;
    size_t       capacity;     /* 2 的幂 */
    size_t       mask;         /* capacity - 1，快速取模 */
    size_t       count;        /* active 条目数 */
    size_t       tombstones;   /* tombstone 数 */
    pthread_rwlock_t lock;
} mem_t;

#define MEM_INITIAL_CAPACITY 1024
#define MEM_LOAD_FACTOR_NUM  3   /* 0.75 = 3/4 */
#define MEM_LOAD_FACTOR_DEN  4

/* ------ 时间获取 ------ */
static int64_t mem_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------ FNV-1a 64-bit 哈希 ------ */
static uint64_t mem_hash(const char *key)
{
    uint64_t h = 14695981039346656037ULL;  /* FNV offset basis */
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;  /* FNV prime */
    }
    return h;
}

/* ------ 释放单个 entry ------ */
static void mem_entry_free(mem_entry_t *e)
{
    if (!e) return;
    free(e->key);
    free(e->val);
    e->key = NULL; e->val = NULL; e->val_len = 0;
    e->expires_at_ms = 0; e->state = MEM_EMPTY;
}

/* ------ Resize + rehash（需持 wrlock）------ */
static int mem_resize_rehash(mem_t *m, size_t new_cap)
{
    mem_entry_t *old = m->buckets;
    size_t old_cap = m->capacity;

    mem_entry_t *nb = (mem_entry_t *)calloc(new_cap, sizeof(mem_entry_t));
    if (!nb) return NSBC_NOMEM;

    size_t nmask = new_cap - 1;

    /* 搬迁所有 active 条目 */
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].state != MEM_ACTIVE) continue;

        uint64_t h = mem_hash(old[i].key);
        size_t idx = h & nmask;
        /* 新表找首个空位/removed */
        while (nb[idx].state == MEM_ACTIVE) {
            idx = (idx + 1) & nmask;
        }
        /* 转移：复用 old[i] 的 key/val（不 free 不 malloc）*/
        nb[idx].key = old[i].key;
        nb[idx].val = old[i].val;
        nb[idx].val_len = old[i].val_len;
        nb[idx].expires_at_ms = old[i].expires_at_ms;
        nb[idx].state = MEM_ACTIVE;
        old[i].key = NULL; old[i].val = NULL;
        old[i].state = MEM_EMPTY;
    }

    free(old);
    m->buckets = nb;
    m->capacity = new_cap;
    m->mask = nmask;
    m->tombstones = 0;

    return NSBC_OK;
}

/* ------ 查找 entry 位置（线性探测）------
 * 找到 → 返回索引；没找到 → 返回第一个 removed/empty 位置（tombstone 优先）*/
static size_t mem_find_or_hole(mem_t *m, const char *key, int *found_out)
{
    uint64_t h = mem_hash(key);
    size_t idx = h & m->mask;
    size_t first_tombstone = (size_t)-1;
    int found = 0;

    for (size_t probes = 0; probes <= m->mask; probes++) {
        mem_entry_t *e = &m->buckets[idx];
        if (e->state == MEM_EMPTY) break;  /* 探测链终了 */
        if (e->state == MEM_REMOVED) {
            if (first_tombstone == (size_t)-1) first_tombstone = idx;
        } else {  /* ACTIVE */
            if (strcmp(e->key, key) == 0) { found = 1; break; }
        }
        idx = (idx + 1) & m->mask;
    }

    if (found) { *found_out = 1; return idx; }
    *found_out = 0;
    return (first_tombstone != (size_t)-1) ? first_tombstone : idx;
}

/* ------ 检查 entry 过期（配合 lazy cleanup）------ */
static int mem_is_expired(const mem_entry_t *e)
{
    if (!e || e->state != MEM_ACTIVE || e->expires_at_ms == 0) return 0;
    return mem_now_ms() >= e->expires_at_ms;
}

/* ------ 随机采样清理过期条目（每次 write 后调用）------ */
static void mem_sample_cleanup(mem_t *m)
{
    size_t samples = (m->count > 32) ? 32 : m->count;
    if (samples == 0) return;

    int64_t now = mem_now_ms();
    size_t cleared = 0;
    size_t start = (size_t)(now & m->mask);  /* 伪随机起始 */

    for (size_t i = 0; i < m->capacity && cleared < samples; i++) {
        size_t idx = (start + i) & m->mask;
        mem_entry_t *e = &m->buckets[idx];
        if (e->state == MEM_ACTIVE && e->expires_at_ms > 0 && now >= e->expires_at_ms) {
            mem_entry_free(e);
            e->state = MEM_REMOVED;
            m->count--;
            m->tombstones++;
            cleared++;
        }
    }
}

/* ==================================================================
 * mem backend ops 实现
 * ================================================================== */

static int mem_open(const char *dsn, void **handle)
{
    (void)dsn;  /* mem backend 无 DSN */
    if (!handle) return NSBC_INVAL;

    mem_t *m = (mem_t *)calloc(1, sizeof(*m));
    if (!m) return NSBC_NOMEM;

    m->capacity = MEM_INITIAL_CAPACITY;
    m->mask = m->capacity - 1;
    m->buckets = (mem_entry_t *)calloc(m->capacity, sizeof(mem_entry_t));
    if (!m->buckets) { free(m); return NSBC_NOMEM; }

    pthread_rwlock_init(&m->lock, NULL);
    *handle = m;
    return NSBC_OK;
}

static void mem_close(void *handle)
{
    mem_t *m = (mem_t *)handle;
    if (!m) return;

    for (size_t i = 0; i < m->capacity; i++)
        mem_entry_free(&m->buckets[i]);

    free(m->buckets);
    pthread_rwlock_destroy(&m->lock);
    free(m);
}

static int mem_get(void *handle, const char *key, char **val, size_t *len)
{
    mem_t *m = (mem_t *)handle;
    if (!m || !key || !val || !len) return NSBC_INVAL;

    pthread_rwlock_rdlock(&m->lock);

    int found;
    size_t idx = mem_find_or_hole(m, key, &found);

    if (!found) {
        pthread_rwlock_unlock(&m->lock);
        return NSBC_NOENT;
    }

    mem_entry_t *e = &m->buckets[idx];

    /* Lazy cleanup：过期则返回 NOENT，由 set/del 路径在 wrlock 下真正删除 */
    if (mem_is_expired(e)) {
        pthread_rwlock_unlock(&m->lock);
        return NSBC_NOENT;
    }

    /* 拷贝 value 给调用者（调用者负责 free）*/
    uint8_t *copy = (uint8_t *)malloc(e->val_len);
    if (!copy) { pthread_rwlock_unlock(&m->lock); return NSBC_NOMEM; }
    memcpy(copy, e->val, e->val_len);

    pthread_rwlock_unlock(&m->lock);

    *val = (char *)copy;
    *len = e->val_len;
    return NSBC_OK;
}

static int mem_set(void *handle, const char *key, const char *val, size_t len, int ttl_ms)
{
    mem_t *m = (mem_t *)handle;
    if (!m || !key || !val) return NSBC_INVAL;

    pthread_rwlock_wrlock(&m->lock);

    /* 检查负载因子：(count + tombstones) / capacity > 0.75 → resize */
    size_t total_entries = m->count + m->tombstones;
    if (total_entries * MEM_LOAD_FACTOR_DEN > m->capacity * MEM_LOAD_FACTOR_NUM) {
        int rc = mem_resize_rehash(m, m->capacity * 2);
        if (rc != NSBC_OK) {
            pthread_rwlock_unlock(&m->lock);
            return rc;
        }
    }

    int found;
    size_t idx = mem_find_or_hole(m, key, &found);

    int64_t expires = (ttl_ms <= 0) ? 0 : mem_now_ms() + ttl_ms;

    if (found) {
        /* 更新已有 entry */
        mem_entry_t *e = &m->buckets[idx];
        /* 过期的也按更新处理（相当于重新 set）*/
        free(e->val);
        e->val = (uint8_t *)malloc(len);
        if (!e->val) { pthread_rwlock_unlock(&m->lock); return NSBC_NOMEM; }
        memcpy(e->val, val, len);
        e->val_len = len;
        e->expires_at_ms = expires;
        /* state 保持 ACTIVE */
    } else {
        /* 新插入 */
        mem_entry_t *e = &m->buckets[idx];
        /* 如果是 tombstone 位置，回收 tombstones 计数 */
        if (e->state == MEM_REMOVED) m->tombstones--;

        e->key = strdup(key);
        if (!e->key) { pthread_rwlock_unlock(&m->lock); return NSBC_NOMEM; }
        e->val = (uint8_t *)malloc(len);
        if (!e->val) { free(e->key); e->key = NULL; pthread_rwlock_unlock(&m->lock); return NSBC_NOMEM; }
        memcpy(e->val, val, len);
        e->val_len = len;
        e->expires_at_ms = expires;
        e->state = MEM_ACTIVE;
        m->count++;
    }

    /* 每次 set 后随机清理过期条目 */
    mem_sample_cleanup(m);

    pthread_rwlock_unlock(&m->lock);
    return NSBC_OK;
}

static int mem_del(void *handle, const char *key)
{
    mem_t *m = (mem_t *)handle;
    if (!m || !key) return NSBC_INVAL;

    pthread_rwlock_wrlock(&m->lock);

    int found;
    size_t idx = mem_find_or_hole(m, key, &found);

    if (!found) {
        pthread_rwlock_unlock(&m->lock);
        return NSBC_NOENT;
    }

    mem_entry_t *e = &m->buckets[idx];

    /* 如果过期（lazy 发现），也算删除成功 */
    mem_entry_free(e);
    e->state = MEM_REMOVED;   /* Tombstone！保持线性探测链不中断 */
    m->count--;
    m->tombstones++;

    /* 顺便清理一些过期的 */
    mem_sample_cleanup(m);

    pthread_rwlock_unlock(&m->lock);
    return NSBC_OK;
}

static int mem_ping(void *handle)
{
    (void)handle;
    return NSBC_OK;   /* mem backend 永远活着 */
}

static const cachedb_backend_ops_t g_mem_ops = {
    .name  = "mem",
    .open  = mem_open,
    .close = mem_close,
    .get   = mem_get,
    .set   = mem_set,
    .del   = mem_del,
    .ping  = mem_ping,
};

int cachedb_mem_register(void)
{
    return cachedb_backend_register(&g_mem_ops);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
