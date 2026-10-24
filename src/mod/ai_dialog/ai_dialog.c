/* REQ-AI-001：AI 对话模块（可选加载，DYNAMIC 组）
 *
 * 三阶段管道：
 *   [1] 媒体抓流    —— bridge_add_rec_feed_hook 挂 B 腿原始 PCMU/G.711 帧
 *                      → per-call ring buffer (64KB)
 *   [2] ASR          —— 凑 2s 音频块落临时文件（.wav）
 *                      → curl multipart POST OpenAI Whisper → transcript text
 *   [3] LLM + TTS    —— transcript → OpenAI Chat / Ollama (可选) → reply text
 *                      → GET ElevenLabs TTS → PCM16 raw stream
 *                      → g711_pcm16_to_ulaw → PCMU
 *                      → bridge_send_raw 注入 A 腿
 *
 * 模块分层：
 *   MOD_L_WORKER_READY  —— 早于 signaling FINAL（桥接层已就绪）
 *   非 REQUIRED          —— DYNAMIC 组 dlopen
 *   deps = {"signaling"} —— 桥接层必须先 init
 *
 * 依赖编译开关：NSBC_WITH_CURL=1（-lcurl）
 * 无 cURL 时编译跳过走 mock 模式（见 ai_session_thread 里 #ifdef）
 *
 * nsbc.yaml 配置：
 *   ai_dialog:
 *     enabled: 1
 *     asr:
 *       endpoint: https://api.openai.com/v1/audio/transcriptions
 *       api_key: sk-...
 *       model: whisper-1
 *       language: zh         # 可空自动检测
 *     llm:
 *       endpoint: https://api.openai.com/v1/chat/completions
 *       api_key: sk-...
 *       model: gpt-4o-mini
 *       system_prompt: "你是一个友好的电话客服..."
 *       # 可选本地 ollama: endpoint=http://localhost:11434/api/chat, api_key=空
 *     tts:
 *       endpoint: https://api.elevenlabs.io/v1/text-to-speech
 *       api_key: ...
 *       voice_id: 21m00Tcm4TlvDq8ikWAM   # ElevenLabs demo voice
 *     # 音频参数（默认 PCMU 8kHz 20ms 帧）
 *     chunk_ms: 2000        # 每 2s 做一次 ASR
 *     target_pt: 0          # 0=PCMU, 8=PCMA
 */

#include "module.h"
#include "cfg.h"
#include "log.h"
#include "media_bridge.h"
#include "g711_codec.h"
#include "b2bua.h"        /* P0-1: b2b_call_destroy_add */
#include "dp_sleep.h"     /* P0-2: dp_sleep_app_reg() */
#include "app_registry.h" /* P0-2: app_register / app_desc_t */
#include "app_ctx.h"      /* struct nsbc_ctx 完整定义（ctx->cfg）*/

#ifdef NSBC_WITH_CURL
#define HAVE_REAL_ASR_TTS 1
#include "ai_http.h"
#endif

#include "ai_backend_registry.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern bridge_mgr_t *sig_bridge_instance(void);

/* ================================================================== */
/* 配置结构体                                                          */
/* ================================================================== */

typedef struct ai_cfg {
    int enabled;
    char asr_endpoint[256];
    char asr_api_key[256];
    char asr_model[64];
    char asr_language[16];
    char llm_endpoint[256];
    char llm_api_key[256];
    char llm_model[64];
    char llm_system_prompt[512];
    int  llm_enabled;
    char tts_endpoint[256];
    char tts_api_key[256];
    char tts_voice_id[128];
    int  chunk_ms;
    int  target_pt;    /* 0=PCMU, 8=PCMA */
} ai_cfg_t;

/* ================================================================== */
/* per-call 会话                                                       */
/* ================================================================== */

#define AI_RING_CAP (256 * 1024)   /* 256KB 足够 ~32s @ 8kHz PCMU */
#define AI_MAX_CHUNKS 16           /* 最多累计 16 个 chunk（≈ 32s） */

