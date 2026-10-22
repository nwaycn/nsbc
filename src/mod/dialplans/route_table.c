/* 中继/中继组选路——粗骨架（REQ-PBX-ROUTE-002/003，P0）
 *
 * 接线：配置加载（P1 起从 dbal/配置）填充；signaling 出局分支在
 * dp_execute target 非分机/业务号时查 rt_select，取 trunk.host/proto
 * 交 b2b_originate（同时去除 originate 硬编码 UDP）。
 * 选路算法：priority 升序组内 weight 散列（主备语义靠 priority 分层，
 * 不引入额外依赖）；熔断状态走 __atomic 快读。
 */
#include "route_table.h"
#include "sip_profile.h"   /* sip_ip_in_cidr_list —— P2-14 层 2 CIDR 匹配 */

#include <arpa/inet.h>     /* inet_pton —— P2-14 层 3/4 host IP 比较 */
#include <strings.h>       /* strcasecmp */

#define RT_TRUNK_MAX 4096
#define RT_GROUP_CAP 1024

struct route_table {
    trunk_t        trunks[RT_TRUNK_MAX];
    int            n_trunks;
    trunk_group_t  groups[RT_GROUP_CAP];
    int            n_groups;
    int            fails[RT_TRUNK_MAX];
    int            open[RT_TRUNK_MAX];
    uint64_t       in_use[RT_TRUNK_MAX];
};

int rt_create(route_table_t **out)
{
    route_table_t *t;
    int i;
    if (out == NULL)
        return NSBC_INVAL;
    t = (route_table_t *)calloc(1, sizeof(*t));
    if (t == NULL)
        return NSBC_NOMEM;
    for (i = 0; i < RT_TRUNK_MAX; ++i)
        t->open[i] = 1;
    *out = t;
    return NSBC_OK;
}

void rt_destroy(route_table_t *t)
{
    free(t);
}

