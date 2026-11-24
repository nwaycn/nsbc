/* snapshot_job.c — 视频抓图作业引擎（方案文档 §3.4）
 *
 * 数据流：
 *   media_bridge relay（H.264 RTP 重组）
 *     → bridge_video_frame_cb
 *       → media_bug_dispatch_video_au（全局消费者，AU 边界对齐）
 *         → snap_tap_cb（只做过滤 + 拷入有界槽位）
 *           → worker 线程：SPS/PPS 缓存 → H.264 解码（mod_av）
 *              → 黑帧/分辨率检查 → MJPEG 编码（mod_av）→ 落盘
 *
 * 不改变呼叫 relay/转码模式；作业结束即从 tap 匹配中移除。
 * 全局并发上限 SNAP_JOB_MAX，终态保留供状态查询。
 */
#include "snapshot_job.h"

#include "media_bug.h"
#include "media_codec.h"
#include "nsbc_common.h"
#include "b2bua.h"      /* b2b_call_is_alive：验证裸 call_key 未复用 */
#include "event_bus.h"  /* nsbc_event_push：抓图完成事件（ESL/WS 订阅） */
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SNAP_JOB_MAX            16
#define SNAP_AU_CAP             (512 * 1024)   /* 与 RTP_VIDEO_AU_MAX 一致 */
#define SNAP_JPEG_CAP           (1024 * 1024)
#define SNAP_PLI_INTERVAL_MS    500
#define SNAP_POLL_US            20000
#define SNAP_PS_CAP             512
#define SNAP_MIN_W              160
#define SNAP_MIN_H              120
#define SNAP_MULTI_WINDOW_MS    800            /* 多帧选优窗口 */
#define SNAP_MAX_FAILS          3

#define SNAP_DIR_DEFAULT        "/tmp/nsbc_snapshots"

enum { SNAP_FREE = 0, SNAP_RUNNING = 1, SNAP_DONE = 2, SNAP_RESERVED = 3 };

typedef struct snap_job {
    int      state;
    uint64_t id;
    bridge_mgr_t *m;
    void    *call;
    int      leg;
    int      want_frames;
    int      timeout_ms;
    int      cancel;
    uint64_t start_ms;
    uint64_t done_ms;
    snapshot_done_cb_t cb;
    void    *ud;

    /* tap → worker：双槽，生产者只写空闲槽，消费者持有 busy 槽 */
    pthread_mutex_t lock;
    uint8_t *buf[2];
    size_t   blen[2];
    uint8_t  bkey[2];
    uint32_t bts[2];
    uint32_t bssrc[2];
    uint8_t  ready[2];
    uint8_t  busy[2];
    uint64_t got_key;
    uint32_t drop_count;
    int      cid;                 /* 本腿视频编码（首帧 AU 确定，原子读写）*/

    snapshot_result_info_t info;
    pthread_t th;
    int       th_started;
} snap_job_t;

static snap_job_t      g_jobs[SNAP_JOB_MAX];
static pthread_mutex_t g_jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;
static uint64_t        g_next_id = 1;
static volatile int    g_active_jobs = 0;

/* 每呼叫最短间隔（§4.6：同一呼叫 ≥2s），避免业务侧刷图打爆媒体面。
 * 按 (call_key, leg) 限频：双逐腿抓图（leg=-1）时两腿互不影响。
 * 裸 call_key 仅做等值比较；条目循环覆盖。 */
#define SNAP_CALLGATE_N     32
static struct {
    void    *call;
    int      leg;
    uint64_t last_ms;
} g_callgate[SNAP_CALLGATE_N];
static int g_callgate_i = 0;

/* ------------------------------------------------------------------ */
/* 基础工具                                                            */
/* ------------------------------------------------------------------ */

const char *snapshot_result_name(int result)
{
    switch (result) {
    case SNAPSHOT_OK:            return "OK";
    case SNAPSHOT_INVAL:         return "INVAL";
    case SNAPSHOT_NO_VIDEO:      return "NO_VIDEO";
    case SNAPSHOT_NO_KEYFRAME:   return "NO_KEYFRAME";
    case SNAPSHOT_NO_PARAM_SETS: return "NO_PARAM_SETS";
    case SNAPSHOT_NO_CODEC:      return "NO_CODEC";
    case SNAPSHOT_DECODE_FAIL:   return "DECODE_FAIL";
    case SNAPSHOT_TOO_SMALL:     return "TOO_SMALL";
    case SNAPSHOT_BLACK_FRAME:   return "BLACK_FRAME";
    case SNAPSHOT_BUSY:          return "BUSY";
    case SNAPSHOT_TIMEOUT:       return "TIMEOUT";
    case SNAPSHOT_CANCEL:        return "CANCEL";
    default:                     return "UNKNOWN";
    }
}

static const char *snap_dir(void)
{
    const char *d = getenv("NSBC_SNAPSHOT_DIR");
    return (d && d[0]) ? d : SNAP_DIR_DEFAULT;
}

/* 到期清理（§3.4.6 生物特征数据 TTL）：删除快照目录中过期的 snap-*.jpg。
 * NSBC_SNAPSHOT_TTL_S 秒，默认 3600；<=0 关闭。仅 worker 线程调用，
 * 每个作业最多扫描一次，不在媒体热路径。 */
static void snap_ttl_sweep(void)
{
    const char *env = getenv("NSBC_SNAPSHOT_TTL_S");
    long ttl = (env != NULL) ? strtol(env, NULL, 10) : 3600;
    if (ttl <= 0) return;

    DIR *d = opendir(snap_dir());
    if (d == NULL) return;

    time_t cutoff = time(NULL) - (time_t)ttl;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "snap-", 5) != 0) continue;
        size_t n = strlen(de->d_name);
        if (n < 4 || strcmp(de->d_name + n - 4, ".jpg") != 0) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", snap_dir(), de->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
            st.st_mtime < cutoff)
            unlink(path);
    }
    closedir(d);
}

/* ------------------------------------------------------------------ */
/* 媒体 tap（媒体线程）                                                */
/* ------------------------------------------------------------------ */

