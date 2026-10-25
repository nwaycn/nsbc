/* 话单汇聚/落库——粗骨架（REQ-PBX-CDR-001，P0→P1 接线 spool+锁）
 *
 * 聚合规则：
 *  START 建 open 片段（存主被叫/方向/中继/start_ms）；
 *  ANSWER 记 answer_ms；
 *  END 合并片段→算 duration/billsec→入待写环→尝试批量 persist；
 *  END 缺失（崩溃/异常）的悬挂片段由周期扫描按超时补写 FAILED（P1）。
 *
 * 线程模型：emit 来自呼叫工作线程，flush 来自 OAM/周期线程；用 pthread_mutex
 * 保护 ring_head/ring_tail/open[]（P0 注释已留"接线批改锁"——现在就加）。
 *
 * 持久化降级链：persist_batch(DBAL) → spool.bin 文件 → DROP + drop 计数。
 * 启动时 replay 把 spool.bin 里遗留的 CDR push 进 ring 先落 DBAL。
 */
#include "cdr.h"
#include "uDS_client.h"  /* uds_send_cdr — 小写 uds_client.h 是 typo，真实文件名是 uDS_client.h */
#include "app_ctx.h"     /* nsbc_get_ctx() — END 分支里判 billing 是否启用 */
#include "obs_sub.h"    /* obs_sub_publish_cdr — CDR END 订阅推送 */
#include <cjson/cJSON.h>
#include "log.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define CDR_SPOOL_MAGIC   0x43445250u  /* "CDRP" */
#define CDR_SPOOL_VER     1

/* 前向声明（下面两个 helper 仍为 static，内部用） */
static void ring_push_locked(cdr_sink_t *s, const cdr_event_t *ev);
static void cdr_sink_drain_inline(cdr_sink_t *s, unsigned max_batches);

typedef struct {
    int used;
    cdr_event_t ev;
} cdr_open_t;

struct cdr_sink {
    pthread_mutex_t lock;
    pthread_mutex_t flush_lock;
    pthread_cond_t flush_cond;
    cdr_ops_t       ops;
    worker_job_t    flush_job;
    int             flush_pending;
    int             closing;
    cdr_open_t      open[CDR_OPEN_CAP];
    cdr_event_t     ring[CDR_RING_CAP];
    int             ring_head;   /* 最旧槽位（满时被覆盖 → 自动推进）*/
    int             ring_tail;   /* 下一个写位置 */
    uint64_t        ring_seq[CDR_RING_CAP];
    uint64_t        flush_seq;
    uint64_t        total_emitted;
    uint64_t        total_persisted;
    uint64_t        total_spooled;
    uint64_t        total_dropped;
    char            spool_path[512];
};

/* ---------- ring helpers ---------- */

static int ring_full(const cdr_sink_t *s)
{
    return (s->ring_tail + 1) % CDR_RING_CAP == s->ring_head;
}

static int ring_count(const cdr_sink_t *s)
{
    int n = s->ring_tail - s->ring_head;
    if (n < 0)
        n += CDR_RING_CAP;
    return n;
}

/* ---------- lifecycle ---------- */

