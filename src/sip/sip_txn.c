#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sip_txn.h"
#include "metric_reg.h"
#include "nsbc_hash.h"
#include "nsbc_list.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RFC3261 §17 四事务状态机；定时器全部在分层时间轮上（每对象两个
 * 内嵌节点：重传节点 + 绝对超时节点）。 */

#define TXN_SHARDS 64
#define TXN_BUCKETS 64
#define BRANCH_MAX 96

/* 外部 thread-local：sip_transport.c 定义，b2bua.c 写入，txn.c / sip_tr_send 读取 */
extern __thread uint16_t g_rx_local_port;

struct sip_txn {
    nsbc_hnode_t link;
    txn_kind_t kind;
    txn_state_t state;
    char branch[BRANCH_MAX];
    size_t blen;
    sip_method_t method;
    nsbc_sa_t peer;
    int reliable;

    char *req;
    size_t req_len;
    char *resp;
    size_t resp_len;

    tw_node_t tm_retr; /* A/E/G */
    tw_node_t tm_dead; /* B/F/H 或 D/I/J/K */
    uint32_t interval;

    /* 本地 UDP 发送端口：创建事务时从 g_rx_local_port 捕获，
     * 确保事务重发时仍用同一 src_port（与 Via/Contact 宣告端口一致）
     * —— WS 入向场景下 g_rx_local_port 为 0，b2bua.c 会在调用
     * txn_client_new 前显式设为 c->prof->sip_udp_port */
    uint16_t local_port;

    txn_tu_fn tu;
    void *ud;
    txn_mgr_t *mgr;

    int dead;
    long refs; /* hash 1 + 在轮定时器 1 + 输入路径临持（__atomic） */
};

typedef struct {
    pthread_mutex_t lock;
    nsbc_hnode_t *buckets[TXN_BUCKETS];
} txn_shard_t;

struct txn_mgr {
    txn_mgr_cfg_t cfg;
    txn_shard_t sh[TXN_SHARDS];
    mem_slab_t *slab_own;
    /* OBS 计数 */
    unsigned long active;
    unsigned long rin, rout, tmo;
};

static uint64_t txn_hash(const char *b, size_t blen, sip_method_t meth,
                         txn_kind_t kind)
{
    uint64_t h = nsbc_hash2(b, blen, &meth, sizeof(meth));
    h ^= (uint64_t)kind * 0x9E3779B97F4A7C15ULL;
    return h;
}

static txn_shard_t *shard_for_hash(txn_mgr_t *m, uint64_t h)
{
    return &m->sh[nsbc_hash_shard(h, TXN_SHARDS)];
}

static nsbc_hnode_t **bucket_for_hash(txn_shard_t *s, uint64_t h)
{
    unsigned idx = (unsigned)((h >> 6) & (TXN_BUCKETS - 1));
    return &s->buckets[idx];
}

static txn_shard_t *shard_for(txn_mgr_t *m, const char *b, size_t blen,
                              sip_method_t meth, txn_kind_t kind)
{
    return shard_for_hash(m, txn_hash(b, blen, meth, kind));
}

static long t_ref(sip_txn_t *t)
{
    return __atomic_add_fetch(&t->refs, 1, __ATOMIC_ACQ_REL);
}
static long t_unref(sip_txn_t *t)
{
    long r = __atomic_sub_fetch(&t->refs, 1, __ATOMIC_RELEASE);
    if (r == 0) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        txn_mgr_t *m = t->mgr;
        if (t->req)
            mem_arena_free(m->cfg.pkt_arena, t->req);
        if (t->resp)
            mem_arena_free(m->cfg.pkt_arena, t->resp);
        mem_obj_release(t);
        __atomic_sub_fetch(&m->active, 1, __ATOMIC_RELAXED);
    }
    return r;
}

static void t_send(sip_txn_t *t, const char *buf, size_t len)
{
    /* 恢复发送端口：确保重发时仍用 txn 创建时捕获的 local_port
     * （txn_client_new 首发会走本函数，此时 local_port 已设好） */
    if (t->local_port > 0)
        g_rx_local_port = t->local_port;
    if (t->mgr->cfg.send) {
        char ps[128]; nsbc_sa_fmt(&t->peer, ps, sizeof(ps));
        llog(LOG_INFO, "txn", "t_send: method=%d kind=%d len=%zu peer=%s reliable=%d",
             t->method, t->kind, len, ps, t->reliable);
        t->mgr->cfg.send(t->mgr->cfg.send_ud, buf, len, &t->peer,
                         t->reliable);
    }
}

/* ---- 定时器回调（时间轮线程，锁外） ---- */
static void arm(sip_txn_t *t, tw_node_t *n, uint32_t ms, tw_cb_fn cb,
                uint64_t now);

static void t_remove_hash(sip_txn_t *t)
{
    txn_mgr_t *m = t->mgr;
    uint64_t hash = txn_hash(t->branch, t->blen, t->method, t->kind);
    txn_shard_t *s = shard_for_hash(m, hash);
    pthread_mutex_lock(&s->lock);
    if (!t->dead) {
        t->dead = 1;
        nsbc_hnode_t **pp = bucket_for_hash(s, hash);
        while (*pp) {
            if (*pp == &t->link) {
                *pp = t->link.next;
                break;
            }
            pp = &(*pp)->next;
        }
        if (tw_del(m->cfg.tw, &t->tm_retr))
            t_unref(t);
        if (tw_del(m->cfg.tw, &t->tm_dead))
            t_unref(t);
        pthread_mutex_unlock(&s->lock);
        t_unref(t); /* hash 引用 */
    } else {
        pthread_mutex_unlock(&s->lock);
    }
}

