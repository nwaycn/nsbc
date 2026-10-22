/* CCMD 呼叫级命令组（REQ-CCMD-001，P0）
 *
 * 命令：uuid_kill / uuid_hold / uuid_transfer / uuid_bridge /
 *       uuid_broadcast / uuid_break / uuid_record / uuid_answer /
 *       uuid_send_dtmf / uuid_getvar / uuid_setvar / originate / status
 *
 * 与 obs_control.c 内建命令（show channels/show calls/hangup/config.* 等）
 * 分离：内建命令保持在 dispatch() 硬编码 if 链，新命令全部注册到
 * cmd_registry（参数校验 + 权限分级 + 审计铁律一次实现）。
 *
 * L1/L2 分级：
 *   uuid_kill / uuid_record (start) / uuid_transfer / uuid_hold /
 *   uuid_bridge / originate / sofia profile <rescan> → L2（写操作）
 *   uuid_dump / uuid_getvar / status → L1（只读）
 *   uuid_answer / uuid_break / uuid_send_dtmf → L2（修改媒体/状态）
 *
 * 安全铁律：
 *   - Call-ID 白名单（b2b_valid_callid）参数先行校验，杜绝帧注入；
 *   - uuid_kill 同步触发 BYE/CANCEL + 释放端口 + 写 CDR（REQ-CDR-001，
 *     b2b_terminate_callid 内部已做）；
 *   - 所有 L2 命令全程审计（log_audit，参数快照但不暴露敏感值）。
 */
#include "app_ctx.h"
#include "b2bua.h"
#include "cmd_registry.h"
#include "log.h"
#include "media_bug.h"
#include "hep_sink.h"
#include "vad_engine.h"
#include "nsbc_common.h"
#include "uDS_client.h"
#include <cjson/cJSON.h>
#include <time.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <unistd.h>

/* ---- 通用辅助：从 cmd_ctx.ud 拿 nsbc_ctx_t ---- */
static nsbc_ctx_t *ctx_from_ud(void *ud)
{
    return (nsbc_ctx_t *)ud;
}

static b2b_core_t *b2b_from_ctx(nsbc_ctx_t *c)
{
    return c ? (b2b_core_t *)c->b2b : NULL;
}

/* ---- Call-ID 参数提取 + 白名单 ---- */
static int pick_callid(cmd_ctx_t *c, char **argv, int argc, char **callid_out)
{
    (void)c;
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        if (c && c->io_printf) c->io_printf(c->io_ud, "ERR missing call-id\n");
        return 0;
    }
    size_t len = strlen(argv[1]);
    if (!b2b_valid_callid(argv[1], len)) {
        if (c && c->io_printf) c->io_printf(c->io_ud, "ERR invalid call-id\n");
        return 0;
    }
    *callid_out = argv[1];
    return 1;
}

/* ==================================================================
 * uuid_kill <call-id> [cause]（REQ-CCMD-001，L2）
 *   cause 默认 487（正常放弃），可选 16(BUSY) / 486(CONGESTION) 等。
 *   审计记 cause + call-id（不暴露其他）。
 * ==================================================================*/

static int cmd_uuid_kill(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;

    int cause = 487;
    if (argc >= 3 && argv[2][0] != '\0')
        cause = atoi(argv[2]);
    if (cause < 1 || cause > 699) {
        c->io_printf(c->io_ud, "ERR invalid cause range [1..699]\n");
        return NSBC_INVAL;
    }

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) {
        c->io_printf(c->io_ud, "ERR b2b not available\n");
        return NSBC_ERR;
    }
    int rc = b2b_terminate_callid(b, callid, strlen(callid), cause);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK killed %s cause=%d\n", callid, cause);
        log_audit("oam", "uuid-kill", "call-id=%s cause=%d result=ok",
                  callid, cause);
    } else if (rc == NSBC_NOENT) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        log_audit("oam", "uuid-kill", "call-id=%s result=notfound", callid);
    } else {
        c->io_printf(c->io_ud, "ERR kill failed rc=%d\n", rc);
        log_audit("oam", "uuid-kill", "call-id=%s result=err rc=%d", callid, rc);
    }
    return rc == NSBC_OK ? NSBC_OK : NSBC_ERR;
}

/* ==================================================================
 * uuid_cancel <call-id> [leg]（REQ-CCMD-001，L2）
 *   leg=-1（默认）双腿都 cancel；0=仅 A；1=仅 B。
 *   腿状态 CALLING/EARLY/reinv 才发 CANCEL；已 confirm/终止 → skip。
 * ==================================================================*/
static int cmd_uuid_cancel(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;

    int leg = -1;
    if (argc >= 3 && argv[2][0] != '\0') {
        leg = atoi(argv[2]);
        if (leg < -1 || leg > 1) {
            c->io_printf(c->io_ud, "ERR leg must be -1|0|1\n"); return NSBC_INVAL;
        }
    }

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }

    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        log_audit("oam", "uuid-cancel", "call-id=%s leg=%d result=notfound", callid, leg);
        return NSBC_NOENT;
    }
    int rc = b2b_cancel_call(call, leg);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK cancel dispatched leg=%d %s\n", leg, callid);
        log_audit("oam", "uuid-cancel", "call-id=%s leg=%d result=ok", callid, leg);
    } else if (rc == NSBC_NOENT) {
        c->io_printf(c->io_ud, "ERR no-early-leg (all confirmed/terminated)\n");
        log_audit("oam", "uuid-cancel", "call-id=%s leg=%d result=no-early-leg", callid, leg);
    } else {
        c->io_printf(c->io_ud, "ERR cancel failed rc=%d\n", rc);
        log_audit("oam", "uuid-cancel", "call-id=%s leg=%d result=err rc=%d", callid, leg, rc);
    }
    return rc;
}

/* ==================================================================
 * uuid_mute <call-id> [on|off] [leg]（REQ-CCMD-001，L2）
 *   默认 on；leg=-1（默认）双腿；0=仅 A；1=仅 B。
 *   改 call->muted[leg] 标志，媒体线程下一个 RTP 包自动清零 payload。
 * ==================================================================*/
static int cmd_uuid_mute(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;

    int on = 1;
    int leg = -1;
    if (argc >= 3 && argv[2][0] != '\0') {
        on = (strcmp(argv[2], "on") == 0) ? 1 : 0;
    }
    if (argc >= 4 && argv[3][0] != '\0') {
        leg = atoi(argv[3]);
        if (leg < -1 || leg > 1) {
            c->io_printf(c->io_ud, "ERR leg must be -1|0|1\n"); return NSBC_INVAL;
        }
    }

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }

    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        log_audit("oam", "uuid-mute", "call-id=%s on=%d leg=%d result=notfound", callid, on, leg);
        return NSBC_NOENT;
    }
    int rc = b2b_mute_call(call, leg, on);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK mute=%d leg=%d %s\n", on, leg, callid);
        log_audit("oam", "uuid-mute", "call-id=%s on=%d leg=%d result=ok", callid, on, leg);
    } else {
        c->io_printf(c->io_ud, "ERR mute failed rc=%d\n", rc);
        log_audit("oam", "uuid-mute", "call-id=%s on=%d leg=%d result=err rc=%d", callid, on, leg, rc);
    }
    return rc;
}

/* ==================================================================
 * uuid_vad_start <call-id> [threshold] — 对指定呼叫启动 VAD 监控
 * uuid_vad_stop  <call-id>            — 停止 VAD 监控
 * ==================================================================*/

static int cmd_uuid_vad_start(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    (void)ud;

    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;

    uint16_t threshold = 0; /* 0 = 用默认值 */
    if (argc >= 3 && argv[2][0] != '\0') {
        int t = atoi(argv[2]);
        if (t < 100 || t > 30000) {
            c->io_printf(c->io_ud, "ERR threshold must be 100..30000\n");
            return NSBC_INVAL;
        }
        threshold = (uint16_t)t;
    }

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }

    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        log_audit("oam", "uuid-vad-start", "call-id=%s result=notfound", callid);
        return NSBC_NOENT;
    }

    int rc = vad_start_for_call(call, 3 /* leg_mask=3 双腿 */, threshold);
    if (rc == 0) {
        c->io_printf(c->io_ud, "OK vad_start call=%s threshold=%u\n", callid,
                     threshold ? threshold : VAD_DEFAULT_THRESHOLD);
        log_audit("oam", "uuid-vad-start", "call-id=%s threshold=%u result=ok",
                  callid, threshold ? threshold : VAD_DEFAULT_THRESHOLD);
    } else {
        c->io_printf(c->io_ud, "ERR vad_start failed rc=%d\n", rc);
        log_audit("oam", "uuid-vad-start", "call-id=%s result=err rc=%d", callid, rc);
    }
    return rc == 0 ? NSBC_OK : NSBC_ERR;
}

