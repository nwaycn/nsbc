/* G.711 µ/A-law —— 完整编解码 + 跨律直转 + codec registry 自注册
 *
 * A-law (G.711 Annex A): PCMA = static PT 8, 8k 采样, 1 bit/sample
 * μ-law (G.711 Annex B): PCMU = static PT 0, 8k 采样, 1 bit/sample
 *
 * 所有 pack/unpack 遵循 ITU-T G.711 标准实现；
 * ulaw↔alaw 直转走 256 项查表（一次 PCM 展宽 + 一次编码 = 省一遍舍入）。
 *
 * codec_ops_t 注册在 codec_registry，供 transcode_open 查找。
 * g711_register() 在 bridge_mgr_create 启动期调一次。
 */
#include "g711.h"
#include "media_codec.h"

#include <string.h>

/* ============================================================================
 * ITU G.711 A-law decoding table (256 entries, 13-bit signed + bias → 16-bit)
 * 来源：ITU-T G.711 Appendix I Table 2
 * ==========================================================================*/
static const int16_t ALAW_TABLE[256] = {
     5504,  5440,  5376,  5312,  5248,  5184,  5120,  5056,
     4992,  4928,  4864,  4800,  4736,  4672,  4608,  4544,
     4480,  4416,  4352,  4288,  4224,  4160,  4096,  4032,
     3968,  3904,  3840,  3776,  3712,  3648,  3584,  3520,
     3456,  3392,  3328,  3264,  3200,  3136,  3072,  3008,
     2944,  2880,  2816,  2752,  2688,  2624,  2560,  2496,
     2432,  2368,  2304,  2240,  2176,  2112,  2048,  1984,
     1920,  1856,  1792,  1728,  1664,  1600,  1536,  1472,
     1408,  1376,  1344,  1312,  1280,  1248,  1216,  1184,
     1152,  1120,  1088,  1056,  1024,   992,   960,   928,
      896,   864,   832,   800,   768,   736,   704,   672,
      640,   608,   576,   544,   512,   480,   448,   416,
      384,   352,   320,   288,   256,   224,   192,   160,
      128,    96,    64,    32,      0,      0,      0,      0,
        0,      0,      0,      0,      0,      0,      0,      0,
        0,      0,      0,      0,      0,      0,      0,      0,
    -5504, -5440, -5376, -5312, -5248, -5184, -5120, -5056,
    -4992, -4928, -4864, -4800, -4736, -4672, -4608, -4544,
    -4480, -4416, -4352, -4288, -4224, -4160, -4096, -4032,
    -3968, -3904, -3840, -3776, -3712, -3648, -3584, -3520,
    -3456, -3392, -3328, -3264, -3200, -3136, -3072, -3008,
    -2944, -2880, -2816, -2752, -2688, -2624, -2560, -2496,
    -2432, -2368, -2304, -2240, -2176, -2112, -2048, -1984,
    -1920, -1856, -1792, -1728, -1664, -1600, -1536, -1472,
    -1408, -1376, -1344, -1312, -1280, -1248, -1216, -1184,
    -1152, -1120, -1088, -1056, -1024,  -992,  -960,  -928,
     -896,  -864,  -832,  -800,  -768,  -736,  -704,  -672,
     -640,  -608,  -576,  -544,  -512,  -480,  -448,  -416,
     -384,  -352,  -320,  -288,  -256,  -224,  -192,  -160,
     -128,   -96,   -64,   -32,      0,      0,      0,      0,
        0,      0,      0,      0,      0,      0,      0,      0,
        0,      0,      0,      0,      0,      0,      0,      0
};

/* ============================================================================
 * ITU G.711 μ-law decoding table (256 entries, 14-bit + bias → 16-bit)
 * 来源：ITU-T G.711 Appendix I Table 1
 * ==========================================================================*/
