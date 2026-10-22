/* 分机画像管理——粗骨架（REQ-PBX-EXT-001/003/004，P0/P1）
 *
 * 接线：ep_init（endpoints.c）建 ep_store 后同建 ext_mgr 挂 ctx；
 * 画像从配置/DB（P1 dbal 异步加载）填充；呼叫路由在 dp_execute
 * 命中内呼层后调 ext_lookup 取策略；注册仍只走 ep_store 凭证。
 * 分片锁实现与 location_db 同模式；粗骨架先给静态槽表轮廓。
 */
#include "extension.h"

#define EXT_SHARDS 64

typedef struct ext_node {
    ext_profile_t p;
    struct ext_node *next;
} ext_node_t;

typedef struct {
    /* 接线时在此补 pthread_mutex_t lock（与 location_db 分片同模式） */
    ext_node_t *head;
    uint64_t count;
} ext_shard_t;

struct ext_mgr {
    ext_shard_t sh[EXT_SHARDS];
};

static unsigned hash_key(const char *tenant, const char *number)
{
    const char *s;
    unsigned h = 2166136261u;
    for (s = tenant; *s; ++s) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    h ^= ':';
    for (s = number; *s; ++s) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

int ext_mgr_create(int shard_bits, ext_mgr_t **out)
{
    (void)shard_bits; /* 粗骨架固定 64 片；shard_bits 接线时生效 */
    if (out == NULL)
        return NSBC_INVAL;
    *out = (ext_mgr_t *)calloc(1, sizeof(ext_mgr_t));
    return *out != NULL ? NSBC_OK : NSBC_NOMEM;
    /* TODO：各片 pthread_mutex_init；RCU P1 替换读写锁 */
}

void ext_mgr_destroy(ext_mgr_t *m)
{
    if (m == NULL)
        return;
    /* TODO：遍历释放节点/销毁锁 */
    free(m);
}

static int key_eq(const ext_profile_t *p, const char *tenant,
                  const char *number)
{
    return strcmp(p->tenant, tenant) == 0 &&
           strcmp(p->number, number) == 0;
}

int ext_upsert(ext_mgr_t *m, const ext_profile_t *p)
{
    ext_shard_t *s;
    ext_node_t *n;
    if (m == NULL || p == NULL || p->number[0] == '\0')
        return NSBC_INVAL;
    if (strlen(p->tenant) >= EXT_TENANT_MAX ||
        strlen(p->number) >= EXT_NUMBER_MAX)
        return NSBC_INVAL;
    s = &m->sh[hash_key(p->tenant, p->number) & (EXT_SHARDS - 1)];
    for (n = s->head; n; n = n->next) {
        if (key_eq(&n->p, p->tenant, p->number)) {
            n->p = *p; /* TODO：片锁内 */
            return NSBC_OK;
        }
    }
    n = (ext_node_t *)calloc(1, sizeof(*n));
    if (n == NULL)
        return NSBC_NOMEM;
    n->p = *p;
    n->next = s->head;
    s->head = n;
    ++s->count;
    return NSBC_OK;
}

int ext_remove(ext_mgr_t *m, const char *tenant, const char *number)
{
    ext_shard_t *s;
    ext_node_t **pp;
    if (m == NULL || tenant == NULL || number == NULL)
        return NSBC_INVAL;
    s = &m->sh[hash_key(tenant, number) & (EXT_SHARDS - 1)];
    for (pp = &s->head; *pp; pp = &(*pp)->next) {
        if (key_eq(&(*pp)->p, tenant, number)) {
            ext_node_t *dead = *pp;
            *pp = dead->next;
            free(dead);
            --s->count;
            return NSBC_OK;
        }
    }
    return NSBC_NOENT;
}

const ext_profile_t *ext_lookup(ext_mgr_t *m, const char *tenant,
                                const char *number)
{
    ext_shard_t *s;
    ext_node_t *n;
    if (m == NULL || tenant == NULL || number == NULL)
        return NULL;
    s = &m->sh[hash_key(tenant, number) & (EXT_SHARDS - 1)];
    for (n = s->head; n; n = n->next) {
        if (key_eq(&n->p, tenant, number))
            return &n->p; /* TODO：RCU 无锁读，调用方在宽限期内使用 */
    }
    return NULL;
}

int ext_has_perm(const ext_profile_t *p, uint32_t need)
{
    return (p != NULL && p->status == EXT_ST_ENABLED &&
            (p->cos & need) == need);
}

int ext_same_tenant(const ext_profile_t *a, const ext_profile_t *b)
{
    return a != NULL && b != NULL &&
           strcmp(a->tenant, b->tenant) == 0;
}

uint64_t ext_count(const ext_mgr_t *m, const char *tenant)
{
    uint64_t total = 0;
    int i;
    if (m == NULL)
        return 0;
    if (tenant == NULL) {
        for (i = 0; i < EXT_SHARDS; ++i)
            total += m->sh[i].count;
        return total;
    }
    /* 粗骨架：精确租户计数遍历（仅 OAM 低频调用，非快路径） */
    for (i = 0; i < EXT_SHARDS; ++i) {
        const ext_node_t *n;
        for (n = m->sh[i].head; n; n = n->next) {
            if (strcmp(n->p.tenant, tenant) == 0)
                ++total;
        }
    }
    return total;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
