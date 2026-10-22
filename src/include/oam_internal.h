#ifndef NSBC_OAM_INTERNAL_H
#define NSBC_OAM_INTERNAL_H

/* oam 层内部装配接口（模块 start/stop 调用），不对 mod 层暴露 */
#include "app_ctx.h"
#include "cmd_registry.h"

int obs_metrics_start(nsbc_ctx_t *ctx);
void obs_metrics_stop(nsbc_ctx_t *ctx);

int obs_control_start(nsbc_ctx_t *ctx);
void obs_control_stop(nsbc_ctx_t *ctx);

/* 命令注册组：oam 模块 start 时一次性调，填 cmd_registry */
void reload_commands_register(cmd_reg_t *reg);
void call_commands_register(cmd_reg_t *reg, nsbc_ctx_t *ctx);

#endif
