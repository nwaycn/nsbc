/* P0 运行时 mod/ 存根 —— 签名严格来自 .h API 头 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "app_ctx.h"
#include "dialplan.h"
#include "app_registry.h"
#include "endpoints.h"
#include "route_table.h"
#include "recording.h"
#include "outbound_reg.h"
#include "recorder.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#include <cjson/cJSON.h>

/* stub_mods.c: out_err 路径 snprintf 截断保护
 * snprintf 返回值 >= sizeof(dst) 时表示输出被截断，
 * 这里统一追加 "..." 确保错误消息不被静默截断，影响运维诊断 */
static inline void _snprintf_err(char *dst, size_t dst_sz, const char *fmt, ...) {
    if (!dst || dst_sz == 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, dst_sz, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= dst_sz) {
        /* 截断或编码错误 → 末尾写 "..." 提示 */
        size_t keep = dst_sz >= 4 ? dst_sz - 4 : 0;
        if (keep > 0) snprintf(dst + keep, dst_sz - keep, "...");
        llog(LOG_WARN, "cfg", "stub_mods: err_msg truncated — original dst=%zu fmt=%.32s",
             dst_sz, fmt);
    }
}

/* dialplan（dialplan.c）、route_table（route_table.c）、endpoints（endpoints.c）
 * 均已入编真实实现（见 Makefile / builtin.c）。 */

/* dp 上下文访问器（core/signaling.c 提供，core 层内跨文件） */
extern dp_ctx_t *sig_dp_ctx(void);

/* —— recording 存根（recording.h + recorder.h 正确签名） ——
 * §17：src/mod/recording/recording.c + src/media/rec/recorder.c 已纳入编译（见 Makefile），
 * 本块存根不再需要，用 #if 0 禁用避免 duplicate symbol 链接错误
 * 保留原因：API 签名参考（未来 recording 模块重构时对照）；稳定后可删除 */
#if 0
int recording_apply_dp_action(rec_mgr_t *mgr, uint8_t rec_action,
                              const char *callid, const char *reason,
                              const char *rec_path_template,
                              const char *caller, const char *callee,
                              char *rec_path_out, size_t path_cap) {
    (void)mgr; (void)rec_action; (void)callid; (void)reason;
    (void)rec_path_template; (void)caller; (void)callee;
    (void)rec_path_out; (void)path_cap; return 0;
}
int recording_feed_frame(const char *callid, int leg, const media_frame_t *frm) {
    (void)callid; (void)leg; (void)frm; return 0;
}
uint8_t recording_parse_action(const char *s) { (void)s; return 0; }
rec_sess_t *recording_find_by_callid(const char *callid, int leg) { (void)callid; (void)leg; return NULL; }

int  rec_open(rec_mgr_t *m, const rec_open_t *req, rec_sess_t **out) {
    (void)m; (void)req; if (out) *out = NULL; return 0;
}
int  rec_stop(rec_sess_t *s, char *path_out, size_t path_cap, uint64_t *bytes_out) {
    (void)s; (void)path_out; (void)path_cap; (void)bytes_out; return 0;
}
int  rec_feed_raw(rec_sess_t *s, int leg, const void *payload, size_t len,
                  int payload_type) {
    (void)s; (void)leg; (void)payload; (void)len; (void)payload_type; return 0;
}
#endif  /* §17: 存根已由真实实现取代 */

/* endpoints 已由 src/mod/endpoints/endpoints.c 提供真实实现；
 * outreg 已由 src/mod/outbound_reg/outbound_reg.c 提供真实实现 */

/* obs_control 已由 src/oam/obs_control.c 提供真实实现 */

/* ======================================================================
 * 配置热加载（REQ-DATA-SVC-003）：Web DB 全量推送 → 整表替换
 *
 * 数据源：Web 管理系统数据库（sqlite/postgres/mysql）经 control.sock
 * 推送 JSON 数组；命令面见 oam/reload_commands.c。
 *
 * 并发模型：
 *  - obs_control 命令分发当前单线程；模块级 g_reload_lock 兜底防未来重入
 *  - 旧表释放用"两代保留"：换表后上一代表指针存 g_*_old[slot]，
 *    下一次 reload 才 free——数据面（dp_execute/rt_select 无锁读）
 *    在换表瞬间持有的旧表指针至少再安全一个完整的 reload 周期
 *
 * ext_table_reload_json：src/mod/endpoints/endpoints.c 真实现
 * sip_profiles_reload_json：src/sip/sip_profile.c 真实现
 * ====================================================================== */

static pthread_mutex_t g_reload_lock = PTHREAD_MUTEX_INITIALIZER;
static route_table_t *g_rt_old[2];
static dp_table_t *g_dp_old[2];
static int g_rt_old_idx;
static int g_dp_old_idx;

/* ---- trunks.reload：JSON → trunk_t[] → 全新 route_table 原子换 ---- */

/* Web 侧 bool_i64 序列化：enabled/immediate 等布尔字段输出 1/0 数字
 * （也接受原生 true/false）；既非 bool 也非 number 时返回 def */
static int json_truthy(const cJSON *j, int def)
{
    if (!j)
        return def;
    if (cJSON_IsBool(j))
        return cJSON_IsTrue(j) ? 1 : 0;
    if (cJSON_IsNumber(j))
        return j->valueint != 0;
    return def;
}

