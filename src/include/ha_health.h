#ifndef NSBC_HA_HEALTH_H
#define NSBC_HA_HEALTH_H

/* HA 健康检查与仲裁依据（REQ-HA-002，P0 健康门 / P1 BFD/VIP 联动）
 *
 * 与 ha.c（心跳+角色仲裁）分工：
 *  - ha.c：对端存活与角色（ACTIVE/STANDBY）；
 *  - 本模块：本机/对端"能不能扛流量"的健康评分，作为主动升主与
 *    拒绝抢占的门，避免双主/脑裂场景下病机抢主。
 * 输入：事件循环滞后、CPS 拒绝率、CPU/RSS（OAM 采样）、注册/对话
 * 复制水位差；对端心跳 RTT/丢包（P1 升级 BFD 亚秒探测）。
 * VIP：漂移由 keepalived/LB 外部执行，本模块只出"建议持/放 VIP"态。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HA_HC_HEALTHY = 0,   /* 可正常承载/可升主 */
    HA_HC_DEGRADED,      /* 承载但禁止主动抢占 */
    HA_HC_UNHEALTHY      /* 主动放 VIP/拒新呼（配过载策略） */
} ha_hc_state_t;

typedef struct {
    int      evlag_warn_ms;    /* 事件循环滞后告警（默认 200） */
    int      evlag_fail_ms;    /* 超限判 UNHEALTHY（默认 1000） */
    int      cpu_warn_pct;     /* 默认 80 */
    int      cpu_fail_pct;     /* 默认 95 */
    int      rss_warn_mb;      /* 默认 0=不判 */
    uint64_t repl_lag_warn;    /* 复制水位差告警条数 */
} ha_hc_cfg_t;

typedef struct {
    int      cpu_pct;
    int      rss_mb;
    int      evloop_lag_ms;
    int      overload_reject_rate_pct; /* 近窗口过载拒绝率 */
    uint64_t repl_lag;                 /* 仅备端有意义 */
} ha_hc_sample_t;

typedef struct ha_hc ha_hc_t;

int  ha_hc_create(const ha_hc_cfg_t *cfg, ha_hc_t **out);
void ha_hc_destroy(ha_hc_t *h);

/* 周期采样写入（OAM 1s tick）；返回判定状态（出参可空） */
ha_hc_state_t ha_hc_sample_local(ha_hc_t *h, const ha_hc_sample_t *s);
ha_hc_state_t ha_hc_local(ha_hc_t *h);

/* 对端探测结果（心跳/BFD 回调）：连续 fail_n 次判对端不可见 */
void ha_hc_peer_probe(ha_hc_t *h, int ok, int rtt_ms);
int  ha_hc_peer_visible(ha_hc_t *h);
int  ha_hc_peer_rtt_ms(ha_hc_t *h);

/* 仲裁建议：
 *  备端在"对端不可见 && 本机 HEALTHY"时建议 TAKE_OVER；
 *  本机 UNHEALTHY 时建议 RELEASE_VIP（让 LB/keepalived 摘流）；
 *  DEGRADED 不主动抢占（已有主则维持，自己持主则继续服务）。 */
typedef enum {
    HA_HC_HOLD = 0,
    HA_HC_TAKE_OVER,
    HA_HC_RELEASE_VIP
} ha_hc_action_t;
ha_hc_action_t ha_hc_recommend(ha_hc_t *h, int i_am_active);

const char *ha_hc_state_name(ha_hc_state_t st);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_HA_HEALTH_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
