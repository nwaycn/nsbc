/* security.h — Phase 2/4/5 安全模块独立（CIDR 黑名单 + hook 自注册 + NIP 电子围栏全能力）
 *
 * Phase 2：独立成 security 模块，自注册 b2b_register_hook_add 拦截 REGISTER
 * Phase 4：security_check_ip 升级为纯 C IPv4 CIDR 前缀匹配；新增统一静态黑名单
 *   security_blacklist_check()：IP CIDR + UA 子串双表，INVITE/REGISTER/OPTIONS 共用
 * Phase 5（NIP 电子围栏）：三层围栏 + allowlist 模式 + method 禁用 + 模式切换
 *   1. 扩 type：ip_cidr / ua / called_prefix（被叫号码围栏）/ caller_prefix（主叫号码围栏）
 *   2. 加 kind：deny（命中即丢）/ allow（命中才放，allow 配了未命中一律丢）
 *   3. 加 fence_mode：0=performance（跳过所有软件检查，硬件处理）/ 1=security（完整）
 *   4. 加 method_blocked 位图：禁用 PUBLISH/SUBSCRIBE/NOTIFY/REFER/MESSAGE/INFO 等
 *   5. 默认 UA 黑名单种子（60+ 条，NIP ua_black.txt）——零配置可用
 */
#ifndef NSBC_SECURITY_H
#define NSBC_SECURITY_H

#include "nsbc_common.h"
#include "net_sock.h"      /* nsbc_sa_t (完整定义：ss + len) + nsbc_sa_fmt */
#include "sip_msg.h"       /* sip_method_t（method 禁用位图） */

#include <sys/socket.h>   /* struct sockaddr（统一检查入参） */

