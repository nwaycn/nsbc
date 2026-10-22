/* mem_arena.c — 统一共享内存 Arena（REQ-ARCH-005，7.1.5） */
#include "mem.h"

#include <sys/mman.h>

#define MEM_ALIGN 16u
#define MEM_PAGE 4096u
#define MEM_DEFAULT_BYTES (4u * 1024u * 1024u)
#define MEM_HDR_MAGIC 0x4d454d48u /* 'MEMH' */
#define MEM_GUARD_BYTES 16

/* 物理块：连续排布，物理链（p_phys/n_phys）支持边界标记与双向合并；
 * free_next 仅在空闲块上有效。布局：
 *   [hdr][payload psize(16 对齐)][debug guard 16B] */
typedef struct mem_blk {
    uint32_t magic;
    uint32_t psize;
    int used;
    struct mem_blk *p_phys;
    struct mem_blk *n_phys;
    struct mem_blk *free_next;
} mem_blk_t;

#define MEM_HDR_SIZE ((size_t)(((sizeof(mem_blk_t) + MEM_ALIGN - 1) / MEM_ALIGN) * MEM_ALIGN))

static const uint8_t mem_guard_pat[MEM_GUARD_BYTES] = {
    0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE,
    0x5A, 0x5A, 0xA5, 0xA5, 0xCC, 0x33, 0xCC, 0x33};

struct mem_arena {
    mem_arena_cfg_t cfg;
    uint8_t *base;
    size_t bytes;
    mem_blk_t *fl_head;
    mem_stats_t st;
};

const char *mem_domain_name(mem_domain_t d)
{
    static const char *names[MEM_DOMAIN__COUNT] = {
        "signal", "media", "transcode", "record", "packet"};
    if (d < 0 || d >= MEM_DOMAIN__COUNT)
        return "?";
    return names[d];
}

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static uint8_t *payload_of(mem_blk_t *b)
{
    return (uint8_t *)b + MEM_HDR_SIZE;
}

static uint8_t *guard_of(mem_blk_t *b)
{
    return payload_of(b) + b->psize;
}

static void guard_fill(mem_arena_t *a, mem_blk_t *b)
{
    if (a->cfg.debug)
        memcpy(guard_of(b), mem_guard_pat, MEM_GUARD_BYTES);
}

static int guard_ok(const mem_arena_t *a, const mem_blk_t *b)
{
    if (!a->cfg.debug)
        return 1;
    const uint8_t *guard =
        (const uint8_t *)b + MEM_HDR_SIZE + b->psize;
    return memcmp(guard, mem_guard_pat, MEM_GUARD_BYTES) == 0;
}

static void fl_unlink(mem_arena_t *a, mem_blk_t *b)
{
    mem_blk_t *prev = NULL, *cur = a->fl_head;
    while (cur && cur != b) {
        prev = cur;
        cur = cur->free_next;
    }
    if (!cur)
        return;
    if (prev)
        prev->free_next = b->free_next;
    else
        a->fl_head = b->free_next;
    b->free_next = NULL;
}

static void fl_push(mem_arena_t *a, mem_blk_t *b)
{
    b->free_next = a->fl_head;
    a->fl_head = b;
}

nsbc_rc_t mem_arena_create(const mem_arena_cfg_t *cfg, mem_arena_t **out)
{
    if (!cfg || !out)
        return NSBC_INVAL;

    mem_arena_t *a = (mem_arena_t *)calloc(1, sizeof(*a));
    if (!a)
        return NSBC_NOMEM;
    a->cfg = *cfg;

    size_t want = cfg->bytes ? cfg->bytes : MEM_DEFAULT_BYTES;
    a->bytes = align_up(want, MEM_PAGE);

    /* REQ-ARCH-005：匿名 mmap 预留，页按需分配（预留不预触） */
    void *p = mmap(NULL, a->bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        free(a);
        return NSBC_NOMEM;
    }
    a->base = (uint8_t *)p;
    /* 大页可选：不可用/不支持时静默回退普通页 */
    if (cfg->hugepage)
        (void)madvise(a->base, a->bytes, MADV_HUGEPAGE);

    int g = cfg->debug ? MEM_GUARD_BYTES : 0;
    if (a->bytes < MEM_HDR_SIZE + (size_t)g + MEM_ALIGN) {
        munmap(a->base, a->bytes);
        free(a);
        return NSBC_INVAL;
    }

    mem_blk_t *b = (mem_blk_t *)a->base;
    b->magic = MEM_HDR_MAGIC;
    b->psize = (uint32_t)(a->bytes - MEM_HDR_SIZE - (size_t)g);
    b->used = 0;
    b->p_phys = b->n_phys = NULL;
    b->free_next = NULL;
    a->fl_head = b;
    guard_fill(a, b);

    a->st.total_bytes = a->bytes;
    *out = a;
    return NSBC_OK;
}

void mem_arena_destroy(mem_arena_t *a)
{
    if (!a)
        return;
    munmap(a->base, a->bytes);
    free(a);
}

