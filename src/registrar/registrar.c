#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "registrar.h"
#include "b2bua.h"      /* b2b_guard_notify — 认证失败上报 security 封禁 */
#include "ha.h"         /* ha_role() — STANDBY 跳过 reg_push（loc_db 照常写，DB 事件不双写）
                         * HA P1: ha_repl_publish() — 主→备状态复制接线点 */
#include "metric_reg.h"
#include "sip_build.h"
#include "sip_profile.h"
#include "nsbc_id.h"    /* nsbc_rand_token — 401/403 响应自动生成 To tag */
#include "sip_txn.h"
#include "security.h"   /* security_check_fence — REGISTER 也走全局围栏（deny + allow + method 禁用） */
#include "uDS_client.h"
#include "log.h"
#include "net_sock.h"
#include <time.h>       /* clock_gettime(CLOCK_MONOTONIC) — perf 计时 */

/* signaling.c：从 AF_UNSPEC nsbc_sa_t padding 提取 ws_conn*
 * （WS/WSS REGISTER 的 src 藏有 conn 指针，见 sig_ws_bridge_rx） */
extern void *sig_ws_bridge_conn_from_sa(const nsbc_sa_t *sa);

#include <cjson/cJSON.h>
#include <arpa/inet.h>
#include <stdatomic.h>   /* C11 _Atomic + atomic_load_explicit + memory_order_* */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TB_IP_N     4096
#define TB_AOR_N    4096

/* 从 sip_auth_t.params[] 按 name 找 value，拷贝到 out（out_sz 含 NUL）。
 * 返回值：找到返回 1，未找到返回 0。out 首字节设为 '\0'。 */
static int auth_param_get(const sip_auth_t *a, const char *name,
                          char *out, size_t out_sz)
{
    out[0] = '\0';
    if (!a || !name) return 0;
    size_t nlen = strlen(name);
    for (int i = 0; i < a->nparams; ++i) {
        const sip_param_t *p = &a->params[i];
        if (p->name.p && p->name.len == nlen &&
            strncasecmp(p->name.p, name, nlen) == 0) {
            if (p->val.p && p->val.len > 0) {
                size_t cp = p->val.len < out_sz - 1 ? p->val.len : out_sz - 1;
                memcpy(out, p->val.p, cp);
                out[cp] = '\0';
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

typedef struct {
    pthread_mutex_t lock;
    double tokens;
    uint64_t last_ms;
    uint32_t key;
} tbucket_t;

/* 必须 16 字节对齐 —— cmpxchg16b 要求目标地址 16 字节对齐。
 * 定义必须在 struct reg_core 之前——_Atomic(token_state_t) 需要已知类型。 */
typedef struct {
    uint64_t last_ms;
    double   tokens;
} token_state_t __attribute__((aligned(16)));

struct reg_core {
    reg_cfg_t cfg;
    digest_cfg_t dig;
    tbucket_t ip[TB_IP_N];
    tbucket_t aor[TB_AOR_N];
    /* 全局令牌桶 —— lock-free：把 (时间戳, token) 打包成 16 字节 struct，
     * 用单次 128-bit CAS (cmpxchg16b on x86_64) 同时更新两个字段。
     * 单次 CAS 保证两个字段同步更新——要么全新，要么全旧，
     * 中间态不存在，双 CAS 竞态完全消除。 */
    _Atomic(token_state_t) state;
    uint64_t n_total, n_rej, n_thr;
};

static int tb_take(tbucket_t *b, uint32_t key, int cps, uint64_t now)
{
    /* cps <= 0 = 不限流（开源默认：REGISTER 消耗极少资源，不对其做并发限制） */
    if (cps <= 0)
        return 1;
    pthread_mutex_lock(&b->lock);
    if (b->key != key) {
        b->key = key;
        b->tokens = (double)cps;
        b->last_ms = now;
    } else {
        double add = (double)(now - b->last_ms) * cps / 1000.0;
        b->tokens += add;
        if (b->tokens > cps)
            b->tokens = cps;
        b->last_ms = now;
    }
    int ok = b->tokens >= 1.0;
    if (ok)
        b->tokens -= 1.0;
    pthread_mutex_unlock(&b->lock);
    return ok;
}

static uint32_t sa_key(const nsbc_sa_t *sa)
{
    const struct sockaddr_in *s4 = (const struct sockaddr_in *)&sa->ss;
    if (s4->sin_family == AF_INET)
        return ntohl(s4->sin_addr.s_addr);
    const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)&sa->ss;
    uint32_t h = 0;
    const uint8_t *p = s6->sin6_addr.s6_addr;
    for (int i = 0; i < 16; ++i)
        h = h * 31u + p[i];
    return h;
}

static uint32_t str_key(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static int global_take(reg_core_t *r, uint64_t now)
{
    /* global_cps <= 0 = 不限流（开源默认） */
    if (r->cfg.global_cps <= 0)
        return 1;
    /* Lock-free 全局令牌桶 —— 单次 128-bit CAS
     *
     * 把 (last_ms, tokens) 打包成 16 字节 token_state_t，
     * 用 atomic_compare_exchange 对整个 struct 做单次 CAS。
     *
     * 为什么要打包？双 CAS 不是原子的——线程 A 刷了时间戳但还没扣 token，
     * 线程 B 看到时间戳"已是最新"就跳过 token 补充，但 token 还是旧值 0
     * → 所有并发 REGISTER 被拒。这就是当前 bug！
     *
     * 单次 CAS 保证：要么两个字段同时看到新值，要么同时看到旧值——
     * 中间态不存在，竞态完全消除。
     *
     * C11 _Atomic struct 在 GCC x86_64 上编译成 cmpxchg16b。
     * token_state_t 16 字节 + aligned(16) → 满足 cmpxchg16b 对齐要求。 */
    const double cap = (double)r->cfg.global_cps;

    for (;;) {
        /* 1. 整体 load（要么全新要么全旧，中间态不可能） */
        token_state_t cur = atomic_load_explicit(&r->state, memory_order_acquire);

        /* 2. 计算新 token（基于 cur.last_ms） */
        token_state_t next = cur;
        next.last_ms = now;
        if ((int64_t)(now - cur.last_ms) > 0) {
            double add = (double)(now - cur.last_ms) * r->cfg.global_cps / 1000.0;
            next.tokens = cur.tokens + add;
            if (next.tokens > cap) next.tokens = cap;
        }

        /* 3. 检查够不够 */
        if (next.tokens < 1.0)
            return 0;  /* 限流（确实没 token，不是竞态） */

        /* 4. 扣 1 token */
        next.tokens -= 1.0;

        /* 5. 单次 CAS 原子更新两个字段
         * expected=cur, desired=next —— 如果别人在我们计算期间改了状态，
         * CAS 失败 → cur 被更新成新值 → 重试 */
        if (atomic_compare_exchange_weak_explicit(
                &r->state, &cur, next,
                memory_order_acq_rel, memory_order_acquire))
            return 1;  /* 成功 */
        /* else: cur 被自动更新成最新值，continue 重试 */
    }
}

/* 在 Contact 头原始值里取 ;expires= / +sip.instance= / reg-id= */
static long param_num(const char *s, size_t len, const char *name)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl < len; ++i) {
        if (s[i] != ';')
            continue;
        size_t k = i + 1;
        while (k < len && (s[k] == ' ' || s[k] == '\t'))
            ++k;
        if (k + nl < len && !strncasecmp(s + k, name, nl) &&
            s[k + nl] == '=') {
            char tmp[16];
            size_t j = 0, q = k + nl + 1;
            if (q < len && s[q] == '"')
                ++q;
            while (q < len && s[q] >= '0' && s[q] <= '9' &&
                   j < sizeof(tmp) - 1)
                tmp[j++] = s[q++];
            if (j) {
                tmp[j] = '\0';
                return atol(tmp);
            }
            return -1;
        }
    }
    return -1;
}

static void param_str(const char *s, size_t len, const char *name,
                      char *out, size_t outsz)
{
    out[0] = '\0';
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl < len; ++i) {
        if (s[i] != ';')
            continue;
        size_t k = i + 1;
        while (k < len && (s[k] == ' ' || s[k] == '\t'))
            ++k;
        if (k + nl < len && !strncasecmp(s + k, name, nl) &&
            s[k + nl] == '=') {
            size_t q = k + nl + 1, j = 0;
            int quoted = q < len && s[q] == '"';
            if (quoted)
                ++q;
            while (q < len && j < outsz - 1) {
                if (quoted && s[q] == '"')
                    break;
                if (!quoted && s[q] == ';')
                    break;
                out[j++] = s[q++];
            }
            out[j] = '\0';
            return;
        }
    }
}

/* Contact 存储文本：头值原文去首尾空白（含尖括号与参数原样保留） */
static void copy_addr(char *dst, size_t cap, const sip_hdr_t *h)
{
    const char *p = h->value.p;
    size_t n = h->value.len;
    while (n && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        ++p, --n;
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' ||
                 p[n - 1] == '\r' || p[n - 1] == '\n'))
        --n;
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

/* 从 aor 里提取 user 部分（@ 前面的短号/分机号）—— grep 'user=1006'
 * 就能把同一个分机的注册链 + 通话链（b2bua new_invite/cdr）全部聚合。
 * 典型：aor="1006@nsbc.local" → out="1006"；aor="1006" → out="1006"。 */
static inline void _aor_user(const char *aor, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!aor || !aor[0]) return;
    const char *at = strchr(aor, '@');
    size_t ulen = at ? (size_t)(at - aor) : strlen(aor);
    if (ulen >= out_sz) ulen = out_sz - 1;
    memcpy(out, aor, ulen);
    out[ulen] = '\0';
}

/* === 统一 dup 判定（与 loc_upsert 对齐）===
 * 判断新 REGISTER 的 binding b 是否与现有 probe[i] 是"同一终端刷新"
 * 而不是新设备接入——是 dup 就不该 evict 老的，让 loc_upsert 自然覆盖。
 *
 * 四条判定（按可靠性从高到低）：
 *   ① Contact 字符串完全相同（UA 用同一端口刷新的常规路径）
 *   ② +sip.instance 相同（RFC 5627 终端唯一标识——WS 重连后 contact/callid 全变，
 *      但 SIP.js/FreeSWITCH 等成熟 UA 都会在 Contact 里带 +sip.instance="<urn:uuid:...>"，
 *      这个 UUID 跨 WS 重建稳定不变，是"同一浏览器/软电话"的最准指纹）
 *   ③ Call-ID 相同（同一 Call-ID 的 REGISTER 刷新）
 *   ④ instance 为空时（老 UA 不带 +sip.instance）用 received_ip + transport 兜底
 *      —— 同一 IP + 同一 transport（WS/WSS/UDP）大概率同一终端断线重连
 *
 * ★ 为什么这条放在 loc_upsert 之前？
 *   因为 max_registrations_per_ext evict 逻辑在 L1191 先于 loc_upsert 执行，
 *   如果 dup 判定不覆盖 instance，WS 重连后的"新 contact + 新 callid"会被当成新设备
 *   → 触发 evict 踢掉老的，然后 loc_upsert 才发现 instance 相同但已经晚了。
 *   两层 dup 判定必须对齐。 */
