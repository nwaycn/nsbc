/* NSBC UDS 统一帧协议 — C 侧声明（REQ-DATA-SVC-002 P0）
 *
 * 帧格式（与 web/src/uds_server.rs 完全对齐）：
 *   偏移  长度  字段
 *     0     1    magic         固定 0x01
 *     1     1    frame_type    0x1=HELLO  0x2=HELLO_ACK  0x3=CMD_REQ  0x4=CMD_RSP
 *                              0x5=PUSH    0x6=ACK         0x7=HB       0xF=ERR
 *     2     4    total_len     大端 NBO，整帧字节数（含 12B 固定头）
 *     6     4    ticket        大端 NBO，REQ→RSP 配对；PUSH/HELLO/HB 用 0
 *    10     1    flags         bit0=more_fragment  bit1=compressed  bit2-7=0
 *    11     1    rsvd          置 0
 *    12     N    payload       NBO JSON
 *
 * 固定头 = 12 字节。payload_len = total_len - 12。
 *   PUSH payload:  {"kind":"cdr","payload":{...}}
 *                  {"kind":"register","payload":{...}}
 *                  {"kind":"runtime","payload":{...}}
 *                  {"kind":"audit","payload":{...}}
 *   HELLO payload: {"version":1,"hostname":"..","pid":1234}
 *   HB:            {}
 */
#ifndef NSBC_UDS_CLIENT_H
#define NSBC_UDS_CLIENT_H

#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 协议常量 ---- */

#define NSBC_FRAME_MAGIC       0x01
#define NSBC_FRAME_HDR_LEN     12      /* magic(1)+type(1)+total_len(4)+ticket(4)+flags(1)+rsvd(1) */
#define NSBC_MAX_PAYLOAD       (1024 * 1024)   /* 1MB */
#define NSBC_MAX_FRAME         (NSBC_FRAME_HDR_LEN + NSBC_MAX_PAYLOAD)

/* frame_type (1 字节) */
#define FT_HELLO          0x01
#define FT_HELLO_ACK      0x02
#define FT_CMD_REQ        0x03
#define FT_CMD_RSP        0x04
#define FT_DATA_PUSH      0x05    /* C→Web 事件推送（当前 data.sock 主用） */
#define FT_ACK            0x06
#define FT_HEARTBEAT      0x07
#define FT_ERROR          0x0F

/* 批量聚合配置 */
#define UDS_BATCH_MAX        100
#define UDS_BATCH_TIMEOUT_MS 2000
#define UDS_RECONNECT_BASE_MS 500
#define UDS_RECONNECT_MAX_MS 30000

/* ---- 生命周期 ---- */

int  uds_client_init(const char *sock_path);
int  uds_client_start(void);
void uds_client_stop(void);
void uds_client_shutdown(void);

/* ---- 推送 API ---- */
int uds_send_cdr(const char *json);
int uds_send_register(const char *json);
int uds_send_runtime(const char *json);
int uds_send_audit(const char *json);
int uds_send_session(const char *json);
int uds_send_vad(const char *json);
int uds_flush_now(void);

/* ---- 同步命令请求（Web DB 实时查询，S2）----
 * 发 FT_CMD_REQ + ticket，阻塞等待匹配 ticket 的 FT_CMD_RSP。
 *   cmd_json:  {"cmd":"ext.ha1","user":"1001","realm":"nsbc.local"}
 *   resp:      RSP payload（JSON 文本，NUL 结尾），如 {"ok":true,"ha1":".."}
 * 返回 NSBC_OK=收到 RSP；NSBC_ERR=未连接/发送失败/超时/并发槽满。
 * 线程安全；并发上限 UDS_REQ_SLOTS（16），超限立即失败不排队。
 * 注意：不得在持有 endpoints 分片锁等高频锁的路径里长阻塞调用。 */
int uds_request(const char *cmd_json, int timeout_ms,
                char *resp, size_t resp_cap);

/* REQ 并发等待槽数 */
#define UDS_REQ_SLOTS       16
#define UDS_REQ_TIMEOUT_MS  2000    /* uds_request 允许的上限超时 */

/* ---- 帧编码 ----
 * frame_type: FT_DATA_PUSH / FT_HELLO / ...
 * ticket:     REQ→RSP 配对 ID；PUSH 用 0
 * payload:    JSON 文本（不含头部）
 * 返回: 实际编码字节数；失败 0
 */
size_t uds_encode_frame(uint8_t frame_type, uint32_t ticket,
                        const char *payload, size_t payload_len,
                        uint8_t *out, size_t out_sz);

static inline const char *uds_default_sock_path(void)
{
    return "/var/run/nsbc/data.sock";
}

#ifdef __cplusplus
}
#endif
#endif /* NSBC_UDS_CLIENT_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
