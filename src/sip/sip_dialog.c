#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sip_dialog.h"
#include "nsbc_hash.h"
#include "nsbc_id.h"
#include "nsbc_list.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define DLG_SHARDS 1024   /* 64 → 1024: find_obj 链表扫描从 ~780→~49 dialog，延迟降 16 倍 */

typedef struct {
    pthread_mutex_t lock;
    nsbc_hnode_t *head;
} dlg_shard_t;

struct dlg_mgr {
    dlg_mgr_cfg_t cfg;
    mem_slab_t *slab_own;
    dlg_shard_t sh[DLG_SHARDS];
    unsigned long active;
};

typedef struct dlg_obj {
    nsbc_hnode_t link;
    sip_dialog_t d;
} dlg_obj_t;

static void tok_copy(char *dst, size_t cap, sip_tok_t t)
{
    size_t n = t.len < cap - 1 ? t.len : cap - 1;
    memcpy(dst, t.p, n);
    dst[n] = '\0';
}

static dlg_shard_t *shard(dlg_mgr_t *m, const char *cid, size_t clen,
                          const char *lt, size_t ltlen)
{
    uint64_t h = nsbc_hash2(cid, clen, lt, ltlen);
    return &m->sh[nsbc_hash_shard(h, DLG_SHARDS)];
}

int dlg_mgr_create(const dlg_mgr_cfg_t *cfg, dlg_mgr_t **out)
{
    dlg_mgr_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NSBC_NOMEM;
    m->cfg = *cfg;
    for (int i = 0; i < DLG_SHARDS; ++i)
        pthread_mutex_init(&m->sh[i].lock, NULL);
    if (!m->cfg.slab) {
        mem_slab_cfg_t sc = {"sip_dialog", sizeof(dlg_obj_t),
                            cfg->capacity > 0 ? cfg->capacity : 50000, 8,
                            cfg->arena, NULL, NULL};
        if (mem_slab_create(&sc, &m->slab_own) != NSBC_OK) {
            free(m);
            return NSBC_NOMEM;
        }
        m->cfg.slab = m->slab_own;
    }
    *out = m;
    return NSBC_OK;
}

void dlg_mgr_destroy(dlg_mgr_t *m)
{
    for (int i = 0; i < DLG_SHARDS; ++i) {
        nsbc_hnode_t *h = m->sh[i].head;
        while (h) {
            dlg_obj_t *o = NSBC_CONTAINER_OF(h, dlg_obj_t, link);
            h = h->next;
            mem_obj_release(o);
        }
        pthread_mutex_destroy(&m->sh[i].lock);
    }
    if (m->slab_own)
        mem_slab_destroy(m->slab_own);
    free(m);
}

static dlg_obj_t *obj_new(dlg_mgr_t *m)
{
    dlg_obj_t *o = mem_slab_alloc_direct(m->cfg.slab);
    if (!o)
        return NULL;
    memset(o, 0, sizeof(*o));
    nsbc_hnode_init(&o->link);
    __atomic_add_fetch(&m->active, 1, __ATOMIC_RELAXED);
    return o;
}

static void obj_destroy(dlg_mgr_t *m, dlg_obj_t *o)
{
    __atomic_sub_fetch(&m->active, 1, __ATOMIC_RELAXED);
    mem_obj_release(o);
}

