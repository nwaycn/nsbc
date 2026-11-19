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
#include "vad_engine.h"   /* REQ-AI-VAD-001: vad_start_for_call / vad_stop_for_call + HPF API */
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

    /* external request to transfer AI session to human agent (blind transfer)
     * 设 transfer_dest + transfer_pending=1 → ai_session_thread 下次循环调
     * b2b_transfer_call 发 REFER + 设置 running=0 让线程退出。 */
    char      transfer_dest[256];
    int       transfer_pending;

    struct ai_session *next;
} ai_session_t;

typedef struct ai_store {
    pthread_mutex_t lock;
    ai_session_t   *head;
    bridge_mgr_t   *bridge;
    void           *bridge_hook;
    ai_cfg_t        cfg;
} ai_store_t;

static ai_store_t g_ai;
static cfg_t *g_cfg_ptr = NULL;

/* 前向声明：ai_rec_feed 里 pthread_create 要用；ai_session_thread_stub 在文件底部定义 */
static void *ai_session_thread_stub(void *arg);
/* P0-1 / P0-2: ai_init 里注册的两个函数，本文件底部定义 */
static void ai_on_call_destroy(void *call_key, int a_final, void *ud);
static app_rc_t ai_app_exec(app_ctx_t *c, const char *data, void *ud);

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

