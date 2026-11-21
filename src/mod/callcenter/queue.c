/* ACD 队列——粗骨架（REQ-PBX-CC-002~005，P2）
 *
 * 装配：core 在话务事件（座席空闲/新入队/挂机率）上调 cq_dispatch；
 * 选中座席经 b2b 振铃，接起 cq_answered + agent_answer；挂机 cq_finish
 * → agent_release（WRAP）。等待音/MoH 走 player；溢出目标经 dialplan
 * 重新进入路由（IVR/VM/另一队列）。
 *
 * 并发模型（P1 修复）：
 *   cc_mgr_t 内部加 pthread_mutex_t cq_lock，保护 slots[]/wait[]/n_wait
 *   等数组的并发读写（enqueue/dispatch/abandon/tick_overflow 可能被
 *   多个 worker 线程同时调）。
 *   加锁顺序：cq_lock → pool_lock（agent 函数内部持 pool_lock，
 *   queue 函数调 agent 函数前已持 cq_lock），无反向依赖→不死锁。
 */
#include "queue.h"
#include "log.h"
#include <pthread.h>
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
    /* Phase 1：sync resolve 选路所需的历史（不依赖 agent_pool）。
     * m_last_end_ms/m_answered 按 members[] 下标，settle 时更新。 */
    uint64_t    m_last_end_ms[CQ_MEMB_MAX];
    uint32_t    m_answered[CQ_MEMB_MAX];
    uint32_t    rr_cursor;
} cq_slot_t;

/* ------------------------------------------------------------------
 * 哈希工具（开放寻址，字符串键 = tenant + ':' + ext）
 * ------------------------------------------------------------------ */
static uint32_t cq_hash2(const char *a, const char *b)
{
    uint32_t h = 2166136261u;   /* FNV-1a */
    while (a != NULL && *a) { h ^= (uint8_t)*a++; h *= 16777619u; }
    h ^= (uint8_t)':'; h *= 16777619u;
    while (b != NULL && *b) { h ^= (uint8_t)*b++; h *= 16777619u; }
    h ^= h >> 16;
    return h;
}

static uint32_t cq_hash_ptr(const void *p)
{
    uint64_t v = (uint64_t)(uintptr_t)p;
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33;
    return (uint32_t)v;
}

/* ------------------------------------------------------------------
 * 呼叫占用表：call_key → (租户, 队列名, 分机)
 *   - 队列派发登记队列名（结算统计用）；
 *   - 直呼/外呼只登记分机（忙线判定用，队列名为空）。
 * 定长开放寻址哈希 + 64 分片读写锁；删除打墓碑，墓碑过多时原地重建。
 * 容量：64 × 128 = 8192 路并发呼叫；超出时打 WARN 并放弃登记（不会死锁）。
 * ------------------------------------------------------------------ */
#define CQ_CALL_SHARDS 64
#define CQ_CALL_BKTS   256                       /* 负载率 <= 0.5 */
#define CQ_CALL_TOMB   ((void *)1)

typedef struct {
    void  *call_key;                             /* NULL=空槽，CQ_CALL_TOMB=墓碑 */
    char   name[CQ_NAME_MAX];                    /* 队列名（仅队列派发非空） */
    char   tenant[2][CQ_TENANT_MAX];             /* [0]=B 腿 [1]=A 腿 */
    char   ext[2][AGENT_EXT_MAX];                /* [0]=被叫 [1]=主叫 */
} cq_call_ent_t;

typedef struct {
    pthread_rwlock_t lk;
    cq_call_ent_t    b[CQ_CALL_BKTS];
    int              live;
    int              tomb;
} cq_call_shard_t;

/* ------------------------------------------------------------------
 * 分机状态表：tenant+ext → busy 引用计数 / 置忙
 * 不删除条目（置忙需要跨 reload 保留），容量 64 × 512 = 32768 个分机。
 * ------------------------------------------------------------------ */
#define CQ_EXT_SHARDS 64
#define CQ_EXT_BKTS   512

typedef struct {
    char     tenant[CQ_TENANT_MAX];
    char     ext[AGENT_EXT_MAX];
    int      busy;
    int      paused;
    int      pause_reason;
    uint64_t since_ms;
} cq_ext_ent_t;

typedef struct {
    pthread_rwlock_t lk;
    cq_ext_ent_t     b[CQ_EXT_BKTS];
    int              used;
} cq_ext_shard_t;

struct cc_mgr {
    agent_pool_t *pool;
    cq_slot_t     slots[CQ_QUEUE_CAP];
    int           n;
    pthread_mutex_t cq_lock;  /* P1 并发保护：队列/成员数组 */
    cq_call_shard_t call_tab[CQ_CALL_SHARDS];
    cq_ext_shard_t  ext_tab[CQ_EXT_SHARDS];
    /* Phase 2：异步派发回调（cq_tick 内部选到座席后调用） */
    cq_dispatch_cb disp_cb;
    void          *disp_ud;
    /* 3.3：成员可振铃过滤（注册查询等），与 resolve 选路共用；空=不过滤 */
    cq_member_avail_fn avail_cb;
    void              *avail_ud;
    /* 3.3：max_wait 超时回调（组合根回 480 / 溢出路由） */
    cq_overflow_cb ovf_cb;
    void          *ovf_ud;
};

int cq_create(agent_pool_t *pool, cc_mgr_t **out)
{
    if (out == NULL)
        return NSBC_INVAL;
    *out = (cc_mgr_t *)calloc(1, sizeof(cc_mgr_t));
    if (*out == NULL)
        return NSBC_NOMEM;
    (*out)->pool = pool;
    pthread_mutex_init(&(*out)->cq_lock, NULL);  /* P1 */
    for (int i = 0; i < CQ_CALL_SHARDS; ++i)
        pthread_rwlock_init(&(*out)->call_tab[i].lk, NULL);
    for (int i = 0; i < CQ_EXT_SHARDS; ++i)
        pthread_rwlock_init(&(*out)->ext_tab[i].lk, NULL);
    return NSBC_OK;
}

void cq_destroy(cc_mgr_t *m)
{
    if (m == NULL)
        return;
    for (int i = 0; i < CQ_CALL_SHARDS; ++i)
        pthread_rwlock_destroy(&m->call_tab[i].lk);
    for (int i = 0; i < CQ_EXT_SHARDS; ++i)
        pthread_rwlock_destroy(&m->ext_tab[i].lk);
    pthread_mutex_destroy(&m->cq_lock);  /* P1 */
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

/* 内部版本：调用方必须已持有 cq_lock。
 * 整表 reload（queue_table_reload_json）已持锁遍历，必须用它——
 * cq_lock 是非递归 pthread mutex，reload 里再调 cq_add 会二次加锁自死锁
 * （表现为 reload 线程永久挂起、队列表被清空、resolve 报 queue missing）。 */
static int cq_add_locked(cc_mgr_t *m, const cq_queue_t *q)
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
    /* 新 slot：先清零，避免复用到已删除队列残留的统计（m_answered/rr_cursor 等）。
     * 更新已有 slot 时保留统计（上面 s->q = *q 分支）。 */
    memset(&m->slots[m->n], 0, sizeof(m->slots[m->n]));
    m->slots[m->n].q = *q;
    ++m->n;
    return NSBC_OK;
}