static inline int is_dup_binding(const loc_binding_t *probe,
                                 const loc_binding_t *b)
{
    /* ① Contact 相同：最经典 dup */
    if (strcmp(probe->contact, b->contact) == 0) {
        char _u[64]; _aor_user(probe->aor, _u, sizeof(_u));
        llog(LOG_DEBUG, "reg",
             "DUP-HIT[①contact] user=%s aor=%s contact=%.80s",
             _u, probe->aor, probe->contact);
        return 1;
    }
    /* ② instance 相同：WS 重连最稳的 dup 指纹 */
    if (b->instance[0] && probe->instance[0] &&
        strcmp(probe->instance, b->instance) == 0) {
        char _u[64]; _aor_user(probe->aor, _u, sizeof(_u));
        llog(LOG_DEBUG, "reg",
             "DUP-HIT[②instance] user=%s aor=%s instance=%.60s",
             _u, probe->aor, b->instance);
        return 1;
    }
    /* ③ Call-ID 相同：同 Call-ID 的 REGISTER 刷新 */
    if (b->callid[0] && strcmp(probe->callid, b->callid) == 0) {
        char _u[64]; _aor_user(probe->aor, _u, sizeof(_u));
        llog(LOG_DEBUG, "reg",
             "DUP-HIT[③callid] user=%s aor=%s callid=%.60s",
             _u, probe->aor, b->callid);
        return 1;
    }
    /* ④ instance 为空时的软判定：同 IP + 同 transport → 大概率断线重连 */
    if (!b->instance[0] && probe->received_ip[0] && b->received_ip[0] &&
        strcmp(probe->received_ip, b->received_ip) == 0 &&
        probe->transport_proto == b->transport_proto) {
        char _u[64]; _aor_user(probe->aor, _u, sizeof(_u));
        llog(LOG_DEBUG, "reg",
             "DUP-HIT[④ip+transport] user=%s aor=%s ip=%s proto=%d (no instance, soft match)",
             _u, probe->aor, b->received_ip, b->transport_proto);
        return 1;
    }
    return 0;
}

/* === NAT 检测辅助（REQ-NAT-001）===
 * RFC1918 + CGNAT + 回环 + Link-Local 私网判断。
 * IPv4 only（SIP 99% 场景），IPv6 私网暂不处理（nsbc 当前 IPv6 支持有限）。 */
static int is_private_ip(const char *ip)
{
    if (!ip || !ip[0]) return 0;
    struct in_addr a;
    if (inet_pton(AF_INET, ip, &a) != 1) return 0; /* IPv6/FQDN 直接放行 */
    uint32_t h = ntohl(a.s_addr);

    /* 10.0.0.0/8 */
    if ((h & 0xFF000000u) == 0x0A000000u) return 1;
    /* 172.16.0.0/12 */
    if ((h & 0xFFF00000u) == 0xAC100000u) return 1;
    /* 192.168.0.0/16 */
    if ((h & 0xFFFF0000u) == 0xC0A80000u) return 1;
    /* 100.64.0.0/10 CGNAT */
    if ((h & 0xFFC00000u) == 0x64400000u) return 1;
    /* 127.0.0.0/8 回环 */
    if ((h & 0xFF000000u) == 0x7F000000u) return 1;
    /* 169.254.0.0/16 Link-Local */
    if ((h & 0xFFFF0000u) == 0xA9FE0000u) return 1;

    return 0;
}

/* 从 nsbc_sa_t 提取 IP:port 字符串（udp/tcp/tls） */
static void sa_to_str(const nsbc_sa_t *sa, char *ip, size_t ip_sz,
                      uint16_t *port_out)
{
    if (!sa) { ip[0] = '\0'; if (port_out) *port_out = 0; return; }
    if (sa->ss.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&sa->ss;
        inet_ntop(AF_INET, &sin->sin_addr, ip, ip_sz);
        if (port_out) *port_out = ntohs(sin->sin_port);
    } else if (sa->ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&sa->ss;
        inet_ntop(AF_INET6, &sin6->sin6_addr, ip, ip_sz);
        if (port_out) *port_out = ntohs(sin6->sin6_port);
    } else {
        ip[0] = '\0';
        if (port_out) *port_out = 0;
    }
}

/* 解析 Contact 的 host/port（sip_addr_t 深解析后取 uri.host + uri.port） */
static void contact_parts(const sip_hdr_t *contact_h,
                          char *user, size_t user_sz,
                          char *host, size_t host_sz,
                          uint16_t *port_out)
{
    user[0] = '\0'; host[0] = '\0';
    if (port_out) *port_out = 0;
    if (!contact_h || sip_hdr_parse_addr((sip_hdr_t *)contact_h) != NSBC_OK)
        return;
    const sip_addr_t *a = &contact_h->u.addr;
    if (a->uri.user.p && a->uri.user.len > 0) {
        size_t n = a->uri.user.len < user_sz - 1 ? a->uri.user.len : user_sz - 1;
        memcpy(user, a->uri.user.p, n); user[n] = '\0';
    }
    if (a->uri.host.p && a->uri.host.len > 0) {
        size_t n = a->uri.host.len < host_sz - 1 ? a->uri.host.len : host_sz - 1;
        memcpy(host, a->uri.host.p, n); host[n] = '\0';
    }
    if (port_out && a->uri.port.p && a->uri.port.len > 0) {
        char tmp[16]; size_t n = a->uri.port.len < sizeof(tmp) - 1 ? a->uri.port.len : sizeof(tmp) - 1;
        memcpy(tmp, a->uri.port.p, n); tmp[n] = '\0';
        *port_out = (uint16_t)atoi(tmp);
    }
}

/* NAT 检测（REQ-NAT-001）：Contact host 私网 或 Via received 不同 或 src IP 不同 → NAT */
/* 两个 IP 同属一个 RFC1918/CGNAT 网段（10/8、172.16/12、192.168/16、100.64/10）
 * → 视为企业内网直连，不是 NAT（即使 IP 不同主机也直接可达） */
static int same_private_net(const char *a, const char *b)
{
    if (!a || !b || !is_private_ip(a) || !is_private_ip(b)) return 0;
    struct in_addr aa, bb;
    if (inet_pton(AF_INET, a, &aa) != 1 || inet_pton(AF_INET, b, &bb) != 1) return 0;
    uint32_t ha = ntohl(aa.s_addr), hb = ntohl(bb.s_addr);
    /* 同属 10/8 */
    if ((ha & 0xFF000000u) == 0x0A000000u && (hb & 0xFF000000u) == 0x0A000000u) return 1;
    /* 同属 172.16/12 */
    if ((ha & 0xFFF00000u) == 0xAC100000u && (hb & 0xFFF00000u) == 0xAC100000u) return 1;
    /* 同属 192.168/16 */
    if ((ha & 0xFFFF0000u) == 0xC0A80000u && (hb & 0xFFFF0000u) == 0xC0A80000u) return 1;
    /* 同属 100.64/10 CGNAT */
    if ((ha & 0xFFC00000u) == 0x64400000u && (hb & 0xFFC00000u) == 0x64400000u) return 1;
    return 0;
}

/* NAT 检测（REQ-NAT-001） */
static int nat_detect(const char *contact_host,
                      const char *via_received, uint16_t via_rport,
                      const char *src_ip, uint16_t src_port)
{
    if (!contact_host || !contact_host[0]) return 0;

    /* SIP.js WebRTC 占位符域名 → 跳过（WS/WSS 靠连接本身可达，Contact 里的 .invalid 只是占位） */
    if (strstr(contact_host, ".invalid") || strstr(contact_host, "invalid."))
        return 0;

    /* Contact 和真实 src_ip 完全相同 → 无 NAT */
    if (src_ip[0] && strcmp(src_ip, contact_host) == 0)
        return 0;

    /* 两个都是 RFC1918/CGNAT 私网 IP 且同属一个大网段（如都是 10.x）
     * → 企业内网直连，不是 NAT（即使 IP 不同主机也直接可达） */
    if (src_ip[0] && same_private_net(src_ip, contact_host))
        return 0;

    /* Via received 存在且和 Contact 不同 → NAT 改写了 Via（运营商 NAT 补了 received） */
    if (via_received[0] && strcmp(via_received, contact_host) != 0)
        return 1;

    /* src_ip 和 Contact 不同 → 宣告 ≠ 真实，有 NAT
     * （前面已过滤同私网直连场景，走到这里一定是一个私网一个公网） */
    if (src_ip[0] && strcmp(src_ip, contact_host) != 0)
        return 1;

    /* Via rport 和 src_port 不同 → NAT 改了端口 */
    if (via_rport && src_port && via_rport != src_port)
        return 1;

    return 0;
}

/* 根据 SIP_TR_* 返回 transport 字符串 */
static const char *proto_str(int proto)
{
    switch (proto) {
        case SIP_TR_UDP: return "udp";
        case SIP_TR_TCP: return "tcp";
        case SIP_TR_TLS: return "tls";
        case SIP_TR_WS:  return "ws";
        case SIP_TR_WSS: return "wss";
        default:         return "";
    }
}

/* OBS 自埋点（REQ-OBS-001）：注册计数器 render，ud=reg_core；
 * 2026-09-27 A3 自 oam/obs_metrics.c 原样迁入，指标名保持不变。 */
static int r_reg(char *b, size_t cap, void *ud)
{
    reg_core_t *r = (reg_core_t *)ud;
    loc_db_t *db = r ? r->cfg.db : NULL;
    return snprintf(b, cap,
                    "nsbc_registrations %llu\n"
                    "nsbc_reg_requests_total %llu\n"
                    "nsbc_reg_rejected_total %llu\n"
                    "nsbc_reg_throttled_total %llu\n",
                    (unsigned long long)(db ? loc_db_count(db) : 0),
                    (unsigned long long)(r ? reg_total(r) : 0),
                    (unsigned long long)(r ? reg_rejected(r) : 0),
                    (unsigned long long)(r ? reg_throttled(r) : 0));
}

/* === HA P1: STANDBY REPL_REG apply 回调 ===
 * transport recv 线程调 — proxy 分发到各订阅者。
 * 处理：type=REPL_REG + payload 是 loc_binding_t（ACTIVE publish 时 memcpy 整包）
 *       expire_ms > 0 → loc_upsert（内存 upsert）
 *       expire_ms == 0 → loc_remove（注销）
 * 只重建 loc_db 内存 —— 绝对不调 reg_push（防双写 DB）
 * 返回 NSBC_OK = 接受（ingest 推进 applied_seq）；非 OK → 计入 warn 但不跳序 */
