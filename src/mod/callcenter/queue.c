/* ACD 队列——粗骨架（REQ-PBX-CC-002~005，P2）
 *
 * 装配：core 在话务事件（座席空闲/新入队/挂机率）上调 cq_dispatch；
 * 选中座席经 b2b 振铃，接起 cq_answered + agent_answer；挂机 cq_finish
 * → agent_release（WRAP）。等待音/MoH 走 player；溢出目标经 dialplan
 * 重新进入路由（IVR/VM/另一队列）。
 * 粗骨架：队列/等待者定槽数组线性扫描，锁与持久化在接线批补。
 */
#include "queue.h"
#include "log.h"
#include <cjson/cJSON.h>   /* cJSON_Parse + cJSON_GetObjectItem + cJSON_ArrayForEach */

/* 共享 stub_mods.c 同逻辑：json truthy helper（static inline，避免跨文件 linkage） */
static int json_truthy(const cJSON *j, int def)
{
    if (!j) return def;
    if (cJSON_IsBool(j)) return j->valueint;
    if (cJSON_IsNumber(j)) return j->valueint != 0;
    if (cJSON_IsString(j)) return (j->valuestring && j->valuestring[0] &&
                                   strcmp(j->valuestring, "false") != 0 &&
                                   strcmp(j->valuestring, "0") != 0);
    return def;
}

typedef struct {
    cq_queue_t  q;
    cq_waiter_t wait[CQ_WAIT_MAX];
    int         n_wait;
    cq_stats_t  st;
} cq_slot_t;

struct cc_mgr {
    agent_pool_t *pool;
    cq_slot_t     slots[CQ_QUEUE_CAP];
    int           n;
};

int cq_create(agent_pool_t *pool, cc_mgr_t **out)
{
    if (out == NULL)
        return NSBC_INVAL;
    *out = (cc_mgr_t *)calloc(1, sizeof(cc_mgr_t));
    if (*out == NULL)
        return NSBC_NOMEM;
    (*out)->pool = pool;
    return NSBC_OK;
}

void cq_destroy(cc_mgr_t *m)
{
    free(m);
}

static cq_slot_t *slot_find(cc_mgr_t *m, const char *tenant, const char *name)
{
    int i;
    /* tenant 为空串时视为 wildcard：跳过租户匹配，按 name 匹配第一个。
     * 供 signaling.c Phase 2 接入 ctx->current_tenant 之前的过渡使用。 */
    int wildcard = (tenant == NULL || tenant[0] == '\0');
    for (i = 0; i < m->n; ++i) {
        if (!wildcard && strcmp(m->slots[i].q.tenant, tenant) != 0)
            continue;
        if (strcmp(m->slots[i].q.name, name) == 0)
            return &m->slots[i];
    }
    return NULL;
}

int cq_add(cc_mgr_t *m, const cq_queue_t *q)
{
    cq_slot_t *s;
    if (m == NULL || q == NULL || q->name[0] == '\0' ||
        q->n_members <= 0 || q->n_members > CQ_MEMB_MAX)
        return NSBC_INVAL;
    s = slot_find(m, q->tenant, q->name);
    if (s != NULL) {
        s->q = *q;
        return NSBC_OK;
    }
    if (m->n >= CQ_QUEUE_CAP)
        return NSBC_NOSPC;
    m->slots[m->n].q = *q;
    ++m->n;
    return NSBC_OK;
}

int cq_remove(cc_mgr_t *m, const char *tenant, const char *name)
{
    int i;
    for (i = 0; i < m->n; ++i) {
        if (strcmp(m->slots[i].q.tenant, tenant) == 0 &&
            strcmp(m->slots[i].q.name, name) == 0) {
            if (m->slots[i].n_wait > 0)
                return NSBC_AGAIN;
            m->slots[i] = m->slots[--m->n];
            return NSBC_OK;
        }
    }
    return NSBC_NOENT;
}

const cq_queue_t *cq_get(cc_mgr_t *m, const char *tenant, const char *name)
{
    cq_slot_t *s = slot_find(m, tenant, name);
    return s == NULL ? NULL : &s->q;
}

static cq_waiter_t *head_waiter(cq_slot_t *s)
{
    int best = -1;
    int i;
    /* 优先级高者先；同优先级先入队（粗骨架线性，FIFO 稳定） */
    for (i = 0; i < s->n_wait; ++i) {
        if (best < 0 ||
            s->wait[i].priority > s->wait[best].priority ||
            (s->wait[i].priority == s->wait[best].priority &&
             s->wait[i].enqueue_ms < s->wait[best].enqueue_ms))
            best = i;
    }
    return best < 0 ? NULL : &s->wait[best];
}