static ai_session_t *ai_session_find_or_create(ai_store_t *st, void *call_key,
                                               int pt_in, const char *src)
{
    pthread_mutex_lock(&st->lock);
    ai_session_t *s = NULL;
    for (s = st->head; s; s = s->next) {
        if (s->call_key == call_key) break;
    }
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

        st->head = s;
        llog(LOG_INFO, "ai", "session opened call=%p pt=%d via=%s HPF init",
             call_key, pt_in, src);

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
    pthread_mutex_lock(&st->lock);
    ai_session_t *s = NULL;
    for (s = st->head; s; s = s->next) {
        if (s->call_key == call_key) break;
    }
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
    const uint8_t *pcma_bytes = (const uint8_t *)payload;
    size_t pcma_len = len;

    /* per-frame PCM16 栈缓冲（20ms @ 8kHz PCMA → 160 samples → 320B PCM16，留 1024 够 60ms） */
    int16_t pcm16_buf[1024];
    const uint8_t *ring_data = NULL;
    size_t ring_len = 0;

    /* 写 ring buffer（持 s->lock 保护并发写）；转码+解码都在锁内完成 ——
     * rx_tc 是 per-session 会话，仅在媒体线程单线程访问，无并发。 */
    pthread_mutex_lock(&s->lock);
    if (in_codec != s->in_codec) {
        /* first resolution / codec change (re-INVITE) -> rebuild RX+TX transcode sessions + PCM16 encoder */
        if (s->rx_tc) { transcode_close(s->rx_tc); s->rx_tc = NULL; }
        if (s->tx_tc) { transcode_close(s->tx_tc); s->tx_tc = NULL; }
        if (s->tx_pcm16_enc) { codec_registry_find(in_codec)->fini(s->tx_pcm16_enc); s->tx_pcm16_enc = NULL; }
        s->in_codec = in_codec;
        if (in_codec != CODEC_PCMA) {
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
        }
    }
    if (s->rx_tc) {
        /* codec != PCMA → transcode 先转到 PCMA，再 decode */
        size_t out_len = 0;
        int rc = transcode_process(s->rx_tc, payload, len, tc_buf, sizeof(tc_buf), &out_len);
        if (rc != NSBC_OK) {
            pthread_mutex_unlock(&s->lock);
            return;  /* AGAIN/错误：本帧跳过 */
        }
        pcma_bytes = tc_buf;
        pcma_len = out_len;
    }

    /* PCMA -> PCM16 LE 8kHz mono — AI internal unified format */
    size_t pcm16_samples = g711_alaw_decode(pcma_bytes, pcma_len, pcm16_buf);

    /* P0: HPF strips DC offset and 50Hz mains hum
     * ring buffer previously had no HPF, so ASR fed slightly different audio than VAD detected
     * 现在两边都用同一份 HPF 系数，ASR 吃到干净音频 */
    if (pcm16_samples > 0 && leg >= 0 && leg < 2) {
        vad_hpf_process(&s->hpf[leg], pcm16_buf, pcm16_samples);
    }

    ring_data = (const uint8_t *)pcm16_buf;
    ring_len = pcm16_samples * 2;  /* int16_t → 2 bytes each */

    size_t space = (s->r_head >= s->r_tail)
        ? (AI_RING_CAP - s->r_head + s->r_tail - 1)
        : (s->r_tail - s->r_head - 1);
    if (space < ring_len) {
        /* ring 满：丢最旧（不阻塞媒体线程） */
        s->r_tail = (s->r_tail + (ring_len - space) + 1) % AI_RING_CAP;
        /* 然后算正确新 space */
        space = AI_RING_CAP - 1 - ((s->r_head >= s->r_tail)
              ? (s->r_head - s->r_tail)
              : (s->r_tail - s->r_head));
    }
    if (space >= ring_len) {
        for (size_t i = 0; i < ring_len; ++i) {
            s->ring[s->r_head++] = ring_data[i];
            if (s->r_head >= AI_RING_CAP) s->r_head = 0;
        }
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
    ai_session_t *s = NULL;
    for (s = g_ai.head; s; s = s->next) {
        if (s->call_key == call_key) break;
    }
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
        int rc = vad_start_for_call(call_key, 2 /* leg_mask=A+B */, 0 /* 默认阈值 */);
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
    ai_session_t *s = NULL;
    for (s = g_ai.head; s; s = s->next) {
        if (s->call_key == call_key) break;
    }
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
    ai_session_t *s = NULL;
    for (s = g_ai.head; s; s = s->next) {
        if (s->call_key == call_key) break;
    }
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

        for (size_t off = 0; off < len; off += bytes_per_frame) {
            size_t frame_bytes = (off + bytes_per_frame > len)
                                 ? (len - off) : bytes_per_frame;
            media_frame_t mf = {
                .pcm      = (int16_t *)(data + off),
                .samples  = frame_bytes / 2,
                .rate     = codec_rate,   /*  动态采样率，不再硬编码 8000 */
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
    ai_session_t **pp = &st->head;
    while (*pp && *pp != s) pp = &(*pp)->next;
    if (*pp) *pp = s->next;
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_unlock(&st->lock);

    if (s->rx_tc) { transcode_close(s->rx_tc); s->rx_tc = NULL; }
    if (s->tx_tc) { transcode_close(s->tx_tc); s->tx_tc = NULL; }
    if (s->tx_pcm16_enc) {
        const codec_ops_t *ops = codec_registry_find(s->in_codec != CODEC_NONE ? s->in_codec : CODEC_PCMA);
        if (ops && ops->fini) ops->fini(s->tx_pcm16_enc);
        s->tx_pcm16_enc = NULL;
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
    for (ai_session_t *s = st->head; s; s = s->next) {
        if (s->call_key == call_key) { victim = s; break; }
    }
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

    pthread_mutex_lock(&g_ai.lock);
    ai_session_t *s = NULL;
    for (s = g_ai.head; s; s = s->next) {
        if (s->call_key == call_key) break;
    }
    if (!s) {
        pthread_mutex_unlock(&g_ai.lock);
        llog(LOG_WARN, "ai", "ai_request_transfer: no session for call=%p", call_key);
        return NSBC_NOENT;
    }

    /* dest 格式兜底：如果只给了分机号（如 "1001"），补成 SIP URI。
     * 含 "sip:" 前缀或 "@" 视为已完整。domain 优先用 call 的 orig_host（b2b_call 里有），
     * 拿不到就用默认 "nsbc.local"。 */
    char full_dest[300];
    if (strstr(dest, "sip:") || strchr(dest, '@')) {
        snprintf(full_dest, sizeof(full_dest), "%s", dest);
    } else {
        snprintf(full_dest, sizeof(full_dest), "sip:%s@nsbc.local", dest);
    }

    pthread_mutex_lock(&s->lock);
    snprintf(s->transfer_dest, sizeof(s->transfer_dest), "%s", full_dest);
    s->transfer_pending = 1;
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_unlock(&g_ai.lock);
    llog(LOG_INFO, "ai", "ai_request_transfer call=%p dest=%s", call_key, full_dest);
    llog(LOG_INFO, "ai", "ai_request_transfer call=%p dest=%s (pending)", call_key, dest);
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
    ai_session_t *s = NULL;
    for (s = st->head; s; s = s->next) {
        if (s->call_key == c->call_key) break;
    }
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

    pthread_mutex_lock(&g_ai.lock);
    ai_session_t *s = g_ai.head;
    while (s) {
        ai_session_t *nx = s->next;
        if (s->running) {
            s->running = 0;
            pthread_join(s->th, NULL);
        }
        pthread_mutex_destroy(&s->lock);
        free(s);
        s = nx;
    }
    g_ai.head = NULL;
    pthread_mutex_unlock(&g_ai.lock);
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
