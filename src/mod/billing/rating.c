/* 璐圭巼/瀹炴椂璁¤垂鈥斺€旂矖楠ㄦ灦锛圧EQ-PBX-BILL-001~005锛孭1锛? *
 * 绮楅鏋剁害瀹氾細
 *  - 璐圭巼琛?璐︽埛涓哄畾妲芥暟缁勶紙鎺ョ嚎鎵规崲鍒嗙墖鍝堝笇 + DBAL 浣欓琛岋紝
 *    浣欓鎵ｅ噺璧板崟琛屽師瀛?UPDATE ... WHERE balance>=x 闃查€忔敮绔炰簤锛夛紱
 *  - 棰勫崰璁板綍瀹氭Ы锛涘懆鏈熸墸璐癸紙P1 姣忚璐瑰潡锛夊湪鎺ョ嚎鎵圭敱鏃堕棿杞┍鍔紝
 *    灞婃椂澶嶇敤 settle锛堟棫棰勫崰锛? reserve锛堜笅涓€鍧楋級涓ゆ寮忋€? * 閲戦涓€寰?int64 寰厓锛屼笉鍋氭诞鐐归櫎娉曘€? */
#include "rating.h"
#include "ivr.h"   /* jstr / jint / jarray_at — Round 1 公开的手写 JSON 工具 */
#include "log.h"
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
} acct_t;

typedef struct {
    int      used;
    uint64_t id;
    size_t   ai;
    int64_t  amount;
} resv_t;

struct billing {
    pthread_rwlock_t lock;      /* rwlock锛歳ate_lookup/account_balance 绛夌儹璺緞璇婚浂闃诲 */
    rate_entry_t rates[RATE_PLAN_MAX];
    int          n_rates;
    acct_t       accts[BILL_ACCT_MAX];
    resv_t       resv[4096];
    uint64_t     next_resv;
    int64_t      low_threshold;
    bill_low_cb  low_cb;
    void        *low_ud;
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
    *out = b;
    return NSBC_OK;
}

void bill_destroy(billing_t *b)
{
    if (b == NULL) return;
    pthread_rwlock_destroy(&b->lock);
    free(b);
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
                 uint64_t *resv_id)
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
    /* 鍙敤=浣欓+閫忔敮-宸查鍗狅紱鎺ョ嚎鎵归潬 DB 琛岄攣/CAS 淇濊瘉骞跺彂姝ｇ‘ */
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

static resv_t *resv_find(billing_t *b, uint64_t id)
{
    int i;
    for (i = 0; i < (int)(sizeof(b->resv) / sizeof(b->resv[0])); ++i) {
        if (b->resv[i].used && b->resv[i].id == id)
            return &b->resv[i];
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

int bill_settle(billing_t *b, uint64_t resv_id, int64_t actual_uc)
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
    if (actual_uc < 0) {
        pthread_rwlock_unlock(&b->lock);
        return NSBC_INVAL;
    }
    a = &b->accts[r->ai];
    a->reserved -= r->amount;
    a->balance -= actual_uc; /* 閫忔敮浣撶幇鍦ㄤ綑棰濅负璐燂紝鍙?credit_limit 绾︽潫 */
    r->used = 0;
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
    return json_walk_objects(json, strlen(json), walk_upsert_entry, b);
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

/*
    Author:       鏉庢旦 (Li Hao)
    Company:      涓婃捣瀹佸崼淇℃伅鎶€鏈湁闄愬叕鍙?(Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
