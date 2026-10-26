#ifndef NSBC_B2BUA_H
#define NSBC_B2BUA_H

/* B2BUA 呼叫核心（REQ-SIP-003 等，P0）
 *
 * 两个独立 SIP 对话（Leg A=入向/UAS，Leg B=出向/UAC），各自独立
 * Call-ID/branch/CSeq/Via 序列；本层是唯一两侧状态机的接合点：
 *
 *   Leg FSM：IDLE → CALLING → EARLY → CONFIRMED → TERMINATING → TERMINATED
 *
 * - 100 Trying 本地立即发；18x/2xx 跨 leg 透传（SDP offer/answer）；
 * - glare（re-INVITE/UPDATE 同时）：sip_upd_guard + RFC3264/6141 退避回 491；
 * - CANCEL 仅终结 EARLY；CONFIRMED 后 BYE；B 侧 487/486/480 映射 A 侧同因；
 * - 媒体决策回调（bypass/relay/transcode）在两侧 SDP 齐备时触发一次；
 * - 呼叫稳定键 Call-ID（各 leg 独立）；呼叫表分片哈希 + slab。
 *
 * 与 poc 旧 call_session 的关系：旧单体（call_mgr/call_worker）保留为
 * 已验证快照不接线；本文件是生产 B2BUA 路径。
 */
#include "mem.h"
#include "net_sock.h"
#include "sip_dialog.h"
#include "sip_digest.h"
#include "sip_msg.h"
#include "sip_sess_timer.h"
#include "sip_txn.h"
#include "sip_transport.h"
#include "timer_wheel.h"
#include "chan_var.h"
#include "cdr_event.h"
#include "dialplan.h"   /* REQ-DP-SIP-001：sip_hdr_rewrite_t 嵌入 b2b_call_t */

#ifndef B2B_HDR_MAX
#define B2B_HDR_MAX 256
#endif
#ifndef B2B_SDP_MAX
/* WebRTC SDP（ICE candidates + DTLS fingerprint + 多 codec）audio-only
 * 约 3KB、audio+video 可达 8KB——传统 2048 会在路由前直接 488 */
#define B2B_SDP_MAX 8192
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LEG_IDLE = 0,
    LEG_CALLING,     /* INVITE 已发/已收，未得 1xx */
    LEG_EARLY,       /* 1xx（无 to-tag 的 100 不算） */
    LEG_CONFIRMED,   /* 2xx + ACK 完成 */
    LEG_TERMINATING,
    LEG_TERMINATED
} leg_state_t;

typedef struct b2b_core b2b_core_t;

