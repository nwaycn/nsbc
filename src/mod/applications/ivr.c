/* IVR 执行器——完整实现（REQ-PBX-IVR-001~006 P0；003/006 P1 骨架）
 *
 * 状态机：进节点→播 prompt（ops.play）→arm 超时→收号匹配 choice：
 *   - 精确命中 → 按 act 分发（SUBMENU 进新节点返回 0；TRANSFER/HANGUP 终态返回 1）
 *   - 前缀命中 → 继续收号（blen 累积，多键匹配链）
 *   - 无任何前缀 → invalid_action（或 retries++ 重播提示）
 *   - 超时 → 首位/位间分支处理，retries 耗尽走 timeout_action
 *
 * Profile 来源：Web DB ivr_menus 表 → sync 模块推送 reload JSON →
 * ivr_reload_json 解析填充 g_ivr_profiles 全局表。
 */
#include "ivr.h"
#include "nsbc_common.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>

/* ------- 全局 Profile 注册表（sync 推送填充）------- */
#define IVR_MAX_PROFILES 64
static ivr_profile_t g_ivr_profiles[IVR_MAX_PROFILES];
static int           g_ivr_nprofiles = 0;
static pthread_mutex_t g_ivr_lock = PTHREAD_MUTEX_INITIALIZER;

const ivr_profile_t *ivr_profile_find(const char *name, const char *tenant)
{
    if (!name) return NULL;
    pthread_mutex_lock(&g_ivr_lock);
    for (int i = 0; i < g_ivr_nprofiles; ++i) {
        if (strcmp(g_ivr_profiles[i].name, name) == 0) {
            if (!tenant || !tenant[0] || !g_ivr_profiles[i].tenant[0] ||
                strcmp(g_ivr_profiles[i].tenant, tenant) == 0) {
                pthread_mutex_unlock(&g_ivr_lock);
                return &g_ivr_profiles[i];
            }
        }
    }
    pthread_mutex_unlock(&g_ivr_lock);
    return NULL;
}

/* ------- Profile JSON → ivr_profile_t 解析 -------
 *
 * 入口 JSON（nodes_json 字段直接来自 DB，Web 树编辑器维护）：
 *   [{
 *     "name": "main",
 *     "prompt": "welcome.wav",
 *     "initial_timeout_s": 5,
 *     "interdigit_timeout_s": 3,
 *     "max_retries": 3,
 *     "choices": [{"key":"1","act":"submenu","arg":"sales"},
 *                 {"key":"2","act":"transfer","arg":"1000"},
 *                 {"key":"*","act":"hangup","arg":""}],
 *     "timeout_action": {"act":"hangup","arg":""},
 *     "invalid_action": {"act":"play","arg":"invalid.wav"}
 *   }, ...]
 *
 * 用零依赖的极简 JSON 解析：不引 cjson/jsmn，纯 strchr/strtol + 边界检查，
 * 失败则节点留空 + 记 warn，core 能降级跑最小功能。
 */

static ivr_act_t parse_act(const char *s)
{
    if (!s) return IVR_ACT_HANGUP;
    if (strcmp(s, "submenu") == 0 || strcmp(s, "goto") == 0)    return IVR_ACT_SUBMENU;
    if (strcmp(s, "transfer") == 0 || strcmp(s, "ext") == 0)   return IVR_ACT_TRANSFER;
    if (strcmp(s, "play") == 0 || strcmp(s, "playback") == 0)  return IVR_ACT_PLAY;
    if (strcmp(s, "voicemail") == 0 || strcmp(s, "vm") == 0)   return IVR_ACT_VOICEMAIL;
    if (strcmp(s, "http") == 0 || strcmp(s, "http_query") == 0)return IVR_ACT_HTTP_QUERY;
    if (strcmp(s, "hangup") == 0 || strcmp(s, "bye") == 0)    return IVR_ACT_HANGUP;
    return IVR_ACT_HANGUP;
}

/* act_name(act) helper 暂未被引用（DTMF hook 接进来后加日志时用），
 * 先留 parse_act 反向足够。后续需要再加。 */

/* 从 json 里找 key 的字符串值，返回值起始指针（dst 需预分配）。
 * 格式 "key":"value" —— 支持转义引号。简化版不处理嵌套对象。 */
