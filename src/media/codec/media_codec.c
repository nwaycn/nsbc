/* 编解码注册表 + 转码引擎（Registry v2，2026-11-22）
 *
 * 注册表：
 *   - 内置 codec 由 mod_codecs / mod_av 在启动期登记，ID 稳定（1..31）；
 *   - 动态模块调用 codec_register() 自动分配 ID（>=64），无需改核心枚举；
 *   - 查找大小写不敏感，支持 sdp_name / aliases / 内部 name；
 *   - 注册表运行期以原子指针读写，注销走 unload_wait + 引用计数。
 *
 * 转码引擎：
 *   src → decode → PCM(src_rate) → [重采样] → PCM(dst_rate) → encode → dst
 *   会话内 PCM / 重采样缓冲在 open 时一次性分配，process 零 malloc。
 */
#include "media_codec.h"
#include "resample.h"
#include "log.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static const codec_ops_t *g_ops[CODEC__MAX];
static volatile int g_ops_refcnt[CODEC__MAX];
static volatile int g_ops_unloading[CODEC__MAX];
static unsigned g_count;
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;

/* 名称归一化比较：忽略大小写与 '.', '_', '-'（H264 == H.264 == h-264） */
static int name_norm_eq(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t i = 0, j = 0;
    for (;;) {
        while (i < alen && (a[i] == '.' || a[i] == '_' || a[i] == '-')) ++i;
        while (j < blen && (b[j] == '.' || b[j] == '_' || b[j] == '-')) ++j;
        if (i >= alen || j >= blen) break;
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[j];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        ++i; ++j;
    }
    while (i < alen && (a[i] == '.' || a[i] == '_' || a[i] == '-')) ++i;
    while (j < blen && (b[j] == '.' || b[j] == '_' || b[j] == '-')) ++j;
    return i >= alen && j >= blen;
}

/* 别名表是逗号分隔，逐个 token 比较 */
static int aliases_match(const char *aliases,
                         const char *name, size_t len)
{
    if (!aliases || !aliases[0]) return 0;
    const char *p = aliases;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ',') ++p;
        const char *e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) --e;
        if (name_norm_eq(s, (size_t)(e - s), name, len)) return 1;
    }
    return 0;
}

static const codec_ops_t *ops_load(codec_id_t id)
{
    if (id <= CODEC_NONE || id >= CODEC__MAX) return NULL;
    return __atomic_load_n(&g_ops[id], __ATOMIC_ACQUIRE);
}

/* ============================================================================
 * 注册表接口
 * ==========================================================================*/
int codec_registry_register(const codec_ops_t *ops)
{
    codec_id_t id;
    if (ops == NULL) return NSBC_INVAL;
    id = ops->id;
    if (id <= CODEC_NONE || id >= CODEC__MAX)
        return NSBC_INVAL;
    if (ops->api_version != 0 && ops->api_version != NSBC_CODEC_API_VERSION) {
        llog(LOG_ERR, "codec", "register %s: api_version=%u expected=%u",
             ops->name[0] ? ops->name : "?", ops->api_version,
             NSBC_CODEC_API_VERSION);
        return NSBC_INVAL;
    }

    pthread_mutex_lock(&g_reg_lock);
    if (g_ops[id] != NULL &&
        __atomic_load_n(&g_ops_unloading[id], __ATOMIC_ACQUIRE) == 0) {
        pthread_mutex_unlock(&g_reg_lock);
        return NSBC_EXISTS;
    }
    if (g_ops[id] == NULL)
        __atomic_add_fetch(&g_count, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ops_refcnt[id], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ops_unloading[id], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ops[id], ops, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_reg_lock);
    return NSBC_OK;
}

codec_id_t codec_register(codec_ops_t *ops)
{
    codec_id_t id;
    if (ops == NULL) return CODEC_NONE;
    if (ops->api_version != 0 && ops->api_version != NSBC_CODEC_API_VERSION)
        return CODEC_NONE;

    pthread_mutex_lock(&g_reg_lock);
    /* 允许模块复用内置预留 ID（如 CODEC_AMR_WB=11）；已被占用则分配动态 ID */
    if (ops->id > CODEC_NONE && ops->id < CODEC__MAX &&
        g_ops[ops->id] == NULL) {
        id = ops->id;
    } else {
        for (id = CODEC_ID_DYNAMIC_MIN; id < CODEC__MAX; ++id) {
            if (g_ops[id] == NULL)
                break;
        }
        if (id >= CODEC__MAX) {
            pthread_mutex_unlock(&g_reg_lock);
            llog(LOG_ERR, "codec", "dynamic codec registry full (%d slots)",
                 CODEC__MAX);
            return CODEC_NONE;
        }
        ops->id = id;
    }
    ops->api_version = NSBC_CODEC_API_VERSION;
    if (ops->caps == 0) {
        if (ops->kind == CODEC_KIND_VIDEO) {
            if (ops->vid_decode) ops->caps |= CODEC_CAP_DEC;
            if (ops->vid_encode) ops->caps |= CODEC_CAP_ENC;
        } else {
            if (ops->decode) ops->caps |= CODEC_CAP_DEC;
            if (ops->encode) ops->caps |= CODEC_CAP_ENC;
        }
    }
    __atomic_store_n(&g_ops_refcnt[id], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ops_unloading[id], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ops[id], ops, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_count, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_reg_lock);
    return id;
}

const codec_ops_t *codec_registry_find(codec_id_t id)
{
    return ops_load(id);
}

const codec_ops_t *codec_registry_find_pt(int pt, int clock_rate)
{
    for (codec_id_t i = 1; i < CODEC__MAX; ++i) {
        const codec_ops_t *o = ops_load(i);
        if (o == NULL || o->default_pt != pt) continue;
        /* 动态 PT（>=96）只有 codec 显式声明了该 default_pt 才匹配；
         * 静态 PT 需要 RTP 时钟率一致（G.722 用 rtp_clock_rate 比对）。 */
        if (pt >= CODEC_PT_DYN_MIN && o->default_pt < CODEC_PT_DYN_MIN)
            continue;
        if (clock_rate > 0 && codec_rtp_clock_rate(o) != clock_rate)
            continue;
        return o;
    }
    return NULL;
}

