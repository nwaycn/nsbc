/* AI dialog module (optional, MOD_F_DYNAMIC)
 *
 * 纯媒体桥接层（passthrough）：
 *   [1] 媒体抓流    —— bridge_add_rec_feed_hook 挂 B 腿原始 G.711 帧
 *                      → per-call ring buffer (256KB)
 *   [2] 跨 codec 转码 —— 内部统一 PCMA；in_codec 由首帧 RX 动态确定，
 *                      transcode_open/process 跨 PCMA/G729/G722/... 转码
 *   [3] 注入 + 转人工 —— bridge_send_raw 注入 A 腿；b2b_transfer_call 转人工座席
 *
 * ASR / LLM / TTS 全部由外部 ai_agent 承担（经 pass-through API 读写音频流）。
 *
 * 模块分层：
 *   MOD_L_WORKER_READY  —— 早于 signaling FINAL（桥接层已就绪）
 *   非 REQUIRED          —— DYNAMIC 组 dlopen
 *   deps = {"signaling"} —— 桥接层必须先 init
 *
 * nsbc.yaml 配置：
 *   ai_dialog:
 *     enabled: 1
 */

#include "module.h"
#include "cfg.h"
#include "log.h"
#include "media_bridge.h"
#include "media_codec.h"   /* codec layer: transcode_open/process cross-codec transcode (PCMA/PCMU/G729/G722/...) */
#include "g711.h"          /* AI internal format: PCMU/PCMA <-> PCM16 LE 8kHz */
#include "b2bua.h"        /* P0-1: b2b_call_destroy_add + REQ-AI-TRANSFER-001: b2b_transfer_call */
#include "sdp.h"          /* AI_SESSION_OPENED 的 pt_in 需从 A 腿 SDP offer 解析真实 codec PT */
#include "callctrl.h"     /* REQ-AI-TRANSFER-001: CCF_XFER_BLIND */
#include "dp_sleep.h"     /* P0-2: dp_sleep_app_reg() */
#include "app_registry.h" /* P0-2: app_register / app_desc_t */
#include "app_ctx.h"      /* struct nsbc_ctx 完整定义（ctx->cfg）*/
#include "queue.h"        /* REQ-AI-TRANSFER-002: queue:<name> → 选员分机 */
#include "endpoints.h"    /* ep_lookup：判断队列成员是否内部分机 */
#include "location_db.h"  /* loc_query：判断成员当前是否有注册 */
#include "vad_engine.h"   /* REQ-AI-VAD-001: vad_start_for_call / vad_stop_for_call + HPF API */
#include "noise_suppression.h" /* WebRTC NS：上行固定 8kHz，10ms(80样本) 一块 */
#include "obs.h"          /* obs_event (redundant for AI_SESSION — see uDS_client.h below) */
#include "uDS_client.h"   /* ★ AI_SESSION_OPENED/CLOSED go through UDS 'session' topic → calls_tx → /calls/stream SSE */

#include "ai_backend_registry.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>     /* ENODEV — bridge_send_raw relay 未 latch 时返回此值 */
#include <string.h>
#include <time.h>
#include <unistd.h>

extern bridge_mgr_t *sig_bridge_instance(void);

/* ================================================================== */
/* 配置结构体                                                          */
/* ================================================================== */

typedef struct ai_cfg {
    int enabled;
} ai_cfg_t;

/* ================================================================== */
/* per-call 会话                                                       */
/* ================================================================== */

#define AI_RING_CAP (512 * 1024)   /* 512KB ~16s @ 8kHz PCM16 LE (AI unified format) */

/* ===== per-leg WebRTC DSP（AECM + NS + AGC，固定 8kHz / 10ms=80 样本）=====
 *
 * 只用到 3rd/webrtc 下 aecm.c / agc.c / noise_suppression.c 三个实现。
 * 这里不 include aecm.h / agc.h：那两个头文件会带入 FRAME_LEN / PART_LEN /
 * MAX_DELAY 等宏和一批 static const 表，在 -Wall -Wextra -Werror 下会触发
 * -Wunused-const-variable 直接编译失败。只声明实际用到的符号。 */
extern void    *WebRtcAecm_Create(void);
extern void     WebRtcAecm_Free(void *aecmInst);
extern int32_t  WebRtcAecm_Init(void *aecmInst, int32_t sampFreq);
extern int32_t  WebRtcAecm_BufferFarend(void *aecmInst, const int16_t *farend,
                                        size_t nrOfSamples);
extern int32_t  WebRtcAecm_Process(void *aecmInst, const int16_t *nearendNoisy,
                                   const int16_t *nearendClean, int16_t *out,
                                   size_t nrOfSamples, int16_t msInSndCardBuf);

/* 与 agc.h 的 WebRtcAgcConfig 布局完全一致（int16 / int16 / uint8）*/
typedef struct ai_agc_cfg {
    int16_t targetLevelDbfs;
    int16_t compressionGaindB;
    uint8_t limiterEnable;
} ai_agc_cfg_t;

extern void *WebRtcAgc_Create(void);
extern void  WebRtcAgc_Free(void *agcInst);
extern int   WebRtcAgc_Init(void *agcInst, int32_t minLevel, int32_t maxLevel,
                            int16_t agcMode, uint32_t fs);
extern int   WebRtcAgc_set_config(void *agcInst, ai_agc_cfg_t config);
extern int   WebRtcAgc_AddMic(void *agcInst, int16_t *const *inMic,
                              size_t num_bands, size_t samples);
extern int   WebRtcAgc_Process(void *agcInst, const int16_t *const *inNear,
                               size_t num_bands, size_t samples,
                               int16_t *const *out, int32_t inMicLevel,
                               int32_t *outMicLevel, int16_t echo,
                               uint8_t *saturationWarning);

#define AI_DSP_FRAME      80    /* 10ms @8kHz —— AECM/NS/AGC 统一块长 */
#define AI_NOISE_GATE     80    /* 噪声门阈值（样本绝对值），放在 AGC 之前：
                                 * 静音样本 ±8 直接归零，避免 30~50 残噪被 AGC 放大成 200+ 嘶声 */
#define AI_GATE_PREROLL   1600  /* 200ms @8kHz：静音期预录，VAD 判 SPEECH 时补发（防丢字头）*/
#define AI_GATE_HANGOVER  1600  /* 200ms @8kHz：SPEECH 结束后继续放行（防丢字尾）*/
/* AECM 启动门限提示：AECM 启动阶段要求 farend 缓冲达到
 * bufSizeStart = 3*(delay+10)/40 块（80 样本/块）才结束 startup。
 * 我们的 farend 是「发一包 TTS 补一块」的实时流，缓冲深度 = TX 分包长度
 * （Go 侧 640B/40ms → 4 块）。取 20ms → bufSizeStart=2，20ms/40ms 分包都
 * 能过启动门限；调大反而可能永远进不了回声消除。真实回声延迟由 AECM
 * 内部的二进制延迟估计器（最长 100×PART_LEN ≈ 800ms）自己搜。*/
#define AI_AECM_DELAY_MS  20

typedef struct ai_dsp {
    void     *aecm;         /* WebRtcAecm 实例（8kHz）；NULL = 不可用 */
    void     *agc;          /* WebRtcAgc 实例（8kHz adaptive digital）；NULL = 不可用 */
    NsHandle *ns;           /* WebRTC NS 实例（8kHz）；NULL = 不可用 */
    int32_t   mic_level;    /* AGC 虚拟 mic 电平（adaptive digital 初值 127）*/
    int       far_ever;     /* 是否收到过下行 TTS（没收到过就不跑 AECM）*/
    int       far_pending;  /* 已 BufferFarend 但尚未被 near 消费的 80 样本块数 */
    uint32_t  far_blocks;   /* DIAG：累计喂入 AECM 的 far 块数（0 = 本通没跑过 AECM）*/
    /* DIAG：逐级能量累计（avg|sample|），用于分别量化 AECM / NS / 噪声门 / AGC 的作用 */
    uint64_t  st_aec_sum;   /* AECM 之后 */
    uint64_t  st_ns_sum;    /* NS 之后 */
    uint64_t  st_gate_sum;  /* 噪声门之后（AGC 之前）*/
    uint64_t  st_samples;   /* 累计样本数 */
    uint64_t  st_far_sum;   /* DIAG：far 参考累计 |sample| */
    uint64_t  st_far_samples;
} ai_dsp_t;

typedef struct ai_session {
    void     *call_key;
    int       b_leg;
    int       pt_in;                /* 抓进来的 PT（通常 8=PCMA） */
    /* codec layer: internal baseline is PCMA. in_codec resolved from first RX frame
     * （默认 CODEC_PCMA）；rx_tc/tx_tc 是cross-codec transcode sessions, rebuilt on codec change. */
    codec_id_t     in_codec;        /* 主叫实际 codec（CODEC_PCMA/G729/...）*/
    transcode_sess_t *rx_tc;        /* in_codec → PCMA（收流转码）*/
    transcode_sess_t *tx_tc;        /* PCMA → in_codec（发流转码，format=0 用）*/
    codec_sess_t     *tx_pcm16_enc; /* in_codec encoder（format=1，PCM16 raw → in_codec 一次编码）*/
    uint32_t  ssrc;                 /* 自己注入用的 SSRC */
    uint16_t  seq;
    uint32_t  ts;
    pthread_t th;
    int       running;
    pthread_mutex_t lock;
    uint8_t   ring[AI_RING_CAP];
    size_t    r_head;
    size_t    r_tail;
    time_t    opened_at;
    int       producer;  /* 外部 producer 模式（第三方 AI 直接喂 tx_audio / 拉 rx_audio）*/

    /* per-leg HPF — same butterworth filter as vad_engine, strips DC + 50Hz mains hum
     * ring buffer previously had no HPF, so ASR fed slightly different audio than VAD detected */
    vad_hpf_t hpf[2];       /* leg 0 + leg 1 独立 HPF 状态 */
    ai_dsp_t  dsp[2];       /* per-leg AECM + NS + AGC（两腿音频状态不能混）*/
    /* DIAG：DSP 前后能量统计（每秒汇总一条，判断敲击/噪声是否被压掉）*/
    uint32_t  dsp_frames[2];
    uint32_t  dsp_pre_sum[2];
    uint32_t  dsp_post_sum[2];
    uint32_t  dsp_pre_max[2];
    uint32_t  dsp_post_max[2];
    /* VAD 门控：非语音不写 ring（不送 ai_agent），带 200ms pre-roll + 200ms hangover */
    int16_t   gate_pre[2][AI_GATE_PREROLL]; /* 静音期预录环形缓冲 */
    size_t    gate_pre_pos[2];
    size_t    gate_pre_fill[2];
    int       gate_hang[2];                 /* 剩余 hangover 样本数 */
    int       gate_on[2];                   /* 1 = 当前放行 */
    uint32_t  gate_drop[2];                 /* DIAG：本秒被门控丢掉的帧数 */

    /* external request to transfer AI session to human agent (blind transfer)
     * 设 transfer_dest + transfer_pending=1 → ai_session_thread 下次循环调
     * b2b_transfer_call 发 REFER + 设置 running=0 让线程退出。 */
    char      transfer_dest[256];
    int       transfer_pending;

    struct ai_session *next;
    struct ai_session *hnext;   /* P1-3：call_key 哈希桶链，替代热路径 O(N) 线性扫描 */
} ai_session_t;

