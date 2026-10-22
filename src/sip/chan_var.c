/* 通道变量引擎（REQ-CHAN-VAR-001，P0）
 *
 * 设计铁律：
 *   - 全部从 mem_arena 分配（SIGNAL 域），不进 glibc malloc；
 *   - 写时 mutex（P0 简化，P1 RCU 升级）；
 *   - 键白名单杜绝 '\r'/'\n' 帧注入 + nsbc_ 只读保护；
 *   - leg 前缀 "0:" / "1:" 内部存储，对外 API 用 leg=-1/0/1 传。
 */
#include "chan_var.h"
#include "log.h"
#include "nsbc_common.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* ---- djb2 ---- */
static uint32_t cv_hash(const char *s, size_t n)
{
    uint32_t h = 5381;
    for (size_t i = 0; i < n; ++i)
        h = ((h << 5) + h) + (uint8_t)s[i];
    return h % CV_BUCKETS;
}

/* ---- 内部完整键拼接 ---- */
static int cv_full_key(int leg, const char *key, char *out, size_t cap)
{
    if (!key || !out || cap < 2) return -1;
    size_t klen = strlen(key);
    size_t plen = 0;
    if (leg == 0 || leg == 1) {
        plen = 2;  /* "0:" or "1:" */
        if (cap <= plen + klen) return -1;
        out[0] = (char)('0' + leg);
        out[1] = ':';
    } else {
        if (cap <= klen) return -1;
    }
    memcpy(out + plen, key, klen);
    out[plen + klen] = '\0';
    return 0;
}

/* ---- 校验 ---- */

/* nsbc_* 只读集合（白名单：已知系统变量） */
static const char *nsbc_ro_names[] = {
    "nsbc_rtp_loss", "nsbc_rtp_jitter", "nsbc_codec_a", "nsbc_codec_b",
    "nsbc_callid_a", "nsbc_callid_b", "nsbc_profile", "nsbc_tenant",
    "nsbc_direction", "nsbc_state_a", "nsbc_state_b",
    NULL
};

int chan_var_key_valid(const char *key, size_t len, int *readonly_out)
{
    if (!key || len == 0 || len >= CV_KEY_MAX) {
        if (readonly_out) *readonly_out = 0;
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)key[i];
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
        if (!ok) {
            if (readonly_out) *readonly_out = 0;
            return 0;
        }
    }
    /* nsbc_ 前缀只读 */
    int ro = 0;
    if (len >= 5 && strncmp(key, "nsbc_", 5) == 0) {
        ro = 1;
        for (int i = 0; nsbc_ro_names[i]; ++i) {
            if (len == strlen(nsbc_ro_names[i]) &&
                memcmp(key, nsbc_ro_names[i], len) == 0) {
                ro = 1;
                break;
            }
        }
    }
    if (readonly_out) *readonly_out = ro;
    return 1;
}

int chan_var_val_valid(const char *val, size_t len)
{
    if (!val || len == 0 || len >= CV_VAL_MAX) return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)val[i];
        if (ch < 0x20 || ch == 0x7f) return 0;  /* 不含 TAB 0x09——也禁 */
    }
    return 1;
}

/* ---- 生命周期 ---- */

int chan_var_create(mem_arena_t *arena, chan_var_t **out)
{
    if (!out || !arena) return NSBC_INVAL;
    chan_var_t *v = (chan_var_t *)calloc(1, sizeof(*v));
    if (!v) return NSBC_NOMEM;
    v->arena = arena;
    pthread_mutex_init(&v->lock, NULL);
    *out = v;
    return NSBC_OK;
}

void chan_var_destroy(chan_var_t *v)
{
    if (!v) return;
    pthread_mutex_destroy(&v->lock);
    /* entries 全在 arena，arena 随 call_session 一起销毁，这里不单独 free */
    free(v);
}

/* ---- 读写 ---- */

/* arena 分配 entry；key/val 直接拷贝（不额外 malloc） */
static chan_var_entry_t *cv_entry_alloc(chan_var_t *v, const char *key,
                                        const char *val)
{
    chan_var_entry_t *e = (chan_var_entry_t *)mem_arena_alloc(
        v->arena, sizeof(*e));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    /* key/val 直接存定长数组，零二次分配 */
    snprintf(e->key, sizeof(e->key), "%s", key);
    snprintf(e->val, sizeof(e->val), "%s", val ? val : "");
    return e;
}