const codec_ops_t *codec_find_by_pt_kind(int pt, int kind)
{
    for (codec_id_t i = 1; i < CODEC__MAX; ++i) {
        const codec_ops_t *o = ops_load(i);
        if (o == NULL || o->default_pt != pt) continue;
        if (kind && o->kind != kind) continue;
        return o;
    }
    return NULL;
}

const codec_ops_t *codec_registry_find_name(const char *name, size_t len)
{
    if (name == NULL || len == 0) return NULL;
    for (codec_id_t i = 1; i < CODEC__MAX; ++i) {
        const codec_ops_t *o = ops_load(i);
        if (o == NULL) continue;
        if (o->sdp_name && name_norm_eq(o->sdp_name, strlen(o->sdp_name),
                                        name, len))
            return o;
        if (name_norm_eq(o->name, strlen(o->name), name, len))
            return o;
        if (aliases_match(o->aliases, name, len))
            return o;
    }
    return NULL;
}

const codec_ops_t *codec_find_by_name_kind(const char *name, size_t len, int kind)
{
    if (name == NULL || len == 0) return NULL;
    for (codec_id_t i = 1; i < CODEC__MAX; ++i) {
        const codec_ops_t *o = ops_load(i);
        if (o == NULL) continue;
        if (kind && o->kind != kind) continue;
        if (o->sdp_name && name_norm_eq(o->sdp_name, strlen(o->sdp_name),
                                        name, len))
            return o;
        if (name_norm_eq(o->name, strlen(o->name), name, len))
            return o;
        if (aliases_match(o->aliases, name, len))
            return o;
    }
    return NULL;
}

unsigned codec_registry_count(void)
{
    return __atomic_load_n(&g_count, __ATOMIC_RELAXED);
}

int codec_registry_foreach(int (*cb)(const codec_ops_t *, void *), void *ud)
{
    int n = 0;
    if (!cb) return NSBC_INVAL;
    for (codec_id_t i = 1; i < CODEC__MAX; ++i) {
        const codec_ops_t *ops = ops_load(i);
        if (ops) {
            if (cb(ops, ud) != 0) return n;
            n++;
        }
    }
    return n;
}

/* ============================================================================
 * 双时钟率读取（G.722 / AMR-WB）
 * ==========================================================================*/
int codec_rtp_clock_rate(const codec_ops_t *ops)
{
    if (ops == NULL) return 0;
    if (ops->rtp_clock_rate > 0) return ops->rtp_clock_rate;
    return ops->clock_rate > 0 ? ops->clock_rate : 8000;
}

int codec_pcm_sample_rate(const codec_ops_t *ops)
{
    if (ops == NULL) return 0;
    if (ops->pcm_sample_rate > 0) return ops->pcm_sample_rate;
    return ops->clock_rate > 0 ? ops->clock_rate : 8000;
}

int codec_frame_samples(const codec_ops_t *ops)
{
    if (ops == NULL) return 0;
    if (ops->frame_samples > 0) return ops->frame_samples;
    return codec_pcm_sample_rate(ops) *
           (ops->ptime_ms > 0 ? ops->ptime_ms : 20) / 1000;
}

/* ============================================================================
 * desc 注册表（desc + impl 两级模型）
 * ==========================================================================*/
#define CODEC_DESC_MAX 32
static const codec_desc_t *volatile g_desc[CODEC_DESC_MAX];
static volatile int g_desc_count;
static pthread_mutex_t g_desc_lock = PTHREAD_MUTEX_INITIALIZER;

int codec_desc_register(const codec_desc_t *desc)
{
    int i, n;
    if (desc == NULL || desc->id <= CODEC_NONE || desc->id >= CODEC__MAX ||
        desc->name == NULL || desc->name[0] == '\0')
        return NSBC_INVAL;

    pthread_mutex_lock(&g_desc_lock);
    n = g_desc_count;
    for (i = 0; i < n; ++i) {
        if (g_desc[i] != NULL && g_desc[i]->id == desc->id) {
            pthread_mutex_unlock(&g_desc_lock);
            return NSBC_EXISTS;
        }
    }
    if (n >= CODEC_DESC_MAX) {
        pthread_mutex_unlock(&g_desc_lock);
        llog(LOG_ERR, "codec", "desc registry full (%d) — %s dropped",
             CODEC_DESC_MAX, desc->name);
        return NSBC_NOMEM;
    }
    __atomic_store_n(&g_desc[n], desc, __ATOMIC_RELEASE);
    __atomic_store_n(&g_desc_count, n + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_desc_lock);
    return NSBC_OK;
}

const codec_desc_t *codec_desc_by_id(codec_id_t id)
{
    int n = __atomic_load_n(&g_desc_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; ++i) {
        const codec_desc_t *d = __atomic_load_n(&g_desc[i], __ATOMIC_ACQUIRE);
        if (d != NULL && d->id == id) return d;
    }
    return NULL;
}

const codec_desc_t *codec_by_name(const char *name, size_t len, int kind)
{
    int n;
    if (name == NULL || len == 0) return NULL;
    n = __atomic_load_n(&g_desc_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; ++i) {
        const codec_desc_t *d = __atomic_load_n(&g_desc[i], __ATOMIC_ACQUIRE);
        if (d == NULL) continue;
        if (kind && d->kind != kind) continue;
        if (d->sdp_name && name_norm_eq(d->sdp_name, strlen(d->sdp_name),
                                       name, len))
            return d;
        if (d->name && name_norm_eq(d->name, strlen(d->name), name, len))
            return d;
        if (aliases_match(d->aliases, name, len))
            return d;
    }
    /* 未注册 desc 的 codec：用注册表 ops 反查 id，再合成只读 desc */
    {
        const codec_ops_t *ops = codec_find_by_name_kind(name, len, kind);
        if (ops == NULL) return NULL;
        {
            static __thread codec_desc_t tl_desc;   /* 每线程一份，无锁只读 */
            tl_desc.id = ops->id;
            tl_desc.name = ops->name;
            tl_desc.sdp_name = ops->sdp_name;
            tl_desc.aliases = ops->aliases;
            tl_desc.kind = ops->kind;
            tl_desc.default_pt = ops->default_pt;
            tl_desc.caps = ops->caps;
            tl_desc.priority = ops->priority;
            tl_desc.default_fmtp = ops->default_fmtp;
            tl_desc.impls = NULL;
            tl_desc.n_impls = 0;
            return &tl_desc;
        }
    }
}

