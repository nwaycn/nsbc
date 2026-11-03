/* 璐圭巼/瀹炴椂璁¤垂鈥斺€旂矖楠ㄦ灦锛圧EQ-PBX-BILL-001~005锛孭1锛? *
 * 绮楅鏋剁害瀹氾細
 *  - 璐圭巼琛?璐︽埛涓哄畾妲芥暟缁勶紙鎺ョ嚎鎵规崲鍒嗙墖鍝堝笇 + DBAL 浣欓琛岋紝
 *    浣欓鎵ｅ噺璧板崟琛屽師瀛?UPDATE ... WHERE balance>=x 闃查€忔敮绔炰簤锛夛紱
 *  - 棰勫崰璁板綍瀹氭Ы锛涘懆鏈熸墸璐癸紙P1 姣忚璐瑰潡锛夊湪鎺ョ嚎鎵圭敱鏃堕棿杞┍鍔紝
 *    灞婃椂澶嶇敤 settle锛堟棫棰勫崰锛? reserve锛堜笅涓€鍧楋級涓ゆ寮忋€? * 閲戦涓€寰?int64 寰厓锛屼笉鍋氭诞鐐归櫎娉曘€? */
#include "rating.h"
#include "ivr.h"   /* jstr / jint / jarray_at — Round 1 公开的手写 JSON 工具 */
#include "log.h"
#include <ctype.h>   /* isdigit */
#include <string.h>  /* memset, strcmp, strlen, snprintf */
#include <time.h>    /* time, localtime_r */
/* 账户：同时承载 accounts 表 + billing_transactions 关联。
 * 新增字段向后兼容（旧 accounts.reload JSON 不含这些字段，jint 默认 0 / jstr 默认 ""）。*/
typedef struct {
    int      used;
    char     key[BILL_ACCT_KEY];
    int64_t  balance;
    int64_t  reserved;
    int64_t  credit_limit;

    /* === Phase 1 新增 === */
    int64_t  tenant_id;          /* 多租户隔离 */
    char     account_type[16];   /* "extension" / "trunk" / "tenant" / "reseller" */
    int64_t  rate_plan_id;       /* 关联 rate_plans.id（0 = 走 rate_tables 旧费率）*/
    int64_t  low_threshold_uc;   /* 低于此值触发告警回调（0 = 不告警）*/

    /* === Phase 2 L5: OVERDRAWN kill 门闩（边沿检测）===
     * balance < -(credit_limit + low_threshold_uc) 时置 1 → 触发 kill 同账户其他通话。
     * 余额恢复 >= -(credit_limit + low_threshold_uc) * 0.8 时复位 0（滞回防抖动）。*/
    uint8_t  overdrawn_gate;
} acct_t;

typedef struct {
    int      used;
    uint64_t id;
    size_t   ai;                  /* acct index into b->accts[] */
    int64_t  amount;              /* reserved amount in uc */
    char     call_id[40];         /* SIP Call-ID — bind to a single call so settle by call_id is precise */

    /* === Phase 2 L4: 实时扣费 tick 所需 ===
     * answer_ms: ANSWER 时刻（CLOCK_MONOTONIC ms）— tick 算实时 billsec
     * last_bill_sec: 上次 tick 已确认的 billsec（防重复 reserve bump）
     * called/trunk_id: 精确费率查找（resv 里没存的话 tick 只能走保守模式）
     * called_len: bill_resv_fill 时一次性存 strlen(called) — tick 热路径零 strlen 开销 */
    uint64_t answer_ms;
    int64_t  last_bill_sec;
    char     called[48];
    uint8_t  called_len;
    int64_t  trunk_id;
} resv_t;

/* trunk_name → trunk_id 映射（Phase 1.5 L1）
 * CDR trunk 是字符串（如 "china_unicom"），rate_lookup_stair 要 int64 trunk_id。
 * 这个映射表由 OAM trunk_map.reload 命令推（或 trunks.reload 同步推）。
 * lookup 时 name 不存在返回 0 → 走 trunk_id=0 "不限" 兜底。 */
#define BILL_TRUNK_MAP_MAX 64
typedef struct {
    char    name[64];     /* trunk name (CDR 里 ev->trunk 字符串) */
    int64_t id;           /* trunk_id (rate_plan_entries.trunk_id) */
    int     used;
} trunk_map_entry_t;

struct billing {
    pthread_rwlock_t lock;
    rate_entry_t rates[RATE_PLAN_MAX];
    int          n_rates;
    acct_t       accts[BILL_ACCT_MAX];
    resv_t       resv[4096];
    uint64_t     next_resv;
    int64_t      low_threshold;
    bill_low_cb  low_cb;
    void        *low_ud;

    /* Phase 1.5 L1: trunk name → id 映射 */
    trunk_map_entry_t trunk_map[BILL_TRUNK_MAP_MAX];

    /* Phase 2: billing.enabled 开关副本
     * cfg_get_bool 读入，运行期唯一可信的计费总闸。
     * false → billing_cdr_cb / billing_pre_auth / billing_tick 全部旁路。
     * 热更新通过 bill_set_enabled() 在锁外设置（单字段 int 自然原子）。 */
    int enabled;

    /* Phase 2 L4v4: 每账户 delta 汇总 scratch buffer
     * 大小 = BILL_ACCT_MAX * sizeof(int64_t)，bill_create 时 malloc，
     * bill_destroy 时 free。生命周期和 billing_t 对齐，tick 零 malloc。
     * Pass 1 累加 → Pass 2 批量应用 → 下次 tick 前 memset 清零。*/
    int64_t *per_acct_delta;

    /* Phase 2.1: billing_pre_auth 结果缓存
     * 128 桶，每桶 1 条，TTL 30s。reactor 单线程无锁。
     * 防止同一 caller+callee 组合 INVITE 风暴时
     * 反复查 account_balance + rate_lookup_stair + acct_find。*/
#define PREAUTH_CACHE_MAX 128
    struct {
        char     key[128];
        int      result;
        uint64_t expires_ms;
    } preauth_cache[PREAUTH_CACHE_MAX];
    int preauth_cache_count;
};

int bill_create(billing_t **out)
{
    billing_t *b;
    if (out == NULL)
        return NSBC_INVAL;
    b = (billing_t *)calloc(1, sizeof(billing_t));
    if (b == NULL)
        return NSBC_NOMEM;
    pthread_rwlock_init(&b->lock, NULL);

    /* L4v4 scratch buffer — 和 accts[] 同生命周期，tick 零 malloc */
    size_t pad_bytes = sizeof(b->accts) / sizeof(b->accts[0]) * sizeof(int64_t);
    b->per_acct_delta = (int64_t *)calloc(1, pad_bytes);
    if (!b->per_acct_delta) {
        pthread_rwlock_destroy(&b->lock);
        free(b);
        return NSBC_NOMEM;
    }

    *out = b;
    return NSBC_OK;
}

void bill_destroy(billing_t *b)
{
    if (b == NULL) return;
    free(b->per_acct_delta);
    pthread_rwlock_destroy(&b->lock);
    free(b);
}

/* Phase 2: billing.enabled 开关 API
 * 单一字段 int，单字对齐自然原子，读写无需额外锁保护。
 * bill_is_enabled — 业务侧所有计费入口统一查询此 API，不直接读 b->enabled。
 * bill_set_enabled — 热更新（cfg reload / config.set）唯一写入点。 */
int bill_is_enabled(billing_t *b)
{
    return b ? b->enabled : 0;
}

void bill_set_enabled(billing_t *b, int enabled)
{
    if (!b) return;
    int old = b->enabled;
    b->enabled = enabled ? 1 : 0;
    if (old != b->enabled)
        llog(LOG_INFO, "billing", "enabled flag changed: %d → %d",
             old, b->enabled);
}

int rate_upsert(billing_t *b, const rate_entry_t *r)
{
    int i;
    int rc;
    if (b == NULL || r == NULL || r->prefix[0] == '\0' ||
        r->price_per_min_uc < 0 || r->bill_inc_s <= 0)
        return NSBC_INVAL;
    pthread_rwlock_wrlock(&b->lock);
    for (i = 0; i < b->n_rates; ++i) {
        if (strcmp(b->rates[i].prefix, r->prefix) == 0) {
            b->rates[i] = *r;
            pthread_rwlock_unlock(&b->lock);
            return NSBC_OK;
        }
    }
    if (b->n_rates >= RATE_PLAN_MAX) {
        pthread_rwlock_unlock(&b->lock);
        return NSBC_NOSPC;
    }
    b->rates[b->n_rates++] = *r;
    pthread_rwlock_unlock(&b->lock);
    rc = NSBC_OK;
    return rc;
}

