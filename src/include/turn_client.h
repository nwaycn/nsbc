#ifndef NSBC_TURN_CLIENT_H
#define NSBC_TURN_CLIENT_H

/* TURN 客户端（RFC 5766；RELAY 候选，REQ-NAT-001 组成）
 *
 * Allocate（long-term 凭证，RFC5389 §15.3）→ 周期 Refresh →
 * CreatePermission（对端 IP）→ ChannelBind（0x4000..0x7FFF）→
 * 媒体经 ChannelData（4B 头）或 Send/Data Indication 收发。
 *
 * 分层：放 media 层（不是 net）：需组合 net socket（net 层）与
 * STUN 报文解析（parse 层），net/parse 同 rank 禁止横向 include。
 */
#include "nsbc_common.h"
#include "net_sock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURN_CHANNEL_MIN   0x4000u
#define TURN_CHANNEL_MAX   0x7fffu
#define TURN_LIFETIME_DEF  600      /* 服务端缺省 10min */
#define TURN_REFRESH_AHEAD 60       /* 提前 60s 续约 */

typedef enum {
    TURN_ST_IDLE = 0,
    TURN_ST_ALLOCATING,
    TURN_ST_ALLOCATED,
    TURN_ST_REFRESHING,
    TURN_ST_ERROR
} turn_state_t;

typedef struct turn_sess turn_sess_t;

typedef struct {
    nsbc_sa_t   server;       /* TURN 服务端（UDP/TLS 后续扩展） */
    const char *user;         /* long-term 凭证用户名 */
    const char *realm;        /* 可空：401 质询后回填 */
    const uint8_t *key;       /* SASLprep(user:realm:pass) HMAC 密钥 */
    size_t      key_len;      /* 20（SHA1）/32（SHA2 扩展） */
    int         lifetime_s;
    int         channel_data; /* 1=优先 ChannelData，0=Indication */
} turn_cfg_t;

int  turn_allocate(turn_sess_t **out, const turn_cfg_t *cfg);
void turn_close(turn_sess_t *t);

/* reactor 收到服务端回包喂入；返回需发出的请求帧（调用方 UDP 发出）。
 * 粗骨架：内部状态机在接线批实现，当前返回 NOTSUP。 */
int  turn_on_packet(turn_sess_t *t, const uint8_t *buf, size_t len,
                    uint8_t *out, size_t out_cap, size_t *out_len);

/* 续约到期检查（时间轮周期调；need=1 时 out 填 Refresh 请求帧） */
int  turn_due_refresh(turn_sess_t *t, uint64_t now_ms,
                      uint8_t *out, size_t cap, size_t *out_len, int *need);

/* 新对端：CreatePermission + ChannelBind（两帧合并窗口内发） */
int  turn_permission_add(turn_sess_t *t, const nsbc_sa_t *peer,
                         uint16_t *channel_out,
                         uint8_t *buf, size_t cap, size_t *out_len);

/* 媒体封装/解封装：RTP → ChannelData；ChannelData → 载荷+对端 */
int  turn_wrap_channel(turn_sess_t *t, uint16_t channel,
                       const uint8_t *p, size_t plen,
                       uint8_t *out, size_t cap, size_t *out_len);
int  turn_unwrap_data(turn_sess_t *t, const uint8_t *buf, size_t len,
                      nsbc_sa_t *peer_out,
                      const uint8_t **payload, size_t *plen);

turn_state_t turn_state(const turn_sess_t *t);
int  turn_relay_addr(const turn_sess_t *t, nsbc_sa_t *out);

#ifdef __cplusplus
}
#endif
#endif
