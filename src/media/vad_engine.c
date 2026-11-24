/* vad_engine.c — AI barge-in VAD 引擎实现（v2 生产级算法）
 *
 * v2: production VAD algorithm ported from na_stream/VadWord
 *   旧：单 RMS 固定阈值 + 二态 FSM → 噪音环境下漏检/误报
 *   Butterworth HPF (DC+mains) + dual-window energy (NEAR vs FAR background)
 *       + 自适应阈值 + 三态表驱动 FSM + 1s trace ring + 200ms pre-roll
 *
 * 热路径约束：vad_on_rtp_frame 是 media_bug global consumer 回调
 *   —— 必须无锁、无阻塞、无 I/O、无 malloc。
 *
 * 并发模型（继承 + 增强）：
 *   - g_states 数组加全局 pthread_mutex_t g_states_lock — OAM 线程
 *     （vad_start_for_call/vad_stop_for_call）和热路径（vad_on_rtp_frame）
 *     并发读写 g_states[idx].active/call_key/threshold/state 时持锁。
 *     锁内只做简单赋值（<10ns），不会阻塞；
 *   - g_ring.shutdown 从 volatile 改成 _Atomic int，跨线程可见性保证。
 *   - 热路径 producer 无锁写 ring.tail（atomic store_release），
 *     consumer 侧读 tail（atomic load_acquire）后取事件。
 */

#include "vad_engine.h"
#include "media_bug.h"
#include "rtp.h"
#include "nsbc_common.h"    /* nsbc_rc_t, NSBC_OK */
#include "media_codec.h"   /* CODEC_PCMU=0, CODEC_PCMA=8 */
#include "g711.h"           /* g711_ulaw_decode, g711_alaw_decode */
#include "uDS_client.h"     /* uds_send_vad —— 推 JSON VAD_EVENT 到 data.sock */
#include "log.h"
#include "common_audio/vad/include/webrtc_vad.h"  /* 二级判据：WebRTC VAD（频谱 GMM，压制敲击类瞬态）*/

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* WebRTC VAD 激进等级（0..3）：2 = 较激进，能压掉敲门/敲桌子的宽带瞬态，
 * 同时保留正常语音；调到 3 更严格但可能漏掉轻声字。 */
#define VAD_WEBRTC_MODE     2
#define VAD_WEBRTC_RATE     8000
#define VAD_WEBRTC_FRAME    80      /* 10ms @8kHz —— WebRTC VAD 只接受 10/20/30ms */

/* ============================================================================
 * ===== HPF：二阶 Butterworth 高通滤波器（RBJ Cookbook 系数）=====
 *   strips DC offset and 50Hz mains hum. Direct Form I, hot-path friendly.
 *  vad_hpf_t 类型定义已移至 vad_engine.h（跨模块共享）。
 * ==========================================================================*/

void vad_hpf_init(vad_hpf_t *hpf, float cutoff_hz, float sample_rate) {
    if (!hpf || sample_rate <= 0.0f) return;
    if (cutoff_hz < 1.0f) cutoff_hz = 1.0f;
    if (cutoff_hz > sample_rate * 0.49f) cutoff_hz = sample_rate * 0.49f;

    memset(hpf, 0, sizeof(*hpf));
    const float omega = 2.0f * 3.14159265f * cutoff_hz / sample_rate;
    const float cos_omega = cosf(omega);
    const float sin_omega = sinf(omega);
    const float alpha = sin_omega / (2.0f * VAD_HPF_Q_BUTTER);

    const float a0_inv = 1.0f / (1.0f + alpha);

    /* RBJ Cookbook Highpass */
    hpf->b0 =  (1.0f + cos_omega) * 0.5f * a0_inv;
    hpf->b1 = -(1.0f + cos_omega) * a0_inv;
    hpf->b2 =  (1.0f + cos_omega) * 0.5f * a0_inv;
    hpf->a1 = (-2.0f * cos_omega) * a0_inv;
    hpf->a2 =  (1.0f - alpha) * a0_inv;
}

/* Direct Form I 处理一帧 PCM（16bit LE mono）。热路径零 malloc。*/
void vad_hpf_process(vad_hpf_t *hpf, int16_t *pcm, size_t samples) {
    if (!hpf || !pcm || samples == 0) return;
    /* b0==0 && a1==0 说明没初始化过 → 直通 */
    if (hpf->b0 == 0.0f && hpf->a1 == 0.0f) return;

    float x1 = hpf->x1, x2 = hpf->x2;
    float y1 = hpf->y1, y2 = hpf->y2;

    for (size_t i = 0; i < samples; ++i) {
        const float x = (float)pcm[i];
        /* Direct Form I: y[n] = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2 */
        float y = hpf->b0 * x + hpf->b1 * x1 + hpf->b2 * x2
                - hpf->a1 * y1 - hpf->a2 * y2;

        /* 去 denormal（极小值会导致 CPU 飙升）*/
        if (fabsf(y) < 1.0e-10f) y = 0.0f;

        /* 饱和 */
        if (y >  32767.0f) y =  32767.0f;
        if (y < -32768.0f) y = -32768.0f;

        pcm[i] = (int16_t)lrintf(y);

        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
    }

    hpf->x1 = x1; hpf->x2 = x2;
    hpf->y1 = y1; hpf->y2 = y2;
}

/* ============================================================================
 * ===== per-call per-leg 状态槽（v2 扩展）=====
 *
 * 固定大小数组预分配。256 calls × 32KB ≈ 8MB 静态内存，可接受。
 * 热路径只查 active==1 的槽。
 * ==========================================================================*/
typedef struct {
    /* --- 固定小字段（cache line 友好）--- */
    void        *call_key;
    int          active;

    /* --- 状态（每 leg 独立）--- */
    vad_state_t  state[VAD_LEGS_PER_CALL];
    uint16_t     threshold[VAD_LEGS_PER_CALL];     /* 自适应阈值 */
    uint16_t     last_rms[VAD_LEGS_PER_CALL];      /* 诊断 */

    /* --- 双窗口能量（每 leg 独立）--- */
    double       near_power[VAD_LEGS_PER_CALL];    /* 近窗累加值 */
    double       far_power[VAD_LEGS_PER_CALL];     /* 远窗累加值 */
    double       active_begin_far[VAD_LEGS_PER_CALL]; /* 进入 SPEECH 时的 far_power 快照 */

    /* --- 状态机计数（sample 单位）--- */
    int          active_count[VAD_LEGS_PER_CALL];
    int          silence_count[VAD_LEGS_PER_CALL];
    int          last_silence_trace[VAD_LEGS_PER_CALL];
    int          active_begin_trace[VAD_LEGS_PER_CALL];

    /* --- HPF 状态（每 leg 独立）--- */
    vad_hpf_t    hpf[VAD_LEGS_PER_CALL];

    /* --- WebRTC VAD 二级判据（每 leg 独立实例，NULL = 创建失败 → fail-open）--- */
    VadInst     *wvad[VAD_LEGS_PER_CALL];

    /* --- 非 G.711 解码器（lazy 创建，每 leg 独立；PCM tap 未启用时兜底）--- */
    codec_sess_t      *dec[VAD_LEGS_PER_CALL];
    const codec_ops_t *dec_ops[VAD_LEGS_PER_CALL];

    /* --- 1s Trace ring（每 leg 独立环形缓冲）--- */
    int          trace_wpos[VAD_LEGS_PER_CALL];
    short        trace_data[VAD_LEGS_PER_CALL][VAD_TRACE_SIZE];
} vad_call_state_t;

