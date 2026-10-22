#ifndef NSBC_PERCPU_H
#define NSBC_PERCPU_H

/* REQ-OBS-002：per-CPU 统计，槽位缓存行对齐杜绝 false sharing；
 * 采集开销目标 <1% CPU。槽位数按在线 CPU 数创建，不可动态变（启动期建）。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE    /* sched_getcpu() 需要 — 命令行已 -D_GNU_SOURCE 时自动跳过 */
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>     /* sched_getcpu() 声明在这里（不是 unistd.h）*/
#include "nsbc_atomic.h"
#include "nsbc_common.h"

typedef struct {
    unsigned long v;
    char pad[NSBC_CACHELINE - sizeof(unsigned long)];
} pcnt_slot_t NSBC_CACHELINE_ALIGNED;

typedef struct {
    unsigned n;
    pcnt_slot_t *slots;
} pcnt_t;

static inline unsigned pcnt_cpus(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1u;
}

static inline int pcnt_init(pcnt_t *c, unsigned n)
{
    if (n == 0)
        n = pcnt_cpus();
    c->n = n;
    if (posix_memalign((void **)&c->slots, NSBC_CACHELINE,
                       (size_t)n * sizeof(pcnt_slot_t)) != 0)
        return NSBC_NOMEM;
    memset(c->slots, 0, (size_t)n * sizeof(pcnt_slot_t));
    return NSBC_OK;
}

static inline void pcnt_free(pcnt_t *c)
{
    free(c->slots);
    c->slots = NULL;
}

static inline unsigned pcpt_idx(void)
{
#if defined(__linux__)
    unsigned cpu = (unsigned)sched_getcpu();
    extern unsigned pcnt_cpus(void);
    return cpu % pcnt_cpus();
#else
    return 0;
#endif
}

static inline void pcnt_inc(pcnt_t *c, unsigned cpu)
{
    sat_add(c->slots[cpu % c->n].v, 1);
}
static inline void pcnt_add(pcnt_t *c, unsigned cpu, unsigned long d)
{
    sat_add(c->slots[cpu % c->n].v, d);
}
static inline void pcnt_inc_here(pcnt_t *c) { pcnt_inc(c, pcpt_idx()); }

static inline unsigned long pcnt_sum(const pcnt_t *c)
{
    unsigned long s = 0;
    for (unsigned i = 0; i < c->n; ++i)
        s += sat_read_relaxed(c->slots[i].v);
    return s;
}

#endif /* NSBC_PERCPU_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
