#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "port_table.h"
#include "metric_reg.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 单 IP：指针表 + 空闲偶数端口栈 */
typedef struct {
    pthread_mutex_t lock;
    char ip[64];
    int used;
    void **slot;           /* 65536 */
    uint16_t *free_even;   /* 空闲偶数端口栈 */
    int nfree;
    int n_in_use;
} ip_table_t;

struct ptable {
    ptable_cfg_t cfg;
    ip_table_t **ips;
    int nips;
    uint64_t in_use;
};

static ip_table_t *ip_get(ptable_t *t, const char *ip, int create)
{
    for (int i = 0; i < t->nips; ++i) {
        if (t->ips[i]->used && !strcmp(t->ips[i]->ip, ip))
            return t->ips[i];
    }
    if (!create)
        return NULL;
    if (t->nips >= t->cfg.max_local_ips)
        return NULL;
    ip_table_t *it = calloc(1, sizeof(*it));
    if (!it)
        return NULL;
    pthread_mutex_init(&it->lock, NULL);
    snprintf(it->ip, sizeof(it->ip), "%s", ip);
    it->slot = calloc(65536, sizeof(void *));
    uint16_t lo = t->cfg.range_lo & ~1u;
    uint16_t hi = t->cfg.range_hi & ~1u;
    int cap = ((int)hi - (int)lo) / 2 + 1;
    it->free_even = malloc((size_t)cap * sizeof(uint16_t));
    if (!it->slot || !it->free_even) {
        free(it->slot);
        free(it->free_even);
        free(it);
        return NULL;
    }
    /* 高端口优先弹栈：自顶向下压 */
    for (int p = lo; p <= (int)hi; p += 2)
        it->free_even[it->nfree++] = (uint16_t)p;
    it->used = 1;
    t->ips[t->nips++] = it;
    return it;
}

/* OBS 自埋点（REQ-OBS-001）：在分配 RTP 端口数 render，ud=ptable；
 * 2026-09-27 A3 自 oam/obs_metrics.c 原样迁入，指标名保持不变。 */
static int r_ports(char *b, size_t cap, void *ud)
{
    ptable_t *p = (ptable_t *)ud;
    return snprintf(b, cap, "nsbc_rtp_ports_in_use %llu\n",
                    (unsigned long long)(p ? ptable_in_use(p) : 0));
}

int ptable_create(const ptable_cfg_t *cfg, ptable_t **out)
{
    ptable_t *t = calloc(1, sizeof(*t));
    if (!t)
        return NSBC_NOMEM;
    t->cfg = cfg ? *cfg : (ptable_cfg_t){0};
    if (t->cfg.range_lo == 0)
        t->cfg.range_lo = 20000;
    if (t->cfg.range_hi == 0)
        t->cfg.range_hi = 60000;
    if (t->cfg.max_local_ips <= 0)
        t->cfg.max_local_ips = 64;
    t->cfg.range_lo &= ~1u;
    t->cfg.range_hi &= ~1u;
    t->ips = calloc((size_t)t->cfg.max_local_ips, sizeof(*t->ips));
    if (!t->ips) {
        free(t);
        return NSBC_NOMEM;
    }
    metric_source_register("nsbc_rtp_ports",
                           "RTP ports currently allocated", r_ports, t);
    *out = t;
    return NSBC_OK;
}

void ptable_destroy(ptable_t *t)
{
    if (!t)
        return;
    for (int i = 0; i < t->nips; ++i) {
        free(t->ips[i]->slot);
        free(t->ips[i]->free_even);
        free(t->ips[i]);
    }
    free(t->ips);
    free(t);
}

int ptable_alloc_pair(ptable_t *t, const char *local_ip, void *owner,
                      uint16_t *rtp_port, uint16_t *rtcp_port)
{
    return ptable_alloc_pair_range(t, local_ip, owner, 0, 0,
                                   rtp_port, rtcp_port);
}

