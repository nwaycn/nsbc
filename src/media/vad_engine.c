/* vad_engine.c — AI barge-in VAD 引擎实现
 *
 * 热路径约束：vad_on_rtp_frame 是 media_bug global consumer 回调，
 * 必须无锁、无阻塞、无 I/O、无 malloc。
 *
 * 状态表：固定大小数组 + call_key 线性探查哈希。
 * 每帧工作：查表 → rtp_parse → pt=0/8 → g711_decode → RMS → 状态机 → 变化时 ring_push。
 */

#include "vad_engine.h"
#include "media_bug.h"
#include "rtp.h"
#include "nsbc_common.h"    /* nsbc_rc_t, NSBC_OK */
#include "media_codec.h"   /* CODEC_PCMU=0, CODEC_PCMA=8 */
#include "g711.h"           /* g711_ulaw_decode, g711_alaw_decode */
#include "uDS_client.h"     /* uds_send_session —— P1: 推 JSON VAD_EVENT 到 data.sock */
#include "log.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ===== per-call per-leg 状态槽 ===== */
typedef struct {
    void        *call_key;          /* NULL = 空槽 */
    uint16_t     threshold;          /* RMS 阈值（可变，uuid_vad_start 可覆盖） */
    vad_state_t  state[VAD_LEGS_PER_CALL];
    int          hangover_frames[VAD_LEGS_PER_CALL]; /* 连续同状态帧数 */
    uint16_t     last_rms[VAD_LEGS_PER_CALL];        /* 上一帧 RMS（诊断） */
    volatile int active;
} vad_call_state_t;

static vad_call_state_t g_states[VAD_MAX_CALLS];

/* ===== MPSC ring（状态变化异步推送）===== */
typedef struct {
    vad_event_t slots[VAD_RING_SIZE];
    uint32_t    head;           /* consumer 读 */
    uint32_t    tail;           /* producer 写 */
    pthread_cond_t not_empty;
    pthread_mutex_t lock;      /* consumer 侧用（热路径 producer 无锁） */
    volatile int  shutdown;
} vad_ring_t;

static vad_ring_t g_ring;
static pthread_t g_worker_tid;

/* ===== 工具函数 ===== */
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* call_key → 槽索引（线性探查，空槽 = NULL） */
static int find_or_alloc_slot(void *call_key) {
    uintptr_t h = ((uintptr_t)call_key) >> 4; /* 低地址对齐 */
    for (int i = 0; i < VAD_MAX_CALLS; ++i) {
        int idx = (h + i) % VAD_MAX_CALLS;
        if (!g_states[idx].call_key) {
            g_states[idx].call_key = call_key;
            g_states[idx].threshold = VAD_DEFAULT_THRESHOLD;
            g_states[idx].active = 1;
            g_states[idx].state[0] = VAD_STATE_SILENCE;
            g_states[idx].state[1] = VAD_STATE_SILENCE;
            g_states[idx].hangover_frames[0] = 0;
            g_states[idx].hangover_frames[1] = 0;
            g_states[idx].last_rms[0] = 0;
            g_states[idx].last_rms[1] = 0;
            return idx;
        }
        if (g_states[idx].call_key == call_key && g_states[idx].active)
            return idx;
    }
    return -1; /* 满了 */
}

static int find_slot(void *call_key) {
    uintptr_t h = ((uintptr_t)call_key) >> 4;
    for (int i = 0; i < VAD_MAX_CALLS; ++i) {
        int idx = (h + i) % VAD_MAX_CALLS;
        if (!g_states[idx].call_key) return -1; /* 空槽 */
        if (g_states[idx].call_key == call_key && g_states[idx].active)
            return idx;
    }
    return -1;
}

/* ===== 热路径：RMS 计算 ===== */
static inline uint16_t compute_rms_pcm(const int16_t *pcm, size_t samples) {
    if (!pcm || samples == 0) return 0;
    /* 用 32bit 累加避免溢出 */
    int32_t sum_sq = 0;
    for (size_t i = 0; i < samples; ++i) {
        int32_t s = (int32_t)pcm[i];
        sum_sq += s * s;
    }
    /* 近似：用移位代替 sqrt（热路径优化）
     * 但这里直接用 sqrt —— VAD 是可选功能，perf 优先但不差这一点 */
    double mean = (double)sum_sq / (double)samples;
    return (uint16_t)(mean > 0.0 ? sqrt(mean) : 0.0);
}

