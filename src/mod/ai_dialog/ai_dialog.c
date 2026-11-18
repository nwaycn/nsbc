/* REQ-AI-001：AI 对话模块（可选加载，DYNAMIC 组）
 *
 * 纯媒体桥接层（passthrough）：
 *   [1] 媒体抓流    —— bridge_add_rec_feed_hook 挂 B 腿原始 PCMU/G.711 帧
 *                      → per-call ring buffer (256KB)
 *   [2] 跨 codec 转码 —— 内部统一 PCMU；in_codec 由首帧 RX 动态确定，
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
#include "media_codec.h"   /* 统一 codec 层：transcode_open/process 跨 codec 转码（PCMA/PCMU/G729/G722/...） */
#include "b2bua.h"        /* P0-1: b2b_call_destroy_add + REQ-AI-TRANSFER-001: b2b_transfer_call */
#include "callctrl.h"     /* REQ-AI-TRANSFER-001: CCF_XFER_BLIND */
#include "dp_sleep.h"     /* P0-2: dp_sleep_app_reg() */
#include "app_registry.h" /* P0-2: app_register / app_desc_t */
#include "app_ctx.h"      /* struct nsbc_ctx 完整定义（ctx->cfg）*/

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

#define AI_RING_CAP (256 * 1024)   /* 256KB 足够 ~32s @ 8kHz PCMU */

