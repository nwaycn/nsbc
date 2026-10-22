#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "net_sock.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

void nsbc_sa_clear(nsbc_sa_t *a)
{
    memset(a, 0, sizeof(*a));
    a->len = sizeof(a->ss);
}

int nsbc_sa_family(const nsbc_sa_t *a) { return a->ss.ss_family; }

uint16_t nsbc_sa_port(const nsbc_sa_t *a)
{
    if (a->ss.ss_family == AF_INET)
        return ntohs(((const struct sockaddr_in *)&a->ss)->sin_port);
    if (a->ss.ss_family == AF_INET6)
        return ntohs(((const struct sockaddr_in6 *)&a->ss)->sin6_port);
    return 0;
}

void nsbc_sa_set_port(nsbc_sa_t *a, uint16_t port)
{
    if (a->ss.ss_family == AF_INET)
        ((struct sockaddr_in *)&a->ss)->sin_port = htons(port);
    else if (a->ss.ss_family == AF_INET6)
        ((struct sockaddr_in6 *)&a->ss)->sin6_port = htons(port);
}

int nsbc_sa_fmt(const nsbc_sa_t *a, char *out, size_t outsz)
{
    char ip[INET6_ADDRSTRLEN] = {0};
    if (a->ss.ss_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)&a->ss;
        inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
        int n = snprintf(out, outsz, "%s:%u", ip, ntohs(s->sin_port));
        return (n > 0 && (size_t)n < outsz) ? NSBC_OK : NSBC_NOMEM;
    }
    if (a->ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)&a->ss;
        inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
        int n = snprintf(out, outsz, "[%s]:%u", ip, ntohs(s->sin6_port));
        return (n > 0 && (size_t)n < outsz) ? NSBC_OK : NSBC_NOMEM;
    }
    if (outsz)
        out[0] = '\0';
    return NSBC_INVAL;
}

static int same_addr(const struct in_addr *a, const struct in_addr *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

int nsbc_sa_eq_addr(const nsbc_sa_t *a, const nsbc_sa_t *b)
{
    if (a->ss.ss_family != b->ss.ss_family) {
        /* v4-mapped AF_INET6 ↔ AF_INET 归一化（dual-stack socket 收 IPv4
         * 报文时 recvfrom 返回 sockaddr_in6 family=AF_INET6，需与 peer
         * 的 AF_INET 比较。relay_* 已显式用 dualstack=0，但作为防御
         * 放在公共函数里，避免其他调用方再踩同类坑）。 */
        struct in_addr av, bv;
        const struct in_addr *ap = NULL, *bp = NULL;
        if (a->ss.ss_family == AF_INET) {
            ap = &((const struct sockaddr_in *)&a->ss)->sin_addr;
        } else if (a->ss.ss_family == AF_INET6) {
            const struct sockaddr_in6 *x =
                (const struct sockaddr_in6 *)&a->ss;
            if (IN6_IS_ADDR_V4MAPPED(&x->sin6_addr)) {
                memcpy(&av, &x->sin6_addr.s6_addr[12], 4);
                ap = &av;
            }
        }
        if (b->ss.ss_family == AF_INET) {
            bp = &((const struct sockaddr_in *)&b->ss)->sin_addr;
        } else if (b->ss.ss_family == AF_INET6) {
            const struct sockaddr_in6 *y =
                (const struct sockaddr_in6 *)&b->ss;
            if (IN6_IS_ADDR_V4MAPPED(&y->sin6_addr)) {
                memcpy(&bv, &y->sin6_addr.s6_addr[12], 4);
                bp = &bv;
            }
        }
        if (ap && bp)
            return memcmp(ap, bp, sizeof(*ap)) == 0;
        return 0;
    }
    if (a->ss.ss_family == AF_INET)
        return same_addr(&((const struct sockaddr_in *)&a->ss)->sin_addr,
                         &((const struct sockaddr_in *)&b->ss)->sin_addr);
    if (a->ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)&a->ss;
        const struct sockaddr_in6 *y = (const struct sockaddr_in6 *)&b->ss;
        return memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr)) ==
               0;
    }
    return 0;
}

int nsbc_sa_eq(const nsbc_sa_t *a, const nsbc_sa_t *b)
{
    return nsbc_sa_eq_addr(a, b) && nsbc_sa_port(a) == nsbc_sa_port(b);
}

