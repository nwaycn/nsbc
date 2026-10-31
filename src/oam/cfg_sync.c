/* config_sync — C core ↔ Rust web 配置全量拉取
 *
 * 两条入口：
 *   1) cmd_reload（obs_control.c）— YAML reload 成功后拉 DB 同步
 *   2) oam_start（oam_module.c） — 启动时 retry 拉 DB 表
 *   3) SIGHUP reload（main.c）— 与 control.sock reload 同路径
 *
 * 协议（CMD_REQ）:
 *   C → Web: {"cmd":"cfg.pull_all"}
 *   Web → C: {"ok":true,
 *             "extensions":[...],    "gateways":[...],
 *             "trunks":[...],         "dialplan_rules":[...],
 *             "sip_profiles":[...],   "queues":[...],
 *             "ivr_menus":[...],
 *             "security_blacklist":{fence_mode,rules:[...]}}
 *
 * 各模块 reload_json 期望的 JSON 格式：
 *   ext_table_reload_json        — 纯 array
 *   gw_table_reload_json         — 纯 array（stub no-op，返回 OK）
 *   trunk_table_reload_json      — 纯 array 或 {"trunks":[...]}
 *   dp_rules_reload_json         — 纯 array
 *   sip_profiles_reload_json     — 纯 array
 *   queue_table_reload_json      — 纯 array
 *   ivr_menus_reload_json        — 纯 array
 *   security_blacklist_reload_json — 纯 array 或 object {fence_mode, rules:[...]}
 *
 * 本模块 cJSON_PrintUnformatted 把返回值的各字段单独序列化成：
 *   纯 array → 直接喂 _reload_json
 *   object   → 完整 JSON（含 fence_mode + rules）喂 security_blacklist_reload_json
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nsbc_common.h"
#include "uDS_client.h"
#include "route_table.h"
#include "dialplan.h"
#include "sip_profile.h"
#include "endpoints.h"     /* ext_table_reload_json */
#include "queue.h"         /* queue_table_reload_json */
#include "ivr.h"           /* ivr_menus_reload_json */
#include "security.h"      /* security_blacklist_reload_json */
#include "app_ctx.h"       /* nsbc_get_ctx() — reload 后打印 trunk 数 */
#include "log.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- 外部 reload_json 函数（无专用头文件）---- */
extern int trunk_table_reload_json(const char *json, char *out_err, size_t err_sz);
extern int dp_rules_reload_json(const char *json, char *out_err, size_t err_sz);
extern int sip_profiles_reload_json(const char *json, char *out_err, size_t err_sz);
extern int gw_table_reload_json(const char *json, char *out_err, size_t err_sz);

/* ---- 从 cJSON Object 提取 array 节点并序列化成纯 JSON array ----
 * 失败返回 NULL；调用方负责 free()。 */
static char *json_extract_array_str(const cJSON *root, const char *key)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!arr || !cJSON_IsArray(arr))
        return NULL;
    return cJSON_PrintUnformatted(arr);
}

/* ---- 从 cJSON Object 提取任意节点（array 或 object）并序列化成 JSON ----
 * 用于 security_blacklist（object {fence_mode, rules:[...]}）。
 * 失败返回 NULL；调用方负责 free()。 */
static char *json_extract_str(const cJSON *root, const char *key)
{
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!node)
        return NULL;
    return cJSON_PrintUnformatted(node);
}

/* ---- 通用 reload helper：提取 array 并调用 reload_json ----
 * node_key:       返回 JSON object 里的 key 名
 * reload_fn:      reload_json 函数指针
 * node_label:     日志标签（"trunks" / "sip_profiles" / ...）
 * accept_missing: 1=key 缺失只打 INFO（可选模块），0=缺失打 WARN（预期存在）
 */
typedef int (*reload_json_fn)(const char *, char *, size_t);

static void cfg_reload_array(const cJSON *root, const char *node_key,
                             reload_json_fn reload_fn, const char *node_label,
                             int accept_missing, int *any_loaded)
{
    char *arr = json_extract_array_str(root, node_key);
    if (arr) {
        char err[128] = {0};
        int rc2 = reload_fn(arr, err, sizeof(err));
        if (rc2 == NSBC_OK) {
            llog(LOG_INFO, "cfg-sync", "pull_all: %s OK", node_label);
            if (any_loaded) *any_loaded = 1;
        } else {
            llog(LOG_WARN, "cfg-sync", "pull_all: %s reload failed: %s",
                 node_label, err);
        }
        free(arr);
    } else if (!accept_missing) {
        llog(LOG_WARN, "cfg-sync", "pull_all: %s field missing or non-array in response",
             node_label);
    }
    /* accept_missing=1 时 field 缺失打 INFO 即可 */
}

/* ---- 单次 pull_all（不 retry）----
 * 返回 NSBC_OK 表示至少拉到一个模块；NSBC_ERR 表示整个请求失败
 * （data.sock 未连接 / DB 未就绪 / JSON 解析失败）。 */
