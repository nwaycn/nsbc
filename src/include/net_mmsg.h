#ifndef NSBC_NET_MMSG_H
#define NSBC_NET_MMSG_H

/* REQ-SIP-006 / REQ-MEDIA-FWD-001：批量收发
 *
 * recvmmsg 一次系统调用取 ≤batch 个数据报（建议 32）；
 * 发送 sendmmsg 攒批，并支持 UDP GSO（UDP_SEGMENT，Linux ≥4.18）：
 *   单系统调用发出 ≤64KB 连续段（约 40 个 1500B 数据报）；
 *   内核不支持（ENOSYS/EINVAL）自动回退 sendmmsg → sendto。
 * 3.10 内核无 GSO（REQ-ENV-001），回退路径必须等价正确。
 */

#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include "net_sock.h"

#define NET_BATCH 32
#define NET_GSO_MAX_BYTES 65535

typedef struct {
    void *buf;   /* 调用方（Arena）预取的缓冲 */
    size_t cap;
    size_t len;
    nsbc_sa_t peer;  /* 对端 */
    nsbc_sa_t local; /* 本包目的本地地址（pktinfo；未启用 len=0） */
} net_dgram_t;

typedef struct {
    struct mmsghdr mm[NET_BATCH];
    struct iovec iov[NET_BATCH];
    uint8_t cmsg_buf[NET_BATCH * 64] __attribute__((aligned(8)));
} net_rx_t;

void net_rx_prepare(net_rx_t *rx, net_dgram_t dg[NET_BATCH]);
/* 返回收到的数据报数；<0 errno 语义（EAGAIN 正常） */
int net_rx_recv(int fd, net_rx_t *rx, net_dgram_t dg[NET_BATCH]);

typedef struct {
    const void *buf;
    size_t len;
    nsbc_sa_t dst;
} net_tx_dgram_t;

/* 批量发送（目的可不同）；返回成功投递给内核的数据报数，<0 全部失败 */
int net_tx_batch(int fd, const net_tx_dgram_t *dg, int n);

/* GSO：nseg 个等长 segment（seglen 字节）连续放在 buf 中，同一目的 dst；
 * 返回 NSBC_OK / 失败码；不支持时内部回退逐包 sendto（对调用方透明）。 */
int net_tx_gso(int fd, const void *buf, size_t seglen, int nseg,
               const nsbc_sa_t *dst);

#endif /* NSBC_NET_MMSG_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