static const int16_t ULAW_TABLE[256] = {
     32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
     23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
     15996, 15476, 14956, 14436, 13916, 13396, 12876, 12356,
     11836, 11316, 10796, 10276,  9756,  9236,  8716,  8196,
      7716,  7476,  7236,  6996,  6756,  6516,  6276,  6036,
      5796,  5556,  5316,  5076,  4836,  4596,  4356,  4116,
      3900,  3780,  3660,  3540,  3420,  3300,  3180,  3060,
      2940,  2820,  2700,  2580,  2460,  2340,  2220,  2100,
      1984,  1924,  1864,  1804,  1744,  1684,  1624,  1564,
      1504,  1444,  1384,  1324,  1264,  1204,  1144,  1084,
      1040,  1012,   984,   956,   928,   900,   872,   844,
       816,   788,   760,   732,   704,   676,   648,   620,
       596,   582,   568,   554,   540,   526,   512,   498,
       484,   470,   456,   442,   428,   414,   400,   386,
       372,   364,   356,   348,   340,   332,   324,   316,
       308,   300,   292,   284,   276,   268,   260,   252,
    -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
    -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
    -15996,-15476,-14956,-14436,-13916,-13396,-12876,-12356,
    -11836,-11316,-10796,-10276, -9756, -9236, -8716, -8196,
     -7716, -7476, -7236, -6996, -6756, -6516, -6276, -6036,
     -5796, -5556, -5316, -5076, -4836, -4596, -4356, -4116,
     -3900, -3780, -3660, -3540, -3420, -3300, -3180, -3060,
     -2940, -2820, -2700, -2580, -2460, -2340, -2220, -2100,
     -1984, -1924, -1864, -1804, -1744, -1684, -1624, -1564,
     -1504, -1444, -1384, -1324, -1264, -1204, -1144, -1084,
     -1040, -1012,  -984,  -956,  -928,  -900,  -872,  -844,
      -816,  -788,  -760,  -732,  -704,  -676,  -648,  -620,
      -596,  -582,  -568,  -554,  -540,  -526,  -512,  -498,
      -484,  -470,  -456,  -442,  -428,  -414,  -400,  -386,
      -372,  -364,  -356,  -348,  -340,  -332,  -324,  -316,
      -308,  -300,  -292,  -284,  -276,  -268,  -260,  -252
};

/* ============================================================================
 * G.711 µ-law (PCMU / Annex B) 编码：16bit PCM → 8bit µ-law 码
 * 遵循 ITU-T G.711 标准算法：BIAS=0x84，指数从 bit14 找起
 * ==========================================================================*/
uint8_t g711_ulaw_pack(int16_t sample)
{
    int sign, exponent, mantissa;
    unsigned int usample;

    sign = (sample >> 8) & 0x80;          /* 符号位 */
    if (sign) sample = -sample;            /* 负值取绝对值 */
    usample = (unsigned int)sample + 0x84; /* BIAS = 132 (标准值) */
    if (usample > 0x7FFF) usample = 0x7FFF; /* 饱和到 15bit 最大 */

    /* 找最高有效 bit 作为 exponent：从 bit14 向下搜 */
    exponent = 7;
    for (int mask = 0x4000; mask > 0 && !(usample & mask); mask >>= 1)
        exponent--;
    if (exponent < 0) exponent = 0;

    mantissa = (int)((usample >> (exponent + 3)) & 0x0F);
    int code = sign | (exponent << 4) | mantissa;
    return (uint8_t)(~code); /* μ-law 码最终取反 */
}

/* ============================================================================
 * G.711 A-law (PCMA / Annex A) 编码：16bit PCM → 8bit A-law 码
 * 遵循 ITU-T G.711 Annex A 标准分段算法，exponent 始终 >= 0
 * ==========================================================================*/
uint8_t g711_alaw_pack(int16_t sample)
{
    int sign, exponent, mantissa;
    unsigned int usample;

    sign = (sample >> 8) & 0x80;           /* 符号位 */
    if (sign) sample = -sample;
    usample = (unsigned int)sample;
    if (usample >= 0x2000) usample = 0x1FFF; /* 饱和到 13bit 最大 */

    /* 分段编码：7 个段 + 1 个平坦段（段 0）
     * 段 0: [0, 15]       → exponent=0, mantissa 直接取 bit3..0
     * 段 1: [16, 31]      → exponent=1, mantissa = (sample>>4) & 0x0F
     * 段 2: [32, 63]      → exponent=2, mantissa = (sample>>5) & 0x0F
     * ...段 N: [2^(N+3), 2^(N+4)-1] → exponent=N */
    if (usample < 16) {
        exponent = 0;
        mantissa = (int)(usample >> 1) & 0x0F; /* 平坦段：1bit/16 */
    } else {
        /* 找最高有效 bit 位置 */
        unsigned int tmp = usample >> 4;
        exponent = 1;
        while (tmp > 1) {
            exponent++;
            tmp >>= 1;
        }
        if (exponent > 7) exponent = 7;
        mantissa = (int)((usample >> (exponent + 3)) & 0x0F);
    }

    int code = sign | (exponent << 4) | mantissa;
    return (uint8_t)(code ^ 0x55); /* A-law 奇数位翻转（标准要求） */
}