static void cb_retr(uint64_t now, void *arg);
static void cb_dead(uint64_t now, void *arg);

static void arm(sip_txn_t *t, tw_node_t *n, uint32_t ms, tw_cb_fn cb,
                uint64_t now)
{
    tw_add(t->mgr->cfg.tw, n, now + ms, cb, t);
    t_ref(t); /* 在轮 = +1；cb 结束若未重挂则对应 unref */
}

static void cb_retr(uint64_t now, void *arg)
{
    sip_txn_t *t = (sip_txn_t *)arg;
    txn_mgr_t *m = t->mgr;
    t_unref(t); /* 本次在轮引用已消费；若重新 arm 会再 +1 */
    uint32_t next = 0;
    switch (t->state) {
    case TXN_ICT_CALLING:
        t_send(t, t->req, t->req_len);
        __atomic_add_fetch(&m->rout, 1, __ATOMIC_RELAXED);
        t->interval = t->interval * 2;
        if (t->interval > m->cfg.t2_ms)
            t->interval = m->cfg.t2_ms;
        next = t->interval;
        break;
    case TXN_NICT_TRYING:
        t_send(t, t->req, t->req_len);
        __atomic_add_fetch(&m->rout, 1, __ATOMIC_RELAXED);
        t->interval = t->interval * 2;
        if (t->interval > m->cfg.t2_ms)
            t->interval = m->cfg.t2_ms;
        next = t->interval;
        break;
    case TXN_IST_COMPLETED:
        if (t->resp) {
            t_send(t, t->resp, t->resp_len);
            __atomic_add_fetch(&m->rout, 1, __ATOMIC_RELAXED);
        }
        t->interval = t->interval * 2;
        if (t->interval > m->cfg.t2_ms)
            t->interval = m->cfg.t2_ms;
        next = t->interval;
        break;
    default:
        return;
    }
    if (next)
        arm(t, &t->tm_retr, next, cb_retr, now);
}

static void cb_dead(uint64_t now, void *arg)
{
    sip_txn_t *t = (sip_txn_t *)arg;
    txn_mgr_t *m = t->mgr;
    t_unref(t); /* 消费本次在轮引用 */
    (void)now;
    switch (t->state) {
    case TXN_ICT_CALLING:   /* Timer B */
    case TXN_NICT_TRYING:   /* Timer F */
    case TXN_NICT_PROCEEDING:
    case TXN_IST_COMPLETED: /* Timer H */
        if (t->tu)
            t->tu(t, NULL, TXN_EV_TIMEOUT, t->ud);
        __atomic_add_fetch(&m->tmo, 1, __ATOMIC_RELAXED);
        t->state = (t->kind == TXN_ICT) ? TXN_ICT_TERMINATED
                 : (t->kind == TXN_IST) ? TXN_IST_TERMINATED
                 : TXN_NICT_TERMINATED;
        t_remove_hash(t);
        break;
    case TXN_IST_PROCEEDING:  /* BUG-1 兜底 Timer P 触发：IST 从未收到过 2xx+ */
    case TXN_NIST_TRYING:     /* BUG-1 兜底 Timer F 触发：NIST 从未收到过 2xx+ */
        /* early return 场景（ACL deny / profile deny / 号码不存在等）。
         * 没有 server_respond 被调，所以没人 arm Timer H/J，
         * 我们在 TXN_NEW 时 arm 的 Timer P/F 就是最后的兜底。 */
        llog(LOG_INFO, "txn",
             "cb_dead: terminating orphan txn state=%d kind=%d refs=%ld",
             t->state, t->kind, t->refs);
        __atomic_add_fetch(&m->tmo, 1, __ATOMIC_RELAXED);
        t_remove_hash(t);
        break;
    case TXN_ICT_COMPLETED: /* Timer D */
    case TXN_IST_CONFIRMED: /* Timer I */
    case TXN_NIST_COMPLETED: /* Timer J */
    case TXN_NICT_COMPLETED: /* Timer K */
        t_remove_hash(t);
        break;
    default:
        break;
    }
}

/* OBS 自埋点（REQ-OBS-001）：事务计数器 render，ud=txn_mgr；
 * 2026-09-27 A3 自 oam/obs_metrics.c 原样迁入，指标名保持不变。 */
static int r_txn(char *b, size_t cap, void *ud)
{
    txn_mgr_t *t = (txn_mgr_t *)ud;
    if (!t)
        return snprintf(b, cap, "nsbc_txn_active 0\n");
    return snprintf(b, cap,
                    "nsbc_txn_active %llu\n"
                    "nsbc_txn_retrans_in_total %llu\n"
                    "nsbc_txn_retrans_out_total %llu\n"
                    "nsbc_txn_timeouts_total %llu\n",
                    (unsigned long long)txn_mgr_active(t),
                    (unsigned long long)txn_mgr_retrans_in(t),
                    (unsigned long long)txn_mgr_retrans_out(t),
                    (unsigned long long)txn_mgr_timeouts(t));
}

