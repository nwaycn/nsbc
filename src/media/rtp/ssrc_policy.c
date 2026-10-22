#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ssrc_policy.h"

#include <pthread.h>
#include <string.h>

/* ★ RACE-FIX helper：所有公开函数入口持锁，内部 helper (find/alloc_entry/is_aux_pt)
 * 假设调用者已持锁（relay_cb 单线程内串行调用，admit 从 SIP 线程调持锁保护并发）。 */
#define SSRC_LOCK(p)   pthread_mutex_lock(&(p)->mu)
#define SSRC_UNLOCK(p) pthread_mutex_unlock(&(p)->mu)

void ssrc_policy_init(ssrc_policy_t *p, const ssrc_policy_cfg_t *cfg)
{
    memset(p, 0, sizeof(*p));
    pthread_mutex_init(&p->mu, NULL);
    if (cfg)
        p->cfg = *cfg;
    if (p->cfg.switch_idle_ms == 0)
        p->cfg.switch_idle_ms = 5000;
    if (p->cfg.max_sources <= 0 ||
        p->cfg.max_sources > SSRC_POLICY_MAX_SRC)
        p->cfg.max_sources = SSRC_POLICY_MAX_SRC;
}

static int is_aux_pt(const ssrc_policy_t *p, uint8_t pt)
{
    for (int i = 0; i < p->cfg.n_aux_pt; ++i)
        if (p->cfg.aux_pt[i] == pt)
            return 1;
    return 0;
}

static src_entry_t *find(ssrc_policy_t *p, uint32_t ssrc)
{
    for (int i = 0; i < p->n; ++i)
        if (p->src[i].used && p->src[i].ssrc == ssrc)
            return &p->src[i];
    return NULL;
}

static src_entry_t *alloc_entry(ssrc_policy_t *p, uint32_t ssrc,
                                uint64_t now)
{
    if (p->n >= p->cfg.max_sources) {
        /* 回收最久未用的非主源 */
        src_entry_t *victim = NULL;
        for (int i = 0; i < p->n; ++i) {
            if (p->src[i].used && !p->src[i].primary &&
                (!victim || p->src[i].last_ms < victim->last_ms))
                victim = &p->src[i];
        }
        if (!victim)
            return NULL;
        memset(victim, 0, sizeof(*victim));
        victim->used = 1;
        victim->ssrc = ssrc;
        victim->last_ms = now;
        return victim;
    }
    src_entry_t *e = &p->src[p->n++];
    e->used = 1;
    e->ssrc = ssrc;
    e->last_ms = now;
    return e;
}

void ssrc_policy_admit(ssrc_policy_t *p, uint32_t ssrc, int aux)
{
    src_entry_t *e = find(p, ssrc);
    if (!e)
        e = alloc_entry(p, ssrc, 0);
    if (e) {
        e->aux = aux;
        /* REQ-MEDIA-SSRC-003：预登记的非 aux SSRC = 合法主源，直接标 primary。
         * 之前只设 aux 不设 primary → ssrc_policy_on_rtp 判定时
         * "found entry + !primary + !aux" 还是 SRC_DEC_DROP，admit 白加。
         * WebRTC unified-plan 每个 m-line 各一个 primary（audio + video），
         * 多 primary 同时存在是合法的。 */
        if (!aux) {
            e->primary = 1;
            /* 同时置 learned_primary=1，挡住未登记的新 SSRC 被"先到先学"放行。
             * 攻击者随机 SSRC 会命中 inject_drops++ 路径而非新 primary。 */
            p->learned_primary = 1;
        }
    }
}

void ssrc_policy_set_aux_pt(ssrc_policy_t *p, uint8_t pt)
{
    if (p->cfg.n_aux_pt < 8)
        p->cfg.aux_pt[p->cfg.n_aux_pt++] = pt;
}

