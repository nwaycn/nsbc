/* endpoint_registry.c — Phase 1 新建：协议端点自注册 + 安全卸载
 *
 * 设计来源：白皮书 §2.1 endpoint_ops_t + §2.3 安全卸载模板
 * 原子风格：项目约定 __atomic 内置函数（非 C11 stdatomic.h）
 *
 * 使用流程（模块 init）：
 *   endpoint_ops_t ops = { .name="sip", .accept=..., ... };
 *   endpoint_registry_register(&ops);
 *   // ... module fini:
 *   endpoint_registry_unregister("sip");  // 内部会先置 unloading + 自旋 wait
 *
 * 调用流程（signaling 入站处理）：
 *   int slot = -1;
 *   const endpoint_ops_t *ops = endpoint_registry_find_inc("sip", &slot);
 *   if (ops) { ops->handle_invite(ctx, call); endpoint_registry_dec(slot); }
 */
#include "endpoint_registry.h"
#include "log.h"
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <sched.h>
#include <time.h>

/* === 条目结构（白皮书安全卸载模板） === */
typedef struct {
    const endpoint_ops_t *ops;   /* ops 指针（动态模块卸载后会悬空，必须先 unloading+wait） */
    volatile int refcnt;        /* 当前在执行的回调数（__atomic_fetch_add/sub） */
    volatile int unloading;     /* 卸载标记：1=阻止新调用进来 */
} ep_slot_t;

static ep_slot_t g_slots[ENDPOINT_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* === 注册 === */
int endpoint_registry_register(const endpoint_ops_t *ops)
{
    if (!ops || !ops->name || !ops->name[0]) return NSBC_INVAL;

    pthread_mutex_lock(&g_lock);
    /* 同名已存在 → NSBC_EXISTS（endpoint 协议不应被覆盖） */
    for (int i = 0; i < ENDPOINT_MAX; ++i) {
        if (g_slots[i].ops && strcmp(g_slots[i].ops->name, ops->name) == 0) {
            pthread_mutex_unlock(&g_lock);
            log_warn("endpoint_reg", "'%s' already registered", ops->name);
            return NSBC_EXISTS;
        }
    }
    /* 找空槽 */
    for (int i = 0; i < ENDPOINT_MAX; ++i) {
        if (g_slots[i].ops == NULL && __atomic_load_n(&g_slots[i].unloading, __ATOMIC_RELAXED) == 0) {
            g_slots[i].ops = ops;
            __atomic_store_n(&g_slots[i].refcnt, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&g_slots[i].unloading, 0, __ATOMIC_RELAXED);
            pthread_mutex_unlock(&g_lock);
            log_info("endpoint_reg", "registered '%s' slot=%d", ops->name, i);
            return NSBC_OK;
        }
    }
    pthread_mutex_unlock(&g_lock);
    log_err("endpoint_reg", "table full (%d)", ENDPOINT_MAX);
    return NSBC_NOSPC;
}

/* === 查找 + INC === */
const endpoint_ops_t *endpoint_registry_find_inc(const char *name, int *out_slot)
{
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < ENDPOINT_MAX; ++i) {
        if (__atomic_load_n(&g_slots[i].unloading, __ATOMIC_ACQUIRE)) continue;
        const endpoint_ops_t *op = __atomic_load_n(&g_slots[i].ops, __ATOMIC_ACQUIRE);
        if (!op) continue;
        if (strcmp(op->name, name) == 0) {
            __atomic_fetch_add(&g_slots[i].refcnt, 1, __ATOMIC_ACQ_REL);
            if (out_slot) *out_slot = i;
            return op;
        }
    }
    return NULL;
}

/* === 查找（不带 INC，只查） === */
const endpoint_ops_t *endpoint_registry_find(const char *name)
{
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < ENDPOINT_MAX; ++i) {
        if (__atomic_load_n(&g_slots[i].unloading, __ATOMIC_ACQUIRE)) continue;
        const endpoint_ops_t *op = __atomic_load_n(&g_slots[i].ops, __ATOMIC_ACQUIRE);
        if (op && strcmp(op->name, name) == 0) return op;
    }
    return NULL;
}

/* === DEC === */
void endpoint_registry_dec(int slot)
{
    if (slot < 0 || slot >= ENDPOINT_MAX) return;
    __atomic_fetch_sub(&g_slots[slot].refcnt, 1, __ATOMIC_ACQ_REL);
}