static int cmd_uuid_vad_stop(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    (void)ud;

    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }

    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        log_audit("oam", "uuid-vad-stop", "call-id=%s result=notfound", callid);
        return NSBC_NOENT;
    }

    int rc = vad_stop_for_call(call);
    if (rc == 0) {
        c->io_printf(c->io_ud, "OK vad_stop call=%s\n", callid);
        log_audit("oam", "uuid-vad-stop", "call-id=%s result=ok", callid);
    } else {
        c->io_printf(c->io_ud, "ERR vad_stop failed rc=%d\n", rc);
    }
    return rc == 0 ? NSBC_OK : NSBC_ERR;
}

/* ==================================================================
 * uuid_hold <call-id> [on|off]（REQ-CCMD-001，L2）
 *   默认 hold=on；P0 占位 NOTSUP。
 * ==================================================================*/

static int cmd_uuid_hold(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;

    int hold = 1;
    if (argc >= 3 && argv[2][0] != '\0')
        hold = (strcmp(argv[2], "on") == 0) ? 1 : 0;

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }

    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        log_audit("oam", "uuid-hold", "call-id=%s hold=%d result=notfound", callid, hold);
        return NSBC_NOENT;
    }
    int rc = b2b_hold_call(call, -1, hold, NULL, 0);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK hold=%d %s\n", hold, callid);
        log_audit("oam", "uuid-hold", "call-id=%s hold=%d result=ok", callid, hold);
        /* REQ-UDS-002：payload 补齐 from/to，Rust 侧写入 calls 表。
         * state 字段已有（held/active），正确表达 hold/unhold 后状态。
         * cause 不适用；dir 沿用 calls 表已有值（空串保护生效）。*/
        {
            char sess[384];
            snprintf(sess, sizeof(sess),
                     "{\"event\":\"%s\",\"call_id\":\"%s\",\"state\":\"%s\","
                     "\"from\":\"%s\",\"to\":\"%s\"}",
                     hold ? "hold" : "unhold", callid, hold ? "held" : "active",
                     call->leg[0].from, call->leg[0].ruri);
            uds_send_session(sess);
        }
    } else if (rc == NSBC_NOTSUP) {
        c->io_printf(c->io_ud, "ERR not-supported\n");
    } else {
        c->io_printf(c->io_ud, "ERR hold failed rc=%d\n", rc);
    }
    return rc;
}

/* ==================================================================
 * uuid_transfer <call-id> <dest>（REQ-CCMD-001，L2）
 *   P0 占位 NOTSUP。
 * ==================================================================*/

static int cmd_uuid_transfer(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;
    if (argc < 3 || !argv[2] || argv[2][0] == '\0') {
        c->io_printf(c->io_ud, "ERR missing dest\n");
        return NSBC_INVAL;
    }
    const char *dest = argv[2];
    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }
    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        log_audit("oam", "uuid-transfer", "call-id=%s dest=%s result=notfound",
                  callid, dest);
        return NSBC_NOENT;
    }
    int rc = b2b_transfer_call(call, -1, dest, 0, NULL);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK transfered %s → %s\n", callid, dest);
        log_audit("oam", "uuid-transfer", "call-id=%s dest=%s result=ok", callid, dest);
    } else if (rc == NSBC_NOTSUP) {
        c->io_printf(c->io_ud, "ERR not-supported\n");
    } else {
        c->io_printf(c->io_ud, "ERR transfer failed rc=%d\n", rc);
    }
    return rc;
}

/* ==================================================================
 * uuid_bridge <call-id-A> <call-id-B>（REQ-CCMD-001，L2）
 * ==================================================================*/

static int cmd_uuid_bridge(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    if (argc < 3) { c->io_printf(c->io_ud, "ERR usage: uuid_bridge <A> <B>\n"); return NSBC_INVAL; }
    const char *a = argv[1], *b = argv[2];
    if (!b2b_valid_callid(a, strlen(a)) || !b2b_valid_callid(b, strlen(b))) {
        c->io_printf(c->io_ud, "ERR invalid call-id\n"); return NSBC_INVAL;
    }
    b2b_core_t *core = b2b_from_ctx(ctx_from_ud(ud));
    if (!core) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }
    int rc = b2b_bridge(core, a, b);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK bridged %s ↔ %s\n", a, b);
        log_audit("oam", "uuid-bridge", "a=%s b=%s result=ok", a, b);
    } else if (rc == NSBC_NOENT) {
        c->io_printf(c->io_ud, "ERR notfound\n");
    } else if (rc == NSBC_NOTSUP) {
        c->io_printf(c->io_ud, "ERR not-supported\n");
    } else {
        c->io_printf(c->io_ud, "ERR bridge failed rc=%d\n", rc);
    }
    return rc;
}

/* ==================================================================
 * uuid_broadcast / uuid_break（REQ-CCMD-001，L2）
 *   uuid_broadcast <call-id> <path>   注入音频
 *   uuid_break <call-id>              打断当前媒体流
 * ==================================================================*/

static int cmd_uuid_broadcast(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;
    if (argc < 3) { c->io_printf(c->io_ud, "ERR missing wav path\n"); return NSBC_INVAL; }
    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }
    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) { c->io_printf(c->io_ud, "ERR notfound\n"); return NSBC_NOENT; }
    int rc = b2b_broadcast_start(call, -1, argv[2]);
    if (rc == NSBC_OK) c->io_printf(c->io_ud, "OK broadcast %s %s\n", callid, argv[2]);
    else if (rc == NSBC_NOTSUP) c->io_printf(c->io_ud, "ERR not-supported\n");
    else c->io_printf(c->io_ud, "ERR broadcast failed rc=%d\n", rc);
    return rc;
}

static int cmd_uuid_break(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;
    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }
    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) { c->io_printf(c->io_ud, "ERR notfound\n"); return NSBC_NOENT; }

    /* §8 双模扩展：通过 ctx 槽位（非反向 include）停 pb + cancel sleep */
    nsbc_ctx_t *ctx = ctx_from_ud(ud);
    int pb = 0, sl = 0;
    if (ctx) {
        if (ctx->pb_stop_if_playing) pb = ctx->pb_stop_if_playing(call);
        if (ctx->dp_sleep_cancel)   sl = ctx->dp_sleep_cancel(call);
    }

    /* 原有：打断 media_bug/broadcast（NOTSUP 也不是错误——说明没挂 broadcast） */
    int rc = b2b_broadcast_break(call, -1);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK break %s (pb=%d sleep=%d)\n", callid, pb, sl);
    } else if (rc == NSBC_NOTSUP) {
        c->io_printf(c->io_ud, "OK break %s (pb=%d sleep=%d) [broadcast not-active]\n",
                     callid, pb, sl);
        rc = NSBC_OK;  /* broadcast 未挂但 pb/sleep 可能已停——算成功 */
    } else {
        c->io_printf(c->io_ud, "ERR break failed rc=%d\n", rc);
    }

    /* fire ESL 事件 */
    if (ctx && ctx->esl_event_fire) {
        ctx->esl_event_fire("PLAYBACK_STOP",
                            "{\"reason\":\"uuid_break\"}",
                            strlen("{\"reason\":\"uuid_break\"}"));
    }
    return rc;
}

/* ==================================================================
 * uuid_record <call-id> start|stop [path]（REQ-CCMD-001，L2）
 * ==================================================================*/