int ptable_alloc_pair_range(ptable_t *t, const char *local_ip, void *owner,
                            uint16_t lo, uint16_t hi,
                            uint16_t *rtp_port, uint16_t *rtcp_port)
{
    ip_table_t *it = ip_get(t, local_ip ? local_ip : "0.0.0.0", 1);
    if (!it)
        return NSBC_NOSPC;

    /* 范围归一：取偶；lo==0/hi==0 → 表默认段（fast path 同旧语义） */
    uint16_t rlo = (lo == 0) ? t->cfg.range_lo : (uint16_t)(lo & ~1u);
    uint16_t rhi = (hi == 0) ? t->cfg.range_hi : (uint16_t)(hi & ~1u);
    if (rhi < rlo)
        rhi = rlo; /* 倒序段：容错为单端口 */

    /* 超时锁：正常应立即拿到；若卡 → 说明某个线程持 it->lock 没释放。
     * 1 秒超时后打 ERROR 并返回 NOSPC（让 offer_hook 走失败路径，不阻塞整个 SIP 线程）。 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    int rc = pthread_mutex_timedlock(&it->lock, &ts);
    if (rc != 0) {
        llog(LOG_ERR, "media",
             "ptable: timedlock FAILED after 1s! local_ip=%s owner=%p → DEADLOCK? "
             "returning NOSPC to avoid SIP thread hang",
             local_ip ? local_ip : "(null)", owner);
        return NSBC_NOSPC;
    }
    /* 诊断：如果锁已被持有→线程会卡在这里。卡的时候抓 gdb bt 看持有者栈 */
    if (it->nfree == 0) {
        pthread_mutex_unlock(&it->lock);
        return NSBC_NOSPC;
    }
    uint16_t p;
    if (rlo <= t->cfg.range_lo && rhi >= t->cfg.range_hi) {
        /* 覆盖全段：栈顶 O(1) 弹出（与旧实现完全一致的热路径） */
        p = it->free_even[--it->nfree];
    } else {
        /* 范围限定：自栈顶向下找首个落段空闲端口，与栈顶交换后弹出 */
        int idx = -1;
        for (int i = it->nfree - 1; i >= 0; --i) {
            uint16_t q = it->free_even[i];
            if (q >= rlo && q <= rhi) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            pthread_mutex_unlock(&it->lock);
            return NSBC_NOSPC; /* 段内耗尽 */
        }
        p = it->free_even[idx];
        it->free_even[idx] = it->free_even[it->nfree - 1];
        --it->nfree;
    }
    it->slot[p] = owner;
    it->slot[p + 1] = owner;
    it->n_in_use += 2;
    pthread_mutex_unlock(&it->lock);
    __sync_fetch_and_add(&t->in_use, 2);
    *rtp_port = p;
    *rtcp_port = (uint16_t)(p + 1);
    return NSBC_OK;
}

void ptable_release_pair(ptable_t *t, const char *local_ip, uint16_t rtp_port)
{
    ip_table_t *it = ip_get(t, local_ip ? local_ip : "0.0.0.0", 0);
    if (!it)
        return;
    uint16_t p = rtp_port & ~1u;
    pthread_mutex_lock(&it->lock);
    if (it->slot[p]) {
        it->slot[p] = NULL;
        it->slot[p + 1] = NULL;
        it->free_even[it->nfree++] = p;
        it->n_in_use -= 2;
        pthread_mutex_unlock(&it->lock);
        __sync_fetch_and_sub(&t->in_use, 2);
        return;
    }
    pthread_mutex_unlock(&it->lock);
}

void *ptable_lookup(ptable_t *t, const char *local_ip, uint16_t port)
{
    ip_table_t *it = ip_get(t, local_ip ? local_ip : "0.0.0.0", 0);
    if (!it)
        return NULL;
    /* 读侧：slot 指针写者在互斥内发布；指针读写原子性由自然对齐保证，
     * P1 改 RCU 后此读锁可去。 */
    return it->slot[port];
}

uint64_t ptable_in_use(ptable_t *t)
{
    return __sync_add_and_fetch(&t->in_use, 0);
}