static vad_call_state_t g_states[VAD_MAX_CALLS];
static pthread_mutex_t g_states_lock;  /* OAM 线程持有，热路径快速查询 */

/* ============================================================================
 * ===== MPSC ring（状态变化异步推送）=====
 *
 * producer（热路径 vad_state_step）：无锁写 tail + atomic store_release
 * consumer（vad_worker 线程）：atomic load_acquire 读 tail
 *
 * 只在状态从 SILENCE→SPEECH 或 SPEECH/HESITATION→SILENCE 时推事件。
 * HESITATION 中间停顿也推事件（让外部知道"说一半歇气了"）。
 * ==========================================================================*/
typedef struct {
    vad_event_t slots[VAD_RING_SIZE];
    uint32_t    head;           /* consumer 读 */
    uint32_t    tail;           /* producer 写 */
    pthread_cond_t not_empty;
    pthread_mutex_t lock;      /* consumer 侧用（热路径 producer 无锁） */
    _Atomic int  shutdown;
} vad_ring_t;

static vad_ring_t g_ring;
static pthread_t g_worker_tid;

/* ============================================================================
 * ===== 工具函数 =====
 * ==========================================================================*/
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* call_key → 槽索引（线性探查，空槽 = NULL）
 * 调用者须已持 g_states_lock。*/
static int find_or_alloc_slot(void *call_key) {
    uintptr_t h = ((uintptr_t)call_key) >> 4;
    for (int i = 0; i < VAD_MAX_CALLS; ++i) {
        int idx = (h + i) % VAD_MAX_CALLS;
        if (!g_states[idx].call_key) {
            memset(&g_states[idx], 0, sizeof(g_states[idx]));
            g_states[idx].call_key = call_key;
            for (int leg = 0; leg < VAD_LEGS_PER_CALL; ++leg) {
                g_states[idx].state[leg] = VAD_STATE_SILENCE;
                g_states[idx].threshold[leg] = VAD_ACTIVE_VOLUME_THRESHOLD;
                g_states[idx].far_power[leg] = 30.0;  /* 背景噪声 EMA 合理初值（PCMA 静音 avg abs ≈ 0-20）*/
                vad_hpf_init(&g_states[idx].hpf[leg], (float)VAD_HPF_CUTOFF_HZ, 8000.0f);
            }
            g_states[idx].active = 1;
            return idx;
        }
        if (g_states[idx].call_key == call_key && g_states[idx].active)
            return idx;
    }
    return -1;
}

static int find_slot(void *call_key) {
    uintptr_t h = ((uintptr_t)call_key) >> 4;
    for (int i = 0; i < VAD_MAX_CALLS; ++i) {
        int idx = (h + i) % VAD_MAX_CALLS;
        if (!g_states[idx].call_key) return -1;
        if (g_states[idx].call_key == call_key && g_states[idx].active)
            return idx;
    }
    return -1;
}

/* ============================================================================
 * ===== 热路径：双窗口能量滑动更新（每 sample 调用）=====
 *
 * NEAR 近窗滑 N 步就减一帧；FAR 远窗滑 M 步就减一帧（背景噪声基线）。
 * 时间 O(1) per sample —— 只加只减。
 * ==========================================================================*/
static inline void power_update_sliding(vad_call_state_t *cs, int leg,
                                        const int16_t *pcm, size_t samples) {
    double *near = &cs->near_power[leg];
    short  *ring = cs->trace_data[leg];
    int    *wpos = &cs->trace_wpos[leg];

    for (size_t i = 0; i < samples; ++i) {
        double abs_s = (double)abs(pcm[i]);

        /* trace ring 写入（环形）；计数器在 1<<30 处回绕（是 TRACE_SIZE 的整数倍），
         * 避免 int 约 3 天溢出为负后 % 出负下标越界 */
        ring[*wpos % VAD_TRACE_SIZE] = pcm[i];
        if (++(*wpos) >= (1 << 30)) *wpos = 0;

        /* NEAR：滑窗更新 —— 加新 sample，减最老的（10ms avg abs = 当前能量）*/
        double drop_near = (double)abs(ring[(*wpos - VAD_NEAR_SAMPLES + VAD_TRACE_SIZE) % VAD_TRACE_SIZE]);
        *near += abs_s - drop_near;
    }
    /* FAR（背景噪声基线）不再在 per-sample 热路径里滑窗。
     * 改成 fsm_step 里基于 near_a 的条件 EMA —— 只在静音时跟随，说话中冻结。
     * 原因：原版 far 和 near 共享 trace ring，说话时 far 也被语音能量拉高
     * （实测 near=145, far=399, above_nf 永远为 0 — 依赖 near>far 的 FSM 条件全废）*/
}

/* 近窗平均（VAD_NEAR_SAMPLES 个 sample 的 avg abs）*/
static inline double near_ave(vad_call_state_t *cs, int leg) {
    return cs->near_power[leg] / (double)VAD_NEAR_SAMPLES;
}
/* 背景噪声基线（EMA avg abs）—— 只在 near 低时更新，说话中冻结 */
static inline double far_ave(vad_call_state_t *cs, int leg) {
    return cs->far_power[leg];  /* 现在 far_power 本身就是 EMA avg 值，不再做滑窗除法 */
}

/* ============================================================================
 * ===== 热路径：RMS 计算（诊断用）=====
 * ==========================================================================*/
static inline uint16_t compute_rms_pcm(const int16_t *pcm, size_t samples) {
    if (!pcm || samples == 0) return 0;
    int32_t sum_sq = 0;
    for (size_t i = 0; i < samples; ++i) {
        int32_t s = (int32_t)pcm[i];
        sum_sq += s * s;
    }
    double mean = (double)sum_sq / (double)samples;
    return (uint16_t)(mean > 0.0 ? sqrt(mean) : 0.0);
}

/* ============================================================================
 * ===== 表驱动三态 FSM —— 状态转换 + 事件投递 =====
 *
 * 设计约束（nsbc-fsm skill）：
 *   - 状态转换表驱动，禁止深层 if-else
 *   - 动作函数纯粹：只做副作用（推 ring、更新计数），不写状态
 *   - 状态变更由 dispatch 统一控制
 *
 * 稳定约束（stability-first P0）：
 *   - 热路径零 malloc、零锁（无锁写 ring.tail，atomic store_release）
 *   - HPF applied post-decode, so DC offset/mains hum does not bias energy estimation
 * ==========================================================================*/