#ifdef __cplusplus
extern "C" {
#endif

/* security config：从 YAML 加载
 * blacklist_cidrs: "10.0.0.0/24,192.168.1.0/24" 逗号分隔 CIDR
 * block_on_register: 1=REGISTER 命中黑名单直接 PERM_DENIED
 * block_on_invite: 1=INVITE 命中黑名单 PERM_DENIED
 * auto_ban（内置动态封禁）：REGISTER 认证失败在窗口内累计超阈值
 *   → 自动封禁源 IP 一段时间；封禁期间 REGISTER/INVITE 均静默丢弃
 *   （不回 403/404，防 SBC 指纹暴露）。表为内存态，重启进程即清零。
 * fence_mode: 1=security（完整软件围栏）/ 0=performance（全关，硬件处理）
 * method_blocked: 位图（1<<(method-1)），PUBLISH/SUBSCRIBE/NOTIFY 默认禁
 *   reload_json 会覆盖这些字段（Web 为准）。 */
typedef struct {
    int block_on_register;
    int block_on_invite;
    char blacklist_cidrs[1024];   /* 静态 CIDR 黑名单（YAML 种子） */
    char ua_blacklist[1024];      /* UA 黑名单（YAML 种子，逗号分隔子串） */
    int auto_ban;                 /* 动态封禁开关（默认 1） */
    int authfail_threshold;       /* 窗口内失败次数阈值（默认 5） */
    int authfail_window_s;        /* 计数窗口秒（默认 60） */
    int ban_duration_s;           /* 封禁时长秒（默认 3600） */
    int ratelimit_regs;           /* REGISTER ratelimit：窗口内次数阈值（默认 20） */
    int ratelimit_window_s;       /* REGISTER/INVITE ratelimit 共用窗口秒（默认 60） */
    int ratelimit_invites;        /* 被动阈值：窗口内 txn slab 分配失败次数（默认 30）。
                                    * 攻击者 flood 打满 slab 才触发；正常流量不可能打满 slab。
                                    * 设 0 关闭此阈值。与 ratelimit_invites_active 双阈值独立。 */
    int ratelimit_invites_active; /* 主动阈值：per-IP 每秒新 INVITE 次数（默认 20）。
                                    * 还没造成 slab 压力就按频率拦——攻击者发 > 20 INVITE/s
                                    * 立刻封，正常合法呼叫不会触发（坐席并发不会超过此数）。
                                    * 设 0 关闭此主动阈值。共用 ratelimit_window_s 窗口秒。 */
    /* NIP Phase 5 新增 */
    int fence_mode;               /* 1=security（默认）/ 0=performance */
    uint32_t method_blocked;      /* 禁用 method 位图：(1<<(SIP_M_xxx - 1)) */
} security_cfg_t;

/* 查询源 IP 是否命中黑名单（CIDR 匹配简化骨架）*/
int security_check_ip(const char *ip, const security_cfg_t *cfg);

/* ==== 统一静态安全规则（IP CIDR + UA + 号码前缀，Web 安全中心管理） ==== */

/* 检查结果 —— 区分 deny 命中类型 + allow 拒绝 */
typedef enum {
    BLACKLIST_OK = 0,   /* 放行 */
    BLACKLIST_IP,       /* deny：命中 IP CIDR */
    BLACKLIST_UA,       /* deny：命中 UA 子串 */
    BLACKLIST_CALLED,   /* deny：命中被叫前缀 */
    BLACKLIST_CALLER,   /* deny：命中主叫前缀 */
    BLACKLIST_METHOD,   /* deny：method 被禁用 */
    BLACKLIST_DENIED_ALLOW  /* allowlist 配了但未命中 —— 必须放才放行 */
} blacklist_result_t;

/* 统一黑白名单检查 —— 保留 Phase 4 签名（向后兼容）。
 * 内部仅查 IP/UA deny 规则 + fence_mode + method_blocked；
 * 不涉及 allowlist / 号码前缀（需要完整 ctx 才查）。 */
blacklist_result_t security_blacklist_check(const struct sockaddr *src,
                                            const char *ua);

/* ==== NIP Phase 5：完整围栏检查（deny + allow + 号码前缀 + method） ==== */

/* 完整围栏检查上下文（扩展参数，避免修改原签名） */
typedef struct {
    /* 输入 —— 调用方填充 */
    const struct sockaddr *src;   /* 源地址 */
    const char *ua;              /* 顶层 User-Agent（无 → NULL/空串） */
    sip_method_t method;         /* INVITE/REGISTER/OPTIONS/... */
    const char *caller;          /* From URI user part（无 → NULL/空串） */
    const char *callee;          /* Request-URI user part（无 → NULL/空串） */
    /* 输出 —— 被命中的 deny 类型 / allow 拒绝 */
    blacklist_result_t hit;      /* BLACKLIST_OK 表示放行 */
} security_fence_ctx_t;

/* 完整围栏检查：deny 命中或 allow 配了但未命中 → ctx->hit != BLACKLIST_OK
 * 性能模式（fence_mode=0）直接直返 OK（跳过所有软件检查）。
 * method 禁用在最前（性能最高，位图 O(1)）。
 * 调用方必须静默丢弃（不回任何 SIP 响应，防 SBC 指纹暴露）。 */
void security_check_fence(security_fence_ctx_t *ctx);

/* 围栏命中日志（512:1 采样防扫描流量刷屏）。
 *   hit   : security_check_fence 的 ctx->hit
 *   ip/ua : 源 IP 字符串与 UA 值（纯展示用）
 *   method: "INVITE" / "REGISTER" / "OPTIONS" / ...
 * 输出格式：fence-drop: deny_type ip=X ua="U" method=M caller=C callee=N (dropped=N) */
void security_fence_log_drop(blacklist_result_t hit, const char *ip,
                             const char *ua, const char *method,
                             const char *caller, const char *callee);

/* ==== Web 推送全量替换（`security.blacklist.reload <json>`，OAM L2） ====
 * JSON 数组，元素：
 *   {"type":"ip_cidr"|"ua"|"called_prefix"|"caller_prefix",
 *    "kind":"deny"|"allow",
 *    "value":"...","comment":"...","enabled":true}
 * 顶级还可带 "fence_mode":0/1 / "method_blocked":位图数字（可选，缺省不变）
 * 单条非法（CIDR 解析失败 / UA 超长 / 未知 type/kind）→ WARN 跳过，不阻断
 * 整体替换（文档 §7：非法配置不阻断启动/更新）；整表原子换（wrlock）。 */
int security_blacklist_reload_json(const char *json, char *out_err,
                                   size_t err_sz);

/* 当前 fence_mode 快照（atomic 读取）——b2bua.c 热路径可能直接查 */
int security_fence_mode_get(void);

/* reactor 层前置封禁/黑名单查询（recvfrom 后 parse 前调用）：
 * 返回 1 表示该 IP 应被静默丢弃（封禁中 / 静态 CIDR 黑名单命中），
 * 调用方直接 continue 跳过 parse 和 b2b_on_rx；返回 0 放行。
 * 性能敏感：内部原子读 fence_mode + 哈希桶链表，无 parse 成本。 */
int security_is_blocked(const nsbc_sa_t *src);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_SECURITY_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