static int jstr(const char *json, const char *key, char *dst, size_t cap)
{
    if (!json || !key || !dst || cap == 0) return -1;
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, "\"")) != NULL) {
        if (strncmp(p + 1, key, klen) == 0 && p[klen + 1] == '\"') {
            const char *colon = strchr(p + klen + 2, ':');
            if (!colon) return -1;
            const char *qv = strchr(colon, '\"');
            if (!qv) return -1;
            qv++;
            size_t n = 0;
            while (*qv && *qv != '\"' && n < cap - 1) {
                if (*qv == '\\' && *(qv + 1)) qv++;
                dst[n++] = *qv++;
            }
            dst[n] = '\0';
            return 0;
        }
        p++;
    }
    return -1;
}

static int jint(const char *json, const char *key, int dflt)
{
    char buf[16];
    if (jstr(json, key, buf, sizeof(buf)) == 0) {
        char *end = NULL;
        long v = strtol(buf, &end, 10);
        if (end != buf) return (int)v;
    }
    return dflt;
}

/* 解析 action 对象 {"act":"transfer","arg":"1000","url":""}
 * act_out: 写入解析后的 act 枚举
 * arg_out: 写入 arg 字符串（可选） */
static void jaction(const char *json, ivr_act_t *act_out, char *arg_out, size_t arg_cap)
{
    char act_name[32] = "";
    if (jstr(json, "act", act_name, sizeof(act_name)) == 0)
        *act_out = parse_act(act_name);
    else
        *act_out = IVR_ACT_HANGUP;
    if (arg_out && arg_cap > 0) {
        if (jstr(json, "arg", arg_out, arg_cap) != 0 &&
            jstr(json, "target", arg_out, arg_cap) != 0) {
            arg_out[0] = '\0';
        }
    }
}

/* jarray_at: 从 json 里找第 idx 个数组元素，返回元素起始指针（不含外层 [）+ 长度。
 * 关键：数组元素之间用顶级逗号切分，不是 [ 计数（[{a},{b}] 结构里 [ 只有一个，
 * 但元素有两个）。跟踪 arr_depth([]) + obj_depth({}) + 字符串跳过三重保护。 */
static const char *jarray_at(const char *json, int idx, size_t *out_len)
{
    if (!json || !out_len) return NULL;
    const char *p = strchr(json, '[');
    if (!p) return NULL;
    int arr_depth = 0, obj_depth = 0, in_str = 0;
    int cur = -1;
    const char *elem_start = NULL;
    for (const char *q = p; *q; ++q) {
        if (*q == '\\' && in_str) { ++q; continue; }
        if (*q == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (*q == '[') {
            arr_depth++;
            if (arr_depth == 1 && !elem_start) {
                elem_start = q + 1;   /* 跳过顶级 [ */
                cur = 0;              /* 第一个元素就是 cur=0 */
            }
        } else if (*q == ']') {
            if (arr_depth == 1) {
                /* 顶级数组结束 */
                if (elem_start) {
                    size_t elemLen = (size_t)(q - elem_start);
                    /* 空数组 []：elem_start 就在 ] 前面，elemLen==0，视为无元素 */
                    if (elemLen == 0) return NULL;
                    if (idx >= 0 && cur == idx) {
                        *out_len = elemLen;
                        return elem_start;
                    }
                }
                return NULL;  /* 找不到 */
            }
            arr_depth--;
        } else if (*q == '{') {
            obj_depth++;
        } else if (*q == '}') {
            obj_depth--;
        } else if (*q == ',' && arr_depth == 1 && obj_depth == 0) {
            /* 顶级逗号：切分元素边界 */
            if (cur == idx) {
                *out_len = (size_t)(q - elem_start);
                return elem_start;
            }
            cur++;
            elem_start = q + 1;   /* 下一个元素起点（跳过逗号） */
            /* 跳过逗号后的空格 */
            while (*elem_start == ' ') elem_start++;
        }
    }
    return NULL;
}

/* 从顶层 json 里找指定 key 的嵌套对象值（{"act":"hangup","arg":""}），
 * 返回对象起始指针 + 长度（含 {}）。简化版：只找第一个同名 key。 */
static const char *jobj_at(const char *json, const char *key, size_t *out_len)
{
    if (!json || !key || !out_len) return NULL;
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, "\"")) != NULL) {
        if (strncmp(p + 1, key, klen) == 0 && p[klen + 1] == '\"') {
            const char *colon = strchr(p + klen + 2, ':');
            if (!colon) return NULL;
            const char *brace = strchr(colon, '{');
            if (!brace) return NULL;
            int depth = 0, in_str = 0;
            for (const char *q = brace; *q; ++q) {
                if (*q == '\\' && in_str) { ++q; continue; }
                if (*q == '"') { in_str = !in_str; continue; }
                if (in_str) continue;
                if (*q == '{') depth++;
                else if (*q == '}') {
                    depth--;
                    if (depth == 0) {
                        *out_len = (size_t)(q - brace + 1);
                        return brace;
                    }
                }
            }
            return NULL;
        }
        p++;
    }
    return NULL;
}

