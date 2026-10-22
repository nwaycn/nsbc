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

struct ha_repl {
    repl_ops_t ops;
    int        active;
    uint64_t   pub_seq;
    uint64_t   applied_seq;
    uint64_t   ack_seq;
    uint64_t   gaps;
    int        ring_used;
    repl_rec_t ring[REPL_RING_CAP];
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
    r->active = is_active;
    *out = r;
    return NSBC_OK;
}

void repl_destroy(ha_repl_t *r)
{
    free(r);
}

int64_t repl_publish(ha_repl_t *r, repl_type_t type, uint16_t ver,
                     const void *payload, uint16_t len)
{
    repl_rec_t *rec;
    if (r == NULL || !r->active || type < 0 || type >= REPL_TYPE_MAX)
        return NSBC_INVAL;
    if (len > REPL_PAYLOAD_MAX || (len > 0 && payload == NULL))
        return NSBC_INVAL;
    if (r->ring_used >= REPL_RING_CAP)
        return NSBC_NOSPC; /* 备端落后：转全量/告警（见头注释） */
    rec = &r->ring[r->pub_seq % REPL_RING_CAP];
    memset(rec, 0, sizeof(*rec));
    rec->seq = ++r->pub_seq;
    rec->type = type;
    rec->ver = ver;
    rec->len = len;
    if (len > 0)
        memcpy(rec->payload, payload, len);
    ++r->ring_used;
    if (r->ops.send != NULL) {
        int rc = r->ops.send(r->ops.ud, rec);
        if (rc != NSBC_OK) {
            /* 发送失败留环（ha 通道线程重试）；计数经 OAM 补 */
            return (int64_t)rec->seq;
        }
    }
    return (int64_t)rec->seq;
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
    if (rec->seq > expect) {
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
    if (ack_seq <= r->pub_seq) {
        uint64_t base = r->ack_seq;
        r->ack_seq = ack_seq;
        /* 淘汰已确认环占用（粗骨架：水位差即环存量，封顶非负） */
        if (ack_seq > base) {
            uint64_t adv = ack_seq - base;
            r->ring_used -= (int)(adv < (uint64_t)r->ring_used
                                      ? adv : (uint64_t)r->ring_used);
        }
    }
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

uint64_t repl_gap_count(ha_repl_t *r)
{
    return r == NULL ? 0 : r->gaps;
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
