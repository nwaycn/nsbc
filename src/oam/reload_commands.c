/* 配置热加载命令组（REQ-DATA-SVC-003 P0，骨架）
 *
 * 命令注册到 cmd_registry（obs_control.c 的 cmd_reg）：
 *   extensions.reload <json>      CMD_L2_ADMIN
 *   gateways.reload <json>        CMD_L2_ADMIN
 *   trunks.reload <json>          CMD_L2_ADMIN
 *   dialplan-rules.reload <json>  CMD_L2_ADMIN
 *   profiles.reload <json>        CMD_L2_ADMIN  ← sip_profile.c 已有骨架实现
 *   profiles.list                 CMD_L1_MONITOR
 *
 * 每个 reload handler 做三件事：
 *   [1] cJSON_Parse(argv[1]) 解析 JSON 数组
 *   [2] 调对应模块的 _reload_json()（如 ext_table_reload_json）
 *   [3] log_audit("oam", "control-config-reload", "module=... n=...")
 *
 * 失败：cmd_printf 回 ERR + out_err；log_audit 记结果
 * 成功：cmd_printf 回 OK loaded=N
 *
 * cJSON 依赖：假期回来在 CMakeLists.txt 加 cJSON 子模块或 pkg-config
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cmd_registry.h"
#include "log.h"
#include "sip_profile.h"
#include "sip_transport.h"
#include "app_ctx.h"       /* nsbc_get_ctx() — 拿 ctx->sip_tr */
#include "nsbc_common.h"

#include <stdio.h>
#include <string.h>

/* ---- 模块 reload 函数签名（假期回来各模块实现） ----
 * 每个函数：cJSON 解析 → 替换内存表；失败返回 NSBC_ERR + out_err
 * 与 sip_profile.c 的 sip_profiles_reload_json() 完全同模式 */
extern int ext_table_reload_json(const char *json, char *out_err, size_t err_sz);
extern int gw_table_reload_json(const char *json, char *out_err, size_t err_sz);
extern int trunk_table_reload_json(const char *json, char *out_err, size_t err_sz);
extern int dp_rules_reload_json(const char *json, char *out_err, size_t err_sz);
extern int queue_table_reload_json(const char *json, char *out_err, size_t err_sz);

/* ---- 通用 reload handler（5 个命令共享） ---- */

#define RELOAD_JSON_MAX (256 * 1024)  /* 单表最大 JSON：256KB 足够 1000 条 */

static int cmd_reload_generic(
    cmd_ctx_t *c,
    int argc, char **argv,
    void *ud)
{
    const char *module = (const char *)ud;  /* reload name（审计用） */
    if (!c || !c->io_printf) return NSBC_INVAL;
    if (argc < 2) {
        c->io_printf(c->io_ud, "ERR usage: %s.reload <json>\n", module);
        return NSBC_INVAL;
    }
    if (strlen(argv[1]) > RELOAD_JSON_MAX) {
        c->io_printf(c->io_ud, "ERR json too large (max %d)\n", RELOAD_JSON_MAX);
        return NSBC_INVAL;
    }

    /* 假期回来：调对应模块的 _reload_json()
     *   int rc = ext_table_reload_json(argv[1], err, sizeof(err)); */
    char err[256] = {0};
    int rc;
    if (strcmp(module, "extensions") == 0)       rc = ext_table_reload_json(argv[1], err, sizeof(err));
    else if (strcmp(module, "gateways") == 0)    rc = gw_table_reload_json(argv[1], err, sizeof(err));
    else if (strcmp(module, "trunks") == 0)     rc = trunk_table_reload_json(argv[1], err, sizeof(err));
    else if (strcmp(module, "dialplan-rules") == 0) rc = dp_rules_reload_json(argv[1], err, sizeof(err));
    else if (strcmp(module, "profiles") == 0)   rc = sip_profiles_reload_json(argv[1], err, sizeof(err));
    else if (strcmp(module, "queues") == 0)    rc = queue_table_reload_json(argv[1], err, sizeof(err));
    else { rc = NSBC_INVAL; snprintf(err, sizeof(err), "unknown module %s", module); }

    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK %s.reload success\n", module);
        log_audit("oam", "control-config-reload", "module=%s result=ok", module);

        /* ---- profiles.reload 成功后：同步 transport 监听端口 ----
         * profile 只存储配置数据不拥有 socket；transport 才是唯一监听者。
         * Web UI 改了 external profile 端口（5080），这里必须把新端口
         * 追加到 transport，否则新端口没人监听 → external profile 收不到包。 */
        if (strcmp(module, "profiles") == 0) {
            nsbc_ctx_t *ctx = nsbc_get_ctx();
            if (ctx && ctx->sip_tr) {
                int src = sip_tr_sync_from_profiles((sip_transport_t *)ctx->sip_tr);
                llog(LOG_INFO, "oam", "profiles.reload → sip_tr_sync_from_profiles rc=%d", src);
            } else {
                llog(LOG_WARN, "oam", "profiles.reload: sip_tr not ready, skip listener sync");
            }
        }
    } else {
        c->io_printf(c->io_ud, "ERR %s.reload failed: %s\n", module, err[0] ? err : "unknown");
        log_audit("oam", "control-config-reload",
                  "module=%s result=err rc=%d", module, rc);
    }
    return rc;
}

