#ifndef NSBC_SSRC_POLICY_H
#define NSBC_SSRC_POLICY_H

/* 多源 SSRC 白名单（REQ-MEDIA-SEC-003，替代 parse 层单 SSRC guard 的
 * 生产口径；旧 rtp_ssrc_guard_* 保留给 poc 兼容路径）
 *
 * 允许的合法多源：
 *  - 主发言源（primary）；
 *  - RTX/FEC 修复流（按 SDP a=rtpmap 的 payload type 识别）；
 *  - BUNDLE 同端口其它 m-line 的 SSRC（显式登记）；
 *  - 发言切换：主源静默超阈值后接纳新主源（会议/MCU 场景）。
 * 安全：
 *  - RFC3550 §8.2 SSRC collision：入向 SSRC 等于本侧出向 SSRC 立即丢弃；
 *  - SSRC↔CNAME 绑定：CNAME 变更视为伪造，丢弃并计数；
 *  - 白名单外第三个及以上新 SSRC：丢弃 + 注入计数（不学习）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSRC_POLICY_MAX_SRC 8
#define SSRC_CNAME_MAX      256

typedef enum {
    SRC_DEC_DROP = 0,
    SRC_DEC_PRIMARY,
    SRC_DEC_AUX,       /* RTX/FEC/BUNDLE 白名单源 */
    SRC_DEC_NEW_PRIMARY/* 发言切换接纳的新主源 */
} src_decision_t;

typedef struct {
    uint32_t ssrc;
    int used;
    int primary;
    int aux;            /* RTX/FEC/BUNDLE */
    uint64_t last_ms;
    uint64_t pkts;
    char cname[SSRC_CNAME_MAX];
    int cname_set;
} src_entry_t;

typedef struct {
    uint32_t local_ssrc;       /* 本侧出向 SSRC（collision 检测） */
    uint32_t aux_pt[8];        /* RTX/FEC payload type 集合 */
    int n_aux_pt;
    uint64_t switch_idle_ms;   /* 主源静默多久允许切换，默认 5000 */
    int max_sources;           /* 默认 SSRC_POLICY_MAX_SRC */
} ssrc_policy_cfg_t;

typedef struct {
    ssrc_policy_cfg_t cfg;
    src_entry_t src[SSRC_POLICY_MAX_SRC];
    int n;
    int learned_primary;
    uint64_t inject_drops;
    uint64_t collision_drops;
    uint64_t cname_drops;
    uint64_t switches;
    /* ★ RACE-FIX: SIP 线程 admit_ssrc_from_sdp ↔ reactor 线程
     * ssrc_policy_on_rtp 并发读写 p->n / p->src[] / p->learned_primary
     * → data race → video SSRC 半写被丢 → 概率性单向视频。
     * pthread_mutex 保护所有修改 + 读操作。 */
    pthread_mutex_t mu;
} ssrc_policy_t;

void ssrc_policy_init(ssrc_policy_t *p, const ssrc_policy_cfg_t *cfg);

/* BUNDLE 显式登记合法 SSRC（SDP a=ssrc 学习后调用） */
void ssrc_policy_admit(ssrc_policy_t *p, uint32_t ssrc, int aux);
void ssrc_policy_set_aux_pt(ssrc_policy_t *p, uint8_t pt);

/* re-INVITE/UPDATE 媒体重指：保留 CNAME 偏好，允许一次重新学习 */
void ssrc_policy_retarget(ssrc_policy_t *p);

/* 每个入向 RTP 包判定 */
src_decision_t ssrc_policy_on_rtp(ssrc_policy_t *p, uint32_t ssrc,
                                  uint8_t pt, uint64_t now_ms);

/* RTCP SDES CNAME 绑定校验；返回 1 一致/已学习，0 冲突丢弃 */
int ssrc_policy_on_cname(ssrc_policy_t *p, uint32_t ssrc, const char *cname,
                         size_t clen, uint64_t now_ms);

/* collision：对端把我们的 SSRC 打回来时，本侧重选后更新 */
void ssrc_policy_set_local(ssrc_policy_t *p, uint32_t local_ssrc);

uint64_t ssrc_policy_inject_drops(const ssrc_policy_t *p);
uint64_t ssrc_policy_collision_drops(const ssrc_policy_t *p);

#ifdef __cplusplus
}
#endif
#endif

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
