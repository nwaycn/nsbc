/* SIP 过载控制——粗骨架（REQ-SIP-015，P1）
 *
 * P0 不接线；P1 接线点：transport 收包分类后、事务层创建前调 ovl_admit；
 * 定时器（时间轮）每窗口喂 CPU/计数并 ovl_render_hdr 到响应；
 * oc 头当前在 sip 解析器无枚举槽，按自定义头原样透传读取（P1 补解析）。
 */
#include "sip_overload.h"

struct sip_ovl {
    ovl_cfg_t cfg;
    int       overloaded;
    int       reduction;      /* 当前削减百分比 */
    uint32_t  oc_seq;         /* RFC7683 §4：每次变化递增 */
    uint32_t  win_admits;     /* 当前窗口放行计数（rate 算法） */
    uint64_t  win_start_ms;
};

void ovl_cfg_default(ovl_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->algo = OVL_ALGO_CPU;
    cfg->reduction_init = 10;
    cfg->reduction_max = 90;
    cfg->cpu_hi = 85;
    cfg->cpu_lo = 70;
    cfg->cps_limit = 0;
    cfg->window_ms = 500;
    cfg->retry_after_s = OVL_RETRY_AFTER_DEF;
}

int ovl_create(const ovl_cfg_t *cfg, sip_ovl_t **out)
{
    sip_ovl_t *o;
    if (cfg == NULL || out == NULL)
        return NSBC_INVAL;
    o = (sip_ovl_t *)calloc(1, sizeof(*o));
    if (o == NULL)
        return NSBC_NOMEM;
    o->cfg = *cfg;
    o->reduction = 0;
    *out = o;
    return NSBC_OK;
}

void ovl_destroy(sip_ovl_t *ov)
{
    free(ov);
}

int ovl_admit(sip_ovl_t *ov, ovl_prio_t prio, uint32_t rnd_percent)
{
    /* CRITICAL（CANCEL/BYE/ACK/重传）任何负载下都放行 */
    if (ov == NULL)
        return NSBC_INVAL;
    if (prio == OVL_PRIO_CRITICAL || !ov->overloaded)
        return NSBC_OK;
    /* 优先级越低越早削减：PROBE/NEW/DIALOG 各加 10% 宽容度阶梯 */
    if ((int)rnd_percent < ov->reduction - (int)prio * 10)
        return NSBC_AGAIN;
    return NSBC_OK;
}

void ovl_note_admit(sip_ovl_t *ov, uint64_t now_ms)
{
    if (ov == NULL)
        return;
    if (ov->win_start_ms == 0 ||
        now_ms - ov->win_start_ms >= (uint64_t)ov->cfg.window_ms) {
        ov->win_start_ms = now_ms;
        ov->win_admits = 0;
    }
    ++ov->win_admits;
}

void ovl_update_measure(sip_ovl_t *ov, int cpu_pct, int peer_reduction,
                        uint64_t now_ms)
{
    int old;
    if (ov == NULL)
        return;
    (void)now_ms;
    old = ov->reduction;
    if (ov->cfg.algo == OVL_ALGO_CPU) {
        if (!ov->overloaded && cpu_pct >= ov->cfg.cpu_hi) {
            ov->overloaded = 1;
            ov->reduction = ov->cfg.reduction_init;
        } else if (ov->overloaded && cpu_pct <= ov->cfg.cpu_lo) {
            ov->overloaded = 0;
            ov->reduction = 0;
        } else if (ov->overloaded) {
            ov->reduction += 10; /* 粗骨架固定步长，P1 按反馈环调参 */
            if (ov->reduction > ov->cfg.reduction_max)
                ov->reduction = ov->cfg.reduction_max;
        }
    } else if (ov->cfg.algo == OVL_ALGO_LOSS) {
        ov->overloaded = peer_reduction > 0;
        ov->reduction = peer_reduction;
    }
    /* TODO(P1)：RATE 算法按 win_admits/window 换算 CPS 闸门 */
    if (old != ov->reduction)
        ++ov->oc_seq;
}

int ovl_render_hdr(sip_ovl_t *ov, char *buf, size_t cap, size_t *out_len)
{
    const char *algo;
    int n;
    if (ov == NULL || buf == NULL || cap == 0)
        return NSBC_INVAL;
    if (out_len != NULL)
        *out_len = 0;
    if (!ov->overloaded || ov->reduction <= 0)
        return NSBC_OK;
    switch (ov->cfg.algo) {
    case OVL_ALGO_LOSS: algo = "loss"; break;
    case OVL_ALGO_RATE: algo = "rate"; break;
    case OVL_ALGO_CPU:  algo = "cpu"; break;
    default: return NSBC_OK;
    }
    n = snprintf(buf, cap, "oc: %s=%d;oc-validity=%d;oc-seq=%u\r\n",
                 algo, ov->reduction,
                 ov->cfg.window_ms / 1000, ov->oc_seq);
    if (n < 0 || (size_t)n >= cap)
        return NSBC_NOSPC;
    if (out_len != NULL)
        *out_len = (size_t)n;
    return NSBC_OK;
}

void ovl_on_peer_hdr(sip_ovl_t *ov, const char *algo, size_t algo_len, int reduction, int validity_s, uint32_t seq)
{
    /* TODO(P1)：algo 必须与本地配置一致（RFC7683 §5），
     * oc-seq 回退/重复时忽略；当前仅留存参数，不做协商。 */
    if (ov == NULL || algo == NULL || algo_len == 0)
        return;
    (void)reduction;
    (void)validity_s;
    (void)seq;
}

int ovl_overloaded(const sip_ovl_t *ov)
{
    return (ov != NULL) ? ov->overloaded : 0;
}

int ovl_retry_after(const sip_ovl_t *ov)
{
    return (ov != NULL) ? ov->cfg.retry_after_s : OVL_RETRY_AFTER_DEF;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
