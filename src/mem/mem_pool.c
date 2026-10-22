/* mem_pool.c — Bump Allocator（Phase 1 增量 2026-10-15）
 *
 * 核心：一个连续 buffer + 一个 current 指针。alloc 时 current+=aligned_size，
 * reset 时 current=base。O(1) 分配 + O(1) 整块回收。
 *
 * 设计细节：
 *   - 16 字节对齐（和 mem_arena MEM_ALIGN 一致）
 *   - 支持从 mem_arena 预留（物理隔离，零 malloc 调用）
 *   - 线程独占：内部无锁无原子，caller 负责 worker 线程独享
 *   - 统计跨 reset 累加（nalloc/nfail），used 归零
 *
 * TODO（压测后）：
 *   - pool 自动扩容（reset 时检查峰值 used，若接近 full 则重建更大 pool）
 *   - 跨 pool realloc（很少需要，先不做）
 */

#include "mem_pool.h"
#include "mem.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#define MEM_POOL_ALIGN   16u
#define MEM_POOL_DEFAULT (128u * 1024u) /* 128KB 默认，压测后调整 */

/* 结构体定义（头文件前向声明） */
struct mem_pool {
    uint8_t *base;       /* 预分配 buffer 起始 */
    uint8_t *current;    /* bump 指针（下一个分配位置） */
    size_t   total;      /* 总字节数 */
    size_t   used;       /* 当前已用（跨 reset 归零） */
    uint64_t nalloc;     /* 累计分配次数（跨 reset 累加） */
    uint64_t nfail;      /* 累计 OOM 次数（跨 reset 累加） */
    int      from_arena; /* 非 0：base 来自 mem_arena，销毁时走 arena_free */
    mem_arena_t *arena; /* 归属域（from_arena 时有效） */
};

static inline size_t align_up_pool(size_t v)
{
    return (v + MEM_POOL_ALIGN - 1) & ~(MEM_POOL_ALIGN - 1);
}

mem_pool_t *mem_pool_create(size_t bytes, mem_arena_t *arena)
{
    size_t total = bytes ? bytes : MEM_POOL_DEFAULT;
    size_t cap = sizeof(mem_pool_t) + total;
    mem_pool_t *p;

    if (arena) {
        p = (mem_pool_t *)mem_arena_alloc(arena, cap);
        if (!p) return NULL;
        memset(p, 0, cap);
        p->from_arena = 1;
        p->arena = arena;
    } else {
        p = (mem_pool_t *)calloc(1, cap);
        if (!p) return NULL;
        p->from_arena = 0;
        p->arena = NULL;
    }
    p->base = (uint8_t *)(p + 1);
    p->current = p->base;
    p->total = total;
    p->used = 0;
    p->nalloc = 0;
    p->nfail = 0;
    return p;
}

void mem_pool_destroy(mem_pool_t *p)
{
    if (!p) return;
    if (p->from_arena && p->arena) {
        mem_arena_free(p->arena, p);
    } else {
        free(p);
    }
}

void mem_pool_reset(mem_pool_t *p)
{
    if (!p) return;
    p->current = p->base;
    p->used = 0;
    /* 注意：不 memset base——上次分配的残留字节不会被读（都是从 current 之后
     * 开始写入的）。如果 caller 需要清零，应该用 mem_pool_calloc 或自行 memset。
     * 这是性能关键路径，不做额外清零。 */
}

/* ================================================================
 * 内部无日志版本（caller 已经记日志时调用）
 * ================================================================ */

static void *pool_alloc(mem_pool_t *p, size_t aligned)
{
    uint8_t *next;
    if (!p || !aligned) return NULL;
    if (aligned > (size_t)(p->base + p->total - p->current)) {
        p->nfail++;
        return NULL;
    }
    next = p->current;
    p->current += aligned;
    p->used += aligned;
    p->nalloc++;
    return next;
}

/* ================================================================
 * 公开 API（带 debug 版本）
 * ================================================================ */

void *mem_pool_alloc(mem_pool_t *p, size_t n)
{
    return mem_pool_alloc_debug(p, n, NULL, 0);
}

void *mem_pool_alloc_debug(mem_pool_t *p, size_t n, const char *file, int line)
{
    size_t aligned;
    void *ret;
    if (!p) return NULL;
    aligned = align_up_pool(n);
    ret = pool_alloc(p, aligned);
    if (!ret && file) {
        /* OOM 日志：用 log_debug 控制，默认不打（防止生产海量日志）。
         * 需要排查时调 log.level=debug 即可看到哪些 SIP 包解析占用过高。 */
        log_debug("mem_pool", "OOM: need=%zuB avail=%zuB nfail=%llu caller=%s:%d",
                  n, mem_pool_avail(p), (unsigned long long)p->nfail, file, line);
    }
    return ret;
}

void *mem_pool_calloc(mem_pool_t *p, size_t count, size_t size)
{
    return mem_pool_calloc_debug(p, count, size, NULL, 0);
}

void *mem_pool_calloc_debug(mem_pool_t *p, size_t count, size_t size,
                            const char *file, int line)
{
    size_t total;
    void *ret;
    if (!p || !count || !size) return NULL;
    /* 乘法溢出保护（32 位极限：count * size > SIZE_MAX） */
    if (count > (size_t)-1 / size) {
        if (file)
            log_warn("mem_pool", "calloc overflow: count=%zu size=%zu caller=%s:%d",
                     count, size, file, line);
        return NULL;
    }
    total = align_up_pool(count * size);
    ret = pool_alloc(p, total);
    if (ret) {
        memset(ret, 0, count * size);
    } else if (file) {
        log_debug("mem_pool", "calloc OOM: need=%zuB avail=%zuB caller=%s:%d",
                  total, mem_pool_avail(p), file, line);
    }
    return ret;
}

char *mem_pool_strdup(mem_pool_t *p, const char *s)
{
    size_t len;
    char *dst;
    if (!p || !s) return NULL;
    len = strlen(s);
    dst = (char *)mem_pool_alloc(p, len + 1);
    if (!dst) return NULL;
    memcpy(dst, s, len);
    dst[len] = '\0';
    return dst;
}

/* ================================================================
 * 查询
 * ================================================================ */

size_t mem_pool_used(const mem_pool_t *p)
{
    return p ? p->used : 0;
}

size_t mem_pool_total(const mem_pool_t *p)
{
    return p ? p->total : 0;
}

size_t mem_pool_avail(const mem_pool_t *p)
{
    return p ? (p->base + p->total - p->current) : 0;
}

uint64_t mem_pool_nalloc(const mem_pool_t *p)
{
    return p ? p->nalloc : 0;
}

uint64_t mem_pool_nfail(const mem_pool_t *p)
{
    return p ? p->nfail : 0;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
