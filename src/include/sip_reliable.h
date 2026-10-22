#ifndef NSBC_SIP_RELIABLE_H
#define NSBC_SIP_RELIABLE_H

/* 可靠预应答（RFC 3262 100rel/PRACK）与会话更新（RFC 3311 UPDATE）
 *
 * 对应：
 *   REQ-SIP-010（P0）UPDATE：早期/对话内更新会话参数而不改变对话状态；
 *   SDP-003（P1）100rel/PRACK：18x 可靠传输与 RSeq/RACK 配对。
 * 现状：PRACK/UPDATE 在 b2bua 方法分发裸回 501；glare 退避工具在
 * sip_msg（sip_upd_guard，零生产调用）。本头为生产接线定义每 Leg 状态。
 *
 * 分层：sip 层；仅依赖 parse（sip_msg）与 base。
 */
#include "nsbc_common.h"
#include "sip_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REL100_RSEQ_START  1L    /* RFC 3262 §4：RSeq 从 1 起单调递增 */

/* 单 Leg（单向）100rel 状态。UAS 侧维护已发可靠 18x 的 RSeq；
 * UAC 侧维护已收最大 RSeq，去重/乱序/补传由实现层判定。 */
typedef struct {
    int      enabled;        /* 对端 Require/Supported 含 100rel */
    long     next_rseq;      /* UAS：下一个待分配 RSeq */
    long     last_recv_rseq; /* UAC：已确认的最大 RSeq（去重） */
    int      awaiting_prack; /* UAS：存在未确认可靠 18x */
    uint64_t sent_ms;        /* 单调时钟：重传 18x 的计时基准 */
} rel100_leg_t;

/* UPDATE 协商上下文（每 Leg）：
 *  - early 阶段允许带 SDP precondition 更新（RFC 3312 联动）；
 *  - confirmed 阶段更新目标媒体；glare 用 sip_upd_guard_t。 */
typedef struct {
    int              peer_supports_update; /* Supported/Allow 含 UPDATE */
    int              update_in_progress;
    sip_upd_guard_t *guard; /* 可空：由 b2bua leg 持有，本结构只引用 */
} upd_leg_t;

void rel100_leg_init(rel100_leg_t *r, int enabled);
void upd_leg_init(upd_leg_t *u, sip_upd_guard_t *guard);

/* UAS：分配新 RSeq（可靠 18x 发送前调用） */
int  rel100_alloc_rseq(rel100_leg_t *r, long *out_rseq);
/* UAC：收到可靠 18x（180/183 + Require:100rel），RSeq 去重检查；
 * 返回 OK 接受新值 / EXISTS 重传（回缓存 PRACK 语义）/ PROTO 乱序。 */
int  rel100_on_provisional(rel100_leg_t *r, long rseq);
/* UAS：收到 PRACK，校验 RACK=CSeq RSeq（骨架） */
int  rel100_on_prack(rel100_leg_t *r, long cseq, long rseq);

/* 是否可发 UPDATE：对端支持且无未决更新（glare 归 sip_upd_begin 判 491） */
int  upd_can_send(const upd_leg_t *u);
/* 入向 UPDATE 处理入口（骨架：校验状态/转交 offer-answer/回 200） */
int  upd_on_request(upd_leg_t *u, const sip_msg_t *msg);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_SIP_RELIABLE_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