/* ---- 生命周期 ---- */
int txn_mgr_create(const txn_mgr_cfg_t *cfg, txn_mgr_t **out)
{
    txn_mgr_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NSBC_NOMEM;
    m->cfg = *cfg;
    for (int i = 0; i < TXN_SHARDS; ++i)
        pthread_mutex_init(&m->sh[i].lock, NULL);
    if (!m->cfg.slab) {
        mem_slab_cfg_t sc = {
            "sip_txn", sizeof(struct sip_txn),
            cfg->capacity > 0 ? cfg->capacity : 50000, 8,
            cfg->sig_arena, NULL, NULL};
        fprintf(stderr, "[txn] creating slab name=%s obj_size=%zu capacity=%d arena=%p\n",
                sc.name, sc.obj_size, sc.capacity, (void *)sc.arena);
        if (mem_slab_create(&sc, &m->slab_own) != NSBC_OK) {
            free(m);
            return NSBC_NOMEM;
        }
        fprintf(stderr, "[txn] slab created slab_own=%p\n", (void *)m->slab_own);
        m->cfg.slab = m->slab_own;
    }
    metric_source_register("nsbc_txn", "SIP transaction counters", r_txn, m);
    *out = m;
    return NSBC_OK;
}

void txn_mgr_destroy(txn_mgr_t *m)
{
    /* 停机路径：清空残留（worker/timer 已停） */
    for (int i = 0; i < TXN_SHARDS; ++i) {
        for (int b = 0; b < TXN_BUCKETS; ++b) {
            nsbc_hnode_t *h = m->sh[i].buckets[b];
            while (h) {
                sip_txn_t *t = NSBC_CONTAINER_OF(h, sip_txn_t, link);
                h = h->next;
                tw_del(m->cfg.tw, &t->tm_retr);
                tw_del(m->cfg.tw, &t->tm_dead);
                if (t->req)
                    mem_arena_free(m->cfg.pkt_arena, t->req);
                if (t->resp)
                    mem_arena_free(m->cfg.pkt_arena, t->resp);
                mem_obj_release(t);
            }
        }
        pthread_mutex_destroy(&m->sh[i].lock);
    }
    if (m->slab_own)
        mem_slab_destroy(m->slab_own);
    free(m);
}

/* 装配期后置绑定（b2b 与 txn 互相依赖：txn 在 init 建、b2b 在 start 建，
 * 必须在 reactor 收包前调用一次）。 */
void txn_mgr_set_server_tu(txn_mgr_t *m, txn_tu_fn tu, void *ud)
{
    m->cfg.server_tu = tu;
    m->cfg.server_ud = ud;
}

static sip_txn_t *t_alloc(txn_mgr_t *m, txn_kind_t kind,
                          sip_method_t meth)
{
    sip_txn_t *t = mem_slab_alloc_direct(m->cfg.slab);
    if (!t)
        return NULL;
    memset(t, 0, sizeof(*t));
    nsbc_hnode_init(&t->link);
    tw_node_init(&t->tm_retr);
    tw_node_init(&t->tm_dead);
    t->kind = kind;
    t->method = meth;
    t->mgr = m;
    t->refs = 0; /* 入 hash +1；每个在轮定时器 +1 */
    __atomic_add_fetch(&m->active, 1, __ATOMIC_RELAXED);
    return t;
}

static char *copy_buf(txn_mgr_t *m, const char *buf, size_t len)
{
    char *p = mem_arena_alloc(m->cfg.pkt_arena, len + 1);
    if (!p)
        return NULL;
    memcpy(p, buf, len);
    p[len] = '\0';
    return p;
}

sip_txn_t *txn_client_new(txn_mgr_t *m, txn_kind_t kind,
                          const char *branch, size_t blen,
                          sip_method_t method, const nsbc_sa_t *dst,
                          int reliable, const char *buf, size_t len,
                          txn_tu_fn tu, void *ud, uint64_t now_ms)
{
    if (blen == 0 || blen >= BRANCH_MAX || (kind != TXN_ICT &&
        kind != TXN_NICT))
        return NULL;
    sip_txn_t *t = t_alloc(m, kind, method);
    if (!t)
        return NULL;
    memcpy(t->branch, branch, blen);
    t->blen = blen;
    if (dst)
        t->peer = *dst;
    t->reliable = reliable;
    t->local_port = g_rx_local_port; /* 捕获当前本地发送端口 */
    t->tu = tu;
    t->ud = ud;
    t->req = copy_buf(m, buf, len);
    if (!t->req) {
        mem_obj_release(t);
        return NULL;
    }
    t->req_len = len;
    t->interval = m->cfg.t1_ms;
    t->state = (kind == TXN_ICT) ? TXN_ICT_CALLING : TXN_NICT_TRYING;

    uint64_t hash = txn_hash(t->branch, t->blen, method, kind);
    txn_shard_t *s = shard_for_hash(m, hash);
    pthread_mutex_lock(&s->lock);
    nsbc_hnode_t **bucket = bucket_for_hash(s, hash);
    t->link.next = *bucket;
    *bucket = &t->link;
    t_ref(t); /* hash 引用 */
    pthread_mutex_unlock(&s->lock);

    /* 首发 */
    t_send(t, buf, len);
    if (!reliable) {
        arm(t, &t->tm_retr, m->cfg.t1_ms, cb_retr, now_ms); /* A/E */
    } else {
        tw_node_init(&t->tm_retr); /* 可靠传输无重传 */
    }
    arm(t, &t->tm_dead, 64u * m->cfg.t1_ms, cb_dead, now_ms); /* B/F */
    return t;
}

