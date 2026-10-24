/* ==========================================================================
 * hello 模块 —— 演示性心跳服务（用于教学，非生产模块）
 * ==========================================================================
 *
 * 功能概述：
 *   启动后每秒打印一条 LOG_NOTICE 心跳日志（计数器递增），同时通过
 *   obs_event 上报 HEARTBEAT 事件。stop 停止心跳线程，fini 清理。
 *
 *   设计目的：
 *     - 演示 NSBC_MOD 宏（非 NSBC_MOD0，含 start/stop）
 *     - 演示从 ctx->cfg 读取 YAML 配置
 *     - 演示 obs_event 事件上报（OAM 事件总线）
 *     - 演示 pthread 启停、ctx 透传
 *     - 作为 examples/04_walkthrough_hello.md 讲解文档的真实代码对照
 *
 * 配置项（configs/nsbc.yaml）：
 *   hello.enabled:        true/false  （默认 true）
 *   hello.interval_ms:    毫秒        （默认 1000）
 *
 * 不被任何生产逻辑依赖；MOD_L_FINAL 层。
 * ==========================================================================
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "app_ctx.h"
#include "log.h"
#include "module.h"
#include "obs.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>


/* ---- 模块私有状态 ---- */
typedef struct {
    pthread_t       thr;        /* 心跳线程 */
    pthread_mutex_t mtx;        /* 保护 stop/cnt */
    pthread_cond_t  cv;         /* 用于可中断 sleep */
    int             stop;       /* 1 = 请求退出 */
    int64_t         cnt;        /* 心跳次数 */
    long            interval_ms; /* 心跳间隔（从配置读） */
    int             running;    /* 1 = 线程已在运行 */
} hello_state_t;

static hello_state_t *g_hello;


/* ---- 工具：可中断 sleep ---- */
static void hello_sleep_ms(hello_state_t *s, long ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&s->mtx);
    /* pthread_cond_timedwait 会在被 stop 置位时提前返回 */
    pthread_cond_timedwait(&s->cv, &s->mtx, &ts);
    pthread_mutex_unlock(&s->mtx);
}


/* ---- 心跳线程主循环 ---- */
static void *hello_thread(void *arg)
{
    hello_state_t *s = (hello_state_t *)arg;
    llog(LOG_INFO, "hello", "heartbeat thread started, interval=%ldms",
         s->interval_ms);

    while (1) {
        pthread_mutex_lock(&s->mtx);
        if (s->stop) { pthread_mutex_unlock(&s->mtx); break; }
        pthread_mutex_unlock(&s->mtx);

        hello_sleep_ms(s, s->interval_ms);

        pthread_mutex_lock(&s->mtx);
        if (s->stop) { pthread_mutex_unlock(&s->mtx); break; }
        s->cnt++;
        int64_t c = s->cnt;
        pthread_mutex_unlock(&s->mtx);

        llog(LOG_NOTICE, "hello", "heartbeat #%lld", (long long)c);
        obs_event(6, "HELLO_HEARTBEAT", "count=%lld", (long long)c);
    }

    llog(LOG_INFO, "hello", "heartbeat thread exit, total=%lld",
         (long long)s->cnt);
    return NULL;
}


/* ==========================================================================
 * init —— 在 MOD_L_FINAL 层被调用一次
 * ==========================================================================
 * 职责：malloc 内部状态、从 YAML 读配置、向 obs 报告事件。
 * 此时 reactor / worker / socket 都已经就绪，但本模块不依赖它们。
 */
static int hello_init(struct nsbc_ctx *ctx)
{
    hello_state_t *s = calloc(1, sizeof(*s));
    if (!s) {
        llog(LOG_ERR, "hello", "init: OOM");
        return NSBC_NOMEM;
    }

    /* 从配置读间隔（缺省 1000ms）。ctx 和 ctx->cfg 在 init 阶段已经有效 */
    long interval = 1000;
    if (ctx && ctx->cfg) {
        cfg_get_int(ctx->cfg, "hello.interval_ms", &interval, NULL);
        if (interval < 10) {
            llog(LOG_WARN, "hello", "interval_ms=%ld too small, clamp to 10",
                 interval);
            interval = 10;
        }
    }
    s->interval_ms = interval;
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cv, NULL);

    g_hello = s;

    llog(LOG_INFO, "hello", "init ok (interval=%ldms)", s->interval_ms);
    obs_event(6, "HELLO_INIT", "interval_ms=%ld", s->interval_ms);
    return NSBC_OK;
}