static int cmd_uuid_record(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;
    if (argc < 3) { c->io_printf(c->io_ud, "ERR usage: uuid_record <id> start|stop [path]\n"); return NSBC_INVAL; }
    const char *action = argv[2];
    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }
    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) { c->io_printf(c->io_ud, "ERR notfound\n"); return NSBC_NOENT; }

    int rc;
    if (strcmp(action, "start") == 0) {
        rc = b2b_record_start(call, 0 /* MIXED */,
                              argc >= 4 ? argv[3] : NULL, "manual");
    } else if (strcmp(action, "stop") == 0) {
        char path[256] = {0};
        rc = b2b_record_stop(call, "manual", path, sizeof(path));
        if (rc == NSBC_OK) {
            c->io_printf(c->io_ud, "OK record stopped %s path=%s\n", callid, path[0] ? path : "-");
            {
                char sess[512];
                snprintf(sess, sizeof(sess),
                         "{\"event\":\"record_stop\",\"call_id\":\"%s\",\"path\":\"%s\"}",
                         callid, path[0] ? path : "");
                uds_send_session(sess);
            }
            return NSBC_OK;
        }
    } else {
        c->io_printf(c->io_ud, "ERR invalid action (start|stop)\n");
        return NSBC_INVAL;
    }
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK record start %s\n", callid);
        {
            char sess[512];
            snprintf(sess, sizeof(sess),
                     "{\"event\":\"record_start\",\"call_id\":\"%s\"}",
                     callid);
            uds_send_session(sess);
        }
        return NSBC_OK;
    }
    if (rc == NSBC_NOTSUP) c->io_printf(c->io_ud, "ERR not-supported\n");
    else c->io_printf(c->io_ud, "ERR record failed rc=%d\n", rc);
    return rc;
}

/* ==================================================================
 * uuid_answer <call-id>（REQ-CCMD-001，L2）——P0 已实装
 * ==================================================================*/

static int cmd_uuid_answer(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;
    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }
    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) { c->io_printf(c->io_ud, "ERR notfound\n"); return NSBC_NOENT; }
    int rc = b2b_answer_call(call, -1, NULL, 0);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK answered %s\n", callid);
        log_audit("oam", "uuid-answer", "call-id=%s result=ok", callid);
        return NSBC_OK;
    }
    if (rc == NSBC_NOENT) { c->io_printf(c->io_ud, "ERR already-connected\n"); return NSBC_OK; }
    c->io_printf(c->io_ud, "ERR answer failed rc=%d\n", rc);
    return rc;
}

/* ==================================================================
 * uuid_send_dtmf <call-id> <digits>（REQ-CCMD-001，L2）
 * ==================================================================*/

static int cmd_uuid_send_dtmf(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;
    if (argc < 3 || !argv[2] || argv[2][0] == '\0' || strlen(argv[2]) > 32) {
        c->io_printf(c->io_ud, "ERR invalid digits len [1..32]\n");
        return NSBC_INVAL;
    }
    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }
    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) { c->io_printf(c->io_ud, "ERR notfound\n"); return NSBC_NOENT; }
    int rc = b2b_send_dtmf(call, 0 /* A 腿（入信令端） */,
                           argv[2], strlen(argv[2]), 26, 100);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK dtmf sent %s %s\n", callid, argv[2]);
        {
            char sess[256];
            snprintf(sess, sizeof(sess),
                     "{\"event\":\"dtmf\",\"call_id\":\"%s\",\"digits\":\"%s\"}",
                     callid, argv[2]);
            uds_send_session(sess);
        }
    } else if (rc == NSBC_NOTSUP) c->io_printf(c->io_ud, "ERR not-supported\n");
    else c->io_printf(c->io_ud, "ERR dtmf failed rc=%d\n", rc);
    return rc;
}

/* ==================================================================
 * send_msg <call-id> <body> / send_msg <body>（无 call-id → 当前会话占位）
 * 对话内发送 MESSAGE（RFC 3428，REQ-CCMD-001），L2，审计。
 * 实际当前会话 UUID 定位是 P1（需会话表）；P0 版本强制 call-id 位置参数。
 * ==================================================================*/
static int cmd_send_msg(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;
    /* body 从 argv[2] 起（含后续所有 argv 拼接，支持空格文本） */
    if (argc < 3 || !argv[2] || argv[2][0] == '\0') {
        c->io_printf(c->io_ud, "ERR missing message body\n");
        return NSBC_INVAL;
    }
    size_t body_cap = 4096;
    char *body = alloca(body_cap);
    body[0] = '\0';
    size_t used = 0;
    for (int i = 2; i < argc; ++i) {
        size_t al = strlen(argv[i]);
        if (used + al + 2 >= body_cap) break;
        if (used) body[used++] = ' ';
        memcpy(body + used, argv[i], al);
        used += al;
    }
    body[used] = '\0';

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }
    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) { c->io_printf(c->io_ud, "ERR notfound\n"); return NSBC_NOENT; }

    log_audit("oam", "send_msg", "callid=%s body=%s", callid, body);

    int rc = b2b_send_message(call, -1 /* 自动选对端 */, body, used);
    if (rc == NSBC_OK) c->io_printf(c->io_ud, "OK msg sent %s %s\n", callid, body);
    else if (rc == NSBC_NOENT) c->io_printf(c->io_ud, "ERR notfound\n");
    else if (rc == NSBC_AGAIN) c->io_printf(c->io_ud, "ERR busy (in-dialog pending)\n");
    else c->io_printf(c->io_ud, "ERR send_msg failed rc=%d\n", rc);
    return rc;
}

/* ==================================================================
 * uuid_getvar / uuid_setvar <call-id> <name> [value]（L1/L2）
 *
 * 通道变量需先挂在 b2b_call_t 上（P1 接线）。P0 占位：
 *   getvar → ERR not-supported
 *   setvar → ERR not-supported
 * 注：uuid_setvar 是 L2（写操作），uuid_getvar 是 L1（只读）。
 * ==================================================================*/

static int cmd_uuid_getvar(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;
    if (argc < 3 || !argv[2] || argv[2][0] == '\0') {
        c->io_printf(c->io_ud, "ERR missing var name\n"); return NSBC_INVAL;
    }
    const char *name = argv[2];

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }

    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        return NSBC_NOENT;
    }
    char val[512];
    int rc = b2b_call_get_var(call, -1, name, val, sizeof(val));
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK %s=%s\n", name, val);
    } else if (rc == NSBC_NOENT) {
        c->io_printf(c->io_ud, "ERR var not found\n");
    } else {
        c->io_printf(c->io_ud, "ERR getvar failed rc=%d\n", rc);
    }
    return rc;
}

static int cmd_uuid_setvar(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;
    if (argc < 3 || !argv[2] || argv[2][0] == '\0') {
        c->io_printf(c->io_ud, "ERR missing var name\n"); return NSBC_INVAL;
    }
    const char *name = argv[2];
    const char *value = (argc >= 4 && argv[3]) ? argv[3] : "";

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }

    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        log_audit("oam", "uuid-setvar", "call-id=%s name=%s result=notfound", callid, name);
        return NSBC_NOENT;
    }
    int rc = b2b_call_set_var(call, -1, name, value);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK set %s\n", name);
        log_audit("oam", "uuid-setvar", "call-id=%s name=%s result=ok", callid, name);
    } else if (rc == NSBC_PERM_DENIED) {
        c->io_printf(c->io_ud, "ERR permission denied (system readonly)\n");
        log_audit("oam", "uuid-setvar", "call-id=%s name=%s result=perm-denied", callid, name);
    } else {
        c->io_printf(c->io_ud, "ERR setvar failed rc=%d\n", rc);
        log_audit("oam", "uuid-setvar", "call-id=%s name=%s result=err rc=%d", callid, name, rc);
    }
    return rc;
}

/* ==================================================================
 * FS 风格 {k=v,k=v} 变量块解析（originate / uuid_bridge 共用）
 *
 * 解析 argv[1] 里最前方的 "{...}" 变量块；无变量块时 *out_buf 指向 src。
 * 变量名大小写不敏感；白名单外变量只 warn 不 reject。
 * ==================================================================*/