/* 状态机输入条件（从当前能量/计数计算）*/
typedef struct {
    double near_a;         /* 近窗 avg abs */
    double far_a;          /* 远窗 avg abs = 背景噪声基线 */
    int    above_near_far; /* near > far × 倍率 → 能量上升 */
    int    below_near_thr; /* near < threshold → 能量下降 */
    int    sil_got_pause;  /* silence_count >= PAUSE_SILENCE_MIN_SAMPLES → 中间停 */
    int    sil_got_start;  /* silence_count >= START_SILENCE_MIN_SAMPLES → 结束 */
    int    act_got_pause;  /* active_count >= PAUSE_ACTIVE_MIN_SAMPLES → 继续说 */
    int    act_got_start;  /* active_count >= START_ACTIVE_MIN_SAMPLES → 有效说话 */
    double threshold_val;  /* 当前自适应 threshold */
    double custom_thr;     /* 可配置的 active_begin_far 阈值覆盖 */
    int    wvad_speech;    /* WebRTC VAD 投票结果：1=本帧过半 10ms 子帧判为语音 */
} fsm_input_t;

/* 动作函数 —— 只做副作用，不写 cs->state[leg] */
static void act_push_event(vad_call_state_t *cs, int leg, vad_state_t new_state,
                           uint16_t rms, uint16_t thresh, int preroll) {
    vad_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.call_key   = cs->call_key;
    ev.leg        = leg;
    ev.new_state  = new_state;
    ev.rms        = rms;
    ev.threshold  = thresh;
    ev.timestamp_ns = now_ns();

    if (preroll) {
        ev.has_preroll   = 1;
        int pr = cs->trace_wpos[leg] - VAD_PREROLL_SAMPLES;
        if (pr < 0) pr = 0;
        ev.preroll_offset = pr;
    }

    /* 多 reactor 线程会并发投递：tail/slot/signal 必须在同一把锁内完成，
     * 否则两个生产者拿到相同 tail 写同一槽，且 signal 丢唤醒 */
    pthread_mutex_lock(&g_ring.lock);
    {
        uint32_t next = (g_ring.tail + 1) & (VAD_RING_SIZE - 1);
        uint32_t head = __atomic_load_n(&g_ring.head, __ATOMIC_ACQUIRE);
        if (next != head) {
            g_ring.slots[g_ring.tail] = ev;
            __atomic_store_n(&g_ring.tail, next, __ATOMIC_RELEASE);
            pthread_cond_signal(&g_ring.not_empty);
        }
    }
    pthread_mutex_unlock(&g_ring.lock);
    /* ring 满了就丢 —— VAD_EVENT 是诊断/边界信息，丢了主路径不崩 */
}

static void act_enter_speech(vad_call_state_t *cs, int leg, const fsm_input_t *in,
                             uint16_t rms) {
    /* 记录进入 SPEECH 的时刻和 far_power 快照（自适应 threshold 基线）*/
    cs->active_begin_far[leg] = in->far_a > 0.0 ? in->far_a : 30.0;
    cs->active_begin_trace[leg] = cs->trace_wpos[leg];
    cs->active_count[leg] = 0;
    cs->silence_count[leg] = 0;
    cs->threshold[leg] = (uint16_t)in->threshold_val;

    act_push_event(cs, leg, VAD_STATE_SPEECH, rms, cs->threshold[leg], 1 /* preroll */);
}

static void act_enter_hesitation(vad_call_state_t *cs, int leg, uint16_t rms) {
    cs->silence_count[leg] = 0;
    cs->threshold[leg] = VAD_ACTIVE_VOLUME_THRESHOLD / 2;  /* 中间歇气时降低阈值 —— 更快重新进入 SPEECH */
    act_push_event(cs, leg, VAD_STATE_HESITATION, rms, cs->threshold[leg], 0);
}

static void act_enter_silence(vad_call_state_t *cs, int leg, uint16_t rms) {
    cs->last_silence_trace[leg] = cs->trace_wpos[leg];
    cs->active_count[leg] = 0;
    cs->silence_count[leg] = 0;
    cs->threshold[leg] = VAD_ACTIVE_VOLUME_THRESHOLD;  /* 回到默认高阈值 */
    act_push_event(cs, leg, VAD_STATE_SILENCE, rms, cs->threshold[leg], 0);
}

/* FSM 转换表 —— 每一行 = (from_state, condition_check, new_state, action_func)
 * dispatch 按 from_state 扫描，第一个匹配条件命中就执行 action 并推进状态。*/
typedef void (*fsm_action_t)(vad_call_state_t *, int, const fsm_input_t *, uint16_t);

typedef struct {
    vad_state_t   from;
    int           condition;   /* 条件码（见下方 enum），0=恒真 */
    vad_state_t   to;
    fsm_action_t  action;
} fsm_transition_t;

/* 条件码 —— 用整数字段简化表驱动查找 */
enum {
    FSM_COND_ALWAYS = 0,
    /* SILENCE 状态 */
    FSM_COND_SIL_TO_SPEECH,       /* near>far*3 & far>threshold*2 → 正常开始说话 */
    FSM_COND_SIL_TO_SPEECH_BURST, /* near>threshold*3 & far>near*3 → 突发开始 */
    /* SPEECH 状态 */
    FSM_COND_SPEECH_TO_SIL,       /* near<threshold & sil>=START_SILENCE → 说完了 */
    FSM_COND_SPEECH_TO_HESIT,     /* near<threshold & sil>=PAUSE_SILENCE → 中间歇气 */
    /* HESITATION 状态 */
    FSM_COND_HESIT_TO_SPEECH,     /* near>far*3 & act>=PAUSE_ACTIVE → 继续说 */
    FSM_COND_HESIT_TO_SIL,        /* sil>=START_SILENCE → 歇太长=说完了 */
};

/* SILENCE → SPEECH（简化版）：
 *   只要 near_a（最近 10ms 能量）超过 threshold * 0.6，且 active_count >= MIN_SAMPLES，就触发。
 *   原版条件（far_a > 背景 2-3 倍）在低噪声环境下永远不成立 —— 这里彻底简化。*/
static int cond_sil_to_speech(vad_call_state_t *cs, int leg, const fsm_input_t *in) {
    /* 核心条件：能量显著上升 near > threshold*0.6，且持续 >= START_ACTIVE_MIN_SAMPLES；
     * WebRTC VAD 必须确认是语音（敲击/敲桌子的宽带瞬态会被判非语音 → 不进入 SPEECH）*/
    if (in->wvad_speech
        && in->near_a > (double)cs->threshold[leg] * 0.6
        && cs->active_count[leg] >= VAD_START_ACTIVE_MIN_SAMPLES)
        return 1;
    return 0;
}

/* SILENCE → SPEECH（突发）：near 突然跳到大值 —— 保持原有逻辑（但用简化 threshold 比较）。
 * 注意：持续时间要求与普通入口一致（不能用 /2，否则敲击 80ms 就能绕过 160ms 门槛）。*/
static int cond_sil_to_speech_burst(vad_call_state_t *cs, int leg, const fsm_input_t *in) {
    /* near 超过 threshold * 1.5 且 active_count 够长；同样要求 WebRTC VAD 确认 */
    if (in->wvad_speech
        && cs->active_count[leg] >= VAD_START_ACTIVE_MIN_SAMPLES
        && in->near_a > (double)cs->threshold[leg] * 1.5)
        return 1;
    return 0;
}