/* ==========================================================================
 * start —— FINAL 层模块 init 全部完成后，按拓扑序调用
 * ==========================================================================
 * 职责：启动心跳线程。演示"开始产生副作用"的时机。
 * 如果配置里 hello.enabled=false，这里直接跳过线程启动。
 */
static int hello_start(struct nsbc_ctx *ctx)
{
    if (!g_hello) return NSBC_ERR;

    int enabled = 1;
    if (ctx && ctx->cfg) {
        cfg_get_bool(ctx->cfg, "hello.enabled", &enabled, NULL);
    }
    if (!enabled) {
        llog(LOG_WARN, "hello", "disabled by config hello.enabled=false, skip start");
        return NSBC_OK; /* 返回 OK：不是 REQUIRED 模块，不启动也不致命 */
    }

    int rc = pthread_create(&g_hello->thr, NULL, hello_thread, g_hello);
    if (rc != 0) {
        llog(LOG_ERR, "hello", "pthread_create failed rc=%d", rc);
        return NSBC_ERR;
    }
    g_hello->running = 1;

    obs_event(6, "HELLO_START", "interval_ms=%ld", g_hello->interval_ms);
    return NSBC_OK;
}


/* ==========================================================================
 * stop —— SIGTERM 时被调用（反序，先于其他模块 stop 的模块后停）
 * ==========================================================================
 * 职责：摘流量。这里置 stop=1 并 signal 唤醒 cond，让线程自行退出。
 * 不做 free——fini 会处理。呼叫面模块必须在 stop 返回后能被 drain。
 */
static void hello_stop(struct nsbc_ctx *ctx)
{
    (void)ctx;
    if (!g_hello || !g_hello->running) return;

    llog(LOG_NOTICE, "hello", "stop requested");
    pthread_mutex_lock(&g_hello->mtx);
    g_hello->stop = 1;
    pthread_cond_signal(&g_hello->cv);
    pthread_mutex_unlock(&g_hello->mtx);

    /* 等线程退出（fini 不会再 join，这里在 stop 里 join 更干净） */
    pthread_join(g_hello->thr, NULL);
    g_hello->running = 0;
    obs_event(6, "HELLO_STOP", "total_heartbeats=%lld", (long long)g_hello->cnt);
}


/* ==========================================================================
 * fini —— stop 之后反序调用，释放所有资源
 * ==========================================================================
 */
static void hello_fini(struct nsbc_ctx *ctx)
{
    (void)ctx;
    if (!g_hello) return;

    pthread_mutex_destroy(&g_hello->mtx);
    pthread_cond_destroy(&g_hello->cv);

    llog(LOG_INFO, "hello", "fini ok (total heartbeats=%lld)",
         (long long)g_hello->cnt);
    free(g_hello);
    g_hello = NULL;
}


/* ==========================================================================
 * 模块描述符
 * ==========================================================================
 *
 * NSBC_MOD( name, version, level, flags, deps_array,
 *           init, start, stop, fini )
 *
 * 本模块说明：
 *   - name = "hello"  → 宏生成 extern const nsbc_module_t nsbcm_hello;
 *   - level = MOD_L_FINAL → 所有基础框架都 ready 后才 init
 *   - flags = 0 → 既不是 CALL_PATH（心跳线程可以随时停），也不是 REQUIRED
 *   - deps = NULL → 不依赖其他模块
 *   - init→start→stop→fini 四个回调全实现（对比 NSBC_MOD0 是只有 init+fini）
 * ==========================================================================
 */
NSBC_MOD_CAT(hello,                              /* name */
         1,                                  /* version */
         MOD_L_FINAL,                        /* level */
         0,                                  /* flags */
         MOD_CAT_UTILITY,                    /* category */
         (const char *[]){ NULL },           /* deps：无依赖，哨兵数组 */
         hello_init, hello_start, hello_stop, hello_fini);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
