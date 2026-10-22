/* 命令注册表——粗骨架（REQ-OAM-008）
 *
 * 装配：oam 模块 init 建 cmd_reg，内建命令（show registrations/
 * calls/counters、reload、stop 等）与各模块命令同表登记；obs_control
 * 控制连接把已认证级别写入 cmd_ctx_t 后调 cmd_execute。
 * L2 二次授权：P0 仅级别位判定（未过级拒绝并提示重认证）；TOTP/
 * 确认令牌在 P1 认证模块补（cmd_ctx_l2_grant 是其回填口）。
 */
#include "cmd_registry.h"

struct cmd_reg {
    cmd_desc_t cmds[CMD_MAX];
    int n;
};

int cmd_reg_create(cmd_reg_t **out)
{
    if (out == NULL)
        return NSBC_INVAL;
    *out = (cmd_reg_t *)calloc(1, sizeof(cmd_reg_t));
    return *out != NULL ? NSBC_OK : NSBC_NOMEM;
}

void cmd_reg_destroy(cmd_reg_t *r)
{
    free(r);
}

static cmd_desc_t *find(cmd_reg_t *r, const char *name)
{
    int i;
    for (i = 0; i < r->n; ++i) {
        if (strcmp(r->cmds[i].name, name) == 0)
            return &r->cmds[i];
    }
    return NULL;
}

int cmd_register(cmd_reg_t *r, const cmd_desc_t *d)
{
    cmd_desc_t *slot;
    if (r == NULL || d == NULL || d->name[0] == '\0' || d->fn == NULL)
        return NSBC_INVAL;
    if (d->level != CMD_L1_MONITOR && d->level != CMD_L2_ADMIN)
        return NSBC_INVAL;
    slot = find(r, d->name);
    if (slot != NULL) {
        *slot = *d; /* 允许同名重登记（模块 reload 口径） */
        return NSBC_OK;
    }
    if (r->n >= CMD_MAX)
        return NSBC_NOSPC;
    r->cmds[r->n++] = *d;
    return NSBC_OK;
}

int cmd_unregister(cmd_reg_t *r, const char *name)
{
    int i;
    if (r == NULL || name == NULL)
        return NSBC_INVAL;
    for (i = 0; i < r->n; ++i) {
        if (strcmp(r->cmds[i].name, name) == 0) {
            r->cmds[i] = r->cmds[--r->n];
            return NSBC_OK;
        }
    }
    return NSBC_NOENT;
}

int cmd_registered(cmd_reg_t *r, const char *name)
{
    return (r != NULL && name != NULL && find(r, name) != NULL) ? 1 : 0;
}

int cmd_execute(cmd_reg_t *r, cmd_ctx_t *c, int argc, char **argv)
{
    cmd_desc_t *d;
    if (r == NULL || c == NULL || argc <= 0 || argv == NULL)
        return NSBC_INVAL;
    d = find(r, argv[0]);
    if (d == NULL)
        return NSBC_NOENT;
    if (c->authed_level < (int)d->level)
        return NSBC_PERM_DENIED;
    if (d->level == CMD_L2_ADMIN && !c->l2_confirmed)
        return NSBC_PERM_DENIED; /* 控制连接据此发起二次授权 */
    return d->fn(c, argc, argv, d->ud);
}

int cmd_help(cmd_reg_t *r, cmd_ctx_t *c, const char *prefix)
{
    int i, n = 0;
    if (r == NULL || c == NULL || c->io_printf == NULL)
        return NSBC_INVAL;
    for (i = 0; i < r->n; ++i) {
        const cmd_desc_t *d = &r->cmds[i];
        if (prefix != NULL && prefix[0] != '\0' &&
            strncmp(d->name, prefix, strlen(prefix)) != 0)
            continue;
        c->io_printf(c->io_ud, "%-24s [%c] %s\r\n", d->name,
                     d->level == CMD_L2_ADMIN ? '2' : '1', d->help);
        ++n;
    }
    return n;
}

int cmd_count(cmd_reg_t *r)
{
    return r == NULL ? 0 : r->n;
}

void cmd_ctx_l2_grant(cmd_ctx_t *c)
{
    if (c != NULL)
        c->l2_confirmed = 1;
}

void cmd_ctx_l2_revoke(cmd_ctx_t *c)
{
    if (c != NULL)
        c->l2_confirmed = 0;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