int ivr_reload_json(const char *name, const char *tenant,
                    const char *nodes_json, const char *entry_node)
{
    if (!name || !nodes_json) return NSBC_INVAL;
    (void)entry_node;   /* 入口节点由 ivr_session_start 调用方传入，不存 profile */
    pthread_mutex_lock(&g_ivr_lock);
    int slot = -1;
    for (int i = 0; i < g_ivr_nprofiles; ++i) {
        if (strcmp(g_ivr_profiles[i].name, name) == 0 &&
            (!tenant || !tenant[0] || !g_ivr_profiles[i].tenant[0] ||
             strcmp(g_ivr_profiles[i].tenant, tenant) == 0)) {
            slot = i; break;
        }
    }
    if (slot < 0) {
        if (g_ivr_nprofiles >= IVR_MAX_PROFILES) {
            pthread_mutex_unlock(&g_ivr_lock);
            return NSBC_NOMEM;
        }
        slot = g_ivr_nprofiles++;
    }
    memset(&g_ivr_profiles[slot], 0, sizeof(ivr_profile_t));
    snprintf(g_ivr_profiles[slot].name, sizeof(g_ivr_profiles[slot].name), "%s", name);
    if (tenant && tenant[0])
        snprintf(g_ivr_profiles[slot].tenant, sizeof(g_ivr_profiles[slot].tenant), "%s", tenant);

    /* 解析 nodes 数组 */
    int n = 0;
    for (int i = 0; i < IVR_NODE_MAX; ++i) {
        size_t elem_len = 0;
        const char *elem = jarray_at(nodes_json, i, &elem_len);
        if (!elem) break;
        char buf[4096];
        if (elem_len >= sizeof(buf)) elem_len = sizeof(buf) - 1;
        memcpy(buf, elem, elem_len); buf[elem_len] = '\0';

        ivr_node_t *nd = &g_ivr_profiles[slot].nodes[n];
        jstr(buf, "name", nd->name, sizeof(nd->name));
        jstr(buf, "prompt", nd->prompt, sizeof(nd->prompt));
        nd->initial_timeout_s    = jint(buf, "initial_timeout_s", 5);
        nd->interdigit_timeout_s = jint(buf, "interdigit_timeout_s", 3);
        nd->max_retries          = jint(buf, "max_retries", 3);

        /* timeout_action / invalid_action —— 用 jobj_at 提取嵌套对象后再调 jaction，
         * 避免直接从顶层 buf 瞎猜 act/arg 字段。action 是 ivr_action_t 结构体（不是 enum 指针）。 */
        {
            size_t ta_len = 0;
            const char *ta = jobj_at(buf, "timeout_action", &ta_len);
            if (ta && ta_len > 2) {
                char tobj[512];
                size_t tl = ta_len < sizeof(tobj) - 1 ? ta_len : sizeof(tobj) - 1;
                memcpy(tobj, ta, tl); tobj[tl] = '\0';
                jaction(tobj, &nd->timeout_action.act, nd->timeout_action.arg, sizeof(nd->timeout_action.arg));
            } else {
                nd->timeout_action.act = IVR_ACT_HANGUP; nd->timeout_action.arg[0] = '\0';
            }
        }
        {
            size_t ia_len = 0;
            const char *ia = jobj_at(buf, "invalid_action", &ia_len);
            if (ia && ia_len > 2) {
                char iobj[512];
                size_t il = ia_len < sizeof(iobj) - 1 ? ia_len : sizeof(iobj) - 1;
                memcpy(iobj, ia, il); iobj[il] = '\0';
                jaction(iobj, &nd->invalid_action.act, nd->invalid_action.arg, sizeof(nd->invalid_action.arg));
            } else {
                nd->invalid_action.act = IVR_ACT_HANGUP; nd->invalid_action.arg[0] = '\0';
            }
        }

        /* choices 数组 */
        int ci = 0;
        for (int j = 0; j < 16 && ci < 16; ++j) {
            size_t choice_len = 0;
            const char *choice = jarray_at(buf, j, &choice_len);
            if (!choice) break;
            char cbuf[512];
            if (choice_len >= sizeof(cbuf)) choice_len = sizeof(cbuf) - 1;
            memcpy(cbuf, choice, choice_len); cbuf[choice_len] = '\0';
            /* 跳过非 choice 对象（timeout_action/invalid_action 是嵌套对象，
             * 没有 key 字段；choices 数组元素必须有 key） */
            char test[8];
            if (jstr(cbuf, "key", test, sizeof(test)) != 0) continue;

            ivr_choice_t *ch = &nd->choices[ci];
            jstr(cbuf, "key", ch->key, sizeof(ch->key));
            jaction(cbuf, &ch->action.act, ch->action.arg, sizeof(ch->action.arg));
            ci++;
        }
        nd->nchoices = ci;

        n++;
    }
    g_ivr_profiles[slot].n_nodes = n;
    pthread_mutex_unlock(&g_ivr_lock);
    llog(LOG_INFO, "ivr", "reload profile name=%s tenant=%s nodes=%d",
         name, tenant ? tenant : "-", n);
    return NSBC_OK;
}

