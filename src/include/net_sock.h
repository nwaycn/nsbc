#ifndef NSBC_NET_SOCK_H
#define NSBC_NET_SOCK_H

/* socket 基础封装（net 层）：
 * - 全程 SOCK_NONBLOCK|SOCK_CLOEXEC；
 * - SO_REUSEPORT 多接收线程（REQ-SIP-006 / REQ-MEDIA-FWD-001）；
 * - IPv4/IPv6 双栈，IPv6 通配 socket 关 V6ONLY（REQ-ENV-004）；
 * - IP_PKTINFO：多 IP 下区分入向本地地址（port 表选表依据，7.6）。
 */

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "nsbc_common.h"

typedef struct {
    struct sockaddr_storage ss;
    socklen_t len;
} nsbc_sa_t;

void nsbc_sa_clear(nsbc_sa_t *a);
uint16_t nsbc_sa_port(const nsbc_sa_t *a);
void nsbc_sa_set_port(nsbc_sa_t *a, uint16_t port);
int nsbc_sa_family(const nsbc_sa_t *a);
/* 输出 "1.2.3.4:5060" / "[2001:db8::1]:5060" */
int nsbc_sa_fmt(const nsbc_sa_t *a, char *out, size_t outsz);
/* 地址（含端口）/仅地址 比较 */
int nsbc_sa_eq(const nsbc_sa_t *a, const nsbc_sa_t *b);
int nsbc_sa_eq_addr(const nsbc_sa_t *a, const nsbc_sa_t *b);

/* host=NULL：通配；family=AF_UNSPEC 时按 host 字面量猜，NULL 用 AF_INET6 双栈 */
int nsbc_sa_from(nsbc_sa_t *a, const char *host, uint16_t port, int family);

typedef struct {
    int reuseport;       /* 多线程同一绑定 */
    int rcvbuf;          /* 0 不设置 */
    int sndbuf;
    int pktinfo;         /* 收包附带本地目的地址（IP_PKTINFO） */
    int dualstack;       /* IPv6 通配监听双栈 */
} net_udp_cfg_t;

int net_udp_open(const char *bind_host, uint16_t port, const net_udp_cfg_t *cfg);
int net_tcp_listen(const char *bind_host, uint16_t port, int backlog,
                   int reuseport);
int net_tcp_accept(int lfd, nsbc_sa_t *peer);
int net_set_nonblock_cloexec(int fd);

#endif /* NSBC_NET_SOCK_H */