#define AI_STORE_BUCKETS 256   /* 2 的幂 */

typedef struct ai_store {
    pthread_mutex_t lock;
    ai_session_t   *head;
    /* P1-3：call_key → session 的分片哈希。媒体线程每包查找从 O(会话数) 降到 O(1)。
     * hcount 用原子读作快速判空（无会话时热路径直接返回，不取锁）。 */
    ai_session_t   *htab[AI_STORE_BUCKETS];
    int             hcount;
    bridge_mgr_t   *bridge;
    void           *bridge_hook;
    ai_cfg_t        cfg;
    struct nsbc_ctx *ctx;   /* 转队列选员需要 ctx->cq_mgr / ctx->loc_db */
} ai_store_t;

static ai_store_t g_ai;
static cfg_t *g_cfg_ptr = NULL;

/* 前向声明：ai_rec_feed 里 pthread_create 要用；ai_session_thread_stub 在文件底部定义 */
static void *ai_session_thread_stub(void *arg);
/* P0-1 / P0-2: ai_init 里注册的两个函数，本文件底部定义 */
static void ai_on_call_destroy(void *call_key, int a_final, void *ud);
static app_rc_t ai_app_exec(app_ctx_t *c, const char *data, void *ud);
/* per-leg WebRTC DSP：session 创建处要调 ai_dsp_init，定义在 bridge hook 段 */
static void ai_dsp_init(ai_dsp_t *d);
static void ai_dsp_free(ai_dsp_t *d);
static void ai_dsp_far(ai_dsp_t *d, const int16_t *buf, size_t n);
static void ai_dsp_near(ai_dsp_t *d, int16_t *buf, size_t n);

/* P0-2: 给 dp app 或全局 hook 共用的 session 查找/创建辅助 */

/*  从 A 腿 SDP offer 解析第一个 audio codec 的 PT。
 * local:ai 模式下 A 腿自答，AI_SESSION_OPENED 事件的 pt_in 不能硬编码 0（PCMU）——
 * 否则 microsip 配 PCMA（pt=8）时，Go agent 会把 PCMA 误判成 PCMU，
 * 导致 TX 编码错乱 / ASR 精度下降。offer 里首个 audio 段的首个 codec 即真实 PT。 */
static int ai_first_audio_pt(b2b_call_t *call)
{
    if (!call) return 0;
    b2b_leg_t *a = &call->leg[0];
    if (!a->offer_len || !a->offer[0]) return 0;

    sdp_t s;
    memset(&s, 0, sizeof(s));
    if (sdp_parse(&s, a->offer, a->offer_len) != NSBC_OK)
        return 0;

    for (int i = 0; i < s.nmedia; ++i) {
        sdp_media_t *m = &s.media[i];
        if (m->port == 0 || m->rejected) continue;
        if (m->type != SDP_M_AUDIO) continue;
        if (sdp_media_build_codecs(&s, m) == NSBC_OK && m->ncodecs > 0)
            return m->codecs[0].pt;
    }
    return 0;
}

/* P1-3：call_key 哈希桶操作。以下 *_locked 后缀表示调用方必须持 st->lock。 */
static unsigned ai_ck_bucket(const void *k)
{
    uintptr_t v = (uintptr_t)k;
    v ^= v >> 7;
    return (unsigned)(v & (AI_STORE_BUCKETS - 1));
}

static ai_session_t *ai_store_find_locked(ai_store_t *st, void *call_key)
{
    for (ai_session_t *s = st->htab[ai_ck_bucket(call_key)]; s; s = s->hnext)
        if (s->call_key == call_key)
            return s;
    return NULL;
}

static void ai_store_link_locked(ai_store_t *st, ai_session_t *s)
{
    unsigned b = ai_ck_bucket(s->call_key);
    s->hnext = st->htab[b];
    st->htab[b] = s;
    __atomic_add_fetch(&st->hcount, 1, __ATOMIC_RELAXED);
    s->next = st->head;
    st->head = s;
}

static void ai_store_unlink_locked(ai_store_t *st, ai_session_t *s)
{
    unsigned b = ai_ck_bucket(s->call_key);
    ai_session_t **hp = &st->htab[b];
    while (*hp && *hp != s)
        hp = &(*hp)->hnext;
    if (*hp) {
        *hp = s->hnext;
        __atomic_sub_fetch(&st->hcount, 1, __ATOMIC_RELAXED);
    }
    ai_session_t **pp = &st->head;
    while (*pp && *pp != s)
        pp = &(*pp)->next;
    if (*pp)
        *pp = s->next;
}

static ai_session_t *ai_session_find_or_create(ai_store_t *st, void *call_key,
                                               int pt_in, const char *src)
{
    pthread_mutex_lock(&st->lock);
    ai_session_t *s = ai_store_find_locked(st, call_key);
    if (!s) {
        s = (ai_session_t *)calloc(1, sizeof(*s));
        if (!s) { pthread_mutex_unlock(&st->lock); return NULL; }
        s->call_key = call_key;
        s->b_leg = 1;
        s->pt_in = pt_in;
        s->in_codec = CODEC_PCMA;  /* 默认 PCMA；首帧 RX 到达后动态修正为实际 codec */
        s->ssrc = 0x4E534243 ^ ((uintptr_t)call_key & 0xFFFFFFFFu);
        s->seq = (uint16_t)(rand() & 0xFFFF);
        s->ts = 0;
        s->opened_at = time(NULL);
        pthread_mutex_init(&s->lock, NULL);

        /* per-leg HPF init, 50Hz cutoff / 8kHz mono — same coefficients as vad_engine*/
        vad_hpf_init(&s->hpf[0], 50.0f, 8000.0f);
        vad_hpf_init(&s->hpf[1], 50.0f, 8000.0f);

        /* per-leg WebRTC DSP：AECM(回声消除) + NS(降噪 Mild 6dB) + AGC(数字增益)。
         * 两腿音频状态不能混，各一套实例；某一级创建失败只降级该级，不影响通话 */
        for (int i = 0; i < 2; i++) {
            ai_dsp_init(&s->dsp[i]);
        }
        ai_store_link_locked(st, s);
        llog(LOG_INFO, "ai",
             "session opened call=%p pt=%d via=%s HPF init AECM=%s NS=%s AGC=%s",
             call_key, pt_in, src,
             (s->dsp[0].aecm && s->dsp[1].aecm) ? "on(8k)" : "off",
             (s->dsp[0].ns && s->dsp[1].ns) ? "on(8k,mild)" : "off",
             (s->dsp[0].agc && s->dsp[1].agc) ? "on(8k,+12dB)" : "off");

        /* emit AI_SESSION_OPENED via uds_send_session('session' topic)
         * → UDS data.sock → uds_server.rs → calls_tx.send()
         * → /calls/stream (SSE) → Go SseEventToFrame fallback:
         *   payload["event"]="ai_session_opened" → strings.ToUpper → Domain="AI_SESSION_OPENED" ✅
         * Go agent's aiSessionTracker.markOpened(callID) on receive.
         * Must NOT use obs_event here — obs_event writes to C-internal g_ev ring,
         * web/src has zero subscribers, Go agent would never see it (confirmed
         * by poller.rs comment + grep for calls_tx.send = only uds_server.rs). */
        {
            b2b_call_t *b2b = (b2b_call_t *)call_key;
            const char *cid = (b2b && b2b->leg[0].callid[0])
                              ? b2b->leg[0].callid : "unknown";
            /* in_codec 字段用 pt_in 解析真实 codec_id（PCMA=8 → CODEC_PCMA），
             * 不能直接报 s->in_codec（默认 PCMA，首帧才动态修正）——否则 Go agent
             * 会在 AI_SESSION_OPENED 阶段把 PCMA 误判成 PCMU。 */
            const codec_ops_t *opsc = (pt_in > 0) ? codec_registry_find_pt(pt_in, 8000) : NULL;
            int in_codec_report = opsc ? (int)opsc->id : (int)s->in_codec;
            char sess[384];
            snprintf(sess, sizeof(sess),
                "{\"event\":\"ai_session_opened\",\"call_id\":\"%s\","
                "\"pt_in\":%d,\"in_codec\":%d}",
                cid, pt_in, in_codec_report);
            uds_send_session(sess);
        }
        s->running = 1;
        int prc = pthread_create(&s->th, NULL, ai_session_thread_stub, s);
        if (prc != 0) {
            llog(LOG_ERR, "ai", "session opened but pthread_create failed rc=%d (EAGAIN=%d)! thread NOT started", prc, 11);
            s->running = 0;
        } else {
            llog(LOG_INFO, "ai", "pthread_create OK thread started call=%p tid=%lu", call_key, (unsigned long)s->th);
        }
    } else {
        /* 复用旧 session —— 如果 thread 已退出（running=0 或 th=0），重新启动 */
        llog(LOG_INFO, "ai", "session found (reuse) call=%p pt=%d running=%d → check thread state",
             call_key, pt_in, s->running);
        pthread_mutex_lock(&s->lock);
        int need_restart = (s->running == 0);
        pthread_mutex_unlock(&s->lock);
        if (need_restart) {
            s->running = 1;
            int prc = pthread_create(&s->th, NULL, ai_session_thread_stub, s);
            if (prc != 0) {
                llog(LOG_ERR, "ai", "reuse session pthread_create failed rc=%d (EAGAIN=%d)! running=0", prc, 11);
                s->running = 0;
            } else {
                llog(LOG_INFO, "ai", "reuse pthread_create OK call=%p tid=%lu", call_key, (unsigned long)s->th);
            }
        }
    }
    pthread_mutex_unlock(&st->lock);
    return s;
}

/* ================================================================== */
/* bridge 动态 hook 回调（媒体线程）                                    */
/* ================================================================== */

/* ===== per-leg DSP 生命周期与处理 ===== */

