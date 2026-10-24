/* security.c — Phase 2 新建：CIDR 黑名单 + 自注册 b2b_register_hook
 *
 * 背景：dialplan.c/signaling.c 的 CIDR 拒绝逻辑是硬编码 rule 级过滤，
 * Phase 2 独立成 security 模块——在 b2b_register_hook 链最前置位置
 * 拦截 REGISTER，命中黑名单 → PERM_DENIED（silent drop，黑产扫描者
 * 连 401 challenge 都收不到）。
 *
 * 架构：
 *   security_init → b2b_register_hook_add(sec_reg_cb, &g_cfg)
 *   security_fini → b2b_register_hook_rm(sec_reg_cb, &g_cfg)
 *
 * CIDR 匹配：当前简化骨架（security_check_ip 只做字符串比较预留位），
 * 生产应引入 libcidr 做标准 CIDR 前缀匹配；libcidr 是 BSD-licensed 单文件。
 *
 * 2026-10-18：《nsbc安全管理当前状况》第二部分 —— 统一静态黑名单
 * （IP CIDR + UA）。security_blacklist_check() 为 INVITE/REGISTER/OPTIONS
 * 共用检查点；数据源 web.db security_blacklist 表（Web 安全中心管理），
 * 经 `security.blacklist.reload <json>` 全量原子替换；YAML 仅作启动种子。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* strcasestr（UA 大小写不敏感子串匹配） */
#endif
#include "security.h"
#include "module.h"
#include "app_ctx.h"        /* struct nsbc_ctx + ctx->cfg */
#include "b2bua.h"          /* b2b_register_hook_add/rm + b2b_guard_hook_add/rm */
#include "cfg.h"            /* cfg_get_bool/int/str — security.* 配置加载 */
#include "cmd_registry.h"   /* cmd_register / cmd_desc_t / cmd_ctx_t（OAM 命令） */
#include "obs.h"            /* obs_control_get_cmd_reg */
#include "log.h"
#include "sip_msg.h"        /* sip_hdr_get / SIP_H_USER_AGENT（REGISTER 入口取 UA） */

#include <cjson/cJSON.h>    /* security.blacklist.reload <json> 解析 */
#include <arpa/inet.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

/* ---- 前向声明：封禁表核心（sec_reg_cb 先于定义使用） ---- */
static int  ban_is_banned(const char *ip);
static int  ban_note_fail(const char *ip);
static int  ban_note_reg_req(const char *ip);
static int  ban_note_inv_fail(const char *ip);   /* 被动：txn slab 分配失败计数 */
static int  ban_note_inv_count(const char *ip); /* 主动：per-IP 新 INVITE 频率计数 */
static uint32_t ban_duration_for_strikes(uint32_t base_s, int strikes);

/* === 全局配置（init 时从 YAML 加载；简化骨架：默认全 0）=== */
static security_cfg_t g_cfg;

/* ==================================================================
 * Phase 4: 纯 C IPv4 CIDR 前缀匹配（替代 libcidr 重依赖）
 *
 * 算法：
 *   1. parse_cidr("10.0.0.0/24", &net, &prefix) → net=0x0A000000, prefix=24
 *   2. ipv4_to_uint32("10.0.0.5", &ip) → ip=0x0A000005
 *   3. mask = ~0U << (32 - prefix)   (prefix=0 时 mask=0 匹配所有)
 *   4. (ip & mask) == (net & mask) → 命中
 * ==================================================================*/

/* 把 "a.b.c.d" 转成 uint32_t（network byte order — 即大端序）
 * inet_pton(AF_INET) 直接把 4 字节按网络序塞进 struct sockaddr_in.sin_addr，
 * 所以 *(uint32_t*)&sin_addr 就是目标值。*/
static int ipv4_to_uint32(const char *ip_str, uint32_t *out)
{
    if (!ip_str || !out) return -1;
    struct in_addr a;
    if (inet_pton(AF_INET, ip_str, &a) != 1) return -1;
    memcpy(out, &a.s_addr, 4);  /* 大端序 */
    return 0;
}

/* 解析 CIDR 字符串 "a.b.c.d/prefix"
 * prefix 缺省时默认 32（精确匹配该 IP）
 * 返回 0=OK, -1=格式错 */
static int parse_cidr(const char *cidr_str, uint32_t *out_net, int *out_prefix)
{
    if (!cidr_str || !out_net || !out_prefix) return -1;

    char buf[INET_ADDRSTRLEN + 16];  /* CIDR 串：最大 "255.255.255.255/32" = 18 字节 */
    strncpy(buf, cidr_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* trim 前后空格 */
    char *start = buf;
    while (*start == ' ' || *start == '\t') start++;
    size_t len = strlen(start);
    while (len > 0 && (start[len-1] == ' ' || start[len-1] == '\t')) {
        start[--len] = '\0';
    }
    if (!start[0]) return -1;

    /* 找 '/' */
    char *slash = strchr(start, '/');
    char ip_part[INET_ADDRSTRLEN];
    int prefix;

    if (slash) {
        size_t ip_len = (size_t)(slash - start);
        if (ip_len >= sizeof(ip_part)) return -1;
        memcpy(ip_part, start, ip_len);
        ip_part[ip_len] = '\0';
        prefix = atoi(slash + 1);
    } else {
        strncpy(ip_part, start, sizeof(ip_part) - 1);
        ip_part[sizeof(ip_part) - 1] = '\0';
        prefix = 32;  /* 默认：单 IP 精确匹配 */
    }

    if (prefix < 0 || prefix > 32) return -1;
    if (ipv4_to_uint32(ip_part, out_net) != 0) return -1;

    *out_prefix = prefix;
    return 0;
}

/* CIDR 前缀匹配：(ip & mask) == (net & mask) */
static int cidr_match(uint32_t ip, uint32_t net, int prefix)
{
    uint32_t mask = (prefix == 0) ? 0U : (~0U << (32 - prefix));
    return (ip & mask) == (net & mask);
}

/* === security_check_ip — Phase 4 升级为标准 CIDR 前缀匹配 ===
 * 纯 C 实现，不依赖 libcidr，几十行代码搞定生产级 IPv4 CIDR 匹配。
 * 返回 0=未命中，1=命中黑名单，-1=CIDR 格式错（告警但不阻断）。*/
int security_check_ip(const char *ip, const security_cfg_t *cfg)
{
    if (!ip || !cfg) return 0;
    if (!cfg->blacklist_cidrs[0]) return 0;

    /* 先把查询 IP 转成 uint32_t */
    uint32_t ip_uint;
    if (ipv4_to_uint32(ip, &ip_uint) != 0) {
        log_debug("security", "ip '%s' is not valid IPv4, skip CIDR check", ip);
        return 0;
    }

    /* 按逗号分隔遍历每个 CIDR 条目 */
    char cidrs[1024];  /* 复制一份避免修改 cfg 常量 */
    strncpy(cidrs, cfg->blacklist_cidrs, sizeof(cidrs) - 1);
    cidrs[sizeof(cidrs) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(cidrs, ",", &saveptr);
    while (token) {
        uint32_t net;
        int prefix;
        if (parse_cidr(token, &net, &prefix) == 0) {
            if (cidr_match(ip_uint, net, prefix)) {
                log_notice("security", "ip '%s' hit blacklist CIDR '%s' (prefix=%d)",
                           ip, token, prefix);
                return 1;
            }
        } else {
            log_warn("security", "invalid CIDR entry '%s' in blacklist (skipped)", token);
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
    return 0;
}

/* === 从 nsbc_sa_t 提取 IP 字符串（signaling.c 同款模式）=== */
static void sa_to_ip_str(const nsbc_sa_t *sa, char *out, size_t outsz)
{
    if (!sa || !out || outsz < INET6_ADDRSTRLEN) return;
    const struct sockaddr *s = (const struct sockaddr *)sa;
    if (s->sa_family == AF_INET) {
        inet_ntop(AF_INET, &((const struct sockaddr_in *)s)->sin_addr, out, outsz);
    } else if (s->sa_family == AF_INET6) {
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)s)->sin6_addr, out, outsz);
    }
    /* AF_UNSPEC → out 保持空串 */
}

/* ==================================================================
 * 统一静态黑名单（IP CIDR + UA）——《nsbc安全管理当前状况》第二部分
 *
 * 数据流：Web 安全中心（web.db security_blacklist 表）→ control.sock
 * `security.blacklist.reload <json>` → 本表全量原子替换（wrlock）。
 * YAML security.blacklist_cidrs / security.ua_blacklist 仅在 init 时
 * 种子加载一次（Web 未连接期间的 fallback；Web 推送后以数据库为准）。
 *
 * 热路径：SIP 每包（INVITE/REGISTER/OPTIONS）→ security_blacklist_check
 * 读锁遍历。表空时零成本快路径直接返回；持锁内只做内存匹配，禁止 IO。
 * ==================================================================*/

#define BL_CIDR_MAX     256    /* CIDR 规则容量（文档 §2） */
#define BL_UA_MAX       128    /* UA 规则容量（NIP 60+ 默认种子扩容） */
#define BL_PREFIX_MAX   128    /* 号码前缀规则容量（called/caller 各 64） */
#define BL_VAL_MAX      128    /* value / UA pattern 长度上限（与 Web 校验对齐） */
#define BL_COMMENT_MAX  64

/* 规则 kind：0=deny（黑名单，命中即丢）/ 1=allow（白名单，命中才放）
 * allowlist 语义：配了 allow 的规则后，只有命中 allow 的才放行；
 * 未命中 allow 的一律丢（等效 NIP "允许设备：FreeSWITCH*" 模式）。 */
#define BL_KIND_DENY   0
#define BL_KIND_ALLOW  1

typedef struct {
    int      family;                    /* AF_INET / AF_INET6 */
    uint32_t net[4];                    /* 网络地址（host 序 4 字） */
    int      prefix;                    /* 0..32（v4）/ 0..128（v6） */
    uint8_t  enabled;
    char     value[BL_VAL_MAX];         /* 原始 CIDR 串（OAM 展示/日志） */
    char     comment[BL_COMMENT_MAX];
} bl_cidr_t;

typedef struct {
    char     pattern[BL_VAL_MAX];       /* 大小写不敏感子串 */
    uint8_t  enabled;
    char     comment[BL_COMMENT_MAX];
} bl_ua_t;

/* 号码前缀规则（called_prefix = 被叫前缀，caller_prefix = 主叫前缀）。
 * 匹配模式："100*"→100 开头；"100"→精确；"*"→所有；"900*"→900 开头。
 * 空串 / "*" 前缀不进规则（空串默认不匹配，防误杀）。 */
typedef struct {
    char     prefix[BL_VAL_MAX];        /* 如 "100*" / "900" / "*" */
    uint8_t  enabled;
    char     comment[BL_COMMENT_MAX];
} bl_prefix_t;

/* deny + allow 两套独立表（原子替换：wrlock 换指针时整表 memcpy 一次完成） */
static bl_cidr_t  g_deny_cidr[BL_CIDR_MAX];
static int        g_deny_cidr_n;
static bl_ua_t    g_deny_ua[BL_UA_MAX];
static int        g_deny_ua_n;
static bl_prefix_t g_deny_called[BL_PREFIX_MAX];
static int        g_deny_called_n;
static bl_prefix_t g_deny_caller[BL_PREFIX_MAX];
static int        g_deny_caller_n;

static bl_cidr_t  g_allow_cidr[BL_CIDR_MAX];
static int        g_allow_cidr_n;
static bl_ua_t    g_allow_ua[BL_UA_MAX];
static int        g_allow_ua_n;
static bl_prefix_t g_allow_called[BL_PREFIX_MAX];
static int        g_allow_called_n;
static bl_prefix_t g_allow_caller[BL_PREFIX_MAX];
static int        g_allow_caller_n;

static pthread_rwlock_t g_bl_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/* fence_mode / method_blocked：gcc atomic 内置，热路径零锁 */
static int      g_fence_mode = 1;    /* 1=security（默认完整检查） */
static uint32_t g_method_blocked = 0; /* 默认空（禁 method 由 Web 配） */

/* 解析 CIDR（v4/v6，"a.b.c.d[/p]" / "x::y[/p]"）。
 * 缺省 prefix：v4=32 / v6=128（单 IP 精确匹配）。
 * prefix 段必须是纯数字（防 "10.0.0.0/abc"→atoi=0 匹配全网的误杀）。
 * 返回 0=OK，-1=格式错。 */
static int bl_parse_cidr(const char *str, int *out_fam,
                         uint32_t net[4], int *out_prefix)
{
    if (!str || !out_fam || !net || !out_prefix) return -1;

    char buf[BL_VAL_MAX + 8];
    snprintf(buf, sizeof(buf), "%s", str);
    char *s = buf;
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
        s[--len] = '\0';
    if (!len || len >= BL_VAL_MAX) return -1;

    char ip_part[BL_VAL_MAX];
    int prefix = -1;
    char *slash = strchr(s, '/');
    if (slash) {
        size_t il = (size_t)(slash - s);
        if (il == 0) return -1;
        memcpy(ip_part, s, il);
        ip_part[il] = '\0';
        const char *p = slash + 1;
        if (!*p) return -1;
        for (const char *q = p; *q; ++q)
            if (*q < '0' || *q > '9') return -1;
        prefix = atoi(p);
    } else {
        snprintf(ip_part, sizeof(ip_part), "%s", s);
    }

    struct in_addr a4;
    if (inet_pton(AF_INET, ip_part, &a4) == 1) {
        uint32_t h;
        memcpy(&h, &a4.s_addr, 4);
        net[0] = ntohl(h);
        net[1] = net[2] = net[3] = 0;
        if (prefix < 0) prefix = 32;
        if (prefix > 32) return -1;
        *out_fam = AF_INET;
        *out_prefix = prefix;
        return 0;
    }

    struct in6_addr a6;
    if (inet_pton(AF_INET6, ip_part, &a6) == 1) {
        const unsigned char *b = a6.s6_addr;
        for (int i = 0; i < 4; ++i)
            net[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16)
                   | ((uint32_t)b[i * 4 + 2] << 8) | (uint32_t)b[i * 4 + 3];
        if (prefix < 0) prefix = 128;
        if (prefix > 128) return -1;
        *out_fam = AF_INET6;
        *out_prefix = prefix;
        return 0;
    }
    return -1;
}

/* 前缀位运算匹配（文档 §2 建议）：
 *   v4: (ip & mask) == (net & mask)，mask = ~0U << (32 - prefix)
 *   v6: 逐 32 位字，bits = prefix - i*32 决定该字掩码
 * IPv4-mapped IPv6（::ffff:x.x.x.x）先还原成 IPv4 再匹配（文档 §7）。
 * prefix=0 → mask=0 → 匹配所有地址。 */
static int bl_ip_match(const struct sockaddr *sa, const bl_cidr_t *r)
{
    if (!sa || !r || !r->enabled) return 0;

    int fam = sa->sa_family;
    uint32_t ip[4] = {0, 0, 0, 0};

    if (fam == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
        const unsigned char *b = s6->sin6_addr.s6_addr;
        static const unsigned char zero10[10] = {0};
        if (memcmp(b, zero10, 10) == 0 && b[10] == 0xff && b[11] == 0xff) {
            /* IPv4-mapped：还原 IPv4（host 序） */
            fam = AF_INET;
            ip[0] = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16)
                  | ((uint32_t)b[14] << 8) | (uint32_t)b[15];
        } else {
            for (int i = 0; i < 4; ++i)
                ip[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16)
                      | ((uint32_t)b[i * 4 + 2] << 8) | (uint32_t)b[i * 4 + 3];
        }
    } else if (fam == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;
        uint32_t h;
        memcpy(&h, &s4->sin_addr.s_addr, 4);
        ip[0] = ntohl(h);
    } else {
        return 0;   /* AF_UNSPEC（WS 桥等）→ 只交给 UA 检查 */
    }

    if (fam == AF_INET && r->family == AF_INET) {
        uint32_t mask = (r->prefix <= 0) ? 0u
                      : (r->prefix >= 32) ? 0xffffffffu
                      : (~0u << (32 - r->prefix));
        return (ip[0] & mask) == (r->net[0] & mask);
    }
    if (fam == AF_INET6 && r->family == AF_INET6) {
        for (int i = 0; i < 4; ++i) {
            int bits = r->prefix - i * 32;
            uint32_t mask = (bits <= 0) ? 0u
                          : (bits >= 32) ? 0xffffffffu
                          : (~0u << (32 - bits));
            if ((ip[i] & mask) != (r->net[i] & mask))
                return 0;
        }
        return 1;
    }
    return 0;   /* v4 地址 vs v6 规则（或反之）不匹配 */
}

