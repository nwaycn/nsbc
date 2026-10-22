#ifndef NSBC_TCP_FRAMER_H
#define NSBC_TCP_FRAMER_H

/* RFC 3261 §7.4.2 / §18.3 / REQ-SIP-017 / REQ-SIP-TCP-001
 *
 * TCP 上 SIP 消息边界仅由 Content-Length 决定（禁止 Transfer-Encoding:
 * chunked）；无 Content-Length 的 TCP/TLS 消息是协议错误，必须关闭连接。
 * 本分帧器是纯逻辑（不碰 socket），TLS 解密后的数据同样喂入。
 */

#include <stddef.h>
#include <stdint.h>
#include "nsbc_common.h"

typedef enum {
    FRAME_OK = 0,        /* 取到 0 条或多条完整消息，见返回的消息数 */
    FRAME_NEED_MORE = 1, /* 暂无完整消息，等待更多字节 */
    FRAME_OVERFLOW = 2,  /* 超 max_msg（REQ-SIP-005：慢速攻击防护，断连） */
    FRAME_PROTO = 3      /* 无 CL / chunked / 头部畸形：400 并断连 */
} frame_rc_t;

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t start; /* 已消费位置 */
    size_t end;   /* 有效数据尾 */
    size_t max_msg;
} tcp_framer_t;

void tcp_framer_init(tcp_framer_t *f, uint8_t *buf, size_t cap,
                     size_t max_msg);

/* 喂入 data（可为 NULL/0 表示仅尝试抽取缓冲内已有数据）。
 * 每取到一条完整消息回调 cb(msg, len)；回调返回非 0 中止抽取。
 * 返回 NSBC_OK（已抽取≥1 条，或数据不足 NEED_MORE 经 *out_rc 给出）。
 * *out_rc：FRAME_OK / NEED_MORE / OVERFLOW / PROTO；*n_msgs：抽取条数。
 */
int tcp_framer_feed(tcp_framer_t *f, const uint8_t *data, size_t len,
                    int (*cb)(void *ud, const uint8_t *msg, size_t len),
                    void *ud, frame_rc_t *out_rc, int *n_msgs);

/* 连接复用保活：发送 CRLF（RFC 5626 §3.5 keepalive）；返回写出长度 */
#define TCP_KEEPALIVE "\r\n\r\n"

#endif /* NSBC_TCP_FRAMER_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