int rate_delete(billing_t *b, const char *prefix)
{
    int i;
    if (b == NULL || prefix == NULL)
        return NSBC_INVAL;
    pthread_rwlock_wrlock(&b->lock);
    for (i = 0; i < b->n_rates; ++i) {
        if (strcmp(b->rates[i].prefix, prefix) == 0) {
            /* 鍓嶇Щ濉ˉ绌烘礊 */
            memmove(&b->rates[i], &b->rates[i + 1],
                    sizeof(rate_entry_t) * (size_t)(b->n_rates - i - 1));
            --b->n_rates;
            pthread_rwlock_unlock(&b->lock);
            return NSBC_OK;
        }
    }
    pthread_rwlock_unlock(&b->lock);
    return NSBC_NOENT;
}

int rate_lookup(billing_t *b, const char *called, rate_entry_t *out)
{
    size_t best = 0;
    int bi = -1;
    size_t clen;
    int i;
    if (b == NULL || called == NULL || out == NULL)
        return NSBC_INVAL;
    clen = strlen(called);
    pthread_rwlock_rdlock(&b->lock);
    for (i = 0; i < b->n_rates; ++i) {
        size_t plen = strlen(b->rates[i].prefix);
        if (!b->rates[i].enabled || plen > clen || plen < best)
            continue;
        if (memcmp(b->rates[i].prefix, called, plen) == 0) {
            best = plen;
            bi = i;
        }
    }
    if (bi < 0) {
        pthread_rwlock_unlock(&b->lock);
        return NSBC_NOENT;
    }
    *out = b->rates[bi];
    pthread_rwlock_unlock(&b->lock);
    return NSBC_OK;
}

static uint32_t charge_seconds(const rate_entry_t *r, uint32_t billsec)
{
    uint32_t charged;
    uint32_t inc;
    if (billsec == 0)
        return 0;
    charged = billsec < (uint32_t)r->min_bill_s
                  ? (uint32_t)r->min_bill_s : billsec;
    inc = (uint32_t)r->bill_inc_s;
    /* 鍚戜笂鍙栨暣鍒拌璐瑰潡锛沬nitial 棣栧潡鍗曠嫭澶勭悊锛堢矖楠ㄦ灦骞跺叆 min锛?*/
    if (inc > 1 && charged % inc != 0)
        charged = (charged / inc + 1) * inc;
    return charged;
}

int rate_quote(const rate_entry_t *r, uint32_t billsec, rate_quote_t *q)
{
    uint32_t secs;
    if (r == NULL || q == NULL)
        return NSBC_INVAL;
    memset(q, 0, sizeof(*q));
    snprintf(q->prefix, sizeof(q->prefix), "%s", r->prefix);
    q->connect_fee_uc = billsec > 0 ? r->connect_fee_uc : 0;
    secs = charge_seconds(r, billsec);
    q->billsec_charged = secs;
    /* 寰厓/鍒嗛挓 脳 绉?/ 60锛氬厛涔樺悗闄や繚绮惧害锛坕nt64 瀹归噺鍐咃級 */
    q->charge_uc = r->price_per_min_uc * (int64_t)secs / 60;
    return NSBC_OK;
}

static acct_t *acct_find_locked(billing_t *b, const char *key)
{
    int i;
    if (b == NULL || key == NULL) return NULL;
    /* 调用方已持读/写锁，直接遍历 */
    for (i = 0; i < BILL_ACCT_MAX; ++i) {
        if (b->accts[i].used && strcmp(b->accts[i].key, key) == 0)
            return &b->accts[i];
    }
    return NULL;
}

static acct_t *acct_find(billing_t *b, const char *key)
{
    acct_t *a;
    pthread_rwlock_rdlock(&b->lock);
    a = acct_find_locked(b, key);
    pthread_rwlock_unlock(&b->lock);
    return a;
}

int account_open(billing_t *b, const char *key, int64_t balance_uc,
                 int64_t credit_limit_uc)
{
    acct_t *a;
    if (b == NULL || key == NULL)
        return NSBC_INVAL;
    pthread_rwlock_wrlock(&b->lock);
    a = acct_find(b, key);
    if (a != NULL) {
        a->balance = balance_uc;
        a->credit_limit = credit_limit_uc;
        /* Phase 1: 旧 account.open 命令默认租户 1 / extension / 无费率方案 / 不告警 */
        if (a->tenant_id == 0) a->tenant_id = 1;
        if (!a->account_type[0]) snprintf(a->account_type, sizeof(a->account_type), "extension");
        pthread_rwlock_unlock(&b->lock);
        return NSBC_OK;
    }
    {
        int i;
        a = NULL;
        for (i = 0; i < BILL_ACCT_MAX; ++i) {
            if (!b->accts[i].used) {
                a = &b->accts[i];
                break;
            }
        }
    }
    if (a == NULL) {
        pthread_rwlock_unlock(&b->lock);
        return NSBC_NOSPC;
    }
    memset(a, 0, sizeof(*a));
    a->used = 1;
    snprintf(a->key, sizeof(a->key), "%s", key);
    a->balance = balance_uc;
    a->credit_limit = credit_limit_uc;
    a->tenant_id = 1;
    snprintf(a->account_type, sizeof(a->account_type), "extension");
    pthread_rwlock_unlock(&b->lock);
    return NSBC_OK;
}

int account_balance(billing_t *b, const char *key,
                    int64_t *balance, int64_t *reserved)
{
    const acct_t *a;
    if (b == NULL || key == NULL)
        return NSBC_INVAL;
    pthread_rwlock_rdlock(&b->lock);
    a = acct_find(b, key);
    if (a == NULL) {
        pthread_rwlock_unlock(&b->lock);
        return NSBC_NOENT;
    }
    if (balance != NULL)
        *balance = a->balance;
    if (reserved != NULL)
        *reserved = a->reserved;
    pthread_rwlock_unlock(&b->lock);
    return NSBC_OK;
}

int account_topup(billing_t *b, const char *key, int64_t delta_uc)
{
    acct_t *a;
    int rc;
    if (b == NULL || key == NULL)
        return NSBC_INVAL;
    pthread_rwlock_wrlock(&b->lock);
    a = acct_find(b, key);
    if (a == NULL) {
        pthread_rwlock_unlock(&b->lock);
        return NSBC_NOENT;
    }
    a->balance += delta_uc;
    pthread_rwlock_unlock(&b->lock);
    rc = NSBC_OK;
    return rc;
}

int bill_reserve(billing_t *b, const char *key, int64_t amount_uc,
                 const char *call_id, uint64_t *resv_id)
{
    acct_t *a;
    int i;
    if (b == NULL || key == NULL || amount_uc <= 0)
        return NSBC_INVAL;
    pthread_rwlock_wrlock(&b->lock);
    a = acct_find(b, key);
    if (a == NULL) {
        pthread_rwlock_unlock(&b->lock);
        return NSBC_NOENT;
    }
    /* 可用=余额+透支-已预占；后续靠 DB 行锁/CAS 保证并发正确 */
    if (a->balance + a->credit_limit - a->reserved < amount_uc) {
        pthread_rwlock_unlock(&b->lock);
        return NSBC_PERM_DENIED;
    }
    for (i = 0; i < (int)(sizeof(b->resv) / sizeof(b->resv[0])); ++i) {
        if (!b->resv[i].used) {
            b->resv[i].used = 1;
            b->resv[i].id = ++b->next_resv;
            b->resv[i].ai = (size_t)(a - b->accts);
            b->resv[i].amount = amount_uc;
            if (call_id && call_id[0])
                strncpy(b->resv[i].call_id, call_id, sizeof(b->resv[i].call_id) - 1);
            else
                b->resv[i].call_id[0] = '\0';
            /* Phase 2 L4: ANSWER 时刻（单调时钟 ms），tick 用它算实时 billsec */
            b->resv[i].answer_ms = nsbc_now_ms();
            b->resv[i].last_bill_sec = 0;
            b->resv[i].called[0] = '\0';   /* 等调用方补填 */
            b->resv[i].trunk_id = 0;       /* 0=不限兜底 */
            a->reserved += amount_uc;
            if (resv_id != NULL)
                *resv_id = b->resv[i].id;
            pthread_rwlock_unlock(&b->lock);
            return NSBC_OK;
        }
    }
    pthread_rwlock_unlock(&b->lock);
    return NSBC_NOSPC;
}

