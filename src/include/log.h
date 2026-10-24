#ifndef NSBC_LOG_H
#define NSBC_LOG_H

/* REQ-ARCH-009（P0）：异步结构化日志
 *
 * - 每线程固定容量环形缓冲（不可交接域，posix_memalign 分配），
 *   日志线程聚合成批落盘；
 * - 缓冲满：DEBUG/INFO 丢弃并计数，ERR 及以上短暂自旋保底，
 *   严禁因磁盘抖动反压信令线程；
 * - 审计通道独立（audit 文件/O_APPEND），任何级别都不允许丢弃；
 * - REQ-OBS-004：日志可携带 Call-ID/trace-id 做端到端关联。
 */

#include <stddef.h>
#include <stdint.h>
#include "utils.h"   /* mkdir_p() — 通用工具模块 src/utils/utils.c */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_EMERG = 0,
    LOG_ALERT = 1,
    LOG_CRIT = 2,
    LOG_ERR = 3,
    LOG_WARN = 4,
    LOG_NOTICE = 5,
    LOG_INFO = 6,
    LOG_DEBUG = 7
} log_level_t;

#define LOG_RECORD_MAX 768

/* path=NULL 输出到 stderr；ring_slots 必须是 2 的幂，建议 512。
 * 返回非 0 失败（启动 fail-fast）。 */
int log_init(const char *path, log_level_t level, uint32_t ring_slots);
int log_audit_open(const char *path);
void log_set_level(log_level_t level);
log_level_t log_get_level(void);  /* 当前级别（供 console loglevel 回显） */
void log_set_rotate(long long max_bytes, int keep);  /* size-based self-rotate; 0 bytes = off */
void log_stop(void); /* 排空并停日志线程 */

/* 工作线程显式登记（worker/net/timer 线程创建时调用，退出时 detach） */
int log_thread_attach(void);
void log_thread_detach(void);

void log_emit(log_level_t lvl, const char *mod, const char *callid,
              const char *file, int line,
              const char *fmt, ...) __attribute__((format(printf, 6, 7)));

/* 审计：独立通道、同步 O_APPEND，绝不因日志环满丢弃 */
void log_audit(const char *actor, const char *action, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define llog(lvl, mod, ...) log_emit((lvl), (mod), NULL, __FILE__, __LINE__, __VA_ARGS__)
#define llog_cid(lvl, mod, cid, ...)                                           \
    log_emit((lvl), (mod), (cid), __FILE__, __LINE__, __VA_ARGS__)

/* 便捷宏（模块名 + fmt...）：recording.c / outbound_reg.c / sip_transport.c 使用 */
#define log_emerg(mod, ...)  llog(LOG_EMERG,  (mod), __VA_ARGS__)
#define log_alert(mod, ...)  llog(LOG_ALERT,  (mod), __VA_ARGS__)
#define log_crit(mod, ...)   llog(LOG_CRIT,   (mod), __VA_ARGS__)
#define log_err(mod, ...)    llog(LOG_ERR,    (mod), __VA_ARGS__)
#define log_warn(mod, ...)   llog(LOG_WARN,   (mod), __VA_ARGS__)
#define log_notice(mod, ...) llog(LOG_NOTICE, (mod), __VA_ARGS__)
#define log_info(mod, ...)   llog(LOG_INFO,   (mod), __VA_ARGS__)
#define log_debug(mod, ...)  llog(LOG_DEBUG,  (mod), __VA_ARGS__)

/* 丢弃/写入计数（REQ-ARCH-009 验收） */
uint64_t log_dropped(void);
uint64_t log_written(void);

/* 从所有 per-thread ring 导出未消费的日志记录。
 * 在 g_reg_lock 下读取 head-tail 之间的条目（不推进 tail，不干扰 logger 线程），
 * 逐条 memcpy 到 out_buf（每条末尾已带 '\n'，由 log_emit 格式化时写入）。
 * max_lines 上限保护（建议 200~2000），返回写入的字节数；buf 不够装时截断
 * 最后一条。out_buf=NULL 返回估算字节数（仅计数）。 */
size_t log_ring_tail(char *out_buf, size_t out_sz, int max_lines);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_LOG_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
