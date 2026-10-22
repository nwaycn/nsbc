#ifndef NSBC_MEM_POOL_H
#define NSBC_MEM_POOL_H

/* 事务级/包级内存池（Bump Allocator，Phase 1 增量 2026-10-15）
 *
 * 设计目标：消除高 CPS 下的 malloc/free 系统调用开销与锁竞争。
 * Worker 线程/事务预分配大块内存，SIP 收包/解析/路由/构建全程
 * mem_pool_alloc；处理完毕后不调用 free，直接 mem_pool_reset 指针
 * 回拨，实现 O(1) 整块回收。
 *
 * 线程安全：每个 pool 实例只能被一个线程使用（worker 线程独占）；
 *           内部无锁，零原子操作。
 * 边界：空间不足返回 NULL，调用方按降级路径处理（如 malloc 兜底
 *       或丢弃该包）；严禁越界写，建议调用方配合已知上限值做预先
 *       预留（SIP_MAX_MSG 级别）。
 *
 * 生命周期：
 *   worker 启动期 → pool_create（一次）
 *   每包处理前    → pool_reset（O(1) 指针回拨）
 *   处理中        → pool_alloc / pool_calloc
 *   worker 退出  → pool_destroy
 *
 * 示例：
 *   mem_pool_t *p = mem_pool_create(256 * 1024, NULL);
 *   while (worker_running) {
 *       mem_pool_reset(p);
 *       sip_msg_t *msg = mem_pool_alloc(p, sizeof(*msg));
 *       msg->buf = mem_pool_alloc(p, SIP_MAX_MSG);
 *       sip_parse(msg);
 *       sip_route(msg);
 *       sip_build_reply(msg);
 *       send(msg->buf);
 *       // reset 自动释放 msg/buf —— 不需要 free
 *   }
 *   mem_pool_destroy(p);
 */

#include "nsbc_common.h"
#include "mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 类型定义                                                            */
/* ------------------------------------------------------------------ */

typedef struct mem_pool mem_pool_t;

/* ------------------------------------------------------------------ */
/* 创建/销毁                                                           */
/* ------------------------------------------------------------------ */

/* 创建 bump allocator。
 * bytes：预分配字节数；0 用默认 128KB。
 * arena：可选；非 NULL 时从该域 Arena 预留（物理隔离）；NULL 时 malloc。
 * 返回 NULL 表示 OOM。 */
mem_pool_t *mem_pool_create(size_t bytes, mem_arena_t *arena);

/* 销毁：释放整块预分配内存。销毁后所有 pool_alloc 返回的指针失效，
 * 调用方必须确保没有并发使用。 */
void mem_pool_destroy(mem_pool_t *p);

/* ------------------------------------------------------------------ */
/* 分配（O(1) bump，不扫描 free list）                                  */
/* ------------------------------------------------------------------ */

/* 从 pool 分配 n 字节，自动做 16 字节对齐。
 * 返回 NULL 表示空间不足（pool 没被 reset 过，或预分配太小）。
 * 禁止 free() 此指针——回收靠 mem_pool_reset 整块回拨。 */
void *mem_pool_alloc(mem_pool_t *p, size_t n);

/* 批量分配并清零：count * size，用于 SIP 报文解析的 token 数组
 * （sip_tok_t 通常需要 32~64 个，提前算好 count×size 一次性拿）。 */
void *mem_pool_calloc(mem_pool_t *p, size_t count, size_t size);

/* 字符串拷贝（pool 内部分配 + strcpy），解析时常用于保留 From/User-Agent
 * 等原始头字符串到 pool 缓冲区。返回 NULL 表示空间不足。 */
char *mem_pool_strdup(mem_pool_t *p, const char *s);

/* ------------------------------------------------------------------ */
/* O(1) 整块回收                                                       */
/* ------------------------------------------------------------------ */

/* 指针拨回原点，整块重用。不调用 free，O(1) 操作。
 * 处理完一个 SIP 包/事务后调用，释放整个包生命周期内分配的所有内存。 */
void mem_pool_reset(mem_pool_t *p);

/* ------------------------------------------------------------------ */
/* 统计与查询                                                           */
/* ------------------------------------------------------------------ */

/* 当前已用字节数（reset 后归零） */
size_t mem_pool_used(const mem_pool_t *p);
/* 预分配总字节数 */
size_t mem_pool_total(const mem_pool_t *p);
/* 累计分配次数（跨 reset 累加） */
uint64_t mem_pool_nalloc(const mem_pool_t *p);
/* 累计 OOM 次数（跨 reset 累加，用于监控是否需要调大 bytes） */
uint64_t mem_pool_nfail(const mem_pool_t *p);
/* 剩余空间 */
size_t mem_pool_avail(const mem_pool_t *p);

/* ------------------------------------------------------------------ */
/* 便捷宏：配合 caller 位置日志，失败时打印 pool 水位                    */
/* ------------------------------------------------------------------ */
#define MEM_POOL_ALLOC(p, n) \
    mem_pool_alloc_debug((p), (n), __FILE__, __LINE__)

#define MEM_POOL_CALLOC(p, count, size) \
    mem_pool_calloc_debug((p), (count), (size), __FILE__, __LINE__)

/* 内部实现：正常返回 NULL 时顺带打日志（DEBUG 级别），方便定位哪个包
 * 解析占用过高导致 OOM。生产构建仍调用 debug 版本，但日志由 log_level 控制。 */
void *mem_pool_alloc_debug(mem_pool_t *p, size_t n, const char *file, int line);
void *mem_pool_calloc_debug(mem_pool_t *p, size_t count, size_t size,
                            const char *file, int line);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_MEM_POOL_H */
