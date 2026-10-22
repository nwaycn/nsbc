#ifndef NSBC_SIP_PROFILE_H
#define NSBC_SIP_PROFILE_H

/* SIP Profile（REQ-SIP-PROFILE-001）
 *
 * 多 Profile 实例：每个 profile 是自包含的 SIP 行为 bundle，
 * 拥有独立的 RTP 端口范围、NAT 映射、SIP 协议特性开关
 * （PRACK/UPDATE/DTMF 模式）。C 侧维护 sip_profile_t 数组。
 *
 * 【重要】profile **不拥有独立监听 socket**：SIP UDP/TCP/TLS 统一由
 * signaling transport（sip_transport.c）监听。历史教训：profile 曾独立
 * SO_REUSEPORT bind 5060 但从未注册 reactor，内核把包 hash 到无人读的
 * socket → REGISTER 随机丢弃（生产事故）。profile 仅承载配置数据，
 * 后续按 (入端口/目标 IP/域名) 匹配 profile 应用其行为参数。
 *
 * 与现有 CFG_L_PROFILE 的关系：
 *   CFG_L_PROFILE（cfg.h）是"配置覆盖层"——键名如 profile.<name>.net.sip_udp_port；
 *   sip_profile_t 数组是"运行时对象"。两层互补：DB → cfg_set(CFG_L_PROFILE,
 *   ...) → 热加载 sip_profile_t 数组。
 *
 * 数据源：Rust Web sip_profiles DB 表 → UDS `profiles.reload <json>` → 替换内存数组。
 */

#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIP_PROFILE_NAME_MAX   32
#define SIP_PROFILE_IP_MAX     64   /* IPv6 "ffff:...:5060" 足够 */

typedef enum {
    SIP_PROFILE_OK = 0,       /* 正常运行 */
    SIP_PROFILE_ERROR,        /* bind 失败 / socket 异常 */
    SIP_PROFILE_DISABLED,     /* enabled=0 */
    SIP_PROFILE_RELOADING     /* reload 中间态 */
} sip_profile_state_t;

typedef enum {
    PROFILE_DTMF_RFC4733 = 0, /* 默认：RFC 4733 telephone-event RTP */
    PROFILE_DTMF_INFO,        /* SIP INFO application/dtmf-relay */
    PROFILE_DTMF_INBAND,      /* G.711 双频 Goertzel */
    PROFILE_DTMF_AUTO         /* 自动：SDP 协商优先 RFC4733，回退 INFO */
} profile_dtmf_mode_t;

/* 每个名单最多预编译 CIDR 条数（超出回退字符串逐包解析） */
#define PROFILE_ACL_MAX 64

/* 预编译 CIDR 规则（主机字节序 net/mask 对） */
typedef struct { uint32_t net, mask; } profile_cidr_t;

/* 单 Profile 配置 + 运行时状态
 * 字段与 Rust Web sip_profiles DB 表 1:1 对齐（字段名/类型/默认值）。
 * 假期回来用 cfg_schema_register_profile() 注册 profile.<name>.* 覆盖路径。 */
typedef struct sip_profile {
    /* ---- 标识 ---- */
    char name[SIP_PROFILE_NAME_MAX];       /* 唯一键 */
    int  enabled;                          /* 1=启用 */

    /* ---- SIP 监听 ---- */
    char sip_ip[SIP_PROFILE_IP_MAX];       /* 绑定 IP，"0.0.0.0" = 所有网卡 */
    char external_sip_ip[SIP_PROFILE_IP_MAX]; /* NAT 公网 IP（空=不改写） */
    uint16_t sip_udp_port;                 /* 默认 5060 */
    uint16_t sip_tcp_port;                 /* 默认 5060 */
    uint16_t sip_tls_port;                 /* 默认 5061 */
    int      enable_udp;                   /* 默认 1 */
    int      enable_tcp;                   /* 默认 1 */
    int      enable_tls;                   /* 默认 0 */

    /* ---- RTP/媒体 ---- */
    char     rtp_ip[SIP_PROFILE_IP_MAX];          /* 绑定 RTP IP */
    char     external_rtp_ip[SIP_PROFILE_IP_MAX]; /* NAT 公网 RTP IP */
    uint16_t rtp_port_start;                      /* 默认 10000 */
    uint16_t rtp_port_end;                        /* 默认 59999 */

    /* ---- SIP 协议特性 ---- */
    int enable_prack;                      /* RFC 3262 100rel/PRACK，默认 0 */
    int enable_update;                     /* RFC 3311 UPDATE，默认 1 */
    int enable_t140;                       /* T.140 text over SIP INFO，默认 0 */

    /* ---- DTMF ---- */
    profile_dtmf_mode_t dtmf_mode;         /* 默认 PROFILE_DTMF_RFC4733 */

    /* ---- 鉴权 ---- */
    char realm[64];                        /* 默认 "nsbc.local" */
    int  auth_calls;                       /* 1=internal: 必须 Digest 认证才允许 INVITE/REGISTER
                                              0=external: FreeSWITCH external 模式，默认不强制认证
                                              默认 0；黑名单 IP 一律 silent drop 独立于此 */

    /* ---- IP 访问控制（CIDR 格式，逗号分隔） ---- */
    char ip_whitelist[512];                /* 白名单：空=不限制；有值则仅名单内 IP 放行 */
    char ip_blacklist[512];                /* 黑名单：空=不拦截；命中则 **silent drop 零响应**（防扫描探测） */

/* ---- 运行时（C 侧填充，DB 不存） ---- */
    sip_profile_state_t state;
    int  udp_fd;                           /* 恒 -1：无独立 socket（signaling transport 统一监听） */
    int  tcp_fd;
    int  tls_fd;
    uint32_t active_calls;                 /* 当前活跃呼叫数 */

    /* ---- ACL 预编译（reload 时由 ip_whitelist/ip_blacklist 编译，DB 不存）----
     * 数据面判定走 compiled 数组（纯 uint32 比较，零字符串解析）。
     * n < 0  = 条目超 PROFILE_ACL_MAX，热路径回退字符串逐包解析；
     * n == 0 = 未编译（手工填充字符串字段的场景），同样回退字符串路径。 */
    profile_cidr_t wl[PROFILE_ACL_MAX];
    int            wl_n;
    profile_cidr_t bl[PROFILE_ACL_MAX];
    int            bl_n;
} sip_profile_t;

