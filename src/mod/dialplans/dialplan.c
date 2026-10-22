#include "dialplan.h"
#include "dp_sleep.h"
#include "app_registry.h"
#include "module.h"
#include "sip_profile.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#define DP_MAX_NUMBER 32

int dp_ctx_init(dp_ctx_t *ctx)
{
    if (!ctx)
        return NSBC_INVAL;
    ctx->active = NULL;
    /* §7: 非阻塞 sleep 模块 init（幂等） */
    dp_sleep_init();
    return NSBC_OK;
}

void dp_ctx_fini(dp_ctx_t *ctx)
{
    if (!ctx)
        return;
    dp_table_t *t = __atomic_exchange_n(&ctx->active, NULL, __ATOMIC_ACQ_REL);
    dp_table_free(t);
}

int dp_table_init(dp_table_t *t, size_t cap)
{
    if (!t || cap == 0)
        return NSBC_INVAL;
    t->rules = calloc(cap, sizeof(dp_rule_t));
    if (!t->rules)
        return NSBC_NOMEM;
    t->count = 0;
    t->cap = cap;
    return NSBC_OK;
}

void dp_table_free(dp_table_t *t)
{
    if (!t)
        return;
    for (size_t i = 0; i < t->count; ++i) {
        pm_fini(&t->rules[i].match);
        /* REQ-DP-APP-001：释放 reload 时 malloc 的 app_chain 数组 */
        if (t->rules[i].app_chain) {
            free(t->rules[i].app_chain);
            t->rules[i].app_chain = NULL;
            t->rules[i].app_chain_n = 0;
        }
    }
    free(t->rules);
    t->rules = NULL;
    t->count = t->cap = 0;
    free(t);
}

/* rule 为模板：PM_REGEX 的 pcre2_code 所有权转移给表
 * 调用侧模板在 dp_rule_add 成功后会 pm_fini——此处把模板 re 置 NULL，
 * 让调用侧 pm_fini 变成 no-op，避免 double-free / dangling pointer */
int dp_rule_add(dp_table_t *t, dp_rule_t *rule)
{
    if (!t || !rule)
        return NSBC_INVAL;
    if (t->count >= t->cap)
        return NSBC_NOMEM;

    dp_rule_t *dst = &t->rules[t->count];
    *dst = *rule;
    /* 所有权转移：模板的 re 转给表，模板置 NULL 避免后续 pm_fini 释放表内指针 */
    if (dst->match.type == PM_REGEX && dst->match.re != NULL)
        rule->match.re = NULL;
    t->count++;
    return NSBC_OK;
}

/* ====== §15 新增：从 target 前缀自动推断 layer（彻底取代手动 dp_rule_t.layer 配置）====== */
static dp_layer_t dp_infer_layer_from_target(const char *target)
{
    if (!target || !target[0]) return DP_LAYER_EXT;       /* 空 target 默认 EXT */
    /* Advisor 2026-10-12n: 已移除 target[0] == '$' → EXT 的拍脑袋判断！
     * 变量替换完 target 可能变成 gateway:xxx / trunk:xxx 等，替换后再推断 layer。 */
    if (strncmp(target, "extension:", 10) == 0)      return DP_LAYER_EXT;
    if (strncmp(target, "queue:", 6) == 0)           return DP_LAYER_BIZ;
    if (strncmp(target, "ivr:", 4) == 0)            return DP_LAYER_BIZ;
    if (strncmp(target, "vm:", 3) == 0)              return DP_LAYER_BIZ;
    if (strncmp(target, "conference:", 11) == 0)     return DP_LAYER_BIZ;
    if (strncmp(target, "hangup", 6) == 0)           return DP_LAYER_BIZ;  /* hangup 算 BIZ 业务动作 */
    if (strncmp(target, "gateway:", 8) == 0)         return DP_LAYER_OUT;
    if (strncmp(target, "route:", 6) == 0)           return DP_LAYER_OUT;
    if (strncmp(target, "trunk:", 6) == 0)           return DP_LAYER_OUT;
    return DP_LAYER_EXT;  /* fallback */
}

/* Advisor 2026-10-12n: ${N} 变量替换 helper
 * 扫描 in，遇到 ${N} 时用 groups[N] 替换（N 支持 0-9，但只用到 PM_MAX_GROUPS-1）。
 * out 缓冲区大小 outsz，返回写入长度（不含 \0）或 -1 溢出。 */
