/* 呼叫业务——粗骨架（REQ-PBX-SS-001~006，P0 转移/前转/驻留代答）
 *
 * 装配：applications 模块 init 建 ccf_mgr，ops 由 signaling 注入
 * （send_refer 用 b2bua+ sip_refer 原语；originate 用 b2b_originate；
 * terminate 用 b2b_terminate；announce 用 media/play）。
 * 规则表按租户/分机分片；呼叫键只作表值，绝不解引用。
 */
#include "callctrl.h"

#define CCF_FWD_CAP     200000
#define CCF_PARK_CAP    4096

typedef struct {
    char tenant[32];
    char ext[32];
    ccf_fwd_rule_t rule[4]; /* ccf_fwd_kind_t 全枚举 */
} ccf_sub_t;

typedef struct {
    char orbit[CCF_ORBIT_MAX];
    void *call_key;
    uint64_t parked_ms;
} ccf_park_slot_t;

struct ccf_mgr {
    ccf_ops_t ops;
    ccf_sub_t subs[CCF_FWD_CAP]; /* TODO：换分片哈希，粗骨架定槽 */
    int       n_subs;
    ccf_park_slot_t park[CCF_PARK_CAP];
    int       n_park;
    unsigned  orbit_seq;
};

int ccf_create(const ccf_ops_t *ops, ccf_mgr_t **out)
{
    ccf_mgr_t *m;
    if (ops == NULL || out == NULL || ops->send_refer == NULL ||
        ops->originate == NULL || ops->terminate == NULL)
        return NSBC_INVAL;
    m = (ccf_mgr_t *)calloc(1, sizeof(*m));
    if (m == NULL)
        return NSBC_NOMEM;
    m->ops = *ops;
    m->orbit_seq = 0;
    *out = m;
    return NSBC_OK;
}

void ccf_destroy(ccf_mgr_t *m)
{
    free(m);
}

static ccf_sub_t *sub_find(ccf_mgr_t *m, const char *tenant, const char *ext)
{
    int i;
    for (i = 0; i < m->n_subs; ++i) {
        if (strcmp(m->subs[i].tenant, tenant) == 0 &&
            strcmp(m->subs[i].ext, ext) == 0)
            return &m->subs[i];
    }
    return NULL;
}

static ccf_sub_t *sub_get(ccf_mgr_t *m, const char *tenant, const char *ext)
{
    ccf_sub_t *s = sub_find(m, tenant, ext);
    if (s != NULL)
        return s;
    if (m->n_subs >= CCF_FWD_CAP)
        return NULL;
    s = &m->subs[m->n_subs++];
    snprintf(s->tenant, sizeof(s->tenant), "%s", tenant);
    snprintf(s->ext, sizeof(s->ext), "%s", ext);
    return s;
}

int ccf_fwd_set(ccf_mgr_t *m, const char *tenant, const char *ext,
                ccf_fwd_kind_t kind, const ccf_fwd_rule_t *r)
{
    ccf_sub_t *s;
    if (m == NULL || tenant == NULL || ext == NULL || r == NULL ||
        kind < 0 || kind > CCF_FWD_UNAVAILABLE)
        return NSBC_INVAL;
    s = sub_get(m, tenant, ext);
    if (s == NULL)
        return NSBC_NOSPC;
    s->rule[kind] = *r;
    return NSBC_OK;
}

int ccf_fwd_get(ccf_mgr_t *m, const char *tenant, const char *ext,
                ccf_fwd_kind_t kind, ccf_fwd_rule_t *out)
{
    ccf_sub_t *s;
    if (out == NULL || kind < 0 || kind > CCF_FWD_UNAVAILABLE)
        return NSBC_INVAL;
    s = sub_find(m, tenant, ext);
    if (s == NULL || !s->rule[kind].enabled)
        return NSBC_NOENT;
    *out = s->rule[kind];
    return NSBC_OK;
}

int ccf_fwd_clear(ccf_mgr_t *m, const char *tenant, const char *ext,
                  ccf_fwd_kind_t kind)
{
    ccf_sub_t *s;
    if (kind < 0 || kind > CCF_FWD_UNAVAILABLE)
        return NSBC_INVAL;
    s = sub_find(m, tenant, ext);
    if (s == NULL)
        return NSBC_NOENT;
    memset(&s->rule[kind], 0, sizeof(s->rule[kind]));
    return NSBC_OK;
}

