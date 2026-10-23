/* mem_slab.c — 类型化 slab 对象池 + per-thread magazine
 *（REQ-ARCH-006，7.1.5） */
#include "mem.h"

#include <pthread.h>

#define MEM_ALIGN 16u
#define MEM_OBJ_MAGIC 0x4d454f42u /* 'MEOB' */
#define MEM_DEFAULT_BATCH 8
#define MEM_MAG_CACHES 8
#define MEM_MAG_MAX_BATCH 16
#define MEM_MIN_OBJ 16

/* 每个对象带归属头：释放路径按头直接还共享池，不要求原线程 */
typedef struct mem_obj_hdr {
    uint32_t magic;
    uint32_t slot;
    struct mem_slab *slab;
    int built; /* 构造回调是否已执行（每对象只执行一次） */
} mem_obj_hdr_t;

#define MEM_OBJ_HDR_SIZE \
    ((size_t)(((sizeof(mem_obj_hdr_t) + MEM_ALIGN - 1) / MEM_ALIGN) * MEM_ALIGN))

struct mem_slab {
    char name[32];
    size_t obj_size;
    size_t stride;
    int capacity;
    int batch;
    mem_arena_t *arena;
    uint8_t *mem;
    int *stack; /* 共享空闲槽栈 */
    int nfree;
    pthread_mutex_t lock;
    mem_ctor_fn ctor;
    void *ud;
    uint64_t nalloc, nrelease, nfail;
};

typedef struct {
    mem_slab_t *slab;
    void *ptrs[MEM_MAG_MAX_BATCH];
    int n;
} mem_cache_t;

struct mem_mag {
    mem_cache_t c[MEM_MAG_CACHES];
    int nc;
};

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

int mem_recommended_capacity(int cps, double avg_lifetime_s, int burst_factor)
{
    if (cps <= 0 || avg_lifetime_s <= 0.0 || burst_factor < 1)
        return 0;
    return (int)((double)burst_factor * (double)cps * avg_lifetime_s);
}

nsbc_rc_t mem_slab_create(const mem_slab_cfg_t *cfg, mem_slab_t **out)
{
    if (!cfg || !out || !cfg->arena || cfg->capacity <= 0 ||
        cfg->obj_size == 0)
        return NSBC_INVAL;

    mem_slab_t *s = (mem_slab_t *)calloc(1, sizeof(*s));
    if (!s)
        return NSBC_NOMEM;

    snprintf(s->name, sizeof(s->name), "%s", cfg->name ? cfg->name : "?");
    s->obj_size = align_up(cfg->obj_size, MEM_ALIGN);
    s->stride = MEM_OBJ_HDR_SIZE + s->obj_size;
    s->capacity = cfg->capacity;
    s->batch = cfg->batch > 0 ? cfg->batch : MEM_DEFAULT_BATCH;
    if (s->batch > MEM_MAG_MAX_BATCH)
        s->batch = MEM_MAG_MAX_BATCH;
    s->arena = cfg->arena;
    s->ctor = cfg->ctor;
    s->ud = cfg->ud;

    /* 对象槽与空闲栈都从归属域 Arena 预留：物理隔离、容量固定 */
    s->mem = (uint8_t *)mem_arena_alloc(cfg->arena,
                                        (size_t)cfg->capacity * s->stride);
    if (!s->mem)
        goto fail;
    s->stack = (int *)mem_arena_alloc(cfg->arena,
                                     (size_t)cfg->capacity * sizeof(int));
    if (!s->stack)
        goto fail;

    if (pthread_mutex_init(&s->lock, NULL) != 0)
        goto fail;

    for (int i = 0; i < cfg->capacity; i++) {
        mem_obj_hdr_t *h = (mem_obj_hdr_t *)(s->mem + (size_t)i * s->stride);
        h->magic = MEM_OBJ_MAGIC;
        h->slot = 0;
        h->slab = NULL;
        h->built = 0;
    }
    /* 初始全部空闲（槽 0 位于栈顶，最先被取出） */
    for (int i = 0; i < cfg->capacity; i++)
        s->stack[i] = cfg->capacity - 1 - i;
    s->nfree = cfg->capacity;

    *out = s;
    return NSBC_OK;

fail:
    if (s->stack)
        mem_arena_free(cfg->arena, s->stack);
    if (s->mem)
        mem_arena_free(cfg->arena, s->mem);
    free(s);
    return NSBC_NOMEM;
}

void mem_slab_destroy(mem_slab_t *s)
{
    if (!s)
        return;
    pthread_mutex_destroy(&s->lock);
    mem_arena_free(s->arena, s->stack);
    mem_arena_free(s->arena, s->mem);
    free(s);
}

static void *slot_payload(mem_slab_t *s, int slot)
{
    return s->mem + (size_t)slot * s->stride + MEM_OBJ_HDR_SIZE;
}

static mem_obj_hdr_t *obj_hdr(void *obj)
{
    return (mem_obj_hdr_t *)((uint8_t *)obj - MEM_OBJ_HDR_SIZE);
}