/* UA 大小写不敏感子串匹配（文档 §2/§7：UA 为空不命中，防误杀） */
static int bl_ua_match(const char *ua, const bl_ua_t *r)
{
    if (!ua || !ua[0] || !r || !r->enabled || !r->pattern[0])
        return 0;
    return strcasestr(ua, r->pattern) != NULL;
}

/* 号码前缀匹配（NIP 号码围栏）。三种模式：
 *   "*" / ""      → 不做规则（空规则跳过，调用方已过滤）
 *   "xxx"         → 精确匹配（pattern 无 '*' 后缀）
 *   "xxx*"        → 前缀匹配（去掉尾部 '*' 后匹配开头）
 * value 为空 → 不命中（防误杀）。大小写不敏感（号码不含字母，无影响）。 */
static int bl_prefix_match(const char *value, const bl_prefix_t *r)
{
    if (!value || !value[0] || !r || !r->enabled || !r->prefix[0])
        return 0;
    const char *p = r->prefix;
    size_t pl = strlen(p);
    int is_prefix = (pl >= 2 && p[pl - 1] == '*');
    if (is_prefix) {
        pl--;
        if (pl == 0) return 0;  /* 只有 "*" 无效 */
        while (pl > 0 && p[pl - 1] == '*') --pl;  /* "***" → 只剩 "*"，无效 */
        if (pl == 0) return 0;
    }
    /* 空 pattern（去掉 '*' 后）→ 无效 */
    if (pl == 0) return 0;
    size_t vl = strlen(value);
    if (is_prefix) {
        if (vl < pl) return 0;
        return strncmp(value, p, pl) == 0;
    }
    return vl == pl && strcmp(value, p) == 0;
}

/* === NIP Phase 5：完整围栏检查 === */

/* 号码前缀 + allowlist + method 禁用 —— 完整围栏流程。
 * 入参 ctx 必须填充 src / ua / method / caller / callee；
 * 输出 ctx->hit：BLACKLIST_OK 放行，其他 = 命中类型。
 *
 * 优先级（与 NIP 三层围栏一致）：
 *   1. fence_mode=0 → 直接 OK（硬件处理，跳过所有软件检查）
 *   2. method_blocked → BLACKLIST_METHOD（O(1) 位图）
 *   3. deny 表：IP / UA / called / caller 任一命中 → 返回
 *   4. allow 表：同维度 allow 配了但 value 为空 → DENIED_ALLOW
 *                有 allow 但无匹配 → DENIED_ALLOW
 *   5. 全部通过 → OK
 *
 * 持锁内只做内存匹配；锁外不允许 IO（日志是采样的，安全）。 */
void security_check_fence(security_fence_ctx_t *ctx)
{
    if (!ctx) return;

    /* fence_mode 性能模式 → 硬件处理，全关 */
    if (__atomic_load_n(&g_fence_mode, __ATOMIC_RELAXED) == 0) {
        ctx->hit = BLACKLIST_OK;
        return;
    }

    /* method 禁用位图（O(1)，比读锁还快） */
    if (ctx->method > SIP_M_UNKNOWN && ctx->method < SIP_M_MESSAGE + 1) {
        uint32_t mask = (1u << ((uint32_t)ctx->method - 1u));
        if ((__atomic_load_n(&g_method_blocked, __ATOMIC_RELAXED) & mask) != 0) {
            ctx->hit = BLACKLIST_METHOD;
            return;
        }
    }

    /* === 读锁：deny + allow 全表遍历 === */
    pthread_rwlock_rdlock(&g_bl_rwlock);

    /* deny: IP CIDR */
    if (ctx->src && ctx->src->sa_family != AF_UNSPEC && g_deny_cidr_n > 0) {
        for (int i = 0; i < g_deny_cidr_n; ++i) {
            if (bl_ip_match(ctx->src, &g_deny_cidr[i])) {
                pthread_rwlock_unlock(&g_bl_rwlock);
                ctx->hit = BLACKLIST_IP;
                return;
            }
        }
    }
    /* deny: UA */
    if (ctx->ua && ctx->ua[0] && g_deny_ua_n > 0) {
        for (int i = 0; i < g_deny_ua_n; ++i) {
            if (bl_ua_match(ctx->ua, &g_deny_ua[i])) {
                pthread_rwlock_unlock(&g_bl_rwlock);
                ctx->hit = BLACKLIST_UA;
                return;
            }
        }
    }
    /* deny: 被叫前缀 */
    if (ctx->callee && ctx->callee[0] && g_deny_called_n > 0) {
        for (int i = 0; i < g_deny_called_n; ++i) {
            if (bl_prefix_match(ctx->callee, &g_deny_called[i])) {
                pthread_rwlock_unlock(&g_bl_rwlock);
                ctx->hit = BLACKLIST_CALLED;
                return;
            }
        }
    }
    /* deny: 主叫前缀 */
    if (ctx->caller && ctx->caller[0] && g_deny_caller_n > 0) {
        for (int i = 0; i < g_deny_caller_n; ++i) {
            if (bl_prefix_match(ctx->caller, &g_deny_caller[i])) {
                pthread_rwlock_unlock(&g_bl_rwlock);
                ctx->hit = BLACKLIST_CALLER;
                return;
            }
        }
    }

    /* allowlist：配了但 value 空 or 无匹配 → DENIED_ALLOW
     * 语义等价 NIP "允许设备：FreeSWITCH*" —— 配了 allowlist 后白名单模式。
     * 任一维度配了 allow 才算进入白名单模式；没配的维度默认放行。 */
    int allow_hit = 0, allow_miss = 0;
    /* allow: IP CIDR —— 若配了（allow_n > 0），必须命中 */
    if (g_allow_cidr_n > 0) {
        if (ctx->src && ctx->src->sa_family != AF_UNSPEC) {
            int hit = 0;
            for (int i = 0; i < g_allow_cidr_n; ++i)
                if (bl_ip_match(ctx->src, &g_allow_cidr[i])) { hit = 1; break; }
            if (hit) allow_hit++; else allow_miss++;
        } else {
            allow_miss++;  /* allow CIDR 配了但 src 不可查（WS 桥等）→ 拒绝 */
        }
    }
    /* allow: UA */
    if (g_allow_ua_n > 0) {
        if (ctx->ua && ctx->ua[0]) {
            int hit = 0;
            for (int i = 0; i < g_allow_ua_n; ++i)
                if (bl_ua_match(ctx->ua, &g_allow_ua[i])) { hit = 1; break; }
            if (hit) allow_hit++; else allow_miss++;
        } else {
            allow_miss++;  /* UA allow 配了但请求无 UA → 拒绝 */
        }
    }
    /* allow: 被叫前缀 */
    if (g_allow_called_n > 0) {
        if (ctx->callee && ctx->callee[0]) {
            int hit = 0;
            for (int i = 0; i < g_allow_called_n; ++i)
                if (bl_prefix_match(ctx->callee, &g_allow_called[i])) { hit = 1; break; }
            if (hit) allow_hit++; else allow_miss++;
        } else {
            allow_miss++;  /* called allow 配了但被叫不可查 → 拒绝 */
        }
    }
    /* allow: 主叫前缀 */
    if (g_allow_caller_n > 0) {
        if (ctx->caller && ctx->caller[0]) {
            int hit = 0;
            for (int i = 0; i < g_allow_caller_n; ++i)
                if (bl_prefix_match(ctx->caller, &g_allow_caller[i])) { hit = 1; break; }
            if (hit) allow_hit++; else allow_miss++;
        } else {
            allow_miss++;
        }
    }

    pthread_rwlock_unlock(&g_bl_rwlock);

    if (allow_miss > 0) {
        ctx->hit = BLACKLIST_DENIED_ALLOW;
    } else {
        ctx->hit = BLACKLIST_OK;
    }
}

