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
        ++g_count;
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
    ++g_count;
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
        if (o != NULL && o->default_pt == pt && pt < CODEC_PT_DYN_MIN &&
            (clock_rate <= 0 || o->clock_rate == clock_rate))
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
    return g_count;
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
        if (g_count > 0) --g_count;
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
    int             src_rate;
    int             dst_rate;
    int             channels;
    int             opened;
    uint8_t         dst_pt;    /* 目标腿协商 PT，0xFF=未设置 */
    resampler_t    *rs;
    int16_t        *pcm;       /* 解码输出 */
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

int transcode_open(codec_id_t src, codec_id_t dst, transcode_sess_t **out)
{
    const codec_ops_t *d = codec_registry_find(src);
    const codec_ops_t *e = codec_registry_find(dst);
    return transcode_open2(src, dst,
                           d ? d->clock_rate : 8000,
                           e ? e->clock_rate : 8000,
                           d ? d->channels : 1, out);
}

int transcode_open2(codec_id_t src, codec_id_t dst,
                    int src_rate, int dst_rate, int channels,
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
    if (channels <= 0) channels = 1;
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
    if (dops->channels && eops->channels && dops->channels != eops->channels)
        return NSBC_NOTSUP;   /* 暂不做声道变换 */

    t = (transcode_sess_t *)calloc(1, sizeof(*t));
    if (t == NULL) return NSBC_NOMEM;
    t->src = src;
    t->dst = dst;
    t->dops = dops;
    t->eops = eops;
    t->src_rate = src_rate;
    t->dst_rate = dst_rate;
    t->channels = channels;
    t->dst_pt = 0xFF;
    t->pcm = (int16_t *)calloc(XCODE_PCM_CAP, sizeof(int16_t));
    if (t->pcm == NULL) { free(t); return NSBC_NOMEM; }

    if (src_rate != dst_rate) {
        rs_cfg_t cfg;
        size_t cap = (size_t)XCODE_PCM_CAP * (size_t)dst_rate /
                     (size_t)src_rate + 64;
        if (cap > 131072) cap = 131072;
        t->rs_cap = cap;
        t->rs_pcm = (int16_t *)calloc(cap, sizeof(int16_t));
        if (t->rs_pcm == NULL) { free(t->pcm); free(t); return NSBC_NOMEM; }
        cfg.in_rate = src_rate;
        cfg.out_rate = dst_rate;
        cfg.channels = channels;
        cfg.quality = 1;
        rc = resampler_open(&cfg, &t->rs);
        if (rc != NSBC_OK) {
            free(t->rs_pcm); free(t->pcm); free(t);
            return rc;
        }
    }

    if (dops->init) {
        rc = dops->init(NULL, &t->dec);
        if (rc != NSBC_OK) goto fail;
    }
    if (eops->encoder_init)
        rc = eops->encoder_init(NULL, &t->enc);
    else if (eops->init)
        rc = eops->init(NULL, &t->enc);
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
    free(t->pcm);
    free(t);
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
    frm.channels = t->channels;
    rc = t->dops->decode(t->dec, in, in_len, &frm);
    if (rc != NSBC_OK) return rc;
    if (frm.samples > cap) return NSBC_INVAL;
    *written = frm.samples;
    return NSBC_OK;
}

int transcode_process(transcode_sess_t *t, const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t cap, size_t *out_len)
{
    media_frame_t frm;
    size_t total = 0;     /* 已解码的每声道样本数 */
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
                                  t->pcm + total,
                                  XCODE_PCM_CAP - total, &got);
            if (rc != NSBC_OK) {
                llog(LOG_WARN, "codec", "decode frame %d/%d rc=%d dropped",
                     i, nf, rc);
                continue;
            }
            total += got;
        }
    } else {
        if (in_len > XCODE_PCM_CAP) {
            llog(LOG_WARN, "codec", "transcode payload too large (%zu)", in_len);
            return NSBC_INVAL;
        }
        rc = xcode_decode_one(t, in, in_len, t->pcm, XCODE_PCM_CAP, &total);
        if (rc != NSBC_OK) return rc;
    }
    if (total == 0) return NSBC_OK;

    /* --- 2. resample（仅在采样率不同时）--- */
    memset(&frm, 0, sizeof(frm));
    frm.pcm = t->pcm;
    frm.samples = total;
    frm.rate = t->src_rate;
    frm.channels = t->channels;

    if (t->rs) {
        size_t in_total = total * (size_t)t->channels;
        size_t out_total = 0;
        rc = resampler_process(t->rs, t->pcm, in_total,
                               t->rs_pcm, t->rs_cap * (size_t)t->channels,
                               &out_total);
        if (rc != NSBC_OK) return rc;
        if (out_total == 0) return NSBC_OK;
        frm.pcm = t->rs_pcm;
        frm.samples = out_total / (size_t)t->channels;
        frm.rate = t->dst_rate;
    }

    /* --- 3. encode：PCM → RTP 载荷 --- */
    if (!t->eops->encode) return NSBC_NOTSUP;
    flen = 0;
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

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