static int trunk_json_parse_one(cJSON *obj, trunk_t *tr,
                                char *out_err, size_t err_sz, int idx)
{
    memset(tr, 0, sizeof(*tr));
    cJSON *j;

    j = cJSON_GetObjectItem(obj, "name");
    if (!j || !cJSON_IsString(j) || !j->valuestring[0]) {
        _snprintf_err(out_err, err_sz, "trunk[%d] missing name", idx);
        return NSBC_ERR;
    }
    snprintf(tr->name, sizeof(tr->name), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "kind");
    if (j && cJSON_IsString(j) && j->valuestring[0]) {
        if (strcmp(j->valuestring, "register") == 0)
            tr->kind = TRUNK_REGISTER;
        else if (strcmp(j->valuestring, "auth_out") == 0)
            tr->kind = TRUNK_AUTH_OUT;
        else
            tr->kind = TRUNK_STATIC_PEER; /* static_peer/未知默认静态 */
    } else if (j && cJSON_IsNumber(j)) {
        tr->kind = (trunk_kind_t)j->valueint;
        if ((int)tr->kind < 0 || tr->kind > TRUNK_AUTH_OUT)
            tr->kind = TRUNK_STATIC_PEER;
    }

    j = cJSON_GetObjectItem(obj, "proto");
    if (j && cJSON_IsNumber(j))
        tr->proto = j->valueint;

    j = cJSON_GetObjectItem(obj, "host");
    if (!j || !cJSON_IsString(j) || !j->valuestring[0]) {
        _snprintf_err(out_err, err_sz, "trunk[%d]=%s missing host", idx, tr->name);
        return NSBC_ERR;
    }
    snprintf(tr->host, sizeof(tr->host), "%s", j->valuestring);

    tr->port = 5060;
    j = cJSON_GetObjectItem(obj, "port");
    if (j && cJSON_IsNumber(j) && j->valueint > 0 && j->valueint <= 65535)
        tr->port = (uint16_t)j->valueint;

    j = cJSON_GetObjectItem(obj, "auth_user");
    if (j && cJSON_IsString(j))
        snprintf(tr->auth_user, sizeof(tr->auth_user), "%s", j->valuestring);
    j = cJSON_GetObjectItem(obj, "realm");
    if (j && cJSON_IsString(j))
        snprintf(tr->realm, sizeof(tr->realm), "%s", j->valuestring);
    j = cJSON_GetObjectItem(obj, "codec_set");
    if (j && cJSON_IsString(j))
        snprintf(tr->codec_set, sizeof(tr->codec_set), "%s", j->valuestring);
    j = cJSON_GetObjectItem(obj, "profile_name");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->profile_name, sizeof(tr->profile_name), "%s", j->valuestring);

    tr->weight = 1;
    j = cJSON_GetObjectItem(obj, "weight");
    if (j && cJSON_IsNumber(j) && j->valueint > 0)
        tr->weight = j->valueint;
    tr->priority = 1;
    j = cJSON_GetObjectItem(obj, "priority");
    if (j && cJSON_IsNumber(j) && j->valueint > 0)
        tr->priority = j->valueint;
    tr->max_channels = 0;
    j = cJSON_GetObjectItem(obj, "max_channels");
    if (j && cJSON_IsNumber(j) && j->valueint > 0)
        tr->max_channels = j->valueint;

    tr->enabled = 1;
    j = cJSON_GetObjectItem(obj, "enabled");
    if (j && !json_truthy(j, 1))
        tr->enabled = 0;

    /* === FreeSWITCH gateway 兼容 + 用户新增字段 ===
     * 专家 3.4：显式非法值（如 expire_seconds=0 或 <60）要记 warning 日志；
     * 空字符串判定：strlen() == 0 与 NULL 语义一致（专家 2.5）。
     * 所有新字段的 JSON key 直接对 Rust Trunk 结构体字段名，Web 端原样序列化。 */

    /* password：明文（Web 侧 UDS 用 Unix socket 传输，安全；HTTP API 不暴露） */
    j = cJSON_GetObjectItem(obj, "password");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->password, sizeof(tr->password), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "from_user");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->from_user, sizeof(tr->from_user), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "from_domain");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->from_domain, sizeof(tr->from_domain), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "proxy");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->proxy, sizeof(tr->proxy), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "register_proxy");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->register_proxy, sizeof(tr->register_proxy), "%s", j->valuestring);

    /* expire_seconds：默认 3600，<60 记 warn 日志但仍使用默认值（专家 3.4） */
    tr->expire_seconds = 3600;
    j = cJSON_GetObjectItem(obj, "expire_seconds");
    if (j && cJSON_IsNumber(j) && j->valueint >= 0) {
        if (j->valueint < 60) {
            log_warn("trunk_json_parse",
                     "trunk=%s expire_seconds=%d < 60s, using default 3600",
                     tr->name, j->valueint);
        } else {
            tr->expire_seconds = (uint32_t)j->valueint;
        }
    }

    /* retry_seconds：默认 5，0 合法（FS 语义=不重试，退避直接到 30min 封顶） */
    tr->retry_seconds = 5;
    j = cJSON_GetObjectItem(obj, "retry_seconds");
    if (j && cJSON_IsNumber(j) && j->valueint >= 0)
        tr->retry_seconds = (uint32_t)j->valueint;

    j = cJSON_GetObjectItem(obj, "caller_id_in_from");
    tr->caller_id_in_from = (uint8_t)json_truthy(j, 0);

    j = cJSON_GetObjectItem(obj, "contact_params");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->contact_params, sizeof(tr->contact_params), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "extension_in_contact");
    tr->extension_in_contact = (uint8_t)json_truthy(j, 0);

    j = cJSON_GetObjectItem(obj, "ping_seconds");
    tr->ping_seconds = (uint32_t)(j && cJSON_IsNumber(j) && j->valueint >= 0 ? j->valueint : 0);

    j = cJSON_GetObjectItem(obj, "inbound_extension");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->inbound_extension, sizeof(tr->inbound_extension), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "cid_type");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->cid_type, sizeof(tr->cid_type), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "rfc5626");
    tr->rfc5626_enabled = (uint8_t)json_truthy(j, 0);

    j = cJSON_GetObjectItem(obj, "reg_id");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->reg_id, sizeof(tr->reg_id), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "peer_ip");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->peer_ip, sizeof(tr->peer_ip), "%s", j->valuestring);

    /* 用户新增：号码池 */
    j = cJSON_GetObjectItem(obj, "caller_id_pool");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->caller_id_pool, sizeof(tr->caller_id_pool), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "per_number_max_channels");
    tr->per_number_max_channels =
        (uint32_t)(j && cJSON_IsNumber(j) && j->valueint >= 0 ? j->valueint : 0);

    /* === 3GPP IMS 自动头注入（REQ-TRUNK-IMS-001） === */
    j = cJSON_GetObjectItem(obj, "ims_enabled");
    tr->ims_enabled = (uint8_t)json_truthy(j, 0);

    j = cJSON_GetObjectItem(obj, "ims_domain");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->ims_domain, sizeof(tr->ims_domain), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "ims_auto_privacy");
    tr->ims_auto_privacy = (uint8_t)json_truthy(j, 0);

    j = cJSON_GetObjectItem(obj, "ims_auto_pani");
    tr->ims_auto_pani = (uint8_t)json_truthy(j, 0);

    j = cJSON_GetObjectItem(obj, "ims_pani_value");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(tr->ims_pani_value, sizeof(tr->ims_pani_value), "%s", j->valuestring);

    return NSBC_OK;
}