int cdr_sink_create(const cdr_ops_t *ops, cdr_sink_t **out)
{
    cdr_sink_t *s;
    if (out == NULL)
        return NSBC_INVAL;
    s = (cdr_sink_t *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NSBC_NOMEM;
    pthread_mutex_init(&s->lock, NULL);
    pthread_mutex_init(&s->flush_lock, NULL);
    pthread_cond_init(&s->flush_cond, NULL);
    if (ops != NULL)
        s->ops = *ops;
    s->flush_job.fn = NULL;
    s->flush_job.arg = s;
    *out = s;
    return NSBC_OK;
}

void cdr_sink_destroy(cdr_sink_t *s)
{
    if (s == NULL)
        return;
    pthread_mutex_lock(&s->lock);
    s->closing = 1;
    while (s->flush_pending)
        pthread_cond_wait(&s->flush_cond, &s->lock);
    pthread_mutex_unlock(&s->lock);
    cdr_sink_drain_inline(s, 0); /* 优雅停机 drain（尽力） */
    pthread_cond_destroy(&s->flush_cond);
    pthread_mutex_destroy(&s->flush_lock);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

/* ---------- open fragment table ---------- */

static cdr_open_t *open_find(cdr_sink_t *s, const char *call_id)
{
    int i;
    for (i = 0; i < CDR_OPEN_CAP; ++i) {
        if (s->open[i].used &&
            strcmp(s->open[i].ev.call_id, call_id) == 0)
            return &s->open[i];
    }
    return NULL;
}

static cdr_open_t *open_alloc(cdr_sink_t *s)
{
    int i;
    for (i = 0; i < CDR_OPEN_CAP; ++i) {
        if (!s->open[i].used)
            return &s->open[i];
    }
    return NULL;
}

/* ---------- spool binary file ---------- */

/* C-07 修复：spool 不再覆盖旧批次。
 * 新逻辑：如果已有 spool 文件，先读出来 → 合并当前 batch → 原子写回。
 * 这样连续 DB 故障的多批次不会互相覆盖丢失。 */
static int spool_write(cdr_sink_t *s, const cdr_event_t *batch, int n)
{
    if (n <= 0 || s->spool_path[0] == '\0')
        return NSBC_INVAL;

    char tmp[576];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->spool_path);

    /* 先读现有 spool 内容（如果有） */
    uint32_t old_cnt = 0;
    cdr_event_t *old_batch = NULL;

    FILE *existing = fopen(s->spool_path, "rb");
    if (existing) {
        uint32_t magic = 0, ver = 0, cnt = 0;
        if (fread(&magic, 4, 1, existing) == 1 && magic == CDR_SPOOL_MAGIC &&
            fread(&ver, 4, 1, existing) == 1 && ver == CDR_SPOOL_VER &&
            fread(&cnt, 4, 1, existing) == 1 && cnt > 0 && cnt <= 65536) {
            old_batch = (cdr_event_t *)malloc(sizeof(cdr_event_t) * cnt);
            if (old_batch && fread(old_batch, sizeof(cdr_event_t), cnt, existing) == cnt)
                old_cnt = cnt;
            else {
                /* 读取失败或格式损坏 → 丢弃旧内容（避免无限增长） */
                free(old_batch); old_batch = NULL;
            }
        }
        fclose(existing);
    }

    /* 合并：旧 + 新 */
    /* C-07 修复：合并后加上限，避免 DB 长期故障时 spool 文件无界增长。
     * CDR_SPOOL_MAX_ENTRIES 作为硬上限，超出时丢弃最旧条目。 */
#define CDR_SPOOL_MAX_ENTRIES 65536u
    uint32_t total = old_cnt + (uint32_t)n;
    uint32_t skip_old = 0;
    if (total > CDR_SPOOL_MAX_ENTRIES) {
        skip_old = total - CDR_SPOOL_MAX_ENTRIES;
        total = CDR_SPOOL_MAX_ENTRIES;
        if (skip_old > old_cnt) skip_old = old_cnt;  /* 不能丢新的 */
    }

    cdr_event_t *merged = NULL;
    if (old_cnt > 0) {
        merged = (cdr_event_t *)malloc(sizeof(cdr_event_t) * total);
        if (!merged) {
            free(old_batch);
            return NSBC_NOMEM;
        }
        /* 先拷贝 old 中未被丢弃的部分（跳过前 skip_old 条） */
        uint32_t old_kept = old_cnt - skip_old;
        if (old_kept > 0)
            memcpy(merged, old_batch + skip_old, sizeof(cdr_event_t) * old_kept);
        /* 再拷贝新 batch */
        memcpy(merged + old_kept, batch, sizeof(cdr_event_t) * n);
    } else {
        merged = (cdr_event_t *)batch;  /* 直接用当前 batch */
    }

    /* 原子写 tmp → rename */
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        free(old_batch);
        if (merged != batch) free(merged);
        return NSBC_ERR;
    }

    uint32_t magic = CDR_SPOOL_MAGIC;
    uint32_t ver   = CDR_SPOOL_VER;
    uint32_t cnt   = total;
    int wr_ok = fwrite(&magic, 4, 1, f) == 1 &&
                fwrite(&ver, 4, 1, f) == 1 &&
                fwrite(&cnt, 4, 1, f) == 1 &&
                fwrite(merged, sizeof(cdr_event_t), total, f) == total;
    fflush(f); fsync(fileno(f)); fclose(f);

    free(old_batch);
    if (merged != batch) free(merged);

    if (!wr_ok) {
        unlink(tmp);
        return NSBC_ERR;
    }
    if (rename(tmp, s->spool_path) != 0) {
        unlink(tmp);
        return NSBC_ERR;
    }
    return NSBC_OK;
}

