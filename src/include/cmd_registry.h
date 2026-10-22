#ifndef NSBC_CMD_REGISTRY_H
#define NSBC_CMD_REGISTRY_H

/* OAM 控制命令注册表（REQ-OAM-008，P0 统一入口 / P1 二级授权）
 *
 * 背景：obs_control 的命令分发为硬编码 if 链，模块（registrar/
 * billing/ha）想加 show/control 命令得改 oam 核心。注册表让模块 init
 * 时登记命令（名称/帮助/权限级/处理函数），控制连接统一 lookup 执行。
 * 权限两级：
 *  L1（monitor）：只读 show/get，登录口令；
 *  L2（admin）：写操作/reload/kill，需二次授权（控制连接重认证或
 *  TOTP/确认令牌，P1；P0 先做级别判定，未过级返 PERM_DENIED）。
 * 分层：oam（3）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CMD_NAME_MAX  32
#define CMD_HELP_MAX  128
#define CMD_MAX       256
#define CMD_LINE_MAX  1024

typedef enum {
    CMD_L1_MONITOR = 1,   /* 只读 */
    CMD_L2_ADMIN = 2      /* 写/高危 */
} cmd_level_t;

/* 执行上下文：输出写 io_printf（控制连接/测试缓冲），uid 已认证身份，
 * ud 为注册时携带的模块对象指针。 */
typedef struct {
    void *uid;
    int   authed_level;
    int   l2_confirmed;        /* 本连接已完成 L2 二次授权 */
    int  (*io_printf)(void *io_ud, const char *fmt, ...);
    void *io_ud;
} cmd_ctx_t;

typedef int (*cmd_fn_t)(cmd_ctx_t *c, int argc, char **argv, void *ud);

typedef struct {
    char        name[CMD_NAME_MAX];
    char        help[CMD_HELP_MAX];
    cmd_level_t level;
    cmd_fn_t    fn;
    void       *ud;
} cmd_desc_t;

typedef struct cmd_reg cmd_reg_t;

int  cmd_reg_create(cmd_reg_t **out);
void cmd_reg_destroy(cmd_reg_t *r);

int  cmd_register(cmd_reg_t *r, const cmd_desc_t *d);
int  cmd_unregister(cmd_reg_t *r, const char *name);

/* 只读查询：命令名是否已注册（无副作用）。控制连接在 token 化
 * 参数缓冲前先预判：仅注册表命令才允许就地切词，内建硬编码命令
 * 的原始参数（如 rate.reload 的整段 JSON）必须保持完整。 */
int  cmd_registered(cmd_reg_t *r, const char *name);

/* 执行已切词命令行；L2 未二次授权返 PERM_DENIED（控制连接据此发起
 * 重认证流程）。未知命令 NOENT。 */
int  cmd_execute(cmd_reg_t *r, cmd_ctx_t *c, int argc, char **argv);

/* 帮助：逐行 "name  help"（io 写出），prefix 可空=全部 */
int  cmd_help(cmd_reg_t *r, cmd_ctx_t *c, const char *prefix);
int  cmd_count(cmd_reg_t *r);

/* ---- 命令组注册（obs_control.c 启动时装配） ----
 * reload_commands.c：extensions/gateways/trunks/dialplan-rules/profiles
 *                    .reload + profiles.list（REQ-DATA-SVC-003）
 * call_commands.c：uuid_* / status / originate / sofia / media_bug /
 *                  hep 呼叫级命令组（REQ-CCMD-001） */
struct nsbc_ctx;
void reload_commands_register(cmd_reg_t *r);
void call_commands_register(cmd_reg_t *r, struct nsbc_ctx *ctx);

/* L2 二次授权结果回填（认证模块校验通过后调），仅置位本会话 */
void cmd_ctx_l2_grant(cmd_ctx_t *c);
void cmd_ctx_l2_revoke(cmd_ctx_t *c);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_CMD_REGISTRY_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
