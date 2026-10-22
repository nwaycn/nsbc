/* mem_ring.c — SPSC / MPSC 无锁环形队列（REQ-ARCH-008，7.1.5） */
#include "mem.h"

#include "nsbc_atomic.h"

#define MEM_CACHELINE 64

static uint32_t round_pow2(uint32_t v, uint32_t maxv)
{
    if (v < 2)
        return 2;
    v--;
    for (uint32_t s = 1; s < 32; s <<= 1)
        v |= v >> s;
    v++;
    return v > maxv ? maxv : v;
}

/* ===================== SPSC ===================== */

struct mem_spsc {
    void **slots;
    uint32_t mask;
    mem_arena_t *arena;
    NSBC_CACHELINE_ALIGNED uint32_t head; /* 消费者维护，生产者读 */
    NSBC_CACHELINE_ALIGNED uint32_t tail; /* 生产者维护，消费者读 */
};

mem_spsc_t *mem_spsc_create(uint32_t capacity, mem_arena_t *arena)
{
    mem_spsc_t *q = (mem_spsc_t *)calloc(1, sizeof(*q));
    if (!q)
        return NULL;

    uint32_t cap = round_pow2(capacity, 0x80000000u);
    size_t bytes = (size_t)cap * sizeof(void *);

    if (arena) {
        q->slots = (void **)mem_arena_alloc(arena, bytes);
        q->arena = arena;
    } else {
        void *p = NULL;
        if (posix_memalign(&p, MEM_CACHELINE, bytes) != 0) {
            free(q);
            return NULL;
        }
        q->slots = (void **)p;
    }
    if (!q->slots) {
        free(q);
        return NULL;
    }

    q->mask = cap - 1;
    q->head = 0;
    q->tail = 0;
    return q;
}

void mem_spsc_destroy(mem_spsc_t *q)
{
    if (!q)
        return;
    if (q->arena)
        mem_arena_free(q->arena, q->slots);
    else
        free(q->slots);
    free(q);
}

int mem_spsc_push(mem_spsc_t *q, void *msg)
{
    if (!q)
        return NSBC_INVAL;
    uint32_t t = __atomic_load_n((&q->tail), __ATOMIC_RELAXED);
    uint32_t h = __atomic_load_n((&q->head), __ATOMIC_ACQUIRE);
    if (t - h > q->mask) /* size = t-h == cap 即满 */
        return NSBC_ERR;
    q->slots[t & q->mask] = msg;
    __atomic_store_n((&q->tail), (t + 1), __ATOMIC_RELEASE);
    return NSBC_OK;
}

int mem_spsc_pop(mem_spsc_t *q, void **out)
{
    if (!q || !out)
        return NSBC_INVAL;
    uint32_t h = __atomic_load_n((&q->head), __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n((&q->tail), __ATOMIC_ACQUIRE);
    if (h == t)
        return NSBC_ERR;
    *out = q->slots[h & q->mask];
    __atomic_store_n((&q->head), (h + 1), __ATOMIC_RELEASE);
    return NSBC_OK;
}

uint32_t mem_spsc_size(const mem_spsc_t *q)
{
    if (!q)
        return 0;
    uint32_t h = __atomic_load_n((&q->head), __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n((&q->tail), __ATOMIC_ACQUIRE);
    return t - h;
}

/* ===================== MPSC（Vyukov bounded） ===================== */

typedef struct {
    size_t seq;
    uintptr_t data;
} mpsc_cell_t;

struct mem_mpsc {
    mpsc_cell_t *cells;
    size_t mask;
    mem_arena_t *arena;
    NSBC_CACHELINE_ALIGNED size_t tail; /* 生产者 CAS 抢位 */
    NSBC_CACHELINE_ALIGNED size_t head; /* 消费者唯一 */
};

mem_mpsc_t *mem_mpsc_create(uint32_t capacity, mem_arena_t *arena)
{
    mem_mpsc_t *q = (mem_mpsc_t *)calloc(1, sizeof(*q));
    if (!q)
        return NULL;

    size_t cap = round_pow2(capacity, 0x80000000u);
    size_t bytes = cap * sizeof(mpsc_cell_t);

    if (arena) {
        q->cells = (mpsc_cell_t *)mem_arena_alloc(arena, bytes);
        q->arena = arena;
    } else {
        void *p = NULL;
        if (posix_memalign(&p, MEM_CACHELINE, bytes) != 0) {
            free(q);
            return NULL;
        }
        q->cells = (mpsc_cell_t *)p;
    }
    if (!q->cells) {
        free(q);
        return NULL;
    }

    q->mask = cap - 1;
    for (size_t i = 0; i < cap; i++)
        q->cells[i].seq = (i);
    q->tail = (0);
    q->head = (0);
    return q;
}

void mem_mpsc_destroy(mem_mpsc_t *q)
{
    if (!q)
        return;
    if (q->arena)
        mem_arena_free(q->arena, q->cells);
    else
        free(q->cells);
    free(q);
}

int mem_mpsc_push(mem_mpsc_t *q, void *msg)
{
    if (!q)
        return NSBC_INVAL;

    size_t pos = __atomic_load_n((&q->tail), __ATOMIC_RELAXED);
    for (;;) {
        mpsc_cell_t *cell = &q->cells[pos & q->mask];
        size_t seq = __atomic_load_n((&cell->seq), __ATOMIC_ACQUIRE);
        long dif = (long)seq - (long)pos;
        if (dif == 0) {
            /* 抢到该槽的写入权 */
            if (__atomic_compare_exchange_n((&q->tail), (&pos), (pos + 1), 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                break;
            /* CAS 失败：pos 被更新为最新 tail，重试 */
        } else if (dif < 0) {
            return NSBC_ERR; /* 满：背压由上层处理 */
        } else {
            pos = __atomic_load_n((&q->tail), __ATOMIC_RELAXED);
        }
    }

    mpsc_cell_t *cell = &q->cells[pos & q->mask];
    __atomic_store_n((&cell->data), ((uintptr_t)msg), __ATOMIC_RELAXED);
    __atomic_store_n((&cell->seq), (pos + 1), __ATOMIC_RELEASE);
    return NSBC_OK;
}

int mem_mpsc_pop(mem_mpsc_t *q, void **out)
{
    if (!q || !out)
        return NSBC_INVAL;

    size_t pos = __atomic_load_n((&q->head), __ATOMIC_RELAXED);
    mpsc_cell_t *cell = &q->cells[pos & q->mask];
    size_t seq = __atomic_load_n((&cell->seq), __ATOMIC_ACQUIRE);
    if ((long)seq - (long)(pos + 1) != 0)
        return NSBC_ERR; /* 空（或该槽尚未发布） */

    *out = (void *)__atomic_load_n((&cell->data), __ATOMIC_RELAXED);
    /* 一整圈后该槽才可重新使用：seq = pos + cap + 1 */
    __atomic_store_n((&cell->seq), (pos + q->mask + 1), __ATOMIC_RELEASE);
    __atomic_store_n((&q->head), (pos + 1), __ATOMIC_RELAXED);
    return NSBC_OK;
}

uint32_t mem_mpsc_size(const mem_mpsc_t *q)
{
    if (!q)
        return 0;
    size_t t = __atomic_load_n((&q->tail), __ATOMIC_ACQUIRE);
    size_t h = __atomic_load_n((&q->head), __ATOMIC_ACQUIRE);
    return (uint32_t)(t - h);
}
