/* ai_backend_registry.h — AI 后端自注册 + 安全卸载（Phase 2 基础设施）
 *
 * 设计来源：白皮书 §2.1 ai_backend_ops_t + §2.3 安全卸载模板
 * 原子风格：项目约定 __atomic 内置函数（非 C11 stdatomic.h）
 *
 * 使用流程（模块 init）：
 *   ai_backend_ops_t ops = { .name="elevenlabs", .tts=..., .ud=api_key };
 *   ai_backend_registry_register(&ops);
 *   // ... module fini:
 *   ai_backend_registry_unregister("elevenlabs");  // 内部先置 unloading + 自旋 wait
 *
 * 调用流程（ai_dialog / asr_tts）：
 *   int slot = -1;
 *   const ai_backend_ops_t *ops = ai_backend_registry_find_inc("whisper", &slot);
 *   if (ops) { ops->tts(...); ai_backend_registry_dec(slot); }
 */
#ifndef NSBC_AI_BACKEND_REGISTRY_H
#define NSBC_AI_BACKEND_REGISTRY_H

#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AI_BACKEND_MAX 8

/* AI Backend ops（白皮书 §2.1 设计）
 * tts: 文本转 PCM（16kHz mono int16），返回 NSBC_OK + pcm_out/pcm_len
 *      调用方必须 free(*pcm_out)
 * asr: PCM 转文本，返回 NSBC_OK + text_out
 *      调用方必须 free(*text_out)
 * llm: 可选，给 AI Dialog 更复杂推理（可选函数指针）
 * ud: backend 私有句柄（如 ElevenLabs API key / Whisper 模型路径）*/
typedef struct ai_backend_ops {
    const char *name;                    /* "elevenlabs", "whisper", "doubao", "ollama" */
    int  (*tts)(const char *text, int sample_rate, char **pcm_out, size_t *pcm_len, void *ud);
    int  (*asr)(const char *pcm_in, size_t pcm_len, int sample_rate, char **text_out, void *ud);
    int  (*llm)(const char *prompt, char **text_out, void *ud);  /* 可选：NULL=不支持 */
    void *ud;                            /* backend 私有句柄（init 时设置，tts/asr/llm 回调传入）*/
} ai_backend_ops_t;

/* 注册/注销
 * register: 同名已存在 → 覆盖（动态模块 reload 同 backend 版本）
 * unregister: 置 unloading + 自旋 wait refcnt→0 + 清空 */
int  ai_backend_registry_register(const ai_backend_ops_t *ops);
int  ai_backend_registry_unregister(const char *name);

/* 查找 + INC/DEC（卸载安全）*/
const ai_backend_ops_t *ai_backend_registry_find_inc(const char *name, int *out_slot);
void                    ai_backend_registry_dec(int slot);
const ai_backend_ops_t *ai_backend_registry_find(const char *name);  /* 不带 INC */

/* 卸载等待（给 modmgr_unload 调）*/
int  ai_backend_registry_unload_wait(const char *name, int timeout_ms);

/* 遍历 + 计数（OAM show ai_backends 用）*/
int  ai_backend_registry_count(void);
const ai_backend_ops_t *ai_backend_registry_find_by_slot(int slot);

/* =========================================================================
 * REQ-AI-MEDIA-001 / REQ-AI-TRANSFER-001: Pass-through 公共 API
 *
 * 设计：
 *   ai_dialog 编译成动态模块 mod_ai_dialog.so —— 主二进制链接器看不到 .so 里的符号。
 *   所以这 4 个公共 API 在 ai_backend_registry.c（主二进制）里实现为
 *   **wrapper**，内部通过函数指针转发给 mod_ai_dialog.so 的真实实现。
 *
 *   mod_ai_dialog.so 的 ai_init() 里调 ai_passthrough_register_fns()
 *   注册 4 个真实函数指针。
 *   如果模块没加载，指针为 NULL，wrapper 返回 NSBC_NOENT。
 * ========================================================================= */

