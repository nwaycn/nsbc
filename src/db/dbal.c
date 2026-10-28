/* Architectural constraint guard — C core nsbcd does NOT touch DB.
 * Rust web side (sqlx Any driver) is the sole persistence layer.
 *
 * Enabling: comment out "#define NSBC_NO_DBAL" below AND remove src/db/%.c
 *           from Makefile filter-out (L131-173). Intended for future
 *           MySQL/PG driver (REQ-DB-002) when C side truly needs DB access.
 *
 * Default behaviour (NSBC_NO_DBAL defined → skip entire file):
 *   C core compiles clean with zero DB linkage — confirmed by audit P3. */
#define NSBC_NO_DBAL
#ifndef NSBC_NO_DBAL

/* 异步 DBAL + group commit 实现（REQ-DB-001/003/005，P0）。
 * SQLite 路径需 -DNSBC_WITH_SQLITE -lsqlite3；否则 mysql/postgres
 * 返回 NSBC_NOTSUP（P1），none 静默丢弃（CDR 缓存降级，可观测计数）。 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dbal.h"
#include "log.h"
#include "nsbc_common.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef NSBC_WITH_SQLITE
#include <sqlite3.h>
#endif

#define DB_QUEUE_CAP 8192

typedef struct {
    char *sql;
    dbal_done_fn cb;
    void *ud;
    uint64_t enq_ms;
    int used;
} db_job_t;

struct dbal {
    dbal_cfg_t cfg;
    enum { DB_NONE = 0, DB_SQLITE, DB_STUB_UNSUP } kind;
#ifdef NSBC_WITH_SQLITE
    sqlite3 *sq;
#endif
    db_job_t q[DB_QUEUE_CAP];
    unsigned qh, qt; /* h=worker 取；t=入队写 */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thr;
    int stop;
    uint64_t committed, dropped;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void job_done(db_job_t *j, int rc)
{
    if (j->cb)
        j->cb(rc, j->ud);
    free(j->sql);
    j->sql = NULL;
    j->used = 0;
}

static int sqlite_exec_raw(struct dbal *d, const char *sql)
{
#ifdef NSBC_WITH_SQLITE
    char *err = NULL;
    int rc = sqlite3_exec(d->sq, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) {
            llog(LOG_WARN, "dbal", "sql error: %s", err);
            sqlite3_free(err);
        }
        return NSBC_ERR;
    }
    return NSBC_OK;
#else
    (void)d;
    (void)sql;
    return NSBC_NOTSUP;
#endif
}

/* 取出当前可提交批次（调用时持锁）；返回条数 */
static unsigned take_batch_locked(dbal_t *d, db_job_t *out, unsigned maxn)
{
    unsigned n = 0;
    while (n < maxn && d->qh != d->qt) {
        out[n++] = d->q[d->qh];
        d->q[d->qh].used = 0;
        d->q[d->qh].sql = NULL;
        d->qh = (d->qh + 1) % DB_QUEUE_CAP;
    }
    return n;
}

static void *dbal_worker(void *arg)
{
    dbal_t *d = (dbal_t *)arg;
    db_job_t batch[1024];

    for (;;) {
        pthread_mutex_lock(&d->lock);
        while (!d->stop && d->qh == d->qt)
            pthread_cond_wait(&d->cond, &d->lock);
        if (d->stop) {
            pthread_mutex_unlock(&d->lock);
            break;
        }
        /* group commit 计时：等到最老语句年龄达标或行数达标 */
        uint64_t oldest = d->q[d->qh].enq_ms;
        uint64_t pending =
            (d->qt + DB_QUEUE_CAP - d->qh) % DB_QUEUE_CAP;
        if (pending < (unsigned)d->cfg.group_commit_rows) {
            uint64_t wait_until =
                oldest + (uint64_t)d->cfg.group_commit_ms;
            uint64_t cur = now_ms();
            if (wait_until > cur) {
                struct timespec ts;
                ts.tv_sec = wait_until / 1000;
                ts.tv_nsec = (long)(wait_until % 1000) * 1000000;
                pthread_cond_timedwait(&d->cond, &d->lock, &ts);
            }
        }
        unsigned n = take_batch_locked(d, batch,
                                       (unsigned)d->cfg.group_commit_rows >
                                               sizeof(batch) / sizeof(batch[0])
                                           ? sizeof(batch) / sizeof(batch[0])
                                           : (unsigned)d->cfg
                                                 .group_commit_rows);
        pthread_mutex_unlock(&d->lock);
        if (n == 0)
            continue;

        int txn = (d->kind == DB_SQLITE);
        if (txn)
            sqlite_exec_raw(d, "BEGIN IMMEDIATE");
        for (unsigned i = 0; i < n; ++i) {
            int rc;
            if (d->kind == DB_SQLITE)
                rc = sqlite_exec_raw(d, batch[i].sql);
            else
                rc = NSBC_OK; /* none：丢弃即成功；UNSUP 不入队 */
            job_done(&batch[i], rc);
        }
        if (txn)
            sqlite_exec_raw(d, "COMMIT");
        __sync_add_and_fetch(&d->committed, n);
    }
    return NULL;
}