/* SPEECH → SILENCE：near < threshold 且 silence_count >= 150ms */
static int cond_speech_to_sil(vad_call_state_t *cs, int leg, const fsm_input_t *in) {
    (void)cs; (void)leg;
    return (in->below_near_thr && in->sil_got_start);
}

/* SPEECH → HESITATION：near < threshold 且 silence_count >= 40ms */
static int cond_speech_to_hesit(vad_call_state_t *cs, int leg, const fsm_input_t *in) {
    (void)cs; (void)leg;
    return (in->below_near_thr && in->sil_got_pause && !in->sil_got_start);
}

/* HESITATION → SPEECH：near 又上来了。
 * 必须同时满足：WebRTC VAD 确认 + 近端能量恢复到 threshold*0.8 以上。
 * 只判 above_near_far 会被敲击尾巴（rms 16~35）拉回 SPEECH，继续放行给 ai_agent。*/
static int cond_hesit_to_speech(vad_call_state_t *cs, int leg, const fsm_input_t *in) {
    return (in->wvad_speech && in->above_near_far > 0 && in->act_got_pause
            && in->near_a > (double)cs->threshold[leg] * 0.8);
}

/* HESITATION → SILENCE：歇气太久（>=150ms）*/
static int cond_hesit_to_sil(vad_call_state_t *cs, int leg, const fsm_input_t *in) {
    (void)cs; (void)leg;
    return (in->sil_got_start);
}

/* 动作函数：dispatch 包装 */
static void act_enter_speech_wrap(vad_call_state_t *cs, int leg, const fsm_input_t *in, uint16_t rms) {
    act_enter_speech(cs, leg, in, rms);
}
static void act_enter_hesit_wrap(vad_call_state_t *cs, int leg, const fsm_input_t *in, uint16_t rms) {
    (void)in;
    act_enter_hesitation(cs, leg, rms);
}
static void act_enter_sil_wrap(vad_call_state_t *cs, int leg, const fsm_input_t *in, uint16_t rms) {
    (void)in;
    act_enter_silence(cs, leg, rms);
}

/* 转换表 —— 按 from_state 分组；每条 transition 先检查前一条条件，
 * 未命中就继续往下扫。last 行恒真 catch-all（SPEECH/SILENCE 保持不变）。*/
static const fsm_transition_t FSM_TABLE[] = {
    /* ===== SILENCE 状态（0）===== */
    { VAD_STATE_SILENCE,    FSM_COND_SIL_TO_SPEECH,         VAD_STATE_SPEECH,     act_enter_speech_wrap   },
    { VAD_STATE_SILENCE,    FSM_COND_SIL_TO_SPEECH_BURST,   VAD_STATE_SPEECH,     act_enter_speech_wrap   },
    /* catch-all：保持 SILENCE */
    { VAD_STATE_SILENCE,    FSM_COND_ALWAYS,                VAD_STATE_SILENCE,    NULL                    },

    /* ===== SPEECH 状态（1）===== */
    { VAD_STATE_SPEECH,     FSM_COND_SPEECH_TO_SIL,         VAD_STATE_SILENCE,    act_enter_sil_wrap      },
    { VAD_STATE_SPEECH,     FSM_COND_SPEECH_TO_HESIT,       VAD_STATE_HESITATION, act_enter_hesit_wrap    },
    /* catch-all：保持 SPEECH —— 更新自适应 threshold */
    { VAD_STATE_SPEECH,     FSM_COND_ALWAYS,                VAD_STATE_SPEECH,     NULL                    },

    /* ===== HESITATION 状态（2）===== */
    { VAD_STATE_HESITATION, FSM_COND_HESIT_TO_SIL,          VAD_STATE_SILENCE,    act_enter_sil_wrap      },
    { VAD_STATE_HESITATION, FSM_COND_HESIT_TO_SPEECH,       VAD_STATE_SPEECH,     act_enter_speech_wrap   },
    /* catch-all：保持 HESITATION */
    { VAD_STATE_HESITATION, FSM_COND_ALWAYS,                VAD_STATE_HESITATION, NULL                    },
};

static const int FSM_TABLE_SIZE = (int)(sizeof(FSM_TABLE) / sizeof(FSM_TABLE[0]));

