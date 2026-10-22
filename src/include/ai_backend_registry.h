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