typedef struct ai_session {
    void     *call_key;
    int       b_leg;
    int       pt_in;                /* 抓进来的 PT（通常 0=PCMU） */
    /* ★ 统一 codec 层：内部统一 PCMU（ASR/TTS 基准）。in_codec 由首帧 RX 动态确定
     * （默认 CODEC_PCMU）；rx_tc/tx_tc 是跨 codec 转码会话，codec 变化时重建。 */
    codec_id_t     in_codec;        /* 主叫实际 codec（CODEC_PCMA/G729/...）*/
    transcode_sess_t *rx_tc;        /* in_codec → PCMU（收流转码）*/
    transcode_sess_t *tx_tc;        /* PCMU → in_codec（发流转码，format=0 用）*/
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

    /* REQ-AI-TRANSFER-001：外部请求 AI 转人工座席（blind transfer）。
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
        s->in_codec = CODEC_PCMU;  /* 默认 PCMU；首帧 RX 到达后动态修正为实际 codec */
        s->ssrc = 0x4E534243 ^ ((uintptr_t)call_key & 0xFFFFFFFFu);
        s->seq = (uint16_t)(rand() & 0xFFFF);
        s->ts = 0;
        s->opened_at = time(NULL);
        pthread_mutex_init(&s->lock, NULL);
        st->head = s;
        llog(LOG_INFO, "ai", "session opened call=%p pt=%d via=%s",
             call_key, pt_in, src);
        s->running = 1;
        int prc = pthread_create(&s->th, NULL, ai_session_thread_stub, s);
        if (prc != 0) {
            llog(LOG_ERR, "ai", "session opened but pthread_create failed rc=%d (EAGAIN=%d)! thread NOT started", prc, 11);
            s->running = 0;
        } else {
            llog(LOG_INFO, "ai", "★pthread_create OK thread started call=%p tid=%lu", call_key, (unsigned long)s->th);
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
                llog(LOG_INFO, "ai", "★reuse pthread_create OK call=%p tid=%lu", call_key, (unsigned long)s->th);
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
    static __thread unsigned g_rx = 0;
    llog(LOG_INFO, "ai", "★AF g_rx=%u call=%p pt=%d leg=%d st=%p en=%d",
         ++g_rx, call_key, pt, leg, (void*)st, st->cfg.enabled);

    /* ★ 2026-11-17 修复：两边都抓（leg=0 主叫 + leg=1 被叫）。
     * 之前只抓 leg=1（被叫），但 SIPp UAS 被叫不发 RTP → hook 零调用。
     * 用户可能在主叫也可能在被叫 —— 两边都进 ring buffer，thread 做 ASR 时拿到完整对话。
     * 注：后续可加 per-leg ring 分区避免 AI 听到自己 TTS 回复，但 demo 模式 ASR 返回空不影响。*/
    (void)leg;  /* 两边都抓 —— 不做 leg 过滤 */

    /* ★ 核心修复（2026-11-17）：全局 hook 只负责喂数据，不自动创建 session。
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

    /* ★ 统一 codec 层：静态 PT → codec_id。动态 PT（≥96，如 Opus）需 SDP rtpmap
     * 建立 per-session 映射（见 media_codec.h），暂不支持 → drop，不污染 ring。 */
    const codec_ops_t *ops = codec_registry_find_pt(pt, 8000);
    if (!ops) {
        llog(LOG_DEBUG, "ai", "rec_feed pt=%d 未注册/动态PT → drop call=%p", pt, call_key);
        return;
    }
    codec_id_t in_codec = ops->id;

    /* 内部统一 PCMU（ASR/TTS 基准）：in_codec != PCMU 时经 transcode 转码。 */
    const uint8_t *ring_data = (const uint8_t *)payload;
    size_t ring_len = len;
    uint8_t tc_buf[2048];

    /* 写 ring buffer（持 s->lock 保护并发写）；转码也在锁内完成 ——
     * rx_tc 是 per-session 会话，仅在媒体线程单线程访问，无并发。 */
    pthread_mutex_lock(&s->lock);
    if (in_codec != s->in_codec) {
        /* 首次确定 / codec 变化（re-INVITE）→ 重建 RX+TX 转码会话 + PCM16 encoder */
        if (s->rx_tc) { transcode_close(s->rx_tc); s->rx_tc = NULL; }
        if (s->tx_tc) { transcode_close(s->tx_tc); s->tx_tc = NULL; }
        if (s->tx_pcm16_enc) { codec_registry_find(in_codec)->fini(s->tx_pcm16_enc); s->tx_pcm16_enc = NULL; }
        s->in_codec = in_codec;
        if (in_codec != CODEC_PCMU) {
            transcode_sess_t *rx = NULL, *tx = NULL;
            if (transcode_open(in_codec, CODEC_PCMU, &rx) == NSBC_OK) s->rx_tc = rx;
            if (transcode_open(CODEC_PCMU, in_codec, &tx) == NSBC_OK) s->tx_tc = tx;
            /* format=1 优化：raw PCM16 直接 encode 到 in_codec，跳过中间 PCMU 有损编码 */
            const codec_ops_t *enc_ops = codec_registry_find(in_codec);
            if (enc_ops) {
                codec_sess_t *enc = NULL;
                if (enc_ops->encoder_init) enc_ops->encoder_init(NULL, &enc);
                else if (enc_ops->init)    enc_ops->init(NULL, &enc);
                s->tx_pcm16_enc = enc;
            }
            llog(LOG_INFO, "ai", "transcode init call=%p in=%d→PCMU rx=%p PCMU→in tx=%p pcm16_enc=%p",
                 call_key, (int)in_codec, (void*)s->rx_tc, (void*)s->tx_tc, (void*)s->tx_pcm16_enc);
        }
    }
    if (s->rx_tc) {
        size_t out_len = 0;
        int rc = transcode_process(s->rx_tc, payload, len, tc_buf, sizeof(tc_buf), &out_len);
        if (rc != NSBC_OK) {
            pthread_mutex_unlock(&s->lock);
            return;  /* AGAIN/错误：本帧跳过 */
        }
        ring_data = tc_buf;
        ring_len = out_len;
    }

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
/* REQ-AI-MEDIA-001：Pass-through 公共 API —— 让第三方 AI 直接读写音频流     */
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
 *   - RX（用户→AI）：PCMU 8kHz，每次 ai_rec_feed 入队的是完整 RTP 载荷（通常 160 字节 / 20ms）
 *   - TX（AI→用户）：PCMU 8kHz 原始字节（**调用方必须喂 PCMU，本模块不管 codec 转换**），
 *     bridge_send_raw 按 RTP 节奏注入 —— codec 转换已模块化嵌入（media_bridge 只管 RTP 封装 + sendto）
 *   - 缓冲区大小：256KB（≈32s @ 8kHz PCMU）
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
    s->producer = producer ? 1 : 0;
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_unlock(&g_ai.lock);
    llog(LOG_INFO, "ai", "ai_set_producer call=%p producer=%d", call_key, producer);
    return NSBC_OK;
}

/* 第三方 AI 拉取用户语音（PCMU 8kHz 原始字节，不重采样）
 *
 * call_key:  b2b_call_t*
 * out_buf:   调用方缓冲区（至少 out_max_len 字节）
 * out_max_len: 最多读多少字节（建议 160 的整数倍 = 20ms 的 RTP 载荷）
 * 返回：实际读取的字节数（0 = ring buffer 空 / 无 session / session 未启动）
 *
 * 语义：非阻塞。ring buffer 里有多少读多少，**消费语义**（读了就不保留）。
 * 建议调用方以 20ms（160 字节）节奏调，和 ai_rec_feed 入队节奏对齐。 */
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
 * data:      PCMU 8kHz 原始字节（内部统一 PCMU；本模块按协商 codec 经 transcode 转码）
 * len:       字节数
 * 返回：NSBC_OK / NSBC_NOENT / NSBC_INVAL / NSBC_AGAIN
 *
 * 语义：非阻塞。内部调 bridge_send_raw 按 RTP 节奏注入 A 腿。
 * 建议 20ms（160 字节 PCMU）节奏喂，和媒体桥接节奏对齐。
 * relay 不存在 / 还没 latch（-ENODEV）时返回 NSBC_AGAIN，调用方决定 retry 还是 drop。
 *
 * 设计决定：ai_dialog 内部统一 PCMU；跨 codec 转码经统一 codec 层
 * transcode_open/process（decode→encode 两段流水线，支持 PCMA/G729/G722/Opus 等）。
 * 调用方（外部 AI agent / 内建 demo）只需喂 PCMU，无需感知对端 codec。 */
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
    uint8_t   pt       = 0;  /* 默认 PCMU */
    if (in_codec != CODEC_PCMU && in_codec != CODEC_NONE) {
        const codec_ops_t *cops = codec_registry_find(in_codec);
        if (cops && cops->default_pt >= 0) pt = (uint8_t)cops->default_pt;
    }
    transcode_sess_t *tx_tc       = s->tx_tc;
    codec_sess_t     *tx_pcm16_enc = s->tx_pcm16_enc;
    uint32_t  ssrc     = s->ssrc;
    uint16_t *seq_ptr  = &s->seq;
    uint32_t *ts_ptr   = &s->ts;
    pthread_mutex_unlock(&s->lock);

    /* ★ format=1（PCM16 raw）→ 一次 encode 到 in_codec（无中间 PCMU 有损）
     * format=0（PCMU）→ 走 transcode PCMU→in_codec（旧路径，兼容） */
    const uint8_t *tx_data = data;
    size_t tx_len = len;
    uint8_t tc_buf[2048];

    if (format == 1) {
        /* --- format=1：PCM16 raw → in_codec（一次 encode）---
         * Go agent NWAYTTS 合成的是 8kHz 16bit mono PCM16_LE，
         * 我们跳过 Go 侧 PCM16→PCMU（8bit 有损编码），直接在 C 侧
         * 用 G.711 encoder.encode() 一次编码到 in_codec。
         * 对比原来两次编码（Go PCM16→PCMU + C PCMU→PCMA），
         * 现在只编码 1 次 = 和用户说话链路同保真度。 */
        const codec_ops_t *enc_ops = codec_registry_find(
            in_codec != CODEC_NONE ? in_codec : CODEC_PCMU);
        if (!enc_ops || !enc_ops->encode) return NSBC_NOENT;

        /* 懒创建 encoder sess（欢迎语阶段 in_codec 可能还是默认 CODEC_PCMU）*/
        codec_sess_t *enc = tx_pcm16_enc;
        if (!enc) {
            if (enc_ops->encoder_init) enc_ops->encoder_init(NULL, &enc);
            else if (enc_ops->init)    enc_ops->init(NULL, &enc);
            /* 赋值回 session（下次就懒不到了）*/
            pthread_mutex_lock(&s->lock);
            if (!s->tx_pcm16_enc) s->tx_pcm16_enc = enc;
            pthread_mutex_unlock(&s->lock);
        }

        /* 按 codec ptime_ms 分帧 encode — G.711 ptime=20ms,
         * 即 160 samples PCM16 = 320 bytes → encode 后 80 bytes PCMA */
        int ptime_ms = enc_ops->ptime_ms > 0 ? enc_ops->ptime_ms : 20;
        size_t samples_per_frame = (size_t)ptime_ms * 8; /* 8kHz → samples */
        size_t bytes_per_frame   = samples_per_frame * 2; /* 16bit → bytes */

        for (size_t off = 0; off < len; off += bytes_per_frame) {
            size_t frame_bytes = (off + bytes_per_frame > len)
                                 ? (len - off) : bytes_per_frame;
            media_frame_t mf = {
                .pcm      = (int16_t *)(data + off),
                .samples  = frame_bytes / 2,
                .rate     = 8000,
                .channels = 1,
            };
            size_t out_len = 0;
            int rc = enc_ops->encode(enc, &mf, tc_buf, sizeof(tc_buf), &out_len);
            if (rc != NSBC_OK || out_len == 0) continue;

            int n = bridge_send_raw(g_ai.bridge, call_key, 0,
                                    tc_buf, out_len, pt, ssrc, seq_ptr, ts_ptr, 160);
            if (n < 0 && n == -ENODEV) return NSBC_AGAIN;
        }
        return NSBC_OK;
    }

    /* --- format=0：PCMU → in_codec（transcode 一次或直通）---
     * 内部统一 PCMU，如果 in_codec == PCMU 则不转码直接注入 */
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
        const codec_ops_t *ops = codec_registry_find(s->in_codec != CODEC_NONE ? s->in_codec : CODEC_PCMU);
        if (ops && ops->fini) ops->fini(s->tx_pcm16_enc);
        s->tx_pcm16_enc = NULL;
    }

    llog(LOG_INFO, "ai", "thread exit call=%p", s->call_key);
    pthread_mutex_destroy(&s->lock);
    free(s);
    return NULL;
}

