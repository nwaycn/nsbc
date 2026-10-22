/* dbal 模块描述符（REQ-DB-001/005）：进程启动期建异步 worker，
 * 关键路径只入队；默认 sqlite WAL，未编入驱动时降级为 none 不阻断启动。 */
#include "app_ctx.h"
#include "cfg.h"
#include "dbal.h"
#include "log.h"
#include "module.h"

static int dbal_mod_init(struct nsbc_ctx *ctx)
{
    /* cfg_get_str 返回单个 __thread 临时窗，driver/dsn 必须各自拷贝，
     * 否则第二次取值会把 driver 覆盖成 dsn。 */
    char driver[64] = "sqlite";
    char dsn[256] = "file:/var/lib/nsbc/nsbc.db";
    const char *sp = NULL;
    long pool = 8, gcms = 10, gcrows = 100;
    if (cfg_get_str(ctx->cfg, "db.driver", &sp, NULL) == NSBC_OK)
        snprintf(driver, sizeof(driver), "%s", sp);
    if (cfg_get_str(ctx->cfg, "db.dsn", &sp, NULL) == NSBC_OK)
        snprintf(dsn, sizeof(dsn), "%s", sp);
    cfg_get_int(ctx->cfg, "db.pool_size", &pool, NULL);
    cfg_get_int(ctx->cfg, "db.group_commit_ms", &gcms, NULL);
    cfg_get_int(ctx->cfg, "db.group_commit_rows", &gcrows, NULL);

    dbal_cfg_t dc = {driver, dsn, (int)pool, (int)gcms, (int)gcrows};
    dbal_t *d = NULL;
    int rc = dbal_create(&dc, &d);
    if (rc != NSBC_OK) {
        llog(LOG_WARN, "dbal", "dbal init failed rc=%d (CDR cache degraded)",
             rc);
        return NSBC_OK; /* 数据库不阻断信令面启动（降级可观测） */
    }
    ctx->dbal = d;
    return NSBC_OK;
}

static void dbal_mod_fini(struct nsbc_ctx *ctx)
{
    dbal_destroy((dbal_t *)ctx->dbal);
    ctx->dbal = NULL;
}

NSBC_MOD0_CAT(dbal, 1, MOD_L_EARLY, 0, MOD_CAT_CORE, dbal_mod_init, dbal_mod_fini);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