static int substitute_groups(const char *in, char *out, size_t outsz,
                             const char groups[PM_MAX_GROUPS][PM_GROUP_SZ])
{
    size_t i = 0, o = 0;
    while (in[i] && o < outsz) {
        if (in[i] == '$') {
            if (in[i+1] == '{') {
                /* ${N} — 花括号形式 */
                size_t j = i + 2;
                while (in[j] && in[j] != '}') ++j;
                if (in[j] == '}') {
                    char numbuf[8];
                    size_t nl = j - (i + 2);
                    if (nl >= sizeof(numbuf)) nl = sizeof(numbuf) - 1;
                    memcpy(numbuf, in + i + 2, nl); numbuf[nl] = '\0';
                    int idx = (int)strtol(numbuf, NULL, 10);
                    if (idx >= 0 && idx < PM_MAX_GROUPS && groups[idx][0]) {
                        const char *val = groups[idx];
                        size_t vl = strlen(val);
                        if (o + vl >= outsz) return -1;  /* 溢出 */
                        memcpy(out + o, val, vl); o += vl;
                    }
                    i = j + 1;
                    continue;
                }
            } else if (in[i+1] >= '0' && in[i+1] <= '9') {
                /* $N — 标准 Perl 正则捕获组形式，兼容用户习惯 */
                int idx = in[i+1] - '0';
                size_t advance = 2;
                /* 多位数：$12 这种 */
                if (in[i+2] >= '0' && in[i+2] <= '9') {
                    idx = idx * 10 + (in[i+2] - '0');
                    advance = 3;
                }
                if (idx >= 0 && idx < PM_MAX_GROUPS && groups[idx][0]) {
                    const char *val = groups[idx];
                    size_t vl = strlen(val);
                    if (o + vl >= outsz) return -1;
                    memcpy(out + o, val, vl); o += vl;
                }
                i += advance;
                continue;
            }
        }
        /* 普通字符（包括不匹配的 $，原样输出） */
        if (o + 1 >= outsz) return -1;  /* 溢出 */
        out[o++] = in[i++];
    }
    out[o] = '\0';
    return (int)o;
}

/* §15 重写：profile_name → prio → id
 * profile_name 字典序把同 context 的规则聚在一起，dp_execute 扫描到目标分区后提前 break。
 * layer 已从排序中移除（由 dp_infer_layer_from_target 自动推断）。 */
static int rule_cmp(const void *a, const void *b)
{
    const dp_rule_t *ra = a, *rb = b;
    int r = strcmp(ra->profile_name, rb->profile_name);
    if (r != 0) return r < 0 ? -1 : 1;
    if (ra->prio != rb->prio) return ra->prio < rb->prio ? -1 : 1;
    return ra->id < rb->id ? -1 : 1;
}

int dp_table_sort(dp_table_t *t)
{
    if (!t)
        return NSBC_INVAL;
    qsort(t->rules, t->count, sizeof(dp_rule_t), rule_cmp);
    return NSBC_OK;
}

int dp_ctx_publish(dp_ctx_t *ctx, dp_table_t *new_table, dp_table_t **old)
{
    if (!ctx || !new_table)
        return NSBC_INVAL;
    dp_table_t *prev =
        __atomic_exchange_n(&ctx->active, new_table, __ATOMIC_ACQ_REL);
    /* PoC：单线程可立即回收；真实实现经 RCU 宽限期（synchronize_rcu）后释放 */
    if (old)
        *old = prev;
    else
        dp_table_free(prev);
    return NSBC_OK;
}

static const dp_table_t *ctx_snapshot(const dp_ctx_t *ctx)
{
    return __atomic_load_n(&ctx->active, __ATOMIC_ACQUIRE);
}

static int apply_number(const dp_rule_t *r, const char *called, char *out,
                        size_t outsz)
{
    const char *p = called;
    if (r->strip) {
        size_t l = strlen(p);
        if (r->strip > l)
            return NSBC_INVAL;
        p += r->strip;
    }
    int n = snprintf(out, outsz, "%s%s", r->add_prefix, p);
    if (n < 0 || (size_t)n >= outsz)
        return NSBC_INVAL;
    return NSBC_OK;
}

/* §15 重写：两级查找 —— 先按 profile_name 定位分区，再在分区内按 prio/id 顺序扫描。
 * 同 profile 规则经 rule_cmp 排序后自然相邻，扫完分区立即 break，热路径不扫完整表。 */
