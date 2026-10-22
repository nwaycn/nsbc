#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "timer_wheel.h"

#include <pthread.h>
#include <stdlib.h>

#define TW_N0 (1u << TW_L0_BITS)
#define TW_N1 (1u << TW_LN_BITS)
#define TW_N2 (1u << TW_LN_BITS)
#define TW_M0 (TW_N0 - 1u)
#define TW_M1 (TW_N1 - 1u)
#define TW_M2 (TW_N2 - 1u)

static uint64_t tw_ticks(const tw_t *w, uint64_t ms)
{
    return ms / w->tick_ms;
}

int tw_init(tw_t *w, uint64_t tick_ms, uint64_t now_ms)
{
    if (tick_ms == 0)
        return NSBC_INVAL;
    w->tick_ms = tick_ms;
    w->now_ms = now_ms;
    pthread_mutex_init(&w->lock, NULL);
    const uint32_t n[TW_LEVELS] = {TW_N0, TW_N1, TW_N2};
    for (int l = 0; l < TW_LEVELS; ++l) {
        w->lv[l].nslots = n[l];
        w->lv[l].cur = 0;
        w->lv[l].slots = calloc(n[l], sizeof(nsbc_node_t));
        if (!w->lv[l].slots) {
            for (int k = 0; k < l; ++k)
                free(w->lv[k].slots);
            pthread_mutex_destroy(&w->lock);
            return NSBC_NOMEM;
        }
        for (uint32_t i = 0; i < n[l]; ++i)
            nsbc_node_init(&w->lv[l].slots[i]);
    }
    return NSBC_OK;
}

void tw_fini(tw_t *w)
{
    for (int l = 0; l < TW_LEVELS; ++l)
        free(w->lv[l].slots);
    pthread_mutex_destroy(&w->lock);
}

void tw_node_init(tw_node_t *n)
{
    nsbc_node_init(&n->link);
    n->expire_ms = 0;
    n->cb = NULL;
    n->arg = NULL;
    n->rotation = 0;
    n->level = 0;
    n->queued = 0;
    n->cancelled = 0;
}

/* 按绝对到期 tick 挂槽；调用时 node 已不在任何槽中（调用方持锁）。
 * round_cur=1（新增/续约路径）：到期 tick 落在当前 tick 桶内时挂入
 * 下一槽——当前槽在本次推进开始时已过，下一槽在跨 tick 边界时排空；
 * round_cur=0（级联/重挂路径）：按精确 et 挂槽。 */
static void tw_enqueue(tw_t *w, tw_node_t *n, uint64_t now_tick,
                       int round_cur)
{
    uint64_t et = tw_ticks(w, n->expire_ms);
    uint64_t d = (et > now_tick) ? et - now_tick : 1;
    uint64_t s0tick = (round_cur && et <= now_tick) ? now_tick + 1 : et;
    nsbc_node_t *slot;
    if (d < TW_N0) {
        n->level = 0;
        slot = &w->lv[0].slots[(uint32_t)s0tick & TW_M0];
    } else if (d < (uint64_t)TW_N0 * TW_N1) {
        n->level = 1;
        slot = &w->lv[1].slots[(uint32_t)(et >> TW_L0_BITS) & TW_M1];
    } else {
        n->level = 2;
        slot = &w->lv[2].slots[(uint32_t)(et >> (TW_L0_BITS + TW_LN_BITS)) &
                               TW_M2];
    }
    n->queued = 1;
    nsbc_list_insert(slot, &n->link);
}

int tw_add(tw_t *w, tw_node_t *n, uint64_t expire_ms, tw_cb_fn cb,
           void *arg)
{
    pthread_mutex_lock(&w->lock);
    if (!n || !cb || expire_ms <= w->now_ms || n->queued) {
        pthread_mutex_unlock(&w->lock);
        return NSBC_INVAL;
    }
    uint64_t d_ticks = (expire_ms - w->now_ms + w->tick_ms - 1) / w->tick_ms;
    if (d_ticks >= (uint64_t)TW_N0 * TW_N1 * TW_N2) {
        pthread_mutex_unlock(&w->lock);
        return NSBC_INVAL; /* 超出 L2 跨度：调用方自行分段重挂 */
    }
    n->expire_ms = expire_ms;
    n->cb = cb;
    n->arg = arg;
    n->cancelled = 0;
    tw_enqueue(w, n, tw_ticks(w, w->now_ms), 1);
    pthread_mutex_unlock(&w->lock);
    return NSBC_OK;
}

int tw_del(tw_t *w, tw_node_t *n)
{
    pthread_mutex_lock(&w->lock);
    if (!n || !n->queued) {
        pthread_mutex_unlock(&w->lock);
        return 0;
    }
    nsbc_list_del(&n->link);
    n->queued = 0;
    pthread_mutex_unlock(&w->lock);
    return 1;
}

int tw_mod(tw_t *w, tw_node_t *n, uint64_t expire_ms)
{
    pthread_mutex_lock(&w->lock);
    int was = (n && n->queued);
    if (was) {
        nsbc_list_del(&n->link);
        n->queued = 0;
    }
    if (!was || !n->cb || expire_ms <= w->now_ms) {
        pthread_mutex_unlock(&w->lock);
        return NSBC_INVAL;
    }
    uint64_t d_ticks = (expire_ms - w->now_ms + w->tick_ms - 1) / w->tick_ms;
    if (d_ticks >= (uint64_t)TW_N0 * TW_N1 * TW_N2) {
        pthread_mutex_unlock(&w->lock);
        return NSBC_INVAL;
    }
    n->expire_ms = expire_ms;
    n->cancelled = 0;
    tw_enqueue(w, n, tw_ticks(w, w->now_ms), 1);
    pthread_mutex_unlock(&w->lock);
    return NSBC_OK;
}

