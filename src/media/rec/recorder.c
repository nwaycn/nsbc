/* 通话录音（REQ-REC-001~004，P1 真实现）
 *
 * 文件命名：base_dir/yyyymmdd/<callid>_<mix|a|b>_<seq>.wav
 * 原子写：open → fseek → 44B WAV头 → 运行时 fwrite PCM → stop 时回填头 size 字段 → fsync
 * 线程模型：rec_feed 来自媒体快路径（bridge/mixer PCM 帧回调），rec_stop 来自
 * dialplan 动作（通常在呼叫控制线程）。rec_sess 用 pthread_mutex_t lock
 * 保护 FILE* + bytes。暂停时 frame 正常丢弃（state==PAUSED → return OK 不报错）。
 *
 * REQ-REC-006：rec_feed_raw 是媒体桥 raw G711 路径
 *   → 解码 G711 成 PCM → 同步喂 s->fp（WAV）+ raw_fp（原始）
 *   G711 解码用 src/media/codec/g711.c 查表（16-bit PCM mono 8kHz）。 */

#include "recorder.h"
#include "nsbc_common.h"
#include "log.h"
#include "g711.h"          /* G.711 A-law/μ-law 查表解码 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alloca.h>       /* Linux/glibc alloca() — rec_feed_raw 栈上 PCM 解码缓冲区 */
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

/* §17 static 函数前向声明（文件内部使用） */
static void fill_wav_header(uint8_t *hdr, int rate, int channels);
static void patch_wav_sizes(uint8_t *hdr, uint64_t data_bytes);

/* mkdir_p 已在 src/base/log.c 实现，include/log.h 已声明 —— 直接用 */

#define REC_SEQ_MAX   99999
#define REC_DATE_MAX  16

static const uint8_t WAV_HEADER_SKELETON[44] = {
    'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
    16,0,0,0,    /* fmt_size=16 */
    1,0,          /* format_tag=1 (PCM) */
    1,0,          /* channels=1 */
    0,0,0,0,      /* sample_rate placeholder */
    0,0,0,0,      /* byte_rate placeholder */
    2,0,          /* block_align placeholder */
    16,0,         /* bits_per_sample=16 */
    'd','a','t','a',
    0,0,0,0       /* data_size placeholder */
};

/* ---------- rec_mgr 完整定义（recorder.h 只有前向声明） ---------- */
struct rec_mgr {
    rec_mgr_cfg_t  cfg;     /* 配置：路径/格式/采样率/声道/并发上限 */
    rec_stat_t     st;      /* OAM 计数：active/total/failed/bytes */
};

/* ---------- rec_sess 扩展 ---------- */

struct rec_sess {
    rec_mgr_t       *mgr;
    rec_open_t       req;
    rec_state_t      state;
    _Atomic int      closing;     /* rec_stop 置 1，rec_feed/rec_feed_raw 快路径跳过 */
    _Atomic int      segment_switching; /* segment close/open 进行中 —— 快路径 skip 这包 */
    uint64_t         bytes;      /* 当前 segment 已写 PCM 字节数（不含 WAV 头） */
    uint64_t         total_bytes;/* 本次录音总累计（所有 segment 合计）*/
    char             tmp_path[REC_PATH_MAX];
    char             final_path[REC_PATH_MAX];   /* 当前 segment final path */
    char             date_dir[REC_PATH_MAX];      /* 复用 segment_open_next */
    char             leg_sfx[8];                   /* "a" / "b" —— 每条 leg 独立 mono sess */
    int              rate;       /* 第一帧协商后锁定的采样率（G711=8000）*/
    int              channels;   /* ★ 永远=1 —— 录音架构是分腿 mono + stop 时离线 merge_stereo_wav */
    int              seq;        /* 当前 segment 序号 */
    int              rate_locked; /* 第一帧已适配 —— 不再改 */
    int              feed_mode;  /* 0=未定, 1=PCM(rec_feed), 2=G711_RAW(rec_feed_raw) —— 只允许一条路径写 fp */
    FILE            *fp;
    /* REQ-REC-006：原始 G.711 payload 直写（延迟 open，首帧 feed_raw 时创建）
     * 无 WAV 头，纯原始字节。raw_fp 独立于 fp（PCM 路径）。
     * raw_bytes 独立于 bytes（bytes 用于 WAV header data_size 计算）。*/
    FILE            *raw_fp;
    char             raw_ext[8];
    uint64_t         raw_bytes;   /* 原始 payload 累计字节（独立计数，不污染 WAV 头） */
    pthread_mutex_t  lock;        /* 仅保护 segment close/open 和 fclose/rewind 等非线程安全 FILE* 操作 */
};

