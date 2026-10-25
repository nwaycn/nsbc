#ifndef NSBC_MEDIA_BRIDGE_H
#define NSBC_MEDIA_BRIDGE_H

/* 媒体三模式桥接（REQ-MEDIA-MODE，P0）
 *
 * bypass：不分配 RTP 端口、端口表无项，SDP 原样透传（配置白名单/可信对端
 *         或双方公网直达时）；
 * relay ：每侧分配 (RTP,RTCP) 端口对，latching 首包学习对端地址、多源
 *         SSRC 白名单（REQ-MEDIA-SEC-003）、出向 SSRC 重写、RTCP 透传；
 * transcode：编解码无交集时的决策；P0 只做决策（返回 NSBC_NOTSUP），
 *         执行引擎 P1（呼叫按 488/回落处理由上层决定）。
 */
#include "net_sock.h"
#include "port_table.h"
#include "reactor.h"
#include "ssrc_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MB_BYPASS = 0,
    MB_RELAY,
    MB_TRANSCODE,
    MB_SRTP_RELAY  /* DTLS-SRTP relay（WebRTC 方向 DTLS 终结 + libsrtp2 保护） */
} mb_mode_t;

typedef struct bridge_mgr bridge_mgr_t;

/* per-call RTP 端口段查询（REQ-SIP-PROFILE-001）：relay 建立前按 call_key
 * 问组合根该呼叫的 RTP 端口段（通常来自 b2b_call_t 归属的 sip_profile_t
 * rtp_port_start/end）。回调必须把 lo/hi 输出预置 0（=表默认段）。
 * media 层经回调取值、禁止解引用 call_key（REQ-ARCH-011）。 */
typedef void (*mb_call_port_range_fn)(void *ud, void *call_key,
                                      uint16_t *lo, uint16_t *hi);

/* B 腿是否 WS/WSS 注册（浏览器 WebRTC）——offer 阶段 B 的 SDP 未知
 * （MicrosIP→WebRTC 方向 offer 是纯 SDP，bridge_decide 判不出 WebRTC），
 * 组合根从 b2b_call_t.leg[1]（proto / ruri transport=ws）回答。
 * 返回 1=WebRTC 腿；0/无回调=普通腿。media 层禁止解引用 call_key。 */
typedef int (*mb_call_b_is_ws_fn)(void *ud, void *call_key);

/* REQ-MEDIA-DP-001：拨号规则级媒体模式 override 查询。
 * 从 b2b_call_t.media_mode 返回：0=auto(按自动决策), 1=bypass(强制绕过), 2=relay(强制转发)。
 * 回调可空；media 层禁止解引用 call_key（REQ-ARCH-011 解耦）。
 * override 仅对非 WebRTC 生效（WebRTC 必须 SRTP_RELAY，bypass 会破坏通话）。 */
typedef uint8_t (*mb_call_media_mode_fn)(void *ud, void *call_key);

/* REQ-NAT-002：per-call NAT 公网 RTP IP 查询。
 * 返回 sip_profile_t.external_rtp_ip（profile 数据库里的公网 IP），
 * 空字符串或 NULL 表示关闭 NAT（用 local_ip 做 fallback）。
 * media 层禁止解引用 call_key（REQ-ARCH-011）。 */
typedef const char *(*mb_call_external_rtp_ip_fn)(void *ud, void *call_key);