int cq_add(cc_mgr_t *m, const cq_queue_t *q)
{
    if (m == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&m->cq_lock);  /* P1 */
    int rc = cq_add_locked(m, q);
    pthread_mutex_unlock(&m->cq_lock);
    return rc;
}

int cq_remove(cc_mgr_t *m, const char *tenant, const char *name)
{
    int i;
    if (m == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&m->cq_lock);  /* P1 */
    for (i = 0; i < m->n; ++i) {
        if (strcmp(m->slots[i].q.tenant, tenant) == 0 &&
            strcmp(m->slots[i].q.name, name) == 0) {
            if (m->slots[i].n_wait > 0) {
                pthread_mutex_unlock(&m->cq_lock);
                return NSBC_AGAIN;
            }
            m->slots[i] = m->slots[--m->n];
            pthread_mutex_unlock(&m->cq_lock);
            return NSBC_OK;
        }
    }
    pthread_mutex_unlock(&m->cq_lock);
    return NSBC_NOENT;
}

const cq_queue_t *cq_get(cc_mgr_t *m, const char *tenant, const char *name)
{
    cq_slot_t *s = slot_find(m, tenant, name);
    return s == NULL ? NULL : &s->q;
}

/* 取最高优先级、尚未派发的 waiter（dispatch_ms==0）。
 * 已派发者正在振铃，必须跳过，否则每秒 tick 会把同一通电话重复派给更多座席。 */
static cq_waiter_t *pending_waiter(cq_slot_t *s)
{
    int best = -1;
    int i;
    /* 优先级高者先；同优先级先入队（粗骨架线性，FIFO 稳定） */
    for (i = 0; i < s->n_wait; ++i) {
        if (s->wait[i].dispatch_ms != 0)
            continue;
        if (best < 0 ||
            s->wait[i].priority > s->wait[best].priority ||
            (s->wait[i].priority == s->wait[best].priority &&
             s->wait[i].enqueue_ms < s->wait[best].enqueue_ms))
            best = i;
    }
    return best < 0 ? NULL : &s->wait[best];
}

/* 按 call_key 查找 waiter（不区分是否已派发）。 */
static cq_waiter_t *find_waiter(cq_slot_t *s, const void *call_key)
{
    int i;
    for (i = 0; i < s->n_wait; ++i) {
        if (s->wait[i].call_key == call_key)
            return &s->wait[i];
    }
    return NULL;
}

int cq_enqueue(cc_mgr_t *m, const char *tenant, const char *name,
               cq_waiter_t w)
{
    cq_slot_t *s;
    if (m == NULL || w.call_key == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&m->cq_lock);  /* P1 */
    s = slot_find(m, tenant, name);
    if (s == NULL || !s->q.enabled) {
        pthread_mutex_unlock(&m->cq_lock);
        return s == NULL ? NSBC_NOENT : NSBC_PERM_DENIED;
    }
    if (s->n_wait >= CQ_WAIT_MAX) {
        pthread_mutex_unlock(&m->cq_lock);
        return NSBC_NOSPC;
    }
    s->wait[s->n_wait++] = w;
    ++s->st.total_enqueued;
    pthread_mutex_unlock(&m->cq_lock);
    return NSBC_OK;
}

int cq_abandon(cc_mgr_t *m, void *call_key)
{
    int si, wi;
    if (m == NULL || call_key == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&m->cq_lock);  /* P1 */
    for (si = 0; si < m->n; ++si) {
        cq_slot_t *s = &m->slots[si];
        for (wi = 0; wi < s->n_wait; ++wi) {
            if (s->wait[wi].call_key == call_key) {
                s->wait[wi] = s->wait[--s->n_wait];
                ++s->st.total_abandoned;
                pthread_mutex_unlock(&m->cq_lock);
                return NSBC_OK;
            }
        }
    }
    pthread_mutex_unlock(&m->cq_lock);
    return NSBC_NOENT;
}

int cq_waiting_count(cc_mgr_t *m, const char *tenant, const char *name)
{
    cq_slot_t *s;
    if (m == NULL) return 0;
    pthread_mutex_lock(&m->cq_lock);  /* P1 */
    s = slot_find(m, tenant, name);
    int n = s == NULL ? 0 : s->n_wait;
    pthread_mutex_unlock(&m->cq_lock);
    return n;
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
    pthread_mutex_lock(&m->cq_lock);  /* P1: cq_lock → pool_lock（agent 函数内部） */
    s = slot_find(m, tenant, name);
    if (s == NULL) { pthread_mutex_unlock(&m->cq_lock); return NSBC_NOENT; }
    w = pending_waiter(s);
    if (w == NULL) { pthread_mutex_unlock(&m->cq_lock); return 0; }
    n = collect_idle(m, s, idle, CQ_MEMB_MAX);
    if (n == 0) { pthread_mutex_unlock(&m->cq_lock); return 0; }
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
        w->dispatch_ms = now_ms;
        snprintf(w->agent_ext, sizeof(w->agent_ext), "%s", out_ext[0]);
        pthread_mutex_unlock(&m->cq_lock);
        return lim;
    }
    pick_ext(&idle[pick], out_ext, 0);
    if (agent_reserve(m->pool, tenant, idle[pick].ext, now_ms) != NSBC_OK) {
        pthread_mutex_unlock(&m->cq_lock);
        return 0;
    }
    *out_call_key = w->call_key;
    w->dispatch_ms = now_ms;
    snprintf(w->agent_ext, sizeof(w->agent_ext), "%s", idle[pick].ext);
    pthread_mutex_unlock(&m->cq_lock);
    return 1;
}

/* ==================================================================
 * 呼叫占用表 / 分机状态表 —— 内部实现
 * 锁序：call_shard(rw) → ext_shard(rw)；ext 只在内部短临界区持有。
 * ================================================================== */

static cq_call_shard_t *call_shard_of(cc_mgr_t *m, const void *key)
{
    return &m->call_tab[cq_hash_ptr(key) % CQ_CALL_SHARDS];
}

static cq_call_ent_t *call_find_locked(cq_call_shard_t *sh, const void *key)
{
    uint32_t h = cq_hash_ptr(key);
    int i = (int)((h >> 6) & (CQ_CALL_BKTS - 1));
    for (int n = 0; n < CQ_CALL_BKTS; ++n) {
        cq_call_ent_t *e = &sh->b[i];
        if (e->call_key == NULL)
            return NULL;
        if (e->call_key == key)
            return e;
        i = (i + 1) & (CQ_CALL_BKTS - 1);
    }
    return NULL;
}

