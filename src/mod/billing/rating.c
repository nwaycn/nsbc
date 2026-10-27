/* 璐圭巼/瀹炴椂璁¤垂鈥斺€旂矖楠ㄦ灦锛圧EQ-PBX-BILL-001~005锛孭1锛? *
 * 绮楅鏋剁害瀹氾細
 *  - 璐圭巼琛?璐︽埛涓哄畾妲芥暟缁勶紙鎺ョ嚎鎵规崲鍒嗙墖鍝堝笇 + DBAL 浣欓琛岋紝
 *    浣欓鎵ｅ噺璧板崟琛屽師瀛?UPDATE ... WHERE balance>=x 闃查€忔敮绔炰簤锛夛紱
 *  - 棰勫崰璁板綍瀹氭Ы锛涘懆鏈熸墸璐癸紙P1 姣忚璐瑰潡锛夊湪鎺ョ嚎鎵圭敱鏃堕棿杞┍鍔紝
 *    灞婃椂澶嶇敤 settle锛堟棫棰勫崰锛? reserve锛堜笅涓€鍧楋級涓ゆ寮忋€? * 閲戦涓€寰?int64 寰厓锛屼笉鍋氭诞鐐归櫎娉曘€? */
#include "rating.h"
#include "ivr.h"   /* jstr / jint / jarray_at — Round 1 公开的手写 JSON 工具 */
#include "log.h"
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
    pthread_rwlock_wrlock(&b->lock);
    a = acct_find(b, key);
    if (a != NULL) {
        a->balance = balance_uc;
        a->credit_limit = credit_limit_uc;
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
    a->used = 1;
    snprintf(a->key, sizeof(a->key), "%s", key);
    a->balance = balance_uc;
    a->reserved = 0;
    a->credit_limit = credit_limit_uc;
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

        /* balance_uc / credit_limit_uc 可能是字符串或整数 — jint 处理整数，
         * 字符串数字值（如 "1000000"）jint 也能正确解析（strtol 走字符串）*/
        balance = (int64_t)jint(buf, "balance_uc", -1);
        if (balance < 0) balance = (int64_t)jint(buf, "balance", 0);
        credit  = (int64_t)jint(buf, "credit_limit_uc", 0);
        if (credit == 0) credit = (int64_t)jint(buf, "credit_limit", 0);

        if (account_open(b, key, balance, credit) != NSBC_OK) {
            ++n_fail;
        } else {
            ++n_ok;
        }
    }
    llog(LOG_INFO, "billing", "accounts.reload: upserted=%d fail=%d total_seen=%d",
         n_ok, n_fail, i);
    return NSBC_OK;
}

/*
    Author:       鏉庢旦 (Li Hao)
    Company:      涓婃捣瀹佸崼淇℃伅鎶€鏈湁闄愬叕鍙?(Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