/* ------- session 状态机 ------- */

struct ivr_sess {
    const ivr_profile_t *p;
    void               *call_key;
    ivr_ops_t           ops;
    const ivr_node_t   *cur;
    char                buf[IVR_DIGITS_MAX];
    int                 blen;
    int                 retries;
    int                 finished;
    uint64_t            deadline_ms;   /* 超时定时器（当前简化：由 media bridge 喂 ivr_on_timeout） */
    int                 phase;         /* 0=首位等待, 1=位间等待 */
};

static const ivr_node_t *node_find(const ivr_profile_t *p, const char *name)
{
    if (!p) return NULL;
    for (int i = 0; i < p->n_nodes; ++i) {
        if (strcmp(p->nodes[i].name, name) == 0)
            return &p->nodes[i];
    }
    return NULL;
}

static void enter_node(ivr_sess_t *s, const ivr_node_t *n)
{
    s->cur = n;
    s->buf[0] = '\0';
    s->blen = 0;
    s->phase = 0; /* 首位等待 */
    if (s->ops.play != NULL && n->prompt[0] != '\0') {
        (void)s->ops.play(s->ops.ud, s->call_key, n->prompt, 0);
    } else {
        /* 无 prompt — 直接 arm 收号超时（首位阶段） */
        s->deadline_ms = nsbc_now_ms() + (uint64_t)n->initial_timeout_s * 1000u;
    }
}

int ivr_session_start(const ivr_profile_t *p, const char *entry,
                      void *call_key, const ivr_ops_t *ops,
                      ivr_sess_t **out)
{
    if (!p || !entry || !call_key || !ops || !out)
        return NSBC_INVAL;
    const ivr_node_t *n = node_find(p, entry);
    if (!n) {
        llog(LOG_WARN, "ivr", "entry node '%s' not found in profile '%s'", entry, p->name);
        return NSBC_NOENT;
    }
    ivr_sess_t *s = (ivr_sess_t *)calloc(1, sizeof(*s));
    if (!s) return NSBC_NOMEM;
    s->p = p;
    s->call_key = call_key;
    s->ops = *ops;
    *out = s;
    enter_node(s, n);
    return NSBC_OK;
}

void ivr_session_end(ivr_sess_t *s)
{
    free(s);
}