/* 取 top Via branch；返回长度，0 表示无/非 cookie（*cookie 置 0） */
static size_t via_branch(const sip_msg_t *msg, int *cookie)
{
    sip_hdr_t *v = sip_hdr_get(msg, SIP_H_VIA);
    if (!v || sip_hdr_parse_via(v) != NSBC_OK) {
        /* 尝试从原始 buf 手动提取 branch — 绕过 sip_parse value.len bug */
        const char *p = msg->buf;
        const char *end = msg->buf + msg->buflen;
        while (p && end && p < end - 4) {
            if ((p[0] == 'V' || p[0] == 'v') &&
                (p[1] == 'I' || p[1] == 'i') &&
                (p[2] == 'A' || p[2] == 'a') &&
                (p[3] == ':' || p[3] == ' ' || p[3] == '\t')) {
                const char *q = p + 4;
                while (q < end - 7) {
                    if (strncasecmp(q, "branch=", 7) == 0) {
                        q += 7;
                        const char *bs = q;
                        while (q < end && *q != ';' && *q != '\r' && *q != '\n' && *q != ' ')
                            q++;
                        size_t bl = q - bs;
                        if (bl >= 7 && bl < BRANCH_MAX) {
                            *cookie = (strncmp(bs, "z9hG4bK", 7) == 0);
                            return bl;
                        }
                        return 0;
                    }
                    q++;
                }
                return 0;
            }
            p++;
        }
        return 0;
    }
    sip_tok_t b = v->u.via.branch;
    if (!b.p || b.len < 7) {
        *cookie = 0;
        return 0;
    }
    *cookie = (strncmp(b.p, "z9hG4bK", 7) == 0);
    return b.len < BRANCH_MAX ? b.len : BRANCH_MAX - 1;
}

static sip_txn_t *find_locked(txn_shard_t *s, uint64_t hash,
                              const char *branch, size_t blen,
                              sip_method_t meth, txn_kind_t kind)
{
    nsbc_hnode_t **bucket = bucket_for_hash(s, hash);
    for (nsbc_hnode_t *h = *bucket; h; h = h->next) {
        sip_txn_t *t = NSBC_CONTAINER_OF(h, sip_txn_t, link);
        if (t->kind == kind &&
            t->method == meth && t->blen == blen &&
            !memcmp(t->branch, branch, blen))
            return t;
    }
    return NULL;
}

static int status_of(const sip_msg_t *m)
{
    return m->status;
}

/* 客户端响应处理（调用方持有 t 的临持引用，但不持分片锁） */
static void client_on_response(txn_mgr_t *m, sip_txn_t *t,
                               const sip_msg_t *msg, uint64_t now)
{
    int st = status_of(msg);
    int provisional = (st >= 100 && st < 200);
    uint64_t hash = txn_hash(t->branch, t->blen, t->method, t->kind);
    txn_shard_t *s = shard_for_hash(m, hash);
    pthread_mutex_lock(&s->lock);
    switch (t->state) {
    case TXN_ICT_CALLING:
        if (provisional) {
            t->state = TXN_ICT_PROCEEDING;
            if (tw_del(m->cfg.tw, &t->tm_retr))
                t_unref(t);
            /* B 定时器在 PROCEEDING 仍保留（RFC 语义上 B 仅 CALLING，
             * 但保留不影响正确性：2xx/终态会摘除） */
            pthread_mutex_unlock(&s->lock);
            if (t->tu)
                t->tu(t, msg, TXN_EV_PROVISIONAL, t->ud);
            return;
        }
        goto ict_final;
    case TXN_ICT_PROCEEDING:
        if (provisional) {
            pthread_mutex_unlock(&s->lock);
            if (t->tu)
                t->tu(t, msg, TXN_EV_PROVISIONAL, t->ud);
            return;
        }
    ict_final:
        /* 2xx 的逐跳重传吸收交给 dialog/B2BUA；非 2xx ACK 由 TU 构造。
         * 事务进入 TERMINATED 留 Timer D 吸收迟到/重传响应。 */
        t->state = TXN_ICT_COMPLETED;
        if (tw_del(m->cfg.tw, &t->tm_retr))
            t_unref(t);
        if (tw_del(m->cfg.tw, &t->tm_dead))
            t_unref(t);
        pthread_mutex_unlock(&s->lock);
        if (t->tu)
            t->tu(t, msg, TXN_EV_FINAL, t->ud);
        pthread_mutex_lock(&s->lock);
        if (!t->reliable) {
            arm(t, &t->tm_dead,
                m->cfg.timer_d_ms ? m->cfg.timer_d_ms : 32000, cb_dead,
                now);
        } else {
            t->dead = 1; /* TCP 无 D：立即摘 hash（在锁内安全） */
            nsbc_hnode_t **pp = bucket_for_hash(s, hash);
            while (*pp) {
                if (*pp == &t->link) {
                    *pp = t->link.next;
                    break;
                }
                pp = &(*pp)->next;
            }
        }
        pthread_mutex_unlock(&s->lock);
        if (t->dead && t->reliable)
            t_unref(t);
        break;
    case TXN_NICT_TRYING:
        if (provisional) {
            t->state = TXN_NICT_PROCEEDING;
            if (tw_del(m->cfg.tw, &t->tm_retr))
                t_unref(t); /* E 停；F 保留 */
            pthread_mutex_unlock(&s->lock);
            if (t->tu)
                t->tu(t, msg, TXN_EV_PROVISIONAL, t->ud);
            return;
        }
        goto nict_final;
    case TXN_NICT_PROCEEDING:
        if (provisional) {
            pthread_mutex_unlock(&s->lock);
            if (t->tu)
                t->tu(t, msg, TXN_EV_PROVISIONAL, t->ud);
            return;
        }
    nict_final:
        t->state = TXN_NICT_COMPLETED;
        if (tw_del(m->cfg.tw, &t->tm_retr))
            t_unref(t);
        if (tw_del(m->cfg.tw, &t->tm_dead))
            t_unref(t);
        pthread_mutex_unlock(&s->lock);
        if (t->tu)
            t->tu(t, msg, TXN_EV_FINAL, t->ud);
        pthread_mutex_lock(&s->lock);
        if (!t->reliable)
            arm(t, &t->tm_dead, m->cfg.t4_ms, cb_dead, now); /* K */
        else {
            t->dead = 1;
            nsbc_hnode_t **pp = bucket_for_hash(s, hash);
            while (*pp) {
                if (*pp == &t->link) {
                    *pp = t->link.next;
                    break;
                }
                pp = &(*pp)->next;
            }
        }
        pthread_mutex_unlock(&s->lock);
        if (t->dead && t->reliable)
            t_unref(t);
        break;
    default:
        /* COMPLETED/TERMINATED：吸收重传 */
        pthread_mutex_unlock(&s->lock);
        break;
    }
    (void)now;
}

