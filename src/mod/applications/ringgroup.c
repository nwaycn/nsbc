/* 振铃组——粗骨架（REQ-PBX-RG-001，P0）
 *
 * 接线：dialplan BIZ 层 target 指向振铃组业务号时由组合根调
 * rg_ring_targets，对每成员经 b2b 发 INVITE 分叉腿（同 Call-ID 不同
 * tag），首接 200 后取消其余腿（CANCEL）；超时按 fallback 转 IVR/VM。
 * 成员忙/DND/未注册在发起前用 extension+presence 过滤。
 */
#include "ringgroup.h"

#define RG_CAP 4096

struct rg_mgr {
    ringgroup_t groups[RG_CAP];
    int         n;
    uint64_t    last_answer_ts[RG_CAP][RG_MEMBER_MAX];
};

int rg_create(rg_mgr_t **out)
{
    if (out == NULL)
        return NSBC_INVAL;
    *out = (rg_mgr_t *)calloc(1, sizeof(rg_mgr_t));
    return *out != NULL ? NSBC_OK : NSBC_NOMEM;
}

void rg_destroy(rg_mgr_t *r)
{
    free(r);
}

static int rg_idx(const rg_mgr_t *r, const char *tenant, const char *name)
{
    int i;
    for (i = 0; i < r->n; ++i) {
        if (strcmp(r->groups[i].tenant, tenant) == 0 &&
            strcmp(r->groups[i].name, name) == 0)
            return i;
    }
    return -1;
}

int rg_add(rg_mgr_t *r, const ringgroup_t *g)
{
    int i;
    if (r == NULL || g == NULL || g->name[0] == '\0' ||
        g->nmembers <= 0 || g->nmembers > RG_MEMBER_MAX)
        return NSBC_INVAL;
    i = rg_idx(r, g->tenant, g->name);
    if (i >= 0) {
        r->groups[i] = *g;
        return NSBC_OK;
    }
    if (r->n >= RG_CAP)
        return NSBC_NOSPC;
    r->groups[r->n] = *g;
    ++r->n;
    return NSBC_OK;
}

int rg_remove(rg_mgr_t *r, const char *tenant, const char *name)
{
    int i = rg_idx(r, tenant, name);
    if (i < 0)
        return NSBC_NOENT;
    r->groups[i] = r->groups[--r->n];
    return NSBC_OK;
}

const ringgroup_t *rg_get(rg_mgr_t *r, const char *tenant, const char *name)
{
    int i;
    if (r == NULL || tenant == NULL || name == NULL)
        return NULL;
    i = rg_idx(r, tenant, name);
    return i < 0 ? NULL : &r->groups[i];
}

int rg_ring_targets(rg_mgr_t *r, const char *tenant, const char *name,
                    char out[][32], int out_cap, uint32_t rnd)
{
    const ringgroup_t *g;
    int i, n;
    if (r == NULL || out == NULL || out_cap <= 0)
        return 0;
    g = rg_get(r, tenant, name);
    if (g == NULL || !g->enabled)
        return 0;
    n = g->nmembers < out_cap ? g->nmembers : out_cap;
    switch (g->strategy) {
    case RG_STRATEGY_SIMUL:
    case RG_STRATEGY_SEQUENCE:
        for (i = 0; i < n; ++i)
            snprintf(out[i], 32, "%s", g->members[i]);
        /* TODO(SEQ)：以上次接听成员位为起点轮转（需记 last_pick） */
        break;
    case RG_STRATEGY_RANDOM: {
        int idx[RG_MEMBER_MAX];
        for (i = 0; i < n; ++i)
            idx[i] = i;
        for (i = n - 1; i > 0; --i) {
            int j = (int)(rnd % (uint32_t)(i + 1));
            int tmp = idx[i];
            idx[i] = idx[j];
            idx[j] = tmp;
        }
        for (i = 0; i < n; ++i)
            snprintf(out[i], 32, "%s", g->members[idx[i]]);
        break;
    }
    case RG_STRATEGY_MEMORY:
        /* TODO(P1)：按 last_answer_ts 最旧优先排序 */
        for (i = 0; i < n; ++i)
            snprintf(out[i], 32, "%s", g->members[i]);
        break;
    }
    return n;
}

static int member_slot(const ringgroup_t *g, const char *member)
{
    int i;
    for (i = 0; i < g->nmembers; ++i) {
        if (strcmp(g->members[i], member) == 0)
            return i;
    }
    return -1;
}

void rg_note_answered(rg_mgr_t *r, const char *tenant, const char *name,
                      const char *member, uint64_t ts)
{
    int gi, mi;
    if (r == NULL || member == NULL)
        return;
    gi = rg_idx(r, tenant, name);
    if (gi < 0)
        return;
    mi = member_slot(&r->groups[gi], member);
    if (mi >= 0)
        r->last_answer_ts[gi][mi] = ts;
}

int rg_in_wrap(rg_mgr_t *r, const char *tenant, const char *name,
               const char *member, uint64_t now_ms)
{
    const ringgroup_t *g;
    int gi, mi;
    uint64_t last, window;
    if (r == NULL || member == NULL || now_ms == 0)
        return 0;
    gi = rg_idx(r, tenant, name);
    if (gi < 0)
        return 0;
    g = &r->groups[gi];
    if (g->wrap_time_s <= 0)
        return 0;
    mi = member_slot(g, member);
    if (mi < 0)
        return 0;
    last = r->last_answer_ts[gi][mi];
    if (last == 0)
        return 0;
    window = (uint64_t)g->wrap_time_s * 1000u;
    return now_ms - last < window;
}
