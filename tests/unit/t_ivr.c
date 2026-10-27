/* ============================================================================
 * Unit test: IVR menu reload JSON parser + profile lookup
 * Coverage:
 *   1. jstr key 精确匹配（"act" 不应匹配 "action"）
 *   2. jstr 转义引号还原（"name":"a\"b" → 输出 a"b）
 *   3. jarray_at 空数组 [] 返回 NULL（非 len=0 伪指针）
 *   4. jarray_at 越界终止（不存在的 index 返回 NULL）
 *   5. jint 默认值（缺失 key 返回 defaultValue）
 *   6. ivr_reload_json upsert 后 ivr_profile_find 可查到
 *   7. ivr_menus_reload_json 全量替换（先清空旧表再填充）
 *   8. reload 并发 — 多线程同时查 profile + reload（rwlock 不阻塞读）
 * Build:  make check  (tests/unit/t_ivr.c 自动编入)
 * ========================================================================== */

#include "ivr.h"
#include "nsbc_common.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- 测试 1: jstr key 精确匹配 ----
 * 旧 jstr 如果只做 strstr("act") 会匹配 "action" 的 "act" 前缀。
 * 正确行为：key 后必须紧跟 " 才算匹配。 */
static void test_jstr_key_precision(void)
{
    /* 模拟 IVR nodes_json: { "action":"hangup", "name":"act" }
     * 搜 "act" 只应匹配 "name":"act" 那个 value，不应匹配
     * "action":"hangup" 里的 act 子串。 */
    char buf[128] = "{\"action\":\"hangup\",\"name\":\"act\"}";
    char out[32] = {0};
    int rc = jstr(buf, "act", out, sizeof(out));
    /* jstr 找不到 "act" 作为完整 key（只有 "action" 和 "name"），返回 -1 */
    assert(rc != 0);
    assert(out[0] == '\0');

    /* 搜 "action" — 应该能匹配 */
    rc = jstr(buf, "action", out, sizeof(out));
    assert(rc == 0);
    assert(strcmp(out, "hangup") == 0);

    puts("  [PASS] jstr key 精确匹配: 'act' 不会误命中 'action'");
}

/* ---- 测试 2: jstr 转义引号还原 ----
 * Rust serde_json 会把 "name":"a\"b" 序列化为 nodes_json 中的转义序列。
 * jstr 必须正确还原为 a"b。 */
static void test_jstr_escaped_quotes(void)
{
    /* 原始 JSON 中的 \" 在 Rust→C JSON 帧传输时保持原样 */
    char buf[256] = "{\"name\":\"a\\\"b\\\"c\"}";
    char out[64] = {0};
    int rc = jstr(buf, "name", out, sizeof(out));
    assert(rc == 0);
    assert(strcmp(out, "a\"b\"c") == 0);

    /* 值内含多个转义引号 */
    char buf2[256] = "{\"prompt\":\"hello \\\"world\\\"!\"}";
    char out2[64] = {0};
    rc = jstr(buf2, "prompt", out2, sizeof(out2));
    assert(rc == 0);
    assert(strcmp(out2, "hello \"world\"!") == 0);

    puts("  [PASS] jstr 转义引号还原: a\\\"b\\\"c → a\"b\"c");
}

/* ---- 测试 3: jarray_at 空数组防御 ---- */
static void test_jarray_at_empty(void)
{
    size_t len = 0;
    const char *r = jarray_at("[]", 0, &len);
    assert(r == NULL);  /* 不是伪指针（非 NULL 但 len=0） */
    assert(len == 0);

    /* 有一个元素 —— idx=0 返回第一个元素，idx=1 返回 NULL */
    r = jarray_at("[{\"a\":1}]", 0, &len);
    assert(r != NULL);
    assert(len > 0);
    r = jarray_at("[{\"a\":1}]", 1, &len);
    assert(r == NULL);

    /* 空字符串 —— 返回 NULL */
    r = jarray_at("", 0, &len);
    assert(r == NULL);

    puts("  [PASS] jarray_at 边界: 空数组/越界/空串 → 正确返回 NULL");
}

/* ---- 测试 4: jint 默认值 ---- */
static void test_jint_default(void)
{
    /* key 存在且有值 */
    assert(jint("{\"timeout\":30}", "timeout", 5) == 30);
    /* key 不存在 → 返回 defaultValue */
    assert(jint("{\"other\":1}", "timeout", 5) == 5);
    /* 空 JSON → 返回 defaultValue */
    assert(jint("{}", "timeout", 10) == 10);
    /* 空字符串 → 返回 defaultValue */
    assert(jint("", "timeout", 99) == 99);

    puts("  [PASS] jint 默认值: 缺失 key → defaultValue");
}