/* 前缀匹配：choice.key == buf（精确）或 choice.key 以 buf 开头（前缀）
 * 精确命中返回 EXACT(1)，前缀命中返回 PREFIX(0)，无匹配返回 -1 */
static int match_choice(const ivr_node_t *n, const char *buf, const ivr_choice_t **out_exact, const ivr_choice_t **out_prefix)
{
    *out_exact = NULL;
    *out_prefix = NULL;
    for (int i = 0; i < n->nchoices; ++i) {
        const char *key = n->choices[i].key;
        if (!key || strcmp(key, "t") == 0) continue;
        if (strcmp(key, buf) == 0) {
            *out_exact = &n->choices[i];
            return 1;
        }
        size_t bl = strlen(buf);
        if (strncmp(key, buf, bl) == 0 && strlen(key) > bl)
            *out_prefix = &n->choices[i];
    }
    return (*out_exact || *out_prefix) ? 0 : -1;
}

int ivr_on_dtmf(ivr_sess_t *s, char digit, ivr_action_t *out_act)
{
    if (!s || !out_act || !s->cur) return NSBC_INVAL;
    if (s->finished) return NSBC_INVAL;

    /* 累积 digits */
    if (s->blen >= IVR_DIGITS_MAX - 1) {
        /* buffer 满 — invalid */
        *out_act = s->cur->invalid_action;
        s->finished = 1;
        return 1;
    }
    s->buf[s->blen++] = digit;
    s->buf[s->blen] = '\0';
    s->phase = 1; /* 位间 */

    const ivr_choice_t *exact = NULL;
    const ivr_choice_t *prefix = NULL;
    int rc = match_choice(s->cur, s->buf, &exact, &prefix);

    if (rc == 1 && exact) {
        /* 精确命中 — 执行动作 */
        if (exact->action.act == IVR_ACT_SUBMENU) {
            /* 跳节点 — 不终态，继续收号（新节点自己 enter → play prompt → arm 超时） */
            const ivr_node_t *next = node_find(s->p, exact->action.arg);
            if (next) {
                enter_node(s, next);
                return 0;
            } else {
                llog(LOG_WARN, "ivr", "submenu target '%s' not found in '%s'",
                     exact->action.arg, s->p->name);
                /* 降级：invalid */
                if (s->retries < s->cur->max_retries) {
                    ++s->retries;
                    enter_node(s, s->cur);
                    return 0;
                }
                *out_act = s->cur->invalid_action;
                s->finished = 1;
                return 1;
            }
        }
        *out_act = exact->action;
        s->finished = 1;
        return 1;
    }
    if (rc == 0 && prefix) {
        /* 仅前缀命中 — 继续收号，arm 位间超时 */
        s->deadline_ms = nsbc_now_ms() + (uint64_t)s->cur->interdigit_timeout_s * 1000u;
        return 0;
    }

    /* 无任何匹配 */
    if (s->retries < s->cur->max_retries) {
        ++s->retries;
        s->buf[0] = '\0'; s->blen = 0;
        enter_node(s, s->cur);
        return 0;
    }
    *out_act = s->cur->invalid_action;
    s->finished = 1;
    return 1;
}

int ivr_on_timeout(ivr_sess_t *s, ivr_action_t *out_act)
{
    if (!s || !out_act || !s->cur) return NSBC_INVAL;
    if (s->finished) return NSBC_INVAL;

    /* 显式超时键 t 优先 */
    for (int i = 0; i < s->cur->nchoices; ++i) {
        if (strcmp(s->cur->choices[i].key, "t") == 0) {
            *out_act = s->cur->choices[i].action;
            s->finished = 1;
            return 1;
        }
    }
    if (s->retries < s->cur->max_retries) {
        ++s->retries;
        enter_node(s, s->cur);
        return 0;
    }
    *out_act = s->cur->timeout_action;
    s->finished = 1;
    return 1;
}

void ivr_on_prompt_end(ivr_sess_t *s)
{
    if (!s || !s->cur) return;
    /* prompt 播完 → arm 收号超时（首位阶段） */
    s->phase = 0;
    s->deadline_ms = nsbc_now_ms() + (uint64_t)s->cur->initial_timeout_s * 1000u;
}

