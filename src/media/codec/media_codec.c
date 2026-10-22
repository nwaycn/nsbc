/* 编解码注册表 + 转码引擎（REQ-TRANS-001/004/005，P0 完整实现）
 *
 * 注册表：启动期写、运行期只读的静态指针数组（按 codec_id 直达索引）
 *         G.711 内置自注册（g711_register 在 bridge_mgr_create 调）；
 *         G.729/AMR 等商业 codec 由 src/mod/codecs 描述符登记。
 *
 * 转码引擎：src→dst 经过 decode → PCM → encode 流水线
 *         （G.711↔G.711 可走直转路径：decode 后 encode，中间都是 8k mono，
 *          PCM buffer 大小 = RTP payload_len * sizeof(int16_t)）。
 *
 * 转码会话 per-direction：一条 transcode_sess_t 处理 src_codec → dst_codec
 * 的实时转码；G.711 PCMA↔PCMU 场景下 payload size 不变（都是 1 byte/sample），
 * 所以 RTP header 的 marker/seq/timestamp 保持原样即可。
 */
#include "media_codec.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const codec_ops_t *g_ops[CODEC__MAX];
static unsigned g_count;

/* Phase 1 SAFE1: 安全卸载并行字段（每个 codec slot 一条）*/
static volatile int g_ops_refcnt[CODEC__MAX];     /* 当前 in-flight 转码会话数 */
static volatile int g_ops_unloading[CODEC__MAX];  /* 卸载标记：1=阻止新调用 */

/* ============================================================================
 * 注册表接口
 * ==========================================================================*/
int codec_registry_register(const codec_ops_t *ops)
{
    if (ops == NULL || ops->id <= CODEC_NONE || ops->id >= CODEC__MAX)
        return NSBC_INVAL;
    /* Phase 1 SAFE1: unloading 状态的 slot 可以被同名新 codec 覆盖注册
     * （动态模块卸载后可以重新加载同 codec） */
    if (g_ops[ops->id] != NULL
        && __atomic_load_n(&g_ops_unloading[ops->id], __ATOMIC_ACQUIRE) == 0)
        return NSBC_EXISTS;
    g_ops[ops->id] = ops;
    __atomic_store_n(&g_ops_refcnt[ops->id], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ops_unloading[ops->id], 0, __ATOMIC_RELAXED);
    ++g_count;
    return NSBC_OK;
}

const codec_ops_t *codec_registry_find(codec_id_t id)
{
    if (id <= CODEC_NONE || id >= CODEC__MAX)
        return NULL;
    return g_ops[id];
}

const codec_ops_t *codec_registry_find_pt(int pt, int clock_rate)
{
    int i;
    /* 静态 PT 直查（G.711 PCMU=0 / PCMA=8 都在这里命中）；
     * 动态 PT（96+）需 SDP rtpmap 反查，接线时由 SDP 协商层建立
     * per-session PT→id 映射后直传 id，不走这里。 */
    (void)clock_rate;
    for (i = 1; i < CODEC__MAX; ++i) {
        if (g_ops[i] != NULL && g_ops[i]->default_pt == pt && pt < CODEC_PT_DYN_MIN)
            return g_ops[i];
    }
    return NULL;
}

const codec_ops_t *codec_registry_find_name(const char *name, size_t len)
{
    int i;
    if (name == NULL || len == 0 || len >= CODEC_NAME_MAX)
        return NULL;
    for (i = 1; i < CODEC__MAX; ++i) {
        if (g_ops[i] != NULL && strlen(g_ops[i]->name) == len &&
            memcmp(g_ops[i]->name, name, len) == 0)
            return g_ops[i];
    }
    return NULL;
}

unsigned codec_registry_count(void)
{
    return g_count;
}

/* Phase 5 CLI: 遍历所有已注册 codec（OAM show codecs）*/
int codec_registry_foreach(int (*cb)(const codec_ops_t *, void *), void *ud)
{
    if (!cb) return NSBC_INVAL;
    int n = 0;
    for (int i = 1; i < CODEC__MAX; ++i) {
        const codec_ops_t *ops = __atomic_load_n(&g_ops[i], __ATOMIC_ACQUIRE);
        if (ops) {
            if (cb(ops, ud) != 0) return n;  /* 回调非 0 提前终止 */
            n++;
        }
    }
    return n;
}

/* Phase 1 SAFE1: find_inc —— 卸载安全版本
 * 检查 unloading + atomic refcnt++；调用者用完必须调 codec_registry_dec */
const codec_ops_t *codec_registry_find_inc(codec_id_t id, int *out_slot)
{
    if (id <= CODEC_NONE || id >= CODEC__MAX) return NULL;
    if (__atomic_load_n(&g_ops_unloading[id], __ATOMIC_ACQUIRE)) return NULL;
    const codec_ops_t *ops = __atomic_load_n(&g_ops[id], __ATOMIC_ACQUIRE);
    if (!ops) return NULL;
    __atomic_fetch_add(&g_ops_refcnt[id], 1, __ATOMIC_ACQ_REL);
    if (out_slot) *out_slot = (int)id;
    return ops;
}

/* Phase 1 SAFE1: dec —— 配对 find_inc 使用 */
void codec_registry_dec(int slot)
{
    if (slot <= CODEC_NONE || slot >= CODEC__MAX) return;
    __atomic_fetch_sub(&g_ops_refcnt[slot], 1, __ATOMIC_ACQ_REL);
}