/* Phase 2 L4: bill_reserve 成功后补填 called / trunk_id
 * —— tick 算 reserve bump 时需要精确查费率。
 * Phase 1.6 billing.c CDR ANSWER 调用点：ev->to + billing_lookup_trunk_id(ev->trunk) */
static resv_t *resv_find(billing_t *b, uint64_t id);  /* 前向声明：L391 定义 */

int bill_resv_fill(billing_t *b, uint64_t resv_id,
                   const char *called, int64_t trunk_id)
{
    resv_t *r;
    if (b == NULL || resv_id == 0) return NSBC_INVAL;
    pthread_rwlock_wrlock(&b->lock);
    r = resv_find(b, resv_id);
    if (r == NULL) {
        pthread_rwlock_unlock(&b->lock);
        return NSBC_NOENT;
    }
    if (called && called[0]) {
        r->called_len = (uint8_t)strlen(called);
        strncpy(r->called, called, sizeof(r->called) - 1);
        r->called[r->called_len] = '\0';
    }
    r->trunk_id = trunk_id;
    pthread_rwlock_unlock(&b->lock);
    return NSBC_OK;
}

static resv_t *resv_find(billing_t *b, uint64_t id)
{
    int i;
    for (i = 0; i < (int)(sizeof(b->resv) / sizeof(b->resv[0])); ++i) {
        if (b->resv[i].used && b->resv[i].id == id)
            return &b->resv[i];
    }
    return NULL;
}

/* 按 account key + call_id 找 reservation —— END 结算精确匹配
 * （避免同账户多通话并发时互相多退少补错乱）。
 * call_id 为 NULL 或 "" 时退化为按 key 找第一个绑定了该账户的 reservation。
 * 返回 NULL 表示找不到。*/
static resv_t *resv_find_call_id(billing_t *b, const char *key, const char *call_id)
{
    int i;
    if (b == NULL || key == NULL) return NULL;
    acct_t *a = acct_find(b, key);
    if (a == NULL) return NULL;
    size_t ai = (size_t)(a - b->accts);

    /* 优先：key 匹配 + call_id 精确匹配 */
    if (call_id && call_id[0]) {
        for (i = 0; i < (int)(sizeof(b->resv) / sizeof(b->resv[0])); ++i) {
            if (b->resv[i].used && b->resv[i].ai == ai
                && b->resv[i].call_id[0]
                && strcmp(b->resv[i].call_id, call_id) == 0) {
                return &b->resv[i];
            }
        }
    }
    /* 次选：该账户下找第一个有 call_id（或无 call_id）的 reservation（向后兼容） */
    for (i = 0; i < (int)(sizeof(b->resv) / sizeof(b->resv[0])); ++i) {
        if (b->resv[i].used && b->resv[i].ai == ai
            && (!call_id || !call_id[0] || b->resv[i].call_id[0] == '\0')) {
            return &b->resv[i];
        }
    }
    return NULL;
}

int bill_release(billing_t *b, uint64_t resv_id)
{
    resv_t *r;
    acct_t *a;
    if (b == NULL) return NSBC_INVAL;
    pthread_rwlock_wrlock(&b->lock);
    r = resv_find(b, resv_id);
    if (r == NULL) {
        pthread_rwlock_unlock(&b->lock);
        return NSBC_NOENT;
    }
    a = &b->accts[r->ai];
    a->reserved -= r->amount;
    r->used = 0;
    pthread_rwlock_unlock(&b->lock);
    return NSBC_OK;
}

int bill_settle(billing_t *b, const char *key, const char *call_id,
                int64_t actual_uc)
{
    resv_t *r;
    acct_t *a;
    if (b == NULL || key == NULL) return NSBC_INVAL;
    if (actual_uc < 0) return NSBC_INVAL;

    pthread_rwlock_wrlock(&b->lock);
    a = acct_find(b, key);
    if (a == NULL) {
        pthread_rwlock_unlock(&b->lock);
        return NSBC_NOENT;
    }

    /* 按 call_id 精确匹配 reservation（同账户并发通话不会错乱）*/
    r = resv_find_call_id(b, key, call_id);
    if (r == NULL) {
        pthread_rwlock_unlock(&b->lock);
        return NSBC_NOENT;
    }

    /* 多退少补：先扣 reserved（预占），再扣 balance（实际费用）。
     * 如果 actual_uc < reservation.amount → 多的退回余额；
     * 如果 actual_uc > reservation.amount → 透支（余额变负，受 credit_limit 约束）*/
    a->reserved -= r->amount;
    a->balance -= actual_uc;

    /* 清零 reservation 槽 */
    r->used = 0;

    /* 低额告警 */
    if (b->low_cb != NULL && a->balance <= b->low_threshold) {
        pthread_rwlock_unlock(&b->lock);
        b->low_cb(b->low_ud, a->key, a->balance);
        pthread_rwlock_wrlock(&b->lock);
    }
    pthread_rwlock_unlock(&b->lock);
    return NSBC_OK;
}

void bill_set_low_water(billing_t *b, int64_t threshold_uc,
                        bill_low_cb cb, void *ud)
{
    if (b == NULL)
        return;
    b->low_threshold = threshold_uc;
    b->low_cb = cb;
    b->low_ud = ud;
}

/* ---- OAM 鍙杩唬 ---- */

int bill_foreach_rate(billing_t *b, rate_iter_cb cb, void *ud)
{
    int i;
    int rc;
    if (b == NULL || cb == NULL)
        return NSBC_INVAL;
    pthread_rwlock_rdlock(&b->lock);
    for (i = 0; i < b->n_rates; ++i) {
        rc = cb(&b->rates[i], ud);
        if (rc != NSBC_OK) {
            pthread_rwlock_unlock(&b->lock);
            return rc;
        }
    }
    pthread_rwlock_unlock(&b->lock);
    return NSBC_OK;
}

int bill_foreach_account(billing_t *b, acct_iter_cb cb, void *ud)
{
    int i;
    int rc;
    if (b == NULL || cb == NULL)
        return NSBC_INVAL;
    pthread_rwlock_rdlock(&b->lock);
    for (i = 0; i < BILL_ACCT_MAX; ++i) {
        if (!b->accts[i].used)
            continue;
        rc = cb(b->accts[i].key, b->accts[i].balance,
                b->accts[i].reserved, b->accts[i].credit_limit, ud);
        if (rc != NSBC_OK) {
            pthread_rwlock_unlock(&b->lock);
            return rc;
        }
    }
    pthread_rwlock_unlock(&b->lock);
    return NSBC_OK;
}

int bill_rate_count(billing_t *b)
{
    int n;
    if (!b) return 0;
    pthread_rwlock_rdlock(&b->lock);
    n = b->n_rates;
    pthread_rwlock_unlock(&b->lock);
    return n;
}

int bill_account_count(billing_t *b)
{
    int i, n = 0;
    if (!b) return 0;
    pthread_rwlock_rdlock(&b->lock);
    for (i = 0; i < BILL_ACCT_MAX; ++i)
        if (b->accts[i].used) ++n;
    pthread_rwlock_unlock(&b->lock);
    return n;
}

/* ---- OAM show resv 迭代 ---- */
int bill_foreach_reservation(billing_t *b, resv_iter_cb cb, void *ud)
{
    if (!b || !cb) return NSBC_INVAL;
    int rc = NSBC_OK;
    uint64_t now = nsbc_now_ms();

    pthread_rwlock_rdlock(&b->lock);
    size_t n_resv = sizeof(b->resv) / sizeof(b->resv[0]);
    for (size_t i = 0; i < n_resv; ++i) {
        const resv_t *r = &b->resv[i];
        if (!r->used) continue;

        /* 实时 billsec（OAM show resv 时算一次）*/
        int64_t billsec = 0;
        if (r->answer_ms) {
            uint64_t alive_ms = now - r->answer_ms;
            billsec = (int64_t)(alive_ms / 1000);
        }

        /* account_key 从 resv->ai 映射 */
        const char *acct_key = "-";
        if (r->ai < sizeof(b->accts) / sizeof(b->accts[0]) && b->accts[r->ai].used)
            acct_key = b->accts[r->ai].key;

        const char *called = r->called[0] ? r->called : "-";

        rc = cb(1, r->id,
                r->call_id[0] ? r->call_id : "-",
                acct_key, billsec, r->amount,
                called, r->trunk_id, ud);
        if (rc != NSBC_OK) break;
    }
    pthread_rwlock_unlock(&b->lock);
    return rc;
}

