#ifndef NSBC_CALLCTRL_H
#define NSBC_CALLCTRL_H

/* 呼叫业务控制（REQ-PBX-SS-001 转接 / 002 前转 / 003 驻留代答，
 * 004~006 其他补充业务；Follow-me/find-me P1）
 *
 * 架构铁律：mod 不直接操作 sip/b2bua 对象。呼叫键为不透明 void*
 * （A2 中立化模式：与 media 的 call_key 同口径，禁止解引用），所有
 * 信令动作经 ccf_ops_t 回调，由 core/signaling 组合根在装配期注入
 * （实现体在 b2bua：发 REFER/BYE/INVITE）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CCF_NUMBER_MAX   48
#define CCF_ORBIT_MAX    16
#define CCF_PICKUP_GRP_MAX 32

/* 前转类型（SDP/信令均在振铃前/振铃中决策） */
typedef enum {
    CCF_FWD_UNCONDITIONAL = 0, /* CFU：所有来电 */
    CCF_FWD_BUSY,              /* CFB：分机忙（并发路数满/DND） */
    CCF_FWD_NOANSWER,          /* CFNR：超时无应答（默认 20s） */
    CCF_FWD_UNAVAILABLE        /* CFNRc：未注册/不可达 */
} ccf_fwd_kind_t;

typedef struct {
    char target[CCF_NUMBER_MAX];
    int  timeout_s;       /* CFNR 超时 */
    int  enabled;
} ccf_fwd_rule_t;

/* 转接方式（业务层语义；REFER/Replaces 的信令拼装在 sip_refer） */
typedef enum {
    CCF_XFER_BLIND = 0,   /* REFER 立即拆原腿 */
    CCF_XFER_CONSULTED    /* 先咨询，Replaces 接续（attended） */
} ccf_xfer_kind_t;

/* 组合根注入的信令动作集（全部不可为空，除显式标注） */
typedef struct {
    /* 向 call_key 的所属 Leg 发 REFER（盲转）或 INVITE+Replaces */
    int  (*send_refer)(void *ud, void *call_key, const char *target,
                       ccf_xfer_kind_t kind, void *consult_key);
    /* 新建一路外呼（Follow-me/前转目标落地） */
    int  (*originate)(void *ud, const char *target, void **new_key);
    /* 拆指定呼叫（原因码透传 Q.850） */
    int  (*terminate)(void *ud, void *call_key, int cause);
    /* 播放提示音（前转通知/驻留号播报），可空 */
    int  (*play_announce)(void *ud, void *call_key, const char *clip);
    void *ud;
} ccf_ops_t;

typedef struct ccf_mgr ccf_mgr_t;

int  ccf_create(const ccf_ops_t *ops, ccf_mgr_t **out);
void ccf_destroy(ccf_mgr_t *m);

/* 前转规则管理（按分机；自服务 feature code 经 DigitMap 改） */
int  ccf_fwd_set(ccf_mgr_t *m, const char *tenant, const char *ext,
                 ccf_fwd_kind_t kind, const ccf_fwd_rule_t *r);
int  ccf_fwd_get(ccf_mgr_t *m, const char *tenant, const char *ext,
                 ccf_fwd_kind_t kind, ccf_fwd_rule_t *out);
int  ccf_fwd_clear(ccf_mgr_t *m, const char *tenant, const char *ext,
                   ccf_fwd_kind_t kind);
/* 振铃前/中查询：busy/noanswer/unavailable 条件由调用方置 1 */
int  ccf_fwd_resolve(ccf_mgr_t *m, const char *tenant, const char *ext,
                     int busy, int noanswer, int unavailable,
                     ccf_fwd_rule_t *out);

/* 转接（业务入口；consult_key 仅 CONSULTED 时有效） */
int  ccf_transfer(ccf_mgr_t *m, void *call_key, const char *target,
                  ccf_xfer_kind_t kind, void *consult_key);

/* 驻留：分配 orbit 号（*00..），呼叫挂起（播放等待音/音乐由媒体面） */
int  ccf_park(ccf_mgr_t *m, void *call_key, char *orbit_out,
              size_t cap);
/* 代接：从 orbit 取回呼叫接到 target_ext */
int  ccf_park_retrieve(ccf_mgr_t *m, const char *orbit,
                       const char *target_ext, void **call_key_out);
/* 指定代接/组代接：grp=分机组名，ringing_ext 指定代接某分机（可空） */
int  ccf_pickup(ccf_mgr_t *m, const char *grp, const char *ringing_ext,
                const char *target_ext, void **call_key_out);

/* Follow-me（P1）：分机振铃同时顺序/并行呼列表号码，超时统一 */
int  ccf_followme_set(ccf_mgr_t *m, const char *tenant, const char *ext,
                      const char *const *targets, int n, int timeout_s);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_CALLCTRL_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