static int parse_fs_brace_vars(const char *src, const char **out_buf,
                                int *opt_timeout,
                                char *opt_orig_num, size_t opt_orig_num_sz,
                                char *opt_orig_name, size_t opt_orig_name_sz,
                                char *opt_realm, size_t opt_realm_sz)
{
    if (!src || !*src || !out_buf) return NSBC_INVAL;
    /* 跳过开头空白 */
    while (*src == ' ' || *src == '\t') src++;
    if (*src != '{') { *out_buf = src; return NSBC_OK; }
    const char *rb = strchr(src + 1, '}');
    if (!rb) { *out_buf = src; return NSBC_INVAL; }
    /* 变量串 */
    size_t blen = (size_t)(rb - src - 1);
    if (blen == 0) { *out_buf = rb + 1; return NSBC_OK; }
    char buf[1024];
    if (blen >= sizeof(buf)) blen = sizeof(buf) - 1;
    memcpy(buf, src + 1, blen); buf[blen] = '\0';

    /* 按 ',' 分隔 k=v */
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        /* trim 前后空格 */
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t tlen = strlen(tok);
        while (tlen > 0 && (tok[tlen-1] == ' ' || tok[tlen-1] == '\t')) tok[--tlen] = '\0';
        if (!*tok) continue;
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = tok;
        const char *val = eq + 1;
        if (!*val) continue;

        /* 变量名白名单（strcasecmp 实现大小写不敏感） */
        if (!strcasecmp(key, "originate_timeout")) {
            if (*opt_timeout == 0) { /* 允许 argv[2..] 后置覆盖 */
                int is_num = 1;
                for (const char *p = val; *p; ++p) if (*p < '0' || *p > '9') { is_num = 0; break; }
                if (is_num) {
                    int t = atoi(val);
                    if (t >= 0 && t <= 3600) *opt_timeout = t;
                }
            }
        } else if (!strcasecmp(key, "origination_caller_id_number")) {
            if (!opt_orig_num[0] && strlen(val) < opt_orig_num_sz)
                snprintf(opt_orig_num, opt_orig_num_sz, "%s", val);
        } else if (!strcasecmp(key, "effective_caller_id_number")) {
            /* effective 是 FS 别名 —— 只在 origination 未设时填 */
            if (!opt_orig_num[0] && strlen(val) < opt_orig_num_sz)
                snprintf(opt_orig_num, opt_orig_num_sz, "%s", val);
        } else if (!strcasecmp(key, "origination_caller_id_name")) {
            if (!opt_orig_name[0] && strlen(val) < opt_orig_name_sz)
                snprintf(opt_orig_name, opt_orig_name_sz, "%s", val);
        } else if (!strcasecmp(key, "realm") || !strcasecmp(key, "tenant")) {
            /* 多租户：显式指定被叫所在 realm（租户域）。
             * cmd_originate 会用它把 call-url 重写成 user@realm 格式再调 resolve。 */
            if (opt_realm && opt_realm_sz && !opt_realm[0] && strlen(val) < opt_realm_sz)
                snprintf(opt_realm, opt_realm_sz, "%s", val);
        } else {
            /* 其他 FS 变量（ignore_early_media / export_media / hangup_after_bridge 等）
             * 暂不处理，只 warn */
            llog(LOG_INFO, "oam", "originate brace var skip: %s", key);
        }
    }
    /* out_buf = '}' 之后，跳过后续空白 */
    const char *p = rb + 1;
    while (*p == ' ' || *p == '\t') p++;
    *out_buf = p;
    return NSBC_OK;
}

/* ==================================================================
 * originate [FS-brace-vars]<call-url> [&bridge(url)|&park|...] [key=value...]
 *
 * 完整 FS API 风格 + 原有 key=value 后置兼容。
 * ==================================================================*/