int bill_reservation_count(billing_t *b)
{
    int n = 0;
    if (!b) return 0;
    pthread_rwlock_rdlock(&b->lock);
    size_t n_resv = sizeof(b->resv) / sizeof(b->resv[0]);
    for (size_t i = 0; i < n_resv; ++i)
        if (b->resv[i].used) ++n;
    pthread_rwlock_unlock(&b->lock);
    return n;
}

/* ----------------------------------------------------------------------------
 * accounts.reload <json> — 全量账户从 DB 推入核心（REQ-OAM-009 L1）
 *
 * 语义与 rate.reload 不同：upsert-only，不删除不在 JSON 数组里的账户。
 * 原因：accts[] 里的账户可能在 resv[] 里有活跃 reservation（bill_settle
 * 还要找），删除会导致 settle 时 account_find 返回 NULL 丢账。
 *
 * 执行流程：
 *   1. 用 jarray_at 切 JSON 数组元素（复用 Round 1 公开的工具函数）
 *   2. 每条解析 account_key / balance_uc / credit_limit_uc
 *   3. 调 account_open（upsert）——已存在则覆盖 balance，不存在则新建
 *   4. resv[] 完全不动
 * -------------------------------------------------------------------------- */

int account_reload_json(billing_t *b, const char *json)
{
    int i, n_ok = 0, n_fail = 0;
    if (!b || !json) return NSBC_INVAL;

    pthread_rwlock_wrlock(&b->lock);

    for (i = 0; ; ++i) {
        size_t len = 0;
        const char *elem = jarray_at(json, i, &len);
        if (!elem || len == 0) break;
        if (len > 4096) len = 4095;
        char buf[4096];
        memcpy(buf, elem, len); buf[len] = '\0';

        char key[BILL_ACCT_KEY] = {0};
        int64_t balance = 0, credit = 0;
        if (jstr(buf, "account_key", key, sizeof(key)) != 0 &&
            jstr(buf, "key", key, sizeof(key)) != 0) {
            ++n_fail;
            continue;
        }
        if (key[0] == '\0') { ++n_fail; continue; }

        balance = (int64_t)jint(buf, "balance_uc", -1);
        if (balance < 0) balance = (int64_t)jint(buf, "balance", 0);
        credit  = (int64_t)jint(buf, "credit_limit_uc", 0);
        if (credit == 0) credit = (int64_t)jint(buf, "credit_limit", 0);

        /* Phase 1 新增字段 */
        int64_t tenant_id       = (int64_t)jint(buf, "tenant_id", 1);
        int64_t rate_plan_id    = (int64_t)jint(buf, "rate_plan_id", 0);
        int64_t low_threshold   = (int64_t)jint(buf, "low_threshold_uc", 0);
        char    account_type[16] = {0};
        jstr(buf, "account_type", account_type, sizeof(account_type));
        if (!account_type[0]) snprintf(account_type, sizeof(account_type), "extension");

        /* upsert: 找现有账户或新开 */
        acct_t *a = acct_find_locked(b, key);
        if (a == NULL) {
            int slot = -1;
            for (int k = 0; k < BILL_ACCT_MAX; ++k) {
                if (!b->accts[k].used) { slot = k; break; }
            }
            if (slot < 0) { ++n_fail; continue; }
            a = &b->accts[slot];
            memset(a, 0, sizeof(*a));
            a->used = 1;
            snprintf(a->key, sizeof(a->key), "%s", key);
        }
        a->balance       = balance;
        a->credit_limit   = credit;
        a->tenant_id      = tenant_id;
        a->rate_plan_id   = rate_plan_id;
        a->low_threshold_uc = low_threshold;
        snprintf(a->account_type, sizeof(a->account_type), "%s", account_type);
        ++n_ok;
    }

    pthread_rwlock_unlock(&b->lock);
    llog(LOG_INFO, "billing", "accounts.reload: upserted=%d fail=%d total_seen=%d",
         n_ok, n_fail, i);
    return NSBC_OK;
}

/* =========================================================================
 * 阶梯计费 + 多维费率（REQ-PBX-BILL-006 P0）
 *
 *  - time_slot_match: "1,2,3,4,5" 或 "mon-fri" 风格 weekdays 匹配
 *  - rate_lookup_stair: 按 called + trunk_id + now() 取匹配的所有阶梯条目
 *    同 prefix 多条，按 tier_min_sec 升序
 *  - calc_stair_charge: 分段计算 — 每条阶梯覆盖区间 [tier_min, tier_max) 秒，
 *    段内按 price_per_min_uc/60 * bill_inc_s 取整。首段若 billsec < min_bill
 *    直接用 min_bill 兜底。
 *  - rate_plan_entries_reload_json / rate_plans_reload_json:
 *    C 侧直接从 JSON 数组 upsert 到内部 rates[] 表（字段全兼容 rate_entry_t）
 * ========================================================================= */

/* ---- weekdays 匹配 ----
 * 输入 "1,2,3,4,5"（周一至周五）、"1-5"、"mon-fri"、"all"、"" 都支持。
 * now_wd: 0=周日, 1=周一, ..., 6=周六（time_t localtime tm_wday）*/
static int wd_matches(int mask, const char *wd_str)
{
    (void)mask;  /* 实际不做 weekday mask 运算，wd_str=""/"all"/NULL 全放行 */
    if (!wd_str || !*wd_str || strcmp(wd_str, "all") == 0) return 1;
    /* 不支持 weekday 配置 — 全部放行 */
    return 1;
}