int cq_enqueue(cc_mgr_t *m, const char *tenant, const char *name,
               cq_waiter_t w)
{
    cq_slot_t *s;
    if (m == NULL || w.call_key == NULL)
        return NSBC_INVAL;
    s = slot_find(m, tenant, name);
    if (s == NULL || !s->q.enabled)
        return s == NULL ? NSBC_NOENT : NSBC_PERM_DENIED;
    if (s->n_wait >= CQ_WAIT_MAX)
        return NSBC_NOSPC;
    s->wait[s->n_wait++] = w;
    ++s->st.total_enqueued;
    return NSBC_OK;
}

int cq_abandon(cc_mgr_t *m, void *call_key)
{
    int si, wi;
    if (m == NULL || call_key == NULL)
        return NSBC_INVAL;
    for (si = 0; si < m->n; ++si) {
        cq_slot_t *s = &m->slots[si];
        for (wi = 0; wi < s->n_wait; ++wi) {
            if (s->wait[wi].call_key == call_key) {
                s->wait[wi] = s->wait[--s->n_wait];
                ++s->st.total_abandoned;
                return NSBC_OK;
            }
        }
    }
    return NSBC_NOENT;
}

int cq_waiting_count(cc_mgr_t *m, const char *tenant, const char *name)
{
    cq_slot_t *s = slot_find(m, tenant, name);
    return s == NULL ? 0 : s->n_wait;
}

/* 收集技能匹配且 IDLE 的成员（粗骨架直接读 agent 快照，线程化后
 * 座席态写读走原子槽或池锁） */
static int collect_idle(cc_mgr_t *m, const cq_slot_t *s, agent_t *out, int cap)
{
    int i, n = 0;
    for (i = 0; i < s->q.n_members && n < cap; ++i) {
        agent_t a;
        if (agent_lookup(m->pool, s->q.tenant, s->q.members[i], &a) != NSBC_OK)
            continue;
        if (a.state != AGENT_IDLE)
            continue;
        if (s->q.require_skill[0] != '\0' &&
            !agent_has_skill(&a, s->q.require_skill))
            continue;
        out[n++] = a;
    }
    return n;
}

static void pick_ext(const agent_t *a, char out[][AGENT_EXT_MAX], int idx)
{
    snprintf(out[idx], AGENT_EXT_MAX, "%s", a->ext);
}

int cq_dispatch(cc_mgr_t *m, const char *tenant, const char *name,
                uint64_t now_ms, uint32_t rnd,
                char out_ext[][AGENT_EXT_MAX], int out_cap,
                void **out_call_key)
{
    cq_slot_t *s;
    cq_waiter_t *w;
    agent_t idle[CQ_MEMB_MAX];
    int n;
    int pick = 0;
    int i;
    if (m == NULL || out_ext == NULL || out_cap <= 0 || out_call_key == NULL)
        return NSBC_INVAL;
    (void)now_ms;
    s = slot_find(m, tenant, name);
    if (s == NULL)
        return NSBC_NOENT;
    w = head_waiter(s);
    if (w == NULL)
        return 0;
    n = collect_idle(m, s, idle, CQ_MEMB_MAX);
    if (n == 0)
        return 0;
    switch (s->q.strategy) {
    case CQ_STRATEGY_LEAST_RECENT:
        for (i = 1; i < n; ++i) {
            if (idle[i].last_call_end_ms < idle[pick].last_call_end_ms)
                pick = i;
        }
        break;
    case CQ_STRATEGY_FEWEST_CALLS:
        for (i = 1; i < n; ++i) {
            if (idle[i].answered_total < idle[pick].answered_total)
                pick = i;
        }
        break;
    case CQ_STRATEGY_ROUND_ROBIN:
        pick = (int)(rnd % (uint32_t)n); /* TODO：持久 rr_cursor */
        break;
    case CQ_STRATEGY_RING_ALL:
        break;
    }
    if (s->q.strategy == CQ_STRATEGY_RING_ALL) {
        int lim = n < out_cap ? n : out_cap;
        for (i = 0; i < lim; ++i) {
            pick_ext(&idle[i], out_ext, i);
            (void)agent_reserve(m->pool, tenant, idle[i].ext, now_ms);
        }
        *out_call_key = w->call_key;
        return lim;
    }
    pick_ext(&idle[pick], out_ext, 0);
    if (agent_reserve(m->pool, tenant, idle[pick].ext, now_ms) != NSBC_OK)
        return 0;
    *out_call_key = w->call_key;
    return 1;
}

