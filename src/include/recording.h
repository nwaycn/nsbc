/* 通话录音模块层 API（REQ-REC-001~004）
 *
 * 与 recorder.h 分工：
 *   recorder.h    — media 层，纯录制设备（rec_open / rec_feed / rec_stop / rec_stat）
 *   recording.h   — module 层，挂载 nsbc_ctx、接 dialplan action、维护
 *                   per-callid 会话注册表、fini 兜底 flush。
 *
 * 包含链：callctrl → recording.h; recording.c → recorder.h + recording.h
 *        避免 media 反向 include module（recorder.h 不 include recording.h） */
#ifndef NSBC_RECORDING_H
#define NSBC_RECORDING_H

#include "recorder.h"
#include "media_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* dialplan action 接线（REQ-REC-003）。
 *
 * callctrl 在 dp_execute 返回 dp_result 后调用：
 *   if (res.rec_action != 0) {
 *       char rec_path[REC_PATH_MAX];
 *       recording_apply_dp_action(ctx->rec_mgr, res.rec_action,
 *                                 call->call_id, "dialplan",
 *                                 rec_path, sizeof(rec_path));
 *       // stop 时回填 cdr->rec_path，供 poller 写 DB → cdr.metadata.rec_path
 *       cdr->rec_path[0] = '\0';
 *       if (rec_path[0]) snprintf(cdr->rec_path, sizeof(cdr->rec_path), "%s", rec_path);
 *   }
 *
 * 动作码与 dp_rule_t.rec_action 对齐：
 *   0 = none（默认，不调用本函数）
 *   1 = start recording
 *   2 = stop recording（常见 hangup_any 兜底）
 *
 * REQ-REC-007：rec_path_template 透传。dialplan 规则里配的路径模板
 * （"{caller}-{callee}-{callid}.wav" 之类），空串=用系统默认命名。
 * caller/callee 填主叫号/被叫号，供模板变量替换。 */
int recording_apply_dp_action(rec_mgr_t *mgr, uint8_t rec_action,
                              const char *callid, const char *reason,
                              const char *rec_path_template,
                              const char *caller, const char *callee,
                              char *rec_path_out, size_t path_cap);

/* 媒体快路径喂帧（bridge/mixer PCM 回调里调用，每帧一次）。
 * leg=-1 表示上层已经 MIXED 合并好双腿 */
int recording_feed_frame(const char *callid, int leg, const media_frame_t *frm);

/* dp_rule_t.rec_action 构造器：从字符串解析动作码。
 *   "start" / "recording=start" / "action=recording=start" → 1
 *   "stop"  / "recording=stop"  / "action=recording=stop"  → 2
 *   其他 → 0（none）
 * 用在 dialplan XML / ini 解析器里把 rule 文本属性落成 dp_rule_t.rec_action。 */
uint8_t recording_parse_action(const char *s);

/* REQ-REC-006：按 callid+leg 反查 rec_sess_t*（bridge.rec_feed_fn wrapper 用）。
 * 分腿独立录制：leg=0 返回 A-leg mono sess，leg=1 返回 B-leg mono sess。
 * 返回 NULL 表示该 callid 该腿未开录制。 */
rec_sess_t *recording_find_by_callid(const char *callid, int leg);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_RECORDING_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