/* Phase 1 SAFE1: unload_wait —— 先置 unloading=true → 自旋等待 refcnt→0 → 清空 slot */
int codec_registry_unload_wait(codec_id_t id, int timeout_ms)
{
    if (id <= CODEC_NONE || id >= CODEC__MAX) return NSBC_INVAL;
    __atomic_store_n(&g_ops_unloading[id], 1, __ATOMIC_RELEASE);
    int spins = 0;
    int deadline = timeout_ms > 0 ? timeout_ms : 5000;
    while (__atomic_load_n(&g_ops_refcnt[id], __ATOMIC_ACQUIRE) > 0) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (++spins >= deadline) {
            log_err("codec_reg", "unload timeout id=%d refcnt=%d — fail-safe leak",
                    id, __atomic_load_n(&g_ops_refcnt[id], __ATOMIC_ACQUIRE));
            return NSBC_BUSY;
        }
    }
    __atomic_store_n(&g_ops[id], NULL, __ATOMIC_RELEASE);
    return NSBC_OK;
}

/* Phase 1 SAFE1: unregister —— unload_wait + 递减 g_count */
int codec_registry_unregister(codec_id_t id)
{
    int rc = codec_registry_unload_wait(id, 5000);
    if (rc == NSBC_OK) {
        if (g_count > 0) --g_count;
    }
    return rc;
}

/* ============================================================================
 * 转码会话实现
 * ==========================================================================*/
struct transcode_sess {
    codec_id_t      src;
    codec_id_t      dst;
    const codec_ops_t *dops;   /* 解码器 ops */
    const codec_ops_t *eops;   /* 编码器 ops */
    codec_sess_t   *dec;       /* 解码器会话（dops->init 创建） */
    codec_sess_t   *enc;       /* 编码器会话（eops->init 创建） */
    int             opened;
    /* G.711 8k→8k mono 的 PCM 中间 buffer：
     * 最大 RTP payload 约 1500B，预留 3000 int16_t = 6000B 足够 */
    int16_t         pcm_buf[1500];
};

int transcode_open(codec_id_t src, codec_id_t dst, transcode_sess_t **out)
{
    const codec_ops_t *dops;
    const codec_ops_t *eops;
    transcode_sess_t *t;
    int rc;

    if (out == NULL || src <= CODEC_NONE || dst <= CODEC_NONE)
        return NSBC_INVAL;
    *out = NULL;
    if (src == dst)
        return NSBC_INVAL; /* 同编码不需转码（调用方应走 bypass） */

    dops = codec_registry_find(src);
    eops = codec_registry_find(dst);
    if (dops == NULL || eops == NULL)
        return NSBC_NOTSUP; /* 专利 codec 未装/未授权 */

    t = (transcode_sess_t *)calloc(1, sizeof(*t));
    if (t == NULL)
        return NSBC_NOMEM;
    t->src = src;
    t->dst = dst;
    t->dops = dops;
    t->eops = eops;

    /* 创建解码器会话 */
    if (dops->init) {
        rc = dops->init(NULL, &t->dec);
        if (rc != NSBC_OK) {
            free(t);
            return rc;
        }
    }
    /* 创建编码器会话 — encoder_init 优先（mod_av 区分 dec/enc ctx）
     * NULL fallback 到 init（mod_codecs 的 G.711/G.729 共用 init 路径）*/
    if (eops->encoder_init) {
        rc = eops->encoder_init(NULL, &t->enc);
    } else if (eops->init) {
        rc = eops->init(NULL, &t->enc);
    } else {
        rc = NSBC_NOTSUP;
    }
    if (rc != NSBC_OK) {
            if (t->dec && dops->fini) dops->fini(t->dec);
            free(t);
            return rc;
    }
    t->opened = 1;
    *out = t;
    return NSBC_OK;
}

void transcode_close(transcode_sess_t *t)
{
    if (t == NULL)
        return;
    /* 顺序：先编码冲尾再释放解码 */
    if (t->enc && t->eops && t->eops->fini)
        t->eops->fini(t->enc);
    if (t->dec && t->dops && t->dops->fini)
        t->dops->fini(t->dec);
    free(t);
}

codec_id_t transcode_dst_codec_id(const transcode_sess_t *t)
{
    return t ? t->dst : CODEC_NONE;
}

int transcode_process(transcode_sess_t *t, const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t cap, size_t *out_len)
{
    media_frame_t frm;
    int rc;

    if (t == NULL || in == NULL || out == NULL || out_len == NULL)
        return NSBC_INVAL;
    if (!t->opened)
        return NSBC_NOTSUP;
    if (in_len == 0) {
        *out_len = 0;
        return NSBC_OK;
    }

    /* --- 1. decode：RTP payload → PCM frame --- */
    memset(&frm, 0, sizeof(frm));
    frm.pcm = t->pcm_buf;
    frm.samples = 0;
    frm.rate = t->dops ? t->dops->clock_rate : 8000;
    frm.channels = t->dops ? t->dops->channels : 1;

    if (!t->dops || !t->dops->decode)
        return NSBC_NOTSUP;
    rc = t->dops->decode(t->dec, in, in_len, &frm);
    if (rc != NSBC_OK)
        return rc;

    /* --- 2. encode：PCM frame → RTP payload --- */
    if (!t->eops || !t->eops->encode)
        return NSBC_NOTSUP;
    rc = t->eops->encode(t->enc, &frm, out, cap, out_len);
    return rc;
}

/* ============================================================================
 * SDP fmtp 协商裁剪（P0 不接线——G.711 无 fmtp）
 * ==========================================================================*/
int codec_fmtp_negotiate(codec_id_t id, const char *offer, const char *answer,
                         char *dst, size_t cap)
{
    if (dst == NULL || cap == 0)
        return NSBC_INVAL;
    (void)id;
    (void)offer;
    (void)answer;
    return NSBC_NOTSUP;
}