/* ===== 热路径：状态机 ===== */
static inline void vad_state_step(vad_call_state_t *cs, int leg, uint16_t rms) {
    cs->last_rms[leg] = rms;
    int above = (rms >= cs->threshold);

    if (cs->state[leg] == VAD_STATE_SILENCE) {
        if (above) {
            cs->hangover_frames[leg]++;
            if (cs->hangover_frames[leg] >= VAD_SPEECH_FRAMES) {
                cs->state[leg] = VAD_STATE_SPEECH;
                cs->hangover_frames[leg] = 0;
                /* 状态变化 → 异步投递 VAD_EVENT_SPEECH */
                vad_event_t ev = {
                    .call_key = cs->call_key,
                    .leg      = leg,
                    .new_state = VAD_STATE_SPEECH,
                    .rms      = rms,
                    .timestamp_ns = now_ns(),
                };
                /* 热路径：直接写 ring.tail（无锁），满了丢 */
                uint32_t next = (g_ring.tail + 1) & (VAD_RING_SIZE - 1);
                if (next != g_ring.head) {
                    g_ring.slots[g_ring.tail] = ev;
                    __atomic_store_n(&g_ring.tail, next, __ATOMIC_RELEASE);
                    pthread_cond_signal(&g_ring.not_empty); /* 有锁开销，但只在状态变化时调一次 */
                }
            }
        } else {
            cs->hangover_frames[leg] = 0; /* 没连续超阈值 → 重置 */
        }
    } else { /* SPEECH */
        if (!above) {
            cs->hangover_frames[leg]++;
            if (cs->hangover_frames[leg] >= VAD_SILENCE_FRAMES) {
                cs->state[leg] = VAD_STATE_SILENCE;
                cs->hangover_frames[leg] = 0;
                vad_event_t ev = {
                    .call_key = cs->call_key,
                    .leg      = leg,
                    .new_state = VAD_STATE_SILENCE,
                    .rms      = rms,
                    .timestamp_ns = now_ns(),
                };
                uint32_t next = (g_ring.tail + 1) & (VAD_RING_SIZE - 1);
                if (next != g_ring.head) {
                    g_ring.slots[g_ring.tail] = ev;
                    __atomic_store_n(&g_ring.tail, next, __ATOMIC_RELEASE);
                    pthread_cond_signal(&g_ring.not_empty);
                }
            }
        } else {
            cs->hangover_frames[leg] = 0;
        }
    }
}

/* ===== 热路径回调（media_bug global consumer）===== */
void vad_on_rtp_frame(media_bug_frame_t *frame, void *ud) {
    (void)ud;
    if (!frame || !frame->call_key || !frame->data || frame->data_len < 12) return;

    /* 只处理 A/B 腿（0 或 1） */
    if (frame->leg != 0 && frame->leg != 1) return;

    /* 查状态槽（热路径：查表是 O(1)，预分配数组直接索引） */
    int idx = find_slot(frame->call_key);
    if (idx < 0) return; /* 不在监控列表里 */

    /* 自己 rtp_parse（media_bug dispatch 没填 pt/ssrc/seq/ts） */
    rtp_pkt_t pkt;
    nsbc_rc_t prc = rtp_parse(frame->data, frame->data_len, &pkt);
    if (prc != NSBC_OK) return;

    /* 只处理 G.711 PCMU(pt=0) / PCMA(pt=8) */
    if (pkt.pt != 0 && pkt.pt != 8) return;

    /* payload → PCM */
    int16_t pcm_stack[320]; /* 最大 G.711 payload ≈ 320 字节 → 320 samples → 640B
                             * rtp.hdr_len + payload ≤ RELAY_BUF ≈ 1500B，这里给 320 够 */
    int16_t *pcm = pcm_stack;
    size_t pcm_cap = sizeof(pcm_stack) / sizeof(pcm_stack[0]);

    if (pkt.payload_len > pcm_cap) {
        /* 大 payload 走临时缓冲区（热路径避免 malloc，但这里极少见） */
        static __thread int16_t pcm_large[8192];
        pcm = pcm_large;
        pcm_cap = sizeof(pcm_large) / sizeof(pcm_large[0]);
        if (pkt.payload_len > pcm_cap) return;
    }

    size_t samples;
    if (pkt.pt == 0) /* PCMU */
        samples = g711_ulaw_decode(pkt.payload, pkt.payload_len, pcm);
    else /* PCMA */
        samples = g711_alaw_decode(pkt.payload, pkt.payload_len, pcm);

    if (samples == 0) return;

    /* RMS → 状态机 */
    uint16_t rms = compute_rms_pcm(pcm, samples);
    vad_state_step(&g_states[idx], frame->leg, rms);
}

/* ===== Worker 线程：异步推 data.sock VAD_EVENT（P1 升级）=====
 *
 * 复用 uds_send_session（通用 JSON 推送，data.sock Event Bus 协议）。
 * JSON schema: {"event":"VAD_EVENT","call_key":"0x%p","leg":0,"state":"SPEECH|SILENCE","rms":N,"ts_ns":N} */
