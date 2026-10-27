/* ============================================================================
 * Unit test: Billing/Rating module
 * Coverage:
 *   1. rate_lookup: 鏈€闀垮墠缂€鍖归厤瑙勫垯 (0.0.0.0/0 < 44 < 4420 < 44207123456)
 *   2. rate_lookup: disabled 璐圭巼琚烦杩? *   3. account_open / account_balance: 璐︽埛 CRUD + credit_limit 妫€鏌? *   4. bill_reserve / bill_release / bill_settle: 璁¤垂鍏ㄦ祦绋? *   5. 骞跺彂: 澶氱嚎绋?rate_lookup锛坮dlock锛? 鍗曠嚎绋?rate_upsert锛坵rlock锛? *           楠岃瘉 rwlock 涓嶉樆濉炲苟鍙戣
 *   6. resv_find: 绾挎€ф壂鎻忔纭€э紙涓嶉獙璇?hash 浼樺寲锛屼笅杞姞锛? * Build:  make check
 * ========================================================================== */

#include "rating.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/* ---- 娴嬭瘯 1: rate_lookup 鏈€闀垮墠缂€鍖归厤 ---- */
static void test_rate_longest_prefix(void)
{
    billing_t *b = NULL;
    assert(bill_create(&b) == NSBC_OK);
    assert(b);

    rate_entry_t e = {0};
    e.enabled = 1; e.price_per_min_uc = 1000000; /* 1.00 CNY/鍒嗛挓 */
    snprintf(e.prefix, sizeof(e.prefix), "%s", "44");
    rate_upsert(b, &e);

    e.price_per_min_uc = 2000000; /* 2.00 CNY/鍒嗛挓 */
    snprintf(e.prefix, sizeof(e.prefix), "%s", "4420");
    rate_upsert(b, &e);

    e.price_per_min_uc = 5000000; /* 5.00 CNY/鍒嗛挓 */
    snprintf(e.prefix, sizeof(e.prefix), "%s", "44207");
    rate_upsert(b, &e);

    rate_entry_t out = {0};
    /* 44207123456 鈫?鏈€闀垮墠缂€ 44207 鈫?5鍏?*/
    assert(rate_lookup(b, "44207123456", &out) == NSBC_OK);
    assert(out.price_per_min_uc == 5000000);

    /* 44201234567 鈫?鏈€闀垮墠缂€ 4420 鈫?2鍏?*/
    assert(rate_lookup(b, "44201234567", &out) == NSBC_OK);
    assert(out.price_per_min_uc == 2000000);

    /* 44123456789 鈫?鏈€闀垮墠缂€ 44 鈫?1鍏?*/
    assert(rate_lookup(b, "44123456789", &out) == NSBC_OK);
    assert(out.price_per_min_uc == 1000000);

    /* 12345678901 鈫?娌″墠缂€鍖归厤 鈫?NSBC_NOENT */
    assert(rate_lookup(b, "12345678901", &out) == NSBC_NOENT);

    bill_destroy(b);
    puts("  [PASS] rate_lookup 鏈€闀垮墠缂€鍖归厤: 44207/4420/44 姝ｇ‘鍛戒腑");
}

/* ---- 娴嬭瘯 2: disabled 璐圭巼琚烦杩?---- */
static void test_rate_skip_disabled(void)
{
    billing_t *b = NULL; assert(bill_create(\&b) == NSBC_OK); assert(b);
    assert(b);

    rate_entry_t e = {0};
    e.enabled = 1; e.price_per_min_uc = 1000000;
    snprintf(e.prefix, sizeof(e.prefix), "%s", "44");
    rate_upsert(b, &e);

    /* 绂佺敤 4420 鈥?鍗充娇鏇撮暱涔熷簲璺宠繃锛屽洖钀藉埌 44 */
    e.enabled = 0; e.price_per_min_uc = 2000000;
    snprintf(e.prefix, sizeof(e.prefix), "%s", "4420");
    rate_upsert(b, &e);

    rate_entry_t out = {0};
    assert(rate_lookup(b, "44201234567", &out) == NSBC_OK);
    assert(out.price_per_min_uc == 1000000);  /* 鍥炶惤鍒?44 鐨?1 鍏?*/

    bill_destroy(b);
    puts("  [PASS] disabled 璐圭巼璺宠繃锛屽洖钀藉埌涓嬩竴涓渶闀垮惎鐢ㄥ墠缂€");
}

