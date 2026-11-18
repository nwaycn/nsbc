/* vad_engine.h — AI barge-in VAD 引擎（REQ-AI-BARGEIN-001 P0 → P1 升级）
 *
 * v2 架构升级（从 na_stream/VadWord 移植生产级算法）：
 *   旧：单 RMS 固定阈值 + 二态 FSM（SILENCE↔SPEECH）
 *   新：双窗口能量估计（NEAR 近窗 vs FAR 远窗=背景噪声）+ 自适应阈值 + 三态 FSM
 *       + 二阶 Butterworth 高通滤波（strips DC offset + mains hum)
 *       + 1s 回溯环形缓冲 + 120ms pre-roll（语音前缀不丢）
 *       + HESITATION 中间停顿状态（说一半歇气不算断句）
 *
 * 设计约束：
 *   1. 回调在 media_bug global consumer 热路径（每帧 ~20ms 调一次）
 *      → 必须零 malloc、零阻塞、零锁（producer 无锁写 tail，消费者原子读）
 *   2. 状态表预分配（固定大小数组 + call_key 线性探查哈希）
 *   3. 状态变化走 MPSC ring → worker 异步推 data.sock VAD_EVENT
 *   4. 只支持 G.711 PCMU(pt=0) / PCMA(pt=8) —— 其他编码 fallback
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

/* ===== FSM 状态机（三态，表驱动）===== */
typedef enum {
    VAD_STATE_SILENCE    = 0,  /* 静音 / 初始状态 */
    VAD_STATE_SPEECH     = 1,  /* 语音活跃 */
    VAD_STATE_HESITATION = 2,  /* 中间停顿（说一半歇气，不算断句） */
    VAD_STATE_NUM        = 3,
} vad_state_t;

/* ===== per-call per-leg 状态槽 ===== */
#define VAD_MAX_CALLS       256     /* 最大并发 VAD 监控呼叫数 */
#define VAD_LEGS_PER_CALL   2       /* A=0 B=1 */

/* ===== 帧内双窗口能量参数（对齐 8kHz 采样率）===== */
#define VAD_NEAR_SAMPLES    80      /* 近窗 = 10ms @8kHz — 当前能量 */
#define VAD_FAR_SAMPLES     800     /* 远窗 = 100ms @8kHz — 背景噪声基线 */
#define VAD_TRACE_SIZE      8000    /* 1s @8kHz 环形缓冲 — 回溯 pre-roll */
#define VAD_PREROLL_SAMPLES 960     /* 120ms pre-roll — 检测到 SPEECH 后向前回溯 */

/* ===== 能量阈值（PCM sample avg abs 值，绝对基线）===== */
#define VAD_SILENCE_VOLUME_THRESHOLD  60    /* 绝对噪声 floor */
#define VAD_ACTIVE_VOLUME_THRESHOLD   700   /* 绝对阈值 ceiling（人声正常范围 100-3000）*/

/* 兼容旧名 —— 外部 OAM call_commands.c 还在用 VAD_DEFAULT_THRESHOLD */
#define VAD_DEFAULT_THRESHOLD VAD_ACTIVE_VOLUME_THRESHOLD

/* ===== 状态机持续时间下限（sample 单位，8kHz）===== */
#define VAD_START_ACTIVE_MIN_SAMPLES  640   /* 80ms — SPEECH 必须持续 ≥80ms 才算有效说话 */
#define VAD_PAUSE_ACTIVE_MIN_SAMPLES  320   /* 40ms — HESITATION 恢复 SPEECH 的最短激活 */
#define VAD_START_SILENCE_MIN_SAMPLES 1200  /* 150ms — SPEECH→SILENCE 的最短静默（断句判定）*/
#define VAD_PAUSE_SILENCE_MIN_SAMPLES 320   /* 40ms — SPEECH→HESITATION 的最短静默 */

/* ===== MPSC ring ===== */
#define VAD_RING_SIZE       4096    /* 事件 ring 槽数 */
#define VAD_HDR_LEN         16      /* RTP header 最小预留 */

/* ===== HPF 参数（二阶 Butterworth 高通）===== */
#define VAD_HPF_CUTOFF_HZ   50      /* strips DC offset + 50Hz mains hum */
#define VAD_HPF_Q_BUTTER    0.70710678f  /* 1/sqrt(2) */

/* ===== MPSC ring 里投递的 VAD_EVENT 描述 ===== */
typedef struct {
    void        *call_key;
    int          leg;              /* 0=A 1=B */
    vad_state_t  new_state;        /* 新状态 */
    uint16_t     rms;              /* 触发时刻的 RMS（诊断） */
    uint16_t     threshold;         /* 触发时刻的自适应 threshold（诊断）*/
    uint64_t     timestamp_ns;
    int          has_preroll;      /* 1=pre-roll 数据已就绪 */
    int          preroll_offset;    /* 回溯偏移（samples），从 trace_wpos 往前数 */
} vad_event_t;

/* ===== HPF：二阶 Butterworth 高通滤波器（跨模块共享）=====
 * ported from na_stream/VadWord. Strips DC offset + 50Hz mains hum.
 * Usage: one vad_hpf_t per call per leg. Call after G.711 decode, before RMS/energy estimation.
 * Direct Form I 实现，热路径零 malloc。 */
typedef struct {
    float b0, b1, b2;   /* 分子系数 */
    float a1, a2;       /* 分母系数（RBJ Cookbook a2 已带负号处理）*/
    float x1, x2;       /* 输入延迟线 */
    float y1, y2;       /* 输出延迟线 */
} vad_hpf_t;

void vad_hpf_init   (vad_hpf_t *hpf, float cutoff_hz, float sample_rate);
void vad_hpf_process(vad_hpf_t *hpf, int16_t *pcm, size_t samples);

/* ===== 生命周期 ===== */
int  vad_engine_init(void);            /* 注册 media_bug consumer + 起 worker 线程 */
void vad_engine_shutdown(void);        /* 注销 consumer + 停 worker + 清理状态表 */

/* ===== OAM 控制 ===== */
int  vad_start_for_call(void *call_key, int leg_mask, uint16_t threshold);
int  vad_stop_for_call (void *call_key);
void vad_stop_all(void);

/* REQ-AI-BARGEIN-001: 查询 per-call per-leg VAD 状态（外部 AI barge-in 决策用）*/
int  vad_query_for_call(void *call_key, int leg, vad_state_t *state_out, uint16_t *rms_out);

/* v2 新增：取 pre-roll 数据（从 trace ring 向前回溯 120ms）
 * 返回 0=成功，
 *   **out_ring_ptr → trace_data[leg] 起始指针（环形）
 *   *out_start_idx → pre-roll 起始索引（0..VAD_TRACE_SIZE-1）
 *   *out_samples → pre-roll 样本数（最多 960 = 120ms @8kHz）
 * 外部使用：先读 out_ring_ptr[out_start_idx..TRACE_SIZE-1]，
 *   环形回绕再读 out_ring_ptr[0..剩余数-1] */
int  vad_get_preroll(void *call_key, int leg,
                     short **out_ring_ptr, int *out_start_idx, int *out_samples);

/* ===== 内部：media_bug 回调（热路径入口，外部不应调）===== */
void vad_on_rtp_frame(media_bug_frame_t *frame, void *ud);

/* ===== OAM 状态输出（供 show vad / show media_bugs 使用）===== */
int  vad_status(char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_VAD_ENGINE_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