typedef struct ai_session {
    void     *call_key;
    int       b_leg;
    int       pt_in;                /* 抓进来的 PT（通常 0=PCMU） */
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
    int       enabled;   /* 1 = 已被 dp "ai" app 显式启用；全局 hook 也能启 session 但 enabled=0 */

    /* ASR chunk 临时文件路径（每个 chunk 落一个 .pcm） */
    char      chunk_paths[AI_MAX_CHUNKS][64];
    int       n_chunks;
    int       chunk_bytes_target;  /* = chunk_ms * 8000 / 1000 字节（PCMU 8k） */

    int16_t  *pcm_buf;              /* 累计的 PCM16 缓冲（ASR 和 LLM 之后 TTS 用）*/
    size_t    pcm_samples;          /* 累计的 16bit 样本数（不是字节）*/
    size_t    pcm_cap;

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
        s->ssrc = 0x4E534243 ^ ((uintptr_t)call_key & 0xFFFFFFFFu);
        s->seq = (uint16_t)(rand() & 0xFFFF);
        s->ts = 0;
        s->chunk_bytes_target = st->cfg.chunk_ms * 8000 / 1000;
        s->opened_at = time(NULL);
        s->enabled = (strcmp(src, "dp_ai") == 0);  /* 只走 dp app 才显式启用 */
        pthread_mutex_init(&s->lock, NULL);
        st->head = s;
        llog(LOG_INFO, "ai", "session opened call=%p pt=%d via=%s enabled=%d chunk_ms=%d",
             call_key, pt_in, src, s->enabled, st->cfg.chunk_ms);
        s->running = 1;
        pthread_create(&s->th, NULL, ai_session_thread_stub, s);
    }
    pthread_mutex_unlock(&st->lock);
    return s;
}

/* ================================================================== */
/* 工具：从 nsbc.yaml 读字符串 / int（统一空串默认）                    */
/* ================================================================== */

static void cfg_str(const char *key, char *out, size_t outsz, const char *def)
{
    const char *v = NULL;
    if (cfg_get_str(g_cfg_ptr, key, &v, NULL) == NSBC_OK && v && v[0]) {
        snprintf(out, outsz, "%s", v);
    } else if (def) {
        snprintf(out, outsz, "%s", def);
    } else {
        out[0] = '\0';
    }
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

    /* 只抓 B 腿（被叫方语音），PCMU/PCMA */
    if (leg != 1) return;
    if (pt != 0 && pt != 8) return;

    /* P0-2: session 查找/创建统一走 helper（ai_app_exec 会提前创建；
     * 没走 dp app 的 fallback 全局 hook 路径也能自动建）*/
    ai_session_t *s = ai_session_find_or_create(st, call_key, pt, "hook");
    if (!s) return;

    /* 写 ring buffer（持 s->lock 保护并发写） */
    pthread_mutex_lock(&s->lock);
    size_t space = (s->r_head >= s->r_tail)
        ? (AI_RING_CAP - s->r_head + s->r_tail - 1)
        : (s->r_tail - s->r_head - 1);
    if (space < len) {
        /* ring 满：丢最旧（不阻塞媒体线程） */
        s->r_tail = (s->r_tail + (len - space) + 1) % AI_RING_CAP;
        /* 然后算正确新 space */
        space = AI_RING_CAP - 1 - ((s->r_head >= s->r_tail)
              ? (s->r_head - s->r_tail)
              : (s->r_tail - s->r_head));
    }
    if (space >= len) {
        for (size_t i = 0; i < len; ++i) {
            s->ring[s->r_head++] = ((const uint8_t *)payload)[i];
            if (s->r_head >= AI_RING_CAP) s->r_head = 0;
        }
    }
    pthread_mutex_unlock(&s->lock);
}

/* ================================================================== */
/* ASR → transcript（openai whisper multipart）                         */
/* ================================================================== */

