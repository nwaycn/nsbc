/* 100rel/PRACK 与 UPDATE——粗骨架
 * REQ-SIP-010（P0 UPDATE）、SDP-003（P1 RFC3262 可靠预应答）
 *
 * P0 阶段：纯状态工具与 NOTSUP 入口，不接 b2bua 方法分发。
 * 接线点（P1）：b2bua 18x 发送分支（require 100rel 时 rel100_alloc_rseq）、
 * PRACK 入向（当前 501）、UPDATE 入向（当前 501）。
 */
#include "sip_reliable.h"

void rel100_leg_init(rel100_leg_t *r, int enabled)
{
    if (r == NULL)
        return;
    memset(r, 0, sizeof(*r));
    r->enabled = enabled ? 1 : 0;
    r->next_rseq = REL100_RSEQ_START;
}

void upd_leg_init(upd_leg_t *u, sip_upd_guard_t *guard)
{
    if (u == NULL)
        return;
    memset(u, 0, sizeof(*u));
    u->guard = guard;
}

int rel100_alloc_rseq(rel100_leg_t *r, long *out_rseq)
{
    if (r == NULL || out_rseq == NULL || !r->enabled)
        return NSBC_INVAL;
    if (r->awaiting_prack)
        return NSBC_AGAIN; /* 上一可靠 18x 未确认，不得发新值（RFC3262） */
    *out_rseq = r->next_rseq++;
    r->awaiting_prack = 1;
    return NSBC_OK;
}

int rel100_on_provisional(rel100_leg_t *r, long rseq)
{
    if (r == NULL || rseq <= 0)
        return NSBC_INVAL;
    if (rseq == r->last_recv_rseq)
        return NSBC_EXISTS;   /* 重传：重发对应 PRACK，不重复处理 */
    if (rseq < r->last_recv_rseq)
        return NSBC_PROTO;    /* 乱序回退 */
    r->last_recv_rseq = rseq;
    return NSBC_OK;
}

int rel100_on_prack(rel100_leg_t *r, long cseq, long rseq)
{
    /* TODO(P1, RFC3262 §6)：RACK="RSeq CSeq Method" 全字段配对校验，
     * 不匹配回 488；当前仅清等待位，不接事务层。 */
    if (r == NULL || cseq <= 0 || rseq <= 0)
        return NSBC_INVAL;
    if (!r->awaiting_prack)
        return NSBC_NOENT;
    r->awaiting_prack = 0;
    return NSBC_OK;
}

int upd_can_send(const upd_leg_t *u)
{
    if (u == NULL || !u->peer_supports_update || u->update_in_progress)
        return 0;
    return 1;
}

int upd_on_request(upd_leg_t *u, const sip_msg_t *msg)
{
    /* TODO(P0 接线 REQ-SIP-010)：early 仅允许无 SDP 或 precondition SDP；
     * glare 经 guard sip_upd_begin→491 + 退避；2xx 不带 offer（RFC3311）。 */
    if (u == NULL || msg == NULL)
        return NSBC_INVAL;
    return NSBC_NOTSUP;
}
