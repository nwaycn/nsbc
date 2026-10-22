#ifndef NSBC_SIPREC_H
#define NSBC_SIPREC_H

/* SIPREC 录音会话（REQ-REC-005/006，P2）
 *
 * RFC 7865（SIP 协议机制）+ RFC 7866（metadata XML）：以 B2BUA 身份
 * 向录音服务器发 INVITE（multipart/related：application/sdp +
 * application/rs-metadata+xml），媒体经 RTP 分叉复制送出；
 * 暂停/恢复/停止用 re-INVITE 更新 metadata state；
 * 参与者（主被叫/中继）变更发增量 metadata（<participant> 增删）。
 *
 * 分层：media（RTP 分叉在媒体快路径）；SIP 信令不直接发——
 * 经 core 组合根回调（b2bua originate 能力），本模块只产 body/策略。
 * XML 转义防注入（xml_parse 同源规则），禁外部实体。
 */
#include "nsbc_common.h"
#include "rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIPREC_URI_MAX      192
#define SIPREC_NAME_MAX     128
#define SIPREC_META_MAX     4096

typedef enum {
    SRC_ST_IDLE = 0,
    SRC_ST_INVITING,   /* INVITE 已发等 200 */
    SRC_ST_ACTIVE,
    SRC_ST_PAUSED,
    SRC_ST_STOPPED,
    SRC_ST_FAILED
} src_state_t;

typedef struct {
    char aor[SIPREC_URI_MAX];    /* 参与者 AOR */
    char display[SIPREC_NAME_MAX];
    int  role_caller;            /* 1 主叫 0 被叫 */
    int  muted;
} src_participant_t;

typedef struct {
    char        recorder_uri[SIPREC_URI_MAX]; /* sip:rs@recorder */
    char        session_id[64];               /* UUID */
    const char *org;                          /* 组织/租户标识 */
    int         fork_mixed;                   /* 1=混音一路 0=双腿两路 */
    int         send_rtcp;
} src_cfg_t;

typedef struct siprec_sess siprec_sess_t;

int  siprec_create(const src_cfg_t *cfg, siprec_sess_t **out);
void siprec_destroy(siprec_sess_t *s);

/* 生成初始 metadata（start 事件 + 参与者列表）；有界 XML 写入 */
int  siprec_render_start(siprec_sess_t *s, const src_participant_t *parts,
                         int nparts, char *out, size_t cap, size_t *out_len);
/* 增量/状态 metadata：state=pause/resume/stop 或参与者变更 */
int  siprec_render_update(siprec_sess_t *s, const char *state,
                          const src_participant_t *changed, int nchanged,
                          char *out, size_t cap, size_t *out_len);

/* 媒体分叉：对每路转发 RTP 复制一份发往录音服务器目标（零阻塞，
 * 经独立 SPSC + 发送 worker；录音服务器不可达不影响原通话）。 */
int  siprec_attach_rtp(siprec_sess_t *s, int leg, void *rtp_src_key);
void siprec_detach_rtp(siprec_sess_t *s);

/* 状态机迁移由组合根在 SIP 应答到达时回填（200→ACTIVE…） */
int  siprec_on_sip_state(siprec_sess_t *s, int sip_status,
                         src_state_t want);
src_state_t siprec_state(const siprec_sess_t *s);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_SIPREC_H */
