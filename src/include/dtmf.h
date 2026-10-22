#ifndef NSBC_DTMF_H
#define NSBC_DTMF_H

/* DTMF 统一入口（REQ-DTMF-001，P0）
 *
 * 三来源归一：
 *  1) RFC 4733 telephone-event RTP 载荷（rtp_telephone 已有严格解析，
 *     但零调用）：按 event/volume/duration/end 组包，重传 3 次，
 *     duration 增量；本模块做收端去重、抖动与发端状态机；
 *  2) INBAND 模拟双频（G.711 透传场景）：Goertzel 检测（media_dsp）；
 *  3) SIP INFO（application/dtmf-relay / h245-signal）：方法族当前 501，
 *     经组合根从 sip 层桥入（sip 不反向依赖 media，回调槽装配）。
 * 统一输出 digit 事件给 IVR/会议/拨号（DigitMap 收号）。
 */
#include "nsbc_common.h"
#include "rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DTMF_DIGIT_MAX   1   /* 0-9 * # A-D 单字符 */
#define DTMF_MIN_MS      40  /* RFC4733 最短有效持续 */
#define DTMF_END_GAP_MS  20  /* end=1 后同号再确认间隔 */

typedef enum {
    DTMF_SRC_RFC4733 = 0,
    DTMF_SRC_INBAND,
    DTMF_SRC_INFO
} dtmf_src_t;

typedef struct {
    char    digit;     /* '0'-'9' '*' '#' 'A'-'D'；0=无 */
    int     volume;    /* RFC4733 0..63 dBov；INBAND 为能量估算 */
    uint16_t duration_ms;
    int     end;       /* 键抬起（duration 终值） */
    dtmf_src_t src;
} dtmf_event_t;

typedef struct dtmf_rx dtmf_rx_t;
typedef struct dtmf_tx dtmf_tx_t;

/* 收端：去重/重传合并/最小持续确认；出键时机为 end=1（或达长按时长） */
int  dtmf_rx_create(int expect_payload_type, dtmf_rx_t **out);
void dtmf_rx_destroy(dtmf_rx_t *r);
/* RFC4733 包喂入（payload 已去 RTP 头）：返回 1=产出一个完整事件 */
int  dtmf_rx_on_rfc4733(dtmf_rx_t *r, const rtp_dtmf_t *d, dtmf_event_t *out);
/* INBAND PCM 喂入（透传/转码后 G.711 PCM），内部调 DSP 双频检测 */
int  dtmf_rx_on_pcm(dtmf_rx_t *r, const int16_t *pcm, size_t samples,
                    int rate, dtmf_event_t *out);
/* INFO 体直接事件（已鉴权/已限幅，校验持续字段） */
int  dtmf_rx_on_info(dtmf_rx_t *r, char digit, uint16_t duration_ms,
                     dtmf_event_t *out);

/* 发端：一次按键生成 RFC4733 包序列（首包+更新+end，重传统计），
 * 调用方按 ptime 节拍调 dtmf_tx_next 取待发载荷；透传模式发 INBAND
 * 的场景由调用方关闭 tx 并改注音频（NOTSUP 标记 P1）。 */
int  dtmf_tx_create(int payload_type, dtmf_tx_t **out);
void dtmf_tx_destroy(dtmf_tx_t *t);
int  dtmf_tx_press(dtmf_tx_t *t, char digit, int volume, uint16_t dur_ms);
/* RTP 节拍：返回 1=本次需发（payload/len/rtp_ts 增量由调用方封 RTP），
 * 0=空闲；end 包按 RFC4733 重发 3 次。 */
int  dtmf_tx_next(dtmf_tx_t *t, uint32_t rtp_ts_step,
                  uint8_t *payload, size_t cap, size_t *len,
                  uint32_t *rtp_ts, int *marker);

/* 单音有效性（DigitMap 前的粗过滤，A-D 可选支持） */
int dtmf_digit_valid(char d, int allow_letter);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_DTMF_H */
