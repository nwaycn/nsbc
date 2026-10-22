/* 费率/实时计费——粗骨架（REQ-PBX-BILL-001~005，P1）
 *
 * 粗骨架约定：
 *  - 费率表/账户为定槽数组（接线批换分片哈希 + DBAL 余额行，
 *    余额扣减走单行原子 UPDATE ... WHERE balance>=x 防透支竞争）；
 *  - 预占记录定槽；周期扣费（P1 每计费块）在接线批由时间轮驱动，
 *    届时复用 settle（旧预占）+ reserve（下一块）两段式。
 * 金额一律 int64 微元，不做浮点除法。
 */
#include "rating.h"

typedef struct {
    int      used;
    char     key[BILL_ACCT_KEY];
    int64_t  balance;
    int64_t  reserved;
    int64_t  credit_limit;
} acct_t;

typedef struct {
    int      used;
    uint64_t id;
    size_t   ai;
    int64_t  amount;
} resv_t;

struct billing {
    pthread_mutex_t lock;       /* 所有 public API 互斥 */
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
    pthread_mutex_init(&b->lock, NULL);
    *out = b;
    return NSBC_OK;
}

void bill_destroy(billing_t *b)
{
    if (b == NULL) return;
    pthread_mutex_destroy(&b->lock);
    free(b);
}

int rate_upsert(billing_t *b, const rate_entry_t *r)
{
    int i;
    int rc;
    if (b == NULL || r == NULL || r->prefix[0] == '\0' ||
        r->price_per_min_uc < 0 || r->bill_inc_s <= 0)
        return NSBC_INVAL;
    pthread_mutex_lock(&b->lock);
    for (i = 0; i < b->n_rates; ++i) {
        if (strcmp(b->rates[i].prefix, r->prefix) == 0) {
            b->rates[i] = *r;
            pthread_mutex_unlock(&b->lock);
            return NSBC_OK;
        }
    }
    if (b->n_rates >= RATE_PLAN_MAX) {
        pthread_mutex_unlock(&b->lock);
        return NSBC_NOSPC;
    }
    b->rates[b->n_rates++] = *r;
    pthread_mutex_unlock(&b->lock);
    rc = NSBC_OK;
    return rc;
}

int rate_delete(billing_t *b, const char *prefix)
{
    int i;
    if (b == NULL || prefix == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&b->lock);
    for (i = 0; i < b->n_rates; ++i) {
        if (strcmp(b->rates[i].prefix, prefix) == 0) {
            /* 前移填补空洞 */
            memmove(&b->rates[i], &b->rates[i + 1],
                    sizeof(rate_entry_t) * (size_t)(b->n_rates - i - 1));
            --b->n_rates;
            pthread_mutex_unlock(&b->lock);
            return NSBC_OK;
        }
    }
    pthread_mutex_unlock(&b->lock);
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
    pthread_mutex_lock(&b->lock);
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
        pthread_mutex_unlock(&b->lock);
        return NSBC_NOENT;
    }
    *out = b->rates[bi];
    pthread_mutex_unlock(&b->lock);
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
    /* 向上取整到计费块；initial 首块单独处理（粗骨架并入 min） */
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
    /* 微元/分钟 × 秒 / 60：先乘后除保精度（int64 容量内） */
    q->charge_uc = r->price_per_min_uc * (int64_t)secs / 60;
    return NSBC_OK;
}

static acct_t *acct_find(billing_t *b, const char *key)
{
    int i;
    for (i = 0; i < BILL_ACCT_MAX; ++i) {
        if (b->accts[i].used && strcmp(b->accts[i].key, key) == 0)
            return &b->accts[i];
    }
    return NULL;
}

int account_open(billing_t *b, const char *key, int64_t balance_uc,
                 int64_t credit_limit_uc)
{
    acct_t *a;
    if (b == NULL || key == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&b->lock);
    a = acct_find(b, key);
    if (a != NULL) {
        a->balance = balance_uc;
        a->credit_limit = credit_limit_uc;
        pthread_mutex_unlock(&b->lock);
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
        pthread_mutex_unlock(&b->lock);
        return NSBC_NOSPC;
    }
    a->used = 1;
    snprintf(a->key, sizeof(a->key), "%s", key);
    a->balance = balance_uc;
    a->reserved = 0;
    a->credit_limit = credit_limit_uc;
    pthread_mutex_unlock(&b->lock);
    return NSBC_OK;
}

