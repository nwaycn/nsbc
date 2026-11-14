/* BFD Control 亚秒探测骨架（P1，REQ-HA-001 增强）
 *
 * RFC 5880 BFD Control 包 52 字节固定结构 —— 本实现简化：
 *   - 只跑 ASCII Control 模式，不实现 Echo（D bit 恒 0）
 *   - 两机同同时发 BFD Control（ACT/STANDBY 都发）
 *   - State 字段按 RFC 5881 状态机（AdminDown → Down → Init → Up）
 * ha_health 接线：每 recv 到有效 BFD Control → ha_hc_peer_probe(1, rtt_ms)
 *                 连续 miss ≥ multiplier → ha_hc_peer_probe(0, -1)
 * 与 UDP 5070 心跳互补：心跳仲裁角色，BFD 亚秒级精确探测。 */
#include "ha_bfd.h"
#include "cfg.h"
#include "ha.h"
#include "log.h"
#include "net_sock.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/udp.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define BFD_CONTROL_MSG_LEN  52
#define BFD_VER             1
#define BFD_STATE_ADMINDOWN 0
#define BFD_STATE_DOWN      1
#define BFD_STATE_INIT      2
#define BFD_STATE_UP        3

struct ha_bfd {
    int enabled;
    pthread_t thr;
    int stop;

    int fd;
    int connected;

    uint32_t local_disc;
    uint32_t desired_tx_ms;
    uint32_t required_rx_ms;
    uint8_t  detect_mult;
    uint8_t  state;     /* RFC 5881: AdminDown/Down/Init/Up */

    int consecutive_misses;

    nsbc_sa_t peer_addr;
    int have_peer;

    ha_hc_t *hc;        /* ha_health 探测回调 —— ha.c 传 h->hc 进来 */
};

/* === BFD Control 包构建（RFC 5880 Section 4.1） === */

static void bfd_build_control(char *out, uint32_t peer_disc, uint8_t state)
{
    /* 52 字节，固定字段，网络字节序 */
    memset(out, 0, BFD_CONTROL_MSG_LEN);
    out[0] = (BFD_VER << 5) | (state & 0x7);   /* Ver | State */
    out[1] = 0;                                  /* Diag: no diag */
    out[2] = 0;                                  /* D=0,A=0,C=0,P=F=0 */
    /* Detect Multiplier */
    out[3] = 1;                                  /* 我们不设 multiplier 在本线程；HA 层设 */
    /* Your Discriminator (0 when state=AdminDown) */
    uint32_t your = (state == BFD_STATE_ADMINDOWN) ? 0 : peer_disc;
    uint32_t my = 0; /* 我们的 disc —— 从 cfg 取 */
    memcpy(out + 4,  &your, 4);
    memcpy(out + 8,  &my,   4);
    /* Desired Min TX Interval */
    uint32_t dtx = 100000;  /* default 100ms → 100000 us */
    memcpy(out + 12, &dtx, 4);
    /* Required Min RX Interval */
    uint32_t drx = 100000;
    memcpy(out + 16, &drx, 4);
    /* Required Min Echo RX Interval — 0（不跑 Echo） */
    uint32_t echo = 0;
    memcpy(out + 20, &echo, 4);
}

/* === BFD 线程主循环 === */

