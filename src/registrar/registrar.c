#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "registrar.h"
#include "b2bua.h"      /* b2b_guard_notify — 认证失败上报 security 封禁 */
#include "metric_reg.h"
#include "sip_build.h"
#include "sip_profile.h"
#include "sip_txn.h"
#include "uDS_client.h"
#include "log.h"
#include "net_sock.h"

/* signaling.c：从 AF_UNSPEC nsbc_sa_t padding 提取 ws_conn*
 * （WS/WSS REGISTER 的 src 藏有 conn 指针，见 sig_ws_bridge_rx） */
extern void *sig_ws_bridge_conn_from_sa(const nsbc_sa_t *sa);

#include <cjson/cJSON.h>
#include <arpa/inet.h>

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

struct reg_core {
    reg_cfg_t cfg;
    digest_cfg_t dig;
    tbucket_t ip[TB_IP_N];
    tbucket_t aor[TB_AOR_N];
    pthread_mutex_t glock;
    double gtokens;
    uint64_t glast_ms;
    uint64_t n_total, n_rej, n_thr;
};

static int tb_take(tbucket_t *b, uint32_t key, int cps, uint64_t now)
{
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
    pthread_mutex_lock(&r->glock);
    double add = (double)(now - r->glast_ms) * r->cfg.global_cps / 1000.0;
    r->gtokens += add;
    if (r->gtokens > r->cfg.global_cps)
        r->gtokens = r->cfg.global_cps;
    r->glast_ms = now;
    int ok = r->gtokens >= 1.0;
    if (ok)
        r->gtokens -= 1.0;
    pthread_mutex_unlock(&r->glock);
    return ok;
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

int reg_core_create(const reg_cfg_t *cfg, reg_core_t **out)
{
    reg_core_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NSBC_NOMEM;
    r->cfg = *cfg;
    if (r->cfg.min_expires <= 0)
        r->cfg.min_expires = 60;
    if (r->cfg.max_expires <= 0)
        r->cfg.max_expires = 3600;
    if (r->cfg.default_expires <= 0)
        r->cfg.default_expires = 3600;
    if (r->cfg.ip_cps <= 0)
        r->cfg.ip_cps = 50;
    if (r->cfg.aor_cps <= 0)
        r->cfg.aor_cps = 10;
    if (r->cfg.global_cps <= 0)
        r->cfg.global_cps = 100000;
    if (r->cfg.retry_after_s <= 0)
        r->cfg.retry_after_s = 30;
    if (digest_init(&r->dig, cfg->realm, cfg->digest_secret,
                cfg->nonce_ttl_ms ? cfg->nonce_ttl_ms : 300000,
                cfg->ha1_cb, cfg->ha1_ud) != NSBC_OK) {
        free(r);
        return NSBC_NOMEM;
    }
    pthread_mutex_init(&r->glock, NULL);
    for (int i = 0; i < TB_IP_N; ++i)
        pthread_mutex_init(&r->ip[i].lock, NULL);
    for (int i = 0; i < TB_AOR_N; ++i)
        pthread_mutex_init(&r->aor[i].lock, NULL);
    metric_source_register("nsbc_reg", "registration counters", r_reg, r);
    *out = r;
    return NSBC_OK;
}

void reg_core_destroy(reg_core_t *r)
{
    if (!r)
        return;
    for (int i = 0; i < TB_IP_N; ++i)
        pthread_mutex_destroy(&r->ip[i].lock);
    for (int i = 0; i < TB_AOR_N; ++i)
        pthread_mutex_destroy(&r->aor[i].lock);
    pthread_mutex_destroy(&r->glock);
    free(r);
}

static void respond(reg_core_t *r, sip_tr_proto_t proto, const nsbc_sa_t *dst,
                    const sip_msg_t *req, int status, const char *extra,
                    const char *body, size_t bodylen, sip_txn_t *txn)
{
    char out[SIP_MAX_MSG];
    int n = sip_build_response(out, sizeof(out), req, status, NULL, NULL,
                               NULL, extra, body, bodylen);
    if (n <= 0) return;
    /* 优先走 txn 层：自动缓存响应 + 吸收同 branch 重传 */
    if (txn) {
        txn_send_response(txn, out, (size_t)n, nsbc_now_ms());
    } else {
        sip_tr_send(r->cfg.tr, proto, out, (size_t)n, dst);
    }
}

/* ---- write-through: REGISTER 事件 → data.sock ---- */

/* REGISTER 状态 → JSON → uds_send_register（write-through）。
 * expires_sec=0 表示注销；>0 表示注册成功。
 * 非阻塞：UDS 不可用时静默丢，uds_client 内部 ring 满自动丢最旧。
 * NAT 字段（REQ-NAT-001）：received_ip/received_port/nat_detected/public_contact/transport
 *   由调用方从 loc_binding_t 传入，用于 Web registrations 表落库与前端展示。 */
static void reg_push(const loc_binding_t *b, long expires_sec)
{
    if (!b) return;
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

    /* P2-XX External profile REGISTER guard：
     * external profile (auth_calls=0) 设计用途是接 SIP 中继/DID 的 INVITE，
     * 不接受 USER-Agent 的 REGISTER（类似 FreeSWITCH public.xml）。
     * 命中 external profile 的 REGISTER 一律 silent drop，省掉后续 Digest 流程，
     * 也防止外部扫描通过 public profile 探测分机 HA1。 */
    {
        const sip_profile_t *prof = sip_profile_match_src(src_ip, NULL);
        if (prof && prof->auth_calls == 0) {
            llog(LOG_WARN, "reg",
                 "REGISTER silent-drop: external profile '%s' does not accept registrations, src=%s",
                 prof->name, src_ip);
            return NSBC_OK;
        }
    }

    /* AoR 取 To 头 */
    sip_hdr_t *toh = sip_hdr_get(m, SIP_H_TO);
    if (!toh || sip_hdr_parse_addr(toh) != NSBC_OK ||
        !toh->u.addr.uri.user.p) {
        respond(r, proto, src, m, 400, NULL, NULL, 0, txn);
        return NSBC_OK;
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

    /* ---- 从 REGISTER 请求 To 头里提取 realm（= URI host）----
     * 这是多租户的 tenant 标识，digest_verify 用它查 HA1，loc_add 也用它当 AoR host。
     * 之前错误地用 profile realm / 全局 cfg.realm → tenant 1004 注册 @a.com 但
     * HA1 查表用 realm=nsbc.local → user_unknown → 403。 */
    char reg_realm[64];
    if (u->host.p && u->host.len && u->host.len < sizeof(reg_realm)) {
        memcpy(reg_realm, u->host.p, u->host.len);
        reg_realm[u->host.len] = '\0';
    } else if (r->cfg.realm && r->cfg.realm[0]) {
        snprintf(reg_realm, sizeof(reg_realm), "%s", r->cfg.realm);
    } else {
        snprintf(reg_realm, sizeof(reg_realm), "nsbc.local");
    }
    llog(LOG_INFO, "reg", "REGISTER aor=%s tenant_realm=%s", aor, reg_realm);

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
        return NSBC_OK;
    }

    /* Digest 鉴权 —— realm 用 REGISTER 请求 To 头的 URI host（reg_realm），
     * 不用 profile realm / 全局 cfg.realm —— 租户标识来自请求本身。
     * 这保证 1004@a.com 注册时 HA1 查表用 realm=a.com，与 Web DB 里存的 realm 对齐。 */
    digest_cfg_t dig = r->dig;
    dig.realm = reg_realm;

    /* Digest 鉴权 */
    sip_hdr_t *auth = sip_hdr_get(m, SIP_H_AUTHORIZATION);
    llog(LOG_INFO, "reg", "REGISTER Authorization hdr ptr=%p", (void*)auth);
    if (!auth || sip_hdr_parse_auth(auth) != NSBC_OK) {
        char ch[512], extra[600];
        digest_make_challenge(&dig, 0, ch, sizeof(ch));
        snprintf(extra, sizeof(extra), "WWW-Authenticate: %s\r\n", ch);
        llog(LOG_INFO, "reg", "REGISTER → 401 challenge (auth=%p)", (void*)auth);
        llog(LOG_WARN, "reg",
             "key=fail2ban src_ip=%s reason=no_auth_header aor=%s",
             src_ip, aor);
        respond(r, proto, src, m, 401, extra, NULL, 0, txn);
        return NSBC_OK;
    }
    llog(LOG_INFO, "reg", "REGISTER with Authorization header → verifying...");
    dig_auth_rc_t ar = digest_verify(&dig, m, &auth->u.auth, now);
    llog(LOG_INFO, "reg", "digest_verify rc=%d (0=OK)", ar);
    if (ar == DIG_AUTH_STALE_NONCE) {
        char ch[512], extra[600];
        digest_make_challenge(&dig, 1, ch, sizeof(ch));
        snprintf(extra, sizeof(extra), "WWW-Authenticate: %s\r\n", ch);
        llog(LOG_INFO, "reg", "REGISTER → 401 stale nonce");
        respond(r, proto, src, m, 401, extra, NULL, 0, txn);
        return NSBC_OK;
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
        return NSBC_OK;
    }
    llog(LOG_INFO, "reg", "REGISTER auth OK → processing loc_add...");
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
            llog(LOG_INFO, "reg", "REGISTER Path echo: %s", path_extra);
        }
    }

    /* ---- RFC 3608：Service-Route 生成 + 入站回显 ----
     * 我们作为 registrar/CSCF 生成 Service-Route 告诉 UE 后续
     * INVITE/SUBSCRIBE 等请求应该用什么 Route 头来到达我们。
     * UE 拿到后自动把 Service-Route 值拷贝到后续请求的 Route 头。
     * 格式："Service-Route: <sip:{tenant_realm};lr>\r\n"
     *   - lr = loose router（RFC 3608 §4.4 强制）
     * 同时请求里如果已经带了 Service-Route（outreg 上游注入），也原样回显。 */
    char generated_service_route[256]; generated_service_route[0] = '\0';
    {
        /* 1. 先生成本地 Service-Route（registrar 角色） */
        snprintf(generated_service_route, sizeof(generated_service_route),
                 "<sip:%s;lr>", reg_realm);
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
            llog(LOG_INFO, "reg", "REGISTER Service-Route generated: %s",
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
    llog(LOG_INFO, "reg", "loc_add: Contact hdr ptr=%p star=%d val_len=%zd",
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
        return NSBC_OK;
    }

    /* 逐 Contact 处理 */
    int nc = 0;
    for (sip_hdr_t *h = ch0; h; h = sip_hdr_next(m, h)) {
        llog(LOG_INFO, "reg", "loc_add loop: h=%p type=%d", (void*)h, h->type);
        if (h->type != SIP_H_CONTACT)
            continue;
        int prc = sip_hdr_parse_addr(h);
        llog(LOG_INFO, "reg", "loc_add: parse_addr rc=%d star=%d val_len=%zd val=%.100s", prc,
             prc == NSBC_OK ? (int)h->u.addr.star : -1,
             h->value.len, h->value.p ? h->value.p : "(null)");
        if (prc != NSBC_OK)
            continue;
        long exp = param_num(h->value.p, h->value.len, "expires");
        if (exp < 0)
            exp = def_exp;
        if (exp < r->cfg.min_expires && exp != 0) {
            char extra[64];
            snprintf(extra, sizeof(extra), "Min-Expires: %d\r\n",
                     r->cfg.min_expires);
            respond(r, proto, src, m, 423, extra, NULL, 0, txn);
            return NSBC_OK;
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
            llog(LOG_INFO, "reg", "NAT detected contact=%s public_contact=%s src=%s:%u via_recv=%s",
                 b.contact, b.public_contact, src_ip, src_port, via_recv);
        } else {
            /* 公网直连（含 WS/WSS）：public_contact = 原始 Contact */
            snprintf(b.public_contact, sizeof(b.public_contact), "%s", b.contact);
            llog(LOG_INFO, "reg", "no NAT contact=%s src=%s:%u", b.contact, src_ip, src_port);
        }

        llog(LOG_INFO, "reg", "loc_upsert call: aor=%s contact=%s public=%s nat=%d expire_ms=%lu exp=%ld now=%lu",
             b.aor, b.contact, b.public_contact, b.nat_detected,
             (unsigned long)b.expire_ms, exp, (unsigned long)now);
        loc_rc_t rc = loc_upsert(r->cfg.db, &b, now);
        if (rc == LOC_FULL) {
            char extra[64];
            snprintf(extra, sizeof(extra), "Retry-After: %d\r\n",
                     r->cfg.retry_after_s);
            respond(r, proto, src, m, 503, extra, NULL, 0, txn);
            return NSBC_OK;
        }
        if (rc != LOC_OK) {
            /* LOC_STALE_CSEQ = 同 Call-ID CSeq 回退，LOC_EXPIRED = upsert 时已过期 */
            llog(LOG_WARN, "reg",
                 "loc_upsert rc=%d skip aor=%s contact=%s callid=%s cseq=%u",
                 rc, b.aor, b.contact, b.callid, b.cseq);
            continue;  /* 跳过这个 Contact，不 reg_push（data store 不被污染） */
        }
        reg_push(&b, exp);  /* write-through: 注册成功（含 NAT 元数据） */
        ++nc;
    }
    if (!nc && !ch0) {
        /* 无 Contact 且非 *：查询口径（P0 直接 200 空列表） */
        respond(r, proto, src, m, 200,
                hdr_prefix[0] ? hdr_prefix : NULL, NULL, 0, txn);
        return NSBC_OK;
    }

    /* 200 回显当前绑定 */
    loc_binding_t list[16];
    int n = loc_query(r->cfg.db, aor, list, 16, now);
    char extra[4096]; /* 扩大：hdr_prefix(1024) + Contact 链(3072) */
    size_t used = 0;
    /* Service-Route + Path prepend */
    if (hdr_prefix[0]) {
        snprintf(extra, sizeof(extra), "%s", hdr_prefix);
        used = strlen(extra);
    }
    for (int i = 0; i < n; ++i) {
        long e = (long)((list[i].expire_ms - now) / 1000);
        int w = snprintf(extra + used, sizeof(extra) - used,
                         "Contact: %s;expires=%ld\r\n",
                         list[i].contact, e > 0 ? e : 0);
        if (w < 0 || (size_t)w >= sizeof(extra) - used)
            break;
        used += (size_t)w;
    }
    respond(r, proto, src, m, 200, extra[0] ? extra : NULL, NULL, 0, txn);
    return NSBC_OK;
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
