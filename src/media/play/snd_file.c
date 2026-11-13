/*
 * snd_file.c —— 统一音频文件解码 + 重采样/声道合并
 *
 * P0 backend：原生 RIFF PCM WAV（零依赖）
 *   - 自动读 fmt chunk 的 rate/channels/bits
 *   - channels != 1 → 线性合并（L+R)/2
 *   - rate != 8000 → 线性插值重采样到 8k
 *   - bits != 16 → WARN 拒绝（当前只支持 16bit PCM）
 *   - 流式：按需从 fp 读原始样本 → 合并 → 重采样 → 攒进 8k 缓冲
 *
 * P1 backend：FFmpeg 6.x + swresample（由 -DHAVE_FFMPEG 控制编译）
 *
 * 输出硬锁：8kHz / mono / 16bit signed little-endian
 */

#include "snd_file.h"
#include "log.h"
#include "nsbc_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ---------- snd_src ---------- */

struct snd_src {
    char path[256];
    snd_backend_t backend;

    /* === 原生 PCM WAV backend 字段 === */
    FILE *fp;
    int rate;
    int channels;
    int bits;
    uint32_t data_bytes;
    uint32_t data_offset;      /* data chunk 在文件内的起始 offset */
    uint32_t pos;              /* 已消费的原始样本字节数 */

    /* === 流式重采样缓冲（8k / mono / S16LE 输出） ===
     * 攒够 samples_needed 就 pull 走，E+16KB 常驻 */
    int16_t *rbuf;
    size_t   rbuf_cap;
    size_t   rbuf_used;        /* 已填入 */
    size_t   rbuf_pos;         /* 已 pull 走 */

#ifdef HAVE_FFMPEG
    /* === FFmpeg backend 字段（P1） === */
    void *av_ctx;              /* 不透明，避免这里 include libavcodec 头 */
#endif
};

/* ---------- 小端读 ---------- */

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* ---------- 原生 PCM WAV backend ---------- */

/* 线性合并 N 声道 → mono（取平均）。in/out 不允许 overlap。
 * in 是原始交错 PCM；frames 是输出 mono 样本数 = in_bytes / (channels * bits/8)。 */
static void pcm_mix_to_mono(const uint8_t *in, size_t in_bytes,
                            int channels, int bits, int16_t *out)
{
    size_t frames = in_bytes / (size_t)(channels * bits / 8);
    if (bits == 16) {
        const int16_t *p = (const int16_t *)in;
        for (size_t f = 0; f < frames; f++) {
            int32_t sum = 0;
            for (int ch = 0; ch < channels; ch++)
                sum += (int32_t)p[f * channels + ch];
            out[f] = (int16_t)(sum / channels);
        }
    } else if (bits == 8) {
        /* 8bit PCM 是 unsigned（0-255, 中值 128），先减 128*N 再平均再 <<8
         * —— 避免先平均再左移丢精度（Bugfix B6） */
        const uint8_t *p = in;
        for (size_t f = 0; f < frames; f++) {
            int32_t sum = 0;
            for (int ch = 0; ch < channels; ch++)
                sum += (int32_t)p[f * channels + ch];
            /* sum - 128*N = signed int32 范围内平均后再 <<8 升到 16bit */
            out[f] = (int16_t)(((sum - 128 * channels) / channels) << 8);
        }
    }
}

/* 线性插值重采样：orig → 8k。
 * orig/orig_frames_mono 是声道合并后的 mono 16bit 序列；
 * out 缓冲区容量 dst_cap（int16_t 样本数）——不会写越界。
 * 返回实际写入的样本数。
 * Bugfix B1：用 ceil((orig*8)/orig_rate) 算 out_frames，整除截断会丢末尾时间。
 * Bugfix B8：加 dst_cap 参数，调用方可预先限制输出大小。 */
static size_t pcm_linear_resample(const int16_t *orig, size_t orig_frames_mono,
                                  int orig_rate, int16_t *out, size_t dst_cap)
{
    if (orig_frames_mono == 0 || dst_cap == 0) return 0;
    if (orig_rate == 8000) {
        size_t n = orig_frames_mono < dst_cap ? orig_frames_mono : dst_cap;
        memcpy(out, orig, n * sizeof(int16_t));
        return n;
    }
    double step = (double)orig_rate / 8000.0;
    /* ceil 除法覆盖原始时间范围的所有样本；dst_cap clamp 防溢出 */
    size_t out_frames = (size_t)((double)orig_frames_mono / step + 0.99999);
    if (out_frames > dst_cap) out_frames = dst_cap;
    if (out_frames == 0) return 0;
    for (size_t n = 0; n < out_frames; n++) {
        double idx = (double)n * step;
        size_t i0 = (size_t)idx;
        size_t i1 = i0 + 1 < orig_frames_mono ? i0 + 1 : i0;
        double frac = idx - (double)i0;
        int32_t s = (int32_t)orig[i0] * (1.0 - frac)
                  + (int32_t)orig[i1] * frac;
        out[n] = (int16_t)(s >  32767 ?  32767 :
                           s < -32768 ? -32768 : (int16_t)s);
    }
    return out_frames;
}

