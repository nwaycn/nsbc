/* SIPREC——粗骨架（REQ-REC-005/006，P2）
 *
 * 注意：RFC 7866 metadata 时间戳用 UTC（realtime.h 墙上时钟），
 * 与 RTP 单调时钟分离；XML 经安全转义（&<>"），显示名不可信字段
 * 一律转义；RTP 分叉使用独立 SSRC（REQ-MEDIA-SEC-003 出向重写）。
 */
#include "siprec.h"

struct siprec_sess {
    src_cfg_t   cfg;
    src_state_t state;
    int         nparts;
};

int siprec_create(const src_cfg_t *cfg, siprec_sess_t **out)
{
    siprec_sess_t *s;
    if (cfg == NULL || out == NULL || cfg->recorder_uri[0] == '\0')
        return NSBC_INVAL;
    s = (siprec_sess_t *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NSBC_NOMEM;
    s->cfg = *cfg;
    s->state = SRC_ST_IDLE;
    *out = s;
    return NSBC_OK;
}

void siprec_destroy(siprec_sess_t *s)
{
    if (s != NULL)
        free(s);
}

int siprec_render_start(siprec_sess_t *s, const src_participant_t *parts, int nparts, char *out, size_t cap, size_t *out_len)
{
    if (s == NULL || parts == NULL || out == NULL || out_len == NULL)
        return NSBC_INVAL;
    (void)cap;
    *out_len = 0;
    s->nparts = nparts;
    /* TODO(P2)：rs-metadata 文档（recording/session/participant/stream
     * 元素 + xml:lang/state 属性），显示名 XML 转义防注入。 */
    return NSBC_NOTSUP;
}

int siprec_render_update(siprec_sess_t *s, const char *state, const src_participant_t *changed, int nchanged, char *out, size_t cap, size_t *out_len)
{
    if (s == NULL || state == NULL || out == NULL || out_len == NULL)
        return NSBC_INVAL;
    (void)changed;
    (void)nchanged;
    (void)cap;
    *out_len = 0;
    /* TODO(P2)：state ∈ {paused,resumed,stopped}；参与者增删用
     * 同一 session id 的增量 metadata；SDP 可省（媒体不变）。 */
    return NSBC_NOTSUP;
}

int siprec_attach_rtp(siprec_sess_t *s, int leg, void *rtp_src_key)
{
    if (s == NULL || rtp_src_key == NULL)
        return NSBC_INVAL;
    if (s->state != SRC_ST_ACTIVE)
        return NSBC_INVAL;
    (void)leg;
    /* TODO(P2)：媒体快路径注册第二接收者（独立 SSRC 重写、独立序号），
     * 发送失败静默熔断（录音不可达绝不影响原通话媒体）。 */
    return NSBC_NOTSUP;
}

void siprec_detach_rtp(siprec_sess_t *s)
{
    if (s == NULL)
        return;
    /* TODO：注销分叉接收者并等待在途复制排空（有界） */
}

int siprec_on_sip_state(siprec_sess_t *s, int sip_status, src_state_t want)
{
    if (s == NULL)
        return NSBC_INVAL;
    if (sip_status == 200) {
        s->state = want == SRC_ST_PAUSED ? SRC_ST_PAUSED : SRC_ST_ACTIVE;
        return NSBC_OK;
    }
    /* 4xx/5xx/6xx：INVITE 失败转 FAILED（不影响主通话，只发告警事件） */
    if (sip_status >= 400 && sip_status <= 699) {
        s->state = SRC_ST_FAILED;
        return NSBC_OK;
    }
    return NSBC_PROTO;
}

src_state_t siprec_state(const siprec_sess_t *s)
{
    return s == NULL ? SRC_ST_IDLE : s->state;
}