static int cmd_originate(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        c->io_printf(c->io_ud, "ERR missing call-url\n"); return NSBC_INVAL;
    }

    /* ---- 1. FS 大括号变量块 ---- */
    int opt_timeout = 0;
    char opt_caller[128] = {0};
    char opt_caller_name[128] = {0};
    char opt_realm[128] = {0};
    char bridge_url_buf[512] = {0};  /* 前置：argv[2+] 位置参数先写这里 */
    const char *bridge_url_saved = NULL;
    int post_app = B2B_APP_NONE;
    const char *rest = NULL;
    int rc = parse_fs_brace_vars(argv[1], &rest, &opt_timeout,
                                  opt_caller, sizeof(opt_caller),
                                  opt_caller_name, sizeof(opt_caller_name),
                                  opt_realm, sizeof(opt_realm));
    if (rc != NSBC_OK) {
        c->io_printf(c->io_ud, "ERR invalid brace-vars\n"); return NSBC_INVAL;
    }
    if (!rest || !*rest) {
        c->io_printf(c->io_ud, "ERR empty call-url after brace-vars\n"); return NSBC_INVAL;
    }

    /* rest 里可能还有 &app —— 先切出 dest 部分（& 之前） */
    const char *url = rest;
    size_t ul = strlen(url);
    if (ul > 512) { c->io_printf(c->io_ud, "ERR call-url too long\n"); return NSBC_INVAL; }
    const char *amp = strchr(url, '&');
    /* URL 字符集检查（& 之前部分） */
    {
        size_t url_ul = amp ? (size_t)(amp - url) : ul;
        for (size_t i = 0; i < url_ul; ++i) {
            unsigned char ch = (unsigned char)url[i];
            if (ch < 0x21 || ch > 0x7e) { c->io_printf(c->io_ud, "ERR invalid call-url\n"); return NSBC_INVAL; }
        }
    }

    /* ---- 2. argv[2..]：key=value 或位置参数（bridge 目标）----
     * FS 风格 "originate url1 url2" → url2 是 bridge 目标；
     * 也可以混合："originate {vars}url1 url2 timeout=30"。 */
    for (int i = 2; i < argc; ++i) {
        const char *kv = argv[i];
        if (!kv || !*kv) continue;
        const char *eq = strchr(kv, '=');
        if (!eq) {
            /* 非 key=value → bridge 目标（FS 位置参数风格） */
            if (bridge_url_buf[0]) {
                c->io_printf(c->io_ud, "ERR duplicate-bridge-url:%s\n", kv);
                return NSBC_INVAL;
            }
            size_t ulen = strlen(kv);
            if (ulen >= sizeof(bridge_url_buf)) {
                c->io_printf(c->io_ud, "ERR bridge-url too long\n");
                return NSBC_INVAL;
            }
            /* 如果这个位置参数还带 &app（如 "1001&echo"），切出 & 前部分当 bridge URL */
            const char *pa = strchr(kv, '&');
            if (pa) {
                ulen = (size_t)(pa - kv);
                /* & 后面的 app 也解析（同 &app 逻辑） */
                const char *app = pa + 1;
                size_t an = 0;
                while (app[an] && app[an] != '(' && app[an] != ')' &&
                       app[an] != ' ' && app[an] != '\t') an++;
                if (an == 6 && !strncasecmp(app, "bridge", 6)) {
                    /* 嵌套 bridge → 忽略（只支持一层） */
                } else if (an == 4 && !strncasecmp(app, "park", 4)) {
                    post_app = B2B_APP_PARK;
                } else if (an == 6 && !strncasecmp(app, "hangup", 6)) {
                    post_app = B2B_APP_HANGUP;
                } else if (an == 4 && !strncasecmp(app, "echo", 4)) {
                    post_app = B2B_APP_ECHO;
                } else if (an > 0) {
                    llog(LOG_INFO, "oam", "originate argv-app skip: %.*s", (int)an, app);
                }
            }
            memcpy(bridge_url_buf, kv, ulen);
            bridge_url_buf[ulen] = '\0';
            bridge_url_saved = bridge_url_buf;
            llog(LOG_INFO, "oam", "originate: argv[2+] bridge-url='%s'", bridge_url_buf);
            continue;
        }
        size_t klen = (size_t)(eq - kv);
        size_t vlen = strlen(eq + 1);
        if (klen == 0 || vlen == 0) {
            c->io_printf(c->io_ud, "ERR empty-key-or-value in %s\n", kv);
            return NSBC_INVAL;
        }
        if (klen > 32) {
            c->io_printf(c->io_ud, "ERR key-too-long in %s\n", kv);
            return NSBC_INVAL;
        }
        char key[33] = {0};
        memcpy(key, kv, klen);

        if (strcmp(key, "timeout") == 0) {
            const char *v = eq + 1;
            int is_num = 1;
            for (const char *p = v; *p; ++p) if (*p < '0' || *p > '9') { is_num = 0; break; }
            if (!is_num) { c->io_printf(c->io_ud, "ERR timeout:not-an-integer\n"); return NSBC_INVAL; }
            opt_timeout = atoi(v);
            if (opt_timeout < 0 || opt_timeout > 3600) {
                c->io_printf(c->io_ud, "ERR timeout:out-of-range (0..3600)\n"); return NSBC_INVAL;
            }
        } else if (strcmp(key, "orig_caller_id") == 0) {
            if (vlen > 64) { c->io_printf(c->io_ud, "ERR orig_caller_id:too-long\n"); return NSBC_INVAL; }
            const char *v = eq + 1;
            for (const char *p = v; *p; ++p) {
                unsigned char ch = (unsigned char)*p;
                int ok = (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '*' || ch == '#';
                if (!ok) { c->io_printf(c->io_ud, "ERR orig_caller_id:invalid-char\n"); return NSBC_INVAL; }
            }
            snprintf(opt_caller, sizeof(opt_caller), "%s", v);
        } else if (strcmp(key, "orig_caller_name") == 0) {
            if (vlen >= sizeof(opt_caller_name)) {
                c->io_printf(c->io_ud, "ERR orig_caller_name:too-long\n"); return NSBC_INVAL;
            }
            snprintf(opt_caller_name, sizeof(opt_caller_name), "%s", eq + 1);
        } else {
            c->io_printf(c->io_ud, "ERR unknown-param:%s\n", key);
            return NSBC_INVAL;
        }
    }

    /* ---- 3. 尾随 app 解析（&bridge(url) / &park / &hangup / &echo） ----
     * 注意：bridge_url_buf / bridge_url_saved / post_app 已在函数顶部声明，
     * argv[2+] 位置参数可能已写值——这里 &app 解析时要先检查冲突。 */
    if (amp) {
        const char *app = amp + 1;
        /* app 名 = 到 '(' / ')' / 空白 / '\0' 为止 */
        size_t an = 0;
        while (app[an] && app[an] != '(' && app[an] != ')' &&
               app[an] != ' ' && app[an] != '\t') an++;
        if (an == 6 && !strncasecmp(app, "bridge", 6)) {
            const char *lp = strchr(app, '(');
            if (lp) {
                /* &bridge(url) —— 提取括号内 url */
                const char *rp = strchr(lp + 1, ')');
                if (rp && rp > lp + 1) {
                    size_t blen = (size_t)(rp - lp - 1);
                    if (blen < sizeof(bridge_url_buf)) {
                        memcpy(bridge_url_buf, lp + 1, blen);
                        bridge_url_buf[blen] = '\0';
                        bridge_url_saved = bridge_url_buf;
                    }
                }
            }
        } else if (an == 4 && !strncasecmp(app, "park", 4)) {
            post_app = B2B_APP_PARK;      /* REQ-ORIGINATE-003 待补：answer 后 hold 双腿 */
        } else if (an == 6 && !strncasecmp(app, "hangup", 6)) {
            post_app = B2B_APP_HANGUP;    /* REQ-ORIGINATE-003 待补：answer 后挂断 */
        } else if (an == 4 && !strncasecmp(app, "echo", 4)) {
            post_app = B2B_APP_ECHO;      /* 预留（无环回媒体，运行时告警） */
        } else if (an > 0) {
            /* 其他 app（playback/answer 等）暂不支持，忽略不拒 */
            llog(LOG_INFO, "oam", "originate app skip: %.*s", (int)an, app);
        }
    }

    /* ---- 3a. 冲突检测：bridge 模式与 post_app 互斥 ----
     * argv[2+] 位置参数可能先填了 bridge_url，argv[1] 的 &app 又写了 post_app —— 报错。 */
    if (bridge_url_buf[0] && post_app != B2B_APP_NONE) {
        c->io_printf(c->io_ud,
                     "ERR bridge-url AND post-app are mutually exclusive\n");
        return NSBC_INVAL;
    }

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }

    /* ---- 4. 主 dest 路由解析 ---- */
    char ruri[512];
    size_t rlen = amp ? (size_t)(amp - url) : ul;
    if (rlen >= sizeof(ruri)) rlen = sizeof(ruri) - 1;
    memcpy(ruri, url, rlen);
    ruri[rlen] = '\0';

    /* ---- 4a. FS call-url → SIP URI 重写（多租户 realm）----
     * 如果大括号块里显式传了 realm/tenant，且 ruri 里没有 '@'（纯 FS 风格
     * `user/1001` 或 `1001`），重写成 `1001@realm` 让 resolve 正确路由到
     * 租户的 Location DB。有 '@'（已是 SIP URI 格式）则原样透传。 */
    if (opt_realm[0] && !strchr(ruri, '@')) {
        /* 支持两种 FS 格式：
         *   "user/1001" → "1001@realm"
         *   "1001"      → "1001@realm" */
        const char *fs_num = ruri;           /* 默认整个 ruri 就是号码 */
        const char *slash = strchr(ruri, '/');
        if (slash) fs_num = slash + 1;        /* user/1001 → 取 / 后面 */
        size_t num_len = strlen(fs_num);
        size_t realm_len = strlen(opt_realm);
        size_t new_len = num_len + 1 /* @ */ + realm_len;
        if (new_len < sizeof(ruri)) {
            memmove(ruri, fs_num, num_len);   /* 覆盖写 */
            ruri[num_len] = '@';
            memcpy(ruri + num_len + 1, opt_realm, realm_len);
            ruri[new_len] = '\0';
            rlen = new_len;
            llog(LOG_INFO, "oam", "originate: ruri rewritten → '%s' (realm=%s)",
                 ruri, opt_realm);
        }
    }

    b2b_orig_t o;
    memset(&o, 0, sizeof(o));
    o.from_user = "nsbc-api";
    o.to_uri = ruri;

    nsbc_sa_t dst;
    char out_ruri[256];
    char out_to[256];
    memset(&dst, 0, sizeof(dst));
    rc = b2b_resolve_dst(b, ruri, rlen, &dst,
                             out_ruri, sizeof(out_ruri),
                             out_to, sizeof(out_to));
    if (rc != NSBC_OK) {
        c->io_printf(c->io_ud, "ERR resolve failed rc=%d\n", rc);
        log_audit("oam", "originate", "url=%s result=resolve-fail rc=%d", argv[1], rc);
        return rc;
    }
    o.to_uri = out_ruri[0] ? out_ruri : ruri;
    o.dst = dst;

    /* ---- 5. bridge URL 路由解析（如有） ---- */
    if (bridge_url_saved && bridge_url_buf[0]) {
        char br_ruri[512];
        size_t br_len = strlen(bridge_url_buf);
        memcpy(br_ruri, bridge_url_buf, br_len + 1);

        /* ---- 5a. bridge URL 也需要 FS → SIP URI 重写（realm）----
         * 同主 dest：有 realm 且无 @ → 重写成 user@realm */
        if (opt_realm[0] && !strchr(br_ruri, '@')) {
            const char *fs_num = br_ruri;
            const char *slash = strchr(br_ruri, '/');
            if (slash) fs_num = slash + 1;
            size_t num_len = strlen(fs_num);
            size_t realm_len = strlen(opt_realm);
            size_t new_len = num_len + 1 + realm_len;
            if (new_len < sizeof(br_ruri)) {
                memmove(br_ruri, fs_num, num_len);
                br_ruri[num_len] = '@';
                memcpy(br_ruri + num_len + 1, opt_realm, realm_len);
                br_ruri[new_len] = '\0';
                br_len = new_len;
                llog(LOG_INFO, "oam",
                     "originate: bridge-ruri rewritten → '%s' (realm=%s)",
                     br_ruri, opt_realm);
            }
        }

        nsbc_sa_t br_dst;
        char br_out_ruri[256], br_out_to[256];
        memset(&br_dst, 0, sizeof(br_dst));
        rc = b2b_resolve_dst(b, br_ruri, br_len, &br_dst,
                                br_out_ruri, sizeof(br_out_ruri),
                                br_out_to, sizeof(br_out_to));
        if (rc != NSBC_OK) {
            c->io_printf(c->io_ud, "ERR bridge resolve failed rc=%d\n", rc);
            log_audit("oam", "originate", "bridge=%s result=resolve-fail rc=%d", bridge_url_buf, rc);
            return rc;
        }
        o.bridge_url = bridge_url_buf;
        o.bridge_dst = br_dst;
    }

    /* ---- 6. 传递参数 ---- */
    if (opt_timeout > 0)
        o.timeout_ms = opt_timeout * 1000;
    if (opt_caller[0])
        o.orig_caller_id = opt_caller;
    if (opt_caller_name[0])
        o.orig_caller_id_name = opt_caller_name;
    /* REQ-ORIGINATE-003 待补：尾随 app（&park/&hangup/&echo），bridge 模式此值为 NONE */
    o.post_app = post_app;

    b2b_call_t *call = NULL;
    rc = b2b_originate(b, &o, &call);
    if (rc == NSBC_OK) {
        char cid[DLG_CALLID_MAX] = {0};
        if (call) {
            call_lock(call);
            /* bridge 模式：report 先呼的 leg[0]（A 腿）；单腿模式：leg[1] */
            int report_leg = (bridge_url_saved && bridge_url_buf[0]) ? 0 : 1;
            snprintf(cid, sizeof(cid), "%s", call->leg[report_leg].callid[0]
                     ? call->leg[report_leg].callid : "");
            call_unlock(call);
        }
        c->io_printf(c->io_ud, "OK originate-dispatched call-id=%s\n", cid);
        log_audit("oam", "originate", "url=%s call-id=%s bridge=%s result=ok",
                  argv[1], cid, bridge_url_buf[0] ? bridge_url_buf : "-");
    } else if (rc == NSBC_NOSPC) {
        c->io_printf(c->io_ud, "ERR no-space (concurrent limit)\n");
        log_audit("oam", "originate", "url=%s result=no-space", argv[1]);
    } else if (rc == NSBC_TIMEOUT) {
        c->io_printf(c->io_ud, "ERR originate timeout\n");
        log_audit("oam", "originate", "url=%s result=timeout", argv[1]);
    } else {
        c->io_printf(c->io_ud, "ERR originate failed rc=%d\n", rc);
        log_audit("oam", "originate", "url=%s result=err rc=%d", argv[1], rc);
    }
    return rc;
}