/* 解析单个 trunk_group JSON 对象 → trunk_group_t
 * sticky_by_caller: 可选，默认 0；members: 字符串数组 */
static int group_json_parse_one(cJSON *obj, trunk_group_t *g,
                                char *out_err, size_t err_sz, int idx)
{
    memset(g, 0, sizeof(*g));
    cJSON *j;

    j = cJSON_GetObjectItem(obj, "name");
    if (!j || !cJSON_IsString(j) || !j->valuestring[0]) {
        _snprintf_err(out_err, err_sz, "group[%d] missing name", idx);
        return NSBC_ERR;
    }
    snprintf(g->name, sizeof(g->name), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "members");
    if (!j || !cJSON_IsArray(j)) {
        _snprintf_err(out_err, err_sz, "group[%d]=%s missing members array", idx, g->name);
        return NSBC_ERR;
    }
    int nm = cJSON_GetArraySize(j);
    if (nm <= 0 || nm > RT_GROUP_MAX) {
        _snprintf_err(out_err, err_sz, "group[%d]=%s members=%d (1..%d)",
                 idx, g->name, nm, RT_GROUP_MAX);
        return NSBC_ERR;
    }
    for (int i = 0; i < nm; ++i) {
        cJSON *mj = cJSON_GetArrayItem(j, i);
        if (!mj || !cJSON_IsString(mj) || !mj->valuestring[0]) {
            _snprintf_err(out_err, err_sz, "group[%d]=%s members[%d] invalid", idx, g->name, i);
            return NSBC_ERR;
        }
        snprintf(g->members[i], sizeof(g->members[i]), "%s", mj->valuestring);
    }
    g->nmembers = nm;

    j = cJSON_GetObjectItem(obj, "sticky_by_caller");
    g->sticky_by_caller = (uint8_t)json_truthy(j, 0);

    return NSBC_OK;
}