/* Phase 4 兼容入口 —— 只查 deny IP/UA + fence_mode + method 禁用，
 * 不涉及 allowlist / 号码前缀（这些需要完整 ctx）。
 * b2bua.c Phase 4 调用点仍可直接用此函数；未来建议全量改 fence。 */
blacklist_result_t security_blacklist_check(const struct sockaddr *src,
                                            const char *ua)
{
    security_fence_ctx_t ctx = {0};
    ctx.src = src;
    ctx.ua = ua;
    ctx.method = SIP_M_UNKNOWN;  /* 无 method 信息 → 不查 method 禁用 */
    security_check_fence(&ctx);
    return ctx.hit;
}

/* fence_mode 快照（atomic 读取 —— b2bua.c 热路径可能直接查，
 * 避免每条包都进 fence_check 的第一行判断）。 */
int security_fence_mode_get(void)
{
    return __atomic_load_n(&g_fence_mode, __ATOMIC_RELAXED);
}

/* 命中日志：512:1 采样（与封禁命中同限流惯例，防扫描流量刷屏） */
void security_fence_log_drop(blacklist_result_t hit, const char *ip,
                             const char *ua, const char *method,
                             const char *caller, const char *callee)
{
    static unsigned n = 0;
    if (hit == BLACKLIST_OK) return;
    if (n++ % 512 == 0) {
        const char *why = "unknown";
        switch (hit) {
        case BLACKLIST_IP:    why = "ip_cidr"; break;
        case BLACKLIST_UA:    why = "ua"; break;
        case BLACKLIST_CALLED: why = "called_prefix"; break;
        case BLACKLIST_CALLER: why = "caller_prefix"; break;
        case BLACKLIST_METHOD: why = "method"; break;
        case BLACKLIST_DENIED_ALLOW: why = "allowlist-miss"; break;
        default: break;
        }
        log_notice("security",
                   "fence-drop: %s ip=%s ua=\"%s\" method=%s caller=%s callee=%s (dropped=%u)",
                   why,
                   (ip && ip[0]) ? ip : "-",
                   (ua && ua[0]) ? ua : "-",
                   method ? method : "-",
                   caller && caller[0] ? caller : "-",
                   callee && callee[0] ? callee : "-",
                   n);
    }
}

/* 兼容旧签名 —— 内部调 security_fence_log_drop */
void security_blacklist_log_drop(blacklist_result_t r, const char *ip,
                                 const char *ua, const char *method)
{
    security_fence_log_drop(r, ip, ua, method, NULL, NULL);
}

/* === Web 推送全量替换（文档 §1/§7 + NIP Phase 5） ===
 * JSON schema：
 *   数组模式（兼容）：[{"type","kind"(deny/allow,默认deny),"value","comment","enabled"}, ...]
 *   对象模式（扩展）：{"fence_mode":0|1,"method_blocked":位图数字,
 *                     "rules":[...上面数组...]}
 * 单条非法跳过（不阻断整体）；deny/allow 各表独立；fence_mode/method_blocked
 * 独立 atomic store（整次 reload 完成后再原子换，避免中间态）。 */
int security_blacklist_reload_json(const char *json, char *out_err,
                                   size_t err_sz)
{
    if (out_err && err_sz) out_err[0] = '\0';
    if (!json || !json[0]) {
        if (out_err) snprintf(out_err, err_sz, "empty json");
        return NSBC_ERR;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        if (out_err) snprintf(out_err, err_sz, "invalid json");
        return NSBC_ERR;
    }

    static bl_cidr_t  n_dc[BL_CIDR_MAX];
    static bl_ua_t    n_du[BL_UA_MAX];
    static bl_prefix_t n_dc2[BL_PREFIX_MAX];
    static bl_prefix_t n_dcr[BL_PREFIX_MAX];
    static bl_cidr_t  n_ac[BL_CIDR_MAX];
    static bl_ua_t    n_au[BL_UA_MAX];
    static bl_prefix_t n_ac2[BL_PREFIX_MAX];
    static bl_prefix_t n_acr[BL_PREFIX_MAX];
    memset(n_dc, 0, sizeof(n_dc));
    memset(n_du, 0, sizeof(n_du));
    memset(n_dc2, 0, sizeof(n_dc2));
    memset(n_dcr, 0, sizeof(n_dcr));
    memset(n_ac, 0, sizeof(n_ac));
    memset(n_au, 0, sizeof(n_au));
    memset(n_ac2, 0, sizeof(n_ac2));
    memset(n_acr, 0, sizeof(n_acr));
    int ndc = 0, ndu = 0, ndc2 = 0, ndcr = 0;
    int nac = 0, nau = 0, nac2 = 0, nacr = 0;
    int skipped = 0;

    /* 顶级 fence_mode / method_blocked —— 对象模式 */
    int new_fence_mode = -1;         /* -1=不更新 */
    uint32_t new_method_blocked = 0xffffffffu;  /* 哨兵=不更新 */
    int new_auto_ban = -1;
    int new_authfail_threshold = -1;
    int new_authfail_window_s = -1;
    int new_ban_duration_s = -1;
    int new_ratelimit_regs = -1;
    int new_ratelimit_window_s = -1;
    int new_ratelimit_invites = -1;
    int new_ratelimit_invites_active = -1;

    cJSON *arr = NULL;
    if (cJSON_IsArray(root)) {
        arr = root;
    } else if (cJSON_IsObject(root)) {
        cJSON *jm = cJSON_GetObjectItem(root, "rules");
        if (cJSON_IsArray(jm)) arr = jm;
        cJSON *jf = cJSON_GetObjectItem(root, "fence_mode");
        if (cJSON_IsNumber(jf)) new_fence_mode = jf->valueint ? 1 : 0;
        cJSON *jmb = cJSON_GetObjectItem(root, "method_blocked");
        if (cJSON_IsNumber(jmb)) new_method_blocked = (uint32_t)jmb->valueint;

        /* fail2ban 五维（顶级 JSON 可选字段，缺省不更新） */
        cJSON *jab = cJSON_GetObjectItem(root, "auto_ban");
        if (cJSON_IsBool(jab)) new_auto_ban = cJSON_IsTrue(jab) ? 1 : 0;
        cJSON *jat = cJSON_GetObjectItem(root, "authfail_threshold");
        if (cJSON_IsNumber(jat) && jat->valueint >= 1 && jat->valueint <= 1000)
            new_authfail_threshold = jat->valueint;
        cJSON *jaw = cJSON_GetObjectItem(root, "authfail_window_s");
        if (cJSON_IsNumber(jaw) && jaw->valueint >= 1 && jaw->valueint <= 86400)
            new_authfail_window_s = jaw->valueint;
        cJSON *jbd = cJSON_GetObjectItem(root, "ban_duration_s");
        if (cJSON_IsNumber(jbd) && jbd->valueint >= 1 && jbd->valueint <= 604800)
            new_ban_duration_s = jbd->valueint;
        cJSON *jrr = cJSON_GetObjectItem(root, "ratelimit_regs");
        if (cJSON_IsNumber(jrr) && jrr->valueint >= 1 && jrr->valueint <= 1000)
            new_ratelimit_regs = jrr->valueint;
        cJSON *jrw = cJSON_GetObjectItem(root, "ratelimit_window_s");
        if (cJSON_IsNumber(jrw) && jrw->valueint >= 1 && jrw->valueint <= 3600)
            new_ratelimit_window_s = jrw->valueint;
        cJSON *jri = cJSON_GetObjectItem(root, "ratelimit_invites");
        if (cJSON_IsNumber(jri) && jri->valueint >= 0 && jri->valueint <= 1000)
            new_ratelimit_invites = jri->valueint;
        cJSON *jria = cJSON_GetObjectItem(root, "ratelimit_invites_active");
        if (cJSON_IsNumber(jria) && jria->valueint >= 0 && jria->valueint <= 1000)
            new_ratelimit_invites_active = jria->valueint;
    }
    if (!arr) {
        cJSON_Delete(root);
        if (out_err) snprintf(out_err, err_sz, "json must be array or object with rules");
        return NSBC_ERR;
    }

    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        const char *type    = cJSON_GetStringValue(cJSON_GetObjectItem(it, "type"));
        const char *kind_s  = cJSON_GetStringValue(cJSON_GetObjectItem(it, "kind"));
        const char *value   = cJSON_GetStringValue(cJSON_GetObjectItem(it, "value"));
        const char *comment = cJSON_GetStringValue(cJSON_GetObjectItem(it, "comment"));
        cJSON *jen = cJSON_GetObjectItem(it, "enabled");
        int enabled = cJSON_IsBool(jen) ? (cJSON_IsTrue(jen) ? 1 : 0) : 1;

        int kind = BL_KIND_DENY;
        if (kind_s && strcmp(kind_s, "allow") == 0) kind = BL_KIND_ALLOW;

        if (!type || !value || !value[0]) { ++skipped; continue; }

        #define FILL_CIDR(arr, arr_n, CAP, type_str) do { \
            if ((arr_n) >= (CAP)) { ++skipped; break; } \
            bl_cidr_t *r = &(arr)[(arr_n)]; \
            if (bl_parse_cidr(value, &r->family, r->net, &r->prefix) != 0) { \
                log_warn("security", "fence reload: invalid %s '%s' skipped", type_str, value); \
                ++skipped; break; \
            } \
            r->enabled = (uint8_t)enabled; \
            snprintf(r->value, sizeof(r->value), "%s", value); \
            snprintf(r->comment, sizeof(r->comment), "%s", comment ? comment : ""); \
            ++(arr_n); \
        } while (0)

        #define FILL_UA(arr, arr_n, CAP, type_str) do { \
            size_t vl = strlen(value); \
            if (vl == 0 || vl > BL_VAL_MAX || (arr_n) >= (CAP)) { ++skipped; break; } \
            bl_ua_t *r = &(arr)[(arr_n)]; \
            snprintf(r->pattern, sizeof(r->pattern), "%s", value); \
            snprintf(r->comment, sizeof(r->comment), "%s", comment ? comment : ""); \
            r->enabled = (uint8_t)enabled; \
            ++(arr_n); \
        } while (0)

        #define FILL_PREFIX(arr, arr_n, CAP, type_str) do { \
            size_t vl = strlen(value); \
            if (vl == 0 || vl > BL_VAL_MAX || (arr_n) >= (CAP)) { ++skipped; break; } \
            bl_prefix_t *r = &(arr)[(arr_n)]; \
            snprintf(r->prefix, sizeof(r->prefix), "%s", value); \
            snprintf(r->comment, sizeof(r->comment), "%s", comment ? comment : ""); \
            r->enabled = (uint8_t)enabled; \
            ++(arr_n); \
        } while (0)

        if (strcmp(type, "ip_cidr") == 0) {
            if (kind == BL_KIND_DENY) FILL_CIDR(n_dc, ndc, BL_CIDR_MAX, "cidr");
            else FILL_CIDR(n_ac, nac, BL_CIDR_MAX, "cidr");
        } else if (strcmp(type, "ua") == 0) {
            if (kind == BL_KIND_DENY) FILL_UA(n_du, ndu, BL_UA_MAX, "ua");
            else FILL_UA(n_au, nau, BL_UA_MAX, "ua");
        } else if (strcmp(type, "called_prefix") == 0) {
            if (kind == BL_KIND_DENY) FILL_PREFIX(n_dc2, ndc2, BL_PREFIX_MAX, "called_prefix");
            else FILL_PREFIX(n_ac2, nac2, BL_PREFIX_MAX, "called_prefix");
        } else if (strcmp(type, "caller_prefix") == 0) {
            if (kind == BL_KIND_DENY) FILL_PREFIX(n_dcr, ndcr, BL_PREFIX_MAX, "caller_prefix");
            else FILL_PREFIX(n_acr, nacr, BL_PREFIX_MAX, "caller_prefix");
        } else {
            log_warn("security", "fence reload: unknown type '%s' skipped", type);
            ++skipped;
        }
    }
    cJSON_Delete(root);

    pthread_rwlock_wrlock(&g_bl_rwlock);
    memcpy(g_deny_cidr, n_dc, sizeof(g_deny_cidr));
    g_deny_cidr_n = ndc;
    memcpy(g_deny_ua, n_du, sizeof(g_deny_ua));
    g_deny_ua_n = ndu;
    memcpy(g_deny_called, n_dc2, sizeof(g_deny_called));
    g_deny_called_n = ndc2;
    memcpy(g_deny_caller, n_dcr, sizeof(g_deny_caller));
    g_deny_caller_n = ndcr;
    memcpy(g_allow_cidr, n_ac, sizeof(g_allow_cidr));
    g_allow_cidr_n = nac;
    memcpy(g_allow_ua, n_au, sizeof(g_allow_ua));
    g_allow_ua_n = nau;
    memcpy(g_allow_called, n_ac2, sizeof(g_allow_called));
    g_allow_called_n = nac2;
    memcpy(g_allow_caller, n_acr, sizeof(g_allow_caller));
    g_allow_caller_n = nacr;
    pthread_rwlock_unlock(&g_bl_rwlock);

    /* fence_mode / method_blocked 独立 atomic store（整次 reload 完成后） */
    if (new_fence_mode >= 0) __atomic_store_n(&g_fence_mode, new_fence_mode, __ATOMIC_RELAXED);
    if (new_method_blocked != 0xffffffffu) __atomic_store_n(&g_method_blocked, new_method_blocked, __ATOMIC_RELAXED);

    /* fail2ban 五维直接更新 g_cfg（热路径 ban_note_* 直接读，无锁） */
    if (new_auto_ban >= 0)               g_cfg.auto_ban = new_auto_ban;
    if (new_authfail_threshold >= 0)     g_cfg.authfail_threshold = new_authfail_threshold;
    if (new_authfail_window_s >= 0)      g_cfg.authfail_window_s = new_authfail_window_s;
    if (new_ban_duration_s >= 0)         g_cfg.ban_duration_s = new_ban_duration_s;
    if (new_ratelimit_regs >= 0)         g_cfg.ratelimit_regs = new_ratelimit_regs;
    if (new_ratelimit_window_s >= 0)     g_cfg.ratelimit_window_s = new_ratelimit_window_s;
    if (new_ratelimit_invites >= 0)     g_cfg.ratelimit_invites = new_ratelimit_invites;
    if (new_ratelimit_invites_active >= 0) g_cfg.ratelimit_invites_active = new_ratelimit_invites_active;

    log_notice("security",
               "fence reload: deny(cidr=%d ua=%d called=%d caller=%d) "
               "allow(cidr=%d ua=%d called=%d caller=%d) fence=%d mblk=0x%x "
               "auto_ban=%d thr=%d win=%ds ban=%ds rl_regs=%d rl_win=%ds "
               "skipped=%d (db authoritative)",
               ndc, ndu, ndc2, ndcr, nac, nau, nac2, nacr,
               __atomic_load_n(&g_fence_mode, __ATOMIC_RELAXED),
               __atomic_load_n(&g_method_blocked, __ATOMIC_RELAXED),
               g_cfg.auto_ban, g_cfg.authfail_threshold, g_cfg.authfail_window_s,
               g_cfg.ban_duration_s, g_cfg.ratelimit_regs, g_cfg.ratelimit_window_s,
               skipped);
    return NSBC_OK;

    #undef FILL_CIDR
    #undef FILL_UA
    #undef FILL_PREFIX
}