static int reg_repl_apply(void *ud, const repl_rec_t *rec)
{
    reg_core_t *r = ud;
    if (!r || !rec) return NSBC_INVAL;
    if (rec->type != REPL_REG)
        return NSBC_OK;   /* 不是 REPL_REG，跳过（b2bua 负责 REPL_DIALOG） */
    if (rec->len < sizeof(loc_binding_t))
        return NSBC_PROTO;   /* payload 格式不匹配 */
    const loc_binding_t *b = (const loc_binding_t *)rec->payload;
    if (b->aor[0] == '\0') return NSBC_PROTO;
    uint64_t now = nsbc_now_ms();
    if (b->expire_ms == 0 || b->expire_ms <= now) {
        /* 注销 — expire_ms=0 表示主动 remove；<=now 表示过期 */
        loc_remove(r->cfg.db, b->aor, b->contact[0] ? b->contact : NULL);
    } else {
        /* 注册/刷新 — 只写内存，不调 reg_push（防双写） */
        (void)loc_upsert(r->cfg.db, b, now);
    }
    return NSBC_OK;
}

int reg_core_create(const reg_cfg_t *cfg, reg_core_t **out)
{
    reg_core_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NSBC_NOMEM;
    r->cfg = *cfg;
    if (r->cfg.min_expires <= 0)
        r->cfg.min_expires = 60;
    if (r->cfg.max_expires <= 0)
        r->cfg.max_expires = 600;
    if (r->cfg.default_expires <= 0)
        r->cfg.default_expires = 600;
    if (r->cfg.multiple_registrations <= 0)
        r->cfg.multiple_registrations = 1;   /* 默认允许多 Contact 同 AoR（FS 默认 true） */
    /* force_expires / expires_jitter_s / max_registrations_per_ext 默认 0 = 关闭 */
    /* 默认 0 = 不限流（开源默认：REGISTER 消耗极少资源，
     * 核心并发限制只在 INVITE 侧（sip.max_concurrent_calls）做）。
     * 生产环境如需防注册风暴，可在 cfg 里显式设 registrar.rps_per_ip / rps_per_aor / rps_global。 */
    if (r->cfg.ip_cps < 0)  r->cfg.ip_cps = 0;      /* 0 = 不限 */
    if (r->cfg.aor_cps < 0) r->cfg.aor_cps = 0;     /* 0 = 不限 */
    if (r->cfg.global_cps < 0) r->cfg.global_cps = 0; /* 0 = 不限 */
    if (r->cfg.retry_after_s <= 0)
        r->cfg.retry_after_s = 30;
    if (digest_init(&r->dig, cfg->realm, cfg->digest_secret,
                cfg->nonce_ttl_ms ? cfg->nonce_ttl_ms : 300000,
                cfg->ha1_cb, cfg->ha1_ud) != NSBC_OK) {
        free(r);
        return NSBC_NOMEM;
    }
    /* state 是 _Atomic(token_state_t) —— calloc 已清零
     * last_ms=0, tokens=0，和旧 mutex 版本行为一致 */
    for (int i = 0; i < TB_IP_N; ++i)
        pthread_mutex_init(&r->ip[i].lock, NULL);
    for (int i = 0; i < TB_AOR_N; ++i)
        pthread_mutex_init(&r->aor[i].lock, NULL);
    metric_source_register("nsbc_reg", "registration counters", r_reg, r);

    /* === HA P1: 注册 REPL_REG apply 回调（STANDBY 侧内存重建）===
     * 只重建 loc_db 内存 —— **绝对不调 reg_push**（防双写 DB）
     * role 守卫：apply 回调里不检查 role（proxy 分发前 transport 已 role 仲裁）
     * subscribe 时序：reg_core_create 先于 ha_repl_transport 线程启动，无 race */
    ha_repl_subscribe_apply(reg_repl_apply, r);

    *out = r;
    return NSBC_OK;
}

void reg_core_destroy(reg_core_t *r)
{
    if (!r)
        return;
    digest_free(&r->dig);
    for (int i = 0; i < TB_IP_N; ++i)
        pthread_mutex_destroy(&r->ip[i].lock);
    for (int i = 0; i < TB_AOR_N; ++i)
        pthread_mutex_destroy(&r->aor[i].lock);
    /* glock 已移除 —— atomic 变量不需要 destroy */
    free(r);
}

static void respond(reg_core_t *r, sip_tr_proto_t proto, const nsbc_sa_t *dst,
                    const sip_msg_t *req, int status, const char *extra,
                    const char *body, size_t bodylen, sip_txn_t *txn)
{
    /* RFC 3261 §8.2.6.2：REGISTER 2xx OK 响应 To 头可以不带 tag，
     * 但所有其他响应（401/403/404/480/503 等）**必须带** tag ——
     * 否则 MicroSIP/Yealink 等客户端不认这是对它 REGISTER 的有效响应，
     * 直接丢弃，或者对于 401 不认为是 challenge → 不响应 digest。 */
    char auto_to_tag[24] = {0};
    const char *to_tag = NULL;
    if (status >= 200 && status < 300) {
        /* 2xx OK —— 可选 tag（B2BUA 场景加，普通 UAS 可以不加） */
    } else {
        /* 非 2xx —— 必须加 tag，防止客户端丢弃 */
        nsbc_rand_token(auto_to_tag, sizeof(auto_to_tag), 10);
        to_tag = auto_to_tag;
    }

    char out[SIP_MAX_MSG];
    int n = sip_build_response(out, sizeof(out), req, status, NULL, to_tag,
                               NULL, extra, body, bodylen);
    if (n <= 0) return;
    /* NAT: RFC 3261 §18.2.2 — Via 响应补 received=src_ip;rport=src_port
     * 让 NAT conntrack 更新，保证响应能回到正确的内网地址
     * ★ BUG-FIX: patch 会增长消息（;rport → ;rport=N;received=IP），
     *   必须用 patch 后的 len 发送，否则尾部被截断 → 客户端收到残缺
     *   SIP 消息 → Wireshark 报 malformed → MicroSIP 无法解析 challenge */
    size_t len = (size_t)n;
    if (dst) {
        char src_ip[INET6_ADDRSTRLEN] = "";
        uint16_t src_port = 0;
        sa_to_str(dst, src_ip, sizeof(src_ip), &src_port);
        sip_via_patch_received_rport(out, &len, sizeof(out), src_ip, src_port);
    }
    /* ★ 诊断（2026-11-14 "错误的网关"排查）：完整响应字节 dump。
     * 之前只打了流程节点日志，但客户端真正收到的是什么字节没验证过——
     * 服务端视角"200 OK 已发"不等于"客户端能解析"。此 dump 能直接暴露：
     * 头部畸形 / 截断 / Service-Route 等客户端可能不认的内容。 */
    {
        char _rip[INET6_ADDRSTRLEN] = "-";
        uint16_t _rport = 0;
        if (dst) sa_to_str(dst, _rip, sizeof(_rip), &_rport);
        llog(LOG_DEBUG, "reg",
             "REG-RESP status=%d dst=%s:%u len=%zu",
             status, _rip, (unsigned)_rport, len);
        /* 字节全量 dump 只在 DEBUG 全开时才有意义（诊断服务端发了什么字节）。
         * INFO 级只打印元信息；需要看完整响应体时手动开 DEBUG。 */
    }
    /* 优先走 txn 层：自动缓存响应 + 吸收同 branch 重传 */
    if (txn) {
        txn_ref(txn);  /* ★ 防竞态：protect txn_send_response 期间 txn 不被释放 */
        txn_send_response(txn, out, len, nsbc_now_ms());
        txn_unref(txn);
    } else {
        sip_tr_send(r->cfg.tr, proto, out, len, dst);
    }
}

/* ---- write-through: REGISTER 事件 → data.sock ---- */

/* REGISTER 状态 → JSON → uds_send_register（write-through）。
 * expires_sec=0 表示注销；>0 表示注册成功。
 * 非阻塞：UDS 不可用时静默丢，uds_client 内部 ring 满自动丢最旧。
 * NAT 字段（REQ-NAT-001）：received_ip/received_port/nat_detected/public_contact/transport
 *   由调用方从 loc_binding_t 传入，用于 Web registrations 表落库与前端展示。 */
void reg_push(const loc_binding_t *b, long expires_sec)
{
    if (!b) return;
    /* ★ STANDBY 跳过 DB/事件推送（但 loc_upsert 在之前已完成，内存 loc_db 有数据）
     * 防双机同时 upsert 同一个 UA 到 Rust side registrations 表（幂等但无意义）。
     * failover 后新 ACTIVE 节点的 loc_db 已有注册数据，INVITE 可立即路由。 */
    if (ha_role() != HA_ROLE_ACTIVE)
        return;

    /* 从 AOR 提取 user 部分（去掉 sip: 和 @domain） */
    const char *aor = b->aor;
    const char *user = aor;
    const char *at = strchr(aor, '@');
    char user_buf[128];
    if (at) {
        size_t ulen = (size_t)(at - aor);
        if (ulen >= sizeof(user_buf)) ulen = sizeof(user_buf) - 1;
        memcpy(user_buf, aor, ulen);
        user_buf[ulen] = '\0';
        user = user_buf;
    }

    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddStringToObject(o, "user", user);
    cJSON_AddStringToObject(o, "contact", b->contact);
    if (b->instance[0])
        cJSON_AddStringToObject(o, "instance_id", b->instance);
    cJSON_AddNumberToObject(o, "expires_sec", (double)(expires_sec > 0 ? expires_sec : 0));
    /* === NAT 元数据（REQ-NAT-001）=== */
    cJSON_AddStringToObject(o, "received_ip",   b->received_ip);
    cJSON_AddNumberToObject(o, "received_port",  (double)b->received_port);
    cJSON_AddNumberToObject(o, "nat_detected",  (double)b->nat_detected);
    cJSON_AddStringToObject(o, "public_contact", b->public_contact);
    cJSON_AddStringToObject(o, "transport",      proto_str(b->transport_proto));

    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (json) {
        (void)uds_send_register(json);
        free(json);
    }
}

