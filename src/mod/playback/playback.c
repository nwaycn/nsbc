/* mod_playback — FreeSWITCH 风格播放模块
 *
 * 职责：
 *   1. 生命周期：pb_engine_start → 注册 dialplan apps → pb_engine_stop
 *   2. Dialplan apps：
 *        playback         — 阻塞播放 WAV 直到 EOF（uuid_broadcast 风格，P0 简化：启动后立即返回，
 *                           调用方（dp_execute）自己决定是否等 EOF；后续 P1 用 event_wait 加 sync 模式）
 *        playback_loop    — MoH/等待音乐模式（loop=1）
 *        stop_playback    — 显式停当前 leg 播放
 *        playback_chain   — 管道符拼接 "a.wav|b.wav|c.wav"，P0 按序启动 P1 自动串联
 *   3. ESL 事件：PLAYBACK_START / PLAYBACK_STOP（playback_engine.c 内部已 fire）
 *   4. 依赖：player.c（src/media/play/）、media_bridge.c（relay 查找）
 *
 * 设计约束：模块不依赖 signaling/billing 等高层模块，只依赖 media_bridge 和 dialplans 的 app_registry。 */
#include "module.h"
#include "playback_engine.h"
#include "app_ctx.h"       /* app_ctx_t / nsbc_ctx_t */
#include "app_registry.h"  /* app_register */
#include "dp_sleep.h"      /* dp_sleep_app_reg() */
#include "nsbc_common.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------
 * app_playback — FreeSWITCH uuid_broadcast / uuid_displace 风格
 *   data = "<wav_path>[|side][|loop]"  — 管道符参数，默认 side=PB_SIDE_BOTH, loop=0
 * ----------------------------------------------------------------*/
static app_rc_t app_playback_exec(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!c || !c->call_key) return APP_RC_FAILED;

    /* 解析参数：| 分隔 */
    char *wav_path = NULL;
    int side = PB_SIDE_BOTH;
    int loop = 0;

    if (data && data[0]) {
        char *dup = strdup(data);
        if (!dup) return APP_RC_FAILED;
        char *tok = strtok(dup, "|");
        if (tok) {
            wav_path = tok;
            tok = strtok(NULL, "|");
            if (tok) {
                /* side 参数：0=A, 1=B, both/-1=双腿 */
                if (strcasecmp(tok, "both") == 0 || strcmp(tok, "-1") == 0)
                    side = PB_SIDE_BOTH;
                else
                    side = atoi(tok);
                tok = strtok(NULL, "|");
                if (tok) {
                    loop = atoi(tok);
                }
            }
        }
        if (wav_path) wav_path = strdup(wav_path); /* 独立拷贝（dup 会被 strtok 修改）*/
        free(dup);
    }

    if (!wav_path || !wav_path[0]) {
        llog(LOG_WARN, "pb", "app_playback: no wav_path");
        free(wav_path);
        return APP_RC_FAILED;
    }

    /* 先停可能在播的（避免同一 leg 多个播放重叠）*/
    pb_stop_if_playing(c->call_key);

    pb_ctx_t *pb = NULL;
    int rc = pb_start(c->call_key, wav_path, side, loop, &pb);
    if (rc != NSBC_OK) {
        llog(LOG_WARN, "pb", "app_playback start FAIL call=%p wav=%s rc=%d",
             c->call_key, wav_path, rc);
        free(wav_path);
        return APP_RC_FAILED;
    }

    llog(LOG_INFO, "pb", "app_playback OK call=%p wav=%s side=%d loop=%d pb=%p",
         c->call_key, wav_path, side, loop, (void *)pb);
    free(wav_path);

    /* 未来 P1：block 模式等 EOF（event_wait PLAYBACK_STOP 事件）；
     * P0 简化：立即返回，播放由 reactor 时间轮后台跑。 */
    return APP_RC_OK;
}

/* ----------------------------------------------------------------
 * app_playback_loop — MoH / 等待音乐模式（loop=1，参数同 playback）
 * ----------------------------------------------------------------*/
static app_rc_t app_playback_loop_exec(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!c || !c->call_key) return APP_RC_FAILED;

    const char *wav = data ? data : NULL;
    if (!wav || !wav[0]) return APP_RC_FAILED;

    pb_stop_if_playing(c->call_key);

    pb_ctx_t *pb = NULL;
    int rc = pb_start(c->call_key, wav, PB_SIDE_BOTH, 1 /* loop=1 */, &pb);
    if (rc != NSBC_OK) {
        llog(LOG_WARN, "pb", "app_playback_loop FAIL call=%p wav=%s rc=%d",
             c->call_key, wav, rc);
        return APP_RC_FAILED;
    }

    llog(LOG_INFO, "pb", "app_playback_loop OK call=%p wav=%s pb=%p",
         c->call_key, wav, (void *)pb);
    return APP_RC_OK;
}

/* ----------------------------------------------------------------
 * app_stop_playback — 显式停当前 leg 播放（hangup_any / digit 打断）
 * ----------------------------------------------------------------*/
static app_rc_t app_stop_playback_exec(app_ctx_t *c, const char *data, void *ud)
{
    (void)data; (void)ud;
    if (!c || !c->call_key) return APP_RC_FAILED;

    int stopped = pb_stop_if_playing(c->call_key);
    llog(LOG_INFO, "pb", "app_stop_playback call=%p stopped=%d", c->call_key, stopped);
    return APP_RC_OK;
}

