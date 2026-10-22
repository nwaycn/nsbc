#ifndef NSBC_PLAYER_H
#define NSBC_PLAYER_H

#include "nsbc_common.h"

/* 对应 REQ-MEDIA-PLAY-001/002 */
#define PLAYER_FRAME_MS 10

typedef struct pcm_src {
    char name[64];
    int16_t *samples;
    size_t n;            /* 样本数 */
    int rate;            /* 采样率 */
    int owns_samples;
    int refcnt;          /* 引用计数，原子操作 */
    struct pcm_src *next;
} pcm_src_t;

typedef struct {
    pcm_src_t *head;
} player_reg_t;

typedef struct {
    pcm_src_t *src;
    size_t pos;
    int loop;            /* MoH/排队音乐循环播放 */
    int eof;
} player_t;

int player_reg_init(player_reg_t *reg);
void player_reg_fini(player_reg_t *reg);

/* 载入标准 PCM S16LE 单声道 WAV（自动遍历 chunk，不限定 44 字节头） */
int player_load_wav(player_reg_t *reg, const char *path, const char *name);

/* 注册内存音源（PoC 自检使用；copy=1 时内部拷贝） */
int player_register_pcm(player_reg_t *reg, const char *name,
                        const int16_t *samples, size_t n, int rate, int copy);

pcm_src_t *player_get(player_reg_t *reg, const char *name);
void player_put(pcm_src_t *src);

int player_open(player_t *pl, pcm_src_t *src, int loop);
/* 拉取 frames 个样本，返回实际写入数；EOF 返回 0 */
int player_pull(player_t *pl, int16_t *out, int frames);

#endif /* NSBC_PLAYER_H */