/* 完整 b2b_leg_t（与 b2bua.c 内部定义 1:1 对齐，所有字段均在这里） */
typedef struct b2b_leg {
    leg_state_t state;
    sip_dialog_t *dlg;
    sip_txn_t *inv_txn;
    sip_txn_t *srv_txn;
    sip_txn_t *out_txn;
    sip_txn_t *cancel_txn;
    sip_tr_proto_t proto;
    int reliable;
    nsbc_sa_t dst;
    /* === NAT / Transport 元数据（REQ-NAT-002：resolve_cb 从 loc_binding 传入）===
     * 为什么不在 resolve_cb 里用 loc_binding_t 的 public_contact 替代？
     * 因为 public_contact 只改了 host:port，SDP 里的 a=candidate ICE candidate
     * 也需要 rewrite——那用的是 ext_rtp_ip（sip_profile 里的公网 RTP IP）。
     *
     * nat_detected=0 + Contact 也是私网 → 内网部署内网注册（如 10.0.0.x），
     * 不走 NAT rewrite，dst 直接用 Contact 转 sockaddr。
     * nat_detected=1 → 公网 SBC + 私网分机注册，dst 用 public_contact，
     * SDP 里的 o=/c=/a=candidate IP 用 ext_rtp_ip 改写。
     * nat_detected=0 + Contact 是公网 IP → 公网直连注册，都不用 rewrite。 */
    int nat_detected;
    char ext_rtp_ip[INET6_ADDRSTRLEN];   /* sip_profile.external_rtp_ip（NAT rewrite 用） */
    char callid[DLG_CALLID_MAX];
    char from[B2B_HDR_MAX];
    char to[B2B_HDR_MAX];
    char ruri[B2B_HDR_MAX];
    char target[B2B_HDR_MAX];
    char loc_tag[DLG_TAG_MAX];
    char rem_tag[DLG_TAG_MAX];
    /* peer_tag = 对端 dialog local tag（200 OK / 终响应的 To 头 tag）。
     * 与 rem_tag（dialog remote tag = 200 OK From 头 tag）严格区分：
     *   - rem_tag 给 BYE/INFO/MESSAGE/UPDATE 等 dialog 内请求的 To 头 tag 用
     *   - peer_tag 只给 ACK 用（ACK 确认的是 INVITE 的 200 OK/终响应，To tag 必须
     *     等于那个响应的 To 头 tag = 对端 UAS 的 local tag）
     * Bug 历史：之前 ACK 也错用 rem_tag → SIP.js 检查 ACK To tag ≠ dialog local tag →
     * 不认 ACK → Timer H 触发 200 OK 重传（死循环）。 */
    char peer_tag[DLG_TAG_MAX];
    char routes[DLG_ROUTE_MAX][DLG_URI_MAX];
    int nroutes;
    long cseq;
    int reinv;
    char last_branch[40];
    /* IMS Service-Route（RFC 3608）：resolve_cb 从 loc_binding_t 填入，
     * send_invite_b 出站时 prepend 到 Route 头链。值形如 "<sip:tenant_realm;lr>" */
    char service_route[256];
    char offer[B2B_SDP_MAX];
    size_t offer_len;
    char answer[B2B_SDP_MAX];
    size_t answer_len;
    sip_upd_guard_t guard;
    /* Session Timer (RFC 4028)：原始头存储 + 每腿独立协商 + 定时刷新/超时 */
    char session_expires[64];  /* 原始头值，如 "120;refresher=uac" 或协商后 "Session-Expires: N;refresher=xxx\r\n" */
    char min_se[32];           /* 原始 Min-SE 头值 */
    int  timer_supported;     /* 对端 Supported:timer 标记 */
    sess_timer_t st;
    tw_node_t   refresh_node;
    tw_node_t   expire_node;
    uint8_t     refresh_pending; /* refresh_cb 发出的 re-INVITE 未确认标记 */
    /* 100rel (RFC 3262) 可靠临时响应 */
    unsigned    local_rseq;     /* 本 leg 发的 reliable 1xx 的 RSeq（0=无 pending） */
    unsigned    peer_rseq;      /* 对端 leg 原始 RSeq（收到的 reliable 1xx） */

    /* Provisional 去重 guard（RFC 3261 §17.1.1：UDP 上对端会重传 provisional，
     * txn ICT 在 PROCEEDING 状态下每收到一个 provisional 都回调 TU——B2BUA 不能
     * 盲目转发给 A-leg，否则会重复发 180/183。记录最后一次已转发的 status，
     * 相同 status 的重传直接丢弃；不同 status（180→183）仍正常转发。
     * reliable 1xx（100rel）不受限——RSeq 递增代表新消息。 */
    int         last_prov_status;  /* 0 = 未转发过任何 provisional */

    /* === P1-12：INVITE 401/407 两轮挑战 per-call 存储（完全独立于 outreg_trunk_t） ===
     *
     * 为什么独立：outreg_build_auth_hdr() 接受 outreg_trunk_t*，但 INVITE 挑战可能
     * 发生在 TRUNK_AUTH_OUT / TRUNK_STATIC_PEER 上（这些 trunk 在 outreg 数组里没有
     * 实例）。更关键的是——如果把 INVITE 的 401 challenge nonce 写入 outreg_trunk_t.ch_*
     * 会覆盖 REGISTER 侧的 challenge 缓存，搞乱注册状态机。
     *
     * 因此：per-call 独立存储，ha1 从 trunk_t.password 现算（digest_make_ha1），
     * 直接调 digest_client_build_auth() 构建头值。两轮挑战通过 auth_flags bit 标记：
     *   bit0 = 407 Proxy-Authenticate 已收到并处理
     *   bit1 = 401 WWW-Authenticate 已收到并处理
     * 两轮都 set 时 → 重发 INVITE 同时带 Proxy-Authorization + Authorization 两种头。 */
    uint8_t     auth_flags;
    char        proxy_ch_realm[96];
    char        proxy_ch_nonce[128];
    char        proxy_ch_opaque[128];
    char        proxy_ch_qop[16];
    char        www_ch_realm[96];
    char        www_ch_nonce[128];
    char        www_ch_opaque[128];
    char        www_ch_qop[16];
    uint32_t    nc_counter;       /* Digest nc 计数器（每次发新 INVITE 必须递增） */
    char        www_auth_hdr[768];  /* "Authorization: Digest ..." 头值（不含前缀） */
    char        proxy_auth_hdr[768]; /* "Proxy-Authorization: Digest ..." 头值（不含前缀） */
    uint8_t     auth_attempts;     /* INVITE challenge 重发计数（上限 3 次，防死循环） */
} b2b_leg_t;

typedef struct call_link {
    struct call_link *next;
    struct b2b_call *call;
    int leg;
} call_link_t;

