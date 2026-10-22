#ifndef NSBC_FAX_H
#define NSBC_FAX_H

/* 传真（REQ-FAX-001，P1）
 *
 * 两模式：
 *  1) G.711 透传（PCMA/PCMU + clearmode/a=fax）：检测 CNG 1100Hz 0.5s/3s
 *     后关 VAD/回声抑制、锁 codec、打 T.38 无关的透传标记，PTIME 20ms
 *     不抖动缓存（或浅缓存）；
 *  2) T.38 UDPTL 中继（IFP 包经 UDPTL，冗余/纠错 FEC/parity 可配）：
 *     INVITE a=image/t38，Re-INVITE 切换媒体，独立 UDPTL 端口对。
 * 分层：media；DTMF 不与传真混用，CNG 检测在本模块。
 */
#include "nsbc_common.h"
#include "rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FAX_MODE_OFF = 0,
    FAX_MODE_G711_PASSTHROUGH,
    FAX_MODE_T38_RELAY
} fax_mode_t;

typedef enum {
    FAX_EV_NONE = 0,
    FAX_EV_CNG_DETECTED,   /* 1100Hz 主叫单音 → 可发起 T.38 切换 */
    FAX_EV_CED_DETECTED,   /* 2100Hz 被叫应答音 */
    FAX_EV_T38_START,
    FAX_EV_T38_STOP,
    FAX_EV_PAGE_DONE,
    FAX_EV_FAILED
} fax_event_t;

typedef struct {
    fax_mode_t mode_init;       /* OFF=自动检测后切换 */
    int        t38_enabled;
    int        t38_max_rate;    /* bps：14400/9600… */
    int        udptl_ec;        /* 0 none / 1 FEC / 2 parity */
    int        udptl_redundancy;
    int        passthrough_pt;  /* 透传 PT（8/0） */
} fax_cfg_t;

typedef struct fax_sess fax_sess_t;

int  fax_create(const fax_cfg_t *cfg, fax_sess_t **out);
void fax_destroy(fax_sess_t *s);

/* G.711 透传期喂音频做 CNG/CED 检测（返回事件；NONE=无） */
fax_event_t fax_detect_tones(fax_sess_t *s, const int16_t *pcm,
                             size_t samples, int rate);

/* 模式切换（re-INVITE 成功后调用）；T38 切走 RTP 端口表另分配 UDPTL */
int  fax_switch_mode(fax_sess_t *s, fax_mode_t mode);
fax_mode_t fax_mode(const fax_sess_t *s);

/* UDPTL 收发（T.38 IFP）：封帧=seq(2)+控制+IFP，冗余按配置预填 */
int  fax_udptl_send(fax_sess_t *s, const uint8_t *ifp, size_t ifp_len,
                    uint8_t *out, size_t cap, size_t *out_len);
int  fax_udptl_recv(fax_sess_t *s, const uint8_t *buf, size_t len,
                    const uint8_t **ifp, size_t *ifp_len);

/* 透传帧打标（关闭抖动缓冲/EC 的提示位，供 bridge 快路径查询） */
int  fax_passthrough_active(const fax_sess_t *s);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_FAX_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
