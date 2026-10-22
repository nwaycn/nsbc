#ifndef NSBC_REACTOR_H
#define NSBC_REACTOR_H

/* epoll reactor（core 层，每 I/O 线程一个实例）
 *
 * - 水平触发 + 非阻塞 fd（EPOLLONESHOT 可选）；
 * - 定时器不在 reactor 内建 timerfd（REQ-ARCH-007 禁用）：
 *   调用方按 tw_next_wait_ms 计算 epoll_wait 超时，唤醒后推进时间轮；
 * - reactor 不感知业务：fd→(cb,arg) 直查数组/哈希表由 net/sip 层注册。
 */

#include <stdint.h>
#include <sys/epoll.h>
#include "nsbc_common.h"

typedef struct reactor reactor_t;
typedef void (*reactor_cb_fn)(int fd, uint32_t events, void *arg);

reactor_t *reactor_create(int max_events);
void reactor_destroy(reactor_t *r);

int reactor_add(reactor_t *r, int fd, uint32_t events, reactor_cb_fn cb,
                void *arg);
int reactor_mod(reactor_t *r, int fd, uint32_t events);
int reactor_del(reactor_t *r, int fd);

/* 等待并处理一批事件；返回处理条数，<0 错误（EINTR 内部重试） */
int reactor_wait(reactor_t *r, int timeout_ms);

/* 优雅停止：从任意线程调用，使 reactor_wait 立即返回 */
void reactor_stop(reactor_t *r);
int reactor_stopping(const reactor_t *r);

#endif /* NSBC_REACTOR_H */