/* 函数指针类型（签名和下面 4 个 wrapper 完全对齐）*/
typedef int     (*ai_fp_set_producer_t)(void *call_key, int producer);
typedef size_t  (*ai_fp_get_rx_audio_t)(void *call_key, uint8_t *out_buf, size_t out_max_len);
typedef int     (*ai_fp_send_tx_audio_t)(void *call_key, const uint8_t *data, size_t len, int format);
typedef int     (*ai_fp_request_transfer_t)(void *call_key, const char *dest);

/* mod_ai_dialog.so 的 ai_init 里调这个注册真实实现 */
typedef struct {
    ai_fp_set_producer_t      set_producer;
    ai_fp_get_rx_audio_t      get_rx_audio;
    ai_fp_send_tx_audio_t     send_tx_audio;
    ai_fp_request_transfer_t  request_transfer;
} ai_passthrough_fns_t;

int ai_passthrough_register_fns(const ai_passthrough_fns_t *fns);
int ai_passthrough_unregister(void);  /* fini 时调 */
int ai_passthrough_available(void);   /* 1=模块已加载 0=未加载 */

/* REQ-AI-TRANSFER-001：ai_dialog 公共 API —— 请求 AI session 在下次管道跑完后
 * 调 b2b_transfer_call 发 blind transfer REFER 给 dest。
 *
 * 非阻塞：只设 session->transfer_pending=1 + transfer_dest，
 * 实际 REFER 由 ai_session_thread 自己触发（避免跨线程碰 b2b_call_t.lock）。
 *
 * call_key:  b2b_call_t*（不透明指针）
 * dest:      目标分机号或完整 SIP URI，最长 256 字节，可见 ASCII
 * 返回：     NSBC_OK=设置成功；NSBC_NOENT=call_key 没 session 或模块未加载；NSBC_INVAL=dest 非法 */
int  ai_request_transfer(void *call_key, const char *dest);

/* REQ-AI-MEDIA-001：Pass-through 公共 API —— AI 模块做媒体管道，ASR/LLM/TTS 交给外部第三方 AI
 *
 * 架构：
 *   ai_dialog (C)  ← ring buffer + bridge_send_raw + transfer 钩子
 *        ↓
 *   外部 AI agent (Python/Go/Rust) → ai_get_rx_audio 拉用户语音 → ASR/LLM/TTS → ai_send_tx_audio 推回复
 *
 * 为什么这么分：C 核心只做媒体桥接（零 HTTP/curl 副作用）；第三方 AI 想用什么云 ASR/本地 LLM 都能接；
 * 转人工座席是 B2BUA 业务层能力，留在 C 核心（b2b_transfer_call 已实现）；
 * 内建 ASR/LLM/TTS 管道降级为 demo 模式（cfg.llm_enabled=0 默认关闭）。 */

/* 设置 session producer 模式：
 *   producer=1 → ai_session_thread 空转，外部 AI 通过 ai_get_rx_audio/ai_send_tx_audio 直接接管
 *   producer=0 → 默认，由 cfg.llm_enabled 控制内建 demo 管道开关
 * 返回：NSBC_OK / NSBC_NOENT / NSBC_INVAL */
int     ai_set_producer(void *call_key, int producer);

/* 拉取用户语音（PCMU 8kHz）。非阻塞，消费语义（读了就不保留）。
 * 返回：实际读取的字节数（0 = ring buffer 空 / 无 session / 模块未加载）*/
size_t  ai_get_rx_audio(void *call_key, uint8_t *out_buf, size_t out_max_len);

/* 推送回复音频给用户。format=0 PCMU 8kHz；format=1 PCM16（函数内自动转 PCMU）。
 * 非阻塞。内部 bridge_send_raw 按 RTP 节奏注入 A 腿。
 * 返回：NSBC_OK / NSBC_NOENT / NSBC_INVAL / NSBC_NOMEM */
int     ai_send_tx_audio(void *call_key, const uint8_t *data, size_t len, int format);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_AI_BACKEND_REGISTRY_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
