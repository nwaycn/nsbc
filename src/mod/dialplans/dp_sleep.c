/* dp_sleep.c — Dialplan 非阻塞 Sleep（§7 2026-10-15）
 *
 * 设计：
 *   - 静态全局 app_reg_t（dp_sleep_init 时创建 + app_builtin_register）
 *   - dp_sleep_add：分配 state → 拷贝 rule（防 DP reload 失效）→ tw_add
 *   - tw 回调：dp_sleep_resume_cb → 手动继续执行 app_chain 从 resume_idx
 *   - dp_sleep_cancel：链表找 call_key → tw_del → free
 *   - 单 reactor 线程操作，无锁
 */

#include "dp_sleep.h"
#include "app_ctx.h"
#include "app_registry.h"
#include "log.h"
#include "nsbc_common.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================
 * 静态全局：app_reg + 待恢复链表
 * ================================================================ */

static app_reg_t *g_app_reg = NULL;

typedef struct dp_sleep_state {
    nsbc_node_t link;
    void       *call_key;       /* b2b_call_t* — 不透明，唯一 key */
    tw_t       *tw;             /* 原 tw 指针，供 tw_del 用 */
    tw_node_t   node;           /* timer_wheel 节点 */
    int         ms;
    uint64_t    start_ms;
    size_t      resume_idx;     /* 恢复后从 app_chain[resume_idx] 继续 */
    uint32_t    rule_id;
    size_t      app_chain_n;
    dp_app_action_t *app_chain; /* malloc 拷贝（字符串指针指向 DP 内部——短 sleep 安全） */
} dp_sleep_state_t;

static dp_sleep_state_t *g_sleep_head = NULL;

/* ================================================================
 * 链表
 * ================================================================ */

static void sleep_state_push(dp_sleep_state_t *s)
{
    s->link.next = (nsbc_node_t *)g_sleep_head;
    g_sleep_head = s;
}

static void sleep_state_remove(dp_sleep_state_t *s)
{
    dp_sleep_state_t **pp = &g_sleep_head;
    while (*pp && *pp != s) pp = (dp_sleep_state_t **)&(*pp)->link.next;
    if (*pp) *pp = (dp_sleep_state_t *)s->link.next;
}

static dp_sleep_state_t *sleep_state_find(void *call_key)
{
    for (dp_sleep_state_t *s = g_sleep_head; s;
         s = (dp_sleep_state_t *)s->link.next) {
        if (s->call_key == call_key) return s;
    }
    return NULL;
}

static void sleep_state_free(dp_sleep_state_t *s)
{
    if (!s) return;
    if (s->app_chain) free(s->app_chain);
    free(s);
}

/* ================================================================
 * timer_wheel 回调 —— 从 resume_idx 继续执行 app_chain
 * ================================================================ */

static void dp_sleep_resume_cb(uint64_t now_ms, void *arg)
{
    (void)now_ms;
    dp_sleep_state_t *s = (dp_sleep_state_t *)arg;
    if (!s) return;

    llog(LOG_INFO, "dp_sleep", "resume: call_key=%p chain_n=%zu resume_idx=%zu",
         s->call_key, s->app_chain_n, s->resume_idx);

    sleep_state_remove(s);

    /* 手动继续：从 resume_idx 开始逐个 app_execute */
    if (!g_app_reg || !s->app_chain) { sleep_state_free(s); return; }

    app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.call_key = s->call_key;
    /* leg / vars 等后续扩展 */

    for (size_t i = s->resume_idx; i < s->app_chain_n; ++i) {
        const dp_app_action_t *a = &s->app_chain[i];
        app_rc_t rc = APP_RC_OK;
        int rc2 = app_execute(g_app_reg, a->app, &ctx, a->data, &rc);
        if (rc2 != NSBC_OK) {
            llog(LOG_WARN, "dp_sleep", "resume[%zu] unknown app '%s'", i, a->app);
            break;
        }
        if (rc == APP_RC_PENDING) {
            /* 又一个非阻塞 app → 复制当前 state 重新挂 timer */
            dp_sleep_state_t *next = (dp_sleep_state_t *)calloc(1, sizeof(*next));
            if (!next) { sleep_state_free(s); return; }
            *next = *s;                       /* 浅拷贝（含 app_chain 指针所有权转移） */
            s->app_chain = NULL;               /* 防止 sleep_state_free 时 free 两次 */
            next->node = (tw_node_t){0};
            next->resume_idx = i + 1;
            tw_node_init(&next->node);
            int rc3 = tw_add(next->tw, &next->node,
                             nsbc_now_ms() + (uint64_t)next->ms,
                             dp_sleep_resume_cb, next);
            if (rc3 != NSBC_OK) { sleep_state_free(next); }
            else { sleep_state_push(next); }
            sleep_state_free(s);
            return;
        }
        if (rc == APP_RC_FAILED || rc == APP_RC_HANGUP) break;
    }
    sleep_state_free(s);
}

