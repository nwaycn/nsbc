/* call_session.c — 呼叫会话管理：parse 与 mem 模块结合（7.5/7.1.5） */
#include "call_session.h"

#include <pthread.h>
#include "nsbc_atomic.h"

#define CALL_HASH_BUCKETS 256
#define CALLID_MAX 96
#define TAG_MAX 64

#define DEFAULT_MAX_CALLS 4096
#define DEFAULT_SIG_BYTES (64ull * 1024 * 1024)
#define DEFAULT_PKT_BYTES (128ull * 1024 * 1024)
#define DEFAULT_MED_BYTES (64ull * 1024 * 1024)

/* ---- refcounted 收包缓冲（固定 SIP_MAX_MSG 大小的 slab 对象） ---- */
typedef struct pktbuf {
    uint32_t refs;
    uint32_t len;
    uint8_t data[SIP_MAX_MSG];
} pktbuf_t;

struct call_session {
    char call_id[CALLID_MAX];
    char from_tag[TAG_MAX];
    char to_tag[TAG_MAX];

    call_state_t state;
    call_media_t media;

    rtp_gate_t gate;
    rtp_ssrc_guard_t ssrc;
    sip_upd_guard_t upd;

    int precond_active;
    int precond_met;
    uint64_t created_ms;
    uint64_t connected_ms;
    uint64_t ended_ms;

    call_mgr_t *mgr;
    call_session_t *hnext;
    int in_table;
};

struct call_mgr {
    mem_arena_t *sig, *pkt, *med;
    mem_slab_t *sip_slab, *sdp_slab, *sess_slab, *pktbuf_slab;
    mem_rcu_t *rcu;

    struct {
        pthread_mutex_t mu;
        call_session_t *head;
    } buckets[CALL_HASH_BUCKETS];

    uint64_t active;
    uint64_t peak;
    uint64_t created;
    uint64_t ended;
    uint64_t recycled;
    uint64_t failed;
};

struct call_worker {
    call_mgr_t *mgr;
    mem_mag_t *mag;
    mem_rcu_reader_t *rd;
};

/* ---------------- helpers ---------------- */

static uint32_t hash_callid(const char *p, size_t len)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)p[i];
        h *= 16777619u;
    }
    return h % CALL_HASH_BUCKETS;
}

static void tok_copy(char *dst, size_t cap, sip_tok_t t)
{
    if (t.p == NULL) {
        dst[0] = 0;
        return;
    }
    size_t n = t.len < cap - 1 ? t.len : cap - 1;
    memcpy(dst, t.p, n);
    dst[n] = 0;
}

static int callid_eq(const call_session_t *cs, const char *id, size_t len)
{
    return strlen(cs->call_id) == len && memcmp(cs->call_id, id, len) == 0;
}

/* ---------------- pktbuf ---------------- */

static pktbuf_t *pktbuf_get(call_worker_t *w)
{
    pktbuf_t *pb = (pktbuf_t *)mem_mag_alloc(w->mag, w->mgr->pktbuf_slab);
    if (pb) {
        __atomic_store_n((&pb->refs), (1), __ATOMIC_SEQ_CST);
        pb->len = 0;
    }
    return pb;
}

static void pktbuf_unref(void *p)
{
    pktbuf_t *pb = (pktbuf_t *)p;
    uint32_t r = __atomic_fetch_sub((&pb->refs), (1), __ATOMIC_SEQ_CST);
    if (r == 1) {
        /* 引用归零：不要求归还原线程，直接还共享池 */
        mem_obj_release(pb);
    }
}

/* ---------------- manager create/destroy ---------------- */

