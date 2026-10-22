/* RFC 5285 RTP 头扩展逐项迭代：
 * profile 0xBEDE → one-byte header（id 4bit，len 4bit = 数据长度-1）
 * profile 0x100? 实际 0x100 为 two-byte header（id 8bit，len 8bit）
 * id=0 为填充（one-byte），id=15 为终止；解析全程不越过扩展数据边界 */

#include "rtp.h"

void rtp_ext_iter_init(rtp_ext_iter_t *it, const rtp_pkt_t *pkt)
{
    it->p = it->end = NULL;
    it->two_byte = 0;
    if (!pkt->extension || !pkt->ext_data)
        return;
    if (pkt->ext_profile == 0xbede) {
        it->two_byte = 0;
    } else if (pkt->ext_profile == 0x100) {
        it->two_byte = 1;
    } else {
        return; /* 非 RFC 5285 profile，交由上层按自定义扩展处理 */
    }
    it->p = pkt->ext_data;
    it->end = pkt->ext_data + (size_t)pkt->ext_words * 4;
}

int rtp_ext_next(rtp_ext_iter_t *it, rtp_ext_elem_t *e)
{
    if (!it->p)
        return 0;

    if (!it->two_byte) {
        while (it->p < it->end && *it->p == 0)
            ++it->p; /* padding */
        if (it->p >= it->end)
            return 0;
        uint8_t id = (*it->p >> 4) & 0xf;
        uint8_t n = (*it->p & 0xf) + 1;
        ++it->p;
        if (id == 15)
            return 0; /* terminator */
        if (it->p + n > it->end)
            return 0; /* 畸形：元素越界 */
        e->id = id;
        e->data = it->p;
        e->len = n;
        it->p += n;
        return 1;
    }

    while (it->p < it->end && *it->p == 0)
        ++it->p; /* padding */
    if (it->p + 2 > it->end)
        return 0;
    uint8_t id = it->p[0];
    uint8_t n = it->p[1];
    it->p += 2;
    if (id == 0)
        return 0;
    if (it->p + n > it->end)
        return 0;
    e->id = id;
    e->data = it->p;
    e->len = n;
    it->p += n;
    return 1;
}