/* ---- C 侧运行时 API（假期回来实现） ---- */

/* 从 DB JSON 批量加载：profiles.reload 入口
 * JSON 格式：[{"name":"default", ...}, {"name":"wan1", ...}]
 * 成功返回 NSBC_OK，失败返回首个错误（out_err 可空）。
 * 实现语义：解析 JSON → 逐条校验（失败整体回滚，旧表不动）→
 * wrlock 原子换指针 → 旧数组进 retired 墓地（shutdown 才释放，
 * 读侧指针永不过期）。 */
int sip_profiles_reload_json(const char *json, char *out_err, size_t err_sz);

/* 校验并激活单 profile（**无独立 socket**：SIP 监听统一由 signaling
 * transport 承担，profile 仅承载配置/行为数据）。
 * enabled=0 → DISABLED；校验失败（非法 sip_ip / 端口 0 / RTP 段非法）
 * → ERROR。历史：曾独立 SO_REUSEPORT bind 与 transport 抢包且无人读
 * → REGISTER 随机丢弃，已废除。 */
int sip_profile_bind(sip_profile_t *prof);

/* 关闭单 profile 的 socket（保留 API 兼容；现恒无 fd，实际为 no-op） */
void sip_profile_unbind(sip_profile_t *prof);

/* 按名字查找（trunk/gateway 通过 profile_name 引用） */
sip_profile_t *sip_profile_find(const char *name);

/* ---- IP 访问控制（CIDR 匹配，单文件纯 C 实现不依赖 libcidr） ---- */

/* 判断 src_ip 是否命中 CIDR 列表（逗号分隔字符串）。
 * 返回 1=命中，0=未命中，-1=列表为空或无效输入。
 * 支持格式："10.0.0.0/24,192.168.1.100/32,172.16.0.0/12"
 * IPv4 only（SIP 99% 场景用 IPv4）。 */
int sip_ip_in_cidr_list(const char *cidr_list, const char *src_ip);

/* 高一级：白/黑名单联合判定。
 * 返回值：
 *   SIP_IP_ACCEPT  (0)  — 放行（白名单匹配 或 白名单空 且 黑名单不匹配）
 *   SIP_IP_DROP    (1)  — 黑名单命中 → **caller 应 silent drop（零响应）**
 *   SIP_IP_REJECT  (2)  — 白名单存在但未命中 → 可选回 403（实现可决定） */
#define SIP_IP_ACCEPT 0
#define SIP_IP_DROP   1
#define SIP_IP_REJECT 2
int sip_profile_check_ip(const sip_profile_t *prof, const char *src_ip);

/* 数据面多 profile 归属判定（按源 IP；b2b_on_rx / registrar / handle_new_invite
 * 调用，两遍扫描：黑名单全域优先，白名单归属）：
 *   - 任一 enabled profile 黑名单命中 → *verdict=SIP_IP_DROP（caller silent drop）
 *   - 某白名单命中 → 返回该 profile（归属，用其 realm 等行为参数）
 *   - 均未命中但存在白名单配置 → *verdict=SIP_IP_REJECT（caller 可回 403）
 *   - 无任何 ACL 配置 → 返回 NULL + ACCEPT（全局默认放行，行为不变）
 * verdict 可为 NULL（只关心归属不关心判定）。持 rdlock，热路径零分配。
 * 返回指针指向数组元素：配置字段加载后不可变（运行时字段 active_calls
 * 由归属方原子读写），旧数组 reload 后进墓地不释放——指针在整个进程
 * 生命周期内安全（可存入 b2b_call_t 等长生命周期对象）。 */
sip_profile_t *sip_profile_match_src(const char *src_ip, int *verdict);

/* 枚举（reload/list 命令） */
unsigned sip_profile_count(void);
sip_profile_t *sip_profile_at(unsigned idx);

/* 按 SIP 监听端口查找 profile（UDP/TCP 都查；返回第一个 enabled+OK 的）。
 * 多端口场景（external=5080 / internal=5060）下，这是最高效的归属判定——
 * 从 5080 进来的包直接归属 external profile，无需等 IP 匹配。
 * 找不到返回 NULL（端口未被任何 profile 使用）。 */
sip_profile_t *sip_profile_match_port(uint16_t udp_port, uint16_t tcp_port);

/* ---- 配置 Schema 声明（cfg_schema.c 假期回来追加） ----
 * 每个 profile 覆盖路径（与 DB 表字段同名）：
 *   profile.<name>.sip_ip / profile.<name>.sip_udp_port / ...
 *   profile.<name>.enable_prack / profile.<name>.dtmf_mode
 *   共享路径（所有 profile 适用）：profile.default.enable_prack = false
 * 函数签名预留： */
struct cfg;  /* 前向声明：cfg.h 的 struct cfg（此处仅需不完整类型） */
int cfg_schema_register_profile(struct cfg *c);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_SIP_PROFILE_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