struct b2b_call {
    pthread_mutex_t lock;
    int dead;
    int media_done;
    int active;
    int a_final;
    /* ★ 谁发起的挂断：0=unknown（nbs内部处理），1=A腿(主叫/用户侧)发BYE，2=B腿(被叫/对端侧)发BYE
     * 在 b2bua.c SIP_M_BYE 入口处由收到 BYE 的 leg 决定；
     * CALL_END 事件 / CDR emit 时读此值填 term=A_UA / B_UA / LOCAL */
    int hangup_by;
    uint32_t dump_mark;
    tw_node_t noanswer_node;
    uint64_t start_ms;
    uint64_t answer_ms;
    int redirect_count;
    int privacy_history;
    int privacy_id;
    char history_info[1024];
    char orig_to[128];
    char clip[48];
    char orig_number[48];
    char dst_number[48];
    /* FreeSWITCH 风格精细化主叫覆盖（DP 透传自 dp_result_t）
     * orig_caller_id_num 纯 user part，host 自动走 trunk.resolve_from_host 或 local_host */
    char orig_caller_id_num[48];
    char orig_caller_id_name[48];
    /* FreeSWITCH effective_caller_id_* 系列：运行时合成值
     * 优先级链全部跑完后存入，供 CDR emit 记录最终生效的主叫号/名
     * effective_caller_id_number = From URI user part（最终值）
     * effective_caller_id_name   = From Display name（最终值，无则空串） */
    char effective_caller_id_num[48];
    char effective_caller_id_name[48];
    /* P2-15 inbound_extension 双记录（文档 §4.2.2 [4]） */
    char original_called[48];  /* 入站 INVITE 原始 RURI user（未改写前） */
    char trunk_called[48];     /* inbound_extension 改写后目标 */
    /* REQ-DP-SIP-001：拨号规则命中的 SIP 头改写全量（signaling.c 从 dres 透传） */
    sip_hdr_rewrite_t hdr_rw;
    /* REQ-MEDIA-DP-001：拨号规则级媒体模式 override（0=auto,1=bypass,2=relay）
     * media_bridge 在 bridge_decide 时从 call 里读，WebRTC 强制 SRTP_RELAY 不受影响 */
    uint8_t media_mode;
    /* ★ 拨号规则级 3GPP IMS 强制覆盖（从 dp_result_t 透传，三态：0=auto/1=开/2=关）
     * 优先级链：call->force_* (dialplan route) → call->prof->enable_* (profile) → 默认
     * 例：IMS 中继 route 强制 force_precondition=1，老 PSTN 中继强制 force_precondition=2 */
    uint8_t force_prack;
    uint8_t force_precondition;
    uint8_t force_timer;
    uint8_t force_gruu;
    char codec_a[32];
    char codec_b[32];
    uint8_t rtp_loss_pct;
    uint16_t rtp_jitter_ms;
    int32_t mos_x1000;
    uint64_t early_183_ms;
    uint64_t early_end_ms;
    uint32_t early_bytes;
    char b_key[128];
    int key_held;
    char rt_name[32];
    /* REQ-TRUNK-HDR-001：A 腿源 IP（纯 IP 字符串，无端口），
     * handle_new_invite 收到入站 INVITE 时从 src (nsbc_sa_t) 提取并存入。
     * 供 custom_headers ${from_ip} 变量替换使用。 */
    char from_ip[INET6_ADDRSTRLEN];
    /* Final recording file path (stereo mix when both legs recorded).
     * Filled by the recording fallback stop at call_destroy / sig_on_end_cb,
     * consumed by b2b_emit_cdr(END) so the CDR carries the recording path. */
    char rec_path[CDR_REC_MAX];
    void *rec[2];
    sip_txn_t *refer_txn;
    /* re-INVITE hold/unhold 标志：bitmask，bit0=leg0 hold，bit1=leg1 hold。
     * 入站 re-INVITE 带 c=0.0.0.0 / a=sendonly 时置位，对端腿 hold 也置位。 */
    uint8_t hold_legs;
    /* Emergency URN call flag (RFC 6881 / ETSI TS 183 041): set when the
     * inbound INVITE R-URI is urn:service:sos.* — written into the CDR
     * emergency bit by b2b_emit_cdr(END). */
    uint8_t emergency;
    /* 归属 SIP Profile（REQ-SIP-PROFILE-001）：handle_new_invite 按主叫
     * 源 IP 匹配（sip_profile_match_src），call_destroy 对称递减
     * active_calls。NULL = 无归属（外呼 originate / 无 ACL 配置）。
     * 指针安全：profile 数组内容加载后不可变，reload 后旧数组进墓地
     * 不释放——在途呼叫持有旧指针递减旧对象计数，新表重新累计。 */
    struct sip_profile *prof;
    /* B 腿出站 Profile：resolve 后根据 dp_layer 决定
     *   EXT/BIZ → prof（内部分机/队列，走 A 腿 profile 出站）
     *   OUT → external profile（中继/域外，走 external 出站）
     * 用于 send_invite_b 选 adv_host/adv_port，确保出站源地址正确。
     * NULL fallback → 同 prof。 */
    struct sip_profile *out_prof;
    chan_var_t *vars;
    b2b_core_t *core;
    b2b_leg_t leg[2];
    call_link_t link[2];
    /* REQ-ORIGINATE-001：per-call noanswer timer override（毫秒）。
     * >0 覆盖 co->cfg.ring_noanswer_ms；<=0 走全局（memset 默认 0 不影响入站 INVITE）。 */
    int noanswer_ms_override;
    /* REQ-ORIGINATE-003 待补：originate 尾随 app（&park/&hangup/&echo）。
     * 取值 B2B_APP_*；B2B_APP_NONE 表示普通外呼（无后置动作）。 */
    int post_app;
    /* REQ-MEDIA-MUTE-001：per-leg mute 标志（uuid_mute）。
     * muted[0]=1 → A 腿侧收的音频被静音（发往 B 腿时清零）；
     * muted[1]=1 → B 腿侧收的音频被静音（发往 A 腿时清零）。
     * 媒体线程 relay_recv_one 转发前查此字段；0/1 值直接 memset(0) payload。
     * 非原子——整个 call 的状态修改靠 call->lock（OAM 线程持锁写、媒体线
     * 程读时无锁但 int 自然对齐原子写；极端竞态下一帧可能漏 mute/多 mute
     * 一帧，可忽略）。 */
    int muted[2];
    /* signaling.c CDR END 链路填完完整 cdr_event_t 后，
     * 调 cdr_event_to_json() 存完整 JSON 字符串——b2bua.c 后续
     * 推 UDS/SESSION_END 事件时直接 memcpy 复用，不另造残缺 snprintf。
     * 1024 足够容纳 codec/IP/媒体质量等全部字段；
     * 超长时 snprintf 截断（剩余字段 P0 降级不影响主流程）。 */
    char cdr_json[1024];
    /* IVR session opaque pointer — set by dialplan 'ivr' app when the call
     * enters IVR menu. media_bridge relay_recv_one uses it to deliver RFC4733
     * DTMF digits via ivr_on_dtmf(). NULL = no IVR active on this call. */
    void *ivr_sess;
    /* REQ-SEC-006：dialplan 规则命中标记（signaling.c dp_execute 返回值透传）。
     * 0=NSBC_ERR 没命中任何规则 → 扫号/未知来源 → CDR 里 is_malicious=1
     * 1=NSBC_OK 命中规则 → 合法业务。PERM_DENIED 走 silent drop 不 emit CDR。 */
    uint8_t dp_matched;
    /* 命中的规则名（可选，仅日志/审计用，CDR threat 诊断时看）。空串=没命中。 */
    char dp_rule_name[64];
};
typedef struct b2b_call b2b_call_t;

