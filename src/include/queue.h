#ifndef NSBC_QUEUE_H
#define NSBC_QUEUE_H

/* ACD 队列（REQ-PBX-CC-002~005，P2）
 *
 *  - 002 排队与路由：空闲座席按策略选取（同振全部/最久未接/轮询/
 *    最少通话/技能优先），无空闲入队等；
 *  - 003 优先级排队与溢出（VIP/超时转另一队列或语音信箱）；
 *  - 004 座席话后整理（与 agent WRAP 联动）；
 *  - 005 服务水平统计：入队数/接起数/放弃数/最长等待/可接阈值计数。
 * 预测式/预览外呼（P3）不在本头，后续外呼批次扩展。
 */
#include "nsbc_common.h"
#include "agent.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CQ_NAME_MAX      32
#define CQ_TENANT_MAX    32
#define CQ_MEMB_MAX      128
#define CQ_WAIT_MAX      256
#define CQ_OVERFLOW_MAX  48
#define CQ_QUEUE_CAP     256

typedef enum {
    CQ_STRATEGY_RING_ALL = 0,  /* 全空闲同振，首接听 */
    CQ_STRATEGY_LEAST_RECENT,  /* 最久未通话 */
    CQ_STRATEGY_ROUND_ROBIN,   /* 轮询（上次接通的下一位） */
    CQ_STRATEGY_FEWEST_CALLS   /* 累计接起最少 */
} cq_strategy_t;

typedef struct {
    char name[CQ_NAME_MAX];
    char tenant[CQ_TENANT_MAX];
    char members[CQ_MEMB_MAX][AGENT_EXT_MAX];
    int  n_members;
    char require_skill[AGENT_SKILL_LEN]; /* 空=不筛技能 */
    cq_strategy_t strategy;
    int  wrap_time_s;
    int  max_wait_s;                    /* 超时溢出 */
    char overflow[CQ_OVERFLOW_MAX];     /* 目标队列/IVR/VM 名 */
    int  moh_clip_id;                   /* 等待音乐（player 素材 id） */
    int  enabled;
} cq_queue_t;

typedef struct {
    void    *call_key;      /* 不透明呼叫键 */
    char     cli[32];       /* 主叫（VIP 判定由外部打分置 priority） */
    int      priority;      /* 大值先服务 */
    uint64_t enqueue_ms;
    uint64_t connect_ms;    /* 接起时刻（0=未接） */
} cq_waiter_t;

typedef struct {
    uint64_t total_enqueued;
    uint64_t total_answered;
    uint64_t total_abandoned;
    uint64_t total_overflowed;
    uint64_t answered_within_sla; /* SLA 阈值秒由参数给 */
    uint64_t max_wait_ms;
} cq_stats_t;

typedef struct cc_mgr cc_mgr_t;

int  cq_create(agent_pool_t *pool, cc_mgr_t **out);
void cq_destroy(cc_mgr_t *m);

int  cq_add(cc_mgr_t *m, const cq_queue_t *q);
int  cq_remove(cc_mgr_t *m, const char *tenant, const char *name);
const cq_queue_t *cq_get(cc_mgr_t *m, const char *tenant, const char *name);

/* 入队/放弃/溢出：waiter 由调用方填 call_key/cli/priority/enqueue_ms */
int  cq_enqueue(cc_mgr_t *m, const char *tenant, const char *name,
                cq_waiter_t w);
int  cq_abandon(cc_mgr_t *m, void *call_key);
int  cq_waiting_count(cc_mgr_t *m, const char *tenant, const char *name);

/* 选座席（座席空闲/技能/WRAP 结束事件触发）：
 *  - RING_ALL：out 填满全部可用座席，返回数量；
 *  - 其他策略：返回 1 个座席（呼叫同时 agent_reserve）。
 * now_ms：当前单调时钟；rnd：轮询/洗牌熵（调用方取）。 */
int  cq_dispatch(cc_mgr_t *m, const char *tenant, const char *name,
                 uint64_t now_ms, uint32_t rnd,
                 char out_ext[][AGENT_EXT_MAX], int out_cap,
                 void **out_call_key);

/* 接起/整理回调（驱动座席机与统计；answered_ms 用于 SLA） */
int  cq_answered(cc_mgr_t *m, const char *tenant, const char *name,
                 const char *ext, uint64_t answered_ms, int sla_s);
int  cq_finish(cc_mgr_t *m, const char *tenant, const char *name,
               const char *ext, uint64_t now_ms);

/* 超时扫描（时间轮周期调）：max_wait 到期者移溢出，返回溢出数；
 * overflow 目标的实际路由由组合根执行。 */
int  cq_tick_overflow(cc_mgr_t *m, const char *tenant, const char *name,
                      uint64_t now_ms);

void cq_stats(cc_mgr_t *m, const char *tenant, const char *name,
              cq_stats_t *out);

/* Web DB → C 侧整表 reload（OAM UDS 命令 `queues.reload` 调用）
 * json: Web queues 表导出的 JSON 数组；out_err/err_sz 填错误描述
 * 返回 NSBC_OK 或 <0（-1 = 业务错误，NSBC_ERR = 系统错，NSBC_INVAL = 参数错） */
int queue_table_reload_json(const char *json, char *out_err, size_t err_sz);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_QUEUE_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