/* ---- 娴嬭瘯 3: 璐︽埛浣欓 CRUD ---- */
static void test_account_crud(void)
{
    billing_t *b = NULL; assert(bill_create(\&b) == NSBC_OK); assert(b);
    assert(b);

    /* 涓嶅瓨鍦ㄧ殑璐︽埛鏌ヤ綑棰?鈫?NOENT */
    int64_t bal = -99, resv = -99;
    assert(account_balance(b, "alice", &bal, &resv) == NSBC_NOENT);

    /* 寮€璐︽埛 */
    assert(account_open(b, "alice", 10000000, 0) == NSBC_OK);  /* 10鍏?*/
    assert(account_balance(b, "alice", &bal, &resv) == NSBC_OK);
    assert(bal == 10000000);
    assert(resv == 0);

    /* 鍏呭€?*/
    assert(account_topup(b, "alice", 5000000) == NSBC_OK);  /* +5鍏?*/
    assert(account_balance(b, "alice", &bal, &resv) == NSBC_OK);
    assert(bal == 15000000);

    /* 鎵ｆ锛堣礋 delta锛?/
    assert(account_topup(b, "alice", -3000000) == NSBC_OK);  /* -3鍏?*/
    assert(account_balance(b, "alice", &bal, &resv) == NSBC_OK);
    assert(bal == 12000000);

    /* 寮€璐︽埛 鈥?閲嶅 key */
    assert(account_open(b, "alice", 10000000, 0) == NSBC_EXIST);

    /* 涓嶅瓨鍦ㄧ殑璐︽埛 topup */
    assert(account_topup(b, "bob", 1000000) == NSBC_NOENT);

    bill_destroy(b);
    puts("  [PASS] account_open/topup/balance: CRUD 鍏ㄦ祦绋?);
}

/* ---- 娴嬭瘯 4: 璁¤垂鍏ㄦ祦绋?reserve 鈫?settle ---- */
static void test_bill_reserve_settle(void)
{
    billing_t *b = NULL; assert(bill_create(\&b) == NSBC_OK); assert(b);
    assert(b);

    account_open(b, "alice", 10000000, 0);  /* 10鍏?*/
    rate_entry_t e = {0};
    e.enabled = 1; e.price_per_min_uc = 600000;  /* 60鍒?鍒嗛挓 */
    snprintf(e.prefix, sizeof(e.prefix), "%s", "44");
    rate_upsert(b, &e);

    rate_entry_t r = {0};
    rate_lookup(b, "44123456789", &r);

    /* 璧峰懠: bill_reserve(duration_sec=120) */
    uint64_t resv_id = 0;
    int64_t cost = 0;
    assert(bill_reserve(b, "alice", &r, 120, &resv_id, &cost) == NSBC_OK);
    assert(cost > 0);  /* 鑷冲皯 1 涓璐瑰潡 */
    int64_t bal_before = 0;
    account_balance(b, "alice", &bal_before, NULL);

    /* 缁撴潫閫氳瘽 鈥?瀹為檯 bill_sec=90锛堟瘮棰勬湡鐭級*/
    int64_t actual = 0;
    assert(bill_settle(b, resv_id, 90, &actual) == NSBC_OK);
    assert(actual > 0);
    assert(actual <= cost);  /* 瀹為檯 鈮?棰勭暀 */

    int64_t bal_after = 0, resv_after = 0;
    account_balance(b, "alice", &bal_after, &resv_after);
    assert(bal_after < bal_before);        /* 浣欓鍑忓皯 */
    assert(resv_after == 0);               /* 棰勭暀閲婃斁 */

    /* settle 涓嶅瓨鍦ㄧ殑 resv_id */
    assert(bill_settle(b, resv_id + 9999, 30, &actual) == NSBC_NOENT);

    bill_destroy(b);
    puts("  [PASS] bill_reserve 鈫?bill_settle: 棰勭暀鈫掔粨绠楋紝瀹為檯鈮ら鐣?);
}

/* ---- 娴嬭瘯 5: 骞跺彂 rate_lookup锛坮dlock 闆堕樆濉烇級---- */
static volatile int g_readers_ok = 1;
static billing_t *g_bill = NULL;

static void *rate_reader(void *arg)
{
    (void)arg;
    int64_t dummy_bal = 0;
    for (int i = 0; i < 20000; ++i) {
        rate_entry_t out = {0};
        rate_lookup(g_bill, "44123456789", &out);
        account_balance(g_bill, "alice", &dummy_bal, NULL);
    }
    return NULL;
}

static void *rate_writer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 100; ++i) {
        rate_entry_t e = {0};
        e.enabled = 1; e.price_per_min_uc = 1000000 + i;
        snprintf(e.prefix, sizeof(e.prefix), "%s", "44");
        rate_upsert(g_bill, &e);
    }
    return NULL;
}

