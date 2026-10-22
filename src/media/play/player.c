#include "player.h"

#include <ctype.h>

int player_reg_init(player_reg_t *reg)
{
    if (!reg)
        return NSBC_INVAL;
    reg->head = NULL;
    return NSBC_OK;
}

void player_reg_fini(player_reg_t *reg)
{
    if (!reg)
        return;
    pcm_src_t *s = reg->head;
    while (s) {
        pcm_src_t *next = s->next;
        if (s->refcnt != 0)
            NSBC_LOGE("source %s freed with refcnt=%d\n", s->name, s->refcnt);
        if (s->owns_samples)
            free(s->samples);
        free(s);
        s = next;
    }
    reg->head = NULL;
}

static pcm_src_t *reg_find(player_reg_t *reg, const char *name)
{
    for (pcm_src_t *s = reg->head; s; s = s->next)
        if (strcmp(s->name, name) == 0)
            return s;
    return NULL;
}

static int reg_add_src(player_reg_t *reg, const char *name, int16_t *samples,
                       size_t n, int rate, int owns)
{
    pcm_src_t *s = calloc(1, sizeof(*s));
    if (!s) {
        if (owns)
            free(samples);
        return NSBC_NOMEM;
    }
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->samples = samples;
    s->n = n;
    s->rate = rate;
    s->owns_samples = owns;
    s->refcnt = 0;
    s->next = reg->head;
    reg->head = s;
    return NSBC_OK;
}

int player_register_pcm(player_reg_t *reg, const char *name,
                        const int16_t *samples, size_t n, int rate, int copy)
{
    if (!reg || !name || !samples || n == 0)
        return NSBC_INVAL;
    if (reg_find(reg, name))
        return NSBC_INVAL;

    int16_t *buf;
    int owns = 0;
    if (copy) {
        buf = malloc(n * sizeof(int16_t));
        if (!buf)
            return NSBC_NOMEM;
        memcpy(buf, samples, n * sizeof(int16_t));
        owns = 1;
    } else {
        buf = (int16_t *)samples;
    }
    return reg_add_src(reg, name, buf, n, rate, owns);
}

/* 小端读取 */
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

int player_load_wav(player_reg_t *reg, const char *path, const char *name)
{
    if (!reg || !path || !name)
        return NSBC_INVAL;
    if (reg_find(reg, name))
        return NSBC_INVAL;

    FILE *f = fopen(path, "rb");
    if (!f)
        return NSBC_ERR;

    uint8_t hdr[12];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return NSBC_INVAL;
    }

    int fmt_ok = 0, rate = 8000, channels = 1, bits = 16;
    int16_t *data = NULL;
    size_t data_bytes = 0;

    uint8_t ch[8];
    while (fread(ch, 1, sizeof(ch), f) == sizeof(ch)) {
        uint32_t cid = rd_u32(ch);
        uint32_t csz = rd_u32(ch + 4);
        long cstart = ftell(f);

        if (cid == 0x20746d66) { /* "fmt " */
            uint8_t fmt[16] = {0};
            if (csz < 16 || fread(fmt, 1, 16, f) != 16)
                break;
            if (rd_u16(fmt) != 1) /* PCM */
                break;
            channels = rd_u16(fmt + 2);
            rate = (int)rd_u32(fmt + 4);
            bits = rd_u16(fmt + 14);
            fmt_ok = (channels == 1 && bits == 16);
        } else if (cid == 0x61746164) { /* "data" */
            if (!fmt_ok)
                break;
            data_bytes = csz;
            data = malloc(data_bytes);
            if (!data)
                break;
            if (fread(data, 1, data_bytes, f) != data_bytes) {
                free(data);
                data = NULL;
            }
            break;
        }
        fseek(f, cstart + (long)((csz + 1) & ~1u), SEEK_SET);
    }
    fclose(f);

    if (!data)
        return fmt_ok ? NSBC_NOMEM : NSBC_INVAL;

    return reg_add_src(reg, name, data, data_bytes / sizeof(int16_t), rate, 1);
}

pcm_src_t *player_get(player_reg_t *reg, const char *name)
{
    pcm_src_t *s = reg_find(reg, name);
    if (s)
        __atomic_add_fetch(&s->refcnt, 1, __ATOMIC_ACQ_REL);
    return s;
}

void player_put(pcm_src_t *src)
{
    if (!src)
        return;
    int n = __atomic_sub_fetch(&src->refcnt, 1, __ATOMIC_ACQ_REL);
    if (n < 0)
        NSBC_LOGE("source %s negative refcnt\n", src->name);
}

int player_open(player_t *pl, pcm_src_t *src, int loop)
{
    if (!pl || !src)
        return NSBC_INVAL;
    pl->src = src;
    pl->pos = 0;
    pl->loop = loop;
    pl->eof = 0;
    return NSBC_OK;
}

int player_pull(player_t *pl, int16_t *out, int frames)
{
    if (!pl || !out || frames <= 0 || !pl->src)
        return NSBC_INVAL;
    if (pl->eof)
        return 0;

    int w = 0;
    while (w < frames) {
        size_t avail = pl->src->n - pl->pos;
        if (avail == 0) {
            if (pl->loop) {
                pl->pos = 0;
                continue;
            }
            pl->eof = 1;
            break;
        }
        size_t take = (size_t)(frames - w) < avail ? (size_t)(frames - w)
                                                   : avail;
        memcpy(out + w, pl->src->samples + pl->pos, take * sizeof(int16_t));
        pl->pos += take;
        w += (int)take;
    }
    return w;
}