int dp_execute(const dp_ctx_t *ctx, const dp_call_t *call, dp_result_t *res)
{
    if (!ctx || !call || !res || !call->called)
        return NSBC_INVAL;
    if (!call->profile_name || !call->profile_name[0])
        return NSBC_INVAL;  /* §15：调用方必须填来源 profile */

    const dp_table_t *t = ctx_snapshot(ctx);
    if (!t || t->count == 0)
        return NSBC_ERR;

    /* §19 debug: dump ctx + call */
    llog(LOG_INFO, "dp", "execute: called='%s' profile='%s' src_ip='%s' perm=%d table_count=%zu",
         call->called ? call->called : "(null)",
         call->profile_name ? call->profile_name : "(null)",
         call->src_ip ? call->src_ip : "(null)",
         call->perm, t->count);

    memset(res, 0, sizeof(*res));
    int denied = 0;
    /* §15 第一优先级：CIDR reject 跟踪
     * cidr_reject_any=1 表示 profile 匹配的 rule 里至少有一条 CIDR 白名单拒绝了 src_ip。
     * 循环结束后：if in_partition && cidr_reject_any && !hit_any_rule → 这个 src_ip
     * 不是任何 rule 的合法源 → PERM_DENIED (silent drop)。
     * 用 res->target[0]=='\0' 等价 !hit_any（命中 rule 时 L447 return，target 已填）。*/
    int cidr_reject_any = 0;

    /* call->src_ip 是可选参数：只有 external profile（来自 SIP trunk）才需要 CIDR 过滤
     * internal profile 已在 RX 入口做 Digest 认证，src_ip 可空 */

    int in_partition = 0;

    for (size_t i = 0; i < t->count; ++i) {
        const dp_rule_t *r = &t->rules[i];
        if (!r->enabled) {
            llog(LOG_INFO, "dp", "  rule[%zu] id=%u prof='%s' DISABLED", i, r->id, r->profile_name);
            continue;
        }

        /* 【一级】分区定位（profile_name 精确匹配） */
        if (strcmp(r->profile_name, call->profile_name) != 0) {
            llog(LOG_INFO, "dp", "  rule[%zu] id=%u prof='%s' != call.prof='%s' → skip (in_partition=%d)",
                 i, r->id, r->profile_name, call->profile_name, in_partition);
            if (in_partition) break;    /* 已扫描完目标分区，提前退出 */
            continue;                   /* 还没到目标分区，跳过 */
        }
        in_partition = 1;
        llog(LOG_INFO, "dp", "  rule[%zu] id=%u prof='%s' ← in partition", i, r->id, r->profile_name);

        /* 【二级】规则级 IP CIDR 准入（§15 新增）
         * 第一优先级：CIDR reject 是最前置的安全闸门，profile 级路由和号码变换
         * 之前必须过。cidr_reject_any 置位 → 全 rule reject 时直接 PERM_DENIED。*/
        if (r->src_ip_cidr[0] && strcmp(r->src_ip_cidr, "0.0.0.0/0") != 0
            && call->src_ip && call->src_ip[0]) {
            if (sip_ip_in_cidr_list(r->src_ip_cidr, call->src_ip) != 1) {
                llog(LOG_INFO, "dp", "  rule[%zu] id=%u CIDR '%s' rejects src_ip → skip",
                     i, r->id, r->src_ip_cidr);
                cidr_reject_any = 1;
                continue;
            }
        }

        /* 【三级】被叫号段 pattern 匹配 + 捕获组提取 */
        char groups[PM_MAX_GROUPS][PM_GROUP_SZ];
        memset(groups, 0, sizeof(groups));
        int pm = pm_match_groups(&r->match, call->called, groups);
        llog(LOG_INFO, "dp", "  rule[%zu] id=%u type=%d match='%s' vs called='%s' → pm=%d groups[0..2]='%s','%s','%s'",
             i, r->id, r->match.type, r->match.pattern, call->called, pm,
             groups[0], groups[1], groups[2]);
        if (!pm) continue;

        /* 命中即定案：权限不足直接拒绝，不允许继续匹配绕过权限 */
        if (call->perm < r->perm) { denied = 1; break; }

        /* Advisor 2026-10-12n: 正则捕获组变量替换
         * 所有字符串字段先过 substitute_groups，再用于号码变换和 layer 推断 */
        char target_sub[sizeof(res->target)];
        char clip_sub[sizeof(res->clip)];
        char addprefix_sub[sizeof(r->add_prefix)];

        substitute_groups(r->target, target_sub, sizeof(target_sub), groups);
        substitute_groups(r->clip, clip_sub, sizeof(clip_sub), groups);
        substitute_groups(r->add_prefix, addprefix_sub, sizeof(addprefix_sub), groups);
        char dst_override_sub[sizeof(res->dst_number)];
        substitute_groups(r->dst_override, dst_override_sub, sizeof(dst_override_sub), groups);

        /* hdr_rw 所有字段也做替换（重要：sip_invite_from_uri / sip_invite_domain 等可能含变量） */
        substitute_groups(r->hdr_rw.sip_invite_from, res->hdr_rw.sip_invite_from, sizeof(res->hdr_rw.sip_invite_from), groups);
        substitute_groups(r->hdr_rw.sip_invite_from_uri, res->hdr_rw.sip_invite_from_uri, sizeof(res->hdr_rw.sip_invite_from_uri), groups);
        substitute_groups(r->hdr_rw.sip_from_uri, res->hdr_rw.sip_from_uri, sizeof(res->hdr_rw.sip_from_uri), groups);
        substitute_groups(r->hdr_rw.sip_from_display, res->hdr_rw.sip_from_display, sizeof(res->hdr_rw.sip_from_display), groups);
        substitute_groups(r->hdr_rw.sip_invite_from_params, res->hdr_rw.sip_invite_from_params, sizeof(res->hdr_rw.sip_invite_from_params), groups);
        substitute_groups(r->hdr_rw.sip_invite_to_uri, res->hdr_rw.sip_invite_to_uri, sizeof(res->hdr_rw.sip_invite_to_uri), groups);
        substitute_groups(r->hdr_rw.sip_invite_to_params, res->hdr_rw.sip_invite_to_params, sizeof(res->hdr_rw.sip_invite_to_params), groups);
        substitute_groups(r->hdr_rw.sip_invite_req_uri, res->hdr_rw.sip_invite_req_uri, sizeof(res->hdr_rw.sip_invite_req_uri), groups);
        substitute_groups(r->hdr_rw.sip_invite_tel_params, res->hdr_rw.sip_invite_tel_params, sizeof(res->hdr_rw.sip_invite_tel_params), groups);
        substitute_groups(r->hdr_rw.sip_invite_contact_params, res->hdr_rw.sip_invite_contact_params, sizeof(res->hdr_rw.sip_invite_contact_params), groups);
        substitute_groups(r->hdr_rw.sip_invite_domain, res->hdr_rw.sip_invite_domain, sizeof(res->hdr_rw.sip_invite_domain), groups);
        substitute_groups(r->hdr_rw.sip_invite_params, res->hdr_rw.sip_invite_params, sizeof(res->hdr_rw.sip_invite_params), groups);
        substitute_groups(r->hdr_rw.sip_invite_pid_params, res->hdr_rw.sip_invite_pid_params, sizeof(res->hdr_rw.sip_invite_pid_params), groups);

        /* 号码变换：用替换后的 add_prefix（strip 还是 r->strip 固定） */
        dp_rule_t r_copy = *r;
        memcpy(r_copy.add_prefix, addprefix_sub, sizeof(r_copy.add_prefix));
        if (apply_number(&r_copy, call->called, res->dst_number,
                         sizeof(res->dst_number)) != NSBC_OK)
            return NSBC_ERR;

        /* Advisor 2026-10-12n: target 组合语法解析
         * ┌────────────────────────────────────────────────────────────────────┐
         * │ target 语法（substitute_groups 替换变量后解析）                        │
         * ├──────────────────────┬─────────────────────────────────────────────┤
         * │ gateway:all_trunks   │ 纯路由，dst_number 走 apply_number(strip)   │
         * │ trunk:asterlink      │ 纯路由                                     │
         * │ ${2}                 │ 纯号码 → layer=EXT, dst_number=groups[2]   │
         * │ gateway:all_trunks|${2} │ 组合！ route=左边, dst_number=右边        │
         * │ trunk:asterlink|18621575908 │ 组合，右边也可以是纯字面量             │
         * └──────────────────────┴─────────────────────────────────────────────┘
         * 用 '|' 分隔 route_part 和 dn_part，两者都先过 substitute_groups。*/
        char route_part[sizeof(target_sub)];
        char dn_part[sizeof(target_sub)];
        route_part[0] = '\0'; dn_part[0] = '\0';

        const char *sep = strchr(target_sub, '|');
        if (sep) {
            /* 组合语法：route_part|dn_part */
            size_t rl = sep - target_sub;
            if (rl >= sizeof(route_part)) rl = sizeof(route_part) - 1;
            memcpy(route_part, target_sub, rl); route_part[rl] = '\0';
            snprintf(dn_part, sizeof(dn_part), "%s", sep + 1);
            llog(LOG_INFO, "dp", "  target COMBINED: route='%s' dn='%s'", route_part, dn_part);
        } else {
            /* 旧逻辑：整体当 route（可能是纯号码） */
            snprintf(route_part, sizeof(route_part), "%s", target_sub);
        }

        /* layer 从 route_part 推断 */
        res->layer = dp_infer_layer_from_target(route_part);

        /* res->target 给 signaling 用 = route_part */
        snprintf(res->target, sizeof(res->target), "%s", route_part);

        /* dst_number 覆盖规则（优先级从高到低）：
         *
         *   ★ 0) strip>0 或 add_prefix 非空 → apply_number 已经算出变换后号码，
         *      **直接跳过所有覆盖**。这是用户显式配置的号码变换意图（去前导/加前缀），
         *      优先级高于 dst_override 等——否则 Web 侧若错误地把原始号码填进 dst_override，
         *      就会把 strip 的结果覆盖回去（日志 dst_override='11001' strip=1 → 没变）。
         *
         *      注意：apply_number 在 L295 已经执行，结果写入 res->dst_number；
         *      此处只需保护它不被后续逻辑覆盖。
         *
         *   1) dst_override_sub 非空 → 直接用（用户显式强制覆盖，与 target 解耦）
         *   2) | 组合语法 dn_part 非空 → 用 dn_part（向后兼容旧配置）
         *   3) target_sub 不是 route prefix 且 strip=0 && add_prefix 空
         *      → target_sub 就是 dst_number（纯变量/纯号码场景）
         *   4) 否则 → apply_number(strip/add_prefix) 的结果（step 0 已执行） */
        int strip_add_effective = (r->strip > 0 || addprefix_sub[0]);
        if (strip_add_effective) {
            /* step 0：apply_number 结果已在 res->dst_number，保护不被覆盖 */
            llog(LOG_INFO, "dp", "  rule[%zu] dst_number from apply_number(strip=%u add_prefix='%s') → '%s'",
                 i, r->strip, addprefix_sub, res->dst_number);
        } else if (dst_override_sub[0]) {
            snprintf(res->dst_number, sizeof(res->dst_number), "%s", dst_override_sub);
            llog(LOG_INFO, "dp", "  rule[%zu] dst_number from dst_override='%s'", i, dst_override_sub);
        } else if (dn_part[0]) {
            snprintf(res->dst_number, sizeof(res->dst_number), "%s", dn_part);
            llog(LOG_INFO, "dp", "  rule[%zu] dst_number from dn_part='%s' (legacy | combo)", i, dn_part);
        } else {
            int is_route_pfx =
                (strncmp(route_part, "gateway:", 8) == 0) ||
                (strncmp(route_part, "trunk:", 6) == 0) ||
                (strncmp(route_part, "route:", 6) == 0) ||
                (strncmp(route_part, "extension:", 10) == 0) ||
                (strncmp(route_part, "queue:", 6) == 0) ||
                (strncmp(route_part, "ivr:", 4) == 0) ||
                (strncmp(route_part, "hangup", 6) == 0);
            int no_apply = (r->strip == 0 && addprefix_sub[0] == '\0');
            if (no_apply && !is_route_pfx && route_part[0]) {
                snprintf(res->dst_number, sizeof(res->dst_number), "%s", route_part);
                llog(LOG_INFO, "dp", "  rule[%zu] dst_number override from target='%s'", i, route_part);
            }
        }
        res->rule_id = r->id;
        res->rec_action = r->rec_action;
        res->media_mode = r->media_mode;   /* REQ-MEDIA-DP-001：媒体模式透传到 media_bridge */
        snprintf(res->rec_path_template, sizeof(res->rec_path_template), "%s", r->rec_path_template);
        if (clip_sub[0])
            snprintf(res->clip, sizeof(res->clip), "%s", clip_sub);
        /* FreeSWITCH 风格精细化主叫覆盖：origination_caller_id_number/name 也做变量替换 */
        char oc_num[sizeof(res->orig_caller_id_num)];
        char oc_name[sizeof(res->orig_caller_id_name)];
        substitute_groups(r->orig_caller_id_num, oc_num, sizeof(oc_num), groups);
        substitute_groups(r->orig_caller_id_name, oc_name, sizeof(oc_name), groups);
        if (oc_num[0])
            snprintf(res->orig_caller_id_num, sizeof(res->orig_caller_id_num), "%s", oc_num);
        if (oc_name[0])
            snprintf(res->orig_caller_id_name, sizeof(res->orig_caller_id_name), "%s", oc_name);
        /* 捕获组透传到 dp_result_t（signaling/b2bua 可按需再用） */
        memcpy(res->groups, groups, sizeof(res->groups));
        /* §15 新增：命中的 profile_name 带出来做日志/CDR */
        snprintf(res->profile_name, sizeof(res->profile_name),
                 "%s", r->profile_name);

        llog(LOG_INFO, "dp", "  rule[%zu] HIT: id=%u layer=%d target='%s' dst_number='%s' clip='%s' groups[1]='%s'",
             i, r->id, res->layer, target_sub, res->dst_number, clip_sub,
             groups[1][0] ? groups[1] : "(none)");

        /* ====== REQ-DP-APP-001：执行 Application 链 ======
         * 号码变换 + 路由已确定 → 按 app_chain 顺序依次执行
         * （pre_answer/playback/sleep/uuid_break/set/answer 等）。
         *
         * sleep 返回 APP_RC_PENDING → timer_wheel 挂起（dp_sleep_add），
         * 回调 resume 继续后续 app → 正常流程，非错误。
         * FAILED/HANGUP → log WARN 但仍 return NSBC_OK（signaling 只管路由，
         * 应用层失败不影响号码变换结果，可由上层按需处理）。 */
        if (r->app_chain_n > 0) {
            app_ctx_t actx;
            memset(&actx, 0, sizeof(actx));
            actx.call_key  = (void *)call->call_key;   /* b2b_call_t* 不透明键 */
            actx.leg       = 0;                         /* A 腿（Early Media 注入侧） */
            actx.dp_target = res->target;               /* 路由目的地 */
            actx.dp_layer  = (res->layer == DP_LAYER_EXT) ? "ext" :
                             (res->layer == DP_LAYER_BIZ) ? "biz" : "out";
            actx.dp_prio   = r->prio;
            app_rc_t arc = dp_execute_app_chain(dp_sleep_app_reg(), &actx, r, NULL);
            if (arc == APP_RC_PENDING)
                llog(LOG_INFO, "dp", "  app_chain suspended (timer pending, will resume)");
            else if (arc == APP_RC_FAILED)
                llog(LOG_WARN, "dp", "  app_chain FAILED at some app");
            else if (arc == APP_RC_HANGUP)
                llog(LOG_WARN, "dp", "  app_chain HANGUP (call terminated)");
            else
                llog(LOG_INFO, "dp", "  app_chain completed OK");
        }

        return NSBC_OK;   /* 命中即返回 */
    }
    /* 第一优先级 CIDR reject：profile 分区里至少有一条 rule CIDR 拒了 src_ip，
     * 且没有任何 rule 最终命中 → PERM_DENIED silent drop（黑产扫描者连号码是否
     * 存在都无从知道）。CIDR 全 reject 但有 rule CIDR 通配 0.0.0.0/0 或无 CIDR
     * 最终命中 → cidr_reject_any=0 → 正常返回。*/
    return denied || (in_partition && cidr_reject_any && res->target[0] == '\0')
        ? NSBC_PERM_DENIED : NSBC_ERR;
}

const dp_rule_t *dp_find_immediate(const dp_ctx_t *ctx, const char *digits)
{
    const dp_table_t *t = ctx_snapshot(ctx);
    if (!t)
        return NULL;
    for (size_t i = 0; i < t->count; ++i) {
        const dp_rule_t *r = &t->rules[i];
        if (r->enabled && r->immediate && pm_match(&r->match, digits))
            return r; /* 指针指向 active 表，RCU 读侧安全 */
    }
    return NULL;
}

/* 模块描述符：P0 规则表随配置热加载（RCU 换表），无启停动作 */
static int dialplan_mod_init(struct nsbc_ctx *ctx)
{
    (void)ctx;
    return NSBC_OK;
}

NSBC_MOD0_CAT(dialplan, 1, MOD_L_WORKER_READY, 0, MOD_CAT_APPLICATION, dialplan_mod_init, NULL);