static void call_rehash_locked(cq_call_shard_t *sh)
{
    cq_call_ent_t old[CQ_CALL_BKTS];
    memcpy(old, sh->b, sizeof(old));
    memset(sh->b, 0, sizeof(sh->b));
    sh->live = 0;
    sh->tomb = 0;
    for (int i = 0; i < CQ_CALL_BKTS; ++i) {
        uint32_t h;
        int j;
        if (old[i].call_key == NULL || old[i].call_key == CQ_CALL_TOMB)
            continue;
        h = cq_hash_ptr(old[i].call_key);
        j = (int)((h >> 6) & (CQ_CALL_BKTS - 1));
        while (sh->b[j].call_key != NULL)
            j = (j + 1) & (CQ_CALL_BKTS - 1);
        sh->b[j] = old[i];
        ++sh->live;
    }
}

static cq_call_ent_t *call_insert_locked(cq_call_shard_t *sh, void *key)
{
    uint32_t h = cq_hash_ptr(key);
    int i = (int)((h >> 6) & (CQ_CALL_BKTS - 1));
    int tomb = -1;
    for (int n = 0; n < CQ_CALL_BKTS; ++n) {
        cq_call_ent_t *e = &sh->b[i];
        if (e->call_key == CQ_CALL_TOMB) {
            if (tomb < 0) tomb = i;
        } else if (e->call_key == NULL) {
            if (tomb >= 0) { --sh->tomb; i = tomb; }
            e = &sh->b[i];
            memset(e, 0, sizeof(*e));
            e->call_key = key;
            ++sh->live;
            return e;
        }
        i = (i + 1) & (CQ_CALL_BKTS - 1);
    }
    if (tomb >= 0) {
        --sh->tomb;
        memset(&sh->b[tomb], 0, sizeof(sh->b[tomb]));
        sh->b[tomb].call_key = key;
        ++sh->live;
        return &sh->b[tomb];
    }
    return NULL;
}

static void call_remove_locked(cq_call_shard_t *sh, cq_call_ent_t *e)
{
    e->call_key = CQ_CALL_TOMB;
    e->name[0] = '\0';
    e->tenant[0][0] = e->tenant[1][0] = '\0';
    e->ext[0][0] = e->ext[1][0] = '\0';
    --sh->live;
    ++sh->tomb;
    if (sh->tomb > CQ_CALL_BKTS / 4)
        call_rehash_locked(sh);
}

static cq_ext_shard_t *ext_shard_of(cc_mgr_t *m, const char *tenant,
                                    const char *ext)
{
    return &m->ext_tab[cq_hash2(tenant, ext) % CQ_EXT_SHARDS];
}

/* create=1 时不存在则新建；分片满返回 NULL。占位判定用 ext[0]（ext 必非空）。 */
static cq_ext_ent_t *ext_get_locked(cq_ext_shard_t *sh, const char *tenant,
                                    const char *ext, int create, int *created)
{
    uint32_t h = cq_hash2(tenant, ext);
    int i = (int)((h >> 6) & (CQ_EXT_BKTS - 1));
    const char *tn = tenant ? tenant : "";
    for (int n = 0; n < CQ_EXT_BKTS; ++n) {
        cq_ext_ent_t *e = &sh->b[i];
        if (e->ext[0] == '\0') {
            if (!create || sh->used >= CQ_EXT_BKTS)
                return NULL;
            memset(e, 0, sizeof(*e));
            snprintf(e->tenant, sizeof(e->tenant), "%s", tn);
            snprintf(e->ext, sizeof(e->ext), "%s", ext);
            e->since_ms = nsbc_now_ms();
            ++sh->used;
            if (created) *created = 1;
            return e;
        }
        if (strcmp(e->tenant, tn) == 0 && strcmp(e->ext, ext) == 0)
            return e;
        i = (i + 1) & (CQ_EXT_BKTS - 1);
    }
    return NULL;
}

/* 分机忙线 +1 / -1（内部，调用方不得持 ext 锁） */
static void ext_busy_add(cc_mgr_t *m, const char *tenant, const char *ext, int delta)
{
    cq_ext_shard_t *sh;
    cq_ext_ent_t *e;
    if (m == NULL || ext == NULL || ext[0] == '\0')
        return;
    sh = ext_shard_of(m, tenant, ext);
    pthread_rwlock_wrlock(&sh->lk);
    e = ext_get_locked(sh, tenant, ext, delta > 0, NULL);
    if (e != NULL) {
        e->busy += delta;
        if (e->busy < 0) e->busy = 0;
        e->since_ms = nsbc_now_ms();
    }
    pthread_rwlock_unlock(&sh->lk);
}

/* call_key 占用登记（slot：0=B 腿被叫，1=A 腿主叫）。
 * tenant/ext 变化时先释放旧分机再占用新分机。
 * do_inc=0：新分机已由 ext_try_claim 原子占位，这里只记录归属。
 * name 非空时写入队列归属（保留已有队列名，避免 mark_busy 把统计归属清掉）。 */
static int call_upsert(cc_mgr_t *m, void *call_key, const char *tenant,
                       const char *name, const char *ext, int slot, int do_inc)
{
    cq_call_shard_t *cs;
    cq_call_ent_t *e;
    const char *tn = tenant ? tenant : "";
    int need_inc = 0;
    char old_tn[CQ_TENANT_MAX], old_ext[AGENT_EXT_MAX];

    if (m == NULL || call_key == NULL || ext == NULL || ext[0] == '\0' ||
        slot < 0 || slot > 1)
        return NSBC_INVAL;

    cs = call_shard_of(m, call_key);
    old_tn[0] = old_ext[0] = '\0';
    pthread_rwlock_wrlock(&cs->lk);
    e = call_find_locked(cs, call_key);
    if (e != NULL) {
        int same = strcmp(e->tenant[slot], tn) == 0 &&
                   strcmp(e->ext[slot], ext) == 0;
        if (!same) {
            snprintf(old_tn, sizeof(old_tn), "%s", e->tenant[slot]);
            snprintf(old_ext, sizeof(old_ext), "%s", e->ext[slot]);
            snprintf(e->tenant[slot], sizeof(e->tenant[slot]), "%s", tn);
            snprintf(e->ext[slot], sizeof(e->ext[slot]), "%s", ext);
            need_inc = 1;
        }
        if (name != NULL && name[0] != '\0')
            snprintf(e->name, sizeof(e->name), "%s", name);
    } else {
        e = call_insert_locked(cs, call_key);
        if (e == NULL) {
            pthread_rwlock_unlock(&cs->lk);
            llog(LOG_WARN, "cc", "call table full (>%d), drop busy trace",
                 CQ_CALL_SHARDS * (CQ_CALL_BKTS / 2));
            return NSBC_NOSPC;
        }
        snprintf(e->tenant[slot], sizeof(e->tenant[slot]), "%s", tn);
        snprintf(e->ext[slot], sizeof(e->ext[slot]), "%s", ext);
        if (name != NULL)
            snprintf(e->name, sizeof(e->name), "%s", name);
        need_inc = 1;
    }
    pthread_rwlock_unlock(&cs->lk);

    /* 锁已释放，再动 ext 分片：避免持 call 锁跨分片等待 */
    if (need_inc) {
        if (old_ext[0] != '\0')
            ext_busy_add(m, old_tn, old_ext, -1);
        if (do_inc)
            ext_busy_add(m, tn, ext, +1);
    }
    return NSBC_OK;
}