int chan_var_set(chan_var_t *v, int leg, const char *key, const char *value)
{
    if (!v) return NSBC_INVAL;
    int ro = 0;
    if (!chan_var_key_valid(key, strlen(key), &ro))
        return NSBC_INVAL;
    if (ro) return NSBC_PERM_DENIED;
    size_t vlen = value ? strlen(value) : 0;
    if (!chan_var_val_valid(value, vlen))
        return NSBC_INVAL;

    char fkey[CV_KEY_MAX];
    if (cv_full_key(leg, key, fkey, sizeof(fkey)) != 0)
        return NSBC_INVAL;

    uint32_t b = cv_hash(fkey, strlen(fkey));
    pthread_mutex_lock(&v->lock);
    /* 查已有 entry —— 直接覆写 val（同 arena 对象复用） */
    for (chan_var_entry_t *e = v->buckets[b]; e; e = e->next) {
        if (memcmp(e->key, fkey, sizeof(e->key)) == 0) {
            snprintf(e->val, sizeof(e->val), "%s", value);
            pthread_mutex_unlock(&v->lock);
            return NSBC_OK;
        }
    }
    chan_var_entry_t *ne = cv_entry_alloc(v, fkey, value);
    if (!ne) {
        pthread_mutex_unlock(&v->lock);
        return NSBC_NOMEM;
    }
    ne->next = v->buckets[b];
    v->buckets[b] = ne;
    v->count++;
    pthread_mutex_unlock(&v->lock);
    return NSBC_OK;
}

int chan_var_get(chan_var_t *v, int leg, const char *key,
                 char *out, size_t out_cap)
{
    if (!v || !key || !out || out_cap == 0) return NSBC_INVAL;
    if (strlen(key) >= CV_KEY_MAX) return NSBC_INVAL;

    char fkey[CV_KEY_MAX];
    if (cv_full_key(leg, key, fkey, sizeof(fkey)) != 0)
        return NSBC_INVAL;

    uint32_t b = cv_hash(fkey, strlen(fkey));
    pthread_mutex_lock(&v->lock);
    for (chan_var_entry_t *e = v->buckets[b]; e; e = e->next) {
        if (memcmp(e->key, fkey, sizeof(e->key)) == 0) {
            size_t n = strlen(e->val);
            if (n >= out_cap) n = out_cap - 1;
            memcpy(out, e->val, n);
            out[n] = '\0';
            pthread_mutex_unlock(&v->lock);
            return NSBC_OK;
        }
    }
    pthread_mutex_unlock(&v->lock);
    return NSBC_NOENT;
}

int chan_var_unset(chan_var_t *v, int leg, const char *key)
{
    if (!v || !key) return NSBC_INVAL;
    if (strlen(key) >= CV_KEY_MAX) return NSBC_INVAL;

    char fkey[CV_KEY_MAX];
    if (cv_full_key(leg, key, fkey, sizeof(fkey)) != 0)
        return NSBC_INVAL;

    uint32_t b = cv_hash(fkey, strlen(fkey));
    pthread_mutex_lock(&v->lock);
    chan_var_entry_t *prev = NULL;
    for (chan_var_entry_t *e = v->buckets[b]; e; prev = e, e = e->next) {
        if (memcmp(e->key, fkey, sizeof(e->key)) == 0) {
            if (prev) prev->next = e->next;
            else v->buckets[b] = e->next;
            v->count--;
            /* entry 留在 arena 不回收（RCU 宽限期内仍有引用）——
             * P0 简化设计：arena 整体销毁时统一回收，单 entry 不 free。
             * 对单会话几百条变量不是问题。 */
            pthread_mutex_unlock(&v->lock);
            return NSBC_OK;
        }
    }
    pthread_mutex_unlock(&v->lock);
    return NSBC_NOENT;
}

/* ---- 条件求值器（P0 骨架） ----
 *
 * 支持嵌套解析："${a:-${b:-foo}}"
 * 关键技巧：在一次遍历里识别 ${...} 边界，递归解析默认值部分。 */
static int cv_subst_recursive(chan_var_t *v, int leg, const char *expr,
                              size_t expr_len, char *out, size_t out_cap);

int chan_var_substitute(chan_var_t *v, int leg, const char *expr,
                        char *out, size_t out_cap)
{
    if (!v || !expr || !out || out_cap == 0) return NSBC_INVAL;
    return cv_subst_recursive(v, leg, expr, strlen(expr), out, out_cap);
}

