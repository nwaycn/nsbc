#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "net_mmsg.h"
#include "log.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef UDPM_SEG
#define UDPM_SEG 2
#endif
#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#ifndef ENOIOCTLCMD
#define ENOIOCTLCMD 515
#endif

void net_rx_prepare(net_rx_t *rx, net_dgram_t dg[NET_BATCH])
{
    memset(rx, 0, sizeof(*rx));
    for (int i = 0; i < NET_BATCH; ++i) {
        rx->mm[i].msg_hdr.msg_iov = &rx->iov[i];
        rx->mm[i].msg_hdr.msg_iovlen = 1;
        rx->mm[i].msg_hdr.msg_name = &dg[i].peer.ss;
        rx->mm[i].msg_hdr.msg_namelen = sizeof(dg[i].peer.ss);
        rx->mm[i].msg_hdr.msg_control = NULL;
        rx->mm[i].msg_hdr.msg_controllen = 0;
        rx->iov[i].iov_base = dg[i].buf;
        rx->iov[i].iov_len = dg[i].cap;
        dg[i].peer.len = 0;
        dg[i].local.len = 0;
    }
}

/* 每个收包都解析 cmsg（pktinfo 决定多 IP port 表选择，7.6）。
 * recvmmsg 不支持逐包独立 control 缓冲时，用 timeout=0 单批并共用 64B：
 * Linux 上各包 cmsg 依次写入 msg_control，长度由 msg_controllen 限定。 */
static void parse_pktinfo(struct msghdr *m, nsbc_sa_t *local)
{
    for (struct cmsghdr *c = CMSG_FIRSTHDR(m); c != NULL;
         c = CMSG_NXTHDR(m, c)) {
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO &&
            CMSG_LEN(sizeof(struct in_pktinfo)) <= c->cmsg_len) {
            const struct in_pktinfo *pi = (const struct in_pktinfo *)CMSG_DATA(c);
            struct sockaddr_in *s = (struct sockaddr_in *)&local->ss;
            s->sin_family = AF_INET;
            s->sin_addr = pi->ipi_addr;
            local->len = sizeof(*s);
        } else if (c->cmsg_level == IPPROTO_IPV6 &&
                   c->cmsg_type == IPV6_PKTINFO &&
                   CMSG_LEN(sizeof(struct in6_pktinfo)) <= c->cmsg_len) {
            const struct in6_pktinfo *pi =
                (const struct in6_pktinfo *)CMSG_DATA(c);
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&local->ss;
            s->sin6_family = AF_INET6;
            s->sin6_addr = pi->ipi6_addr;
            local->len = sizeof(*s);
        }
    }
}

int net_rx_recv(int fd, net_rx_t *rx, net_dgram_t dg[NET_BATCH])
{
    /* 每个包单独 64B cmsg 空间（cmsg_buf 按 64B/包切分） */
    for (int i = 0; i < NET_BATCH; ++i) {
        rx->mm[i].msg_hdr.msg_control = &rx->cmsg_buf[i * 64];
        rx->mm[i].msg_hdr.msg_controllen = 64;
        rx->mm[i].msg_hdr.msg_iov->iov_base = dg[i].buf;
        rx->mm[i].msg_hdr.msg_iov->iov_len = dg[i].cap;
    }
    int n = recvmmsg(fd, rx->mm, NET_BATCH, 0, NULL);
    if (n < 0)
        return -1;
    for (int i = 0; i < n; ++i) {
        dg[i].len = (size_t)rx->mm[i].msg_len;
        dg[i].peer.len = rx->mm[i].msg_hdr.msg_namelen;
        parse_pktinfo(&rx->mm[i].msg_hdr, &dg[i].local);
        /* local 端口对 port 表无意义（就是本 socket），置 0 由查表填 */
        nsbc_sa_set_port(&dg[i].local, 0);
    }
    return n;
}

int net_tx_batch(int fd, const net_tx_dgram_t *dg, int n)
{
    if (n <= 0)
        return 0;
    int sent = 0;
    for (int i = 0; i < n; ++i) {
        ssize_t rc = sendto(fd, dg[i].buf, dg[i].len, MSG_NOSIGNAL,
                   (struct sockaddr *)&dg[i].dst.ss, dg[i].dst.len);
        if (rc >= 0) {
            ++sent;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            char ip[INET6_ADDRSTRLEN] = "-";
            if (dg[i].dst.ss.ss_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)&dg[i].dst.ss;
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            }
            llog(LOG_WARN, "net",
                 "sendto fd=%d len=%zu dst=%s failed errno=%d(%s)",
                 fd, dg[i].len, ip, errno, strerror(errno));
            break;
        }
    }
    return sent;
}

int net_tx_gso(int fd, const void *buf, size_t seglen, int nseg,
               const nsbc_sa_t *dst)
{
#if defined(UDP_SEGMENT)
    struct {
        struct cmsghdr h;
        uint16_t gso_size;
    } cmsg;
    struct iovec iov;
    struct msghdr m;
    memset(&m, 0, sizeof(m));
    memset(&cmsg, 0, sizeof(cmsg));
    iov.iov_base = (void *)buf;
    iov.iov_len = seglen * (size_t)nseg;
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_name = (void *)&dst->ss;
    m.msg_namelen = dst->len;
    m.msg_control = &cmsg;
    m.msg_controllen = sizeof(cmsg);
    cmsg.h.cmsg_len = CMSG_LEN(sizeof(uint16_t));
    cmsg.h.cmsg_level = SOL_UDP;
    cmsg.h.cmsg_type = UDP_SEGMENT;
    cmsg.gso_size = (uint16_t)seglen;
    if (sendmsg(fd, &m, MSG_NOSIGNAL) >= 0)
        return NSBC_OK;
    if (errno != ENOSYS && errno != EINVAL && errno != ENOIOCTLCMD)
        return NSBC_ERR;
    /* fallthrough：内核不支持 GSO */
#endif
    const uint8_t *p = (const uint8_t *)buf;
    for (int i = 0; i < nseg; ++i) {
        if (sendto(fd, p + i * seglen, seglen, MSG_NOSIGNAL,
                   (struct sockaddr *)&dst->ss, dst->len) < 0 &&
            errno != EAGAIN && errno != EWOULDBLOCK)
            return NSBC_ERR;
    }
    return NSBC_OK;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