int txn_client_input(txn_mgr_t *m, sip_msg_t *msg, const nsbc_sa_t *src,
                     int reliable, uint64_t now_ms)
{
    (void)src;
    (void)reliable;
    int cookie = 0;
    size_t blen = via_branch(msg, &cookie);
    if (!blen)
        return TXN_DROP;
    sip_hdr_t *cs = sip_hdr_get(msg, SIP_H_CSEQ);
    if (!cs || sip_hdr_parse_cseq(cs) != NSBC_OK)
        return TXN_DROP;
    char branch[BRANCH_MAX];
    /* 先尝试从 sip_parse 的 Via 取，不行就从 buf 手动搜 */
    sip_hdr_t *vh = sip_hdr_get(msg, SIP_H_VIA);
    int via_rc = vh ? sip_hdr_parse_via(vh) : NSBC_ERR;
    if (via_rc == NSBC_OK && vh->u.via.branch.p && vh->u.via.branch.len >= blen) {
        memcpy(branch, vh->u.via.branch.p, blen);
    } else {
        /* fallback: 从 buf 手动拷贝 branch */
        const char *p = msg->buf;
        const char *end = msg->buf + msg->buflen;
        while (p && end && p < end - 4) {
            if ((p[0] == 'V' || p[0] == 'v') &&
                (p[1] == 'I' || p[1] == 'i') &&
                (p[2] == 'A' || p[2] == 'a') &&
                (p[3] == ':' || p[3] == ' ' || p[3] == '\t')) {
                const char *q = p + 4;
                while (q < end - 7) {
                    if (strncasecmp(q, "branch=", 7) == 0) {
                        q += 7;
                        size_t i = 0;
                        while (q[i] && q[i] != ';' && q[i] != '\r' && q[i] != '\n' && q[i] != ' ' && i < blen) {
                            branch[i] = q[i];
                            i++;
                        }
                        break;
                    }
                    q++;
                }
                break;
            }
            p++;
        }
    }
    /* INVITE 响应 → 匹配 ICT；非 INVITE (REGISTER/OPTIONS) → 匹配 NICT */
    txn_kind_t client_kind =
        (cs->u.cseq.method == SIP_M_INVITE) ? TXN_ICT : TXN_NICT;
    uint64_t hash =
        txn_hash(branch, blen, cs->u.cseq.method, client_kind);
    txn_shard_t *s = shard_for_hash(m, hash);
    pthread_mutex_lock(&s->lock);
    sip_txn_t *t = find_locked(s, hash, branch, blen, cs->u.cseq.method,
                               client_kind);
    if (t)
        t_ref(t);
    pthread_mutex_unlock(&s->lock);
    if (!t)
        return TXN_DROP;
    client_on_response(m, t, msg, now_ms);
    t_unref(t);
    return NSBC_OK;
}

/* 服务端新事务创建（持分片锁调用，返回后已在表中） */
static sip_txn_t *server_create_locked(txn_mgr_t *m, sip_msg_t *msg,
                                       const char *branch, size_t blen,
                                       const nsbc_sa_t *src, int reliable,
                                       txn_kind_t kind, uint64_t hash)
{
    sip_txn_t *t = t_alloc(m, kind, msg->method);
    if (!t) {
        llog(LOG_WARN, "txn", "server_create: t_alloc FAILED slab=%p kind=%d method=%d",
             m->cfg.slab, kind, msg->method);
        return NULL;
    }
    memcpy(t->branch, branch, blen);
    t->blen = blen;
    t->peer = *src;
    t->reliable = reliable;
    t->state = (kind == TXN_IST) ? TXN_IST_PROCEEDING
                                 : TXN_NIST_TRYING;
    t->tu = NULL;
    t->req = copy_buf(m, msg->buf, msg->buflen);
    if (!t->req) {
        llog(LOG_WARN, "txn", "server_create: copy_buf FAILED pkt_arena=%p buflen=%zu",
             m->cfg.pkt_arena, msg->buflen);
        mem_obj_release(t);
        return NULL;
    }
    t->req_len = msg->buflen;
    txn_shard_t *s = shard_for_hash(m, hash);
    nsbc_hnode_t **bucket = bucket_for_hash(s, hash);
    t->link.next = *bucket;
    *bucket = &t->link;
    t_ref(t); /* hash */
    return t;
}

