/* 分机/中继对象与凭证库（第 5 章，P0 凭证回调；对象策略 P1 展开）
 *
 * P0 范围：
 * - 内存分机表（user → 预计算 HA1），作为 registrar Digest 校验的
 *   digest_ha1_fn 唯一数据源（明文密码不入事务层）；
 * - 中继对象（鉴权外呼/IP 白名单/CODEC 策略）P1 由配置/DB 加载。
 * 表用分片互斥；容量固定，关键注册路径不 malloc（HA1 为定长内联）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "app_ctx.h"
#include "endpoints.h"
#include "log.h"
#include "module.h"
#include "uDS_client.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cjson/cJSON.h>

#define EP_SHARDS 64
/* EP_USER_MAX / EP_HA1_LEN / EP_CLIP_MAX / ep_ext_t 已在 endpoints.h 定义 */
#define EP_BUCKET_CAP 4096

typedef struct {
    pthread_mutex_t lock;
    ep_ext_t *head;
    int count;
} ep_shard_t;

typedef struct {
    ep_shard_t sh[EP_SHARDS];
} ep_store_t;

static ep_store_t *g_ep;

static unsigned hash_user(const char *s, size_t n)
{
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

/* shard 内插入/更新一条完整分机记录（user+realm 双键相同则整条覆盖） */
static int ep_ext_insert(ep_store_t *e, const ep_ext_t *ne)
{
    unsigned si = hash_user(ne->user, strlen(ne->user)) & (EP_SHARDS - 1);
    ep_shard_t *s = &e->sh[si];
    pthread_mutex_lock(&s->lock);
    for (ep_ext_t *x = s->head; x; x = x->next) {
        if (strcmp(x->user, ne->user) == 0 &&
            strcmp(x->realm, ne->realm) == 0) {
            memcpy(x->ha1, ne->ha1, EP_HA1_LEN + 1);
            x->neg = ne->neg;
            x->neg_until_ms = ne->neg_until_ms;
            memcpy(x->clip, ne->clip, sizeof(x->clip));
            x->clir = ne->clir;
            x->max_contacts = ne->max_contacts;
            /* 外部分机字段（每次 reload 全量覆盖） */
            memcpy(x->ext_type, ne->ext_type, sizeof(x->ext_type));
            memcpy(x->external_number, ne->external_number, sizeof(x->external_number));
            memcpy(x->trunk_name, ne->trunk_name, sizeof(x->trunk_name));
            memcpy(x->display_number, ne->display_number, sizeof(x->display_number));
            pthread_mutex_unlock(&s->lock);
            return NSBC_OK;
        }
    }
    if (s->count >= EP_BUCKET_CAP) {
        pthread_mutex_unlock(&s->lock);
        return NSBC_NOSPC;
    }
    ep_ext_t *x = calloc(1, sizeof(*x));
    if (!x) {
        pthread_mutex_unlock(&s->lock);
        return NSBC_NOMEM;
    }
    *x = *ne;
    x->used = 1;
    x->next = s->head;
    s->head = x;
    ++s->count;
    pthread_mutex_unlock(&s->lock);
    return NSBC_OK;
}

/* === 外部分机查询（REQ-PBX-EXT-EXT-001） ===
 * signaling.c resolve_cb / queue 振铃时查 ext_type / trunk_name / external_number。
 * 返回内部内存对象指针（只读；reload 后旧指针悬空但调用方同步单次使用安全）。 */
const struct ep_ext *ep_lookup(const char *user, const char *realm)
{
    if (!g_ep || !user || !user[0]) return NULL;
    const char *rlm = (realm && realm[0]) ? realm : "nsbc.local";
    unsigned si = hash_user(user, strlen(user)) & (EP_SHARDS - 1);
    ep_shard_t *s = &g_ep->sh[si];
    pthread_mutex_lock(&s->lock);
    for (ep_ext_t *x = s->head; x; x = x->next) {
        if (x->neg) continue;
        if (strcmp(x->user, user) != 0) continue;
        /* Phase 2 多租户：realm 强制精确匹配，不再允许空串通配。
         * 以前 x->realm[0]=='\0' 时跳过匹配，导致任何 realm 的 SIP
         * 注册都能命中 DB 分机 → 跨租户认证绕过漏洞。
         * DB 中 realm 为空串的分机只能匹配 rlm 也为空串的请求
         * （registrar 会把空 realm 归一化到 cfg.realm）。 */
        if (strcmp(x->realm, rlm) != 0) continue;
        pthread_mutex_unlock(&s->lock);
        return x;
    }
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

/* 供配置/DB 层（P1）登记分机：ha1hex 为 32 字符 MD5 HEX */
int ep_extension_add(ep_store_t *e, const char *user, const char *ha1hex)
{
    if (strlen(user) == 0 || strlen(user) >= EP_USER_MAX ||
        strlen(ha1hex) != EP_HA1_LEN)
        return NSBC_INVAL;
    ep_ext_t ne;
    memset(&ne, 0, sizeof(ne));
    snprintf(ne.user, sizeof(ne.user), "%s", user);
    memcpy(ne.ha1, ha1hex, EP_HA1_LEN);
    ne.ha1[EP_HA1_LEN] = '\0';
    return ep_ext_insert(e, &ne);
}

/* Web 侧 bool_i64 序列化：enabled/clir 等布尔字段输出 1/0 数字
 * （也接受原生 true/false）；既非 bool 也非 number 时返回 def */
static int json_truthy(const cJSON *j, int def)
{
    if (!j)
        return def;
    if (cJSON_IsBool(j))
        return cJSON_IsTrue(j) ? 1 : 0;
    if (cJSON_IsNumber(j))
        return j->valueint != 0;
    return def;
}

/* digest_ha1_fn 实现（经 ctx 间接登记，见 sig_init）。
 * 返回 thread-local 拷贝而非节点内指针：锁内 memcpy，解锁后调用方
 * （digest_verify）使用的生命周期仅到本线程下一次 lookup——
 * reload 整链销毁后旧指针会悬空，必须值返回。
 *
 * 三级查找（REQ-DATA-SVC-003 混合式兜底）：
 *   L1 内存表（Web 全量推送 + 本回调回填；含负缓存 TTL 判断）
 *   L2 UDS 实时查询 Web DB（uds_request，EP_UDS_TIMEOUT_MS 超时；
 *      uds 线程未起/断链立即失败，不阻塞注册路径）
 *   L3 结果回填内存表（正=完整条目；负=neg+TTL 抑制重复查询） */
static __thread char tls_ha1[EP_HA1_LEN + 1];

#define EP_UDS_TIMEOUT_MS   200   /* L2 查询超时（注册路径容忍上限） */
#define EP_NEG_TTL_MS     30000   /* not_found/disabled 负缓存 TTL */
#define EP_NEG_TTL_ERR_MS  3000   /* db_error/uds 失败负缓存 TTL */

static int64_t ep_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* realm 转安全 JSON 字符串体（截断+去引号/反斜杠/控制字符） */
static void ep_json_escape(const char *in, size_t in_len,
                           char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 1 < out_sz; ++i) {
        unsigned char c = (unsigned char)in[i];
        out[o++] = (c == '"' || c == '\\' || c < 0x20) ? '_' : (char)c;
    }
    out[o] = '\0';
}

/* L3：写负缓存条目（user+realm 查询确认不存在/查询失败） */
static void ep_neg_cache_put(ep_store_t *e, const char *u, const char *rlm,
                             int64_t ttl_ms)
{
    ep_ext_t ne;
    memset(&ne, 0, sizeof(ne));
    snprintf(ne.user, sizeof(ne.user), "%s", u);
    snprintf(ne.realm, sizeof(ne.realm), "%s", rlm);
    ne.neg = 1;
    ne.neg_until_ms = ep_now_ms() + ttl_ms;
    ep_ext_insert(e, &ne);
}

static const char *ep_ha1_lookup(const char *realm, size_t rlen,
                                 const char *user, size_t ulen, void *ud)
{
    ep_store_t *e = (ep_store_t *)ud;
    if (!e || ulen == 0 || ulen >= EP_USER_MAX)
        return NULL;
    char u[EP_USER_MAX];
    memcpy(u, user, ulen);
    u[ulen] = '\0';

    /* realm 归一化（L1 键）：空 → 全局默认。registrar 传入的 realm 来自
     * sip_profile 匹配（多 profile realm）或全局 cfg.realm。 */
    char rlm[64];
    if (rlen >= sizeof(rlm))
        rlen = sizeof(rlm) - 1;
    if (rlen) {
        memcpy(rlm, realm, rlen);
        rlm[rlen] = '\0';
    } else {
        snprintf(rlm, sizeof(rlm), "nsbc.local");
    }

    int64_t now = ep_now_ms();

    /* L1：内存表（user+realm 双键，精确匹配）。
     * Phase 2 多租户：先精确匹配 realm；miss 后用空 realm 兜底
     * （兼容旧数据——旧条目 realm 可能是空串）。 */
    unsigned si = hash_user(u, ulen) & (EP_SHARDS - 1);
    ep_shard_t *s = &e->sh[si];
    pthread_mutex_lock(&s->lock);
    const char *found_ha1 = NULL;
    for (ep_ext_t *x = s->head; x; x = x->next) {
        if (strcmp(x->user, u) != 0)
            continue;
        if (x->neg && now < x->neg_until_ms) {
            /* 精确 realm 的负缓存仍成立 */
            break;
        }
        if (x->neg)
            continue; /* 负缓存过期 → 让 L2 去查 */
        /* 精确匹配优先；miss 再用空 realm 兜底（旧数据兼容） */
        if (strcmp(x->realm, rlm) == 0 || strcmp(x->realm, "") == 0) {
            memcpy(tls_ha1, x->ha1, EP_HA1_LEN + 1);
            found_ha1 = tls_ha1;
            break;
        }
    }
    pthread_mutex_unlock(&s->lock);
    if (found_ha1) return found_ha1;
    llog(LOG_INFO, "ep", "ha1 lookup L1 MISS user=%s realm=%s → L2 UDS query", u, rlm);

    /* L2：UDS 实时查询 Web DB（锁外阻塞，最长 EP_UDS_TIMEOUT_MS） */
    char rlm_esc[64];
    ep_json_escape(rlm, strlen(rlm), rlm_esc, sizeof(rlm_esc));
    char req[256];
    snprintf(req, sizeof(req),
             "{\"cmd\":\"ext.ha1\",\"user\":\"%s\",\"realm\":\"%s\"}", u, rlm_esc);

    char rsp[512];
    int rc = uds_request(req, EP_UDS_TIMEOUT_MS, rsp, sizeof(rsp));
    if (rc != NSBC_OK) {
        /* 未连接/超时/槽满：短负缓存，避免注册风暴打穿 UDS */
        llog(LOG_WARN, "ep",
             "ha1 lookup L2 UDS FAILED user=%s realm=%s rc=%d (not connected?)",
             u, rlm, rc);
        ep_neg_cache_put(e, u, rlm, EP_NEG_TTL_ERR_MS);
        return NULL;
    }
    llog(LOG_INFO, "ep", "ha1 lookup L2 rsp user=%s realm=%s: %s", u, rlm, rsp);

    cJSON *r = cJSON_Parse(rsp);
    if (!r) {
        llog(LOG_WARN, "ep", "ha1 lookup L2 parse failed user=%s realm=%s", u, rlm);
        ep_neg_cache_put(e, u, rlm, EP_NEG_TTL_ERR_MS);
        return NULL;
    }
    cJSON *okj = cJSON_GetObjectItem(r, "ok");
    cJSON *h1 = cJSON_GetObjectItem(r, "ha1");
    if (okj && cJSON_IsTrue(okj) && h1 && cJSON_IsString(h1) &&
        strlen(h1->valuestring) == EP_HA1_LEN) {
        /* 正结果：完整条目回填内存表（含 clip/clir/max_contacts） */
        ep_ext_t ne;
        memset(&ne, 0, sizeof(ne));
        snprintf(ne.user, sizeof(ne.user), "%s", u);
        snprintf(ne.realm, sizeof(ne.realm), "%s", rlm);
        memcpy(ne.ha1, h1->valuestring, EP_HA1_LEN + 1);
        cJSON *j;
        j = cJSON_GetObjectItem(r, "clip");
        if (j && cJSON_IsString(j))
            snprintf(ne.clip, sizeof(ne.clip), "%s", j->valuestring);
        j = cJSON_GetObjectItem(r, "clir");
        if (j && json_truthy(j, 0))
            ne.clir = 1;
        j = cJSON_GetObjectItem(r, "max_contacts");
        if (j && cJSON_IsNumber(j) && j->valueint > 0)
            ne.max_contacts = j->valueint;
        cJSON_Delete(r);
        ep_ext_insert(e, &ne);
        memcpy(tls_ha1, ne.ha1, EP_HA1_LEN + 1);
        return tls_ha1;
    }
    /* 负结果：not_found/disabled 长负缓存；其余（db_error 等）短负缓存 */
    cJSON *rsn = cJSON_GetObjectItem(r, "reason");
    int long_neg = rsn && cJSON_IsString(rsn) &&
                   (strcmp(rsn->valuestring, "not_found") == 0 ||
                    strcmp(rsn->valuestring, "disabled") == 0);
    cJSON_Delete(r);
    ep_neg_cache_put(e, u, rlm, long_neg ? EP_NEG_TTL_MS : EP_NEG_TTL_ERR_MS);
    return NULL;
}

static int ep_init(struct nsbc_ctx *ctx)
{
    ep_store_t *e = calloc(1, sizeof(*e));
    if (!e)
        return NSBC_NOMEM;
    for (int i = 0; i < EP_SHARDS; ++i)
        pthread_mutex_init(&e->sh[i].lock, NULL);
    g_ep = e;
    ctx->ha1_cb = ep_ha1_lookup;
    ctx->ha1_ud = e;
    return NSBC_OK;
}

/* 解析 "user=ha1,user=ha1,..." 格式，填充全局分机表。
 * 在 signaling sig_start 里调用（ep_init 已完成）。 */
int ep_extensions_load(const char *cfg_str)
{
    if (!cfg_str || !g_ep)
        return NSBC_OK;
    size_t slen = strlen(cfg_str);
    if (slen == 0)
        return NSBC_OK;
    char *dup = strdup(cfg_str);
    if (!dup)
        return NSBC_NOMEM;
    int loaded = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(dup, ",", &saveptr); tok;
         tok = strtok_r(NULL, ",", &saveptr)) {
        /* trim */
        while (*tok == ' ' || *tok == '\t')
            ++tok;
        char *eq = strchr(tok, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *user = tok;
        char *ha1 = eq + 1;
        /* trim ha1 */
        size_t hl = strlen(ha1);
        while (hl && (ha1[hl - 1] == ' ' || ha1[hl - 1] == '\t'))
            ha1[--hl] = '\0';
        /* trim user */
        size_t ul = strlen(user);
        while (ul && (user[ul - 1] == ' ' || user[ul - 1] == '\t'))
            user[--ul] = '\0';
        if (ul == 0 || hl != EP_HA1_LEN)
            continue;
        int rc = ep_extension_add(g_ep, user, ha1);
        if (rc == NSBC_OK)
            ++loaded;
        else
            llog(LOG_WARN, "ep", "extension add %s failed rc=%d", user, rc);
    }
    free(dup);
    llog(LOG_INFO, "ep", "loaded %d extensions from cfg", loaded);
    return NSBC_OK;
}

/* ------------------------------------------------------------------ */
/* Web DB 全量推送 → 整表替换（REQ-DATA-SVC-003）                      */
/* ------------------------------------------------------------------ */

/* 单条 JSON → ep_ext_t；失败返回 -1（err 已填） */
static int ep_json_parse_one(cJSON *obj, ep_ext_t *ne,
                             char *out_err, size_t err_sz, int idx)
{
    memset(ne, 0, sizeof(*ne));
    cJSON *j;

    j = cJSON_GetObjectItem(obj, "extension");
    if (!j || !cJSON_IsString(j) || !j->valuestring[0]) {
        snprintf(out_err, err_sz, "item[%d] missing extension", idx);
        return -1;
    }
    snprintf(ne->user, sizeof(ne->user), "%s", j->valuestring);

    /* 外部分机类型判断（先读，ha1 校验依赖它） */
    j = cJSON_GetObjectItem(obj, "ext_type");
    const char *is_ext = (j && cJSON_IsString(j) &&
                          strcmp(j->valuestring, "external") == 0) ? "external" : "internal";
    snprintf(ne->ext_type, sizeof(ne->ext_type), "%s", is_ext);

    j = cJSON_GetObjectItem(obj, "ha1");
    if (strcmp(is_ext, "external") == 0) {
        /* 外部分机不注册 → ha1 可为空（Digest 校验不会查它） */
        if (j && cJSON_IsString(j))
            snprintf(ne->ha1, sizeof(ne->ha1), "%s", j->valuestring);
    } else {
        if (!j || !cJSON_IsString(j) || strlen(j->valuestring) != EP_HA1_LEN) {
            snprintf(out_err, err_sz, "item[%d]=%s ha1 invalid (need 32 hex)",
                     idx, ne->user);
            return -1;
        }
        memcpy(ne->ha1, j->valuestring, EP_HA1_LEN + 1);
    }

    /* realm：强制精确匹配（Phase 2 多租户安全加固）
     * JSON 缺失/空值 → 安全默认 "nsbc.local"（以前留空 = 通配，是漏洞） */
    j = cJSON_GetObjectItem(obj, "realm");
    if (j && cJSON_IsString(j) && j->valuestring[0])
        snprintf(ne->realm, sizeof(ne->realm), "%s", j->valuestring);
    else
        snprintf(ne->realm, sizeof(ne->realm), "nsbc.local");

    j = cJSON_GetObjectItem(obj, "enabled");
    if (j && !json_truthy(j, 1))
        return 0; /* 停用分机：跳过（不入表） */
    ne->used = 1;

    j = cJSON_GetObjectItem(obj, "clip");
    if (j && cJSON_IsString(j))
        snprintf(ne->clip, sizeof(ne->clip), "%s", j->valuestring);

    j = cJSON_GetObjectItem(obj, "clir");
    if (j && json_truthy(j, 0))
        ne->clir = 1;

    j = cJSON_GetObjectItem(obj, "max_contacts");
    if (j && cJSON_IsNumber(j) && j->valueint > 0)
        ne->max_contacts = j->valueint;

    /* === 外部分机字段 === */
    if (strcmp(is_ext, "external") == 0) {
        j = cJSON_GetObjectItem(obj, "external_number");
        if (j && cJSON_IsString(j))
            snprintf(ne->external_number, sizeof(ne->external_number), "%s", j->valuestring);
        j = cJSON_GetObjectItem(obj, "trunk_name");
        if (j && cJSON_IsString(j))
            snprintf(ne->trunk_name, sizeof(ne->trunk_name), "%s", j->valuestring);
        j = cJSON_GetObjectItem(obj, "display_number");
        if (j && cJSON_IsString(j))
            snprintf(ne->display_number, sizeof(ne->display_number), "%s", j->valuestring);
        llog(LOG_INFO, "ep", "reload: external ext %s → trunk '%s' num=%s display=%s",
             ne->user, ne->trunk_name, ne->external_number, ne->display_number);
    }

    return 1;
}

static void ep_store_destroy(ep_store_t *e)
{
    for (int i = 0; i < EP_SHARDS; ++i) {
        ep_ext_t *x = e->sh[i].head;
        while (x) {
            ep_ext_t *nx = x->next;
            free(x);
            x = nx;
        }
        pthread_mutex_destroy(&e->sh[i].lock);
    }
    free(e);
}

int ext_table_reload_json(const char *json, char *out_err, size_t err_sz)
{
    if (!json || !*json) {
        if (out_err) snprintf(out_err, err_sz, "empty json");
        return NSBC_INVAL;
    }
    if (!g_ep) {
        if (out_err) snprintf(out_err, err_sz, "endpoints store not ready");
        return NSBC_ERR;
    }

    /* [1] 临时 store：完整建好才交换，失败不动线上表（事务性） */
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsArray(root)) {
        if (out_err) snprintf(out_err, err_sz, "invalid json: expected array");
        if (root) cJSON_Delete(root);
        return NSBC_INVAL;
    }
    ep_store_t *tmp = calloc(1, sizeof(*tmp));
    if (!tmp) {
        cJSON_Delete(root);
        if (out_err) snprintf(out_err, err_sz, "OOM");
        return NSBC_NOMEM;
    }
    for (int i = 0; i < EP_SHARDS; ++i)
        pthread_mutex_init(&tmp->sh[i].lock, NULL);

    int n = cJSON_GetArraySize(root);
    int loaded = 0;
    for (int i = 0; i < n; ++i) {
        cJSON *obj = cJSON_GetArrayItem(root, i);
        if (!obj || !cJSON_IsObject(obj)) {
            if (out_err) snprintf(out_err, err_sz, "item[%d] not object", i);
            goto rollback;
        }
        ep_ext_t ne;
        int r = ep_json_parse_one(obj, &ne, out_err, err_sz, i);
        if (r < 0)
            goto rollback;
        if (r == 0)
            continue; /* enabled=false 跳过 */
        int rc = ep_ext_insert(tmp, &ne);
        if (rc != NSBC_OK) {
            if (out_err) snprintf(out_err, err_sz,
                                 "item[%d]=%s insert failed rc=%d",
                                 i, ne.user, rc);
            goto rollback;
        }
        ++loaded;
    }
    cJSON_Delete(root);

    /* [2] 逐 shard 指针交接（容器地址不动：registrar 值拷贝的 ha1_ud
     *     仍指向 g_ep；锁序固定 tmp→g_ep，与并发 reload/查询无死锁） */
    for (int i = 0; i < EP_SHARDS; ++i) {
        pthread_mutex_lock(&tmp->sh[i].lock);
        pthread_mutex_lock(&g_ep->sh[i].lock);
        ep_ext_t *old_head = g_ep->sh[i].head;
        int old_count = g_ep->sh[i].count;
        g_ep->sh[i].head = tmp->sh[i].head;
        g_ep->sh[i].count = tmp->sh[i].count;
        tmp->sh[i].head = old_head;
        tmp->sh[i].count = old_count;
        pthread_mutex_unlock(&g_ep->sh[i].lock);
        pthread_mutex_unlock(&tmp->sh[i].lock);
    }

    /* [3] tmp 现持有全部旧链：锁外销毁（交接完成后无并发读者） */
    ep_store_destroy(tmp);

    llog(LOG_NOTICE, "ep", "reload: %d extensions active (from %d pushed)",
         loaded, n);
    return NSBC_OK;

rollback:
    ep_store_destroy(tmp);
    cJSON_Delete(root);
    return NSBC_ERR;
}

static void ep_fini(struct nsbc_ctx *ctx)
{
    ep_store_t *e = g_ep;
    if (!e)
        return;
    if (ctx->ha1_ud == e) {
        ctx->ha1_cb = NULL;
        ctx->ha1_ud = NULL;
    }
    ep_store_destroy(e);
    g_ep = NULL;
}

/* signaling 依赖本模块：在 WORKER_READY 层初始化，早于 FINAL 的 signaling */
NSBC_MOD0_CAT(endpoints, 1, MOD_L_WORKER_READY, MOD_F_REQUIRED, MOD_CAT_ENDPOINT, ep_init,
          ep_fini);
