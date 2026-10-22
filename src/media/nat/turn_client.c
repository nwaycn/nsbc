/* TURN 客户端——粗骨架（RFC 5766，REQ-NAT-001）
 *
 * 实现注意（接线批）：
 *  - 首个 Allocate 不带凭证，401 取 REALM/NONCE 后用 HMAC-SHA1 重发；
 *  - 五元素二元组，超时/重传统一走时间轮（RFC5389 RTO 500ms 倍增）；
 *  - ChannelData 首 2 位为 01（0x4000+），与 STUN(00)/RTP 区分；
 *  - 权限 5 分钟、通道绑定 10 分钟（Refresh 刷新全部）。
 */
#include "turn_client.h"
#include "stun_msg.h"

struct turn_sess {
    turn_cfg_t  cfg;
    turn_state_t state;
    uint16_t    next_channel;
    uint64_t    alloc_ms;
};

int turn_allocate(turn_sess_t **out, const turn_cfg_t *cfg)
{
    turn_sess_t *t;
    if (out == NULL || cfg == NULL || cfg->key == NULL)
        return NSBC_INVAL;
    t = (turn_sess_t *)calloc(1, sizeof(*t));
    if (t == NULL)
        return NSBC_NOMEM;
    t->cfg = *cfg;
    t->state = TURN_ST_ALLOCATING;
    t->next_channel = TURN_CHANNEL_MIN;
    *out = t;
    /* TODO：构造 Allocate 请求帧（REQUESTED-TRANSPORT UDP=17），
     * 无凭证先发，等 401。 */
    return NSBC_NOTSUP;
}

void turn_close(turn_sess_t *t)
{
    /* TODO：发 Refresh LIFETIME=0 后释放（不等应答） */
    free(t);
}

int turn_on_packet(turn_sess_t *t, const uint8_t *buf, size_t len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    stun_msg_t m;
    if (t == NULL || buf == NULL)
        return NSBC_INVAL;
    (void)out;
    (void)out_cap;
    if (out_len != NULL)
        *out_len = 0;
    if (stun_parse(buf, len, &m) != NSBC_OK)
        return NSBC_PROTO;
    /* TODO：按 method/class 状态机推进（成功取 XOR-RELAYED-ADDRESS、
     * LIFETIME；错误码 401 补认证重发，437 不匹配等）。 */
    return NSBC_NOTSUP;
}

int turn_due_refresh(turn_sess_t *t, uint64_t now_ms, uint8_t *out, size_t cap, size_t *out_len, int *need)
{
    if (t == NULL || need == NULL)
        return NSBC_INVAL;
    *need = 0;
    (void)now_ms;
    (void)out;
    (void)cap;
    if (out_len != NULL)
        *out_len = 0;
    /* TODO：alloc_ms+lifetime-60s 判定，need=1 时渲染 Refresh 帧 */
    return NSBC_OK;
}

int turn_permission_add(turn_sess_t *t, const nsbc_sa_t *peer, uint16_t *channel_out, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (t == NULL || peer == NULL || buf == NULL || out_len == NULL)
        return NSBC_INVAL;
    (void)cap;
    if (t->next_channel > TURN_CHANNEL_MAX)
        return NSBC_NOSPC;
    /* TODO：CreatePermission(XOR-PEER-ADDRESS) + ChannelBind 两帧 */
    if (channel_out != NULL)
        *channel_out = t->next_channel;
    ++t->next_channel;
    return NSBC_NOTSUP;
}

int turn_wrap_channel(turn_sess_t *t, uint16_t channel, const uint8_t *p, size_t plen, uint8_t *out, size_t cap, size_t *out_len)
{
    if (t == NULL || p == NULL || out == NULL || out_len == NULL)
        return NSBC_INVAL;
    if (cap < 4 + plen)
        return NSBC_NOSPC;
    /* TODO：0x4000|channel，长度按网络序写入后拷载荷 */
    (void)channel;
    return NSBC_NOTSUP;
}

int turn_unwrap_data(turn_sess_t *t, const uint8_t *buf, size_t len, nsbc_sa_t *peer_out, const uint8_t **payload, size_t *plen)
{
    /* TODO：ChannelData（0x40..）直解；Data Indication 取
     * XOR-PEER-ADDRESS+DATA。 */
    if (t == NULL || buf == NULL || payload == NULL || plen == NULL)
        return NSBC_INVAL;
    (void)len;
    (void)peer_out;
    return NSBC_NOTSUP;
}

turn_state_t turn_state(const turn_sess_t *t)
{
    return t == NULL ? TURN_ST_IDLE : t->state;
}

int turn_relay_addr(const turn_sess_t *t, nsbc_sa_t *out)
{
    if (t == NULL || out == NULL)
        return NSBC_INVAL;
    /* TODO：Allocate 成功响应的 XOR-RELAYED-ADDRESS 回填 */
    return NSBC_NOTSUP;
}