/* 摘出某 L0 槽中到期节点到 fired 链；未到期重挂。持锁调用 */
static void tw_drain_slot(tw_t *w, nsbc_node_t *slot, uint64_t now_tick,
                          nsbc_node_t *fired)
{
    nsbc_node_t *pos, *tmp;
    nsbc_list_for_each_safe(pos, tmp, slot)
    {
        tw_node_t *n = NSBC_CONTAINER_OF(pos, tw_node_t, link);
        nsbc_list_del(pos);
        n->queued = 0;
        if (n->cancelled)
            continue;
        if (tw_ticks(w, n->expire_ms) <= now_tick) {
            nsbc_list_append(fired, pos); /* 锁外统一回调 */
        } else {
            tw_enqueue(w, n, now_tick, 0);
        }
    }
}

uint32_t tw_advance(tw_t *w, uint64_t now_ms)
{
    nsbc_node_t fired;
    nsbc_node_init(&fired);
    uint32_t fired_n = 0;

    pthread_mutex_lock(&w->lock);
    if (now_ms <= w->now_ms) {
        pthread_mutex_unlock(&w->lock);
        return 0;
    }
    uint64_t offset = w->now_ms % w->tick_ms;
    uint64_t start = tw_ticks(w, w->now_ms);
    uint64_t end = tw_ticks(w, now_ms);
    for (uint64_t t = start + 1; t <= end; ++t) {
        uint32_t s0 = (uint32_t)t & TW_M0;
        if (s0 == 0) {
            uint32_t s1 = (uint32_t)(t >> TW_L0_BITS) & TW_M1;
            if (s1 == 0) {
                uint32_t s2 =
                    (uint32_t)(t >> (TW_L0_BITS + TW_LN_BITS)) & TW_M2;
                nsbc_node_t *slot2 = &w->lv[2].slots[s2];
                nsbc_node_t *p, *tmp;
                nsbc_list_for_each_safe(p, tmp, slot2)
                {
                    tw_node_t *n = NSBC_CONTAINER_OF(p, tw_node_t, link);
                    nsbc_list_del(p);
                    n->queued = 0;
                    if (!n->cancelled)
                        tw_enqueue(w, n, t, 0);
                }
            }
            nsbc_node_t *slot1 = &w->lv[1].slots[s1];
            nsbc_node_t *p, *tmp;
            nsbc_list_for_each_safe(p, tmp, slot1)
            {
                tw_node_t *n = NSBC_CONTAINER_OF(p, tw_node_t, link);
                nsbc_list_del(p);
                n->queued = 0;
                if (!n->cancelled)
                    tw_enqueue(w, n, t, 0);
            }
        }
        w->now_ms = (t == end) ? now_ms : (t * w->tick_ms + offset);
        tw_drain_slot(w, &w->lv[0].slots[s0], t, &fired);
    }
    w->now_ms = now_ms;
    /* 统计到期数并在锁外回调 */
    for (nsbc_node_t *p = fired.next; p != &fired; p = p->next)
        ++fired_n;
    pthread_mutex_unlock(&w->lock);

    nsbc_node_t *pos, *tmp;
    nsbc_list_for_each_safe(pos, tmp, &fired)
    {
        nsbc_list_del(pos);
        tw_node_t *n = NSBC_CONTAINER_OF(pos, tw_node_t, link);
        n->cb(now_ms, n->arg);
    }
    return fired_n;
}

uint64_t tw_next_wait_ms(const tw_t *w)
{
    /* 调用点仅在推进线程；仍加锁保证与插入互斥的发布语义 */
    pthread_mutex_lock((pthread_mutex_t *)&w->lock);
    uint64_t now_tick = tw_ticks(w, w->now_ms);
    uint64_t ret = UINT64_MAX;
    /* L0：找最近非空槽 */
    for (uint32_t i = 1; i <= TW_N0; ++i) {
        uint32_t s = (uint32_t)(now_tick + i) & TW_M0;
        if (!nsbc_list_empty(&w->lv[0].slots[s])) {
            ret = i * w->tick_ms;
            break;
        }
    }
    if (ret == UINT64_MAX) {
        /* L1 非空：最近不超过一个 L0 跨度 */
        for (uint32_t i = 0; i < TW_N1; ++i) {
            uint32_t s = ((uint32_t)(now_tick >> TW_L0_BITS) + i) & TW_M1;
            if (!nsbc_list_empty(&w->lv[1].slots[s])) {
                ret = (uint64_t)TW_N0 * w->tick_ms;
                break;
            }
        }
    }
    if (ret == UINT64_MAX) {
        for (uint32_t i = 0; i < TW_N2; ++i) {
            uint32_t s =
                ((uint32_t)(now_tick >> (TW_L0_BITS + TW_LN_BITS)) + i) &
                TW_M2;
            if (!nsbc_list_empty(&w->lv[2].slots[s])) {
                ret = (uint64_t)TW_N0 * TW_N1 * w->tick_ms;
                break;
            }
        }
    }
    pthread_mutex_unlock((pthread_mutex_t *)&w->lock);
    return ret;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
