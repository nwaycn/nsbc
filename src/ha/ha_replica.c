/* HA 复制——粗骨架（REQ-HA-002 P1 / REQ-HA-004）
 *
 * 线程模型（接线批）：publish 来自呼叫工作线程，send 走独立 ha
 * 通道线程（环形 SPSC 或分片 MPSC）；当前骨架无锁、无真实网络，
 * ops 未装时只走水位计数（早期启动/单机口径）。
 * 缺口策略：备端期望 expected = applied_seq+1；收到更大 seq 即缺口，
 * 先尝试 on_gap 重传；缺口记录到达前的更晚记录只更新 peek 水位不入
 * 应用（粗骨架简化：不做乱序缓存，直接等重传）。
 */
#include "ha_replica.h"
#include <pthread.h>

struct ha_repl {
    repl_ops_t ops;
    int        active;
    uint64_t   pub_seq;
    uint64_t   applied_seq;
    uint64_t   ack_seq;
    uint64_t   gaps;
    int        ring_used;
    repl_rec_t ring[REPL_RING_CAP];
    int        accept_any_seq;  /* FULL resync inflight: skip gap check, accept out-of-order */
    /* publish 来自多个业务线程（registrar + b2bua/reactor），必须互斥保护
     * pub_seq / ring_used / ring[]，否则序号与负载会错配 */
    pthread_mutex_t lock;
};

int repl_create(const repl_ops_t *ops, int is_active, ha_repl_t **out)
{
    ha_repl_t *r;
    if (out == NULL)
        return NSBC_INVAL;
    r = (ha_repl_t *)calloc(1, sizeof(*r));
    if (r == NULL)
        return NSBC_NOMEM;
    if (ops != NULL)
        r->ops = *ops;
    pthread_mutex_init(&r->lock, NULL);
    r->active = is_active;
    *out = r;
    return NSBC_OK;
}

void repl_destroy(ha_repl_t *r)
{
    if (r == NULL) return;
    pthread_mutex_destroy(&r->lock);
    free(r);
}

void repl_set_ops(ha_repl_t *r,
                  int (*apply)(void *ud, const repl_rec_t *rec),
                  int (*on_gap)(void *ud, uint64_t expect, uint64_t got),
                  void *ud)
{
    if (r == NULL) return;
    if (apply)  r->ops.apply  = apply;
    if (on_gap) r->ops.on_gap = on_gap;
    if (ud)     r->ops.ud     = ud;
}

void repl_set_accept_any_seq(ha_repl_t *r, int v)
{
    if (r == NULL) return;
    r->accept_any_seq = v;
}

int64_t repl_publish(ha_repl_t *r, repl_type_t type, uint16_t ver,
                     const void *payload, uint16_t len)
{
    repl_rec_t *rec;
    if (r == NULL || !r->active || type < 0 || type >= REPL_TYPE_MAX)
        return NSBC_INVAL;
    if (len > REPL_PAYLOAD_MAX || (len > 0 && payload == NULL))
        return NSBC_INVAL;
    /* 多生产者互斥：seq 分配、slot 写入、发送入队必须在同一临界区，
     * 否则两个线程会拿到不同 seq 却写到同一 slot */
    pthread_mutex_lock(&r->lock);
    if (r->ring_used >= REPL_RING_CAP) {
        pthread_mutex_unlock(&r->lock);
        return NSBC_NOSPC; /* 备端落后：转全量/告警（见头注释） */
    }
    rec = &r->ring[r->pub_seq % REPL_RING_CAP];
    memset(rec, 0, sizeof(*rec));
    rec->seq = ++r->pub_seq;
    rec->type = type;
    rec->ver = ver;
    rec->len = len;
    if (len > 0)
        memcpy(rec->payload, payload, len);
    ++r->ring_used;
    {
        uint64_t seq = rec->seq;
        if (r->ops.send != NULL) {
            int rc = r->ops.send(r->ops.ud, rec);
            pthread_mutex_unlock(&r->lock);
            if (rc != NSBC_OK) {
                /* 发送失败留环（ha 通道线程重试）；计数经 OAM 补 */
                return (int64_t)seq;
            }
        } else {
            pthread_mutex_unlock(&r->lock);
        }
        return (int64_t)seq;
    }
}

