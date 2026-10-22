/* endpoint_registry.h — 协议端点自注册（SIP / WebRTC / AI / ...）
 *
 * Phase 1 基础设施：把 signaling.c 里硬编码的 SIP 调用链 → 由
 * endpoint_registry 统一分发。AI 虚拟端点 "ai" 类型 → mod_ai_endpoint
 * 自注册后可直接 sip:ai/gpt4_agent 路由。
 *
 * 安全卸载：每个条目带 refcnt + unloading 原子字段；
 *           modmgr 卸载模块时先置 unloading=true，自旋等待 refcnt→0 后清空条目。
 *           找不到条目时带 INC 返回指针，用完后必须 dec。
 */
#ifndef NSBC_ENDPOINT_REGISTRY_H
#define NSBC_ENDPOINT_REGISTRY_H

#include "nsbc_common.h"
#include "b2bua.h"     /* b2b_call_t */
#include "sip_msg.h"   /* sip_msg_t */

#ifdef __cplusplus
extern "C" {
#endif

#define ENDPOINT_MAX 8

/* 端点 ops 结构（白皮书 §2.1 设计）
 * - accept: 快速判断自己能不能处理这个入站 msg（返回 NSBC_OK 接管）
 * - handle_invite: 入站 INVITE 处理（SDP 协商前）
 * - handle_destroy: 呼叫销毁时清理 endpoint 私有 ctx
 * ctx 是 endpoint 自己创建/管理的私有上下文，通过 accept 返回给调用方 */
typedef struct {
    const char *name;                               /* "sip", "webrtc", "ai", "websocket" */
    int  (*accept)(sip_msg_t *m, void **ctx);       /* 返回 NSBC_OK 表示自己接管；ctx 出参 */
    int  (*handle_invite)(void *ctx, b2b_call_t *c); /* 入站 INVITE 钩子 */
    void (*handle_destroy)(void *ctx, int reason);   /* 呼叫销毁 */
} endpoint_ops_t;

/* 注册/注销
 * register: 同名已存在则返回 NSBC_EXISTS（endpoint 不应覆盖，避免协议混淆）
 * unregister: 标记 unloading → 等待 refcnt→0 → 清空条目；超时返回 NSBC_BUSY */
int  endpoint_registry_register(const endpoint_ops_t *ops);
int  endpoint_registry_unregister(const char *name);

/* 查找 + INC/DEC（卸载安全）
 * find_inc: 跳过 unloading 条目，找到后原子 refcnt++；找不到返回 NULL
 * dec: 用完后必须调用
 * find: 不带 INC（只查，用于遍历/日志），返回 NULL 或条目的 ops 指针 */
const endpoint_ops_t *endpoint_registry_find_inc(const char *name, int *out_slot);
void                  endpoint_registry_dec(int slot);
const endpoint_ops_t *endpoint_registry_find(const char *name);

/* 卸载安全等待
 * unload_wait: 置 unloading=true 后自旋等待 refcnt→0（超时 fail-fast 返回 NSBC_BUSY）
 * timeout_ms=0 表示无限等待（生产慎用） */
int endpoint_registry_unload_wait(const char *name, int timeout_ms);

/* 计数 + 遍历（OAM show endpoints 用） */
int endpoint_registry_count(void);

/* 按槽位号查（OAM show endpoints 遍历）：
 * out_unloading / out_refcnt 可选传 NULL；零锁，运行期只读安全 */
const endpoint_ops_t *endpoint_registry_find_by_slot(int slot, int *out_unloading, int *out_refcnt);

/* Phase 5 XL: 遍历所有 endpoint，第一个 accept(m, &ctx) 返回 NSBC_OK 的就返回。
 * 自动 atomic refcnt++，调用方用完必须 endpoint_registry_dec(*out_slot)。
 * accept 返回 NSBC_PERM_DENIED 也视为"已处理"（安全拦截 endpoint 可以用）。
 * 全部遍历完没有 endpoint 接管 → 返回 NULL。
 *
 * 典型调用方：b2b_server_tu INVITE 分支，在 handle_new_invite 之前加分发：
 *   const endpoint_ops_t *ep = endpoint_registry_try_accept(m, &slot, &ep_ctx);
 *   if (ep) {
 *       ep->handle_invite(build_invite_ctx(co, t, m, ...), NULL);  // endpoint 内部创建 call
 *       endpoint_registry_dec(slot);
 *       return;
 *   }
 *   // fallback: 原有 handle_new_invite 路径 */
const endpoint_ops_t *endpoint_registry_try_accept(sip_msg_t *m, int *out_slot, void **out_ctx);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_ENDPOINT_REGISTRY_H */