call_mgr_t *call_mgr_create(const call_mgr_cfg_t *cfg)
{
    call_mgr_cfg_t def = {0};
    if (!cfg)
        cfg = &def;

    int max_calls = cfg->max_calls > 0 ? cfg->max_calls : DEFAULT_MAX_CALLS;
    int pktbufs = cfg->pktbuf_count > 0 ? cfg->pktbuf_count : max_calls * 2;

    call_mgr_t *m = (call_mgr_t *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;

    mem_arena_cfg_t ac = {0};
    ac.bytes = cfg->signal_bytes ? cfg->signal_bytes : DEFAULT_SIG_BYTES;
    ac.domain = MEM_DOMAIN_SIGNAL;
    ac.hugepage = 1;
    ac.debug = 1;
    if (mem_arena_create(&ac, &m->sig) != NSBC_OK)
        goto fail0;

    ac.bytes = cfg->packet_bytes ? cfg->packet_bytes : DEFAULT_PKT_BYTES;
    ac.domain = MEM_DOMAIN_PACKET;
    if (mem_arena_create(&ac, &m->pkt) != NSBC_OK)
        goto fail1;

    ac.bytes = cfg->media_bytes ? cfg->media_bytes : DEFAULT_MED_BYTES;
    ac.domain = MEM_DOMAIN_MEDIA;
    if (mem_arena_create(&ac, &m->med) != NSBC_OK)
        goto fail2;

    for (int i = 0; i < CALL_HASH_BUCKETS; i++) {
        if (pthread_mutex_init(&m->buckets[i].mu, NULL) != 0)
            goto fail3;
    }

    m->rcu = mem_rcu_create();
    if (!m->rcu)
        goto fail3;

    mem_slab_cfg_t sc = {0};
    sc.arena = m->sig;

    sc.name = "sip_msg";
    sc.obj_size = sizeof(sip_msg_t);
    sc.capacity = max_calls * 2;
    sc.batch = 8;
    if (mem_slab_create(&sc, &m->sip_slab) != NSBC_OK)
        goto fail4;

    sc.name = "sdp";
    sc.obj_size = sizeof(sdp_t);
    sc.capacity = max_calls;
    sc.batch = 8;
    if (mem_slab_create(&sc, &m->sdp_slab) != NSBC_OK)
        goto fail4;

    sc.name = "call_session";
    sc.obj_size = sizeof(call_session_t);
    sc.capacity = max_calls;
    sc.batch = 8;
    if (mem_slab_create(&sc, &m->sess_slab) != NSBC_OK)
        goto fail4;

    sc.name = "pktbuf";
    sc.obj_size = sizeof(pktbuf_t);
    sc.capacity = pktbufs;
    sc.batch = 8;
    if (mem_slab_create(&sc, &m->pktbuf_slab) != NSBC_OK)
        goto fail4;

    return m;

fail4:
    mem_rcu_destroy(m->rcu);
fail3:
    for (int i = 0; i < CALL_HASH_BUCKETS; i++)
        pthread_mutex_destroy(&m->buckets[i].mu);
fail2:
    mem_arena_destroy(m->med);
fail1:
    mem_arena_destroy(m->sig);
fail0:
    free(m);
    return NULL;
}

void call_mgr_destroy(call_mgr_t *m)
{
    if (!m)
        return;
    /* 注：调用契约要求所有 worker 已先销毁、无在表会话；
     * RCU 先于 slab 销毁：强制释放残留 limbo 对象时要归还 slab。 */
    mem_rcu_destroy(m->rcu);
    mem_slab_destroy(m->pktbuf_slab);
    mem_slab_destroy(m->sess_slab);
    mem_slab_destroy(m->sdp_slab);
    mem_slab_destroy(m->sip_slab);
    for (int i = 0; i < CALL_HASH_BUCKETS; i++)
        pthread_mutex_destroy(&m->buckets[i].mu);
    mem_arena_destroy(m->pkt);
    mem_arena_destroy(m->med);
    mem_arena_destroy(m->sig);
    free(m);
}

/* ---------------- worker ---------------- */

call_worker_t *call_worker_create(call_mgr_t *m)
{
    if (!m)
        return NULL;
    call_worker_t *w = (call_worker_t *)calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->mgr = m;
    w->mag = mem_mag_create();
    if (!w->mag) {
        free(w);
        return NULL;
    }
    w->rd = mem_rcu_thread_join(m->rcu);
    if (!w->rd) {
        mem_mag_destroy(w->mag);
        free(w);
        return NULL;
    }
    return w;
}

void call_worker_destroy(call_worker_t *w)
{
    if (!w)
        return;
    mem_rcu_thread_leave(w->rd);
    mem_mag_destroy(w->mag);
    free(w);
}

void call_worker_read_lock(call_worker_t *w)
{
    mem_rcu_read_lock(w->rd);
}

void call_worker_read_unlock(call_worker_t *w)
{
    mem_rcu_read_unlock(w->rd);
}

/* ---------------- hash table ---------------- */

static call_session_t *table_find(call_mgr_t *m, const char *id, size_t len,
                                  uint32_t b)
{
    call_session_t *cs;
    pthread_mutex_lock(&m->buckets[b].mu);
    for (cs = m->buckets[b].head; cs; cs = cs->hnext)
        if (callid_eq(cs, id, len))
            break;
    pthread_mutex_unlock(&m->buckets[b].mu);
    return cs;
}

static void table_insert(call_mgr_t *m, call_session_t *cs, uint32_t b)
{
    pthread_mutex_lock(&m->buckets[b].mu);
    cs->hnext = m->buckets[b].head;
    m->buckets[b].head = cs;
    cs->in_table = 1;
    pthread_mutex_unlock(&m->buckets[b].mu);
}

static void table_unlink(call_mgr_t *m, call_session_t *cs, uint32_t b)
{
    pthread_mutex_lock(&m->buckets[b].mu);
    call_session_t **pp = &m->buckets[b].head;
    while (*pp && *pp != cs)
        pp = &(*pp)->hnext;
    if (*pp) {
        *pp = cs->hnext;
        cs->in_table = 0;
    }
    pthread_mutex_unlock(&m->buckets[b].mu);
}

call_session_t *call_mgr_find(call_mgr_t *m, const char *callid, size_t len)
{
    if (!m || !callid || len == 0)
        return NULL;
    return table_find(m, callid, len, hash_callid(callid, len));
}

/* ---------------- session create/end ---------------- */

static call_session_t *session_new(call_worker_t *w, const char *id,
                                   size_t idlen, const sip_msg_t *m)
{
    call_mgr_t *mg = w->mgr;
    call_session_t *cs =
        (call_session_t *)mem_mag_alloc(w->mag, mg->sess_slab);
    if (!cs)
        return NULL;
    memset(cs, 0, sizeof(*cs)); /* 复用对象：全量重置 */

    tok_copy(cs->call_id, sizeof(cs->call_id), (sip_tok_t){id, idlen});

    sip_hdr_t *fh = sip_hdr_get(m, SIP_H_FROM);
    if (fh) {
        sip_hdr_parse_addr(fh);
        tok_copy(cs->from_tag, sizeof(cs->from_tag), fh->u.addr.tag);
    }

    cs->state = (CALL_STATE_INIT);
    rtp_gate_init(&cs->gate);
    rtp_ssrc_guard_init(&cs->ssrc);
    sip_upd_guard_init(&cs->upd);
    cs->created_ms = nsbc_now_ms();
    cs->mgr = mg;

    table_insert(mg, cs, hash_callid(id, idlen));

    uint64_t a = __atomic_fetch_add((&mg->active), (1), __ATOMIC_SEQ_CST) + 1;
    __atomic_fetch_add((&mg->created), (1), __ATOMIC_SEQ_CST);
    uint64_t pk = __atomic_load_n((&mg->peak), __ATOMIC_SEQ_CST);
    while (a > pk && !__atomic_compare_exchange_n((&mg->peak), (&pk), (a), 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        ;
    return cs;
}

static void retire_recycle(void *p)
{
    call_session_t *cs = (call_session_t *)p;
    __atomic_fetch_add((&cs->mgr->recycled), (1), __ATOMIC_SEQ_CST);
    mem_obj_release(p);
}

static void session_end(call_session_t *cs, int failed)
{
    call_mgr_t *m = cs->mgr;

    call_state_t prev = __atomic_exchange_n((&cs->state), (CALL_STATE_TERMINATED), __ATOMIC_SEQ_CST);
    if (prev == CALL_STATE_TERMINATED)
        return; /* 幂等：重复终止 */

    cs->ended_ms = nsbc_now_ms();
    if (cs->in_table)
        table_unlink(m, cs, hash_callid(cs->call_id, strlen(cs->call_id)));

    __atomic_fetch_sub((&m->active), (1), __ATOMIC_SEQ_CST);
    __atomic_fetch_add((&m->ended), (1), __ATOMIC_SEQ_CST);
    if (failed)
        __atomic_fetch_add((&m->failed), (1), __ATOMIC_SEQ_CST);

    /* 先摘链，再经宽限期回收：读侧在此期间仍可安全持有指针 */
    mem_rcu_retire(m->rcu, cs, retire_recycle);
}

/* ---------------- apply SDP ---------------- */

/* 返回：1=成功；0=SDP 畸形；-1=对象池耗尽。
 * *held=媒体最终不可通（inactive/rejected/legacy hold）；
 * *remote_changed=对端媒体端口发生变化。 */
static int apply_sdp(call_worker_t *w, call_session_t *cs, const sip_msg_t *m,
                     int *held, int *remote_changed)
{
    *held = 0;
    *remote_changed = 0;

    if (m->body.len == 0)
        return 1;

    sdp_t *s = (sdp_t *)mem_mag_alloc(w->mag, w->mgr->sdp_slab);
    if (!s)
        return -1;

    int rc = 1;
    if (sdp_parse(s, m->body.p, m->body.len) != NSBC_OK) {
        rc = 0;
        goto out;
    }
    if (s->nmedia == 0)
        goto out; /* 无媒体段：成功无操作 */

    sdp_media_t *mm = &s->media[0];
    int old_port = cs->media.remote_port;

    sdp_media_build_codecs(s, mm);

    call_media_t c;
    memset(&c, 0, sizeof(c));
    c.remote_port = mm->port;
    c.dir = sdp_media_effective_dir(s, mm);
    c.t38 = sdp_media_is_t38(mm);

    sip_tok_t algo, fp;
    if (sdp_fingerprint_get(s, mm, &algo, &fp) == NSBC_OK)
        c.secure = 1;
    else
        c.secure = sdp_media_has_crypto(mm) ? 1 : 0;

    if (sdp_attr_find(mm->attrs, mm->nattrs, "rtcp-mux"))
        c.rtcp_mux = 1;

    int n = mm->ncodecs > 4 ? 4 : mm->ncodecs;
    for (int i = 0; i < n; i++)
        tok_copy(c.codec[i], sizeof(c.codec[i]), mm->codecs[i].enc);
    c.ncodec = n;

    /* 前提条件：Require precondition 或存在 curr 即构建 */
    int pc_active = 0, pc_met = 0;
    sip_hdr_t *rq = sip_hdr_get(m, SIP_H_REQUIRE);
    if ((rq && sip_hdr_has_option(rq, "precondition")) ||
        sdp_attr_find(mm->attrs, mm->nattrs, "curr")) {
        if (sdp_media_build_preconds(s, mm) != NSBC_OK) {
            rc = 0;
            goto out;
        }
        pc_active = sdp_media_preconditions_active(mm);
        pc_met = sdp_media_preconditions_met(mm);
    }
    cs->precond_active = pc_active;
    cs->precond_met = pc_met;

    int dir_open = c.dir != SDP_DIR_INACTIVE && !mm->rejected &&
                   !sdp_media_is_legacy_hold(s, mm);
    *held = !dir_open;

    int early = __atomic_load_n((&cs->state), __ATOMIC_SEQ_CST) == CALL_STATE_EARLY;
    rtp_gate_update(&cs->gate, pc_active, pc_met, early, dir_open);

    if (old_port != 0 && mm->port != old_port)
        *remote_changed = 1;

    cs->media = c;

out:
    mem_mag_free(w->mag, s);
    return rc;
}

/* ---------------- dispatch ---------------- */

int call_worker_dispatch(call_worker_t *w, const void *wire, size_t len,
                         call_msg_result_t *out)
{
    if (!w || !wire || !out)
        return NSBC_INVAL;
    memset(out, 0, sizeof(*out));
    out->ev = CALL_EV_NONE;
    out->state = CALL_STATE_INIT;

    if (len == 0 || len > SIP_MAX_MSG) {
        out->ev = CALL_EV_BADMSG;
        return NSBC_OK;
    }

    /* 1) 收包缓冲：packet 域 slab，引用计数（拷贝只发生在收包边界一次） */
    pktbuf_t *pb = pktbuf_get(w);
    if (!pb) {
        out->ev = CALL_EV_BUSY;
        return NSBC_OK;
    }
    memcpy(pb->data, wire, len);
    pb->len = (uint32_t)len;

    /* 2) SIP 对象：信令域 slab 经 magazine */
    sip_msg_t *m = (sip_msg_t *)mem_mag_alloc(w->mag, w->mgr->sip_slab);
    if (!m) {
        pktbuf_unref(pb);
        out->ev = CALL_EV_BUSY;
        return NSBC_OK;
    }

    if (sip_parse(m, (char *)pb->data, len) != NSBC_OK) {
        out->ev = CALL_EV_BADMSG;
        goto done;
    }

    /* 3) 稳定关联键：Call-ID */
    sip_hdr_t *idh = sip_hdr_get(m, SIP_H_CALL_ID);
    if (!idh) {
        out->ev = CALL_EV_BADMSG;
        goto done;
    }
    const char *id = idh->value.p;
    size_t idlen = idh->value.len;
    call_session_t *cs = table_find(w->mgr, id, idlen, hash_callid(id, idlen));
    out->session = cs;

    call_event_t ev = CALL_EV_NONE;
    int held = 0, changed = 0;

    if (m->is_request) {
        switch (m->method) {
        case SIP_M_INVITE:
            if (!sip_msg_in_dialog(m)) {
                /* 初始 INVITE（To 无 tag） */
                if (cs)
                    break; /* 重传：状态不变 */
                cs = session_new(w, id, idlen, m);
                if (!cs) {
                    ev = CALL_EV_BUSY;
                    break;
                }
                int r = apply_sdp(w, cs, m, &held, &changed);
                if (r == 0) {
                    session_end(cs, 1);
                    ev = CALL_EV_FAILED;
                } else if (r < 0) {
                    session_end(cs, 1);
                    ev = CALL_EV_FAILED;
                } else {
                    __atomic_store_n((&cs->state), (CALL_STATE_TRYING), __ATOMIC_SEQ_CST);
                    ev = CALL_EV_CREATED;
                }
            } else {
                /* in-dialog re-INVITE */
                if (!cs)
                    break;
                if (!sip_upd_begin(&cs->upd)) {
                    ev = CALL_EV_GLARE;
                    break;
                }
                int r = apply_sdp(w, cs, m, &held, &changed);
                sip_upd_end(&cs->upd);
                if (r == 0) {
                    ev = CALL_EV_BADMSG;
                } else if (held) {
                    __atomic_store_n((&cs->state), (CALL_STATE_HELD), __ATOMIC_SEQ_CST);
                    ev = CALL_EV_HELD;
                } else {
                    call_state_t ps = __atomic_load_n((&cs->state), __ATOMIC_SEQ_CST);
                    if (ps == CALL_STATE_HELD)
                        ev = CALL_EV_RESUMED;
                    else
                        ev = CALL_EV_UPDATED;
                    __atomic_store_n((&cs->state), (CALL_STATE_CONNECTED), __ATOMIC_SEQ_CST);
                    if (changed)
                        rtp_ssrc_guard_retarget(&cs->ssrc);
                }
            }
            break;

        case SIP_M_UPDATE:
            if (!cs)
                break;
            if (!sip_upd_begin(&cs->upd)) {
                ev = CALL_EV_GLARE;
                break;
            }
            int r2 = apply_sdp(w, cs, m, &held, &changed);
            sip_upd_end(&cs->upd);
            if (r2 == 0) {
                ev = CALL_EV_BADMSG;
            } else if (held) {
                __atomic_store_n((&cs->state), (CALL_STATE_HELD), __ATOMIC_SEQ_CST);
                ev = CALL_EV_HELD;
            } else {
                call_state_t ps2 = __atomic_load_n((&cs->state), __ATOMIC_SEQ_CST);
                ev = (ps2 == CALL_STATE_HELD) ? CALL_EV_RESUMED
                                             : CALL_EV_UPDATED;
                __atomic_store_n((&cs->state), (CALL_STATE_CONNECTED), __ATOMIC_SEQ_CST);
                if (changed)
                    rtp_ssrc_guard_retarget(&cs->ssrc);
            }
            break;

        case SIP_M_PRACK:
            if (cs)
                ev = CALL_EV_EARLY;
            break;

        case SIP_M_ACK:
            if (cs && __atomic_load_n((&cs->state), __ATOMIC_SEQ_CST) == CALL_STATE_CONNECTED)
                ev = CALL_EV_CONNECTED;
            break;

        case SIP_M_CANCEL:
            if (cs && __atomic_load_n((&cs->state), __ATOMIC_SEQ_CST) < CALL_STATE_CONNECTED) {
                session_end(cs, 0);
                ev = CALL_EV_ENDED;
            }
            break;

        case SIP_M_BYE:
            if (cs) {
                session_end(cs, 0);
                ev = CALL_EV_ENDED;
            }
            break;

        default:
            ev = CALL_EV_NONE;
            break;
        }
    } else {
        /* response */
        int code = m->status;
        sip_hdr_t *cseqh = sip_hdr_get(m, SIP_H_CSEQ);
        sip_method_t cseqm = SIP_M_UNKNOWN;
        if (cseqh && sip_hdr_parse_cseq(cseqh) == NSBC_OK)
            cseqm = cseqh->u.cseq.method;

        if (code == 100) {
            if (cs) {
                __atomic_store_n((&cs->state), (CALL_STATE_TRYING), __ATOMIC_SEQ_CST);
                ev = CALL_EV_PROGRESS;
            }
        } else if (code >= 180 && code < 200) {
            if (!cs)
                goto done;
            sip_hdr_t *th = sip_hdr_get(m, SIP_H_TO);
            if (th) {
                sip_hdr_parse_addr(th);
                if (cs->to_tag[0] == 0)
                    tok_copy(cs->to_tag, sizeof(cs->to_tag),
                             th->u.addr.tag);
            }
            int r3 = apply_sdp(w, cs, m, &held, &changed);
            if (r3 == 0) {
                ev = CALL_EV_BADMSG;
            } else {
                __atomic_store_n((&cs->state), (CALL_STATE_EARLY), __ATOMIC_SEQ_CST);
                ev = CALL_EV_EARLY;
            }
        } else if (code >= 200 && code < 300) {
            if (cseqm == SIP_M_INVITE) {
                if (!cs)
                    goto done;
                int r4 = apply_sdp(w, cs, m, &held, &changed);
                if (r4 == 0) {
                    ev = CALL_EV_BADMSG;
                } else if (held) {
                    __atomic_store_n((&cs->state), (CALL_STATE_HELD), __ATOMIC_SEQ_CST);
                    ev = CALL_EV_HELD;
                } else {
                    __atomic_store_n((&cs->state), (CALL_STATE_CONNECTED), __ATOMIC_SEQ_CST);
                    cs->connected_ms = nsbc_now_ms();
                    ev = CALL_EV_CONNECTED;
                    if (changed)
                        rtp_ssrc_guard_retarget(&cs->ssrc);
                }
            } else if (cseqm == SIP_M_BYE) {
                if (cs) {
                    session_end(cs, 0);
                    ev = CALL_EV_ENDED;
                }
            } else {
                ev = CALL_EV_UPDATED;
            }
        } else if (code >= 300) {
            if (!cs)
                goto done;
            if (cseqm == SIP_M_BYE) {
                session_end(cs, 0);
                ev = CALL_EV_ENDED;
            } else {
                /* INVITE/UPDATE 等失败：PoC 直接结束呼叫 */
                session_end(cs, 1);
                ev = CALL_EV_FAILED;
            }
        }
    }

    out->ev = ev;
    if (cs)
        out->state = __atomic_load_n((&cs->state), __ATOMIC_SEQ_CST);
    out->session = cs;

done:
    mem_mag_free(w->mag, m);
    pktbuf_unref(pb);
    return NSBC_OK;
}

/* ---------------- media ---------------- */

call_media_act_t call_session_media(call_session_t *cs, const uint8_t *buf,
                                    size_t len)
{
    if (!cs || !buf)
        return MEDIA_DROP;

    pkt_kind_t k = rtp_classify(buf, len);
    if (k == PKT_STUN || k == PKT_TURN_CHANNEL || k == PKT_DTLS ||
        k == PKT_UNKNOWN)
        return MEDIA_OTHER;
    if (k == PKT_RTCP)
        return MEDIA_RTCP;

    rtp_pkt_t p;
    if (rtp_parse(buf, len, &p) != NSBC_OK)
        return MEDIA_OTHER;

    /* SSRC 防护：未学习/伪造流量先丢弃（学习在首包） */
    if (!rtp_ssrc_guard_check(&cs->ssrc, p.ssrc))
        return MEDIA_DROP;

    if (rtp_gate_forward(&cs->gate))
        return MEDIA_FORWARD;
    if (rtp_gate_forward_early(&cs->gate))
        return MEDIA_EARLY;
    return MEDIA_DROP;
}

/* ---------------- accessors / stats ---------------- */

call_state_t call_session_state(const call_session_t *cs)
{
    return cs ? __atomic_load_n((&cs->state), __ATOMIC_SEQ_CST) : CALL_STATE_INIT;
}

const char *call_session_callid(const call_session_t *cs)
{
    return cs ? cs->call_id : NULL;
}

const call_media_t *call_session_media_info(const call_session_t *cs)
{
    return cs ? &cs->media : NULL;
}

uint64_t call_session_started_ms(const call_session_t *cs)
{
    return cs ? cs->created_ms : 0;
}

void call_mgr_stat(const call_mgr_t *m, call_mgr_stat_t *out)
{
    if (!m || !out)
        return;
    out->active = __atomic_load_n((&m->active), __ATOMIC_SEQ_CST);
    out->peak = __atomic_load_n((&m->peak), __ATOMIC_SEQ_CST);
    out->created = __atomic_load_n((&m->created), __ATOMIC_SEQ_CST);
    out->ended = __atomic_load_n((&m->ended), __ATOMIC_SEQ_CST);
    out->recycled = __atomic_load_n((&m->recycled), __ATOMIC_SEQ_CST);
    out->failed = __atomic_load_n((&m->failed), __ATOMIC_SEQ_CST);
}

int call_mgr_advance(call_mgr_t *m)
{
    return m ? mem_rcu_advance(m->rcu) : NSBC_INVAL;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
