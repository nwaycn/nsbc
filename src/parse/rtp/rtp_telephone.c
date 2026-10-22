/* RFC 4733 telephone-event 载荷：
 * byte0 event；byte1 bit7=E(结束) bit6=R(保留) bit0-5=volume；byte2-3 duration */

#include "rtp.h"

nsbc_rc_t rtp_parse_dtmf(const uint8_t *buf, size_t len, rtp_dtmf_t *d)
{
    memset(d, 0, sizeof(*d));
    if (len < 4)
        return NSBC_ERR;
    d->event = buf[0];
    d->end = (buf[1] >> 7) & 0x1;
    /* R 位（bit6）必须为 0：严格模式校验 */
    if ((buf[1] >> 6) & 0x1)
        return NSBC_ERR;
    d->volume = buf[1] & 0x3f;
    d->duration = rtp_rd16(buf + 2);
    return NSBC_OK;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