/* NIP 默认 UA 黑名单种子（文档 ua_black.txt，60+ 条）。
 * YAML 未配置 ua_blacklist 时自动加载，零配置可用。
 * 命中这些 UA 的设备一律静默丢弃（deny）。 */
static const char *const g_nip_default_ua_deny[] = {
    "friendly-scanner", "sipvicious", "sipsak", "sip-scan", "csipsimple",
    "siparmyknife", "sipcli", "sipv", "sip-user-agent", "simpleSIP",
    "vaxsipuseragent", "vaxisipuseragent", "pplsip", "3cxphonesystem",
    "miz-phone", "ozeki", "hello", "voip", "test agent", "agfeo",
    "telovoxpbx", "zte", "communigatepro", "mitel", "polycom",
    "tandberg", "abto", "pjsip", "avaya", "cisco", "cisco-sipgateway",
    "cisco-cucm", "fpbx", "webitel", "technicolor", "sphairon",
    "smartswitch", "linksys", "astpp", "kamailio", "sippy", "mera",
    "independant", "skipper", "rv", "iwar", "sundayddr", "gulp",
    "sivus", "devolo", "&", "voxalot", "zxcvfdf11", "fgfd", "97",
    "ipbx-2.11.0", "nxnxbxa", "tramb2017", "mic", "tmd", "badua",
    NULL  /* sentinel */
};

/* YAML 种子加载（init 一次）：security.blacklist_cidrs /
 * security.ua_blacklist 逗号列表 → deny 表。YAML ua_blacklist 为空时
 * 自动追加 NIP 默认 60+ 条 UA 黑名单，零配置可用。
 * Web 推送到达后被 DB 全量替换（数据库为准，YAML/NIP 仅 fallback）。 */
static void bl_seed_from_cfg(void)
{
    pthread_rwlock_wrlock(&g_bl_rwlock);
    g_deny_cidr_n = 0;  g_deny_ua_n = 0;
    g_deny_called_n = 0; g_deny_caller_n = 0;
    g_allow_cidr_n = 0; g_allow_ua_n = 0;
    g_allow_called_n = 0; g_allow_caller_n = 0;

    /* 1. YAML blacklist_cidrs → deny cidr */
    if (g_cfg.blacklist_cidrs[0]) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", g_cfg.blacklist_cidrs);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save)) {
            if (g_deny_cidr_n >= BL_CIDR_MAX) break;
            bl_cidr_t *r = &g_deny_cidr[g_deny_cidr_n];
            if (bl_parse_cidr(tok, &r->family, r->net, &r->prefix) != 0) {
                log_warn("security",
                         "fence seed: invalid cidr '%s' skipped", tok);
                memset(r, 0, sizeof(*r));
                continue;
            }
            r->enabled = 1;
            snprintf(r->value, sizeof(r->value), "%s", tok);
            snprintf(r->comment, sizeof(r->comment), "yaml");
            ++g_deny_cidr_n;
        }
    }
    /* 2. YAML ua_blacklist → deny ua */
    int yaml_ua_n = 0;
    if (g_cfg.ua_blacklist[0]) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", g_cfg.ua_blacklist);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save)) {
            if (g_deny_ua_n >= BL_UA_MAX) break;
            size_t tl = strlen(tok);
            if (tl == 0 || tl > BL_VAL_MAX) continue;
            bl_ua_t *r = &g_deny_ua[g_deny_ua_n];
            snprintf(r->pattern, sizeof(r->pattern), "%s", tok);
            snprintf(r->comment, sizeof(r->comment), "yaml");
            r->enabled = 1;
            ++g_deny_ua_n; ++yaml_ua_n;
        }
    }
    /* 3. YAML ua_blacklist 为空 → 追加 NIP 默认 UA 黑名单 */
    if (yaml_ua_n == 0) {
        for (const char *const *pp = g_nip_default_ua_deny; *pp; ++pp) {
            if (g_deny_ua_n >= BL_UA_MAX) break;
            if (!*pp || !**pp) continue;
            size_t tl = strlen(*pp);
            if (tl >= BL_VAL_MAX) continue;
            bl_ua_t *r = &g_deny_ua[g_deny_ua_n];
            snprintf(r->pattern, sizeof(r->pattern), "%s", *pp);
            snprintf(r->comment, sizeof(r->comment), "nip-default");
            r->enabled = 1;
            ++g_deny_ua_n;
        }
    }
    int nc = g_deny_cidr_n, nu = g_deny_ua_n;
    pthread_rwlock_unlock(&g_bl_rwlock);

    if (yaml_ua_n == 0)
        log_info("security",
                 "fence seed: deny(cidr=%d ua=%d) [nip-default ua loaded]",
                 nc, nu);
    else
        log_info("security",
                 "fence seed: deny(cidr=%d ua=%d) [yaml]", nc, nu);
}

/* === b2b_register_hook 回调 ===
 * security 在 REGISTER 链最前置位置：
 *   1. 统一静态黑名单（IP CIDR + UA，Web 安全中心管理）→ PERM_DENIED
 *      （文档 §4：置于 auto-ban 之前；命中不进 401/403、不计入 auto-ban）
 *   2. 动态封禁表命中（auto_ban）→ PERM_DENIED（silent drop）
 *   3. 静态 CIDR 黑名单命中（block_on_register，YAML 旧路径）→ PERM_DENIED
 * 未命中 → NSBC_OK（让链继续走 registrar / 其他模块）。 */
