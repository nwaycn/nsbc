/* STUN 报文解析/构造——粗骨架（RFC 5389，REQ-NAT-001）
 *
 * 接线批补：stun_build_binding_resp（含 XOR-MAPPED-ADDRESS）、
 * LONG-TREM 认证（user/realm/nonce/MI）与 FINGERPRINT 末位重写。
 */
#include "stun_msg.h"

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

int stun_parse(const uint8_t *buf, size_t len, stun_msg_t *m)
{
    if (buf == NULL || m == NULL || len < STUN_HDR_LEN)
        return NSBC_INVAL;
    memset(m, 0, sizeof(*m));
    m->type = rd16(buf);
    m->msg_len = rd16(buf + 2);
    m->cookie = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                ((uint32_t)buf[6] << 8) | buf[7];
    memcpy(m->tid, buf + 8, STUN_TID_LEN);
    /* 严格按 RFC5389 magic cookie 判定；RFC3489 旧客户端兼容（无 cookie）
     * 由 nat_ice 层在 agent 配置允许时回退处理。 */
    if (m->cookie != STUN_COOKIE)
        return NSBC_PROTO;
    if ((size_t)m->msg_len + STUN_HDR_LEN > len)
        return NSBC_PROTO; /* 截断（RFC5389：忽略多余尾部） */
    m->attrs = buf + STUN_HDR_LEN;
    m->attrs_len = m->msg_len;
    return NSBC_OK;
}

int stun_method(const stun_msg_t *m)
{
    /* 方法位 0..3 / 5..7 / 9..11 折叠（RFC5389 §6） */
    if (m == NULL)
        return -1;
    return (int)((m->type & 0x000fu) | ((m->type & 0x00e0u) >> 1) |
                 ((m->type & 0x0e00u) >> 2));
}

int stun_class(const stun_msg_t *m)
{
    /* 类位 C0=bit4，C1=bit8（RFC5389 §6） */
    if (m == NULL)
        return -1;
    return (int)(((m->type >> 4) & 0x1u) | (((m->type >> 8) & 0x1u) << 1));
}

int stun_attr_first(const stun_msg_t *m, stun_attr_t *a)
{
    if (m == NULL || a == NULL || m->attrs_len < STUN_ATTR_HDR_LEN)
        return 0;
    a->type = rd16(m->attrs);
    a->len = rd16(m->attrs + 2);
    a->v = m->attrs + STUN_ATTR_HDR_LEN;
    return 1;
}

int stun_attr_next(const stun_msg_t *m, stun_attr_t *a)
{
    size_t step, used;
    const uint8_t *next;
    if (m == NULL || a == NULL || a->v == NULL)
        return 0;
    step = STUN_ATTR_HDR_LEN + a->len;
    step = (step + 3u) & ~(size_t)3; /* 属性 4B 对齐填充 */
    used = (size_t)(a->v - m->attrs) - STUN_ATTR_HDR_LEN + step;
    if (used + STUN_ATTR_HDR_LEN > m->attrs_len)
        return 0;
    next = m->attrs + used;
    a->type = rd16(next);
    a->len = rd16(next + 2);
    a->v = next + STUN_ATTR_HDR_LEN;
    return 1;
}

int stun_attr_find(const stun_msg_t *m, uint16_t type, stun_attr_t *out)
{
    stun_attr_t a;
    if (out == NULL || !stun_attr_first(m, &a))
        return 0;
    do {
        if (a.type == type) {
            *out = a;
            return 1;
        }
    } while (stun_attr_next(m, &a));
    return 0;
}

int stun_xor_addr(const stun_attr_t *a, int *family, uint8_t *addr, size_t addr_cap, uint16_t *port)
{
    if (a == NULL || a->v == NULL || a->len < 8 || addr == NULL)
        return NSBC_INVAL;
    /* TODO：family(0x01 v4/0x02 v6)，port XOR cookie 高16，
     * addr XOR magic+TID；当前仅校验不取值。 */
    (void)family;
    (void)addr_cap;
    (void)port;
    return NSBC_NOTSUP;
}

int stun_build_binding_req(uint8_t *buf, size_t cap,
                           const uint8_t tid[STUN_TID_LEN],
                           size_t *out_len)
{
    if (buf == NULL || tid == NULL || out_len == NULL ||
        cap < STUN_HDR_LEN)
        return NSBC_INVAL;
    memset(buf, 0, STUN_HDR_LEN);
    buf[0] = 0x00;
    buf[1] = 0x01; /* Binding Request */
    /* len=0 */
    buf[4] = 0x21; buf[5] = 0x12; buf[6] = 0xa4; buf[7] = 0x42;
    memcpy(buf + 8, tid, STUN_TID_LEN);
    *out_len = STUN_HDR_LEN;
    return NSBC_OK;
}

int stun_append_auth(uint8_t *buf, size_t cap, size_t *len_io, const char *user, const char *realm, const uint8_t *key, size_t key_len)
{
    if (buf == NULL || len_io == NULL || key == NULL)
        return NSBC_INVAL;
    (void)cap;
    (void)user;
    (void)realm;
    (void)key_len;
    /* TODO（完整性批）：USERNAME/REALM/NONCE + MI HMAC-SHA1（摘要在
     * FINGERPRINT 之前，长度字段随之回填），末尾 FINGERPRINT CRC32。 */
    return NSBC_NOTSUP;
}
