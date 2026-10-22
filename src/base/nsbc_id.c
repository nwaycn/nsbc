#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nsbc_id.h"
#include "nsbc_common.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 0x0001
#endif
#ifndef SYS_getrandom
#if defined(__x86_64__)
#define SYS_getrandom 318
#elif defined(__aarch64__)
#define SYS_getrandom 278
#endif
#endif

static int rand_from_syscall(void *buf, size_t len)
{
#if defined(SYS_getrandom)
    size_t off = 0;
    while (off < len) {
        long r = syscall(SYS_getrandom, (char *)buf + off, len - off,
                         GRND_NONBLOCK);
        if (r < 0)
            return -1;
        if (r == 0)
            return -1;
        off += (size_t)r;
    }
    return 0;
#else
    (void)buf;
    (void)len;
    return -1;
#endif
}

static int rand_from_urandom(void *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t r = read(fd, (char *)buf + off, len - off);
        if (r <= 0) {
            close(fd);
            return -1;
        }
        off += (size_t)r;
    }
    close(fd);
    return 0;
}

int nsbc_rand_fill(void *buf, size_t len)
{
    if (rand_from_syscall(buf, len) == 0)
        return NSBC_OK;
    if (rand_from_urandom(buf, len) == 0)
        return NSBC_OK;
    return NSBC_ERR; /* 无随机源：启动期 fail-fast，严禁降级弱随机 */
}

static const char hex[] = "0123456789abcdef";

int nsbc_rand_token(char *out, size_t outsz, size_t hex_len)
{
    uint8_t b[64];
    if (hex_len > sizeof(b) * 2 || outsz < hex_len + 1)
        return NSBC_INVAL;
    if (nsbc_rand_fill(b, (hex_len + 1) / 2) != NSBC_OK)
        return NSBC_ERR;
    for (size_t i = 0; i < hex_len; ++i)
        out[i] = hex[(b[i / 2] >> ((i & 1) * 4)) & 0xf];
    out[hex_len] = '\0';
    return NSBC_OK;
}

int nsbc_uuid4(char *out, size_t outsz)
{
    uint8_t b[16];
    if (outsz < 37 || nsbc_rand_fill(b, sizeof(b)) != NSBC_OK)
        return NSBC_INVAL;
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40); /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80); /* variant 10xx */
    snprintf(out, outsz,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
             b[10], b[11], b[12], b[13], b[14], b[15]);
    return NSBC_OK;
}

int nsbc_callid_make(char *out, size_t outsz, const char *host_tag)
{
    char tok[33];
    if (nsbc_rand_token(tok, sizeof(tok), 32) != NSBC_OK)
        return NSBC_ERR;
    int n = snprintf(out, outsz, "%s@%s", tok,
                     (host_tag && *host_tag) ? host_tag : "nsbc");
    return (n > 0 && (size_t)n < outsz) ? NSBC_OK : NSBC_NOMEM;
}

uint32_t nsbc_ssrc_random(void)
{
    uint32_t s;
    if (nsbc_rand_fill(&s, sizeof(s)) != NSBC_OK)
        return 0;
    return s;
}