typedef struct {
    ptable_t *ptable;
    reactor_t *re;
    const char *local_ip;   /* 内网 bind 地址 + SDP c= 重写 fallback */
    int allow_bypass;       /* 默认 0：外网 SBC 默认锚定 */
    int trust_private;      /* 可信内网允许 bypass（默认 1） */
    /* REQ-REC-006：raw RTP payload 录制回调槽（同 cdr_emit 模式）
     * 媒体快路径旁路，函数不得阻塞（rec_feed_raw 内部已持自己的 lock）。
     * call_key=bridge 侧不透明呼叫键（cnode_t.call，实际是 b2b_call_t*），
     * 供回调函数反查到 rec_sess_t；payload=RTP payload（去头），
     * len=payload_len，leg=side (0/1)，pt=RTP pt（0=PCMU, 8=PCMA）。 */
    void (*rec_feed_fn)(void *ud, void *call_key, const void *payload,
                        size_t len, int leg, int pt);
    void *rec_feed_ud;
    /* REQ-SIP-PROFILE-001：per-call RTP 端口段（可空=全部走表默认段） */
    mb_call_port_range_fn call_port_range;
    void *call_port_range_ud;
    /* REQ-DTLS-001：B 腿 WebRTC 判定（可空=仅按 SDP 内容判） */
    mb_call_b_is_ws_fn call_b_is_ws;
    void *call_b_is_ws_ud;
    /* REQ-MEDIA-DP-001：拨号规则级媒体模式 override（可空=按自动决策） */
    mb_call_media_mode_fn call_media_mode;
    void *call_media_mode_ud;
    /* REQ-NAT-002：per-call NAT 公网 RTP IP（可空=profile 没配=关闭 NAT） */
    mb_call_external_rtp_ip_fn call_external_rtp_ip;
    void *call_external_rtp_ip_ud;
} mb_cfg_t;

int bridge_mgr_create(const mb_cfg_t *cfg, bridge_mgr_t **out);
void bridge_mgr_destroy(bridge_mgr_t *m);

/* 依据两侧 SDP 决策；answer 可为 NULL（仅 offer 阶段：默认 relay/bypass） */
int bridge_decide(bridge_mgr_t *m, const char *offer, size_t offer_len,
                  const char *answer, size_t answer_len, mb_mode_t *mode);

/* SDP 改写方向（REQ-DTLS-001 方向感知）：
 * ASIS  —— 纯改 c=/m= 地址端口；
 * PLAIN —— 目标是明文端（MicroSIP 等）：m= proto 改 RTP/AVP，
 *          剥 ICE/DTLS 属性（ice-ufrag/pwd/fingerprint/setup/candidate）；
 * WS    —— 目标是 WebRTC 端：m= proto 改 UDP/TLS/RTP/SAVPF，剥对端
 *          旧 ICE/DTLS 凭证（调用方随后注入本侧 dtls_server 的新凭证）。 */
typedef enum {
    MB_SDP_ASIS = 0,
    MB_SDP_PLAIN,
    MB_SDP_WS,
} mb_sdp_dir_t;

/* SDP c=/m= 地址端口重写（所有媒体段，每段端口递增 2）+ 方向相关的
 * proto/属性改写（见 mb_sdp_dir_t）。
 * rtp_port==0 约定：BYPASS 直连透传——c= 和 m= 端口全部保留原值
 * （m= 端口写 0 会被对端按 RFC 3264 当"流被拒"）。
 * 返回写出长度；NSBC_NOSPC 缓冲不足。 */
int bridge_rewrite_sdp(const char *in, size_t in_len, char *out,
                       size_t outsz, const char *ip, uint16_t rtp_port,
                       mb_sdp_dir_t dir);

/* 一个 relay 呼叫实例：两对端口、两条单向转发。
 * range_lo/hi：per-call RTP 端口段（REQ-SIP-PROFILE-001）；0,0=表默认段。
 * 段内耗尽返回 NOSPC——调用方（bridge_offer_hook）回落表默认段重试。 */
typedef struct rtp_relay rtp_relay_t;

typedef struct {
    uint16_t a_rtp, a_rtcp; /* 朝 A 侧（入向主叫方） */
    uint16_t b_rtp, b_rtcp; /* 朝 B 侧（下游） */
} relay_ports_t;

int rtp_relay_start(bridge_mgr_t *m, rtp_relay_t **out,
                    relay_ports_t *ports,
                    uint16_t range_lo, uint16_t range_hi);
void rtp_relay_stop(rtp_relay_t *r);

/* 由 SDP 连接地址设置对端（B 的真实地址/A 的地址）；latching 未完成前
 * 仅允许该地址首包锁定 */
void rtp_relay_set_peer_a(rtp_relay_t *r, const nsbc_sa_t *sa);
void rtp_relay_set_peer_b(rtp_relay_t *r, const nsbc_sa_t *sa);

