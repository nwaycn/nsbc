#ifndef NSBC_SIP_SESS_TIMER_H
#define NSBC_SIP_SESS_TIMER_H

/* RFC 4028 会话定时（Session-Expires / Min-SE / refresher 参数）
 * 对应 REQ-SIP-014（P0 缺口：当前零代码）。
 *
 * 机制：协商出会话刷新间隔与 refresher 角色；refresher 在间隔过半时
 * 发 re-INVITE/UPDATE；对端在间隔+偏移内未收到刷新即发 BYE 拆话，
 * 回收僵尸对话（UDP NAT 静默/对端崩溃）。
 *
 * 分层：sip 层；仅依赖 base（单调时钟）。时间轮 arm 由 b2bua leg 持有，
 * 本模块只做协商/判定纯逻辑，不直接调 transport。
 */
#include "nsbc_common.h"
#include "sip_event.h" /* sip_refresher_t（同 sip 层） */

#ifdef __cplusplus
extern "C" {
#endif

#define SESS_TIMER_MIN_SE_S   90    /* RFC4028 §4 定时器最小值（秒） */
#define SESS_TIMER_DEF_SE_S   1800  /* 缺省 30min */
#define SESS_TIMER_GRACE_MS   32000 /* 超时宽限：对齐事务 Timer B 32s */

typedef struct {
    int      min_se_s;    /* 本地 Min-SE（下限钳制，默认 90） */
    int      def_se_s;    /* 发起时 Session-Expires 缺省 */
    int      max_se_s;    /* 本地接受上限（可配） */
} sess_timer_cfg_t;

/* 每对话每 Leg 各持一份（B2BUA 两侧各自协商，独立刷新） */
typedef struct {
    int              active;       /* 对端支持 timer 且协商成功 */
    int              interval_s;   /* 协商后的 Session-Expires */
    sip_refresher_t  refresher;    /* 本侧角色；SIP_REF_NONE=对端刷新 */
    uint64_t         armed_ms;     /* 单调时钟：定时器武装时刻 */
    int              refresh_sent; /* 已发刷新未应答，防重入 */
} sess_timer_t;

/* 协商结果（UAC 发 INVITE 前填提议，收 200/422 后回填结果） */
typedef struct {
    int              interval_s;
    int              min_se_s;
    sip_refresher_t  refresher;
} sess_timer_nego_t;

void sess_timer_cfg_default(sess_timer_cfg_t *cfg);
void sess_timer_init(sess_timer_t *st);

/* UAC 发起：根据配置生成本地提议（Session-Expires/Min-SE/Supported:timer） */
int  sess_timer_uac_offer(const sess_timer_cfg_t *cfg, sess_timer_nego_t *out);
/* UAS 收到提议：钳制 interval=max(min_se, min(提议, 本地max))，
 * 无 refresher 参数时 UAS 按 RFC4028 §5.1 强制 refresher=uas */
int  sess_timer_uas_negotiate(const sess_timer_cfg_t *cfg,
                              const sess_timer_nego_t *in,
                              sess_timer_nego_t *out);
/* 200 应答侧确认角色并武装（now_ms=CLOCK_MONOTONIC） */
int  sess_timer_arm(sess_timer_t *st, const sess_timer_nego_t *n,
                    uint64_t now_ms);

/* refresher：是否到刷新点（过半），到点返回 1 并置 refresh_sent */
int  sess_timer_due_refresh(sess_timer_t *st, uint64_t now_ms);
/* 被刷新方：会话是否已超时（interval+grace），超时返回 1→发 BYE */
int  sess_timer_expired(const sess_timer_t *st, uint64_t now_ms);
/* 收到任意 re-INVITE/UPDATE 重置武装点 */
void sess_timer_reset(sess_timer_t *st, uint64_t now_ms);
/* 刷新 2xx 应答后清 refresh_sent */
void sess_timer_refresh_acked(sess_timer_t *st, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_SIP_SESS_TIMER_H */