/* ==================================================================
 * status（REQ-CCMD-001，L1 只读）—— 系统综合状态
 * ==================================================================*/

static int cmd_status(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)argc; (void)argv;
    if (!c || !c->io_printf) return NSBC_INVAL;
    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    char out[4096];
    int rc = b2b_status(b, out, sizeof(out));
    if (rc == NSBC_OK)
        c->io_printf(c->io_ud, "%s", out);
    else
        c->io_printf(c->io_ud, "ERR status unavailable\n");
    return rc;
}

/* ==================================================================
 * sofia profile <name> rescan（REQ-SIP-PROFILE-001，L2）
 *
 * 与 sip_profiles_reload_json 同口径——reload_commands.c 已有
 * profiles.reload，这里做别名：解析出 profile 名 → 调 sip_profile 重加载。
 * P0 占位：直接返回 ERR not-supported（实际 reloadjson 接口需 JSON 体）。
 * ==================================================================*/

static int cmd_sofia(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)ud;
    if (!c || !c->io_printf) return NSBC_INVAL;
    /* argv[1]=profile / argv[2]=<name> / argv[3]=rescan */
    if (argc < 4 || !argv[1] || strcmp(argv[1], "profile") != 0 ||
        !argv[2] || argv[2][0] == '\0' || !argv[3] || strcmp(argv[3], "rescan") != 0) {
        c->io_printf(c->io_ud, "ERR usage: sofia profile <name> rescan\n");
        return NSBC_INVAL;
    }
    c->io_printf(c->io_ud, "ERR not-supported (use profiles.reload)\n");
    log_audit("oam", "sofia-profile-rescan", "name=%s result=not-supported", argv[2]);
    return NSBC_NOTSUP;
}

/* ==================================================================
 * uuid_media_bug <call-id> start|stop|list [type-mask] [filter]
 *
 * REQ-MEDIA-BUG-001 L2：为指定 call 的媒体流附加 bug。
 *   start [mask=0x1F] [filter]  — mask 可省略默认全开；filter 未来用
 *   stop  [bug-id]               — 不指定 bug-id 则停所有 per-call bugs
 *   list                         — 列所有 per-call bugs（含 id/type/attach_time）
 * ==================================================================*/

static int cmd_uuid_media_bug(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)ud;
    if (!c || !c->io_printf) return NSBC_INVAL;
    if (argc < 3) {
        c->io_printf(c->io_ud, "ERR usage: uuid_media_bug <call-id> start|stop|list [mask]\n");
        return NSBC_INVAL;
    }
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;
    const char *action = argv[2];

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    b2b_call_t *call = NULL;
    if (b) call = b2b_find_callid(b, callid, strlen(callid));

    if (strcmp(action, "start") == 0) {
        uint32_t mask = 0x0F; /* 默认全开：RAW_RTP|RTCP|HEADER|SIP */
        if (argc >= 4 && argv[3]) {
            mask = (uint32_t)strtoul(argv[3], NULL, 0);
            if (mask == 0 || mask > 0xFF) {
                c->io_printf(c->io_ud, "ERR invalid mask (0x1..0xFF)\n");
                return NSBC_INVAL;
            }
        }
        if (!call) {
            c->io_printf(c->io_ud, "ERR call not found %s\n", callid);
            return NSBC_NOENT;
        }
        /* leg_mask=3 双腿；cb=NULL 让帧只被全局 consumers（hep_sink）处理 */
        uint64_t bug_id = media_bug_attach(call, 3,
                                           (media_bug_type_t)mask, NULL, NULL);
        if (bug_id > 0) {
            c->io_printf(c->io_ud, "OK bug started id=%lu mask=0x%x %s\n",
                         (unsigned long)bug_id, mask, callid);
            log_audit("oam", "uuid-media-bug",
                      "call-id=%s mask=0x%x result=ok id=%lu",
                      callid, mask, (unsigned long)bug_id);
            return NSBC_OK;
        }
        c->io_printf(c->io_ud, "ERR attach failed\n");
        return NSBC_ERR;
    }
    if (strcmp(action, "stop") == 0) {
        if (argc >= 4 && argv[3]) {
            uint64_t bug_id = (uint64_t)strtoull(argv[3], NULL, 0);
            media_bug_detach(bug_id);
            c->io_printf(c->io_ud, "OK bug %lu stopped\n", (unsigned long)bug_id);
            log_audit("oam", "uuid-media-bug",
                      "call-id=%s action=stop id=%lu",
                      callid, (unsigned long)bug_id);
        } else {
            if (!call) {
                c->io_printf(c->io_ud, "ERR call not found %s\n", callid);
                return NSBC_NOENT;
            }
            media_bug_detach_all_for_call(call);
            c->io_printf(c->io_ud, "OK all bugs stopped for %s\n", callid);
            log_audit("oam", "uuid-media-bug",
                      "call-id=%s action=stop-all", callid);
        }
        return NSBC_OK;
    }
    if (strcmp(action, "list") == 0) {
        char out[1024];
        media_bug_status(out, sizeof(out));
        c->io_printf(c->io_ud, "%s", out);
        return NSBC_OK;
    }
    c->io_printf(c->io_ud, "ERR unknown action (start|stop|list)\n");
    return NSBC_INVAL;
}

/* ==================================================================
 * media_bug purge_all — 全局强制清理所有 bugs（L2，审计）
 * ==================================================================*/

static int cmd_media_bug_purge(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)argc; (void)argv; (void)ud;
    if (!c || !c->io_printf) return NSBC_INVAL;
    media_bug_purge_all();
    /* purge_all 返回 void；用 status 再看一眼清理后的 per-call bugs 数 */
    char out[512];
    media_bug_status(out, sizeof(out));
    c->io_printf(c->io_ud, "OK purged all bugs\n%s", out);
    log_audit("oam", "media-bug-purge", "all purged");
    return NSBC_OK;
}

/* ==================================================================
 * hep status|flush|reload — HEP3 推流 OAM（REQ-MEDIA-HEP-001）
 * ==================================================================*/