int trunk_table_reload_json(const char *json, char *out_err, size_t err_sz)
{
    if (!json || !*json) {
        if (out_err) _snprintf_err(out_err, err_sz, "empty json");
        return NSBC_INVAL;
    }
    nsbc_ctx_t *ctx = nsbc_get_ctx();
    if (!ctx || !ctx->rt) {
        if (out_err) _snprintf_err(out_err, err_sz, "route_table not ready");
        return NSBC_ERR;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        if (out_err) _snprintf_err(out_err, err_sz, "invalid json: parse failed");
        return NSBC_INVAL;
    }

    /* 支持两种格式：
     *   旧格式（向后兼容）：纯 array of trunk → root 本身就是 trunks
     *   新格式（扩展）：object { "trunks": [...], "groups": [...] }
     *     两者各自可选，但 object 至少要有一个 */
    cJSON *trunks_arr = NULL;
    cJSON *groups_arr = NULL;

    if (cJSON_IsArray(root)) {
        trunks_arr = root;                   /* 纯 array → 所有 item 当 trunk */
    } else if (cJSON_IsObject(root)) {
        trunks_arr = cJSON_GetObjectItem(root, "trunks");
        groups_arr = cJSON_GetObjectItem(root, "groups");
        if (!trunks_arr && !groups_arr) {
            if (out_err) _snprintf_err(out_err, err_sz,
                                  "invalid json: object needs 'trunks' or 'groups'");
            cJSON_Delete(root);
            return NSBC_INVAL;
        }
    } else {
        if (out_err) _snprintf_err(out_err, err_sz,
                              "invalid json: expected array or object");
        cJSON_Delete(root);
        return NSBC_INVAL;
    }

    /* 建新表（含 DNS 预解析，rt_trunk_add 内做）；失败不碰线上表 */
    route_table_t *nrt = NULL;
    if (rt_create(&nrt) != NSBC_OK) {
        cJSON_Delete(root);
        if (out_err) _snprintf_err(out_err, err_sz, "rt_create OOM");
        return NSBC_NOMEM;
    }

    int  loaded = 0;
    int  pushed_n = 0;
    /* 同时收集 TRUNK_REGISTER 类型 trunk → 传给 outreg_reload_from_route_table */
#define OUTREG_MAX 64
    trunk_t reg_list[OUTREG_MAX];
    size_t  reg_cnt = 0;

    /* ---- 解析 trunks ---- */
    if (trunks_arr) {
        int n = cJSON_GetArraySize(trunks_arr);
        pushed_n = n;
        for (int i = 0; i < n; ++i) {
            cJSON *obj = cJSON_GetArrayItem(trunks_arr, i);
            if (!obj || !cJSON_IsObject(obj)) {
                if (out_err) _snprintf_err(out_err, err_sz, "trunk[%d] not object", i);
                goto rollback;
            }
            trunk_t tr;
            if (trunk_json_parse_one(obj, &tr, out_err, err_sz, i) != NSBC_OK)
                goto rollback;
            if (!tr.enabled)
                continue; /* 停用中继不入表 */
            if (rt_trunk_add(nrt, &tr) != NSBC_OK) {
                if (out_err) _snprintf_err(out_err, err_sz,
                                     "trunk[%d]=%s add failed", i, tr.name);
                goto rollback;
            }
            ++loaded;
            if (tr.kind == TRUNK_REGISTER && reg_cnt < OUTREG_MAX) {
                reg_list[reg_cnt++] = tr;
            } else if (tr.kind == TRUNK_REGISTER) {
                llog(LOG_WARN, "outreg",
                     "trunk_table_reload: register trunk overflow, skip %s", tr.name);
            }
        }
    }

    /* ---- 解析 groups（新格式才有；旧格式 groups_arr=NULL，跳过）---- */
    int grp_loaded = 0;
    if (groups_arr) {
        int ng = cJSON_GetArraySize(groups_arr);
        for (int i = 0; i < ng; ++i) {
            cJSON *obj = cJSON_GetArrayItem(groups_arr, i);
            if (!obj || !cJSON_IsObject(obj)) {
                if (out_err) _snprintf_err(out_err, err_sz, "group[%d] not object", i);
                goto rollback;
            }
            trunk_group_t g;
            if (group_json_parse_one(obj, &g, out_err, err_sz, i) != NSBC_OK)
                goto rollback;
            if (rt_group_add(nrt, &g) != NSBC_OK) {
                if (out_err) _snprintf_err(out_err, err_sz,
                                     "group[%d]=%s add failed", i, g.name);
                goto rollback;
            }
            ++grp_loaded;
        }
    }

    cJSON_Delete(root);

    /* 原子换表 + 两代保留 */
    pthread_mutex_lock(&g_reload_lock);
    route_table_t *prev =
        __atomic_exchange_n(&ctx->rt, nrt, __ATOMIC_ACQ_REL);
    route_table_t *dead = g_rt_old[g_rt_old_idx];
    g_rt_old[g_rt_old_idx] = prev;
    g_rt_old_idx ^= 1;
    pthread_mutex_unlock(&g_reload_lock);
    if (dead)
        rt_destroy(dead);

    /* 关键：换表后立即通知 outreg 做增量 reload（打通 Web UDS reload 链路，P0 核心） */
    if (reg_cnt > 0) {
        outreg_reload_from_route_table(reg_list, reg_cnt);
    } else {
        /* 新表没有任何 register trunk → 清空 outreg（相当于 unreg 全部） */
        outreg_reload_from_route_table(NULL, 0);
    }

    llog(LOG_NOTICE, "route",
         "trunks reload: %d active (%d pushed, %d groups)",
         loaded, pushed_n, grp_loaded);
    return NSBC_OK;

rollback:
    rt_destroy(nrt);
    cJSON_Delete(root);
    return NSBC_ERR;
}

/* ---- dialplan-rules.reload：JSON → dp_rule_t[] → dp_ctx_publish ---- */

/* match_expr → pm_pattern（统一走 pm_init，REGEX 当场编译）：
 *   "=1001"  精确      "_9X." 数字通配（'_' 前缀丢弃）
 *   "%^9+$"  正则（显式 % 前缀）   "[3:11]" 长度区间
 *   裸 "^1\d{2}$" / "^51350820$" → 自动识别为 REGEX（含 ^ $ \d \w [...] ? * + | () {} 等 regex 元字符）
 *   裸 "1001" 默认精确 */
