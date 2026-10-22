/* DTMF——粗骨架（REQ-DTMF-001，P0）
 *
 * 接线点：media_bridge RTP 快路径在 rtp_classify/PT=telephone-event
 * 时旁路桥到 dtmf_rx（不混入音频转发）；b2bua INFO 分支（当前 501）
 * 经 core/signaling 回调槽调 dtmf_rx_on_info；IVR/DigitMap 收号消费
 * dtmf 事件（mod 层，经组合根事件槽，不反向 include）。
 */
#include "dtmf.h"

struct dtmf_rx {
    int           pt;
    char          cur;
    uint16_t      cur_dur;
    int           armed;
    dtmf_event_t  last;
};

struct dtmf_tx {
    int      pt;
    char     digit;
    int      volume;
    uint16_t total;
    int      sends_left;
    int      in_end;
};

int dtmf_rx_create(int expect_payload_type, dtmf_rx_t **out)
{
    dtmf_rx_t *r;
    if (out == NULL)
        return NSBC_INVAL;
    r = (dtmf_rx_t *)calloc(1, sizeof(*r));
    if (r == NULL)
        return NSBC_NOMEM;
    r->pt = expect_payload_type;
    *out = r;
    return NSBC_OK;
}

void dtmf_rx_destroy(dtmf_rx_t *r)
{
    free(r);
}

int dtmf_rx_on_rfc4733(dtmf_rx_t *r, const rtp_dtmf_t *d, dtmf_event_t *out)
{
    char digit;
    if (r == NULL || d == NULL || out == NULL)
        return NSBC_INVAL;
    digit = (d->event <= 9) ? (char)('0' + d->event)
          : (d->event == 10) ? '*'
          : (d->event == 11) ? '#'
          /* 前三支已排除 0..11，此处即 event>=12，只需封顶 15(A..D) */
          : (d->event <= 15) ? (char)('A' + d->event - 12)
          : '\0';
    if (digit == '\0' || !dtmf_digit_valid(digit, 1))
        return NSBC_PROTO;
    /* TODO(P0)：同号 duration 单调去重重传；换号未 end 丢弃/记异常；
     * end=1 且 duration≥40ms 出完整事件。粗骨架仅回填。 */
    memset(out, 0, sizeof(*out));
    out->digit = digit;
    out->volume = d->volume;
    out->duration_ms = d->duration;
    out->end = d->end;
    out->src = DTMF_SRC_RFC4733;
    (void)r;
    return d->end ? 1 : 0;
}

int dtmf_rx_on_pcm(dtmf_rx_t *r, const int16_t *pcm, size_t samples, int rate, dtmf_event_t *out)
{
    if (r == NULL || pcm == NULL || out == NULL)
        return NSBC_INVAL;
    (void)samples;
    (void)rate;
    /* TODO(P0)：dsp_dtmf_scan（Goertzel 双频+20ms 持续确认），
     * 与 RFC4733 路径共用去重状态机，src 标 INBAND。 */
    return NSBC_NOTSUP;
}

int dtmf_rx_on_info(dtmf_rx_t *r, char digit, uint16_t duration_ms, dtmf_event_t *out)
{
    if (r == NULL || out == NULL)
        return NSBC_INVAL;
    if (!dtmf_digit_valid(digit, 0))
        return NSBC_PROTO;
    memset(out, 0, sizeof(*out));
    out->digit = digit;
    out->duration_ms = duration_ms;
    out->end = 1; /* INFO 语义为已完成按键 */
    out->src = DTMF_SRC_INFO;
    /* TODO(P0)：INFO 与 4733 同源防双发（同呼叫二选一锁定 30s）。 */
    return 1;
}

int dtmf_tx_create(int payload_type, dtmf_tx_t **out)
{
    dtmf_tx_t *t;
    if (out == NULL)
        return NSBC_INVAL;
    t = (dtmf_tx_t *)calloc(1, sizeof(*t));
    if (t == NULL)
        return NSBC_NOMEM;
    t->pt = payload_type;
    *out = t;
    return NSBC_OK;
}

void dtmf_tx_destroy(dtmf_tx_t *t)
{
    free(t);
}

int dtmf_tx_press(dtmf_tx_t *t, char digit, int volume, uint16_t dur_ms)
{
    if (t == NULL || !dtmf_digit_valid(digit, 1))
        return NSBC_INVAL;
    if (t->digit != '\0')
        return NSBC_AGAIN; /* 上一按键序列未发完 */
    t->digit = digit;
    t->volume = volume < 0 ? 10 : (volume > 63 ? 63 : volume);
    t->total = dur_ms > 0 ? dur_ms : 180;
    t->sends_left = 1;
    t->in_end = 0;
    return NSBC_OK;
}

int dtmf_tx_next(dtmf_tx_t *t, uint32_t rtp_ts_step, uint8_t *payload, size_t cap, size_t *len, uint32_t *rtp_ts, int *marker)
{
    if (t == NULL || payload == NULL || len == NULL)
        return NSBC_INVAL;
    (void)rtp_ts_step;
    (void)cap;
    (void)rtp_ts;
    (void)marker;
    *len = 0;
    if (t->digit == '\0')
        return 0;
    /* TODO(P0)：4B telephone-event（event/volume/E/duration）打包，
     * duration 随 ptime 累加，到 total 置 E，end 包重发 3 次。 */
    return NSBC_NOTSUP;
}

int dtmf_digit_valid(char d, int allow_letter)
{
    if ((d >= '0' && d <= '9') || d == '*' || d == '#')
        return 1;
    if (allow_letter && d >= 'A' && d <= 'D')
        return 1;
    return 0;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