/* ================================================================
 * 公开 API
 * ================================================================ */

int dp_sleep_add(tw_t *tw, void *call_key, const dp_rule_t *rule,
                 size_t resume_idx, int ms)
{
    if (!tw || !call_key || !rule || ms <= 0) return NSBC_INVAL;
    if (ms > 600000) ms = 600000;  /* 封顶 10min */

    dp_sleep_state_t *prev = sleep_state_find(call_key);
    if (prev) dp_sleep_cancel(call_key);

    dp_sleep_state_t *s = (dp_sleep_state_t *)calloc(1, sizeof(*s));
    if (!s) return NSBC_NOMEM;

    s->call_key = call_key;
    s->tw = tw;
    s->ms = ms;
    s->start_ms = nsbc_now_ms();
    s->resume_idx = resume_idx;
    s->rule_id = rule->id;
    s->app_chain_n = rule->app_chain_n;

    if (rule->app_chain_n > 0 && rule->app_chain) {
        s->app_chain = (dp_app_action_t *)malloc(rule->app_chain_n * sizeof(dp_app_action_t));
        if (!s->app_chain) { free(s); return NSBC_NOMEM; }
        memcpy(s->app_chain, rule->app_chain,
               rule->app_chain_n * sizeof(dp_app_action_t));
        /* 注意：app_chain[i].app/.data 是 rule 里的字符串指针（const char*）。
         * DP reload 会换表——但 sleep 时间短（<60s），reload 不频繁，P0 安全。
         * P1 需 deep-copy 字符串。 */
    }

    tw_node_init(&s->node);
    uint64_t expire_ms = nsbc_now_ms() + (uint64_t)ms;
    int rc = tw_add(tw, &s->node, expire_ms, dp_sleep_resume_cb, s);
    if (rc != NSBC_OK) { sleep_state_free(s); return rc; }

    sleep_state_push(s);
    llog(LOG_INFO, "dp_sleep", "armed: call_key=%p ms=%d resume_idx=%zu",
         call_key, ms, resume_idx);
    return NSBC_OK;
}

int dp_sleep_cancel(void *call_key)
{
    dp_sleep_state_t *s = sleep_state_find(call_key);
    if (!s) return 0;
    sleep_state_remove(s);
    tw_del(s->tw, &s->node);
    llog(LOG_INFO, "dp_sleep", "cancelled: call_key=%p (sl=%dms elapsed=%llums)",
         call_key, s->ms,
         (unsigned long long)(nsbc_now_ms() - s->start_ms));
    sleep_state_free(s);
    return 1;
}

int dp_sleep_active(void *call_key)
{
    return sleep_state_find(call_key) ? 1 : 0;
}

/* ================================================================
 * init / fini
 * ================================================================ */

void dp_sleep_init(void)
{
    if (g_app_reg) return;
    int rc = app_reg_create(&g_app_reg);
    if (rc != NSBC_OK) {
        llog(LOG_ERR, "dp_sleep", "app_reg_create rc=%d", rc);
        return;
    }
    app_builtin_register(g_app_reg);
    llog(LOG_NOTICE, "dp_sleep", "init: app_reg ready (apps=%d)",
         app_count(g_app_reg));

    /* §8: 绑定 ctx 槽位让 oam 层（call_commands / uuid_break）能调
     *      dp_sleep_cancel / dp_sleep_active 而不反向 include mod 层头。
     *      pb_stop_if_playing 在 media 模块初始化时绑定。 */
    nsbc_ctx_t *ctx = nsbc_get_ctx();
    if (ctx) {
        ctx->dp_sleep_cancel = dp_sleep_cancel;
        ctx->dp_sleep_active = dp_sleep_active;
    }
}

void dp_sleep_fini(void)
{
    dp_sleep_state_t *s = g_sleep_head;
    while (s) {
        dp_sleep_state_t *next = (dp_sleep_state_t *)s->link.next;
        tw_del(s->tw, &s->node);
        sleep_state_free(s);
        s = next;
    }
    g_sleep_head = NULL;
    if (g_app_reg) { app_reg_destroy(g_app_reg); g_app_reg = NULL; }
}

app_reg_t *dp_sleep_app_reg(void) { return g_app_reg; }