sip_dialog_t *dlg_uas_new(dlg_mgr_t *m, const sip_msg_t *req)
{
    sip_hdr_t *cid = sip_hdr_get(req, SIP_H_CALL_ID);
    sip_hdr_t *from = sip_hdr_get(req, SIP_H_FROM);
    sip_hdr_t *contact = sip_hdr_get(req, SIP_H_CONTACT);
    sip_hdr_t *cseq = sip_hdr_get(req, SIP_H_CSEQ);
    /* ★ P0: cseq 空指针解引用崩溃 — 之前直接 parse_cseq(cseq) 没判 NULL */
    if (!m || !cid || !from || !cseq)
        return NULL;
    /* ★ P0: callid 长度检查 — 与 dlg_uac_new 保持一致，
     *   超长 Call-ID 会静默截断后 find_obj 里 memcmp 不会误匹配但行为不一致 */
    if (cid->value.len >= DLG_CALLID_MAX || from->u.addr.tag.len >= DLG_TAG_MAX)
        return NULL;
    if (sip_hdr_parse_addr(from) != NSBC_OK ||
        sip_hdr_parse_cseq(cseq) != NSBC_OK)
        return NULL;

    dlg_obj_t *o = obj_new(m);
    if (!o)
        return NULL;
    sip_dialog_t *d = &o->d;
    tok_copy(d->callid, sizeof(d->callid), cid->value);
    nsbc_rand_token(d->local_tag, DLG_TAG_MAX, 8);
    tok_copy(d->remote_tag, sizeof(d->remote_tag), from->u.addr.tag);
    d->dir = DLG_UAS;
    d->state = DLG_EARLY;
    d->remote_cseq = cseq->u.cseq.number;
    d->init_method = req->method;
    if (contact && sip_hdr_parse_addr(contact) == NSBC_OK)
        tok_copy(d->target, sizeof(d->target), contact->u.addr.uri.host);
    /* UAS 路由集 = Record-Route 顺序拷贝 */
    int n = 0;
    for (sip_hdr_t *rr = sip_hdr_get(req, SIP_H_RECORD_ROUTE);
         rr && n < DLG_ROUTE_MAX; rr = sip_hdr_next(req, rr)) {
        if (sip_hdr_parse_route(rr) == NSBC_OK) {
            tok_copy(d->routes[n], DLG_URI_MAX, rr->u.route.uri.host);
            ++n;
        }
    }
    d->nroutes = n;

    dlg_shard_t *s = shard(m, d->callid, strlen(d->callid), d->local_tag,
                           strlen(d->local_tag));
    pthread_mutex_lock(&s->lock);
    o->link.next = s->head;
    s->head = &o->link;
    pthread_mutex_unlock(&s->lock);
    return d;
}

sip_dialog_t *dlg_uac_new(dlg_mgr_t *m, const char *callid, size_t clen,
                          const char *local_tag, size_t ltlen,
                          const char *contact_target, long cseq,
                          sip_method_t init_method)
{
    if (clen >= DLG_CALLID_MAX || ltlen >= DLG_TAG_MAX)
        return NULL;
    dlg_obj_t *o = obj_new(m);
    if (!o)
        return NULL;
    sip_dialog_t *d = &o->d;
    memcpy(d->callid, callid, clen);
    d->callid[clen] = '\0';
    memcpy(d->local_tag, local_tag, ltlen);
    d->local_tag[ltlen] = '\0';
    if (contact_target)
        snprintf(d->target, sizeof(d->target), "%s", contact_target);
    d->dir = DLG_UAC;
    d->state = DLG_EARLY;
    d->local_cseq = cseq;
    d->init_method = init_method;

    dlg_shard_t *s = shard(m, d->callid, strlen(d->callid), d->local_tag,
                           strlen(d->local_tag));
    pthread_mutex_lock(&s->lock);
    o->link.next = s->head;
    s->head = &o->link;
    pthread_mutex_unlock(&s->lock);
    return d;
}

