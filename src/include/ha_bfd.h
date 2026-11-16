#ifndef NSBC_HA_BFD_H
#define NSBC_HA_BFD_H

/* BFD 亚秒探测（P1，REQ-HA-001 增强）
 *
 * 职责：RFC 5880 Control 包独立 UDP socket 探测对端存活；与 UDP 5070 心跳
 * 互补（心跳仲裁角色，BFD 精确探测）。
 * 线程模型：独立 pthread（与心跳线程分离），poll() 周期发送 + 接收。
 * ha_health 接线：每收到 BFD Control 调 ha_hc_peer_probe(ok, rtt_ms)；
 *                 连续 detect_multiplier 次未收到则调 ha_hc_peer_probe(0, -1)。
 * socket 非阻塞 + SO_RCVTIMEO 兜底。
 */
#include "nsbc_common.h"
#include "ha_health.h"
#include "net_sock.h"    /* nsbc_sa_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      enabled;
    uint32_t local_discriminator;
    uint32_t desired_min_tx_interval;   /* ms */
    uint32_t required_min_rx_interval;  /* ms */
    uint8_t  detect_multiplier;
    int      have_peer;
    nsbc_sa_t peer_addr;
    /* P1 接线：ha_health 探测回调 —— ha_bfd 线程每收到有效 Control 调
     * ha_hc_peer_probe(hc, 1, rtt_ms)；连续 miss ≥ multiplier 调
     * ha_hc_peer_probe(hc, 0, -1)。由 ha.c 传 h->hc 进来。 */
    ha_hc_t *hc;
} ha_bfd_cfg_t;

typedef struct ha_bfd ha_bfd_t;

int  ha_bfd_create(const ha_bfd_cfg_t *cfg, ha_bfd_t **out);
void ha_bfd_destroy(ha_bfd_t *b);

#ifdef __cplusplus
}
#endif
#endif

/*
    Author:       李浩 (Lihao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