static char *do_asr(ai_cfg_t *cfg, const char *pcm_path, size_t pcm_len)
{
    (void)cfg;
    (void)pcm_len;
    /* 把 PCMU 临时文件转成 16kHz WAV（Whisper 偏好）——P1 简化：
     * 直接 POST raw PCM16（8kHz mono）；或者落一个最小 .wav 头。
     * 这里选落 .wav 头：44字节 header + PCMU → PCM16 → 写文件
     * 更兼容 Whisper。 */
    char wav_path[256];
    snprintf(wav_path, sizeof(wav_path), "%s.wav", pcm_path);

    /* 写 .wav：44 字节 header + 8kHz mono PCMU → PCM16 */
    FILE *fin = fopen(pcm_path, "rb");
    FILE *fout = fopen(wav_path, "wb");
    if (!fin || !fout) {
        if (fin) fclose(fin);
        if (fout) fclose(fout);
        return NULL;
    }
    static const char wav_hdr[44] = {
        /* RIFF + WAVE fmt */
        'R','I','F','F', 0,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
        0x40,0x1F, 0,0, 0x80,0x3E, 0,0, 2,0, 16,0,
        'd','a','t','a', 0,0,0,0,
    };
    fwrite(wav_hdr, 44, 1, fout);

    uint8_t pcmu_buf[4096];
    int16_t pcm16_buf[4096];
    while (1) {
        size_t n = fread(pcmu_buf, 1, sizeof(pcmu_buf), fin);
        if (n == 0) break;
        g711_ulaw_to_pcm16(pcmu_buf, pcm16_buf, n);
        fwrite(pcm16_buf, 2, n, fout);
    }
    /* 回填 data chunk 长度：filesize - 44 */
    long sz = ftell(fout);
    int32_t data_len = (int32_t)(sz - 44);
    fseek(fout, 4, SEEK_SET);
    int32_t riff_len = data_len + 36;
    fwrite(&riff_len, 4, 1, fout);
    fseek(fout, 40, SEEK_SET);
    fwrite(&data_len, 4, 1, fout);
    fclose(fin);
    fclose(fout);

#ifdef HAVE_REAL_ASR_TTS
    /* Phase R-14: 通过 ai_backend_registry 调用 whisper_openai ASR
     * registry 签名是 (pcm_in, pcm_len, sample_rate, ...) —— 这里 PCMU 刚转成的 PCM16 就是 pcm_in
     * 原来的 PCMU→PCM16→WAV 转换挪到 whisper_openai_ops.asr wrapper 里了 */
    FILE *fr = fopen(wav_path, "rb");
    if (!fr) { unlink(wav_path); return NULL; }
    fseek(fr, 0, SEEK_END);
    long sz = ftell(fr);
    fseek(fr, 44, SEEK_SET);  /* 跳过 44 字节 WAV header，后面的就是 PCM16 raw */
    size_t pcm16_len = (sz > 44) ? (size_t)(sz - 44) : 0;
    if (pcm16_len == 0) { fclose(fr); unlink(wav_path); return NULL; }
    char *pcm16_buf = (char *)malloc(pcm16_len);
    if (!pcm16_buf) { fclose(fr); unlink(wav_path); return NULL; }
    fread(pcm16_buf, 1, pcm16_len, fr);
    fclose(fr);
    unlink(wav_path);

    int slot = -1;
    const ai_backend_ops_t *ops = ai_backend_registry_find_inc("whisper_openai", &slot);
    if (!ops || !ops->asr) {
        llog(LOG_WARN, "ai", "whisper_openai backend not registered");
        free(pcm16_buf);
        return NULL;
    }
    char *text = NULL;
    int rc = ops->asr(pcm16_buf, pcm16_len, 8000, &text, ops->ud);
    ai_backend_registry_dec(slot);
    free(pcm16_buf);
    if (rc == NSBC_OK && text) return text;
    return NULL;
#else
    /* mock：没 cURL —— 返回占位文本让后续 TTS 管道有数据 */
    unlink(wav_path);
    return strdup("(mock ASR) 我想查询账单");
#endif
}

/* ================================================================== */
/* LLM（chat completion）→ reply text                                   */
/* ================================================================== */

static char *do_llm(ai_cfg_t *cfg, const char *transcript)
{
    if (!cfg->llm_enabled) return strdup(transcript); /* 没配 LLM —— transcript 直接当回复 */

#ifdef HAVE_REAL_ASR_TTS
    /* Phase R-14: 通过 ai_backend_registry 调用 openai_chat LLM */
    int slot = -1;
    const ai_backend_ops_t *ops = ai_backend_registry_find_inc("openai_chat", &slot);
    if (!ops || !ops->llm) {
        llog(LOG_WARN, "ai", "openai_chat backend not registered");
        return strdup(transcript);
    }
    char *reply = NULL;
    int rc = ops->llm(transcript, &reply, ops->ud);
    ai_backend_registry_dec(slot);
    if (rc == NSBC_OK && reply) return reply;
    return strdup(transcript);
#else
    return strdup(transcript);
#endif
}

/* ================================================================== */
/* TTS → PCM16 raw → bridge_send_raw 注入                               */
/* ================================================================== */

#ifdef HAVE_REAL_ASR_TTS
/* 简化 JSON escape（只转义 ElevenLabs POST body 里 text 可能含的字符） */
static char *json_escape_simple(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    /* 最坏：每个 UTF-8 字节变 \uXXXX（不过 ElevenLabs 只需要 \"\\\b\f\n\r\t 8 种）*/
    char *out = (char *)malloc(n * 2 + 4);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  out[j++] = '\\'; out[j++] = '"';  break;
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
            case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
            case '\t': out[j++] = '\\'; out[j++] = 't';  break;
            case '\b': out[j++] = '\\'; out[j++] = 'b';  break;
            case '\f': out[j++] = '\\'; out[j++] = 'f';  break;
            default:   out[j++] = (char)c;                break;
        }
    }
    out[j] = '\0';
    return out;
}
#endif /* HAVE_REAL_ASR_TTS */

