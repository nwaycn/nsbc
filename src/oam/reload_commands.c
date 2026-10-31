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
#include "app_ctx.h"       /* nsbc_get_ctx() — 拿 ctx->sip_tr / ctx->rt */
#include "nsbc_common.h"
#include "ivr.h"
#include "route_table.h"   /* rt_trunk_count / rt_trunk_at — trunks.list */
#include "dialplan.h"      /* dp_oam_get_active — dialplan.show */

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

#define RELOAD_JSON_MAX (32 * 1024 * 1024)  /* 32MB — 支持 90000+ extensions 全量 reload */

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
    else if (strcmp(module, "ivr") == 0)        rc = ivr_menus_reload_json(argv[1], err, sizeof(err));
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
static int cmd_ivr_reload(cmd_ctx_t *c, int argc, char **argv, void *ud)
    { (void)ud; return cmd_reload_generic(c, argc, argv, (void *)"ivr"); }

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

/* ---- trunks.list：遍历 route_table，输出所有已加载 trunk ----
 * 列：NAME / KIND / HOST:PORT / ENABLED / OPEN / IN_USE / PROFILE
 * route_table 由 trunk_table_reload_json 原子替换，OAM 命令读 ctx->rt 即可
 * （命令执行期间不会被替换——reload 走同一条 control.sock 串行命令通道）。 */
static int cmd_trunks_list(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)argc; (void)argv; (void)ud;
    if (!c || !c->io_printf) return NSBC_INVAL;

    nsbc_ctx_t *ctx = nsbc_get_ctx();
    if (!ctx || !ctx->rt) {
        c->io_printf(c->io_ud, "trunks=0 (no route_table)\n");
        return NSBC_OK;
    }

    int n = rt_trunk_count(ctx->rt);
    if (n == 0) {
        c->io_printf(c->io_ud, "trunks=0\n");
        return NSBC_OK;
    }

    c->io_printf(c->io_ud,
                 "%-24s %-12s %-32s %-8s %-6s %-8s %-16s\n",
                 "NAME", "KIND", "HOST:PORT", "ENABLED", "OPEN", "IN_USE", "PROFILE");
    for (int i = 0; i < n; ++i) {
        const trunk_t *tr = rt_trunk_at(ctx->rt, i);
        if (!tr) continue;
        const char *kind = (tr->kind == TRUNK_STATIC_PEER) ? "static" :
                           (tr->kind == TRUNK_REGISTER)    ? "register" :
                           (tr->kind == TRUNK_AUTH_OUT)    ? "auth_out" : "?";
        c->io_printf(c->io_ud,
                     "%-24s %-12s %-16s:%-5u %-8d %-6s %-8lu %-16s\n",
                     tr->name, kind, tr->host, tr->port,
                     tr->enabled,
                     rt_trunk_is_open(ctx->rt, i) ? "open" : "CLOSED",
                     (unsigned long)rt_trunk_in_use(ctx->rt, i),
                     tr->profile_name[0] ? tr->profile_name : "-");
    }
    c->io_printf(c->io_ud, "-- %d trunks\n", n);

    /* 同时列出 trunk groups（如有） */
    int ng = rt_group_count(ctx->rt);
    if (ng > 0) {
        c->io_printf(c->io_ud, "\n%-24s %-8s %s\n", "GROUP", "MEMBERS", "STICKY");
        for (int i = 0; i < ng; ++i) {
            const trunk_group_t *g = rt_group_at(ctx->rt, i);
            if (!g) continue;
            c->io_printf(c->io_ud, "%-24s %-8d %s\n",
                         g->name, g->nmembers,
                         g->sticky_by_caller ? "sticky" : "hash");
        }
        c->io_printf(c->io_ud, "-- %d groups\n", ng);
    }
    return NSBC_OK;
}

/* ---- dialplan.show：遍历 dp_ctx.active，输出所有已加载规则 ----
 * 列：ID / PRIO / PROFILE / MATCH / LAYER / TARGET / STRIP / ADD_PREFIX / ENABLED
 * active 表 RCU 原子读，所有权属 signaling —— 只读不 free。 */
