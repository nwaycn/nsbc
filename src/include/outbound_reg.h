/* NSBC → 外部 IMS / SIP 中继 出局注册（REQ-SIP-OUT-001，P1）
 *
 * trunk_t.kind == TRUNK_REGISTER 时，本模块作为 UAC 向对端 P-CSCF / 注册服务器
 * 发 REGISTER；处理 401/407 质询（Digest 鉴权，复用 trunk_t.auth_user / realm）、
 * 成功后按 Expires 默认 3600s 续注、403/超时→FAILED 等指数退避重连。
 *
 * 依赖链：
 *   mod/outbound_reg → sip_build / sip_transport / sip_digest / timer_wheel / route_table
 *   目录方向合规（mod 在 registrar/oam/ha 同层）。 */
#ifndef NSBC_OUTBOUND_REG_H
#define NSBC_OUTBOUND_REG_H

#include "nsbc_common.h"
#include "app_ctx.h"       /* nsbc_ctx_t */
#include "timer_wheel.h"   /* tw_node_t */
#include "sip_transport.h"
#include "sip_digest.h"
#include "route_table.h"   /* trunk_t —— reload API 参数类型 */

#ifdef __cplusplus
extern "C" {
#endif

/* per-trunk 注册状态机（简单 FSM，状态转换见 state.c diagram） */
typedef enum {
    OR_IDLE = 0,    /* 未启动 / trunk disabled */
    OR_REGISTERING, /* 已发 REGISTER 首包 / 等待 401 或 200 */
    OR_REGISTERED,  /* 200 OK 已收，计时器待续注 */
    OR_CHALLENGED,  /* 收到 401/407，等待鉴权后重发 */
    OR_FAILED,      /* 403 / 6xx / 指数退避冷却期 */
    OR_RETRY_WAIT,  /* 退避中，next_send_ms 到期后重新发 REGISTER */
    OR_DISABLED,    /* reload enabled=0，停所有 timer，发 Expires=0（专家 3.6） */
    OR_INVALID,     /* reload profile 不存在 / transport 未就绪（专家 2.6） */
} or_state_t;

/* 专家 1.3：与 or_state_t 正交的健康状态（OPTIONS 探测） */
typedef enum {
    GW_HEALTH_HEALTHY     = 0,
    GW_HEALTH_DEGRADED    = 1,
    GW_HEALTH_UNREACHABLE = 2,
} gw_health_t;

/* 号码池解析上限（专家文档 §2.2） */
#ifndef GW_POOL_MAX
#define GW_POOL_MAX (32)
#endif

typedef struct outreg_trunk {
    char          name[64];       /* trunk 名（匹配 route_table） */
    or_state_t    state;
    uint32_t      cseq;
    uint32_t      expires;        /* 对端下发，默认 3600（运行时） */
    uint32_t      retry_backoff_ms;
    uint32_t      next_send_ms;
    /* === 注册失败诊断（RETRY_WAIT 路径用，200 OK 时清零） ===
     * reg_fail_count：连续失败次数（每次 403/5xx/6xx/超时 +1，challenge 不计入）
     * first_fail_ms：首次失败的单调时钟 ms（只在 0→第一次失败时写一次，
     *                方便算出"已挂 X 秒"；200 OK 成功时清零） */
    uint32_t      reg_fail_count;
    uint64_t      first_fail_ms;
    char          call_id[64];
    char          contact[256];   /* Contact: <sip:auth_user@local_host:port> */
    tw_node_t     timer;
    int           leg;            /* 0=主进程 init 序号 */
    uint8_t       unregistering;  /* 1=已发 Expires=0 REGISTER，等对端应答 */

    /* ---- Digest 鉴权 ---- */
    char          auth_user[64];            /* 副本（trunk_t.auth_user） */
    char          realm[96];                /* 副本（trunk_t.realm） */
    char          ha1[SIP_RESP_LEN];        /* 预计算 HA1（init 时从 cfg password/ha1 键加载） */
    int           ha1_set;                  /* 1=ha1 已计算，0=无凭证 */
    /* 401/407 challenge 缓存（下次 send_register 用）
     * 专家 2.1：两套独立缓存 —— INVITE 可能同时收到 407（Proxy-Authenticate）和 401（WWW-Authenticate） */
    char          ch_realm[96];
    char          ch_nonce[128];
    char          ch_opaque[128];
    char          ch_qop[16];               /* "auth" / "auth-int" / "" */
    uint32_t      nc_counter;               /* Digest nc 计数器（每发一次 +1，8 hex 零填充） */
    char          auth_hdr[768];            /* 完整 Authorization 头（不含 "Authorization: " 前缀） */

    /* Proxy-Authenticate 407 挑战缓存 —— 用于 Proxy-Authorization 头（INVITE 第二轮） */
    char          p_realm[96];
    char          p_nonce[128];
    char          p_opaque[128];
    char          p_qop[16];
    uint32_t      p_nc_counter;             /* Proxy 路径独立的 nc 计数器 */
    char          proxy_auth_hdr[768];      /* 完整 Proxy-Authorization 头（不含 "Proxy-Authorization: " 前缀） */

    /* === FreeSWITCH 兼容配置字段（cfg_ 前缀，从 trunk_t 复制，专家 3.3） === */
    uint32_t      cfg_expire_sec;       /* 期望过期时间（DB 配置，默认 3600） */
    uint32_t      cfg_retry_sec;        /* 退避基数（DB 配置，默认 5s） */
    uint32_t      cfg_ping_sec;         /* OPTIONS 探测间隔（DB 配置，0=关闭） */
    char          cfg_from_user[64];
    char          cfg_from_domain[128];
    char          cfg_proxy[128];
    char          cfg_register_proxy[128];
    char          cfg_contact_params[128];
    uint8_t       cfg_caller_id_in_from;
    uint8_t       cfg_extension_in_contact;
    char          cfg_inbound_ext[64];
    uint8_t       cfg_rfc5626;
    char          cfg_reg_id[32];
    char          cfg_peer_ip[256];
    char          profile_name[64];          /* 绑定的 SIP Profile（专家 2.7 前置校验） */

    /* === 用户新增：号码池 === */
    char          cfg_caller_pool[1024];     /* 逗号分隔号码池原文 */
    uint32_t      cfg_per_num_max;           /* 单号码最大并发（0=不限） */
    char          pool_caller[GW_POOL_MAX][64]; /* 解析后的号码池 */
    uint32_t      pool_count;                   /* pool_caller 有效数量 */
    uint32_t      per_num_active[GW_POOL_MAX];  /* 每号码当前并发 */
    uint32_t      total_active;                 /* trunk 总并发（gateway 级） */

    /* === 健康探测（专家 1.3：正交于注册状态机，独立 timer） ===
     * ping_fail_count 递增策略（文档 §3.2）：
     *   连续超时 >= 3  → health_state = DEGRADED（降权 weight×0.5）
     *   连续超时 >= 10 → health_state = UNREACHABLE（从候选剔除）
     *   收到 200 OK     → ping_fail_count=0, health_state=HEALTHY
     *   收到 403        → 直接 UNREACHABLE（运营商显式拒绝）
     *   cfg_ping_sec == 0 → 探测关闭（ping_timer 永不 arm） */
    tw_node_t    ping_timer;
    char         ping_call_id[64];     /* 独立 Call-ID，避免与 REGISTER 响应匹配混淆 */
    uint32_t     ping_cseq;
    uint32_t     ping_fail_count;
    uint64_t     last_ping_ms;
    uint8_t      health_state;          /* gw_health_t */
} outreg_trunk_t;

/* 注册所有 TRUNK_REGISTER 类型中继；route_table=NULL 或空则跳过。 */
int outreg_init(nsbc_ctx_t *ctx);
void outreg_fini(nsbc_ctx_t *ctx);

/* 绑定 sip_transport_t + local_host（signaling 组合根装配）。outreg 不依赖 core/signaling.h
 * —— 只拿到 transport 指针用于出站发送；local_host 用于 Via/Contact 广告。
 *   传 NULL transport 则 send_register 返回 NSBC_NOTSUP（状态机照常跑但不真发包）。
 *   绑定后自动触发所有 trunk 的首注 REGISTER。 */
void outreg_bind_transport(sip_transport_t *tr, const char *local_host);

/* 主动注销（BYE/close 语义弱但能让对端尽快清注册）：
 * 发 Expires=0 REGISTER；stop=1 则定时器停且标记 OR_IDLE。 */
int outreg_unregister_all(int stop);

/* OAM 计数（show registrations 扩展到"出站注册"表） */
typedef struct {
    uint32_t active;    /* 当前发 REGISTER 的 trunk 数 */
    uint32_t registered;/* 状态=OR_REGISTERED */
    uint32_t failed;    /* 状态=OR_FAILED / OR_RETRY_WAIT */
    uint32_t total;
} outreg_stat_t;
void outreg_stat(outreg_stat_t *out);

/* 收信令回调：b2bua/sip_transport 收到响应后调用本函数
 * (via ctx->event_fn / 组合根装配)。msg=响应，原 REGISTER 的 Call-ID 在
 * msg->cseq.call_id；auth_hdr=WWW-Authenticate / Proxy-Authenticate 值。 */
int outreg_on_register_response(const char *call_id, int status,
                                const char *auth_hdr);

/* 内部（timer 到期 / 首注）：发一个 REGISTER 给 trunk 对应的对端 */
int outreg_send_register(const char *trunk_name, int is_retry);

/* ---------- reload from route_table（打通 Web UDS reload 链路，P0 核心）----------
 *
 * 由 trunk_table_reload_json() 在原子换表后调用，传入新 route_table 中
 * TRUNK_REGISTER 类型的 trunk 列表。outreg 内部做增量更新：
 *   - 同名 trunk：拷贝 cfg_* 字段 → 若密码变了重算 ha1 → profile 变了触发 unreg+reg
 *                 保留 state/call_id/cseq/challenge 缓存（续注不打断）
 *   - 新增 trunk：前置校验 profile 存在 + transport 就绪 → 创建 outreg_trunk_t → 发首注 REGISTER
 *                 校验失败 → 标 OR_INVALID + warn 日志（不阻塞其他 trunk）
 *   - 删除 trunk：自然过期（route_table 已换表拿不到 host/port，无法发 Expires=0）
 *
 * 并发模型：全程持有 g_lock（pthread_mutex_t），粗粒度锁，规模 < 64 trunk 竞争极低。 */
int outreg_reload_from_route_table(const trunk_t *list, size_t count);

/* ---------- 独立 Digest 鉴权头构建（P1-11，供 REGISTER + INVITE 401/407 共用）----------
 *
 * 调用方（REGISTER send_register / INVITE 构造）：
 *   outreg_build_auth_hdr(tr, "REGISTER", ruri, OUTREG_HDR_AUTH, buf, sizeof(buf));
 *   outreg_build_auth_hdr(tr, "INVITE",   ruri, OUTREG_HDR_PROXY, buf, sizeof(buf));
 *
 * hdr_type 参数决定使用哪套 challenge 缓存 + 写入哪套 nc_counter：
 *   OUTREG_HDR_AUTH  → 用 ch_realm/ch_nonce/ch_qop + nc_counter   → Authorization 头
 *   OUTREG_HDR_PROXY → 用 p_realm/p_nonce/p_qop + p_nc_counter   → Proxy-Authorization 头
 *
 * 成功返回 NSBC_OK 且 out_buf 含完整头值（不含 "Authorization: " 前缀），
 * 若 ha1_set=0 或对应 challenge nonce 为空，out_buf[0]='\0' 并返回 NSBC_OK（表示"跳过鉴权"）。
 * 注意：调用后 nc_counter 或 p_nc_counter 会自增 +1（每次发新报文 nc 必须递增）。 */
#define OUTREG_HDR_AUTH  (0)
#define OUTREG_HDR_PROXY (1)
int outreg_build_auth_hdr(outreg_trunk_t *trunk,
                          const char *method,
                          const char *uri,
                          int hdr_type,
                          char *out_buf, size_t out_sz);

/* ---------- OPTIONS 健康探测（P1-9，文档 §3.2）----------
 *
 * 发送独立的 OPTIONS 探测报文（ping_call_id 独立于 REGISTER call_id，on_ping_response 独立回调）。
 *   outreg_send_options(name) → 发 OPTIONS；call_id 用 ping_call_id
 *   outreg_on_options_response(ping_call_id, status, roundtrip_ms) → 更新 health_state
 *   outreg_get_health(name) → gw_health_t（路由层调它做权重折减，不会改动 route_table RCU）
 *
 * cfg_ping_sec==0 时 outreg_send_options 直接返回 NSBC_ERR（不允许发送）。
 * 返回值：NSBC_OK 表示已发送，NSBC_ERR 表示条件不满足（OR_DISABLED/OR_INVALID/cfg_ping_sec=0/transport 未绑定）。 */
int  outreg_send_options(const char *trunk_name);
void outreg_on_options_response(const char *ping_call_id, int status, uint64_t rtt_ms);
uint8_t outreg_get_health(const char *trunk_name);   /* 返回 gw_health_t，未知 trunk → HEALTHY（不阻断选路） */

/* ---------- 呼入 trunk 四层匹配（P2-14，文档 §4.2.1 层 1）----------
 *
 * register 型 trunk 反向关联：入站 INVITE 的 To-user / PAI 匹配 outreg_trunk_t.auth_user
 * 仅在 state==OR_REGISTERED 时才算命中（未注册不表示此 trunk）。
 * 匹配成功 → 返回复制 trunk name 到 out（假设 len>=RT_NAME_MAX）；未命中 → 0。
 * 调用方（handle_new_invite）：先调 outreg_find_by_auth_user（层 1）
 *   没命中 → rt_match_inbound_trunk（层 2/3/4） */
int outreg_find_by_auth_user(const char *auth_user, char *out_name, size_t out_len);

/* ---------- Prometheus 指标（P1-9）----------
 * 文档 §3.2：nsbc_gateway_health_state / nsbc_gateway_ping_fail_total
 * 在 outreg 内部直接用 metric_reg 注册（调用 metric_counter_inc / metric_gauge_set）。
 * 声明在这里方便外部确认指标名；具体实现嵌在 on_ping_response 状态变化处。 */

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