/* 号码解析：由 dialplan 模块注册；成功填 dst(网络地址) 与新 R-URI */
typedef int (*b2b_resolve_fn)(b2b_call_t *call, const char *ruri,
                              size_t ruri_len, nsbc_sa_t *dst,
                              char *out_ruri, size_t outsz,
                              char *out_to, size_t out_to_sz, void *ud);

/* 媒体决策：两侧 SDP 齐备时调用一次，返回模式（见 media/bridge） */
typedef enum { B2B_M_BYPASS = 0, B2B_M_RELAY, B2B_M_TRANSCODE }
    b2b_media_mode_t;
typedef int (*b2b_media_fn)(b2b_call_t *call, const char *sdp_a,
                            size_t sdp_a_len, const char *sdp_b,
                            size_t sdp_b_len, void *ud);

/* 单 Leg 媒体面只读视图（REQ-MEDIA-SEC-003⑤ P0 / REQ-OBS-010 P1）。
 * 纯标量中立结构：b2b 层不依赖 media，由组合根（signaling）查询 bridge
 * 并翻译填入；valid=0 表示媒体层无此呼叫记录（uuid_dump 输出 UNAVAILABLE，
 * 严禁估算）。bypass Leg：gate_open=1 但 SSRC/VQ 均无值（设备不过媒体）。
 * ssrc_in = 该 Leg 对端入向学习主源；ssrc_out = 本端重写后发往该 Leg 的
 * 主源 SSRC。VQ 取该 Leg 对端 RTCP SR/RR 首块：fraction 为 0..255 原值
 * （/256），lost 为累计丢包，jitter 为媒体时钟 tick（未做 ms 换算）。 */
typedef struct {
    int valid;
    b2b_media_mode_t mode;
    int gate_open;
    uint32_t ssrc_in;
    int have_ssrc_in;
    uint32_t ssrc_out;
    int have_ssrc_out;
    uint8_t vq_fraction;
    uint32_t vq_lost;
    uint32_t vq_jitter;
    int have_vq;
    /* REQ-MEDIA-SEC-003⑤ P0：SSRC↔CNAME 绑定 + 安全计数（单侧视角） */
    char cname[64];
    int have_cname;
    uint64_t ssrc_inject_drops;
    uint64_t ssrc_collision_drops;
} b2b_media_view_t;
/* uuid_dump 持 call 锁时回调（锁序 call→media 与 offer/answer hook 一致）；
 * call_key 不透明，媒体层只以指针值查表。无记录返回 NSBC_NOENT。 */
typedef int (*b2b_media_view_fn)(void *call_key, int b_leg,
                                 b2b_media_view_t *view, void *ud);
/* SDP 出口重写（relay 端口锚定）：返回写出长度或 NSBC_* 错误。
 * call_key 为不透明呼叫身份键（内部即 b2b_call_t*）：跨层实现方
 * （如 media）只允许以指针值做表键/比较，禁止解引用（REQ-ARCH-011）。 */
typedef int (*b2b_sdp_hook_fn)(void *call_key, const char *in,
                               size_t inlen, char *out, size_t outsz,
                               void *ud);
typedef void (*b2b_end_hook_fn)(void *call_key, void *ud);

/* 无主响应旁路 */
typedef void (*b2b_resp_hook_fn)(const char *call_id, int status,
                                 const char *www_auth_hdr, void *ud);

/* REGISTER 交给 registrar 模块（返回 NSBC_OK 表示已处理）
 * txn 是 server 侧 NIST/IST 事务指针，registrar 可用 txn_send_response
 * 让 txn 自动吸收后续重传；NULL 表示无事务（stateless 模式） */
typedef int (*b2b_register_fn)(sip_tr_proto_t proto, const nsbc_sa_t *src,
                               sip_msg_t *msg, const char *raw, size_t rawlen,
                               sip_txn_t *txn, void *ud);