/* 原子占位：仅“空闲（busy==0 且未置忙）”时把 busy 置 1。
 * 返回 1=成功，0=已被占用/置忙/表满。
 * 与 cq_pick_member 配合，消除“选完再登记”窗口内的重复派发。 */
static int ext_try_claim(cc_mgr_t *m, const char *tenant, const char *ext)
{
    cq_ext_shard_t *sh;
    cq_ext_ent_t *e;
    if (m == NULL || ext == NULL || ext[0] == '\0')
        return 0;
    sh = ext_shard_of(m, tenant, ext);
    pthread_rwlock_wrlock(&sh->lk);
    e = ext_get_locked(sh, tenant, ext, 1, NULL);
    if (e == NULL || e->busy > 0 || e->paused) {
        pthread_rwlock_unlock(&sh->lk);
        return 0;
    }
    e->busy = 1;
    e->since_ms = nsbc_now_ms();
    pthread_rwlock_unlock(&sh->lk);
    return 1;
}

/* 该 call_key 是否已占用同一分机（重复 resolve/重协商时避免把自己当忙线） */
static int call_owns(cc_mgr_t *m, const void *call_key, const char *tenant,
                     const char *ext)
{
    cq_call_shard_t *cs;
    cq_call_ent_t *e;
    int own = 0;
    if (m == NULL || call_key == NULL || ext == NULL)
        return 0;
    cs = call_shard_of(m, call_key);
    pthread_rwlock_rdlock(&cs->lk);
    e = call_find_locked(cs, call_key);
    if (e != NULL &&
        strcmp(e->tenant[0], tenant ? tenant : "") == 0 &&
        strcmp(e->ext[0], ext) == 0)
        own = 1;
    pthread_rwlock_unlock(&cs->lk);
    return own;
}

int cq_ext_mark_busy_slot(cc_mgr_t *m, void *call_key, const char *tenant,
                          const char *ext, int slot)
{
    return call_upsert(m, call_key, tenant, NULL, ext, slot, 1);
}

int cq_ext_mark_busy(cc_mgr_t *m, void *call_key, const char *tenant,
                     const char *ext)
{
    return call_upsert(m, call_key, tenant, NULL, ext, 0, 1);
}

void cq_ext_release_call(cc_mgr_t *m, void *call_key)
{
    cq_call_shard_t *cs;
    cq_call_ent_t *e;
    char tn[2][CQ_TENANT_MAX], ext[2][AGENT_EXT_MAX];
    int k;
    if (m == NULL || call_key == NULL)
        return;
    cs = call_shard_of(m, call_key);
    pthread_rwlock_wrlock(&cs->lk);
    e = call_find_locked(cs, call_key);
    if (e == NULL) {
        pthread_rwlock_unlock(&cs->lk);
        return;
    }
    for (k = 0; k < 2; ++k) {
        snprintf(tn[k], sizeof(tn[k]), "%s", e->tenant[k]);
        snprintf(ext[k], sizeof(ext[k]), "%s", e->ext[k]);
    }
    call_remove_locked(cs, e);
    pthread_rwlock_unlock(&cs->lk);
    for (k = 0; k < 2; ++k) {
        if (ext[k][0] != '\0')
            ext_busy_add(m, tn[k], ext[k], -1);
    }
}

int cq_ext_pause(cc_mgr_t *m, const char *tenant, const char *ext, int reason)
{
    cq_ext_shard_t *sh;
    cq_ext_ent_t *e;
    if (m == NULL || ext == NULL || ext[0] == '\0')
        return NSBC_INVAL;
    sh = ext_shard_of(m, tenant, ext);
    pthread_rwlock_wrlock(&sh->lk);
    e = ext_get_locked(sh, tenant, ext, 1, NULL);
    if (e == NULL) {
        pthread_rwlock_unlock(&sh->lk);
        return NSBC_NOSPC;
    }
    e->paused = 1;
    e->pause_reason = reason;
    e->since_ms = nsbc_now_ms();
    pthread_rwlock_unlock(&sh->lk);
    llog(LOG_INFO, "cc", "ext pause tenant=%s ext=%s reason=%d",
         tenant ? tenant : "", ext, reason);
    return NSBC_OK;
}

int cq_ext_resume(cc_mgr_t *m, const char *tenant, const char *ext)
{
    cq_ext_shard_t *sh;
    cq_ext_ent_t *e;
    if (m == NULL || ext == NULL || ext[0] == '\0')
        return NSBC_INVAL;
    sh = ext_shard_of(m, tenant, ext);
    pthread_rwlock_wrlock(&sh->lk);
    e = ext_get_locked(sh, tenant, ext, 0, NULL);
    if (e == NULL) {
        pthread_rwlock_unlock(&sh->lk);
        return NSBC_NOENT;
    }
    e->paused = 0;
    e->pause_reason = CQ_EXT_REASON_NONE;
    e->since_ms = nsbc_now_ms();
    pthread_rwlock_unlock(&sh->lk);
    llog(LOG_INFO, "cc", "ext resume tenant=%s ext=%s",
         tenant ? tenant : "", ext);
    return NSBC_OK;
}

int cq_ext_is_free(cc_mgr_t *m, const char *tenant, const char *ext)
{
    cq_ext_shard_t *sh;
    cq_ext_ent_t *e;
    int free_;
    if (m == NULL || ext == NULL || ext[0] == '\0')
        return 1;
    sh = ext_shard_of(m, tenant, ext);
    pthread_rwlock_rdlock(&sh->lk);
    e = ext_get_locked(sh, tenant, ext, 0, NULL);
    free_ = (e == NULL) || (e->busy == 0 && !e->paused);
    pthread_rwlock_unlock(&sh->lk);
    return free_;
}

int cq_ext_get(cc_mgr_t *m, const char *tenant, const char *ext,
               cq_ext_state_t *out)
{
    cq_ext_shard_t *sh;
    cq_ext_ent_t *e;
    if (m == NULL || ext == NULL || out == NULL)
        return NSBC_INVAL;
    sh = ext_shard_of(m, tenant, ext);
    pthread_rwlock_rdlock(&sh->lk);
    e = ext_get_locked(sh, tenant, ext, 0, NULL);
    if (e == NULL) {
        pthread_rwlock_unlock(&sh->lk);
        return NSBC_NOENT;
    }
    out->busy = e->busy;
    out->paused = e->paused;
    out->pause_reason = e->pause_reason;
    out->since_ms = e->since_ms;
    pthread_rwlock_unlock(&sh->lk);
    return NSBC_OK;
}

