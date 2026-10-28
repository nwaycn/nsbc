/* Architectural constraint guard — C core nsbcd does NOT touch DB.
 * Rust web side (sqlx Any driver) is the sole persistence layer.
 *
 * Enabling: comment out "#define NSBC_NO_DBAL" below AND remove src/db/%.c
 *           from Makefile filter-out. */
#ifndef NSBC_NO_DBAL
#define NSBC_DBAL_H

/* 数据库访问抽象层（REQ-DB-003/005，P0）
 *
 * - 业务代码只经本层访问数据库，严禁直接调用 sqlite3/libpq/mysql API；
 * - 全异步：写/查提交到固定容量环形队列，专用 DB worker 落库；
 *   队列满按背压返回 NSBC_AGAIN（调用方超时熔断/降级，严禁无界增长）；
 * - group commit：worker 攒够 group_commit_rows 行或最老语句等待超过
 *   group_commit_ms 即一次事务批量提交（REQ-DB-005，≥100 行或 ≥10ms）；
 * - SQLite 内置（WAL），MySQL/PG 为 P1；driver=none 时丢弃并计数
 *   （无盘环境/单元测试）。
 * 关键路径零同步等待：dbal_exec_async 只复制 SQL 并入队。
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dbal dbal_t;

typedef void (*dbal_done_fn)(int rc, void *ud);

typedef struct {
    const char *driver; /* sqlite / mysql / postgres / none */
    const char *dsn;    /* sqlite: "file:/var/lib/nsbc/nsbc.db" */
    int pool_size;
    int group_commit_ms;
    int group_commit_rows;
} dbal_cfg_t;

int dbal_create(const dbal_cfg_t *cfg, dbal_t **out);
void dbal_destroy(dbal_t *d);

/* 异步执行（P0 为写接口；查询 future 为 P1）。
 * 返回 NSBC_OK=已入队；NSBC_AGAIN=队列满背压；NSBC_NOTSUP=无驱动。 */
int dbal_exec_async(dbal_t *d, const char *sql, dbal_done_fn cb, void *ud);

uint64_t dbal_queued(dbal_t *d);
uint64_t dbal_committed(dbal_t *d);
uint64_t dbal_dropped(dbal_t *d);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_DBAL_H */
#endif /* NSBC_NO_DBAL — end of dead-code DBAL guard */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