static int sec_reg_cb(sip_tr_proto_t proto, const nsbc_sa_t *src,
                      sip_msg_t *m, const char *raw, size_t rawlen,
                      sip_txn_t *txn, void *ud)
{
    (void)proto; (void)raw; (void)rawlen; (void)txn;
    security_cfg_t *cfg = (security_cfg_t *)ud;
    if (!cfg) return NSBC_OK;

    char ip[INET6_ADDRSTRLEN] = {0};
    sa_to_ip_str(src, ip, sizeof(ip));

    /* 1. NIP 完整围栏：method + IP + UA（REGISTER 无主叫/被叫）
     *    命中 → 静默丢弃 NSBC_PERM_DENIED（不进 401/403、不计入 auto-ban）。
     *    优先级：method 禁用 → deny 全表 → allowlist 未命中。 */
    {
        char ua[256] = {0};
        if (m) {
            sip_hdr_t *uh = sip_hdr_get(m, SIP_H_USER_AGENT);
            if (uh && uh->value.p && uh->value.len > 0) {
                size_t un = uh->value.len;
                if (un > sizeof(ua) - 1) un = sizeof(ua) - 1;
                memcpy(ua, uh->value.p, un);
                ua[un] = '\0';
            }
        }
        security_fence_ctx_t fctx = {0};
        fctx.src = (const struct sockaddr *)src;
        fctx.method = SIP_M_REGISTER;
        fctx.ua = ua;
        security_check_fence(&fctx);
        if (fctx.hit != BLACKLIST_OK) {
            security_fence_log_drop(fctx.hit, ip, ua, "REGISTER", NULL, NULL);
            return NSBC_PERM_DENIED;
        }
    }

    if (!ip[0]) return NSBC_OK;  /* AF_UNSPEC 空地址 → 放行（WS 桥等） */

    /* Ratelimit：纯频率，覆盖 MicroSIP 扫用户表场景。
     * 阈值/窗口从 g_cfg 读（init / Web reload 均可调整）。 */
    if (cfg->auto_ban && ban_note_reg_req(ip)) {
        log_warn("security",
                 "ratelimit auto-ban ip=%s: %d REGISTERs in %ds window, banning %ds",
                 ip, cfg->ratelimit_regs, cfg->ratelimit_window_s,
                 ban_duration_for_strikes(cfg->ban_duration_s, 1));
        return NSBC_PERM_DENIED;
    }

    /* 动态封禁：命中即静默丢（限频采样日志，防攻击流量刷屏） */
    if (cfg->auto_ban && ban_is_banned(ip)) {
        static unsigned ban_drop_n = 0;
        if (ban_drop_n++ % 512 == 0)
            log_notice("security",
                       "REGISTER silent-drop: banned ip=%s (dropped=%u)",
                       ip, ban_drop_n);
        return NSBC_PERM_DENIED;
    }

    if (!cfg->block_on_register) return NSBC_OK;  /* 静态黑名单未启用 → 放行 */
    if (security_check_ip(ip, cfg)) {
        log_notice("security", "REGISTER PERM_DENIED ip=%s (blacklist)", ip);
        return NSBC_PERM_DENIED;   /* ★ silent drop：黑产连 401 都收不到 */
    }
    return NSBC_OK;
}

/* ==================================================================
 * 内置动态封禁（in-process fail2ban）
 *
 * 公网暴露后恶意 REGISTER 暴力破解：认证失败在窗口内累计超阈值
 * → 自动封禁源 IP。封禁期间 REGISTER（sec_reg_cb）与 INVITE
 * （guard 链）均静默丢弃。表为内存态，重启进程即清零（解封途径）。
 *
 * 实现：FNV-1a 哈希 + 桶链表 + 全局 mutex（攻击流量为每秒百级，
 * 单锁足够；正常呼叫路径不受影响——只在 REGISTER 403 后计数）。
 * ==================================================================*/
typedef struct ban_ent {
    char ip[INET6_ADDRSTRLEN];
    int  fails;              /* 窗口内累计 authfail 次数 */
    int  reqs;               /* 窗口内累计 REGISTER 请求次数（ratelimit） */
    int  inv_req;            /* 被动：窗口内累计 INVITE/REGISTER 事务层分配失败次数
                             * （txn slab NOMEM/NOSPC 时 b2bua 上报的 flood 指标） */
    int  inv_count;          /* 主动：窗口内累计新 INVITE 到达次数（per-IP 频率）
                             * b2bua 在 txn_server_input 之前上报；
                             * 阈值 ratelimit_invites_active，共用 ratelimit_window_s。 */
    uint64_t win_start;      /* 计数窗口起点（mono ms） */
    uint64_t ban_until;      /* 封禁到期时刻（mono ms） */
    uint64_t last_act;       /* 最近活动（满表驱逐用） */
    int  strikes;            /* 累计封禁次数（递进封禁） */
    struct ban_ent *next;
} ban_ent_t;

#define BAN_HASH_BUCKETS 256   /* 2 的幂（与 hash 掩码对齐） */
#define BAN_MAX_ENTRIES  4096

static pthread_mutex_t g_ban_lock = PTHREAD_MUTEX_INITIALIZER;
static ban_ent_t *g_ban_tab[BAN_HASH_BUCKETS];
static int g_ban_count;

/* FNV-1a 32 位（项目惯例，与 cachedb 同款） */
static unsigned ban_hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h & (BAN_HASH_BUCKETS - 1);
}

static ban_ent_t *ban_find_locked(const char *ip)
{
    for (ban_ent_t *e = g_ban_tab[ban_hash(ip)]; e; e = e->next)
        if (strcmp(e->ip, ip) == 0)
            return e;
    return NULL;
}

/* 满表驱逐（锁内）：先清「封禁与窗口均已过期」的死项；
 * 仍满则驱逐 last_act 最旧的（正常客户 IP 很少触达 4096 上限）。
 * 返回驱逐后是否腾出空间。 */
static int ban_evict_locked(uint64_t now)
{
    int freed = 0;
    for (int b = 0; b < BAN_HASH_BUCKETS; ++b) {
        ban_ent_t **pp = &g_ban_tab[b];
        while (*pp) {
            ban_ent_t *e = *pp;
            uint64_t win_end = e->win_start +
                (uint64_t)g_cfg.authfail_window_s * 1000;
            if (e->ban_until <= now && win_end <= now) {
                *pp = e->next;
                free(e);
                --g_ban_count;
                ++freed;
            } else {
                pp = &e->next;
            }
        }
    }
    if (freed > 0)
        return 1;
    /* 全是活跃项：驱逐最旧 */
    ban_ent_t *oldest = NULL;
    for (int b = 0; b < BAN_HASH_BUCKETS; ++b)
        for (ban_ent_t *e = g_ban_tab[b]; e; e = e->next)
            if (!oldest || e->last_act < oldest->last_act)
                oldest = e;
    if (!oldest)
        return g_ban_count < BAN_MAX_ENTRIES;
    log_warn("security", "ban table full, evict oldest ip=%s", oldest->ip);
    ban_ent_t **pp = &g_ban_tab[ban_hash(oldest->ip)];
    while (*pp && *pp != oldest)
        pp = &(*pp)->next;
    if (*pp) {
        *pp = oldest->next;
        free(oldest);
        --g_ban_count;
    }
    return 1;
}

/* 递进封禁时长：第 n 次封禁 = base * 2^(n-1)，上限 7 天 */
static uint32_t ban_duration_for_strikes(uint32_t base_s, int strikes)
{
    if (strikes <= 1) return base_s;
    uint64_t d = (uint64_t)base_s << (strikes - 1);
    if (d > 7*86400) d = 7*86400;
    return (uint32_t)d;
}

/* 认证失败上报：窗口内计数，超阈值递进封禁。
 * 返回 1=本次上报触发了封禁。 */
static int ban_note_fail(const char *ip)
{
    uint64_t now = nsbc_now_ms();
    int banned_now = 0;
    pthread_mutex_lock(&g_ban_lock);
    ban_ent_t *e = ban_find_locked(ip);
    if (!e) {
        if (g_ban_count >= BAN_MAX_ENTRIES && !ban_evict_locked(now)) {
            pthread_mutex_unlock(&g_ban_lock);
            return 0;
        }
        e = calloc(1, sizeof(*e));
        if (!e) { pthread_mutex_unlock(&g_ban_lock); return 0; }
        snprintf(e->ip, sizeof(e->ip), "%s", ip);
        e->win_start = now;
        unsigned b = ban_hash(ip);
        e->next = g_ban_tab[b];
        g_ban_tab[b] = e;
        ++g_ban_count;
    }
    e->last_act = now;
    if (e->ban_until > now) { pthread_mutex_unlock(&g_ban_lock); return 0; }
    if (now - e->win_start > (uint64_t)g_cfg.authfail_window_s * 1000) {
        e->win_start = now; e->fails = 0;
    }
    if (++e->fails >= g_cfg.authfail_threshold) {
        e->strikes++;
        e->ban_until = now + (uint64_t)ban_duration_for_strikes(g_cfg.ban_duration_s, e->strikes) * 1000;
        banned_now = 1;
    }
    pthread_mutex_unlock(&g_ban_lock);
    return banned_now;
}

/* REGISTER ratelimit：纯频率，窗口内超阈值直接封（不管认证成败）。
 * 覆盖 MicroSIP 扫用户表场景——不停 REGISTER 触发 404/403 但没 authfail。
 * 阈值/窗口从 g_cfg 读（init / Web reload 均可调整）。 */
static int ban_note_reg_req(const char *ip)
{
    uint64_t now = nsbc_now_ms();
    int banned_now = 0;
    int window_s = g_cfg.ratelimit_window_s > 0 ? g_cfg.ratelimit_window_s : 60;
    int regs_thr = g_cfg.ratelimit_regs > 0 ? g_cfg.ratelimit_regs : 20;
    pthread_mutex_lock(&g_ban_lock);
    ban_ent_t *e = ban_find_locked(ip);
    if (!e) {
        if (g_ban_count >= BAN_MAX_ENTRIES && !ban_evict_locked(now)) {
            pthread_mutex_unlock(&g_ban_lock);
            return 0;
        }
        e = calloc(1, sizeof(*e));
        if (!e) { pthread_mutex_unlock(&g_ban_lock); return 0; }
        snprintf(e->ip, sizeof(e->ip), "%s", ip);
        e->win_start = now;
        unsigned b = ban_hash(ip);
        e->next = g_ban_tab[b];
        g_ban_tab[b] = e;
        ++g_ban_count;
    }
    e->last_act = now;
    if (e->ban_until > now) { pthread_mutex_unlock(&g_ban_lock); return 0; }
    if (now - e->win_start > (uint64_t)window_s * 1000) {
        e->win_start = now; e->reqs = 0;
    }
    if (++e->reqs >= regs_thr) {
        e->strikes++;
        e->ban_until = now + (uint64_t)ban_duration_for_strikes(g_cfg.ban_duration_s, e->strikes) * 1000;
        banned_now = 1;
    }
    pthread_mutex_unlock(&g_ban_lock);
    return banned_now;
}

/* INVITE/REGISTER 事务层分配失败 ratelimit：与 REGISTER ratelimit 共用窗口秒，
 * 阈值由 ratelimit_invites 控制（默认 30，设 0 关闭）。
 * b2bua 在 txn_server_input 返回负码（slab 满/NOMEM/NOSPC）时调此函数。
 * 典型场景：攻击者 flood 大量不同 branch 的 INVITE 打满 slab → 自己先被封，
 * 正常合法呼叫不受影响（阈值足够大时，正常呼叫不可能打满）。 */
