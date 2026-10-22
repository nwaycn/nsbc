#include "mixer.h"

#define VAD_DEFAULT_PER_SAMPLE 200

void mixer_init(mixer_t *m, int count)
{
    if (!m || count <= 0 || count > MIX_MAX_MEMBERS)
        return;
    memset(m, 0, sizeof(*m));
    m->count = count;
    m->vad_per_sample = VAD_DEFAULT_PER_SAMPLE;
    m->hangover = MIX_HANGOVER_FRAMES;
    m->last_speaker = -1;
    for (int i = 0; i < count; ++i)
        m->members[i].present = 1;
}

static int16_t sat_s16(int32_t v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

/* 帧绝对能量和，返回 0 表示静音帧 */
static uint64_t frame_energy(const int16_t *p, int frames)
{
    if (!p)
        return 0;
    uint64_t e = 0;
    for (int i = 0; i < frames; ++i)
        e += (uint64_t)(p[i] < 0 ? -p[i] : p[i]);
    return e;
}

int mixer_process(mixer_t *m, const int16_t *const *in, int16_t **out,
                  int frames)
{
    if (!m || !in || !out || frames <= 0)
        return -1;

    int n = m->count;
    int active[MIX_MAX_MEMBERS];
    int active_cnt = 0;
    uint64_t energies[MIX_MAX_MEMBERS];

    /* 1. 逐成员 VAD：能量门限 + 在会/静音状态 */
    for (int i = 0; i < n; ++i) {
        energies[i] = 0;
        active[i] = 0;
        if (!m->members[i].present || m->members[i].muted || !in[i])
            continue;
        energies[i] = frame_energy(in[i], frames);
        if (energies[i] >= (uint64_t)m->vad_per_sample * (uint64_t)frames) {
            active[i] = 1;
            active_cnt++;
        }
    }

    /* 2. 活跃说话人：能量最大者 + hangover 保持 */
    int speaker = -1;
    uint64_t best = 0;
    for (int i = 0; i < n; ++i)
        if (active[i] && energies[i] > best) {
            best = energies[i];
            speaker = i;
        }

    if (speaker >= 0) {
        m->last_speaker = speaker;
        m->speaker_hold = m->hangover;
    } else if (m->speaker_hold > 0) {
        m->speaker_hold--;
        speaker = m->last_speaker;
    } else {
        m->last_speaker = -1;
    }

    /* 3. 每路输出 = 其余活跃成员之和按活跃数归一化（能量归一化 + 削波保护） */
    int divisor = active_cnt > 0 ? active_cnt : 1;
    for (int j = 0; j < n; ++j) {
        if (!m->members[j].present) {
            memset(out[j], 0, frames * sizeof(int16_t));
            continue;
        }
        for (int k = 0; k < frames; ++k) {
            int32_t sum = 0;
            for (int i = 0; i < n; ++i)
                if (i != j && active[i])
                    sum += in[i][k];
            out[j][k] = sat_s16(sum / divisor);
        }
    }

    return speaker;
}