/* 启动 replay：把 spool 文件里遗留的 CDR push 进 ring。
 * 成功后 unlink 文件（避免重复 replay）。 */
static void spool_replay(cdr_sink_t *s)
{
    if (s->spool_path[0] == '\0') return;

    FILE *f = fopen(s->spool_path, "rb");
    if (f == NULL) return; /* 文件不存在 = 无遗留，正常 */

    uint32_t magic = 0, ver = 0, cnt = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != CDR_SPOOL_MAGIC ||
        fread(&ver, 4, 1, f) != 1 || ver != CDR_SPOOL_VER ||
        fread(&cnt, 4, 1, f) != 1) {
        fclose(f); unlink(s->spool_path);
        return; /* 格式损坏：丢弃（避免无限循环） */
    }

    /* 上限 64K 防炸 */
    if (cnt == 0 || cnt > 65536) { fclose(f); return; }

    cdr_event_t *tmp = (cdr_event_t *)malloc(sizeof(cdr_event_t) * cnt);
    if (tmp == NULL) { fclose(f); return; }

    if (fread(tmp, sizeof(cdr_event_t), cnt, f) != cnt) {
        free(tmp); fclose(f);
        return; /* 截断：整份丢弃 */
    }
    fclose(f);

    pthread_mutex_lock(&s->lock);
    int i;
    for (i = 0; i < (int)cnt; ++i) {
        /* ring_push_locked 内部已处理 ring_full → 自动覆盖最旧，不用手动 flush */
        ring_push_locked(s, &tmp[i]);
    }
    pthread_mutex_unlock(&s->lock);

    free(tmp);
    unlink(s->spool_path); /* replay 完清文件——flush 成功后 DBAL 已有 */
}

/* ---------- ring push & emit ---------- */

static void ring_push_locked(cdr_sink_t *s, const cdr_event_t *ev)
{
    if (ring_full(s)) {
        ++s->total_dropped;
        s->ring_head = (s->ring_head + 1) % CDR_RING_CAP;
    }
    s->ring[s->ring_tail] = *ev;
    s->ring_tail = (s->ring_tail + 1) % CDR_RING_CAP;
    s->ring_seq[(s->ring_tail + CDR_RING_CAP - 1) % CDR_RING_CAP] =
        ++s->total_emitted;
}

static void fill_durations(cdr_event_t *ev)
{
    if (ev->start_ms != 0 && ev->end_ms >= ev->start_ms)
        ev->duration_s = (uint32_t)((ev->end_ms - ev->start_ms) / 1000u);
    if (ev->answer_ms != 0 && ev->end_ms >= ev->answer_ms)
        ev->billsec = (uint32_t)((ev->end_ms - ev->answer_ms) / 1000u);
}

static int cdr_sink_has_unflushed_locked(const cdr_sink_t *s)
{
    return ring_count(s) > 0 && s->flush_seq < s->total_emitted;
}

static void cdr_sink_drain_inline(cdr_sink_t *s, unsigned max_batches)
{
    unsigned batches = 0;
    for (;;) {
        int rc = cdr_sink_flush(s);
        if (rc <= 0)
            return;
        if (max_batches && ++batches >= max_batches)
            return;

        pthread_mutex_lock(&s->lock);
        int pending = cdr_sink_has_unflushed_locked(s);
        pthread_mutex_unlock(&s->lock);
        if (!pending)
            return;
    }
}