static int ban_note_inv_fail(const char *ip)
{
    if (!g_cfg.auto_ban) return 0;
    int window_s = g_cfg.ratelimit_window_s > 0 ? g_cfg.ratelimit_window_s : 60;
    int thr = g_cfg.ratelimit_invites > 0 ? g_cfg.ratelimit_invites : 30;
    if (thr <= 0) return 0;
    uint64_t now = nsbc_now_ms();
    int banned_now = 0;
    pthread_mutex_lock(&g_ban_lock);
    ban_ent_t *e = ban_find_locked(ip);
    if (!e) {
        if (g_ban_count >= BAN_MAX_ENTRIES && !ban_evict_locked(now)) {
            pthread_mutex_unlock(&g_ban_lock);
            return 0;
        }
        e = calloc(1, sizeof(*e));
        if (!e) { pthread_mutex_unlock(&g_ban_lock); return 0; }
        snprintf(e->ip, sizeof(e->ip), "%s", ip);
        e->win_start = now;
        unsigned b = ban_hash(ip);
        e->next = g_ban_tab[b];
        g_ban_tab[b] = e;
        ++g_ban_count;
    }
    e->last_act = now;
    if (e->ban_until > now) { pthread_mutex_unlock(&g_ban_lock); return 0; }
    if (now - e->win_start > (uint64_t)window_s * 1000) {
        e->win_start = now; e->inv_req = 0;
    }
    if (++e->inv_req >= thr) {
        e->strikes++;
        e->ban_until = now + (uint64_t)ban_duration_for_strikes(g_cfg.ban_duration_s, e->strikes) * 1000;
        banned_now = 1;
    }
    pthread_mutex_unlock(&g_ban_lock);
    return banned_now;
}

/* 主动 INVITE flood 检测：per-IP 窗口内新 INVITE 到达次数。
 * 阈值 ratelimit_invites_active（默认 20），共用 ratelimit_window_s（默认 60s）。
 * 与 ban_note_inv_fail 被动阈值独立——攻击者还没打满 slab 就被封。
 * 正常合法呼叫不会触发（坐席并发远低于 20 INVITE/s per IP）。
 * b2bua 在 txn_server_input 之前调本函数（纯 notify，返回值忽略）。 */
static int ban_note_inv_count(const char *ip)
{
    if (!g_cfg.auto_ban) return 0;
    int window_s = g_cfg.ratelimit_window_s > 0 ? g_cfg.ratelimit_window_s : 60;
    int thr = g_cfg.ratelimit_invites_active > 0 ? g_cfg.ratelimit_invites_active : 20;
    if (thr <= 0) return 0;
    uint64_t now = nsbc_now_ms();
    int banned_now = 0;
    pthread_mutex_lock(&g_ban_lock);
    ban_ent_t *e = ban_find_locked(ip);
    if (!e) {
        if (g_ban_count >= BAN_MAX_ENTRIES && !ban_evict_locked(now)) {
            pthread_mutex_unlock(&g_ban_lock);
            return 0;
        }
        e = calloc(1, sizeof(*e));
        if (!e) { pthread_mutex_unlock(&g_ban_lock); return 0; }
        snprintf(e->ip, sizeof(e->ip), "%s", ip);
        e->win_start = now;
        unsigned b = ban_hash(ip);
        e->next = g_ban_tab[b];
        g_ban_tab[b] = e;
        ++g_ban_count;
    }
    e->last_act = now;
    if (e->ban_until > now) { pthread_mutex_unlock(&g_ban_lock); return 0; }
    if (now - e->win_start > (uint64_t)window_s * 1000) {
        e->win_start = now; e->inv_count = 0;
    }
    if (++e->inv_count >= thr) {
        e->strikes++;
        e->ban_until = now + (uint64_t)ban_duration_for_strikes(g_cfg.ban_duration_s, e->strikes) * 1000;
        banned_now = 1;
    }
    pthread_mutex_unlock(&g_ban_lock);
    return banned_now;
}

/* 成功清零：auth OK 后清除该 IP 的 fails/reqs/inv_req/inv_count 计数。
 * 正常用户偶尔输错密码不会被封死。 */
static void ban_clear_counts(const char *ip)
{
    uint64_t now = nsbc_now_ms();
    pthread_mutex_lock(&g_ban_lock);
    ban_ent_t *e = ban_find_locked(ip);
    if (e && e->ban_until <= now) {
        e->fails = 0;
        e->reqs = 0;
        e->inv_req = 0;
        e->inv_count = 0;
        e->win_start = now;
        e->last_act = now;
    }
    pthread_mutex_unlock(&g_ban_lock);
}

/* 封禁查询：1=封禁中（顺手刷新 last_act 供驱逐参考） */
static int ban_is_banned(const char *ip)
{
    uint64_t now = nsbc_now_ms();
    int hit = 0;
    pthread_mutex_lock(&g_ban_lock);
    ban_ent_t *e = ban_find_locked(ip);
    if (e) {
        e->last_act = now;
        if (e->ban_until > now)
            hit = 1;
    }
    pthread_mutex_unlock(&g_ban_lock);
    return hit;
}

/* reactor 层前置封禁/黑名单查询（recvfrom 后 parse 前调用）。
 * 只查 IP：ban_is_banned（动态 auto-ban 表）+ security_check_ip（静态 CIDR 黑名单）。
 * 不查 UA/号码前缀——它们需要 parse 后的 SIP 头，放在 b2bua 层更合适。
 * 返回 1=静默丢弃，0=放行。 */
int security_is_blocked(const nsbc_sa_t *src)
{
    if (!src) return 0;
    /* Key must be the PURE IP (no port): nsbc_sa_fmt() appends ":port",
     * which never matches the ban table (keyed by pure IP from
     * registrar/guard paths) nor parses as an address for CIDR matching.
     * sa_to_ip_str() is the same formatter the ban producers use. */
    char ip[INET6_ADDRSTRLEN] = {0};
    sa_to_ip_str(src, ip, sizeof(ip));
    if (!ip[0]) return 0;
    /* 1. 动态封禁表（auto-ban） */
    if (ban_is_banned(ip)) return 1;
    /* 2. 静态 CIDR 黑名单（仅 block_on_register/invite 启用时） */
    if ((g_cfg.block_on_register || g_cfg.block_on_invite) &&
        security_check_ip(ip, &g_cfg))
        return 1;
    return 0;
}

/* CDR END emit 时查 threat — 只读、无副作用、线程安全。
 * 顺序：ban（已封禁）→ blacklist（静态 deny）→ return 1。
 * 两个查询都加各自的锁，都是 hash 桶链表遍历（O(1)~O(k)）。 */
int security_get_threat(const char *ip, char *reason_out, size_t reason_sz)
{
    if (!reason_out || reason_sz == 0) return 0;
    reason_out[0] = '\0';
    if (!ip || !ip[0]) return 0;

    /* 1. 动态封禁表（优先级最高 — 已被 auto_ban 判定为恶意） */
    pthread_mutex_lock(&g_ban_lock);
    ban_ent_t *be = ban_find_locked(ip);
    pthread_mutex_unlock(&g_ban_lock);
    if (be && be->ban_until > nsbc_now_ms()) {
        snprintf(reason_out, reason_sz, "ban");
        return 1;
    }

    /* 2. 静态黑名单（CIDR / UA deny 表） */
    if (g_cfg.block_on_register || g_cfg.block_on_invite) {
        if (security_check_ip(ip, &g_cfg)) {
            snprintf(reason_out, reason_sz, "blacklist");
            return 1;
        }
    }

    return 0;
}

static void ban_clear_all(void)
{
    pthread_mutex_lock(&g_ban_lock);
    for (int b = 0; b < BAN_HASH_BUCKETS; ++b) {
        ban_ent_t *e = g_ban_tab[b];
        while (e) {
            ban_ent_t *n = e->next;
            free(e);
            e = n;
        }
        g_ban_tab[b] = NULL;
    }
    g_ban_count = 0;
    pthread_mutex_unlock(&g_ban_lock);
}

/* === b2b_guard_hook 回调（INVITE 入口检查 + 认证失败上报） === */
static int sec_guard_cb(const char *src_ip, b2b_guard_evt_t evt, void *ud)
{
    (void)ud;
    if (!src_ip || !src_ip[0]) return NSBC_OK;

    if (evt == B2B_GUARD_AUTH_FAIL) {
        if (!g_cfg.auto_ban) return NSBC_OK;
        if (ban_note_fail(src_ip)) {
            log_warn("security",
                     "auto-ban ip=%s: %d auth failures in %ds window, "
                     "banning for %ds (silent-drop REGISTER/INVITE)",
                     src_ip, g_cfg.authfail_threshold,
                     g_cfg.authfail_window_s, g_cfg.ban_duration_s);
        }
        return NSBC_OK;
    }

    if (evt == B2B_GUARD_AUTH_OK) {
        if (g_cfg.auto_ban) ban_clear_counts(src_ip);
        return NSBC_OK;
    }

    if (evt == B2B_GUARD_INVITE_FAIL) {
        /* b2bua 在 txn_server_input 返回负码时上报（slab 满/NOMEM/NOSPC）。
         * 累计 inv_req 超 ratelimit_invites 阈值 → auto-ban。
         * 正常合法呼叫不可能在短时间内打满 slab，攻击者 flood 才会触发。 */
        if (ban_note_inv_fail(src_ip)) {
            log_warn("security",
                     "auto-ban ip=%s: %d txn slab alloc-fails in %ds window, "
                     "banning for %ds (INVITE/REGISTER flood detected)",
                     src_ip, g_cfg.ratelimit_invites > 0 ? g_cfg.ratelimit_invites : 30,
                     g_cfg.ratelimit_window_s > 0 ? g_cfg.ratelimit_window_s : 60,
                     ban_duration_for_strikes(g_cfg.ban_duration_s, 1));
        }
        return NSBC_OK;  /* 纯上报，拦截在 ban_is_banned 处生效 */
    }

    if (evt == B2B_GUARD_INVITE_COUNT) {
        /* b2bua 在 txn_server_input 之前上报每个新 INVITE（主动防护）。
         * 累计 inv_count 超 ratelimit_invites_active 阈值 → auto-ban。
         * 攻击者还没造成 slab 压力就被封，正常合法呼叫不会触发。 */
        if (ban_note_inv_count(src_ip)) {
            log_warn("security",
                     "auto-ban ip=%s: %d new INVITEs in %ds window, "
                     "banning for %ds (INVITE flood detected, active ratelimit)",
                     src_ip, g_cfg.ratelimit_invites_active > 0 ? g_cfg.ratelimit_invites_active : 20,
                     g_cfg.ratelimit_window_s > 0 ? g_cfg.ratelimit_window_s : 60,
                     ban_duration_for_strikes(g_cfg.ban_duration_s, 1));
        }
        return NSBC_OK;  /* 纯上报，拦截在 ban_is_banned 处生效 */
    }

    /* B2B_GUARD_INVITE：动态封禁 + 静态 CIDR 黑名单 */
    if (g_cfg.auto_ban && ban_is_banned(src_ip)) {
        static unsigned ban_drop_n = 0;
        if (ban_drop_n++ % 512 == 0)
            log_notice("security",
                       "INVITE silent-drop: banned ip=%s (dropped=%u)",
                       src_ip, ban_drop_n);
        return NSBC_PERM_DENIED;
    }
    if (g_cfg.block_on_invite && security_check_ip(src_ip, &g_cfg)) {
        log_notice("security", "INVITE PERM_DENIED ip=%s (blacklist)", src_ip);
        return NSBC_PERM_DENIED;
    }
    return NSBC_OK;
}