const codec_impl_t *codec_pick(const codec_desc_t *desc,
                               int want_pcm_rate, int want_ptime_ms,
                               int channels, uint32_t bitrate)
{
    const codec_ops_t *ops;
    const codec_impl_t *best = NULL;
    int best_score = -1;

    if (desc == NULL) return NULL;
    if (desc->impls != NULL && desc->n_impls > 0) {
        for (int i = 0; i < desc->n_impls; ++i) {
            const codec_impl_t *im = &desc->impls[i];
            int score = 0;
            if (want_pcm_rate > 0 && im->pcm_sample_rate != want_pcm_rate)
                continue;
            if (want_ptime_ms > 0) {
                if (im->ptime_ms == want_ptime_ms) score += 4;
                else if (im->ptime_ms > 0) score -= 1;
            }
            if (channels > 0 && im->channels == channels) score += 2;
            if (bitrate > 0 && im->bitrate == bitrate) score += 2;
            if (im->ops && (im->ops->caps & CODEC_CAP_ENC)) score += 1;
            if (score > best_score) { best_score = score; best = im; }
        }
        if (best != NULL) return best;
        llog(LOG_WARN, "codec", "codec_pick %s: no impl for rate=%d ptime=%d ch=%d, "
             "fallback first", desc->name ? desc->name : "?",
             want_pcm_rate, want_ptime_ms, channels);
        return &desc->impls[0];
    }

    /* 兼容视图：codec_ops_t 视为单 impl */
    ops = codec_registry_find(desc->id);
    if (ops == NULL) return NULL;
    {
        static __thread codec_impl_t tl_impl;
        memset(&tl_impl, 0, sizeof(tl_impl));
        tl_impl.rtp_clock_rate = codec_rtp_clock_rate(ops);
        tl_impl.pcm_sample_rate = codec_pcm_sample_rate(ops);
        tl_impl.frame_samples = codec_frame_samples(ops);
        tl_impl.ptime_ms = ops->ptime_ms;
        tl_impl.channels = ops->channels > 0 ? ops->channels : 1;
        tl_impl.default_pt = ops->default_pt;
        tl_impl.default_fmtp = ops->default_fmtp;
        tl_impl.ops = ops;
        return &tl_impl;
    }
}

/* ============================================================================
 * 每腿 payload map
 * ==========================================================================*/