static void cdr_sink_flush_worker(void *arg)
{
    cdr_sink_t *s = (cdr_sink_t *)arg;
    unsigned batches = 0;
    int rc = 0;
    for (;;) {
        rc = cdr_sink_flush(s);
        ++batches;

        pthread_mutex_lock(&s->lock);
        int pending = cdr_sink_has_unflushed_locked(s);
        if (rc < 0 || !pending) {
            s->flush_pending = 0;
            pthread_cond_broadcast(&s->flush_cond);
            pthread_mutex_unlock(&s->lock);
            if (rc < 0)
                log_warn("cdr", "background flush failed rc=%d", rc);
            return;
        }
        pthread_mutex_unlock(&s->lock);

        if (batches >= 4) {
            rc = wpool_submit_any(s->ops.workers, &s->flush_job);
            if (rc == NSBC_OK)
                return;
            log_warn("cdr", "background flush resubmit rejected rc=%d; draining inline", rc);
            batches = 0;
        }
    }
}

static void cdr_sink_schedule_flush(cdr_sink_t *s)
{
    if (!s->ops.workers) {
        (void)cdr_sink_flush(s);
        return;
    }

    pthread_mutex_lock(&s->lock);
    if (s->closing || s->flush_pending) {
        pthread_mutex_unlock(&s->lock);
        return;
    }
    s->flush_pending = 1;
    s->flush_job.fn = cdr_sink_flush_worker;
    pthread_mutex_unlock(&s->lock);

    int rc = wpool_submit_any(s->ops.workers, &s->flush_job);
    if (rc == NSBC_OK)
        return;

    pthread_mutex_lock(&s->lock);
    s->flush_pending = 0;
    pthread_cond_broadcast(&s->flush_cond);
    pthread_mutex_unlock(&s->lock);
    log_warn("cdr", "background flush rejected rc=%d; flushing synchronously", rc);
    cdr_sink_drain_inline(s, 4);
}

