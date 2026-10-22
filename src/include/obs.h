#ifndef NSBC_OBS_H
#define NSBC_OBS_H

/* OAM 事件总线公共接口（REQ-OAM-004、REQ-OAM-006，P0）
 *
 * 事件总线：<DOMAIN>_<ACTION> 结构化事件，P0 落内存环形缓冲供实时
 * 控制连接查看，P1 扩展订阅推送（WS/SSE/Webhook）。
 *
 * 注意：指标源注册表机制（REQ-OBS-001/003）已在 A3 下沉为 core 框架
 * 设施 metric_reg.h（metric_source_register / metric_render_all），
 * 业务模块自埋点，oam 只保留 exporter；本头不再含注册表 API。
 */
#include <stddef.h>
#include "cmd_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 事件：P0 环形留存最近 OBS_EVENT_RING 条，供 control 接口 tail 查询 */
void obs_event(const char *domain_action, const char *fmt, ...);

/* 取最近事件（最多 max 条），以 "\n" 分隔；返回写出长度 */
int obs_events_tail(char *buf, size_t cap, int max);

/* REQ-OAM-008 / §8：暴露 cmd_reg 给 ESL/playback/security 等模块注册命令。
 * 返回 NULL 表示 obs_control 未启动或注册表不存在。 */
cmd_reg_t *obs_control_get_cmd_reg(void);

/* 在 init 阶段创建 cmd_registry（让 security/billing 等模块 init 就能注册命令）。
 * obs_control_start 会跳过已创建的 reg（幂等），不会覆盖其他模块已注册的命令。 */
int obs_cmd_registry_init(struct nsbc_ctx *ctx);

#ifdef __cplusplus
}
#endif
#endif
