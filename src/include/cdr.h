#ifndef NSBC_CDR_H
#define NSBC_CDR_H

/* 话单落库（REQ-PBX-CDR-001，P0）
 *
 * sip/core 只发 cdr_event_t（经 nsbc_ctx_t.cdr_emit 槽），本模块：
 *  - 按 call_id 聚合 START/ANSWER 片段，END 时补齐时长/释放原因；
 *  - END 话单先入内存环（批量/削峰），经 persist 回调批量写 DBAL；
 *  - 写库失败落本地 spool 文件（断电不丢；P1），进程退出 drain。
 * 隐私：emergency=1 的话单豁免按租户的留存期清理（紧急救援对账）。
 */
#include "nsbc_common.h"
#include "cdr_event.h"
#include "worker.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CDR_RING_CAP  8192
#define CDR_OPEN_CAP  20000   /* 在通片段槽（call_id 聚合） */
#define CDR_BATCH_MAX 256

typedef struct {
    /* DBAL insert（接线批实现；失败返回非 0，话单留环重试） */
    int (*persist_batch)(void *ud, const cdr_event_t *ev, int n);
    void *ud;
    worker_pool_t *workers; /* optional bounded pool for persistence flush */
} cdr_ops_t;

typedef struct cdr_sink cdr_sink_t;

int  cdr_sink_create(const cdr_ops_t *ops, cdr_sink_t **out);
void cdr_sink_destroy(cdr_sink_t *s);

/* 挂到 nsbc_ctx_t.cdr_emit（签名 cdr_emit(void *ud, const cdr_event_t *)） */
void cdr_sink_emit(void *ud, const cdr_event_t *ev);

/* 立即刷一批（停机 drain/周期任务）；返回已刷条数，<0 错误 */
int  cdr_sink_flush(cdr_sink_t *s);

/* 在通片段数（OAM：CDR 聚合槽占用） */
int  cdr_sink_open_count(cdr_sink_t *s);
int  cdr_sink_pending_count(const cdr_sink_t *s);

/* OAM 只读：格式化最近 N 条已完成 CDR 到文本缓冲（非消费，不改 ring 头；
 * 从 ring_tail 向 head 倒序迭代——新事件靠 tail，即倒序输出 = 最新在前）。
 * 输出帧格式每 CDR 一行 k=v token（与 Web 侧 parse_cdr_recent 对齐）；
 * 尾部附锚行 `cdrs=N`（N 实际输出条数）。返回 NSBC_OK 或负错误码。 */
int  cdr_sink_recent(cdr_sink_t *s, int limit, char *buf, size_t bufsz);

/* OAM 只读计数（供 `show cdr`） */
uint64_t cdr_sink_total_emitted(const cdr_sink_t *s);
uint64_t cdr_sink_total_persisted(const cdr_sink_t *s);
uint64_t cdr_sink_total_spooled(const cdr_sink_t *s);
uint64_t cdr_sink_total_dropped(const cdr_sink_t *s);

/* 启动时 replay 用：billing_init 设置 spool 路径后调 replay 把遗留文件
 * 话单 push 进 ring，再进入 emit 消费循环。 */
void cdr_sink_set_spool_path(cdr_sink_t *s, const char *path);
void cdr_sink_replay_spool(cdr_sink_t *s);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_CDR_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