int txn_server_input(txn_mgr_t *m, sip_msg_t *msg, const nsbc_sa_t *src,
                     int reliable, uint64_t now_ms)
{
    (void)now_ms;
    int cookie = 0;
    size_t blen = via_branch(msg, &cookie);
    if (!blen || !cookie)
        return TXN_STATELESS;

    char branch[BRANCH_MAX];
    {
        sip_hdr_t *vh = sip_hdr_get(msg, SIP_H_VIA);
        sip_tok_t b = vh ? vh->u.via.branch : (sip_tok_t){0};
        /* 防御：sip_hdr_parse_via 可能失败（branch.p==NULL），via_branch 走了 buf 手动路径返回 blen */
        if (b.p && b.len >= blen) {
            memcpy(branch, b.p, blen);
        } else {
            /* fallback: 从 buf 手动拷贝 branch */
            const char *p = msg->buf;
            const char *end = msg->buf + msg->buflen;
            while (p && end && p < end - 4) {
                if ((p[0] == 'V' || p[0] == 'v') &&
                    (p[1] == 'I' || p[1] == 'i') &&
                    (p[2] == 'A' || p[2] == 'a') &&
                    (p[3] == ':' || p[3] == ' ' || p[3] == '\t')) {
                    const char *q = p + 4;
                    while (q < end - 7) {
                        if (strncasecmp(q, "branch=", 7) == 0) {
                            q += 7;
                            size_t i = 0;
                            while (q[i] && q[i] != ';' && q[i] != '\r' &&
                                   q[i] != '\n' && q[i] != ' ' && i < blen) {
                                branch[i] = q[i];
                                i++;
                            }
                            break;
                        }
                        q++;
                    }
                    break;
                }
                p++;
            }
        }
    }
    /* ACK 按 RFC3261 §17.2.3 匹配到同 branch 的 IST（INVITE 键 + IST kind） */
    sip_method_t key_meth =
        (msg->method == SIP_M_ACK) ? SIP_M_INVITE : msg->method;
    /* server 侧 kind：INVITE/ACK→IST，非 INVITE→NIST */
    txn_kind_t server_kind =
        (key_meth == SIP_M_INVITE) ? TXN_IST : TXN_NIST;
    uint64_t hash = txn_hash(branch, blen, key_meth, server_kind);
    txn_shard_t *s = shard_for_hash(m, hash);
    pthread_mutex_lock(&s->lock);
    sip_txn_t *t = find_locked(s, hash, branch, blen, key_meth, server_kind);
    if (t)
        t_ref(t);
    if (!t) {
        if (msg->method == SIP_M_ACK) {
            /* 无对应 INVITE 事务：交由 TU 对话层（2xx 重传吸收在对话核心） */
            pthread_mutex_unlock(&s->lock);
            return TXN_STATELESS;
        }
        txn_kind_t kind =
            (msg->method == SIP_M_INVITE) ? TXN_IST : TXN_NIST;
        t = server_create_locked(m, msg, branch, blen, src, reliable, kind,
                                 hash);
        if (t) {
            t_ref(t); /* 临持给锁外 TU 回调 */
            /* 兜底 Timer：IST/NIST txn 创建后如果后续没走到 server_respond(status>=200)
             * （ACL drop / profile deny / 号码不存在 / b2bua early return 等），
             * tm_dead 永远不会被 arm，导致 refs=hash(1) 永不释放 → slab 泄漏！
             * 先 arm tm_dead 兜底：IST 用 Timer P (64*T2=256s)，NIST 用 Timer F (64*T1=32s)。
             * 后续 server_respond(status>=200) arm Timer H/I/J/K 之前先 tw_del 取消兜底。 */
            uint32_t dead_ms;
            if (kind == TXN_IST)
                dead_ms = 64u * m->cfg.t2_ms;     /* Timer P: 256s 默认 */
            else
                dead_ms = 64u * m->cfg.t1_ms;     /* Timer F: 32s 默认 */
            arm(t, &t->tm_dead, dead_ms, cb_dead, now_ms);
        }
        pthread_mutex_unlock(&s->lock);
        if (!t)
            return NSBC_NOMEM;
        if (m->cfg.server_tu)
            m->cfg.server_tu(t, msg, TXN_EV_NEW_REQUEST, m->cfg.server_ud);
        t_unref(t); /* 临持 */
        return TXN_NEW;
    }
    pthread_mutex_unlock(&s->lock);

    /* 命中既有事务：重传 / ACK 吸收 */
    pthread_mutex_lock(&s->lock);
    if (msg->method == SIP_M_ACK && t->kind == TXN_IST) {
        if (t->state == TXN_IST_COMPLETED) {
            t->state = TXN_IST_CONFIRMED;
            if (tw_del(m->cfg.tw, &t->tm_retr))
                t_unref(t); /* G */
            if (tw_del(m->cfg.tw, &t->tm_dead))
                t_unref(t); /* H */
            pthread_mutex_unlock(&s->lock);
            if (m->cfg.server_tu)
                m->cfg.server_tu(t, msg, TXN_EV_ACK, m->cfg.server_ud);
            pthread_mutex_lock(&s->lock);
            if (!t->reliable)
                arm(t, &t->tm_dead, m->cfg.t4_ms, cb_dead, now_ms); /* I */
            else {
                t->dead = 1;
                nsbc_hnode_t **pp = bucket_for_hash(s, hash);
                while (*pp) {
                    if (*pp == &t->link) {
                        *pp = t->link.next;
                        break;
                    }
                    pp = &(*pp)->next;
                }
            }
            __atomic_add_fetch(&m->rin, 1, __ATOMIC_RELAXED);
            pthread_mutex_unlock(&s->lock);
            if (t->dead && t->reliable)
                t_unref(t);
        } else {
            /* PROCEEDING 中收到 ACK（对非 2xx 的 ACK 不应到 IST） */
            pthread_mutex_unlock(&s->lock);
        }
    } else if (t->resp) {
        /* 请求重传：重发最近响应。
         * 对 REGISTER 不再放行 TU —— 以前 registrar 绕过 txn 直接 sip_tr_send
         * 导致 t->resp 永远 NULL，auth challenge retry 只能靠 branch 复用命中。
         * 现在 registrar 走 txn_send_response，401/200 都会让 NIST 进入 COMPLETED，
         * 后续同 branch 包一律视为重传，重放 resp。client 侧 outreg 每次 auth retry
         * 都新建 txn（新 branch），走正常 NEW 路径。 */
        t_send(t, t->resp, t->resp_len);
        __atomic_add_fetch(&m->rin, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&m->rout, 1, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&s->lock);
    } else {
        /* 尚无响应可重发：说明 registrar 还在处理中，吸收重传，避免重复 TU */
        __atomic_add_fetch(&m->rin, 1, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&s->lock);
    }
    t_unref(t);
    return NSBC_OK;
}