static void ai_dsp_init(ai_dsp_t *d)
{
    memset(d, 0, sizeof(*d));
    d->mic_level = 127;   /* WebRtcAgc_Init 在 adaptive digital 下的 mid-point */

    /* AECM：8kHz 窄带回声消除 */
    d->aecm = WebRtcAecm_Create();
    if (d->aecm && WebRtcAecm_Init(d->aecm, 8000) != 0) {
        WebRtcAecm_Free(d->aecm);
        d->aecm = NULL;
    }

    /* AGC：adaptive digital，目标 -3dBFS / 压缩增益 12dB / 开 limiter */
    d->agc = WebRtcAgc_Create();
    if (d->agc) {
        ai_agc_cfg_t cfg = { 3, 12, 1 };
        if (WebRtcAgc_Init(d->agc, 0, 255, 2 /* kAgcModeAdaptiveDigital */, 8000) != 0 ||
            WebRtcAgc_set_config(d->agc, cfg) != 0) {
            WebRtcAgc_Free(d->agc);
            d->agc = NULL;
        }
    }

    /* NS：8kHz / Mild(6dB) */
    d->ns = WebRtcNs_Create();
    if (d->ns && (WebRtcNs_Init(d->ns, 8000) != 0 ||
                  WebRtcNs_set_policy(d->ns, 0) != 0)) {
        WebRtcNs_Free(d->ns);
        d->ns = NULL;
    }
}

static void ai_dsp_free(ai_dsp_t *d)
{
    if (d->aecm) { WebRtcAecm_Free(d->aecm); d->aecm = NULL; }
    if (d->agc)  { WebRtcAgc_Free(d->agc);   d->agc  = NULL; }
    if (d->ns)   { WebRtcNs_Free(d->ns);     d->ns   = NULL; }
}

/* 求和 |sample|（DIAG 用）*/
static uint64_t ai_sum_abs(const int16_t *b, size_t n)
{
    uint64_t s = 0;
    for (size_t i = 0; i < n; i++) s += (uint64_t)(b[i] < 0 ? -b[i] : b[i]);
    return s;
}

/* 下行（我们发给话机的 TTS）作为 AECM 远端参考，只在注入腿（leg 0）调用。
 * AECM 只接受 80/160 样本块，不足一块的尾巴丢弃（G.711 帧恒为 160 样本）。*/
static void ai_dsp_far(ai_dsp_t *d, const int16_t *buf, size_t n)
{
    if (!d->aecm || !buf) return;
    for (size_t off = 0; off + AI_DSP_FRAME <= n; off += AI_DSP_FRAME) {
        if (WebRtcAecm_BufferFarend(d->aecm, buf + off, AI_DSP_FRAME) >= 0) {
            d->far_pending++;
            d->far_blocks++;
            d->far_ever = 1;
            d->st_far_sum += ai_sum_abs(buf + off, AI_DSP_FRAME);
            d->st_far_samples += AI_DSP_FRAME;
        }
    }
}

/* 上行 DSP 链：AECM（回声消除）→ NS（降噪）→ 噪声门 → AGC（增益），原地 10ms 一块。 */
static void ai_dsp_near(ai_dsp_t *d, int16_t *buf, size_t n)
{
    static const int16_t silence[AI_DSP_FRAME] = { 0 };

    for (size_t off = 0; off + AI_DSP_FRAME <= n; off += AI_DSP_FRAME) {
        int16_t *frame = buf + off;

        /* AECM：收到过下行 TTS 才启用；没有待消费的远端块时补静音，
         * 保证 far/near 两条流按同一时间轴推进（否则延迟估计会失准）。*/
        if (d->aecm && d->far_ever) {
            if (d->far_pending > 0) d->far_pending--;
            else WebRtcAecm_BufferFarend(d->aecm, silence, AI_DSP_FRAME);

            int16_t aec_out[AI_DSP_FRAME];
            if (WebRtcAecm_Process(d->aecm, frame, NULL, aec_out,
                                   AI_DSP_FRAME, AI_AECM_DELAY_MS) >= 0) {
                memcpy(frame, aec_out, sizeof(aec_out));
            }
        }
        d->st_aec_sum += ai_sum_abs(frame, AI_DSP_FRAME);

        /* NS：Analyze 必须先喂同一块（噪声底估计在 Analyze 内）*/
        if (d->ns) {
            int16_t *band[1] = { frame };
            WebRtcNs_Analyze(d->ns, frame);
            WebRtcNs_Process(d->ns, (const int16_t *const *)band, 1, band);
        }
        d->st_ns_sum += ai_sum_abs(frame, AI_DSP_FRAME);

        /* Noise gate：必须在 AGC 之前 —— 否则 30~50 的残噪会被 AGC 放大到 200+
         * 变成嘶声（实测 pre_max=41 → post_max=257）。*/
        for (size_t i = 0; i < AI_DSP_FRAME; i++) {
            if (frame[i] < AI_NOISE_GATE && frame[i] > -AI_NOISE_GATE) {
                frame[i] = 0;
            }
        }
        d->st_gate_sum += ai_sum_abs(frame, AI_DSP_FRAME);
        d->st_samples += AI_DSP_FRAME;

        /* AGC：AddMic 先统计包络，Process 再按增益表输出（in/out 可同缓冲）*/
        if (d->agc) {
            int16_t *mic[1] = { frame };
            int32_t out_level = d->mic_level;
            uint8_t saturation = 0;
            WebRtcAgc_AddMic(d->agc, mic, 1, AI_DSP_FRAME);
            if (WebRtcAgc_Process(d->agc, (const int16_t *const *)mic, 1,
                                  AI_DSP_FRAME, mic, d->mic_level, &out_level,
                                  (int16_t)(d->aecm && d->far_ever), &saturation) == 0) {
                d->mic_level = out_level;
            }
        }
    }
}

/* DIAG：avg|sample|，口径与 vad_engine 的 rms 一致（0..32767）*/
static int32_t ai_rms16(const int16_t *buf, size_t n)
{
    if (!buf || n == 0) return 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = buf[i];
        sum += (uint64_t)(v < 0 ? -v : v);
    }
    return (int32_t)(sum / n);
}

/* DIAG：DSP 前后能量对比，每腿每秒一条（50 帧 @20ms）。
 * 判定方法：
 *   pre_max 有尖峰、post_max 明显下降 → 被 AECM/NS 压掉了；
 *   post_max ≈ pre_max               → 原样进了 ring buffer（即被 ASR 录进去）。
 * 注意：AECM 只消 TTS 回声、NS 是频谱减法，对敲桌子这类宽带瞬态基本无效，
 *       AGC 还会把未被压掉的低电平内容再放大 —— 这条日志就是用来量化这个的。*/
static void ai_dsp_diag(ai_session_t *s, void *call_key, int leg,
                        int32_t pre, int32_t post)
{
    s->dsp_frames[leg]++;
    s->dsp_pre_sum[leg]  += (uint32_t)pre;
    s->dsp_post_sum[leg] += (uint32_t)post;
    if ((uint32_t)pre  > s->dsp_pre_max[leg])  s->dsp_pre_max[leg]  = (uint32_t)pre;
    if ((uint32_t)post > s->dsp_post_max[leg]) s->dsp_post_max[leg] = (uint32_t)post;

    if (s->dsp_frames[leg] < 50) return;

    uint32_t n = s->dsp_frames[leg];
    uint64_t sm = s->dsp[leg].st_samples;
    uint32_t aec_avg  = sm ? (uint32_t)(s->dsp[leg].st_aec_sum  / sm) : 0;
    uint32_t ns_avg   = sm ? (uint32_t)(s->dsp[leg].st_ns_sum   / sm) : 0;
    uint32_t gate_avg = sm ? (uint32_t)(s->dsp[leg].st_gate_sum / sm) : 0;
    uint64_t fsm = s->dsp[leg].st_far_samples;
    uint32_t far_avg = fsm ? (uint32_t)(s->dsp[leg].st_far_sum / fsm) : 0;
    llog(LOG_INFO, "ai",
         "DIAG dsp call=%p leg=%d pre_avg=%u aec_avg=%u ns_avg=%u gate_avg=%u post_avg=%u"
         " pre_max=%u post_max=%u far=%u far_avg=%u gate=%d drop=%u frames=%u",
         call_key, leg,
         s->dsp_pre_sum[leg] / n, aec_avg, ns_avg, gate_avg,
         s->dsp_post_sum[leg] / n, s->dsp_pre_max[leg], s->dsp_post_max[leg],
         s->dsp[leg].far_blocks, far_avg, s->gate_on[leg], s->gate_drop[leg], n);
    s->dsp_frames[leg]    = 0;
    s->dsp_pre_sum[leg]   = 0;
    s->dsp_post_sum[leg]  = 0;
    s->dsp_pre_max[leg]   = 0;
    s->dsp_post_max[leg]  = 0;
    s->gate_drop[leg]     = 0;
    s->dsp[leg].st_aec_sum  = 0;
    s->dsp[leg].st_ns_sum   = 0;
    s->dsp[leg].st_gate_sum = 0;
    s->dsp[leg].st_samples  = 0;
    s->dsp[leg].st_far_sum  = 0;
    s->dsp[leg].st_far_samples = 0;
}

/* ring buffer 写入（持 s->lock 由调用方负责）*/
static void ai_ring_put(ai_session_t *s, const int16_t *pcm, size_t n)
{
    size_t ring_len = n * 2;
    const uint8_t *ring_data = (const uint8_t *)pcm;

    size_t space = (s->r_head >= s->r_tail)
        ? (AI_RING_CAP - s->r_head + s->r_tail - 1)
        : (s->r_tail - s->r_head - 1);
    if (space < ring_len) {
        /* ring 满：丢最旧（不阻塞媒体线程） */
        s->r_tail = (s->r_tail + (ring_len - space) + 1) % AI_RING_CAP;
        space = AI_RING_CAP - 1 - ((s->r_head >= s->r_tail)
              ? (s->r_head - s->r_tail)
              : (s->r_tail - s->r_head));
    }
    if (space >= ring_len) {
        /* P2-12：两段 memcpy 替代逐字节写 + 每字节取模。 */
        size_t first = AI_RING_CAP - s->r_head;
        if (first > ring_len) first = ring_len;
        memcpy(&s->ring[s->r_head], ring_data, first);
        if (ring_len > first)
            memcpy(s->ring, ring_data + first, ring_len - first);
        s->r_head += ring_len;
        if (s->r_head >= AI_RING_CAP) s->r_head -= AI_RING_CAP;
    }
}

/* VAD 门控：VAD_STATE_SILENCE 不写 ring（非人声不送 ai_agent）。
 *  - pre-roll：静音期最近 200ms 留在 gate_pre，VAD 判 SPEECH 时按时间顺序补发 → 不丢字头
 *  - hangover：SPEECH 结束后继续放行 200ms → 不丢字尾
 *  - VAD 不可用（未 start / 该 leg 无槽位）→ fail-open 全放行，不影响业务 */