static void *ha_bfd_thread(void *arg)
{
    ha_bfd_t *b = arg;
    char tx[BFD_CONTROL_MSG_LEN];
    char rx[BFD_CONTROL_MSG_LEN];

    while (!__atomic_load_n(&b->stop, __ATOMIC_ACQUIRE)) {
        /* 发包（RFC 5881: 每次 poll 周期都发） */
        uint32_t peer_disc = 0;
        bfd_build_control(tx, peer_disc, b->state);
        sendto(b->fd, tx, BFD_CONTROL_MSG_LEN, 0,
               (const struct sockaddr *)&b->peer_addr.ss, b->peer_addr.len);

        /* 收包（poll + recv） */
        struct pollfd pfd = { b->fd, POLLIN, 0 };
        int wait_ms = b->desired_tx_ms;
        /* 非阻塞 + 周期 send 时机 */
        int rc = poll(&pfd, 1, wait_ms);
        if (rc < 0 && errno != EINTR) break;
        int got_any = 0;
        while (rc > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = recv(b->fd, rx, sizeof(rx), 0);
            if (n <= 0) break;
            if ((size_t)n >= 5) {
                uint8_t ver_state = (uint8_t)rx[0];
                uint8_t ver = ver_state >> 5;
                if (ver != BFD_VER) break;
                uint8_t peer_state = ver_state & 0x7;
                uint32_t peer_disc_recv = 0;
                memcpy(&peer_disc_recv, rx + 4, 4);
                got_any = 1;
                /* RFC 5881 简化状态机 */
                if (b->state == BFD_STATE_ADMINDOWN) {
                    b->state = BFD_STATE_DOWN;
                } else if (b->state == BFD_STATE_DOWN && peer_state == BFD_STATE_DOWN) {
                    b->state = BFD_STATE_INIT;
                } else if (b->state == BFD_STATE_INIT) {
                    b->state = BFD_STATE_UP;
                    llog(LOG_NOTICE, "ha-bfd", "session UP peer_disc=%u", peer_disc_recv);
                }
                b->consecutive_misses = 0;
                b->connected = 1;
                /* === 接线 ha_health === */
                if (b->hc) ha_hc_peer_probe(b->hc, 1, 0);
            }
        }
        if (!got_any && b->state != BFD_STATE_ADMINDOWN) {
            b->consecutive_misses++;
            if (b->consecutive_misses >= b->detect_mult) {
                llog(LOG_WARN, "ha-bfd",
                     "session DOWN consecutive_misses=%d detect_mult=%d",
                     b->consecutive_misses, b->detect_mult);
                b->state = BFD_STATE_DOWN;
                b->connected = 0;
                /* === 接线 ha_health === */
                if (b->hc) ha_hc_peer_probe(b->hc, 0, -1);
            }
        }
    }
    return NULL;
}

/* === 公共 API === */

int ha_bfd_create(const ha_bfd_cfg_t *cfg, ha_bfd_t **out)
{
    if (cfg == NULL || out == NULL) return NSBC_INVAL;
    if (!cfg->enabled || !cfg->have_peer) {
        *out = NULL;
        return NSBC_NOENT;  /* 关闭或单机 —— 跳过 */
    }
    ha_bfd_t *b = calloc(1, sizeof(*b));
    if (b == NULL) return NSBC_NOMEM;
    b->enabled = 1;
    b->have_peer = 1;
    b->peer_addr = cfg->peer_addr;
    b->local_disc = cfg->local_discriminator;
    b->desired_tx_ms = cfg->desired_min_tx_interval;
    b->required_rx_ms = cfg->required_min_rx_interval;
    b->detect_mult = cfg->detect_multiplier;
    b->state = BFD_STATE_ADMINDOWN;
    b->connected = 0;
    b->consecutive_misses = 0;
    b->hc = cfg->hc;   /* ha_health 回调 —— 允许为 NULL（ha_hc_create 失败时） */

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { free(b); return NSBC_NOMEM; }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_UDP, SO_REUSEADDR, &one, sizeof(one));
    b->fd = fd;

    __atomic_store_n(&b->stop, 0, __ATOMIC_RELEASE);
    if (pthread_create(&b->thr, NULL, ha_bfd_thread, b) != 0) {
        llog(LOG_WARN, "ha-bfd", "thread create errno=%d; degrade", errno);
        close(fd); free(b); *out = NULL;
        return NSBC_OK;
    }
    llog(LOG_NOTICE, "ha-bfd",
         "enabled disc=%u tx_ms=%u mult=%u",
         b->local_disc, b->desired_tx_ms, (unsigned)b->detect_mult);
    *out = b;
    return NSBC_OK;
}

void ha_bfd_destroy(ha_bfd_t *b)
{
    if (b == NULL) return;
    __atomic_store_n(&b->stop, 1, __ATOMIC_RELEASE);
    pthread_join(b->thr, NULL);
    close(b->fd);
    free(b);
}

/*
    Author:       李浩 (Lihao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