/* OBS：转发/丢弃计数 */
uint64_t rtp_relay_pkts(const rtp_relay_t *r);
uint64_t rtp_relay_drops(const rtp_relay_t *r);

/* REQ-ORIGINATE-003 &echo：RTP 环回开关。
 * on=1 时 relay 把 side 收到的 RTP 发回同一侧（side→side），
 * 实现"通话方听到自己回声"。调用需在 B 腿 LEG_CONFIRMED 之后。 */
void rtp_relay_set_echo(rtp_relay_t *r, int on);

/* b2bua 挂接：offer/answer SDP 出口重写；内部完成决策+端口分配+relay 建立。
 * call_key 为 b2b 层传入的不透明呼叫身份键（b2b_call_t*），本层只以其
 * 指针值做 mgr 表键（每呼叫一个），禁止解引用（REQ-ARCH-011 解耦）。 */
int bridge_offer_hook(void *call_key, const char *in, size_t inlen,
                      char *out, size_t outsz, void *ud);
int bridge_answer_hook(void *call_key, const char *in, size_t inlen,
                       char *out, size_t outsz, void *ud);
/* 呼叫结束释放（挂 b2b terminate；P0 由 media confirm 回调反注册兜底） */
void bridge_release_call(bridge_mgr_t *m, void *call_key);

/* 两侧 SDP 齐备后的最终模式确认（挂 b2b media 回调）：
 * 已建 relay 实例不降级 bypass（媒体面已锚定）；决策为 transcode 时把
 * 模式落账供诊断（执行引擎 P1，本函数不改变转发路径）。 */
int bridge_confirm(bridge_mgr_t *m, void *call_key, const char *offer,
                   size_t offer_len, const char *answer, size_t answer_len);

/* 呼叫媒体面只读视图（REQ-MEDIA-SEC-003⑤ / REQ-OBS-010，OAM 低频路径）。
 * 数组下标为 relay 物理侧：[0]=朝 A（answer 对 A 广告 a_rtp，fd_rtp[0]
 * 收 A 报文）、[1]=朝 B（offer 对 B 广告 b_rtp，fd_rtp[1] 收 B 报文）；
 * b2b Leg 与侧号同号：leg0(A)→[0]、leg1(B)→[1]。
 * 无呼叫记录返回 NSBC_NOENT；bypass（无 relay）gate 恒 OPEN、SSRC/VQ 无值
 * （设备不过媒体，严禁估算）。诊断槽由媒体线程 __atomic 写入。 */
typedef struct {
    int valid;
    mb_mode_t mode;
    int gate_open[2];
    uint32_t ssrc_in[2];   int have_ssrc_in[2];  /* 入向学习主源 */
    uint32_t ssrc_out[2];  int have_ssrc_out[2]; /* 重写后发往对侧 */
    uint8_t  rr_fraction[2]; /* RTCP RR 首块：fraction lost 原值（/256） */
    uint32_t rr_lost[2];     /* 累计丢包 24bit */
    uint32_t rr_jitter[2];   /* 到达间隔抖动（媒体时钟 tick，未换算） */
    int have_rr[2];
    /* RTP seq-gap 本地推断（无 RTCP 时的退化数据源） */
    uint32_t rtp_expected[2];
    uint32_t rtp_lost[2];
    /* REQ-MEDIA-SEC-003⑤ P0：SSRC↔CNAME 绑定 + ssrc_unauthorized/collision */
    char     cname[2][64];
    int      have_cname[2];
    uint64_t ssrc_inject_drops[2];   /* ≈ ssrc_unauthorized（白名单外陌生 SSRC） */
    uint64_t ssrc_collision_drops[2];
} mb_view_t;
int bridge_call_view(bridge_mgr_t *m, void *call_key, mb_view_t *v);

/* OAM 活跃会话遍历（REQ-OAM-010 "show transcode"）：
 * 回调参数：call_key（不透明指针，仅用于 Web 端映射 call-id）、
 * mode（bypass/relay/transcode）、relay 端口对（bypass 时为 0）、
 * pkts/drops 计数；ud 透传。返回 NSBC_ERR 提前终止。 */