/* dispatch：从 from_state 开始扫表，第一个匹配条件的 transition 执行 */
static inline void fsm_step(vad_call_state_t *cs, int leg, uint16_t rms,
                            double near_a, double far_a, double custom_thr,
                            int frame_samples, int wvad_speech) {
    const vad_state_t from = cs->state[leg];

    /* === 背景噪声 EMA 更新（far_power 重定义：真正的背景基线）===
     * 原版 far 和 near 共享 trace ring 滑窗 — 说话时 far 也被语音拉高（far=399 而 near=145），
     * 导致 above_nf(near>far) 永远为 0，依赖该关系的 FSM 条件全废。
     * 现在 far 只在"确认静音"时跟随 near 更新；说话中完全冻结 —— 保证 far 始终反映背景噪声。 */
    {
        double *far_ema = &cs->far_power[leg];
        if (near_a < (double)VAD_SILENCE_VOLUME_THRESHOLD) {
            /* 真静音（near < 40）— 快速跟随背景噪声变化（α=0.05，时间常数约 200ms）*/
            *far_ema = *far_ema * 0.95 + near_a * 0.05;
        } else if (near_a < (double)cs->threshold[leg] * 0.3) {
            /* 低能量过渡区（40 ≤ near < threshold*0.3）— 极慢跟随，不被短暂/突发噪声污染 */
            *far_ema = *far_ema * 0.995 + near_a * 0.005;
        }
        /* near_a >= threshold*0.3：说话中 — far 完全冻结，绝不被语音能量污染 */
    }

    /* --- 动态计算条件 --- */
    fsm_input_t in;
    memset(&in, 0, sizeof(in));
    in.near_a = near_a;
    in.far_a  = far_a;
    in.above_near_far = (near_a > far_a) ? 1 : 0;
    in.below_near_thr = (near_a < (double)cs->threshold[leg]) ? 1 : 0;
    in.sil_got_pause  = (cs->silence_count[leg] >= VAD_PAUSE_SILENCE_MIN_SAMPLES) ? 1 : 0;
    in.sil_got_start  = (cs->silence_count[leg] >= VAD_START_SILENCE_MIN_SAMPLES) ? 1 : 0;
    in.act_got_pause  = (cs->active_count[leg] >= VAD_PAUSE_ACTIVE_MIN_SAMPLES) ? 1 : 0;
    in.act_got_start  = (cs->active_count[leg] >= VAD_START_ACTIVE_MIN_SAMPLES) ? 1 : 0;
    /* 自适应 threshold —— 取背景噪声的 3 倍 vs 600 上限 vs active_begin_far 的 2 倍 */
    in.threshold_val = (double)cs->threshold[leg];  /* 先用旧值，下面更新 */
    in.custom_thr    = custom_thr;
    in.wvad_speech   = wvad_speech;

    /* 自适应 threshold 动态更新（只在 SPEECH 状态更新）*/
    if (from == VAD_STATE_SPEECH) {
        double noise_based = in.far_a * 3.0;
        double active_based = cs->active_begin_far[leg] * 2.0;
        double auto_thr = noise_based;
        if (auto_thr < active_based) auto_thr = active_based;
        if (auto_thr < 60.0) auto_thr = 60.0;
        if (auto_thr > (double)VAD_ACTIVE_VOLUME_THRESHOLD) auto_thr = VAD_ACTIVE_VOLUME_THRESHOLD;
        in.threshold_val = auto_thr;
        cs->threshold[leg] = (uint16_t)auto_thr;
    }

    /* --- 计数更新（sample 单位 —— 用本帧实际 samples 数）---
     * 关键：active_count 用衰减代替清零。near 短暂低于 far 时不清零 active_count ——
     * 只有当 near 持续低于 threshold * 0.4 超过 300ms 才清零。
     * 原版 near<far 就清零 → active_count 永远累不到 640（near/far 微小波动）*/
    if (in.near_a > (double)cs->threshold[leg] * 0.4) {
        cs->active_count[leg] += frame_samples;
        cs->silence_count[leg] = 0;
    } else {
        /* 衰减：缓慢减少 active_count（不是立刻清零），silence_count 累加 */
        cs->active_count[leg] = (cs->active_count[leg] > frame_samples / 2)
                                ? cs->active_count[leg] - frame_samples / 2 : 0;
        cs->silence_count[leg] += frame_samples;
    }

    /* --- 扫表 dispatch --- */
    for (int i = 0; i < FSM_TABLE_SIZE; ++i) {
        if (FSM_TABLE[i].from != from) continue;  /* 只扫 from_state 匹配的行 */

        const fsm_transition_t *tr = &FSM_TABLE[i];
        int match = 0;

        switch (tr->condition) {
            case FSM_COND_ALWAYS:                match = 1; break;
            case FSM_COND_SIL_TO_SPEECH:         match = cond_sil_to_speech(cs, leg, &in); break;
            case FSM_COND_SIL_TO_SPEECH_BURST:   match = cond_sil_to_speech_burst(cs, leg, &in); break;
            case FSM_COND_SPEECH_TO_SIL:         match = cond_speech_to_sil(cs, leg, &in); break;
            case FSM_COND_SPEECH_TO_HESIT:       match = cond_speech_to_hesit(cs, leg, &in); break;
            case FSM_COND_HESIT_TO_SPEECH:       match = cond_hesit_to_speech(cs, leg, &in); break;
            case FSM_COND_HESIT_TO_SIL:          match = cond_hesit_to_sil(cs, leg, &in); break;
        }

        if (match) {
            /* 状态变更（只有 from != to 才算真转换；catch-all 行 to==from）*/
            if (from != tr->to) {
                cs->state[leg] = tr->to;
                if (tr->action) tr->action(cs, leg, &in, rms);
            } else if (tr->action) {
                /* catch-all 也可能有动作（但目前都是 NULL）*/
                tr->action(cs, leg, &in, rms);
            }
            return;  /* 第一个匹配就 dispatch */
        }
    }
}

/* ============================================================================
 * ===== 热路径回调（media_bug global consumer）=====
 *
 * 每帧调用一次，典型帧长 20ms = 160 bytes G.711 payload → 160 samples PCM。
 * 流程：
 *   rtp_parse → pt=0/8 → g711_decode → HPF → 双窗口能量更新 → FSM step
 * 锁策略：
 *   - g_states_lock 保护 find_slot + cs 读写（持锁时间 <<10ns，只有简单赋值）
 *   - ring.producer 无锁（atomic store_release 写 tail）
 *   - worker consumer 用 condvar（不在此路径）
 * ==========================================================================*/