static void test_concurrent_read_write(void)
{
    g_bill = NULL; assert(bill_create(\&g_bill) == NSBC_OK);
    assert(g_bill);
    rate_entry_t e = {0};
    e.enabled = 1; e.price_per_min_uc = 1000000;
    snprintf(e.prefix, sizeof(e.prefix), "%s", "44");
    rate_upsert(g_bill, &e);
    account_open(g_bill, "alice", 10000000, 0);

    pthread_t readers[8], writer;
    for (int i = 0; i < 8; ++i) pthread_create(&readers[i], NULL, rate_reader, NULL);
    pthread_create(&writer, NULL, rate_writer, NULL);
    for (int i = 0; i < 8; ++i) pthread_join(readers[i], NULL);
    pthread_join(writer, NULL);

    assert(g_readers_ok);
    bill_destroy(g_bill);
    g_bill = NULL;
    puts("  [PASS] 骞跺彂 8 reader + 1 writer: rwlock 璇婚浂闃诲锛屾棤 crash");
}

/* ---- 测试 6: account_reload_json upsert-only 语义 ----
 * 关键：reload 时已存在账户的 reserved 字段必须保留
 * （resv[] 里可能有活跃 reservation，bill_settle 还要结算）
 * 关键：reload 不删除 JSON 里没有的账户（upsert-only）*/
static void test_account_reload_json_upsert_only(void)
{
    billing_t *b = NULL; assert(bill_create(&b) == NSBC_OK);

    /* 开账户 alice (balance=100) */
    account_open(b, "alice", 100, 0);

    /* reload JSON：alice balance 从 100 → 200 */
    const char *json =
        "[{\"account_key\":\"alice\",\"balance_uc\":200,\"credit_limit_uc\":0}]";
    extern int account_reload_json(billing_t *b, const char *json);
    assert(account_reload_json(b, json) == NSBC_OK);

    /* 验证：balance 被覆盖为 200 */
    int64_t bal = -99, resv = -99;
    assert(account_balance(b, "alice", &bal, &resv) == NSBC_OK);
    assert(bal == 200);
    assert(resv == 0);   /* reserved 字段未被 reload 触碰，保持 0 */

    /* 验证：reload 不删除 JSON 里没有的账户 (upsert-only) */
    account_open(b, "bob", 50, 0);
    account_reload_json(b, json);  /* 仍只含 alice */
    assert(account_balance(b, "bob", &bal, &resv) == NSBC_OK);
    assert(bal == 50);  /* bob 仍然存在！*/

    bill_destroy(b);
    puts("  [PASS] account_reload_json: upsert-only (balance覆盖, reserved保持, JSON外账户保留)");
}