static void snap_tap_cb(media_bug_frame_t *f, void *ud)
{
    (void)ud;
    if (f == NULL || f->data == NULL || f->data_len == 0) return;
    if (f->type != BUG_VIDEO_AU) return;
    /* 支持的全部视频编码；参数集/解码按 codec_id 分派 */
    if (f->codec_id != CODEC_H264 && f->codec_id != CODEC_H265 &&
        f->codec_id != CODEC_VP8  && f->codec_id != CODEC_VP9) return;
    if (f->data_len > SNAP_AU_CAP) return;
    if (__atomic_load_n(&g_active_jobs, __ATOMIC_RELAXED) == 0) return;

    pthread_mutex_lock(&g_jobs_lock);
    snap_job_t *j = NULL;
    for (int i = 0; i < SNAP_JOB_MAX; ++i) {
        if (g_jobs[i].state == SNAP_RUNNING &&
            g_jobs[i].call == f->call_key && g_jobs[i].leg == f->leg) {
            j = &g_jobs[i];
            break;
        }
    }
    if (j == NULL) {
        pthread_mutex_unlock(&g_jobs_lock);
        return;
    }

    /* 拿到作业锁后立即释放全局锁：大帧拷贝不再阻塞其它呼叫的媒体线程。
     * 槽位复用发生在 join 之后且持 j->lock 改 call/leg，因此这里必须在
     * 锁内复核归属，防止旧匹配把本呼叫的帧写进别人的新作业。 */
    pthread_mutex_lock(&j->lock);
    pthread_mutex_unlock(&g_jobs_lock);

    if (j->call != f->call_key || j->leg != f->leg) {
        pthread_mutex_unlock(&j->lock);
        return;
    }

    /* 首帧确定编码：worker 据此选择解码器与参数集处理方式 */
    if (__atomic_load_n(&j->cid, __ATOMIC_RELAXED) == 0)
        __atomic_store_n(&j->cid, (int)f->codec_id, __ATOMIC_RELAXED);

    /* 关键帧之前的数据对解码无意义，直接丢弃（等待 PLI 响应）*/
    if (j->got_key == 0 && !f->keyframe) {
        pthread_mutex_unlock(&j->lock);
        return;
    }

    int slot = -1;
    for (int i = 0; i < 2; ++i)
        if (!j->ready[i] && !j->busy[i]) { slot = i; break; }
    if (slot < 0) {
        /* 两槽都在用时覆盖一个尚未被取走的 ready 槽（保留最新帧）*/
        for (int i = 0; i < 2; ++i)
            if (!j->busy[i]) { slot = i; break; }
        if (slot >= 0)
            __atomic_add_fetch(&j->drop_count, 1, __ATOMIC_RELAXED); /* 覆盖未取走的帧 */
    }
    if (slot < 0) {
        __atomic_add_fetch(&j->drop_count, 1, __ATOMIC_RELAXED); /* 消费者正在解码两槽 */
        pthread_mutex_unlock(&j->lock);
        return;
    }
    if (j->buf[slot] == NULL) {
        /* 缓冲应在作业启动时预分配；未预分配则丢帧，严禁媒体线程 malloc */
        __atomic_add_fetch(&j->drop_count, 1, __ATOMIC_RELAXED);
    } else {
        memcpy(j->buf[slot], f->data, f->data_len);
        j->blen[slot] = f->data_len;
        j->bkey[slot] = (uint8_t)(f->keyframe ? 1 : 0);
        j->bts[slot]  = f->ts;
        j->bssrc[slot] = f->ssrc;
        j->ready[slot] = 1;
        if (f->keyframe && j->got_key == 0) j->got_key = 1;
    }
    pthread_mutex_unlock(&j->lock);
}

static media_bug_consumer_t g_snap_consumer = {
    .name = "snapshot",
    .type_mask = BUG_VIDEO_AU,
    .cb = snap_tap_cb,
    .ud = NULL,
};

static void snap_global_init(void)
{
    for (int i = 0; i < SNAP_JOB_MAX; ++i)
        pthread_mutex_init(&g_jobs[i].lock, NULL);
    if (media_bug_register_global_consumer(&g_snap_consumer) != 0)
        llog(LOG_WARN, "snapshot", "failed to register BUG_VIDEO_AU consumer");
    atexit(snapshot_job_purge_all);
}

/* ------------------------------------------------------------------ */
/* H.264 参数集缓存 / AU 组装                                          */
/* ------------------------------------------------------------------ */

/* 返回 p 处起始码长度（3 或 4），不是起始码返回 0 */
static size_t snap_start_code(const uint8_t *p, size_t n)
{
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return 4;
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return 3;
    return 0;
}

/* 遍历 H.264 Annex-B NAL：标记是否含 SPS/PPS/IDR，并把 SPS/PPS 存入缓存。 */
static void snap_scan_au_h264(const uint8_t *au, size_t len,
                              uint8_t *sps, size_t *sps_len,
                              uint8_t *pps, size_t *pps_len,
                              int *found7, int *found8, int *found5)
{
    size_t i = 0;
    *found7 = *found8 = *found5 = 0;
    while (i + 3 <= len) {
        size_t sc = snap_start_code(au + i, len - i);
        if (sc == 0) { ++i; continue; }
        size_t nstart = i + sc;
        if (nstart >= len) break;
        size_t j = nstart + 1;
        while (j + 3 <= len && snap_start_code(au + j, len - j) == 0) ++j;
        size_t nend = (j + 3 <= len) ? j : len;
        size_t nlen = nend - nstart;
        if (nlen == 0) { i = nstart; continue; }

        int type = au[nstart] & 0x1f;
        if (type == 7) {
            *found7 = 1;
            size_t c = nlen < SNAP_PS_CAP ? nlen : SNAP_PS_CAP;
            memcpy(sps, au + nstart, c);
            *sps_len = c;
        } else if (type == 8) {
            *found8 = 1;
            size_t c = nlen < SNAP_PS_CAP ? nlen : SNAP_PS_CAP;
            memcpy(pps, au + nstart, c);
            *pps_len = c;
        } else if (type == 5) {
            *found5 = 1;
        }
        i = nend;
    }
}

/* 遍历 H.265 Annex-B NAL（RFC 7798）：NAL 头 2 字节，type=(b0>>1)&0x3F。
 * 缓存 VPS(32)/SPS(33)/PPS(34)，标记 IRAP(16..21)。 */