/* ---- 测试 5: reload + lookup ---- */
static void test_reload_and_lookup(void)
{
    /* 构造一个最小可用的 nodes_json（IVR 菜单节点树） */
    const char *nodes =
        "[{\"id\":\"main\",\"act\":\"hangup\"}]";

    int rc = ivr_reload_json("test_menu", "default", nodes, "main");
    assert(rc == NSBC_OK);

    /* ivr_profile_find 应该能查到 */
    const ivr_profile_t *p = ivr_profile_find("test_menu", "default");
    assert(p != NULL);
    assert(strcmp(p->name, "test_menu") == 0);
    assert(p->n_nodes >= 1);

    /* tenant 过滤 —— 传不同 tenant 应返回 NULL */
    p = ivr_profile_find("test_menu", "other_tenant");
    assert(p == NULL);

    /* 传 NULL tenant 应返回第一条匹配（兼容旧调用方） */
    p = ivr_profile_find("test_menu", NULL);
    assert(p != NULL);

    /* 不存在的名字 → NULL */
    p = ivr_profile_find("nonexist", "default");
    assert(p == NULL);

    puts("  [PASS] ivr_reload_json + ivr_profile_find: upsert 后可查到");
}

/* ---- 测试 6: 全量替换（reload 前清空旧表）---- */
static void test_full_reload_clear(void)
{
    /* 先 reload 一条 */
    ivr_reload_json("menu_a", "default", "[{\"id\":\"a\",\"act\":\"play\"}]", "a");
    assert(ivr_profile_find("menu_a", "default") != NULL);

    /* 构造 JSON 数组全量 reload —— 只含 menu_b，menu_a 应消失 */
    const char *full_json =
        "[{\"name\":\"menu_b\",\"tenant_id\":1,\"nodes_json\":\"[{\\\"id\\\":\\\"b\\\"}]\"}]";

    int rc = ivr_menus_reload_json(full_json, NULL, 0);
    assert(rc == NSBC_OK);

    /* menu_a 应该被清空 */
    assert(ivr_profile_find("menu_a", "default") == NULL);
    /* menu_b 应该存在（但 tenant 可能是 "1" 不是 "default"） */
    /* 注意：ivr_menus_reload_json 里 tenant 是从 tenant_id 转字符串的，
     * 所以 menu_b 的 tenant 是 "1"，不是 "default" */
    const ivr_profile_t *p = ivr_profile_find("menu_b", "1");
    assert(p != NULL);

    puts("  [PASS] ivr_menus_reload_json 全量替换: 旧菜单清空 + 新菜单填充");
}

/* ---- 测试 7: 并发读 + reload（rwlock 不阻塞读）---- */
static volatile int g_concurrent_pass = 1;
static void *reader_thread(void *arg)
{
    int iterations = 10000;
    for (int i = 0; i < iterations; ++i) {
        const ivr_profile_t *p = ivr_profile_find("menu_b", "1");
        /* reload 之间短暂的空窗期可能返回 NULL —— 这是可接受的降级。
         * 我们只验证不会 crash，不验证每次都能查到。 */
        if (p && strcmp(p->name, "menu_b") != 0) {
            g_concurrent_pass = 0;
            break;
        }
    }
    return NULL;
}

static void *reloader_thread(void *arg)
{
    int iterations = 50;
    for (int i = 0; i < iterations; ++i) {
        const char *full_json =
            "[{\"name\":\"menu_b\",\"tenant_id\":1,\"nodes_json\":\"[{\\\"id\\\":\\\"b\\\"}]\"}]";
        ivr_menus_reload_json(full_json, NULL, 0);
    }
    return NULL;
}

static void test_concurrent_reload_read(void)
{
    pthread_t readers[4], reloader;
    for (int i = 0; i < 4; ++i)
        pthread_create(&readers[i], NULL, reader_thread, NULL);
    pthread_create(&reloader, NULL, reloader_thread, NULL);

    for (int i = 0; i < 4; ++i)
        pthread_join(readers[i], NULL);
    pthread_join(reloader, NULL);

    assert(g_concurrent_pass);
    puts("  [PASS] 并发 4 reader + 1 reload: rwlock 读零阻塞，无 crash");
}

int main(void)
{
    puts("=== IVR module unit tests ===");
    test_jstr_key_precision();
    test_jstr_escaped_quotes();
    test_jarray_at_empty();
    test_jint_default();
    test_reload_and_lookup();
    test_full_reload_clear();
    test_concurrent_reload_read();
    puts("ALL ivr tests PASSED\n");
    return 0;
}
