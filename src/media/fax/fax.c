/* 传真——粗骨架（REQ-FAX-001，P1）
 *
 * 接线注意：T.38 是 image 媒体，不与 RTP 共享端口（SDP 独立 m=image
 * 段，6.1.1 端口模型另行分配 UDPTL 偶对，port_table 扩类型而非复用）；
 * CNG/CED 检测可用 Goertzel 1100/2100Hz 固定门限，误触发要经连续
 * 周期确认（CNG 0.5s on/3s off 至少一轮）。
 */
#include "fax.h"

struct fax_sess {
    fax_cfg_t cfg;
    fax_mode_t mode;
    int       passthrough;
};

int fax_create(const fax_cfg_t *cfg, fax_sess_t **out)
{
    fax_sess_t *s;
    if (cfg == NULL || out == NULL)
        return NSBC_INVAL;
    s = (fax_sess_t *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NSBC_NOMEM;
    s->cfg = *cfg;
    s->mode = cfg->mode_init;
    *out = s;
    return NSBC_OK;
}

void fax_destroy(fax_sess_t *s)
{
    free(s);
}

fax_event_t fax_detect_tones(fax_sess_t *s, const int16_t *pcm, size_t samples, int rate)
{
    if (s == NULL || pcm == NULL)
        return FAX_EV_FAILED;
    (void)samples;
    (void)rate;
    /* TODO(P1)：1100/2100Hz Goertzel + 节奏状态机（CNG/CED 确认） */
    return FAX_EV_NONE;
}

int fax_switch_mode(fax_sess_t *s, fax_mode_t mode)
{
    if (s == NULL)
        return NSBC_INVAL;
    if (mode == FAX_MODE_T38_RELAY && !s->cfg.t38_enabled)
        return NSBC_PERM_DENIED;
    s->mode = mode;
    s->passthrough = (mode == FAX_MODE_G711_PASSTHROUGH);
    /* TODO：通知 bridge 关 EC/VAD/JB；T38 时申请 UDPTL 端口并切换 */
    return NSBC_OK;
}

fax_mode_t fax_mode(const fax_sess_t *s)
{
    return s == NULL ? FAX_MODE_OFF : s->mode;
}

int fax_udptl_send(fax_sess_t *s, const uint8_t *ifp, size_t ifp_len, uint8_t *out, size_t cap, size_t *out_len)
{
    if (s == NULL || ifp == NULL || out == NULL || out_len == NULL)
        return NSBC_INVAL;
    (void)ifp_len;
    (void)cap;
    *out_len = 0;
    /* TODO(P1)：UDPTL 帧（RFC 4148）seq + 控制字段 + 主 IFP +
     * 冗余历史 IFP（FEC 时按 udptl_redundancy 预填） */
    return NSBC_NOTSUP;
}

int fax_udptl_recv(fax_sess_t *s, const uint8_t *buf, size_t len, const uint8_t **ifp, size_t *ifp_len)
{
    if (s == NULL || buf == NULL || ifp == NULL || ifp_len == NULL)
        return NSBC_INVAL;
    (void)len;
    /* TODO(P1)：seq 去重/乱序重排、FEC/parity 恢复、提取最新 IFP */
    return NSBC_NOTSUP;
}

int fax_passthrough_active(const fax_sess_t *s)
{
    return (s != NULL) ? s->passthrough : 0;
}