/* P0-1: b2b_call_destroy 回调 —— 主动关 per-call session 线程
 * 在 call_destroy 末尾（mem_obj_release 之前）调，必须尽快返回（禁阻塞）。
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

    /* 告诉线程该退出了 */
    victim->running = 0;

    /* 最多等 10s pthread_join —— transfer 触发 + 线程清理可能耗时，
     * 加长到 10s 避免 call_destroy 还没等完 AI 请求就释放 call 对象。
     * ★ pthread_timedjoin_np 的 timespec 是**绝对时间**（CLOCK_REALTIME），
     * 之前写成 {10,0} 等于 1970 年，立即超时 → call_destroy 提前释放 call
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
/* REQ-AI-TRANSFER-001：公共 API —— 请求 AI session 转人工座席           */
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
    pthread_mutex_lock(&s->lock);
    snprintf(s->transfer_dest, sizeof(s->transfer_dest), "%s", dest);
    s->transfer_pending = 1;
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_unlock(&g_ai.lock);
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
        s = ai_session_find_or_create(st, c->call_key, 0, "dp_ai");
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

    /* REQ-AI-MEDIA-001 / REQ-AI-TRANSFER-001: 注册 pass-through 公共 API 函数指针
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

    /* REQ-AI-MEDIA-001: 先 unregister passthrough fns（避免 fini 过程中还有外部 AI 调进来）*/
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