static int cmd_dialplan_show(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)argc; (void)argv; (void)ud;
    if (!c || !c->io_printf) return NSBC_INVAL;

    dp_table_t *tbl = dp_oam_get_active();
    if (!tbl || tbl->count == 0) {
        c->io_printf(c->io_ud, "dialplan_rules=0 (no active table)\n");
        return NSBC_OK;
    }

    c->io_printf(c->io_ud,
                 "%-6s %-6s %-12s %-20s %-6s %-24s %-6s %-12s %-8s\n",
                 "ID", "PRIO", "PROFILE", "MATCH", "LAYER", "TARGET", "STRIP", "ADD_PREFIX", "PERM");
    for (size_t i = 0; i < tbl->count; ++i) {
        const dp_rule_t *r = &tbl->rules[i];
        const char *layer = (r->layer == DP_LAYER_EXT) ? "EXT" :
                            (r->layer == DP_LAYER_OUT) ? "OUT" :
                            (r->layer == DP_LAYER_BIZ) ? "BIZ" : "?";
        /* match pattern：按 pm_type_t 类型加前缀（PREFIX/EXACT/RANGE/DIGIT/REGEX） */
        char match_str[32];
        const char *type_tag =
            (r->match.type == PM_REGEX) ? "re" :
            (r->match.type == PM_PREFIX) ? "pre" :
            (r->match.type == PM_EXACT) ? "eq" :
            (r->match.type == PM_RANGE) ? "rng" :
            (r->match.type == PM_DIGIT) ? "dig" : "?";
        snprintf(match_str, sizeof(match_str), "%s:%s",
                 type_tag, r->match.pattern[0] ? r->match.pattern : "");
        c->io_printf(c->io_ud,
                     "%-6u %-6d %-12s %-20s %-6s %-24s %-6d %-12s %-8u\n",
                     r->id, r->prio,
                     r->profile_name[0] ? r->profile_name : "-",
                     match_str, layer,
                     r->target[0] ? r->target : "-",
                     r->strip,
                     r->add_prefix[0] ? r->add_prefix : "-",
                     r->perm);
    }
    c->io_printf(c->io_ud, "-- %zu rules (active)\n", tbl->count);
    return NSBC_OK;
}

/* ---- no-op reload handler：Core 端暂不消费 route_groups / ha_groups / ivr_menus ----
 * 这些模块的内存结构未来由 Core 自己接管，但当前 Web DB 是权威数据源。
 * 命令仍然注册以便 Web 侧 UDS 链路可通（reload 按钮点了能收到 OK）。 */

static int cmd_reload_noop(cmd_ctx_t *c, int argc, char **argv, void *ud) {
    (void)argv;
    const char *module = (const char *)ud;
    if (!c || !c->io_printf) return NSBC_INVAL;
    if (argc < 2) {
        c->io_printf(c->io_ud, "ERR usage: %s.reload <json>\n", module);
        return NSBC_INVAL;
    }
    /* 日志：reload 请求到达，但 Core 端暂不消费这张表（Web DB 为准） */
    c->io_printf(c->io_ud, "OK %s.reload accepted (no-op — Web DB authoritative)\n", module);
    return NSBC_OK;
}

/* ---- 模块 init：注册到 cmd_reg（obs_control.c 持有） ---- */

void reload_commands_register(cmd_reg_t *reg)
{
    if (!reg) return;
    cmd_desc_t descs[] = {
        /* ---- reload 热加载 ---- */
        { "extensions.reload",     "Reload extensions from JSON (Web→core push)",     "reload", CMD_L2_ADMIN, cmd_extensions_reload, NULL },
        { "gateways.reload",       "Reload gateways from JSON",                       "reload", CMD_L2_ADMIN, cmd_gateways_reload,   NULL },
        { "trunks.reload",         "Reload trunks from JSON (trunk_t)",               "reload", CMD_L2_ADMIN, cmd_trunks_reload,     NULL },
        { "dialplan-rules.reload", "Reload dialplan rules from JSON (dp_rule)",       "reload", CMD_L2_ADMIN, cmd_dialplan_rules_reload, NULL },
        { "queues.reload",         "Reload ACD queues from JSON (cc_mgr)",            "reload", CMD_L2_ADMIN, cmd_queues_reload,     NULL },
        { "profiles.reload",       "Reload SIP profiles RCU atomic replace",          "reload", CMD_L2_ADMIN, cmd_profiles_reload,   NULL },
        { "ha.reload",             "Reload HA groups from JSON (Web DB authoritative)", "reload", CMD_L2_ADMIN, cmd_reload_noop, (void*)"ha" },
        { "ivr.reload",            "Reload IVR menus from JSON (nodes JSON tree)",       "reload", CMD_L2_ADMIN, cmd_ivr_reload,     NULL },
        { "route_groups.reload",   "Reload route groups from JSON (Web DB authoritative)", "reload", CMD_L2_ADMIN, cmd_reload_noop, (void*)"route_groups" },

        /* ---- show 只读查询 ---- */
        { "profiles.list",         "List loaded SIP profiles",                        "show", CMD_L1_MONITOR, cmd_profiles_list,    NULL },
        { "trunks.list",           "List loaded trunks and groups from route_table",  "show", CMD_L1_MONITOR, cmd_trunks_list,      NULL },
        { "dialplan.show",         "List active dialplan rules",                      "show", CMD_L1_MONITOR, cmd_dialplan_show,    NULL },
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
