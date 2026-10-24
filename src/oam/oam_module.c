/* oam 层模块描述符：Prometheus exporter + 实时控制接口（REQ-OAM-002/004）。
 * init 创建 cmd_reg（security/billing 等模块 init 阶段就依赖它注册命令），
 * start 绑定端口/套接字。
 * 热加载边界（REQ-OAM-003）：仅 metrics_bind 支持先开后关。 */
#include "module.h"
#include "obs.h"
#include "oam_internal.h"
#include "uDS_client.h"
#include "esl.h"
#include "cmd_registry.h"
#include "playback_engine.h"
#include "vad_engine.h"
#include "log.h"

static int oam_init(struct nsbc_ctx *ctx)
{
    /* CCMD 统一注册表：在 init 阶段创建，让 security/billing/playback 等模块
     * init 阶段就能注册命令（oam_start 绑定 socket 是后续事）。
     * obs_control_start 里有幂等检查，不会覆盖 init 创建的 reg。 */
    return obs_cmd_registry_init(ctx);
}

static int oam_start(struct nsbc_ctx *ctx)
{
    int rc;

    /* A4：事件总线上线后再开放给 ha 等同层模块经 ctx 槽上报 */
    ctx->event_fn = obs_event;

    /* === 第 1 批：观测接口先启动（cmd_reg 已在 init 创建好）=== */
    rc = obs_metrics_start(ctx);
    if (rc != NSBC_OK)
        return rc;
    /* 控制套接字失败不致命（容器内可禁用：置空 unix 路径时返回 INVAL） */
    rc = obs_control_start(ctx);
    if (rc != NSBC_OK && rc != NSBC_INVAL)
        return rc;

    /* === 第 2 批：媒体/数据服务 === */
    /* Playback Engine（Phase 3 增量 2026-10-15）：
     * WAV→PCM S16LE→PCMU→RTP 单侧注入引擎。 */
    pb_engine_start();

    /* Data Service：C→Web write-through + L2 ext.ha1 查询。
     * 路径硬编码（不读配置）：与 Web uds_server 默认路径对齐。 */
    rc = uds_client_init("/var/run/nsbc/data.sock");
    if (rc == NSBC_OK) {
        rc = uds_client_start();
        if (rc != NSBC_OK) {
            llog(LOG_WARN, "oam", "uds_client_start failed rc=%d (non-fatal)", rc);
        } else {
            llog(LOG_NOTICE, "oam", "uds client started → /var/run/nsbc/data.sock");
        }
    } else {
        llog(LOG_WARN, "oam", "uds_client_init failed rc=%d (non-fatal)", rc);
    }

    /* VAD Engine：AI barge-in P0 实时 VAD 引擎。
     * 注册为 media_bug global consumer（无锁热路径），worker 异步推状态变化。 */
    rc = vad_engine_init();
    if (rc != 0) {
        llog(LOG_WARN, "oam", "vad_engine_init failed rc=%d (non-fatal, AI barge-in disabled)", rc);
    } else {
        llog(LOG_NOTICE, "oam", "vad_engine initialized (AI barge-in ready)");
    }

    /* === 第 3 批：ESL Socket ===
     * 2026-10-20 暂停：control.sock 已提供全部 OAM 命令能力，
     * Rust 边车（nsbc-web）只用 data.sock 收事件、control.sock 发命令，
     * esl.sock 在实际部署中从未启用。保留 esl_server.c 文件（留后门），
     * 这里注释掉启动块，有需要时再恢复。 */
#if 0
    if (ctx) {
        esl_cfg_t esl_cfg = {
            .uds_path = "/var/run/nsbc/esl.sock",
            .tcp_addr = "0.0.0.0:8021",
            .max_clients = 64,
            .password = NULL,   /* P0 无认证；P1 加 mTLS */
        };
        if (esl_start(ctx->re, &esl_cfg, obs_control_get_cmd_reg()) == NSBC_OK) {
            ctx->esl_event_fire = esl_event_fire;
            llog(LOG_NOTICE, "oam", "ESL socket started (shared cmd_reg with obs_control)");

            /* Phase 4: 绑定 event_handler 通配订阅 — 内核事件自动推 ESL */
            esl_bind_event_bus();
        } else {
            llog(LOG_WARN, "oam", "ESL start failed (non-fatal, continuing)");
        }
    }
#endif

    obs_event(5, "OAM_STARTED", "metrics, control, playback, uds client, and ESL interface up");
    return NSBC_OK;
}

static void oam_stop(struct nsbc_ctx *ctx)
{
    (void)ctx;

    /* Playback Engine 优雅停机（先停，避免 VAD stop 时还有在播） */
    pb_engine_stop();

    /* VAD Engine：注销 media_bug consumer + 停 worker + 清状态表 */
    vad_engine_shutdown();

    /* ESL Socket 已暂停（2026-10-20）：无启动就不 stop，避免空指针 */
#if 0
    esl_unbind_event_bus();
    esl_stop();
    if (ctx) ctx->esl_event_fire = NULL;
#endif

    /* Data Service 优雅停机：flush ring 里剩余的 CDR/注册事件 */
    uds_client_stop();
    uds_client_shutdown();
    llog(LOG_NOTICE, "oam", "uds client stopped");

    obs_control_stop(ctx);
    obs_metrics_stop(ctx);
}

static const char *oam_deps[] = {"signaling"};
NSBC_MOD_CAT(oam, 1, MOD_L_FINAL, MOD_F_REQUIRED, MOD_CAT_OBSERVABILITY, oam_deps, oam_init, oam_start,
         oam_stop, NULL);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