void ssrc_policy_retarget(ssrc_policy_t *p)
{
    /* 主源标记清除但保留 CNAME：CNAME 相同的源直接恢复；其余重新学习 */
    p->learned_primary = 0;
    for (int i = 0; i < p->n; ++i) {
        if (p->src[i].used && p->src[i].primary)
            p->src[i].primary = 0;
    }
}

void ssrc_policy_set_local(ssrc_policy_t *p, uint32_t local_ssrc)
{
    p->cfg.local_ssrc = local_ssrc;
}

src_decision_t ssrc_policy_on_rtp(ssrc_policy_t *p, uint32_t ssrc,
                                  uint8_t pt, uint64_t now_ms)
{
    /* RFC3550 §8.2 collision */
    if (ssrc == p->cfg.local_ssrc && ssrc != 0) {
        ++p->collision_drops;
        return SRC_DEC_DROP;
    }

    src_entry_t *e = find(p, ssrc);
    int aux_pt = is_aux_pt(p, pt);

    if (e) {
        e->pkts++;
        e->last_ms = now_ms;
        if (aux_pt)
            e->aux = 1;
        return e->primary ? SRC_DEC_PRIMARY
                          : (e->aux ? SRC_DEC_AUX : SRC_DEC_DROP);
    }

    /* 全新 SSRC */
    if (aux_pt) {
        e = alloc_entry(p, ssrc, now_ms);
        if (e) {
            e->aux = 1;
            return SRC_DEC_AUX;
        }
        ++p->inject_drops;
        return SRC_DEC_DROP;
    }

    if (!p->learned_primary) {
        e = alloc_entry(p, ssrc, now_ms);
        if (!e) {
            ++p->inject_drops;
            return SRC_DEC_DROP;
        }
        e->primary = 1;
        p->learned_primary = 1;
        return SRC_DEC_PRIMARY;
    }

    /* 发言切换：现主源静默超过阈值才接纳 */
    src_entry_t *cur = NULL;
    for (int i = 0; i < p->n; ++i)
        if (p->src[i].used && p->src[i].primary)
            cur = &p->src[i];
    if (cur && now_ms - cur->last_ms >= p->cfg.switch_idle_ms) {
        cur->primary = 0;
        e = alloc_entry(p, ssrc, now_ms);
        if (!e) {
            ++p->inject_drops;
            return SRC_DEC_DROP;
        }
        e->primary = 1;
        ++p->switches;
        return SRC_DEC_NEW_PRIMARY;
    }

    ++p->inject_drops;
    return SRC_DEC_DROP;
}

int ssrc_policy_on_cname(ssrc_policy_t *p, uint32_t ssrc, const char *cname,
                         size_t clen, uint64_t now_ms)
{
    if (clen >= SSRC_CNAME_MAX)
        clen = SSRC_CNAME_MAX - 1;
    src_entry_t *e = find(p, ssrc);
    if (!e) {
        /* RTCP 先到：预登记，等 RTP 学习时核对 */
        e = alloc_entry(p, ssrc, now_ms);
        if (!e)
            return 0;
    }
    if (e->cname_set && (strlen(e->cname) != clen ||
                         memcmp(e->cname, cname, clen))) {
        ++p->cname_drops;
        return 0;
    }
    if (!e->cname_set) {
        memcpy(e->cname, cname, clen);
        e->cname[clen] = '\0';
        e->cname_set = 1;
    }
    return 1;
}

uint64_t ssrc_policy_inject_drops(const ssrc_policy_t *p)
{
    /* 媒体线程 ++ 递增（plain，单线程 per pol 实例）；OAM 低频读经此
     * getter 统一用 __atomic_load_n，防御 TSan/弱内存模型（ARM）。 */
    return __atomic_load_n(&p->inject_drops, __ATOMIC_ACQUIRE);
}
uint64_t ssrc_policy_collision_drops(const ssrc_policy_t *p)
{
    return __atomic_load_n(&p->collision_drops, __ATOMIC_ACQUIRE);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