int cq_ext_list_json(cc_mgr_t *m, const char *tenant, char *out, size_t cap)
{
    size_t pos = 0;
    int first = 1;
    const char *want = tenant ? tenant : "";
    if (m == NULL || out == NULL || cap == 0)
        return NSBC_INVAL;
    pos += (size_t)snprintf(out + pos, cap - pos, "[");
    for (int si = 0; si < CQ_EXT_SHARDS && pos < cap; ++si) {
        cq_ext_shard_t *sh = &m->ext_tab[si];
        pthread_rwlock_rdlock(&sh->lk);
        for (int i = 0; i < CQ_EXT_BKTS; ++i) {
            cq_ext_ent_t *e = &sh->b[i];
            int n;
            if (e->ext[0] == '\0')
                continue;
            if (want[0] != '\0' && strcmp(e->tenant, want) != 0)
                continue;
            n = snprintf(out + pos, cap - pos,
                         "%s{\"tenant\":\"%s\",\"ext\":\"%s\",\"busy\":%d,"
                         "\"paused\":%d,\"reason\":%d,\"since_ms\":%llu}",
                         first ? "" : ",", e->tenant, e->ext, e->busy,
                         e->paused, e->pause_reason,
                         (unsigned long long)e->since_ms);
            if (n < 0 || (size_t)n >= cap - pos) {
                pthread_rwlock_unlock(&sh->lk);
                return NSBC_NOSPC;
            }
            pos += (size_t)n;
            first = 0;
        }
        pthread_rwlock_unlock(&sh->lk);
    }
    if (pos + 2 > cap)
        return NSBC_NOSPC;
    out[pos++] = ']';
    out[pos] = '\0';
    return (int)pos;
}

/* ==================================================================
 * Phase 1：sync resolve 阶段的只读选路 + 统计结算
 *   两阶段：第一阶段只在本队列锁内拷贝成员快照，第二阶段脱离队列锁做
 *   在线/忙线判定（每个分机一次 O(1) 哈希读），避免“持 cq_lock 调外部
 *   模块”的跨模块长临界区。
 * ================================================================== */
static int member_index(const cq_queue_t *q, const char *ext)
{
    int i;
    for (i = 0; i < q->n_members; ++i) {
        if (strcmp(q->members[i], ext) == 0)
            return i;
    }
    return -1;
}

int cq_pick_member(cc_mgr_t *m, const char *tenant, const char *name,
                   uint32_t rnd, cq_member_avail_fn avail, void *ud,
                   void *call_key, char *out_ext, size_t outsz)
{
    cq_slot_t *s;
    char exts[CQ_MEMB_MAX][AGENT_EXT_MAX];
    uint64_t last_end[CQ_MEMB_MAX];
    uint32_t answered[CQ_MEMB_MAX];
    char qtenant[CQ_TENANT_MAX];
    cq_strategy_t strat = CQ_STRATEGY_RING_ALL;
    int nm = 0, i, n = 0, k, start = 0;
    int cand[CQ_MEMB_MAX];
    (void)rnd;

    if (m == NULL || out_ext == NULL || outsz == 0)
        return NSBC_INVAL;

    /* 阶段 1：快照（只持队列锁做 memcpy，不调用任何外部函数） */
    pthread_mutex_lock(&m->cq_lock);
    s = slot_find(m, tenant, name);
    if (s == NULL || !s->q.enabled) {
        pthread_mutex_unlock(&m->cq_lock);
        return NSBC_NOENT;
    }
    nm = s->q.n_members;
    if (nm > CQ_MEMB_MAX) nm = CQ_MEMB_MAX;
    strat = s->q.strategy;
    snprintf(qtenant, sizeof(qtenant), "%s", s->q.tenant);
    for (i = 0; i < nm; ++i) {
        snprintf(exts[i], sizeof(exts[i]), "%s", s->q.members[i]);
        last_end[i] = s->m_last_end_ms[i];
        answered[i] = s->m_answered[i];
    }
    pthread_mutex_unlock(&m->cq_lock);

    /* 阶段 2：过滤（注册/在线回调 + 忙线/置忙）——无队列锁 */
    for (i = 0; i < nm; ++i) {
        if (avail != NULL && !avail(exts[i], ud))
            continue;
        if (!cq_ext_is_free(m, qtenant, exts[i]))
            continue;
        cand[n++] = i;
    }
    if (n == 0)
        return -1;

    /* 阶段 3：按策略排序候选项（小数组插入排序，不跨外部调用） */
    if (strat == CQ_STRATEGY_LEAST_RECENT ||
        strat == CQ_STRATEGY_FEWEST_CALLS) {
        for (i = 1; i < n; ++i) {
            int key = cand[i], j = i - 1;
            while (j >= 0 &&
                   (strat == CQ_STRATEGY_LEAST_RECENT
                    ? last_end[cand[j]] > last_end[key]
                    : answered[cand[j]] > answered[key])) {
                cand[j + 1] = cand[j];
                --j;
            }
            cand[j + 1] = key;
        }
    } else if (strat == CQ_STRATEGY_ROUND_ROBIN) {
        pthread_mutex_lock(&m->cq_lock);
        s = slot_find(m, tenant, name);
        start = s ? (int)(s->rr_cursor % (uint32_t)n) : 0;
        pthread_mutex_unlock(&m->cq_lock);
    }

    /* 阶段 4：原子占位——并发下同一分机只会被一个呼叫抢到，抢不到顺延下一个 */
    for (k = 0; k < n; ++k) {
        int idx = cand[(start + k) % n];
        if (call_key != NULL) {
            /* 同一呼叫重复 resolve 到同一分机：沿用，不重复占位 */
            if (!call_owns(m, call_key, qtenant, exts[idx])) {
                if (!ext_try_claim(m, qtenant, exts[idx]))
                    continue;
                if (call_upsert(m, call_key, qtenant, name, exts[idx], 0, 0)
                    != NSBC_OK) {
                    ext_busy_add(m, qtenant, exts[idx], -1);  /* 回滚占位 */
                    continue;
                }
            }
        }
        if (strat == CQ_STRATEGY_ROUND_ROBIN) {
            pthread_mutex_lock(&m->cq_lock);
            s = slot_find(m, tenant, name);
            if (s) s->rr_cursor = (uint32_t)((start + k + 1) % n);
            pthread_mutex_unlock(&m->cq_lock);
        }
        snprintf(out_ext, outsz, "%s", exts[idx]);
        return 0;
    }
    return -1;
}

