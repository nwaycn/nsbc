/* REFER 原语——粗骨架
 * REQ-SIP-002（REFER 方法）/ REQ-PBX-SS-001（转接信令载体，P0）
 *
 * 业务（咨询转/盲转/Replaces 接续）在 core/signaling 组合根装配
 * mod/applications 策略；本文件仅解析/渲染/状态机，零业务决策。
 * b2bua REFER 分支当前 501，接线在 P0 通话业务批（callctrl）同步进行。
 */
#include "sip_refer.h"

int refer_parse(const sip_msg_t *msg, refer_req_t *out)
{
    sip_hdr_t *h;
    if (msg == NULL || out == NULL || msg->method != SIP_M_REFER)
        return NSBC_INVAL;
    memset(out, 0, sizeof(*out));
    h = sip_hdr_get(msg, SIP_H_REFER_TO);
    if (h == NULL || h->value.p == NULL || h->value.len == 0)
        return NSBC_PROTO; /* RFC3515：缺 Refer-To 回 400 */
    out->refer_to = h->value;
    h = sip_hdr_get(msg, SIP_H_REFERRED_BY);
    if (h != NULL)
        out->referred_by = h->value;
    /* TODO(P0 接线)：从 Refer-To URI 参数解 ?Replaces= 与 norefersub；
     * 当前不深解 URI，has_replaces 由独立 Replaces 头判定。 */
    h = sip_hdr_get(msg, SIP_H_REPLACES);
    if (h != NULL && h->value.p != NULL && h->value.len > 0) {
        if (refer_parse_replaces(&h->value, &out->replaces) != NSBC_OK)
            return NSBC_PROTO;
        out->has_replaces = 1;
    }
    return NSBC_OK;
}

int refer_parse_replaces(const sip_tok_t *param, refer_replaces_t *out)
{
    /* TODO(P0)：按 RFC3891 §3 拆 "cid;to-tag=t;from-tag=f;early-only"；
     * 粗骨架只校验非空，真正拆分在接线批补（复用 sip_params）。 */
    if (param == NULL || out == NULL || param->p == NULL ||
        param->len == 0)
        return NSBC_INVAL;
    memset(out, 0, sizeof(*out));
    out->callid = *param;
    return NSBC_NOTSUP;
}

int refer_sipfrag_render(int status, const char *reason,
                         char *buf, size_t cap, size_t *out_len)
{
    int n;
    if (buf == NULL || cap == 0 || status < 100 || status > 699)
        return NSBC_INVAL;
    n = snprintf(buf, cap, "SIP/2.0 %d %s\r\n\r\n", status,
                 reason != NULL ? reason : "");
    if (n < 0 || (size_t)n >= cap)
        return NSBC_NOSPC;
    if (out_len != NULL)
        *out_len = (size_t)n;
    return NSBC_OK;
}

int refer_state_next(refer_state_t cur, refer_state_t want)
{
    /* 合法迁移：RCVD→STARTED→ALERTING→CONNECTED/FAILED；任意态→FAILED */
    if (want == REFER_ST_FAILED)
        return NSBC_OK;
    switch (cur) {
    case REFER_ST_NONE:
        return want == REFER_ST_RCVD ? NSBC_OK : NSBC_INVAL;
    case REFER_ST_RCVD:
        return want == REFER_ST_STARTED ? NSBC_OK : NSBC_INVAL;
    case REFER_ST_STARTED:
        return (want == REFER_ST_ALERTING || want == REFER_ST_CONNECTED)
                   ? NSBC_OK : NSBC_INVAL;
    case REFER_ST_ALERTING:
        return want == REFER_ST_CONNECTED ? NSBC_OK : NSBC_INVAL;
    default:
        return NSBC_INVAL;
    }
}

int refer_replaces_match(const refer_replaces_t *r,
                         const char *callid, size_t cid_len,
                         const char *to_tag, size_t tt_len,
                         const char *from_tag, size_t ft_len)
{
    /* TODO(P0)：逐字段常量时间比对；粗骨架返回 NOTSUP，
     * 由呼叫业务批与 dlg_match_msg 接同一稳定键（Call-ID+双 tag）。 */
    if (r == NULL || callid == NULL || to_tag == NULL || from_tag == NULL)
        return NSBC_INVAL;
    (void)cid_len;
    (void)tt_len;
    (void)ft_len;
    return NSBC_NOTSUP;
}