static void snap_scan_au_h265(const uint8_t *au, size_t len,
                              uint8_t *vps, size_t *vps_len,
                              uint8_t *sps, size_t *sps_len,
                              uint8_t *pps, size_t *pps_len,
                              int *found32, int *found33, int *found34,
                              int *found_irap)
{
    size_t i = 0;
    *found32 = *found33 = *found34 = *found_irap = 0;
    while (i + 3 <= len) {
        size_t sc = snap_start_code(au + i, len - i);
        if (sc == 0) { ++i; continue; }
        size_t nstart = i + sc;
        if (nstart + 2 > len) break;
        size_t j = nstart + 2;
        while (j + 3 <= len && snap_start_code(au + j, len - j) == 0) ++j;
        size_t nend = (j + 3 <= len) ? j : len;
        size_t nlen = nend - nstart;
        if (nlen < 2) { i = nstart; continue; }

        int type = (au[nstart] >> 1) & 0x3F;
        uint8_t *dst = NULL;
        size_t   *dst_len = NULL;
        if (type == 32)      { *found32 = 1; dst = vps; dst_len = vps_len; }
        else if (type == 33) { *found33 = 1; dst = sps; dst_len = sps_len; }
        else if (type == 34) { *found34 = 1; dst = pps; dst_len = pps_len; }
        else if (type >= 16 && type <= 21) { *found_irap = 1; }

        if (dst != NULL) {
            size_t c = nlen < SNAP_PS_CAP ? nlen : SNAP_PS_CAP;
            memcpy(dst, au + nstart, c);
            *dst_len = c;
        }
        i = nend;
    }
}

/* ------------------------------------------------------------------ */
/* 画面质量评估                                                        */
/* ------------------------------------------------------------------ */

/* 以 luma 平面（YUV420P/YUVJ420P/NV12 的 plane0）做均值/方差/梯度。
 * 抽样步长 2，720p 约 23 万次访问，单帧 <1ms。 */
static void snap_luma_stats(const video_frame_t *f, double *mean,
                            double *var, double *sharp)
{
    const uint8_t *p = (f->data[0] != NULL) ? f->data[0] : NULL;
    int w = f->width, h = f->height, ls = f->linesize[0];
    *mean = 0.0; *var = 0.0; *sharp = 0.0;
    if (p == NULL || w < 3 || h < 3 || ls < w) return;

    double sum = 0.0, sum2 = 0.0, lap = 0.0;
    uint64_t n = 0;
    for (int y = 1; y < h - 1; y += 2) {
        const uint8_t *row = p + (size_t)y * (size_t)ls;
        const uint8_t *up  = row - ls;
        const uint8_t *dn  = row + ls;
        for (int x = 1; x < w - 1; x += 2) {
            double v = (double)row[x];
            sum  += v;
            sum2 += v * v;
            int gx = (int)row[x + 1] - (int)row[x - 1];
            int gy = (int)dn[x] - (int)up[x];
            lap += (double)((gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy));
            ++n;
        }
    }
    if (n == 0) return;
    *mean = sum / (double)n;
    double m2 = *mean * *mean;
    double v = sum2 / (double)n - m2;
    *var = (v > 0.0) ? v : 0.0;
    *sharp = lap / (double)n;
}