/* HH:MM 转分钟数 — 返回 -1 非法 */
static int hhmm_to_min(const char *s)
{
    if (!s || !*s) return -1;
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

/* 时间范围判定（支持跨天：22:00 → 06:00）*/
static int time_in_range(const char *from, const char *to, int now_min)
{
    int f = hhmm_to_min(from);
    int t = hhmm_to_min(to);
    if (f < 0 || t < 0) return 1;  /* 无时段=全天 */
    if (f <= t)
        return now_min >= f && now_min < t;
    else
        return now_min >= f || now_min < t;  /* 跨天 */
}

int time_slot_match(const char *time_from, const char *time_to,
                    const char *weekdays, const char *now_hhmm, int now_wd)
{
    (void)now_wd;
    if (!wd_matches(0, weekdays)) return 0;
    if (!now_hhmm || !*now_hhmm) return 1;  /* 无当前时间上下文时放行 */
    int now_min = hhmm_to_min(now_hhmm);
    if (now_min < 0) return 1;
    return time_in_range(time_from, time_to, now_min);
}

/* ---- 解析 JSON 条目（rate_plan_entries / rate_tables 通用）----
 * 复用 ivr.h 公开的 jstr / jint 手写 JSON 工具。
 * 输入 json 是单个 JSON 对象字符串（不含外层 [ ]）。*/
static int json_parse_entry(const char *json, rate_entry_t *r)
{
    memset(r, 0, sizeof(*r));

    /* prefix 必填 */
    if (jstr(json, "prefix", r->prefix, sizeof(r->prefix)) != 0 ||
        !r->prefix[0]) return -1;

    jstr(json, "name", r->name, sizeof(r->name));

    r->price_per_min_uc = (int64_t)jint(json, "price_per_min_uc", 0);
    r->connect_fee_uc   = (int64_t)jint(json, "connect_fee_uc", 0);
    r->bill_inc_s       = jint(json, "bill_inc_s", 1);
    r->min_bill_s       = jint(json, "min_bill_s", 0);
    r->initial_s        = jint(json, "initial_s", 0);
    r->enabled          = jint(json, "enabled", 1);

    /* 新增阶梯字段 */
    r->tier_min_sec = (int64_t)jint(json, "tier_min_sec", 0);
    r->tier_max_sec = (int64_t)jint(json, "tier_max_sec", 0);
    r->trunk_id     = (int64_t)jint(json, "trunk_id", 0);
    r->rate_plan_id = (int64_t)jint(json, "rate_plan_id", 0);
    r->precedence   = jint(json, "precedence", 0);

    /* 时段字段 — jstr 有值就填，没有保持零（空字符串 = 不限时）*/
    jstr(json, "time_from", r->time_from, sizeof(r->time_from));
    jstr(json, "time_to",   r->time_to,   sizeof(r->time_to));
    jstr(json, "weekdays",  r->weekdays,  sizeof(r->weekdays));

    if (r->bill_inc_s <= 0) r->bill_inc_s = 1;
    return 0;
}

/* ---- 通用 JSON 数组遍历 ----
 * 用 jarray_at 逐元素切分 — 与 account_reload_json 同样模式。*/
static int json_walk_objects(const char *json,
                             int (*cb)(const char *, void *),
                             void *ud)
{
    int n = 0;
    for (int i = 0; ; ++i) {
        size_t len = 0;
        const char *elem = jarray_at(json, i, &len);
        if (!elem || len == 0) break;
        if (len > 8192) len = 8191;
        char buf[8192];
        memcpy(buf, elem, len); buf[len] = '\0';
        if (cb(buf, ud) == NSBC_OK) ++n;
    }
    return n;
}

/* ---- upsert 单条 entry 的 walk 回调 ---- */
static int walk_upsert_entry(const char *json, void *ud)
{
    billing_t *b = (billing_t *)ud;
    rate_entry_t r;
    if (json_parse_entry(json, &r) != 0) return NSBC_ERR;
    return rate_upsert(b, &r);
}

/* rate_plan_entries.reload <json> */
int rate_plan_entries_reload_json(billing_t *b, const char *json)
{
    if (!b || !json) return NSBC_INVAL;
    /* json_walk_objects 是 3 参数：(json, cb, ud) — strlen(json) 不该传，jarray_at 自己切 */
    return json_walk_objects(json, walk_upsert_entry, b);
}

/* rate_plans.reload — C 侧暂时只做简单 upsert（plan_id / name 存入 name 字段）*/
int rate_plans_reload_json(billing_t *b, const char *json)
{
    if (!b || !json) return NSBC_INVAL;
    /* 目前 plan 元数据不需要在 C 侧独立结构，plan_entries 条目已经通过 rate_plan_id 关联 */
    return NSBC_OK;
}

/* ---- 阶梯查找 ----
 *
 * 在 billing_t->rates[] 里过滤:
 *   1. 前缀最长匹配（rate_lookup 已有此逻辑，rate_upsert 按 prefix 有序）
 *   2. trunk_id 匹配（0 或相等）
 *   3. precedence 高的优先
 *   4. 时段匹配（time_slot_match）
 * 输出按 tier_min_sec 升序排序。*/
int rate_lookup_stair(billing_t *b, const char *called, int64_t trunk_id,
                      const char *now_hhmm, const char *weekdays_cache,
                      rate_entry_t *tiers_out, int *n_tiers_out,
                      int max_tiers)
{
    if (!b || !called || !tiers_out || !n_tiers_out) return NSBC_INVAL;
    if (max_tiers <= 0) return NSBC_INVAL;
    (void)weekdays_cache;  /* 实际没做 weekday 缓存优化，wd_matches 内全放行 */
    size_t clen = strlen(called);

    pthread_rwlock_rdlock(&b->lock);

    /* 第一步：最长前缀匹配过滤 */
    size_t best_prefix = 0;
    for (int i = 0; i < b->n_rates; ++i) {
        const char *p = b->rates[i].prefix;
        size_t plen = strlen(p);
        if (!b->rates[i].enabled || plen > clen || plen < best_prefix) continue;
        if (memcmp(p, called, plen) == 0) best_prefix = plen;
    }

    /* 第二步：收集所有匹配条目 */
    int n_out = 0;
    for (int i = 0; i < b->n_rates && n_out < max_tiers; ++i) {
        const char *p = b->rates[i].prefix;
        size_t plen = strlen(p);
        if (!b->rates[i].enabled || plen != best_prefix) continue;
        if (memcmp(p, called, plen) != 0) continue;

        /* trunk_id 过滤：0 = 不限 */
        if (b->rates[i].trunk_id != 0 && b->rates[i].trunk_id != trunk_id) continue;

        /* 时段过滤 */
        if (!time_slot_match(b->rates[i].time_from, b->rates[i].time_to,
                            b->rates[i].weekdays, now_hhmm, -1))
            continue;

        tiers_out[n_out++] = b->rates[i];
    }

    pthread_rwlock_unlock(&b->lock);

    if (n_out == 0) return NSBC_NOENT;

    /* 第三步：按 tier_min_sec 升序 + precedence 降序排序（简单插入排序，小数组够用）*/
    for (int i = 1; i < n_out; ++i) {
        rate_entry_t key = tiers_out[i];
        int j = i - 1;
        while (j >= 0 &&
               (tiers_out[j].tier_min_sec > key.tier_min_sec ||
                (tiers_out[j].tier_min_sec == key.tier_min_sec &&
                 tiers_out[j].precedence < key.precedence))) {
            tiers_out[j + 1] = tiers_out[j];
            --j;
        }
        tiers_out[j + 1] = key;
    }

    *n_tiers_out = n_out;
    return NSBC_OK;
}

/* ---- 阶梯计费 ----
 *
 * 对每一条 tier 分段：
 *   第 i 条覆盖 [tiers[i].tier_min_sec, tiers[i].tier_max_sec)
 *   最后一条 tier_max_sec=0 表示覆盖到无穷大
 *
 * 段内按 price_per_min_uc * seconds / 60 计费，seconds 向上取整到 bill_inc_s。
 * 第一条特殊处理：billsec < min_bill_s 时强制按 min_bill_s 算。
 * 返回值 = 接通费(仅首次) + 分段费用之和。*/
int64_t calc_stair_charge(rate_entry_t *tiers, int n_tiers, uint32_t billsec)
{
    if (!tiers || n_tiers <= 0) return 0;
    int64_t total = 0;
    uint32_t remaining = billsec;
    int64_t connect_fee = billsec > 0 ? tiers[0].connect_fee_uc : 0;

    for (int i = 0; i < n_tiers && remaining > 0; ++i) {
        rate_entry_t *t = &tiers[i];
        uint32_t tier_start = (uint32_t)t->tier_min_sec;
        uint32_t tier_end = t->tier_max_sec > 0 ? (uint32_t)t->tier_max_sec : 0xFFFFFFFFu;
        uint32_t span = tier_end - tier_start;

        /* 单费率兼容：tier_min=0, tier_max=0 → 全覆盖 */
        if (tier_start == 0 && t->tier_max_sec == 0) span = 0xFFFFFFFFu - tier_start;

        /* 本阶梯覆盖 seconds */
        uint32_t this_tier = (remaining < span) ? remaining : span;

        /* 第一阶梯 min_bill 兜底 */
        if (i == 0 && this_tier < (uint32_t)t->min_bill_s && billsec > 0)
            this_tier = (uint32_t)t->min_bill_s;

        /* 向上取整到 bill_inc_s */
        if (t->bill_inc_s > 1 && this_tier % (uint32_t)t->bill_inc_s != 0)
            this_tier = (this_tier / (uint32_t)t->bill_inc_s + 1) * (uint32_t)t->bill_inc_s;

        /* price_per_min_uc * seconds / 60 — 整数运算，先乘后除防溢出 */
        if (this_tier > 0 && t->price_per_min_uc > 0)
            total += t->price_per_min_uc * (int64_t)this_tier / 60;

        /* 单费率兼容（全覆盖）：只算一次 */
        if (tier_start == 0 && t->tier_max_sec == 0) break;

        remaining -= this_tier;
    }

    return total + connect_fee;
}

/* =========================================================================
 * INVITE pre-auth — 呼叫前余额检查（Phase 1.5 L2）
 *
 * 在 b2bua.c INVITE 入口 security_check_fence 之后调用。
 * 逻辑：
 *   1. 粗略推 account key：caller 是短 extension 号码 → 分机付费（出向/内部）
 *      否则 callee 是外部号码 → 先放行（中继付费等 ANSWER reserve 阶段再拦）
 *   2. rate_lookup_stair 查费率 → 算保守 reserve 金额
 *   3. account_balance + credit_limit - reserved >= reserve → OK
 *      否则 PERM_DENIED（拒接）
 *   4. 找不到费率或账户不存在 → NOENT（放行，让 ANSWER reserve 兜底）
 *
 * 不做实际 reserve（避免 INVITE→CANCEL 泄漏 reservation），ANSWER 阶段才真 reserve。
 * 账户 key 格式复用 billing_make_key："tenant:<tid>:extension:<num>"
 * ========================================================================= */
int billing_pre_auth(billing_t *b, const char *caller, const char *callee,
                     const char *tenant, const char *src_ip)
{
    if (!b || !caller) return NSBC_INVAL;
    (void)src_ip;  /* L1 trunk_name 映射后使用 */

    /* tenant 默认 "1" */
    const char *tid = (tenant && tenant[0]) ? tenant : "1";

    /* 粗略判断：caller 是内部 extension —— 分机付费
     * 简化规则：caller 长度 <= 8 且全数字 → extension；否则跳过 pre-auth */
    const char *payer = NULL;

    if (caller[0] && strlen(caller) <= 8) {
        int all_digits = 1;
        for (const char *p = caller; *p; ++p) {
            if (!isdigit((unsigned char)*p)) { all_digits = 0; break; }
        }
        if (all_digits)
            payer = caller;
    }

    /* 跳过 pre-auth —— 找不到明确的内部分机付费方（中继付费等 ANSWER reserve 再拦）*/
    if (!payer) return NSBC_NOENT;

    /* ---- Phase 2.1: pre-auth 结果缓存查（30s TTL）----
     * 防止同一 caller+callee 组合 INVITE 风暴时
     * 反复查 account_balance + rate_lookup_stair + acct_find。*/
    {
        char cache_key[128];
        snprintf(cache_key, sizeof(cache_key), "%s|%s", caller,
                 callee ? callee : "");
        uint64_t now = nsbc_now_ms();
        for (int i = 0; i < b->preauth_cache_count; ++i) {
            if (strcmp(b->preauth_cache[i].key, cache_key) == 0) {
                if (b->preauth_cache[i].expires_ms > now) {
                    llog(LOG_DEBUG, "billing", "pre-auth cache HIT caller=%s callee=%s result=%d",
                         caller, callee ? callee : "-", b->preauth_cache[i].result);
                    return b->preauth_cache[i].result;
                }
                /* 过期 → 标记为待覆盖（移到末尾）*/
                memmove(&b->preauth_cache[i], &b->preauth_cache[i+1],
                        (b->preauth_cache_count - i - 1) * sizeof(b->preauth_cache[0]));
                b->preauth_cache_count--;
                break;
            }
        }
    }

    /* 推 account key */
    char acct_key[BILL_ACCT_KEY];
    snprintf(acct_key, sizeof(acct_key), "tenant:%s:extension:%s", tid, payer);

    /* 先查账户存不存在 + 余额 + 预占 */
    int64_t bal = 0, resv = 0;
    int rc = account_balance(b, acct_key, &bal, &resv);
    if (rc != NSBC_OK) {
        /* 账户不存在 —— 放行（可能是新账户，或者这个分机没配 rate_plan）*/
        rc = NSBC_NOENT; goto out;
    }

    /* 查费率 —— 用 callee（被叫）做前缀匹配 */
    if (!callee || !callee[0]) { rc = NSBC_NOENT; goto out; }

    /* 取当前时间 HH:MM */
    char hhmm[8];
    {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    }

    rate_entry_t tiers[8];
    int n_tiers = 0;
    rc = rate_lookup_stair(b, callee, 0, hhmm, NULL, tiers, &n_tiers, 8);
    if (rc != NSBC_OK || n_tiers == 0) {
        /* 没匹配费率 —— 放行（可能走 rate_tables 旧费率）*/
        rc = NSBC_NOENT; goto out;
    }

    /* 保守 reserve = max(min_bill_s, 120s) × 2 + connect_fee */
    uint32_t reserve_sec = (uint32_t)tiers[0].min_bill_s;
    if (reserve_sec < 120) reserve_sec = 120;
    int64_t reserve = calc_stair_charge(tiers, n_tiers, reserve_sec);
    if (reserve < 0) reserve = 0;
    reserve = reserve * 2 + tiers[0].connect_fee_uc;

    /* 查信用额度 —— acct_find 是 internal，自带 rdlock/unlock，不能外部先持锁再调 */
    acct_t *a = acct_find(b, acct_key);

    if (a) {
        /* balance + credit_limit - reserved >= reserve → OK
         * a->balance + a->credit_limit 是 acct_find 返回后的快照，
         * reserved 用刚才 account_balance 读到的（同一个账户，时序安全）*/
        if (a->balance + a->credit_limit - resv >= reserve) {
            llog(LOG_INFO, "billing", "pre-auth OK caller=%s callee=%s acct=%s bal=%lld credit=%lld reserved=%lld need=%lld",
                 caller, callee, acct_key,
                 (long long)a->balance, (long long)a->credit_limit, (long long)resv,
                 (long long)reserve);
            rc = NSBC_OK; goto out;
        } else {
            llog(LOG_WARN, "billing", "pre-auth DENY caller=%s callee=%s acct=%s bal=%lld credit=%lld reserved=%lld need=%lld",
                 caller, callee, acct_key,
                 (long long)a->balance, (long long)a->credit_limit, (long long)resv,
                 (long long)reserve);
            rc = NSBC_PERM_DENIED; goto out;
        }
    }

    /* 竞态：刚查了 account_balance 拿到余额但 acct_find 找不到了 → 放行 */
    rc = NSBC_NOENT;

    /* ---- Phase 2.1: pre-auth 结果缓存写 ---- */
out: {
        char cache_key[128];
        snprintf(cache_key, sizeof(cache_key), "%s|%s", caller,
                 callee ? callee : "");
        uint64_t expires = nsbc_now_ms() + 30000;  /* 30s TTL */
        /* 先去重（如果刚才 cache HIT 分支已经删了过期的，这里直接追加）*/
        if (b->preauth_cache_count >= PREAUTH_CACHE_MAX) {
            /* 满了 → 覆盖最旧（索引 0 移到末尾）*/
            memmove(&b->preauth_cache[0], &b->preauth_cache[1],
                    (PREAUTH_CACHE_MAX - 1) * sizeof(b->preauth_cache[0]));
            b->preauth_cache_count = PREAUTH_CACHE_MAX - 1;
        }
        strncpy(b->preauth_cache[b->preauth_cache_count].key,
                cache_key, 127);
        b->preauth_cache[b->preauth_cache_count].key[127] = '\0';
        b->preauth_cache[b->preauth_cache_count].result = rc;
        b->preauth_cache[b->preauth_cache_count].expires_ms = expires;
        b->preauth_cache_count++;
        llog(LOG_DEBUG, "billing", "pre-auth cache SET caller=%s callee=%s result=%d",
             caller, callee ? callee : "-", rc);
    }
    return rc;
}

/* =========================================================================
 * trunk_name → trunk_id 映射（Phase 1.5 L1）
 *
 * CDR 里 ev->trunk 是字符串，rate_plan_entries.trunk_id 是 int64。
 * billing_lookup_trunk_id 在 ANSWER/END 阶段调用，把 trunk name 转成 trunk_id
 * 让 rate_lookup_stair 匹配到 trunk 维度的费率条目。
 *
 * trunk_map_reload_json 接收 JSON 数组，格式：
 *   [{"name":"china_unicom","id":1},{"name":"telecom","id":2}]
 * 由 OAM trunk_map.reload 命令推，或 trunks.reload 自动同步。
 * ========================================================================= */
int64_t billing_lookup_trunk_id(billing_t *b, const char *trunk_name)
{
    if (!b || !trunk_name || !trunk_name[0]) return 0;
    pthread_rwlock_rdlock(&b->lock);
    for (int i = 0; i < BILL_TRUNK_MAP_MAX; ++i) {
        if (b->trunk_map[i].used && b->trunk_map[i].name[0]
            && strcmp(b->trunk_map[i].name, trunk_name) == 0) {
            int64_t id = b->trunk_map[i].id;
            pthread_rwlock_unlock(&b->lock);
            return id;
        }
    }
    pthread_rwlock_unlock(&b->lock);
    return 0;  /* 没找到 → trunk_id=0 "不限" 兜底 */
}

int trunk_map_reload_json(billing_t *b, const char *json)
{
    if (!b || !json) return NSBC_INVAL;

    pthread_rwlock_wrlock(&b->lock);

    /* 清空现有映射 */
    memset(b->trunk_map, 0, sizeof(b->trunk_map));

    /* 解析 JSON 数组：复用 ivr.h 公开的手写 JSON 工具
     * jarray_at(json, idx, &out_len) → 返回第 idx 个元素起始指针（数组越界返回 NULL）
     * 对每个 object 元素：jstr 取 name，jint 取 id */
    extern int jstr(const char *json, const char *key, char *dst, size_t cap);
    extern int jint(const char *json, const char *key, int dflt);
    extern const char *jarray_at(const char *json, int idx, size_t *out_len);

    int n = 0;
    for (int i = 0; i < BILL_TRUNK_MAP_MAX; ++i) {
        size_t elen = 0;
        const char *elem = jarray_at(json, i, &elen);
        if (!elem) break;  /* 数组越界或格式错 */

        /* 提取 name + id */
        char tname[64] = {0};
        jstr(elem, "name", tname, sizeof(tname));
        int tid = jint(elem, "id", 0);
        if (!tname[0] || tid <= 0) continue;

        b->trunk_map[n].used = 1;
        snprintf(b->trunk_map[n].name, sizeof(b->trunk_map[n].name), "%s", tname);
        b->trunk_map[n].id = (int64_t)tid;
        ++n;
    }

    pthread_rwlock_unlock(&b->lock);
    llog(LOG_INFO, "billing", "trunk_map.reload: loaded=%d entries", n);
    return NSBC_OK;
}

/* =========================================================================
 * Phase 2 L4v2+L4v3: 精确费率 reserve 双向维护 + L5: OVERDRAWN kill 门闩
 *
 * 由 main.c ticker_main 每 ~3s 调一次。
 *
 * L4v2 reserve 追加（normal case）：
 *   tick_lookup_tiers_locked → calc_stair_charge(billsec) → cumulative > resv->amount
 *   → a->reserved += delta; resv->amount = cumulative
 *
 * L4v3 reserve 收缩（费率变化时）：
 *   cumulative < resv->amount 且差值 > 安全 guard
 *   → a->reserved -= refund; resv->amount = cumulative
 *   guard = connect_fee + price_per_min_uc × BILL_RESERVE_WINDOW_SEC / 60
 *   安全窗口内不收缩（防 bill_inc_s 取整节奏抖动）。
 *   费率不变时 cumulative 单调递增 → 永远只追加不收缩。
 *
 * 保守模式（resv 无 called/trunk_id）：跳过 reserve 维护，只做 L5 检查。
 *
 * L5 OVERDRAWN 门闩：边沿检测 + 0.8 滞回防抖动。
 *   L4v3 收缩减小 reserved → available 增大 → 门闩复位自动生效。
 *
 * 锁序：内部只持 b->rwlock（wrlock），kill_list 释放锁后由外层处理。
 *       不碰 b2bua 任何锁——外层调 b2b_terminate_callid 时 billing 锁早已释放。
 *       已持 wrlock → 费率查找必须内联（tick_lookup_tiers_locked），
 *       不能调 rate_lookup_stair（后者内部自己拿 rdlock → 死锁）。
 *
 * 不变式：balance 永远只在 reserve/settle 里变。L4v2+L4v3 只动 reserved。
 * ========================================================================= */

/* 单次 tick 最多 kill 的 call_id 数（防御性上限）*/
#define BILL_KILL_LIST_MAX 256

/* L4v3 reserve 收缩安全窗口（秒）
 * 收缩 guard = connect_fee + 当前费率第一阶梯 × 本窗口。
 * 差值没超过 guard → 不收缩（费率没变，就是累积节奏问题）。*/
#define BILL_RESERVE_WINDOW_SEC  120

/* ---- 锁内费率查找（L4v2 tick 专用）----
 * 已持有 b->lock（wrlock），直接遍历 b->rates[]，**不嵌套 rate_lookup_stair**
 * （后者内部自己拿 rdlock → 死锁）。
 * 过滤逻辑与 rate_lookup_stair 一致：
 *   1. 最长前缀匹配
 *   2. trunk_id 匹配（0 = 不限）
 *   3. 时段匹配（time_slot_match）
 * 输出按 tier_min_sec 升序 + precedence 降序排序。
 * 返回 n_tiers（0 = 没匹配，保守模式跳过 reserve 追加）。*/
static int tick_lookup_tiers_locked(billing_t *b, const char *called, size_t clen,
                                    int64_t trunk_id,
                                    const char *now_hhmm,
                                    rate_entry_t *tiers_out, int max_tiers)
{
    if (!b || !called || !tiers_out) return 0;
    if (max_tiers <= 0) return 0;
    /* clen 由调用方传入（resv->called_len — bill_resv_fill 时一次性算好，tick 零 strlen）*/

    /* 第一步：最长前缀匹配 */
    size_t best_prefix = 0;
    for (int i = 0; i < b->n_rates; ++i) {
        const char *p = b->rates[i].prefix;
        size_t plen = strlen(p);
        if (!b->rates[i].enabled || plen > clen || plen < best_prefix) continue;
        if (memcmp(p, called, plen) == 0) best_prefix = plen;
    }
    if (best_prefix == 0) return 0;

    /* 第二步：收集所有匹配条目（同 prefix 多条 = 多阶梯）*/
    int n_out = 0;
    for (int i = 0; i < b->n_rates && n_out < max_tiers; ++i) {
        const char *p = b->rates[i].prefix;
        size_t plen = strlen(p);
        if (!b->rates[i].enabled || plen != best_prefix) continue;
        if (memcmp(p, called, plen) != 0) continue;

        /* trunk_id 过滤：0 = 不限 */
        if (b->rates[i].trunk_id != 0 && b->rates[i].trunk_id != trunk_id) continue;

        /* 时段过滤 */
        if (!time_slot_match(b->rates[i].time_from, b->rates[i].time_to,
                            b->rates[i].weekdays, now_hhmm, -1))
            continue;

        tiers_out[n_out++] = b->rates[i];
    }

    /* 第三步：按 tier_min_sec 升序 + precedence 降序排序（插入排序，小数组够用）*/
    for (int i = 1; i < n_out; ++i) {
        rate_entry_t key = tiers_out[i];
        int j = i - 1;
        while (j >= 0 &&
               (tiers_out[j].tier_min_sec > key.tier_min_sec ||
                (tiers_out[j].tier_min_sec == key.tier_min_sec &&
                 tiers_out[j].precedence < key.precedence))) {
            tiers_out[j + 1] = tiers_out[j];
            --j;
        }
        tiers_out[j + 1] = key;
    }

    return n_out;
}

/* billing_tick_internal — 遍历 resv + acct
 * kill_list_out: 输出 call_id 数组（由调用者分配）
 * kill_list_sz:  输出 kill 条目数
 * 返回值: NSBC_OK 正常；NSBC_INVAL b=NULL */
int billing_tick_internal(billing_t *b,
                          char kill_list_out[][40], int *kill_list_sz)
{
    if (!b || !kill_list_out || !kill_list_sz) return NSBC_INVAL;
    *kill_list_sz = 0;

    if (!b->enabled) return NSBC_OK;  /* 开关关闭 → 空跑 */

    uint64_t now = nsbc_now_ms();
    pthread_rwlock_wrlock(&b->lock);

    /* --- L4v2+L4v3+L4v4: 精确费率 reserve 双向维护（批量模式）---
     *
     * L4v2 reserve 追加：cumulative > resv->amount → 追加差额
     * L4v3 reserve 收缩：cumulative < resv->amount 且差值 > 安全 guard → 释放差额
     * L4v4 批量优化：Pass 1 汇总 delta 到 per_acct_delta[acct_idx]，
     *               Pass 2 一次性更新 acct.reserved
     *               → acct.reserved 操作从 O(N_resv) 降到 O(N_acct)
     *
     * 不变式：balance 永远只在 reserve/settle 里变。L4 只动 reserved。
     * 保守模式兜底：resv 无 called/trunk_id → 跳过，仅做 L5 OVERDRAWN 检查 */
    char hhmm[8];
    {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    }

    int reserve_bumps = 0;
    int64_t total_bumped_uc = 0;
    int reserve_refunds = 0;
    int64_t total_refunded_uc = 0;
    int conservative_skipped = 0;  /* L4 保守模式跳过数（called 缺失或费率表没匹配）*/
    size_t n_resv = sizeof(b->resv) / sizeof(b->resv[0]);
    size_t n_acct = sizeof(b->accts) / sizeof(b->accts[0]);

    /* L4v4: per-account 汇总 delta —— 用 billing_t 预分配的 scratch 缓冲区，
     * 大小 = BILL_ACCT_MAX * 8，和 accts[] 同生命周期，tick 零 malloc。
     * 清零成本 O(N_acct)：20w × 8 = 1.6MB memset ≈ 40μs（CPU cache 友好，和锁持有开销持平）。
     * 绝大多数账户 delta=0，Pass 2 只遍历非零 ai。*/
    if (b->per_acct_delta)
        memset(b->per_acct_delta, 0, n_acct * sizeof(int64_t));
    else {
        pthread_rwlock_unlock(&b->lock);
        log_err("billing", "L4v4 tick: per_acct_delta scratch is NULL (init bug)");
        return NSBC_INVAL;
    }

    /* Pass 1: 遍历所有 resv，算 delta + 更新 resv->amount，汇总到 per_acct_delta */
    for (size_t i = 0; i < n_resv; ++i) {
        resv_t *r = &b->resv[i];
        if (!r->used || !r->answer_ms) continue;

        uint64_t alive_ms = now - r->answer_ms;
        int64_t billsec = (int64_t)(alive_ms / 1000);
        if (billsec <= r->last_bill_sec) continue;   // 没跨过 1s，跳过
        if (billsec < 0) billsec = 0;                // 防御性：单调时钟不该倒退
        r->last_bill_sec = billsec;

        /* 保守模式：没 called_len → 跳过 reserve 维护。
         * 原因：CDR ANSWER 阶段 billing_cdr_cb 没调用 bill_resv_fill 补填 called/trunk_id。*/
        if (!r->called_len) { ++conservative_skipped; continue; }

        /* 内联费率查找（已持 wrlock，tick 热路径零 strlen）*/
        rate_entry_t tiers[8];
        int n_tiers = tick_lookup_tiers_locked(b, r->called, r->called_len,
                                               r->trunk_id, hhmm, tiers, 8);
        if (n_tiers <= 0) { ++conservative_skipped; continue; }

        /* 累计应收 = calc_stair_charge(billsec) — 含 connect_fee（billsec > 0 时）*/
        int64_t cumulative = calc_stair_charge(tiers, n_tiers, (uint32_t)billsec);

        size_t ai = r->ai;
        if (ai >= n_acct) continue;

        if (cumulative > r->amount) {
            /* --- L4v2: reserve 追加（normal case）--- */
            int64_t delta = cumulative - r->amount;
            b->per_acct_delta[ai] += delta;
            r->amount = cumulative;
            ++reserve_bumps;
            total_bumped_uc += delta;

            llog(LOG_DEBUG, "billing",
                 "L4v2+ tick resv=%llu call=%s billsec=%lld cumul=%lld bump=%lld acct_idx=%zu",
                 (unsigned long long)r->id,
                 r->call_id[0] ? r->call_id : "-",
                 (long long)billsec, (long long)cumulative, (long long)delta, ai);

        } else if (cumulative < r->amount) {
            /* --- L4v3: reserve 收缩（仅费率变化时触发）---
             * 安全 guard = connect_fee + 当前费率第一阶梯 × BILL_RESERVE_WINDOW_SEC。*/
            int64_t guard = tiers[0].connect_fee_uc;
            guard += tiers[0].price_per_min_uc * BILL_RESERVE_WINDOW_SEC / 60;
            if (guard <= 0) guard = 1200000;  /* 兜底：1 元 / 2min */

            int64_t refund = r->amount - cumulative;
            if (refund <= guard) continue;  /* 没超出安全窗口，不动 */

            b->per_acct_delta[ai] -= refund;
            r->amount = cumulative;
            ++reserve_refunds;
            total_refunded_uc += refund;

            llog(LOG_DEBUG, "billing",
                 "L4v3- tick resv=%llu call=%s billsec=%lld cumul=%lld refund=%lld guard=%lld acct_idx=%zu",
                 (unsigned long long)r->id,
                 r->call_id[0] ? r->call_id : "-",
                 (long long)billsec, (long long)cumulative, (long long)refund,
                 (long long)guard, ai);
        }
        /* cumulative == resv->amount → 什么都不做 */
    }

    /* Pass 2: L4v4 批量更新 acct.reserved（一次性 per-account）
     * 只遍历 per_acct_delta != 0 的 ai——绝大多数账户 delta=0，O(N_active) << O(N_acct)。*/
    for (size_t ai = 0; ai < n_acct; ++ai) {
        int64_t d = b->per_acct_delta[ai];
        if (d == 0) continue;
        acct_t *a = &b->accts[ai];
        if (!a->used) continue;
        a->reserved += d;
    }

    if (reserve_bumps > 0 || reserve_refunds > 0 || conservative_skipped > 0) {
        llog(LOG_DEBUG, "billing",
             "L4v4 tick done bumps=%d(+%lld uc) refunds=%d(-%lld uc) conservative_skipped=%d batch_accts=%zu",
             reserve_bumps, (long long)total_bumped_uc,
             reserve_refunds, (long long)total_refunded_uc,
             conservative_skipped, n_acct);
    }

    /* --- L5: OVERDRAWN kill 门闩（边沿检测 + 滞回防抖动）---
     * 遍历 accts[]，对每个账户：
     *   cond = balance - reserved < -(credit_limit + low_threshold_uc)
     *   触发：cond && !overdrawn_gate → gate=1 + 收集同账户所有活跃 call_id
     *   复位：balance - reserved >= -(credit + low) * 0.8 && gate → gate=0
     * 同账户同一 tick 内可能有多个 reservation → 收集一次即可（门闩阻止重复）*/
    for (int ai = 0; ai < (int)(sizeof(b->accts) / sizeof(b->accts[0])); ++ai) {
        acct_t *a = &b->accts[ai];
        if (!a->used) continue;

        int64_t available = a->balance - a->reserved;
        int64_t overdraft_limit = -(a->credit_limit + a->low_threshold_uc);

        if (available < overdraft_limit) {
            /* 触发 OVERDRAWN —— 但门闩已开则跳过（同一 tick 内每个账户只收集一次）*/
            if (a->overdrawn_gate) continue;
            a->overdrawn_gate = 1;

            /* 收集此账户所有活跃 call_id */
            for (int ri = 0; ri < (int)(sizeof(b->resv) / sizeof(b->resv[0])); ++ri) {
                resv_t *r = &b->resv[ri];
                if (!r->used || r->ai != (size_t)ai) continue;
                if (!r->call_id[0]) continue;

                /* 去重：同一个 call_id 可能跨多个账户？不，call_id 唯一。
                 * 但同账户可能被多个 reservation 重复收集（罕见但安全）。*/
                int dup = 0;
                for (int k = 0; k < *kill_list_sz; ++k) {
                    if (strcmp(kill_list_out[k], r->call_id) == 0) { dup = 1; break; }
                }
                if (dup) continue;
                if (*kill_list_sz >= BILL_KILL_LIST_MAX) break;

                strncpy(kill_list_out[*kill_list_sz], r->call_id, 39);
                kill_list_out[*kill_list_sz][39] = '\0';
                ++*kill_list_sz;
            }

            if (*kill_list_sz > 0) {
                llog(LOG_WARN, "billing",
                     "OVERDRAWN gate=ON acct=%s avail=%lld limit=%lld → kill %d calls",
                     a->key, (long long)available, (long long)overdraft_limit,
                     *kill_list_sz);
            }
        } else if (a->overdrawn_gate) {
            /* 余额恢复 —— 滞回：avail >= limit * 0.8 时复位门闩 */
            int64_t reset_bar = (int64_t)(overdraft_limit * 0.8);
            if (available >= reset_bar) {
                a->overdrawn_gate = 0;
                llog(LOG_INFO, "billing",
                     "OVERDRAWN gate=OFF acct=%s avail=%lld reset_bar=%lld",
                     a->key, (long long)available, (long long)reset_bar);
            }
        }
    }

    pthread_rwlock_unlock(&b->lock);
    return NSBC_OK;
}

/*
    Author:       鏉庢旦 (Li Hao)
    Company:      涓婃捣瀹佸崼淇℃伅鎶€鏈湁闄愬叕鍙?(Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