static void *vad_worker(void *arg) {
    (void)arg;
    llog(LOG_NOTICE, "vad", "worker started (P1: data.sock VAD_EVENT push)");

    while (!g_ring.shutdown) {
        uint32_t tail = __atomic_load_n(&g_ring.tail, __ATOMIC_ACQUIRE);
        if (g_ring.head == tail) {
            pthread_mutex_lock(&g_ring.lock);
            pthread_cond_wait(&g_ring.not_empty, &g_ring.lock);
            pthread_mutex_unlock(&g_ring.lock);
            continue;
        }

        vad_event_t ev = g_ring.slots[g_ring.head];
        __atomic_store_n(&g_ring.head, (g_ring.head + 1) & (VAD_RING_SIZE - 1), __ATOMIC_RELEASE);

        /* P1: 推 JSON VAD_EVENT 到 data.sock Event Bus */
        const char *state_str = (ev.new_state == VAD_STATE_SPEECH) ? "SPEECH" : "SILENCE";
        char json[256];
        int n = snprintf(json, sizeof(json),
            "{\"event\":\"VAD_EVENT\",\"call_key\":\"%p\",\"leg\":%d,\"state\":\"%s\",\"rms\":%u,\"ts_ns\":%lu}",
            ev.call_key, ev.leg, state_str, ev.rms, (unsigned long)ev.timestamp_ns);
        if (n > 0 && n < (int)sizeof(json)) {
            uds_send_vad(json);   /* 异步非阻塞：UDS 内部有 ring buffer，kind="vad_event" */
        }

        /* 同时打 audit log（调试用） */
        llog(LOG_INFO, "vad", "VAD_EVENT call=%p leg=%d state=%s rms=%u",
             ev.call_key, ev.leg, state_str, ev.rms);
    }

    llog(LOG_NOTICE, "vad", "worker stopped");
    return NULL;
}

/* ===== 生命周期 ===== */

/* 文件级 static consumer：init 时注册，shutdown 时注销（需要指针匹配） */
static const media_bug_consumer_t kVadConsumer = {
    .name      = "vad_engine",
    .type_mask = BUG_RAW_RTP,
    .cb        = vad_on_rtp_frame,
    .ud        = NULL,
};

int vad_engine_init(void) {
    int rc = media_bug_register_global_consumer(&kVadConsumer);
    if (rc != 0) {
        llog(LOG_ERR, "vad", "media_bug_register_global_consumer rc=%d", rc);
        return rc;
    }

    /* 初始化 ring */
    memset(&g_ring, 0, sizeof(g_ring));
    pthread_mutex_init(&g_ring.lock, NULL);
    pthread_cond_init(&g_ring.not_empty, NULL);

    /* 起 worker 线程 */
    pthread_create(&g_worker_tid, NULL, vad_worker, NULL);

    llog(LOG_NOTICE, "vad", "vad_engine initialized (consumer registered, worker started)");
    return 0;
}

void vad_engine_shutdown(void) {
    g_ring.shutdown = 1;
    pthread_cond_signal(&g_ring.not_empty);
    pthread_join(g_worker_tid, NULL);

    pthread_mutex_destroy(&g_ring.lock);
    pthread_cond_destroy(&g_ring.not_empty);

    /* 注销 media_bug consumer */
    media_bug_unregister_global_consumer(&kVadConsumer);

    /* 清空状态表 */
    memset(g_states, 0, sizeof(g_states));

    llog(LOG_NOTICE, "vad", "vad_engine shutdown complete");
}

/* ===== OAM 控制 ===== */
int vad_start_for_call(void *call_key, int leg_mask, uint16_t threshold) {
    if (!call_key) return -1;
    int idx = find_or_alloc_slot(call_key);
    if (idx < 0) {
        llog(LOG_WARN, "vad", "VAD state table full (%d slots)", VAD_MAX_CALLS);
        return -1;
    }
    if (threshold > 0) g_states[idx].threshold = threshold;
    g_states[idx].active = 1;
    g_states[idx].state[0] = VAD_STATE_SILENCE;
    g_states[idx].state[1] = VAD_STATE_SILENCE;
    g_states[idx].hangover_frames[0] = 0;
    g_states[idx].hangover_frames[1] = 0;

    llog(LOG_NOTICE, "vad", "vad_start call=%p leg_mask=%d threshold=%u slot=%d",
         call_key, leg_mask, g_states[idx].threshold, idx);
    return 0;
}

int vad_stop_for_call(void *call_key) {
    if (!call_key) return -1;
    int idx = find_slot(call_key);
    if (idx < 0) return 0; /* 本来就没监控 */

    g_states[idx].active = 0;
    g_states[idx].call_key = NULL;

    llog(LOG_NOTICE, "vad", "vad_stop call=%p slot=%d", call_key, idx);
    return 0;
}

void vad_stop_all(void) {
    for (int i = 0; i < VAD_MAX_CALLS; ++i) {
        if (g_states[i].active) {
            g_states[i].active = 0;
            g_states[i].call_key = NULL;
        }
    }
}

/* ===== OAM 状态输出 ===== */
int vad_status(char *out, size_t outsz) {
    int active = 0;
    for (int i = 0; i < VAD_MAX_CALLS; ++i)
        if (g_states[i].active) ++active;

    return snprintf(out, outsz,
        "vad_engine: active_calls=%d ring_head=%u ring_tail=%u threshold_default=%u\n",
        active, g_ring.head, g_ring.tail, VAD_DEFAULT_THRESHOLD);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
