/* hep_sink.h — HEP3 实时推流到 Homer/SIPCAPTURE（REQ-MEDIA-HEP-001）
 *
 * 作为 media_bug 的全局消费者：注册 BUG_RAW_RTP | BUG_RTCP_MIRROR |
 * BUG_SIPCAPTURE_TAP 三类回调，将帧封装为 HEP3 经 UDP 发往 Homer。
 *
 * HEP3（真实 chunks 协议）格式：
 *   magic "HEP3" (4B) + total_len (2B, htons) + chunks[]
 * 每个 chunk: vendor_id (2B, htons=0x0000) + type_id (2B, htons)
 *           + chunk_len (2B, htons) + data (chunk_len B)
 *
 * IPv4 RTP 帧需要 ~11 个 chunks，总 header 约 99-130B（可变长）。
 */
#ifndef NSBC_HEP_SINK_H
#define NSBC_HEP_SINK_H

#include <stdint.h>
#include <stddef.h>
#include "media_bug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== HEP3 协议常量 ===== */

/* Payload type（chunk type 0x000d） */
#define HEP3_PT_RTP      0x01
#define HEP3_PT_RTCP     0x02
#define HEP3_PT_SIP      0x03
#define HEP3_PT_CUSTOM   0xFF

/* IP family（chunk type 0x0001） */
#define HEP3_IPV4        1
#define HEP3_IPV6        2

/* Transport（chunk type 0x0006） */
#define HEP3_UDP         17
#define HEP3_TCP         6

/* Chunk Type IDs（vendor = 0x0000 IANA default） */
#define HEP3_CHUNK_IP_VERSION   0x0001   /* 1B: 4 or 6 */
#define HEP3_CHUNK_SRC_IP       0x0002   /* 4B IPv4 / 16B IPv6 */
#define HEP3_CHUNK_DST_IP       0x0003   /* 4B IPv4 / 16B IPv6 */
#define HEP3_CHUNK_SRC_PORT     0x0004   /* 2B htons */
#define HEP3_CHUNK_DST_PORT     0x0005   /* 2B htons */
#define HEP3_CHUNK_PROTOCOL     0x0006   /* 1B: 17=UDP, 6=TCP */
#define HEP3_CHUNK_CAPTURE_ID   0x0007   /* 4B htonl */
#define HEP3_CHUNK_TIME_SEC     0x0008   /* 8B htobe64 (seconds since epoch) */
#define HEP3_CHUNK_TIME_USEC    0x0009   /* 4B htonl (microseconds) */
#define HEP3_CHUNK_PAYLOAD_TYPE 0x000d   /* 1B */
#define HEP3_CHUNK_PAYLOAD      0x000c   /* variable */
/* 可选 chunks（P1） */
#define HEP3_CHUNK_AGENT_ID     0x000a   /* variable string */
#define HEP3_CHUNK_AUTH_KEY     0x000b   /* variable */
#define HEP3_CHUNK_NODE_ID      0x000f   /* variable */

/* Vendor ID */
#define HEP3_VENDOR_IANA        0x0000

/* Header buffer = 完整 HEP3 帧（chunks + 内联 PAYLOAD chunk）
 * MTU 限制 ≤1472B（IPv4），留冗余到 1400B */
#define HEP3_MAX_FRAME_LEN      1400

/* ===== 配置 ===== */
typedef struct {
    char    server_host[64];   /* "10.0.0.25" */
    uint16_t server_port;      /* 9060 默认 */
    uint32_t capture_id;       /* 客户侧分配 */
    int      transport;        /* UDP 仅（P0） */
    int      ring_size;        /* MPSC ring 帧数（默认 8192） */
    int      flush_batch;      /* sendmmsg 批量（默认 32） */
    int      flush_interval_ms; /* 强制刷盘（默认 5ms） */
    int      socket_pool;      /* UDP socket per-core（默认 4） */
} hep_cfg_t;

/* 帧缓冲：完整 HEP3 帧（magic + chunks + 内联 PAYLOAD） */
typedef struct {
    uint8_t  frame[HEP3_MAX_FRAME_LEN];   /* 完整 HEP3 帧 */
    size_t   frame_len;                   /* 实际帧长度 */
    uint64_t timestamp_ns;
} hep_frame_t;

/* ===== 公共 API ===== */

/* 创建：从 cfg 创建 hep_sink，内部启动 per-core worker 线程 */
int hep_sink_create(const hep_cfg_t *cfg);

/* 销毁：所有 worker 退出，ring 排空，socket 关闭 */
void hep_sink_destroy(void);

/* 命令族（nsbcd hep ...） */
int hep_cmd_status(char *out, size_t outsz);
int hep_cmd_flush(char *out, size_t outsz);
int hep_cmd_reload(char *out, size_t outsz);

/* media_bug 全局消费者回调（内部函数，media_bug 自动调用） */
void hep_sink_on_frame(media_bug_frame_t *frame, void *ud);

/* OAM 指标暴露（Prometheus C-Exporter） */
uint64_t hep_frames_sent(void);
uint64_t hep_frames_dropped(const char *reason); /* reason=ring_full/send_error */
uint64_t hep_backlog_depth(void);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_HEP_SINK_H */