/* ----------------------------------------------------------------
 * app_playback_chain — 管道符拼接多文件
 *   data = "a.wav|b.wav|c.wav"
 *   P0：内部用 pb_start 依次启动，每个 EOF 自动 stop（播放顺序由 reactor 并行）；
 *   P1：pb_start_chain 存 chain list 到 pb_ctx_t，EOF 自动 start 下一个（真正串联）
 * ----------------------------------------------------------------*/
static app_rc_t app_playback_chain_exec(app_ctx_t *c, const char *data, void *ud)
{
    (void)ud;
    if (!c || !c->call_key || !data || !data[0]) return APP_RC_FAILED;

    /* 管道符 split */
    char *dup = strdup(data);
    if (!dup) return APP_RC_FAILED;
    int cap = 8;
    const char **list = (const char **)calloc(cap, sizeof(*list));
    if (!list) { free(dup); return APP_RC_FAILED; }
    int n = 0;
    char *tok = strtok(dup, "|");
    while (tok) {
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok) - 1;
        while (end > tok && *end == ' ') *end = '\0';
        if (n + 1 >= cap) {
            cap *= 2;
            list = (const char **)realloc(list, cap * sizeof(*list));
            if (!list) break;
        }
        list[n++] = tok;
        tok = strtok(NULL, "|");
    }
    free(dup);

    if (n == 0) { free(list); return APP_RC_FAILED; }

    pb_stop_if_playing(c->call_key);

    /* P0：依次启动（注意 pb_start_chain 当前也是 P0 简化实现，
     * 实际是并行播第一个文件，后续文件调用方自己决定）*/
    pb_ctx_t *pb = NULL;
    int rc = pb_start_chain(c->call_key, list, n, PB_SIDE_BOTH, &pb);
    free(list);
    if (rc != NSBC_OK) {
        llog(LOG_WARN, "pb", "app_playback_chain FAIL call=%p rc=%d", c->call_key, rc);
        return APP_RC_FAILED;
    }

    llog(LOG_INFO, "pb", "app_playback_chain OK call=%p n=%d pb=%p",
         c->call_key, n, (void *)pb);
    return APP_RC_OK;
}

/* =================================================================
 * 模块生命周期
 * =================================================================*/
static int pb_init(struct nsbc_ctx *ctx)
{
    if (!ctx) return NSBC_INVAL;

    int rc = pb_engine_start();
    if (rc != NSBC_OK) {
        llog(LOG_WARN, "pb", "pb_engine_start FAIL rc=%d — playback disabled", rc);
        return NSBC_OK; /* 模块仍然成功 init，只是 engine 没起来 */
    }

    /* 让 oam / call_commands 能调 pb_stop_if_playing */
    pb_bind_ctx_slots(ctx);

    /* 注册 dialplan apps */
    app_reg_t *reg = dp_sleep_app_reg();
    if (!reg) {
        llog(LOG_WARN, "pb", "dp_sleep_app_reg() null — apps not registered");
        return NSBC_OK;
    }

    app_desc_t descs[] = {
        { "playback",
          "阻塞播放 WAV 直到 EOF (data=wav_path[|side][|loop], side=0/1/both, loop=0|1)",
          app_playback_exec, NULL },
        { "playback_loop",
          "等待音乐/MoH 模式 (data=wav_path)",
          app_playback_loop_exec, NULL },
        { "stop_playback",
          "显式停当前 leg 播放（hangup_any / 按键打断）",
          app_stop_playback_exec, NULL },
        { "playback_chain",
          "管道符拼接多文件按序播放 (data=a.wav|b.wav|c.wav)",
          app_playback_chain_exec, NULL },
    };
    int n = (int)(sizeof(descs) / sizeof(descs[0]));
    for (int i = 0; i < n; ++i) {
        int arc = app_register(reg, &descs[i]);
        llog(LOG_INFO, "pb", "Phase 0: app_register %s=%d", descs[i].name, arc);
    }

    llog(LOG_INFO, "pb", "mod_playback init OK — %d apps registered", n);
    return NSBC_OK;
}

static void pb_fini(struct nsbc_ctx *ctx)
{
    if (!ctx) return;

    /* 先反注册 apps — 防止 shutdown 期间还有新调用进来 */
    app_reg_t *reg = dp_sleep_app_reg();
    if (reg) {
        app_unregister(reg, "playback");
        app_unregister(reg, "playback_loop");
        app_unregister(reg, "stop_playback");
        app_unregister(reg, "playback_chain");
    }

    pb_engine_stop();
    llog(LOG_INFO, "pb", "mod_playback fini OK");
}

/* =================================================================
 * NSBC_MOD 宏 — 模块定义
 * 依赖：无（media_bridge 是所有媒体层模块的基础，不需要显式声明）
 * level = MOD_L_CALL_PATH — 拨号计划层播放属于呼叫生命周期核心能力
 * =================================================================*/
NSBC_MOD(playback,         /* name */
         1,                /* version */
         MOD_L_FINAL,      /* level：所有基础框架 ready 后才 init */
         0,                /* flags */
         (const char *[]){ NULL },  /* deps：无依赖，NULL 哨兵数组 */
         pb_init,          /* init */
         NULL,             /* start：engine_start 在 init 内完成 */
         NULL,             /* stop */
         pb_fini);         /* fini */