int txn_ist_absorb_ack(sip_txn_t *t, uint64_t now_ms)
{
    (void)now_ms;
    txn_mgr_t *m = t->mgr;
    txn_shard_t *s = shard_for(m, t->branch, t->blen, t->method, t->kind);
    pthread_mutex_lock(&s->lock);
    if (t->kind != TXN_IST || t->state != TXN_IST_COMPLETED) {
        pthread_mutex_unlock(&s->lock);
        llog(LOG_WARN, "txn",
             "absorb_ack: skip (kind=%d want=IST state=%d want=COMPLETED)",
             t->kind, t->state);
        return 0;
    }
    t->state = TXN_IST_CONFIRMED;
    int g_del = tw_del(m->cfg.tw, &t->tm_retr);
    int h_del = tw_del(m->cfg.tw, &t->tm_dead);
    llog(LOG_INFO, "txn",
         "absorb_ack: OK, disarm Timer G=%d H=%d branch=%.*s",
         g_del, h_del, (int)t->blen, t->branch);
    if (g_del)
        t_unref(t); /* G */
    if (h_del)
        t_unref(t); /* H */
    pthread_mutex_unlock(&s->lock);
    if (!t->reliable)
        arm(t, &t->tm_dead, m->cfg.t4_ms, cb_dead, now_ms); /* I */
    return 1;
}

/* 公共 API：强制解除 IST 的 Timer H/G。
 * 场景：ACK 因 branch 不同（RFC 合规）被 txn_server_input 返回
 * TXN_STATELESS，对话层调用 txn_ist_absorb_ack 但可能因状态检查
 * 失败（竞态）返回 0 —— 调用方必须再调本函数确保 Timer H 被解除，
 * 否则 32 秒后 Timer H 触发 TXN_EV_TIMEOUT → 发送 BYE 挂断。 */
void txn_ist_force_disarm_h(sip_txn_t *t)
{
    if (!t) return;
    txn_mgr_t *m = t->mgr;
    txn_shard_t *s = shard_for(m, t->branch, t->blen, t->method, t->kind);
    pthread_mutex_lock(&s->lock);
    int g_del = tw_del(m->cfg.tw, &t->tm_retr);
    int h_del = tw_del(m->cfg.tw, &t->tm_dead);
    if (t->kind == TXN_IST && t->state == TXN_IST_COMPLETED)
        t->state = TXN_IST_CONFIRMED;
    /* 在锁内 capture 所有日志字段（t_unref 可能释放内存，之后不可访问） */
    int st = t->state;
    size_t bl = t->blen;
    const char *br = t->branch;
    pthread_mutex_unlock(&s->lock);
    llog(LOG_WARN, "txn",
         "ist_force_disarm_h: disarm G=%d H=%d state=%d branch=%.*s",
         g_del, h_del, st, (int)bl, br);
    if (g_del) t_unref(t);
    if (h_del) t_unref(t);
}