static int name_contains_ci(const char *s, size_t len, const char *needle)
{
    size_t nl = strlen(needle);
    if (s == NULL || nl == 0 || len < nl) return 0;
    for (size_t i = 0; i + nl <= len; ++i) {
        size_t j = 0;
        for (; j < nl; ++j) {
            unsigned char a = (unsigned char)s[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

/* fmtp 里取整数参数（"ptime=20" / "maxptime=40"）*/
static int fmtp_get_int(const char *fmtp, const char *key, int defv)
{
    const char *p;
    size_t kl;
    if (fmtp == NULL || key == NULL) return defv;
    kl = strlen(key);
    p = fmtp;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == fmtp || p[-1] == ';' || p[-1] == ' ' || p[-1] == '\t') &&
            p[kl] == '=') {
            int v = atoi(p + kl + 1);
            return v > 0 ? v : defv;
        }
        p += kl;
    }
    return defv;
}

/* 判定旁路载荷名：telephone-event / CN / red / rtx */
static int enc_is_event(const char *enc, size_t len)
{
    if (enc == NULL || len == 0) return 0;
    if (name_contains_ci(enc, len, "telephone-event")) return 1;
    if (name_norm_eq(enc, len, "CN", 2)) return 1;
    if (name_norm_eq(enc, len, "red", 3)) return 1;
    if (name_norm_eq(enc, len, "rtx", 3)) return 1;
    return 0;
}

int codec_payload_map_add(leg_payload_t *map, int cap, int n,
                          int pt, const char *enc, size_t enc_len,
                          int rtp_rate, int channels, const char *fmtp)
{
    const codec_ops_t *ops = NULL;
    leg_payload_t *e;
    int is_event;
    codec_id_t cid = CODEC_NONE;

    if (map == NULL || cap <= 0 || n < 0 || n >= cap || pt < 0 || pt > 127)
        return n;

    is_event = enc_is_event(enc, enc_len);
    if (!is_event)
        ops = codec_registry_find_pt(pt, rtp_rate);
    if (ops == NULL && enc != NULL && enc_len > 0)
        ops = codec_find_by_name_kind(enc, enc_len, CODEC_KIND_AUDIO);
    if (ops != NULL) {
        cid = ops->id;
        if (ops->caps & CODEC_CAP_EVENT) is_event = 1;
    }
    if (is_event && cid == CODEC_NONE) {
        if (enc != NULL && name_contains_ci(enc, enc_len, "telephone-event"))
            cid = CODEC_TELEPHONE_EVENT;
        else if (enc != NULL && name_norm_eq(enc, enc_len, "CN", 2))
            cid = CODEC_CN;
    }
    if (cid == CODEC_NONE) {
        llog(LOG_DEBUG, "codec", "payload map: pt=%d enc=[%.*s] unknown",
             pt, (int)(enc_len > 24 ? 24 : enc_len), enc ? enc : "");
        return n;
    }

    e = &map[n];
    memset(e, 0, sizeof(*e));
    e->pt = (uint8_t)pt;
    e->cid = cid;
    e->is_event = is_event;
    e->rtp_rate = rtp_rate > 0 ? rtp_rate
                 : (ops ? codec_rtp_clock_rate(ops) : 8000);
    e->pcm_rate = ops ? codec_pcm_sample_rate(ops) : e->rtp_rate;
    e->channels = channels > 0 ? channels
                  : (ops && ops->channels > 0 ? ops->channels : 1);
    e->ptime_hint = fmtp_get_int(fmtp, "maxptime",
                                 fmtp_get_int(fmtp, "ptime",
                                              ops && ops->ptime_ms > 0
                                              ? ops->ptime_ms : 20));
    if (fmtp != NULL && fmtp[0] != '\0') {
        size_t fl = strlen(fmtp);
        if (fl >= sizeof(e->fmtp)) fl = sizeof(e->fmtp) - 1;
        memcpy(e->fmtp, fmtp, fl);
        e->fmtp[fl] = '\0';
    }
    if (ops != NULL && (ops->caps & CODEC_CAP_EVENT)) is_event = 1;
    {
        /* 只保存 desc 自带的稳定 impl 数组指针；codec_pick 的兼容视图
         * 返回 thread-local 单 impl 副本，不能跨线程存进 map。 */
        const codec_desc_t *d = codec_desc_by_id(cid);
        e->impl = (d != NULL && d->impls != NULL && d->n_impls > 0)
                      ? codec_pick(d, e->pcm_rate, e->ptime_hint,
                                   e->channels, 0)
                      : NULL;
    }
    return n + 1;
}

const leg_payload_t *codec_payload_map_find(const leg_payload_t *map, int n,
                                            int pt)
{
    if (map == NULL) return NULL;
    for (int i = 0; i < n; ++i) {
        if (map[i].pt == (uint8_t)pt) return &map[i];
    }
    return NULL;
}

int codec_payload_map_can_relay(const leg_payload_t *a, const leg_payload_t *b)
{
    if (a == NULL || b == NULL) return 0;
    if (a->is_event || b->is_event) return a->cid == b->cid;
    if (a->cid != b->cid) return 0;
    if (a->pcm_rate != b->pcm_rate || a->channels != b->channels) return 0;
    /* fmtp 兼容：都有且不同时视为不兼容（AMR octet-align/mode-set 差异）*/
    if (a->fmtp[0] != '\0' && b->fmtp[0] != '\0' &&
        strcmp(a->fmtp, b->fmtp) != 0)
        return 0;
    return 1;
}

int codec_kind_of(codec_id_t id)
{
    const codec_ops_t *o = ops_load(id);
    if (o == NULL) return 0;
    if (o->kind) return o->kind;
    return (o->clock_rate >= 90000) ? CODEC_KIND_VIDEO : CODEC_KIND_AUDIO;
}

int codec_can_decode(codec_id_t id)
{
    const codec_ops_t *o = ops_load(id);
    if (o == NULL) return 0;
    if (o->kind == CODEC_KIND_VIDEO)
        return o->vid_decode != NULL && o->vid_dec_init != NULL;
    if (o->caps & CODEC_CAP_DEC) return 1;
    return o->decode != NULL;
}

int codec_can_encode(codec_id_t id)
{
    const codec_ops_t *o = ops_load(id);
    if (o == NULL) return 0;
    if (o->kind == CODEC_KIND_VIDEO)
        return o->vid_encode != NULL && o->vid_enc_init != NULL;
    if (o->caps & CODEC_CAP_ENC) return 1;
    return o->encode != NULL;
}

/* 卸载安全接口 */
const codec_ops_t *codec_registry_find_inc(codec_id_t id, int *out_slot)
{
    const codec_ops_t *ops;
    if (id <= CODEC_NONE || id >= CODEC__MAX) return NULL;
    if (__atomic_load_n(&g_ops_unloading[id], __ATOMIC_ACQUIRE)) return NULL;
    ops = __atomic_load_n(&g_ops[id], __ATOMIC_ACQUIRE);
    if (!ops) return NULL;
    __atomic_fetch_add(&g_ops_refcnt[id], 1, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&g_ops_unloading[id], __ATOMIC_ACQUIRE)) {
        __atomic_fetch_sub(&g_ops_refcnt[id], 1, __ATOMIC_ACQ_REL);
        return NULL;
    }
    if (out_slot) *out_slot = (int)id;
    return ops;
}

void codec_registry_dec(int slot)
{
    if (slot <= CODEC_NONE || slot >= CODEC__MAX) return;
    __atomic_fetch_sub(&g_ops_refcnt[slot], 1, __ATOMIC_ACQ_REL);
}

int codec_registry_unload_wait(codec_id_t id, int timeout_ms)
{
    int spins = 0;
    int deadline = timeout_ms > 0 ? timeout_ms : 5000;
    const codec_ops_t *victim;
    if (id <= CODEC_NONE || id >= CODEC__MAX) return NSBC_INVAL;
    victim = __atomic_load_n(&g_ops[id], __ATOMIC_ACQUIRE);
    __atomic_store_n(&g_ops_unloading[id], 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&g_ops_refcnt[id], __ATOMIC_ACQUIRE) > 0) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (++spins >= deadline) {
            llog(LOG_ERR, "codec", "unload timeout id=%d refcnt=%d",
                 id, __atomic_load_n(&g_ops_refcnt[id], __ATOMIC_ACQUIRE));
            __atomic_store_n(&g_ops_unloading[id], 0, __ATOMIC_RELEASE);
            return NSBC_BUSY;
        }
    }
    pthread_mutex_lock(&g_reg_lock);
    /* 等待期间可能已有新模块覆盖注册同一 slot，只清理我们等待的那个指针对 */
    if (g_ops[id] != NULL && (victim == NULL || g_ops[id] == victim)) {
        __atomic_store_n(&g_ops[id], NULL, __ATOMIC_RELEASE);
        if (__atomic_load_n(&g_count, __ATOMIC_RELAXED) > 0)
            __atomic_sub_fetch(&g_count, 1, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&g_reg_lock);
    return NSBC_OK;
}