int nsbc_sa_from(nsbc_sa_t *a, const char *host, uint16_t port, int family)
{
    nsbc_sa_clear(a);
    if (host == NULL || *host == '\0' || strcmp(host, "*") == 0) {
        if (family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&a->ss;
            s->sin_family = AF_INET;
            s->sin_addr.s_addr = htonl(INADDR_ANY);
            s->sin_port = htons(port);
            a->len = sizeof(*s);
        } else {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&a->ss;
            s->sin6_family = AF_INET6;
            s->sin6_addr = in6addr_any;
            s->sin6_port = htons(port);
            a->len = sizeof(*s);
        }
        return NSBC_OK;
    }
    struct in6_addr v6;
    struct in_addr v4;
    if (inet_pton(AF_INET, host, &v4) == 1) {
        if (family == AF_INET6) {
            /* dual-stack 套接字：IPv4 字面量必须转 v4-mapped（::ffff:a.b.c.d），
             * 通配 0.0.0.0 等价 in6addr_any；否则 bind(2) 返回 EINVAL。 */
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&a->ss;
            s->sin6_family = AF_INET6;
            s->sin6_port = htons(port);
            if (v4.s_addr == htonl(INADDR_ANY)) {
                s->sin6_addr = in6addr_any;
            } else {
                memset(&s->sin6_addr, 0, sizeof(s->sin6_addr));
                s->sin6_addr.s6_addr[10] = 0xff;
                s->sin6_addr.s6_addr[11] = 0xff;
                memcpy(&s->sin6_addr.s6_addr[12], &v4.s_addr, 4);
            }
            a->len = sizeof(*s);
            return NSBC_OK;
        }
        struct sockaddr_in *s = (struct sockaddr_in *)&a->ss;
        s->sin_family = AF_INET;
        s->sin_addr = v4;
        s->sin_port = htons(port);
        a->len = sizeof(*s);
        return NSBC_OK;
    }
    if (inet_pton(AF_INET6, host, &v6) == 1) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&a->ss;
        s->sin6_family = AF_INET6;
        s->sin6_addr = v6;
        s->sin6_port = htons(port);
        a->len = sizeof(*s);
        return NSBC_OK;
    }
    return NSBC_INVAL; /* 主机名走 dns_cache，不在这里隐式解析 */
}

static void set_rcvbuff(int fd, int bytes, int which)
{
    if (bytes <= 0)
        return;
    setsockopt(fd, SOL_SOCKET, which, &bytes, (socklen_t)sizeof(bytes));
}

int net_set_nonblock_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return NSBC_ERR;
    int c = fcntl(fd, F_GETFD, 0);
    if (c < 0 || fcntl(fd, F_SETFD, c | FD_CLOEXEC) < 0)
        return NSBC_ERR;
    return NSBC_OK;
}

int net_udp_open(const char *bind_host, uint16_t port, const net_udp_cfg_t *cfg)
{
    /* REQ: 禁 IPv6 监听——一律 IPv4。旧 dualstack=1 会走 AF_INET6+V6ONLY=0
     * 双栈，现强制 AF_INET；bind_host 显式带 v6 字面量也忽略。 */
    int family = AF_INET;
    (void)cfg; /* dualstack/pktinfo 等字段暂不使用（IPv4-only） */
    int fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return NSBC_ERR;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (cfg && cfg->reuseport)
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
    if (cfg && cfg->pktinfo)
        setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &yes, sizeof(yes));
    if (cfg) {
        set_rcvbuff(fd, cfg->rcvbuf, SO_RCVBUF);
        set_rcvbuff(fd, cfg->sndbuf, SO_SNDBUF);
    }
    nsbc_sa_t a;
    if (nsbc_sa_from(&a, bind_host, port, family) != NSBC_OK) {
        close(fd);
        return NSBC_INVAL;
    }
    if (bind(fd, (struct sockaddr *)&a.ss, a.len) < 0) {
        close(fd);
        return NSBC_ERR;
    }
    return fd;
}

int net_tcp_listen(const char *bind_host, uint16_t port, int backlog,
                   int reuseport)
{
    /* REQ: 禁 IPv6 监听——一律 IPv4。旧逻辑按 bind_host 中是否含 ':' 判
     * AF_INET6，现强制 AF_INET。 */
    int family = AF_INET;
    int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return NSBC_ERR;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (reuseport)
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
    nsbc_sa_t a;
    if (nsbc_sa_from(&a, bind_host, port, family) != NSBC_OK) {
        close(fd);
        return NSBC_INVAL;
    }
    if (bind(fd, (struct sockaddr *)&a.ss, a.len) < 0 ||
        listen(fd, backlog > 0 ? backlog : 256) < 0) {
        close(fd);
        return NSBC_ERR;
    }
    return fd;
}

int net_tcp_accept(int lfd, nsbc_sa_t *peer)
{
    nsbc_sa_clear(peer);
    int fd = accept4(lfd, (struct sockaddr *)&peer->ss, &peer->len,
                     SOCK_NONBLOCK | SOCK_CLOEXEC);
    return fd; /* <0：errno 由调用方处理（EAGAIN 正常） */
}