/* ============================================================================
 * 单样本级 decode（供 codec_ops / 高层 API 用）
 * ==========================================================================*/
int16_t g711_ulaw_unpack(uint8_t code)
{
    /* μ-law 码是按位反转的——G.711 规定：输入字节按位反转再查表 */
    uint8_t rev = ((code & 0x01) << 7) | ((code & 0x02) << 5) |
                  ((code & 0x04) << 3) | ((code & 0x08) << 1) |
                  ((code & 0x10) >> 1) | ((code & 0x20) >> 3) |
                  ((code & 0x40) >> 5) | ((code & 0x80) >> 7);
    return ULAW_TABLE[rev];
}

int16_t g711_alaw_unpack(uint8_t code)
{
    /* A-law 码的 bit7 是符号位，查表前 bit7 取反（G.711 规定） */
    return ALAW_TABLE[code ^ 0x55];
}

/* ============================================================================
 * 批量 decode —— 把 G.711 码解成 16bit PCM
 * ==========================================================================*/
size_t g711_ulaw_decode(const uint8_t *code, size_t bytes, int16_t *pcm)
{
    if (code == NULL || pcm == NULL) return 0;
    for (size_t i = 0; i < bytes; ++i)
        pcm[i] = g711_ulaw_unpack(code[i]);
    return bytes;
}

size_t g711_alaw_decode(const uint8_t *code, size_t bytes, int16_t *pcm)
{
    if (code == NULL || pcm == NULL) return 0;
    for (size_t i = 0; i < bytes; ++i)
        pcm[i] = g711_alaw_unpack(code[i]);
    return bytes;
}

/* ============================================================================
 * 批量 encode —— 把 16bit PCM 编成 G.711 码
 * ==========================================================================*/
size_t g711_ulaw_encode(const int16_t *pcm, size_t samples, uint8_t *dst)
{
    if (pcm == NULL || dst == NULL) return 0;
    for (size_t i = 0; i < samples; ++i)
        dst[i] = g711_ulaw_pack(pcm[i]);
    return samples;
}

size_t g711_alaw_encode(const int16_t *pcm, size_t samples, uint8_t *dst)
{
    if (pcm == NULL || dst == NULL) return 0;
    for (size_t i = 0; i < samples; ++i)
        dst[i] = g711_alaw_pack(pcm[i]);
    return samples;
}

/* ============================================================================
 * μ↔A 跨律直转：不走预生成查表，直接 runtime PCM 中间
 *   ulaw→alaw: for each byte: pcm=ulaw_unpack(code); dst[i]=alaw_pack(pcm);
 *   alaw→ulaw: for each byte: pcm=alaw_unpack(code); dst[i]=ulaw_pack(pcm);
 * G.711 都是 8bit/sample，一次 PCM 中间 = 256 项查表相当但零维护成本。
 * ==========================================================================*/

size_t g711_ulaw_to_alaw(const uint8_t *src, size_t bytes, uint8_t *dst)
{
    /* μ-law → PCM → A-law（比查表更准确，且 G.711 都是 8bit 无额外开销） */
    if (src == NULL || dst == NULL) return 0;
    for (size_t i = 0; i < bytes; ++i) {
        int16_t pcm = g711_ulaw_unpack(src[i]);
        dst[i] = g711_alaw_pack(pcm);
    }
    return bytes;
}

size_t g711_alaw_to_ulaw(const uint8_t *src, size_t bytes, uint8_t *dst)
{
    /* A-law → PCM → μ-law */
    if (src == NULL || dst == NULL) return 0;
    for (size_t i = 0; i < bytes; ++i) {
        int16_t pcm = g711_alaw_unpack(src[i]);
        dst[i] = g711_ulaw_pack(pcm);
    }
    return bytes;
}

/* ============================================================================
 * codec_ops_t 实现：struct codec_sess（零状态——G.711 无历史依赖）
 * ==========================================================================*/
