#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "tcp_framer.h"

#include <ctype.h>
#include <string.h>

void tcp_framer_init(tcp_framer_t *f, uint8_t *buf, size_t cap,
                     size_t max_msg)
{
    f->buf = buf;
    f->cap = cap;
    f->start = f->end = 0;
    f->max_msg = max_msg;
}

static const uint8_t *find_header_end(const uint8_t *p, const uint8_t *end)
{
    /* 起始行起的头部以 CRLFCRLF 结束（严格按 RFC 3261） */
    for (const uint8_t *q = p; q + 4 <= end; ++q) {
        if (q[0] == '\r' && q[1] == '\n' && q[2] == '\r' && q[3] == '\n')
            return q + 4;
    }
    return NULL;
}

/* 在头部范围 [p, hend) 内解析 Content-Length；
 * - 有且仅有一个合法 CL：*cl 输出；
 * - 无 CL：FRAME_PROTO（TCP MUST）；
 * - 重复/非法：FRAME_PROTO；
 * - 发现 Transfer-Encoding: chunked：FRAME_PROTO（§7.4.2 禁止）。
 */
static frame_rc_t parse_content_length(const uint8_t *p, const uint8_t *hend,
                                       long *cl)
{
    int seen = 0;
    long val = 0;
    const uint8_t *line = p;
    while (line < hend) {
        const uint8_t *le = memchr(line, '\n', (size_t)(hend - line));
        if (!le)
            return FRAME_PROTO;
        const uint8_t *le2 = le;
        if (le2 > line && le2[-1] == '\r')
            --le2;
        const uint8_t *colon = NULL;
        for (const uint8_t *q = line; q < le2; ++q) {
            if (*q == ':') {
                colon = q;
                break;
            }
        }
        if (colon) {
            size_t nl = (size_t)(colon - line);
            static const char cl_name[] = "content-length";
            if (nl == sizeof(cl_name) - 1) {
                int match = 1;
                for (size_t i = 0; i < nl; ++i) {
                    if (tolower(line[i]) != cl_name[i]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    const uint8_t *v = colon + 1;
                    while (v < le2 && (*v == ' ' || *v == '\t'))
                        ++v;
                    long x = 0;
                    int digits = 0;
                    for (; v < le2; ++v) {
                        if (*v < '0' || *v > '9')
                            return FRAME_PROTO;
                        x = x * 10 + (*v - '0');
                        if (x > 0x100000) /* 超 1MiB 直接判非法 */
                            return FRAME_PROTO;
                        ++digits;
                    }
                    if (!digits)
                        return FRAME_PROTO;
                    if (seen)
                        return FRAME_PROTO;
                    seen = 1;
                    val = x;
                }
            }
            static const char te_name[] = "transfer-encoding";
            size_t tl = (size_t)(colon - line);
            if (tl == sizeof(te_name) - 1) {
                int match = 1;
                for (size_t i = 0; i < tl; ++i) {
                    if (tolower(line[i]) != te_name[i]) {
                        match = 0;
                        break;
                    }
                }
                if (match)
                    return FRAME_PROTO;
            }
        }
        line = le + 1;
    }
    if (!seen)
        return FRAME_PROTO;
    *cl = val;
    return FRAME_OK;
}

static void compact(tcp_framer_t *f)
{
    if (f->start == 0)
        return;
    size_t n = f->end - f->start;
    memmove(f->buf, f->buf + f->start, n);
    f->start = 0;
    f->end = n;
}

int tcp_framer_feed(tcp_framer_t *f, const uint8_t *data, size_t len,
                    int (*cb)(void *ud, const uint8_t *msg, size_t len),
                    void *ud, frame_rc_t *out_rc, int *n_msgs)
{
    *out_rc = FRAME_NEED_MORE;
    *n_msgs = 0;
    if (len > f->cap - f->end + f->start) {
        compact(f);
        if (len > f->cap - f->end)
            return NSBC_NOSPC; /* 单包超过缓冲：上层直接断连 */
    }
    if (len)
        memcpy(f->buf + f->end, data, len);
    f->end += len;

    for (;;) {
        compact(f);
        const uint8_t *p = f->buf + f->start;
        const uint8_t *end = f->buf + f->end;
        /* RFC 5626 §3.5.1：独立 CRLF 为连接保活，直接跳过 */
        while (end - p >= 2 && p[0] == '\r' && p[1] == '\n') {
            p += 2;
            f->start += 2;
        }
        if (p >= end)
            break;
        const uint8_t *hend = find_header_end(p, end);
        if (!hend) {
            if ((size_t)(end - p) > f->max_msg) {
                *out_rc = FRAME_OVERFLOW;
                return NSBC_OK;
            }
            break;
        }
        long cl;
        frame_rc_t rc = parse_content_length(p, hend, &cl);
        if (rc != FRAME_OK) {
            *out_rc = FRAME_PROTO;
            return NSBC_OK;
        }
        size_t total = (size_t)(hend - p) + (size_t)cl;
        if (total > f->max_msg) {
            *out_rc = FRAME_OVERFLOW;
            return NSBC_OK;
        }
        if ((size_t)(end - p) < total)
            break; /* 等正文剩余字节 */
        if (cb && cb(ud, p, total) != 0) {
            f->start += total;
            break;
        }
        f->start += total;
        ++*n_msgs;
        *out_rc = FRAME_OK;
    }
    /* 帧状态（OK/NEED_MORE/OVERFLOW/PROTO）一律经 *out_rc 给出；
     * 只有单包超缓冲这类资源错误才以返回值上报 */
    return NSBC_OK;
}