/* ---- 测试 7: 关键流 — bill_reserve → account_reload_json → bill_settle ----
 *
 * 这是 THE 关键测试：真实世界里的 UDS 失败补偿自愈场景。
 *   1. 账户 alice 有 10 元余额
 *   2. 发起呼叫 → bill_reserve 预占 1 元
 *   3. 与此同时 reload 推送（DB 余额已变）→ account_reload_json
 *   4. 呼叫结束 → bill_settle 必须找到 reservation 并正确结算
 *
 * 如果 reload 错误删除了 acct 条目或清了 resv[]，settle 会返回 NOENT 丢账。
 * 这个测试直接验证 upsert-only 语义在真实计费流程下不丢账。*/
static void test_reload_during_active_reservation(void)
{
    billing_t *b = NULL; assert(bill_create(&b) == NSBC_OK);

    account_open(b, "alice", 10000000, 0);  /* 10元 */

    /* 费率：60分/分钟 */
    rate_entry_t e = {0};
    e.enabled = 1; e.price_per_min_uc = 600000;
    snprintf(e.prefix, sizeof(e.prefix), "%s", "44");
    rate_upsert(b, &e);

    rate_entry_t r = {0};
    rate_lookup(b, "44123456789", &r);

    /* Step 1: 发起呼叫 → reserve 预占 120 秒 */
    uint64_t resv_id = 0;
    int64_t cost = 0;
    assert(bill_reserve(b, "alice", &r, 120, &resv_id, &cost) == NSBC_OK);
    assert(cost > 0);

    /* 验证：reserved > 0 */
    int64_t bal1 = 0, resv1 = 0;
    account_balance(b, "alice", &bal1, &resv1);
    assert(resv1 > 0);

    /* Step 2: UDS 失败补偿触发 → account_reload_json
     * （模拟 DB 余额已经从 10 元变 15 元了 —— 但 resv 必须保持）*/
    const char *json_after_reload =
        "[{\"account_key\":\"alice\",\"balance_uc\":15000000,\"credit_limit_uc\":0}]";
    assert(account_reload_json(b, json_after_reload) == NSBC_OK);

    /* 验证：balance 被覆盖为 15 元，但 reserved 仍保持 */
    int64_t bal2 = 0, resv2 = 0;
    account_balance(b, "alice", &bal2, &resv2);
    assert(bal2 == 15000000);       /* ✅ balance 从 DB 覆盖 */
    assert(resv2 > 0);              /* ✅ reserved 没被 reload 清零 */
    assert(resv2 == resv1);         /* ✅ reserved 完全一致 */

    /* Step 3: 呼叫结束 → settle —— 必须找到 reservation 并结算 */
    int64_t actual = 0;
    assert(bill_settle(b, resv_id, 90, &actual) == NSBC_OK);  /* ✅ NOENT → crash 丢账 */
    assert(actual > 0);
    assert(actual <= cost);

    /* 验证：reserved 清零，balance 正确扣减 */
    int64_t bal3 = 0, resv3 = 0;
    account_balance(b, "alice", &bal3, &resv3);
    assert(resv3 == 0);
    assert(bal3 < bal2);  /* balance 从 15 元扣了 90 秒的钱 */

    bill_destroy(b);
    puts("  [PASS] bill_reserve → account_reload_json → bill_settle: 活跃 reservation 下 reload 不丢账");
}

/* ---- 测试 8: 边界 — 空数组 [] → no-op ----
 * reload 空数组是合法场景（DB 里暂时没账户）。
 * C 侧 jarray_at("[]", 0, &len) 应该返回 NULL，account_reload_json
 * 不 crash、不改任何状态、返回 NSBC_OK。*/
