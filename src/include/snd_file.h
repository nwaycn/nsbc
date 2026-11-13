/* snd_file.h —— 统一音频文件解码 + 重采样/声道合并入口
 *
 * 目标：让 ringback/playback app 能吃任意格式音频文件（WAV/MP3/MP4/FLAC/Opus/…），
 * 统一输出 8kHz / mono / S16LE PCM 喂给 G.711/G.729/Opus 协商编码链。
 *
 * 后端探测（从快到慢，自动 fallback）：
 *   1. 原生 PCM WAV（零依赖，文件头 RIFF+fmt+PCM+mono+8k+16bit 直接吃）
 *      —— fmt chunk 不是 8k/mono/16bit → 内置线性重采样 + 声道合并
 *   2. FFmpeg 6.x + libswresample（可选，configure -DHAVE_FFMPEG=1）
 *      —— avcodec_open2 + av_read_frame + swr_convert 统一重采样到 8k/mono
 *   3. libsndfile + libmpg123（可选，configure 探测后自动启用）
 *      —— P2，当前 stub
 *
 * 约束：
 *   - 每个 snd_src_t 自己持有 decoder ctx，线程安全（pb_timer_cb 在 reactor
 *     线程零锁执行，每个 pb_ctx → 每个 snd_src_t 独立）
 *   - 流式 API：每次 snd_file_pull 只解码/重采样"按需样本数"，整个 10MB 文件
 *     常驻内存 ≈ 16KB 重采样缓冲区，不预加载全文件
 *   - 输出硬锁：8000Hz / mono / 16bit signed little-endian
 *
 * REQ-DP-APP-001 / REQ-MEDIA-PLAY-003
 */
#ifndef NSBC_SND_FILE_H
#define NSBC_SND_FILE_H

#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SND_BACKEND_NONE = 0,
    SND_BACKEND_PCM_WAV,   /* 内置 RIFF PCM WAV + 重采样/合并 */
    SND_BACKEND_FFMPEG,    /* FFmpeg 6.x + swresample */
    SND_BACKEND_SNDFILE,   /* libsndfile — P2 */
    SND_BACKEND_MPG123,    /* libmpg123 — P2 */
} snd_backend_t;

typedef struct snd_src snd_src_t;

/* 打开任意音频文件 → 返回 snd_src。文件格式由后端自动探测。
 *   force_backend — 0=自动 fallback；或指定 SND_BACKEND_*
 *   out_backend   — 可选，返回实际选用的后端（可 NULL）
 * 返回 NSBC_OK / NSBC_INVAL / NSBC_NOTSUP（所有 backend 都不可用时）
 *               / NSBC_ERR（文件打不开 / 格式不识别 / 解码失败） */
int snd_file_open(const char *path, snd_src_t **out,
                  int force_backend, int *out_backend);

/* 流式拉取 8k/mono/S16LE 样本
 *   out  — 调用方提供的 int16_t 缓冲区，>= frames * sizeof(int16_t)
 *   frames — 目标样本数（pb 引擎固定 80 = 10ms @ 8k）
 *   loop — 0=EOF 返回 0；1=EOF 自动回绕（保持音乐/回铃循环）
 * 返回实际写入的样本数；0 = EOF 且 loop=0 */
int snd_file_pull(snd_src_t *s, int16_t *out, int frames, int loop);

/* 关闭 + 释放所有资源（decoder ctx + 重采样缓冲 + 原始文件句柄） */
void snd_file_close(snd_src_t *s);

/* 诊断辅助：打开后查询实际 src 信息 */
int  snd_file_rate(snd_src_t *s);     /* 原始采样率（解码后统一 8000） */
int  snd_file_channels(snd_src_t *s); /* 原始声道数（合并后统一 1） */
int  snd_file_backend(snd_src_t *s);
const char *snd_file_path(snd_src_t *s);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_SND_FILE_H */
