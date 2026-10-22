#ifndef NSBC_NAT_ICE_H
#define NSBC_NAT_ICE_H

/* ICE 代理（RFC 8445 full ICE；lite 对端兼容）——REQ-NAT-001 主体
 *
 * 组成：候选收集（host/srflx 经 STUN 服务器/prflx/relay 经 TURN）、
 * 角色（controlling/controlled + tie-breaker）、检查清单状态机、
 * 提名（regular/aggressive）、consent freshness（RFC 7675，15s 事务
 * 30s 断流）；component=1 RTP，rtcp-mux/BUNDLE 下 component 收敛。
 *
 * SDP 交互：a=ice-ufrag/ice-pwd/ice-options:trickle/candidate 行的
 * 文本解析在 parse/sdp 层；本模块只收结构化候选与凭证。
 *
 * 分层：media（组合 net socket + stun 解析）；被 media_bridge/
 * rtp 收包环在 rtp_classify==PKT_STUN 时回调（同端口复流）。
 */
#include "nsbc_common.h"
#include "net_sock.h"
#include "stun_msg.h"
#include "turn_client.h" /* turn_sess_t（同 media 层） */

#ifdef __cplusplus
extern "C" {
#endif

#define ICE_UFRAG_MAX    256
#define ICE_PWD_MAX      256
#define ICE_FOUND_MAX    32
#define ICE_PAIR_MAX     32

typedef enum {
    ICE_CAND_HOST = 0,
    ICE_CAND_SRFLX,
    ICE_CAND_PRFLX,
    ICE_CAND_RELAY
} ice_cand_type_t;

typedef enum {
    ICE_PAIR_FROZEN = 0,
    ICE_PAIR_WAITING,
    ICE_PAIR_IN_PROGRESS,
    ICE_PAIR_SUCCEEDED,
    ICE_PAIR_FAILED,
    ICE_PAIR_CANCELLED
} ice_pair_state_t;

/* RFC 8445 §11 代理总状态 */
typedef enum {
    ICE_ST_NOT_RUNNING = 0,
    ICE_ST_GATHERING,
    ICE_ST_CHECKING,
    ICE_ST_CONNECTED,
    ICE_ST_COMPLETED,
    ICE_ST_FAILED,
    ICE_ST_DISCONNECTED,
    ICE_ST_CLOSED
} ice_agent_state_t;

typedef struct {
    ice_cand_type_t type;
    int             component;   /* 1=RTP 2=RTCP */
    int             proto_udp;   /* 粗骨架仅 UDP */
    uint32_t        priority;
    char            foundation[32];
    nsbc_sa_t       addr;        /* 候选地址（relay=relayed 地址） */
    nsbc_sa_t       base;        /* srflx/relay 的 base（host 自身） */
} ice_cand_t;

typedef struct {
    ice_cand_t       local;
    ice_cand_t       remote;
    ice_pair_state_t state;
    uint64_t         priority;
    int              nominated;
    int              consent_ok;
} ice_pair_t;

typedef struct ice_agent ice_agent_t;

typedef struct {
    int       full_ice;       /* 1=full，0=lite（仅本端作为 full 对 lite） */
    int       trickle;        /* 支持 trickle ICE（RFC 8838） */
    int       controlling;   /* 初始角色愿望（SDP setup 冲突由 tie-break 解） */
    uint32_t  ta_ms;         /*  pacing：RFC8445 推荐 50ms（G.8） */
    uint32_t  consent_ms;    /* RFC7675：15s 周期，30s 失败断流 */
    nsbc_sa_t stun_server;   /* srflx 获取（可空） */
    turn_sess_t *turn;       /* relay 候选（可空） */
} ice_cfg_t;

int  ice_agent_create(const ice_cfg_t *cfg, ice_agent_t **out);
void ice_agent_destroy(ice_agent_t *a);

/* 本端/对端凭证（a=ice-ufrag/ice-pwd） */
int  ice_set_local_cred(ice_agent_t *a, const char *ufrag, const char *pwd);
int  ice_set_remote_cred(ice_agent_t *a, const char *ufrag, const char *pwd);

/* 候选管理：本地收集由内部经 STUN/TURN 触发；远端来自 SDP/trickle */
int  ice_add_local_candidate(ice_agent_t *a, const ice_cand_t *c);
int  ice_add_remote_candidate(ice_agent_t *a, const ice_cand_t *c);

/* 开始检查（生成清单、按 pair 优先级排序、置 CHECKING） */
int  ice_start_checks(ice_agent_t *a, uint64_t tiebreaker);
/* 角色冲突（487 Role Conflict）：切换 controlling 并重排队列 */
int  ice_switch_role(ice_agent_t *a, uint64_t tiebreaker);

/* 同媒体端口收到 STUN：是连通检查则生成 Binding Success 响应帧；
 * 是 consent 事务则刷新 pair.consent_ok；未知对端 srflx 学习 prflx。
 * 返回写出长度；非 STUN/非本 agent 凭证返 NOENT（调用方放行 RTP 路径）。 */
int  ice_on_stun(ice_agent_t *a, const uint8_t *buf, size_t len,
                 const nsbc_sa_t *src, const nsbc_sa_t *local,
                 uint8_t *resp, size_t resp_cap, size_t *resp_len);

/* 周期驱动（时间轮）：发下一检查（ta pacing）、consent 事务、
 * 超时迁移 DISCONNECTED/FAILED；need_tx=1 时帧在 out 待发往 dst。 */
int  ice_tick(ice_agent_t *a, uint64_t now_ms,
              uint8_t *out, size_t cap, size_t *out_len,
              nsbc_sa_t *dst, int *need_tx);

ice_agent_state_t ice_state(const ice_agent_t *a);
/* 选定 pair（nominated+valid 最高优先级），未选定返 NOENT */
int  ice_selected_pair(const ice_agent_t *a, ice_pair_t *out);

/* trickle：补候选后重排检查清单（CHECKING/COMPLETED 均可追加） */
int  ice_end_of_candidates(ice_agent_t *a);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_NAT_ICE_H */
