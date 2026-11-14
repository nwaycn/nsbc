#ifndef NSBC_HA_H
#define NSBC_HA_H

/* 主备仲裁（REQ-HA-001/002，P0）：UDP 心跳 + 失效检测 + 角色仲裁。
 *
 * P0 范围：单播心跳、连续 failover_ms 不可见判定对端失效、角色迁移
 * （事件入总线/审计）；配合 systemd 进程监督（REQ-7.10）与过载 503
 * （REQ-HA-003）。VIP 漂移由外部 keepalived/LB 执行（本进程只出角色状态）。
 * P1：会话/注册状态复制、BFD、Hitless Upgrade（REQ-HA-004）。
 */
#include "ha_replica.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HA_ROLE_ACTIVE = 0,
    HA_ROLE_STANDBY
} ha_role_t;

/* 未启用 HA 时始终返回 ACTIVE */
ha_role_t ha_role(void);

/* === HA P1 状态复制 wrapper（传输层自动装配；返回 repl_publish 的 seq 或 <0 错误）
 * - role 守卫：非 ACTIVE 或 transport 未就绪 → 直接返回 0（不调用 repl_publish）
 * - 业务模块只接 ha.h，不直接 include ha_repl_transport.h/调 repl_publish
 *   （repl_ops.send 由 transport 内部注入，业务层无感） */
int64_t ha_repl_publish(repl_type_t type, uint16_t ver,
                        const void *payload, uint16_t len);

/* === HA P1 备端 apply 订阅（多路分发，transport recv 线程调用）===
 * STANDBY 收到 REPL_REG / REPL_DIALOG / REPL_FULL 时通过本订阅链分发给
 * 业务模块重建内存状态。
 *   apply: 重建回调 — 成功 NSBC_OK；失败 NSBC_INVAL/NOMEM 记 warn
 *   ud:    业务模块私有 ctx（registrar 传 reg_core_t* / b2bua 传 b2b_core_t*）
 * 返回值: 0=注册成功；<0=已满（MAX_SUBSCRIBERS=4）
 * 允许重复注册相同 apply（幂等）。业务模块只 include ha.h。 */
int ha_repl_subscribe_apply(int (*apply)(void *ud, const repl_rec_t *rec),
                            void *ud);

/* 多路 apply 分发 proxy（ha_repl_transport ops.apply 调用本函数，内部遍历订阅链）。
 * 业务模块不要直接调此函数。签名匹配 repl_ops_t.apply — ud 参数忽略。 */
int ha_repl_apply_proxy(void *ud, const repl_rec_t *rec);

/* === HA P1 角色切换回调订阅 ===
 * ha 心跳线程 role 仲裁结果变更时（特别是 STANDBY→ACTIVE failover），
 * 通过本订阅链通知业务模块 — b2bua 在此重建对话锚点、registrar 可选重建其他状态。
 *   cb:   回调 — 业务模块定义，role 参数是目标角色
 *   ud:   业务模块私有 ctx（b2bua 传 b2b_core_t*）
 * 返回值: 0=注册成功；<0=已满（MAX_ROLE_SUBSCRIBERS=4）
 * 线程安全：业务 init 时 subscribe；role switch 发生在心跳线程 — init 时序保证 subscribe
 * 先于首次 role switch。proxy 内部简单数组遍历，无锁。 */
typedef void (*ha_role_switch_fn)(void *ud, ha_role_t role);
int ha_repl_subscribe_role_switch(ha_role_switch_fn cb, void *ud);

/* 角色切换 proxy（ha.c 心跳 transition 内部调）。遍历订阅链分发新 role。 */
void ha_repl_role_switch_proxy(ha_role_t new_role);

/* === HA P1 Hitless Upgrade 冻结控制面 ===
 * freeze_ms>0：ms 后自动解冻；0：永久直到 ha_unfreeze_input()
 * is_frozen()=1 时 INVITE 入站备机丢弃（原有门控）+ 主机拒绝新呼（新增 P1 门控） */
void ha_freeze_input(int freeze_ms);
void ha_unfreeze_input(void);
int  ha_is_frozen(void);

/* === HA P1 Hitless Upgrade 公共入口 ===
 * 调用方: OAM CLI / HTTP API 触发升级
 * 流程: FREEZE → DRAIN (等 in-dialog BYE) → FULL push → YIELD ACTIVE→STANDBY → 返回
 * force: 预留，当前忽略
 * 返回: NSBC_OK=已触发；NSBC_NOENT=非 ACTIVE；NSBC_NOSPC=重入 */
int ha_hitless_upgrade(int force);

/* === HA P1 Drain 活跃对话查询订阅（Hitless Upgrade DRAIN 阶段用）===
 * drain 阶段：FREEZE 后拒绝新呼，等 in-dialog BYE 让 active_count 降到 0 再 yield。
 * 业务模块（b2bua）在 init 时注册 — 返回当前活跃对话数（0=空，可以 yield）。
 * 返回值: 0=注册成功；<0=已满（MAX_DRAIN_SUBSCRIBERS=4） */
typedef int64_t (*ha_drain_count_fn)(void *ud);
int ha_drain_subscribe(ha_drain_count_fn cb, void *ud);

/* DRAIN proxy — 遍历订阅链取最小活跃数（多个业务模块取 max）。
 * 返回 max 活跃数；无订阅时返回 0（安全退化，立即 yield）。 */
int64_t ha_drain_proxy(void);

/* HA P1 只读 OAM（repl connected / ack 水位） */
int      ha_repl_connected(void);
uint64_t ha_repl_last_ack(void);

/* === HA P2 OAM CLI：ha.status ===
 * 填充 buf 返回人类可读的多行状态文本；bsz 足够装下（建议 ≥ 4096）。
 * 内容：role / peer / hb / failover / prio / frozen / health /
 *       replica connected + lag / BFD state / hitless 状态。
 * 返回值：实际写入字节数（snprintf 口径），负数=buf 不够或内部错。
 * 设计为单函数一次性吐 —— 命令处理侧只管 printf，OAM 不暴露 struct 细节。 */
int ha_status(char *buf, size_t bsz);

#ifdef __cplusplus
}
#endif
#endif

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
