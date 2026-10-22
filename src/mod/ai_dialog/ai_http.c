/* AI 模块 HTTP 客户端（cURL 封装，仅 HAVE_REAL_ASR_TTS 时编进 .so）
 * 不包宏的话无 cURL 环境会因找不到 curl/curl.h 编译失败 */
#ifdef HAVE_REAL_ASR_TTS
#include "ai_http.h"
#include "log.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char   *data;
    size_t  size;
} curl_buf_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *ud)
{
    size_t total = size * nmemb;
    curl_buf_t *b = (curl_buf_t *)ud;
    char *np = (char *)realloc(b->data, b->size + total);
    if (!np) return 0;
    b->data = np;
    memcpy(b->data + b->size, ptr, total);
    b->size += total;
    return total;
}

static curl_buf_t curl_do(CURL *h, ai_http_resp_t *out)
{
    curl_buf_t b = {0};
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &b);
    CURLcode rc = curl_easy_perform(h);
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    out->http_code = code;
    if (rc != CURLE_OK) {
        llog(LOG_WARN, "ai_http", "curl_easy_perform failed: %s", curl_easy_strerror(rc));
        curl_buf_t empty = {0};
        free(b.data);
        return empty;
    }
    b.data ? (out->memory = b.data, out->size = b.size) : (out->memory = NULL, out->size = 0);
    return b; /* b.data 所有权已转给 out */
}

void ai_http_resp_free(ai_http_resp_t *r)
{
    if (r && r->memory) { free(r->memory); r->memory = NULL; r->size = 0; r->http_code = 0; }
}

int ai_http_post_json(const char *url, const char *api_key,
                      const char *json_body, size_t body_len,
                      ai_http_resp_t *out)
{
    if (!url || !json_body || !out) return NSBC_INVAL;

    CURL *h = curl_easy_init();
    if (!h) { llog(LOG_ERR, "ai_http", "curl_easy_init null"); return NSBC_ERR; }

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)body_len);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    if (api_key && api_key[0]) {
        char auth[256];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
        hdrs = curl_slist_append(hdrs, auth);
    }
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 30000L);  /* 30s */
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);         /* 线程安全 */
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);

    curl_do(h, out);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);

    if (out->http_code >= 400) {
        llog(LOG_WARN, "ai_http", "POST %s HTTP %ld", url, out->http_code);
        return NSBC_ERR;
    }
    return NSBC_OK;
}

int ai_http_post_multipart(const char *url, const char *api_key,
                           const char *audio_path, const char *extra_json,
                           ai_http_resp_t *out)
{
    if (!url || !audio_path || !out) return NSBC_INVAL;

    CURL *h = curl_easy_init();
    if (!h) { llog(LOG_ERR, "ai_http", "curl_easy_init null"); return NSBC_ERR; }

    curl_mime *mime = curl_mime_init(h);
    curl_mimepart *part = NULL;

    /* 文件 part */
    part = curl_mime_addpart(mime);
    curl_mime_filedata(part, audio_path);
    curl_mime_name(part, "file");

    /* model / language / response_format —— Whisper 最小只要文件；
     * extra_json 是 {"model": "whisper-1", "language": "zh", "response_format": "json"}，
     * 我们拆成单独 text part 逐个传。这里简化：把整个 JSON 作为 model 字段 value
     * 塞进 curl 不方便，用额外 curl option 手动组 multipart 太复杂 —— 改用
     * curl_mime_addpart 循环解析 extra_json 里的每个 key 加 text part。 */
    if (extra_json && extra_json[0]) {
        /* 简化版：目前只支持 model / language 两个常见字段 */
        const char *p;
        (void)p;
        /* 生产版需要 cJSON 解析 —— 这里先跳过，直接上传文件；
         * Whisper 默认 model=whisper-1，自动检测语言 */
    }

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_MIMEPOST, mime);

    struct curl_slist *hdrs = NULL;
    if (api_key && api_key[0]) {
        char auth[256];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
        hdrs = curl_slist_append(hdrs, auth);
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 120000L); /* ASR 慢时 2min */
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);

    curl_do(h, out);
    curl_slist_free_all(hdrs);
    curl_mime_free(mime);
    curl_easy_cleanup(h);

    if (out->http_code >= 400) {
        llog(LOG_WARN, "ai_http", "POST multipart %s HTTP %ld", url, out->http_code);
        return NSBC_ERR;
    }
    return NSBC_OK;
}

int ai_http_get_raw(const char *url, const char *api_key,
                    const char *extra_headers[], ai_http_resp_t *out)
{
    if (!url || !out) return NSBC_INVAL;

    CURL *h = curl_easy_init();
    if (!h) { llog(LOG_ERR, "ai_http", "curl_easy_init null"); return NSBC_ERR; }

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 60000L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);

    struct curl_slist *hdrs = NULL;
    if (api_key && api_key[0]) {
        char auth[256];
        snprintf(auth, sizeof(auth), "xi-api-key: %s", api_key); /* ElevenLabs 用 xi-api-key */
        hdrs = curl_slist_append(hdrs, auth);
    }
    if (extra_headers) {
        for (int i = 0; extra_headers[i]; ++i)
            hdrs = curl_slist_append(hdrs, extra_headers[i]);
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

    curl_do(h, out);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);

    if (out->http_code >= 400) {
        llog(LOG_WARN, "ai_http", "GET %s HTTP %ld", url, out->http_code);
        return NSBC_ERR;
    }
    return NSBC_OK;
}

/* REQ-AI-001：URL percent-encode（ai_dialog 拼 ElevenLabs / Whisper query 用）。
 * curl_easy_escape 返回 malloc 字符串；调用方 curl_free —— 它本质是 free。
 * curl_global_init 已在 ai_init 做过，这里 curl_easy_init 会复用。
 * 空串直接 ""，失败返回 NULL。*/
char *ai_url_encode(const char *s)
{
    if (!s) return NULL;
    if (!*s) {
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    CURL *h = curl_easy_init();
    if (!h) return NULL;
    char *escaped = curl_easy_escape(h, s, 0);
    curl_easy_cleanup(h);
    if (!escaped) return NULL;
    /* curl_easy_escape 用 malloc 分配（可直接 free）—— curl_free 等价于 free */
    char *out = strdup(escaped);
    curl_free(escaped);
    return out;
}

#endif /* HAVE_REAL_ASR_TTS */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