/* 开文件 + 读 RIFF fmt 校验 + 定位 data chunk。fmt 非 PCM/16bit 拒绝。 */
static int pcm_wav_open(snd_src_t *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        llog(LOG_WARN, "snd_file", "pcm_wav_open: fopen FAIL %s", path);
        return NSBC_ERR;
    }

    uint8_t hdr[12];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        llog(LOG_WARN, "snd_file", "pcm_wav_open: not RIFF/WAVE %s", path);
        return NSBC_INVAL;
    }

    s->fp = f;
    s->rate = 0; s->channels = 0; s->bits = 0;
    s->data_bytes = 0; s->data_offset = 0; s->pos = 0;

    uint8_t ch[8];
    while (fread(ch, 1, sizeof(ch), f) == sizeof(ch)) {
        uint32_t cid = rd_u32(ch);
        uint32_t csz = rd_u32(ch + 4);
        long cstart = ftell(f);

        if (cid == 0x20746d66) { /* "fmt " */
            uint8_t fmt[16] = {0};
            if (csz < 16 || fread(fmt, 1, 16, f) != 16) break;
            if (rd_u16(fmt) != 1) {          /* 必须 PCM */
                fclose(f); s->fp = NULL;
                llog(LOG_WARN, "snd_file", "pcm_wav_open: not PCM format tag=%d %s",
                     rd_u16(fmt), path);
                return NSBC_NOTSUP;
            }
            s->channels = (int)rd_u16(fmt + 2);
            s->rate     = (int)rd_u32(fmt + 4);
            s->bits     = (int)rd_u16(fmt + 14);
            if (s->rate <= 0) {
                fclose(f); s->fp = NULL;
                llog(LOG_WARN, "snd_file", "pcm_wav_open: bad rate=%d %s", s->rate, path);
                return NSBC_INVAL;
            }
            if (s->bits != 8 && s->bits != 16) {
                fclose(f); s->fp = NULL;
                llog(LOG_WARN, "snd_file",
                    "pcm_wav_open: bits=%d — only 8/16 supported %s",
                    s->bits, path);
                return NSBC_NOTSUP;
            }
            /* ★ BUGFIX B3：fmt chunk csz 可能 >16（extra size 等），
             * 必须对齐到 chunk 末尾才能继续找 data chunk */
            fseek(f, cstart + (long)((csz + 1) & ~1u), SEEK_SET);
        } else if (cid == 0x61746164) { /* "data" */
            if (!s->rate) break;            /* 没 fmt chunk */
            s->data_bytes = csz;
            s->data_offset = (uint32_t)cstart;
            fseek(f, cstart, SEEK_SET);     /* 停在 data chunk 起点 */
            llog(LOG_INFO, "snd_file",
                "pcm_wav_open OK %s rate=%d ch=%d bits=%d data=%u bytes → "
                "out=8k/mono/S16LE (resampling=%s)",
                path, s->rate, s->channels, s->bits, s->data_bytes,
                (s->rate != 8000 || s->channels != 1) ? "ON" : "off");
            return NSBC_OK;
        }
        fseek(f, cstart + (long)((csz + 1) & ~1u), SEEK_SET);
    }

    fclose(f); s->fp = NULL;
    llog(LOG_WARN, "snd_file", "pcm_wav_open: no data chunk %s", path);
    return NSBC_INVAL;
}

/* 从 fp 读一块原始样本 → 声道合并 → 重采样 → append 进 rbuf。
 * 每次调用把 rbuf 补到 rbuf_cap（≈ 16KB）。EOF 时不补，直接返回。
 * Bugfix B4：raw_need 不能超过栈上 raw[16KB] 大小，否则越界写。 */
static void pcm_wav_refill_buf(snd_src_t *s)
{
    if (!s->fp) return;
    const int bytes_per_sample = s->bits / 8;
    const int frame_bytes = bytes_per_sample * s->channels;

    while (s->rbuf_used - s->rbuf_pos < s->rbuf_cap / 2) {
        /* ★ BUGFIX B4：raw 只有 16KB 栈空间，raw_need 必须 clamp */
        size_t raw_need = (size_t)(frame_bytes) * 4096;
        if (raw_need > sizeof(uint8_t) * 4096 * 4)
            raw_need = sizeof(uint8_t) * 4096 * 4;
        if (s->pos + raw_need > s->data_bytes)
            raw_need = s->data_bytes - s->pos;
        if (raw_need == 0) return;                      /* EOF */
        if (raw_need < (size_t)frame_bytes) return;     /* 凑不出一整帧 */

        /* 1. 读原始交错 PCM */
        uint8_t raw[4096 * 4];  /* 16KB 栈缓冲；已 clamp 保证不溢出 */
        size_t got = fread(raw, 1, raw_need, s->fp);
        if (got == 0) return;
        size_t actual_frames = got / (size_t)frame_bytes;
        size_t actual_bytes = actual_frames * frame_bytes;
        s->pos += (uint32_t)actual_bytes;
        /* 尾字节不足一帧的丢弃（交错 PCM 必须整帧处理） */

        /* 2. 声道合并 → mono int16 工作区 */
        int16_t mono[4096 * 4];
        if (s->channels == 1 && s->bits == 16) {
            memcpy(mono, raw, actual_bytes);
        } else {
            pcm_mix_to_mono(raw, actual_bytes, s->channels, s->bits, mono);
        }

        /* 先腾出 rbuf 空间：把已 pull 走的部分 remove */
        if (s->rbuf_pos > 0) {
            memmove(s->rbuf, s->rbuf + s->rbuf_pos,
                    (s->rbuf_used - s->rbuf_pos) * sizeof(int16_t));
            s->rbuf_used -= s->rbuf_pos;
            s->rbuf_pos = 0;
        }
        if (s->rbuf_used >= s->rbuf_cap) break;

        /* 3. 重采样到 8k（dst_cap 精确限制输出不越界） */
        int16_t *dst = s->rbuf + s->rbuf_used;
        size_t dst_cap = s->rbuf_cap - s->rbuf_used;
        size_t added = pcm_linear_resample(mono, actual_frames, s->rate,
                                           dst, dst_cap);
        s->rbuf_used += added;

        if (s->pos >= s->data_bytes) return;  /* 一轮读完 */
    }
}