void cdr_sink_emit(void *ud, const cdr_event_t *ev)
{
    cdr_sink_t *s = (cdr_sink_t *)ud;
    cdr_event_t event;
    cdr_open_t *o;
    if (s == NULL || ev == NULL || ev->call_id[0] == '\0')
        return;
    event = *ev;
    fill_durations(&event);
    ev = &event;

    pthread_mutex_lock(&s->lock);
    switch (ev->phase) {
    case CDR_PHASE_START:
        o = open_find(s, ev->call_id);
        if (o == NULL)
            o = open_alloc(s);
        if (o == NULL) { pthread_mutex_unlock(&s->lock); return; }
        o->used = 1;
        o->ev = *ev;
        pthread_mutex_unlock(&s->lock);
        return;
    case CDR_PHASE_ANSWER:
        o = open_find(s, ev->call_id);
        if (o == NULL) {
            o = open_alloc(s);
            if (o == NULL) { pthread_mutex_unlock(&s->lock); return; }
            o->used = 1;
            o->ev = *ev;
            o->ev.phase = CDR_PHASE_START;
        }
        o->ev.answer_ms = ev->answer_ms;
        pthread_mutex_unlock(&s->lock);
        return;
    case CDR_PHASE_END:
        o = open_find(s, ev->call_id);
        if (o == NULL) {
            cdr_event_t lone = *ev;
            fill_durations(&lone);
            ring_push_locked(s, &lone);
            pthread_mutex_unlock(&s->lock);
            cdr_sink_schedule_flush(s);
            /* ESL CDR event fire（Phase 2 增量） */
            {
                nsbc_ctx_t *ctx = nsbc_get_ctx();
                if (ctx && ctx->esl_event_fire) {
                    char *json = cdr_event_to_json(&lone);
                    if (json) {
                        ctx->esl_event_fire("CDR", json, strlen(json));
                        free(json);
                    }
                }
            }
            return;
        }
        o->ev.dir = ev->dir;
        o->ev.disposition = ev->disposition;
        o->ev.cause = ev->cause;
        o->ev.end_ms = ev->end_ms;
        o->ev.emergency = ev->emergency;
        /* REQ-CDR-002 补全：END ev 由 b2b_emit_cdr 填齐网络端点/codec/
         * 媒体质量/threat 标记——必须并进聚合槽，否则这些字段在 START
         * 拷贝时是空的，Web 端 Source IP/Codec/Threat 列永远显示 '-'。 */
        if (ev->from_ip[0] != '\0')
            snprintf(o->ev.from_ip, sizeof(o->ev.from_ip), "%s", ev->from_ip);
        if (ev->codec_a[0] != '\0')
            snprintf(o->ev.codec_a, sizeof(o->ev.codec_a), "%s", ev->codec_a);
        if (ev->codec_b[0] != '\0')
            snprintf(o->ev.codec_b, sizeof(o->ev.codec_b), "%s", ev->codec_b);
        if (ev->a_peer_ip[0] != '\0')
            snprintf(o->ev.a_peer_ip, sizeof(o->ev.a_peer_ip), "%s", ev->a_peer_ip);
        if (ev->b_peer_ip[0] != '\0')
            snprintf(o->ev.b_peer_ip, sizeof(o->ev.b_peer_ip), "%s", ev->b_peer_ip);
        if (ev->is_malicious)
            o->ev.is_malicious = 1;
        if (ev->threat_reason[0] != '\0')
            snprintf(o->ev.threat_reason, sizeof(o->ev.threat_reason), "%s", ev->threat_reason);
        o->ev.rtp_loss_pct = ev->rtp_loss_pct;
        o->ev.rtp_jitter_ms = ev->rtp_jitter_ms;
        o->ev.mos_x1000 = ev->mos_x1000;
        if (ev->rec_path[0] != '\0')
            snprintf(o->ev.rec_path, sizeof(o->ev.rec_path), "%s", ev->rec_path);
        if (ev->trunk[0] != '\0')
            snprintf(o->ev.trunk, sizeof(o->ev.trunk), "%s", ev->trunk);
        o->ev.phase = CDR_PHASE_END;
        fill_durations(&o->ev);
        ring_push_locked(s, &o->ev);
        o->used = 0;
        pthread_mutex_unlock(&s->lock);
        /* 订阅推送：完整 END 话单，JSON 格式（与 Web 侧 uds_server 对齐）。
         * 锁外调用：obs_sub_publish_cdr 内部带自己的锁，不会与 s->lock 死锁
         * ——锁序固定 s->lock → g_sub_hub.lock，无反向路径。
         * 帧格式对齐 events 帧：带 ISO8601 时间戳 + CDR 域 + JSON body。 */
        {
            char *json = cdr_event_to_json(&o->ev);
            if (json) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                struct tm tmv;
                localtime_r(&ts.tv_sec, &tmv);
                char stamp[32];
                strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tmv);
                char frame[OBS_FRAME_MAX];
                int n = snprintf(frame, sizeof(frame),
                    "%s.%03d CDR %s", stamp, (int)(ts.tv_nsec/1000000), json);
                if (n > 0 && (size_t)n < sizeof(frame))
                    obs_sub_publish_cdr("%s", frame);
                free(json);
            }
        }
        cdr_sink_schedule_flush(s);
        /* ESL CDR event fire（Phase 2 增量） */
        {
            nsbc_ctx_t *ctx = nsbc_get_ctx();
            if (ctx && ctx->esl_event_fire) {
                char *json = cdr_event_to_json(&o->ev);
                if (json) {
                    ctx->esl_event_fire("CDR", json, strlen(json));
                    free(json);
                }
            }
        }
        return;
    }
}

/* ---------- flush ---------- */

/* cdr_event_t → JSON 对象（用 cJSON；calloc 已归零的字段直接省略）。
 * 返回 malloc 出来的 JSON 字符串，调用方负责 free。NULL 表示失败。
 * JSON 契约与 web/src/uds_server.rs::write_cdr 完全对齐。
 * 非 static：b2bua.c / signaling.c 的 UDS 推送点复用完整 CDR JSON，不另造一份残缺 snprintf。 */