int reg_on_register(sip_tr_proto_t proto, const nsbc_sa_t *src,
                    sip_msg_t *m, const char *raw, size_t rawlen,
                    sip_txn_t *txn, void *ud)
{
    (void)raw;
    (void)rawlen;
    reg_core_t *r = ud;
    uint64_t now = nsbc_now_ms();
    __sync_fetch_and_add(&r->n_total, 1);

    /* ---- PERF: entry timestamp (CLOCK_MONOTONIC microseconds) ---- */
    struct timespec _t0;
    clock_gettime(CLOCK_MONOTONIC, &_t0);
    int _rc = NSBC_OK;  /* 所有 return 改成 goto reg_out，这里存返回值 */

    /* src IP 字符串（fail2ban 日志用） */
    char src_ip[INET6_ADDRSTRLEN] = "-";
    if (src) {
        if (src->ss.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&src->ss;
            inet_ntop(AF_INET, &sin->sin_addr, src_ip, sizeof(src_ip));
        } else if (src->ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&src->ss;
            inet_ntop(AF_INET6, &sin6->sin6_addr, src_ip, sizeof(src_ip));
        }
    }

    /* P2-XX Profile 归属 + external REGISTER guard：
     * 与 INVITE 入口 (b2bua.c:5692) 保持一致 — 先按入端口匹配 profile，
     * 再 fallback 到源 IP 白名单匹配。之前只用 sip_profile_match_src 按 IP
     * 匹配，当 internal/external 两个 profile 的 whitelist 都包含 127.0.0.1
     * 时，数组遍历第一个命中 external (auth_calls=0) → 所有 REGISTER 被 403，
     * 导致 Python stress / sipp 都无法触发 401 challenge。
     *
     * 端口语义：5060=internal (auth_calls=1, 接受 REGISTER)
     *          5080=external (auth_calls=0, 接受中继 INVITE 但不接受 UA REGISTER)
     *          WS/WSS 入向 txn->local_port 为 0，此时 fallback 到 IP 匹配。 */
    {
        uint16_t in_port = txn_local_port(txn);
        const sip_profile_t *prof = sip_profile_match_port(in_port, 0);
        if (!prof)
            prof = sip_profile_match_src(src_ip, NULL);
        if (prof && prof->auth_calls == 0) {
            llog(LOG_WARN, "reg",
                 "REGISTER 403: external profile '%s' does not accept registrations, src=%s in_port=%u",
                 prof->name, src_ip, in_port);
            respond(r, proto, src, m, 403,
                    "External profile does not accept registrations",
                    NULL, 0, txn);
            _rc = NSBC_OK; goto reg_out;
        }
    }

    /* ---- Security Fence：REGISTER 也走全局围栏（deny + allow + method 禁用）----
     * INVITE/OPTIONS 路径都已经在 b2bua.c 里走 security_check_fence，
     * REGISTER 之前漏掉了 → 拨号规则里配的 allow/deny CIDR、method 禁用对注册无效
     * → 公网扫号器能绕过 allow CIDR 直接撞库 → fail2ban。
     * 放在 profile ACL 之后（profile ACL 是更底层的端口/IP 归属）、任何实质性处理之前。
     * 命中则 silent drop（不回 SIP 响应，防 SBC 指纹暴露，与 INVITE 一致）。
     * force_register_ok=1 调试期整体跳过 fence——auth 都跳了，allowlist 不应该再挡。 */
    if (!(r->cfg.force_register_ok)) {
        char f_ua[128] = {0};
        char f_from[64] = {0};   /* From user part — 注册者分机号 */
        char f_to[64]   = {0};   /* To user part   — 被注册 AoR */
        sip_hdr_t *uah = sip_hdr_get(m, SIP_H_USER_AGENT);
        if (uah && uah->value.len > 0)
            snprintf(f_ua, sizeof(f_ua), "%.*s", (int)uah->value.len, uah->value.p);
        sip_hdr_t *frh = sip_hdr_get(m, SIP_H_FROM);
        if (frh && sip_hdr_parse_addr(frh) == NSBC_OK && frh->u.addr.uri.user.p)
            snprintf(f_from, sizeof(f_from), "%.*s", (int)frh->u.addr.uri.user.len, frh->u.addr.uri.user.p);
        sip_hdr_t *toh_pre = sip_hdr_get(m, SIP_H_TO);
        if (toh_pre && sip_hdr_parse_addr(toh_pre) == NSBC_OK && toh_pre->u.addr.uri.user.p)
            snprintf(f_to, sizeof(f_to), "%.*s", (int)toh_pre->u.addr.uri.user.len, toh_pre->u.addr.uri.user.p);

        security_fence_ctx_t fctx = {0};
        fctx.src    = (const struct sockaddr *)src;
        fctx.method = SIP_M_REGISTER;
        fctx.ua     = f_ua[0]   ? f_ua   : NULL;
        fctx.caller = f_from[0] ? f_from : NULL;
        fctx.callee = f_to[0]   ? f_to   : NULL;
        security_check_fence(&fctx);
        if (fctx.hit != BLACKLIST_OK) {
            llog(LOG_INFO, "reg",
                 "REGISTER → drop by security fence hit=%d"
                 " aor=%s from=%s src_ip=%s ua=%s",
                 fctx.hit, f_to, f_from, src_ip, f_ua);
            security_fence_log_drop(fctx.hit, src_ip, f_ua, "REGISTER", f_from, f_to);
            _rc = NSBC_OK; goto reg_out;  /* silent drop，不回任何响应 */
        }
    }

    /* AoR 取 To 头 */
    sip_hdr_t *toh = sip_hdr_get(m, SIP_H_TO);
    if (!toh || sip_hdr_parse_addr(toh) != NSBC_OK ||
        !toh->u.addr.uri.user.p) {
        respond(r, proto, src, m, 400, NULL, NULL, 0, txn);
        _rc = NSBC_OK; goto reg_out;
    }
    char aor[LOC_AOR_MAX];
    sip_uri_t *u = &toh->u.addr.uri;
    /* AoR host = To 头 URI 里的 host —— 这才是租户标识（a.com / b.com / nsbc.local）。
     * 多租户场景：1004@a.com 注册，AoR 就是 1004@a.com，HA1 也按 (1004, a.com) 存。
     * 之前错误地强制用全局 cfg.realm → 多租户注册全被归一化成 user@nsbc.local，
     * location DB 和 HA1 表 realm 对不上 → resolve 找不到 / digest auth user_unknown。 */
    const char *aor_host = u->host.p && u->host.len ? u->host.p : r->cfg.realm;
    size_t aor_host_len = u->host.len ? u->host.len : strlen(aor_host);
    snprintf(aor, sizeof(aor), "%.*s@%.*s",
             (int)u->user.len, u->user.p,
             (int)aor_host_len, aor_host);

    /* ---- realm 提取：Digest challenge 必须和客户端配置的 realm 一致 ----
     *
     * 多租户架构：To 头 host = 租户 domain（nsbc.local / a.com），
     * Web DB 里 HA1 按 (user, realm) 存 → digest_verify 必须用这个 realm。
     *
     * IP 地址作 realm 客户端不认 → To host 是 IP 时退回 cfg.realm。
     *
     * ★ 关键：challenge realm 必须 = 客户端配的 SIP Domain。
     *   MicroSIP 配 Domain=nsbc.local → challenge 给 realm="nsbc.local" 才能认。
     *   之前 force_register_ok=1 跳过 auth 时这个 bug 永远不暴露！ */
    char reg_realm[64];
    char host_tmp[64] = {0};
    const char *host_override = NULL;
    if (u->host.p && u->host.len > 0) {
        size_t hlen = u->host.len < sizeof(host_tmp) - 1
                    ? u->host.len : sizeof(host_tmp) - 1;
        memcpy(host_tmp, u->host.p, hlen);
        host_tmp[hlen] = '\0';
        struct sockaddr_in sin4;
        struct sockaddr_in6 sin6;
        if (inet_pton(AF_INET, host_tmp, &sin4.sin_addr) != 1 &&
            inet_pton(AF_INET6, host_tmp, &sin6.sin6_addr) != 1) {
            host_override = host_tmp;
        }
    }
    if (host_override) {
        snprintf(reg_realm, sizeof(reg_realm), "%s", host_override);
    } else if (r->cfg.realm && r->cfg.realm[0]) {
        snprintf(reg_realm, sizeof(reg_realm), "%s", r->cfg.realm);
    } else {
        snprintf(reg_realm, sizeof(reg_realm), "nsbc.local");
    }
    /* ===== 诊断入口：完整 REGISTER 请求上下文 =====
     * 打在 realm 确定后（还没进 auth）—— 这是整个注册链的"第一帧"。
     * 后续 AUTH/200/evict/upsert 日志都用同一个 call_id 关联。
     * ★ 同时提取 aor 的 user 部分（短号/分机号）—— grep 'user=1006' 就能聚合
     *   整个注册+通话链（b2bua 的 new_invite/cdr 也打 caller/callee，天然对齐）。 */
    sip_hdr_t *_cid0 = sip_hdr_get(m, SIP_H_CALL_ID);
    sip_hdr_t *_ua0  = sip_hdr_get(m, SIP_H_USER_AGENT);
    char _cid_str[LOC_CALLID_MAX] = "";
    char _ua_str[128] = "";
    if (_cid0 && _cid0->value.len > 0)
        snprintf(_cid_str, sizeof(_cid_str), "%.*s",
                 (int)(_cid0->value.len < sizeof(_cid_str) - 1
                           ? _cid0->value.len
                           : sizeof(_cid_str) - 1),
                 _cid0->value.p);
    if (_ua0 && _ua0->value.len > 0)
        snprintf(_ua_str, sizeof(_ua_str), "%.*s",
                 (int)(_ua0->value.len < sizeof(_ua_str) - 1
                           ? _ua0->value.len
                           : sizeof(_ua_str) - 1),
                 _ua0->value.p);
    /* 从 aor 里提取 user 部分（@ 前面） —— 1006@nsbc.local → user=1006 */
    char _reg_user[64] = "";
    {
        const char *_at = strchr(aor, '@');
        size_t _ulen = _at ? (size_t)(_at - aor) : strlen(aor);
        if (_ulen >= sizeof(_reg_user)) _ulen = sizeof(_reg_user) - 1;
        memcpy(_reg_user, aor, _ulen);
        _reg_user[_ulen] = '\0';
    }
    {
        const char *_role_s =
            ha_role() == HA_ROLE_ACTIVE ? "ACTIVE" : "STANDBY";
        /* ★ 关键增强：CSeq / Expires 头 / Contact 原始值 / Route —— 用于识别
         * 注销请求（expires=0 或 Contact:*）和客户端路由行为（是否回带了
         * 我们 200 OK 里 Service-Route 的值） */
        char _cseq_str[48] = "", _exp_str[32] = "", _ct_str[96] = "";
        char _rt_str[72] = "";
        sip_hdr_t *_h;
        if ((_h = sip_hdr_get(m, SIP_H_CSEQ)) && _h->value.len > 0)
            snprintf(_cseq_str, sizeof(_cseq_str), "%.*s",
                     (int)(_h->value.len < sizeof(_cseq_str) - 1
                               ? _h->value.len : sizeof(_cseq_str) - 1),
                     _h->value.p);
        if ((_h = sip_hdr_get(m, SIP_H_EXPIRES)) && _h->value.len > 0)
            snprintf(_exp_str, sizeof(_exp_str), "%.*s",
                     (int)(_h->value.len < sizeof(_exp_str) - 1
                               ? _h->value.len : sizeof(_exp_str) - 1),
                     _h->value.p);
        if ((_h = sip_hdr_get(m, SIP_H_CONTACT)) && _h->value.len > 0)
            snprintf(_ct_str, sizeof(_ct_str), "%.*s",
                     (int)(_h->value.len < sizeof(_ct_str) - 1
                               ? _h->value.len : sizeof(_ct_str) - 1),
                     _h->value.p);
        if ((_h = sip_hdr_get(m, SIP_H_ROUTE)) && _h->value.len > 0)
            snprintf(_rt_str, sizeof(_rt_str), "%.*s",
                     (int)(_h->value.len < sizeof(_rt_str) - 1
                               ? _h->value.len : sizeof(_rt_str) - 1),
                     _h->value.p);
        llog(LOG_DEBUG, "reg",
             "REGISTER-IN user=%s aor=%s realm=%s src=%s proto=%d ha_role=%s "
             "force_ok=%d call_id=%s ua=%s",
             _reg_user, aor, reg_realm, src_ip, proto, _role_s,
             r->cfg.force_register_ok,
             _cid_str[0] ? _cid_str : "(none)",
             _ua_str[0] ? _ua_str : "(none)");
    }

    llog(LOG_DEBUG, "reg",
         "REGISTER aor=%s realm=%s (to_host=%s cfg.realm=%s)",
         aor, reg_realm,
         host_override ? host_override : "(cfg fallback)",
         r->cfg.realm ? r->cfg.realm : "nsbc.local");

    /* 三级令牌桶 */
    if (!tb_take(&r->ip[sa_key(src) & (TB_IP_N - 1)], sa_key(src),
                 r->cfg.ip_cps, now) ||
        !tb_take(&r->aor[str_key(aor) & (TB_AOR_N - 1)], str_key(aor),
                 r->cfg.aor_cps, now) ||
        !global_take(r, now)) {
        __sync_fetch_and_add(&r->n_thr, 1);
        char extra[64];
        snprintf(extra, sizeof(extra), "Retry-After: %d\r\n",
                 r->cfg.retry_after_s);
        respond(r, proto, src, m, 503, extra, NULL, 0, txn);
        _rc = NSBC_OK; goto reg_out;
    }

    /* Digest 鉴权 —— realm 用 REGISTER 请求 To 头的 URI host（reg_realm），
     * 不用 profile realm / 全局 cfg.realm —— 租户标识来自请求本身。
     * 这保证 1004@a.com 注册时 HA1 查表用 realm=a.com，与 Web DB 里存的 realm 对齐。 */
    digest_cfg_t dig = r->dig;
    dig.realm = reg_realm;

    /* ★★★ User-exists pre-check — 安全加固核心（REQ-SEC-008）★★★
     *
     * 扫号器发 REGISTER，如果收到 401 WWW-Authenticate 就知道"此用户存在
     * 但密码不对"。必须先查 HA1 表——用户不存在就 silent drop，
     * 连 401 challenge 都不发（防止 user enum）。
     *
     * 与 INVITE dp_precheck 同理：信息隐藏比友好错误更重要。
     *
     * ha1_cb 签名：const char *(*cb)(realm, rlen, user, ulen, ud);
     * 返回 NULL = user 不存在 → silent drop。
     * 返回非 NULL（HA1 hex）→ 正常走 401 challenge / digest_verify。 */
    sip_hdr_t *auth = sip_hdr_get(m, SIP_H_AUTHORIZATION);
    {
        const char *ha1_found = NULL;
        /* ★ force_register_ok=1 跳过 ha1 查表 —— 调试期允许任何 AoR 直接注册，
         * 不因 Rust 侧 UDS 超时 / ha1 cache miss 导致 silent drop。
         * 这是之前发现的 BUG：force_register_ok 只跳过 auth 验证，
         * 但 ha1_cb 在它前面 → UDS 查不到 ha1 就 silent drop → 客户端无响应 → "错误网关"。 */
        int skip_ha1_check = (r->cfg.force_register_ok != 0);
        if (!skip_ha1_check && dig.ha1_cb && u->user.p && u->user.len > 0) {
            ha1_found = dig.ha1_cb(reg_realm, strlen(reg_realm),
                                   u->user.p, u->user.len, dig.ha1_ud);
        }
        if (!skip_ha1_check && !ha1_found) {
            /* 用户不存在 → silent drop（既不发 401 也不发 403） */
            llog(LOG_NOTICE, "reg",
                 "REGISTER → silent drop: user NOT FOUND "
                 "aor=%s user_len=%zu realm=%s src_ip=%s auth_present=%d",
                 aor, u->user.len, reg_realm,
                 src_ip, auth ? 1 : 0);
            _rc = NSBC_OK; goto reg_out;
        }
        if (skip_ha1_check) {
            static _Atomic unsigned skc = 0;  /* P1: _Atomic — 多 worker 并发 ++ */
            unsigned sk = atomic_fetch_add(&skc, 1);
            if (sk % 256 == 0) {
                llog(LOG_DEBUG, "reg",
                     "force_register_ok=1 → skip ha1 user-exists check aor=%s", aor);
            }
        }
        /* HA1 存在 → 继续正常流程（无论 auth 头有没有） */
    }

    /* ★★ force_register_ok：读 YAML registrar.force_register_ok（默认 1=跳过 auth）
     * 生产环境设 0 让正常 digest auth 生效。 */
    int auth_ok_override = (r->cfg.force_register_ok != 0);

    /* Digest 鉴权 */
    llog(LOG_DEBUG, "reg", "REGISTER Authorization hdr ptr=%p", (void*)auth);
    if (auth_ok_override) {
        static _Atomic unsigned okc = 0;  /* P1: _Atomic — 多 worker 并发 ++ */
        unsigned ok = atomic_fetch_add(&okc, 1);
        if (ok % 256 == 0) {
            llog(LOG_WARN, "reg",
                 "★ force_register_ok=1 active — ALL REGISTERs skip auth! "
                 "aor=%s src_ip=%s (turn off after troubleshooting)",
                 aor, src_ip);
        }
        goto reg_auth_ok;
    }
    if (!auth || sip_hdr_parse_auth(auth) != NSBC_OK) {
        /* RFC 3261 标准流程：客户端第一次 REGISTER 不带 Authorization
         * 服务器回 401 + WWW-Authenticate challenge，客户端带 auth 重试。 */
        char ch[512], extra[600];
        digest_make_challenge(&dig, 0, ch, sizeof(ch));
        snprintf(extra, sizeof(extra), "WWW-Authenticate: %s\r\n", ch);

        /* ★★ 诊断日志 — 精确区分 "客户端没发 auth 头" vs "发了但解析失败"
         * 同时打印完整 challenge 字符串，方便确认格式对不对。 */
        if (!auth) {
            static __thread uint64_t bare_ms = 0;
            uint64_t now = nsbc_now_ms();
            if (now - bare_ms > 3000) {   /* 每 3 秒聚合一条 — DEBUG 级（正常握手非异常） */
                char _u[64]; _aor_user(aor, _u, sizeof(_u));
                llog(LOG_DEBUG, "reg",
                     "REGISTER → 401 challenge (RFC 3261 handshake, no auth hdr) "
                     "user=%s aor=%s src_ip=%s realm=%s",
                     _u, aor, src_ip, reg_realm);
                bare_ms = now;
            }
        } else {
            llog(LOG_WARN, "reg",
                 "REGISTER → 401 challenge (client SENT Authorization but PARSE FAILED) "
                 "aor=%s src_ip=%s realm=%s auth_hdr_len=%zu",
                 aor, src_ip, reg_realm, auth->value.len);
        }

        respond(r, proto, src, m, 401, extra, NULL, 0, txn);
        _rc = NSBC_OK; goto reg_out;
    }
    llog(LOG_DEBUG, "reg", "REGISTER with Authorization header → verifying...");
    dig_auth_rc_t ar = digest_verify(&dig, m, &auth->u.auth, now);
    llog(LOG_DEBUG, "reg", "digest_verify rc=%d (0=OK)", ar);
    if (ar == DIG_AUTH_STALE_NONCE) {
        char ch[512], extra[600];
        digest_make_challenge(&dig, 1, ch, sizeof(ch));
        snprintf(extra, sizeof(extra), "WWW-Authenticate: %s\r\n", ch);
        llog(LOG_DEBUG, "reg", "REGISTER → 401 stale nonce");
        respond(r, proto, src, m, 401, extra, NULL, 0, txn);
        _rc = NSBC_OK; goto reg_out;
    }
    if (ar != DIG_AUTH_OK) {
        __sync_fetch_and_add(&r->n_rej, 1);
        const char *reason = "auth_failed";
        const char *rc_str = "unknown";
        switch (ar) {
        case DIG_AUTH_USER_UNKNOWN: rc_str = "user_unknown"; break;
        case DIG_AUTH_BAD_NONCE:    rc_str = "bad_nonce";    break;
        case DIG_AUTH_MISMATCH:     rc_str = "response_mismatch"; break;
        case DIG_AUTH_BAD_FORMAT:   rc_str = "bad_format";   break;
        default: rc_str = "rc_other"; break;
        }
        /* 从 auth params 提取 username/realm（安全拷贝到栈上） */
        char auth_user[128] = "-", auth_realm[128] = "-";
        if (auth) {
            auth_param_get(&auth->u.auth, "username", auth_user, sizeof(auth_user));
            auth_param_get(&auth->u.auth, "realm",    auth_realm, sizeof(auth_realm));
        }
        llog(LOG_WARN, "reg",
             "REGISTER → 403 auth failed rc=%d (%s)", ar, rc_str);
        llog(LOG_WARN, "reg",
             "key=fail2ban src_ip=%s reason=%s user=%s realm=%s",
             src_ip, reason, auth_user, auth_realm);
        /* 内置动态封禁：上报认证失败（security 模块计数，窗口内超
         * 阈值自动封禁该 IP，后续 REGISTER/INVITE 静默丢弃） */
        b2b_guard_notify(src_ip, B2B_GUARD_AUTH_FAIL);
        respond(r, proto, src, m, 403, NULL, NULL, 0, txn);
        _rc = NSBC_OK; goto reg_out;
    }
reg_auth_ok:
    llog(LOG_DEBUG, "reg",
         "REGISTER-AUTH-OK user=%s aor=%s src=%s force_ok=%d",
         _reg_user, aor, src_ip, auth_ok_override ? 1 : 0);
    b2b_guard_notify(src_ip, B2B_GUARD_AUTH_OK); /* auth OK：通知 security 清零 fail/ratelimit 计数 */

    /* ---- RFC 3327 §6.2：Path 头回显 ----
     * IMS UE 发 REGISTER 时 P-CSCF 会插入 Path: <sip:p-cscf@...;lr> 头链；
     * REGISTER 200 OK 必须原样回显请求里所有 Path 头，让 UE 知道后续
     * INVITE 该走 P-CSCF 还是直连。我们作为 B2BUA/SBC 不是 S-CSCF，不生成
     * 新 Path，但必须透传回 UE。Path 可能多值（comma-separated 或多行头）。 */
    char path_extra[512]; path_extra[0] = '\0';
    {
        size_t po = 0;
        int first = 1;
        for (sip_hdr_t *h = sip_hdr_get(m, SIP_H_PATH); h;
             h = sip_hdr_next(m, h)) {
            if (h->type != SIP_H_PATH) break;
            if (!first) {
                /* 多 Path 头 → 逗号拼（RFC 3327 允许 comma-separated value） */
                int w = snprintf(path_extra + po, sizeof(path_extra) - po, ", ");
                if (w > 0 && (size_t)w < sizeof(path_extra) - po) po += (size_t)w;
            }
            first = 0;
            int w = snprintf(path_extra + po, sizeof(path_extra) - po,
                             "%.*s", (int)h->value.len, h->value.p ? h->value.p : "");
            if (w > 0 && (size_t)w < sizeof(path_extra) - po) po += (size_t)w;
        }
        if (path_extra[0]) {
            /* 包成完整头行："Path: ...\r\n" */
            char full[600];
            snprintf(full, sizeof(full), "Path: %s\r\n", path_extra);
            path_extra[0] = '\0';
            snprintf(path_extra, sizeof(path_extra), "%s", full);
            llog(LOG_DEBUG, "reg", "REGISTER Path echo: %s", path_extra);
        }
    }

    /* ---- RFC 3608：Service-Route 生成 + 入站回显 ----
     * ★ 2026-11-14 修复：此前无条件发 <sip:{realm};lr>，realm（如
     *   nsbc.local）是公网客户端无法解析的内部域名。遵循 RFC 3608 的
     *   UA（pjsip 等）会把它拷进路由集 → 后续请求发送失败 → pjsip 报
     *   502 Bad Gateway（MicroSIP 中文显示"错误的网关"）。
     *   现改为配置驱动：registrar.service_route_host 为空则完全不发。
     *   同时请求里如果已经带了 Service-Route（outreg 上游注入），原样回显。 */
    char generated_service_route[256]; generated_service_route[0] = '\0';
    {
        /* 仅当显式配置了可路由主机名时才生成（registrar 角色） */
        if (r->cfg.service_route_host[0])
            snprintf(generated_service_route, sizeof(generated_service_route),
                     "<sip:%s;lr>", r->cfg.service_route_host);
    }

    /* 合成 REGISTER 200 OK 公共 extra 前缀：Service-Route 在前，Path 在后，
     * 最后 Contact/Expires 链由各个 respond 调用点再追加 */
    char hdr_prefix[1024]; hdr_prefix[0] = '\0';
    {
        size_t ho = 0;
        if (generated_service_route[0]) {
            int w = snprintf(hdr_prefix + ho, sizeof(hdr_prefix) - ho,
                             "Service-Route: %s\r\n", generated_service_route);
            if (w > 0 && (size_t)w < sizeof(hdr_prefix) - ho) ho += (size_t)w;
            llog(LOG_DEBUG, "reg", "REGISTER Service-Route generated: %s",
                 generated_service_route);
        }
        if (path_extra[0]) {
            int w = snprintf(hdr_prefix + ho, sizeof(hdr_prefix) - ho,
                             "%s", path_extra);
            if (w > 0 && (size_t)w < sizeof(hdr_prefix) - ho) ho += (size_t)w;
        }
    }

    /* 过期基线：Expires 头 */
    long def_exp = r->cfg.default_expires;
    sip_hdr_t *eh = sip_hdr_get(m, SIP_H_EXPIRES);
    if (eh) {
        char tmp[16];
        size_t k = 0;
        while (k < eh->value.len && k < sizeof(tmp) - 1 &&
               eh->value.p[k] >= '0' && eh->value.p[k] <= '9')
            tmp[k] = eh->value.p[k], ++k;
        if (k) {
            tmp[k] = '\0';
            def_exp = atol(tmp);
        }
    }

    /* Contact: * 整 AoR 注销 */
    sip_hdr_t *ch0 = sip_hdr_get(m, SIP_H_CONTACT);
    sip_hdr_t contact_tmp; /* 提前声明，避免 GCC 12 dangling-pointer */
    llog(LOG_DEBUG, "reg", "loc_add: Contact hdr ptr=%p star=%d val_len=%zd",
         (void*)ch0, ch0 ? (int)ch0->u.addr.star : 0,
         ch0 ? ch0->value.len : 0);
    if (ch0) {
        /* 手动 parse Contact — 兼容"带尖括号"和"裸 sip: URI"两种形式
         * RFC 3261 §20.10：Contact 值可以是 "sip:user@host" 或 "<sip:user@host>;param" */
        const char *cv = ch0->value.p;
        size_t cv_len = ch0->value.len;
        size_t actual_len = cv_len;
        /* 只有存在 '>' 尖括号时才截断；否则保持全 len（裸 sip: URI 形式） */
        const char *gt = memchr(cv, '>', cv_len);
        if (gt) actual_len = (size_t)(gt - cv) + 1; /* 包含 '>' */
        contact_tmp = *ch0;
        contact_tmp.value.len = actual_len;
        sip_hdr_parse_addr(&contact_tmp);
        ch0 = &contact_tmp; /* 用处理后的副本 */
    }
    if (ch0 && sip_hdr_parse_addr(ch0) == NSBC_OK && ch0->u.addr.star) {
        char _u[64]; _aor_user(aor, _u, sizeof(_u));
        llog(LOG_DEBUG, "reg",
             "REGISTER-LOGOUT user=%s aor=%s (Contact:* Expires:0 → loc_remove all)",
             _u, aor);
        loc_remove(r->cfg.db, aor, "*");
        /* 注销响应也带 Service-Route + Path（RFC 3608/3327 覆盖所有 REGISTER 200 OK） */
        if (hdr_prefix[0]) {
            char full_extra[1024];
            snprintf(full_extra, sizeof(full_extra),
                     "%sContact: *\r\nExpires: 0\r\n", hdr_prefix);
            respond(r, proto, src, m, 200, full_extra, NULL, 0, txn);
        } else {
            respond(r, proto, src, m, 200, "Contact: *\r\nExpires: 0\r\n",
                    NULL, 0, txn);
        }
        _rc = NSBC_OK; goto reg_out;
    }

    /* 逐 Contact 处理 */
    int nc = 0;
    /* 被移除的 Contact 回显链（RFC 3261 §10.3.5：注销 200 OK 必须带
     * Contact: <uri>;expires=0，不能是裸 200 OK） */
    char removed_ct[1024]; removed_ct[0] = '\0';
    size_t removed_used = 0;
    int removed_n = 0;
    for (sip_hdr_t *h = ch0; h; h = sip_hdr_next(m, h)) {
        llog(LOG_DEBUG, "reg", "loc_add loop: h=%p type=%d", (void*)h, h->type);
        if (h->type != SIP_H_CONTACT)
            continue;
        int prc = sip_hdr_parse_addr(h);
        llog(LOG_DEBUG, "reg", "loc_add: parse_addr rc=%d star=%d val_len=%zd val=%.100s", prc,
             prc == NSBC_OK ? (int)h->u.addr.star : -1,
             h->value.len, h->value.p ? h->value.p : "(null)");
        if (prc != NSBC_OK)
            continue;
        long exp = param_num(h->value.p, h->value.len, "expires");
        if (exp < 0)
            exp = def_exp;

        /* === FS sip-force-expires + sip-expires-max-deviation 语义 ===
         * force_expires > 0：完全覆盖客户端发来的 expires（不管 10s 还是 10000s）
         * expires_jitter_s > 0：force_expires ± jitter 随机化（防注册风暴，
         *   避免所有客户端在同一秒重注册）。
         * jitter 公式：force + rand(-jitter, +jitter)，夹到 [min_expires, max_expires] 区间 */
        if (r->cfg.force_expires > 0) {
            long forced = r->cfg.force_expires;
            if (r->cfg.expires_jitter_s > 0) {
                int j = r->cfg.expires_jitter_s;
                int rnd = rand() % (2 * j + 1) - j;   /* -j..+j */
                forced += rnd;
            }
            if (forced < r->cfg.min_expires) forced = r->cfg.min_expires;
            if (forced > r->cfg.max_expires) forced = r->cfg.max_expires;
            llog(LOG_DEBUG, "reg", "force_expires: client=%ld → server=%ld (force=%d jitter=%d)",
                 exp, forced, r->cfg.force_expires, r->cfg.expires_jitter_s);
            exp = forced;
        }

        if (exp < r->cfg.min_expires && exp != 0) {
            char extra[64];
            snprintf(extra, sizeof(extra), "Min-Expires: %d\r\n",
                     r->cfg.min_expires);
            respond(r, proto, src, m, 423, extra, NULL, 0, txn);
            _rc = NSBC_OK; goto reg_out;
        }
        if (exp > r->cfg.max_expires)
            exp = r->cfg.max_expires;

        if (exp == 0) {
            char ctext[LOC_CONTACT_MAX];
            copy_addr(ctext, sizeof(ctext), h);
            loc_binding_t dummy; memset(&dummy, 0, sizeof(dummy));
            snprintf(dummy.aor, sizeof(dummy.aor), "%s", aor);
            snprintf(dummy.contact, sizeof(dummy.contact), "%s", ctext);
            loc_remove(r->cfg.db, aor, ctext);
            reg_push(&dummy, 0);   /* write-through: 注销快照（NAT 字段留空） */

            /* === HA P1: ACTIVE → REPL_REG action=REMOVE ===
             * loc_binding_t.expire_ms 为 0 时 STANDBY apply 侧应视为注销 */
            {
                int64_t rc = ha_repl_publish(REPL_REG, 1, &dummy, sizeof(dummy));
                if (rc < 0 && rc != NSBC_NOSPC)
                    llog(LOG_WARN, "reg",
                         "repl_publish REG-REMOVE failed rc=%lld aor=%s",
                         (long long)rc, dummy.aor);
            }
            /* ★ RFC 3261 §10.3.5/§10.2.4：200 OK 必须回显被移除的 Contact
             *   （带 expires=0）。2026-11-14 之前这里直接 continue，导致
             *   注销响应是"裸 200 OK"（无 Contact 无 Expires）——UA 无法
             *   从响应判定注册状态，pjsua 等栈按"注册被移除"处理并可能
             *   走异常分支（MicroSIP "错误的网关"）。 */
            {
                int w = snprintf(removed_ct + removed_used,
                                 sizeof(removed_ct) - removed_used,
                                 "Contact: %s;expires=0\r\n", ctext);
                if (w > 0 && (size_t)w < sizeof(removed_ct) - removed_used) {
                    removed_used += (size_t)w;
                    ++removed_n;
                }
            }
            continue;
        }
        loc_binding_t b;
        memset(&b, 0, sizeof(b));
        snprintf(b.aor, sizeof(b.aor), "%s", aor);
        copy_addr(b.contact, sizeof(b.contact), h);
        param_str(h->value.p, h->value.len, "+sip.instance",
                  b.instance, sizeof(b.instance));
        sip_hdr_t *cid = sip_hdr_get(m, SIP_H_CALL_ID);
        if (cid)
            snprintf(b.callid, sizeof(b.callid), "%.*s",
                     (int)(cid->value.len < sizeof(b.callid) - 1
                               ? cid->value.len
                               : sizeof(b.callid) - 1),
                     cid->value.p);
        sip_hdr_t *cs = sip_hdr_get(m, SIP_H_CSEQ);
        if (cs && sip_hdr_parse_cseq(cs) == NSBC_OK)
            b.cseq = (uint32_t)cs->u.cseq.number;
        long rid = param_num(h->value.p, h->value.len, "reg-id");
        if (rid > 0)
            b.reg_id = (uint32_t)rid;
        b.expire_ms = now + (uint64_t)exp * 1000;
        b.regs = 1;
        /* RFC 7118（SIP over WebSocket）：浏览器 Contact host 是 .invalid
         * 假域名，被叫无法经 sockaddr/DNS 路由 — 保存 ws_conn* 到 binding，
         * resolve_cb 直接走 WS 通道回此连接（发送前 ws_conn_alive 校验）。 */
        if (proto == SIP_TR_WS || proto == SIP_TR_WSS)
            b.ws_conn = sig_ws_bridge_conn_from_sa(src);

        /* === NAT 检测 + Contact 改写（REQ-NAT-001）=== */
        b.transport_proto = proto;
        /* IMS Service-Route：存到 binding 供 outreg 场景 resolve 出站时 prepend */
        if (generated_service_route[0])
            snprintf(b.service_route, sizeof(b.service_route),
                     "%s", generated_service_route);
        /* 1. 取 src IP:port */
        char src_ip[INET6_ADDRSTRLEN] = "";
        uint16_t src_port = 0;
        sa_to_str(src, src_ip, sizeof(src_ip), &src_port);

        /* 2. 取 Via 深解析的 received/rport（top Via） */
        char via_recv[64] = "";
        uint16_t via_rport = 0;
        sip_hdr_t *vh0 = sip_hdr_get(m, SIP_H_VIA);
        if (vh0 && sip_hdr_parse_via(vh0) == NSBC_OK) {
            const sip_via_t *v = &vh0->u.via;
            if (v->received.p && v->received.len > 0) {
                size_t n = v->received.len < sizeof(via_recv) - 1 ? v->received.len : sizeof(via_recv) - 1;
                memcpy(via_recv, v->received.p, n); via_recv[n] = '\0';
            }
            if (v->rport.p && v->rport.len > 0) {
                char tmp[16]; size_t n = v->rport.len < sizeof(tmp) - 1 ? v->rport.len : sizeof(tmp) - 1;
                memcpy(tmp, v->rport.p, n); tmp[n] = '\0';
                via_rport = (uint16_t)atoi(tmp);
            }
        }

        /* 3. 解析 Contact host/port */
        char c_user[64] = "", c_host[128] = "";
        uint16_t c_port = 0;
        contact_parts(h, c_user, sizeof(c_user), c_host, sizeof(c_host), &c_port);
        if (c_port == 0) {
            /* 端口未显式 → proto 默认端口 */
            if (proto == SIP_TR_TLS || proto == SIP_TR_WSS) c_port = 5061;
            else if (proto == SIP_TR_UDP || proto == SIP_TR_TCP || proto == SIP_TR_WS) c_port = 5060;
        }

        /* 4. NAT 检测 + received 优先级：Via received > src IP */
        b.nat_detected = nat_detect(c_host, via_recv, via_rport, src_ip, src_port);
        /* ★ REQ-NAT-EXPIRE-001：NAT 场景强制短 expires。
         * 家用路由器 NAT UDP conntrack 默认 ~300s 超时，
         * 如果默认 expires=600s → FreeSWITCH 每 300s 刷新一次 → 刚好卡在 NAT 超时边缘 →
         * NAT 映射先过期 → NSBC 200 OK 发出去但 conntrack 没条目 → 包到不了分机 → 客户端超时 → "错误网关"。
         * 解决：检测到 NAT 时把 exp 压到 nat_expires_short（默认 120s），
         * 客户端每 60s 刷新一次 → 远快于 NAT 超时 → 映射持续活跃。
         * force_expires > 0 时用户手动强制了，优先尊重。 */
        if (b.nat_detected && r->cfg.nat_expires_short > 0 && r->cfg.force_expires == 0) {
            if (exp > r->cfg.nat_expires_short) {
                long old_exp = exp;
                exp = r->cfg.nat_expires_short;
                /* 重新夹逼 min/max：短 expires 也不能低于 min_expires */
                if (exp < r->cfg.min_expires) exp = r->cfg.min_expires;
                b.expire_ms = now + (uint64_t)exp * 1000;
                static _Atomic unsigned nc_override = 0;  /* P1: _Atomic — 多 worker 并发 ++ */
                unsigned nc = atomic_fetch_add(&nc_override, 1);
                if (nc % 256 == 0) {
                    llog(LOG_INFO, "reg",
                         "NAT detected → short expires override aor=%s exp=%ld→%ld "
                         "contact=%s nat_expires_short=%d",
                         b.aor, old_exp, exp, b.contact, r->cfg.nat_expires_short);
                }
            }
        }
        /* received_ip 优先 Via received（存在），否则 src IP */
        if (via_recv[0]) {
            snprintf(b.received_ip, sizeof(b.received_ip), "%s", via_recv);
            b.received_port = via_rport ? via_rport : src_port;
        } else {
            snprintf(b.received_ip, sizeof(b.received_ip), "%s", src_ip);
            b.received_port = src_port;
        }

        /* 5. 改写 public_contact（仅 NAT 场景；公网直连 public_contact = contact） */
        if (b.nat_detected && b.received_ip[0]) {
            snprintf(b.public_contact, sizeof(b.public_contact),
                     "sip:%s@%s:%u;transport=%s",
                     c_user[0] ? c_user : "",  /* user 可能为空（anonymous） */
                     b.received_ip, b.received_port,
                     proto_str(proto));
            /* REQ-LOG-001：NAT detected 日志节流 — 同一 aor+public_contact 30s 只打一条。
             * FreeSWITCH 终端启动时会在 1s 内发 3-5 条 REGISTER（重试），
             * 每条都命中 NAT → 日志刷屏。但 public_contact 变了（端口变）必须打新的。 */
            {
                /* P1: static __thread — 每个 worker 线程独立节流，避免跨线程竞态。
                 * GCC 要求函数内 __thread 必须配 static（file-scope 隐式有 static）。 */
                static __thread char last_aor[128];
                static __thread char last_pub[256];
                static __thread uint64_t last_log_ms;
                uint64_t now_ms = now * 1000ULL;
                int same = (strcmp(last_aor, b.aor) == 0 && strcmp(last_pub, b.public_contact) == 0);
                if (!same || now_ms - last_log_ms > 30000) {
                    strncpy(last_aor, b.aor, sizeof(last_aor) - 1);
                    strncpy(last_pub, b.public_contact, sizeof(last_pub) - 1);
                    last_log_ms = now_ms;
                    llog(LOG_DEBUG, "reg",
                         "NAT detected contact=%s → public_contact=%s src=%s via_recv=%s",
                         b.contact, b.public_contact, src_ip, via_recv);
                } else {
                    llog(LOG_DEBUG, "reg",
                         "NAT detected (throttled) aor=%s public=%s",
                         b.aor, b.public_contact);
                }
            }
        } else {
            /* 公网直连（含 WS/WSS）：public_contact = 原始 Contact */
            snprintf(b.public_contact, sizeof(b.public_contact), "%s", b.contact);
            llog(LOG_DEBUG, "reg", "no NAT contact=%s src=%s:%u", b.contact, src_ip, src_port);
        }

        llog(LOG_DEBUG, "reg", "loc_upsert call: aor=%s contact=%s public=%s nat=%d expire_ms=%lu exp=%ld now=%lu",
             b.aor, b.contact, b.public_contact, b.nat_detected,
             (unsigned long)b.expire_ms, exp, (unsigned long)now);

        /* === FS multiple-registrations=false：一个 AoR 只保留最新 Contact ===
         * 新 REGISTER 进来先整 AoR 清空，再 upsert 当前 contact。
         * 同 Call-ID + CSeq 刷新场景 loc_remove 不影响（刚注册完立即刷新）。 */
        if (r->cfg.multiple_registrations == 0) {
            loc_remove(r->cfg.db, b.aor, NULL);
            llog(LOG_DEBUG, "reg", "multiple_registrations=false → evict all contacts for aor=%s", b.aor);
        }

        /* === FS max-registrations-per-extension=N：per-AoR binding 数上限 ===
         * 在 loc_upsert 之前查当前 AoR 有效 binding 数。
         * multiple_registrations=false 已在上面清空，所以上限检查只对允许多注册的场景生效。
         *
         * ★ 2026-11-11: 达到上限时的行为从"403 拒绝"改为"踢掉最旧 binding"：
         *   单设备场景 (max=1) 下，WebRTC 断线重连会生成新的随机 contact 和新 Call-ID，
         *   永远不会 dup → 累积到 max 后被 403 拒绝 → 用户无法重注册。
         *   新行为：找到 expire_ms 最小的那条（最近要过期的 / 最旧的），loc_remove 掉，
         *   然后继续 upsert 新 binding。相当于 FIFO 的设备替换语义。
         *
         *   dup 判断保持不变：contact 完全相同 OR 同一个 Call-ID（同一终端刷新）。
         *   dup 直接 loc_upsert 覆盖，不踢别人。 */
        if (r->cfg.max_registrations_per_ext > 0 && r->cfg.multiple_registrations != 0) {
            loc_binding_t probe[32];
            int cur = loc_query(r->cfg.db, b.aor, probe, 32, now);
            if (cur >= r->cfg.max_registrations_per_ext) {
                int dup = 0;
                for (int i = 0; i < cur; ++i) {
                    if (is_dup_binding(&probe[i], &b)) { dup = 1; break; }
                }
                if (!dup) {
                    /* ★ 踢掉 expire_ms 最小的那条（最近要过期的 / 最旧的 binding） */
                    int oldest = 0;
                    for (int i = 1; i < cur; ++i) {
                        if (probe[i].expire_ms < probe[oldest].expire_ms) oldest = i;
                    }
                    llog(LOG_INFO, "reg",
                         "max_registrations_per_ext: user=%s aor=%s cur=%d >= max=%d "
                         "→ evict oldest contact=%.80s expire_ms=%lu → allow new contact=%.80s",
                         _reg_user, b.aor, cur, r->cfg.max_registrations_per_ext,
                         probe[oldest].contact,
                         (unsigned long)probe[oldest].expire_ms,
                         b.contact);
                    int removed = loc_remove(r->cfg.db, b.aor, probe[oldest].contact);
                    if (removed <= 0) {
                        /* 极端竞争：被 sweep 清走了 → 回到 cur 检查再试一次 dup → 仍没有 → 403 */
                        cur = loc_query(r->cfg.db, b.aor, probe, 32, now);
                        dup = 0;
                        for (int i = 0; i < cur; ++i) {
                            if (is_dup_binding(&probe[i], &b)) { dup = 1; break; }
                        }
                        if (!dup && cur >= r->cfg.max_registrations_per_ext) {
                            char extra[128];
                            snprintf(extra, sizeof(extra),
                                     "Warning: 399 <%s>;cause=\"registration-limit-reached\";count=%d;max=%d\r\n",
                                     b.aor, cur, r->cfg.max_registrations_per_ext);
                            llog(LOG_WARN, "reg",
                                 "max_registrations_per_ext: evict race lost → 403 reject aor=%s cur=%d max=%d contact=%s",
                                 b.aor, cur, r->cfg.max_registrations_per_ext, b.contact);
                            respond(r, proto, src, m, 403, extra, NULL, 0, txn);
                            _rc = NSBC_OK; goto reg_out;
                        }
                    }
                    /* 踢成功（或 race 后 cur < max）— 继续往下 loc_upsert 新 contact */
                }
            }
        }

        llog(LOG_DEBUG, "reg",
             "loc_upsert-CALL user=%s aor=%s contact=%.80s expires_server=%ld "
             "nat=%d instance=%.60s callid=%.60s src_ip=%s transport=%d",
             _reg_user, b.aor, b.contact, exp, b.nat_detected,
             b.instance[0] ? b.instance : "(none)",
             b.callid[0]   ? b.callid   : "(none)",
             b.received_ip[0] ? b.received_ip : src_ip,
             b.transport_proto);

        loc_rc_t rc = loc_upsert(r->cfg.db, &b, now);
        const char *rc_name =
            rc == LOC_OK ? "OK" :
            rc == LOC_FULL ? "FULL" :
            rc == LOC_STALE_CSEQ ? "STALE_CSEQ" : "OTHER";
        /* 正常 LOC_OK 走 DEBUG（常规路径不吵）；FULL/STALE_CSEQ 是注册被拒信号 → WARN */
        if (rc == LOC_OK) {
            llog(LOG_DEBUG, "reg",
                 "loc_upsert-RETURN user=%s rc=OK aor=%s contact=%.80s",
                 _reg_user, b.aor, b.contact);
        } else {
            llog(LOG_WARN, "reg",
                 "loc_upsert-RETURN user=%s rc=%d(%s) aor=%s contact=%.80s "
                 "callid=%.60s cseq=%u expire_ms=%lu now=%lu",
                 _reg_user,
                 (int)rc, rc_name, b.aor, b.contact,
                 b.callid[0] ? b.callid : "(none)",
                 b.cseq, (unsigned long)b.expire_ms, (unsigned long)now);
        }
        if (rc == LOC_FULL) {
            char extra[64];
            snprintf(extra, sizeof(extra), "Retry-After: %d\r\n",
                     r->cfg.retry_after_s);
            respond(r, proto, src, m, 503, extra, NULL, 0, txn);
            _rc = NSBC_OK; goto reg_out;
        }
        if (rc != LOC_OK) {
            /* LOC_STALE_CSEQ = 同 Call-ID CSeq 回退，LOC_EXPIRED = upsert 时已过期 */
            llog(LOG_WARN, "reg",
                 "loc_upsert rc=%d skip aor=%s contact=%s callid=%s cseq=%u",
                 rc, b.aor, b.contact, b.callid, b.cseq);
            continue;  /* 跳过这个 Contact，不 reg_push（data store 不被污染） */
        }
        reg_push(&b, exp);  /* write-through: 注册成功（含 NAT 元数据） */

        /* === HA P1: ACTIVE → REPL_REG publish ===
         * loc_binding_t 是扁平无指针结构（所有固定数组），可直接 memcpy 为载荷。
         * payload_ver=1 —— 载荷结构版本，后续演进 bump 版本号 */
        {
            int64_t rc = ha_repl_publish(REPL_REG, 1, &b, sizeof(b));
            if (rc < 0 && rc != NSBC_NOSPC)
                llog(LOG_WARN, "reg",
                     "repl_publish REG failed rc=%lld aor=%s contact=%s",
                     (long long)rc, b.aor, b.contact);
        }

        ++nc;
    }
    if (!nc && !ch0) {
        /* 无 Contact 且非 *：查询口径（P0 直接 200 空列表） */
        respond(r, proto, src, m, 200,
                hdr_prefix[0] ? hdr_prefix : NULL, NULL, 0, txn);
        _rc = NSBC_OK; goto reg_out;
    }

    /* 200 回显当前绑定
     * ★ BUG-FIX: 必须带顶层 Expires: N 头（RFC 3261 §10.2 明确要求）。
     * 只在 Contact 里带 ;expires= 参数客户端不认——MicroSIP 等 UA
     * 不看 Contact 里的 expires 参数，只看顶层 Expires 头。
     * NAT 场景我们压短了 expires（如 300→120），但客户端按自己的
     * 默认 300s 刷新 → binding 在 120s 后过期 → 客户端发现注册失效
     * → 弹出"错误网关"。加上 Expires: 120 头告诉客户端新的有效期 →
     * 客户端每 60s（120/2）刷新一次 → NAT 映射持续活跃。 */
    loc_binding_t list[16];
    int n = loc_query(r->cfg.db, aor, list, 16, now);
    char extra[4096]; /* 扩大：Expires(32) + hdr_prefix(1024) + Contact 链(3072) */
    size_t used = 0;
    /* Service-Route + Path prepend */
    if (hdr_prefix[0]) {
        snprintf(extra, sizeof(extra), "%s", hdr_prefix);
        used = strlen(extra);
    }
    /* 顶层 Expires: N —— 取所有 binding 中最大的剩余有效期（RFC 3261 §10.2） */
    long max_e = 0;
    for (int i = 0; i < n; ++i) {
        long e = (long)((list[i].expire_ms - now) / 1000);
        if (e > max_e) max_e = e;
    }
    if (max_e > 0) {
        int w = snprintf(extra + used, sizeof(extra) - used,
                         "Expires: %ld\r\n", max_e);
        if (w > 0) used += (size_t)w;
    } else if (removed_n > 0) {
        /* 纯注销（当前无存活绑定，本次移除了 Contact）→ 顶层 Expires: 0 */
        int w = snprintf(extra + used, sizeof(extra) - used,
                         "Expires: 0\r\n");
        if (w > 0) used += (size_t)w;
    }
    for (int i = 0; i < n; ++i) {
        long e = (long)((list[i].expire_ms - now) / 1000);
        /* NAT: 200 OK Contact 用服务器认可的 public_contact（可达地址）
         * RFC 3261 §10.2：Server-Selected Contact；
         * NAT 场景 public_contact != b.contact；公网直连两者相同 */
        const char *show_contact =
            (list[i].nat_detected && list[i].public_contact[0])
            ? list[i].public_contact : list[i].contact;
        int w = snprintf(extra + used, sizeof(extra) - used,
                         "Contact: %s;expires=%ld\r\n",
                         show_contact, e > 0 ? e : 0);
        if (w < 0 || (size_t)w >= sizeof(extra) - used)
            break;
        used += (size_t)w;
    }
    /* 追加被移除的 Contact 回显（RFC 3261 §10.3.5：expires=0） */
    if (removed_n > 0 && removed_used > 0 &&
        used + removed_used < sizeof(extra)) {
        memcpy(extra + used, removed_ct, removed_used + 1);
        used += removed_used;
    }
    respond(r, proto, src, m, 200, extra[0] ? extra : NULL, NULL, 0, txn);

    /* ===== 诊断：200 OK 最终返回汇总 ===== */
    {
        const char *_role_s =
            ha_role() == HA_ROLE_ACTIVE ? "ACTIVE" : "STANDBY";
        llog(LOG_INFO, "reg",
             "REGISTER-200 user=%s aor=%s contacts_in_aor=%d expires_top=%ld "
             "role=%s nc_this_register=%d",
             _reg_user, aor, n, max_e, _role_s, nc);
    }
    _rc = NSBC_OK; goto reg_out;

    /* ================================================================
     * reg_out: 所有 exit 路径统一汇聚点
     *   - 计算 handler 耗时（CLOCK_MONOTONIC → 微秒）
     *   - LOG_DEBUG 输出（默认 nsbc.yaml info 级别不会打印，零开销）
     * ================================================================ */
reg_out:
    {
        struct timespec _t1;
        clock_gettime(CLOCK_MONOTONIC, &_t1);
        uint64_t _elapsed_us = (uint64_t)(_t1.tv_sec - _t0.tv_sec) * 1000000ULL
                             + (uint64_t)(_t1.tv_nsec - _t0.tv_nsec) / 1000ULL;
        /* aor 在 line 586 之后才计算；早期 exit 点用 "-" 占位 */
        llog(LOG_DEBUG, "reg-perf",
             "REGISTER handler exit aor=%s elapsed=%lu us",
             aor[0] ? aor : "-",
             (unsigned long)_elapsed_us);
    }
    return _rc;
}

uint64_t reg_total(reg_core_t *r)
{
    return r ? __sync_add_and_fetch(&r->n_total, 0) : 0;
}
uint64_t reg_rejected(reg_core_t *r)
{
    return r ? __sync_add_and_fetch(&r->n_rej, 0) : 0;
}
uint64_t reg_throttled(reg_core_t *r)
{
    return r ? __sync_add_and_fetch(&r->n_thr, 0) : 0;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