/* ==================================================================
 * OAM 命令（CCMD 注册表）：Web 封禁管理（查看/搜索/解禁）
 *   security.ban.list [filter]   列出封禁+观察条目（filter=IP 子串）
 *   security.ban.unban <ip>      解禁（清除该 IP 全部状态）
 *   security.ban.clear           清空整表（应急）
 * ==================================================================*/
static int cmd_ban_list(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)ud;
    const char *filter = NULL;
    if (argc >= 1 && argv[0][0]) {
        char *s = argv[0];
        /* OAM 框架 nc 管道调用 argv[0] 会把命令名也塞进来：
         *  "security.ban.list"       → 无 filter
         *  "security.ban.list 192"  → filter = "192"
         *  "192"                    → 直接就是 filter（绕过命令名）
         *  先剥命令名前缀再 trim 才是真正的 filter */
        const char *cmd = "security.ban.list";
        size_t cmdlen = strlen(cmd);
        if (strncmp(s, cmd, cmdlen) == 0) {
            s += cmdlen;  /* 跳过命令名 */
        }
        /* trim 前后空白 + \r\n */
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        size_t len = strlen(s);
        while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                           s[len-1] == '\r' || s[len-1] == '\n'))
            s[--len] = '\0';
        if (s[0]) filter = s;
    }
    uint64_t now = nsbc_now_ms();

    /* 锁内快照到堆数组，锁外输出——io_printf 走网络 send，禁止持
     * g_ban_lock 阻塞（REGISTER 热路径的 ban_is_banned 在等这把锁）。 */
    typedef struct {
        char ip[INET6_ADDRSTRLEN];
        int banned, fails;
        uint64_t remain, ago;
    } ban_snap_t;
    ban_snap_t *snaps = NULL;
    int n_out = 0, n_banned = 0, total = 0;

    pthread_mutex_lock(&g_ban_lock);
    int nb = 0, sum = 0;
    /* ★ DIAG: 先无条件 dump 所有 bucket 条目（锁内，不管 filter） */
    for (int b = 0; b < BAN_HASH_BUCKETS; ++b) {
        for (ban_ent_t *e = g_ban_tab[b]; e; e = e->next) {
            sum++;
            if (g_ban_tab[b]) nb++;
        }
    }
    if (g_ban_count > 0)
        snaps = calloc((size_t)g_ban_count, sizeof(*snaps));
    /* ★ DIAG: 直接遍历打 ip，跟下面的 snaps 循环对比 */
    c->io_printf(c->io_ud, "DIAG_RAW buckets=%d sum=%d g_ban_count=%d\n",
                 nb, sum, g_ban_count);
    for (int b = 0; b < BAN_HASH_BUCKETS; ++b) {
        for (ban_ent_t *e = g_ban_tab[b]; e; e = e->next) {
            c->io_printf(c->io_ud, "DIAG_RAW ip=%s banned=%d\n",
                         e->ip, e->ban_until > now);
        }
    }
    if (snaps) {
        for (int b = 0; b < BAN_HASH_BUCKETS; ++b) {
            for (ban_ent_t *e = g_ban_tab[b]; e; e = e->next) {
                if (filter && !strstr(e->ip, filter))
                    continue;
                ban_snap_t *s = &snaps[n_out++];
                snprintf(s->ip, sizeof(s->ip), "%s", e->ip);
                s->banned = e->ban_until > now;
                s->fails = e->fails;
                s->remain = s->banned ? (e->ban_until - now) / 1000 + 1 : 0;
                s->ago = now > e->last_act ? (now - e->last_act) / 1000 : 0;
                if (s->banned) ++n_banned;
            }
        }
    }
    total = g_ban_count;
    pthread_mutex_unlock(&g_ban_lock);

    /* 诊断行（仅当异常时显示，帮定位 total/shown 不一致） */
    if (!snaps || n_out != total || sum != total) {
        c->io_printf(c->io_ud,
                     "DIAG g_ban_count=%d bucket_sum=%d nonempty_buckets=%d "
                     "snaps_null=%d n_out=%d filter=%s\n",
                     total, sum, nb, snaps == NULL, n_out,
                     filter ? filter : "(null)");
    }

    for (int i = 0; i < n_out; ++i)
        c->io_printf(c->io_ud,
                     "ent ip=%s state=%s fails=%d remaining_s=%llu "
                     "act_ago_s=%llu\n",
                     snaps[i].ip, snaps[i].banned ? "ban" : "watch",
                     snaps[i].fails, (unsigned long long)snaps[i].remain,
                     (unsigned long long)snaps[i].ago);
    free(snaps);

    /* 汇总行在尾（Web 解析只认 ent 行，CLI 用户看统计） */
    c->io_printf(c->io_ud, "bans=%d shown=%d total=%d\n",
                 n_banned, n_out, total);
    return NSBC_OK;
}

static int cmd_ban_unban(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)ud;
    if (argc < 1 || !argv[0][0]) {
        c->io_printf(c->io_ud, "ERR usage: security.ban.unban <ip>\n");
        return NSBC_ERR;
    }
    /* OAM 框架 argv 有两种布局：
     *   echo "security.ban.unban 1.2.3.4" | nc → argv[0]="security.ban.unban 1.2.3.4"
     *   或分参数模式                              → argv[0]="security.ban.unban", argv[1]="1.2.3.4"
     * 先按粘合格式剥前缀，剥完空则 fallback 到 argv[1] */
    char ip[INET6_ADDRSTRLEN * 2];
    const char *cmd = "security.ban.unban";
    size_t cmdlen = strlen(cmd);
    if (strncmp(argv[0], cmd, cmdlen) == 0) {
        strncpy(ip, argv[0] + cmdlen, sizeof(ip) - 1);
        ip[sizeof(ip) - 1] = '\0';
    } else {
        strncpy(ip, argv[0], sizeof(ip) - 1);
        ip[sizeof(ip) - 1] = '\0';
    }
    /* trim 空白 */
    char *p = ip;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    size_t ilen = strlen(p);
    while (ilen > 0 && (p[ilen-1] == ' ' || p[ilen-1] == '\t' ||
                        p[ilen-1] == '\r' || p[ilen-1] == '\n'))
        p[--ilen] = '\0';
    if (!p[0] && argc >= 2 && argv[1] && argv[1][0]) {
        /* fallback：分参数模式的第二个参数 */
        p = argv[1];
        ilen = strlen(p);
        while (ilen > 0 && (p[ilen-1] == ' ' || p[ilen-1] == '\t' ||
                            p[ilen-1] == '\r' || p[ilen-1] == '\n'))
            p[--ilen] = '\0';
    }
    if (!p[0]) {
        c->io_printf(c->io_ud, "ERR ip empty after trim\n");
        return NSBC_ERR;
    }
    pthread_mutex_lock(&g_ban_lock);
    ban_ent_t **pp = &g_ban_tab[ban_hash(p)];
    int removed = 0;
    while (*pp) {
        if (strcmp((*pp)->ip, p) == 0) {
            ban_ent_t *e = *pp;
            *pp = e->next;
            free(e);
            --g_ban_count;
            ++removed;
        } else {
            pp = &(*pp)->next;
        }
    }
    pthread_mutex_unlock(&g_ban_lock);
    if (removed) {
        log_notice("security", "unban ip=%s (via OAM, entry cleared)", p);
        c->io_printf(c->io_ud, "OK security.ban.unban ip=%s removed=%d\n",
                     p, removed);
        return NSBC_OK;
    }
    c->io_printf(c->io_ud, "ERR security.ban.unban ip=%s not found\n", p);
    return NSBC_ERR;
}

static int cmd_ban_clear(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)argc; (void)argv; (void)ud;
    ban_clear_all();
    log_warn("security", "ban table cleared (via OAM)");
    c->io_printf(c->io_ud, "OK security.ban.clear table emptied\n");
    return NSBC_OK;
}

/* ==================================================================
 * OAM 命令：静态黑名单管理（Web 安全中心 ↔ C 核心同步链路）
 *   security.blacklist.list           列出 CIDR + UA 规则（L1 诊断）
 *   security.blacklist.reload <json>  Web DB 全量替换（L2，同 profiles.reload 模式）
 * ==================================================================*/
static int cmd_bl_list(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)argc; (void)argv; (void)ud;

    /* 锁内整表快照到堆，锁外输出——io_printf 走网络 send，
     * 禁止持 g_bl_rwlock（SIP 热路径的 check 在等读锁）。 */
    bl_cidr_t *sc = calloc(BL_CIDR_MAX, sizeof(*sc));
    bl_ua_t   *su = calloc(BL_UA_MAX, sizeof(*su));
    if (!sc || !su) {
        free(sc); free(su);
        c->io_printf(c->io_ud, "ERR security.blacklist.list: oom\n");
        return NSBC_ERR;
    }
    int nc, nu;
    pthread_rwlock_rdlock(&g_bl_rwlock);
    memcpy(sc, g_deny_cidr, sizeof(g_deny_cidr));
    nc = g_deny_cidr_n;
    memcpy(su, g_deny_ua, sizeof(g_deny_ua));
    nu = g_deny_ua_n;
    pthread_rwlock_unlock(&g_bl_rwlock);

    for (int i = 0; i < nc; ++i)
        c->io_printf(c->io_ud,
                     "cidr idx=%d value=%s enabled=%d comment=%s\n",
                     i + 1, sc[i].value, sc[i].enabled ? 1 : 0,
                     sc[i].comment[0] ? sc[i].comment : "-");
    for (int i = 0; i < nu; ++i)
        c->io_printf(c->io_ud,
                     "ua idx=%d value=%s enabled=%d comment=%s\n",
                     i + 1, su[i].pattern, su[i].enabled ? 1 : 0,
                     su[i].comment[0] ? su[i].comment : "-");
    free(sc);
    free(su);

    c->io_printf(c->io_ud, "blacklists cidr=%d ua=%d\n", nc, nu);
    return NSBC_OK;
}

static int cmd_bl_reload(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)ud;
    if (argc < 2 || !argv[1][0]) {
        c->io_printf(c->io_ud, "ERR usage: security.blacklist.reload <json>\n");
        return NSBC_ERR;
    }
    if (strlen(argv[1]) > 256 * 1024) {
        c->io_printf(c->io_ud, "ERR json too large (max 262144)\n");
        return NSBC_ERR;
    }
    char err[256] = {0};
    if (security_blacklist_reload_json(argv[1], err, sizeof(err)) != NSBC_OK) {
        c->io_printf(c->io_ud, "ERR security.blacklist.reload: %s\n",
                     err[0] ? err : "invalid");
        return NSBC_ERR;
    }
    int nc, nu;
    pthread_rwlock_rdlock(&g_bl_rwlock);
    nc = g_deny_cidr_n;
    nu = g_deny_ua_n;
    pthread_rwlock_unlock(&g_bl_rwlock);
    c->io_printf(c->io_ud,
                 "OK security.blacklist.reload loaded cidr=%d ua=%d\n", nc, nu);
    return NSBC_OK;
}

/* security.cfg.reload —— 从 YAML 重新加载 fail2ban 五维配置（手动种子刷新）。
 * Web 推送全量配置时走 security.blacklist.reload 的 JSON 顶级字段，
 * 本命令仅用于手动触发 YAML 种子重读场景（OAM L2）。 */
