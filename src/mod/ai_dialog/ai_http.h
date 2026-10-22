/* AI 模块 HTTP 客户端（cURL 封装）
 *
 * P1 简化：per-call 线程里同步调 cURL —— 够用，后面真要并发可换 libevent+evhttp。
 * 功能：
 *   - http_post_multipart()  —— OpenAI Whisper multipart POST（音频文件 → text）
 *   - http_post_json()        —— OpenAI Chat / 普通 POST JSON body
 *   - http_get_stream()       —— ElevenLabs 流式 TTS（返回 PCM16 raw）
 *
 * 所有函数返回 NSBC_OK/ERR/... 语义（nsbc 风格），输出写到调用方缓冲区。
 * 依赖：libcurl（configure 时 pkg-config 找，NSBC_WITH_CURL=1）。
 */
#ifndef NSBC_AI_HTTP_H
#define NSBC_AI_HTTP_H

#include "nsbc_types.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char    *memory;     /* malloc 的响应体，调用方 free */
    size_t   size;
    long     http_code;  /* 200/401/429... */
} ai_http_resp_t;

/* POST JSON body → 响应 body（Chat completions / embeddings 用） */
int ai_http_post_json(const char *url, const char *api_key,
                      const char *json_body, size_t body_len,
                      ai_http_resp_t *out);

/* POST multipart/form-data → 响应 body（Whisper transcriptions 用）
 * audio_field:      文件字段名（默认 "file"）
 * filename:         上传时的文件名（如 "audio.wav"）
 * content_type:     NULL 自动探测（.wav → audio/wav, .pcm → audio/L16）
 * 额外 kv 字段（model/language/response_format）通过 json_body 传 */
int ai_http_post_multipart(const char *url, const char *api_key,
                           const char *audio_path,
                           const char *extra_json,   /* NULL 可 */
                           ai_http_resp_t *out);

/* GET 返回原始字节流（ElevenLabs TTS PCM16 raw 用） */
int ai_http_get_raw(const char *url, const char *api_key,
                    const char *extra_headers[], /* NULL-terminated; 每个 "Key: Value" */
                    ai_http_resp_t *out);

/* 统一清理 */
void ai_http_resp_free(ai_http_resp_t *r);

/* URL percent-encode（用 curl_easy_escape）。
 * 返回 malloc 出来的编码字符串，调用方 free；失败返回 NULL。
 * 只在 NSBC_WITH_CURL=1 有意义，否则也可以用（curl 全局 init 已在 ai_init 做）。*/
char *ai_url_encode(const char *s);

#ifdef __cplusplus
}
#endif
#endif

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
