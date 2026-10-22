/* 防盗打——粗骨架（REQ-FRAUD，P0 规则 / P1 异常）
 *
 * 实现简化（接线批细化，勿声称完整）：
 *  - 规则按账号键定槽线性扫描，规则链顺序：ALLOW 白名单未命中即拒，
 *    DENY 黑名单命中即拒，再判并发/CPS/消费/时段；
 *  - 窗口为"自首呼起 1h 固定窗"，非滑动窗（按 win_start 滚动清零）；
 *  - P1 的 ASR/短通话只在 call_end 累积，自动封禁动作未实装（转人工
 *    审计/事件上报），仅做阈值置位。
 */
#include "fraud.h"

typedef struct {
    int used;
    char key[FRAUD_KEY_MAX];
    fraud_rule_t rule;
} rule_slot_t;

typedef struct {
    int used;
    fraud_counters_t c;
} subj_slot_t;

struct fraud_guard {
    rule_slot_t rules[FRAUD_RULE_CAP];
    int n_rules;
    subj_slot_t subs[FRAUD_SUBJ_CAP];
    int n_subs;
};

int fraud_create(fraud_guard_t **out)
{
    if (out == NULL)
        return NSBC_INVAL;
    *out = (fraud_guard_t *)calloc(1, sizeof(fraud_guard_t));
    return *out != NULL ? NSBC_OK : NSBC_NOMEM;
}

void fraud_destroy(fraud_guard_t *g)
{
    free(g);
}

static subj_slot_t *subj_get(fraud_guard_t *g, const char *key)
{
    int i;
    for (i = 0; i < g->n_subs; ++i) {
        if (g->subs[i].used && strcmp(g->subs[i].c.key, key) == 0)
            return &g->subs[i];
    }
    if (g->n_subs >= FRAUD_SUBJ_CAP)
        return NULL;
    {
        subj_slot_t *s = &g->subs[g->n_subs++];
        s->used = 1;
        snprintf(s->c.key, sizeof(s->c.key), "%s", key);
        return s;
    }
}

int fraud_rule_add(fraud_guard_t *g, const char *key,
                   const fraud_rule_t *r)
{
    int i;
    if (g == NULL || key == NULL || r == NULL)
        return NSBC_INVAL;
    for (i = 0; i < g->n_rules; ++i) {
        if (g->rules[i].used && strcmp(g->rules[i].key, key) == 0 &&
            g->rules[i].rule.kind == r->kind &&
            strcmp(g->rules[i].rule.prefix, r->prefix) == 0) {
            g->rules[i].rule = *r;
            return NSBC_OK;
        }
    }
    if (g->n_rules >= FRAUD_RULE_CAP)
        return NSBC_NOSPC;
    {
        rule_slot_t *s = &g->rules[g->n_rules++];
        s->used = 1;
        snprintf(s->key, sizeof(s->key), "%s", key);
        s->rule = *r;
    }
    return NSBC_OK;
}

int fraud_rule_remove(fraud_guard_t *g, const char *key,
                      fraud_rule_kind_t kind, const char *prefix)
{
    int i;
    if (g == NULL || key == NULL || prefix == NULL)
        return NSBC_INVAL;
    for (i = 0; i < g->n_rules; ++i) {
        if (g->rules[i].used && strcmp(g->rules[i].key, key) == 0 &&
            g->rules[i].rule.kind == kind &&
            strcmp(g->rules[i].rule.prefix, prefix) == 0) {
            g->rules[i] = g->rules[--g->n_rules];
            return NSBC_OK;
        }
    }
    return NSBC_NOENT;
}

static int prefix_hit(const char *prefix, const char *called)
{
    size_t n = strlen(prefix);
    return strlen(called) >= n && memcmp(prefix, called, n) == 0;
}

static void roll_window(fraud_counters_t *c, uint64_t now_ms)
{
    if (c->win_start_s == 0) {
        c->win_start_s = (uint32_t)(now_ms / 1000u);
        return;
    }
    if ((uint32_t)(now_ms / 1000u) - c->win_start_s >=
        FRAUD_WIN_MS / 1000u) {
        c->calls_in_win = 0;
        c->answered_in_win = 0;
        c->short_in_win = 0;
        c->win_start_s = (uint32_t)(now_ms / 1000u);
    }
}