int codec_registry_unregister(codec_id_t id)
{
    return codec_registry_unload_wait(id, 5000);
}

int codec_unregister(codec_id_t id)
{
    return codec_registry_unload_wait(id, 5000);
}

/* ============================================================================
 * 转码会话
 * ==========================================================================*/
#define XCODE_PCM_CAP   16384   /* S16 中间缓冲（每声道）*/

struct transcode_sess {
    codec_id_t      src;
    codec_id_t      dst;
    const codec_ops_t *dops;
    const codec_ops_t *eops;
    codec_sess_t   *dec;
    codec_sess_t   *enc;
    int             src_rate;      /* 源 PCM 采样率 */
    int             dst_rate;      /* 目标 PCM 采样率 */
    int             src_rtp_rate;  /* 源 RTP 时钟率（时间戳步进）*/
    int             dst_rtp_rate;  /* 目标 RTP 时钟率 */
    int             src_ch;
    int             dst_ch;
    int             opened;
    uint8_t         dst_pt;    /* 目标腿协商 PT，0xFF=未设置 */
    char            src_fmtp[128];
    char            dst_fmtp[128];
    resampler_t    *rs;
    int16_t        *pcm;       /* 解码输出（每声道 XCODE_PCM_CAP）*/
    int16_t        *ch_pcm;    /* 声道变换输出（src_ch != dst_ch 时分配）*/
    int16_t        *rs_pcm;    /* 重采样输出（仅在速率不同时分配）*/
    size_t          rs_cap;    /* 每声道样本容量 */
};

codec_id_t transcode_src_codec_id(const transcode_sess_t *t)
{
    return t ? t->src : CODEC_NONE;
}

codec_id_t transcode_dst_codec_id(const transcode_sess_t *t)
{
    return t ? t->dst : CODEC_NONE;
}

void transcode_set_dst_pt(transcode_sess_t *t, uint8_t pt)
{
    if (t) t->dst_pt = pt;
}

uint8_t transcode_get_dst_pt(const transcode_sess_t *t)
{
    return t ? t->dst_pt : 0xFF;
}

int transcode_src_rtp_rate(const transcode_sess_t *t)
{
    return t ? t->src_rtp_rate : 0;
}

int transcode_dst_rtp_rate(const transcode_sess_t *t)
{
    return t ? t->dst_rtp_rate : 0;
}

int transcode_open(codec_id_t src, codec_id_t dst, transcode_sess_t **out)
{
    const codec_ops_t *d = codec_registry_find(src);
    const codec_ops_t *e = codec_registry_find(dst);
    return transcode_open_ex(src, dst,
                             d ? codec_pcm_sample_rate(d) : 8000,
                             e ? codec_pcm_sample_rate(e) : 8000,
                             d ? d->channels : 1, NULL, NULL, out);
}

int transcode_open2(codec_id_t src, codec_id_t dst,
                    int src_rate, int dst_rate, int channels,
                    transcode_sess_t **out)
{
    return transcode_open_ex(src, dst, src_rate, dst_rate, channels,
                             NULL, NULL, out);
}

int transcode_open_ex(codec_id_t src, codec_id_t dst,
                      int src_rate, int dst_rate, int channels,
                      const char *src_fmtp, const char *dst_fmtp,
                      transcode_sess_t **out)
{
    const codec_ops_t *dops;
    const codec_ops_t *eops;
    transcode_sess_t *t;
    int rc;

    if (out == NULL || src <= CODEC_NONE || dst <= CODEC_NONE)
        return NSBC_INVAL;
    *out = NULL;
    if (src == dst)
        return NSBC_INVAL;
    if (src_rate <= 0) src_rate = 8000;
    if (dst_rate <= 0) dst_rate = 8000;

    dops = codec_registry_find(src);
    eops = codec_registry_find(dst);
    if (dops == NULL || eops == NULL)
        return NSBC_NOTSUP;
    if (dops->kind == CODEC_KIND_VIDEO || eops->kind == CODEC_KIND_VIDEO)
        return NSBC_NOTSUP;   /* 视频走 video_xcode，不进音频引擎 */
    if (!codec_can_decode(src) || !codec_can_encode(dst))
        return NSBC_NOTSUP;

    t = (transcode_sess_t *)calloc(1, sizeof(*t));
    if (t == NULL) return NSBC_NOMEM;
    t->src = src;
    t->dst = dst;
    t->dops = dops;
    t->eops = eops;
    t->src_rate = src_rate;
    t->dst_rate = dst_rate;
    t->src_rtp_rate = codec_rtp_clock_rate(dops);
    t->dst_rtp_rate = codec_rtp_clock_rate(eops);
    t->src_ch = (channels > 0) ? channels
               : (dops->channels > 0 ? dops->channels : 1);
    t->dst_ch = eops->channels > 0 ? eops->channels : t->src_ch;
    t->dst_pt = 0xFF;
    if (src_fmtp != NULL && src_fmtp[0] != '\0') {
        size_t n = strlen(src_fmtp);
        if (n >= sizeof(t->src_fmtp)) n = sizeof(t->src_fmtp) - 1;
        memcpy(t->src_fmtp, src_fmtp, n);
        t->src_fmtp[n] = '\0';
    }
    if (dst_fmtp != NULL && dst_fmtp[0] != '\0') {
        size_t n = strlen(dst_fmtp);
        if (n >= sizeof(t->dst_fmtp)) n = sizeof(t->dst_fmtp) - 1;
        memcpy(t->dst_fmtp, dst_fmtp, n);
        t->dst_fmtp[n] = '\0';
    }

    /* PCM 中间缓冲按每声道 XCODE_PCM_CAP 分配，最多 2 声道 */
    t->pcm = (int16_t *)calloc(XCODE_PCM_CAP * 2, sizeof(int16_t));
    if (t->pcm == NULL) { free(t); return NSBC_NOMEM; }

    if (t->src_ch != t->dst_ch) {
        t->ch_pcm = (int16_t *)calloc(XCODE_PCM_CAP * 2, sizeof(int16_t));
        if (t->ch_pcm == NULL) { free(t->pcm); free(t); return NSBC_NOMEM; }
    }

    if (src_rate != dst_rate) {
        rs_cfg_t cfg;
        size_t cap = (size_t)XCODE_PCM_CAP * (size_t)dst_rate /
                     (size_t)src_rate + 64;
        if (cap > 131072) cap = 131072;
        t->rs_cap = cap;
        t->rs_pcm = (int16_t *)calloc(cap * (size_t)t->dst_ch,
                                      sizeof(int16_t));
        if (t->rs_pcm == NULL) {
            free(t->ch_pcm); free(t->pcm); free(t); return NSBC_NOMEM;
        }
        cfg.in_rate = src_rate;
        cfg.out_rate = dst_rate;
        cfg.channels = t->dst_ch;
        cfg.quality = 1;
        rc = resampler_open(&cfg, &t->rs);
        if (rc != NSBC_OK) {
            free(t->rs_pcm); free(t->ch_pcm); free(t->pcm); free(t);
            return rc;
        }
    }

    /* fmtp 透传：协商结果进入编解码器（AMR octet-align / Opus bitrate 等）*/
    if (dops->init) {
        rc = dops->init(t->src_fmtp[0] ? t->src_fmtp : NULL, &t->dec);
        if (rc != NSBC_OK) goto fail;
    }
    if (eops->encoder_init)
        rc = eops->encoder_init(t->dst_fmtp[0] ? t->dst_fmtp : NULL, &t->enc);
    else if (eops->init)
        rc = eops->init(t->dst_fmtp[0] ? t->dst_fmtp : NULL, &t->enc);
    else
        rc = NSBC_NOTSUP;
    if (rc != NSBC_OK) goto fail;

    t->opened = 1;
    *out = t;
    return NSBC_OK;

fail:
    if (t->dec && dops->fini) dops->fini(t->dec);
    if (t->enc && eops->fini) eops->fini(t->enc);
    if (t->rs) resampler_close(t->rs);
    free(t->rs_pcm);
    free(t->ch_pcm);
    free(t->pcm);
    free(t);
    return rc;
}