static void do_tts_and_inject(ai_store_t *st, ai_session_t *s, const char *text)
{
    (void)st;
    (void)s;
    (void)text;
#ifdef HAVE_REAL_ASR_TTS
    if (!text || !text[0]) return;

    /* Phase R-14: 通过 ai_backend_registry 调用 elevenlabs TTS
     * wrapper 返回 malloc 的 PCM16 raw —— 下面 PCM16→PCMU + bridge 注入逻辑保持不变 */
    int slot = -1;
    const ai_backend_ops_t *ops = ai_backend_registry_find_inc("elevenlabs", &slot);
    if (!ops || !ops->tts) {
        llog(LOG_WARN, "ai", "elevenlabs backend not registered");
        return;
    }
    char *pcm_out = NULL;
    size_t pcm_len = 0;
    int rc = ops->tts(text, 16000, &pcm_out, &pcm_len, ops->ud);
    ai_backend_registry_dec(slot);
    if (rc != NSBC_OK || !pcm_out || pcm_len == 0) {
        llog(LOG_WARN, "ai", "elevenlabs ops->tts rc=%d", rc);
        return;
    }
    llog(LOG_INFO, "ai", "TTS %zu bytes PCM16", pcm_len);

    /* PCM16 raw → PCMU (bridge 注入用 leg=0 的 codec) */
    size_t n_samples = pcm_len / 2;
    const int16_t *pcm = (const int16_t *)pcm_out;
    uint8_t *pcmu = (uint8_t *)malloc(n_samples);
    if (!pcmu) { free(pcm_out); return; }
    g711_pcm16_to_ulaw(pcm, pcmu, n_samples);

    /* 20ms 帧注入：160 字节 PCMU × 8000Hz × 0.02s = 320 样本
     * ENODEV 单次 retry：relay 刚创建时 peer 地址可能还没 comedia 学到
     * （ICE / DTLS-SRTP setup 后才 latch），delay 50ms 再试一次；再失败丢帧。*/
    size_t i = 0;
    int enodev_seen = 0;
    while (i + 160 <= n_samples) {
        int n = bridge_send_raw(st->bridge, s->call_key, 0, /* 朝 A 腿注入 */
                                pcmu + i, 160,
                                st->cfg.target_pt,
                                s->ssrc, &s->seq, &s->ts, 160);
        if (n < 0) {
            if (n == -ENODEV && !enodev_seen) {
                /* 首次 ENODEV：一次 retry（50ms 媒体管线还在 settle）*/
                enodev_seen = 1;
                llog(LOG_INFO, "ai", "bridge_send_raw -ENODEV → retry after 50ms");
                nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 50000000}, NULL);
                continue; /* 同一帧重试 */
            }
            if (n == -ENODEV && enodev_seen) {
                /* retry 仍失败：整条 TTS 注入链路终止，后续帧也不会成功 */
                llog(LOG_WARN, "ai", "bridge_send_raw -ENODEV retry still fail → drop");
                break;
            }
            llog(LOG_WARN, "ai", "bridge_send_raw rc=%d", n);
        }
        enodev_seen = 0;
        i += 160;
    }

    free(pcmu);
    free(pcm_out);
#else
    /* mock：构造 3 帧静音 PCMU 注入 —— 证明 bridge_send_raw 通 */
    static uint8_t silence[160];
    memset(silence, 0x7F, sizeof(silence));
    for (int j = 0; j < 3; ++j) {
        bridge_send_raw(st->bridge, s->call_key, 0, silence, sizeof(silence),
                        st->cfg.target_pt, s->ssrc, &s->seq, &s->ts, 160);
    }
#endif
}

/* ================================================================== */
/* Phase R-14: 三个 ai_backend_ops wrapper —— 注册到 ai_backend_registry  */
/* 每个 wrapper 内部复用 ai_dialog 已有的 HTTP + JSON parse 逻辑          */
/* ================================================================== */

#ifdef HAVE_REAL_ASR_TTS

/* ---- whisper_openai: asr ----
 * pcm_in 是 int16 PCM16 raw 字节流（sample_rate=8000，PCMU 转码产物）
 * wrapper 内部：pcm_in → 写 44 字节 WAV header + pcm_in → temp .wav
 * → ai_http_post_multipart → JSON parse .text → *text_out */