static int trunk_idx(const route_table_t *t, const char *name)
{
    int i;
    for (i = 0; i < t->n_trunks; ++i) {
        if (strcmp(t->trunks[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int group_idx(const route_table_t *t, const char *name)
{
    int i;
    for (i = 0; i < t->n_groups; ++i) {
        if (strcmp(t->groups[i].name, name) == 0)
            return i;
    }
    return -1;
}

int rt_trunk_add(route_table_t *t, const trunk_t *tr)
{
    int i;
    if (t == NULL || tr == NULL || tr->name[0] == '\0')
        return NSBC_INVAL;
    i = trunk_idx(t, tr->name);
    if (i >= 0) {
        t->trunks[i] = *tr;
        goto resolve_dns;
    }
    if (t->n_trunks >= RT_TRUNK_MAX)
        return NSBC_NOSPC;
    t->trunks[t->n_trunks] = *tr;
    t->open[t->n_trunks] = 1;
    i = t->n_trunks++;

resolve_dns:
    /* Advisor 2026-10-01l：配置加载时预解析 DNS → 缓存 cached_sa。
     * resolve_cb OUT 路径直接 memcpy 用，跳过 SIP 信令线程阻塞 getaddrinfo。
     * 解析失败（动态 DNS）→ cached_sa.len==0，resolve_cb 会 fallback 阻塞解析。 */
    nsbc_sa_from(&t->trunks[i].cached_sa, tr->host, tr->port, AF_UNSPEC);
    return NSBC_OK;
}

int rt_trunk_remove(route_table_t *t, const char *name)
{
    int i = trunk_idx(t, name);
    if (i < 0)
        return NSBC_NOENT;
    t->trunks[i] = t->trunks[t->n_trunks - 1];
    t->fails[i] = t->fails[t->n_trunks - 1];
    t->open[i] = t->open[t->n_trunks - 1];
    --t->n_trunks;
    return NSBC_OK;
}

const trunk_t *rt_trunk_get(route_table_t *t, const char *name)
{
    int i;
    if (t == NULL || name == NULL)
        return NULL;
    i = trunk_idx(t, name);
    return i < 0 ? NULL : &t->trunks[i];
}

/* ---------- 呼入 trunk 四层匹配 P2-14 层 2/3/4 ----------
 *
 * route_table 是 RCU 不可变（每次 reload 原子换指针），遍历只读无需锁。
 * src_ip 为空 → 无法按 IP 匹配 → 直接返回 NULL。 */
const trunk_t *rt_match_inbound_trunk(route_table_t *t, const char *src_ip)
{
    if (t == NULL || src_ip == NULL || !*src_ip) return NULL;

    const trunk_t *best = NULL;
    int best_prio = 0x7fffffff;

    /* IPv4 src_addr 转一次，复用于层 3/4 host IP 比较 */
    struct in_addr src_addr;
    int have_src_v4 = (inet_pton(AF_INET, src_ip, &src_addr) == 1);

    for (int i = 0; i < t->n_trunks; ++i) {
        const trunk_t *tr = &t->trunks[i];
        if (!tr->enabled) continue;

        /* 层 2：TRUNK_AUTH_OUT + peer_ip CIDR 匹配 */
        if (tr->kind == TRUNK_AUTH_OUT && tr->peer_ip[0]) {
            if (sip_ip_in_cidr_list(tr->peer_ip, src_ip) == 1) {
                if (!best || tr->priority < best_prio) {
                    best = tr;
                    best_prio = tr->priority;
                }
            }
            continue;  /* TRUNK_AUTH_OUT 只走层 2，不走层 3/4 */
        }

        /* 层 3：TRUNK_STATIC_PEER + host IP 匹配（端口忽略，专家 1.4：ITSP 源端口随机）
         * 层 4：回退——任意 trunk（非 REGISTER）host IP 匹配，多 trunk 取 priority 最小 */
        if ((tr->kind == TRUNK_STATIC_PEER || tr->kind == TRUNK_REGISTER)
            && tr->host[0] && have_src_v4) {
            struct in_addr host_addr;
            if (inet_pton(AF_INET, tr->host, &host_addr) == 1
                && host_addr.s_addr == src_addr.s_addr) {
                if (!best || tr->priority < best_prio) {
                    best = tr;
                    best_prio = tr->priority;
                }
            }
        }
    }

    return best;
}

int rt_group_add(route_table_t *t, const trunk_group_t *g)
{
    int i;
    if (t == NULL || g == NULL || g->name[0] == '\0' ||
        g->nmembers <= 0 || g->nmembers > RT_GROUP_MAX)
        return NSBC_INVAL;
    i = group_idx(t, g->name);
    if (i >= 0) {
        t->groups[i] = *g;
        return NSBC_OK;
    }
    if (t->n_groups >= RT_GROUP_CAP)
        return NSBC_NOSPC;
    t->groups[t->n_groups++] = *g;
    return NSBC_OK;
}

const trunk_group_t *rt_group_get(route_table_t *t, const char *name)
{
    int i;
    if (t == NULL || name == NULL)
        return NULL;
    i = group_idx(t, name);
    return i < 0 ? NULL : &t->groups[i];
}

const trunk_t *rt_select(route_table_t *t, const char *group,
                         const char *caller, int fail_reason)
{
    const trunk_group_t *g;
    const trunk_t *best;
    int best_prio;
    int i;
    if (t == NULL || group == NULL)
        return NULL;
    g = rt_group_get(t, group);
    if (g == NULL)
        return NULL;
    (void)caller;
    (void)fail_reason; /* TODO：按应答码掩码跳过可重试性不同的成员 */
    best = NULL;
    best_prio = 1 << 30;
    for (i = 0; i < g->nmembers; ++i) {
        int ti = trunk_idx(t, g->members[i]);
        if (ti < 0 || !t->trunks[ti].enabled || !t->open[ti])
            continue;
        if (t->trunks[ti].max_channels > 0 &&
            t->in_use[ti] >= (uint64_t)t->trunks[ti].max_channels)
            continue;
        /* 粗骨架：priority 最小者；同优先级按 weight（接线补散列） */
        if (best == NULL || t->trunks[ti].priority < best_prio) {
            best = &t->trunks[ti];
            best_prio = best->priority;
        }
    }
    return best;
}

void rt_acquire(route_table_t *t, const char *name)
{
    int i;
    if (t == NULL || name == NULL)
        return;
    i = trunk_idx(t, name);
    if (i >= 0)
        __sync_add_and_fetch(&t->in_use[i], 1);
}

void rt_release(route_table_t *t, const char *name)
{
    int i;
    if (t == NULL || name == NULL)
        return;
    i = trunk_idx(t, name);
    if (i >= 0 && t->in_use[i] > 0)
        __sync_sub_and_fetch(&t->in_use[i], 1);
}

void rt_mark_fail(route_table_t *t, const char *name)
{
    int i;
    if (t == NULL || name == NULL)
        return;
    i = trunk_idx(t, name);
    if (i < 0)
        return;
    if (++t->fails[i] >= 3) { /* 粗骨架阈值 3；P1 可配+时间窗半开探测 */
        t->open[i] = 0;
    }
}

void rt_mark_ok(route_table_t *t, const char *name)
{
    int i;
    if (t == NULL || name == NULL)
        return;
    i = trunk_idx(t, name);
    if (i >= 0) {
        t->fails[i] = 0;
        t->open[i] = 1;
    }
}

int rt_is_open(route_table_t *t, const char *name)
{
    int i;
    if (t == NULL || name == NULL)
        return 0;
    i = trunk_idx(t, name);
    return i < 0 ? 0 : t->open[i];
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