int ccf_fwd_resolve(ccf_mgr_t *m, const char *tenant, const char *ext,
                    int busy, int noanswer, int unavailable,
                    ccf_fwd_rule_t *out)
{
    ccf_sub_t *s;
    ccf_fwd_kind_t order[4];
    int i, n = 0;
    if (m == NULL || tenant == NULL || ext == NULL || out == NULL)
        return NSBC_INVAL;
    s = sub_find(m, tenant, ext);
    if (s == NULL)
        return NSBC_NOENT;
    /* CFU 最高优先；其次条件链 CFNRc→CFB→CFNR（按部署可配，粗骨架固定） */
    if (s->rule[CCF_FWD_UNCONDITIONAL].enabled)
        order[n++] = CCF_FWD_UNCONDITIONAL;
    if (unavailable && s->rule[CCF_FWD_UNAVAILABLE].enabled)
        order[n++] = CCF_FWD_UNAVAILABLE;
    if (busy && s->rule[CCF_FWD_BUSY].enabled)
        order[n++] = CCF_FWD_BUSY;
    if (noanswer && s->rule[CCF_FWD_NOANSWER].enabled)
        order[n++] = CCF_FWD_NOANSWER;
    for (i = 0; i < n; ++i) {
        if (s->rule[order[i]].target[0] != '\0') {
            *out = s->rule[order[i]];
            return NSBC_OK;
        }
    }
    return NSBC_NOENT;
}

int ccf_transfer(ccf_mgr_t *m, void *call_key, const char *target,
                 ccf_xfer_kind_t kind, void *consult_key)
{
    if (m == NULL || call_key == NULL || target == NULL)
        return NSBC_INVAL;
    if (kind == CCF_XFER_CONSULTED && consult_key == NULL)
        return NSBC_INVAL;
    return m->ops.send_refer(m->ops.ud, call_key, target, kind,
                             consult_key);
}

int ccf_park(ccf_mgr_t *m, void *call_key, char *orbit_out, size_t cap)
{
    ccf_park_slot_t *s;
    int n;
    if (m == NULL || call_key == NULL || orbit_out == NULL || cap < 8)
        return NSBC_INVAL;
    if (m->n_park >= CCF_PARK_CAP)
        return NSBC_NOSPC;
    s = &m->park[m->n_park++];
    s->call_key = call_key;
    s->parked_ms = 0; /* TODO：nsbc_now_ms 注入（头不引 base 函数也行） */
    n = snprintf(s->orbit, sizeof(s->orbit), "*%02u",
                 m->orbit_seq++ % 100u);
    /* TODO：呼叫置 parked（媒体播 MoH），orbit 超时自动回叫原驻留者 */
    if ((size_t)n >= cap)
        return NSBC_NOSPC;
    memcpy(orbit_out, s->orbit, (size_t)n + 1);
    return NSBC_OK;
}

int ccf_park_retrieve(ccf_mgr_t *m, const char *orbit,
                      const char *target_ext, void **call_key_out)
{
    int i;
    if (m == NULL || orbit == NULL || call_key_out == NULL)
        return NSBC_INVAL;
    (void)target_ext;
    for (i = 0; i < m->n_park; ++i) {
        if (strcmp(m->park[i].orbit, orbit) == 0) {
            *call_key_out = m->park[i].call_key;
            m->park[i] = m->park[--m->n_park];
            /* TODO：对 target_ext 发 INVITE（Replaces 接续原对话） */
            return NSBC_OK;
        }
    }
    return NSBC_NOENT;
}

int ccf_pickup(ccf_mgr_t *m, const char *grp, const char *ringing_ext, const char *target_ext, void **call_key_out)
{
    /* TODO(P0)：经注册 Location DB 查 grp 内振铃成员（presence 早期
     * 对话状态），ringing_ext 指定代接优先；对代接者发 INVITE+Replaces。
     * 粗骨架不持有振铃集（presence 批后回填）。 */
    if (m == NULL || target_ext == NULL || call_key_out == NULL)
        return NSBC_INVAL;
    (void)grp;
    (void)ringing_ext;
    *call_key_out = NULL;
    return NSBC_NOTSUP;
}

int ccf_followme_set(ccf_mgr_t *m, const char *tenant, const char *ext,
                     const char *const *targets, int n, int timeout_s)
{
    /* TODO(P1)：sub 内增 targets 定槽（≤4），振铃策略同 ringgroup */
    (void)m;
    (void)tenant;
    (void)ext;
    (void)targets;
    (void)n;
    (void)timeout_s;
    return NSBC_NOTSUP;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