static int cmd_cfg_reload(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)argc; (void)argv;
    struct nsbc_ctx *ctx = (struct nsbc_ctx *)ud;
    if (!ctx || !ctx->cfg) {
        c->io_printf(c->io_ud, "ERR no cfg ctx\n");
        return NSBC_ERR;
    }
    long v;
    const char *s = NULL;

    cfg_get_bool(ctx->cfg, "security.auto_ban", &g_cfg.auto_ban, NULL);
    cfg_get_bool(ctx->cfg, "security.block_on_register", &g_cfg.block_on_register, NULL);
    cfg_get_bool(ctx->cfg, "security.block_on_invite", &g_cfg.block_on_invite, NULL);

    if (cfg_get_int(ctx->cfg, "security.authfail_threshold", &v, NULL) == NSBC_OK
            && v >= 1 && v <= 1000) g_cfg.authfail_threshold = (int)v;
    if (cfg_get_int(ctx->cfg, "security.authfail_window_s", &v, NULL) == NSBC_OK
            && v >= 1 && v <= 86400) g_cfg.authfail_window_s = (int)v;
    if (cfg_get_int(ctx->cfg, "security.ban_duration_s", &v, NULL) == NSBC_OK
            && v >= 1 && v <= 604800) g_cfg.ban_duration_s = (int)v;
    if (cfg_get_int(ctx->cfg, "security.ratelimit_regs", &v, NULL) == NSBC_OK
            && v >= 1 && v <= 1000) g_cfg.ratelimit_regs = (int)v;
    if (cfg_get_int(ctx->cfg, "security.ratelimit_window_s", &v, NULL) == NSBC_OK
            && v >= 1 && v <= 3600) g_cfg.ratelimit_window_s = (int)v;

    if (cfg_get_str(ctx->cfg, "security.blacklist_cidrs", &s, NULL) == NSBC_OK && s && s[0])
        snprintf(g_cfg.blacklist_cidrs, sizeof(g_cfg.blacklist_cidrs), "%s", s);
    if (cfg_get_str(ctx->cfg, "security.ua_blacklist", &s, NULL) == NSBC_OK && s && s[0])
        snprintf(g_cfg.ua_blacklist, sizeof(g_cfg.ua_blacklist), "%s", s);

    c->io_printf(c->io_ud,
                 "OK security.cfg.reload auto_ban=%d thr=%d win=%ds ban=%ds "
                 "rl_regs=%d rl_win=%ds\n",
                 g_cfg.auto_ban, g_cfg.authfail_threshold, g_cfg.authfail_window_s,
                 g_cfg.ban_duration_s, g_cfg.ratelimit_regs, g_cfg.ratelimit_window_s);
    log_info("security", "cfg.reload: auto_ban=%d thr=%d win=%ds ban=%ds rl_regs=%d rl_win=%ds",
             g_cfg.auto_ban, g_cfg.authfail_threshold, g_cfg.authfail_window_s,
             g_cfg.ban_duration_s, g_cfg.ratelimit_regs, g_cfg.ratelimit_window_s);
    return NSBC_OK;
}

/* === init / fini === */
static int security_init(struct nsbc_ctx *ctx)
{
    /* 默认值全量内置（零配置可用，分发场景不依赖 YAML）：
     * 静态黑名单默认启用但列表为空 → 无影响；
     * 动态封禁默认启用：60s 窗口内 5 次认证失败 → 封 1 小时。 */
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.block_on_register = 1;
    g_cfg.block_on_invite = 1;
    g_cfg.auto_ban = 1;
    g_cfg.authfail_threshold = 5;
    g_cfg.authfail_window_s = 60;
    g_cfg.ban_duration_s = 3600;
    g_cfg.ratelimit_regs = 20;
    g_cfg.ratelimit_window_s = 60;
    g_cfg.ratelimit_invites = 30;   /* 被动：INVITE/REGISTER txn slab 分配失败阈值 */
    g_cfg.ratelimit_invites_active = 20;  /* 主动：per-IP 每秒新 INVITE 阈值 */
    g_cfg.fence_mode = 1;          /* NIP：默认 security 模式（完整围栏） */
    g_cfg.method_blocked = 0;      /* 默认不禁任何 SIP method */
    __atomic_store_n(&g_fence_mode, 1, __ATOMIC_RELAXED);    /* atomic 同步内存态 */
    __atomic_store_n(&g_method_blocked, 0, __ATOMIC_RELAXED);

    if (ctx && ctx->cfg) {
        long v;
        const char *s = NULL;
        cfg_get_bool(ctx->cfg, "security.block_on_register",
                     &g_cfg.block_on_register, NULL);
        cfg_get_bool(ctx->cfg, "security.block_on_invite",
                     &g_cfg.block_on_invite, NULL);
        cfg_get_bool(ctx->cfg, "security.auto_ban", &g_cfg.auto_ban, NULL);
        if (cfg_get_str(ctx->cfg, "security.blacklist_cidrs", &s, NULL)
                == NSBC_OK && s && s[0]) {
            snprintf(g_cfg.blacklist_cidrs, sizeof(g_cfg.blacklist_cidrs),
                     "%s", s);
        }
        if (cfg_get_str(ctx->cfg, "security.ua_blacklist", &s, NULL)
                == NSBC_OK && s && s[0]) {
            snprintf(g_cfg.ua_blacklist, sizeof(g_cfg.ua_blacklist), "%s", s);
        }
        if (cfg_get_int(ctx->cfg, "security.authfail_threshold", &v,
                        NULL) == NSBC_OK && v >= 1 && v <= 1000)
            g_cfg.authfail_threshold = (int)v;
        if (cfg_get_int(ctx->cfg, "security.authfail_window_s", &v,
                        NULL) == NSBC_OK && v >= 1 && v <= 86400)
            g_cfg.authfail_window_s = (int)v;
        if (cfg_get_int(ctx->cfg, "security.ban_duration_s", &v,
                        NULL) == NSBC_OK && v >= 1 && v <= 604800)
            g_cfg.ban_duration_s = (int)v;
        if (cfg_get_int(ctx->cfg, "security.ratelimit_regs", &v,
                        NULL) == NSBC_OK && v >= 1 && v <= 1000)
            g_cfg.ratelimit_regs = (int)v;
        if (cfg_get_int(ctx->cfg, "security.ratelimit_window_s", &v,
                        NULL) == NSBC_OK && v >= 1 && v <= 3600)
            g_cfg.ratelimit_window_s = (int)v;
        if (cfg_get_int(ctx->cfg, "security.ratelimit_invites", &v,
                        NULL) == NSBC_OK && v >= 0 && v <= 1000)
            g_cfg.ratelimit_invites = (int)v;
        if (cfg_get_int(ctx->cfg, "security.ratelimit_invites_active", &v,
                        NULL) == NSBC_OK && v >= 0 && v <= 1000)
            g_cfg.ratelimit_invites_active = (int)v;
    }

    /* 静态黑名单 YAML 种子（Web 推送 security.blacklist.reload 到达前生效；
     * 推送后以 web.db 为准——文档 §6：数据库为准，YAML 仅启动加载/fallback） */
    bl_seed_from_cfg();

    int rc = b2b_register_hook_add(sec_reg_cb, &g_cfg);
    log_info("security", "init: b2b_register_hook_add rc=%d (hook=%s)",
             rc, rc == NSBC_OK ? "ok" : "full/dup");
    int grc = b2b_guard_hook_add(sec_guard_cb, NULL);
    log_info("security", "init: b2b_guard_hook_add rc=%d", grc);

    /* OAM 命令（Web 封禁管理 + 静态黑名单同步 + 配置 reload）——CCMD 统一注册表
     * （media_bug 同模式）。list=L1 监控可读；unban/clear/reload=L2 管理员。 */
    cmd_reg_t *creg = obs_control_get_cmd_reg();
    if (creg) {
        const cmd_desc_t descs[] = {
            { "security.ban.list",  "list ban/watch entries [ip-substring]",
              CMD_L1_MONITOR, cmd_ban_list,  NULL },
            { "security.ban.unban", "unban ip (clear entry)",
              CMD_L2_ADMIN,   cmd_ban_unban, NULL },
            { "security.ban.clear", "clear whole ban table",
              CMD_L2_ADMIN,   cmd_ban_clear, NULL },
            { "security.blacklist.list",   "list static blacklist (cidr+ua)",
              CMD_L1_MONITOR, cmd_bl_list,   NULL },
            { "security.blacklist.reload", "reload static blacklist from JSON (Web push)",
              CMD_L2_ADMIN,   cmd_bl_reload, NULL },
            { "security.cfg.reload",       "reload fail2ban config from YAML",
              CMD_L2_ADMIN,   cmd_cfg_reload, ctx },
        };
        for (size_t i = 0; i < sizeof(descs) / sizeof(descs[0]); ++i)
            log_info("security", "cmd_register('%s') rc=%d", descs[i].name,
                     cmd_register(creg, &descs[i]));
    } else {
        log_warn("security", "cmd registry null — ban mgmt cmds unavailable");
    }

    log_info("security",
             "auto-ban %s (threshold=%d window=%ds duration=%ds) "
             "blacklist='%s' ua_blacklist='%s'",
             g_cfg.auto_ban ? "on" : "off", g_cfg.authfail_threshold,
             g_cfg.authfail_window_s, g_cfg.ban_duration_s,
             g_cfg.blacklist_cidrs[0] ? g_cfg.blacklist_cidrs : "-",
             g_cfg.ua_blacklist[0] ? g_cfg.ua_blacklist : "-");
    return (rc == NSBC_OK && grc == NSBC_OK) ? 0 : -1;
}

static void security_fini(struct nsbc_ctx *ctx)
{
    (void)ctx;
    b2b_register_hook_rm(sec_reg_cb, &g_cfg);
    b2b_guard_hook_rm(sec_guard_cb, NULL);
    cmd_reg_t *creg = obs_control_get_cmd_reg();
    if (creg) {
        cmd_unregister(creg, "security.ban.list");
        cmd_unregister(creg, "security.ban.unban");
        cmd_unregister(creg, "security.ban.clear");
        cmd_unregister(creg, "security.blacklist.list");
        cmd_unregister(creg, "security.blacklist.reload");
        cmd_unregister(creg, "security.cfg.reload");
    }
    ban_clear_all();
    pthread_rwlock_wrlock(&g_bl_rwlock);
    g_deny_cidr_n = 0;
    g_deny_ua_n = 0;
    g_allow_cidr_n = 0;
    g_allow_ua_n = 0;
    pthread_rwlock_unlock(&g_bl_rwlock);
    log_info("security", "fini: hooks removed, ban table cleared");
}

/* Phase 2 模块描述符：MOD_L_FINAL=4（白皮书建议 MOD_L_LOGIC 语义别名，值用 4）
 * MOD_CAT_SECURITY 已在 module.h 枚举里。
 * NSBC_MOD0_CAT：零模块依赖变体（b2bua/registrar 是核心代码非模块，
 * 依赖写 "b2bua" 会让 configure 的依赖校验报 not enabled）。 */
/* deps: oam(必须先 init 创建 cmd_reg), signaling(依赖 b2bua hooks) */
static const char *security_deps[] = {"oam", "signaling", NULL};
NSBC_MOD_CAT(security, 1, MOD_L_FINAL, MOD_F_REQUIRED, MOD_CAT_SECURITY,
             security_deps, security_init, NULL, NULL, security_fini);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
