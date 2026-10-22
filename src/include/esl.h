#ifndef NSBC_ESL_H
#define NSBC_ESL_H

/* 统一 ESL/MI Socket（Phase 2 增量 2026-10-15）
 *
 * 类 FreeSWITCH Event Socket Library 的统一双向控制 Socket。支持多客户端
 * 并发连接（UDS + TCP 多路复用），双工协议：
 *
 *   下行控制：客户端发 `api <cmd> <args>` → 同步返回 +OK/-ERR
 *   上行事件：客户端发 `event plain <classes>` 订阅 → 异步推送 Event-Name/JSON
 *
 * 事件总线：`nsbc_event_fire(name, json)` 遍历所有 ESL 连接，有订阅的直接 push。
 *
 * 命令注册：各模块 start 时调用 esl_register_cmd，统一查找执行。
 * 已有 cmd_registry 复用，新增哈希查找优化。
 *
 * 分层：oam（3）。
 */

#include "nsbc_common.h"
#include "cmd_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 配置                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    /* UDS 路径；空则禁用。默认 /run/nsbc/esl.sock */
    const char *uds_path;
    /* TCP 监听地址；空则禁用。默认 0.0.0.0:8021 */
    const char *tcp_addr;
    /* 最大并发连接数 */
    int max_clients;
    /* L1 认证口令（可为空 = 无认证） */
    const char *password;
} esl_cfg_t;

/* ------------------------------------------------------------------ */
/* 模块生命周期（由 oam_module 装配）                                   */
/* ------------------------------------------------------------------ */

nsbc_rc_t esl_start(void *reactor, const esl_cfg_t *cfg, cmd_reg_t *reg);
void esl_stop(void);

/* ------------------------------------------------------------------ */
/* 事件总线                                                             */
/* ------------------------------------------------------------------ */

/* 事件分类位图（按位订阅） */
#define ESL_EVT_CDR           (1u << 0)
#define ESL_EVT_CHANNEL_STATE (1u << 1)
#define ESL_EVT_SIP_REGISTERED (1u << 2)
#define ESL_EVT_SIP_UNREGISTERED (1u << 3)
#define ESL_EVT_CONFIG_CHANGED (1u << 4)
#define ESL_EVT_PLAYBACK_START (1u << 5)
#define ESL_EVT_PLAYBACK_STOP  (1u << 6)
#define ESL_EVT_HANGUP         (1u << 7)
/* ... 后续扩展，最多 31 类 */

/* 名称到位图的映射查找（"CDR" → ESL_EVT_CDR），订阅命令用 */
uint32_t esl_event_class_from_name(const char *name);
const char *esl_event_class_to_name(uint32_t bit);

/* 发布事件。遍历所有 ESL 连接，订阅的直接推送。
 * 非阻塞：某个连接 write 失败只断开那个，不影响其他。 */
void esl_event_fire(const char *name, const char *json_body, size_t json_len);

/* Phase 4: 绑定 event_handler_register 通配订阅 "*"
 * 绑定后，内核所有 event_handler_trigger 事件自动推给 ESL 客户端。
 * 不绑定则 ESL 只能收到 ctx->esl_event_fire 直接调用的事件（CDR/PLAYBACK 等）。*/
int  esl_bind_event_bus(void);
void esl_unbind_event_bus(void);

/* ------------------------------------------------------------------ */
/* 便捷命令注册                                                         */
/* ------------------------------------------------------------------ */

/* 直接调 cmd_registry 的便捷封装，oam_module init 时用 */
int esl_register_cmd(struct cmd_reg *reg, const cmd_desc_t *desc);

/* ------------------------------------------------------------------ */
/* 查询                                                                */
/* ------------------------------------------------------------------ */

int esl_client_count(void);         /* 当前连接数 */
uint64_t esl_event_total_fired(void); /* 累计 fire 次数 */
uint64_t esl_api_total_calls(void);   /* 累计 api 调用次数 */

#ifdef __cplusplus
}
#endif

#endif /* NSBC_ESL_H */