static int whisper_openai_asr(const char *pcm_in, size_t pcm_len,
                              int sample_rate, char **text_out, void *ud)
{
    ai_cfg_t *cfg = (ai_cfg_t *)ud;
    if (!cfg || !pcm_in || pcm_len == 0 || !text_out) return NSBC_INVAL;

    /* 1. mkstemp 临时 WAV 文件 */
    char wav_path[] = "/tmp/nsbc-asr-XXXXXX.wav";
    int fd = mkstemps(wav_path, 4); /* 后缀 ".wav" 共 4 字符 */
    if (fd < 0) {
        llog(LOG_WARN, "ai", "whisper_openai_asr: mkstemps failed");
        return NSBC_ERR;
    }
    FILE *fout = fdopen(fd, "wb");
    if (!fout) { close(fd); return NSBC_ERR; }

    /* 2. 写 44 字节 WAV header（PCM16, mono, sample_rate 由调用方传）
     *    fmt chunk 布局：
     *      AudioFormat(2) | NumChannels(2) | SampleRate(4) | ByteRate(4) | BlockAlign(2) | BitsPerSample(2) */
    int32_t sr32 = sample_rate;
    int16_t bits  = 16;
    int16_t chs   = 1;
    int32_t byte_rate = sample_rate * 2;
    int16_t block_align = 2;
    int32_t data_len = (int32_t)pcm_len;
    int32_t riff_len = data_len + 36;

    fwrite("RIFF", 1, 4, fout);
    fwrite(&riff_len, 4, 1, fout);
    fwrite("WAVE", 1, 4, fout);
    fwrite("fmt ", 1, 4, fout);
    int32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, fout);
    int16_t fmt_tag = 1; /* PCM */
    fwrite(&fmt_tag, 2, 1, fout);
    fwrite(&chs, 2, 1, fout);
    fwrite(&sr32, 4, 1, fout);
    fwrite(&byte_rate, 4, 1, fout);
    fwrite(&block_align, 2, 1, fout);
    fwrite(&bits, 2, 1, fout);
    fwrite("data", 1, 4, fout);
    fwrite(&data_len, 4, 1, fout);

    /* 3. 写 PCM16 数据 + close */
    fwrite(pcm_in, 1, pcm_len, fout);
    fclose(fout);

    /* 4. HTTP multipart POST */
    ai_http_resp_t resp = {0};
    char model_json[256];
    snprintf(model_json, sizeof(model_json),
             "{\"model\":\"%s\"}", cfg->asr_model[0] ? cfg->asr_model : "whisper-1");
    int rc = ai_http_post_multipart(cfg->asr_endpoint, cfg->asr_api_key,
                                    wav_path, model_json, &resp);
    unlink(wav_path);

    if (rc != NSBC_OK) {
        llog(LOG_WARN, "ai", "whisper_openai_asr rc=%d HTTP %ld", rc, resp.http_code);
        ai_http_resp_free(&resp);
        return rc;
    }

    /* 5. JSON parse .text 字段（和 do_asr 里完全一样的解析逻辑）*/
    const char *p = strstr(resp.memory, "\"text\"");
    if (!p) { ai_http_resp_free(&resp); return NSBC_ERR; }
    const char *q = strchr(p + 7, '"');
    if (!q) { ai_http_resp_free(&resp); return NSBC_ERR; }
    char *text = (char *)malloc((q - p - 7) + 1);
    if (!text) { ai_http_resp_free(&resp); return NSBC_ERR; }
    size_t ti = 0;
    for (const char *c = p + 7; c < q; ++c) {
        if (*c == '\\' && c + 1 < q && (c[1] == '"' || c[1] == '\\')) {
            text[ti++] = c[1];
            ++c;
        } else {
            text[ti++] = *c;
        }
    }
    text[ti] = '\0';
    ai_http_resp_free(&resp);
    llog(LOG_INFO, "ai", "whisper_openai transcript: %s", text);
    *text_out = text;
    return NSBC_OK;
}

static const ai_backend_ops_t whisper_openai_ops = {
    .name = "whisper_openai",
    .asr  = whisper_openai_asr,
    .ud   = NULL,  /* init 时填 &g_ai.cfg */
};

/* ---- openai_chat: llm ---- */
static int openai_chat_llm(const char *prompt, char **text_out, void *ud)
{
    ai_cfg_t *cfg = (ai_cfg_t *)ud;
    if (!cfg || !prompt || !text_out) return NSBC_INVAL;
    if (!cfg->llm_enabled) return NSBC_INVAL;

    /* body 构造 + HTTP POST（和 do_llm 里完全一样的逻辑）*/
    char body[8192];
    snprintf(body, sizeof(body),
        "{\"model\":\"%s\","
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}"
        "]}",
        cfg->llm_model[0] ? cfg->llm_model : "gpt-4o-mini",
        cfg->llm_system_prompt[0] ? cfg->llm_system_prompt : "你是一个友好的电话客服，简短回答。",
        prompt);

    ai_http_resp_t resp = {0};
    int rc = ai_http_post_json(cfg->llm_endpoint, cfg->llm_api_key,
                               body, strlen(body), &resp);
    if (rc != NSBC_OK) {
        llog(LOG_WARN, "ai", "openai_chat_llm rc=%d HTTP %ld", rc, resp.http_code);
        ai_http_resp_free(&resp);
        return rc;
    }

    /* JSON parse choices[0].message.content（和 do_llm 里完全一样）*/
    const char *p = strstr(resp.memory, "\"content\"");
    if (!p) { ai_http_resp_free(&resp); return NSBC_ERR; }
    const char *q = strchr(p + 10, '"');
    if (!q) { ai_http_resp_free(&resp); return NSBC_ERR; }
    char *reply = (char *)malloc((q - p - 10) + 1);
    if (!reply) { ai_http_resp_free(&resp); return NSBC_ERR; }
    size_t ti = 0;
    for (const char *c = p + 10; c < q; ++c) {
        if (*c == '\\' && c + 1 < q && (c[1] == '"' || c[1] == '\\' || c[1] == 'n')) {
            reply[ti++] = (c[1] == 'n') ? '\n' : c[1];
            ++c;
        } else {
            reply[ti++] = *c;
        }
    }
    reply[ti] = '\0';
    ai_http_resp_free(&resp);
    llog(LOG_INFO, "ai", "openai_chat reply: %s", reply);
    *text_out = reply;
    return NSBC_OK;
}