void vad_on_rtp_frame(media_bug_frame_t *frame, void *ud) {
    (void)ud;
    static __thread int vd_ent = 0;
    if (vd_ent++ < 3)
        llog(LOG_NOTICE, "vad", "vad_on_rtp_frame ENTRY call=%p leg=%d len=%zu",
             frame ? frame->call_key : NULL, frame ? frame->leg : -1,
             frame ? frame->data_len : 0);
    if (!frame || !frame->call_key) return;
    if (frame->leg != 0 && frame->leg != 1) return;

    rtp_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.pt = -1;

    /* 2) 取 PCM：
     *    a) PCM tap（BUG_PCM_READ）已由框架解码 → 直接用（首选路径，
     *       不依赖 PT，G.729/G.722/Opus 等都能覆盖）；
     *    b) 兼容旧路径：原始 RTP → G.711 查表 / 其他静态 PT lazy 解码；
     *       动态 PT（96-127）没有名字无法可靠解码，必须走 PCM tap。 */
    int16_t pcm_stack[320];  /* 最多 320 字节 payload = 320 samples = 640B */
    int16_t *pcm = pcm_stack;
    size_t pcm_cap = sizeof(pcm_stack) / sizeof(pcm_stack[0]);
    size_t samples = 0;
    int pcm_rate = 8000;
    int pcm_ch = 1;

    if (frame->pcm != NULL && frame->samples > 0) {
        pcm = (int16_t *)(uintptr_t)frame->pcm;
        samples = frame->samples;
        pcm_rate = frame->rate > 0 ? frame->rate : 8000;
        pcm_ch = frame->channels > 0 ? frame->channels : 1;
        goto pcm_ready;
    }

    if (frame->data == NULL || frame->data_len < 12) return;

    /* 1) RTP parse（旧路径兜底） */
    nsbc_rc_t prc = rtp_parse(frame->data, frame->data_len, &pkt);
    if (prc != NSBC_OK) {
        static __thread int vd_pf = 0;
        if (vd_pf++ < 3)
            llog(LOG_WARN, "vad", "vad_on_rtp_frame rtp_parse FAIL rc=%d call=%p",
                 (int)prc, frame->call_key);
        return;
    }
    if (pkt.pt != 0 && pkt.pt != 8) {
        static __thread int vd_pt2 = 0;
        if (vd_pt2++ < 3)
            llog(LOG_NOTICE, "vad",
                 "pt=%d 非 G.711，尝试 codec 解码 call=%p",
                 (int)pkt.pt, frame->call_key);
    }

    if (pkt.pt == 0 || pkt.pt == 8) {
        if (pkt.payload_len > pcm_cap) {
            static __thread int16_t pcm_large[8192];
            pcm = pcm_large;
            pcm_cap = sizeof(pcm_large) / sizeof(pcm_large[0]);
            if (pkt.payload_len > pcm_cap) return;
        }
        samples = (pkt.pt == 0)
                ? g711_ulaw_decode(pkt.payload, pkt.payload_len, pcm)
                : g711_alaw_decode(pkt.payload, pkt.payload_len, pcm);
    } else {
        const codec_ops_t *ops = codec_find_by_pt_kind(pkt.pt, CODEC_KIND_AUDIO);
        static __thread int16_t pcm_dec[4096];
        if (ops == NULL || ops->decode == NULL || ops->init == NULL) {
            static __thread int vd_pt = 0;
            if (vd_pt++ < 3)
                llog(LOG_WARN, "vad",
                     "pt=%d 无可用解码器（动态 PT 请启用 PCM tap）call=%p",
                     (int)pkt.pt, frame->call_key);
            return;
        }
        pthread_mutex_lock(&g_states_lock);
        {
            int di = find_slot(frame->call_key);
            if (di >= 0) {
                vad_call_state_t *dcs = &g_states[di];
                if (dcs->dec_ops[frame->leg] != ops) {
                    if (dcs->dec[frame->leg] && dcs->dec_ops[frame->leg] &&
                        dcs->dec_ops[frame->leg]->fini)
                        dcs->dec_ops[frame->leg]->fini(dcs->dec[frame->leg]);
                    dcs->dec[frame->leg] = NULL;
                    dcs->dec_ops[frame->leg] = ops;
                    ops->init(NULL, &dcs->dec[frame->leg]);
                }
                if (dcs->dec[frame->leg] != NULL) {
                    media_frame_t mf;
                    memset(&mf, 0, sizeof(mf));
                    mf.pcm = pcm_dec;
                    mf.samples = 0;
                    mf.rate = codec_pcm_sample_rate(ops);
                    mf.channels = 1;
                    if (ops->decode(dcs->dec[frame->leg], pkt.payload,
                                    pkt.payload_len, &mf) == NSBC_OK) {
                        samples = mf.samples;
                        pcm = pcm_dec;
                        pcm_rate = mf.rate > 0 ? mf.rate : 8000;
                        pcm_ch = 1;
                    }
                }
            }
        }
        pthread_mutex_unlock(&g_states_lock);
        if (samples > sizeof(pcm_dec) / sizeof(pcm_dec[0])) samples = 0;
    }
    if (samples == 0) return;

pcm_ready:
    /* 2.2) 统一到 8k 单声道（VAD 内部固定 8k / 10ms 子帧）：
     *      多声道先混单，再按整数倍抽取；只做能量/包络分析，无需高质量重采样。 */
    if (pcm_ch > 1 || pcm_rate != 8000) {
        static __thread int16_t pcm8[8192];
        size_t cap8 = sizeof(pcm8) / sizeof(pcm8[0]);
        size_t i, o = 0;
        size_t ch = pcm_ch > 0 ? (size_t)pcm_ch : 1;
        for (i = 0; i < samples && o < cap8; ++i) {
            int32_t acc = 0;
            size_t c;
            for (c = 0; c < ch; ++c)
                acc += pcm[i * ch + c];
            pcm8[o++] = (int16_t)(acc / (int32_t)ch);
        }
        if (pcm_rate > 8000) {
            size_t step = (size_t)(pcm_rate / 8000);
            size_t n8 = 0;
            if (step < 1) step = 1;
            for (i = 0; i < o && n8 < cap8; i += step)
                pcm8[n8++] = pcm8[i];
            o = n8;
        }
        pcm = pcm8;
        samples = o;
        if (samples == 0) return;
    }

    /* 2.5) 诊断：每 50 帧打一次 pt/payload_len/samples/rms_before_hpf */
    static __thread int dec_cnt = 0;
    if ((dec_cnt++ % 50) == 0) {
        uint16_t rms_raw = compute_rms_pcm(pcm, samples);
        int16_t first = pcm[0], mid = pcm[samples/2], last = pcm[samples-1];
        llog(LOG_INFO, "vad",
             "DECODE call=%p leg=%d pt=%d payload_len=%zu samples=%zu "
             "rms_raw=%u first=%d mid=%d last=%d",
             frame->call_key, frame->leg, (int)pkt.pt,
             frame->data_len, samples, (unsigned)rms_raw,
             (int)first, (int)mid, (int)last);
    }

    /* 3) HPF strip DC + mains — zero-malloc hot path, per-leg state from cs->hpf[leg] */
    /* 锁内取 hp 指针，但 HPF 本身是无状态处理（只读写 hpf->x1/y1 等）
     * 这里 hpf 的 x1/y1/x2/y2 读写和 cs 其他字段一起被 g_states_lock 保护 */

    /* 4) RMS（诊断）—— 在 HPF 之后计算，干净的 PCM */
    uint16_t rms = compute_rms_pcm(pcm, samples);

    /* 5) 持锁：查槽 → HPF → 能量 → FSM */
    pthread_mutex_lock(&g_states_lock);
    int idx = find_slot(frame->call_key);
    if (idx < 0) {
        static __thread int vd_nf = 0;
        if (vd_nf++ < 3)
            llog(LOG_WARN, "vad", "vad_on_rtp_frame find_slot MISS call=%p (vad_start 未命中?)",
                 frame->call_key);
    }
    if (idx >= 0) {
        vad_call_state_t *cs = &g_states[idx];
        int leg = frame->leg;

        /* 5a) HPF 处理 */
        vad_hpf_process(&cs->hpf[leg], pcm, samples);

        /* 5a.5) WebRTC VAD 二级判据：按 10ms 子帧投票（过半才算语音）。
         *       没有实例（创建失败）→ fail-open，退化为纯能量 VAD。
         *       作用：敲桌子/敲门是宽带瞬态，能量判据会误判，频谱 GMM 判据能压掉。*/
        int wvad_speech = 1;
        if (cs->wvad[leg]) {
            int votes = 0, total = 0;
            for (size_t off = 0; off + VAD_WEBRTC_FRAME <= samples; off += VAD_WEBRTC_FRAME) {
                if (WebRtcVad_Process(cs->wvad[leg], VAD_WEBRTC_RATE,
                                      pcm + off, VAD_WEBRTC_FRAME) > 0) {
                    votes++;
                }
                total++;
            }
            wvad_speech = (total == 0) ? 1 : (votes * 2 >= total);
        }

        /* 5b) 双窗口能量更新（滑窗 + trace ring）*/
        power_update_sliding(cs, leg, pcm, samples);

        /* 5c) 取 avg */
        double near_a = near_ave(cs, leg);
        double far_a  = far_ave(cs, leg);

        /* 5d) FSM step —— 可能推 ring（状态变化时）
         * samples = 本帧实际 PCM 样本数（G.711 默认 160，异常时可能不同）*/
        fsm_step(cs, leg, rms, near_a, far_a, (double)cs->threshold[leg], (int)samples,
                 wvad_speech);

        /* 5e) 每 50 帧打一次诊断（1s @20ms/frame）—— 观察 near/far/active_count 关系 */
        static __thread int diag_cnt = 0;
        if ((diag_cnt++ % 50) == 0) {
            llog(LOG_INFO, "vad",
                 "DIAG call=%p leg=%d state=%d rms=%u thr=%u "
                 "near=%.1f far=%.1f above_nf=%d wvad=%d "
                 "act_cnt=%d sil_cnt=%d act_begin_far=%.1f",
                 frame->call_key, leg, cs->state[leg],
                 (unsigned)rms, (unsigned)cs->threshold[leg],
                 near_a, far_a, (near_a > far_a) ? 1 : 0, wvad_speech,
                 cs->active_count[leg], cs->silence_count[leg],
                 cs->active_begin_far[leg]);
        }

        /* 5f) 更新诊断 RMS */
        cs->last_rms[leg] = rms;
    }
    pthread_mutex_unlock(&g_states_lock);
}

