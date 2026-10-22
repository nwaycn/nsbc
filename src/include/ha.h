#ifndef NSBC_HA_H
#define NSBC_HA_H

/* 主备仲裁（REQ-HA-001/002，P0）：UDP 心跳 + 失效检测 + 角色仲裁。
 *
 * P0 范围：单播心跳、连续 failover_ms 不可见判定对端失效、角色迁移
 * （事件入总线/审计）；配合 systemd 进程监督（REQ-7.10）与过载 503
 * （REQ-HA-003）。VIP 漂移由外部 keepalived/LB 执行（本进程只出角色状态）。
 * P1：会话/注册状态复制、BFD、Hitless Upgrade（REQ-HA-004）。
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HA_ROLE_ACTIVE = 0,
    HA_ROLE_STANDBY
} ha_role_t;

/* 未启用 HA 时始终返回 ACTIVE */
ha_role_t ha_role(void);

#ifdef __cplusplus
}
#endif
#endif