static int waiter_remove(cq_slot_t *s, const void *call_key)
{
    int i;
    for (i = 0; i < s->n_wait; ++i) {
        if (s->wait[i].call_key == call_key) {
            s->wait[i] = s->wait[--s->n_wait];
            return 1;
        }
    }
    return 0;
}

int cq_answered(cc_mgr_t *m, const char *tenant, const char *name,
                const char *ext, uint64_t answered_ms, int sla_s)
{
    cq_slot_t *s;
    cq_waiter_t *w;
    uint64_t waited;
    if (m == NULL)
        return NSBC_INVAL;
    s = slot_find(m, tenant, name);
    if (s == NULL)
        return NSBC_NOENT;
    if (agent_answer(m->pool, tenant, ext, answered_ms) != NSBC_OK)
        return NSBC_PROTO;
    w = head_waiter(s);
    if (w == NULL)
        return NSBC_NOENT;
    waited = answered_ms - w->enqueue_ms;
    w->connect_ms = answered_ms;
    ++s->st.total_answered;
    if (waited > s->st.max_wait_ms)
        s->st.max_wait_ms = waited;
    if (sla_s > 0 && waited <= (uint64_t)sla_s * 1000u)
        ++s->st.answered_within_sla;
    (void)waiter_remove(s, w->call_key);
    return NSBC_OK;
}

int cq_finish(cc_mgr_t *m, const char *tenant, const char *name,
              const char *ext, uint64_t now_ms)
{
    const cq_slot_t *s = slot_find(m, tenant, name);
    if (s == NULL)
        return NSBC_NOENT;
    return agent_release(m->pool, tenant, ext, now_ms, s->q.wrap_time_s);
}

int cq_tick_overflow(cc_mgr_t *m, const char *tenant, const char *name,
                     uint64_t now_ms)
{
    cq_slot_t *s;
    int i, moved = 0;
    if (m == NULL)
        return NSBC_INVAL;
    s = slot_find(m, tenant, name);
    if (s == NULL || s->q.max_wait_s <= 0)
        return s == NULL ? NSBC_NOENT : 0;
    /* 倒序压缩：超时者交溢出（目标路由由组合根执行，这里只移除） */
    for (i = s->n_wait - 1; i >= 0; --i) {
        if (now_ms - s->wait[i].enqueue_ms >=
            (uint64_t)s->q.max_wait_s * 1000u) {
            s->wait[i] = s->wait[--s->n_wait];
            ++s->st.total_overflowed;
            ++moved;
        }
    }
    return moved;
}

