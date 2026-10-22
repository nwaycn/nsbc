#ifndef NSBC_APP_REGISTRY_H
#define NSBC_APP_REGISTRY_H

/* Dialplan Application 注册表（REQ-DP-APP-001，P0）
 *
 * 与 REQ-CCMD-001 的命令注册表（cmd_registry.h）分离但同构：
 *   - 编译期静态注册（模块 init 时调 app_register）；
 *   - 执行上下文固定（当前 call_session + leg + 通道变量），
 *     作用域限于当前通道，不跨通道（bridge/conference 经 B2BUA API）；
 *   - 执行结果有语义：ok / failed / next_rule / hangup。
 *
 * 与 b2bua 原语关系：Application 是"在呼通道内执行"，CCMD 命令是
 * "通话外控制"；两者共用同一核心实现（如 answer → b2b_answer_call，
 * bridge → REQ-SIP-013 同一 b2b_bridge 原语），入口不同但行为一致。
 *
 * dp_rule_t 扩展：REload JSON 里 app_chain 字段（JSON 数组），
 * dp_rule_add 时同时接收 C 数组。
 */

#include "chan_var.h"
#include "dialplan.h"
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Application 返回码 ---- */
typedef enum {
    APP_RC_OK = 0,          /* 成功，继续执行下一 app */
    APP_RC_FAILED,          /* 失败，Dialplan 执行器走下一 prio 规则 */
    APP_RC_NEXT_RULE,       /* 显式跳到下一 prio 规则（sleep/超时场景） */
    APP_RC_HANGUP,          /* 显式挂断（桥接失败 + reason=hangup 场景） */
    APP_RC_HANDOFF_SILENT,  /* 内部状态迁移，无需返回上层（park 驻留等） */
    APP_RC_PENDING          /* ⏳ 非阻塞：app 挂起（如 sleep 定时器），执行器存上下文，回调 resume */
} app_rc_t;

/* ---- App 上下文（每个 app 独立，禁跨通道解引用）---- */
typedef struct app_ctx {
    void  *call_key;        /* b2b_call_t* 不透明（REQ-ARCH-011） */
    int    leg;             /* 0=A, 1=B（当前正在执行的 leg） */
    chan_var_t *vars;       /* 通道变量（session 级 + leg 级） */
    const char *dp_target;  /* 规则 target 字符串（只读） */
    const char *dp_layer;   /* ext/biz/out（只读） */
    int    dp_prio;         /* 当前命中规则的 prio（排序用） */
} app_ctx_t;

/* ---- app 入口 ---- */
typedef app_rc_t (*app_fn_t)(app_ctx_t *c, const char *data, void *ud);

#define APP_NAME_MAX  32
#define APP_HELP_MAX  128
#define APP_MAX       256

typedef struct {
    char     name[APP_NAME_MAX];
    char     help[APP_HELP_MAX];
    app_fn_t fn;
    void    *ud;
} app_desc_t;

typedef struct app_reg app_reg_t;

int  app_reg_create(app_reg_t **out);
void app_reg_destroy(app_reg_t *r);

/* 注册内建 apps（dp_sleep_init / 模块 start 时调） */
void app_builtin_register(app_reg_t *reg);

int  app_register(app_reg_t *r, const app_desc_t *d);
int  app_unregister(app_reg_t *r, const char *name);

/* Phase 1 SAFE1: 安全卸载扩展 API（不改现有公开签名）
 *   app_registry_find_inc / app_registry_dec —— 卸载安全 find + refcnt 配对
 *   app_registry_unload_wait —— 置 unloading + 自旋 wait refcnt→0 + swap 删除 */
int  app_registry_find_inc(app_reg_t *r, const char *name, int *out_slot);
void app_registry_dec(app_reg_t *r, int slot);
int  app_registry_unload_wait(app_reg_t *r, const char *name, int timeout_ms);

/* 执行单个 app；未找到 NOENT。 */
int  app_execute(app_reg_t *r, const char *name, app_ctx_t *c,
                 const char *data, app_rc_t *rc_out);
int  app_help(app_reg_t *r, const char *prefix,
              int (*io_printf)(void *, const char *, ...), void *ud);
int  app_count(app_reg_t *r);

/* ==================================================================
 * dp_rule_t 扩展：app_chain
 *
 * REQ-DP-APP-001 要求：reload JSON 里新增 `app_chain` 字段——
 *     "app_chain": [{"app": "playback", "data": "/var/sounds/welcome.wav"},
 *                    {"app": "bridge",   "data": "sofia/gateway/gw/1234"}]
 * action_json 先执行号码变换，后 app_chain 顺序执行。
 *
 * dp_rule_t 在 src/include/dialplan.h 已声明（typedef dp_rule_t），
 * 本头提供扩展结构 dp_app_action_t + dp_rule 上挂的 C 数组。
 * reload 时 dp_rules_reload_json 负责解析 JSON → 构造数组。
 * ==================================================================*/

/* 命名 struct：与 dialplan.h 的前向声明 `struct dp_app_action` 统一
 * （dp_rule_t.app_chain 指向本类型；匿名 struct 会成为不同类型） */
typedef struct dp_app_action {
    char app[APP_NAME_MAX];   /* "answer" / "playback" / ... */
    char data[512];           /* app data 参数；空串表示无参 */
} dp_app_action_t;

/* dp_rule_add 重载：传入 app_chain 数组；
 * NULL / n=0 表示此规则无 app_chain（纯 action_json）。
 * rule 内 app_chain 指针由调用方（reload）分配 RCU 可释放内存。 */
int dp_rule_set_app_chain(dp_rule_t *rule,
                          const dp_app_action_t *chain, size_t n);

/* 执行整条 app_chain：ctx 携带所有执行上下文；
 * 任何 app 失败 → 返回 APP_RC_FAILED 并停在该位置；
 * hangup → 返回 APP_RC_HANGUP（执行器终止）；
 * handoff → 返回 APP_RC_HANDOFF_SILENT。 */
app_rc_t dp_execute_app_chain(app_reg_t *reg, app_ctx_t *ctx,
                              const dp_rule_t *rule, size_t *stopped_at);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_APP_REGISTRY_H */
