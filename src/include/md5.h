#ifndef NSBC_MD5_H
#define NSBC_MD5_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* RFC 1321 MD5：仅用于 SIP Digest 鉴权（RFC 2617）；
 * 不得用于任何安全新协议。 */
typedef struct {
    uint32_t a, b, c, d;
    uint64_t bits;
    uint8_t buf[64];
} nsbc_md5_t;

void nsbc_md5_init(nsbc_md5_t *c);
void nsbc_md5_update(nsbc_md5_t *c, const void *data, size_t len);
void nsbc_md5_final(nsbc_md5_t *c, uint8_t out[16]);
void nsbc_md5_hex(const void *data, size_t len, char out33[33]);

/* 拼接多段（NULL 结尾），一次 MD5 小写 hex；参数个数 ≤ 6 */
void nsbc_md5_hex_n(char out33[33], int n, ...);

#ifdef __cplusplus
}
#endif
#endif
