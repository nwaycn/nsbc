#include "md5.h"
#include <stdarg.h>
#include <string.h>

/* 简洁版 RFC1321 MD5 实现（标准算法，无第三方依赖，GCC 4.8 可编） */

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))
#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define FF(a,b,c,d,x,s,ac) (a) += F((b),(c),(d)) + (x) + (uint32_t)(ac); \
    (a) = ROTL((a),(s)); (a) += (b)
#define GG(a,b,c,d,x,s,ac) (a) += G((b),(c),(d)) + (x) + (uint32_t)(ac); \
    (a) = ROTL((a),(s)); (a) += (b)
#define HH(a,b,c,d,x,s,ac) (a) += H((b),(c),(d)) + (x) + (uint32_t)(ac); \
    (a) = ROTL((a),(s)); (a) += (b)
#define II(a,b,c,d,x,s,ac) (a) += I((b),(c),(d)) + (x) + (uint32_t)(ac); \
    (a) = ROTL((a),(s)); (a) += (b)

static void md5_compress(uint32_t st[4], const uint8_t block[64])
{
    uint32_t x[16];
    for (int i = 0; i < 16; ++i)
        x[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) |
               ((uint32_t)block[i * 4 + 3] << 24);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];

    FF(a,b,c,d,x[ 0], 7,0xd76aa478); FF(d,a,b,c,x[ 1],12,0xe8c7b756);
    FF(c,d,a,b,x[ 2],17,0x242070db); FF(b,c,d,a,x[ 3],22,0xc1bdceee);
    FF(a,b,c,d,x[ 4], 7,0xf57c0faf); FF(d,a,b,c,x[ 5],12,0x4787c62a);
    FF(c,d,a,b,x[ 6],17,0xa8304613); FF(b,c,d,a,x[ 7],22,0xfd469501);
    FF(a,b,c,d,x[ 8], 7,0x698098d8); FF(d,a,b,c,x[ 9],12,0x8b44f7af);
    FF(c,d,a,b,x[10],17,0xffff5bb1); FF(b,c,d,a,x[11],22,0x895cd7be);
    FF(a,b,c,d,x[12], 7,0x6b901122); FF(d,a,b,c,x[13],12,0xfd987193);
    FF(c,d,a,b,x[14],17,0xa679438e); FF(b,c,d,a,x[15],22,0x49b40821);

    GG(a,b,c,d,x[ 1], 5,0xf61e2562); GG(d,a,b,c,x[ 6], 9,0xc040b340);
    GG(c,d,a,b,x[11],14,0x265e5a51); GG(b,c,d,a,x[ 0],20,0xe9b6c7aa);
    GG(a,b,c,d,x[ 5], 5,0xd62f105d); GG(d,a,b,c,x[10], 9,0x02441453);
    GG(c,d,a,b,x[15],14,0xd8a1e681); GG(b,c,d,a,x[ 4],20,0xe7d3fbc8);
    GG(a,b,c,d,x[ 9], 5,0x21e1cde6); GG(d,a,b,c,x[14], 9,0xc33707d6);
    GG(c,d,a,b,x[ 3],14,0xf4d50d87); GG(b,c,d,a,x[ 8],20,0x455a14ed);
    GG(a,b,c,d,x[13], 5,0xa9e3e905); GG(d,a,b,c,x[ 2], 9,0xfcefa3f8);
    GG(c,d,a,b,x[ 7],14,0x676f02d9); GG(b,c,d,a,x[12],20,0x8d2a4c8a);

    HH(a,b,c,d,x[ 5], 4,0xfffa3942); HH(d,a,b,c,x[ 8],11,0x8771f681);
    HH(c,d,a,b,x[11],16,0x6d9d6122); HH(b,c,d,a,x[14],23,0xfde5380c);
    HH(a,b,c,d,x[ 1], 4,0xa4beea44); HH(d,a,b,c,x[ 4],11,0x4bdecfa9);
    HH(c,d,a,b,x[ 7],16,0xf6bb4b60); HH(b,c,d,a,x[10],23,0xbebfbc70);
    HH(a,b,c,d,x[13], 4,0x289b7ec6); HH(d,a,b,c,x[ 0],11,0xeaa127fa);
    HH(c,d,a,b,x[ 3],16,0xd4ef3085); HH(b,c,d,a,x[ 6],23,0x04881d05);
    HH(a,b,c,d,x[ 9], 4,0xd9d4d039); HH(d,a,b,c,x[12],11,0xe6db99e5);
    HH(c,d,a,b,x[15],16,0x1fa27cf8); HH(b,c,d,a,x[ 2],23,0xc4ac5665);

    II(a,b,c,d,x[ 0], 6,0xf4292244); II(d,a,b,c,x[ 7],10,0x432aff97);
    II(c,d,a,b,x[14],15,0xab9423a7); II(b,c,d,a,x[ 5],21,0xfc93a039);
    II(a,b,c,d,x[12], 6,0x655b59c3); II(d,a,b,c,x[ 3],10,0x8f0ccc92);
    II(c,d,a,b,x[10],15,0xffeff47d); II(b,c,d,a,x[ 1],21,0x85845dd1);
    II(a,b,c,d,x[ 8], 6,0x6fa87e4f); II(d,a,b,c,x[15],10,0xfe2ce6e0);
    II(c,d,a,b,x[ 6],15,0xa3014314); II(b,c,d,a,x[13],21,0x4e0811a1);
    II(a,b,c,d,x[ 4], 6,0xf7537e82); II(d,a,b,c,x[11],10,0xbd3af235);
    II(c,d,a,b,x[ 2],15,0x2ad7d2bb); II(b,c,d,a,x[ 9],21,0xeb86d391);

    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

