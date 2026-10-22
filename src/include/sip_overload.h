#ifndef NSBC_SIP_OVERLOAD_H
#define NSBC_SIP_OVERLOAD_H

/* SIP 过载控制——粗骨架（REQ-SIP-015，P1）
 *
 * 标准：
 *   RFC 7683：邻居间过载控制头字段 oc / oc-algo / oc-validity / oc-seq；
 *   RFC 7339：三种算法 loss（按比例丢信令）、rate（CPS 上限）、cpu；
 *   RFC 7415：proxy-internal 触发（pi-triggered，可选）。
 * 与本地队列联动：入向准入在事务层之前；被拒新 INVITE 回 503+Retry-After，
 * 中继/重发/已在对话内请求不拒（RFC 7683 §5 安全要点）。
 *
 * 分层：sip 层；纯判定+头渲染，不引 transport；时间基准单调时钟。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OVL_OC_HDR_MAX      96   /* oc 头渲染上限 */
#define OVL_RETRY_AFTER_DEF 30   /* 503 Retry-After 缺省秒 */

typedef enum {
    OVL_ALGO_OFF = 0,
    OVL_ALGO_LOSS,  /* "loss"：削减百分比，按优先级随机丢弃 */
    OVL_ALGO_RATE,  /* "rate"：CPS 闸门 */
    OVL_ALGO_CPU    /* "cpu"：CPU 占用驱动 */
} ovl_algo_t;

/* 信令优先级（RFC 6799 local-priority / Resource-Priority 粗映射），
 * 过载削减从低优先级开始：消息/OPTIONS 探测 < 新 INVITE <
 * 已在对话请求 < CANCEL/BYE/ACK/重发（绝不拒）。 */
typedef enum {
    OVL_PRIO_PROBE = 0, /* OPTIONS 低耗探测 */
    OVL_PRIO_NEW,       /* 新 INVITE/REGISTER */
    OVL_PRIO_DIALOG,    /* 对话内 re-INVITE/UPDATE/REFER */
    OVL_PRIO_CRITICAL   /* CANCEL/BYE/ACK/事务重传 */
} ovl_prio_t;

typedef struct {
    ovl_algo_t algo;
    int        reduction_init;  /* 初始削减百分比 0..100 */
    int        reduction_max;
    int        cpu_hi;          /* cpu 算法进入过载阈值（%） */
    int        cpu_lo;          /* 退出过载阈值（迟滞） */
    int        cps_limit;       /* rate 算法 CPS 上限（0=不限） */
    int        window_ms;       /* 测量窗口/oc-validity（毫秒） */
    int        retry_after_s;
} ovl_cfg_t;

typedef struct sip_ovl sip_ovl_t;

void ovl_cfg_default(ovl_cfg_t *cfg);
int  ovl_create(const ovl_cfg_t *cfg, sip_ovl_t **out);
void ovl_destroy(sip_ovl_t *ov);

/* 准入判定：OK 放行；AGAIN 应回 503（调用方附 Retry-After）。
 * 窗口计数调用方在放行后 ovl_note_admit 记账（rate 算法用）。 */
int  ovl_admit(sip_ovl_t *ov, ovl_prio_t prio, uint32_t rnd_percent);
void ovl_note_admit(sip_ovl_t *ov, uint64_t now_ms);

/* 周期测量喂入（cpu 0..100；loss 为上游反馈的当前削减值） */
void ovl_update_measure(sip_ovl_t *ov, int cpu_pct, int peer_reduction,
                        uint64_t now_ms);

/* 渲染出站响应中的过载头（RFC 7683 单行，未过载 out_len=0） */
int  ovl_render_hdr(sip_ovl_t *ov, char *buf, size_t cap, size_t *out_len);

/* 消费下游邻居响应中的 oc 头（零拷贝；值非法忽略，不致命） */
void ovl_on_peer_hdr(sip_ovl_t *ov, const char *algo, size_t algo_len,
                     int reduction, int validity_s, uint32_t seq);

int  ovl_overloaded(const sip_ovl_t *ov);
int  ovl_retry_after(const sip_ovl_t *ov);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_SIP_OVERLOAD_H */
