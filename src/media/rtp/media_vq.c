/* 语音质量采集——粗骨架（REQ-VQ-001，P1）
 *
 * 接线：media_bridge 每 Leg 持一个 vq_collector；rtcp.c 解析结果
 * 从当前零消费改为喂入 vq_on_rr/vq_on_xr；uuid_dump 的 vq 字段
 * （W4 恒 UNAVAILABLE）改取 vq_report 文本（mos/r_factor/loss/jitter）。
 * MOS/R 用 ITU-T G.107 E-model 简化式（Ie+Id 随 codec 表配置）。
 */
#include "media_vq.h"

struct vq_collector {
    vq_cfg_t    cfg;
    vq_report_t cum;
    uint16_t    last_seq;
    int         primed;
    uint32_t    plc;
    uint64_t    last_lsr;
};

int vq_create(const vq_cfg_t *cfg, vq_collector_t **out)
{
    vq_collector_t *v;
    if (cfg == NULL || out == NULL)
        return NSBC_INVAL;
    v = (vq_collector_t *)calloc(1, sizeof(*v));
    if (v == NULL)
        return NSBC_NOMEM;
    v->cfg = *cfg;
    if (v->cfg.window_ms <= 0)
        v->cfg.window_ms = 5000;
    *out = v;
    return NSBC_OK;
}

void vq_destroy(vq_collector_t *v)
{
    free(v);
}

void vq_on_rtp(vq_collector_t *v, uint16_t seq, uint32_t ts,
               uint64_t recv_ms, int marker)
{
    int16_t diff;
    if (v == NULL)
        return;
    (void)ts;
    (void)marker;
    if (!v->primed) {
        v->last_seq = seq;
        v->primed = 1;
        v->cum.packets_expected = 1;
        return;
    }
    diff = (int16_t)(seq - v->last_seq);
    if (diff > 0) {
        v->cum.packets_expected += (uint32_t)diff;
        if (diff > 1)
            v->cum.packets_lost += (uint32_t)(diff - 1);
        v->last_seq = seq;
    }
    /* TODO(P1)：到达间隔 EWMA→jitter_ms；重复/乱序包归位不重复计损 */
    (void)recv_ms;
}

void vq_on_rr(vq_collector_t *v, const rtcp_rb_t *rb, uint64_t now_ms)
{
    if (v == NULL || rb == NULL)
        return;
    /* TODO：fraction→窗损失率，jitter 字段（时钟单位换算 ms），
     * LSR/DLSR RTT=now-(LSR+DLSR)（RFC3550 A.8）。 */
    v->last_lsr = now_ms;
}

void vq_on_xr(vq_collector_t *v, const rtcp_xr_block_t *b)
{
    if (v == NULL || b == NULL)
        return;
    /* TODO(P1)：bt=5 DLRR（RFC3611 §4.5）单向时延；
     * bt=7/13 信号/语音质量 MOS-LR 直接采纳（对端支持时）。 */
}

void vq_on_discard(vq_collector_t *v, uint32_t n)
{
    if (v != NULL)
        v->cum.discards += n;
}

void vq_on_plc(vq_collector_t *v, uint32_t n)
{
    if (v != NULL)
        v->plc += n;
}

void vq_report(const vq_collector_t *v, vq_report_t *out)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (v == NULL)
        return;
    *out = v->cum;
    /* TODO(P1)：loss_pct_milli 与 E-model R/MOS 计算回填 */
}

void vq_reset(vq_collector_t *v)
{
    if (v == NULL)
        return;
    memset(&v->cum, 0, sizeof(v->cum));
    v->plc = 0;
    v->primed = 0;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
