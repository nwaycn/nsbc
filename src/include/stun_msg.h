#ifndef NSBC_STUN_MSG_H
#define NSBC_STUN_MSG_H

/* STUN 报文解析/构造（RFC 5389；TURN RFC 5766 复用同一消息格式）
 * 对应 REQ-NAT-001（ICE/STUN/TURN，当前全缺，仅 rtp_classify 识别）。
 *
 * 分层：parse 层；零拷贝属性迭代，不做事务重传（属 nat_ice/turn）。
 * 报文识别已在 rtp_classify（PKT_STUN）：首 2 位为 00 且 magic cookie
 * 0x2112A442。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STUN_COOKIE         0x2112A442u
#define STUN_HDR_LEN        20
#define STUN_TID_LEN        12
#define STUN_ATTR_HDR_LEN   4

/* 方法（常用子集） */
#define STUN_M_BINDING              0x001
#define STUN_M_ALLOCATE            0x003
#define STUN_M_REFRESH             0x004
#define STUN_M_CREATE_PERMISSION   0x008
#define STUN_M_CHANNEL_BIND        0x009
#define STUN_M_SEND                0x00a
#define STUN_M_DATA                0x00b

/* 类 */
#define STUN_C_REQUEST     0x00
#define STUN_C_INDICATION  0x01
#define STUN_C_SUCCESS     0x10
#define STUN_C_ERROR       0x11

/* 属性类型（常用子集） */
typedef enum {
    STUN_A_MAPPED_ADDR = 0x0001,
    STUN_A_USERNAME = 0x0006,
    STUN_A_MESSAGE_INTEGRITY = 0x0008,
    STUN_A_ERROR_CODE = 0x0009,
    STUN_A_UNKNOWN_ATTR = 0x000a,
    STUN_A_REALM = 0x0014,
    STUN_A_NONCE = 0x0015,
    STUN_A_XOR_MAPPED_ADDR = 0x0020,
    STUN_A_PRIORITY = 0x0024,
    STUN_A_USE_CANDIDATE = 0x0025,
    STUN_A_FINGERPRINT = 0x8028,
    STUN_A_ICE_CONTROLLED = 0x8029,
    STUN_A_ICE_CONTROLLING = 0x802a,
    STUN_A_SOFTWARE = 0x8022,
    STUN_A_LIFETIME = 0x000d,
    STUN_A_XOR_PEER_ADDR = 0x0012,
    STUN_A_DATA = 0x0013,
    STUN_A_REQED_TRANSPORT = 0x0019,
    STUN_A_CHANNEL_NUMBER = 0x000c
} stun_attr_type_t;

typedef struct {
    uint16_t type;        /* method<<2 | class */
    uint16_t msg_len;     /* 头后字节数（不含 20B 头） */
    uint32_t cookie;
    uint8_t  tid[STUN_TID_LEN];
    const uint8_t *attrs; /* 首属性 */
    size_t   attrs_len;
} stun_msg_t;

typedef struct {
    uint16_t type;
    uint16_t len;
    const uint8_t *v;     /* 值（不含 4B 属性头） */
} stun_attr_t;

/* 校验 cookie/长度；返回 PROTO/OK，不做 FINGERPRINT/MI 校验 */
int  stun_parse(const uint8_t *buf, size_t len, stun_msg_t *m);
int  stun_method(const stun_msg_t *m);
int  stun_class(const stun_msg_t *m);

/* 属性迭代（按 TLV 步进，自动 4 字节对齐） */
int  stun_attr_first(const stun_msg_t *m, stun_attr_t *a);
int  stun_attr_next(const stun_msg_t *m, stun_attr_t *a);
/* 按类型查属性：找到返 1 并填 out（out 内指针随报文生命周期） */
int  stun_attr_find(const stun_msg_t *m, uint16_t type, stun_attr_t *out);

/* XOR-MAPPED-ADDRESS 解址（v4/v6，输出端口主机序与 4/16 字节地址） */
int  stun_xor_addr(const stun_attr_t *a, int *family,
                   uint8_t *addr, size_t addr_cap, uint16_t *port);

/* 构造类（有界缓冲）：Binding 请求（随机 TID 由调用方给） */
int  stun_build_binding_req(uint8_t *buf, size_t cap,
                            const uint8_t tid[STUN_TID_LEN],
                            size_t *out_len);

/* MESSAGE-INTEGRITY HMAC-SHA1 与 FINGERPRINT CRC32 在完整性批实现
 * （RFC5389 §15.4/15.5）；当前仅提供预留位置，返 NOTSUP。 */
int  stun_append_auth(uint8_t *buf, size_t cap, size_t *len_io,
                      const char *user, const char *realm,
                      const uint8_t *key, size_t key_len);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_STUN_MSG_H */
