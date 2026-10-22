/* 会话更新在媒体路径上的两件配套机制：
 * 1. 媒体闸门：RFC 3312 前提条件未满足、hold/inactive 时锁定转发，
 *    183 阶段可仅放行早期媒体；
 * 2. SSRC 防护：re-INVITE/UPDATE 重指对端后允许学习一次新 SSRC，
 *    其余 SSRC 跳变按伪造流量处理（REQ-MEDIA-SEC-001）。 */

#include "rtp.h"

/* ---- 媒体闸门 ---- */
void rtp_gate_init(rtp_gate_t *g) { g->state = RTP_GATE_LOCKED; }

void rtp_gate_update(rtp_gate_t *g, int precond_active, int precond_met,
                     int early_media, int dir_open)
{
    if (precond_active && !precond_met) {
        g->state = early_media ? RTP_GATE_EARLY : RTP_GATE_LOCKED;
        return;
    }
    g->state = dir_open ? RTP_GATE_OPEN : RTP_GATE_LOCKED;
}

int rtp_gate_forward(const rtp_gate_t *g)
{
    return g->state == RTP_GATE_OPEN;
}

int rtp_gate_forward_early(const rtp_gate_t *g)
{
    return g->state != RTP_GATE_LOCKED;
}

/* ---- SSRC 学习/重指 ---- */
void rtp_ssrc_guard_init(rtp_ssrc_guard_t *g)
{
    g->ssrc = 0;
    g->learned = 0;
    g->grace = 0;
}

void rtp_ssrc_guard_retarget(rtp_ssrc_guard_t *g) { g->grace = 1; }

int rtp_ssrc_guard_check(rtp_ssrc_guard_t *g, uint32_t ssrc)
{
    if (!g->learned) {
        g->ssrc = ssrc;
        g->learned = 1;
        return 1;
    }
    if (ssrc == g->ssrc)
        return 1;
    if (g->grace) { /* 重指后唯一一次学习机会 */
        g->ssrc = ssrc;
        g->grace = 0;
        return 1;
    }
    return 0;
}