#ifndef NSBCD_SIGNAL_H
#define NSBCD_SIGNAL_H

/* 信号处理（core 层）：self-pipe 唤醒 reactor，处理器内只做 async-signal-safe
 * 的 write（SIGTERM/SIGINT 优雅停机；SIGHUP 配置重加载检查；SIGPIPE 忽略）。
 */

#include "nsbc_common.h"

int nsbcd_signal_init(void);
int nsbcd_signal_fd(void);
void nsbcd_signal_ack(void); /* reactor 可读后读出管道字节 */

int nsbcd_signal_stop_pending(void);
int nsbcd_signal_reload_pending(void);
void nsbcd_signal_reload_clear(void);

#endif /* NSBCD_SIGNAL_H */