static void ai_gate_push(ai_session_t *s, void *call_key, int leg,
                         const int16_t *pcm, size_t n)
{
    if (n == 0) return;
    if (leg < 0 || leg >= 2) { ai_ring_put(s, pcm, n); return; }

    vad_state_t st = VAD_STATE_SILENCE;
    uint16_t rms = 0;
    int have_vad = (vad_query_for_call(call_key, leg, &st, &rms) == 0);

    if (!have_vad || st != VAD_STATE_SILENCE) {
        if (!s->gate_on[leg]) {
            /* 静音 → 语音：先补发 pre-roll（环形，分两段保持时序）*/
            size_t fill = s->gate_pre_fill[leg];
            if (fill > 0) {
                size_t start = (s->gate_pre_pos[leg] + AI_GATE_PREROLL - fill)
                               % AI_GATE_PREROLL;
                if (start + fill <= AI_GATE_PREROLL) {
                    ai_ring_put(s, &s->gate_pre[leg][start], fill);
                } else {
                    size_t first = AI_GATE_PREROLL - start;
                    ai_ring_put(s, &s->gate_pre[leg][start], first);
                    ai_ring_put(s, &s->gate_pre[leg][0], fill - first);
                }
            }
            s->gate_pre_fill[leg] = 0;
            s->gate_on[leg] = 1;
        }
        s->gate_hang[leg] = AI_GATE_HANGOVER;
        ai_ring_put(s, pcm, n);
        return;
    }

    /* SILENCE：hangover 内继续放行 */
    if (s->gate_on[leg] && s->gate_hang[leg] > 0) {
        s->gate_hang[leg] -= (int)n;
        if (s->gate_hang[leg] < 0) s->gate_hang[leg] = 0;
        ai_ring_put(s, pcm, n);
        return;
    }
    s->gate_on[leg] = 0;
    s->gate_drop[leg]++;

    /* 继续收集 pre-roll（覆盖最旧）*/
    for (size_t i = 0; i < n; i++) {
        s->gate_pre[leg][s->gate_pre_pos[leg]] = pcm[i];
        s->gate_pre_pos[leg] = (s->gate_pre_pos[leg] + 1) % AI_GATE_PREROLL;
        if (s->gate_pre_fill[leg] < AI_GATE_PREROLL) s->gate_pre_fill[leg]++;
    }
}

static void ai_rec_feed(void *ud, void *call_key,
                        const void *payload, size_t len,
                        int leg, int pt)
{
    ai_store_t *st = (ai_store_t *)ud;
    if (!st || !st->cfg.enabled || !call_key) return;

    /*  2026-11-17 修复：两边都抓（leg=0 主叫 + leg=1 被叫）。
     * previously only leg=1 (B side), but SIPp UAS B side sends no RTP -> zero hook invocations.
     * 用户可能在主叫也可能在被叫 —— 两边都进 ring buffer，thread 做 ASR 时拿到完整对话。
     * 注：后续可加 per-leg ring 分区避免 AI 听到自己 TTS 回复，但 demo 模式 ASR 返回空不影响。*/
    (void)leg;  /* 两边都抓 —— 不做 leg 过滤 */

    /*  核心修复（2026-11-17）：全局 hook 只负责喂数据，不自动创建 session。
     * 只有 dialplan 配了 "ai" app（如拨 9999）时 ai_app_exec 才显式创建 session，
     * 避免分机对打、落地呼叫等误进入 AI 管道。
     * 找不到 session → 说明这个 call 没走 AI dialplan → 直接 return，零开销。*/
    /* P1-3：无会话时直接返回，媒体线程热路径不取锁（hcount 原子读作提示）。 */
    if (__atomic_load_n(&st->hcount, __ATOMIC_RELAXED) == 0)
        return;
    pthread_mutex_lock(&st->lock);
    ai_session_t *s = ai_store_find_locked(st, call_key);
    pthread_mutex_unlock(&st->lock);
    if (!s) return;   /* 没有 dialplan ai app 触发 → 跳过 */

    /*   codec layer: static PT maps to codec_id. Dynamic PT (>=96, e.g. Opus) needs SDP rtpmap
     * 建立 per-session 映射（见 media_codec.h），暂不支持 → drop，不污染 ring。 */
    const codec_ops_t *ops = codec_registry_find_pt(pt, 8000);
    if (!ops) {
        llog(LOG_DEBUG, "ai", "rec_feed pt=%d 未注册/动态PT → drop call=%p", pt, call_key);
        return;
    }
    codec_id_t in_codec = ops->id;

    /* P1 defense: clock_rate must be 8kHz (AI internal format is PCM16 LE 8kHz mono)
     * codec_registry_find_pt 完全忽略 clock_rate 参数（被 (void) 吃掉），
     * 所以非 8kHz codec 可能会被误命中 —— 这里显式检查。
     * 允许非 8kHz 但必须走 transcode（transcode 内部做重采样）；
     * 禁止非 8kHz 且 codec 本身就是 G.711 的（不存在，G.711 永远 8kHz）。*/
    if (ops->clock_rate != 8000) {
        /* 非 8kHz：transcode 应该会做重采样，但我们在首次确定 codec 时
         * 打一条 WARN 提醒运维 —— 避免 silent bug（半速/倍速 decode 歪音）*/
        llog(LOG_WARN, "ai", "⚠️ codec %s(pt=%d) clock_rate=%d ≠ 8000 → 依赖 transcode 重采样 call=%p",
             ops->name, pt, ops->clock_rate, call_key);
    }

    /*  AI internal format: PCM16 LE 8kHz mono (NASR ASR / NWAYTTS intermediate)
     * 入站路径：RTP payload → (transcode 非PCMA→PCMA) → g711_alaw_decode → PCM16 → ring buffer
     * 为什么 PCM16？
     *   - PCMA 是 8bit 有损压缩，NASR ASR 喂 PCMA 识别精度差
     *   - PCM16 是完整 16bit 无损，后续 DSP（高通/降噪）也在 PCM16 域做
     *   - ring buffer 每帧字节翻倍（20ms → 320B PCM16 vs 160B PCMA），256KB 够 16s
     *   - Go agent ai_get_rx_audio 直接拿 PCM16 喂 NASR，不用再转 */
    uint8_t tc_buf[2048];
    size_t tc_len = 0;  /* transcode output len */

    /* per-frame PCM16 栈缓冲（20ms @ 8kHz G.711 → 160 samples → 320B PCM16，留 1024 够 60ms） */
    int16_t pcm16_buf[1024];
    size_t pcm16_samples = 0;
    /* G.711 解码器不接收输出容量参数，解码输入必须先钳制到 pcm16_buf 采样数，
     * 否则超长 RTP payload / 转码输出会写穿栈缓冲。*/
    const size_t pcm16_cap = sizeof(pcm16_buf) / sizeof(pcm16_buf[0]);

    /* 写 ring buffer（持 s->lock 保护并发写）；转码+解码都在锁内完成 ——
     * rx_tc 是 per-session 会话，仅在媒体线程单线程访问，无并发。 */
    pthread_mutex_lock(&s->lock);
    if (in_codec != s->in_codec) {
        /* first resolution / codec change (re-INVITE) -> rebuild RX+TX transcode sessions + PCM16 encoder */
        if (s->rx_tc) { transcode_close(s->rx_tc); s->rx_tc = NULL; }
        if (s->tx_tc) { transcode_close(s->tx_tc); s->tx_tc = NULL; }
        if (s->tx_pcm16_enc) { codec_registry_find(in_codec)->fini(s->tx_pcm16_enc); s->tx_pcm16_enc = NULL; }
        s->in_codec = in_codec;
        if (in_codec != CODEC_PCMA && in_codec != CODEC_PCMU) {
            /* 只有非 G.711 codec 才走 transcode（G729/G722/Opus → PCMA）
             * G.711 我们直接原生 ulaw/alaw_decode，绕过 transcode 库的 PCMU↔PCMA 转换链
             * —— 那条链在 e77e82e 引入，实测有 bug（PCMU payload 转 PCMA 后 alaw_decode 解歪）。
             * 参考 vad_engine.c L591-593 也是这么做的。 */
            transcode_sess_t *rx = NULL, *tx = NULL;
            if (transcode_open(in_codec, CODEC_PCMA, &rx) == NSBC_OK) s->rx_tc = rx;
            if (transcode_open(CODEC_PCMA, in_codec, &tx) == NSBC_OK) s->tx_tc = tx;
            /* format=1/2 优化：raw PCM16 直接 encode 到 in_codec，跳过中间 PCMA 有损编码 */
            const codec_ops_t *enc_ops = codec_registry_find(in_codec);
            if (enc_ops) {
                codec_sess_t *enc = NULL;
                if (enc_ops->encoder_init) enc_ops->encoder_init(NULL, &enc);
                else if (enc_ops->init)    enc_ops->init(NULL, &enc);
                s->tx_pcm16_enc = enc;
            }
            llog(LOG_INFO, "ai", "transcode init call=%p in=%d→PCMA rx=%p PCMA→in tx=%p pcm16_enc=%p",
                 call_key, (int)in_codec, (void*)s->rx_tc, (void*)s->tx_tc, (void*)s->tx_pcm16_enc);
        } else {
            llog(LOG_INFO, "ai", "rec_feed direct g711 decode codec=%d(%s) call=%p (no transcode)",
                 (int)in_codec, ops->name, call_key);
        }
    }

    if (in_codec == CODEC_PCMU) {
        /* PCMU (pt=0) — 直接原生 ulaw_decode，绕 transcode */
        pcm16_samples = g711_ulaw_decode(payload, len < pcm16_cap ? len : pcm16_cap,
                                         pcm16_buf);
    } else if (in_codec == CODEC_PCMA) {
        /* PCMA (pt=8) — 直接原生 alaw_decode */
        pcm16_samples = g711_alaw_decode(payload, len < pcm16_cap ? len : pcm16_cap,
                                         pcm16_buf);
    } else if (s->rx_tc) {
        /* 非 G.711 codec (G729/G722/Opus) — 先走 transcode → PCMA → alaw_decode */
        size_t out_len = 0;
        int rc = transcode_process(s->rx_tc, payload, len, tc_buf, sizeof(tc_buf), &out_len);
        if (rc != NSBC_OK) {
            pthread_mutex_unlock(&s->lock);
            return;  /* AGAIN/错误：本帧跳过 */
        }
        tc_len = out_len;
        pcm16_samples = g711_alaw_decode(tc_buf, tc_len < pcm16_cap ? tc_len : pcm16_cap,
                                         pcm16_buf);
    } else {
        llog(LOG_WARN, "ai", "⚠️ rec_feed codec=%d(%s) 既不是 G.711 也没 rx_tc → drop call=%p",
             (int)in_codec, ops ? ops->name : "?", call_key);
        pthread_mutex_unlock(&s->lock);
        return;
    }

    /* P0: HPF strips DC offset and 50Hz mains hum
     * ring buffer previously had no HPF, so ASR fed slightly different audio than VAD detected
     * 现在两边都用同一份 HPF 系数，ASR 吃到干净音频 */
    if (pcm16_samples > 0 && leg >= 0 && leg < 2) {
        vad_hpf_process(&s->hpf[leg], pcm16_buf, pcm16_samples);
    }

    /* 上行 DSP：AECM 回声消除 → NS 降噪 → AGC 数字增益（per-leg，8kHz/10ms 一块）
     * 放在 HPF 之后，让 AEC/AGC 处理的是已去直流和工频干扰的信号。
     * 原先手写的 peak-normalization AGC 已由 WebRTC AGC 取代（避免双重增益）。 */
    int32_t dsp_pre = 0, dsp_post = 0;
    if (pcm16_samples > 0) dsp_pre = ai_rms16(pcm16_buf, pcm16_samples);
    if (pcm16_samples > 0 && leg >= 0 && leg < 2) {
        ai_dsp_near(&s->dsp[leg], pcm16_buf, pcm16_samples);
    }

    /* DIAG：DSP 前后能量（每秒一条）——判断敲击/环境噪声是真被压掉还是原样进了 ASR */
    if (pcm16_samples > 0 && leg >= 0 && leg < 2) {
        dsp_post = ai_rms16(pcm16_buf, pcm16_samples);
    }

    /* VAD 门控：非人声（SILENCE）不写 ring → 不送 ai_agent；
     * 200ms pre-roll 防丢字头，200ms hangover 防丢字尾 */
    ai_gate_push(s, call_key, leg, pcm16_buf, pcm16_samples);

    if (pcm16_samples > 0 && leg >= 0 && leg < 2) {
        ai_dsp_diag(s, call_key, leg, dsp_pre, dsp_post);
    }

    pthread_mutex_unlock(&s->lock);
}