const char *ivr_digits(const ivr_sess_t *s)
{
    return s ? s->buf : NULL;
}

/* ------- 时间轮 hook：检查是否到 deadline ------- */
/* 注：实际 timer_wheel 集成需 media bridge 侧 hook 喂入，
 * 这里只暴露查询接口，供核心 tick 扫描： */
int ivr_check_timeout(ivr_sess_t *s, uint64_t now_ms)
{
    if (!s || !s->cur || s->finished) return 0;
    if (now_ms >= s->deadline_ms) {
        ivr_action_t act;
        int rc = ivr_on_timeout(s, &act);
        if (rc == 1) {
            /* 需要外部执行 act：ops.transfer / 直接 hangup */
            if (act.act == IVR_ACT_TRANSFER && s->ops.transfer)
                (void)s->ops.transfer(s->ops.ud, s->call_key, act.arg);
            /* HANGUP 由 call_destroy 在外部触发 */
        }
        return 1;
    }
    return 0;
}

/* ------- Web sync 推送入口：完整 IVR 菜单数组 -------
 *
 * 输入：[{"id":1,"name":"main_menu","tenant_id":1,"entry_node":"main",
 *         "nodes_json":[{"name":"main","prompt":"welcome.wav",...}],...}]
 * 遍历每条行 JSON，调 ivr_reload_json 解析 → 填 g_ivr_profiles 全局表。
 * 纯 C 解析（同 jstr/jint/jarray_at），不引 cJSON。
 */
int ivr_menus_reload_json(const char *json, char *err, size_t errsz)
{
    if (!json) return NSBC_INVAL;
    if (err && errsz) err[0] = '\0';
    int n_ok = 0, n_fail = 0;
    for (int i = 0; ; ++i) {
        size_t len = 0;
        const char *elem = jarray_at(json, i, &len);
        if (!elem || len == 0) break;   /* 防御：空数组或越界都终止 */
        if (len > 8192) len = 8191;
        char buf[8192];
        memcpy(buf, elem, len); buf[len] = '\0';

        char name[IVR_NAME_MAX] = {0};
        char tenant[32] = {0};
        char entry[IVR_NAME_MAX] = "main";
        char nodes[65536] = {0};

        jstr(buf, "name", name, sizeof(name));
        jstr(buf, "entry_node", entry, sizeof(entry));
        /* tenant_id 转字符串 */
        int tid = jint(buf, "tenant_id", 1);
        snprintf(tenant, sizeof(tenant), "%d", tid);

        /* nodes_json 字段 —— 它本身是个 JSON 字符串 */
        char nj_raw[65536];
        if (jstr(buf, "nodes_json", nj_raw, sizeof(nj_raw)) == 0) {
            /* jstr 处理了转义引号（把 \" 还原为 "），正好直接用 */
            snprintf(nodes, sizeof(nodes), "%s", nj_raw);
        } else {
            /* 兼容：旧前端可能发 options_json */
            if (jstr(buf, "options_json", nj_raw, sizeof(nj_raw)) == 0)
                snprintf(nodes, sizeof(nodes), "%s", nj_raw);
        }

        if (!name[0] || !nodes[0]) {
            /* 诊断：打印原始 JSON 和解析结果，帮前端对齐字段 */
            llog(LOG_WARN, "ivr",
                 "reload_json elem[%d] SKIP: name='%s' nodes=%s "
                 "raw=%.300s",
                 i, name, nodes[0] ? "(present)" : "(empty)", buf);
            n_fail++;
            continue;
        }
        int rc = ivr_reload_json(name, tenant, nodes, entry);
        if (rc == NSBC_OK) n_ok++;
        else {
            llog(LOG_WARN, "ivr",
                 "reload_json elem[%d] ivr_reload_json rc=%d name='%s'",
                 i, rc, name);
            n_fail++;
        }
    }
    llog(LOG_INFO, "ivr", "reload_json: ok=%d fail=%d", n_ok, n_fail);
    if (err && errsz) snprintf(err, errsz, "ok=%d fail=%d", n_ok, n_fail);
    return n_fail > 0 ? NSBC_ERR : NSBC_OK;
}