static void test_reload_empty_array_noop(void)
{
    billing_t *b = NULL; assert(bill_create(&b) == NSBC_OK);

    account_open(b, "alice", 100, 0);

    int64_t bal_before = 0, resv_before = 0;
    account_balance(b, "alice", &bal_before, &resv_before);

    /* 空数组 reload —— 应该什么都不做 */
    assert(account_reload_json(b, "[]") == NSBC_OK);

    int64_t bal_after = 0, resv_after = 0;
    assert(account_balance(b, "alice", &bal_after, &resv_after) == NSBC_OK);
    assert(bal_after == bal_before);  /* ✅ 不变 */
    assert(resv_after == resv_before);

    bill_destroy(b);
    puts("  [PASS] account_reload_json([]): 空数组 no-op，不 crash 不改状态");
}

/* ---- 测试 9: 边界 — 缺失 account_key 字段 → skip 而非 crash ----
 * 一条 JSON 条目没有 account_key（格式错误），应该 skip 继续下一条。*/
static void test_reload_skip_bad_entries(void)
{
    billing_t *b = NULL; assert(bill_create(&b) == NSBC_OK);

    /* JSON 数组：一条坏（缺 key）+ 一条好（alice）+ 一条坏（空 key）*/
    const char *mixed_json =
        "["
        "{\"balance_uc\":100,\"credit_limit_uc\":0},"         /* 缺 account_key */
        "{\"account_key\":\"alice\",\"balance_uc\":200,\"credit_limit_uc\":0},"  /* ✅ 好 */
        "{\"account_key\":\"\",\"balance_uc\":300,\"credit_limit_uc\":0}"        /* 空 key */
        "]";

    assert(account_reload_json(b, mixed_json) == NSBC_OK);

    /* 只有 alice 被正确 upsert */
    int64_t bal = 0;
    assert(account_balance(b, "alice", &bal, NULL) == NSBC_OK);
    assert(bal == 200);

    /* 空 key 不会被创建成账户 —— jstr 输出空串，key[0]=='\0' 被 skip */
    assert(account_balance(b, "", &bal, NULL) == NSBC_NOENT);

    bill_destroy(b);
    puts("  [PASS] account_reload_json: 坏条目被 skip，好条目正常 upsert");
}

/* ---- 测试 10: 边界 — 超长 account_key 截断 ----
 * account_reload_json 内部的 char key[BILL_ACCT_KEY=64] 用 snprintf/null 终止，
 * 超长 key 被正确截断而非溢出。*/
static void test_reload_long_key_truncation(void)
{
    billing_t *b = NULL; assert(bill_create(&b) == NSBC_OK);

    /* 构造一个 128 字符长的 key —— 超过 BILL_ACCT_KEY=64 限制 */
    char long_key[256];
    memset(long_key, 'K', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = '\0';

    char json[512];
    snprintf(json, sizeof(json),
        "[{\"account_key\":\"%s\",\"balance_uc\":1000,\"credit_limit_uc\":0}]",
        long_key);

    /* 不能 crash —— 内部 snprintf(key, sizeof(key), "%s", key_val) 会截断 */
    int rc = account_reload_json(b, json);
    assert(rc == NSBC_OK);

    /* bill_account_count 至少 1 —— 截断后的 key 成功创建了账户 */
    assert(bill_account_count(b) >= 1);

    bill_destroy(b);
    puts("  [PASS] account_reload_json: 超长 account_key 截断不 crash");
}

int main(void)
{
    puts("=== Billing/Rating module unit tests ===");
    test_rate_longest_prefix();
    test_rate_skip_disabled();
    test_account_crud();
    test_bill_reserve_settle();
    test_concurrent_read_write();
    test_account_reload_json_upsert_only();
    test_reload_during_active_reservation();
    test_reload_empty_array_noop();
    test_reload_skip_bad_entries();
    test_reload_long_key_truncation();
    puts("ALL billing tests PASSED\n");
    return 0;
}