/* ================================================================== */
/* Pass-through public API for third-party AI to read/write audio     */
/* ================================================================== */

/* AI 模块架构分层：
 *   ┌──────────────────────────────────────────────────────────┐
 *   │ ai_dialog 模块（本文件，C 核心媒体管道层）                  │
 *   │   • ai_rec_feed hook → ring buffer 入队                   │
 *   │   • ring buffer ← ai_get_rx_audio（第三方 AI 读用户语音）    │
 *   │   • ai_send_tx_audio（第三方 AI 写回复） → bridge_send_raw → A 腿 │
 *   │   • ai_request_transfer → b2b_transfer_call（转人工座席）     │
 *   │   • 纯媒体桥接：抓流 + ring buffer + 跨 codec 转码 + 注入 + 转人工     │
 *   └──────────────────────────────────────────────────────────┘
 *   ┌──────────────────────────────────────────────────────────┐
 *   │ 外部 AI（第三方 Python/Go/Rust agent，跑在 nsbc-web 侧车或独立进程） │
 *   │   • HTTP / UDS 订阅 nsbc 事件 → 知道哪通电话要接管            │
 *   │   • ai_get_rx_audio 拉用户语音 → ASR（本地 Whisper/云端）      │
 *   │   • LLM 对话管理（function call / 多轮上下文 / 知识库检索）      │
 *   │   • TTS（云或本地 VITS/GPT-SoVITS）                         │
 *   │   • ai_send_tx_audio 推回复 → 用户听                         │
 *   │   • ai_request_transfer → b2b_transfer_call（复杂意图识别到转人工）│
 *   └──────────────────────────────────────────────────────────┘
 *
 * 为什么这么分：
 *   - C 核心只做媒体桥接（ring buffer + bridge_send_raw），ASR/LLM/TTS 都在外部
 *   - 第三方 AI 想用什么 ASR/LLM/TTS 都能接（OpenAI / 本地 Ollama / 火山引擎 / 腾讯云）
 *   - 转人工座席是 B2BUA 业务层能力，留在 C 核心（b2b_transfer_call 已实现）
 *
 * 音频格式约定：
 *   - RX（用户→AI）：PCMA 8kHz，每次 ai_rec_feed 入队的是完整 RTP 载荷（通常 160 字节 / 20ms）
 *   - TX（AI→用户）：PCMA 8kHz 原始字节（**调用方必须喂 PCMA，本模块不管 codec 转换**），
 *     bridge_send_raw 按 RTP 节奏注入 —— codec 转换已模块化嵌入（media_bridge 只管 RTP 封装 + sendto）
 *   - 缓冲区大小：256KB（≈32s @ 8kHz PCMA）
 */

/* 外部 producer 模式启动 session（绕过 dp ai app，直接让外部 AI 接管音频）
 *
 * call_key:   b2b_call_t*（不透明指针）
 * producer:   1 = 外部 AI 喂 tx_audio（producer 模式，直接接管音频）
 *             0 = 默认（纯媒体桥接，无内部管道）
 * 返回：NSBC_OK / NSBC_NOENT（没 session）/ NSBC_INVAL */
int ai_set_producer(void *call_key, int producer)
{
    if (!call_key) return NSBC_INVAL;
    pthread_mutex_lock(&g_ai.lock);
    ai_session_t *s = ai_store_find_locked(&g_ai, call_key);
    if (!s) { pthread_mutex_unlock(&g_ai.lock); return NSBC_NOENT; }
    pthread_mutex_lock(&s->lock);
    int old_producer = s->producer;
    s->producer = producer ? 1 : 0;
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_unlock(&g_ai.lock);
    llog(LOG_INFO, "ai", "ai_set_producer call=%p producer=%d", call_key, producer);

    /* producer mode auto-starts/stops VAD monitoring
     * producer=1 → 对 A 腿（被叫用户腿）启动 VAD，Go 侧按 VAD 边界切句
     * producer=0 → 停止 VAD，call_destroy 时也会停 */
    if (producer && !old_producer) {
        int rc = vad_start_for_call(call_key, 3 /* leg_mask=3 双腿：bit0=A, bit1=B */, 0 /* 默认阈值 */);
        llog(rc == 0 ? LOG_INFO : LOG_WARN, "ai",
             "vad_start_for_call call=%p (auto by producer=1) rc=%d", call_key, rc);
    } else if (!producer && old_producer) {
        int rc = vad_stop_for_call(call_key);
        llog(LOG_INFO, "ai", "vad_stop_for_call call=%p (auto by producer=0) rc=%d",
             call_key, rc);
    }
    return NSBC_OK;
}

/* 第三方 AI 拉取用户语音 —— AI 内部统一 PCM16 LE 8kHz mono
 *
 * call_key:    b2b_call_t*
 * out_buf:     调用方缓冲区（至少 out_max_len 字节）
 * out_max_len: 最多读多少字节（建议 320 的整数倍 = 20ms PCM16 @ 8kHz）
 * 返回：实际读取的字节数（0 = ring buffer 空 / 无 session / session 未启动）
 *
 * ring buffer 存 PCM16 LE 8kHz mono（由 ai_rec_feed 的 g711_alaw_decode 入队），
 * 每 20ms 帧 = 160 samples = 320 bytes。NASR ASR 直接喂此 PCM16，
 * 不再走 PCMA→PCM16 有损转换。 */
size_t ai_get_rx_audio(void *call_key, uint8_t *out_buf, size_t out_max_len)
{
    if (!call_key || !out_buf || out_max_len == 0) return 0;
    pthread_mutex_lock(&g_ai.lock);
    ai_session_t *s = ai_store_find_locked(&g_ai, call_key);
    if (!s || !s->running) { pthread_mutex_unlock(&g_ai.lock); return 0; }

    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_ai.lock);   /* 避免死锁：g_ai.lock → s->lock 已按顺序 */

    size_t n = 0;
    while (n < out_max_len && s->r_head != s->r_tail) {
        out_buf[n++] = s->ring[s->r_tail];
        s->r_tail = (s->r_tail + 1) % AI_RING_CAP;
    }
    pthread_mutex_unlock(&s->lock);
    return n;
}

/* 第三方 AI 推送回复音频给用户 —— REQ-AI-MEDIA-001 Pass-through
 *
 * call_key:  b2b_call_t*（不透明指针）
 * data:      按 format 解释（见下）
 * len:       字节数
 * format:    音频格式标识
 *   0 = PCMA 8kHz（legacy，经 transcode PCMA→in_codec）
 *   1 =  PCM16 LE 8kHz mono（AI 内部统一格式，一次 encode 到 in_codec）
 *   2 = PCMA 8kHz（legacy）
 * 返回：NSBC_OK / NSBC_NOENT / NSBC_INVAL / NSBC_AGAIN
 *
 * AI 内部统一格式 = PCM16 LE 8kHz mono：
 *   RX：ai_rec_feed → g711_alaw_decode → ring buffer 存 PCM16 → ai_get_rx_audio 拿 PCM16
 *   TX：外部 AI agent 喂 PCM16 (format=1) → C 侧 codec.encode() 一次 → bridge_send_raw
 * 为什么 PCM16？NASR ASR / NWAYTTS 都是 16bit 中间格式，PCMA 8bit 有损编码会直接降低识别精度。
 * Go agent ai_agent_daemon 所有音频传递用 format=1，cross-codec encoding handled by C side codec layer. */