static dlg_obj_t *find_obj(dlg_mgr_t *m, const char *cid, size_t clen,
                           const char *lt, size_t ltlen,
                           const char *rt, size_t rtlen)
{
    dlg_shard_t *s = shard(m, cid, clen, lt, ltlen);
    pthread_mutex_lock(&s->lock);
    for (nsbc_hnode_t *h = s->head; h; h = h->next) {
        dlg_obj_t *o = NSBC_CONTAINER_OF(h, dlg_obj_t, link);
        sip_dialog_t *d = &o->d;
        /* ★ P1: 热点路径去掉每节点 strlen — 直接用调用方传入长度 memcmp
         *   （memcmp 比 strlen+memcmp 快；调用方是 find_obj/dlg_match_msg
         *   都是已知精确 len，且 DLG_CALLID_MAX/DLG_TAG_MAX 都是定长 buffer） */
        if (!memcmp(d->callid, cid, clen) && d->callid[clen] == '\0' &&
            !memcmp(d->local_tag, lt, ltlen) && d->local_tag[ltlen] == '\0' &&
            (rtlen == 0 || (!memcmp(d->remote_tag, rt, rtlen) && d->remote_tag[rtlen] == '\0'))) {
            pthread_mutex_unlock(&s->lock);
            return o;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

/* EARLY UAC 的 remote_tag 尚空：无 rt 条件匹配唯一同 callid+lt */
static dlg_obj_t *find_uac_early(dlg_mgr_t *m, const char *cid, size_t clen,
                                 const char *lt, size_t ltlen)
{
    dlg_shard_t *s = shard(m, cid, clen, lt, ltlen);
    pthread_mutex_lock(&s->lock);
    for (nsbc_hnode_t *h = s->head; h; h = h->next) {
        dlg_obj_t *o = NSBC_CONTAINER_OF(h, dlg_obj_t, link);
        sip_dialog_t *d = &o->d;
        /* ★ P1: 同样去掉 strlen */
        if (d->dir == DLG_UAC &&
            !memcmp(d->callid, cid, clen) && d->callid[clen] == '\0' &&
            !memcmp(d->local_tag, lt, ltlen) && d->local_tag[ltlen] == '\0' &&
            d->remote_tag[0] == '\0') {
            pthread_mutex_unlock(&s->lock);
            return o;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

sip_dialog_t *dlg_find(dlg_mgr_t *m, const char *callid, size_t clen,
                       const char *local_tag, size_t ltlen,
                       const char *remote_tag, size_t rtlen)
{
    dlg_obj_t *o = find_obj(m, callid, clen, local_tag, ltlen, remote_tag,
                            rtlen);
    return o ? &o->d : NULL;
}

int dlg_uac_on_response(dlg_mgr_t *m, const sip_msg_t *resp)
{
    sip_hdr_t *cid = sip_hdr_get(resp, SIP_H_CALL_ID);
    sip_hdr_t *to = sip_hdr_get(resp, SIP_H_TO);
    if (!cid || !to || sip_hdr_parse_addr(to) != NSBC_OK)
        return NSBC_PROTO;
    sip_tok_t ltt; /* UAC：local tag 在 From */
    sip_hdr_t *from = sip_hdr_get(resp, SIP_H_FROM);
    if (!from || sip_hdr_parse_addr(from) != NSBC_OK)
        return NSBC_PROTO;
    ltt = from->u.addr.tag;

    dlg_obj_t *o;
    if (to->u.addr.tag.p)
        o = find_obj(m, cid->value.p, cid->value.len, ltt.p, ltt.len,
                     to->u.addr.tag.p, to->u.addr.tag.len);
    else
        o = find_uac_early(m, cid->value.p, cid->value.len, ltt.p, ltt.len);
    if (!o)
        return NSBC_NOENT;
    sip_dialog_t *d = &o->d;
    if (to->u.addr.tag.p && d->remote_tag[0] == '\0')
        tok_copy(d->remote_tag, sizeof(d->remote_tag), to->u.addr.tag);
    sip_hdr_t *contact = sip_hdr_get(resp, SIP_H_CONTACT);
    if (contact && sip_hdr_parse_addr(contact) == NSBC_OK)
        tok_copy(d->target, sizeof(d->target), contact->u.addr.uri.host);
    /* UAC 路由集 = Record-Route 逆序：直接从后往前填 d->routes，
     * 省掉 tmp[DLG_ROUTE_MAX][DLG_URI_MAX] 768B 栈空间 */
    int n = 0;
    for (sip_hdr_t *rr = sip_hdr_get(resp, SIP_H_RECORD_ROUTE);
         rr && n < DLG_ROUTE_MAX; rr = sip_hdr_next(resp, rr)) {
        if (sip_hdr_parse_route(rr) == NSBC_OK) {
            tok_copy(d->routes[DLG_ROUTE_MAX - 1 - n], DLG_URI_MAX,
                     rr->u.route.uri.host);
            ++n;
        }
    }
    /* 压缩到 d->routes[0..n-1] — 把尾部 n 个向前挪 n 格 */
    for (int i = 0; i < n; ++i) {
        if (DLG_ROUTE_MAX - 1 - n != i)
            memcpy(d->routes[i], d->routes[DLG_ROUTE_MAX - 1 - n + i], DLG_URI_MAX);
    }
    d->nroutes = n;
    if (resp->status >= 200 && resp->status < 300)
        d->state = DLG_CONFIRMED;
    /* find_obj/find_uac_early 已释放分片锁（各自 unlock）；状态写入与
     * worker 哈希路由一致（同 Call-ID 同 worker），无并发写者。 */
    return NSBC_OK;
}

int dlg_uas_on_request(sip_dialog_t *d, const sip_msg_t *req)
{
    sip_hdr_t *cs = sip_hdr_get(req, SIP_H_CSEQ);
    if (!cs || sip_hdr_parse_cseq(cs) != NSBC_OK)
        return NSBC_PROTO;
    if (cs->u.cseq.number < d->remote_cseq)
        return NSBC_PROTO; /* 旧 CSeq：调用方回 500/忽略 */
    d->remote_cseq = cs->u.cseq.number;
    sip_hdr_t *contact = sip_hdr_get(req, SIP_H_CONTACT);
    if (contact && sip_hdr_parse_addr(contact) == NSBC_OK)
        tok_copy(d->target, sizeof(d->target), contact->u.addr.uri.host);
    return NSBC_OK;
}

sip_dialog_t *dlg_match_msg(dlg_mgr_t *m, const sip_msg_t *msg,
                            dlg_dir_t dir)
{
    sip_hdr_t *cid = sip_hdr_get(msg, SIP_H_CALL_ID);
    sip_hdr_t *from = sip_hdr_get(msg, SIP_H_FROM);
    sip_hdr_t *to = sip_hdr_get(msg, SIP_H_TO);
    if (!cid || !from || !to)
        return NULL;
    if (sip_hdr_parse_addr(from) != NSBC_OK ||
        sip_hdr_parse_addr(to) != NSBC_OK)
        return NULL;
    if (dir == DLG_UAS) {
        /* 本机是 UAS：local tag 在 To */
        return dlg_find(m, cid->value.p, cid->value.len,
                        to->u.addr.tag.p, to->u.addr.tag.len,
                        from->u.addr.tag.p, from->u.addr.tag.len);
    }
    return dlg_find(m, cid->value.p, cid->value.len,
                    from->u.addr.tag.p, from->u.addr.tag.len,
                    to->u.addr.tag.p, to->u.addr.tag.len);
}

void dlg_close(dlg_mgr_t *m, sip_dialog_t *dp)
{
    /* 通过 container_of 反查对象：dp 是 dlg_obj_t 首成员后的字段，
     * 直接用偏移定位 */
    dlg_obj_t *o = NSBC_CONTAINER_OF(dp, dlg_obj_t, d);
    sip_dialog_t *d = dp;
    dlg_shard_t *s = shard(m, d->callid, strlen(d->callid), d->local_tag,
                           strlen(d->local_tag));
    pthread_mutex_lock(&s->lock);
    for (nsbc_hnode_t **pp = &s->head; *pp; pp = &(*pp)->next) {
        if (*pp == &o->link) {
            *pp = o->link.next;
            d->state = DLG_CLOSED;
            pthread_mutex_unlock(&s->lock);
            obj_destroy(m, o);
            return;
        }
    }
    pthread_mutex_unlock(&s->lock);
}

uint64_t dlg_mgr_active(dlg_mgr_t *m)
{
    return __atomic_load_n(&m->active, __ATOMIC_RELAXED);
}