int account_balance(billing_t *b, const char *key,
                    int64_t *balance, int64_t *reserved)
{
    const acct_t *a;
    if (b == NULL || key == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&b->lock);
    a = acct_find(b, key);
    if (a == NULL) {
        pthread_mutex_unlock(&b->lock);
        return NSBC_NOENT;
    }
    if (balance != NULL)
        *balance = a->balance;
    if (reserved != NULL)
        *reserved = a->reserved;
    pthread_mutex_unlock(&b->lock);
    return NSBC_OK;
}

int account_topup(billing_t *b, const char *key, int64_t delta_uc)
{
    acct_t *a;
    int rc;
    if (b == NULL || key == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&b->lock);
    a = acct_find(b, key);
    if (a == NULL) {
        pthread_mutex_unlock(&b->lock);
        return NSBC_NOENT;
    }
    a->balance += delta_uc;
    pthread_mutex_unlock(&b->lock);
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
    pthread_mutex_lock(&b->lock);
    a = acct_find(b, key);
    if (a == NULL) {
        pthread_mutex_unlock(&b->lock);
        return NSBC_NOENT;
    }
    /* 可用=余额+透支-已预占；接线批靠 DB 行锁/CAS 保证并发正确 */
    if (a->balance + a->credit_limit - a->reserved < amount_uc) {
        pthread_mutex_unlock(&b->lock);
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
            pthread_mutex_unlock(&b->lock);
            return NSBC_OK;
        }
    }
    pthread_mutex_unlock(&b->lock);
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
    pthread_mutex_lock(&b->lock);
    r = resv_find(b, resv_id);
    if (r == NULL) {
        pthread_mutex_unlock(&b->lock);
        return NSBC_NOENT;
    }
    a = &b->accts[r->ai];
    a->reserved -= r->amount;
    r->used = 0;
    pthread_mutex_unlock(&b->lock);
    return NSBC_OK;
}

int bill_settle(billing_t *b, uint64_t resv_id, int64_t actual_uc)
{
    resv_t *r;
    acct_t *a;
    if (b == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&b->lock);
    r = resv_find(b, resv_id);
    if (r == NULL) {
        pthread_mutex_unlock(&b->lock);
        return NSBC_NOENT;
    }
    if (actual_uc < 0) {
        pthread_mutex_unlock(&b->lock);
        return NSBC_INVAL;
    }
    a = &b->accts[r->ai];
    a->reserved -= r->amount;
    a->balance -= actual_uc; /* 透支体现在余额为负，受 credit_limit 约束 */
    r->used = 0;
    if (b->low_cb != NULL && a->balance <= b->low_threshold) {
        pthread_mutex_unlock(&b->lock);
        b->low_cb(b->low_ud, a->key, a->balance);
        pthread_mutex_lock(&b->lock);
    }
    pthread_mutex_unlock(&b->lock);
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

/* ---- OAM 只读迭代 ---- */

int bill_foreach_rate(billing_t *b, rate_iter_cb cb, void *ud)
{
    int i;
    int rc;
    if (b == NULL || cb == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&b->lock);
    for (i = 0; i < b->n_rates; ++i) {
        rc = cb(&b->rates[i], ud);
        if (rc != NSBC_OK) {
            pthread_mutex_unlock(&b->lock);
            return rc;
        }
    }
    pthread_mutex_unlock(&b->lock);
    return NSBC_OK;
}

int bill_foreach_account(billing_t *b, acct_iter_cb cb, void *ud)
{
    int i;
    int rc;
    if (b == NULL || cb == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&b->lock);
    for (i = 0; i < BILL_ACCT_MAX; ++i) {
        if (!b->accts[i].used)
            continue;
        rc = cb(b->accts[i].key, b->accts[i].balance,
                b->accts[i].reserved, b->accts[i].credit_limit, ud);
        if (rc != NSBC_OK) {
            pthread_mutex_unlock(&b->lock);
            return rc;
        }
    }
    pthread_mutex_unlock(&b->lock);
    return NSBC_OK;
}

int bill_rate_count(billing_t *b)
{
    int n;
    if (!b) return 0;
    pthread_mutex_lock(&b->lock);
    n = b->n_rates;
    pthread_mutex_unlock(&b->lock);
    return n;
}

int bill_account_count(billing_t *b)
{
    int i, n = 0;
    if (!b) return 0;
    pthread_mutex_lock(&b->lock);
    for (i = 0; i < BILL_ACCT_MAX; ++i)
        if (b->accts[i].used) ++n;
    pthread_mutex_unlock(&b->lock);
    return n;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