static int cfg_pull_all_once(char *out_err, size_t err_sz)
{
    /* 1. 发请求 */
    static const char req[] = "{\"cmd\":\"cfg.pull_all\"}";
    /* 512KB — 8 张表（extensions/gateways/trunks/dialplan_rules/sip_profiles/
     * queues/ivr_menus/security_blacklist）按最宽松估 ≈ 400KB；
     * uds_request resp_cap 会截断，给足空间避免半截 JSON */
    char resp[512 * 1024];
    int rc = uds_request(req, UDS_REQ_TIMEOUT_MS, resp, sizeof(resp));
    if (rc != NSBC_OK) {
        if (out_err && err_sz > 0)
            snprintf(out_err, err_sz, "uds_request failed rc=%d", rc);
        return NSBC_ERR;
    }
    /* 确认响应有 NUL 结尾（uds_request 应该保证，但防御性校验） */
    resp[sizeof(resp) - 1] = '\0';

    /* 2. 解析 JSON */
    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        if (out_err && err_sz > 0) snprintf(out_err, err_sz, "cJSON parse failed");
        return NSBC_ERR;
    }
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!ok || !cJSON_IsTrue(ok)) {
        const char *reason = "";
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "reason");
        if (r && cJSON_IsString(r) && r->valuestring) reason = r->valuestring;
        if (out_err && err_sz > 0)
            snprintf(out_err, err_sz, "pull_all rejected: %s", reason);
        cJSON_Delete(root);
        return NSBC_ERR;
    }

    /* 3. 各模块单独 reload（允许部分失败——某模块表空或解析错不影响其他） */
    int any_loaded = 0;

    /* --- extensions (纯 array, 脱敏 HA1-only) --- */
    cfg_reload_array(root, "extensions",
                     ext_table_reload_json, "extensions",
                     1, &any_loaded);

    /* --- gateways (纯 array, stub no-op) --- */
    cfg_reload_array(root, "gateways",
                     gw_table_reload_json, "gateways",
                     1, &any_loaded);

    /* --- trunks (纯 array / route_table) --- */
    {
        char *arr = json_extract_array_str(root, "trunks");
        if (arr) {
            char err[128] = {0};
            int rc2 = trunk_table_reload_json(arr, err, sizeof(err));
            if (rc2 == NSBC_OK) {
                llog(LOG_INFO, "cfg-sync", "pull_all: trunks OK (table=%d)",
                     rt_trunk_count(nsbc_get_ctx() ? nsbc_get_ctx()->rt : NULL));
                any_loaded = 1;
            } else {
                llog(LOG_WARN, "cfg-sync", "pull_all: trunks reload failed: %s", err);
            }
            free(arr);
        } else {
            llog(LOG_WARN, "cfg-sync", "pull_all: trunks field missing or non-array");
        }
    }

    /* --- dialplan_rules --- */
    cfg_reload_array(root, "dialplan_rules",
                     dp_rules_reload_json, "dialplan_rules",
                     0, &any_loaded);

    /* --- sip_profiles --- */
    cfg_reload_array(root, "sip_profiles",
                     sip_profiles_reload_json, "sip_profiles",
                     0, &any_loaded);

    /* --- queues (ACD) --- */
    cfg_reload_array(root, "queues",
                     queue_table_reload_json, "queues",
                     1, &any_loaded);

    /* --- ivr_menus --- */
    cfg_reload_array(root, "ivr_menus",
                     ivr_menus_reload_json, "ivr_menus",
                     1, &any_loaded);

    /* --- security_blacklist (object: fence_mode + method_blocked + rules[]) ---
     * security_blacklist_reload_json 兼容纯 array 和 object 两种格式。 */
    {
        char *node = json_extract_str(root, "security_blacklist");
        if (node) {
            char err[128] = {0};
            int rc2 = security_blacklist_reload_json(node, err, sizeof(err));
            if (rc2 == NSBC_OK) {
                llog(LOG_INFO, "cfg-sync", "pull_all: security_blacklist OK");
                any_loaded = 1;
            } else {
                llog(LOG_WARN, "cfg-sync", "pull_all: security_blacklist reload failed: %s", err);
            }
            free(node);
        } else {
            llog(LOG_INFO, "cfg-sync", "pull_all: security_blacklist field missing (optional)");
        }
    }

    cJSON_Delete(root);
    return any_loaded ? NSBC_OK : NSBC_ERR;
}

/* ---- 对外 API：带 retry 的 pull_all ----
 * 启动时 data.sock 可能还没 bind（Rust web 晚几秒启动），
 * 所以 retry N 次，每次间隔 delay_ms。
 *   retries:  最大重试次数（0 = 单次）
 *   delay_ms: 每次 retry 间隔（毫秒）
 * 返回 NSBC_OK 表示成功；NSBC_ERR 表示全部重试失败。 */
int cfg_pull_all_from_web(int retries, int delay_ms)
{
    int rc = cfg_pull_all_once(NULL, 0);
    if (rc == NSBC_OK)
        return NSBC_OK;

    int attempt = 1;
    while (attempt <= retries) {
        llog(LOG_INFO, "cfg-sync",
             "pull_all retry %d/%d (waiting %d ms for data.sock)...",
             attempt, retries, delay_ms);
        usleep((useconds_t)delay_ms * 1000);
        rc = cfg_pull_all_once(NULL, 0);
        if (rc == NSBC_OK) return NSBC_OK;
        ++attempt;
    }

    llog(LOG_WARN, "cfg-sync",
         "pull_all failed after %d retries — DB tables NOT loaded from web",
         retries + 1);
    return NSBC_ERR;
}

/* ---- 便捷包装：reload 场景（无 retry，web 肯定已启动）---- */
int cfg_sync_after_reload(void)
{
    return cfg_pull_all_from_web(0, 0);
}

/* ---- 便捷包装：启动场景（有 retry）----
 * 默认 retry 10 次 × 500ms = 总共等 5s */
int cfg_sync_on_startup(void)
{
    return cfg_pull_all_from_web(10, 500);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