static int dp_parse_match(const char *expr, dp_rule_t *r)
{
    size_t n = strlen(expr);
    if (n == 0 || n >= sizeof(r->match.pattern))
        return NSBC_INVAL;

    if (expr[0] == '=')
        return pm_init(&r->match, PM_EXACT, expr + 1, 0, 0);
    if (expr[0] == '%')
        return pm_init(&r->match, PM_REGEX, expr + 1, 0, 255);
    if (expr[0] == '[') {
        /* "[3:11]"：min:max，pattern 本体为空（无附加数字前缀） */
        int mn = 0, mx = 0;
        if (sscanf(expr, "[%d:%d]", &mn, &mx) != 2 ||
            mn < 0 || mx > 255 || mn > mx)
            return NSBC_INVAL;
        return pm_init(&r->match, PM_RANGE, "", (uint8_t)mn, (uint8_t)mx);
    }
    if (expr[0] == '_')
        return pm_init(&r->match, PM_DIGIT, expr + 1, 0, 0);

    /* §19 自动 regex 检测：裸表达式含 regex 元字符 → 当 REGEX 编译
     * 支持 Web 管理端 / DB 直接存标准 regex（^...$ / \d / [...] / | / ? * + 等）
     * 不必强迫前端加 % 前缀 */
    const char *p = expr;
    int looks_regex = 0;
    for (; *p; ++p) {
        switch (*p) {
        case '^': case '$': case '\\': case '[': case ']':
        case '|': case '?': case '*': case '+': case '(': case ')': case '{': case '}':
            looks_regex = 1; break;
        }
        if (looks_regex) break;
    }
    if (looks_regex) {
        /* PCRE2 原生支持 Perl 风格（\d \w \b \A \Z 等），直接传用户写的 pattern。
         * pm_init 内部加了 PCRE2_ANCHORED | PCRE2_ENDANCHORED → 自动整串匹配，
         * 所以裸 "^1\d{2}$" / "1\d{2}" / "1\d+" 都能正确匹配完整 called 号 */
        return pm_init(&r->match, PM_REGEX, expr, 0, 255);
    }

    /* 纯数字串（如 "1001"）或通配（如 "9XX" "7N."）→ 数字通配/精确 */
    for (p = expr; *p; ++p) {
        if (!isdigit((unsigned char)*p) && *p != 'X' && *p != 'N' && *p != 'Z' && *p != '.') {
            /* 含其他字母 → 当 DIGIT 通配符处理（容错） */
            return pm_init(&r->match, PM_DIGIT, expr, 0, 0);
        }
    }
    return pm_init(&r->match, PM_EXACT, expr, 0, 0);
}