static int snap_is_black(double mean, double var)
{
    return (mean < 24.0 && var < 36.0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* 落盘                                                                */
/* ------------------------------------------------------------------ */

static int snap_write_file(uint64_t id, int leg,
                           const uint8_t *data, size_t len,
                           char *out, size_t cap)
{
    const char *dir = snap_dir();
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        llog(LOG_WARN, "snapshot", "mkdir %s failed: %s", dir, strerror(errno));
        return NSBC_ERR;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    /* 先归一到 uint64_t：混用 1000ULL 会让表达式提升为 unsigned long long，
     * 与 PRIu64（LP64 上是 unsigned long）不匹配 */
    uint64_t ms = (uint64_t)tv.tv_sec * 1000u +
                  (uint64_t)(tv.tv_usec / 1000);
    snprintf(out, cap, "%s/snap-%" PRIu64 "-leg%d-%" PRIu64 ".jpg",
             dir, id, leg, ms);
    /* 生物特征数据：创建即 0600，避免 chmod 前的可读窗口 */
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    FILE *fp = (fd >= 0) ? fdopen(fd, "wb") : NULL;
    if (fp == NULL) {
        if (fd >= 0) close(fd);
        llog(LOG_WARN, "snapshot", "open %s failed: %s", out, strerror(errno));
        out[0] = '\0';
        return NSBC_ERR;
    }
    size_t w = fwrite(data, 1, len, fp);
    int rc = (w == len) ? NSBC_OK : NSBC_ERR;
    if (fclose(fp) != 0) rc = NSBC_ERR;
    if (rc != NSBC_OK) {
        llog(LOG_WARN, "snapshot", "write %s short (%zu/%zu)", out, w, len);
        (void)unlink(out);   /* 半截文件不留盘 */
        out[0] = '\0';
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* worker                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const codec_ops_t *dec_ops;
    const codec_ops_t *enc_ops;
    codec_sess_t *dec;
    codec_sess_t *enc;
    codec_id_t cid;        /* 当前解码器对应的编码 */
    int      ps_loaded;    /* SDP sprop 只解析一次 */
    uint8_t *au;
    size_t   au_cap;
    uint8_t  vps[SNAP_PS_CAP];   /* H.265 用 */
    size_t   vps_len;
    uint8_t  sps[SNAP_PS_CAP];
    size_t   sps_len;
    uint8_t  pps[SNAP_PS_CAP];
    size_t   pps_len;
    uint8_t *scratch;      /* JPEG 编码输出 */
    uint8_t *best;
    size_t   best_len;
    double   best_score;
} snap_ctx_t;

static void snap_ctx_free(snap_ctx_t *c)
{
    if (c->dec && c->dec_ops && c->dec_ops->fini) c->dec_ops->fini(c->dec);
    if (c->enc && c->enc_ops && c->enc_ops->fini) c->enc_ops->fini(c->enc);
    free(c->au);
    free(c->scratch);
    free(c->best);
    memset(c, 0, sizeof(*c));
}

/* ------------------------------------------------------------------ */
/* SDP fmtp sprop-parameter-sets（P3-a）                               */
/* ------------------------------------------------------------------ */

static int snap_b64_val(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

/* 标准 base64 解码；不写 NUL。返回字节数，-1=超出 cap。 */
static int snap_b64_decode(const char *in, size_t inlen,
                           uint8_t *out, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < inlen; ++i) {
        int v;
        char ch = in[i];
        if (ch == '=' || ch == ',' || ch == ';' || ch == ' ' ||
            ch == '\r' || ch == '\n')
            break;
        v = snap_b64_val((unsigned char)ch);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    return (int)n;
}

/* 取 fmtp 中指定 key（如 "sprop-vps="）的 base64 值并解码。返回字节数。 */
static size_t snap_fmtp_b64(const char *fmtp, const char *key,
                            uint8_t *out, size_t cap)
{
    const char *p = strstr(fmtp, key);
    if (p == NULL) return 0;
    p += strlen(key);
    const char *end = p;
    while (*end != '\0' && *end != ';' && *end != ' ' &&
           *end != '\r' && *end != '\n')
        ++end;
    int n = snap_b64_decode(p, (size_t)(end - p), out, cap);
    return (n > 0) ? (size_t)n : 0;
}

/* 从该腿 SDP fmtp 预置参数集。
 *   H.264：sprop-parameter-sets=<sps>,<pps>
 *   H.265：sprop-vps= / sprop-sps= / sprop-pps=（各自 base64）
 * 流若只在 fmtp 宣告参数集（不在每个关键帧前重发），没有它就无法解码。 */
static void snap_load_fmtp_sprop(snap_job_t *j, snap_ctx_t *c)
{
    char fmtp[1024];
    if (!media_bridge_call_video_fmtp(j->m, j->call, j->leg,
                                      fmtp, sizeof(fmtp)))
        return;

    codec_id_t cid = (codec_id_t)__atomic_load_n(&j->cid, __ATOMIC_RELAXED);
    if (cid == CODEC_H265) {
        c->vps_len = snap_fmtp_b64(fmtp, "sprop-vps=", c->vps, sizeof(c->vps));
        c->sps_len = snap_fmtp_b64(fmtp, "sprop-sps=", c->sps, sizeof(c->sps));
        c->pps_len = snap_fmtp_b64(fmtp, "sprop-pps=", c->pps, sizeof(c->pps));
        if (c->vps_len && c->sps_len && c->pps_len)
            llog(LOG_INFO, "snapshot",
                 "job=%" PRIu64 " leg=%d sprop from fmtp: vps=%zu sps=%zu pps=%zu",
                 j->id, j->leg, c->vps_len, c->sps_len, c->pps_len);
        return;
    }
    if (cid != CODEC_H264) return;   /* VP8/VP9 无参数集 */

    const char *p = strstr(fmtp, "sprop-parameter-sets=");
    if (p == NULL) return;
    p += strlen("sprop-parameter-sets=");

    const char *end = p;
    while (*end != '\0' && *end != ';' && *end != ' ' &&
           *end != '\r' && *end != '\n')
        ++end;
    const char *comma = memchr(p, ',', (size_t)(end - p));

    size_t n1 = comma ? (size_t)(comma - p) : (size_t)(end - p);
    int d1 = snap_b64_decode(p, n1, c->sps, sizeof(c->sps));
    if (d1 > 0) c->sps_len = (size_t)d1;

    if (comma != NULL) {
        const char *q = comma + 1;
        int d2 = snap_b64_decode(q, (size_t)(end - q), c->pps, sizeof(c->pps));
        if (d2 > 0) c->pps_len = (size_t)d2;
    }

    if (c->sps_len != 0 && c->pps_len != 0)
        llog(LOG_INFO, "snapshot",
             "job=%" PRIu64 " leg=%d sprop from fmtp: sps=%zu pps=%zu",
             j->id, j->leg, c->sps_len, c->pps_len);
}

static int snap_take(snap_job_t *j, int *out_idx)
{
    int idx = -1;
    pthread_mutex_lock(&j->lock);
    for (int i = 0; i < 2; ++i) {
        if (j->ready[i]) {
            j->ready[i] = 0;
            j->busy[i] = 1;
            idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&j->lock);
    if (idx >= 0) *out_idx = idx;
    return idx;
}

static void snap_release(snap_job_t *j, int idx)
{
    pthread_mutex_lock(&j->lock);
    j->busy[idx] = 0;
    pthread_mutex_unlock(&j->lock);
}

/* 组装送解码器的 AU：
 *   - H.264：缺 SPS/PPS 时用缓存前缀补齐；
 *   - H.265：缺 VPS/SPS/PPS 时用缓存前缀补齐（顺序 VPS→SPS→PPS）；
 *   - VP8/VP9：裸帧，原样交给解码器。
 * 返回 NULL 表示无法解码（reason 给出失败原因）。 */
static const uint8_t *snap_build_au(snap_ctx_t *c, codec_id_t cid,
                                    const uint8_t *au, size_t len,
                                    size_t *out_len, int *reason)
{
    /* ---- VP8/VP9：无起始码、无参数集 ---- */
    if (cid == CODEC_VP8 || cid == CODEC_VP9) {
        if (len > c->au_cap) {
            uint8_t *nb = (uint8_t *)realloc(c->au, len);
            if (nb == NULL) { *reason = SNAPSHOT_DECODE_FAIL; return NULL; }
            c->au = nb;
            c->au_cap = len;
        }
        if (c->au == NULL) { *reason = SNAPSHOT_DECODE_FAIL; return NULL; }
        memcpy(c->au, au, len);
        *out_len = len;
        *reason = SNAPSHOT_OK;
        /* DEBUG: 打印 AU 前 32 字节 hex，便于定位 DECODE_FAIL 根因 */
        {
            size_t dmp = len < 32 ? len : 32;
            char hex[97]; hex[0] = '\0';
            for (size_t i = 0; i < dmp; i++) {
                snprintf(hex + i * 3, 4, "%02x ", c->au[i]);
            }
            llog(LOG_INFO, "snapshot",
                 "  AU_DUMP cid=%d len=%zu head=[%s]",
                 (int)cid, len, hex);
        }
        return c->au;
    }

    /* ---- H.265 ---- */
    if (cid == CODEC_H265) {
        int f32 = 0, f33 = 0, f34 = 0, firap = 0;
        snap_scan_au_h265(au, len, c->vps, &c->vps_len, c->sps, &c->sps_len,
                          c->pps, &c->pps_len, &f32, &f33, &f34, &firap);
        if (!f32 && c->vps_len == 0) { *reason = SNAPSHOT_NO_PARAM_SETS; return NULL; }
        if (!f33 && c->sps_len == 0) { *reason = SNAPSHOT_NO_PARAM_SETS; return NULL; }
        if (!f34 && c->pps_len == 0) { *reason = SNAPSHOT_NO_PARAM_SETS; return NULL; }

        size_t total = len;
        if (!f32) total += 4 + c->vps_len;
        if (!f33) total += 4 + c->sps_len;
        if (!f34) total += 4 + c->pps_len;
        if (total > c->au_cap) {
            uint8_t *nb = (uint8_t *)realloc(c->au, total);
            if (nb == NULL) { *reason = SNAPSHOT_DECODE_FAIL; return NULL; }
            c->au = nb;
            c->au_cap = total;
        }
        if (c->au == NULL) { *reason = SNAPSHOT_DECODE_FAIL; return NULL; }

        static const uint8_t sc[4] = { 0, 0, 0, 1 };
        size_t off = 0;
        if (!f32) { memcpy(c->au + off, sc, 4); off += 4;
                    memcpy(c->au + off, c->vps, c->vps_len); off += c->vps_len; }
        if (!f33) { memcpy(c->au + off, sc, 4); off += 4;
                    memcpy(c->au + off, c->sps, c->sps_len); off += c->sps_len; }
        if (!f34) { memcpy(c->au + off, sc, 4); off += 4;
                    memcpy(c->au + off, c->pps, c->pps_len); off += c->pps_len; }
        memcpy(c->au + off, au, len); off += len;
        *out_len = off;
        *reason = SNAPSHOT_OK;
        return c->au;
    }

    /* ---- H.264 ---- */
    int f7 = 0, f8 = 0, f5 = 0;
    snap_scan_au_h264(au, len, c->sps, &c->sps_len, c->pps, &c->pps_len,
                      &f7, &f8, &f5);

    if (!f7 && c->sps_len == 0) { *reason = SNAPSHOT_NO_PARAM_SETS; return NULL; }
    if (!f8 && c->pps_len == 0) { *reason = SNAPSHOT_NO_PARAM_SETS; return NULL; }

    int need7 = (!f7) ? 1 : 0;
    int need8 = (!f8) ? 1 : 0;
    size_t total = len;
    if (need7) total += 4 + c->sps_len;
    if (need8) total += 4 + c->pps_len;

    if (total > c->au_cap) {
        uint8_t *nb = (uint8_t *)realloc(c->au, total);
        if (nb == NULL) { *reason = SNAPSHOT_DECODE_FAIL; return NULL; }
        c->au = nb;
        c->au_cap = total;
    }
    if (c->au == NULL) { *reason = SNAPSHOT_DECODE_FAIL; return NULL; }

    size_t off = 0;
    if (need7) {
        memcpy(c->au + off, "\x00\x00\x00\x01", 4); off += 4;
        memcpy(c->au + off, c->sps, c->sps_len);    off += c->sps_len;
    }
    if (need8) {
        memcpy(c->au + off, "\x00\x00\x00\x01", 4); off += 4;
        memcpy(c->au + off, c->pps, c->pps_len);    off += c->pps_len;
    }
    memcpy(c->au + off, au, len); off += len;

    *out_len = off;
    *reason = SNAPSHOT_OK;
    return c->au;
}

static void *snap_job_thread(void *arg)
{
    snap_job_t *j = (snap_job_t *)arg;
    snap_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    uint64_t deadline = j->start_ms +
        (uint64_t)(j->timeout_ms > 0 ? j->timeout_ms
                                     : SNAPSHOT_DEFAULT_TIMEOUT_MS);

    int  res = SNAPSHOT_NO_KEYFRAME;
    int  captured = 0;
    int  got_key_frame = 0;
    int  fail_count = 0;
    uint64_t last_pli = 0;
    uint64_t key_ms = 0;
    int  width = 0, height = 0;
    uint32_t ssrc = 0, rtp_ts = 0;
    uint64_t bytes = 0;
    char path[256];
    path[0] = '\0';

    ctx.enc_ops = codec_registry_find(CODEC_MJPEG);
    if (ctx.enc_ops == NULL || ctx.enc_ops->vid_enc_init == NULL ||
        ctx.enc_ops->vid_encode == NULL) {
        res = SNAPSHOT_DECODE_FAIL;
        llog(LOG_WARN, "snapshot", "job=%" PRIu64 ": MJPEG encoder unavailable "
             "(mod_av with FFmpeg required)", j->id);
        goto done;
    }

    /* 生物特征数据 TTL：清理过期快照（worker 线程，串行且低频） */
    snap_ttl_sweep();

    /* 解码器与 SDP 参数集在首帧确定编码后再初始化（H.264/H.265/VP8/VP9） */

    for (;;) {
        if (__atomic_load_n(&j->cancel, __ATOMIC_RELAXED)) {
            res = SNAPSHOT_CANCEL;
            break;
        }
        uint64_t now = nsbc_now_ms();
        if (now >= deadline) {
            /* 从未见到关键帧 → NO_KEYFRAME；见到但没抓成 → TIMEOUT */
            if (!got_key_frame) res = SNAPSHOT_NO_KEYFRAME;
            else if (res == SNAPSHOT_NO_KEYFRAME) res = SNAPSHOT_TIMEOUT;
            break;
        }

        int idx = -1;
        if (snap_take(j, &idx) < 0) {
            if (now - last_pli >= SNAP_PLI_INTERVAL_MS) {
                last_pli = now;
                /* 呼叫已结束时裸指针可能被 slab 复用，先做 magic 校验 */
                if (!b2b_call_is_alive(j->call)) {
                    res = SNAPSHOT_CANCEL;
                    break;
                }
                /* 尚未学习到 SSRC 时返回 AGAIN，下一轮再试 */
                (void)media_bridge_request_keyframe(j->m, j->call, j->leg);
            }
            usleep(SNAP_POLL_US);
            continue;
        }

        const uint8_t *au = j->buf[idx];
        size_t au_len = j->blen[idx];
        int key = j->bkey[idx] ? 1 : 0;
        ssrc = j->bssrc[idx];
        rtp_ts = j->bts[idx];

        if (au == NULL || au_len == 0) { snap_release(j, idx); continue; }

        /* 编码由首帧 AU 确定；切换时重建解码器并清空参数集缓存 */
        codec_id_t cid = (codec_id_t)__atomic_load_n(&j->cid, __ATOMIC_RELAXED);
        if (cid == CODEC_NONE) { snap_release(j, idx); continue; }
        if (ctx.cid != cid) {
            if (ctx.dec != NULL && ctx.dec_ops && ctx.dec_ops->fini)
                ctx.dec_ops->fini(ctx.dec);
            ctx.dec = NULL;
            ctx.cid = cid;
            ctx.sps_len = 0;
            ctx.pps_len = 0;
            ctx.vps_len = 0;
            ctx.ps_loaded = 0;
            ctx.dec_ops = codec_registry_find(cid);
            if (ctx.dec_ops == NULL || ctx.dec_ops->vid_dec_init == NULL ||
                ctx.dec_ops->vid_decode == NULL ||
                !codec_can_decode(cid)) {
                snap_release(j, idx);
                res = SNAPSHOT_NO_CODEC;
                llog(LOG_WARN, "snapshot",
                     "job=%" PRIu64 " leg=%d codec=%d: no decoder available",
                     j->id, j->leg, (int)cid);
                break;
            }
            llog(LOG_INFO, "snapshot", "job=%" PRIu64 " leg=%d codec=%d selected",
                 j->id, j->leg, (int)cid);
        }

        /* P3-a：从 SDP fmtp 预置参数集（仅 H.264/H.265；带内缺失时兜底）*/
        if (!ctx.ps_loaded) {
            ctx.ps_loaded = 1;
            snap_load_fmtp_sprop(j, &ctx);
        }

        if (!key && !got_key_frame) { snap_release(j, idx); continue; }
        if (key) {
            got_key_frame = 1;
            if (key_ms == 0) key_ms = now;
        }

        int reason = SNAPSHOT_OK;
        size_t dec_len = 0;
        const uint8_t *dec_au = snap_build_au(&ctx, cid, au, au_len,
                                              &dec_len, &reason);
        if (dec_au == NULL) {
            snap_release(j, idx);
            res = reason;
            if (reason == SNAPSHOT_NO_PARAM_SETS) break;
            if (++fail_count >= SNAP_MAX_FAILS) break;
            continue;
        }

        if (ctx.dec == NULL) {
            if (ctx.dec_ops->vid_dec_init(NULL, &ctx.dec) != NSBC_OK) {
                snap_release(j, idx);
                res = SNAPSHOT_DECODE_FAIL;
                break;
            }
        }

        video_frame_t vf;
        memset(&vf, 0, sizeof(vf));
        int rc = ctx.dec_ops->vid_decode(ctx.dec, dec_au, dec_len, &vf);
        snap_release(j, idx);

        if (rc == NSBC_AGAIN) {
            /* 解码器还在等后续数据；继续等下一帧 */
            continue;
        }
        if (rc != NSBC_OK) {
            res = SNAPSHOT_DECODE_FAIL;
            if (++fail_count >= SNAP_MAX_FAILS) break;
            continue;
        }
        fail_count = 0;
        width = vf.width;
        height = vf.height;

        if (width < SNAP_MIN_W || height < SNAP_MIN_H) {
            if (ctx.dec_ops->vid_frame_free)
                ctx.dec_ops->vid_frame_free(ctx.dec, &vf);
            res = SNAPSHOT_TOO_SMALL;
            break;
        }

        double mean = 0.0, var = 0.0, sharp = 0.0;
        snap_luma_stats(&vf, &mean, &var, &sharp);
        if (snap_is_black(mean, var)) {
            if (ctx.dec_ops->vid_frame_free)
                ctx.dec_ops->vid_frame_free(ctx.dec, &vf);
            res = SNAPSHOT_BLACK_FRAME;
            if (j->want_frames <= 1) break;
            continue;   /* 多帧模式继续等下一帧候选 */
        }

        if (ctx.scratch == NULL)
            ctx.scratch = (uint8_t *)malloc(SNAP_JPEG_CAP);
        if (ctx.scratch == NULL) {
            if (ctx.dec_ops->vid_frame_free)
                ctx.dec_ops->vid_frame_free(ctx.dec, &vf);
            res = SNAPSHOT_DECODE_FAIL;
            break;
        }
        if (ctx.enc == NULL) {
            if (ctx.enc_ops->vid_enc_init(NULL, &ctx.enc) != NSBC_OK) {
                if (ctx.dec_ops->vid_frame_free)
                    ctx.dec_ops->vid_frame_free(ctx.dec, &vf);
                res = SNAPSHOT_DECODE_FAIL;
                break;
            }
        }

        size_t jlen = 0;
        int jkey = 0;
        rc = ctx.enc_ops->vid_encode(ctx.enc, &vf, ctx.scratch, SNAP_JPEG_CAP,
                                     &jlen, &jkey);
        if (ctx.dec_ops->vid_frame_free)
            ctx.dec_ops->vid_frame_free(ctx.dec, &vf);

        if (rc == NSBC_AGAIN) continue;
        if (rc != NSBC_OK || jlen == 0) {
            res = SNAPSHOT_DECODE_FAIL;
            if (++fail_count >= SNAP_MAX_FAILS) break;
            continue;
        }

        ++captured;
        if (j->want_frames <= 1) {
            res = (snap_write_file(j->id, j->leg, ctx.scratch, jlen,
                                   path, sizeof(path)) == NSBC_OK)
                  ? SNAPSHOT_OK : SNAPSHOT_DECODE_FAIL;
            if (res == SNAPSHOT_OK) bytes = jlen;
            break;
        }

        /* 多帧选优：保存梯度最强的一帧 */
        if (ctx.best == NULL) ctx.best = (uint8_t *)malloc(SNAP_JPEG_CAP);
        if (ctx.best != NULL &&
            (ctx.best_len == 0 || sharp > ctx.best_score)) {
            memcpy(ctx.best, ctx.scratch, jlen);
            ctx.best_len = jlen;
            ctx.best_score = sharp;
        }
        uint64_t now2 = nsbc_now_ms();
        if (captured >= j->want_frames ||
            (key_ms != 0 && now2 - key_ms >= SNAP_MULTI_WINDOW_MS)) {
            if (ctx.best != NULL && ctx.best_len > 0 &&
                snap_write_file(j->id, j->leg, ctx.best, ctx.best_len,
                                path, sizeof(path)) == NSBC_OK) {
                res = SNAPSHOT_OK;
                bytes = ctx.best_len;
            } else {
                res = SNAPSHOT_DECODE_FAIL;
            }
            break;
        }
    }

done:
    snap_ctx_free(&ctx);
    {
        snapshot_result_info_t ci;
        uint32_t drops = __atomic_load_n(&j->drop_count, __ATOMIC_RELAXED);
        memset(&ci, 0, sizeof(ci));
        ci.result = res;
        ci.leg = j->leg;
        ci.width = width;
        ci.height = height;
        ci.frames_captured = captured;
        ci.elapsed_ms = nsbc_now_ms() - j->start_ms;
        ci.ssrc = ssrc;
        ci.rtp_ts = rtp_ts;
        ci.bytes = bytes;
        /* 路径可能从未写入（超时等），用 snprintf 保证 NUL 结尾 */
        snprintf(ci.path, sizeof(ci.path), "%s", path);

        pthread_mutex_lock(&g_jobs_lock);
        j->state = SNAP_DONE;
        j->done_ms = nsbc_now_ms();
        j->info = ci;
        __atomic_fetch_sub(&g_active_jobs, 1, __ATOMIC_RELAXED);
        snapshot_done_cb_t cb = j->cb;
        void *ud = j->ud;
        uint64_t id = j->id;
        pthread_mutex_unlock(&g_jobs_lock);

        llog(res == SNAPSHOT_OK ? LOG_INFO : LOG_WARN, "snapshot",
             "job=%" PRIu64 " leg=%d result=%s %dx%d frames=%d %" PRIu64 "ms "
             "drops=%u path=%s", id, ci.leg, snapshot_result_name(res),
             ci.width, ci.height, ci.frames_captured, ci.elapsed_ms,
             drops, ci.path[0] ? ci.path : "-");
        if (res == SNAPSHOT_NO_KEYFRAME)
            llog(LOG_WARN, "snapshot",
                 "job=%" PRIu64 " no keyframe AU seen; peer did not answer PLI "
                 "or GOP is too long", id);

        /* 结果通道（§3.4.4）：元数据走事件总线，不塞图像内容。
         * 事件名 SNAPSHOT；订阅方用 job_id 关联 snapshot_status 查询。 */
        {
            char ev[640];
            int n = snprintf(ev, sizeof(ev),
                "{\"job_id\":%" PRIu64 ",\"leg\":%d,\"result\":\"%s\","
                "\"width\":%d,\"height\":%d,\"frames\":%d,"
                "\"elapsed_ms\":%" PRIu64 ",\"bytes\":%" PRIu64 ","
                "\"ssrc\":%u,\"rtp_ts\":%u,\"path\":\"%s\"}",
                id, ci.leg, snapshot_result_name(res), ci.width, ci.height,
                ci.frames_captured, ci.elapsed_ms, ci.bytes,
                ci.ssrc, ci.rtp_ts, ci.path[0] ? ci.path : "");
            if (n > 0)
                (void)nsbc_event_push("SNAPSHOT", ev);
        }
        if (cb) cb(id, ud, &ci);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 对外 API                                                            */
/* ------------------------------------------------------------------ */

static uint64_t snap_start_fail(snapshot_result_t *why, snapshot_result_t r)
{
    if (why != NULL) *why = r;
    return 0;
}

uint64_t snapshot_job_start(bridge_mgr_t *m, void *call_key, int leg,
                            int timeout_ms, int want_frames,
                            snapshot_done_cb_t cb, void *ud,
                            snapshot_result_t *why)
{
    pthread_once(&g_once, snap_global_init);
    if (why != NULL) *why = SNAPSHOT_OK;

    if (m == NULL || call_key == NULL || leg < 0 || leg > 1)
        return snap_start_fail(why, SNAPSHOT_INVAL);
    if (want_frames < 1) want_frames = 1;
    if (want_frames > SNAPSHOT_MAX_FRAMES) want_frames = SNAPSHOT_MAX_FRAMES;

    if (!media_bridge_call_has_video(m, call_key, leg)) {
        llog(LOG_WARN, "snapshot", "call has no video on leg %d", leg);
        return snap_start_fail(why, SNAPSHOT_NO_VIDEO);
    }

    snap_job_t *j = NULL;
    pthread_mutex_lock(&g_jobs_lock);

    /* 每呼叫每腿限频：SNAPSHOT_MIN_INTERVAL_MS 内重复请求直接拒绝 */
    uint64_t now_ms = nsbc_now_ms();
    for (int i = 0; i < SNAP_CALLGATE_N; ++i) {
        if (g_callgate[i].call == call_key && g_callgate[i].leg == leg) {
            if (now_ms - g_callgate[i].last_ms < SNAPSHOT_MIN_INTERVAL_MS) {
                pthread_mutex_unlock(&g_jobs_lock);
                llog(LOG_WARN, "snapshot",
                     "rate limited: < %dms since last job", SNAPSHOT_MIN_INTERVAL_MS);
                return snap_start_fail(why, SNAPSHOT_BUSY);
            }
            g_callgate[i].last_ms = now_ms;
            goto gate_done;
        }
    }
    g_callgate[g_callgate_i].call = call_key;
    g_callgate[g_callgate_i].leg = leg;
    g_callgate[g_callgate_i].last_ms = now_ms;
    g_callgate_i = (g_callgate_i + 1) % SNAP_CALLGATE_N;
gate_done:

    for (int i = 0; i < SNAP_JOB_MAX; ++i) {
        if (g_jobs[i].state == SNAP_FREE) { j = &g_jobs[i]; break; }
    }
    if (j == NULL) {
        /* 无空闲槽：回收最早完成的作业 */
        uint64_t best_ms = (uint64_t)-1;
        for (int i = 0; i < SNAP_JOB_MAX; ++i) {
            if (g_jobs[i].state == SNAP_DONE && g_jobs[i].done_ms < best_ms) {
                best_ms = g_jobs[i].done_ms;
                j = &g_jobs[i];
            }
        }
        if (j == NULL) {
            pthread_mutex_unlock(&g_jobs_lock);
            llog(LOG_WARN, "snapshot", "busy: %d jobs active", g_active_jobs);
            return snap_start_fail(why, SNAPSHOT_BUSY);
        }
    }

    /* 抢占槽位后先解锁再 join：worker 结束时也要拿 g_jobs_lock，
     * 持锁 join 会死锁。RESERVED 状态阻止其它启动者重复选中该槽。 */
    pthread_t old = j->th;
    int need_join = j->th_started;
    j->th_started = 0;
    j->state = SNAP_RESERVED;
    pthread_mutex_unlock(&g_jobs_lock);

    if (need_join)
        pthread_join(old, NULL);

    pthread_mutex_lock(&g_jobs_lock);
    /* worker 收尾时会把 RESERVED 覆盖成 SNAP_DONE（此时已 join，安全）；
     * 只要槽位没被其它启动者抢占（FREE）就继续复用。 */
    if (j->state != SNAP_RESERVED && j->state != SNAP_DONE) {
        pthread_mutex_unlock(&g_jobs_lock);
        return snap_start_fail(why, SNAPSHOT_BUSY);
    }

    /* 只重置状态字段；互斥量在 snap_global_init 一次性初始化，
     * AU 缓冲保留复用（避免媒体线程 malloc）。
     * 这里必须持 j->lock：媒体 tap 可能已先拿到该锁在写旧作业的槽位。 */
    pthread_mutex_lock(&j->lock);
    j->state = SNAP_RUNNING;
    j->id = g_next_id++;
    j->m = m;
    j->call = call_key;
    j->leg = leg;
    j->want_frames = want_frames;
    j->timeout_ms = timeout_ms;
    j->cancel = 0;
    j->start_ms = nsbc_now_ms();
    j->done_ms = 0;
    j->cb = cb;
    j->ud = ud;
    j->got_key = 0;
    __atomic_store_n(&j->cid, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&j->drop_count, 0, __ATOMIC_RELAXED);
    for (int i = 0; i < 2; ++i) {
        j->blen[i] = 0;
        j->bkey[i] = 0;
        j->bts[i] = 0;
        j->bssrc[i] = 0;
        j->ready[i] = 0;
        j->busy[i] = 0;
    }
    memset(&j->info, 0, sizeof(j->info));
    j->info.result = -1;
    pthread_mutex_unlock(&j->lock);

    /* 控制线程预分配 AU 双槽，媒体 tap 热路径只做 memcpy。
     * 此时仍持 g_jobs_lock，tap 无法进入，分配完成前不会被看到。 */
    for (int i = 0; i < 2; ++i) {
        if (j->buf[i] == NULL)
            j->buf[i] = (uint8_t *)malloc(SNAP_AU_CAP);
    }
    if (j->buf[0] == NULL || j->buf[1] == NULL) {
        /* 预分配失败：直接失败，避免 worker 空等超时 */
        j->state = SNAP_FREE;
        pthread_mutex_unlock(&g_jobs_lock);
        llog(LOG_ERR, "snapshot", "AU buffer alloc failed (%d KB)",
             (int)(SNAP_AU_CAP / 1024));
        return snap_start_fail(why, SNAPSHOT_BUSY);
    }
    __atomic_fetch_add(&g_active_jobs, 1, __ATOMIC_RELAXED);

    if (pthread_create(&j->th, NULL, snap_job_thread, j) != 0) {
        j->state = SNAP_FREE;
        __atomic_fetch_sub(&g_active_jobs, 1, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&g_jobs_lock);
        llog(LOG_WARN, "snapshot", "pthread_create failed");
        return snap_start_fail(why, SNAPSHOT_BUSY);
    }
    j->th_started = 1;
    uint64_t id = j->id;
    pthread_mutex_unlock(&g_jobs_lock);

    llog(LOG_INFO, "snapshot", "job=%" PRIu64 " started leg=%d frames=%d",
         id, leg, want_frames);
    return id;
}

int snapshot_job_get(uint64_t job_id, snapshot_result_info_t *out, int *running)
{
    if (job_id == 0 || out == NULL) return NSBC_INVAL;
    if (running) *running = 0;

    pthread_mutex_lock(&g_jobs_lock);
    for (int i = 0; i < SNAP_JOB_MAX; ++i) {
        if (g_jobs[i].state != SNAP_FREE && g_jobs[i].id == job_id) {
            *out = g_jobs[i].info;
            if (g_jobs[i].state == SNAP_RUNNING) {
                out->result = -1;
                if (running) *running = 1;
            }
            pthread_mutex_unlock(&g_jobs_lock);
            return NSBC_OK;
        }
    }
    pthread_mutex_unlock(&g_jobs_lock);
    return NSBC_NOENT;
}

void snapshot_job_purge_all(void)
{
    pthread_mutex_lock(&g_jobs_lock);
    for (int i = 0; i < SNAP_JOB_MAX; ++i)
        if (g_jobs[i].state == SNAP_RUNNING)
            __atomic_store_n(&g_jobs[i].cancel, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_jobs_lock);

    for (int i = 0; i < SNAP_JOB_MAX; ++i) {
        snap_job_t *j = &g_jobs[i];
        if (j->th_started) {
            pthread_join(j->th, NULL);
            j->th_started = 0;
        }
    }

    pthread_mutex_lock(&g_jobs_lock);
    for (int i = 0; i < SNAP_JOB_MAX; ++i) {
        snap_job_t *j = &g_jobs[i];
        pthread_mutex_lock(&j->lock);   /* 与媒体 tap 互斥 */
        free(j->buf[0]); j->buf[0] = NULL;
        free(j->buf[1]); j->buf[1] = NULL;
        j->state = SNAP_FREE;
        j->id = 0;
        pthread_mutex_unlock(&j->lock);
    }
    pthread_mutex_unlock(&g_jobs_lock);
}
