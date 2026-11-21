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
#define CQ_CALLID_MAX    128   /* SIP Call-ID 上限（入队拷贝，跨线程重查呼叫用） */
/* 等待音乐默认素材（相对 nsbcd 启动目录）：DB moh_sound 为空或 "default" 时使用 */
#define CQ_DEFAULT_MOH_WAV "./sounds/default/moh.wav"

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
    int  moh_clip_id;                   /* 等待音乐（player 素材 id，历史字段） */
    char moh_wav[192];                  /* 等待音乐 WAV 路径（3.3；空="default" →
                                         * CQ_DEFAULT_MOH_WAV，见下） */
    int  enabled;
} cq_queue_t;

typedef struct {
    void    *call_key;      /* 不透明呼叫键（仅在呼叫存活期内有效，dispatch 需用 callid 重查） */
    char     callid[CQ_CALLID_MAX]; /* 入队时拷贝的 SIP Call-ID：跨线程派发/溢出时用它在
                                     * SIP 线程重查呼叫，避免呼叫已释放导致 UAF */
    char     cli[32];       /* 主叫（VIP 判定由外部打分置 priority） */
    int      priority;      /* 大值先服务 */
    uint64_t enqueue_ms;
    uint64_t connect_ms;    /* 接起时刻（0=未接） */
    /* Phase 2 异步 ACD 字段（由 cq_dispatch 填，调用方入队时置 0）：
     * dispatch_ms>0 表示已派发给座席、正在振铃，队列 tick 不再重复派发；
     * agent_ext 记录派给谁，供 cq_finish/超时回收清理。 */
    uint64_t dispatch_ms;
    char     agent_ext[AGENT_EXT_MAX];
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

/* Phase 1（同步 resolve 选路）：按队列策略从 members 里选一个应振铃成员。
 * 与 cq_dispatch 的区别：只读队列配置 + 原子占位，不做 agent_reserve，
 * 不依赖 agent_pool 是否已填充座席——用于 signaling 同步 resolve 阶段。
 * avail：过滤“可振铃”成员（已注册/在线），NULL=全部可用。
 * call_key：非空时在选中瞬间原子执行 busy+1 并登记归属（防并发重复派发），
 *           呼叫结束由 cq_settle_call/ cq_ext_release_call 释放；
 *           为空（originate 无呼叫对象）时只选不占位。
 * 返回 0=成功（out_ext 已填），-1=无可用成员，其它<0=队列不存在等错误。 */
typedef int (*cq_member_avail_fn)(const char *ext, void *ud);
int  cq_pick_member(cc_mgr_t *m, const char *tenant, const char *name,
                    uint32_t rnd, cq_member_avail_fn avail, void *ud,
                    void *call_key, char *out_ext, size_t outsz);

/* === 3.3 排队等待 + MoH（同步 resolve 全忙入队、tick 异步派发）===
 * 同步 resolve 在全部成员忙时不再回 486，而是 cq_enqueue 后让 A 腿 183 等待；
 * 每个 tick 取一个待派发 waiter，用 cq_pick_member 原子占位后交给组合根建 B 腿。
 *
 *   cq_set_member_avail_cb —— 注册“成员是否可振铃”过滤（注册查询等），
 *                             tick 派发与 resolve 选路共用，NULL=不过滤。
 *   cq_peek_pending        —— 取队首待派发 waiter（dispatch_ms==0）的 call_key
 *                             和入队时拷贝的 callid。0=有，1=无，<0=错误。
 *                             跨线程派发必须用 callid 在 SIP 线程重查呼叫
 *                             （call_key 可能在 peek 与执行之间被释放）。
 *                             返回后马上派发前需 cq_mark_dispatched。
 *   cq_mark_dispatched     —— 标记该 waiter 已派发给 ext（置 dispatch_ms/agent_ext）。
 *                             NSBC_NOENT=waiter 已被主叫放弃移除，调用方需回滚占位。
 *   cq_requeue             —— 派发后 B 腿未接通（忙/超时）：清 dispatch_ms 重新排队。
 *   cq_set_overflow_cb     —— max_wait 超时回调（cq_tick 内触发，勿在回调里再调 cq_*）。 */
void cq_set_member_avail_cb(cc_mgr_t *m, cq_member_avail_fn fn, void *ud);
int  cq_peek_pending(cc_mgr_t *m, const char *tenant, const char *name,
                     void **out_key, char *out_callid, size_t cid_sz);
int  cq_mark_dispatched(cc_mgr_t *m, void *call_key, const char *ext,
                        uint64_t now_ms);
void cq_requeue(cc_mgr_t *m, void *call_key);
typedef void (*cq_overflow_cb)(void *call_key, const char *callid,
                               const char *tenant, const char *name, void *ud);
void cq_set_overflow_cb(cc_mgr_t *m, cq_overflow_cb cb, void *ud);

/* Phase 1 统计结算：
 *   cq_track_dispatch —— 记录 call_key 被派发到哪个队列/成员；
 *   cq_settle_call    —— 呼叫结束时按 call_key 结算（answered_ms>0=接起），
 *                        更新 leastrecent/fewestcalls 所需的历史；
 *                        未派发过的 call_key 直接忽略。
 * 两者都不触碰 agent_pool。 */
void cq_track_dispatch(cc_mgr_t *m, void *call_key, const char *tenant,
                       const char *name, const char *ext);
void cq_settle_call(cc_mgr_t *m, void *call_key, uint64_t now_ms,
                    uint64_t answered_ms);

/* ==================================================================
 * 分机忙线 / 置忙状态（REQ-PBX-CC-006）
 *
 * 队列选路必须排除两类成员：
 *   1) 忙线：正在振铃或通话中（可能来自队列派发，也可能是直呼/外呼）；
 *   2) 置忙：座席主动小休（午餐/培训/其他），由 Web 控制台或话机特征码写入。
 *
 * 忙线用引用计数：同一分机同时存在多路呼叫（呼叫等待）时计数 >1，
 * 只有全部结束才恢复空闲。
 *
 * 并发：64 分片 rwlock + 开放寻址哈希；单分机查找 O(1)，不同分片互不阻塞。
 * 锁序固定为 call_shard → ext_shard，禁止反向获取。
 * ================================================================== */
#define CQ_EXT_REASON_NONE      0
#define CQ_EXT_REASON_BREAK     1   /* 午餐/小休 */
#define CQ_EXT_REASON_TRAINING  2
#define CQ_EXT_REASON_OTHER     3

typedef struct {
    int      busy;          /* 忙线引用计数（>0 = 有呼叫占用） */
    int      paused;        /* 1=置忙 */
    int      pause_reason;  /* CQ_EXT_REASON_* */
    uint64_t since_ms;      /* 最近一次状态变更时刻 */
} cq_ext_state_t;

/* 呼叫占用登记：call_key 命中的分机忙线 +1；同一 call_key 重复调用幂等。
 * cq_track_dispatch 旧接口保留，内部同时登记忙线。 */
int  cq_ext_mark_busy(cc_mgr_t *m, void *call_key, const char *tenant,
                      const char *ext);

/* 固定槽位登记：slot 0 = B 腿被叫分机，slot 1 = A 腿主叫分机。
 * ext→ext 呼叫两侧都要占忙线，用不同槽位避免互相覆盖。 */
int  cq_ext_mark_busy_slot(cc_mgr_t *m, void *call_key, const char *tenant,
                           const char *ext, int slot);

/* 释放 call_key 占用的分机忙线（呼叫结束时调；未登记则忽略） */
void cq_ext_release_call(cc_mgr_t *m, void *call_key);

/* 置忙/恢复（reason：CQ_EXT_REASON_*）。返回 NSBC_OK/NOENT/INVAL */
int  cq_ext_pause(cc_mgr_t *m, const char *tenant, const char *ext, int reason);
int  cq_ext_resume(cc_mgr_t *m, const char *tenant, const char *ext);

/* 是否可被队列派发：1=空闲可用；0=忙线/置忙。
 * 未登记的分机按“可用”处理（fail-open，避免外部号码或未配置分机被误排除）。 */
int  cq_ext_is_free(cc_mgr_t *m, const char *tenant, const char *ext);

/* 查询单个分机状态；未登记返回 NSBC_NOENT */
int  cq_ext_get(cc_mgr_t *m, const char *tenant, const char *ext,
                cq_ext_state_t *out);

/* 导出某租户下所有已登记分机状态（JSON 数组），供 OAM/Web 轮询。
 * tenant 为空串=全部租户。返回写入长度，超容量返回 NSBC_NOSPC。 */
int  cq_ext_list_json(cc_mgr_t *m, const char *tenant, char *out, size_t cap);

/* 接起/整理回调（驱动座席机与统计；answered_ms 用于 SLA）
 * Phase 2：cq_answered 改为按 call_key 定位 waiter（异步派发后队列里可能同时
 * 有多个待派发 waiter，按 tenant/name 无法唯一定位），座席取 waiter->agent_ext。
 * 返回 NSBC_NOENT=该 call_key 不在队列（未入队或已被清理）。 */
int  cq_answered(cc_mgr_t *m, void *call_key, uint64_t answered_ms, int sla_s);
int  cq_finish(cc_mgr_t *m, const char *tenant, const char *name,
               const char *ext, uint64_t now_ms);

/* 超时扫描（时间轮周期调）：max_wait 到期者移溢出，返回溢出数；
 * overflow 目标的实际路由由组合根执行。 */
typedef struct {
    void *call_key;
    char  callid[CQ_CALLID_MAX];
} cq_ovf_t;

int  cq_tick_overflow(cc_mgr_t *m, const char *tenant, const char *name,
                      uint64_t now_ms);
/* 同上，但把超时者写入 out（最多 cap 个，含 call_key + callid），
 * 供调用方执行溢出路由（转 queue:/ivr:/挂机等）。返回写入数量。 */
int  cq_tick_overflow_ex(cc_mgr_t *m, const char *tenant, const char *name,
                         uint64_t now_ms, cq_ovf_t *out, int cap);

/* Phase 2 异步 ACD 内核：
 *   cq_dispatch_cb —— 队列选到座席后，由组合根真正发起对座席的呼叫
 *                     （建 B 腿 / 桥接）。cb 在 cq_tick 返回前被调用，
 *                     严禁在 cb 内做阻塞 I/O，也不要再调 cq_* 入口。
 *                     返回 NSBC_OK=已派发；<0=派发失败（该 waiter 已退回
 *                     等待，本 tick 不再继续尝试该队列，避免空转）。
 *   cq_tick        —— 周期驱动（建议 1s）：
 *                     1) agent_tick：WRAP 到期 → IDLE；
 *                     2) 每队列 cq_tick_overflow_ex（超时溢出回调）；
 *                     3) 有 waiter 且可用座席 → cq_dispatch → cb。
 *                     out_cap 由实现固定为 CQ_MEMB_MAX。
 * 返回本次派发/溢出的动作总数（仅用于观测）。 */
typedef int (*cq_dispatch_cb)(void *call_key, const char *callid,
                              const char *tenant, const char *name,
                              const char *ext, void *ud);
void cq_set_dispatch_cb(cc_mgr_t *m, cq_dispatch_cb cb, void *ud);
int  cq_tick(cc_mgr_t *m, uint64_t now_ms);

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
