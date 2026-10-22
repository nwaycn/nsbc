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

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define CDR_SPOOL_MAGIC   0x43445250u  /* "CDRP" */
#define CDR_SPOOL_VER     1

/* 前向声明（下面两个 helper 仍为 static，内部用） */
static void ring_push_locked(cdr_sink_t *s, const cdr_event_t *ev);

typedef struct {
    int used;
    cdr_event_t ev;
} cdr_open_t;

struct cdr_sink {
    pthread_mutex_t lock;
    cdr_ops_t       ops;
    cdr_open_t      open[CDR_OPEN_CAP];
    cdr_event_t     ring[CDR_RING_CAP];
    int             ring_head;   /* 最旧槽位（满时被覆盖 → 自动推进）*/
    int             ring_tail;   /* 下一个写位置 */
    int             flush_tail;  /* 上次 flush 已推送的截止位置（本次从 flush_tail→ring_tail-1 取增量推）*/
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
    if (ops != NULL)
        s->ops = *ops;
    *out = s;
    return NSBC_OK;
}

void cdr_sink_destroy(cdr_sink_t *s)
{
    if (s == NULL)
        return;
    (void)cdr_sink_flush(s); /* 优雅停机 drain（尽力） */
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

/* 追加写 batch 到 spool 文件（原子写：写 new 文件 → rename 覆盖）。
 * 任何失败返回非 0；调用方决定是否 drop。 */
static int spool_write(cdr_sink_t *s, const cdr_event_t *batch, int n)
{
    if (n <= 0 || s->spool_path[0] == '\0')
        return NSBC_INVAL;

    char tmp[576];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->spool_path);

    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        perror("cdr_spool fopen");
        return NSBC_ERR;
    }

    /* magic + ver + count + batch */
    uint32_t magic = CDR_SPOOL_MAGIC;
    uint32_t ver   = CDR_SPOOL_VER;
    uint32_t cnt   = (uint32_t)n;
    if (fwrite(&magic, 4, 1, f) != 1 ||
        fwrite(&ver, 4, 1, f) != 1 ||
        fwrite(&cnt, 4, 1, f) != 1 ||
        fwrite(batch, sizeof(cdr_event_t), n, f) != (size_t)n) {
        fclose(f); unlink(tmp);
        return NSBC_ERR;
    }
    fflush(f); fsync(fileno(f)); fclose(f);

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
    ++s->total_emitted;
}

static void fill_durations(cdr_event_t *ev)
{
    if (ev->start_ms != 0 && ev->end_ms >= ev->start_ms)
        ev->duration_s = (uint32_t)((ev->end_ms - ev->start_ms) / 1000u);
    if (ev->answer_ms != 0 && ev->end_ms >= ev->answer_ms)
        ev->billsec = (uint32_t)((ev->end_ms - ev->answer_ms) / 1000u);
}

void cdr_sink_emit(void *ud, const cdr_event_t *ev)
{
    cdr_sink_t *s = (cdr_sink_t *)ud;
    cdr_open_t *o;
    if (s == NULL || ev == NULL || ev->call_id[0] == '\0')
        return;
    fill_durations((cdr_event_t *)ev); /* 不修改调用方——本地副本 */

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
            (void)cdr_sink_flush(s);
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
        (void)cdr_sink_flush(s);
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
    cJSON_AddNumberToObject(o, "start_ms",   (double)ev->start_ms);
    cJSON_AddNumberToObject(o, "answer_ms",  (double)ev->answer_ms);
    cJSON_AddNumberToObject(o, "end_ms",      (double)ev->end_ms);
    cJSON_AddNumberToObject(o, "billsec",    (double)ev->billsec);
    cJSON_AddNumberToObject(o, "duration_s", (double)ev->duration_s);
    cJSON_AddNumberToObject(o, "cause",       (double)ev->cause);
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

    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return json;
}

/* 增量推送：从 flush_tail 到 ring_tail-1 取新增 CDR → UDS write-through → persist。
 *
 * 关键设计变化：**flush 不再推进 ring_head 清空 ring**。ring 生命周期完全由
 * ring_push_locked 在 ring 满时自动覆盖最旧数据（推进 ring_head）管理。
 *
 * 这样 poller 兜底路径（cdr.recent → 从 ring 读）始终能拿到最近 N 条 CDR，
 * 即使 UDS 断了 write-through 不生效，poller 也能在 UDS 恢复后补上（DB insert
 * 是幂等的 ON CONFLICT DO NOTHING，重复推送不报错）。
 *
 * flush_tail 跟踪本次已推送截止位置：下次只推增量。若 flush_tail 已被 ring 覆盖
 * （不在 ring_head→ring_tail 区间），自动从 ring_head 开始全量推送。*/
int cdr_sink_flush(cdr_sink_t *s)
{
    cdr_event_t batch[CDR_BATCH_MAX];
    int n = 0;
    int rc;
    if (s == NULL)
        return NSBC_INVAL;

    pthread_mutex_lock(&s->lock);

    /* --- 计算起始位置：flush_tail 是否已被覆盖 ---
     * 环形缓冲中，位置 x 在 [head, tail) 区间内的条件：
     *   正常（tail >= head）：head <= x < tail
     *   绕回（tail < head）：x >= head || x < tail
     * 不在区间 → flush_tail 被覆盖了，从 head 开始全量 */
    int ring_n = ring_count(s);
    int from = s->flush_tail;
    if (ring_n == 0) {
        pthread_mutex_unlock(&s->lock);
        return 0;
    }
    int head = s->ring_head, tail = s->ring_tail;
    int in_range;
    if (tail >= head)
        in_range = (from >= head && from < tail);
    else
        in_range = (from >= head || from < tail);
    if (!in_range)
        from = head;

    /* 只读快照：从 from 推到 tail-1，不动 ring_head/ring_tail */
    int idx = from;
    while (idx != tail && n < CDR_BATCH_MAX) {
        batch[n++] = s->ring[idx];
        idx = (idx + 1) % CDR_RING_CAP;
    }
    s->flush_tail = idx;  /* 更新：推到哪就记到哪 */

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
 * call_id/from/to 必给，dir/phase/disp 用名字枚举，emergency 0/1）。 */
static int fmt_one(const cdr_event_t *ev, char *buf, size_t bufsz)
{
    return snprintf(buf, bufsz,
        "cdr call_id=%s tenant=%s from=%s to=%s trunk=%s rec=%s "
        "dir=%s phase=%s disp=%s start_ms=%llu answer_ms=%llu end_ms=%llu "
        "billsec=%u duration_s=%u cause=%d emergency=%d\n",
        f(ev->call_id), f(ev->tenant), f(ev->from), f(ev->to),
        f(ev->trunk), f(ev->rec_path),
        cdr_dir_name(ev->dir), cdr_phase_name(ev->phase),
        cdr_disp_name(ev->disposition),
        (unsigned long long)ev->start_ms,
        (unsigned long long)ev->answer_ms,
        (unsigned long long)ev->end_ms,
        ev->billsec, ev->duration_s, ev->cause, ev->emergency);
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