typedef int (*bridge_iter_cb)(void *call_key, mb_mode_t mode,
                              uint16_t a_rtp, uint16_t b_rtp,
                              uint64_t pkts, uint64_t drops, void *ud);
int bridge_foreach_call(bridge_mgr_t *m, bridge_iter_cb cb, void *ud);
/* 当前活跃会话数（只读，近似值——遍历期无锁） */
int bridge_active_count(bridge_mgr_t *m);

/* ---------- Phase 3 Playback Engine 内部桥接 API ---------- */

/* 查找 call_key 对应的 relay 实例 + 两侧 peer 地址。
 * 成功返回 NSBC_OK 并填 out_relay + out_peer；
 * bypass（无 relay）返回 NSBC_NOENT；未找到返回 NSBC_NOENT。
 * 只持有 shard 锁查询，不影响正常转发路径（和 bridge_call_view 同模式）。 */
int bridge_lookup_relay(bridge_mgr_t *m, void *call_key,
                        rtp_relay_t **out_relay, nsbc_sa_t out_peer[2]);

/* relay 内部 fd 访问（media 层内部用，opaque 类型查询） */
int rtp_relay_fd(const rtp_relay_t *r, int side);
/* relay 某侧是否已 latch 到真实 peer（1=已 latch，0=未 latch） */
int rtp_relay_latched(const rtp_relay_t *r, int side);
/* relay 某侧已 latch 的 peer 地址（未 latch 返回空 nsbc_sa_t） */
nsbc_sa_t rtp_relay_latch_addr(const rtp_relay_t *r, int side);

/* REQ-AI-001：桥接层注入 RTP payload（给 ai_dialog 等外部模块回送音频到通话）。
 * - 向 leg 对侧的真实 peer（comedia latch 优先，fallback peer 配置）sendto
 * - 自动构造 RTP 头（V=2/P=0/X=0/CC=0/M=0 + 指定 PT/SSRC + seq/timestamp）
 * - 调用方（ai_dialog）负责 G.711 PCMU/PCMA 帧本身；本层不做 codec 转换
 * - seq / ts / ts_inc：调用方维护每 call 的 RTP 序列号和时钟 tick；
 *   bridge_send_raw 每次调用后 ++*seq, *ts += ts_inc（PCMU 8kHz 20ms 帧 ts_inc=160）
 * - 返回实际 send 字节数（含 RTP 头），<0 表示失败
 * - 线程安全：shard 锁只用于查 relay（bridge_lookup_relay 内部），sendto 前释放；
 *   relay 内部 fd/地址在 setup 后运行时不变 —— 桥接层媒体线程和外部模块线程并发调安全
 * - **relay 不存在 / 还没 latch 时返回 -ENODEV**（调用方决定 retry 还是 drop） */
int bridge_send_raw(bridge_mgr_t *m, void *call_key, int leg,
                    const void *payload, size_t len, uint8_t pt,
                    uint32_t ssrc, uint16_t *seq, uint32_t *ts, uint32_t ts_inc);

/* REQ-AI-001：rec_feed_fn 动态 hook（ai_dialog / 未来 transcript 等可选模块注册）。
 * signaling 创建 bridge 时设 mb_cfg_t.rec_feed_fn 主槽（录音用），
 * 这里是不碰主槽的独立 hook 链表。媒体线程每次 payload 前先调主槽再遍历 hook。
 * hook 回调签名和 cfg.rec_feed_fn 相同：(ud, call_key, payload, len, leg, pt)。
 * bridge_add_rec_feed_hook / bridge_remove_rec_feed_hook 线程安全（持 rec_hook_lock）。
 * add 返回 hook 节点指针（调用方持引用，remove 时传同指针）；失败返回 NULL。
 * remove 返回 NSBC_OK/NSBC_ERR。 */
typedef void (*mb_rec_hook_fn)(void *ud, void *call_key, const void *payload,
                               size_t len, int leg, int pt);
void *bridge_add_rec_feed_hook(bridge_mgr_t *m, mb_rec_hook_fn fn, void *ud);
int   bridge_remove_rec_feed_hook(bridge_mgr_t *m, void *hook_ptr);

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