static int dp_json_parse_one(cJSON *obj, dp_rule_t *r, uint32_t rid,
                             char *out_err, size_t err_sz, int idx)
{
    memset(r, 0, sizeof(*r));
    cJSON *j;

    r->id = rid;
    r->prio = 100;   /* 同层内小值优先；Web 未指定时靠后 */
    r->enabled = 1;

    j = cJSON_GetObjectItem(obj, "layer");
    if (j && cJSON_IsString(j) && j->valuestring[0]) {
        if (strcmp(j->valuestring, "ext") == 0)
            r->layer = DP_LAYER_EXT;
        else if (strcmp(j->valuestring, "biz") == 0)
            r->layer = DP_LAYER_BIZ;
        else if (strcmp(j->valuestring, "out") == 0)
            r->layer = DP_LAYER_OUT;
        else {
            _snprintf_err(out_err, err_sz, "rule[%d] bad layer '%s'",
                     idx, j->valuestring);
            return NSBC_ERR;
        }
    } else if (j && cJSON_IsNumber(j)) {
        r->layer = (dp_layer_t)j->valueint;
        if (r->layer >= DP_LAYER_CNT) {
            _snprintf_err(out_err, err_sz, "rule[%d] bad layer %d", idx,
                     j->valueint);
            return NSBC_ERR;
        }
    }

    j = cJSON_GetObjectItem(obj, "match_expr");
    if (!j || !cJSON_IsString(j) || !j->valuestring[0]) {
        _snprintf_err(out_err, err_sz, "rule[%d] missing match_expr", idx);
        return NSBC_ERR;
    }
    if (dp_parse_match(j->valuestring, r) != NSBC_OK) {
        _snprintf_err(out_err, err_sz, "rule[%d] bad match_expr '%s'",
                 idx, j->valuestring);
        return NSBC_ERR;
    }

    j = cJSON_GetObjectItem(obj, "target");
    if (j && cJSON_IsString(j))
        snprintf(r->target, sizeof(r->target), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "prio");
    if (j && cJSON_IsNumber(j))
        r->prio = j->valueint;
    j = cJSON_GetObjectItem(obj, "strip");
    if (j && cJSON_IsNumber(j) && j->valueint >= 0)
        r->strip = (uint8_t)j->valueint;
    j = cJSON_GetObjectItem(obj, "perm");
    if (j && cJSON_IsNumber(j) && j->valueint >= 0)
        r->perm = (uint8_t)j->valueint;
    j = cJSON_GetObjectItem(obj, "immediate");
    if (j && json_truthy(j, 0))
        r->immediate = 1;
    /* NOTE: perm/immediate 在后续 action_json 块里也会解析 ——
     * action_json 是权威来源（Web UI 所有拨号动作字段都存这里），
     * 上面 top-level 解析只做 legacy/direct-JSON 兜底，action_json 会覆盖它们 */
    j = cJSON_GetObjectItem(obj, "enabled");
    if (j && !json_truthy(j, 1))
        r->enabled = 0;
    j = cJSON_GetObjectItem(obj, "rec_action");
    if (j && cJSON_IsNumber(j))
        r->rec_action = (uint8_t)j->valueint;

    /* REQ-MEDIA-DP-001：媒体模式 top-level fallback（action_json 是权威来源） */
    j = cJSON_GetObjectItem(obj, "media_mode");
    if (j && cJSON_IsNumber(j) && j->valueint >= 0 && j->valueint <= 2)
        r->media_mode = (uint8_t)j->valueint;

    /* ★ 3GPP IMS 协议特性强制覆盖（三态：0=auto 继承 profile, 1=强制开, 2=强制关） */
    j = cJSON_GetObjectItem(obj, "force_prack");
    if (j && cJSON_IsNumber(j) && j->valueint >= 0 && j->valueint <= 2)
        r->force_prack = (uint8_t)j->valueint;
    j = cJSON_GetObjectItem(obj, "force_precondition");
    if (j && cJSON_IsNumber(j) && j->valueint >= 0 && j->valueint <= 2)
        r->force_precondition = (uint8_t)j->valueint;
    j = cJSON_GetObjectItem(obj, "force_timer");
    if (j && cJSON_IsNumber(j) && j->valueint >= 0 && j->valueint <= 2)
        r->force_timer = (uint8_t)j->valueint;
    j = cJSON_GetObjectItem(obj, "force_gruu");
    if (j && cJSON_IsNumber(j) && j->valueint >= 0 && j->valueint <= 2)
        r->force_gruu = (uint8_t)j->valueint;

    /* §18 新增：可选录音路径模板（空串=系统默认） */
    j = cJSON_GetObjectItem(obj, "rec_path_template");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(r->rec_path_template, sizeof(r->rec_path_template), "%s", j->valuestring);

    /* ====== §15 新增：profile_name（强制分区键）+ src_ip_cidr（规则级 IP 准入）======
     * 默认值保证旧 reload JSON（没有这两个 key）自动继承：
     *   profile_name  = "internal"  （旧规则全是 internal 语义）
     *   src_ip_cidr   = 0.0.0.0/0   （默认放行，具体 rule 再收紧）
     * 安全边界在 auth/profile 层，CIDR 是额外闸门而非主闸门。 */
#define DEFAULT_SRC_IP_CIDR "0.0.0.0/0"
    snprintf(r->profile_name, sizeof(r->profile_name), "internal");
    snprintf(r->src_ip_cidr,  sizeof(r->src_ip_cidr),  DEFAULT_SRC_IP_CIDR);
    j = cJSON_GetObjectItem(obj, "profile_name");
    if (j && cJSON_IsString(j) && j->valuestring[0]) {
        snprintf(r->profile_name, sizeof(r->profile_name), "%s", j->valuestring);
    }
    j = cJSON_GetObjectItem(obj, "src_ip_cidr");
    if (j && cJSON_IsString(j) && j->valuestring[0]) {
        snprintf(r->src_ip_cidr, sizeof(r->src_ip_cidr), "%s", j->valuestring);
    }

    /* 可选 action_json：号码变换 + From 改写 + SIP 头重写 + 录音控制。
     * 兼容两种形态——DB 列存 JSON 文本（String，当场解析）或内联对象 */
    j = cJSON_GetObjectItem(obj, "action_json");
    cJSON *aj = NULL;
    if (j && cJSON_IsString(j) && j->valuestring[0]) {
        aj = cJSON_Parse(j->valuestring);
    } else if (j && cJSON_IsObject(j)) {
        aj = j;
    }
    if (aj) {
        cJSON *a;
        /* —— 号码变换 —— */
        a = cJSON_GetObjectItem(aj, "add_prefix");
        if (a && cJSON_IsString(a))
            snprintf(r->add_prefix, sizeof(r->add_prefix), "%s", a->valuestring);
        /* ★ 被叫号码覆盖（与 target 解耦，前端独立字段） */
        a = cJSON_GetObjectItem(aj, "dst_override");
        if (a && cJSON_IsString(a))
            snprintf(r->dst_override, sizeof(r->dst_override), "%s", a->valuestring);
        a = cJSON_GetObjectItem(aj, "clip");
        if (a && cJSON_IsString(a))
            snprintf(r->clip, sizeof(r->clip), "%s", a->valuestring);
        /* FreeSWITCH 风格精细化主叫覆盖（origination_caller_id_number/name） */
        a = cJSON_GetObjectItem(aj, "origination_caller_id_number");
        if (!a) a = cJSON_GetObjectItem(aj, "orig_caller_id_num");
        if (a && cJSON_IsString(a))
            snprintf(r->orig_caller_id_num, sizeof(r->orig_caller_id_num), "%s", a->valuestring);
        a = cJSON_GetObjectItem(aj, "origination_caller_id_name");
        if (!a) a = cJSON_GetObjectItem(aj, "orig_caller_id_name");
        if (a && cJSON_IsString(a))
            snprintf(r->orig_caller_id_name, sizeof(r->orig_caller_id_name), "%s", a->valuestring);
        a = cJSON_GetObjectItem(aj, "perm");
        if (a && cJSON_IsNumber(a) && a->valueint >= 0)
            r->perm = (uint8_t)a->valueint;
        a = cJSON_GetObjectItem(aj, "immediate");
        if (a && json_truthy(a, 0))
            r->immediate = 1;
        a = cJSON_GetObjectItem(aj, "rec_action");
        if (a && cJSON_IsNumber(a))
            r->rec_action = (uint8_t)a->valueint;
        else if (a && cJSON_IsString(a))
            r->rec_action = recording_parse_action(a->valuestring);

        /* —— REQ-MEDIA-DP-001：拨号规则级媒体模式控制 ——
         * 取值：0=auto(默认), 1=bypass(强制绕过), 2=relay(强制转发) */
        a = cJSON_GetObjectItem(aj, "media_mode");
        if (a && cJSON_IsNumber(a) && a->valueint >= 0 && a->valueint <= 2)
            r->media_mode = (uint8_t)a->valueint;
        else if (a && cJSON_IsString(a)) {
            if      (!strcmp(a->valuestring, "bypass")) r->media_mode = 1;
            else if (!strcmp(a->valuestring, "relay"))  r->media_mode = 2;
            else if (!strcmp(a->valuestring, "auto"))   r->media_mode = 0;
        }

        /* —— REQ-DP-SIP-001：SIP 头重写（sip_hdr_rewrite_t hdr_rw） —— */
        #define RW_STR(field, cap) do { \
            cJSON *_f = cJSON_GetObjectItem(aj, #field); \
            if (_f && cJSON_IsString(_f)) \
                snprintf(r->hdr_rw.field, sizeof(r->hdr_rw.field), "%s", _f->valuestring); \
        } while(0)
        RW_STR(sip_invite_from,        128);
        RW_STR(sip_invite_from_uri,    128);
        RW_STR(sip_from_uri,            128);
        RW_STR(sip_from_display,        128);
        RW_STR(sip_invite_from_params,  128);
        RW_STR(sip_invite_to_uri,       128);
        RW_STR(sip_invite_to_params,    128);
        RW_STR(sip_invite_req_uri,      128);
        RW_STR(sip_invite_tel_params,   128);
        RW_STR(sip_invite_contact_params, 128);
        RW_STR(sip_invite_domain,        64);
        RW_STR(sip_invite_params,       256);
        RW_STR(sip_invite_pid_params,   256);
        #undef RW_STR
        /* legacy 别名：clip_from → sip_from_uri（兼容旧 UI 存的字段） */
        {
            cJSON *_cf = cJSON_GetObjectItem(aj, "clip_from");
            if (_cf && cJSON_IsString(_cf) && !r->hdr_rw.sip_from_uri[0])
                snprintf(r->hdr_rw.sip_from_uri, sizeof(r->hdr_rw.sip_from_uri), "%s", _cf->valuestring);
        }

        if (aj != j)
            cJSON_Delete(aj);
    }

    /* ====== REQ-DP-APP-001：app_chain（Application 链）JSON 数组解析 ======
     * 移到 if (aj) 外面！这样 top-level "app_chain" 无论有没有 "action_json" 都能解析。
     * 支持两种形态：
     *   a) top-level "app_chain": [{"app":"playback","data":"welcome.wav"}, ...]
     *   b) action_json 嵌套 "app_chain": [...]（aj 已 delete，不再 fallback）
     * 优先级：top-level 优先（显式配置 > 隐式嵌套）
     *
     * 内存：calloc 分配 → dp_rule_set_app_chain 只存指针不拷贝
     * dp_table_free 会释放 app_chain（见 dialplan.c） */
    {
        cJSON *alt = cJSON_GetObjectItem(obj, "app_chain");
        if (alt && cJSON_IsArray(alt) && cJSON_GetArraySize(alt) > 0) {
            int nn = cJSON_GetArraySize(alt);
            dp_app_action_t *chain =
                (dp_app_action_t *)calloc((size_t)nn, sizeof(*chain));
            if (chain) {
                int valid = 0;
                for (int k = 0; k < nn; ++k) {
                    cJSON *item = cJSON_GetArrayItem(alt, k);
                    if (!item || !cJSON_IsObject(item)) continue;
                    cJSON *app = cJSON_GetObjectItem(item, "app");
                    if (!app || !cJSON_IsString(app) || !app->valuestring[0])
                        continue;                        /* 必须有 app 名 */
                    snprintf(chain[valid].app,
                             sizeof(chain[valid].app), "%s", app->valuestring);
                    cJSON *data = cJSON_GetObjectItem(item, "data");
                    if (data && cJSON_IsString(data) && data->valuestring[0])
                        snprintf(chain[valid].data,
                                 sizeof(chain[valid].data), "%s", data->valuestring);
                    ++valid;
                }
                if (valid > 0) {
                    dp_rule_set_app_chain(r, chain, (size_t)valid);
                } else {
                    free(chain);                          /* 全无效 → 不挂链 */
                }
            }
        }
    }

    return NSBC_OK;
}

