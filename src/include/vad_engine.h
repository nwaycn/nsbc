/* vad_engine.h — AI barge-in VAD 引擎（REQ-AI-BARGEIN-001 P0）
 *
 * 设计约束：
 *   1. 回调在 media_bug global consumer 热路径（每帧调用）——**必须无锁、无阻塞、无 malloc**
 *   2. 状态表预分配（固定大小数组 + call_key 哈希），热路径只查表写 RMS
 *   3. 状态变化（SILENCE↔SPEECH）走 MPSC ring → worker 异步推 data.sock VAD_EVENT
 *   4. 只支持 G.711 PCMU(pt=0) / PCMA(pt=8) —— 其他编码 fallback 到方案 B（hep_sink 外置 ASR）
 *
 * 注册为 media_bug global consumer（name="vad_engine", type_mask=BUG_RAW_RTP）。
 */
#ifndef NSBC_VAD_ENGINE_H
#define NSBC_VAD_ENGINE_H

#include <stdint.h>
#include <stddef.h>
#include "media_bug.h"   /* media_bug_frame_t / media_bug_cb_t */

#ifdef __cplusplus
extern "C" {
#endif

/* 状态机：hangover 参数控制去抖
 *   SILENCE→SPEECH：连续 VAD_RMS_SPEECH_FRAMES 帧 RMS > VAD_RMS_THRESHOLD
 *   SPEECH→SILENCE：连续 VAD_RMS_SILENCE_FRAMES 帧 RMS < VAD_RMS_THRESHOLD */
typedef enum {
    VAD_STATE_SILENCE = 0,
    VAD_STATE_SPEECH  = 1,
} vad_state_t;

/* per-call per-leg 状态槽（固定大小数组） */
#define VAD_MAX_CALLS 256        /* 最大并发 VAD 监控呼叫数 */
#define VAD_LEGS_PER_CALL 2      /* A=0 B=1 */

/* 配置（OAM uuid_vad_start 可覆盖 threshold，其他编译常量） */
#define VAD_DEFAULT_THRESHOLD  800    /* int16_t RMS 阈值（PCM ±32767，静音通常 < 300，人声 > 1000） */
#define VAD_SPEECH_FRAMES      3      /* 连续 3 帧超阈值 → SPEECH（~60ms @ 50fps） */
#define VAD_SILENCE_FRAMES     10     /* 连续 10 帧低于阈值 → SILENCE（~200ms） */
#define VAD_RING_SIZE          4096   /* MPSC ring 槽数 */
#define VAD_HDR_LEN            16     /* 简单 RTP header 最小 12B + 扩展预留 */

/* MPSC ring 里投递的 VAD_EVENT 描述 */
typedef struct {
    void      *call_key;
    int        leg;          /* 0=A 1=B */
    vad_state_t new_state;
    uint16_t   rms;          /* 触发时的 RMS 值 */
    uint64_t   timestamp_ns;
} vad_event_t;

/* ===== 生命周期 ===== */
int  vad_engine_init(void);            /* 注册 media_bug consumer + 起 worker 线程 */
void vad_engine_shutdown(void);        /* 注销 consumer + 停 worker + 清理状态表 */

/* ===== OAM 控制 ===== */
int  vad_start_for_call(void *call_key, int leg_mask, uint16_t threshold);
int  vad_stop_for_call (void *call_key);
void vad_stop_all(void);

/* ===== 内部：media_bug 回调（热路径入口，外部不应调）===== */
void vad_on_rtp_frame(media_bug_frame_t *frame, void *ud);

/* ===== OAM 状态输出（供 show vad / show media_bugs 使用）===== */
int  vad_status(char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_VAD_ENGINE_H */