typedef struct {
    sip_transport_t *tr;
    dlg_mgr_t *dlg;
    txn_mgr_t *txn;
    tw_t *tw;
    mem_arena_t *sig_arena;
    mem_arena_t *pkt_arena;
    int capacity;
    /* 本侧监听身份 */
    const char *local_host;       /* 出向 Via/Contact 主机（NAT 场景下被 external_sip_ip 覆盖） */
    int local_udp_port;
    /* === NAT 出站改写（REQ-NAT-001）===
     * 空字符串=关闭（走 local_host/local_udp_port）。
     * FreeSWITCH ext-sip-ip / ext-rtp-ip 等效。 */
    const char *external_sip_ip;  /* 出向 Via/Contact/Record-Route 改写为此公网 IP */
    const char *external_rtp_ip;  /* 出向 SDP o=/c= 私网 IP 改写为此公网 RTP IP；空=用 external_sip_ip */
    const char *realm;
    const char *digest_secret;
    b2b_resolve_fn resolve;
    void *resolve_ud;
    b2b_media_fn media;
    void *media_ud;
    /* on_end 单槽（signaling 自己的 rt_release + bridge view）
     * + 多槽链 b2b_call_destroy_add 供其他模块 */
    b2b_end_hook_fn on_end;
    void *end_ud;
    /* === 以下全部为全局多槽 hook 链，模块 init 时调 b2b_*_add 注册 ===
     * offer_hook / answer_hook / on_register / on_unmatched_resp /
     * cdr_emit / media_view —— 都从 cfg 单槽改成了 b2bua.c 全局 hook 链 */
    /* PBX 基础 */
    uint32_t ring_noanswer_ms;
    int auto_redirect_302;
    int max_redirects;
    /* 并发硬限制 */
    uint32_t max_concurrent_calls;
    uint32_t max_inflight_per_aor;
    /* 早期媒体 */
    uint32_t early_media_max_s;
    /* P1 原语基础设施 */
    void *rec_mgr;
    int  (*bridge_ctrl)(void *bridge_ud, void *call_key, int op,
                        const char *arg, void *ptr_arg);
    void *bridge_ctrl_ud;
    const char *user_agent;
    /* Session Timer (RFC 4028) 全局配置 */
    sess_timer_cfg_t sess_timer_cfg;
    int sess_timer_enabled;  /* 0=关闭(仅透传)，1=完整协商+刷新+超时 */
    /* GRUU（RFC 5627 / RFC 7315）：全局唯一 instance-id
     * 空字符串则不拼 ;+sip.instance 参数；YAML 配或启动时自动生成 UUID */
    char gruu_instance_id[64];
    /* P1-12 INVITE 401/407 challenge：route_table 指针（b2bua.c cli_tu 里拿 trunk_t.password 现算 HA1） */
    const struct route_table *rt;
} b2b_cfg_t;

int b2b_core_create(const b2b_cfg_t *cfg, b2b_core_t **out);
void b2b_core_destroy(b2b_core_t *c);

/* 直接挂到 sip_transport 的 rx 回调签名 */
void b2b_on_rx(void *ud, sip_tr_proto_t proto, const nsbc_sa_t *src,
               const nsbc_sa_t *local, const char *buf, size_t len);

/* 挂到 txn_mgr 的服务端事务统一上报口（NEW_REQUEST / ACK 等） */
void b2b_server_tu(sip_txn_t *t, const sip_msg_t *m, txn_event_t ev,
                  void *ud);

/* Phase 5 XL: INVITE 入站默认处理骨架（从 endpoint_registry fallback 调用）
 * 完整包含：profile 匹配、trunk 四层匹配、Session Timer、resolve_cb 路由、B 腿构造。
 * 参数来自 b2b_server_tu：co=ud, t=srv_txn, m=sip_msg, src=peer_addr, local_port=入端口 */
void handle_new_invite(b2b_core_t *co, sip_txn_t *t, const sip_msg_t *m,
                       sip_tr_proto_t proto, const nsbc_sa_t *src, uint16_t local_port);

/* Phase 5 XL: endpoint_registry 分发时传给 handle_invite 的上下文
 *
 * 签名设计：endpoint_ops_t.handle_invite(void *ctx, b2b_call_t *c)
 * SIP endpoint accept 时返回 out_ctx=NULL（不需要造私有 ctx），
 * 然后 b2b_server_tu 自己把所有 INVITE 处理需要的参数打包成此结构传下去。
 *
 * SIP endpoint handle_invite 内部解包 → 调 handle_new_invite()。 */
typedef struct {
    b2b_core_t     *co;        /* b2b 核心（含 cfg.resolve 路由钩子）*/
    sip_txn_t      *t;         /* 服务端 INVITE 事务 */
    const sip_msg_t *m;        /* 入站 SIP 报文 */
    sip_tr_proto_t  proto;     /* 传输协议（UDP/TCP/TLS/WS/WSS）*/
    const nsbc_sa_t *src;      /* 对端地址 */
    uint16_t        local_port;/* 本地监听端口（profile 匹配用）*/
} sip_invite_ctx_t;

/* REQ-ORIGINATE-003：originate 尾随 app 类型。
 * 语法 originate <dest> &bridge(url)：bridge 走顺序外呼（bridge_url 字段）；
 * &park/&hangup/&echo 走 post_app：B 腿 answer 后在 OAM 线程执行后置动作。 */
#define B2B_APP_NONE   0   /* 无后置动作（默认） */
#define B2B_APP_PARK   1   /* answer 后 hold 双腿（暂停媒体） */
#define B2B_APP_HANGUP 2   /* answer 后立即挂断 */
#define B2B_APP_ECHO   3   /* answer 后开启 RTP 环回（两侧各自 side→side，听自己回声） */