int dbal_create(const dbal_cfg_t *cfg, dbal_t **out)
{
    dbal_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NSBC_NOMEM;
    d->cfg = *cfg;
    /* 钳制非法 group commit 参数，防 worker maxn=0 空转 */
    if (d->cfg.group_commit_rows <= 0)
        d->cfg.group_commit_rows = 100;
    if (d->cfg.group_commit_ms < 0)
        d->cfg.group_commit_ms = 10;
    pthread_mutex_init(&d->lock, NULL);
    /* 与 now_ms() 的 CLOCK_MONOTONIC 对齐（默认 cond 用 REALTIME） */
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&d->cond, &ca);
    pthread_condattr_destroy(&ca);

    if (!cfg->driver || strcmp(cfg->driver, "none") == 0) {
        d->kind = DB_NONE;
        llog(LOG_NOTICE, "dbal", "driver=none: writes discarded");
    } else if (strcmp(cfg->driver, "sqlite") == 0) {
#ifdef NSBC_WITH_SQLITE
        const char *path = cfg->dsn;
        if (strncmp(path, "file:", 5) == 0)
            path += 5;
        if (sqlite3_open(path, &d->sq) != SQLITE_OK) {
            llog(LOG_WARN, "dbal", "open %s failed", path);
            sqlite3_close(d->sq);
            d->sq = NULL;
            d->kind = DB_NONE; /* 降级：无盘可用时不阻断启动 */
        } else {
            sqlite3_busy_timeout(d->sq, 3000);
            sqlite_exec_raw(d, "PRAGMA journal_mode=WAL");
            sqlite_exec_raw(d, "PRAGMA synchronous=NORMAL");
            d->kind = DB_SQLITE;
            llog(LOG_NOTICE, "dbal", "sqlite WAL opened: %s", path);
        }
#else
        llog(LOG_WARN, "dbal",
             "sqlite driver built without NSBC_WITH_SQLITE; discarding");
        d->kind = DB_NONE;
#endif
    } else {
        /* mysql/postgres 为 P1（REQ-DB-002），不支持时显式失败可见 */
        llog(LOG_WARN, "dbal", "driver %s not built in (P1)", cfg->driver);
        d->kind = DB_STUB_UNSUP;
    }

    if (pthread_create(&d->thr, NULL, dbal_worker, d) != 0) {
        pthread_cond_destroy(&d->cond);
        pthread_mutex_destroy(&d->lock);
#ifdef NSBC_WITH_SQLITE
        if (d->sq)
            sqlite3_close(d->sq);
#endif
        free(d);
        return NSBC_ERR;
    }
    *out = d;
    return NSBC_OK;
}

int dbal_exec_async(dbal_t *d, const char *sql, dbal_done_fn cb, void *ud)
{
    if (!d)
        return NSBC_ERR;
    if (d->kind == DB_STUB_UNSUP)
        return NSBC_NOTSUP;
    db_job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NSBC_NOMEM;
    j->sql = strdup(sql ? sql : "");
    if (!j->sql) {
        free(j);
        return NSBC_NOMEM;
    }
    j->cb = cb;
    j->ud = ud;
    j->enq_ms = now_ms();

    pthread_mutex_lock(&d->lock);
    unsigned next = (d->qt + 1) % DB_QUEUE_CAP;
    if (next == d->qh) {
        pthread_mutex_unlock(&d->lock);
        __sync_add_and_fetch(&d->dropped, 1);
        free(j->sql);
        free(j);
        return NSBC_AGAIN; /* 背压：调用方熔断/降级（REQ-DB-005） */
    }
    d->q[d->qt] = *j;
    d->q[d->qt].used = 1;
    d->qt = next;
    int signal = (d->qt == (d->qh + 1) % DB_QUEUE_CAP);
    pthread_mutex_unlock(&d->lock);
    if (signal)
        pthread_cond_signal(&d->cond);
    free(j);
    return NSBC_OK;
}

uint64_t dbal_queued(dbal_t *d)
{
    if (!d)
        return 0;
    pthread_mutex_lock(&d->lock);
    uint64_t n = (d->qt + DB_QUEUE_CAP - d->qh) % DB_QUEUE_CAP;
    pthread_mutex_unlock(&d->lock);
    return n;
}

uint64_t dbal_committed(dbal_t *d)
{
    return d ? __sync_add_and_fetch(&d->committed, 0) : 0;
}

uint64_t dbal_dropped(dbal_t *d)
{
    return d ? __sync_add_and_fetch(&d->dropped, 0) : 0;
}

void dbal_destroy(dbal_t *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->lock);
    d->stop = 1;
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
    pthread_join(d->thr, NULL);
    /* 排空残留（停机语义：已入队尽量落库） */
    db_job_t j;
    for (;;) {
        pthread_mutex_lock(&d->lock);
        unsigned n = take_batch_locked(d, &j, 1);
        pthread_mutex_unlock(&d->lock);
        if (!n)
            break;
        int rc = d->kind == DB_SQLITE ? sqlite_exec_raw(d, j.sql) : NSBC_OK;
        job_done(&j, rc);
    }
#ifdef NSBC_WITH_SQLITE
    if (d->sq)
        sqlite3_close(d->sq);
#endif
    pthread_cond_destroy(&d->cond);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

#endif /* NSBC_NO_DBAL — end of dead-code DBAL guard */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
