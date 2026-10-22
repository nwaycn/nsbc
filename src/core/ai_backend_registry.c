/* ai_backend_registry.c — Phase 2 新建：AI 后端自注册 + 安全卸载
 *
 * 设计来源：白皮书 §2.1 ai_backend_ops_t + §2.3 安全卸载模板
 * 原子风格：项目约定 __atomic 内置函数（非 C11 stdatomic.h）
 *
 * 与 endpoint_registry 的关键差异：register 同名已存在 → **覆盖**
 * （AI backend 场景：动态模块 reload 同 backend 版本时，新 ud 指针
 * 替换旧指针；调用方假设覆盖时无并发执行中，refcnt/unloading 保持原值）
 */
#include "ai_backend_registry.h"
#include "log.h"
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <sched.h>
#include <time.h>

/* === 条目结构（白皮书安全卸载模板） === */
typedef struct {
    const ai_backend_ops_t *ops;   /* ops 指针（动态模块卸载后会悬空，必须先 unloading+wait） */
    volatile int refcnt;        /* 当前在执行的回调数（__atomic_fetch_add/sub） */
    volatile int unloading;     /* 卸载标记：1=阻止新调用进来 */
} ab_slot_t;

static ab_slot_t g_backends[AI_BACKEND_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* === 注册（AI backend 允许同名覆盖，endpoint 不允许） === */
int ai_backend_registry_register(const ai_backend_ops_t *ops)
{
    if (!ops || !ops->name || !ops->name[0]) return NSBC_INVAL;

    pthread_mutex_lock(&g_lock);

    /* 同名已存在 → 覆盖（AI backend reload 场景）
     * 注意：覆盖时保持 refcnt/unloading 原值，假设覆盖场景无并发执行中 */
    for (int i = 0; i < AI_BACKEND_MAX; ++i) {
        if (g_backends[i].ops && strcmp(g_backends[i].ops->name, ops->name) == 0) {
            g_backends[i].ops = ops;
            pthread_mutex_unlock(&g_lock);
            log_info("ai_backend", "overwritten '%s' slot=%d (reload)", ops->name, i);
            return NSBC_OK;
        }
    }

    /* 找空槽 */
    for (int i = 0; i < AI_BACKEND_MAX; ++i) {
        if (g_backends[i].ops == NULL && __atomic_load_n(&g_backends[i].unloading, __ATOMIC_RELAXED) == 0) {
            g_backends[i].ops = ops;
            __atomic_store_n(&g_backends[i].refcnt, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&g_backends[i].unloading, 0, __ATOMIC_RELAXED);
            pthread_mutex_unlock(&g_lock);
            log_info("ai_backend", "registered '%s' slot=%d", ops->name, i);
            return NSBC_OK;
        }
    }
    pthread_mutex_unlock(&g_lock);
    log_err("ai_backend", "table full (%d)", AI_BACKEND_MAX);
    return NSBC_NOSPC;
}

/* === 查找 + INC === */
const ai_backend_ops_t *ai_backend_registry_find_inc(const char *name, int *out_slot)
{
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < AI_BACKEND_MAX; ++i) {
        if (__atomic_load_n(&g_backends[i].unloading, __ATOMIC_ACQUIRE)) continue;
        const ai_backend_ops_t *op = __atomic_load_n(&g_backends[i].ops, __ATOMIC_ACQUIRE);
        if (!op) continue;
        if (strcmp(op->name, name) == 0) {
            __atomic_fetch_add(&g_backends[i].refcnt, 1, __ATOMIC_ACQ_REL);
            if (out_slot) *out_slot = i;
            return op;
        }
    }
    return NULL;
}

/* === 查找（不带 INC，只查） === */
const ai_backend_ops_t *ai_backend_registry_find(const char *name)
{
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < AI_BACKEND_MAX; ++i) {
        if (__atomic_load_n(&g_backends[i].unloading, __ATOMIC_ACQUIRE)) continue;
        const ai_backend_ops_t *op = __atomic_load_n(&g_backends[i].ops, __ATOMIC_ACQUIRE);
        if (op && strcmp(op->name, name) == 0) return op;
    }
    return NULL;
}

/* === DEC === */
void ai_backend_registry_dec(int slot)
{
    if (slot < 0 || slot >= AI_BACKEND_MAX) return;
    __atomic_fetch_sub(&g_backends[slot].refcnt, 1, __ATOMIC_ACQ_REL);
}

/* === 卸载等待 === */
int ai_backend_registry_unload_wait(const char *name, int timeout_ms)
{
    if (!name || !name[0]) return NSBC_INVAL;

    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < AI_BACKEND_MAX; ++i) {
        if (g_backends[i].ops && strcmp(g_backends[i].ops->name, name) == 0) {
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);

    if (slot < 0) return NSBC_NOENT;

    /* 1. 先置 unloading=true（阻止新调用进来） */
    __atomic_store_n(&g_backends[slot].unloading, 1, __ATOMIC_RELEASE);

    /* 2. 自旋等待 refcnt→0 */
    int spins = 0;
    int deadline_us = timeout_ms > 0 ? timeout_ms * 1000 : 0;
    while (__atomic_load_n(&g_backends[slot].refcnt, __ATOMIC_ACQUIRE) > 0) {
        if (deadline_us > 0 && spins > deadline_us / 1000) {
            log_err("ai_backend",
                    "unload '%s' timeout (refcnt=%d) — keeping slot but unloading=true",
                    name, __atomic_load_n(&g_backends[slot].refcnt, __ATOMIC_ACQUIRE));
            return NSBC_BUSY;
        }
        sched_yield();
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 * 1000 }; /* 1ms */
        nanosleep(&ts, NULL);
        ++spins;
    }

    /* 3. 安全清空 ops 指针（解锁 + 原子 store） */
    pthread_mutex_lock(&g_lock);
    g_backends[slot].ops = NULL;
    pthread_mutex_unlock(&g_lock);
    log_info("ai_backend", "unloaded '%s' slot=%d (clean)", name, slot);
    return NSBC_OK;
}

/* === 显式 unregister（等 refcnt→0 + 清空条目） === */
int ai_backend_registry_unregister(const char *name)
{
    return ai_backend_registry_unload_wait(name, 5000);  /* 默认 5s 超时 */
}

/* === 计数 === */
int ai_backend_registry_count(void)
{
    int n = 0;
    for (int i = 0; i < AI_BACKEND_MAX; ++i) {
        if (__atomic_load_n(&g_backends[i].ops, __ATOMIC_ACQUIRE) != NULL) ++n;
    }
    return n;
}

/* === 按 slot 查找（给 OAM show ai_backends 遍历用，不带 INC）=== */
const ai_backend_ops_t *ai_backend_registry_find_by_slot(int slot)
{
    if (slot < 0 || slot >= AI_BACKEND_MAX) return NULL;
    return __atomic_load_n(&g_backends[slot].ops, __ATOMIC_ACQUIRE);
}