int ai_send_tx_audio(void *call_key, const uint8_t *data, size_t len, int format)
{
    if (!call_key || !data || len == 0) return NSBC_INVAL;

    pthread_mutex_lock(&g_ai.lock);
    ai_session_t *s = ai_store_find_locked(&g_ai, call_key);
    if (!s) { pthread_mutex_unlock(&g_ai.lock); return NSBC_NOENT; }
    pthread_mutex_unlock(&g_ai.lock);

    /* 读 session 字段用 s->lock —— 与 ai_rec_feed 更新 in_codec/rx_tc/tx_tc 同一把锁，
     * 避免 codec 重建（re-INVITE）与 TX 转码的并发竞态。 */
    pthread_mutex_lock(&s->lock);
    if (!s->running) { pthread_mutex_unlock(&s->lock); return NSBC_NOENT; }

    codec_id_t in_codec = s->in_codec;
    uint8_t   pt       = 8;  /* 默认 PCMA */
    if (in_codec != CODEC_PCMA && in_codec != CODEC_NONE) {
        const codec_ops_t *cops = codec_registry_find(in_codec);
        if (cops && cops->default_pt >= 0) pt = (uint8_t)cops->default_pt;
    }
    transcode_sess_t *tx_tc       = s->tx_tc;
    codec_sess_t     *tx_pcm16_enc = s->tx_pcm16_enc;
    uint32_t  ssrc     = s->ssrc;
    uint16_t *seq_ptr  = &s->seq;
    uint32_t *ts_ptr   = &s->ts;
    pthread_mutex_unlock(&s->lock);

    /* format=1 (PCM16 raw) -> one-shot encode to in_codec, no intermediate PCMA loss
     * format=0（PCMA）→ 走 transcode PCMA→in_codec（旧路径，兼容） */
    const uint8_t *tx_data = data;
    size_t tx_len = len;
    uint8_t tc_buf[2048];

    if (format == 1) {
        /* --- format=1：PCM16 raw → in_codec（一次 encode）---
         * Go agent NWAYTTS 合成的是 8kHz 16bit mono PCM16_LE，
         * 我们跳过 Go 侧 PCM16→PCMA（8bit 有损编码），直接在 C 侧
         * 用 G.711 encoder.encode() 一次编码到 in_codec。
         * 对比原来两次编码（Go PCM16→PCMA + C PCMA→in_codec），
         * 现在只编码 1 次 = 和用户说话链路同保真度。 */
        const codec_ops_t *enc_ops = codec_registry_find(
            in_codec != CODEC_NONE ? in_codec : CODEC_PCMA);
        if (!enc_ops || !enc_ops->encode) return NSBC_NOENT;

        /* 懒创建 encoder sess（欢迎语阶段 in_codec 可能还是默认 CODEC_PCMA）*/
        codec_sess_t *enc = tx_pcm16_enc;
        if (!enc) {
            if (enc_ops->encoder_init) enc_ops->encoder_init(NULL, &enc);
            else if (enc_ops->init)    enc_ops->init(NULL, &enc);
            /* 赋值回 session（下次就懒不到了）*/
            pthread_mutex_lock(&s->lock);
            if (!s->tx_pcm16_enc) s->tx_pcm16_enc = enc;
            pthread_mutex_unlock(&s->lock);
        }

        /* dynamic sample rate from codec_ops->clock_rate instead of hardcoded 8000
         * 硬编码 8kHz 会导致 G.722(16k) 等非 8k codec 半速快放 */
        int codec_rate = enc_ops->clock_rate > 0 ? enc_ops->clock_rate : 8000;
        int input_rate = 8000;  /* Go TTS 喂的固定 8kHz PCM16（format=1 约定）*/

        /* P3 defense: resample gap when codec_rate != 8000
         * 当前 resample.c 只有骨架（resampler_process 返回 NOTSUP），
         * 所以非 8kHz codec 的 TX 路径暂时不通 —— 打 WARN 提醒运维
         * （RX 侧 codec_registry_find_pt 已修复 clock_rate 过滤，
         * 16kHz static PT codec 会被 drop；动态 PT 需要 SDP rtpmap）*/
        if (codec_rate != input_rate) {
            static __thread int rate_mismatch_warned = 0;
            if (!rate_mismatch_warned++) {
                llog(LOG_WARN, "ai",
                    "⚠️ TX path: input PCM16 rate=%d ≠ codec rate=%d (codec=%s) → "
                    "resample skeleton NOTSUP, encode 可能歪音",
                    input_rate, codec_rate, enc_ops->name);
            }
        }

        /* 按 codec ptime_ms + 真实 clock_rate 动态算帧长
         * G.711 ptime=20ms, 8kHz → 160 samples → ts_inc=160
         * G.722 ptime=20ms, 16kHz → 320 samples → ts_inc=320 */
        int ptime_ms = enc_ops->ptime_ms > 0 ? enc_ops->ptime_ms : 20;
        size_t samples_per_frame = (size_t)ptime_ms * (size_t)codec_rate / 1000;
        size_t bytes_per_frame   = samples_per_frame * 2; /* 16bit → bytes */
        uint32_t ts_inc = (uint32_t)ptime_ms * (uint32_t)codec_rate / 1000;

        /* ★ TX 噪声根因修复：尾帧 zero-padding
         *
         * Go sendAudioPaced 每包 640B (40ms)，C 按 320B (20ms) 分帧。
         * 最后一包如果不是整倍数（如 TTS 总长度 6410B → 最后一包 10B），
         * 原来直接 encode 非整帧 → bridge_send_raw 仍然 ts+=160（按整帧）
         * → RTP timestamp 跳跃 → 接收端解码器 concealment/interpolate → 噼啪噪声！
         *
         * 修复：所有帧都 zero-pad 到 bytes_per_frame，确保 encode 输入和
         * ts_inc 严格对齐。尾帧静音 padding 不影响音质（G.711 每 sample 独立编码）。
         *
         * 注：曾考虑 silence gate（跳过 near-zero 帧），但跳过帧会让 ts 不推进，
         * 连续跳过 N 帧后 ts 跳跃 N*ts_inc → 同样产生解码噪声 → 放弃。
         * G.711 编码静音成本极低（每帧 160B），不值得冒 timestamp gap 的风险。 */
        int16_t pcm_frame_buf[1024];  /* 栈上局部缓冲（最多 1024 samples ≈ 64ms @8k）*/

        for (size_t off = 0; off < len; off += bytes_per_frame) {
            size_t remaining = len - off;
            size_t frame_bytes = (remaining > bytes_per_frame) ? bytes_per_frame : remaining;

            /* 拷贝到局部缓冲 + zero-pad 剩余字节 */
            memcpy(pcm_frame_buf, data + off, frame_bytes);
            if (frame_bytes < bytes_per_frame) {
                memset((uint8_t *)pcm_frame_buf + frame_bytes, 0,
                       bytes_per_frame - frame_bytes);
            }

            /* AECM 远端参考：我们发给话机的 TTS 就是回声来源。
             * 只支持窄带 8kHz；只喂注入腿（leg 0）。AECM 内部 farend 环形缓冲
             * 与 ai_rec_feed 的 near 处理同受 s->lock 保护。 */
            if (codec_rate == 8000) {
                pthread_mutex_lock(&s->lock);
                ai_dsp_far(&s->dsp[0], pcm_frame_buf, samples_per_frame);
                pthread_mutex_unlock(&s->lock);
            }

            media_frame_t mf = {
                .pcm      = pcm_frame_buf,
                .samples  = samples_per_frame,   /* 始终整帧 → ts_inc 严格对齐 */
                .rate     = codec_rate,
                .channels = 1,
            };
            size_t out_len = 0;
            int rc = enc_ops->encode(enc, &mf, tc_buf, sizeof(tc_buf), &out_len);
            if (rc != NSBC_OK || out_len == 0) continue;

            int n = bridge_send_raw(g_ai.bridge, call_key, 0,
                                    tc_buf, out_len, pt, ssrc, seq_ptr, ts_ptr, ts_inc);
            if (n < 0 && n == -ENODEV) return NSBC_AGAIN;
        }
        return NSBC_OK;
    }

    /* --- format=0：PCMA → in_codec（transcode 一次或直通）---
     * 内部统一 PCMA，如果 in_codec == PCMA 则不转码直接注入 */
    /* format=0 的 data 是 PCMA 字节流：AECM farend 需要 PCM16，先解一份喂 far */
    {
        int16_t far_pcm[1024];
        size_t  far_n = (len < sizeof(far_pcm) / 2) ? len : sizeof(far_pcm) / 2;
        size_t  got = g711_alaw_decode(data, far_n, far_pcm);
        if (got > 0) {
            pthread_mutex_lock(&s->lock);
            ai_dsp_far(&s->dsp[0], far_pcm, got);
            pthread_mutex_unlock(&s->lock);
        }
    }

    if (tx_tc) {
        size_t out_len = 0;
        int rc = transcode_process(tx_tc, data, len, tc_buf, sizeof(tc_buf), &out_len);
        if (rc != NSBC_OK) {
            if (rc == NSBC_AGAIN) return NSBC_OK;
            return NSBC_NOENT;
        }
        if (out_len == 0) return NSBC_OK;
        tx_data = tc_buf;
        tx_len = out_len;
    }

    /* bridge_send_raw 签名：
     *   int bridge_send_raw(bridge_mgr_t *m, void *call_key, int leg,
     *                       const void *payload, size_t len, uint8_t pt,
     *                       uint32_t ssrc, uint16_t *seq, uint32_t *ts, uint32_t ts_inc);
     * 返回实际 send 字节数（含 RTP 头），<0 表示失败 */
    int n = bridge_send_raw(g_ai.bridge, call_key, 0,
                            tx_data, tx_len, pt, ssrc, seq_ptr, ts_ptr, 160);
    if (n < 0) {
        if (n == -ENODEV) return NSBC_AGAIN;
        return NSBC_NOENT;
    }
    return NSBC_OK;
}

/* ================================================================== */
/* per-call 处理线程 stub（pthread_create 只收一个 (void*)）              */
/* ================================================================== */

static void *ai_session_thread_stub(void *arg)
{
    ai_session_t *s = (ai_session_t *)arg;
    ai_store_t *st = &g_ai;
    llog(LOG_INFO, "ai", "thread start call=%p", s->call_key);

    while (s->running) {
        /* transfer 触发（移到最前，producer 与任何模式都能触发，修复原 bug）*/
        pthread_mutex_lock(&s->lock);
        if (s->transfer_pending && s->transfer_dest[0]) {
            char dest[256];
            snprintf(dest, sizeof(dest), "%s", s->transfer_dest);
            s->transfer_pending = 0;
            pthread_mutex_unlock(&s->lock);

            llog(LOG_INFO, "ai", "ai_session transfer trigger call=%p dest=%s",
                 s->call_key, dest);
            int trc = b2b_transfer_call((b2b_call_t *)s->call_key,
                                        -1, dest,
                                        CCF_XFER_BLIND, NULL, NULL);
            llog(trc == NSBC_OK ? LOG_INFO : LOG_WARN, "ai",
                 "ai_session transfer rc=%d call=%p dest=%s", trc, s->call_key, dest);

            s->running = 0;
            break;
        }
        pthread_mutex_unlock(&s->lock);
        nanosleep((const struct timespec[]){{0, 20 * 1000 * 1000}}, NULL);
    }

    /* ★ transfer 或其他原因退出线程前，必须先停 producer → 停 VAD 引擎 + 通知 ai_agent。
     * VAD 是独立注册在媒体桥的回调，session free 不会自动停它，
     * 会继续把音频转发给 ai_agent → 出现几百条 "grace DONE" 刷屏。 */
    llog(LOG_INFO, "ai", "thread exit cleanup call=%p (producer→0, vad_stop)", s->call_key);
    ai_set_producer(s->call_key, 0);

    /* 线程退出清理 */
    pthread_mutex_lock(&st->lock);
    pthread_mutex_lock(&s->lock);
    ai_store_unlink_locked(st, s);
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_unlock(&st->lock);

    if (s->rx_tc) { transcode_close(s->rx_tc); s->rx_tc = NULL; }
    if (s->tx_tc) { transcode_close(s->tx_tc); s->tx_tc = NULL; }
    if (s->tx_pcm16_enc) {
        const codec_ops_t *ops = codec_registry_find(s->in_codec != CODEC_NONE ? s->in_codec : CODEC_PCMA);
        if (ops && ops->fini) ops->fini(s->tx_pcm16_enc);
        s->tx_pcm16_enc = NULL;
    }
    for (int i = 0; i < 2; i++) {
        ai_dsp_free(&s->dsp[i]);
    }

    llog(LOG_INFO, "ai", "thread exit call=%p", s->call_key);
    pthread_mutex_destroy(&s->lock);
    free(s);
    return NULL;
}

