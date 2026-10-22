#ifndef NSBC_TIMER_WHEEL_H
#define NSBC_TIMER_WHEEL_H

/* REQ-ARCH-007：分层时间轮（P0）
 *
 * 三级轮（tick 默认 100ms）：
 *   L0 256 槽 × 100ms = 25.6s（事务重传密集区）
 *   L1 64 槽 × 25.6s  ≈ 27.3 分钟
 *   L2 64 槽 × 27.3min ≈ 29.1 小时（注册过期/会话刷新上限内）
 * 插入/取消/续约 O(1)；节点内嵌于对象，零额外分配；同一对象可嵌多个节点。
 * 严禁 timerfd/全局堆/红黑树（REQ-ARCH-007）；由专用时间轮线程单调时钟推进，
 * 繁忙追平时一次性 cascading，到期回调可合并（合并策略由回调上层决定）。
 */

#include <stdint.h>
#include <pthread.h>
#include "nsbc_common.h"
#include "nsbc_list.h"

#define TW_LEVELS 3
#define TW_L0_BITS 8
#define TW_LN_BITS 6

typedef struct tw_node tw_node_t;
typedef void (*tw_cb_fn)(uint64_t now_ms, void *arg);

struct tw_node {
    nsbc_node_t link;
    uint64_t expire_ms;
    tw_cb_fn cb;
    void *arg;
    uint32_t rotation; /* 上层轮剩余圈数 */
    uint8_t level;
    uint8_t queued;
    uint8_t cancelled;
};

typedef struct {
    uint64_t tick_ms;
    uint64_t now_ms;
    pthread_mutex_t lock; /* 跨线程 add/del/mod 与推进线程互斥；回调在锁外 */
    struct {
        nsbc_node_t *slots; /* 哨兵数组 */
        uint32_t nslots;
        uint32_t cur;
    } lv[TW_LEVELS];
} tw_t;

/* 失败仅 OOM（槽位数组一次性分配） */
int tw_init(tw_t *w, uint64_t tick_ms, uint64_t now_ms);
void tw_fini(tw_t *w);

void tw_node_init(tw_node_t *n);

/* expire_ms 必须 > now；最远 L2 跨度（约 29h），超出返回 NSBC_INVAL */
int tw_add(tw_t *w, tw_node_t *n, uint64_t expire_ms, tw_cb_fn cb,
           void *arg);
/* 取消：1=已摘除，0=未在轮上（已触发/已取消） */
int tw_del(tw_t *w, tw_node_t *n);
/* 续约 = 取消 + 重挂；返回 NSBC_INVAL 若节点已触发过 */
int tw_mod(tw_t *w, tw_node_t *n, uint64_t expire_ms);

/* 推进到 now_ms，cascading 并执行到期回调；返回触发数。
 * 回调中允许 tw_del/tw_add（对当前节点 tw_del 安全：已摘链）。 */
uint32_t tw_advance(tw_t *w, uint64_t now_ms);

/* 距离下一到期的等待毫秒；无到期返回 UINT64_MAX（调用方自行封顶） */
uint64_t tw_next_wait_ms(const tw_t *w);

#endif /* NSBC_TIMER_WHEEL_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
