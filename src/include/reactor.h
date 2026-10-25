#ifndef NSBC_REACTOR_H
#define NSBC_REACTOR_H

/* epoll reactor（core 层，每 I/O 线程一个实例）
 *
 * - 水平触发 + 非阻塞 fd（EPOLLONESHOT 可选）；
 * - 定时器不在 reactor 内建 timerfd（REQ-ARCH-007 禁用）：
 *   调用方按 tw_next_wait_ms 计算 epoll_wait 超时，唤醒后推进时间轮；
 * - reactor 不感知业务：fd→(cb,arg) 直查数组/哈希表由 net/sip 层注册。
 *
 * P1 owner 归属：
 *   reactor_add/del 读写 owner-only 的 callback 表，严格只允许 owner 线程调用。
 *   reactor_mod 只改内核 epoll mask，任何线程安全。
 *   生产构建默认不强制 owner 断言（已知跨线程调用点见下方审计表）；
 *   定义 NSBC_REACTOR_OWNER_CHECK 时 debug 构建会在 add/del 入口 abort。
 *
 * P1 调用点审计（56 处 reactor 调用，按线程归属分类）：
 *   线程 = reactor owner（安全，未来 manager 抽象零改动）:
 *     - obs_control.c   : reactor_add/del/mod（UDS accept + 连接生命周期 + EAGAIN flush）
 *     - esl_server.c    : reactor_add/del（UDS/TCP accept + 连接生命周期）
 *     - obs_metrics.c   : reactor_add/del（metrics accept + 连接生命周期）
 *     - sip_transport.c : reactor_mod（TCP flush EPOLLOUT + TLS 握手 WANT_READ/WRITE）
 *                         reactor_add（sip_udp_open + sip_tcp_open accept）
 *                         reactor_del（conn_close — 全部在 on_tcp_event 回调上下文 = owner 线程）
 *     - media_bridge.c  : reactor_mod（relay_cb 内部无 reactor_mod 调用）
 *     - main.c          : reactor_add（signal fd 注册，owner 线程）
 *
 *   线程 = 非 owner 但当前安全（单 reactor 模式下 callback 表只写不读竞争）:
 *     - media_bridge.c : reactor_add（rtp_relay_arm）
 *                         reactor_del（rtp_relay_stop）
 *                         — 从媒体/业务线程调用；单 reactor 下安全。
 *                           多 reactor manager 实现后必须改成通过 manager 路由。
 *
 *   reactor_call : 所有路径，跨线程同步 owner 执行（唯一例外：owner 线程直调）。
 */

#include <stdint.h>
#include <sys/epoll.h>
#include "nsbc_common.h"

typedef struct reactor reactor_t;
typedef void (*reactor_cb_fn)(int fd, uint32_t events, void *arg);
typedef int (*reactor_task_fn)(void *arg);

#define REACTOR_TASK_QUEUE_CAP 4096

typedef struct {
    uint32_t queued;
    uint64_t submitted;
    uint64_t completed;
    uint64_t rejected;
} reactor_task_stats_t;

reactor_t *reactor_create(int max_events);
/* Destroy must run on the owner while reactor_wait is inactive and all callers
 * are quiesced. It closes the mailbox before checking for queued commands.
 * Returns NSBC_BUSY if a wait or queued command is still active; callers must
 * not assume the reactor was freed on error. */
int reactor_destroy(reactor_t *r);

/* add/del update the owner-only fd callback table and must be serialized by its
 * owner. mod only changes the kernel epoll mask and is safe from other threads. */
int reactor_add(reactor_t *r, int fd, uint32_t events, reactor_cb_fn cb,
                void *arg);
int reactor_mod(reactor_t *r, int fd, uint32_t events);
int reactor_del(reactor_t *r, int fd);

/* Reactor 创建线程是 owner，reactor_wait 只能由该线程调用。
 * 在 owner 线程同步执行命令；其他线程须在 event loop 启动后调用并等待完成。
 * 调用方必须持有 arg 至本函数返回，且不得持有 callback 可能需要的锁。
 * 队列有界；未启动返回 NSBC_AGAIN，满队列返回 NSBC_NOSPC，停机返回
 * NSBC_SHUTDOWN。owner 线程始终直接执行命令，以支持停止后的资源清理。 */
int reactor_call(reactor_t *r, reactor_task_fn fn, void *arg);
void reactor_get_task_stats(const reactor_t *r, reactor_task_stats_t *out);

/* 等待并处理一批事件；返回处理条数，<0 错误（EINTR 内部重试） */
int reactor_wait(reactor_t *r, int timeout_ms);

/* 优雅停止：从任意线程调用，通过内部 eventfd 唤醒阻塞中的 reactor_wait。
 * 销毁前调用方必须确保 reactor_wait 已返回，不能与 wait 并发 destroy。 */
void reactor_stop(reactor_t *r);
int reactor_stopping(const reactor_t *r);

#endif /* NSBC_REACTOR_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