char *cdr_event_to_json(const cdr_event_t *ev)
{
    if (!ev) return NULL;
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;

    cJSON_AddStringToObject(o, "call_id",    ev->call_id);
    if (ev->tenant[0])       cJSON_AddStringToObject(o, "tenant",     ev->tenant);
    cJSON_AddStringToObject(o, "from",       ev->from);
    cJSON_AddStringToObject(o, "to",         ev->to);
    if (ev->trunk[0])        cJSON_AddStringToObject(o, "trunk",      ev->trunk);
    if (ev->rec_path[0])     cJSON_AddStringToObject(o, "rec_path",   ev->rec_path);
    cJSON_AddStringToObject(o, "dir",        cdr_dir_name(ev->dir));
    cJSON_AddStringToObject(o, "phase",      cdr_phase_name(ev->phase));
    cJSON_AddStringToObject(o, "disposition",cdr_disp_name(ev->disposition));
    cJSON_AddNumberToObject(o, "start_ms", (double)ev->start_ms);
    cJSON_AddNumberToObject(o, "answer_ms", (double)ev->answer_ms);
    cJSON_AddNumberToObject(o, "end_ms", (double)ev->end_ms);
    cJSON_AddNumberToObject(o, "billsec", (double)ev->billsec);
    cJSON_AddNumberToObject(o, "duration_s", (double)ev->duration_s);
    cJSON_AddNumberToObject(o, "cause", (double)ev->cause);
    /* session 通道复用键（REQ-UDS-002）：call_destroy 的 SESSION_END 推送
     * 直接透传本 JSON，Rust dispatch_session_push 读 event/state 更新 calls 表；
     * cdr 通道（write_cdr）只取自己认识的键，多出的键无副作用。 */
    cJSON_AddStringToObject(o, "event", cdr_phase_name(ev->phase));
    cJSON_AddStringToObject(o, "state",
        ev->phase == CDR_PHASE_END ? "terminated"
      : ev->phase == CDR_PHASE_ANSWER ? "answered" : "ringing");
    cJSON_AddNumberToObject(o, "emergency",   (double)ev->emergency);
    /* 网络端点 */
    if (ev->from_ip[0])       cJSON_AddStringToObject(o, "from_ip",    ev->from_ip);
    if (ev->a_peer_ip[0])     cJSON_AddStringToObject(o, "a_peer_ip",  ev->a_peer_ip);
    if (ev->b_peer_ip[0])     cJSON_AddStringToObject(o, "b_peer_ip",  ev->b_peer_ip);

    if (ev->codec_a[0])      cJSON_AddStringToObject(o, "codec_a",      ev->codec_a);
    if (ev->codec_b[0])      cJSON_AddStringToObject(o, "codec_b",      ev->codec_b);
    cJSON_AddNumberToObject(o, "rtp_loss_pct", (double)ev->rtp_loss_pct);
    cJSON_AddNumberToObject(o, "rtp_jitter_ms", (double)ev->rtp_jitter_ms);
    if (ev->mos_x1000 >= 0)  cJSON_AddNumberToObject(o, "mos_x1000",  (double)ev->mos_x1000);
    /* REQ-SEC-005：恶意标记 + 原因 —— 之前漏序列化，Web 端 Threat 列
     * 永远读不到（write_cdr 的 is_malicious 恒为 0）。 */
    if (ev->is_malicious) {
        cJSON_AddNumberToObject(o, "is_malicious", 1);
        if (ev->threat_reason[0])
            cJSON_AddStringToObject(o, "threat_reason", ev->threat_reason);
    }

    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return json;
}

/* 增量推送：按单调序号取尚未处理的 CDR → UDS write-through → persist。
 *
 * 关键设计变化：**flush 不再推进 ring_head 清空 ring**。ring 生命周期完全由
 * ring_push_locked 在 ring 满时自动覆盖最旧数据（推进 ring_head）管理。
 *
 * 这样 poller 兜底路径（cdr.recent → 从 ring 读）始终能拿到最近 N 条 CDR，
 * 即使 UDS 断了 write-through 不生效，poller 也能在 UDS 恢复后补上（DB insert
 * 是幂等的 ON CONFLICT DO NOTHING，重复推送不报错）。
 *
 * 游标被 ring 覆盖时从当前 ring_head 对应序号恢复。*/
