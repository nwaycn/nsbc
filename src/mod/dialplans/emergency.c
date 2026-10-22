/* 紧急呼叫——粗骨架（REQ-PBX-DP-006，P0 基础）
 *
 * 接线：signaling 入呼路由在 dp_execute 之前先 emg_is_emergency，
 * 命中则跳过 authz PIN/计费预检/前转链，用 emg_resolve 的中继组与
 * ELIN 直发；CDR 标 emergency 位（救援对账要求，隐私策略下仍留存）。
 * P1 多来源：话机 LLDP/CDP 位置经 UA 上报/探测写入 zone 反查 ELIN 池。
 */
#include "emergency.h"

struct emg_table {
    emg_route_t routes[EMG_TABLE_MAX];
    int n;
};

int emg_create(emg_table_t **out)
{
    emg_table_t *t;
    if (out == NULL)
        return NSBC_INVAL;
    t = (emg_table_t *)calloc(1, sizeof(*t));
    if (t == NULL)
        return NSBC_NOMEM;
    *out = t;
    return NSBC_OK;
}

void emg_destroy(emg_table_t *t)
{
    free(t);
}

int emg_add(emg_table_t *t, const emg_route_t *r)
{
    if (t == NULL || r == NULL || r->pattern[0] == '\0')
        return NSBC_INVAL;
    if (t->n >= EMG_TABLE_MAX)
        return NSBC_NOSPC;
    t->routes[t->n++] = *r;
    return NSBC_OK;
}

static int pattern_match(const emg_route_t *r, const char *called)
{
    size_t plen = strlen(r->pattern);
    size_t clen = strlen(called);
    if (plen == 0 || clen == 0)
        return 0;
    /* 末位 '*' → 前缀匹配；否则全号精确 */
    if (r->pattern[plen - 1] == '*')
        return clen >= plen - 1 &&
               memcmp(r->pattern, called, plen - 1) == 0;
    return plen == clen && memcmp(r->pattern, called, plen) == 0;
}

int emg_is_emergency(emg_table_t *t, const char *called)
{
    int i;
    if (t == NULL || called == NULL)
        return 0;
    for (i = 0; i < t->n; ++i) {
        if (t->routes[i].enabled && pattern_match(&t->routes[i], called))
            return 1;
    }
    return 0;
}

int emg_resolve(emg_table_t *t, const char *called, const char *zone,
                emg_route_t *out)
{
    int i;
    int fallback = -1;
    if (t == NULL || called == NULL || out == NULL)
        return NSBC_INVAL;
    /* 两轮：先 zone 精确，再空 zone 全局兜底 */
    for (i = 0; i < t->n; ++i) {
        const emg_route_t *r = &t->routes[i];
        if (!r->enabled || !pattern_match(r, called))
            continue;
        if (zone != NULL && strcmp(r->zone, zone) == 0) {
            *out = *r;
            return NSBC_OK;
        }
        if (r->zone[0] == '\0' && fallback < 0)
            fallback = i;
    }
    if (fallback >= 0) {
        *out = t->routes[fallback];
        return NSBC_OK;
    }
    return NSBC_NOENT;
}

int emg_exempt_balance(const emg_route_t *r)
{
    (void)r;
    return 1; /* 紧急呼叫永远免余额（P0 固定策略，不做开关） */
}

int emg_exempt_pin(const emg_route_t *r)
{
    (void)r;
    return 1;
}

int emg_elin_lookup(emg_table_t *t, const char *called, const char *zone,
                    char *out, size_t cap)
{
    emg_route_t r;
    size_t n;
    int rc;
    if (out == NULL || cap == 0)
        return NSBC_INVAL;
    rc = emg_resolve(t, called, zone, &r);
    if (rc != NSBC_OK)
        return rc;
    if (r.elin[0] == '\0')
        return NSBC_NOENT;
    n = strlen(r.elin);
    if (n >= cap)
        return NSBC_NOSPC;
    memcpy(out, r.elin, n + 1);
    /* TODO(P1)：多来源（LLDP/CDP/子网/池）优先于静态表 */
    return NSBC_OK;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