/* P0-1: b2b_call_destroy 回调 —— 主动关 per-call session 线程
 * 在 call_destroy 末尾（mem_obj_release 之前）调，must return quickly (no blocking).
 * 这里只做 running=0 + 异步 pthread_join（timeout 1s，超时 log 但不阻塞 call_destroy）。
 * 线程退出时会从链表摘除 session 并 free。*/
static void ai_on_call_destroy(void *call_key, int a_final, void *ud)
{
    ai_store_t *st = (ai_store_t *)ud;
    if (!st || !call_key) return;

    ai_session_t *victim = NULL;
    pthread_mutex_lock(&st->lock);
    victim = ai_store_find_locked(st, call_key);
    pthread_mutex_unlock(&st->lock);

    if (!victim) return;
    llog(LOG_INFO, "ai", "call_destroy call=%p a_final=%d → stop session",
         call_key, a_final);

    /* emit AI_SESSION_CLOSED via uds_send_session (same 'session' topic path as OPENED) */
    {
        b2b_call_t *b2b = (b2b_call_t *)call_key;
        const char *cid = (b2b && b2b->leg[0].callid[0])
                          ? b2b->leg[0].callid : "unknown";
        char sess[256];
        snprintf(sess, sizeof(sess),
            "{\"event\":\"ai_session_closed\",\"call_id\":\"%s\",\"a_final\":%d}",
            cid, a_final);
        uds_send_session(sess);
    }

    /* 告诉线程该退出了 */
    victim->running = 0;

    /* 最多等 10s pthread_join —— transfer 触发 + 线程清理可能耗时，
     * 加长到 10s 避免 call_destroy 还没等完 AI 请求就释放 call 对象。
     *  pthread_timedjoin_np 的 timespec 是**绝对时间**（CLOCK_REALTIME），
     * old code wrote {10,0} = year 1970, immediate timeout -> call_destroy releases call early
     * 对象，空转线程随后解引用已释放内存（延时崩溃/段错误）。 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    if (pthread_timedjoin_np(victim->th, NULL, &ts) != 0) {
        llog(LOG_WARN, "ai", "call_destroy join timeout (10s) — thread may still run (leak)");
    }
    /* 线程自己会 free session —— 如果 join 成功线程退出时已经 free 了；
     * 如果 timeout，我们在这里兜底 free（但要先从链表摘——可能线程也在 free，
     * 简单起见 timeout 时不 free，让进程退出时 fini 统一清）*/
}

/* ================================================================== */
/* public API — request AI session blind transfer to agent           */
/* ================================================================== */

/* 从 A 腿请求 URI / To 头提取 SIP domain（租户 realm）。
 * 入站 INVITE 的 leg[0].ruri 形如 "sip:1001@acme.com" / "sip:1001@acme.com:5080"。
 * 顺序：先 ruri 后 to；都取不到回退 "nsbc.local"（保持旧行为可跑通）。
 * 为什么不用 core->cfg.realm：b2b_core_t 在 b2bua.h 中是不透明类型，
 * ai_dialog 拿不到 cfg；从呼叫本身提取还能天然跟随多租户。 */
static void ai_realm_of_call(b2b_call_t *b2b, char *out, size_t outsz)
{
    out[0] = '\0';
    if (!b2b || outsz == 0) return;
    const char *cands[2] = { b2b->leg[0].ruri, b2b->leg[0].to };
    for (int i = 0; i < 2 && !out[0]; ++i) {
        const char *s = cands[i];
        if (!s || !s[0]) continue;
        const char *at = strchr(s, '@');
        if (!at) continue;
        at++;
        size_t n = 0;
        while (at[n] && at[n] != ';' && at[n] != '>' && at[n] != '?' &&
               at[n] != ' ' && at[n] != '\r' && at[n] != '\n' &&
               n + 1 < outsz)
            n++;
        if (n > 0) {
            memcpy(out, at, n);
            out[n] = '\0';
        }
    }
    if (!out[0]) snprintf(out, outsz, "%s", "nsbc.local");
}

/* AI 转队列选员：只接受“当前有注册绑定的内部分机”。
 * 外部分机靠 trunk 外呼，无法用 sip:<ext>@<realm> 表达，直接排除；
 * 没有可用成员时由调用方返回失败，避免 REFER 到永远接不通的分机。 */
typedef struct {
    struct nsbc_ctx *ctx;
    const char    *realm;
} ai_qavail_t;

static int ai_queue_member_avail(const char *ext, void *ud)
{
    ai_qavail_t *av = (ai_qavail_t *)ud;
    const ep_ext_t *ep;
    char aor[256];
    loc_binding_t b;
    if (!av || !av->ctx || !ext || !ext[0]) return 0;
    ep = ep_lookup(ext, av->realm);
    if (!ep) return 0;
    if (strcmp(ep->ext_type, "external") == 0) return 0;
    snprintf(aor, sizeof(aor), "%s@%s", ext, av->realm);
    return av->ctx->loc_db
        ? (loc_query((loc_db_t *)av->ctx->loc_db, aor, &b, 1, nsbc_now_ms()) > 0)
        : 0;
}

/* 外部（OAM CLI / LLM function call / Webhook / IVR）可调用此 API
 * 让 AI session 在下次 ASR→LLM→TTS 管道跑完后（或立即如果管道空闲）
 * 调 b2b_transfer_call(call, -1, dest, CCF_XFER_BLIND) 发 REFER。
 *
 * 非阻塞：只设 session->transfer_dest + transfer_pending=1，
 * 实际 transfer 由 ai_session_thread 自己做（避免跨线程碰 b2b_call_t）。
 *
 * 返回：NSBC_OK=设置成功；NSBC_NOENT=call_key 没 session；NSBC_INVAL=dest 非法 */
int ai_request_transfer(void *call_key, const char *dest)
{
    if (!call_key || !dest || !dest[0]) return NSBC_INVAL;
    size_t dl = strlen(dest);
    if (dl > 256) return NSBC_INVAL;
    for (size_t i = 0; i < dl; ++i) {
        unsigned char ch = (unsigned char)dest[i];
        if (ch < 0x21 || ch > 0x7e) return NSBC_INVAL; /* 白名单校验 */
    }

    /* 不支持的 target 前缀：b2b_transfer_call 只会向对端发 REFER，语义是
     * “让对端去拨这个 URI”。对端拨不了 ACD 队列，也无法从 URI 表达
     * “走哪条中继”。这些目标需要 C 核心内部改呼叫目标 + 重建 B 腿
     * （Phase 3 b2b_retarget_call），当前明确拒绝，避免拼出
     * sip:queue:sales@realm 这种坏 URI 静默下沉。
     * 例外：queue:<name> 由本函数先按队列策略选出一个可振铃成员分机，
     * 再转成 sip:<member>@<realm>（直接转给该座席，非排队等待）。 */
    if (!strncmp(dest, "trunk:", 6) ||
        !strncmp(dest, "gateway:", 8) || !strncmp(dest, "route:", 6) ||
        !strncmp(dest, "ivr:", 4)     || !strcmp(dest, "hangup")) {
        llog(LOG_WARN, "ai",
             "ai_request_transfer: dest='%s' 需要内部改目标能力（未实现），拒绝", dest);
        return NSBC_PERM_DENIED;
    }

    pthread_mutex_lock(&g_ai.lock);
    ai_session_t *s = ai_store_find_locked(&g_ai, call_key);
    if (!s) {
        pthread_mutex_unlock(&g_ai.lock);
        llog(LOG_WARN, "ai", "ai_request_transfer: no session for call=%p", call_key);
        return NSBC_NOENT;
    }

    /* dest 归一化：
     *   - 含 "sip:"/"sips:" 前缀或 "@" → 已是完整 SIP URI，原样使用
     *   - "extension:<num>" → 剥前缀后按分机号补全本呼叫 realm
     *   - 其它（纯分机号）→ 按分机号补全本呼叫 realm
     * realm 从 A 腿 ruri/to 提取，不再写死 nsbc.local（多租户下会拨错域）。 */
    char full_dest[300];
    if (!strncmp(dest, "queue:", 6)) {
        /* queue:<name>：按队列策略选一个当前可振铃的内部分机，再转给它。
         * 这是“转到队列的可用座席”，不是排队等待的完整 ACD；没有可用
         * 成员时明确失败，避免 REFER 到一个永远接不通的目标。 */
        const char *qname = dest + 6;
        cc_mgr_t *mgr = (g_ai.ctx && g_ai.ctx->cq_mgr)
                        ? (cc_mgr_t *)g_ai.ctx->cq_mgr : NULL;
        char realm[128];
        char member[AGENT_EXT_MAX];
        ai_qavail_t av;
        int prc;
        if (!qname[0] || mgr == NULL) {
            pthread_mutex_unlock(&g_ai.lock);
            return NSBC_NOENT;
        }
        ai_realm_of_call((b2b_call_t *)call_key, realm, sizeof(realm));
        av.ctx = g_ai.ctx;
        av.realm = realm;
        prc = cq_pick_member(mgr, realm, qname, 0,
                             ai_queue_member_avail, &av, call_key,
                             member, sizeof(member));
        if (prc != 0) {
            pthread_mutex_unlock(&g_ai.lock);
            llog(LOG_WARN, "ai", "transfer queue '%s': no available agent (rc=%d)",
                 qname, prc);
            return NSBC_NOENT;
        }
        snprintf(full_dest, sizeof(full_dest), "sip:%s@%s", member, realm);
        llog(LOG_INFO, "ai", "transfer queue '%s' → agent '%s'@'%s'",
             qname, member, realm);
    } else if (strstr(dest, "sip:") || strchr(dest, '@')) {
        snprintf(full_dest, sizeof(full_dest), "%s", dest);
    } else {
        const char *user = (!strncmp(dest, "extension:", 10)) ? dest + 10 : dest;
        if (!user[0]) {
            pthread_mutex_unlock(&g_ai.lock);
            return NSBC_INVAL;
        }
        char realm[128];
        ai_realm_of_call((b2b_call_t *)call_key, realm, sizeof(realm));
        snprintf(full_dest, sizeof(full_dest), "sip:%s@%s", user, realm);
    }
    /* b2b_transfer_call 对 refer_to 有 256 字节上限，超长在此提前拒绝，
     * 避免设置成功后在 session 线程里才失败、调用方却收到 OK。 */
    if (strlen(full_dest) > 256) {
        pthread_mutex_unlock(&g_ai.lock);
        return NSBC_INVAL;
    }

    pthread_mutex_lock(&s->lock);
    snprintf(s->transfer_dest, sizeof(s->transfer_dest), "%s", full_dest);
    s->transfer_pending = 1;
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_unlock(&g_ai.lock);
    llog(LOG_INFO, "ai", "ai_request_transfer call=%p dest=%s → %s (pending)",
         call_key, dest, full_dest);
    return NSBC_OK;
}