int dp_rules_reload_json(const char *json, char *out_err, size_t err_sz)
{
    if (!json || !*json) {
        if (out_err) _snprintf_err(out_err, err_sz, "empty json");
        return NSBC_INVAL;
    }
    dp_ctx_t *dpc = sig_dp_ctx();
    if (!dpc) {
        if (out_err) _snprintf_err(out_err, err_sz, "dp ctx not ready");
        return NSBC_ERR;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsArray(root)) {
        if (out_err) _snprintf_err(out_err, err_sz, "invalid json: expected array");
        if (root) cJSON_Delete(root);
        return NSBC_INVAL;
    }

    int n = cJSON_GetArraySize(root);
    dp_table_t *tbl = calloc(1, sizeof(*tbl));
    if (!tbl || dp_table_init(tbl, (size_t)(n > 0 ? n : 1)) != NSBC_OK) {
        free(tbl);
        cJSON_Delete(root);
        if (out_err) _snprintf_err(out_err, err_sz, "dp table OOM");
        return NSBC_NOMEM;
    }

    int loaded = 0;
    uint32_t rid = 1;
    for (int i = 0; i < n; ++i) {
        cJSON *obj = cJSON_GetArrayItem(root, i);
        if (!obj || !cJSON_IsObject(obj)) {
            if (out_err) _snprintf_err(out_err, err_sz, "rule[%d] not object", i);
            goto rollback;
        }
        cJSON *jid = cJSON_GetObjectItem(obj, "id");
        uint32_t use_id = (jid && cJSON_IsNumber(jid) && jid->valueint > 0)
                              ? (uint32_t)jid->valueint
                              : rid;
        dp_rule_t rule;
        if (dp_json_parse_one(obj, &rule, use_id, out_err, err_sz, i) !=
            NSBC_OK) {
            pm_fini(&rule.match); /* match_expr 已过则可能已编译 */
            goto rollback;
        }
        ++rid;
        if (!rule.enabled) {
            pm_fini(&rule.match);
            continue; /* 停用规则不入表 */
        }
        int arc = dp_rule_add(tbl, &rule);
        /* dp_rule_add 对 PM_REGEX 表内重编译；调用侧副本统一在此释放 */
        pm_fini(&rule.match);
        if (arc != NSBC_OK) {
            if (out_err) _snprintf_err(out_err, err_sz,
                                 "rule[%d] id=%u add failed", i, rule.id);
            goto rollback;
        }
        ++loaded;
    }
    cJSON_Delete(root);
    dp_table_sort(tbl);

    /* 原子发布（old 传非 NULL：dp_ctx_publish 不立即 free，交两代保留） */
    pthread_mutex_lock(&g_reload_lock);
    dp_table_t *prev = NULL;
    if (dp_ctx_publish(dpc, tbl, &prev) != NSBC_OK) {
        pthread_mutex_unlock(&g_reload_lock);
        dp_table_free(tbl);
        if (out_err) _snprintf_err(out_err, err_sz, "dp_ctx_publish failed");
        return NSBC_ERR;
    }
    dp_table_t *dead = g_dp_old[g_dp_old_idx];
    g_dp_old[g_dp_old_idx] = prev;
    g_dp_old_idx ^= 1;
    pthread_mutex_unlock(&g_reload_lock);
    if (dead)
        dp_table_free(dead);

    llog(LOG_NOTICE, "dialplan", "rules reload: %d active (from %d pushed)",
         loaded, n);
    /* §15 debug：dump 每条规则的 profile/match/target，解决 dp_hit=0 时无据可查 */
    for (size_t i = 0; i < tbl->count; ++i) {
        const dp_rule_t *r = &tbl->rules[i];
        llog(LOG_INFO, "dialplan",
             "rule[%zu] id=%u prio=%d prof=%s match='%s' target=%s rec_action=%d app_chain_n=%zu",
             i, r->id, r->prio, r->profile_name,
             r->match.pattern, r->target, r->rec_action, r->app_chain_n);
    }
    return NSBC_OK;

rollback:
    dp_table_free(tbl);
    cJSON_Delete(root);
    return NSBC_ERR;
}

