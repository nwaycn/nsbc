#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "location_db.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOC_AOR_BUCKETS 64

typedef struct loc_node {
    struct loc_node *next;
    loc_binding_t b;
} loc_node_t;

typedef struct loc_aor {
    struct loc_aor *next;
    loc_node_t *bindings;
    int n;
    char key[LOC_AOR_MAX];
} loc_aor_t;

typedef struct {
    pthread_mutex_t lock;
    loc_aor_t *aors[LOC_AOR_BUCKETS];
    int n; /* 本分片绑定数 */
} loc_shard_t;

struct loc_db {
    loc_db_cfg_t cfg;
    mem_slab_t *slab;
    loc_shard_t sh[LOC_SHARDS];
};

static uint32_t hash_aor(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static loc_aor_t **aor_bucket(loc_shard_t *s, uint32_t hash)
{
    return &s->aors[(hash >> 6) & (LOC_AOR_BUCKETS - 1)];
}

static loc_aor_t *aor_find(loc_aor_t *aor, const char *key)
{
    for (; aor; aor = aor->next)
        if (strcmp(aor->key, key) == 0)
            return aor;
    return NULL;
}

static void aor_unlink_empty(loc_aor_t **bucket, loc_aor_t *aor)
{
    if (aor->n != 0)
        return;
    while (*bucket && *bucket != aor)
        bucket = &(*bucket)->next;
    if (*bucket) {
        *bucket = aor->next;
        free(aor);
    }
}

int loc_db_create(const loc_db_cfg_t *cfg, loc_db_t **out)
{
    loc_db_t *db = calloc(1, sizeof(*db));
    if (!db)
        return NSBC_NOMEM;
    db->cfg = *cfg;
    if (db->cfg.capacity <= 0)
        db->cfg.capacity = 100000;
    if (db->cfg.max_contacts <= 0)
        db->cfg.max_contacts = 10;
    for (int i = 0; i < LOC_SHARDS; ++i)
        pthread_mutex_init(&db->sh[i].lock, NULL);

    mem_slab_cfg_t sc = {"loc_binding", sizeof(loc_node_t),
                         db->cfg.capacity, 16, db->cfg.arena, NULL, NULL};
    nsbc_rc_t rc = mem_slab_create(&sc, &db->slab);
    if (rc != NSBC_OK) {
        for (int i = 0; i < LOC_SHARDS; ++i)
            pthread_mutex_destroy(&db->sh[i].lock);
        free(db);
        return rc;
    }
    *out = db;
    return NSBC_OK;
}

void loc_db_destroy(loc_db_t *db)
{
    if (!db)
        return;
    for (int i = 0; i < LOC_SHARDS; ++i) {
        for (int b = 0; b < LOC_AOR_BUCKETS; ++b) {
            loc_aor_t *aor = db->sh[i].aors[b];
            while (aor) {
                loc_aor_t *next = aor->next;
                free(aor);
                aor = next;
            }
        }
        pthread_mutex_destroy(&db->sh[i].lock);
    }
    mem_slab_destroy(db->slab);
    free(db);
}

loc_rc_t loc_upsert(loc_db_t *db, const loc_binding_t *b, uint64_t now_ms)
{
    uint32_t hash = hash_aor(b->aor);
    uint32_t h = hash & (LOC_SHARDS - 1);
    loc_shard_t *s = &db->sh[h];
    pthread_mutex_lock(&s->lock);

    loc_aor_t **bucket = aor_bucket(s, hash);
    loc_aor_t *aor = aor_find(*bucket, b->aor);
    if (!aor) {
        aor = calloc(1, sizeof(*aor));
        if (!aor) {
            pthread_mutex_unlock(&s->lock);
            return LOC_FULL;
        }
        snprintf(aor->key, sizeof(aor->key), "%s", b->aor);
        aor->next = *bucket;
        *bucket = aor;
    }

    /* Clean only this AoR on the update path; full-table expiration stays in loc_sweep. */
    loc_node_t **pp = &aor->bindings;
    while (*pp) {
        loc_node_t *cur = *pp;
        if (cur->b.expire_ms <= now_ms) {
            *pp = cur->next;
            mem_obj_release(cur);
            --s->n;
            --aor->n;
            continue;
        }
        pp = &cur->next;
    }

    loc_node_t *match = NULL;
    loc_node_t *lru = NULL;  /* 同 AoR 中 expire 最早的（LRU victim） */
    for (loc_node_t *p = aor->bindings; p; p = p->next) {
        /* 同 Call-ID 属同一注册事务系列：CSeq 必须单调。先判次序，
         * 再谈 contact 匹配，否则回退请求会被当刷新吞掉 */
        if (!strcmp(p->b.callid, b->callid)) {
            if (b->cseq < p->b.cseq) {
                aor_unlink_empty(bucket, aor);
                pthread_mutex_unlock(&s->lock);
                return LOC_STALE_CSEQ; /* 同 Call-ID CSeq 回退：拒绝 */
            }
            if (b->cseq == p->b.cseq) {
                match = p; /* 同 CSeq：幂等重传 */
                break;
            }
        }
        if (!strcmp(p->b.contact, b->contact) ||
            (b->instance[0] && p->b.instance[0] &&
             !strcmp(p->b.instance, b->instance))) {
            match = p;
            break;
        }
        /* 跟踪 LRU：同 AoR 中 expire 最早的 */
        if (!lru || p->b.expire_ms < lru->b.expire_ms)
            lru = p;
    }
    if (match) {
        match->b = *b;
        pthread_mutex_unlock(&s->lock);
        return LOC_OK;
    }
    if (aor->n >= db->cfg.max_contacts) {
        /* ★ 不再 LOC_FULL 503——LRU evict 同 AoR 中最早过期的 */
        if (lru) {
            loc_node_t **pp2 = &aor->bindings;
            while (*pp2 && *pp2 != lru) pp2 = &(*pp2)->next;
            if (*pp2) {
                char evicted_contact[LOC_CONTACT_MAX];
                uint64_t evicted_expire = lru->b.expire_ms;
                snprintf(evicted_contact, sizeof(evicted_contact), "%s",
                         lru->b.contact);
                *pp2 = lru->next;
                lru->b = *b;
                lru->next = aor->bindings;
                aor->bindings = lru;
                llog(LOG_INFO, "loc",
                     "loc_upsert LRU-evict aor=%s contact=%s expire_ms=%lu now_ms=%lu",
                     b->aor, evicted_contact, (unsigned long)evicted_expire,
                     (unsigned long)now_ms);
                pthread_mutex_unlock(&s->lock);
                return LOC_OK;
            }
        } else {
            aor_unlink_empty(bucket, aor);
            pthread_mutex_unlock(&s->lock);
            return LOC_FULL;
        }
    }
    loc_node_t *n = mem_slab_alloc_direct(db->slab);
    if (!n) {
        aor_unlink_empty(bucket, aor);
        pthread_mutex_unlock(&s->lock);
        return LOC_FULL;
    }
    n->b = *b;
    n->next = aor->bindings;
    aor->bindings = n;
    ++aor->n;
    ++s->n;
    pthread_mutex_unlock(&s->lock);
    return LOC_OK;
}

int loc_remove(loc_db_t *db, const char *aor, const char *contact)
{
    uint32_t hash = hash_aor(aor);
    uint32_t h = hash & (LOC_SHARDS - 1);
    loc_shard_t *s = &db->sh[h];
    int removed = 0;
    int all = !contact || !strcmp(contact, "*");
    pthread_mutex_lock(&s->lock);
    loc_aor_t **bucket = aor_bucket(s, hash);
    loc_aor_t *entry = aor_find(*bucket, aor);
    if (!entry) {
        pthread_mutex_unlock(&s->lock);
        return 0;
    }
    loc_node_t **pp = &entry->bindings;
    while (*pp) {
        loc_node_t *p = *pp;
        if (all || !strcmp(p->b.contact, contact)) {
            *pp = p->next;
            mem_obj_release(p);
            ++removed;
            --s->n;
            --entry->n;
            continue;
        }
        pp = &p->next;
    }
    aor_unlink_empty(bucket, entry);
    pthread_mutex_unlock(&s->lock);
    return removed;
}

int loc_query(loc_db_t *db, const char *aor, loc_binding_t *out, int max,
              uint64_t now_ms)
{
    uint32_t hash = hash_aor(aor);
    uint32_t h = hash & (LOC_SHARDS - 1);
    loc_shard_t *s = &db->sh[h];
    int n = 0;
    pthread_mutex_lock(&s->lock);
    loc_aor_t *entry = aor_find(*aor_bucket(s, hash), aor);
    for (loc_node_t *p = entry ? entry->bindings : NULL; p && n < max;
         p = p->next) {
        if (p->b.expire_ms <= now_ms) {
            /* 目标 AoR 找到但已过期——单次诊断 log */
            static int diag_exp_seen = 0;
            if (!diag_exp_seen++) {
                llog(LOG_WARN, "loc",
                     "loc_query expire-skip aor=%s expire_ms=%lu now_ms=%lu shard=%u",
                     aor, (unsigned long)p->b.expire_ms,
                     (unsigned long)now_ms, (unsigned)h);
            }
            continue;
        }
        out[n++] = p->b;
    }
    pthread_mutex_unlock(&s->lock);
    /* q 降序（插入排序，max 很小） */
    for (int i = 1; i < n; ++i) {
        loc_binding_t t = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].q < t.q) {
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = t;
    }
    return n;
}