int fraud_check(fraud_guard_t *g, const char *key, const char *called,
                uint64_t now_ms, uint32_t now_s, int *hit_action)
{
    subj_slot_t *s;
    int allow_seen = 0;
    int allow_hit = 0;
    int denied = 0;
    int i;
    if (g == NULL || key == NULL || called == NULL)
        return NSBC_INVAL;
    s = subj_get(g, key);
    if (s == NULL)
        return NSBC_NOSPC;
    roll_window(&s->c, now_ms);
    for (i = 0; i < g->n_rules; ++i) {
        const fraud_rule_t *r;
        if (!g->rules[i].used || strcmp(g->rules[i].key, key) != 0)
            continue;
        r = &g->rules[i].rule;
        if (!r->enabled)
            continue;
        switch (r->kind) {
        case FRAUD_R_PREFIX_ALLOW:
            allow_seen = 1;
            if (prefix_hit(r->prefix, called))
                allow_hit = 1;
            break;
        case FRAUD_R_PREFIX_DENY:
            if (prefix_hit(r->prefix, called))
                denied = 1;
            break;
        case FRAUD_R_MAX_CONCURRENT:
            if (s->c.concurrent >= (int)r->num)
                denied = 1;
            break;
        case FRAUD_R_MAX_CPS:
            /* 粗骨架：固定窗起呼数（非严格每秒）；接线批改秒桶 */
            if (s->c.calls_in_win >= (int)r->num * 3600)
                denied = 1;
            break;
        case FRAUD_R_DAILY_SPEND:
            if (s->c.spend_today_uc >= r->num)
                denied = 1;
            break;
        case FRAUD_R_TIME_BAN:
            if ((r->num & (1LL << (now_s % 86400u / 3600u))) != 0)
                denied = 1;
            break;
        case FRAUD_R_ASR_DROP:
        case FRAUD_R_SHORT_BURST:
            break; /* P1：仅累积，不阻断起呼 */
        }
    }
    if (allow_seen && !allow_hit)
        denied = 1;
    if (hit_action != NULL)
        *hit_action = denied ? FRAUD_ACT_DENY : FRAUD_ACT_ALLOW;
    return denied ? NSBC_PERM_DENIED : NSBC_OK;
}

void fraud_call_begin(fraud_guard_t *g, const char *key, uint64_t now_ms)
{
    subj_slot_t *s;
    if (g == NULL || key == NULL)
        return;
    s = subj_get(g, key);
    if (s == NULL)
        return;
    roll_window(&s->c, now_ms);
    ++s->c.concurrent;
    ++s->c.calls_in_win;
}

void fraud_call_end(fraud_guard_t *g, const char *key, int answered,
                    uint32_t billsec, int64_t spend_uc,
                    uint64_t now_ms, uint32_t today_epoch)
{
    subj_slot_t *s;
    if (g == NULL || key == NULL)
        return;
    s = subj_get(g, key);
    if (s == NULL)
        return;
    if (s->c.day_epoch != today_epoch) {
        s->c.day_epoch = today_epoch;
        s->c.spend_today_uc = 0;
    }
    if (s->c.concurrent > 0)
        --s->c.concurrent;
    if (answered)
        ++s->c.answered_in_win;
    if (answered && billsec < 10)
        ++s->c.short_in_win;
    s->c.spend_today_uc += spend_uc;
    (void)now_ms;
    /* TODO(P1)：ASR/短通话阈值命中→事件上报+自动 WATCH/DENY */
}

int fraud_counters(fraud_guard_t *g, const char *key,
                   fraud_counters_t *out)
{
    int i;
    if (g == NULL || key == NULL || out == NULL)
        return NSBC_INVAL;
    for (i = 0; i < g->n_subs; ++i) {
        if (g->subs[i].used && strcmp(g->subs[i].c.key, key) == 0) {
            *out = g->subs[i].c;
            return NSBC_OK;
        }
    }
    return NSBC_NOENT;
}

const char *fraud_action_name(fraud_action_t a)
{
    switch (a) {
    case FRAUD_ACT_ALLOW: return "allow";
    case FRAUD_ACT_DENY:  return "deny";
    case FRAUD_ACT_WATCH: return "watch";
    }
    return "unknown";
}