static const ai_backend_ops_t openai_chat_ops = {
    .name = "openai_chat",
    .llm  = openai_chat_llm,
    .ud   = NULL,
};

/* ---- elevenlabs: tts ---- */
static int elevenlabs_tts(const char *text, int sample_rate,
                          char **pcm_out, size_t *pcm_len, void *ud)
{
    ai_cfg_t *cfg = (ai_cfg_t *)ud;
    if (!cfg || !text || !text[0] || !pcm_out || !pcm_len) return NSBC_INVAL;

    /* 和 do_tts_and_inject 里完全一样的 ElevenLabs POST 逻辑 */
    char url[512];
    snprintf(url, sizeof(url), "%s/%s?output_format=pcm_16000",
             cfg->tts_endpoint, cfg->tts_voice_id);

    char *escaped = json_escape_simple(text);
    if (!escaped) {
        llog(LOG_WARN, "ai", "elevenlabs_tts json_escape failed");
        return NSBC_ERR;
    }
    char body[4096];
    snprintf(body, sizeof(body),
             "{\"text\":\"%s\",\"model_id\":\"eleven_monolingual_v1\"}", escaped);
    free(escaped);

    ai_http_resp_t resp = {0};
    int rc = ai_http_post_json(url, cfg->tts_api_key, body, strlen(body), &resp);
    if (rc != NSBC_OK || resp.size == 0) {
        llog(LOG_WARN, "ai", "elevenlabs_tts rc=%d HTTP %ld size=%zu",
             rc, resp.http_code, resp.size);
        ai_http_resp_free(&resp);
        return NSBC_ERR;
    }

    /* resp.memory 就是 PCM16 raw —— malloc 拷贝给调用方 */
    *pcm_out = (char *)malloc(resp.size);
    if (!*pcm_out) { ai_http_resp_free(&resp); return NSBC_ERR; }
    memcpy(*pcm_out, resp.memory, resp.size);
    *pcm_len = resp.size;
    ai_http_resp_free(&resp);
    llog(LOG_INFO, "ai", "elevenlabs_tts %zu bytes PCM16", resp.size);
    return NSBC_OK;
}

static const ai_backend_ops_t elevenlabs_ops = {
    .name = "elevenlabs",
    .tts  = elevenlabs_tts,
    .ud   = NULL,
};

#endif /* HAVE_REAL_ASR_TTS */

/* ================================================================== */
/* per-call 处理线程 stub（pthread_create 只收一个 (void*)）              */
/* ================================================================== */