int loc_sweep(loc_db_t *db, uint64_t now_ms)
{
    int removed = 0;
    for (int i = 0; i < LOC_SHARDS; ++i) {
        loc_shard_t *s = &db->sh[i];
        pthread_mutex_lock(&s->lock);
        for (int b = 0; b < LOC_AOR_BUCKETS; ++b) {
            loc_aor_t **ap = &s->aors[b];
            while (*ap) {
                loc_aor_t *aor = *ap;
                loc_node_t **pp = &aor->bindings;
                while (*pp) {
                    loc_node_t *p = *pp;
                    if (p->b.expire_ms && p->b.expire_ms <= now_ms) {
                        *pp = p->next;
                        mem_obj_release(p);
                        ++removed;
                        --s->n;
                        --aor->n;
                        continue;
                    }
                    pp = &p->next;
                }
                if (aor->n == 0) {
                    *ap = aor->next;
                    free(aor);
                } else {
                    ap = &aor->next;
                }
            }
        }
        pthread_mutex_unlock(&s->lock);
    }
    return removed;
}

uint64_t loc_db_count(loc_db_t *db)
{
    uint64_t n = 0;
    for (int i = 0; i < LOC_SHARDS; ++i) {
        pthread_mutex_lock(&db->sh[i].lock);
        n += (uint64_t)db->sh[i].n;
        pthread_mutex_unlock(&db->sh[i].lock);
    }
    return n;
}

/* 遍历所有有效（未过期）绑定，回调 cb 收到 b 的副本 + now_ms。
 * cb 返回非 0 时停止遍历（截断）。过期条目跳过（与 loc_query 同口径）。 */
int loc_db_foreach(loc_db_t *db, uint64_t now_ms,
                   int (*cb)(const loc_binding_t *b, void *ud), void *ud)
{
    int shown = 0;
    for (int i = 0; i < LOC_SHARDS; ++i) {
        pthread_mutex_lock(&db->sh[i].lock);
        for (int b = 0; b < LOC_AOR_BUCKETS; ++b) {
            for (loc_aor_t *aor = db->sh[i].aors[b]; aor; aor = aor->next) {
                for (loc_node_t *p = aor->bindings; p; p = p->next) {
                    int expired = (p->b.expire_ms > 0 &&
                                   p->b.expire_ms <= now_ms);
                    if (!expired) {
                        int rc = cb(&p->b, ud);
                        ++shown;
                        if (rc != 0) {
                            pthread_mutex_unlock(&db->sh[i].lock);
                            return shown;
                        }
                    }
                }
            }
        }
        pthread_mutex_unlock(&db->sh[i].lock);
    }
    return shown;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
