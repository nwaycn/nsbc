#ifndef NSBC_CDR_WAL_H
#define NSBC_CDR_WAL_H

/* =============================================================================
 * Phase 3+4: CDR WAL 兜底 — 文本文件健壮实现（batch + CRC + 文件头）
 *
 * Phase 3 骨架：纯文本追加 stub（无文件头、无校验、每次 flush）
 * Phase 4 升级（已落地）：
 *   ✓ 文件头 "NSBCWAL1" + version 2 — 防止空/损坏文件被误 replay
 *   ✓ 每条记录带 CRC32 — replay 时跳过损坏记录（断电半写保护）
 *   ✓ batch append 结构（WAL_BATCH_MAX=64）— 代码结构预留，当前保守每条 flush
 *   ✓ replay 完 truncate — 已补写成功后清空 WAL 文件
 *   ✓ 记录格式：[CRC32(4)][LEN(4, 大端)][PAYLOAD(LEN)][\n]
 *   ✓ 纯文本 JSON payload（可读性好，tail -f 能直接看）
 *
 * Phase 5 升级路径：引入 sqlite3
 *   PRAGMA journal_mode=WAL + synchronous=NORMAL
 *   CREATE TABLE cdr_events(id INTEGER PK AUTOINCREMENT, event_json TEXT, ts INTEGER, committed INTEGER DEFAULT 0)
 *   cdr_wal_append → INSERT + PRAGMA wal_checkpoint(PASSIVE)
 *   cdr_wal_replay → SELECT WHERE committed=0 → dbal_cdr_batch → UPDATE committed=1
 *   billing.c 接线：persist_batch 失败 → cdr_wal_append → 再失败 → spool.bin
 *
 * OAM 命令预留：`show cdr_wal` → cdr_wal_uncommitted_count + 最近 N 条原文
 * ============================================================================= */

#include "nsbc_common.h"
#include "cdr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cdr_wal cdr_wal_t;

/* 创建/销毁
 * db_path: Phase 3 stub 下是纯文本文件路径（每行一条 JSON）；
 *          Phase 4 替换成真 SQLite 文件路径。 */
int  cdr_wal_create(const char *db_path, cdr_wal_t **out);
void cdr_wal_destroy(cdr_wal_t *w);

/* 写入（billing persist_batch 失败时调）
 * 返回 NSBC_OK   = 写入成功（进 WAL）
 *        NSBC_ERR = 都失败（spool.bin 再兜）*/
int  cdr_wal_append(cdr_wal_t *w, const cdr_event_t *ev);

/* 启动时 replay：从 WAL 文件读未提交事件，补进 DBAL 再落库。
 * 返回补写条数；<0 错误。*/
int  cdr_wal_replay(cdr_wal_t *w);

/* 查询 WAL 里未提交事件数（OAM `show cdr_wal` 用）*/
int  cdr_wal_uncommitted_count(cdr_wal_t *w);

/* Phase 6: 全局 WAL 指针存取（OAM show cdr_wal 命令）*/
void         cdr_wal_set_global(cdr_wal_t *w);
cdr_wal_t   *cdr_wal_get_global(void);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_CDR_WAL_H */