/* ---------- segment helpers（在 lock 保护下调用） ---------- */

/* patch WAV �?+ fsync + close + rename tmp �?final；置 s->fp=NULL */
static int segment_close_locked(rec_sess_t *s)
{
    int rc = NSBC_OK;
    if (s->fp == NULL) return NSBC_OK;
    s->segment_switching = 1;  /* 进入 segment 切换（快路径检测到后 skip 本帧） */

    rewind(s->fp);
    uint8_t hdr[44];
    fill_wav_header(hdr, s->rate, s->channels);
    patch_wav_sizes(hdr, (uint32_t)s->bytes);
    if (fwrite(hdr, 1, 44, s->fp) != 44) rc = NSBC_ERR;
    fflush(s->fp);
    fsync(fileno(s->fp));
    fclose(s->fp);
    s->fp = NULL;

    if (rc == NSBC_OK && rename(s->tmp_path, s->final_path) != 0)
        rc = NSBC_ERR;
    return rc;
}

/* 开一个新 segment（seq 自动 +1）；写完�?s->tmp_path/final_path/s->fp/s->bytes/seq 都更�?*/
static int segment_open_next_locked(rec_sess_t *s)
{
    int next_seq = s->seq + 1;
    if (next_seq >= REC_SEQ_MAX) return NSBC_NOSPC;

    snprintf(s->tmp_path, sizeof(s->tmp_path),
        "%s/%s_%s_%d.wav.tmp", s->date_dir, s->req.callid, s->leg_sfx, next_seq);
    size_t tl = strlen(s->tmp_path);
    memcpy(s->final_path, s->tmp_path, tl - 4);
    s->final_path[tl - 4] = '\0';

    s->fp = fopen(s->tmp_path, "wb");
    if (s->fp == NULL) return NSBC_ERR;
    (void)setvbuf(s->fp, NULL, _IOFBF, 65536);
    uint8_t hdr[44];
    fill_wav_header(hdr, s->rate, s->channels);
    if (fwrite(hdr, 1, 44, s->fp) != 44) {
        fclose(s->fp); s->fp = NULL;
        return NSBC_ERR;
    }
    s->bytes = 0;
    s->seq = next_seq;
    s->segment_switching = 0;  /* segment 切换完成 */
    return NSBC_OK;
}

/* ---------- util ---------- */

static int ensure_date_dir(const char *base, char *out, size_t outsz)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char date[REC_DATE_MAX];
    snprintf(date, sizeof(date), "%04d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    snprintf(out, outsz, "%s/%s", base, date);
    for (char *p = out + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(out, 0755);
            *p = '/';
        }
    }
    (void)mkdir(out, 0755);
    return NSBC_OK;
}

static void fill_wav_header(uint8_t *hdr, int rate, int channels)
{
    memcpy(hdr, WAV_HEADER_SKELETON, 44);
    int byte_rate   = rate * channels * 2;
    int block_align = channels * 2;
    /* channels 1�?1 00, 2�?2 00 ... (little-endian uint16) */
    hdr[22] = (uint8_t)(channels & 0xff);
    hdr[23] = (uint8_t)((channels >> 8) & 0xff);
    /* sample_rate uint32 little-endian */
    hdr[24] = (uint8_t)(rate & 0xff);
    hdr[25] = (uint8_t)((rate >> 8) & 0xff);
    hdr[26] = (uint8_t)((rate >> 16) & 0xff);
    hdr[27] = (uint8_t)((rate >> 24) & 0xff);
    /* byte_rate */
    hdr[28] = (uint8_t)(byte_rate & 0xff);
    hdr[29] = (uint8_t)((byte_rate >> 8) & 0xff);
    hdr[30] = (uint8_t)((byte_rate >> 16) & 0xff);
    hdr[31] = (uint8_t)((byte_rate >> 24) & 0xff);
    /* block_align uint16 */
    hdr[32] = (uint8_t)(block_align & 0xff);
    hdr[33] = (uint8_t)((block_align >> 8) & 0xff);
}

static void patch_wav_sizes(uint8_t *hdr, uint64_t data_bytes)
{
    /* riff_size = data_bytes + 36 (head total - 8) */
    uint32_t riff = (uint32_t)(data_bytes + 36);
    hdr[4]  = (uint8_t)(riff & 0xff);
    hdr[5]  = (uint8_t)((riff >> 8) & 0xff);
    hdr[6]  = (uint8_t)((riff >> 16) & 0xff);
    hdr[7]  = (uint8_t)((riff >> 24) & 0xff);

    hdr[40] = (uint8_t)(data_bytes & 0xff);
    hdr[41] = (uint8_t)((data_bytes >> 8) & 0xff);
    hdr[42] = (uint8_t)((data_bytes >> 16) & 0xff);
    hdr[43] = (uint8_t)((data_bytes >> 24) & 0xff);
}