/* ============================================================================
 * ===== Worker 线程：异步推 data.sock VAD_EVENT =====
 *
 * 复用 uds_send_vad（通用 JSON 推送，data.sock Event Bus 协议）。
 * JSON schema:
 *   {"event":"VAD_EVENT","call_key":"0x%p","leg":0,
 *    "state":"SPEECH|SILENCE|HESITATION",
 *    "rms":N,"threshold":N,"ts_ns":N,"has_preroll":1}
 * ==========================================================================*/
static void *vad_worker(void *arg) {
    (void)arg;
    llog(LOG_NOTICE, "vad", "worker started (v2: adaptive VAD + HPF + trace ring)");


    while (!atomic_load(&g_ring.shutdown)) {
        uint32_t tail;
        /* 谓词与 wait 使用同一把锁，避免生产者 signal 丢失导致事件滞留 */
        pthread_mutex_lock(&g_ring.lock);
        while (!atomic_load(&g_ring.shutdown) &&
               __atomic_load_n(&g_ring.tail, __ATOMIC_ACQUIRE) == g_ring.head) {
            pthread_cond_wait(&g_ring.not_empty, &g_ring.lock);
        }
        pthread_mutex_unlock(&g_ring.lock);
        if (atomic_load(&g_ring.shutdown)) break;

        tail = __atomic_load_n(&g_ring.tail, __ATOMIC_ACQUIRE);
        if (tail == g_ring.head) continue;   /* 虚假唤醒 */

        /* 取一个事件 */
        vad_event_t ev = g_ring.slots[g_ring.head];
        __atomic_store_n(&g_ring.head, (g_ring.head + 1) & (VAD_RING_SIZE - 1), __ATOMIC_RELEASE);

        const char *state_str;
        switch (ev.new_state) {
            case VAD_STATE_SPEECH:     state_str = "SPEECH"; break;
            case VAD_STATE_HESITATION: state_str = "HESITATION"; break;
            default:                   state_str = "SILENCE"; break;
        }

        char json[384];
        int n = snprintf(json, sizeof(json),
            "{\"event\":\"VAD_EVENT\",\"call_key\":\"%p\",\"leg\":%d,\"state\":\"%s\","
            "\"rms\":%u,\"threshold\":%u,\"ts_ns\":%lu,"
            "\"has_preroll\":%d,\"preroll_offset\":%d}",
            ev.call_key, ev.leg, state_str,
            ev.rms, ev.threshold, (unsigned long)ev.timestamp_ns,
            ev.has_preroll, ev.preroll_offset);

        if (n > 0 && n < (int)sizeof(json)) {
            uds_send_vad(json);
        }

        /* audit log（只打状态变化，HESITATION 中间停顿不打 —— 太吵）*/
        if (ev.new_state != VAD_STATE_HESITATION) {
            llog(LOG_INFO, "vad", "VAD_EVENT call=%p leg=%d state=%s rms=%u thr=%u preroll=%d",
                 ev.call_key, ev.leg, state_str, ev.rms, ev.threshold, ev.has_preroll);
        }
    }

    llog(LOG_NOTICE, "vad", "worker stopped");
    return NULL;
}

/* ============================================================================
 * ===== 生命周期 =====
 * ==========================================================================*/

static const media_bug_consumer_t kVadConsumer = {
    .name      = "vad_engine",
    /* P1-5/P2-9：改用框架解码后的 PCM tap，彻底摆脱 PT 0/8 限制 */
    .type_mask = BUG_PCM_READ,
    .cb        = vad_on_rtp_frame,
    .ud        = NULL,
};

int vad_engine_init(void) {
    int rc = media_bug_register_global_consumer(&kVadConsumer);
    if (rc != 0) {
        llog(LOG_ERR, "vad", "media_bug_register_global_consumer rc=%d", rc);
        return rc;
    }

    pthread_mutex_init(&g_states_lock, NULL);
    memset(g_states, 0, sizeof(g_states));

    memset(&g_ring, 0, sizeof(g_ring));
    pthread_mutex_init(&g_ring.lock, NULL);
    pthread_cond_init(&g_ring.not_empty, NULL);

    pthread_create(&g_worker_tid, NULL, vad_worker, NULL);

    llog(LOG_NOTICE, "vad", "vad_engine v2 initialized (adaptive VAD + HPF + trace ring + table-driven FSM)");
    return 0;
}

void vad_engine_shutdown(void) {
    /* shutdown 标志与 signal 同锁，防止消费者刚判断完谓词就永久 sleep */
    pthread_mutex_lock(&g_ring.lock);
    atomic_store(&g_ring.shutdown, 1);
    pthread_cond_broadcast(&g_ring.not_empty);
    pthread_mutex_unlock(&g_ring.lock);
    pthread_join(g_worker_tid, NULL);

    pthread_mutex_destroy(&g_ring.lock);
    pthread_cond_destroy(&g_ring.not_empty);

    media_bug_unregister_global_consumer(&kVadConsumer);

    pthread_mutex_lock(&g_states_lock);
    for (int i = 0; i < VAD_MAX_CALLS; ++i) {
        for (int leg = 0; leg < VAD_LEGS_PER_CALL; ++leg) {
            if (g_states[i].wvad[leg]) {
                WebRtcVad_Free(g_states[i].wvad[leg]);
                g_states[i].wvad[leg] = NULL;
            }
        }
    }
    memset(g_states, 0, sizeof(g_states));
    pthread_mutex_unlock(&g_states_lock);
    pthread_mutex_destroy(&g_states_lock);

    llog(LOG_NOTICE, "vad", "vad_engine v2 shutdown complete");
}

/* ============================================================================
 * ===== OAM 控制 =====
 * ==========================================================================*/