static int cmd_hep(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)ud;
    if (!c || !c->io_printf) return NSBC_INVAL;
    if (argc < 2) { c->io_printf(c->io_ud, "ERR usage: hep status|flush|reload\n"); return NSBC_INVAL; }
    const char *action = argv[1];
    char out[2048];
    if (strcmp(action, "status") == 0) {
        hep_cmd_status(out, sizeof(out));
        c->io_printf(c->io_ud, "%s", out);
        return NSBC_OK;
    }
    if (strcmp(action, "flush") == 0) {
        hep_cmd_flush(out, sizeof(out));
        c->io_printf(c->io_ud, "%s", out);
        return NSBC_OK;
    }
    if (strcmp(action, "reload") == 0) {
        hep_cmd_reload(out, sizeof(out));
        c->io_printf(c->io_ud, "%s", out);
        return NSBC_OK;
    }
    c->io_printf(c->io_ud, "ERR unknown action (status|flush|reload)\n");
    return NSBC_INVAL;
}

/* ==================================================================
 * test_push_cdr — 往 data.sock 推一条模拟 CDR，验证 write-through 链路
 *   用法: test_push_cdr
 *   返回: OK pushed <call_id> → 走 uds_send_cdr → data.sock → Web → SQLite
 *         ERR uds_client not started → data.sock 没连上
 *         ERR uds_send_cdr rc=N → push 失败
 * ==================================================================*/
static int cmd_test_push_cdr(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)argc; (void)argv; (void)ud;
    if (!c || !c->io_printf) return NSBC_INVAL;

    time_t now = time(NULL);
    char cid[64];
    snprintf(cid, sizeof(cid), "TEST-%ld", (long)now);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "call_id",     cid);
    cJSON_AddStringToObject(j, "tenant",      "test");
    cJSON_AddStringToObject(j, "from",        "1001");
    cJSON_AddStringToObject(j, "to",          "1002");
    cJSON_AddStringToObject(j, "trunk",       "SIP-TEST");
    cJSON_AddStringToObject(j, "rec_path",    "TEST");
    cJSON_AddStringToObject(j, "dir",         "outbound");
    cJSON_AddStringToObject(j, "phase",       "setup");
    cJSON_AddStringToObject(j, "disposition", "NORMAL_CLEARING");
    cJSON_AddNumberToObject(j, "start_ms",    (double)(now * 1000));
    cJSON_AddNumberToObject(j, "answer_ms",   (double)(now * 1000 + 1000));
    cJSON_AddNumberToObject(j, "end_ms",       (double)(now * 1000 + 5000));
    cJSON_AddNumberToObject(j, "billsec",      4);
    cJSON_AddNumberToObject(j, "duration_s",   5);
    cJSON_AddNumberToObject(j, "cause",        16);
    cJSON_AddBoolToObject(j,   "emergency",    0);

    char *s = cJSON_PrintUnformatted(j);
    if (!s) {
        c->io_printf(c->io_ud, "ERR json build failed\n");
        cJSON_Delete(j);
        return NSBC_ERR;
    }

    int rc = uds_send_cdr(s);
    cJSON_free(s);
    cJSON_Delete(j);

    if (rc != NSBC_OK) {
        c->io_printf(c->io_ud, "ERR uds_send_cdr rc=%d\n", rc);
        return rc;
    }
    uds_flush_now();
    c->io_printf(c->io_ud, "OK pushed %s → data.sock → Web → SQLite\n", cid);
    return NSBC_OK;
}

/* ==================================================================
 * uuid_displace <call-id>（REQ-CCMD-001 L2）
 *   FreeSWITCH 风格：让 B 腿 answer + A 腿 unhold，接管通话。
 *   实现：若 B 腿已 CONFIRMED 则直接 unhold A 腿；否则先 answer B 腿再 unhold。
 *   复用 b2b_answer_call + b2b_hold_call。
 * ==================================================================*/
static int cmd_uuid_displace(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }

    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        return NSBC_NOENT;
    }

    /* 1) B 腿尚未接通 → 先 answer（200 OK） */
    leg_state_t bs = b2b_leg_state(call, 1);
    if (bs != LEG_CONFIRMED) {
        int rc = b2b_answer_call(call, 1, NULL, 0);
        if (rc != NSBC_OK && rc != NSBC_NOENT) {
            c->io_printf(c->io_ud, "ERR answer B-leg failed rc=%d\n", rc);
            return rc;
        }
    }

    /* 2) A 腿 unhold（hold=0）——这就是 displace 的核心 */
    int rc = b2b_hold_call(call, 0, 0, NULL, 0);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK displaced %s\n", callid);
        log_audit("oam", "uuid-displace", "call-id=%s result=ok", callid);
    } else {
        c->io_printf(c->io_ud, "ERR displace failed rc=%d\n", rc);
        log_audit("oam", "uuid-displace", "call-id=%s result=err rc=%d", callid, rc);
    }
    return rc;
}

/* ==================================================================
 * uuid_park <call-id>（REQ-CCMD-001 L2）
 *   Park = A 腿 hold（媒体停、SIP dialog 保留）。
 *   直接复用 b2b_hold_call(call, leg=-1, hold=1, ...) 双腿 hold。
 * ==================================================================*/
static int cmd_uuid_park(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }

    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        return NSBC_NOENT;
    }
    int rc = b2b_hold_call(call, -1, 1 /*hold*/, NULL, 0);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK parked %s\n", callid);
        log_audit("oam", "uuid-park", "call-id=%s result=ok", callid);
    } else {
        c->io_printf(c->io_ud, "ERR park failed rc=%d\n", rc);
        log_audit("oam", "uuid-park", "call-id=%s result=err rc=%d", callid, rc);
    }
    return rc;
}

/* ==================================================================
 * uuid_unpark <call-id>（REQ-CCMD-001 L2）
 *   反向 park：双腿 unhold，恢复媒体。
 * ==================================================================*/
static int cmd_uuid_unpark(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }

    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        return NSBC_NOENT;
    }
    int rc = b2b_hold_call(call, -1, 0 /*unhold*/, NULL, 0);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK unparked %s\n", callid);
        log_audit("oam", "uuid-unpark", "call-id=%s result=ok", callid);
    } else {
        c->io_printf(c->io_ud, "ERR unpark failed rc=%d\n", rc);
        log_audit("oam", "uuid-unpark", "call-id=%s result=err rc=%d", callid, rc);
    }
    return rc;
}

/* ==================================================================
 * uuid_deflect <call-id> <dest>（REQ-CCMD-001 L2）
 *   盲转（blind transfer via REFER，FreeSWITCH 风格叫 deflect）。
 *   复用 b2b_transfer_call(call, -1, dest, CCF_XFER_BLIND, NULL)。
 * ==================================================================*/
static int cmd_uuid_deflect(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;
    if (argc < 3 || !argv[2] || argv[2][0] == '\0') {
        c->io_printf(c->io_ud, "ERR missing dest\n");
        return NSBC_INVAL;
    }
    const char *dest = argv[2];

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }

    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        log_audit("oam", "uuid-deflect", "call-id=%s dest=%s result=notfound",
                  callid, dest);
        return NSBC_NOENT;
    }

    int rc = b2b_transfer_call(call, -1, dest, 0 /*CCF_XFER_BLIND*/, NULL);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK deflected %s → %s\n", callid, dest);
        log_audit("oam", "uuid-deflect", "call-id=%s dest=%s result=ok", callid, dest);
    } else if (rc == NSBC_NOTSUP) {
        c->io_printf(c->io_ud, "ERR not-supported\n");
    } else {
        c->io_printf(c->io_ud, "ERR deflect failed rc=%d\n", rc);
        log_audit("oam", "uuid-deflect", "call-id=%s dest=%s result=err rc=%d",
                  callid, dest, rc);
    }
    return rc;
}

/* ==================================================================
 * uuid_wait_for_answer <call-id> [timeout_ms]（REQ-CCMD-001 L1 读+微阻塞）
 *   阻塞轮询 B 腿状态，等 LEG_CONFIRMED（200 OK 收到）或 LEG_TERMINATED（失败/挂断）。
 *   默认 30s 超时，每 20ms 轮询一次（调用 call_lock 持锁读 state）。
 *   注意：OAM 线程持 reactor 的 call->lock 不会阻塞 reactor 太久——轮询间隔短、
 *   锁持有极短（仅读 state 立即释放），对实时性影响可忽略。
 * ==================================================================*/