/* ---------- mgr lifecycle ---------- */

int rec_mgr_create(const rec_mgr_cfg_t *cfg, rec_mgr_t **out)
{
    rec_mgr_t *m;
    if (cfg == NULL || out == NULL) return NSBC_INVAL;
    m = (rec_mgr_t *)calloc(1, sizeof(*m));
    if (m == NULL) return NSBC_NOMEM;
    m->cfg = *cfg;
    *out = m;
    return NSBC_OK;
}

void rec_mgr_destroy(rec_mgr_t *m)
{
    free(m);
}

const rec_mgr_cfg_t *rec_mgr_get_cfg(rec_mgr_t *m)
{
    return m ? &m->cfg : NULL;
}

/* ---------- rec_open ---------- */

int rec_open(rec_mgr_t *m, const rec_open_t *req, rec_sess_t **out)
{
    rec_sess_t *s;
    if (m == NULL || req == NULL || out == NULL || req->callid[0] == '\0')
        return NSBC_INVAL;

    s = (rec_sess_t *)calloc(1, sizeof(*s));
    if (s == NULL) return NSBC_NOMEM;

    s->mgr = m;
    s->req = *req;
    s->state = REC_ST_RECORDING;
    s->rate = m->cfg.rate > 0 ? m->cfg.rate : 8000;
    s->channels = 1;  /* 架构硬约束：永远 mono sess —— 分腿独立录 + stop 时离线 merge_stereo_wav */
    s->seq = 0;
    s->segment_switching = 0;
    s->feed_mode = 0;  /* 未定 —— 第一帧进来时由 rec_feed/rec_feed_raw 锁定 */
    pthread_mutex_init(&s->lock, NULL);

    /* 1. 日期目录 + 存储�?sess */
    if (ensure_date_dir(m->cfg.base_dir, s->date_dir, sizeof(s->date_dir)) != NSBC_OK) {
        pthread_mutex_destroy(&s->lock); free(s); return NSBC_ERR;
    }

    /* 2. �?leg 标记存到 sess，供 segment_open_next 复用 */
    /* leg 标记 —— 每条 leg 独立 mono 文件（merge_stereo_wav 按 a/b 合并成真正 stereo）*/
    if (req->leg_mask & 1)      snprintf(s->leg_sfx, sizeof(s->leg_sfx), "a");
    else if (req->leg_mask & 2) snprintf(s->leg_sfx, sizeof(s->leg_sfx), "b");
    else                        snprintf(s->leg_sfx, sizeof(s->leg_sfx), "x");

    /* 3. 找空�?seq 开当前 segment */
    for (int seq = 0; seq < REC_SEQ_MAX; ++seq) {
        snprintf(s->tmp_path, sizeof(s->tmp_path),
            "%s/%s_%s_%d.wav.tmp", s->date_dir, req->callid, s->leg_sfx, seq);
        if (access(s->tmp_path, F_OK) != 0) {
            size_t tl = strlen(s->tmp_path);
            memcpy(s->final_path, s->tmp_path, tl - 4);
            s->final_path[tl - 4] = '\0';
            s->seq = seq;
            break;
        }
    }

    s->fp = fopen(s->tmp_path, "wb");
    if (s->fp == NULL) {
        pthread_mutex_destroy(&s->lock); free(s); return NSBC_ERR;
    }
    /* 64KB 全缓冲 —— 8kHz 16bit mono = 16000B/s，64KB 可缓存 ~4s，
     * 大幅减少隐式 fflush 触发的 syscall 次数 */
    (void)setvbuf(s->fp, NULL, _IOFBF, 65536);
    uint8_t hdr[44];
    fill_wav_header(hdr, s->rate, s->channels);
    if (fwrite(hdr, 1, 44, s->fp) != 44) {
        fclose(s->fp); unlink(s->tmp_path);
        pthread_mutex_destroy(&s->lock); free(s); return NSBC_ERR;
    }

    ++m->st.active;
    *out = s;
    return NSBC_OK;
}

/* ---------- pause/resume ---------- */

int rec_pause(rec_sess_t *s)
{
    if (s == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&s->lock);
    if (s->state != REC_ST_RECORDING) {
        pthread_mutex_unlock(&s->lock); return NSBC_INVAL;
    }
    s->state = REC_ST_PAUSED;
    pthread_mutex_unlock(&s->lock);
    return NSBC_OK;
}