static int cdr_sink_flush_inner(cdr_sink_t *s)
{
    cdr_event_t batch[CDR_BATCH_MAX];
    int n = 0;
    int rc;
    if (s == NULL)
        return NSBC_INVAL;

    pthread_mutex_lock(&s->lock);

    /* Monotonic event sequence numbers distinguish reused ring slots. */
    int ring_n = ring_count(s);
    if (ring_n == 0) {
        pthread_mutex_unlock(&s->lock);
        return 0;
    }
    uint64_t oldest_seq = s->ring_seq[s->ring_head];
    uint64_t latest_seq = s->ring_seq[
        (s->ring_tail + CDR_RING_CAP - 1) % CDR_RING_CAP];
    uint64_t first_seq = s->flush_seq + 1;
    if (first_seq < oldest_seq)
        first_seq = oldest_seq;
    if (first_seq > latest_seq) {
        pthread_mutex_unlock(&s->lock);
        return 0;
    }
    int idx = (s->ring_head + (int)(first_seq - oldest_seq)) % CDR_RING_CAP;
    while (idx != s->ring_tail && n < CDR_BATCH_MAX) {
        batch[n++] = s->ring[idx];
        idx = (idx + 1) % CDR_RING_CAP;
    }
    s->flush_seq = first_seq + (uint64_t)n - 1;

    pthread_mutex_unlock(&s->lock);

    if (n == 0)
        return 0;

    /* === 1) write-through：UDS push → Web 侧 uds_server 直接写 SQLite ===
     * 单条发送（uds_client 内部 ring 聚合，异步不阻塞）。
     * 返回值忽略：UDS 不可用时静默丢（后续 poller 兜底拉取）。*/
    int i;
    for (i = 0; i < n; i++) {
        char *json = cdr_event_to_json(&batch[i]);
        if (json) {
            (void)uds_send_cdr(json);   /* 非阻塞，内部 ring 满了自动丢最旧 */
            free(json);
        }
    }

    /* === 2) persist_batch（WAL） ===
     * WAL append 成功 → 这些 CDR 已落盘，下次 init replay 还能捞回来。
     * persist 失败降级 spool（原子写+rename，比 WAL 弱但比丢强）。*/
    if (s->ops.persist_batch != NULL) {
        rc = s->ops.persist_batch(s->ops.ud, batch, n);
        if (rc == NSBC_OK) {
            s->total_persisted += (uint64_t)n;
            return n;
        }
    }
    rc = spool_write(s, batch, n);
    if (rc == NSBC_OK) {
        s->total_spooled += (uint64_t)n;
        return n;
    }
    /* spool 也写不了——真 drop。但 ring 里还有数据，poller 能兜底拉 */
    s->total_dropped += (uint64_t)n;
    return -rc;
}

int cdr_sink_flush(cdr_sink_t *s)
{
    if (!s)
        return NSBC_INVAL;
    pthread_mutex_lock(&s->flush_lock);
    int rc = cdr_sink_flush_inner(s);
    pthread_mutex_unlock(&s->flush_lock);
    return rc;
}

/* ---------- OAM counters ---------- */

int cdr_sink_open_count(cdr_sink_t *s)
{
    int i, n = 0;
    if (s == NULL)
        return 0;
    pthread_mutex_lock(&s->lock);
    for (i = 0; i < CDR_OPEN_CAP; ++i) {
        if (s->open[i].used) ++n;
    }
    pthread_mutex_unlock(&s->lock);
    return n;
}

int cdr_sink_pending_count(const cdr_sink_t *s)
{
    return s == NULL ? 0 : ring_count(s);
}