static void *ai_session_thread_stub(void *arg)
{
    ai_session_t *s = (ai_session_t *)arg;
    ai_store_t *st = &g_ai;
    llog(LOG_INFO, "ai", "thread start call=%p target_pt=%d chunk_bytes=%d",
         s->call_key, st->cfg.target_pt, s->chunk_bytes_target);

    uint8_t pcmu_chunk[4096]; /* 4KB ≈ 512ms @ 8k */
    FILE *chunk_fp = NULL;
    int chunk_written = 0;

    while (s->running) {
        /* 如果还没被 dp "ai" app 显式启用（enabled=0），只等 enable 或 exit；
         * ring buffer 继续被全局 hook 写着，但 thread 空转不做 ASR/TTS */
        if (!s->enabled) {
            nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 20000000}, NULL);
            continue;
        }

        /* 从 ring buffer 凑 chunk_bytes_target 字节 */
        int did_something = 0;
        pthread_mutex_lock(&s->lock);
        while (chunk_written < s->chunk_bytes_target) {
            size_t avail = (s->r_head >= s->r_tail)
                ? (s->r_head - s->r_tail)
                : (AI_RING_CAP - s->r_tail + s->r_head);
            if (avail == 0) break;
            size_t take = avail < sizeof(pcmu_chunk) ? avail : sizeof(pcmu_chunk);
            size_t i = 0;
            while (i < take && s->r_tail < AI_RING_CAP) {
                pcmu_chunk[i++] = s->ring[s->r_tail++];
                if (s->r_tail >= AI_RING_CAP) s->r_tail = 0;
            }
            while (i < take) { pcmu_chunk[i++] = s->ring[s->r_tail++]; }

            if (!chunk_fp) {
                if (s->n_chunks >= AI_MAX_CHUNKS) {
                    /* 清太老的 chunk 文件 */
                    for (int k = 0; k < s->n_chunks; ++k)
                        unlink(s->chunk_paths[k]);
                    s->n_chunks = 0;
                }
                snprintf(s->chunk_paths[s->n_chunks], sizeof(s->chunk_paths[0]),
                         "/tmp/ai_chunk_%p_%d.pcm", s->call_key, s->n_chunks);
                chunk_fp = fopen(s->chunk_paths[s->n_chunks], "wb");
                chunk_written = 0;
            }
            fwrite(pcmu_chunk, 1, take, chunk_fp);
            chunk_written += (int)take;
            did_something = 1;
        }
        pthread_mutex_unlock(&s->lock);

        if (!did_something) {
            nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 10000000}, NULL); /* 10ms */
            continue;
        }

        if (chunk_written >= s->chunk_bytes_target) {
            fclose(chunk_fp);
            chunk_fp = NULL;
            s->n_chunks++;

            /* === 三阶段管道 === */
            char *transcript = do_asr(&st->cfg, s->chunk_paths[s->n_chunks - 1], chunk_written);
            if (transcript && transcript[0]) {
                char *reply = do_llm(&st->cfg, transcript);
                if (reply && reply[0])
                    do_tts_and_inject(st, s, reply);
                free(reply);
            }
            free(transcript);
            chunk_written = 0;
        }
    }

    /* 线程退出清理 */
    pthread_mutex_lock(&st->lock);
    pthread_mutex_lock(&s->lock);
    /* 从链表摘除 */
    ai_session_t **pp = &st->head;
    while (*pp && *pp != s) pp = &(*pp)->next;
    if (*pp) *pp = s->next;
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_unlock(&st->lock);

    /* 清理 chunk 文件 */
    for (int k = 0; k < s->n_chunks; ++k) unlink(s->chunk_paths[k]);

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

    /* 最多等 1s pthread_join —— ai_session_thread_stub 里 nanosleep 10ms，
     * 最慢 1s 内会响应 running=0 */
    struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
    if (pthread_timedjoin_np(victim->th, NULL, &ts) != 0) {
        llog(LOG_WARN, "ai", "call_destroy join timeout — thread may still run (leak)");
    }
    /* 线程自己会 free session —— 如果 join 成功线程退出时已经 free 了；
     * 如果 timeout，我们在这里兜底 free（但要先从链表摘——可能线程也在 free，
     * 简单起见 timeout 时不 free，让进程退出时 fini 统一清）*/
}

/* P0-2: dp app "ai" 执行 —— 在 INVITE resolve 命中 dialplan ai app_chain 时调
 * 创建 per-call ai_session（早于 ai_rec_feed 全局 hook 创建的时间点）。
 * 立即返回 APP_RC_OK（per-call 线程异步处理媒体 → ASR → LLM → TTS 注入，
 * 不阻塞 dialplan 执行器）。
 * 如果全局 hook 已经先启了 session（enabled=0），这里置 enabled=1 →
 * thread 检测 enabled=1 后才真正启动 ASR/TTS 管道（之前只攒 ring buffer）。*/
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
    if (s) {
        if (!s->enabled) {
            s->enabled = 1;
            llog(LOG_INFO, "ai", "dp ai: existing session call=%p → enabled=1", c->call_key);
        }
    } else {
        pthread_mutex_unlock(&st->lock);
        s = ai_session_find_or_create(st, c->call_key, 0, "dp_ai");
        if (!s) {
            llog(LOG_WARN, "ai", "ai_app_exec: session create failed");
            return APP_RC_FAILED;
        }
        pthread_mutex_lock(&st->lock);
    }
    pthread_mutex_unlock(&st->lock);

    llog(LOG_INFO, "ai", "dp ai app fired call=%p target=%s enabled=%d",
         c->call_key, c->dp_target ? c->dp_target : "", s->enabled);
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

    /* 读配置（cfg_get_int 第3参数是 long *，第4参数是 cfg_layer_t * 或 NULL）*/
    g_ai.cfg.enabled   = 0;      /* 默认禁用 */
    g_ai.cfg.target_pt = 0;      /* 默认 PCMU */
    g_ai.cfg.chunk_ms  = 2000;   /* 默认 2s ASR chunk */
    g_ai.cfg.llm_enabled = 0;

    long tmp = 0;
    if (cfg_get_int(g_cfg_ptr, "ai_dialog.enabled",   &tmp, NULL) == 0) g_ai.cfg.enabled = (int)tmp;
    tmp = g_ai.cfg.target_pt;
    if (cfg_get_int(g_cfg_ptr, "ai_dialog.target_pt", &tmp, NULL) == 0) g_ai.cfg.target_pt = (int)tmp;
    tmp = g_ai.cfg.chunk_ms;
    if (cfg_get_int(g_cfg_ptr, "ai_dialog.chunk_ms",  &tmp, NULL) == 0) g_ai.cfg.chunk_ms = (int)tmp;

    cfg_str("ai_dialog.asr.endpoint",  g_ai.cfg.asr_endpoint,  sizeof(g_ai.cfg.asr_endpoint),
            "https://api.openai.com/v1/audio/transcriptions");
    cfg_str("ai_dialog.asr.api_key",   g_ai.cfg.asr_api_key,   sizeof(g_ai.cfg.asr_api_key), "");
    cfg_str("ai_dialog.asr.model",     g_ai.cfg.asr_model,     sizeof(g_ai.cfg.asr_model), "whisper-1");
    cfg_str("ai_dialog.asr.language",  g_ai.cfg.asr_language,  sizeof(g_ai.cfg.asr_language), "");

    cfg_str("ai_dialog.llm.endpoint",  g_ai.cfg.llm_endpoint,  sizeof(g_ai.cfg.llm_endpoint),
            "https://api.openai.com/v1/chat/completions");
    cfg_str("ai_dialog.llm.api_key",   g_ai.cfg.llm_api_key,   sizeof(g_ai.cfg.llm_api_key), "");
    cfg_str("ai_dialog.llm.model",     g_ai.cfg.llm_model,     sizeof(g_ai.cfg.llm_model), "gpt-4o-mini");
    cfg_str("ai_dialog.llm.system_prompt", g_ai.cfg.llm_system_prompt, sizeof(g_ai.cfg.llm_system_prompt),
            "你是一个友好的电话客服，简短回答。");
    tmp = g_ai.cfg.llm_enabled;
    if (cfg_get_int(g_cfg_ptr, "ai_dialog.llm.enabled", &tmp, NULL) == 0) g_ai.cfg.llm_enabled = (int)tmp;

    cfg_str("ai_dialog.tts.endpoint",  g_ai.cfg.tts_endpoint,  sizeof(g_ai.cfg.tts_endpoint),
            "https://api.elevenlabs.io/v1/text-to-speech");
    cfg_str("ai_dialog.tts.api_key",   g_ai.cfg.tts_api_key,   sizeof(g_ai.cfg.tts_api_key), "");
    cfg_str("ai_dialog.tts.voice_id",  g_ai.cfg.tts_voice_id,  sizeof(g_ai.cfg.tts_voice_id),
            "21m00Tcm4TlvDq8ikWAM");