int repl_ingest(ha_repl_t *r, const repl_rec_t *rec)
{
    uint64_t expect;
    if (r == NULL || rec == NULL || r->active)
        return NSBC_INVAL; /* 主端不 ingest（防环回） */
    if (rec->type < 0 || rec->type >= REPL_TYPE_MAX ||
        rec->len > REPL_PAYLOAD_MAX)
        return NSBC_PROTO;
    if (rec->seq <= r->applied_seq)
        return NSBC_OK; /* 重复/重传已应用：幂等丢弃 */
    expect = r->applied_seq + 1;
    if (rec->seq > expect && !r->accept_any_seq) {
        ++r->gaps;
        if (r->ops.on_gap != NULL)
            (void)r->ops.on_gap(r->ops.ud, expect, rec->seq);
        return NSBC_AGAIN; /* 等重传：调用方保留报文（粗骨架不缓存） */
    }
    if (r->ops.apply != NULL) {
        int rc = r->ops.apply(r->ops.ud, rec);
        if (rc != NSBC_OK)
            return rc;
    }
    r->applied_seq = rec->seq;
    return NSBC_OK;
}

void repl_ack(ha_repl_t *r, uint64_t ack_seq)
{
    if (r == NULL)
        return;
    pthread_mutex_lock(&r->lock);
    if (ack_seq > r->pub_seq) {
        pthread_mutex_unlock(&r->lock);
        return;  /* 越界 ACK（异常/陈旧帧）直接忽略 */
    }
    r->ack_seq = ack_seq;
    /* 环占用直接由水位差计算，避免增量更新丢失 */
    r->ring_used = (int)(r->pub_seq - ack_seq);
    pthread_mutex_unlock(&r->lock);
}

int repl_apply_control(ha_repl_t *r, const repl_rec_t *rec)
{
    if (r == NULL || rec == NULL)
        return NSBC_INVAL;
    /* 控制帧（如 REPL_FULL CLEAR）：投递 apply 但不推进 applied_seq */
    if (r->ops.apply == NULL)
        return NSBC_OK;
    return r->ops.apply(r->ops.ud, rec);
}

void repl_resync_done(ha_repl_t *r, uint64_t snapshot_seq)
{
    if (r == NULL)
        return;
    r->applied_seq = snapshot_seq;
    r->gaps = 0;
}

uint64_t repl_published_seq(ha_repl_t *r)
{
    return r == NULL ? 0 : r->pub_seq;
}

uint64_t repl_applied_seq(ha_repl_t *r)
{
    return r == NULL ? 0 : r->applied_seq;
}

uint64_t repl_ack_seq(ha_repl_t *r)
{
    return r == NULL ? 0 : r->ack_seq;
}

uint64_t repl_gap_count(ha_repl_t *r)
{
    return r == NULL ? 0 : r->gaps;
}

const repl_rec_t *repl_at(ha_repl_t *r, uint64_t seq)
{
    if (r == NULL) return NULL;
    /* 有效范围：(ack_seq, pub_seq] — ring 是 pub_seq 单调增写入、pub%CAP 位置覆盖
     * ack_seq 之前的条可能被新 publish 覆盖掉，pub_seq 之后的还没 publish */
    if (seq <= r->ack_seq || seq > r->pub_seq)
        return NULL;
    return &r->ring[seq % REPL_RING_CAP];
}

const char *repl_type_name(repl_type_t t)
{
    switch (t) {
    case REPL_REG:       return "reg";
    case REPL_DIALOG:    return "dialog";
    case REPL_CDR_FLUSH: return "cdr_flush";
    case REPL_FULL:      return "full";
    case REPL_TYPE_MAX:  break;
    }
    return "unknown";
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
