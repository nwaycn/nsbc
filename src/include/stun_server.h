/* NSBC STUN Server — RFC 5389  Binding Request/Response
 *
 * 最小实现（Phase 1）：
 *   - 仅 UDP 端口监听（默认 3478）
 *   - 仅支持 Binding Request → Binding Success Response
 *   - 返回 XOR-MAPPED-ADDRESS（RFC 5389 推荐）+ MAPPED-ADDRESS（legacy）
 *   - FINGERPRINT 自动处理（请求带就回带）
 *   - SOFTWARE (NSBC/<ver>)
 *
 * 复用 stun_msg.h：stun_parse, stun_attr_find, stun_xor_addr 等
 *
 * 线程模型：pthread recvfrom loop，每包独立处理（无状态）
 */
#ifndef NSBC_STUN_SERVER_H
#define NSBC_STUN_SERVER_H

#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

int  stun_server_init(uint16_t port, int enabled);
int  stun_server_start(void);
void stun_server_stop(void);
void stun_server_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_STUN_SERVER_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