uint64_t cdr_sink_total_emitted(const cdr_sink_t *s)    { return s ? s->total_emitted : 0; }
uint64_t cdr_sink_total_persisted(const cdr_sink_t *s) { return s ? s->total_persisted : 0; }
uint64_t cdr_sink_total_spooled(const cdr_sink_t *s)   { return s ? s->total_spooled : 0; }
uint64_t cdr_sink_total_dropped(const cdr_sink_t *s)   { return s ? s->total_dropped : 0; }

/* ---------- OAM recent（只读，非消费——不动 ring 头） ---------- */

/* fmt helper：空字符串输出 "-"，与 Web 侧 kv_opt 的 "-" filter 对齐 */
static const char *f(const char *s) { return (s && *s) ? s : "-"; }

/* 格式化一条已完成 CDR 为 `cdr k=v ...` 行（与 Web 侧 parse_cdr_line 对齐：
 * call_id/from/to 必给，dir/phase/disp 用名字枚举，emergency 0/1。
 * REQ-CDR-002 补全：追加 from_ip/codec/peer/threat 扩展字段——poller 兜底
 * 路径（UDS 断链时 cdr.recent 轮询落库）不再丢失这些列，与 write_cdr 同口径）。 */
static int fmt_one(const cdr_event_t *ev, char *buf, size_t bufsz)
{
    return snprintf(buf, bufsz,
        "cdr call_id=%s tenant=%s from=%s to=%s trunk=%s rec=%s "
        "dir=%s phase=%s disp=%s start_ms=%llu answer_ms=%llu end_ms=%llu "
        "billsec=%u duration_s=%u cause=%d emergency=%d "
        "from_ip=%s codec_a=%s codec_b=%s a_peer=%s b_peer=%s mal=%d threat=%s\n",
        f(ev->call_id), f(ev->tenant), f(ev->from), f(ev->to),
        f(ev->trunk), f(ev->rec_path),
        cdr_dir_name(ev->dir), cdr_phase_name(ev->phase),
        cdr_disp_name(ev->disposition),
        (unsigned long long)ev->start_ms,
        (unsigned long long)ev->answer_ms,
        (unsigned long long)ev->end_ms,
        ev->billsec, ev->duration_s, ev->cause, ev->emergency,
        f(ev->from_ip), f(ev->codec_a), f(ev->codec_b),
        f(ev->a_peer_ip), f(ev->b_peer_ip),
        ev->is_malicious ? 1 : 0, f(ev->threat_reason));
}

int cdr_sink_recent(cdr_sink_t *s, int limit, char *buf, size_t bufsz)
{
    int n, i, cnt = 0;
    size_t off = 0;

    if (buf == NULL || bufsz == 0)
        return NSBC_INVAL;
    buf[0] = '\0';
    if (limit < 0)
        limit = 0;

    if (s == NULL) { /* billing 模块未加载：空清单而非错误（与 show cdr 同口径） */
        snprintf(buf, bufsz, "cdrs=0\n");
        return NSBC_OK;
    }

    pthread_mutex_lock(&s->lock);
    n = ring_count(s);
    if (n > limit)
        n = limit;
    for (i = 0; i < n; ++i) {
        /* 倒序：ring_tail-1 是最新已完成事件，向 head 方向回退（最新在前） */
        int idx = (s->ring_tail + CDR_RING_CAP - 1 - i) % CDR_RING_CAP;
        int w = fmt_one(&s->ring[idx], buf + off, bufsz - off);
        if (w < 0 || (size_t)w >= bufsz - off)
            break; /* 缓冲耗尽：截断，已写完整行有效 */
        off += (size_t)w;
        ++cnt;
    }
    pthread_mutex_unlock(&s->lock);

    snprintf(buf + off, bufsz - off, "cdrs=%d\n", cnt);
    return NSBC_OK;
}

/* ---------- spool path & replay entry (billing init 调用) ---------- */

void cdr_sink_set_spool_path(cdr_sink_t *s, const char *path)
{
    if (s == NULL || path == NULL) return;
    snprintf(s->spool_path, sizeof(s->spool_path), "%s", path);
}

void cdr_sink_replay_spool(cdr_sink_t *s)
{
    if (s == NULL) return;
    spool_replay(s);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
