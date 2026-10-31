/* config_sync — C core ↔ Rust web 配置全量拉取
 *
 * 两条入口：
 *   1) cmd_reload（obs_control.c）— YAML reload 成功后拉 DB 同步
 *   2) outreg_init / signaling init — 启动时 retry 拉 DB 表
 *
 * 协议（CMD_REQ）:
 *   C → Web: {"cmd":"cfg.pull_all"}
 *   Web → C: {"ok":true, "trunks":[...], "dialplan_rules":[...], "sip_profiles":[...]}
 *
 * 各模块 reload_json 函数期望的 JSON 格式：
 *   trunk_table_reload_json  — 纯 array 或 {"trunks":[...]}
 *   dp_rules_reload_json      — 纯 array
 *   sip_profiles_reload_json  — 纯 array
 * 本模块 cJSON_PrintUnformatted 把返回值的各字段单独序列化成纯 array 后喂进去。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nsbc_common.h"
#include "uDS_client.h"
#include "route_table.h"
#include "dialplan.h"
#include "sip_profile.h"
#include "app_ctx.h"   /* nsbc_get_ctx() — reload 后打印 trunk 数 */
#include "log.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- 外部 reload_json 函数（stub_mods.c 已实现）---- */
extern int trunk_table_reload_json(const char *json, char *out_err, size_t err_sz);
extern int dp_rules_reload_json(const char *json, char *out_err, size_t err_sz);
extern int sip_profiles_reload_json(const char *json, char *out_err, size_t err_sz);

/* ---- 从 cJSON Object 提取 array 节点并序列化成纯 JSON array ----
 * 失败返回 NULL；调用方负责 free()。 */
static char *json_extract_array_str(const cJSON *root, const char *key)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!arr || !cJSON_IsArray(arr))
        return NULL;
    return cJSON_PrintUnformatted(arr);
}

/* ---- 从返回的 JSON 里取 ok 字段 ---- */
static int json_get_ok(const cJSON *root)
{
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    return ok && cJSON_IsTrue(ok);
}

/* ---- 单次 pull_all（不 retry）----
 * 返回 NSBC_OK 表示至少拉到一个模块；NSBC_ERR 表示整个请求失败
 * （data.sock 未连接 / DB 未就绪 / JSON 解析失败）。 */
static int cfg_pull_all_once(char *out_err, size_t err_sz)
{
    /* 1. 发请求 */
    static const char req[] = "{\"cmd\":\"cfg.pull_all\"}";
    /* 256KB — Rust 侧返回 trunks + dialplan_rules + sip_profiles 三个表，
     * 按 200 trunk（平均 500B 含 custom_headers）+ 300 dp rule + 50 profile ≈ 200KB 估
     * uds_request 内部 resp_cap 会截断，给足空间避免半截 JSON */
    char resp[256 * 1024];
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
    if (!json_get_ok(root)) {
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

    /* --- trunks --- */
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
            llog(LOG_INFO, "cfg-sync", "pull_all: trunks field missing in response");
        }
    }

    /* --- dialplan_rules --- */
    {
        char *arr = json_extract_array_str(root, "dialplan_rules");
        if (arr) {
            char err[128] = {0};
            int rc2 = dp_rules_reload_json(arr, err, sizeof(err));
            if (rc2 == NSBC_OK) {
                llog(LOG_INFO, "cfg-sync", "pull_all: dialplan_rules OK");
                any_loaded = 1;
            } else {
                llog(LOG_WARN, "cfg-sync", "pull_all: dialplan_rules reload failed: %s", err);
            }
        }
        free(arr);
    }

    /* --- sip_profiles --- */
    {
        char *arr = json_extract_array_str(root, "sip_profiles");
        if (arr) {
            char err[128] = {0};
            int rc2 = sip_profiles_reload_json(arr, err, sizeof(err));
            if (rc2 == NSBC_OK) {
                llog(LOG_INFO, "cfg-sync", "pull_all: sip_profiles OK");
                any_loaded = 1;
            } else {
                llog(LOG_WARN, "cfg-sync", "pull_all: sip_profiles reload failed: %s", err);
            }
        }
        free(arr);
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