/* 主动外呼（originate，REQ-SIP-012）：从 dialplan/API 触发 */
typedef struct {
    const char *from_user;
    const char *to_uri;
    const char *sdp;
    size_t sdp_len;
    nsbc_sa_t dst;
    /* REQ-ORIGINATE-001：per-call 超时（毫秒，0 或负数 = 用全局 ring_noanswer_ms，
     * 正数 = 覆盖全局）。arm_noanswer_timer() 会自动读 call->noanswer_ms_override。
     * 对应 FS 变量 originate_timeout（秒，我们自动 ×1000）。 */
    int timeout_ms;
    /* REQ-ORIGINATE-001：可选主叫号覆盖（替换 from_user）。
     * 空串/NULL = 用 from_user。仅影响 originate 构造的 From/Contact 头。
     * 对应 FS 变量 origination_caller_id_number / effective_caller_id_number。
     * 两者同时设置时 effective_caller_id_number 优先（SIP From user），
     * origination_caller_id_number 用于 Caller-Id 显示。 */
    const char *orig_caller_id;
    /* REQ-ORIGINATE-002：From 显示名（Caller-Id name），对应 FS origination_caller_id_name。
     * 非空时拼 From 头为 "\"name\" <sip:user@host>"；空时退化为 user@host。 */
    const char *orig_caller_id_name;
    /* REQ-ORIGINATE-003：bridge app — 顺序外呼第二腿 dest。
     * 非空时 b2b_originate 走 bridge 模式：
     *   1. 先呼 leg[0]（主 dest，to_uri）
     *   2. 等 leg[0].answer（默认 60s 超时，走 timeout_ms 覆盖）
     *   3. 再呼 leg[1]（bridge_dst，经 cfg.resolve）
     *   4. 双腿都 answer → media_bridge 自然桥接
     * 解析自 FS &bridge(url)；&park 等其他 app 时此字段为 NULL。 */
    const char *bridge_url;
    nsbc_sa_t bridge_dst;
    /* REQ-ORIGINATE-003 待补：尾随 app（&park/&hangup/&echo）。
     * 与 bridge_url 互斥（语法只有一个 app）：bridge_url 非空时走 bridge 顺序外呼；
     * 否则按 post_app 在 B 腿 answer 后执行 park(hold 双腿)/hangup(terminate)。 */
    int post_app;
    /* REQ-ORIGINATE-004：originate 自动 SDP 的 codec 控制。
     * 由 cmd_originate 从 FS brace vars {audio_codecs=PCMU,G722} /
     * {video_codecs=H264} 解析填入；空串 = 用默认（PCMU 单音频，无 video）。
     * b2b_originate auto_offer 生成时根据这两个字段决定：
     *   - audio_codecs 非空 → 生成多个 a=rtpmap + m=audio 行的 PT 列表
     *   - video_codecs 非空 → 生成 m=video 行（端口 0 + video RTCP mux inactive 模式，
     *     offer_hook 会自动把端口改成非 0 并加 ICE/DTLS/SSRC）
     *
     * 约束：最多 4 个 codec，每个 codec 名 ≤ 16 字节（避免 auto_offer snprintf 溢出）。
     * 只对 o.sdp_len==0（auto_offer 路径）生效；调用方自带完整 o.sdp 时这两个字段被忽略。 */
    char audio_codecs[128];
    char video_codecs[128];
} b2b_orig_t;
int b2b_originate(b2b_core_t *c, const b2b_orig_t *o, b2b_call_t **out);

/* originate 目标解析（REQ-SIP-012）：经核心装配的 cfg.resolve 把
 * call-url 解析为网络地址 + 改写后 R-URI/To；resolve 未装配返回
 * NSBC_ERR。b2b_core 对 oam 层不透明，call_commands 经此口路由。 */
int b2b_resolve_dst(b2b_core_t *c, const char *ruri, size_t ruri_len,
                    nsbc_sa_t *dst, char *out_ruri, size_t outsz,
                    char *out_to, size_t out_to_sz);

/* 主动挂断（REQ-OAM 呼叫控制） */
int b2b_terminate(b2b_call_t *call, int cause_code);

/* 按 Call-ID 强制挂断（REQ-OAM-004，匹配任一 Leg）：
 * 找不到/已结束返回 NSBC_NOENT；参数非法 NSBC_INVAL。
 * 供 obs_control `hangup <call-id>` 等控制面调用，全程审计在调用方。 */
int b2b_terminate_callid(b2b_core_t *c, const char *callid, size_t len,
                         int cause_code);

/* CANCEL 早期 INVITE（uuid_cancel）：
 *   leg=-1 双腿都取消；0=仅 A；1=仅 B。
 *   腿状态为 CALLING/EARLY 才发 CANCEL，已 CONFIRMED/TERMINATED 返回 NOENT。
 *   底层调 static send_cancel_req()。 */
int b2b_cancel_call(b2b_call_t *call, int leg);

/* Mute 通话侧音频（uuid_mute）：
 *   leg=-1 双腿都 mute/unmute；0=仅 A；1=仅 B。
 *   on=1 静音（RTP payload 清零），on=0 恢复。
 *   改 b2b_call_t.muted[] 标志，媒体线程下一个 relay_recv_one 自然生效。 */
int b2b_mute_call(b2b_call_t *call, int leg, int on);

leg_state_t b2b_leg_state(b2b_call_t *call, int b_leg /*0=A 1=B*/);
uint64_t b2b_active_calls(b2b_core_t *c);
uint64_t b2b_total_calls(b2b_core_t *c);
uint64_t b2b_failed_calls(b2b_core_t *c);