/* ---------- FFmpeg backend stub（P1，条件编译） ---------- */
/* 真正实现需要 include libavcodec/libswresample，这里先挂空槽位。
 * configure -DHAVE_FFMPEG 到时用 avcodec_alloc_context3 + swr_convert
 * 替换 pcm_wav_refill_buf 的读/重采样段，snd_file_pull 接口不变。 */

/* ---------- 公开 API ---------- */

int snd_file_open(const char *path, snd_src_t **out,
                  int force_backend, int *out_backend)
{
    if (!path || !out) return NSBC_INVAL;

    snd_src_t *s = calloc(1, sizeof(*s));
    if (!s) return NSBC_NOMEM;

    snprintf(s->path, sizeof(s->path), "%s", path);
    s->rbuf_cap = 16384;  /* 16KB → ≈ 8K 样本 → 1.024 秒 @ 8k */
    s->rbuf = malloc(s->rbuf_cap * sizeof(int16_t));
    if (!s->rbuf) { free(s); return NSBC_NOMEM; }
    s->rbuf_used = s->rbuf_pos = 0;

    int rc = NSBC_ERR;
    int be = SND_BACKEND_NONE;

    /* 自动 fallback：PCM_WAV → FFmpeg → ... */
    if (force_backend == 0 || force_backend == SND_BACKEND_PCM_WAV) {
        if ((rc = pcm_wav_open(s, path)) == NSBC_OK) {
            be = SND_BACKEND_PCM_WAV;
            goto done;
        }
    }
#ifdef HAVE_FFMPEG
    if ((force_backend == 0 || force_backend == SND_BACKEND_FFMPEG) && be == SND_BACKEND_NONE) {
        /* av_open(s) — 等 P1 实现 */
    }
#endif

done:
    if (be == SND_BACKEND_NONE) {
        free(s->rbuf);
        free(s);
        llog(LOG_WARN, "snd_file", "open FAIL all backends exhausted %s", path);
        return NSBC_NOTSUP;
    }
    s->backend = (snd_backend_t)be;
    if (out_backend) *out_backend = be;
    *out = s;
    return NSBC_OK;
}

int snd_file_pull(snd_src_t *s, int16_t *out, int frames, int loop)
{
    if (!s || !out || frames <= 0) return 0;
    int got = 0;
    while (got < frames) {
        /* 1. 从当前缓冲 pull */
        size_t avail = s->rbuf_used - s->rbuf_pos;
        if (avail > 0) {
            size_t take = avail < (size_t)(frames - got) ? avail : (size_t)(frames - got);
            memcpy(out + got, s->rbuf + s->rbuf_pos, take * sizeof(int16_t));
            s->rbuf_pos += take;
            got += (int)take;
            continue;
        }
        /* 2. 缓冲空：尝试 refill */
        pcm_wav_refill_buf(s);
        if (s->rbuf_pos < s->rbuf_used) continue;  /* refill 成功 */
        /* 3. EOF：loop 回绕 */
        if (!loop) break;
        if (s->fp && fseek(s->fp, s->data_offset, SEEK_SET) == 0) {
            s->pos = 0;
            s->rbuf_used = s->rbuf_pos = 0;
        } else {
            break;
        }
    }
    return got;
}

void snd_file_close(snd_src_t *s)
{
    if (!s) return;
    if (s->fp) fclose(s->fp);
    free(s->rbuf);
    free(s);
}

/* ---------- 诊断 ---------- */

int  snd_file_rate(snd_src_t *s)     { return s ? s->rate : 0; }
int  snd_file_channels(snd_src_t *s) { return s ? s->channels : 0; }
int  snd_file_backend(snd_src_t *s)  { return s ? (int)s->backend : 0; }
const char *snd_file_path(snd_src_t *s) { return s ? s->path : ""; }
