#ifndef NSBC_IVR_H
#define NSBC_IVR_H

/* IVR 自动语音应答（REQ-PBX-IVR-001~006）
 *
 * P0（规格口径）：放音/收号/菜单跳转/转接/超时重试。
 * P1：留言（挂 voicemail）、HTTP 查询（后台接口动态路由）。
 *
 * 执行模型：每呼叫一个 ivr_sess（菜单状态机），媒体动作（放音）
 * 经组合根回调到 media/play，按键经 dtmf 事件喂入；DigitMap 风格的
 * 首位/位间超时与重试次数由本状态机持有，时间轮在 core 装配。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IVR_NAME_MAX     32
#define IVR_DIGITS_MAX   32
#define IVR_PROMPT_MAX   128
#define IVR_TARGET_MAX   64
#define IVR_HTTP_URL_MAX 192
#define IVR_NODE_MAX     128
#define IVR_MAX_RETRY    5

typedef enum {
    IVR_ACT_MENU = 0,      /* 留本层继续收号 */
    IVR_ACT_SUBMENU,       /* 跳节点 */
    IVR_ACT_TRANSFER,      /* 转分机/中继/振铃组 */
    IVR_ACT_PLAY,          /* 仅放音后返回/挂机 */
    IVR_ACT_VOICEMAIL,     /* 转语音信箱（P1） */
    IVR_ACT_HTTP_QUERY,    /* HTTP 查询决定目标（P1） */
    IVR_ACT_HANGUP
} ivr_act_t;

typedef struct {
    ivr_act_t act;
    char      arg[IVR_TARGET_MAX];    /* 节点名/目标号/放音文件名 */
    char      url[IVR_HTTP_URL_MAX];  /* HTTP_QUERY 用 */
} ivr_action_t;

typedef struct {
    char        key[8];               /* 单键/多键前缀（t 超时键） */
    ivr_action_t action;
} ivr_choice_t;

typedef struct {
    char        name[IVR_NAME_MAX];
    char        prompt[IVR_PROMPT_MAX];
    int         initial_timeout_s;    /* 首位超时（缺省 5） */
    int         interdigit_timeout_s; /* 位间超时（缺省 3） */
    int         max_retries;          /* 无输入/未命中重试（缺省 3） */
    ivr_action_t timeout_action;      /* 重试耗尽 */
    ivr_action_t invalid_action;      /* 未命中键（粗骨架同超时处理） */
    ivr_choice_t choices[16];
    int         nchoices;
} ivr_node_t;

typedef struct {
    char      name[IVR_NAME_MAX];
    char      tenant[32];
    ivr_node_t nodes[IVR_NODE_MAX];
    int        n_nodes;
} ivr_profile_t;

/* 媒体动作回调（core 装配到 player；放音为异步，播完 ivr_on_prompt_end） */
typedef struct {
    int  (*play)(void *ud, void *call_key, const char *clip, int loop);
    int  (*transfer)(void *ud, void *call_key, const char *target);
    int  (*http_get)(void *ud, const char *url, const char *digits,
                     char *target_out, size_t cap); /* 可空，P1 */
    void *ud;
} ivr_ops_t;

typedef struct ivr_sess ivr_sess_t;

int  ivr_session_start(const ivr_profile_t *p, const char *entry,
                       void *call_key, const ivr_ops_t *ops,
                       ivr_sess_t **out);
void ivr_session_end(ivr_sess_t *s);

/* 收键：返回 1=产生终态动作（转出/挂机），0=仍在菜单，<0=错误/挂断 */
int  ivr_on_dtmf(ivr_sess_t *s, char digit, ivr_action_t *out_act);
/* 超时（时间轮）：按收号阶段判首位/位间超时与重试，输出动作 */
int  ivr_on_timeout(ivr_sess_t *s, ivr_action_t *out_act);
/* 放音结束回调（节点提示播完后开始收号计时） */
void ivr_on_prompt_end(ivr_sess_t *s);

/* 当前缓冲按键串（调试/HTTP 查询参数） */
const char *ivr_digits(const ivr_sess_t *s);

/* Profile 注册表（sync 推送填充 + 执行时查找） */
const ivr_profile_t *ivr_profile_find(const char *name, const char *tenant);

/* ---- IVR 模块内部通用 JSON 解析工具（公开供单元测试 + 其他模块复用） ----
 * 简化版手写 JSON 解析器：支持字符串值、整数值、顶级数组切分、
 * 嵌套对象/数组跳过。不处理转义字符以外的 JSON 语法。 */

/* 从 json 里找 key 的字符串值，输出到 dst（需预分配 cap）。
 * 成功返回 0，未找到返回 -1。支持转义引号还原。 */
int jstr(const char *json, const char *key, char *dst, size_t cap);

/* 从 json 里找 key 的整数值。缺失或非法返回 dflt。 */
int jint(const char *json, const char *key, int dflt);

/* 从 json 里找第 idx 个顶级数组元素，返回元素起始指针 + 长度。
 * 空数组/越界/格式错误返回 NULL。正确处理 [[...],[...]] 嵌套。 */
const char *jarray_at(const char *json, int idx, size_t *out_len);

/* Web sync 推送入口：完整 IVR 菜单数组 JSON
 * 格式：[{"name":"...","tenant_id":1,"entry_node":"main",
 *         "nodes_json":[{"name":"main","prompt":"...","choices":[...],...}]}]
 * 遍历每条调 ivr_reload_json，填充全局表。 */
int ivr_menus_reload_json(const char *json, char *err, size_t errsz);

/* 底层单 Profile 填充（同 modules/applications/ivr.c 内部 helper） */
int ivr_reload_json(const char *name, const char *tenant,
                    const char *nodes_json, const char *entry_node);

/* 时间轮 hook：检查 sess 是否到 deadline，到了自动走 timeout 逻辑 */
int ivr_check_timeout(ivr_sess_t *s, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_IVR_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