#ifdef HAVE_REAL_ASR_TTS
    llog(LOG_INFO, "ai", "ai_dialog enabled=%d target_pt=%d chunk_ms=%d llm=%s",
         g_ai.cfg.enabled, g_ai.cfg.target_pt, g_ai.cfg.chunk_ms,
         g_ai.cfg.llm_enabled ? "on" : "off(mode=passthrough)");
    llog(LOG_INFO, "ai", "ASR endpoint=%s model=%s", g_ai.cfg.asr_endpoint, g_ai.cfg.asr_model);
    llog(LOG_INFO, "ai", "TTS endpoint=%s voice=%s", g_ai.cfg.tts_endpoint, g_ai.cfg.tts_voice_id);
#else
    llog(LOG_INFO, "ai", "ai_dialog enabled=%d [mock mode — no libcurl at build time]", g_ai.cfg.enabled);
#endif

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
            .help = "启动 AI 对话 session（捕获 B 腿语音 → ASR → LLM → TTS → 注入 A 腿）",
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

    /* cURL global init —— 进程一次就行（ai_http.c 每次 curl_easy_init 前也会 init，但
     * 进程级 init 更规范，SSL/全局锁等一次初始化好） */
#ifdef HAVE_REAL_ASR_TTS
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Phase R-14: 注册内置 AI backend ops —— ai_dialog 自己封装 whisper/openai/elevenlabs */
    {
        ai_backend_ops_t *wr = (ai_backend_ops_t *)&whisper_openai_ops;
        wr->ud = &g_ai.cfg;
        ai_backend_registry_register(wr);

        ai_backend_ops_t *cr = (ai_backend_ops_t *)&openai_chat_ops;
        cr->ud = &g_ai.cfg;
        ai_backend_registry_register(cr);

        ai_backend_ops_t *el = (ai_backend_ops_t *)&elevenlabs_ops;
        el->ud = &g_ai.cfg;
        ai_backend_registry_register(el);
    }
#endif

    return NSBC_OK;
}

static void ai_fini(struct nsbc_ctx *ctx)
{
    (void)ctx;
    llog(LOG_INFO, "ai", "ai_dialog fini");

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

#ifdef HAVE_REAL_ASR_TTS
    curl_global_cleanup();
#endif

    memset(&g_ai, 0, sizeof(g_ai));
}

/* deps：signaling 必须先 init（桥接层） */
static const char *ai_deps[] = { "signaling", NULL };

NSBC_MOD_CAT(ai_dialog, 1, MOD_L_WORKER_READY, 0, MOD_CAT_AI, ai_deps, ai_init, NULL, NULL, ai_fini);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