int txn_send_response(sip_txn_t *t, const char *buf, size_t len,
                      uint64_t now_ms)
{
    txn_mgr_t *m = t->mgr;
    /* 解析状态码："SIP/2.0 NNN ..." */
    int status = 0;
    if (len > 12 && !strncmp(buf, "SIP/2.0 ", 8))
        status = atoi(buf + 8);
    if (status < 100)
        return NSBC_PROTO;

    uint64_t hash = txn_hash(t->branch, t->blen, t->method, t->kind);
    txn_shard_t *s = shard_for_hash(m, hash);
    pthread_mutex_lock(&s->lock);
    char *nb = copy_buf(m, buf, len);
    if (nb) {
        if (t->resp)
            mem_arena_free(m->cfg.pkt_arena, t->resp);
        t->resp = nb;
        t->resp_len = len;
    }
    t_send(t, buf, len);

    if (t->kind == TXN_IST) {
        if (status >= 200 && t->state == TXN_IST_PROCEEDING) {
            t->state = TXN_IST_COMPLETED;
            t->interval = m->cfg.t1_ms;
            /* 取消 TXN_NEW 时 arm 的兜底 Timer P，然后 arm 正常 Timer H */
            if (tw_del(m->cfg.tw, &t->tm_dead))
                t_unref(t); /* 抵消 arm 时的 ref */
            if (!t->reliable) {
                arm(t, &t->tm_retr, m->cfg.t1_ms, cb_retr, now_ms); /* G */
            }
            arm(t, &t->tm_dead, 64u * m->cfg.t1_ms, cb_dead, now_ms); /* H */
        }
        /* 1xx：留 PROCEEDING，兜底 Timer P 继续 tick；重传由请求重放触发 */
    } else { /* NIST */
        if (status < 200) {
            t->state = TXN_NIST_PROCEEDING;
        } else if (t->state != TXN_NIST_COMPLETED) {
            t->state = TXN_NIST_COMPLETED;
            /* 取消 TXN_NEW 时 arm 的兜底 Timer F，然后 arm 正常 Timer J */
            if (tw_del(m->cfg.tw, &t->tm_dead))
                t_unref(t); /* 抵消 arm 时的 ref */
            if (!t->reliable) {
                arm(t, &t->tm_dead, 64u * m->cfg.t1_ms, cb_dead,
                    now_ms); /* J */
            } else {
                t->dead = 1;
                nsbc_hnode_t **pp = bucket_for_hash(s, hash);
                while (*pp) {
                    if (*pp == &t->link) {
                        *pp = t->link.next;
                        break;
                    }
                    pp = &(*pp)->next;
                }
            }
        }
    }
    pthread_mutex_unlock(&s->lock);
    if (t->dead)
        t_unref(t); /* TCP 终态摘表 */
    return NSBC_OK;
}

const char *txn_first_request(const sip_txn_t *t, size_t *len)
{
    if (len)
        *len = t->req_len;
    return t->req;
}
txn_state_t txn_state(const sip_txn_t *t) { return t->state; }
txn_kind_t txn_kind(const sip_txn_t *t) { return t->kind; }
const nsbc_sa_t *txn_peer(const sip_txn_t *t) { return &t->peer; }
void *txn_ud(const sip_txn_t *t) { return t->ud; }
void txn_set_ud(sip_txn_t *t, void *ud) { t->ud = ud; }
void txn_set_tu(sip_txn_t *t, txn_tu_fn tu) { t->tu = tu; }

const char *txn_state_name(txn_state_t s)
{
    switch (s) {
    case TXN_ICT_CALLING: return "ICT_CALLING";
    case TXN_ICT_PROCEEDING: return "ICT_PROCEEDING";
    case TXN_ICT_COMPLETED: return "ICT_COMPLETED";
    case TXN_ICT_TERMINATED: return "ICT_TERMINATED";
    case TXN_NICT_TRYING: return "NICT_TRYING";
    case TXN_NICT_PROCEEDING: return "NICT_PROCEEDING";
    case TXN_NICT_COMPLETED: return "NICT_COMPLETED";
    case TXN_NICT_TERMINATED: return "NICT_TERMINATED";
    case TXN_IST_PROCEEDING: return "IST_PROCEEDING";
    case TXN_IST_COMPLETED: return "IST_COMPLETED";
    case TXN_IST_CONFIRMED: return "IST_CONFIRMED";
    case TXN_IST_TERMINATED: return "IST_TERMINATED";
    case TXN_NIST_TRYING: return "NIST_TRYING";
    case TXN_NIST_PROCEEDING: return "NIST_PROCEEDING";
    case TXN_NIST_COMPLETED: return "NIST_COMPLETED";
    case TXN_NIST_TERMINATED: return "NIST_TERMINATED";
    default: return "NONE";
    }
}

/* BYE→200 OK 后主动 teardown txn，不等 Timer H/B/F 自然超时。
 * 幂等：t_remove_hash 内部已检查 t->dead，多次调用安全。 */
void txn_force_terminate(sip_txn_t *t)
{
    if (!t || t->dead)
        return;
    /* 跳过 cb_dead 的 TU 回调（call 已销毁，不需要再通知），
     * 直接置 TERMINATED + t_remove_hash 做：tw_del Timer G/H/B/F + 摘链 + unref */
    t->state = (t->kind == TXN_ICT) ? TXN_ICT_TERMINATED
             : (t->kind == TXN_IST) ? TXN_IST_TERMINATED
             : TXN_NICT_TERMINATED;
    t_remove_hash(t);
}

uint64_t txn_mgr_active(txn_mgr_t *m)
{
    return __atomic_load_n(&m->active, __ATOMIC_RELAXED);
}
uint64_t txn_mgr_retrans_in(txn_mgr_t *m)
{
    return __atomic_load_n(&m->rin, __ATOMIC_RELAXED);
}
uint64_t txn_mgr_retrans_out(txn_mgr_t *m)
{
    return __atomic_load_n(&m->rout, __ATOMIC_RELAXED);
}
uint64_t txn_mgr_timeouts(txn_mgr_t *m)
{
    return __atomic_load_n(&m->tmo, __ATOMIC_RELAXED);
}
uint64_t txn_mgr_capacity(txn_mgr_t *m)
{
    if (!m || !m->cfg.slab) return 0;
    return (uint64_t)mem_slab_capacity(m->cfg.slab);
}
double txn_mgr_usage(txn_mgr_t *m)
{
    uint64_t cap = txn_mgr_capacity(m);
    if (cap == 0) return 0.0;
    uint64_t active = __atomic_load_n(&m->active, __ATOMIC_RELAXED);
    return (double)active / (double)cap;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
