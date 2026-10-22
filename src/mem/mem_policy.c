/* mem_policy.c — 内存池水位与降级策略（7.1.2 / 7.1.5） */
#include "mem.h"

#define MEM_RATIO_NORMAL 0.70 /* 回落至此以下解除降级 */
#define MEM_RATIO_WARN 0.85

mem_level_t mem_level_for_ratio(double r)
{
    if (r >= MEM_RATIO_WARN)
        return MEM_LEVEL_CRIT;
    if (r >= MEM_RATIO_NORMAL)
        return MEM_LEVEL_WARN;
    return MEM_LEVEL_NORMAL;
}

void mem_policy_init(mem_policy_t *p, uint32_t enabled_features)
{
    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    p->enabled = enabled_features;
    p->active = enabled_features;
    p->degraded = 0;
    for (int i = 0; i < MEM_DOMAIN__COUNT; i++)
        p->level[i] = MEM_LEVEL_NORMAL;
}

mem_level_t mem_policy_observe(mem_policy_t *p, mem_domain_t d, double ratio)
{
    if (!p || d < 0 || d >= MEM_DOMAIN__COUNT)
        return MEM_LEVEL_NORMAL;

    mem_level_t lv = mem_level_for_ratio(ratio);
    p->level[d] = lv;

    /* 7.1.2：媒体/转码/录音域危急时，不直接拒绝新呼叫，而是先停止新
     * 呼叫录音、拒绝转码（强制透传），保证核心信令与基础通话。
     * 信令域有独立 Arena，这些域 CRIT 不会也不允许抢占信令内存。 */
    if (!p->degraded && lv == MEM_LEVEL_CRIT &&
        (d == MEM_DOMAIN_MEDIA || d == MEM_DOMAIN_TRANSCODE ||
         d == MEM_DOMAIN_RECORD)) {
        p->degraded = 1;
        p->active &= ~(uint32_t)(MEM_FEAT_RECORD | MEM_FEAT_TRANSCODE);
    }

    /* 恢复需水位回落到安全阈值（70%）以下才解除（滞后设计，防抖动）；
     * 已有呼叫保持原状，仅新呼叫恢复增强能力。 */
    if (p->degraded) {
        int still_crit = 0;
        for (int i = MEM_DOMAIN_MEDIA; i <= MEM_DOMAIN_RECORD; i++)
            if (p->level[i] == MEM_LEVEL_CRIT)
                still_crit = 1;
        if (!still_crit) {
            p->degraded = 0;
            p->active = p->enabled;
        }
    }

    return lv;
}

int mem_policy_allow(const mem_policy_t *p, uint32_t feat)
{
    return p && (p->active & feat) != 0;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