void nsbc_md5_init(nsbc_md5_t *c)
{
    c->a = 0x67452301;
    c->b = 0xefcdab89;
    c->c = 0x98badcfe;
    c->d = 0x10325476;
    c->bits = 0;
}

void nsbc_md5_update(nsbc_md5_t *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t used = (size_t)((c->bits >> 3) & 63);
    c->bits += (uint64_t)len << 3;
    size_t need = 64 - used;
    if (len >= need) {
        memcpy(c->buf + used, p, need);
        md5_compress(&c->a, c->buf);
        p += need;
        len -= need;
        used = 0;
        while (len >= 64) {
            md5_compress(&c->a, p);
            p += 64;
            len -= 64;
        }
    }
    if (len)
        memcpy(c->buf + used, p, len);
}

void nsbc_md5_final(nsbc_md5_t *c, uint8_t out[16])
{
    size_t used = (size_t)((c->bits >> 3) & 63);
    c->buf[used++] = 0x80;
    if (used > 56) {
        memset(c->buf + used, 0, 64 - used);
        md5_compress(&c->a, c->buf);
        used = 0;
    }
    memset(c->buf + used, 0, 56 - used);
    uint64_t b = c->bits;
    for (int i = 0; i < 8; ++i)
        c->buf[56 + i] = (uint8_t)(b >> (8 * i));
    md5_compress(&c->a, c->buf);
    uint32_t st[4] = {c->a, c->b, c->c, c->d};
    for (int i = 0; i < 4; ++i) {
        out[i * 4] = (uint8_t)(st[i]);
        out[i * 4 + 1] = (uint8_t)(st[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(st[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(st[i] >> 24);
    }
}

void nsbc_md5_hex(const void *data, size_t len, char out33[33])
{
    static const char hx[] = "0123456789abcdef";
    uint8_t d[16];
    nsbc_md5_t c;
    nsbc_md5_init(&c);
    nsbc_md5_update(&c, data, len);
    nsbc_md5_final(&c, d);
    for (int i = 0; i < 16; ++i) {
        out33[i * 2] = hx[d[i] >> 4];
        out33[i * 2 + 1] = hx[d[i] & 15];
    }
    out33[32] = '\0';
}

void nsbc_md5_hex_n(char out33[33], int n, ...)
{
    nsbc_md5_t c;
    va_list ap;
    nsbc_md5_init(&c);
    va_start(ap, n);
    for (int i = 0; i < n; ++i) {
        const char *s = va_arg(ap, const char *);
        if (s)
            nsbc_md5_update(&c, s, strlen(s));
    }
    va_end(ap);
    uint8_t d[16];
    static const char hx[] = "0123456789abcdef";
    nsbc_md5_final(&c, d);
    for (int i = 0; i < 16; ++i) {
        out33[i * 2] = hx[d[i] >> 4];
        out33[i * 2 + 1] = hx[d[i] & 15];
    }
    out33[32] = '\0';
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