/* ================================================================== */
/* P0-2: dp app "ai" 执行 —— 在 INVITE resolve 命中 dialplan ai app_chain 时调
 * 创建 per-call ai_session（早于 ai_rec_feed 全局 hook 创建的时间点）。
 * 立即返回 APP_RC_OK（per-call 线程异步处理媒体桥接：抓流/转码/注入/转人工，
 * 不阻塞 dialplan 执行器）。session 由 dp "ai" app 创建即启用。*/
static app_rc_t ai_app_exec(app_ctx_t *c, const char *data, void *ud)
{
    (void)data;
    ai_store_t *st = (ai_store_t *)ud;
    if (!st || !st->cfg.enabled || !c || !c->call_key)
        return APP_RC_FAILED;

    pthread_mutex_lock(&st->lock);
    ai_session_t *s = ai_store_find_locked(st, c->call_key);
    if (!s) {
        pthread_mutex_unlock(&st->lock);
        /* ★ 修复 payload 误判：pt_in 不能硬编码 0（PCMU）。
         * 从 A 腿 SDP offer 解析真实 codec PT（microsip 配 PCMA 时 = 8），
         * AI_SESSION_OPENED 事件带正确的 pt_in/in_codec，Go agent 才能正确编码 TX 音频。 */
        int real_pt = ai_first_audio_pt((b2b_call_t *)c->call_key);
        llog(LOG_INFO, "ai", "ai_app_exec: A-leg offer first audio pt=%d (was hardcoded 0)",
             real_pt);
        s = ai_session_find_or_create(st, c->call_key, real_pt, "dp_ai");
        if (!s) {
            llog(LOG_WARN, "ai", "ai_app_exec: session create failed");
            return APP_RC_FAILED;
        }
        pthread_mutex_lock(&st->lock);
    }
    pthread_mutex_unlock(&st->lock);

    llog(LOG_INFO, "ai", "dp ai app fired call=%p target=%s",
         c->call_key, c->dp_target ? c->dp_target : "");
    return APP_RC_OK;
}

/* ================================================================== */
/* init / fini                                                          */
/* ================================================================== */

static int ai_init(struct nsbc_ctx *ctx)
{
    memset(&g_ai, 0, sizeof(g_ai));
    pthread_mutex_init(&g_ai.lock, NULL);
    g_cfg_ptr = ctx->cfg;
    g_ai.ctx = ctx;

    /* 读配置 —— cfg_get_int 第3参数 long*, 第4参数 cfg_layer_t* 或 NULL
     * 默认值 1 作 fallback；cfg_get_int 成功（返回 0）则用 yaml 值覆盖。
     * 带 layer 诊断：如果被更高优先级层 overlay，log 会揭示来源。*/
    g_ai.cfg.enabled   = 1;      /* 默认启用 */

    long tmp = 0;
    cfg_layer_t got_layer = CFG_L__COUNT;
    int rc = cfg_get_int(g_cfg_ptr, "ai_dialog.enabled", &tmp, &got_layer);
    if (rc == 0) {
        g_ai.cfg.enabled = (int)tmp;
        llog(LOG_INFO, "ai", "cfg_get_int ai_dialog.enabled=%ld (layer=%s, rc=%d)",
             tmp, cfg_layer_name(got_layer), rc);
    } else {
        llog(LOG_WARN, "ai", "cfg_get_int ai_dialog.enabled: rc=%d → keep default %d",
             rc, g_ai.cfg.enabled);
    }

    if (!g_ai.cfg.enabled) {
        llog(LOG_INFO, "ai", "disabled by config");
        return NSBC_OK;
    }

    /* 拿 bridge_mgr —— REQUIRED signaling 先 init，这里桥一定已建 */
    g_ai.bridge = sig_bridge_instance();
    if (!g_ai.bridge) {
        llog(LOG_ERR, "ai", "bridge_mgr null");
        return NSBC_ERR;
    }

    /* 注册 rec_feed hook（不碰 signaling 的录音主槽） */
    g_ai.bridge_hook = bridge_add_rec_feed_hook(g_ai.bridge, ai_rec_feed, &g_ai);
    if (!g_ai.bridge_hook) {
        llog(LOG_ERR, "ai", "bridge_add_rec_feed_hook failed");
        return NSBC_ERR;
    }
    llog(LOG_INFO, "ai", "hook registered on bridge %p", (void *)g_ai.bridge);

    /* P0-2: 注册 dp application "ai" —— 让拨号计划能精准启用 AI 对话
     * nsbc.yaml dialplan.rules: "ai:0:ivr_ai" → 执行 app_chain 里的 "ai" app
     * ai_app_exec 里会启动 per-call ai_session（比全局 hook 更精准）*/
    app_reg_t *reg = dp_sleep_app_reg();
    if (reg) {
        app_desc_t ai_desc = {
            .name = "ai",
            .help = "启动 AI 对话 session（纯媒体桥接：抓流/转码/注入/转人工）",
            .fn   = ai_app_exec,
            .ud   = &g_ai,
        };
        int arc = app_register(reg, &ai_desc);
        llog(arc == NSBC_OK ? LOG_INFO : LOG_WARN, "ai",
             "app_register(ai) rc=%d", arc);
    } else {
        llog(LOG_WARN, "ai", "dp_sleep_app_reg() null — ai dp app 未注册");
    }

    /* P0-1: 注册 call_destroy hook —— 呼叫结束时主动关 per-call session 线程
     * （避免线程空转直到进程退出）*/
    int hrc = b2b_call_destroy_add(ai_on_call_destroy, &g_ai);
    llog(hrc == NSBC_OK ? LOG_INFO : LOG_WARN, "ai",
         "b2b_call_destroy_add rc=%d", hrc);

    /* register pass-through public API function pointers
     *
     * 为什么需要：ai_dialog 编译成 mod_ai_dialog.so，主二进制链接器看不到 .so 里的符号。
     * 主二进制的 ai_backend_registry.c 里有 wrapper ai_set_producer/ai_get_rx_audio/
     * ai_send_tx_audio/ai_request_transfer，内部通过这里注册的函数指针转发。 */
    {
        ai_passthrough_fns_t fns = {
            .set_producer     = ai_set_producer,
            .get_rx_audio     = ai_get_rx_audio,
            .send_tx_audio    = ai_send_tx_audio,
            .request_transfer = ai_request_transfer,
        };
        int prc = ai_passthrough_register_fns(&fns);
        llog(prc == NSBC_OK ? LOG_INFO : LOG_WARN, "ai",
             "passthrough_register rc=%d", prc);
    }

    return NSBC_OK;
}

static void ai_fini(struct nsbc_ctx *ctx)
{
    (void)ctx;
    llog(LOG_INFO, "ai", "ai_dialog fini");

    /* unregister passthrough fns first to avoid late calls during fini*/
    ai_passthrough_unregister();

    /* P0-1: 卸载 call_destroy hook */
    b2b_call_destroy_rm(ai_on_call_destroy, &g_ai);

    if (g_ai.bridge_hook && g_ai.bridge) {
        bridge_remove_rec_feed_hook(g_ai.bridge, g_ai.bridge_hook);
        g_ai.bridge_hook = NULL;
    }

    /* 先持锁标记所有 session 退出，再在锁外 join：
     * 线程退出路径会调 ai_set_producer() 反向获取 g_ai.lock，原实现持锁
     * pthread_join 在存在活跃 AI 会话时必然死锁；且 join 成功后线程已自行
     * 从链表摘除并 free(session)，原实现随后再 free 一次是双重释放。 */
    pthread_mutex_lock(&g_ai.lock);
    size_t nsess = 0;
    for (ai_session_t *s = g_ai.head; s; s = s->next) {
        s->running = 0;
        ++nsess;
    }
    pthread_t *ths = nsess ? (pthread_t *)calloc(nsess, sizeof(*ths)) : NULL;
    size_t nth = 0;
    if (ths || nsess == 0) {
        for (ai_session_t *s = g_ai.head; s; s = s->next)
            if (s->th)          /* 0 = 从未成功创建线程（calloc 初值） */
                ths[nth++] = s->th;
    }
    pthread_mutex_unlock(&g_ai.lock);

    if (ths || nsess == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 10;
        for (size_t i = 0; i < nth; ++i) {
            if (pthread_timedjoin_np(ths[i], NULL, &ts) != 0)
                llog(LOG_WARN, "ai", "fini join timeout — thread left for process exit");
        }
        free(ths);

        pthread_mutex_lock(&g_ai.lock);
        ai_session_t *s = g_ai.head;
        while (s) {
            ai_session_t *nx = s->next;
            if (s->th) {
                /* 线程超时未退出：不释放，避免 UAF（进程即将退出） */
                s = nx;
                continue;
            }
            /* 线程从未启动的 session 也可能已被媒体线程建过转码器，需一并释放 */
            if (s->rx_tc) { transcode_close(s->rx_tc); s->rx_tc = NULL; }
            if (s->tx_tc) { transcode_close(s->tx_tc); s->tx_tc = NULL; }
            if (s->tx_pcm16_enc) {
                const codec_ops_t *ops = codec_registry_find(
                    s->in_codec != CODEC_NONE ? s->in_codec : CODEC_PCMA);
                if (ops && ops->fini) ops->fini(s->tx_pcm16_enc);
                s->tx_pcm16_enc = NULL;
            }
            for (int i = 0; i < 2; i++) {
                ai_dsp_free(&s->dsp[i]);
            }
            pthread_mutex_destroy(&s->lock);
            free(s);
            s = nx;
        }
        g_ai.head = NULL;
        for (int i = 0; i < AI_STORE_BUCKETS; ++i)
            g_ai.htab[i] = NULL;
        __atomic_store_n(&g_ai.hcount, 0, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&g_ai.lock);
    } else {
        llog(LOG_WARN, "ai", "fini: calloc failed, %zu session(s) left for process exit",
             nsess);
    }
    pthread_mutex_destroy(&g_ai.lock);

    memset(&g_ai, 0, sizeof(g_ai));
}

/* deps：signaling 必须先 init（桥接层） */
static const char *ai_deps[] = { "signaling", NULL };

NSBC_MOD_CAT(ai_dialog, 1, MOD_L_FINAL, 0, MOD_CAT_AI, ai_deps, ai_init, NULL, NULL, ai_fini);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
