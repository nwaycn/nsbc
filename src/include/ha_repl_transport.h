#ifndef NSBC_HA_REPL_TRANSPORT_H
#define NSBC_HA_REPL_TRANSPORT_H

/* HA 状态复制 TCP 传输层（P1，REQ-HA-002 / REQ-HA-004）
 *
 * 职责：
 *  - 主端（ACTIVE）监听 TCP 5071，接受备端连接；
 *  - 备端（STANDBY）发起 connect，指数退避重试；
 *  - 帧格式 HaReplV1：16 字节固定头 + payload（repl_rec_t 序列化）；
 *  - 与 ha_replica 集成：transport 自己创建 ha_repl_t，填入 ops.send
 *    （业务线程 repl_publish → ops.send → transport 内部 SPSC 队列 →
 *    transport 线程 drain → TCP send），ops.apply 从 recv 线程调用
 *    repl_ingest，ops.on_gap 预留。
 *
 * 线程模型：独立 pthread（与 UDP 心跳线程分离），防 TCP 拥塞影响
 * 仲裁可用性。socket 非阻塞 + 短周期 poll，业务线程零阻塞。 */

#include "nsbc_common.h"
#include "ha_replica.h"
#include "ha.h"         /* ha_role_t */
#include "net_sock.h"    /* nsbc_sa_t */

#ifdef __cplusplus
extern "C" {
#endif

/* HaReplV1 帧头 16 字节（网络字节序） */
#define HA_REPL_FRAME_MAGIC     0x4852u  /* "HR" */
#define HA_REPL_FRAME_VER       1u
#define HA_REPL_FRAME_ACK_TYPE  0xFFu   /* 备端 ACK 帧 type (payload_len=applied_seq) */
#define HA_REPL_FRAME_GAP_REQ_TYPE 0xFEu  /* 备端请求主端重发缺口 */
#define HA_REPL_FRAME_FULL_REQ_TYPE 0xFDu /* 备端请求主端推全量快照 */

/* GAP_REQ payload：两个 uint64_t (gap_start 期望 seq, gap_end 收到的 seq) */
typedef struct {
    uint64_t gap_start;   /* 期望但缺失的起始 seq */
    uint64_t gap_end;     /* 实际收到的 seq（中间 gap_start..gap_end-1 缺失） */
} ha_repl_gap_req_t;

typedef struct {
    uint16_t magic;
    uint16_t ver;
    uint64_t seq;
    uint8_t  type;
    uint8_t  reserved;
    uint16_t payload_ver;
    uint32_t payload_len;
} ha_repl_frame_hdr_t;

/* transport 配置（从 cfg_get_* 读取） */
typedef struct {
    int      enabled;
    uint16_t port;          /* 默认 5071 */
    uint32_t ring_cap;      /* 默认 8192 */
    uint32_t payload_max;    /* 默认 4096 */
    uint64_t lag_warn;      /* 默认 10000（写入 ha_hc_cfg_t） */
    uint32_t full_resync_interval; /* 秒，0=关闭 */
    int      is_active;     /* ha_role() == ACTIVE 时为 1 */
    int      have_peer;
    nsbc_sa_t peer_addr;
} ha_repl_trans_cfg_t;

typedef struct ha_repl_trans ha_repl_trans_t;

int  ha_repl_trans_create(const ha_repl_trans_cfg_t *cfg,
                          ha_repl_trans_t **out);
void ha_repl_trans_destroy(ha_repl_trans_t *t);

/* 暴露 ha_repl_t 给 registrar/b2bua 业务层调用 repl_publish()
 * 返回 NULL 表示 transport 未就绪（STANDBY / 传输层 disabled / 降级）。 */
ha_repl_t *ha_repl_trans_repl(ha_repl_trans_t *t);

/* 业务层覆盖 ops.apply / ops.on_gap
 * transport 创建时 ops.apply/on_gap 是占位（return NSBC_OK），
 * registrar/b2bua 应在 init 时调此函数把自己的 apply（调 loc_upsert / dialog 重建）注入。 */
void ha_repl_trans_set_apply_ops(ha_repl_trans_t *t,
                                 int (*apply)(void *ud, const repl_rec_t *r),
                                 int (*on_gap)(void *ud, uint64_t expect, uint64_t got),
                                 void *ud);

/* 连接状态供 ha_health 采样 —— connected=1 表示 TCP 握手完成 */
int  ha_repl_trans_connected(ha_repl_trans_t *t);

/* 角色变更通知 —— ha.c 心跳仲裁切换角色时调；transport 据此决定
 * 是 accept 模式（ACTIVE listen）还是 connect 模式（STANDBY 重连）。 */
void ha_repl_trans_notify_role(ha_repl_trans_t *t, ha_role_t role);

/* ACK 水位（OAM 只读） */
uint64_t ha_repl_trans_last_ack(ha_repl_trans_t *t);

/* Hitless Upgrade：ACTIVE 主动触发 FULL push（不等 STANDBY 发 FULL_REQ）
 * 用于 ACTIVE 要让位前把完整状态快照先推给 STANDBY，确保切换零丢状态。
 * 非 ACTIVE 或 transport 未就绪 → 返回 NSBC_NOENT。
 * 内部异步执行（transport 线程），本函数立即返回。 */
int ha_repl_trans_force_full_push(ha_repl_trans_t *t);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_HA_REPL_TRANSPORT_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
