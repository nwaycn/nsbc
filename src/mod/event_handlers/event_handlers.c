/* 内部事件订阅与对外分发（REQ-OAM-004/006，P1）：
 * 控制连接 / WebSocket / Webhook 统一订阅 oam 事件总线，
 * 不按渠道再拆目录。P0 仅模块占位（事件总线核心在 oam 层）。 */
#include "module.h"

static int event_handlers_init(struct nsbc_ctx *ctx)
{
    (void)ctx;
    return NSBC_OK;
}

NSBC_MOD0_CAT(event_handlers, 1, MOD_L_FINAL, 0, MOD_CAT_OBSERVABILITY, event_handlers_init, NULL);