/* 在线诊断快照（REQ-OAM-007 uuid_dump / show channels 明细，
 * REQ-OBS-010 双侧 Leg 视图）。文本渲染进调用方缓冲区：
 *   b2b_list_calls：每呼叫一行（A/B Leg Call-ID+状态），尾行汇总；
 *   b2b_dump_call：按任一 Leg Call-ID 定位，逐 Leg 输出状态机/CSeq/
 *     tag/协商 SDP（长度分帧）；媒体行 mode/gate/ssrc_in/ssrc_out/vq 经
 *     media_view 回调取真值，无媒体记录或 bypass Leg 的 SSRC/VQ 输出
 *     UNAVAILABLE，严禁估算值（REQ-MEDIA-SEC-003⑤/REQ-OBS-010）。
 * 返回 NSBC_OK / NSBC_NOENT / NSBC_INVAL / NSBC_NOSPC（缓冲不足）。 */
int b2b_list_calls(b2b_core_t *c, char *out, size_t outsz);
int b2b_dump_call(b2b_core_t *c, const char *callid, size_t len,
                  char *out, size_t outsz);

/* 供事务层发送应答（内部用，media/dialplan 模块也可用） */
int b2b_respond_leg(b2b_call_t *call, int b_leg, int status,
                    const char *extra, const char *body, size_t bodylen);

/* ---- API 层呼叫控制（cmd_registry 调用） ---- */

/* 按任一 Leg Call-ID 定位呼叫（供 cmd_registry + obs_control 使用）；
 * 不在头文件声明是历史遗留，本次补齐修复隐式声明隐患。 */
b2b_call_t *b2b_find_callid(b2b_core_t *co, const char *callid, size_t len);

/* 向通话注入 DTMF（REQ-CCMD-001，uuid_send_dtmf） */
int b2b_send_dtmf(b2b_call_t *call, int b_leg, const char *digits, size_t len,
                  int volume, uint16_t dur_ms);

/* 对话内发送 MESSAGE（REQ-CCMD-001，uuid_send_msg / send_msg）：
 *   call     : 目标呼叫
 *   leg      : 发起方向（0=A 腿/入呼侧，1=B 腿/出呼侧；-1=自动选对端）
 *   body     : MESSAGE 消息体（默认 Content-Type: text/plain）
 *   body_len : body 长度
 *   异步触发 txn_client_new(TXN_NICT) 转发，CLI 立即返回 OK/ERR；
 *   找不到呼叫/腿未接通 NSBC_NOENT；guard 忙 NSBC_AGAIN；
 *   资源不足 NSBC_ERR。 */
int b2b_send_message(b2b_call_t *call, int leg,
                     const char *body, size_t body_len);

/* ---- API 层呼叫控制续（REQ-CCMD-001 cmd_registry 命令组） ----
 * 与 b2b_find_callid 同批补齐：b2bua.c 已有实现，此前不在头文件
 * 声明是历史遗留，call_commands.c 隐式声明有编译隐患，本次补齐。 */

/* Call-ID 白名单校验（各 uuid_* 命令共用入校：可见 ASCII、非空） */
int b2b_valid_callid(const char *s, size_t len);

/* Pre-Answer（§6 Early Media）：向 A 腿发 183 Session Progress（带 hold SDP），
 * 不发 200 OK（避免计费触发）。B2BUA 状态机转到 PRE_ANSWERED（复用
 * CALL_STATE_EARLY + pre_answer_sent 标志）。media_bridge 可提前介入。
 * 成功后后续的 playback / media 操作可在未接通状态执行。 */
int b2b_pre_answer_call(b2b_call_t *call);

/* ---- Call-Destroy 多槽 Hook（REQ-MEDIA-SEC-004 扩展）----
 *
 * 多个模块（recording / ai_dialog / event_handlers / ...）都需要在 call_destroy
 * 时收到通知，b2b_cfg_t.on_end 单槽不够。这里提供独立的多槽 hook 数组：
 *   b2b_call_destroy_cb：回调类型（call_key 即 b2b_call_t*，ud 注册时传入）
 *   b2b_call_destroy_add：注册，返回 NSBC_OK / NSBC_NOSPC (满)
 *   b2b_call_destroy_rm ：卸载（传注册时的 fn+ud 对；rm_all=1 时清空）
 *
 * 回调时机：call_destroy 内部（rec_mgr stop 之前、slab 归还之前）。
 * 锁序：call_destroy 已持 call 锁（各 hook 回调必须尽快返回，禁阻塞）。 */
#define B2B_END_HOOK_MAX  32
typedef void (*b2b_call_destroy_cb)(void *call_key, int a_final, void *ud);
int  b2b_call_destroy_add(b2b_call_destroy_cb fn, void *ud);
void b2b_call_destroy_rm(b2b_call_destroy_cb fn, void *ud);
void b2b_call_destroy_rm_all(void);

/* ---- SDP Hook 多槽链 ----
 * A腿 offer 发往 B 前 / B腿 answer (2xx) 发往 A 前。
 * 所有注册回调依次执行，输出缓冲区接力传递：
 *   第一个收到原始 SDP 写入 out[0]；
 *   下一个如果 out[0] 空则再吃原始，否则吃上一个的输出。
 * 各模块（media_bridge, security, ...）init 时自注册。
 * 返回 NSBC_OK / NSBC_NOSPC (满 B2B_SDP_HOOK_MAX) */
#define B2B_SDP_HOOK_MAX  16
typedef enum { B2B_SDP_OFFER = 0, B2B_SDP_ANSWER } b2b_sdp_hook_kind_t;
int  b2b_sdp_hook_add(b2b_sdp_hook_kind_t kind, b2b_sdp_hook_fn fn, void *ud);
void b2b_sdp_hook_rm(b2b_sdp_hook_kind_t kind, b2b_sdp_hook_fn fn, void *ud);

