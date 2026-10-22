/* playback_engine.h — Phase 3 Advanced Playback Engine（2026-10-15 增量）
 *
 * 单侧媒体注入：给通话某一腿（A/B/both）注入音频源（WAV 文件），
 * 替换/混播实时 RTP 流。用例：等待音乐（MoH）、语音提示、客服坐席
 * 话术。
 *
 * 核心流程：
 *   oam_cmd "playback start <uuid> <wav_path> <side> [loop]"
 *     → pb_start(call_key, wav, side, loop)
 *       → bridge_lookup_relay 拿到 relay + peer
 *       → reactor 注册定时器 20ms 回调
 *         → player_pull 取 PCM 帧
 *           → 封装 RTP 头 (PCMU pt=0)
 *             → sendto relay fd_rtp[side] → 对端
 *               → player_eof → pb_stop 自动清理
 *
 * 线程模型：全部在 reactor 线程跑（定时器回调里 pull+send），零锁。
 * 呼叫结束时 bridge_release_call → pb 注册的 cleanup 回调自动 stop。
 */
#ifndef NSBC_PLAYBACK_ENGINE_H
#define NSBC_PLAYBACK_ENGINE_H

#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* side 参数：0=A leg，1=B leg，-1=both */
#define PB_SIDE_BOTH (-1)

typedef struct pb_ctx pb_ctx_t;

/* 播放引擎全局上下文（单例，engine_start 一次） */
int pb_engine_start(void);
void pb_engine_stop(void);

/* 启动单侧/双侧播放：
 *   call_key — bridge 侧不透明呼叫键（b2b_call_t*，同 bridge_call_view）
 *   wav_path — WAV 文件路径（S16LE 单声道，player_load_wav 自动识别）；
 *              支持管道符拼接："welcome.wav|digits/${x}.wav|serve.wav"
 *   side     — 0=A, 1=B, PB_SIDE_BOTH(-1)=双腿
 *   loop     — 0=播一次 EOF 自动 stop，1=循环（MoH 模式）
 * 返回 NSBC_OK / NSBC_NOENT（找不到呼叫）/ NSBC_NOMEM / NSBC_ERR。
 * 成功后返回播放句柄 out（可选）。 */
int pb_start(void *call_key, const char *wav_path, int side, int loop,
             pb_ctx_t **out);

/* 管道符拼接模式：chain 数 N 个 wav 文件按顺序依次播放
 * （每个 EOF 自动 start 下一个，最后一个 EOF 自动 stop）。 */
int pb_start_chain(void *call_key, const char **wav_list, int n,
                  int side, pb_ctx_t **out);

/* 强制停止指定播放；call_key 用于 find。
 * ESL/API uuid_break 调这个。 */
int pb_stop(void *call_key);

/* 当前播放会话：若 call_key 在播则停掉并返回 1；否则返回 0 */
int pb_stop_if_playing(void *call_key);

/* 查询指定 call_key 是否在播（返回 1/0），可选填 side + eof + loop */
int pb_status(void *call_key, int *side_out, int *loop_out);

/* OAM/ESL 命令注册：
 *   api playback start <uuid> <wav> [side] [loop]
 *   api playback stop  <uuid> */
int pb_register_commands(void);

/* §8 ctx 槽位绑定：让 oam 层能调 pb_stop_if_playing 而不反向 include
 * media 层头文件。core/main 在模块 start 时调一次。 */
struct nsbc_ctx;
void pb_bind_ctx_slots(struct nsbc_ctx *ctx);

/* 统计（OBS 低频路径） */
uint64_t pb_active_count(void);
uint64_t pb_total_players(void);

#ifdef __cplusplus
}
#endif
#endif

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
