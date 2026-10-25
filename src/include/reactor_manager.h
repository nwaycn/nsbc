#ifndef NSBC_REACTOR_MANAGER_H
#define NSBC_REACTOR_MANAGER_H

/* reactor_manager — reactor 集合抽象
 *
 * P2（阶段 2 基础）：单 reactor 强制 n=1，透明路由层。
 * P3（阶段 3 多 reactor）：真正多 reactor —— 每个 reactor 有独立 pthread
 * 跑 reactor_wait 循环，各自独立 epoll。SIP UDP 用 SO_REUSEPORT 每 reactor
 * 一个 socket，内核分流。reactor_mgr_route(hash_key) = hash_key % n，
 * 业务模块可按 hash 路由到正确 reactor。
 *
 * ctx.re 仍指向 reactors[0]（main 线程跑，耦合 ticker 时间轮）；
 * ctx.mgr 持有 manager 句柄，各模块可通过 reactor_mgr_route + reactor_mgr_get
 * 找到目标 reactor。
 *
 * 线程启动时序（关键——slot table race 防护）：
 *   1. reactor_mgr_create(n, max_events) —— 创建所有 reactor 对象（无线程）
 *   2. modmgr_start_all() —— 各模块 init 时 reactor_add（main 线程，安全）
 *   3. reactor_mgr_start_threads() —— 启动 reactor[1..N-1] 的 pthread
 *   4. main 线程继续跑 reactor[0] loop + ticker 时间轮
 *
 * shutdown 时序：
 *   1. reactor_mgr_stop_all() —— 所有 reactor 设 stop 标志
 *   2. reactor_mgr_join() —— join reactor[1..N-1] 线程（reactor_wait 返回，自 destroy）
 *   3. reactor[0] drain 完成后由 main 线程 destroy
 *   4. reactor_mgr_destroy() —— 只 free 数组和 manager 自身
 *
 * 配置键 core.reactors: 默认 "1"，允许 [1, 64]。 */

#include "reactor.h"
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct reactor_mgr reactor_mgr_t;

/* 创建 manager，内部创建 n 个 reactor 对象（但不启动任何线程）。
 * max_events 传给每个 reactor_create。
 * 返回的 reactor_t* 数组可通过 reactor_mgr_get() 访问；
 * reactor[0] 预留给 main 线程，reactor[1..n-1] 等待 start_threads 启动。 */
reactor_mgr_t *reactor_mgr_create(unsigned n, int max_events);

/* 取 id 号 reactor（id ∈ [0, n)）。
 * reactor[0] 赋给 ctx.re，由 main 线程跑；
 * reactor[1..n-1] 由 reactor_mgr_start_threads 创建的 pthread 跑。 */
reactor_t *reactor_mgr_get(reactor_mgr_t *m, unsigned id);

/* id → reactor* 映射。 */
reactor_t *reactor_mgr_by_id(reactor_mgr_t *m, unsigned id);

unsigned reactor_mgr_count(const reactor_mgr_t *m);

/* 按 hash_key 路由到 reactor id。
 * 单 reactor 时恒返回 0；多 reactor 时 hash_key % m->n。
 * 业务模块用此函数找目标 reactor：
 *   sip_transport → 按 SIP Call-ID hash 路由连接到同一 reactor
 *   media_bridge  → 按 relay 端口范围 hash 路由
 *   oam/metrics   → 固定 reactor[0]（低频操作，不追求并行） */
unsigned reactor_mgr_route(const reactor_mgr_t *m, uint64_t hash_key);

/* 启动 reactor[1..n-1] 的 pthread。
 * 必须在所有模块 reactor_add 完成后调用（slot table race 防护）。
 * reactor[0] 不创建线程——由 main 线程跑。 */
int reactor_mgr_start_threads(reactor_mgr_t *m);

/* Stop 所有 reactor（包括 reactor[0]）。
 * reactor_thread_fn 循环会因 reactor_stopping() 返回而退出。 */
void reactor_mgr_stop_all(reactor_mgr_t *m);

/* Join reactor[1..n-1] 线程。
 * 调用前必须先 reactor_mgr_stop_all()。
 * 每个 reactor 线程在 reactor_wait 返回后会自行调 reactor_destroy。 */
void reactor_mgr_join(reactor_mgr_t *m);

/* 销毁 manager。
 * 若线程已启动（start_threads 被调用过）—— 仅 free 数组和 manager 自身
 *   （每个 reactor 线程退出时已自 destroy）。
 * 若线程未启动（boot 失败路径）—— 由当前线程（main）调 reactor_destroy 每个 reactor
 *   （此时 reactor owner = main，安全）。 */
int reactor_mgr_destroy(reactor_mgr_t *m);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_REACTOR_MANAGER_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