/* === 卸载等待 === */
int endpoint_registry_unload_wait(const char *name, int timeout_ms)
{
    if (!name || !name[0]) return NSBC_INVAL;

    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < ENDPOINT_MAX; ++i) {
        if (g_slots[i].ops && strcmp(g_slots[i].ops->name, name) == 0) {
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);

    if (slot < 0) return NSBC_NOENT;

    /* 1. 先置 unloading=true（阻止新调用进来） */
    __atomic_store_n(&g_slots[slot].unloading, 1, __ATOMIC_RELEASE);

    /* 2. 自旋等待 refcnt→0 */
    int spins = 0;
    int deadline_us = timeout_ms > 0 ? timeout_ms * 1000 : 0;
    while (__atomic_load_n(&g_slots[slot].refcnt, __ATOMIC_ACQUIRE) > 0) {
        if (deadline_us > 0 && spins > deadline_us / 1000) {
            /* 超时：fail-safe 留着泄漏但不 crash（白皮书 §十四 10.4 规则） */
            log_err("endpoint_reg",
                    "unload '%s' timeout (refcnt=%d) — keeping slot but unloading=true",
                    name, __atomic_load_n(&g_slots[slot].refcnt, __ATOMIC_ACQUIRE));
            return NSBC_BUSY;
        }
        sched_yield();
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 * 1000 }; /* 1ms */
        nanosleep(&ts, NULL);
        ++spins;
    }

    /* 3. 安全清空 ops 指针（解锁 + 原子 store） */
    pthread_mutex_lock(&g_lock);
    g_slots[slot].ops = NULL;
    pthread_mutex_unlock(&g_lock);
    log_info("endpoint_reg", "unloaded '%s' slot=%d (clean)", name, slot);
    return NSBC_OK;
}

/* === 显式 unregister（等 refcnt→0 + 清空条目） === */
int endpoint_registry_unregister(const char *name)
{
    return endpoint_registry_unload_wait(name, 5000);  /* 默认 5s 超时 */
}

/* === 计数 === */
int endpoint_registry_count(void)
{
    int n = 0;
    for (int i = 0; i < ENDPOINT_MAX; ++i) {
        if (__atomic_load_n(&g_slots[i].ops, __ATOMIC_ACQUIRE) != NULL) ++n;
    }
    return n;
}

/* === 按槽位号查（OAM show endpoints 遍历用）===
 *
 * 返回 ops 指针（NULL=空槽），带 unloading 标记判断。
 * 运行期只读，零锁（同 try_accept 性能审计结论）。*/
const endpoint_ops_t *endpoint_registry_find_by_slot(int slot, int *out_unloading, int *out_refcnt)
{
    if (slot < 0 || slot >= ENDPOINT_MAX) return NULL;
    int unloading = __atomic_load_n(&g_slots[slot].unloading, __ATOMIC_ACQUIRE);
    const endpoint_ops_t *op = __atomic_load_n(&g_slots[slot].ops, __ATOMIC_ACQUIRE);
    if (out_unloading) *out_unloading = unloading;
    if (out_refcnt)    *out_refcnt    = unloading ? 0
                                    : (int)__atomic_load_n(&g_slots[slot].refcnt, __ATOMIC_RELAXED);
    return op;
}

/* === Phase 5 XL: try_accept — 遍历所有 endpoint ===
 *
 * 遍历所有 ENDPOINT_MAX 槽，跳过 unloading 条目，对每个已注册 endpoint 调 accept。
 * 第一个 accept 返回 NSBC_OK 或 NSBC_PERM_DENIED 的就接管。
 * 接管成功后 atomic refcnt++，用完调用方必须 endpoint_registry_dec。
 * accept 内部创建的 endpoint 私有 ctx 通过 out_ctx 返回（调用方后续传给 handle_invite）。 */
const endpoint_ops_t *endpoint_registry_try_accept(sip_msg_t *m, int *out_slot, void **out_ctx)
{
    if (!m) return NULL;

    /* Phase 7 性能审计：运行期不持锁！
     *
     * endpoint_registry_register/unregister 只在 sig_init / sig_stop 调（单线程串行），
     * 运行期 ops 数组是 immutable。try_accept 直接 __atomic_load_n 取 ops 指针
     * 遍历即可，零锁零竞争。accept 回调本身是 endpoint 私有逻辑（可能是 NULL 判断），
     * 无阻塞。refcnt++ 用 __atomic_fetch_add 是 lock-free。
     *
     * 临界：如果未来允许运行期 unregister（modmgr_unload endpoint），
     * 那时需要 RCU 读侧宣告 sat_rmb_seqcst() 或把 try_accept 放回锁里。
     * 当前 endpoint 卸载只能在 fini（停机期），所以安全。*/

    for (int i = 0; i < ENDPOINT_MAX; ++i) {
        if (__atomic_load_n(&g_slots[i].unloading, __ATOMIC_ACQUIRE)) continue;
        const endpoint_ops_t *op = __atomic_load_n(&g_slots[i].ops, __ATOMIC_ACQUIRE);
        if (!op || !op->accept) continue;

        void *ep_ctx = NULL;
        int rc = op->accept(m, &ep_ctx);
        if (rc == NSBC_OK || rc == NSBC_PERM_DENIED) {
            /* 命中 — lock-free refcnt++，返回 */
            __atomic_fetch_add(&g_slots[i].refcnt, 1, __ATOMIC_ACQ_REL);
            if (out_slot) *out_slot = i;
            if (out_ctx)  *out_ctx  = ep_ctx;
            return op;
        }
    }

    return NULL;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