void cq_track_dispatch(cc_mgr_t *m, void *call_key, const char *tenant,
                       const char *name, const char *ext)
{
    (void)call_upsert(m, call_key, tenant, name, ext, 0, 1);
}

static int waiter_remove(cq_slot_t *s, const void *call_key);

void cq_settle_call(cc_mgr_t *m, void *call_key, uint64_t now_ms,
                    uint64_t answered_ms)
{
    cq_call_shard_t *cs;
    cq_call_ent_t *e;
    char tn[2][CQ_TENANT_MAX], ext[2][AGENT_EXT_MAX], qn[CQ_NAME_MAX];
    cq_slot_t *s;
    int mi, k;

    if (m == NULL || call_key == NULL)
        return;

    cs = call_shard_of(m, call_key);
    pthread_rwlock_wrlock(&cs->lk);
    e = call_find_locked(cs, call_key);
    if (e == NULL) {                 /* 未登记（普通呼叫/仍在排队）→ 只清 waiter */
        pthread_rwlock_unlock(&cs->lk);
        (void)cq_abandon(m, call_key);   /* 3.3：排队中主叫挂断，移除 waiter */
        return;
    }
    for (k = 0; k < 2; ++k) {
        snprintf(tn[k], sizeof(tn[k]), "%s", e->tenant[k]);
        snprintf(ext[k], sizeof(ext[k]), "%s", e->ext[k]);
    }
    snprintf(qn, sizeof(qn), "%s", e->name);
    call_remove_locked(cs, e);
    pthread_rwlock_unlock(&cs->lk);

    /* 分机忙线释放：A/B 两腿各减一次；再落队列统计 */
    for (k = 0; k < 2; ++k) {
        if (ext[k][0] != '\0')
            ext_busy_add(m, tn[k], ext[k], -1);
    }

    if (qn[0] == '\0')
        return;                      /* 直呼/外呼：只做忙线释放 */

    pthread_mutex_lock(&m->cq_lock);
    s = slot_find(m, tn[0], qn);
    if (s != NULL) {
        mi = member_index(&s->q, ext[0]);
        if (mi >= 0 && answered_ms > 0) {
            ++s->m_answered[mi];
            s->m_last_end_ms[mi] = now_ms;
        }
    }
    pthread_mutex_unlock(&m->cq_lock);

    /* 3.3：该呼叫已结束（接起或未接），从等待队列移除 waiter，
     * 避免 dispatch_ms!=0 的僵尸项把 CQ_WAIT_MAX 撑满。 */
    pthread_mutex_lock(&m->cq_lock);
    for (k = 0; k < m->n; ++k)
        (void)waiter_remove(&m->slots[k], call_key);
    pthread_mutex_unlock(&m->cq_lock);
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

int cq_answered(cc_mgr_t *m, void *call_key, uint64_t answered_ms, int sla_s)
{
    cq_slot_t *s = NULL;
    cq_waiter_t *w = NULL;
    uint64_t waited;
    int si;
    if (m == NULL || call_key == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&m->cq_lock);  /* P1 */
    for (si = 0; si < m->n; ++si) {
        w = find_waiter(&m->slots[si], call_key);
        if (w != NULL) { s = &m->slots[si]; break; }
    }
    if (w == NULL) { pthread_mutex_unlock(&m->cq_lock); return NSBC_NOENT; }
    /* 座席取派发时记录的 agent_ext（异步派发后不再依赖 tenant/name 反查） */
    if (agent_answer(m->pool, s->q.tenant, w->agent_ext, answered_ms) != NSBC_OK) {
        pthread_mutex_unlock(&m->cq_lock);
        return NSBC_PROTO;
    }
    waited = answered_ms > w->enqueue_ms ? answered_ms - w->enqueue_ms : 0;
    w->connect_ms = answered_ms;
    ++s->st.total_answered;
    if (waited > s->st.max_wait_ms)
        s->st.max_wait_ms = waited;
    if (sla_s > 0 && waited <= (uint64_t)sla_s * 1000u)
        ++s->st.answered_within_sla;
    (void)waiter_remove(s, call_key);
    pthread_mutex_unlock(&m->cq_lock);
    return NSBC_OK;
}

int cq_finish(cc_mgr_t *m, const char *tenant, const char *name,
              const char *ext, uint64_t now_ms)
{
    cq_slot_t *s;
    int wrap_s, i;
    if (m == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&m->cq_lock);  /* P1: 读 wrap_time_s 需要锁保护 */
    s = slot_find(m, tenant, name);
    if (s == NULL) { pthread_mutex_unlock(&m->cq_lock); return NSBC_NOENT; }
    wrap_s = s->q.wrap_time_s;
    /* 清理该座席名下已派发的 waiter：若 cq_answered 未调用（未接/异常结束），
     * 这里兜底摘除，避免队列残留一条永远不可能被接起的记录。 */
    if (ext != NULL) {
        for (i = 0; i < s->n_wait; ++i) {
            if (s->wait[i].dispatch_ms != 0 &&
                strcmp(s->wait[i].agent_ext, ext) == 0) {
                s->wait[i] = s->wait[--s->n_wait];
                break;
            }
        }
    }
    pthread_mutex_unlock(&m->cq_lock);
    return agent_release(m->pool, tenant, ext, now_ms, wrap_s);
}

int cq_tick_overflow_ex(cc_mgr_t *m, const char *tenant, const char *name,
                        uint64_t now_ms, cq_ovf_t *out, int cap)
{
    cq_slot_t *s;
    int i, moved = 0;
    if (m == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&m->cq_lock);  /* P1 */
    s = slot_find(m, tenant, name);
    if (s == NULL || s->q.max_wait_s <= 0) {
        pthread_mutex_unlock(&m->cq_lock);
        return s == NULL ? NSBC_NOENT : 0;
    }
    /* 倒序压缩：超时者交溢出（目标路由由组合根执行，这里只移除+回传 key/callid）。
     * 已派发（正在振铃）的 waiter 不参与超时溢出——它已占用座席，
     * 由 cq_answered / cq_finish / 呼叫侧 cq_requeue 闭环处理。 */
    for (i = s->n_wait - 1; i >= 0; --i) {
        if (s->wait[i].dispatch_ms != 0)
            continue;
        if (now_ms - s->wait[i].enqueue_ms >=
            (uint64_t)s->q.max_wait_s * 1000u) {
            if (out != NULL && moved < cap) {
                out[moved].call_key = s->wait[i].call_key;
                snprintf(out[moved].callid, sizeof(out[moved].callid), "%s",
                         s->wait[i].callid);
            }
            s->wait[i] = s->wait[--s->n_wait];
            ++s->st.total_overflowed;
            ++moved;
        }
    }
    pthread_mutex_unlock(&m->cq_lock);
    return moved;
}

int cq_tick_overflow(cc_mgr_t *m, const char *tenant, const char *name,
                     uint64_t now_ms)
{
    return cq_tick_overflow_ex(m, tenant, name, now_ms, NULL, 0);
}

void cq_set_dispatch_cb(cc_mgr_t *m, cq_dispatch_cb cb, void *ud)
{
    if (m == NULL) return;
    m->disp_cb = cb;
    m->disp_ud = ud;
}

void cq_set_member_avail_cb(cc_mgr_t *m, cq_member_avail_fn fn, void *ud)
{
    if (m == NULL) return;
    m->avail_cb = fn;
    m->avail_ud = ud;
}

void cq_set_overflow_cb(cc_mgr_t *m, cq_overflow_cb cb, void *ud)
{
    if (m == NULL) return;
    m->ovf_cb = cb;
    m->ovf_ud = ud;
}

/* 3.3：取队首待派发 waiter 的 call_key（不改变状态）。
 * 调用方随后用 cq_pick_member(..., call_key) 原子占位，成功后 cq_mark_dispatched。 */
int cq_peek_pending(cc_mgr_t *m, const char *tenant, const char *name,
                    void **out_key, char *out_callid, size_t cid_sz)
{
    cq_slot_t *s;
    cq_waiter_t *w;
    if (m == NULL || out_key == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&m->cq_lock);
    s = slot_find(m, tenant, name);
    if (s == NULL) {
        pthread_mutex_unlock(&m->cq_lock);
        return NSBC_NOENT;
    }
    w = pending_waiter(s);
    if (w == NULL) {
        pthread_mutex_unlock(&m->cq_lock);
        return 1;
    }
    *out_key = w->call_key;
    if (out_callid && cid_sz)
        snprintf(out_callid, cid_sz, "%s", w->callid);
    pthread_mutex_unlock(&m->cq_lock);
    return 0;
}

int cq_mark_dispatched(cc_mgr_t *m, void *call_key, const char *ext,
                       uint64_t now_ms)
{
    int si;
    if (m == NULL || call_key == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&m->cq_lock);
    for (si = 0; si < m->n; ++si) {
        cq_waiter_t *w = find_waiter(&m->slots[si], call_key);
        if (w != NULL) {
            w->dispatch_ms = now_ms;
            snprintf(w->agent_ext, sizeof(w->agent_ext), "%s", ext ? ext : "");
            pthread_mutex_unlock(&m->cq_lock);
            return NSBC_OK;
        }
    }
    pthread_mutex_unlock(&m->cq_lock);
    return NSBC_NOENT;   /* 主叫已放弃并移出队列 */
}

/* 只释放一个槽位的占用（slot 0 = B 腿被叫分机）。
 * 派发后 B 腿未接通时，座席分机要恢复空闲；但主叫（slot 1）仍在通话，
 * 不能整条删除 call 条目，否则主叫分机的忙线计数会被提前清掉。
 * 两个槽都空时才删除条目。 */
static void call_release_slot(cc_mgr_t *m, void *call_key, int slot)
{
    cq_call_shard_t *cs;
    cq_call_ent_t *e;
    char tn[CQ_TENANT_MAX], ext[AGENT_EXT_MAX];
    if (m == NULL || call_key == NULL || slot < 0 || slot > 1)
        return;
    cs = call_shard_of(m, call_key);
    tn[0] = ext[0] = '\0';
    pthread_rwlock_wrlock(&cs->lk);
    e = call_find_locked(cs, call_key);
    if (e == NULL) {
        pthread_rwlock_unlock(&cs->lk);
        return;
    }
    snprintf(tn, sizeof(tn), "%s", e->tenant[slot]);
    snprintf(ext, sizeof(ext), "%s", e->ext[slot]);
    e->tenant[slot][0] = '\0';
    e->ext[slot][0] = '\0';
    if (e->tenant[1 - slot][0] == '\0' && e->ext[1 - slot][0] == '\0')
        call_remove_locked(cs, e);
    pthread_rwlock_unlock(&cs->lk);
    if (ext[0] != '\0')
        ext_busy_add(m, tn, ext, -1);
}

/* 3.3：派发后 B 腿未接通（486/408/无响应）→ 该 waiter 回到待派发，
 * 同时释放本次派发占用的座席忙线（slot 0），保证该座席可被再次选中。 */
void cq_requeue(cc_mgr_t *m, void *call_key)
{
    int si;
    int hit = 0;
    if (m == NULL || call_key == NULL)
        return;
    pthread_mutex_lock(&m->cq_lock);
    for (si = 0; si < m->n; ++si) {
        cq_waiter_t *w = find_waiter(&m->slots[si], call_key);
        if (w != NULL && w->dispatch_ms != 0) {
            w->dispatch_ms = 0;
            w->agent_ext[0] = '\0';
            hit = 1;
            break;
        }
    }
    pthread_mutex_unlock(&m->cq_lock);
    if (hit)
        call_release_slot(m, call_key, 0);
}


/* 派发回收：座席被 agent_tick 收回（RESERVED 超时→IDLE）或异常离线时，
 * 把仍挂着 dispatch_ms 的 waiter 重置回待派发，避免队列永久卡住。
 * 仍处于 RESERVED/ONCALL 的座席视为服务中，交给 cq_answered / cq_finish。
 * 锁顺序 cq_lock → pool_lock，与 cq_dispatch 一致。 */
static int requeue_stale(cc_mgr_t *m, uint64_t now_ms)
{
    int si, wi, moved = 0;
    pthread_mutex_lock(&m->cq_lock);
    for (si = 0; si < m->n; ++si) {
        cq_slot_t *s = &m->slots[si];
        for (wi = 0; wi < s->n_wait; ++wi) {
            cq_waiter_t *w = &s->wait[wi];
            agent_t a;
            if (w->dispatch_ms == 0)
                continue;
            if (now_ms - w->dispatch_ms < 1000u)  /* 给振铃留观察窗 */
                continue;
            if (agent_lookup(m->pool, s->q.tenant, w->agent_ext, &a) == NSBC_OK &&
                (a.state == AGENT_RESERVED || a.state == AGENT_ONCALL))
                continue;  /* 仍在服务中 */
            w->dispatch_ms = 0;
            w->agent_ext[0] = '\0';
            ++moved;
        }
    }
    pthread_mutex_unlock(&m->cq_lock);
    return moved;
}

int cq_tick(cc_mgr_t *m, uint64_t now_ms)
{
    char names[CQ_QUEUE_CAP][CQ_NAME_MAX];
    char tenants[CQ_QUEUE_CAP][CQ_TENANT_MAX];
    int nq = 0, i, acts = 0;
    if (m == NULL) return 0;

    /* 1) WRAP 到期 → IDLE；RESERVED 超时回收；被回收座席的 waiter 重新可派发 */
    if (m->pool != NULL) {
        acts += agent_tick(m->pool, now_ms);
        /* 3.3：新派发模型由 cq_mark_dispatched 记录，B 腿结果/超时由呼叫侧
         * cq_requeue 闭环。若仍按 agent_pool 状态判定，刚派发出去、
         * pool 尚未标 RESERVED 的 waiter 会在 1 秒后被误判空闲而重复派发。 */
        if (m->disp_cb == NULL)
            acts += requeue_stale(m, now_ms);
    }

    /* 2) 快照队列名（避免持 cq_lock 调用 dispatch/overflow 造成自锁） */
    pthread_mutex_lock(&m->cq_lock);
    for (i = 0; i < m->n && nq < CQ_QUEUE_CAP; ++i) {
        snprintf(names[nq], sizeof(names[nq]), "%s", m->slots[i].q.name);
        snprintf(tenants[nq], sizeof(tenants[nq]), "%s", m->slots[i].q.tenant);
        ++nq;
    }
    pthread_mutex_unlock(&m->cq_lock);

    for (i = 0; i < nq; ++i) {
        cq_ovf_t ovf[CQ_WAIT_MAX];
        int novf;

        /* 3) 超时溢出 */
        novf = cq_tick_overflow_ex(m, tenants[i], names[i], now_ms,
                                   ovf, CQ_WAIT_MAX);
        if (novf > 0) {
            acts += novf;
            if (m->ovf_cb != NULL) {
                for (int k = 0; k < novf; ++k)
                    m->ovf_cb(ovf[k].call_key, ovf[k].callid,
                              tenants[i], names[i], m->ovf_ud);
            }
            llog(LOG_INFO, "cc", "cq_tick: queue '%s/%s' overflow %d",
                 tenants[i], names[i], novf);
        }

        /* 4) 有 waiter 则按策略选成员并派发（cb 负责真正建 B 腿）。
         * 3.3：同步 resolve 路径不维护 agent_pool 状态，改用 cq_pick_member
         * 原子占位（与 resolve 选路同一套逻辑），选到后由组合根建 B 腿。 */
        if (m->disp_cb == NULL)
            continue;
        for (int guard = 0; guard < CQ_MEMB_MAX; ++guard) {
            void *ckey = NULL;
            char cid[CQ_CALLID_MAX] = "";
            char ext[AGENT_EXT_MAX];
            if (cq_peek_pending(m, tenants[i], names[i], &ckey,
                                cid, sizeof(cid)) != 0)
                break;
            if (cq_pick_member(m, tenants[i], names[i],
                               (uint32_t)(now_ms & 0xffffffffu),
                               m->avail_cb, m->avail_ud, ckey,
                               ext, sizeof(ext)) != 0)
                break;   /* 当前无可用成员，留给下一 tick */
            if (cq_mark_dispatched(m, ckey, ext, now_ms) != NSBC_OK) {
                /* waiter 刚被主叫放弃移除 → 回滚占位 */
                cq_ext_release_call(m, ckey);
                continue;
            }
            if (m->disp_cb(ckey, cid, tenants[i], names[i], ext,
                           m->disp_ud) != NSBC_OK) {
                /* 派发失败：该 waiter 已由回调退回等待，本 tick 不再空转重试 */
                ++acts;
                break;
            }
            ++acts;
        }
    }
    return acts;
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
    pthread_mutex_lock(&m->cq_lock);  /* P1: 整表 reload 持 cq_lock */
    /* 3.3：改为原地更新，不再 m->n=0 整体清空。
     * 清空会把正在 183 + MoH 等待的 waiter 一起丢掉（既不派发也不溢出，
     * 主叫只能一直等下去）。行为：
     *   - 同名队列：s->q 覆盖（保留 wait 列表与统计）；
     *   - 新队列：追加；
     *   - 消失队列：有 waiter 的保留为 enabled=0/无成员，等 max_wait 到期由
     *     溢出回调回 480；无 waiter 的直接移除。 */
    uint8_t seen[CQ_QUEUE_CAP];
    memset(seen, 0, sizeof(seen));
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
            pthread_mutex_unlock(&m->cq_lock);  /* P1: 错误路径也要解锁 */
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
        j = cJSON_GetObjectItem(item, "overflow");
        if (j && cJSON_IsString(j))
            snprintf(q.overflow, sizeof(q.overflow), "%s", j->valuestring);
        /* 3.3：等待音乐 WAV 路径（DB moh_sound）；空/"default" 由
         * b2b_queue_hold 回落到 CQ_DEFAULT_MOH_WAV（./sounds/default/moh.wav） */
        j = cJSON_GetObjectItem(item, "moh_sound");
        if (j && cJSON_IsString(j) && j->valuestring[0] &&
            strcmp(j->valuestring, "default") != 0)
            snprintf(q.moh_wav, sizeof(q.moh_wav), "%s", j->valuestring);
        /* members 数组 */
        j = cJSON_GetObjectItem(item, "members");
        if (j && cJSON_IsArray(j)) {
            int mi = 0;
            cJSON *mbr;
            cJSON_ArrayForEach(mbr, j) {
                if (mi >= CQ_MEMB_MAX) break;
                if (cJSON_IsString(mbr) && mbr->valuestring[0]) {
                    snprintf(q.members[mi], sizeof(q.members[0]), "%s",
                             mbr->valuestring);
                    ++mi;
                }
            }
            q.n_members = mi;
        }
        if (q.n_members <= 0) {
            llog(LOG_WARN, "queue", "reload: queue '%s' has no members, skip", q.name);
            continue;
        }
        /* 已持 cq_lock：必须走 locked 版本，否则非递归锁二次加锁自死锁 */
        cq_add_locked(m, &q);
        for (int k = 0; k < m->n; ++k) {
            if (strcmp(m->slots[k].q.tenant, q.tenant) == 0 &&
                strcmp(m->slots[k].q.name, q.name) == 0) {
                seen[k] = 1;
                break;
            }
        }
        ++idx;
    }
    /* 清理本次 JSON 中已不存在的队列（swap-remove；seen 同步搬移） */
    for (int k = 0; k < m->n; ) {
        if (seen[k]) {
            ++k;
            continue;
        }
        if (m->slots[k].n_wait > 0) {
            /* 仍有主叫在等：禁用派发，保留到 max_wait 溢出回调收尾 */
            m->slots[k].q.enabled = 0;
            m->slots[k].q.n_members = 0;
            ++k;
            continue;
        }
        m->slots[k] = m->slots[m->n - 1];
        seen[k] = seen[m->n - 1];
        --m->n;
    }
    cJSON_Delete(root);
    pthread_mutex_unlock(&m->cq_lock);  /* P1 */
    llog(LOG_INFO, "queue", "reload: loaded %d queues (cap=%d)", idx, CQ_QUEUE_CAP);
    return NSBC_OK;
}

NSBC_MOD0_CAT(queue, 2, MOD_L_WORKER_READY, MOD_F_REQUIRED, MOD_CAT_CALL_CENTER,
          queue_mod_init, queue_mod_fini);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