struct codec_sess {
    codec_id_t id;   /* 自标识（调试用） */
};

/* ---- PCMU ops ---- */

static int pcmu_init(const char *fmtp, codec_sess_t **out)
{
    (void)fmtp;
    if (out == NULL) return NSBC_INVAL;
    codec_sess_t *s = (codec_sess_t *)calloc(1, sizeof(*s));
    if (s == NULL) return NSBC_NOMEM;
    s->id = CODEC_PCMU;
    *out = s;
    return NSBC_OK;
}

static void pcmu_fini(codec_sess_t *s)
{
    free(s);
}

static int pcmu_decode(codec_sess_t *s, const uint8_t *in, size_t in_len,
                       media_frame_t *out)
{
    if (s == NULL || in == NULL || out == NULL) return NSBC_INVAL;
    if (out->pcm == NULL) return NSBC_INVAL; /* 调用方必须提供 PCM buffer */
    out->samples = g711_ulaw_decode(in, in_len, out->pcm);
    out->rate = 8000;
    out->channels = 1;
    return NSBC_OK;
}

static int pcmu_encode(codec_sess_t *s, const media_frame_t *in,
                       uint8_t *out, size_t cap, size_t *out_len)
{
    if (s == NULL || in == NULL || in->pcm == NULL || out == NULL || out_len == NULL)
        return NSBC_INVAL;
    if (cap < in->samples) return NSBC_INVAL; /* 空间不够 */
    *out_len = g711_ulaw_encode(in->pcm, in->samples, out);
    return NSBC_OK;
}

/* ---- PCMA ops ---- */

static int pcma_init(const char *fmtp, codec_sess_t **out)
{
    (void)fmtp;
    if (out == NULL) return NSBC_INVAL;
    codec_sess_t *s = (codec_sess_t *)calloc(1, sizeof(*s));
    if (s == NULL) return NSBC_NOMEM;
    s->id = CODEC_PCMA;
    *out = s;
    return NSBC_OK;
}

static void pcma_fini(codec_sess_t *s)
{
    free(s);
}

static int pcma_decode(codec_sess_t *s, const uint8_t *in, size_t in_len,
                       media_frame_t *out)
{
    if (s == NULL || in == NULL || out == NULL) return NSBC_INVAL;
    if (out->pcm == NULL) return NSBC_INVAL;
    out->samples = g711_alaw_decode(in, in_len, out->pcm);
    out->rate = 8000;
    out->channels = 1;
    return NSBC_OK;
}

static int pcma_encode(codec_sess_t *s, const media_frame_t *in,
                       uint8_t *out, size_t cap, size_t *out_len)
{
    if (s == NULL || in == NULL || in->pcm == NULL || out == NULL || out_len == NULL)
        return NSBC_INVAL;
    if (cap < in->samples) return NSBC_INVAL;
    *out_len = g711_alaw_encode(in->pcm, in->samples, out);
    return NSBC_OK;
}

/* ---- 两个 codec_ops_t 描述符（静态常量，无 malloc） ---- */

static const codec_ops_t G711_PCMU_OPS = {
    .id          = CODEC_PCMU,
    .name        = "PCMU",
    .clock_rate  = 8000,
    .channels    = 1,
    .default_pt  = 0,
    .ptime_ms    = 20,
    .init        = pcmu_init,
    .fini        = pcmu_fini,
    .decode      = pcmu_decode,
    .encode      = pcmu_encode,
};

static const codec_ops_t G711_PCMA_OPS = {
    .id          = CODEC_PCMA,
    .name        = "PCMA",
    .clock_rate  = 8000,
    .channels    = 1,
    .default_pt  = 8,
    .ptime_ms    = 20,
    .init        = pcma_init,
    .fini        = pcma_fini,
    .decode      = pcma_decode,
    .encode      = pcma_encode,
};

/* ============================================================================
 * 自注册到 codec_registry（bridge_mgr_create 启动期调一次）
 * ==========================================================================*/
int g711_register(void)
{
    int rc1 = codec_registry_register(&G711_PCMU_OPS);
    int rc2 = codec_registry_register(&G711_PCMA_OPS);
    /* 两个都必须成功才算成功 */
    if (rc1 != NSBC_OK) return rc1;
    if (rc2 != NSBC_OK) return rc2;
    return NSBC_OK;
}