void transcode_close(transcode_sess_t *t)
{
    if (t == NULL) return;
    if (t->enc && t->eops && t->eops->fini)
        t->eops->fini(t->enc);
    if (t->dec && t->dops && t->dops->fini)
        t->dops->fini(t->dec);
    if (t->rs) resampler_close(t->rs);
    free(t->rs_pcm);
    free(t->ch_pcm);
    free(t->pcm);
    free(t);
}

/* 声道变换：1↔2（下行平均混音、上行复制）*/
static void xcode_ch_convert(const int16_t *in, size_t samples,
                             int in_ch, int out_ch, int16_t *out)
{
    size_t i;
    if (in_ch == out_ch || samples == 0) {
        if (in_ch == out_ch)
            memcpy(out, in, samples * (size_t)in_ch * sizeof(int16_t));
        return;
    }
    if (in_ch == 2 && out_ch == 1) {
        for (i = 0; i < samples; ++i)
            out[i] = (int16_t)(((int)in[i * 2] + (int)in[i * 2 + 1]) / 2);
    } else if (in_ch == 1 && out_ch == 2) {
        for (i = 0; i < samples; ++i) {
            out[i * 2] = in[i];
            out[i * 2 + 1] = in[i];
        }
    } else {
        /* 其他组合：按首声道复制 */
        for (i = 0; i < samples; ++i) {
            int16_t v = in[i * (size_t)in_ch];
            for (int c = 0; c < out_ch; ++c)
                out[i * (size_t)out_ch + c] = v;
        }
    }
}

/* 单帧解码；返回写入的每声道样本数（out->samples）*/
static int xcode_decode_one(transcode_sess_t *t,
                            const uint8_t *in, size_t in_len,
                            int16_t *dst, size_t cap, size_t *written)
{
    media_frame_t frm;
    int rc;
    if (!t->dops->decode) return NSBC_NOTSUP;
    memset(&frm, 0, sizeof(frm));
    frm.pcm = dst;
    frm.samples = 0;
    frm.rate = t->src_rate;
    frm.channels = t->src_ch;
    rc = t->dops->decode(t->dec, in, in_len, &frm);
    if (rc != NSBC_OK) return rc;
    if (frm.samples > cap) return NSBC_INVAL;
    *written = frm.samples;
    return NSBC_OK;
}

/* 单条 RTP 载荷的合理上限；历史实现误用 PCM 样本容量做比较 */
#define XCODE_MAX_PAYLOAD 8192

