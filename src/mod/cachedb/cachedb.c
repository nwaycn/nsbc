/* 缓存驱动统一抽象（local/redis/memcached，P1）：
 * 配置/业务属性经 RCU 缓存（REQ-DB-006），键值驱动经本模块注册。
 * P0 仅模块占位（本地 RCU 缓存已在核心侧）。 */
#include "module.h"

static int cachedb_init(struct nsbc_ctx *ctx)
{
    (void)ctx;
    return NSBC_OK;
}

NSBC_MOD0_CAT(cachedb, 1, MOD_L_FINAL, 0, MOD_CAT_UTILITY, cachedb_init, NULL);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
