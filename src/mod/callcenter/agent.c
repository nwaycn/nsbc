/* 座席池——粗骨架（REQ-PBX-CC-001，P2）
 *
 * 迁移合法性（粗骨架固定，P3 可配）：
 *   签入  OFFLINE/PAUSED → IDLE（pause 时 resume 同理回 IDLE）
 *   签退  任意非 ONCALL/RESERVED → OFFLINE
 *   选中  IDLE → RESERVED
 *   应答  RESERVED → ONCALL（计 answered_total）
 *   挂机  ONCALL → WRAP（wrap_time_s>0）否则直接 IDLE
 *   整理到 由 agent_release 在时间轮回调再切 IDLE——本批 release 直接
 *   完成 WRAP→IDLE 落位（定时器在接线批挂）。
 *
 * 并发模型（P1 修复）：
 *   agent_pool_t 内部加 pthread_mutex_t pool_lock，保护 agents[] 数组
 *   的并发读写（find → 状态变更 → n++ 不是原子）。
 *   单 agent 粒度的 agent_lock 暂不加（本模块无此成员，任务说"保留它"
 *   — 这里的保留指不引入，pool_lock 是新增）。
 */
#include "agent.h"
#include <pthread.h>

struct agent_pool {
    agent_t agents[AGENT_CAP];
    int n;
    pthread_mutex_t pool_lock;  /* P1 并发保护：所有公开入口持锁 */
};

int agent_pool_create(agent_pool_t **out)
{
    if (out == NULL)
        return NSBC_INVAL;
    *out = (agent_pool_t *)calloc(1, sizeof(agent_pool_t));
    if (*out == NULL)
        return NSBC_NOMEM;
    /* P1: 初始化池锁 */
    pthread_mutex_init(&(*out)->pool_lock, NULL);
    return NSBC_OK;
}

void agent_pool_destroy(agent_pool_t *p)
{
    if (p == NULL)
        return;
    pthread_mutex_destroy(&p->pool_lock);
    free(p);
}

static agent_t *find(agent_pool_t *p, const char *tenant, const char *ext)
{
    int i;
    for (i = 0; i < p->n; ++i) {
        if (strcmp(p->agents[i].tenant, tenant) == 0 &&
            strcmp(p->agents[i].ext, ext) == 0)
            return &p->agents[i];
    }
    return NULL;
}

int agent_upsert(agent_pool_t *p, const agent_t *a)
{
    agent_t *e;
    if (p == NULL || a == NULL || a->ext[0] == '\0')
        return NSBC_INVAL;
    pthread_mutex_lock(&p->pool_lock);  /* P1: 池锁 */
    e = find(p, a->tenant, a->ext);
    if (e != NULL) {
        agent_state_t st = e->state; /* 配置更新不改登录态 */
        uint64_t since = e->state_since_ms;
        *e = *a;
        e->state = st;
        e->state_since_ms = since;
        pthread_mutex_unlock(&p->pool_lock);
        return NSBC_OK;
    }
    if (p->n >= AGENT_CAP) {
        pthread_mutex_unlock(&p->pool_lock);
        return NSBC_NOSPC;
    }
    p->agents[p->n] = *a;
    /* calloc 归零：state 缺省即 AGENT_OFFLINE(=0)，无需重复赋值 */
    ++p->n;
    pthread_mutex_unlock(&p->pool_lock);
    return NSBC_OK;
}

int agent_lookup(agent_pool_t *p, const char *tenant, const char *ext,
                 agent_t *out)
{
    const agent_t *a;
    if (p == NULL || tenant == NULL || ext == NULL || out == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&p->pool_lock);  /* P1: 池锁 */
    a = find(p, tenant, ext);
    if (a == NULL) {
        pthread_mutex_unlock(&p->pool_lock);
        return NSBC_NOENT;
    }
    *out = *a;
    pthread_mutex_unlock(&p->pool_lock);
    return NSBC_OK;
}

static int to_idle(agent_t *a, uint64_t now_ms)
{
    if (a->state != AGENT_OFFLINE && a->state != AGENT_PAUSED &&
        a->state != AGENT_WRAP)
        return NSBC_INVAL;
    a->state = AGENT_IDLE;
    a->pause_reason = AGENT_PAUSE_NONE;
    a->state_since_ms = now_ms;
    return NSBC_OK;
}

int agent_login(agent_pool_t *p, const char *tenant, const char *ext,
                uint64_t now_ms)
{
    agent_t *a;
    if (p == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&p->pool_lock);  /* P1: 池锁 */
    a = find(p, tenant, ext);
    if (a == NULL) { pthread_mutex_unlock(&p->pool_lock); return NSBC_NOENT; }
    int rc = to_idle(a, now_ms);
    pthread_mutex_unlock(&p->pool_lock);
    return rc;
}

int agent_logout(agent_pool_t *p, const char *tenant, const char *ext)
{
    agent_t *a;
    if (p == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&p->pool_lock);  /* P1: 池锁 */
    a = find(p, tenant, ext);
    if (a == NULL) { pthread_mutex_unlock(&p->pool_lock); return NSBC_NOENT; }
    if (a->state == AGENT_ONCALL || a->state == AGENT_RESERVED) {
        pthread_mutex_unlock(&p->pool_lock);
        return NSBC_AGAIN;
    }
    a->state = AGENT_OFFLINE;
    a->pause_reason = AGENT_PAUSE_NONE;
    a->state_since_ms = 0;
    pthread_mutex_unlock(&p->pool_lock);
    return NSBC_OK;
}