/* ---- REGISTER 多槽链 ----
 * b2bua 收到 REGISTER 后遍历所有注册模块直到某个返回 NSBC_OK。
 * registrar init 时自注册；security 模块可插前置检查返回 PERM_DENIED。 */
#define B2B_REGISTER_HOOK_MAX  8
int  b2b_register_hook_add(b2b_register_fn fn, void *ud);
void b2b_register_hook_rm(b2b_register_fn fn, void *ud);

/* ---- IP guard 多槽链（防暴力破解/恶意扫描，security 模块注册） ----
 * 新 INVITE 入口检查 + REGISTER 认证失败上报。INVITE 事件返回
 * NSBC_PERM_DENIED → 调用方静默丢弃（不回任何响应，防 SBC 指纹暴露）；
 * AUTH_FAIL 为纯上报，返回值忽略。REGISTER 侧封禁不走此链——
 * security 的 sec_reg_cb 已在 REGISTER 链首自查。 */
#define B2B_GUARD_HOOK_MAX  8
typedef enum {
    B2B_GUARD_INVITE = 0,        /* 新 INVITE 入口：PERM_DENIED → 静默丢 */
    B2B_GUARD_AUTH_FAIL,         /* REGISTER 认证失败（registrar 403 路径） */
    B2B_GUARD_AUTH_OK,           /* REGISTER 认证成功（清零 fail/ratelimit 计数） */
    B2B_GUARD_INVITE_FAIL,       /* 被动：INVITE/REGISTER 事务层分配失败（slab 满/NOMEM/NOSPC） */
    B2B_GUARD_INVITE_COUNT,      /* 主动：新 INVITE 到达 txn 层之前计数（per-IP 频率） */
} b2b_guard_evt_t;
typedef int (*b2b_guard_fn)(const char *src_ip, b2b_guard_evt_t evt, void *ud);
int  b2b_guard_hook_add(b2b_guard_fn fn, void *ud);
void b2b_guard_hook_rm(b2b_guard_fn fn, void *ud);
/* 入口检查：任一钩子返回 PERM_DENIED → 拦截 */
int  b2b_guard_check(const char *src_ip, b2b_guard_evt_t evt);
/* 事件上报：遍历钩子，忽略返回值 */
void b2b_guard_notify(const char *src_ip, b2b_guard_evt_t evt);

/* ---- 无主响应旁路多槽链 ----
 * REGISTER 的 200/401/407 等不经过本端事务层的响应 */
#define B2B_RESP_HOOK_MAX  8
int  b2b_resp_hook_add(b2b_resp_hook_fn fn, void *ud);
void b2b_resp_hook_rm(b2b_resp_hook_fn fn, void *ud);

/* ---- CDR 多槽链 ----
 * call_end 时所有注册的 CDR sink 都收到事件 */
typedef void (*b2b_cdr_hook_fn)(void *ud, const cdr_event_t *ev);
#define B2B_CDR_HOOK_MAX  8
int  b2b_cdr_hook_add(b2b_cdr_hook_fn fn, void *ud);
void b2b_cdr_hook_rm(b2b_cdr_hook_fn fn, void *ud);

/* ---- Media View 多槽链 ----
 * uuid_dump 等查媒体状态时各模块都可返回 view */
int  b2b_media_view_hook_add(b2b_media_view_fn fn, void *ud);
void b2b_media_view_hook_rm(b2b_media_view_fn fn, void *ud);

/* 应答（200 OK）/ 保持 / 盲转（uuid_answer/uuid_hold/uuid_transfer） */
int b2b_answer_call(b2b_call_t *call, int b_leg,
                    const char *sdp, size_t sdp_len);
int b2b_hold_call(b2b_call_t *call, int b_leg, int hold,
                  const char *sdp, size_t sdp_len);
int b2b_transfer_call(b2b_call_t *call, int b_leg, const char *dest,
                      int kind, b2b_call_t *consult_call,
                      const char *replaces_hdr);

/* 桥接两路呼叫 / 注入音频 / 打断（uuid_bridge/uuid_broadcast/uuid_break） */
int b2b_bridge(b2b_core_t *co, const char *callid_a, const char *callid_b);
int b2b_broadcast_start(b2b_call_t *call, int b_leg, const char *wav_path);
int b2b_broadcast_break(b2b_call_t *call, int b_leg);

/* 录制起停（uuid_record；mode 取 recording.h rec_mode_t 值） */
int b2b_record_start(b2b_call_t *call, int mode,
                     const char *path, const char *reason);
int b2b_record_stop(b2b_call_t *call, const char *reason,
                    char *path_out, size_t path_cap);

/* 通道变量读写（uuid_getvar/uuid_setvar）+ 综合状态（status） */
int b2b_call_get_var(b2b_call_t *call, int leg, const char *key,
                     char *out, size_t out_cap);
int b2b_call_set_var(b2b_call_t *call, int leg, const char *key,
                     const char *value);
int b2b_status(b2b_core_t *co, char *out, size_t outsz);

/* call 粒度锁：OAM/命令线程读 call 字段必须持有（锁序 call→media，
 * 与 offer/answer hook 一致）。originate 等回读 leg callid 用。 */
void call_lock(b2b_call_t *c);
void call_unlock(b2b_call_t *c);

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