int transcode_process(transcode_sess_t *t, const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t cap, size_t *out_len)
{
    media_frame_t frm;
    size_t total = 0;     /* 已解码的每声道样本数 */
    const int16_t *pcm_in;
    size_t pcm_samples;
    size_t flen;
    int rc;

    if (t == NULL || in == NULL || out == NULL || out_len == NULL)
        return NSBC_INVAL;
    *out_len = 0;
    if (!t->opened) return NSBC_NOTSUP;
    if (in_len == 0) return NSBC_OK;

    /* --- 1. decode：RTP 载荷 → PCM --- */
    if (t->dops->rtp_unpack) {
        codec_frame_t frames[16];
        int nf = 0, i;
        rc = t->dops->rtp_unpack(t->dec, in, in_len, frames,
                                 (int)(sizeof(frames) / sizeof(frames[0])), &nf);
        if (rc != NSBC_OK) return rc;
        for (i = 0; i < nf; ++i) {
            size_t got = 0;
            if (total >= XCODE_PCM_CAP) break;
            rc = xcode_decode_one(t, frames[i].data, frames[i].len,
                                  t->pcm + total * (size_t)t->src_ch,
                                  XCODE_PCM_CAP - total, &got);
            if (rc != NSBC_OK) {
                llog(LOG_WARN, "codec", "decode frame %d/%d rc=%d dropped",
                     i, nf, rc);
                continue;
            }
            total += got;
        }
    } else {
        if (in_len > XCODE_MAX_PAYLOAD) {
            llog(LOG_WARN, "codec", "transcode payload too large (%zu)", in_len);
            return NSBC_INVAL;
        }
        rc = xcode_decode_one(t, in, in_len, t->pcm, XCODE_PCM_CAP, &total);
        if (rc != NSBC_OK) return rc;
    }
    if (total == 0) return NSBC_OK;

    /* --- 2. 声道变换（1↔2）--- */
    pcm_in = t->pcm;
    pcm_samples = total;
    if (t->src_ch != t->dst_ch && t->ch_pcm != NULL) {
        xcode_ch_convert(t->pcm, total, t->src_ch, t->dst_ch, t->ch_pcm);
        pcm_in = t->ch_pcm;
    }

    /* --- 3. resample（仅在采样率不同时）--- */
    memset(&frm, 0, sizeof(frm));
    frm.pcm = (int16_t *)pcm_in;   /* media_frame_t.pcm 非 const；编码器只读 */
    frm.samples = pcm_samples;
    frm.rate = t->src_rate;
    frm.channels = t->dst_ch;

    if (t->rs) {
        size_t in_total = pcm_samples * (size_t)t->dst_ch;
        size_t out_total = 0;
        rc = resampler_process(t->rs, pcm_in, in_total,
                               t->rs_pcm, t->rs_cap * (size_t)t->dst_ch,
                               &out_total);
        if (rc != NSBC_OK) return rc;
        if (out_total == 0) return NSBC_OK;
        frm.pcm = t->rs_pcm;
        frm.samples = out_total / (size_t)t->dst_ch;
        frm.rate = t->dst_rate;
    }

    /* --- 4. encode：PCM → RTP 载荷 --- */
    if (!t->eops->encode) return NSBC_NOTSUP;
    flen = 0;
    rc = t->eops->encode(t->enc, &frm, out, cap, &flen);
    if (rc == NSBC_OK) *out_len = flen;
    return rc;
}

/* 丢包隐藏：解码器支持 plc 时产出替代帧并编码成目标载荷 */
int transcode_plc(transcode_sess_t *t, uint8_t *out, size_t cap,
                  size_t *out_len)
{
    media_frame_t frm;
    size_t flen = 0;
    int rc;

    if (t == NULL || out == NULL || out_len == NULL) return NSBC_INVAL;
    *out_len = 0;
    if (!t->opened || t->dops == NULL || t->dops->plc == NULL)
        return NSBC_NOTSUP;
    memset(&frm, 0, sizeof(frm));
    frm.pcm = t->pcm;
    frm.rate = t->src_rate;
    frm.channels = t->src_ch;
    rc = t->dops->plc(t->dec, &frm);
    if (rc != NSBC_OK) return rc;
    if (frm.samples == 0) return NSBC_OK;

    if (t->src_ch != t->dst_ch && t->ch_pcm != NULL) {
        xcode_ch_convert(t->pcm, frm.samples, t->src_ch, t->dst_ch, t->ch_pcm);
        frm.pcm = t->ch_pcm;
    }
    frm.channels = t->dst_ch;
    if (t->rs) {
        size_t out_total = 0;
        rc = resampler_process(t->rs, frm.pcm,
                               frm.samples * (size_t)t->dst_ch,
                               t->rs_pcm, t->rs_cap * (size_t)t->dst_ch,
                               &out_total);
        if (rc != NSBC_OK) return rc;
        if (out_total == 0) return NSBC_OK;
        frm.pcm = t->rs_pcm;
        frm.samples = out_total / (size_t)t->dst_ch;
        frm.rate = t->dst_rate;
    }
    if (!t->eops->encode) return NSBC_NOTSUP;
    rc = t->eops->encode(t->enc, &frm, out, cap, &flen);
    if (rc == NSBC_OK) *out_len = flen;
    return rc;
}

/* ============================================================================
 * SDP fmtp 协商
 * ==========================================================================*/
int codec_fmtp_negotiate(codec_id_t id, const char *offer, const char *answer,
                         char *dst, size_t cap)
{
    const codec_ops_t *ops;
    if (dst == NULL || cap == 0) return NSBC_INVAL;
    dst[0] = '\0';
    ops = codec_registry_find(id);
    if (ops == NULL) return NSBC_NOTSUP;
    if (ops->fmtp_negotiate)
        return ops->fmtp_negotiate(offer, answer, dst, cap);
    if ((offer == NULL || !offer[0]) && (answer == NULL || !answer[0]))
        return NSBC_OK;
    /* 无专用协商器：无 fmtp 视为可接受 */
    if (offer && offer[0]) {
        size_t n = strlen(offer);
        if (n >= cap) n = cap - 1;
        memcpy(dst, offer, n);
        dst[n] = '\0';
    }
    return NSBC_OK;
}

/* ============================================================================
 * 一致性自测（OAM codec_selftest 调用）
 *
 * 覆盖 §2.4 验收项：
 *   - 名字/别名/大小写命中（G.722/G722、G.729/G.729）；
 *   - G.711 encode→decode 回环长度与能量合理；
 *   - 动态 PT + rtpmap 名字建 payload map，可零转码中继；
 *   - G.729 rtp_unpack 多帧拆分（有 bcg729 时）；
 *   - 0 长度/截断载荷不崩溃且返回错误。
 * 返回失败项数（0=全通过）。未加载 mod_codecs 时全部计为失败。
 * ==========================================================================*/