int rec_resume(rec_sess_t *s)
{
    if (s == NULL) return NSBC_INVAL;
    pthread_mutex_lock(&s->lock);
    if (s->state != REC_ST_PAUSED) {
        pthread_mutex_unlock(&s->lock); return NSBC_INVAL;
    }
    s->state = REC_ST_RECORDING;
    pthread_mutex_unlock(&s->lock);
    return NSBC_OK;
}

/* ---------- rec_feed ---------- */

/*
 * 快路径（每 20ms 一帧 PCM —— 媒体线程调用，严格串行）：
 *   - 无 mutex，仅 2 个 atomic load + FILE* fwrite（libc 内部锁） + bytes++
 *   - segment 切换进行中（segment_switching=1）→ skip 这包（掉 1 帧可接受）
 *   - segment close/open、第一帧 WAV 头重写这些 FILE* 非线程安全操作 → 持 mutex
 */
int rec_feed(rec_sess_t *s, int leg, const media_frame_t *frm)
{
    if (s == NULL || frm == NULL || frm->pcm == NULL) return NSBC_INVAL;
    /* ★ 关闭中快路径 —— rec_stop 已置 closing=1，立即返回避免进锁 */
    if (__atomic_load_n(&s->closing, __ATOMIC_RELAXED)) return NSBC_OK;
    /* ★ segment 切换中 —— skip 这一包（segment_close 进行中，rewind/fclose 中）*/
    if (__atomic_load_n(&s->segment_switching, __ATOMIC_RELAXED)) return NSBC_OK;

    /* state / rate_locked / fp 都是只读检查，无需锁
     * （rec_stop 先置 closing=1 再进锁，segment 切换先置 segment_switching=1）*/
    if (s->state != REC_ST_RECORDING || s->fp == NULL) return NSBC_OK;

    /* ★ feed_mode 锁：只允许一条路径（PCM 或 G711_RAW）写 fp
     * raw 路径先跑了 → feed 拒绝；feed 先跑了 → raw 拒绝 */
    if (__atomic_load_n((int *)&s->feed_mode, __ATOMIC_RELAXED) == 2) {
        llog(LOG_WARN, "rec", "rec_feed: feed_mode=2(G711_RAW) —— skipping (double-write guard)");
        return NSBC_OK;
    }
    __atomic_store_n((int *)&s->feed_mode, 1, __ATOMIC_RELAXED);

    /* 单腿模式过滤（无锁，mode/leg_mask 初始化后不变）*/
    if (s->req.mode == REC_MODE_SINGLE && leg >= 0) {
        if ((leg == 0 && !(s->req.leg_mask & 1)) ||
            (leg == 1 && !(s->req.leg_mask & 2)))
            return NSBC_OK;
    }

    /* ★ 架构硬约束：channels 永远=1 —— 不允许任何路径修改它 */
    if (frm->channels > 0 && frm->channels != 1) {
        llog(LOG_WARN, "rec", "rec_feed: frm->channels=%d != 1 (mono sess hard constraint)", frm->channels);
        /* 强制按 mono 算 nbytes（忽略 frm->channels）*/
    }

    size_t nbytes = frm->samples * 2;  /* 永远 mono: samples × 2 bytes (int16) */
    if (nbytes == 0) return NSBC_OK;

    /* 第一帧 rate 锁定 —— FILE* 非线程安全操作必须持锁
     * 仅第一帧进入一次，后续帧 rate_locked=1 跳过
     * ★ channels 不再改 —— 永远=1 */
    if (!s->rate_locked) {
        pthread_mutex_lock(&s->lock);
        if (s->closing || s->state != REC_ST_RECORDING || s->fp == NULL) {
            pthread_mutex_unlock(&s->lock); return NSBC_OK;
        }
        if (frm->rate > 0 && frm->rate != s->rate) {
            s->rate = frm->rate;
            rewind(s->fp);
            uint8_t hdr[44];
            fill_wav_header(hdr, s->rate, 1);  /* channels 永远=1 */
            if (fwrite(hdr, 1, 44, s->fp) == 44)
                fseek(s->fp, 44, SEEK_SET);
        }
        s->rate_locked = 1;
        pthread_mutex_unlock(&s->lock);
    }

    /* ★★★ 快路径：无锁 fwrite + bytes 更新 ★★★
     * fwrite 内部 libc FILE* 自带锁，安全。bytes/total_bytes 是普通 uint64_t，
     * 因为调用来自媒体线程（严格串行，一次只一个线程进来），不需要 atomic。 */
    uint64_t cap = s->mgr->cfg.max_bytes;
    if (cap > 0 && s->bytes + nbytes > cap) {
        /* 需要 segment 切换 —— CAS 抢 segment_switching flag，确保只有一个线程切换
         * 其他检测到 flag=1 的线程在下次调用时 skip */
        int expected = 0;
        if (__atomic_compare_exchange_n(&s->segment_switching, &expected, 1,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            pthread_mutex_lock(&s->lock);
            if (s->closing) { pthread_mutex_unlock(&s->lock); return NSBC_OK; }
            (void)segment_close_locked(s);    /* 内部也会置 segment_switching=1（幂等）*/
            if (segment_open_next_locked(s) != NSBC_OK) {
                s->state = REC_ST_FAILED;
                pthread_mutex_unlock(&s->lock); return NSBC_OK;
            }
            pthread_mutex_unlock(&s->lock);
            /* segment_open_next_locked 末尾已经重置 segment_switching=0 */
        } else {
            /* 另一个线程已经在切 segment，skip 本帧 */
            return NSBC_OK;
        }
    }

    if (fwrite(frm->pcm, 1, nbytes, s->fp) == nbytes) {
        s->bytes += nbytes;
        s->total_bytes += nbytes;
    }
    return NSBC_OK;
}

/* ---------- rec_stop ---------- */

int rec_stop(rec_sess_t *s, char *path_out, size_t path_cap, uint64_t *bytes_out)
{
    if (s == NULL) return NSBC_INVAL;
    /* ★ 先置 closing=1 —— 媒体线程的 rec_feed/rec_feed_raw 快路径检测到后立即 return，
     * 不会再进入 s->lock。从这一刻起 mutex 不会再被争用，可以安全 destroy。 */
    __atomic_store_n(&s->closing, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&s->lock);

    int rc = segment_close_locked(s);
    /* REQ-REC-006：关�?raw_fp（G.711 原始路径�?*/
    if (s->raw_fp) {
        fflush(s->raw_fp);
        fsync(fileno(s->raw_fp));
        fclose(s->raw_fp);
        s->raw_fp = NULL;
    }
    s->state = rc == NSBC_OK ? REC_ST_STOPPED : REC_ST_FAILED;

    /* path_out 输出当前最后一�?segment �?final_path —�?CDR 里显示的是主文件�?     * �?segment 场景�?seq>0 时我们把 base path（不�?_seq）返回给 caller�?     * 但实际落盘路径是�?seq 的。Web 层按 path glob 能找全所�?segment�?*/
    if (path_out != NULL && path_cap > 0) {
        if (rc == NSBC_OK) {
            snprintf(path_out, path_cap, "%s", s->final_path);
        } else {
            path_out[0] = '\0';
        }
    }
    if (bytes_out != NULL) *bytes_out = s->total_bytes;

    pthread_mutex_unlock(&s->lock);
    pthread_mutex_destroy(&s->lock);  /* closing=1 后无新进锁者，安全 destroy */

    if (s->mgr) {
        __atomic_fetch_sub(&s->mgr->st.active, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s->mgr->st.total, 1, __ATOMIC_RELAXED);
        if (rc != NSBC_OK)
            __atomic_fetch_add(&s->mgr->st.failed, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s->mgr->st.bytes_written, s->total_bytes, __ATOMIC_RELAXED);
    }
    free(s);
    return rc;
}

/* ---------- misc ---------- */

rec_state_t rec_state(const rec_sess_t *s)
{
    return s == NULL ? REC_ST_IDLE : s->state;
}

/* REQ-REC-006：喂原始 RTP payload（G.711a/u 直写 raw 文件 + 解码写 WAV）
 *
 * 无锁快路径（★ 每包 20ms，媒体线程严格串行）：
 *   - 仅 2 个 atomic load + FILE* fwrite（libc 内部锁） + bytes++
 *   - segment 切换 / raw_fp 延迟 open（第一包）才持 mutex
 *   - segment_switching=1 期间 skip（掉 1 帧可接受）*/
int rec_feed_raw(rec_sess_t *s, int leg, const void *payload,
                 size_t len, int payload_type)
{
    (void)leg;
    if (s == NULL || payload == NULL || len == 0) return NSBC_INVAL;
    /* ★ 关闭中快路径 —— rec_stop 已置 closing=1，立即返回避免进锁 */
    if (__atomic_load_n(&s->closing, __ATOMIC_RELAXED)) return NSBC_OK;
    /* ★ segment 切换中 —— skip 这一包（rewind/fclose/fopen 进行中）*/
    if (__atomic_load_n(&s->segment_switching, __ATOMIC_RELAXED)) return NSBC_OK;
    if (s->state != REC_ST_RECORDING) return NSBC_OK;

    /* ★ alloca 硬上限 —— SIP/RTP payload 不可能 >4KB */
    if (len > 4096) return NSBC_INVAL;

    /* G.711 → PCM 查表解码（栈缓冲 + 查表，零 malloc）*/
    int16_t *pcm_buf = (int16_t *)alloca(len * sizeof(int16_t));
    size_t pcm_samples = 0;
    switch (payload_type) {
        case 0:  pcm_samples = g711_ulaw_decode((const uint8_t *)payload, len, pcm_buf); break;
        case 8:  pcm_samples = g711_alaw_decode((const uint8_t *)payload, len, pcm_buf); break;
        default: pcm_samples = 0;  /* 非 G711 PT 暂不支持 PCM 解码 */
    }

    /* ★ feed_mode 锁：只允许一条路径（PCM 或 G711_RAW）写 fp
     * feed 路径先跑了 → raw 拒绝；raw 先跑了 → 锁定 mode=2 */
    int cur_mode = __atomic_load_n((int *)&s->feed_mode, __ATOMIC_RELAXED);
    if (cur_mode == 1) {
        llog(LOG_WARN, "rec", "rec_feed_raw: feed_mode=1(PCM) —— skipping (double-write guard)");
        return NSBC_OK;
    }
    __atomic_store_n((int *)&s->feed_mode, 2, __ATOMIC_RELAXED);

    /* ★ 架构硬约束：channels 永远=1 —— 禁止任何 stereo 转换 */
    if (s->channels >= 2) {
        llog(LOG_WARN, "rec", "rec_feed_raw: channels=%d (should be 1) —— FIXED", s->channels);
        s->channels = 1;
    }

    /* ---- raw_fp 延迟 open（持锁 —— FILE* 非线程安全 open） ----
     * 仅首帧触发一次，raw_fp 不为 NULL 时跳过锁 */
    if (s->raw_fp == NULL) {
        pthread_mutex_lock(&s->lock);
        if (s->raw_fp == NULL && !s->closing && s->state == REC_ST_RECORDING) {
            const char *ext;
            switch (payload_type) {
                case 0:  ext = "g711u"; break;
                case 8:  ext = "g711a"; break;
                default: ext = "raw";   break;
            }
            snprintf(s->raw_ext, sizeof(s->raw_ext), "%s", ext);
            char path[REC_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s_%s_raw.%s",
                     s->date_dir, s->req.callid, s->leg_sfx, ext);
            s->raw_fp = fopen(path, "ab");
            if (s->raw_fp != NULL) {
                (void)setvbuf(s->raw_fp, NULL, _IOFBF, 65536);
            }
        }
        pthread_mutex_unlock(&s->lock);
        if (s->raw_fp == NULL) return NSBC_ERR;
    }

    /* ★★★ 快路径：无锁 fwrite —— libc FILE* 内部锁保护 ★★★ */
    size_t w = fwrite(payload, 1, len, s->raw_fp);
    if (w != len) return NSBC_ERR;
    s->raw_bytes += len;

    /* ---- 写 PCM → WAV ----
     * ★ 永远 mono sess —— 不做 mono→stereo 复制（那是 MIXED 死路径的噪音根因）
     * stop 时 recording_apply_dp_action 会调 merge_stereo_wav 离线合并 a/b 两条 mono */
    if (pcm_samples > 0 && s->fp != NULL) {
        size_t pcm_bytes = pcm_samples * 2;  /* 永远 mono: samples × 2 bytes (int16) */

        /* max_bytes 切段保护 —— CAS 抢 segment_switching */
        uint64_t cap = s->mgr->cfg.max_bytes;
        if (cap > 0 && s->bytes + pcm_bytes > cap) {
            int expected = 0;
            if (__atomic_compare_exchange_n(&s->segment_switching, &expected, 1,
                                            0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                pthread_mutex_lock(&s->lock);
                if (s->closing) { pthread_mutex_unlock(&s->lock); return NSBC_OK; }
                (void)segment_close_locked(s);
                if (segment_open_next_locked(s) != NSBC_OK) {
                    s->state = REC_ST_FAILED;
                    pthread_mutex_unlock(&s->lock); return NSBC_OK;
                }
                pthread_mutex_unlock(&s->lock);
            } else {
                return NSBC_OK;  /* 另一个线程在切 segment，skip */
            }
        }

        if (fwrite(pcm_buf, 1, pcm_bytes, s->fp) == pcm_bytes) {
            s->bytes += pcm_bytes;
            s->total_bytes += pcm_bytes;
        }
    }

    return NSBC_OK;
}

int rec_is_silence(const media_frame_t *frm, int threshold_dbov)
{
    size_t i; int64_t acc = 0;
    if (frm == NULL || frm->samples == 0) return 1;
    for (i = 0; i < frm->samples; ++i)
        acc += (int64_t)frm->pcm[i] * frm->pcm[i];
    (void)threshold_dbov;
    return acc == 0;
}

/* ---------- REQ-REC-007: path template 变量替换 ---------- */

int resolve_rec_path_template(char *out, size_t out_cap,
                              const char *template,
                              const char *caller, const char *callee,
                              const char *callid, const char *base_dir)
{
    if (out == NULL || out_cap == 0 || callid == NULL || !callid[0])
        return NSBC_INVAL;
    if (base_dir == NULL || !base_dir[0])
        base_dir = "./recordings";

    /* 日期时间（UTC） */
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char date[16], time_str[16];
    snprintf(date, sizeof(date), "%04d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    snprintf(time_str, sizeof(time_str), "%02d%02d%02d",
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    char yyyy[8], mm[4], dd[4];
    snprintf(yyyy, sizeof(yyyy), "%04d", tm.tm_year + 1900);
    snprintf(mm,    sizeof(mm),    "%02d", tm.tm_mon + 1);
    snprintf(dd,    sizeof(dd),    "%02d", tm.tm_mday);

    /* 模板为空 → 默认：base_dir/date/{caller}-{callee}-{callid}.wav
     * REQ-REC-007：默认包含 caller/callee/callid，用户不配置也能直接按分机号检索 */
    if (template == NULL || !template[0]) {
        if (!caller || !caller[0]) caller = "anon";
        if (!callee || !callee[0]) callee = "unknown";
        return snprintf(out, out_cap, "%s/%s/%s-%s-%s.wav",
                        base_dir, date, caller, callee, callid);
    }

    /* 扫描模板字符串，替换 {var} */
    size_t pos = 0;
    for (const char *p = template; *p && pos + 1 < out_cap; ++p) {
        if (*p == '{' && *(p + 1)) {
            const char *end = strchr(p, '}');
            if (!end) { out[pos++] = *p; continue; }
            size_t varlen = (size_t)(end - p - 1);
            char var[32];
            if (varlen >= sizeof(var) - 1) varlen = sizeof(var) - 1;
            memcpy(var, p + 1, varlen); var[varlen] = '\0';

            const char *val = NULL;
            if (!strcmp(var, "caller"))      val = (caller && caller[0]) ? caller : "anon";
            else if (!strcmp(var, "callee")) val = (callee && callee[0]) ? callee : "unknown";
            else if (!strcmp(var, "callid")) val = callid;
            else if (!strcmp(var, "date"))   val = date;
            else if (!strcmp(var, "time"))   val = time_str;
            else if (!strcmp(var, "yyyy"))   val = yyyy;
            else if (!strcmp(var, "mm"))     val = mm;
            else if (!strcmp(var, "dd"))     val = dd;
            else {
                /* 未知变量 → 原样输出 {xxx} */
                if (pos + varlen + 2 >= out_cap) break;
                out[pos++] = '{';
                memcpy(out + pos, var, varlen); pos += varlen;
                out[pos++] = '}';
                p = end;
                continue;
            }
            size_t vlen = strlen(val);
            if (pos + vlen >= out_cap) break;
            memcpy(out + pos, val, vlen); pos += vlen;
            p = end; /* 跳过 } */
        } else {
            out[pos++] = *p;
        }
    }
    out[pos] = '\0';

    /* 如果模板里没带目录前缀 → 自动加 date 子目录 */
    if (!strchr(out, '/') && !strchr(out, '\\')) {
        char with_dir[REC_PATH_MAX];
        snprintf(with_dir, sizeof(with_dir), "%s/%s/%s", base_dir, date, out);
        snprintf(out, out_cap, "%s", with_dir);
    } else if (out[0] != '/' && base_dir[0] == '/') {
        /* 绝对路径模板 → 直接用；否则拼 base_dir */
        char full[REC_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", base_dir, out);
        snprintf(out, out_cap, "%s", full);
    }
    return NSBC_OK;
}

/* ---------- REQ-REC-008: 两条 mono WAV → stereo 合并 ---------- */

/* 从 mono WAV 文件里解析出样本数 + PCM 数据（跳过 44B 头） */
static int read_mono_wav_pcm(const char *path, int *rate_out,
                              int16_t **pcm_out, size_t *samples_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NSBC_ERR;

    uint8_t hdr[44];
    if (fread(hdr, 1, 44, f) != 44) { fclose(f); return NSBC_ERR; }
    /* 验证 WAV magic */
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f); return NSBC_ERR;
    }
    int rate = (int)(hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24));
    int channels = (int)(hdr[22] | (hdr[23] << 8));
    /* recorder.c 写的 WAV 是标准 44 字节头，data_size 在 offset 40-43 */
    uint32_t data_size = (uint32_t)(hdr[40] | (hdr[41] << 8) | (hdr[42] << 16) | (hdr[43] << 24));

    int16_t *pcm = (int16_t *)malloc(data_size > 0 ? data_size : 2);
    if (!pcm) { fclose(f); return NSBC_NOMEM; }
    size_t rd = fread(pcm, 1, data_size, f);
    fclose(f);
    (void)channels;
    *rate_out = rate;
    *pcm_out = pcm;
    *samples_out = rd / 2;
    return NSBC_OK;
}

int merge_stereo_wav(const char *a_path, const char *b_path,
                     const char *out_path)
{
    if (!a_path || !b_path || !out_path) return NSBC_INVAL;

    int rate_a = 0, rate_b = 0;
    int16_t *pcm_a = NULL, *pcm_b = NULL;
    size_t samp_a = 0, samp_b = 0;

    if (read_mono_wav_pcm(a_path, &rate_a, &pcm_a, &samp_a) != NSBC_OK)
        return NSBC_ERR;
    if (read_mono_wav_pcm(b_path, &rate_b, &pcm_b, &samp_b) != NSBC_OK) {
        free(pcm_a); return NSBC_ERR;
    }
    int rate = rate_a > 0 ? rate_a : rate_b;

    size_t mix_samp = samp_a > samp_b ? samp_a : samp_b;
    size_t mix_bytes = mix_samp * 2 * 2; /* stereo 16bit = 4 bytes/sample */
    int16_t *stereo = (int16_t *)malloc(mix_bytes > 0 ? mix_bytes : 2);
    if (!stereo) { free(pcm_a); free(pcm_b); return NSBC_NOMEM; }

    for (size_t i = 0; i < mix_samp; ++i) {
        int16_t la = (i < samp_a) ? pcm_a[i] : 0;
        int16_t rb = (i < samp_b) ? pcm_b[i] : 0;
        stereo[i * 2]     = la;  /* A-leg → 左声道 */
        stereo[i * 2 + 1] = rb;  /* B-leg → 右声道 */
    }
    free(pcm_a); free(pcm_b);

    /* 写 stereo WAV —— 先确保目录存在（模板可能带多级子目录） */
    {
        char dir[REC_PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", out_path);
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = '\0'; mkdir_p(dir); }
    }
    FILE *fo = fopen(out_path, "wb");
    if (!fo) { free(stereo); return NSBC_ERR; }
    uint8_t hdr[44];
    memcpy(hdr, WAV_HEADER_SKELETON, 44);
    int byte_rate = rate * 2 * 2;
    hdr[22] = 2; hdr[23] = 0;  /* channels=2 */
    hdr[24] = (uint8_t)(rate & 0xff); hdr[25] = (uint8_t)((rate >> 8) & 0xff);
    hdr[26] = (uint8_t)((rate >> 16) & 0xff); hdr[27] = (uint8_t)((rate >> 24) & 0xff);
    hdr[28] = (uint8_t)(byte_rate & 0xff); hdr[29] = (uint8_t)((byte_rate >> 8) & 0xff);
    hdr[30] = (uint8_t)((byte_rate >> 16) & 0xff); hdr[31] = (uint8_t)((byte_rate >> 24) & 0xff);
    hdr[32] = 4; hdr[33] = 0;  /* block_align=4 */
    uint32_t data_sz = (uint32_t)mix_bytes;
    uint32_t riff_sz = data_sz + 36;
    hdr[4]  = (uint8_t)(riff_sz & 0xff); hdr[5]  = (uint8_t)((riff_sz >> 8) & 0xff);
    hdr[6]  = (uint8_t)((riff_sz >> 16) & 0xff); hdr[7]  = (uint8_t)((riff_sz >> 24) & 0xff);
    hdr[40] = (uint8_t)(data_sz & 0xff); hdr[41] = (uint8_t)((data_sz >> 8) & 0xff);
    hdr[42] = (uint8_t)((data_sz >> 16) & 0xff); hdr[43] = (uint8_t)((data_sz >> 24) & 0xff);

    fwrite(hdr, 1, 44, fo);
    fwrite(stereo, 1, mix_bytes, fo);
    fflush(fo); fsync(fileno(fo)); fclose(fo);
    free(stereo);
    return NSBC_OK;
}

void rec_mgr_stat(const rec_mgr_t *m, rec_stat_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (m != NULL) *out = m->st;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