static void *pop_one(mem_slab_t *s)
{
    if (s->nfree == 0) {
        /* slab 耗尽 — 打 stat */
        fprintf(stderr, "[slab POP_FAIL] slab=%p name='%s' capacity=%d nfree=%d nalloc=%lu nrelease=%lu nfail=%lu\n",
                (void *)s, s->name, s->capacity, s->nfree,
                (unsigned long)s->nalloc, (unsigned long)s->nrelease,
                (unsigned long)s->nfail);
        return NULL;
    }
    int slot = s->stack[--s->nfree];
    mem_obj_hdr_t *h = (mem_obj_hdr_t *)(s->mem + (size_t)slot * s->stride);
    h->slot = (uint32_t)slot;
    h->slab = s;
    void *obj = slot_payload(s, slot);
    if (s->ctor && !h->built) {
        s->ctor(obj, s->ud);
        h->built = 1;
    }
    return obj;
}

void *mem_slab_alloc_direct(mem_slab_t *s)
{
    if (!s)
        return NULL;
    pthread_mutex_lock(&s->lock);
    void *obj = pop_one(s);
    pthread_mutex_unlock(&s->lock);
    if (obj) {
        s->nalloc++;
    } else {
        s->nfail++;
    }
    return obj;
}

void mem_obj_release(void *obj)
{
    if (!obj)
        return;
    mem_obj_hdr_t *h = obj_hdr(obj);
    if (h->magic != MEM_OBJ_MAGIC || !h->slab)
        return;
    mem_slab_t *s = h->slab;
    pthread_mutex_lock(&s->lock);
    if (s->nfree < s->capacity)
        s->stack[s->nfree++] = (int)h->slot;
    pthread_mutex_unlock(&s->lock);
    s->nrelease++;
}

int mem_slab_capacity(mem_slab_t *s)
{
    return s ? s->capacity : 0;
}

int mem_slab_usage(mem_slab_t *s)
{
    if (!s) return 0;
    pthread_mutex_lock(&s->lock);
    int used = s->capacity - s->nfree;
    pthread_mutex_unlock(&s->lock);
    return used;
}

void mem_slab_get_stat(const mem_slab_t *s, mem_slab_stat_t *out)
{
    if (!s || !out)
        return;
    out->nalloc = s->nalloc;
    out->nrelease = s->nrelease;
    out->nfail = s->nfail;
    out->nfree = s->nfree;
    out->capacity = s->capacity;
}

/* ---- per-thread magazine ---- */

mem_mag_t *mem_mag_create(void)
{
    return (mem_mag_t *)calloc(1, sizeof(mem_mag_t));
}

static mem_cache_t *cache_get(mem_mag_t *m, mem_slab_t *s, int create)
{
    for (int i = 0; i < m->nc; i++)
        if (m->c[i].slab == s)
            return &m->c[i];
    if (!create || m->nc >= MEM_MAG_CACHES)
        return NULL;
    mem_cache_t *c = &m->c[m->nc++];
    c->slab = s;
    c->n = 0;
    return c;
}

void *mem_mag_alloc(mem_mag_t *m, mem_slab_t *s)
{
    if (!m || !s)
        return NULL;

    mem_cache_t *c = cache_get(m, s, 1);

    /* 本地空：一次锁从共享池批量搬运，锁竞争摊薄到 1/batch */
    if (c->n == 0) {
        pthread_mutex_lock(&s->lock);
        while (c->n < s->batch && s->nfree > 0)
            c->ptrs[c->n++] = pop_one(s);
        pthread_mutex_unlock(&s->lock);
        if (c->n == 0) {
            s->nfail++;
            return NULL;
        }
    }

    void *obj = c->ptrs[--c->n]; /* 快路径：无锁无原子 */
    s->nalloc++;
    return obj;
}

void mem_mag_free(mem_mag_t *m, void *obj)
{
    if (!obj)
        return;
    mem_obj_hdr_t *h = obj_hdr(obj);
    if (h->magic != MEM_OBJ_MAGIC || !h->slab)
        return;
    mem_slab_t *s = h->slab;

    mem_cache_t *c = cache_get(m, s, 1);
    if (!c) { /* 缓存槽位不足：直接还共享池 */
        mem_obj_release(obj);
        return;
    }

    /* 本地满：整批归还共享池，再放入 */
    if (c->n == MEM_MAG_MAX_BATCH) {
        pthread_mutex_lock(&s->lock);
        while (c->n > 0) {
            void *p = c->ptrs[--c->n];
            mem_obj_hdr_t *ch = obj_hdr(p);
            if (s->nfree < s->capacity)
                s->stack[s->nfree++] = (int)ch->slot;
            s->nrelease++;
        }
        pthread_mutex_unlock(&s->lock);
    }

    c->ptrs[c->n++] = obj; /* 快路径：无锁无原子 */
}

void mem_mag_destroy(mem_mag_t *m)
{
    if (!m)
        return;
    /* 线程退出：本地缓存整体归还共享池，不泄漏 */
    for (int i = 0; i < m->nc; i++) {
        mem_cache_t *c = &m->c[i];
        mem_slab_t *s = c->slab;
        pthread_mutex_lock(&s->lock);
        while (c->n > 0) {
            void *p = c->ptrs[--c->n];
            mem_obj_hdr_t *ch = obj_hdr(p);
            if (s->nfree < s->capacity)
                s->stack[s->nfree++] = (int)ch->slot;
            s->nrelease++;
        }
        pthread_mutex_unlock(&s->lock);
    }
    free(m);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