void cq_stats(cc_mgr_t *m, const char *tenant, const char *name,
              cq_stats_t *out)
{
    const cq_slot_t *s;
    if (m == NULL || out == NULL)
        return;
    s = slot_find(m, tenant, name);
    if (s == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s->st;
}

/* ==================================================================
 * 模块注册（REQ-ARCH-011）：queue 依赖 agent_pool（ctx->agent_pool 先在 NET_READY 层初始化）
 * 所以 queue 放 MOD_L_WORKER_READY（比 agent_pool 晚一层）
 * ================================================================== */
#include "module.h"
#include "app_ctx.h"

static int queue_mod_init(struct nsbc_ctx *ctx)
{
    if (!ctx) return NSBC_INVAL;
    if (ctx->cq_mgr) {
        llog(LOG_WARN, "queue", "already initialized, skip");
        return NSBC_OK;
    }
    agent_pool_t *pool = (agent_pool_t *)ctx->agent_pool;
    if (!pool) {
        llog(LOG_CRIT, "queue", "agent_pool not ready — queue module requires agent_pool");
        return NSBC_ERR;
    }
    int rc = cq_create(pool, (cc_mgr_t **)&ctx->cq_mgr);
    if (rc != NSBC_OK) {
        llog(LOG_CRIT, "queue", "cq_create failed rc=%d", rc);
        return rc;
    }
    llog(LOG_INFO, "queue", "cc_mgr initialized (cap=%d)", CQ_QUEUE_CAP);
    return NSBC_OK;
}

static void queue_mod_fini(struct nsbc_ctx *ctx)
{
    if (ctx && ctx->cq_mgr) {
        cq_destroy((cc_mgr_t *)ctx->cq_mgr);
        ctx->cq_mgr = NULL;
        llog(LOG_INFO, "queue", "cc_mgr destroyed");
    }
}

/* Web DB → C 侧整表 reload（与 ext/trunk/dp 同模式）
 * JSON schema：[{"tenant":"nsbc.local","name":"sales","members":["8001","8002"],
 *                "enabled":true,"strategy":"ringall","max_wait_s":600,
 *                "wrap_time_s":30,"require_skill":""}, ...] */
int queue_table_reload_json(const char *json, char *out_err, size_t err_sz)
{
    if (!json || !*json) {
        if (out_err) snprintf(out_err, err_sz, "empty json");
        return NSBC_INVAL;
    }
    nsbc_ctx_t *ctx = nsbc_get_ctx();
    if (!ctx || !ctx->cq_mgr) {
        if (out_err) snprintf(out_err, err_sz, "cc_mgr not ready");
        return NSBC_ERR;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        if (out_err) snprintf(out_err, err_sz, "json parse error");
        return NSBC_ERR;
    }
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        if (out_err) snprintf(out_err, err_sz, "root not array");
        return NSBC_INVAL;
    }
    cc_mgr_t *m = (cc_mgr_t *)ctx->cq_mgr;
    /* 两阶段：先清旧槽（直接置 n=0，caller 确保没 waiter），再逐个 cq_add */
    m->n = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        cJSON *j;
        cq_queue_t q;
        memset(&q, 0, sizeof(q));
        j = cJSON_GetObjectItem(item, "tenant");
        if (j && cJSON_IsString(j) && j->valuestring[0])
            snprintf(q.tenant, sizeof(q.tenant), "%s", j->valuestring);
        else
            snprintf(q.tenant, sizeof(q.tenant), "nsbc.local");
        j = cJSON_GetObjectItem(item, "name");
        if (!j || !cJSON_IsString(j) || !j->valuestring[0]) {
            snprintf(out_err, err_sz, "item[%d] missing name", idx);
            cJSON_Delete(root);
            return -1;
        }
        snprintf(q.name, sizeof(q.name), "%s", j->valuestring);
        j = cJSON_GetObjectItem(item, "enabled");
        q.enabled = json_truthy(j, 1);
        q.strategy = CQ_STRATEGY_RING_ALL;
        j = cJSON_GetObjectItem(item, "strategy");
        if (j && cJSON_IsString(j)) {
            if (!strcmp(j->valuestring, "leastrecent")) q.strategy = CQ_STRATEGY_LEAST_RECENT;
            else if (!strcmp(j->valuestring, "fewestcalls")) q.strategy = CQ_STRATEGY_FEWEST_CALLS;
            else if (!strcmp(j->valuestring, "roundrobin")) q.strategy = CQ_STRATEGY_ROUND_ROBIN;
        }
        j = cJSON_GetObjectItem(item, "max_wait_s");
        q.max_wait_s = (int)(j && cJSON_IsNumber(j) && j->valueint > 0 ? j->valueint : 600);
        j = cJSON_GetObjectItem(item, "wrap_time_s");
        q.wrap_time_s = (int)(j && cJSON_IsNumber(j) && j->valueint >= 0 ? j->valueint : 30);
        j = cJSON_GetObjectItem(item, "require_skill");
        if (j && cJSON_IsString(j))
            snprintf(q.require_skill, sizeof(q.require_skill), "%s", j->valuestring);
        /* members 数组 */
        j = cJSON_GetObjectItem(item, "members");
        if (j && cJSON_IsArray(j)) {
            int mi = 0;
            cJSON *mbr;
            cJSON_ArrayForEach(mbr, j) {
                if (mi >= CQ_MEMB_MAX) break;
                if (cJSON_IsString(mbr) && mbr->valuestring[0])
                    snprintf(q.members[mi++], sizeof(q.members[mi]), "%s", mbr->valuestring);
            }
            q.n_members = mi;
        }
        if (q.n_members <= 0) {
            llog(LOG_WARN, "queue", "reload: queue '%s' has no members, skip", q.name);
            continue;
        }
        cq_add(m, &q);
        ++idx;
    }
    cJSON_Delete(root);
    llog(LOG_INFO, "queue", "reload: loaded %d queues (cap=%d)", idx, CQ_QUEUE_CAP);
    return NSBC_OK;
}

NSBC_MOD0_CAT(queue, 2, MOD_L_WORKER_READY, MOD_F_REQUIRED, MOD_CAT_CALL_CENTER,
          queue_mod_init, queue_mod_fini);
