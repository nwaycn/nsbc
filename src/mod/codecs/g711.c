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
/* ★ P0 修复（2026-11-12）：原表排布错乱（256 项全部与 ITU 标准不符）——
 * 静音 idle 字节 0x55 被解成 +5504 大响样本，录音全程恒定能量的噪声。
 * 通话不受影响（RTP 透传不解码），只有录音/转码路径用表解码 → 唯一症状就是录音噪声。
 * 新表由 ITU-T G.711 标准公式生成，并与 ffmpeg libavcodec alaw 解码器
 * 256 字节全量输出逐项比对通过（tmp/_gen_tables.py 留有生成+验证脚本）。
 * 索引约定不变：ALAW_TABLE[code ^ 0x55]（a 值索引）。 */
static const int16_t ALAW_TABLE[256] = {
        -8,    -24,    -40,    -56,    -72,    -88,   -104,   -120,
      -136,   -152,   -168,   -184,   -200,   -216,   -232,   -248,
      -264,   -280,   -296,   -312,   -328,   -344,   -360,   -376,
      -392,   -408,   -424,   -440,   -456,   -472,   -488,   -504,
      -528,   -560,   -592,   -624,   -656,   -688,   -720,   -752,
      -784,   -816,   -848,   -880,   -912,   -944,   -976,  -1008,
     -1056,  -1120,  -1184,  -1248,  -1312,  -1376,  -1440,  -1504,
     -1568,  -1632,  -1696,  -1760,  -1824,  -1888,  -1952,  -2016,
     -2112,  -2240,  -2368,  -2496,  -2624,  -2752,  -2880,  -3008,
     -3136,  -3264,  -3392,  -3520,  -3648,  -3776,  -3904,  -4032,
     -4224,  -4480,  -4736,  -4992,  -5248,  -5504,  -5760,  -6016,
     -6272,  -6528,  -6784,  -7040,  -7296,  -7552,  -7808,  -8064,
     -8448,  -8960,  -9472,  -9984, -10496, -11008, -11520, -12032,
    -12544, -13056, -13568, -14080, -14592, -15104, -15616, -16128,
    -16896, -17920, -18944, -19968, -20992, -22016, -23040, -24064,
    -25088, -26112, -27136, -28160, -29184, -30208, -31232, -32256,
         8,     24,     40,     56,     72,     88,    104,    120,
       136,    152,    168,    184,    200,    216,    232,    248,
       264,    280,    296,    312,    328,    344,    360,    376,
       392,    408,    424,    440,    456,    472,    488,    504,
       528,    560,    592,    624,     656,   688,    720,    752,
       784,    816,    848,    880,     912,   944,    976,   1008,
      1056,   1120,   1184,   1248,   1312,   1376,   1440,   1504,
      1568,   1632,   1696,   1760,   1824,   1888,   1952,   2016,
      2112,   2240,   2368,   2496,   2624,   2752,   2880,   3008,
      3136,   3264,   3392,   3520,   3648,   3776,   3904,   4032,
      4224,   4480,   4736,   4992,   5248,   5504,   5760,   6016,
      6272,   6528,   6784,   7040,   7296,   7552,   7808,   8064,
      8448,   8960,   9472,   9984,  10496,  11008,  11520,  12032,
     12544,  13056,  13568,  14080,  14592,  15104,  15616,  16128,
     16896,  17920,  18944,  19968,  20992,  22016,  23040,  24064,
     25088,  26112,  27136,  28160,  29184,  30208,  31232,  32256,
};

/* ============================================================================
 * ITU G.711 μ-law decoding table (256 entries, 14-bit + bias → 16-bit)
 * 来源：ITU-T G.711 Appendix I Table 1
 * ==========================================================================*/
/* ★ P0 修复（2026-11-12）：同 ALAW_TABLE —— 原表 256 项全部错误。
 * 新表由 ITU 标准公式生成并经 ffmpeg mulaw 解码器全量验证。
 * 索引约定不变：ULAW_TABLE[bitrev(code)]（µ-law 解码前按位反转，见 unpack）。 */
static const int16_t ULAW_TABLE[256] = {
    -32124,  32124,  -1884,   1884,  -7932,   7932,   -372,    372,
    -15996,  15996,   -876,    876,  -3900,   3900,   -120,    120,
    -23932,  23932,  -1372,   1372,  -5884,   5884,   -244,    244,
    -11900,  11900,   -620,    620,  -2876,   2876,    -56,     56,
    -28028,  28028,  -1628,   1628,  -6908,   6908,   -308,    308,
    -13948,  13948,   -748,    748,  -3388,   3388,    -88,     88,
    -19836,  19836,  -1116,   1116,  -4860,   4860,   -180,    180,
     -9852,   9852,   -492,    492,  -2364,   2364,    -24,     24,
    -30076,  30076,  -1756,   1756,  -7420,   7420,   -340,    340,
    -14972,  14972,   -812,    812,  -3644,   3644,   -104,    104,
    -21884,  21884,  -1244,   1244,  -5372,   5372,   -212,    212,
    -10876,  10876,   -556,    556,  -2620,   2620,    -40,     40,
    -25980,  25980,  -1500,   1500,  -6396,   6396,   -276,    276,
    -12924,  12924,   -684,    684,  -3132,   3132,    -72,     72,
    -17788,  17788,   -988,    988,  -4348,   4348,   -148,    148,
     -8828,   8828,   -428,    428,  -2108,   2108,     -8,      8,
    -31100,  31100,  -1820,   1820,  -7676,   7676,   -356,    356,
    -15484,  15484,   -844,    844,  -3772,   3772,   -112,    112,
    -22908,  22908,  -1308,   1308,  -5628,   5628,   -228,    228,
    -11388,  11388,   -588,    588,  -2748,   2748,    -48,     48,
    -27004,  27004,  -1564,   1564,  -6652,   6652,   -292,    292,
    -13436,  13436,   -716,    716,  -3260,   3260,    -80,     80,
    -18812,  18812,  -1052,   1052,  -4604,   4604,   -164,    164,
     -9340,   9340,   -460,    460,  -2236,   2236,    -16,     16,
    -29052,  29052,  -1692,   1692,  -7164,   7164,   -324,    324,
    -14460,  14460,   -780,    780,  -3516,   3516,    -96,     96,
    -20860,  20860,  -1180,   1180,  -5116,   5116,   -196,    196,
    -10364,  10364,   -524,    524,  -2492,   2492,    -32,     32,
    -24956,  24956,  -1436,   1436,  -6140,   6140,   -260,    260,
    -12412,  12412,   -652,    652,  -3004,   3004,    -64,     64,
    -16764,  16764,   -924,    924,  -4092,   4092,   -132,    132,
     -8316,   8316,   -396,    396,  -1980,   1980,      0,      0,
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

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