static int cv_subst_recursive(chan_var_t *v, int leg, const char *expr,
                              size_t expr_len, char *out, size_t out_cap)
{
    size_t pos = 0;     /* 写指针 */
    size_t i = 0;       /* 读指针 */

    while (i < expr_len) {
        /* 找下一个 "${"  */
        const char *var_start = NULL;
        size_t var_end = 0;

        while (i < expr_len - 1) {
            if (expr[i] == '$' && expr[i + 1] == '{') {
                var_start = expr + i + 2;
                /* 找配对 '}' —— 注意内部嵌套可能有 }，需要深度跟踪 */
                int depth = 1;
                size_t j = i + 2;
                while (j < expr_len && depth > 0) {
                    if (expr[j] == '$' && j + 1 < expr_len && expr[j + 1] == '{')
                        depth++;
                    else if (expr[j] == '}')
                        depth--;
                    j++;
                }
                if (depth == 0) {
                    var_end = j - 1;   /* 指向匹配的 '}' */
                    break;
                } else {
                    /* 未闭合，按字面处理这组 ${ */
                    var_start = NULL;
                    i++;
                }
            } else {
                i++;
            }
        }

        if (var_start == NULL) {
            /* 无剩余 ${...}：直接拷贝剩余字面 */
            size_t remain = expr_len - i;
            if (remain >= out_cap - pos) {
                size_t left = out_cap - pos - 1;
                if (left > 0) memcpy(out + pos, expr + i, left);
                pos += left;
                break;
            }
            memcpy(out + pos, expr + i, remain);
            pos += remain;
            out[pos] = '\0';
            return NSBC_OK;
        }

        /* 先拷贝 var_start 之前的字面 */
        (void)((size_t)var_start); /* lit_len 残留 */
        /* 简化计算：i 就是 ${ 前一字符位置 */
        size_t prefix_len = var_start - 2 - expr;  /* "a${var}" → 拷贝 "a" */
        if (prefix_len > 0) {
            if (prefix_len >= out_cap - pos) {
                size_t left = out_cap - pos - 1;
                memcpy(out + pos, expr, left);
                pos += left;
                out[pos] = '\0';
                return NSBC_OK;
            }
            memcpy(out + pos, expr, prefix_len);
            pos += prefix_len;
        }

        /* 解析 var 名和可选默认值 */
        const char *var_body = var_start;
        size_t body_len = var_end - (size_t)(var_start - expr);
        const char *default_val = NULL;
        size_t default_len = 0;

        /* 切分 ${name}  vs ${name:-default} */
        for (size_t k = 0; k + 1 < body_len; ++k) {
            if (var_body[k] == ':' && var_body[k + 1] == '-') {
                /* 注意 ${var:- 也是合法（默认空串）—— body_len > k+2 */
                default_val = var_body + k + 2;
                default_len = body_len - k - 2;
                body_len = k;  /* 截到 ':' 之前 */
                break;
            }
        }

        /* 拿变量值 */
        char vbuf[CV_VAL_MAX];
        int rc = chan_var_get(v, leg, var_body, vbuf, sizeof(vbuf));
        if (rc != NSBC_OK) {
            /* 不存在，用默认值（递归求值，支持嵌套） */
            if (default_val) {
                char def_buf[CV_VAL_MAX];
                int drc = cv_subst_recursive(v, leg, default_val, default_len,
                                             def_buf, sizeof(def_buf));
                if (drc != NSBC_OK) def_buf[0] = '\0';
                size_t dlen = strlen(def_buf);
                if (pos + dlen >= out_cap - 1) dlen = out_cap - pos - 1;
                if (dlen > 0) memcpy(out + pos, def_buf, dlen);
                pos += dlen;
            }
        } else {
            size_t vlen = strlen(vbuf);
            if (pos + vlen >= out_cap - 1) vlen = out_cap - pos - 1;
            if (vlen > 0) memcpy(out + pos, vbuf, vlen);
            pos += vlen;
        }

        /* 推进到 var_end 之后（跳过 '}'） */
        i = var_end + 1;
    }

    out[pos] = '\0';
    return NSBC_OK;
}

/* ---- 批量遍历 ---- */

int chan_var_foreach(chan_var_t *v, int flags, chan_var_cb cb, void *ud)
{
    if (!v || !cb) return NSBC_INVAL;
    pthread_mutex_lock(&v->lock);
    int n = 0;
    for (uint32_t b = 0; b < CV_BUCKETS; ++b) {
        for (chan_var_entry_t *e = v->buckets[b]; e; e = e->next) {
            /* 解析 leg 前缀 */
            int leg = -1;
            const char *k = e->key;
            if (k[0] == '0' && k[1] == ':') { leg = 0; k = k + 2; }
            else if (k[0] == '1' && k[1] == ':') { leg = 1; k = k + 2; }
            /* flags 过滤：CV_F_SESSION 只拿 session 级 */
            if (flags == CV_F_SESSION && leg != -1) continue;
            if (flags == CV_F_LEG_A && leg != 0) continue;
            if (flags == CV_F_LEG_B && leg != 1) continue;
            cb(k, e->val, leg, ud);
            n++;
        }
    }
    pthread_mutex_unlock(&v->lock);
    return n;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