static int cmd_uuid_wait_for_answer(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    char *callid = NULL;
    if (!pick_callid(c, argv, argc, &callid)) return NSBC_INVAL;

    int timeout_ms = 30000;
    if (argc >= 3 && argv[2] && argv[2][0] != '\0') {
        int is_num = 1;
        for (const char *p = argv[2]; *p; ++p)
            if (*p < '0' || *p > '9') { is_num = 0; break; }
        if (!is_num) {
            c->io_printf(c->io_ud, "ERR timeout:not-an-integer\n");
            return NSBC_INVAL;
        }
        timeout_ms = atoi(argv[2]);
        if (timeout_ms <= 0 || timeout_ms > 3600000) {
            c->io_printf(c->io_ud, "ERR timeout:out-of-range (1..3600000)\n");
            return NSBC_INVAL;
        }
    }

    b2b_core_t *b = b2b_from_ctx(ctx_from_ud(ud));
    if (!b) { c->io_printf(c->io_ud, "ERR b2b not available\n"); return NSBC_ERR; }

    b2b_call_t *call = b2b_find_callid(b, callid, strlen(callid));
    if (!call) {
        c->io_printf(c->io_ud, "ERR notfound\n");
        return NSBC_NOENT;
    }

    /* 轮询 B 腿 state */
    int waited = 0;
    int step_ms = 20;
    while (waited < timeout_ms) {
        leg_state_t bs;
        call_lock(call);
        bs = call->leg[1].state;
        call_unlock(call);

        if (bs == LEG_CONFIRMED) {
            c->io_printf(c->io_ud, "OK answered after %d ms\n", waited);
            log_audit("oam", "uuid-wait-for-answer",
                      "call-id=%s waited_ms=%d result=ok", callid, waited);
            return NSBC_OK;
        }
        if (bs == LEG_TERMINATED) {
            c->io_printf(c->io_ud, "ERR B-leg failed (state=TERMINATED) before answer\n");
            log_audit("oam", "uuid-wait-for-answer",
                      "call-id=%s result=terminated", callid);
            return NSBC_ERR;
        }
        usleep((useconds_t)step_ms * 1000);
        waited += step_ms;
    }
    c->io_printf(c->io_ud, "ERR timeout after %d ms\n", timeout_ms);
    log_audit("oam", "uuid-wait-for-answer",
              "call-id=%s timeout_ms=%d result=timeout", callid, timeout_ms);
    return NSBC_TIMEOUT;
}

/* ==================================================================
 * 命令注册函数（供 oam 模块 start 时调）
 *   reg   : cmd_registry 句柄（obs_control.c 里的 cmd_reg）
 *   ctx   : 全局 nsbc_ctx_t（每个 cmd_desc_t.ud 共享，命令内解引用拿 b2b/sip_tr/rec_mgr）
 * ==================================================================*/

void call_commands_register(cmd_reg_t *reg, nsbc_ctx_t *ctx)
{
    if (!reg) return;
    /* ud 统一传 ctx，命令函数内部按需解引用拿 b2b/bridge 等 */
    cmd_desc_t descs[] = {
        /* 呼叫级（L1 只读）——uuid_dump 由 obs_control.c 硬编码 if 链实装，
         * 此处不注册；status / uuid_getvar 是新增 */
        { "uuid_getvar",   "获取通道变量（L1，只读）",
          CMD_L1_MONITOR, cmd_uuid_getvar, ctx },
        { "status",        "系统综合状态（L1，只读）",
          CMD_L1_MONITOR, cmd_status, ctx },

        /* 呼叫级（L2 写操作） */
        { "uuid_kill",     "强制挂断通话 [cause]（L2，审计）",
          CMD_L2_ADMIN, cmd_uuid_kill, ctx },
        { "uuid_cancel",   "CANCEL 早期 INVITE [leg=-1|0|1]（L2，审计）",
          CMD_L2_ADMIN, cmd_uuid_cancel, ctx },
        { "uuid_hold",     "hold/resume on|off（L2，审计）",
          CMD_L2_ADMIN, cmd_uuid_hold, ctx },
        { "uuid_mute",     "mute/resume on|off [leg=-1|0|1]（L2，审计）",
          CMD_L2_ADMIN, cmd_uuid_mute, ctx },
        { "uuid_vad_start","启动 VAD 监控 <call-id> [threshold]（L2，AI barge-in P0）",
          CMD_L2_ADMIN, cmd_uuid_vad_start, ctx },
        { "uuid_vad_stop", "停止 VAD 监控 <call-id>（L2，AI barge-in P0）",
          CMD_L2_ADMIN, cmd_uuid_vad_stop, ctx },
        { "uuid_transfer", "盲转 REFER 到 dest（L2，审计）",
          CMD_L2_ADMIN, cmd_uuid_transfer, ctx },
        { "uuid_displace", "B-leg answer + A-leg unhold 接管通话（L2，审计）",
          CMD_L2_ADMIN, cmd_uuid_displace, ctx },
        { "uuid_park",     "hold 双腿暂停媒体（L2，审计）",
          CMD_L2_ADMIN, cmd_uuid_park, ctx },
        { "uuid_unpark",   "unhold 双腿恢复媒体（L2，审计）",
          CMD_L2_ADMIN, cmd_uuid_unpark, ctx },
        { "uuid_deflect",  "盲转 REFER 到 dest（FreeSWITCH deflect 别名，L2，审计）",
          CMD_L2_ADMIN, cmd_uuid_deflect, ctx },
        { "uuid_wait_for_answer", "阻塞等待 B-leg 200 OK [timeout_ms]（L1 读+微阻塞）",
          CMD_L2_ADMIN, cmd_uuid_wait_for_answer, ctx },
        { "uuid_bridge",   "桥接两呼叫（L2，审计）",
          CMD_L2_ADMIN, cmd_uuid_bridge, ctx },
        { "uuid_broadcast","向通话注入音频（L2）",
          CMD_L2_ADMIN, cmd_uuid_broadcast, ctx },
        { "uuid_break",    "打断当前媒体流（L2）",
          CMD_L2_ADMIN, cmd_uuid_break, ctx },
        { "uuid_record",   "start|stop 录制（L2）",
          CMD_L2_ADMIN, cmd_uuid_record, ctx },
        { "uuid_answer",   "200 OK 应答（L2）",
          CMD_L2_ADMIN, cmd_uuid_answer, ctx },
        { "uuid_send_dtmf","发送 DTMF 序列（L2）",
          CMD_L2_ADMIN, cmd_uuid_send_dtmf, ctx },
        { "send_msg",      "对话内发送 MESSAGE 消息体（L2，审计）",
          CMD_L2_ADMIN, cmd_send_msg, ctx },
        { "uuid_setvar",   "设置通道变量（L2，审计）",
          CMD_L2_ADMIN, cmd_uuid_setvar, ctx },

        /* 全局级（L2） */
        { "originate",     "发起外呼 call-url [&app]（L2，审计）",
          CMD_L2_ADMIN, cmd_originate, ctx },
        { "sofia",         "sofia profile <name> rescan（L2，审计）",
          CMD_L2_ADMIN, cmd_sofia, ctx },

        /* REQ-MEDIA-BUG-001：媒体 bug 命令族 */
        { "uuid_media_bug","附加/媒体 bug start|stop|list（L2，审计）",
          CMD_L2_ADMIN, cmd_uuid_media_bug, ctx },
        { "media_bug",     "media_bug purge_all（L2，强制清理）",
          CMD_L2_ADMIN, cmd_media_bug_purge, ctx },

        /* REQ-MEDIA-HEP-001：HEP3 推流 OAM */
        { "hep",           "hep status|flush|reload（L1/L2 混合，审计）",
          CMD_L2_ADMIN, cmd_hep, ctx },

        /* data.sock write-through 测试 */
        { "test_push_cdr", "往 data.sock 推一条模拟 CDR 验证 write-through",
          CMD_L2_ADMIN, cmd_test_push_cdr, ctx },
    };
    for (size_t i = 0; i < sizeof(descs)/sizeof(descs[0]); ++i) {
        int rc = cmd_register(reg, &descs[i]);
        if (rc != NSBC_OK)
            llog(LOG_WARN, "oam", "cmd '%s' register rc=%d", descs[i].name, rc);
    }
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