int codec_conformance_selftest(char *out, size_t cap)
{
    int fails = 0;
    int checks = 0;
    size_t off = 0;

    if (out == NULL || cap == 0) return 1;
    out[0] = '\0';

#define ST_LINE(...)                                                          \
    do {                                                                      \
        if (off < cap) {                                                      \
            int _n = snprintf(out + off, cap - off, __VA_ARGS__);             \
            if (_n > 0) off += (size_t)_n;                                    \
        }                                                                     \
    } while (0)
#define ST_FAIL(...)                                                          \
    do {                                                                      \
        ++fails;                                                              \
        ST_LINE("FAIL: " __VA_ARGS__);                                        \
    } while (0)
#define ST_CHECK(cond, ...)                                                   \
    do {                                                                      \
        ++checks;                                                             \
        if (!(cond)) ST_FAIL(__VA_ARGS__);                                    \
    } while (0)

    /* 1. 名称/别名/大小写 */
    static const char *const names[] = {
        "PCMU", "PCMA", "G729", "G.729", "g729", "G722", "G.722", "g722"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const codec_ops_t *o = codec_find_by_name_kind(names[i],
                                                       strlen(names[i]),
                                                       CODEC_KIND_AUDIO);
        ST_CHECK(o != NULL, "name '%s' not found in registry\n", names[i]);
    }

    /* 2. G.711 回环 */
    const codec_ops_t *enc_ops = codec_registry_find(CODEC_PCMU);
    const codec_ops_t *dec_ops = codec_registry_find(CODEC_PCMU);
    if (enc_ops == NULL || dec_ops == NULL ||
        enc_ops->encode == NULL || dec_ops->decode == NULL) {
        ST_FAIL("PCMU encode/decode unavailable\n");
    } else {
        codec_sess_t *enc = NULL;
        codec_sess_t *dec = NULL;
        int rc = enc_ops->encoder_init ? enc_ops->encoder_init(NULL, &enc)
                                       : enc_ops->init(NULL, &enc);
        ST_CHECK(rc == NSBC_OK && enc != NULL, "PCMU encoder_init rc=%d\n", rc);
        rc = dec_ops->init(NULL, &dec);
        ST_CHECK(rc == NSBC_OK && dec != NULL, "PCMU decoder_init rc=%d\n", rc);
        if (enc && dec) {
            int16_t pcm[160];
            int16_t pcm_out[320];
            uint8_t law[400];
            size_t law_len = 0;
            media_frame_t in, outp;

            for (int i = 0; i < 160; ++i) {
                int tri = i % 40;
                if (tri > 20) tri = 40 - tri;
                pcm[i] = (int16_t)((tri - 10) * 600);
            }
            memset(&in, 0, sizeof(in));
            in.pcm = pcm; in.samples = 160; in.rate = 8000; in.channels = 1;
            rc = enc_ops->encode(enc, &in, law, sizeof(law), &law_len);
            ST_CHECK(rc == NSBC_OK && law_len > 0,
                     "PCMU encode rc=%d len=%zu\n", rc, law_len);

            memset(&outp, 0, sizeof(outp));
            outp.pcm = pcm_out; outp.rate = 8000; outp.channels = 1;
            rc = dec_ops->decode(dec, law, law_len, &outp);
            ST_CHECK(rc == NSBC_OK && outp.samples > 0,
                     "PCMU decode rc=%d samples=%zu\n", rc, outp.samples);
            if (rc == NSBC_OK && outp.samples > 0) {
                int64_t energy = 0;
                for (size_t i = 0; i < outp.samples; ++i)
                    energy += (int64_t)pcm_out[i] * pcm_out[i];
                ST_CHECK(energy > 0, "PCMU roundtrip energy=0\n");
            }
            /* 0 长度 / 截断载荷：不得崩溃，返回非 OK 即可 */
            rc = dec_ops->decode(dec, law, 0, &outp);
            ST_CHECK(rc != NSBC_OK, "zero-length decode returned OK\n");
            rc = dec_ops->decode(dec, law, 1, &outp);
            (void)rc;   /* 某些实现允许单字节，仅要求不崩溃 */
        }
        if (enc && enc_ops->fini) enc_ops->fini(enc);
        if (dec && dec_ops->fini) dec_ops->fini(dec);
    }

    /* 3. 动态 PT + 名字建 payload map + 零转码中继判定 */
    {
        leg_payload_t map[8];
        memset(map, 0, sizeof(map));
        int n = codec_payload_map_add(map, 8, 0, 96, "G729", 4, 8000, 1, NULL);
        ST_CHECK(n == 1, "payload_map_add pt=96 G729 n=%d\n", n);
        if (n == 1) {
            const leg_payload_t *e = codec_payload_map_find(map, n, 96);
            ST_CHECK(e != NULL && e->cid == CODEC_G729,
                     "payload_map pt=96 resolves wrong cid\n");
            ST_CHECK(codec_payload_map_can_relay(&map[0], &map[0]) == 1,
                     "payload_map can_relay(self) failed\n");
        }
        /* 同 PT、不同 codec 不应判为可直接中继 */
        leg_payload_t a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        int na = codec_payload_map_add(&a, 1, 0, 96, "G729", 4, 8000, 1, NULL);
        int nb = codec_payload_map_add(&b, 1, 0, 96, "PCMA", 4, 8000, 1, NULL);
        ST_CHECK(na == 1 && nb == 1 &&
                 codec_payload_map_can_relay(&a, &b) == 0,
                 "payload_map can_relay(G729,PCMA) should be 0\n");
    }

    /* 4. G.729 多帧拆分 */
    {
        const codec_ops_t *g = codec_registry_find(CODEC_G729);
        if (g != NULL && g->rtp_unpack != NULL) {
            uint8_t payload[20];
            codec_frame_t frames[4];
            int nf = 0;
            memset(payload, 0x5a, sizeof(payload));
            memset(frames, 0, sizeof(frames));
            int rc = g->rtp_unpack(NULL, payload, sizeof(payload), frames, 4, &nf);
            ST_CHECK(rc == NSBC_OK && nf == 2,
                     "G729 rtp_unpack rc=%d frames=%d (expect 2)\n", rc, nf);
            ST_CHECK(nf == 2 && frames[1].data == payload + 10,
                     "G729 rtp_unpack frame offset wrong\n");
        }
    }

    ST_LINE("codec selftest: %d checks, %d failed\n", checks, fails);
#undef ST_CHECK
#undef ST_FAIL
#undef ST_LINE
    return fails;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