/* ---- 5 个命令注册 ---- */

static int cmd_extensions_reload(cmd_ctx_t *c, int argc, char **argv, void *ud)
    { (void)ud; return cmd_reload_generic(c, argc, argv, (void *)"extensions"); }
static int cmd_gateways_reload(cmd_ctx_t *c, int argc, char **argv, void *ud)
    { (void)ud; return cmd_reload_generic(c, argc, argv, (void *)"gateways"); }
static int cmd_trunks_reload(cmd_ctx_t *c, int argc, char **argv, void *ud)
    { (void)ud; return cmd_reload_generic(c, argc, argv, (void *)"trunks"); }
static int cmd_dialplan_rules_reload(cmd_ctx_t *c, int argc, char **argv, void *ud)
    { (void)ud; return cmd_reload_generic(c, argc, argv, (void *)"dialplan-rules"); }
static int cmd_profiles_reload(cmd_ctx_t *c, int argc, char **argv, void *ud)
    { (void)ud; return cmd_reload_generic(c, argc, argv, (void *)"profiles"); }
static int cmd_queues_reload(cmd_ctx_t *c, int argc, char **argv, void *ud)
    { (void)ud; return cmd_reload_generic(c, argc, argv, (void *)"queues"); }

static int cmd_profiles_list(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)argc; (void)argv; (void)ud;
    if (!c || !c->io_printf) return NSBC_INVAL;
    if (sip_profile_count() == 0) {
        c->io_printf(c->io_ud, "profiles=0\n");
        return NSBC_OK;
    }
    c->io_printf(c->io_ud,
                 "%-16s %-8s %-16s %-8s %-8s %-8s %-10s\n",
                 "NAME", "ENABLED", "SIP_IP", "UDP", "TCP", "TLS", "STATE");
    for (unsigned i = 0; i < sip_profile_count(); ++i) {
        sip_profile_t *p = sip_profile_at(i);
        if (!p) continue;
        c->io_printf(c->io_ud,
                     "%-16s %-8d %-16s %-8s %-8s %-8s %-10s\n",
                     p->name, p->enabled, p->sip_ip,
                     p->enable_udp  ? (p->sip_udp_port  > 0 ? (const char *)"on"  : (const char *)"0")  : (const char *)"off",
                     p->enable_tcp  ? (p->sip_tcp_port  > 0 ? (const char *)"on"  : (const char *)"0")  : (const char *)"off",
                     p->enable_tls  ? (p->sip_tls_port  > 0 ? (const char *)"on"  : (const char *)"0")  : (const char *)"off",
                     (p->state == SIP_PROFILE_OK ? "OK" :
                      p->state == SIP_PROFILE_ERROR ? "ERROR" :
                      p->state == SIP_PROFILE_DISABLED ? "DISABLED" : "RELOADING"));
    }
    c->io_printf(c->io_ud, "-- %u profiles\n", sip_profile_count());
    return NSBC_OK;
}

/* ---- 模块 init：注册到 cmd_reg（obs_control.c 持有） ---- */

void reload_commands_register(cmd_reg_t *reg)
{
    if (!reg) return;
    cmd_desc_t descs[] = {
        { "extensions.reload",     "Reload extensions from JSON (Web→core push)",     CMD_L2_ADMIN, cmd_extensions_reload, NULL },
        { "gateways.reload",       "Reload gateways from JSON",                       CMD_L2_ADMIN, cmd_gateways_reload,   NULL },
        { "trunks.reload",         "Reload trunks from JSON (trunk_t)",               CMD_L2_ADMIN, cmd_trunks_reload,     NULL },
        { "dialplan-rules.reload", "Reload dialplan rules from JSON (dp_rule)",       CMD_L2_ADMIN, cmd_dialplan_rules_reload, NULL },
        { "queues.reload",         "Reload ACD queues from JSON (cc_mgr)",            CMD_L2_ADMIN, cmd_queues_reload,     NULL },
        { "profiles.reload",       "Reload SIP profiles RCU atomic replace",          CMD_L2_ADMIN, cmd_profiles_reload,   NULL },
        { "profiles.list",         "List loaded SIP profiles",                        CMD_L1_MONITOR, cmd_profiles_list,    NULL },
    };
    for (size_t i = 0; i < sizeof(descs)/sizeof(descs[0]); ++i)
        cmd_register(reg, &descs[i]);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