void *mem_arena_alloc(mem_arena_t *a, size_t n)
{
    if (!a || n == 0)
        return NULL;

    size_t need = align_up(n, MEM_ALIGN);

    /* first-fit 空闲块 */
    mem_blk_t *b = a->fl_head;
    while (b && b->psize < need)
        b = b->free_next;

    if (!b) {
        a->st.nfail++;
        return NULL; /* 耗尽：调用方按 7.1.2 降级，不越域抢占 */
    }
    fl_unlink(a, b);

    int g = a->cfg.debug ? MEM_GUARD_BYTES : 0;
    /* 切分：剩余部分能再容纳一个最小块才切；否则整块交出（保留
     * psize，尾部少量 slack 随块走，不丢字节）。 */
    if (b->psize - need >= MEM_HDR_SIZE + (size_t)g + MEM_ALIGN) {
        uint32_t rem = (uint32_t)(b->psize - need - MEM_HDR_SIZE - (size_t)g);
        mem_blk_t *r = (mem_blk_t *)(payload_of(b) + need + g);
        r->magic = MEM_HDR_MAGIC;
        r->psize = rem;
        r->used = 0;
        r->p_phys = b;
        r->n_phys = b->n_phys;
        if (b->n_phys)
            b->n_phys->p_phys = r;
        b->n_phys = r;
        r->free_next = NULL;
        guard_fill(a, r);
        fl_push(a, r);
        b->psize = (uint32_t)need;
    }

    b->used = 1;
    guard_fill(a, b);

    a->st.used_bytes += b->psize;
    if (a->st.used_bytes > a->st.peak_bytes)
        a->st.peak_bytes = a->st.used_bytes;
    a->st.nalloc++;
    return payload_of(b);
}

void mem_arena_free(mem_arena_t *a, void *p)
{
    if (!a || !p)
        return;

    mem_blk_t *b = (mem_blk_t *)((uint8_t *)p - MEM_HDR_SIZE);
    if (b->magic != MEM_HDR_MAGIC || (uint8_t *)b < a->base ||
        (uint8_t *)b >= a->base + a->bytes) {
        a->st.nbadfree++;
        return;
    }
    if (!b->used) { /* 双重释放 */
        a->st.nbadfree++;
        return;
    }
    if (!guard_ok(a, b)) { /* canary 越界写：不回收，避免破坏空闲链 */
        a->st.nbadfree++;
        return;
    }

    b->used = 0;
    a->st.used_bytes -= b->psize;
    a->st.nfree++;

    int g = a->cfg.debug ? MEM_GUARD_BYTES : 0;

    /* 向后合并（物理后继空闲） */
    mem_blk_t *n = b->n_phys;
    if (n && !n->used) {
        fl_unlink(a, n);
        b->psize += (uint32_t)(MEM_HDR_SIZE + (size_t)g + n->psize);
        b->n_phys = n->n_phys;
        if (n->n_phys)
            n->n_phys->p_phys = b;
    }

    /* 向前合并（物理前驱空闲） */
    mem_blk_t *pr = b->p_phys;
    if (pr && !pr->used) {
        fl_unlink(a, pr);
        pr->psize += (uint32_t)(MEM_HDR_SIZE + (size_t)g + b->psize);
        pr->n_phys = b->n_phys;
        if (b->n_phys)
            b->n_phys->p_phys = pr;
        b = pr;
    }

    guard_fill(a, b);
    fl_push(a, b);
}

int mem_arena_check(const mem_arena_t *a)
{
    if (!a)
        return NSBC_INVAL;

    mem_blk_t *b = (mem_blk_t *)a->base;
    mem_blk_t *prev = NULL;
    uint64_t used_sum = 0;
    int g = a->cfg.debug ? MEM_GUARD_BYTES : 0;

    while (b) {
        if (b->magic != MEM_HDR_MAGIC)
            return NSBC_ERR;
        if (b->p_phys != prev)
            return NSBC_ERR;
        uint8_t *end = guard_of(b) + g;
        if (end < a->base || end > a->base + a->bytes)
            return NSBC_ERR;
        if (b->used) {
            if (!guard_ok(a, b))
                return NSBC_ERR;
            used_sum += b->psize;
        }
        prev = b;
        b = b->n_phys;
    }

    if (guard_of(prev) + g != a->base + a->bytes)
        return NSBC_ERR;
    if (used_sum != a->st.used_bytes)
        return NSBC_ERR;
    return NSBC_OK;
}

const mem_stats_t *mem_arena_stats(const mem_arena_t *a)
{
    return a ? &a->st : NULL;
}

size_t mem_arena_free_payload(const mem_arena_t *a)
{
    if (!a)
        return 0;
    size_t sum = 0;
    for (mem_blk_t *b = (mem_blk_t *)a->base; b; b = b->n_phys)
        if (!b->used)
            sum += b->psize;
    return sum;
}

size_t mem_arena_largest_free(const mem_arena_t *a)
{
    if (!a)
        return 0;
    size_t largest = 0;
    for (mem_blk_t *b = (mem_blk_t *)a->base; b; b = b->n_phys)
        if (!b->used && b->psize > largest)
            largest = b->psize;
    return largest;
}

double mem_arena_frag_rate(const mem_arena_t *a)
{
    size_t free_total = mem_arena_free_payload(a);
    if (free_total == 0)
        return 0.0;
    return 1.0 - (double)mem_arena_largest_free(a) / (double)free_total;
}
