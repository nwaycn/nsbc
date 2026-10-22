#ifndef NSBC_CACHEDB_BACKEND_H
#define NSBC_CACHEDB_BACKEND_H

/* =============================================================================
 * Phase 3: Cachedb 后端自注册（Registry Pattern + GCC __atomic_*）
 *
 * 当前 cachedb.c 是单实现（内存 + 可选 Redis）。Phase 3 升级成多 backend
 * 注册表，各 backend 动态模块独立注册：
 *
 *   backend_register("redis",  redis_ops)  — mod_cachedb_redis 动态模块
 *   backend_register("lmdb",   lmdb_ops)   — mod_cachedb_lmdb  动态模块
 *   backend_register("sqlite", sqlite_ops) — mod_cachedb_sqlite动态模块
 *   backend_register("mem",    mem_ops)    — 内置默认 backend
 *
 * cachedb.c 启动时：
 *   cfg 读 "cachedb.backend=redis" → backend_find("redis") → 使用
 *   找不到则 fallback 到 backend_find("mem")
 *
 * Phase 3 骨架：注册表 + 内置 mem backend（简单哈希表 + ttl tick stub）
 * Phase 4 补全清单：
 *   1. mem backend：补 ttl 到期清理定时器（timer_wheel 或 tick 驱动）
 *   2. 引入 redis / lmdb / sqlite 动态模块（.so/.dll，backend_register 自注册）
 *   3. cachedb.c 接线：cfg 读取 backend 名 → backend_find → 替换现有 if(redis)
 *   4. 指标：metric_reg 暴露 cachedb_backend_ops 的 get/set 延迟、命中率
 *   5. backend_register 返回 NSBC_EXISTS 时允许覆盖（后注册的同名覆盖旧的，
 *      方便测试桩覆盖生产 backend）
 * ============================================================================= */

#include "nsbc_common.h"
#include "nsbc_atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 3 先锁死 4 槽，Phase 4 如果 backend 多了再扩或改成链表/RB 树 */
#define CACHEDB_BACKEND_MAX 4

typedef struct cachedb_backend_ops {
    const char *name;                        /* "redis", "lmdb", "sqlite", "mem" */
    int  (*open)(const char *dsn, void **handle);
    void (*close)(void *handle);
    int  (*get)(void *handle, const char *key, char **val, size_t *len);
    int  (*set)(void *handle, const char *key, const char *val, size_t len,
                int ttl_ms);
    int  (*del)(void *handle, const char *key);
    int  (*ping)(void *handle);              /* 健康检查 */
} cachedb_backend_ops_t;

/* 注册/查找（GCC __atomic_* 风格的并发保护）
 * register: 按 name 线性扫描 + 覆盖（同名后注册的生效）
 * find:     按 name 线性扫描，返回 NULL=未找到
 * 返回 NSBC_OK=注册成功；NSBC_NOSPC=槽已满；NSBC_INVAL=ops 缺字段 */
int  cachedb_backend_register(const cachedb_backend_ops_t *ops);
const cachedb_backend_ops_t *cachedb_backend_find(const char *name);

/* 内置内存 backend（默认 fallback）—— Phase 3 骨架实现，
 * 启动时在 cachedb_init 里先调 cachedb_mem_register() */
int  cachedb_mem_register(void);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_CACHEDB_BACKEND_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
