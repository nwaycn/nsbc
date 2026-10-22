#ifndef NSBC_MIXER_H
#define NSBC_MIXER_H

#include "nsbc_common.h"

/* 对应 REQ-MEDIA-MIX-001~003 */
#define MIX_MAX_MEMBERS 32
#define MIX_HANGOVER_FRAMES 5

typedef struct {
    int present; /* 是否在会 */
    int muted;   /* 成员静音 */
} mix_member_t;

typedef struct mixer {
    mix_member_t members[MIX_MAX_MEMBERS];
    int count;
    int vad_per_sample; /* 单样本平均绝对能量门限 */
    int hangover;       /* 说话人 hangover 帧数 */

    int last_speaker;   /* 当前活跃说话人 */
    int speaker_hold;   /* 剩余保持帧 */
} mixer_t;

void mixer_init(mixer_t *m, int count);

/* 处理一帧：
 * in[count]   各路输入，NULL 表示该帧无声；缓冲由外部媒体池管理
 * out[count]  各路独立输出（去除自身声音），缓冲由调用方提供
 * frames      本帧样本数
 * 返回活跃说话人下标，无人说话返回 -1 */
int mixer_process(mixer_t *m, const int16_t *const *in, int16_t **out,
                  int frames);

#endif /* NSBC_MIXER_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