int vad_start_for_call(void *call_key, int leg_mask, uint16_t threshold) {
    if (!call_key) return -1;
    pthread_mutex_lock(&g_states_lock);
    int idx = find_or_alloc_slot(call_key);
    if (idx < 0) {
        pthread_mutex_unlock(&g_states_lock);
        llog(LOG_WARN, "vad", "VAD state table full (%d slots)", VAD_MAX_CALLS);
        return -1;
    }

    /* leg_mask 控制哪些 leg 启用 VAD（0=A, 1=B）—— v2 简化：两 leg 都启 */
    for (int leg = 0; leg < VAD_LEGS_PER_CALL; ++leg) {
        if (!(leg_mask & (1 << leg))) continue;
        g_states[idx].state[leg] = VAD_STATE_SILENCE;
        g_states[idx].threshold[leg] = threshold > 0 ? threshold : VAD_ACTIVE_VOLUME_THRESHOLD;
        g_states[idx].last_rms[leg] = 0;
        g_states[idx].near_power[leg] = 0;
        g_states[idx].far_power[leg]  = 30.0;  /* 背景噪声 EMA 合理初值（PCMA 静音 avg abs ≈ 0-20）*/
        g_states[idx].active_begin_far[leg] = 0;
        g_states[idx].active_count[leg] = 0;
        g_states[idx].silence_count[leg] = 0;
        g_states[idx].last_silence_trace[leg] = 0;
        g_states[idx].active_begin_trace[leg] = 0;
        g_states[idx].trace_wpos[leg] = 0;
        memset(g_states[idx].trace_data[leg], 0, sizeof(g_states[idx].trace_data[leg]));
        vad_hpf_init(&g_states[idx].hpf[leg], (float)VAD_HPF_CUTOFF_HZ, 8000.0f);

        /* WebRTC VAD 二级判据实例（每 leg 独立）。失败只降级为纯能量 VAD，不影响业务。 */
        if (g_states[idx].wvad[leg]) {
            WebRtcVad_Free(g_states[idx].wvad[leg]);
            g_states[idx].wvad[leg] = NULL;
        }
        VadInst *vi = WebRtcVad_Create();
        if (vi && WebRtcVad_Init(vi) == 0 && WebRtcVad_set_mode(vi, VAD_WEBRTC_MODE) == 0) {
            g_states[idx].wvad[leg] = vi;
        } else {
            if (vi) WebRtcVad_Free(vi);
            llog(LOG_WARN, "vad", "WebRTC VAD create/init failed leg=%d → 退化为纯能量 VAD", leg);
        }
    }
    g_states[idx].active = 1;

    llog(LOG_NOTICE, "vad", "vad_start call=%p leg_mask=%d threshold=%u slot=%d wvad=%s(mode=%d)",
         call_key, leg_mask, g_states[idx].threshold[0], idx,
         (g_states[idx].wvad[0] || g_states[idx].wvad[1]) ? "on" : "off",
         VAD_WEBRTC_MODE);
    pthread_mutex_unlock(&g_states_lock);
    return 0;
}

int vad_stop_for_call(void *call_key) {
    if (!call_key) return -1;
    pthread_mutex_lock(&g_states_lock);
    int idx = find_slot(call_key);
    if (idx >= 0) {
        for (int leg = 0; leg < VAD_LEGS_PER_CALL; ++leg) {
            if (g_states[idx].wvad[leg]) {
                WebRtcVad_Free(g_states[idx].wvad[leg]);
                g_states[idx].wvad[leg] = NULL;
            }
            if (g_states[idx].dec[leg]) {
                if (g_states[idx].dec_ops[leg] && g_states[idx].dec_ops[leg]->fini)
                    g_states[idx].dec_ops[leg]->fini(g_states[idx].dec[leg]);
                g_states[idx].dec[leg] = NULL;
                g_states[idx].dec_ops[leg] = NULL;
            }
        }
        g_states[idx].active = 0;
        g_states[idx].call_key = NULL;
    }
    pthread_mutex_unlock(&g_states_lock);
    return idx >= 0 ? 0 : 0;
}

int vad_query_for_call(void *call_key, int leg, vad_state_t *state_out, uint16_t *rms_out) {
    if (!call_key || !state_out || leg < 0 || leg >= VAD_LEGS_PER_CALL) return -1;
    pthread_mutex_lock(&g_states_lock);
    int idx = find_slot(call_key);
    if (idx < 0 || !g_states[idx].active) { pthread_mutex_unlock(&g_states_lock); return -1; }
    *state_out = g_states[idx].state[leg];
    if (rms_out) *rms_out = g_states[idx].last_rms[leg];
    pthread_mutex_unlock(&g_states_lock);
    return 0;
}

void vad_stop_all(void) {
    pthread_mutex_lock(&g_states_lock);
    for (int i = 0; i < VAD_MAX_CALLS; ++i) {
        if (g_states[i].active) {
            for (int leg = 0; leg < VAD_LEGS_PER_CALL; ++leg) {
                if (g_states[i].wvad[leg]) {
                    WebRtcVad_Free(g_states[i].wvad[leg]);
                    g_states[i].wvad[leg] = NULL;
                }
                if (g_states[i].dec[leg]) {
                    if (g_states[i].dec_ops[leg] && g_states[i].dec_ops[leg]->fini)
                        g_states[i].dec_ops[leg]->fini(g_states[i].dec[leg]);
                    g_states[i].dec[leg] = NULL;
                    g_states[i].dec_ops[leg] = NULL;
                }
            }
            g_states[i].active = 0;
            g_states[i].call_key = NULL;
        }
    }
    pthread_mutex_unlock(&g_states_lock);
}

/* v2 新增：取 pre-roll 数据（从 trace ring 向前回溯 200ms）
 * out_ring_ptr → trace_data[leg] 起始指针（环形）
 * out_start_idx → pre-roll 起始索引（0..VAD_TRACE_SIZE-1）
 * out_samples → pre-roll 样本数（最多 1600 = 200ms @8kHz）
 *
 * 外部使用：先读 out_ring_ptr[out_start_idx..TRACE_SIZE-1]，
 *   若不够再读 out_ring_ptr[0..剩余数-1]（环形回绕）。
 *   也可以直接：
 *     memcpy(out_buf, out_ring_ptr + out_start_idx, remaining)
 *     memcpy(out_buf + remaining, out_ring_ptr, pr - remaining)
 */
int vad_get_preroll(void *call_key, int leg,
                    short **out_ring_ptr, int *out_start_idx, int *out_samples) {
    if (!call_key || !out_ring_ptr || !out_start_idx || !out_samples
        || leg < 0 || leg >= VAD_LEGS_PER_CALL)
        return -1;

    pthread_mutex_lock(&g_states_lock);
    int idx = find_slot(call_key);
    if (idx < 0 || !g_states[idx].active) { pthread_mutex_unlock(&g_states_lock); return -1; }

    short *ring = g_states[idx].trace_data[leg];
    int wpos    = g_states[idx].trace_wpos[leg];

    /* 最多回溯 VAD_PREROLL_SAMPLES = 1600 samples = 200ms（覆盖 160ms 起判延迟）*/
    int pr = VAD_PREROLL_SAMPLES;
    if (pr > wpos) pr = wpos;

    /* 环形回绕：从 wpos 向前数 pr 个 sample 的起点 */
    int start = wpos - pr;
    while (start < 0) start += VAD_TRACE_SIZE;
    start %= VAD_TRACE_SIZE;

    *out_ring_ptr   = ring;
    *out_start_idx  = start;
    *out_samples    = pr;

    pthread_mutex_unlock(&g_states_lock);
    return 0;
}

/* OAM 状态输出 */
int vad_status(char *out, size_t outsz) {
    int active = 0;
    pthread_mutex_lock(&g_states_lock);
    for (int i = 0; i < VAD_MAX_CALLS; ++i)
        if (g_states[i].active) ++active;
    pthread_mutex_unlock(&g_states_lock);

    return snprintf(out, outsz,
        "vad_engine v2: active_calls=%d ring_head=%u ring_tail=%u\n",
        active, g_ring.head, g_ring.tail);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