/* gateways.reload：Gateway 是 Web 侧对 Trunk 的逻辑分组标签（name+enabled），
 * Core 端不消费 gateway 表 — trunk.reload 的 route_table 已包含 gateway_name 关联。
 * 返回 OK no-op 避免 Web→Core reload 链路报 ERR。Web DB 增删改 gateway 即时生效，
 * 无需 Core 端同步。 */
int gw_table_reload_json(const char *json, char *out_err, size_t err_sz)
{
    (void)json; (void)out_err; (void)err_sz;
    return NSBC_OK;
}

/* nsbcm_dialplan 定义在 src/mod/dialplans/dialplan.c，本文件不重复定义 */

/* ---- fini 清理：drain reload 链留下的两代 route_table / dp_table ----
 *
 * reload 用"两代保留"（g_rt_old[2] / g_dp_old[2]）：当前代换表后上一代存
 * g_rt_old[idx]，等下次 reload 才 destroy 上上代。进程退出时如果只 reload
 * 过 0-1 次，g_rt_old[] 里还有活着的表没人 destroy → LSan 泄漏 22MB。
 * main.c fini 在 modmgr_fini_all 之后调本函数，统一 drain 所有残留。 */
void stub_mods_reload_fini(void)
{
    pthread_mutex_lock(&g_reload_lock);
    for (int i = 0; i < 2; ++i) {
        if (g_rt_old[i]) {
            rt_destroy(g_rt_old[i]);
            g_rt_old[i] = NULL;
        }
        if (g_dp_old[i]) {
            dp_table_free(g_dp_old[i]);
            g_dp_old[i] = NULL;
        }
    }
    pthread_mutex_unlock(&g_reload_lock);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