int agent_pause(agent_pool_t *p, const char *tenant, const char *ext,
                agent_pause_reason_t reason)
{
    agent_t *a;
    if (p == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&p->pool_lock);  /* P1: 池锁 */
    a = find(p, tenant, ext);
    if (a == NULL) { pthread_mutex_unlock(&p->pool_lock); return NSBC_NOENT; }
    if (a->state == AGENT_ONCALL || a->state == AGENT_RESERVED) {
        pthread_mutex_unlock(&p->pool_lock);
        return NSBC_AGAIN;
    }
    a->state = AGENT_PAUSED;
    a->pause_reason = reason;
    pthread_mutex_unlock(&p->pool_lock);
    return NSBC_OK;
}

int agent_resume(agent_pool_t *p, const char *tenant, const char *ext,
                 uint64_t now_ms)
{
    agent_t *a;
    if (p == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&p->pool_lock);  /* P1: 池锁 */
    a = find(p, tenant, ext);
    if (a == NULL) { pthread_mutex_unlock(&p->pool_lock); return NSBC_NOENT; }
    int rc = to_idle(a, now_ms);
    pthread_mutex_unlock(&p->pool_lock);
    return rc;
}

int agent_reserve(agent_pool_t *p, const char *tenant, const char *ext,
                  uint64_t now_ms)
{
    agent_t *a;
    if (p == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&p->pool_lock);  /* P1: 池锁 */
    a = find(p, tenant, ext);
    if (a == NULL) { pthread_mutex_unlock(&p->pool_lock); return NSBC_NOENT; }
    if (a->state != AGENT_IDLE) {
        pthread_mutex_unlock(&p->pool_lock);
        return NSBC_AGAIN;
    }
    a->state = AGENT_RESERVED;
    a->state_since_ms = now_ms;
    pthread_mutex_unlock(&p->pool_lock);
    return NSBC_OK;
}

int agent_answer(agent_pool_t *p, const char *tenant, const char *ext,
                 uint64_t now_ms)
{
    agent_t *a;
    if (p == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&p->pool_lock);  /* P1: 池锁 */
    a = find(p, tenant, ext);
    if (a == NULL) { pthread_mutex_unlock(&p->pool_lock); return NSBC_NOENT; }
    if (a->state != AGENT_RESERVED) {
        pthread_mutex_unlock(&p->pool_lock);
        return NSBC_INVAL;
    }
    a->state = AGENT_ONCALL;
    a->state_since_ms = now_ms;
    ++a->answered_total;
    pthread_mutex_unlock(&p->pool_lock);
    return NSBC_OK;
}

int agent_release(agent_pool_t *p, const char *tenant, const char *ext,
                  uint64_t now_ms, int wrap_time_s)
{
    agent_t *a;
    if (p == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&p->pool_lock);  /* P1: 池锁 */
    a = find(p, tenant, ext);
    if (a == NULL) { pthread_mutex_unlock(&p->pool_lock); return NSBC_NOENT; }
    if (a->state != AGENT_ONCALL) {
        pthread_mutex_unlock(&p->pool_lock);
        return NSBC_INVAL;
    }
    a->last_call_end_ms = now_ms;
    if (wrap_time_s > 0) {
        a->state = AGENT_WRAP;
        /* TODO：时间轮 wrap_time_s 后自动 to_idle（队列定时器装配） */
    } else {
        a->state = AGENT_IDLE;
        a->state_since_ms = now_ms;
    }
    pthread_mutex_unlock(&p->pool_lock);
    return NSBC_OK;
}

int agent_has_skill(const agent_t *a, const char *skill)
{
    int i;
    if (a == NULL || skill == NULL)
        return 0;
    for (i = 0; i < a->n_skills; ++i) {
        if (strcmp(a->skills[i], skill) == 0)
            return 1;
    }
    return 0;
}

const char *agent_state_name(agent_state_t st)
{
    switch (st) {
    case AGENT_OFFLINE:  return "offline";
    case AGENT_IDLE:     return "idle";
    case AGENT_RESERVED: return "reserved";
    case AGENT_ONCALL:   return "oncall";
    case AGENT_WRAP:     return "wrap";
    case AGENT_PAUSED:   return "paused";
    default:             return "unknown";
    }
}

/* ==================================================================
 * 模块注册（REQ-ARCH-011）：agent_pool 是 queue 模块的前置依赖
 * MOD_L_NET_READY（与 endpoints 同层）——queue 在 WORKER_READY，比它晚一层
 * ================================================================== */
#include "module.h"
#include "app_ctx.h"
#include "log.h"

static int agent_pool_mod_init(struct nsbc_ctx *ctx)
{
    if (!ctx) return NSBC_INVAL;
    if (ctx->agent_pool) {
        llog(LOG_WARN, "agent_pool", "already initialized, skip");
        return NSBC_OK;
    }
    int rc = agent_pool_create((agent_pool_t **)&ctx->agent_pool);
    if (rc != NSBC_OK) {
        llog(LOG_CRIT, "agent_pool", "agent_pool_create failed rc=%d", rc);
        return rc;
    }
    llog(LOG_INFO, "agent_pool", "agent_pool initialized (cap=%d)", AGENT_CAP);
    return NSBC_OK;
}

static void agent_pool_mod_fini(struct nsbc_ctx *ctx)
{
    if (ctx && ctx->agent_pool) {
        agent_pool_destroy((agent_pool_t *)ctx->agent_pool);
        ctx->agent_pool = NULL;
        llog(LOG_INFO, "agent_pool", "agent_pool destroyed");
    }
}

NSBC_MOD0_CAT(agent_pool, 1, MOD_L_NET_READY, MOD_F_REQUIRED, MOD_CAT_CALL_CENTER,
          agent_pool_mod_init, agent_pool_mod_fini);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
