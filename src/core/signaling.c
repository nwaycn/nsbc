/* 信令面装配模块（REQ-ARCH-010/011，P0）
 *
 * 唯一的呼叫面接线点：transport → txn → b2bua → registrar/media bridge，
 * 全部在本文件按模块描述符装配；其他文件只实现各自单层，不横向互链。
 * 依赖（启动拓扑）：endpoints（HA1 凭证）→ signaling。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "app_ctx.h"
#include "b2bua.h"
#include "cdr_event.h"
#include "cfg.h"
#include "dialplan.h"
#include "endpoint_registry.h"  /* Phase 5 XL: SIP endpoint ops 注册 */
#include "endpoints.h"
#include "location_db.h"
#include "log.h"
#include "media_bridge.h"
#include "obs.h"           /* CALL_END 事件经 obs_event 同时写 ring + 推订阅 */
#include "module.h"
#include "outbound_reg.h"
#include "port_table.h"
#include "recording.h"
#include "realtime.h"
#include "registrar.h"
#include "route_table.h"
#include "queue.h"       /* REQ-PBX-CC-002：ACD 队列查询（cq_get） */
#include "sip_dialog.h"
#include "sip_profile.h"
#include "sip_transport.h"
#include "sip_txn.h"
#include "sdp.h"          /* SDP parse for CDR codec + peer IP extraction */
#include "stun_server.h"
#include "tls_io.h"
#include "ws_listener.h"
#include "cdr_event.h"

#include <ctype.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#define SWEEP_INTERVAL_MS 60000
#define URI_MAX 512

/* 前向声明：定义在本文件尾部（sig_ws_bridge_conn_from_sa 旁），
 * resolve_cb（前部）用它把被叫路由到 WS 注册连接 */
static int sa_hide_ws_conn(nsbc_sa_t *sa, void *conn);

/* CDR END 时从 answer SDP 提取 codec 名 + peer IP:port。
 * 返回 0=ok（两个 out 字段都填了），非 0 表示没找到/解析失败——调用方把 out 当空字符串。 */
static int sig_sdp_extract_cdr(const char *sdp_buf, size_t sdp_len,
                                char *codec_out, size_t codec_sz,
                                char *peer_out,  size_t peer_sz);

typedef struct sig_runtime {
    nsbc_ctx_t *ctx;
    ptable_t *ptable;
    bridge_mgr_t *bridge;
    loc_db_t *loc;
    dlg_mgr_t *dlg;
    txn_mgr_t *txn;
    b2b_core_t *b2b;
    sip_transport_t *tr;
    reg_core_t *reg;
    tls_ctx_t *tls;
    dp_ctx_t dp_ctx; /* Dialplan 规则集（热加载） */
    tw_node_t sweep_node;
    char local_host[128]; /* 监听/SDP 广告地址 */
    char realm[128];
} sig_runtime_t;

/* ------------------------------------------------------------------ */
/* 事务层发送口 → transport                                            */
/* ------------------------------------------------------------------ */
static int tr_send_cb(void *ud, const char *buf, size_t len,
                      const nsbc_sa_t *dst, int reliable)
{
    sig_runtime_t *s = (sig_runtime_t *)ud;
    if (!s->tr)
        return NSBC_ERR;
    int fam = dst ? dst->ss.ss_family : -1;
    llog(LOG_INFO, "txn-send-cb", "len=%zu fam=%d reliable=%d dst_p=%p",
         len, fam, reliable, (const void *)dst);
    /* WS/WSS：dst.ss_family == AF_UNSPEC 是 ws bridge 约定（藏 ws_conn*）。
     * 不能走 TCP/UDP 分支 — 那些按 IP:port 匹配连接，AF_UNSPEC 匹配不上。
     * 用全局 g_ws_bridge_proto 判断（WS vs WSS 由配置决定）。 */
    if (fam == AF_UNSPEC) {
        extern sip_tr_proto_t g_ws_bridge_proto;  /* sig_ws_bridge_init 设 */
        llog(LOG_INFO, "txn-send-cb", "→ WS/WSS branch proto=%d", (int)g_ws_bridge_proto);
        return sip_tr_send(s->tr, g_ws_bridge_proto, buf, len, dst);
    }
    /* P0：TCP/TLS 仅复用入向连接（sip_tr_send 内部匹配），UDP 直发 */
    llog(LOG_INFO, "txn-send-cb", "→ TCP/UDP branch reliable=%d", reliable);
    return sip_tr_send(s->tr, reliable ? SIP_TR_TCP : SIP_TR_UDP, buf, len,
                       dst);
}

/* ------------------------------------------------------------------ */
/* 号码/目的解析（REQ-SIP 路由）：本域分机查 Location DB，域外按字面量  */
/* ------------------------------------------------------------------ */

/* 从 "sip:user@host:port;params"/"<...>" 中取 host/port；
 * 返回 host 起点，hlen 为主机长度，*port 为端口（缺省 5060）。 */
static const char *uri_hostport(const char *p, size_t len, size_t *hlen,
                                uint16_t *port, int *is_ip6)
{
    *is_ip6 = 0;
    *port = 5060;
    *hlen = 0;
    /* 去前导空白与 '<' */
    while (len && (*p == ' ' || *p == '\t' || *p == '<')) {
        ++p;
        --len;
    }
    while (len && (p[len - 1] == ' ' || p[len - 1] == '\t' ||
                   p[len - 1] == '>'))
        --len;
    /* 跳 scheme */
    if (len >= 4 && strncasecmp(p, "sip:", 4) == 0) {
        p += 4;
        len -= 4;
    } else if (len >= 5 && strncasecmp(p, "sips:", 5) == 0) {
        p += 5;
        len -= 5;
        *port = 5061;
    }
    /* 截到首个 ;/? 之前 */
    size_t seg = len;
    for (size_t i = 0; i < len; ++i) {
        if (p[i] == ';' || p[i] == '?') {
            seg = i;
            break;
        }
    }
    /* userinfo：最后一个 '@' 之前 */
    size_t hs = 0;
    for (size_t i = 0; i < seg; ++i) {
        if (p[i] == '@')
            hs = i + 1;
    }
    const char *h = p + hs;
    size_t hl = seg - hs;
    if (hl == 0)
        return NULL;
    if (*h == '[') {
        /* [ipv6] 或 [ipv6]:port */
        *is_ip6 = 1;
        ++h;
        --hl;
        size_t i;
        for (i = 0; i < hl && h[i] != ']'; ++i) {
        }
        if (i == hl)
            return NULL;
        *hlen = i;
        if (i + 1 < hl && h[i + 1] == ':')
            *port = (uint16_t)atoi(h + i + 2);
        return h;
    }
    size_t colon = hl;
    for (size_t i = 0; i < hl; ++i) {
        if (h[i] == ':') {
            colon = i;
            break;
        }
    }
    if (colon < hl)
        *port = (uint16_t)atoi(h + colon + 1);
    *hlen = colon;
    return h;
}

static int uri_to_sa(const char *uri, size_t len, nsbc_sa_t *dst)
{
    size_t hlen = 0;
    uint16_t port = 5060;
    int ip6 = 0;
    const char *h = uri_hostport(uri, len, &hlen, &port, &ip6);
    if (!h || hlen == 0)
        return NSBC_INVAL;
    char host[128];
    if (hlen >= sizeof(host))
        return NSBC_INVAL;
    memcpy(host, h, hlen);
    host[hlen] = '\0';
    return nsbc_sa_from(dst, host, port, ip6 ? AF_INET6 : AF_UNSPEC);
}

/* binding 的 name-addr contact（<sip:...>）→ 裸 R-URI：剥最外层尖括号。
 * Request-URI 语法（RFC 3261 §25.1 Request-Line）不允许 name-addr 的 <>，
 * 严格 UA（SIP.js Grammar）解析首行失败直接丢弃整条 INVITE。
 * URI 本体参数（;transport=ws 等）原样保留。 */
static void ruri_from_contact(const char *contact, char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    out[0] = '\0';
    if (!contact)
        return;
    const char *p = contact;
    size_t n = strlen(p);
    if (n >= 2 && p[0] == '<' && p[n - 1] == '>') {
        ++p;
        n -= 2;
    }
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Phase 0: rec_feed_wrapper 已删除 —— recording 模块 init 时通过
 * bridge_add_rec_feed_hook 自注册 recording_rec_feed 回调（src/mod/recording/recording.c），
 * 零跨模块依赖，模块间完全解耦。*/

/* REQ-SIP-PROFILE-001：per-call RTP 端口段——relay 建立前 media 层经此
 * 回调查询（组合根解引用 call_key；media 层只取值，ARCH-011）。
 * 呼叫归属 profile（handle_new_invite 按主叫源 IP 匹配，SIP 线程写、
 * 这里 SIP 线程读——offer_hook 同线程调用，无跨线程可见性问题）→
 * 返回该 profile 的 rtp_port_start/end；无归属 (0,0)=表默认段。 */
static void sig_call_port_range(void *ud, void *call_key,
                                uint16_t *lo, uint16_t *hi)
{
    (void)ud;
    *lo = 0;
    *hi = 0;
    b2b_call_t *c = (b2b_call_t *)call_key;
    if (c && c->prof) {
        *lo = c->prof->rtp_port_start;
        *hi = c->prof->rtp_port_end;
    }
}

/* REQ-DTLS-001：B 腿是否浏览器 WebRTC（WS/WSS 注册）。
 * offer_hook 同线程调用（SIP 线程读 leg[1]，同 sig_call_port_range 口径）。
 * MicrosIP→WebRTC 方向 offer 是纯 SDP，bridge_decide 判不出 WebRTC，
 * 只能靠 leg[1].proto（WS/WSS）或 ruri transport=ws 判定 → SRTP_RELAY。 */
static int sig_call_b_is_ws(void *ud, void *call_key)
{
    (void)ud;
    b2b_call_t *c = (b2b_call_t *)call_key;
    if (!c)
        return 0;
    if (c->leg[1].proto == SIP_TR_WS || c->leg[1].proto == SIP_TR_WSS)
        return 1;
    return c->leg[1].ruri[0] &&
           (strstr(c->leg[1].ruri, "transport=ws") != NULL ||
            strstr(c->leg[1].ruri, "transport=wss") != NULL);
}

/* REQ-MEDIA-DP-001：拨号规则级媒体模式 override 查询
 * 从 b2b_call_t.media_mode 透传（0=auto, 1=bypass, 2=relay）。
 * media_bridge 在 bridge_offer_hook 里调，WebRTC 场景会忽略 bypass。 */
static uint8_t sig_call_media_mode(void *ud, void *call_key)
{
    (void)ud;
    b2b_call_t *c = (b2b_call_t *)call_key;
    if (!c)
        return 0;
    return c->media_mode;
}

/* REQ-NAT-002：per-call NAT 公网 RTP IP — 从 b2b_call_t.prof 反查 sip_profile_t。
 * profile.reload 热更新后自动生效。profile 没配 external_rtp_ip（空串）→ 返回 NULL →
 * media_bridge 用 local_ip 做 SDP rewrite fallback（不做 NAT）。 */
static const char *sig_call_external_rtp_ip(void *ud, void *call_key)
{
    (void)ud;
    b2b_call_t *c = (b2b_call_t *)call_key;
    if (!c) {
        llog(LOG_INFO, "media", "EXT_RTP: call=NULL");
        return NULL;
    }
    if (!c->prof) {
        llog(LOG_INFO, "media", "EXT_RTP: call=%p prof=NULL", c);
        return NULL;
    }
    llog(LOG_INFO, "media", "EXT_RTP: call=%p prof=[%s] sip=[%s] rtp=[%s]",
         c, c->prof->name,
         c->prof->external_sip_ip, c->prof->external_rtp_ip);
    if (!c->prof->external_rtp_ip[0])
        return NULL;
    return c->prof->external_rtp_ip;
}

/* REQ-NAT-002：per-call NAT 公网 SIP IP（Via/Contact/Record-Route）——同理。 */
static __attribute__((unused)) const char *sig_call_external_sip_ip(void *ud, void *call_key)
{
    (void)ud;
    b2b_call_t *c = (b2b_call_t *)call_key;
    if (!c || !c->prof || !c->prof->external_sip_ip[0])
        return NULL;
    return c->prof->external_sip_ip;
}

/* ==================================================================
 * Phase 5 XL: SIP 协议 endpoint ops — endpoint_registry 分发入口
 *
 * b2b_server_tu INVITE 分支不再硬编码调 handle_new_invite，
 * 而是先 endpoint_registry_try_accept — 找到 accept OK 的 endpoint 就委托。
 * SIP endpoint 永远 accept，handle_invite 内部解包 sip_invite_ctx_t 然后调 handle_new_invite。
 *
 * 未来扩展：ai_endpoint 注册 accept RURI user="ai" 的 INVITE 给 AI 处理；
 * SIP endpoint accept 里可加分流逻辑跳过 ai 前缀。当前版本只 SIP 注册。 */

static int sip_ep_accept(sip_msg_t *m, void **ctx)
{
    if (!m) return NSBC_NOENT;
    *ctx = NULL;  /* 不造私有 ctx，b2b_server_tu 会自己塞 sip_invite_ctx_t */
    return NSBC_OK;  /* 所有 INVITE SIP endpoint 都处理 */
}

static int sip_ep_handle_invite(void *ctx, b2b_call_t *c)
{
    (void)c;  /* call 还没创建，handle_new_invite 内部会创建 */
    sip_invite_ctx_t *ictx = (sip_invite_ctx_t *)ctx;
    if (!ictx || !ictx->co || !ictx->t || !ictx->m) return NSBC_INVAL;

    llog(LOG_INFO, "sig", "Phase 5 XL: sip_endpoint handle_invite → handle_new_invite");
    handle_new_invite(ictx->co, ictx->t, ictx->m,
                      ictx->proto, ictx->src, ictx->local_port);
    return NSBC_OK;
}

static void sip_ep_handle_destroy(void *ctx, int reason)
{
    (void)ctx; (void)reason;
    /* SIP endpoint 不维护私有 ctx — call 的创建/销毁全在 b2b_core 内部 */
}

static const endpoint_ops_t sip_endpoint_ops = {
    .name          = "sip",
    .accept        = sip_ep_accept,
    .handle_invite = sip_ep_handle_invite,
    .handle_destroy = sip_ep_handle_destroy,
};

/* ================================================================== */

static int resolve_cb(b2b_call_t *call, const char *ruri, size_t ruri_len,
                      nsbc_sa_t *dst, char *out_ruri, size_t outsz,
                      char *out_to, size_t out_to_sz, void *ud)
{
    (void)call;
    sig_runtime_t *s = (sig_runtime_t *)ud;

    /* ---- 302 follow 清理：旧 OUT 层 trunk rt_release ----
     * resolve_cb 可能被多次调用（初始 INVITE + 302 follow）。
     * 如果 call->rt_name 已有值（旧 OUT 层 rt_acquire 占了槽），
     * 先 rt_release 再继续新 resolve 路径——否则旧 trunk in_use 泄漏。 */
    if (call && call->rt_name[0] && s->ctx && s->ctx->rt) {
        rt_release(s->ctx->rt, call->rt_name);
        call->rt_name[0] = '\0';
    }

    /* 解析 R-URI 的 user/host 段 */
    char buf[URI_MAX];
    if (ruri_len >= sizeof(buf))
        ruri_len = sizeof(buf) - 1;
    memcpy(buf, ruri, ruri_len);
    buf[ruri_len] = '\0';
    size_t hlen2 = 0;
    uint16_t ignore_port = 0;
    int ip6x = 0;
    const char *h = uri_hostport(buf, ruri_len, &hlen2, &ignore_port, &ip6x);
    if (!h) {
        llog(LOG_WARN, "sig", "resolve: no host in R-URI '%s' → 404", buf);
        return NSBC_NOENT;
    }
    /* 回推 user 起点：scheme 之后到 '@' */
    const char *p = buf;
    size_t plen = ruri_len;
    if (plen >= 4 && strncasecmp(p, "sip:", 4) == 0)
        p += 4;
    else if (plen >= 5 && strncasecmp(p, "sips:", 5) == 0)
        p += 5;
    const char *at = NULL;
    for (const char *q = p; q < h; ++q) {
        if (*q == '@')
            at = q;
    }
    if (!at) {
        /* ---- FS 风格 call-url fallback ----
         * originate 命令传 FreeSWITCH 格式 `user/1001`（/ 分隔，无 @）。
         * uri_hostport 会把整个字符串当 host 返回（因为没有 @ 分隔），
         * 所以走到这里"无 user part"。这里把它重写成 SIP URI 格式
         * `1001@local_host`，然后重走整个 resolve 路径。
         * 支持格式：
         *   user/<number>         → <number>@local_host   （内部注册分机）
         *   其他未知格式 → 原逻辑 NOENT */
        char *slash = strchr(buf, '/');
        if (slash && (slash - buf == 4) && !strncmp(buf, "user", 4)) {
            /* buf = "user/1001" → 重写为 "1001@local_host" */
            const char *fs_number = slash + 1;       /* "1001" */
            size_t fs_number_len = strlen(fs_number);
            const char *fs_host = (s && s->local_host[0]) ? s->local_host
                                 : (s && s->realm[0]) ? s->realm
                                                       : "127.0.0.1";
            size_t fs_host_len = strlen(fs_host);
            size_t new_len = fs_number_len + 1 /* @ */ + fs_host_len;
            if (new_len < sizeof(buf)) {
                char *dst = buf;
                memcpy(dst, fs_number, fs_number_len); dst += fs_number_len;
                *dst++ = '@';
                memcpy(dst, fs_host, fs_host_len); dst += fs_host_len;
                *dst = '\0';
                ruri_len = new_len;
                llog(LOG_INFO, "sig", "resolve: FS call-url rewritten → '%s' (local_host=%s)",
                     buf, fs_host);
                /* 重走：h / p / at 全部重新计算 */
                h = uri_hostport(buf, ruri_len, &hlen2, &ignore_port, &ip6x);
                if (!h) {
                    llog(LOG_WARN, "sig", "resolve: FS rewrite failed, still no host → 404");
                    return NSBC_NOENT;
                }
                p = buf;
                plen = ruri_len;
                if (plen >= 4 && strncasecmp(p, "sip:", 4) == 0)
                    p += 4;
                else if (plen >= 5 && strncasecmp(p, "sips:", 5) == 0)
                    p += 5;
                at = NULL;
                for (const char *q = p; q < h; ++q) {
                    if (*q == '@')
                        at = q;
                }
                if (!at) {
                    llog(LOG_WARN, "sig", "resolve: FS rewrite failed, still no @ → 404");
                    return NSBC_NOENT;
                }
                /* 继续走下面的 user/rhost/in_domain/DB 查询逻辑 */
            } else {
                llog(LOG_WARN, "sig", "resolve: FS rewrite buffer overflow → 404");
                return NSBC_NOENT;
            }
        } else {
            llog(LOG_WARN, "sig", "resolve: no user part in R-URI '%s' → 404", buf);
            return NSBC_NOENT;
        }
    }
    char user[128];
    size_t ulen = (size_t)(at - p);
    if (ulen >= sizeof(user))
        return NSBC_NOENT;
    memcpy(user, p, ulen);
    user[ulen] = '\0';
    char rhost[128];
    if (hlen2 >= sizeof(rhost))
        return NSBC_NOENT;
    memcpy(rhost, h, hlen2);
    rhost[hlen2] = '\0';

    /* 本域？ host 命中监听地址或 realm 时查 Location DB */
    int in_domain = (strcmp(rhost, s->realm) == 0 ||
                     strcmp(rhost, s->local_host) == 0 ||
                     strcmp(rhost, "localhost") == 0 ||
                     strcmp(rhost, "127.0.0.1") == 0);

    /* ---- 先过 Dialplan（P0 规则表）----
     * 命中 EXT 层：dst_number 为变换后号码，target 为分机号；
     * 命中 OUT/BIZ 层：暂不支持（返回 NOENT 让上层 404）；
     * 未命中：走原有 Location DB / 字面量路径。 */
    /* §15：dp_call_t 必须填来源 profile + 源 IP
     * profile_name 优先级链：
     *   1) call->prof 已有值（INVITE 入口 sip_profile_match_src 识别）→ prof->name
     *   2) NULL → fallback "internal"（无 ACL 配置时的安全默认）
     * src_ip：从 A-leg dst 拿 sockaddr → inet_ntop 转字符串 */
    char src_ip_buf[INET6_ADDRSTRLEN] = {0};
    const char *prof_name =
        (call && call->prof && call->prof->name[0]) ? call->prof->name : "internal";
    /* 从 sockaddr 提取 IP 字符串（仅 INVITE 入口有 call） */
    if (call) {
        const struct sockaddr *sa = (const struct sockaddr *)&call->leg[0].dst;
        if (sa->sa_family == AF_INET) {
            inet_ntop(AF_INET, &((const struct sockaddr_in *)sa)->sin_addr,
                      src_ip_buf, sizeof(src_ip_buf));
        } else if (sa->sa_family == AF_INET6) {
            inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)sa)->sin6_addr,
                      src_ip_buf, sizeof(src_ip_buf));
        }
        /* AF_UNSPEC（WS 桥）→ src_ip_buf 保持空串，跳过 CIDR 检查 */
    }

    dp_call_t dcall;
    memset(&dcall, 0, sizeof(dcall));
    dcall.called = user;
    /* REQ-REC-007：主叫号从 A-leg From 头提取（"sip:user@host" 里的 user part）。
     * resolve_cb 匹配的是 INVITE Request-URI → user 是被叫；
     * From 头是来话方主叫。模板变量 {caller} 全靠这个！ */
    char caller_buf[64] = {0};
    if (call && call->leg[0].from[0]) {
        const char *from = call->leg[0].from;
        const char *colon = strchr(from, ':');
        if (colon) {
            const char *at = strchr(colon + 1, '@');
            size_t ulen = at ? (size_t)(at - colon - 1) : strlen(colon + 1);
            if (ulen >= sizeof(caller_buf)) ulen = sizeof(caller_buf) - 1;
            memcpy(caller_buf, colon + 1, ulen);
            caller_buf[ulen] = '\0';
        }
    }
    dcall.caller = caller_buf[0] ? caller_buf : "";
    dcall.perm = 0;
    dcall.profile_name = prof_name;
    dcall.src_ip = src_ip_buf[0] ? src_ip_buf : NULL;
    dcall.call_key = call;    /* REQ-DP-APP-001：传 b2b_call_t* 给 app_chain 执行上下文 */

    dp_result_t dres;
    int dp_rc = dp_execute(&s->dp_ctx, &dcall, &dres);
    int dp_ok = dp_rc == NSBC_OK;
    int dp_layer = dp_ok ? (int)dres.layer : -1;

    /* ---- 第一优先级：rule 级 CIDR reject + dp ACL perm 不足 ----
     * dp_execute 内部遍历 rule 时先 profile 匹配、再 rule 内嵌 CIDR 白名单、
     * 再 pattern match；全部 rule 被 CIDR reject 或 perm 不足 → PERM_DENIED
     * 这里先 return —— 未知 IP 连 resolve 后续都不该进（caller 分机校验也不跑）。*/
    if (dp_rc == NSBC_PERM_DENIED) {
        llog(LOG_NOTICE, "sig",
             "dp PERM_DENIED user=%s rhost=%s perm=%d → silent drop",
             user, rhost, dcall.perm);
        return NSBC_PERM_DENIED;
    }

    /* ---- 第二优先级：internal profile 主叫分机合法性 ----
     * 只在 CIDR 放行之后（第一优先级）才跑。CIDR 已拦掉陌生源，剩下的
     * 内部分机必须是 endpoints 表里注册过的有效分机（防未注册分机盗用
     * trunk 出中继）。external profile 主叫由运营商保证，跳过。
     * 校验失败同样 PERM_DENIED → b2bua silent drop，与 CIDR 拦截保持一致。
     * caller 为空（From 头解析异常）也当作非法 → PERM_DENIED，防伪造主叫。
     *
     * ★ realm 用当前 INVITE R-URI 的 host（rhost）—— 这才是租户标识。
     * 1004 注册在 realm=a.com，INVITE To=1002@a.com → rhost=a.com
     * → ep_lookup(1001, "a.com") 命中。之前错误用 s->realm（全局 nsbc.local）→ miss。
     * 兜底：如果 rhost 是 IP / localhost 等非租户 host，回退 s->realm。 */
    const char *check_realm = rhost;
    if (!rhost[0] || strcmp(rhost, "localhost") == 0 ||
        strcmp(rhost, "127.0.0.1") == 0)
        check_realm = s->realm;
    if (strcmp(prof_name, "internal") == 0) {
        /* call==NULL 是 originate（OAM/API 发起）——caller 由大括号块显式指定，
         * 不依赖 SIP From 头，跳过 endpoints 合法性校验与活跃注册检查。 */
        if (call) {
            if (!caller_buf[0]) {
                llog(LOG_NOTICE, "sig",
                     "caller VALIDATION_FAIL caller=(empty) realm=%s profile=internal → PERM_DENIED (silent drop)",
                     check_realm);
                return NSBC_PERM_DENIED;
            }
            const struct ep_ext *ep = ep_lookup(caller_buf, check_realm);
            if (!ep) {
                llog(LOG_NOTICE, "sig",
                     "caller VALIDATION_FAIL caller=%s realm=%s profile=internal → PERM_DENIED (silent drop)",
                     caller_buf, check_realm);
                return NSBC_PERM_DENIED;
            }
            /* ★ 2026-10-17：internal profile 主叫必须有活跃注册。
             * nsbcd 重启后 location 表清空，但 endpoints 表还在 → 未注册分机
             * 也能呼叫。loc_query 返回 0 条活跃绑定 = 未注册 → PERM_DENIED。
             * external profile (auth_calls=0) 跳过——运营商保证合法性。 */
            if (s->loc) {
                char caller_aor[LOC_AOR_MAX];
                snprintf(caller_aor, sizeof(caller_aor), "%s@%s",
                         caller_buf, check_realm);
                loc_binding_t dummy;
                int n = loc_query(s->loc, caller_aor, &dummy, 1, nsbc_now_ms());
                if (n == 0) {
                    llog(LOG_NOTICE, "sig",
                         "caller NOT_REGISTERED caller=%s realm=%s aor=%s profile=internal → PERM_DENIED (silent drop)",
                         caller_buf, check_realm, caller_aor);
                    return NSBC_PERM_DENIED;
                }
            }
        }
    }

    llog(LOG_INFO, "sig", "resolve: user=%s rhost=%s prof=%s src_ip=%s in_domain=%d dp_hit=%d layer=%d target=%s",
         user, rhost, prof_name,
         dcall.src_ip ? dcall.src_ip : "-",
         in_domain, dp_ok, dp_layer,
         dp_ok ? dres.target : "(none)");
    /* REQ-REC-003：dialplan 命中 recording action → 启停录音会话
     * 幂等：recording_apply_dp_action 内部对已 start / 已 stop 都是 no-op，
     * 所以 resolve_cb 被多次调用（初始 INVITE + 302 follow）也安全 */
    if (dp_ok && dres.rec_action != 0 && s->ctx->rec_mgr && call) {
        recording_apply_dp_action(s->ctx->rec_mgr, dres.rec_action,
                                  call->leg[0].callid, "dialplan",
                                  dres.rec_path_template,
                                  dcall.caller, dcall.called,
                                  NULL, 0);
        /* REQ-REC-006：start 成功后反查两个分腿 sess → 存 call->rec[0] 和 call->rec[1]
         * bridge.rec_feed_fn wrapper 从 call_key 拿 call->rec[leg] 喂对应 sess。
         * Advisor 2026-10-01l：__atomic_store_n(__ATOMIC_RELEASE) 配对
         * rec_feed_wrapper 的 __ATOMIC_ACQUIRE，保证跨线程可见。 */
        if (dres.rec_action == 1 && call->leg[0].callid[0]) {
            for (int leg = 0; leg < 2; ++leg) {
                rec_sess_t *sess = recording_find_by_callid(call->leg[0].callid, leg);
                if (sess) {
                    __atomic_store_n((void **)&call->rec[leg], sess, __ATOMIC_RELEASE);
                    llog(LOG_INFO, "sig", "rec bind leg=%d sess=%p callid=%s", leg, sess, call->leg[0].callid);
                }
            }
        }
    }
    /* REQ-PBX-DP-003：DP 层主叫 CLIP 改写 → 直接写 b2b_call_t.clip
     * （零编译风险：不改 b2b_resolve_fn 签名，走 call 字段通道） */
    if (dp_ok && dres.clip[0] && call) {
        snprintf(call->clip, sizeof(call->clip), "%s", dres.clip);
    }
    /* REQ-DP-SIP-001：整个 SIP 头重写容器全量透传（dp_result_t.hdr_rw → b2b_call_t.hdr_rw） */
    if (dp_ok && call) {
        call->hdr_rw = dres.hdr_rw;
        /* legacy：call.hdr_rw.sip_from_uri 为空但 clip 非空 → 用 clip 填 sip_from_uri
         * （b2b_apply_hdr_rewrite 内部优先级 sip_invite_from > sip_from_uri > clip_from > clip_from_clip，
         * 这里不抢优先级，只让 clip 也能被新框架消费） */
        if (dres.clip[0] && !call->hdr_rw.sip_from_uri[0])
            snprintf(call->hdr_rw.sip_from_uri, sizeof(call->hdr_rw.sip_from_uri),
                     "sip:%s", dres.clip);
        /* REQ-MEDIA-DP-001：拨号规则级媒体模式透传到 b2b_call → media_bridge */
        call->media_mode = dres.media_mode;
    }
    /* FreeSWITCH 风格精细化主叫覆盖：origination_caller_id_number/name 透传
     * orig_caller_id_num 只填 user part，host 在 b2bua.c From 构造时根据 trunk 自动补 */
    if (dp_ok && call) {
        if (dres.orig_caller_id_num[0])
            snprintf(call->orig_caller_id_num, sizeof(call->orig_caller_id_num), "%s", dres.orig_caller_id_num);
        if (dres.orig_caller_id_name[0])
            snprintf(call->orig_caller_id_name, sizeof(call->orig_caller_id_name), "%s", dres.orig_caller_id_name);
    }
    /* REQ-PBX-DP-003：号码变换追溯 → call.dst_number
     * （handle_new_invite b->to/b->ruri 用变换后的号码，但 CDR 需要原始号码追溯变换链）
     * ★ orig_number 在 b2bua.c handle_new_invite 里从 From header 提取（主叫），
     *   这里只设 dst_number（被叫）。之前错用 R-URI user 覆盖 orig_number →
     *   呼叫表显示 caller=1001 callee=1001，两个都是被叫。 */
    if (call) {
        if (dp_ok && dres.dst_number[0]) {
            snprintf(call->dst_number, sizeof(call->dst_number), "%s",
                     dres.dst_number);
        } else {
            /* 未命中 DP 号码变换：变换后号码 = 原始号码（R-URI user） */
            snprintf(call->dst_number, sizeof(call->dst_number), "%s", user);
        }
        /* B 腿出站 Profile：根据 dp_layer 决定
         *   EXT/BIZ/未命中 DP → call->prof（内部分机/队列，走 A 腿 profile 出站）
         *   OUT → external profile（中继/域外，走 external 出站）
         * 302 follow 会再次调 resolve，这里每次都重算覆盖 */
        call->out_prof = call->prof;  /* 默认走 A 腿 profile */
        if (dp_ok && dres.layer == DP_LAYER_OUT) {
            /* 找 external profile：先按名字 "external" 精确匹配，
             * 找不到就跳过 call->prof 取第一个 enabled 的。
             * 只有 internal/external 两个 profile 的场景下，跳过 internal 就是 external */
            sip_profile_t *found = NULL;
            sip_profile_t *first_other = NULL;
            for (unsigned i = 0; i < sip_profile_count(); ++i) {
                sip_profile_t *p = sip_profile_at(i);
                if (!p || !p->enabled) continue;
                if (p == call->prof) continue;
                if (strcasecmp(p->name, "external") == 0) {
                    found = p;
                    break;
                }
                if (!first_other) first_other = p;
            }
            call->out_prof = found ? found : (first_other ? first_other : call->prof);
            llog(LOG_INFO, "sig", "resolve: out_prof → %s (layer=OUT)",
                 call->out_prof ? call->out_prof->name : "(null)");
        }
        /* REQ-ARCH-015 per-key 并发跟踪：预填 call->b_key
         * EXT 层 → AoR "target@realm"（dp_result.target 就是分机 user，无组名歧义）；
         * OUT 层 → 延后到 rt_select 成功之后用 tr->name（dres.target 可能是组名）；
         * 未命中 DP → 空字符串（字面量直连，无 per-key 限制） */
        call->b_key[0] = '\0';
        if (dp_ok && dres.layer == DP_LAYER_EXT) {
            snprintf(call->b_key, sizeof(call->b_key), "%s@%s",
                     dres.target, s->realm);
        }
        /* OUT 层 b_key 在 OUT resolve 路径里 rt_select 成功后填 tr->name */
    }
    if (dp_ok && dres.layer == DP_LAYER_EXT) {
        /* target 空 = 不改写被叫（Internal 分机对打场景，变换后号码直达）；
         * target 非空 = 显式指定目标分机/业务号 */
        const char *ext_user = dres.target[0] ? dres.target
                                              : dres.dst_number;
        if (ext_user[0]) {
            char aor[256];
            int al = snprintf(aor, sizeof(aor), "%s@%s", ext_user, s->realm);
            if (al > 0 && al < (int)sizeof(aor)) {
                loc_binding_t b;
                int n = loc_query(s->loc, aor, &b, 1, nsbc_now_ms());
                llog(LOG_INFO, "sig", "resolve: dp_ext target=%s aor=%s loc_n=%d contact=%s",
                     ext_user, aor, n, n > 0 ? b.contact : "(none)");
                if (n > 0) {
                    /* REQ-NAT-001：优先 NAT 改写后的 public_contact（可达）
                     * NAT 场景 public_contact != b.contact；公网直连两者相同 */
                    const char *route_contact =
                        (b.nat_detected && b.public_contact[0])
                        ? b.public_contact : b.contact;
                    /* WS 注册（RFC 7118）：.invalid Contact 无法 sockaddr
                     * 路由 — binding 存有 ws_conn*，直接走 WS 通道 */
                    if (b.ws_conn && ws_conn_alive(b.ws_conn)) {
                        if (sa_hide_ws_conn(dst, b.ws_conn) == NSBC_OK) {
                            ruri_from_contact(route_contact, out_ruri, outsz);
                            if (out_to && out_to_sz)
                                snprintf(out_to, out_to_sz, "<sip:%s@%s>",
                                         ext_user, s->realm);
                            return NSBC_OK;
                        }
                    } else {
                        if (uri_to_sa(route_contact, strlen(route_contact), dst) == NSBC_OK) {
                            ruri_from_contact(route_contact, out_ruri, outsz);
                            if (out_to && out_to_sz)
                                snprintf(out_to, out_to_sz, "<sip:%s@%s>",
                                         ext_user, s->realm);
                            if (b.nat_detected)
                                llog(LOG_INFO, "sig", "resolve: NAT route contact=%s → %s",
                                     b.contact, route_contact);
                            return NSBC_OK;
                        }
                    }
                }
            }
        }
        /* dialplan 命中但 target 未注册：fall through 到原逻辑（兜底查
         * 原 user，仍可能路由成功，此处不预设 404 结论） */
        llog(LOG_WARN, "sig", "resolve: dp_ext target %s@%s unregistered, "
             "fall through to location lookup", ext_user, s->realm);
    }

    /* ---- DP_LAYER_BIZ queue:（REQ-PBX-CC-002 外部分机振铃）----
     * target="queue:xxx" → 剥前缀拿 queue name → cq_get 取成员 ext 列表
     * → 逐个成员 ep_lookup 判断 internal/external 分别路由。
     *
     * 最简版本（可跑通）：取第一个成员 ext（暂不接 cq_dispatch 策略），
     *   internal → loc_query → 注册地址
     *   external → rt_trunk_get → trunk 外呼
     * agent_pool / cc_mgr 可能未初始化（NULL）时返回 NOENT。 */
    if (dp_ok && dres.layer == DP_LAYER_BIZ &&
        !strncmp(dres.target, "queue:", 6)) {
        const char *qname = dres.target + 6;
        cc_mgr_t *mgr = s->ctx ? (cc_mgr_t *)s->ctx->cq_mgr : NULL;
        if (!mgr) {
            llog(LOG_WARN, "sig", "resolve: queue '%s' → cq_mgr not ready, skip", qname);
            return NSBC_NOENT;
        }
        /* ---- Phase 2 多租户：cq_get 用呼入 SIP realm 作为 tenant 匹配 ----
         * s->realm 即租户名（tenant_name），来自 INVITE realm 字段；
         * queue_table_reload_json 从 Rust sync.rs 收到的 JSON 已用 tenant_name
         * 存 slot.q.tenant（如 "nsbc.local"、"acme.com"）。
         * slot_find 仍保留空串 wildcard fallback（Rust 未 reload 的过渡期）。 */
        const char *caller_tenant = (s && s->realm[0]) ? s->realm : "";
        const cq_queue_t *q = cq_get(mgr, caller_tenant, qname);
        if (!q || !q->enabled || q->n_members <= 0) {
            llog(LOG_WARN, "sig", "resolve: queue '%s' not found/disabled/empty", qname);
            return NSBC_NOENT;
        }
        /* 取第一个成员——以后接 cq_dispatch 做策略选路 */
        const char *ext_user = q->members[0];
        llog(LOG_INFO, "sig", "resolve: queue '%s' → first member '%s' (total=%d)",
             qname, ext_user, q->n_members);

        /* ep_lookup 判断 internal / external */
        const struct ep_ext *ep = ep_lookup(ext_user, s->realm);
        if (!ep) {
            llog(LOG_WARN, "sig", "resolve: queue member '%s' not in ext table", ext_user);
            return NSBC_NOENT;
        }

        if (strcmp(ep->ext_type, "external") == 0 && ep->trunk_name[0]) {
            /* --- 外部分机：走 trunk 外呼 ---
             * trunk_name → rt_trunk_get → 外呼 external_number */
            route_table_t *rt = s->ctx ? s->ctx->rt : NULL;
            if (!rt) return NSBC_NOENT;
            const trunk_t *tr = rt_trunk_get(rt, ep->trunk_name);
            if (!tr || !tr->enabled) {
                llog(LOG_WARN, "sig", "resolve: external ext '%s' trunk '%s' not found",
                     ext_user, ep->trunk_name);
                return NSBC_NOENT;
            }
            /* 外呼目标号码：display_number（主叫显示）用 clip 字段或 ext.clip */
            const char *caller_display =
                ep->display_number[0] ? ep->display_number
                : (ep->clip[0] ? ep->clip : "");
            const char *dst_num =
                ep->external_number[0] ? ep->external_number : dres.dst_number;
            /* 构造 dst_sa */
            if (tr->cached_sa.len > 0) {
                *dst = tr->cached_sa;
            } else if (nsbc_sa_from(dst, tr->host, tr->port, AF_UNSPEC) != NSBC_OK) {
                llog(LOG_WARN, "sig", "resolve: external ext '%s' trunk '%s' resolve fail",
                     ext_user, ep->trunk_name);
                return NSBC_NOENT;
            }
            /* out_ruri + out_to */
            if (out_ruri && outsz) {
                snprintf(out_ruri, outsz, "sip:%s@%s:%u", dst_num, tr->host, tr->port);
            }
            if (out_to && out_to_sz) {
                const char *dom = tr->realm[0] ? tr->realm : tr->host;
                if (caller_display[0]) {
                    snprintf(out_to, out_to_sz, "\"%s\" <sip:%s@%s>",
                             caller_display, dst_num, dom);
                } else {
                    snprintf(out_to, out_to_sz, "<sip:%s@%s>", dst_num, dom);
                }
            }
            /* 占用 trunk + 存 rt_name → sig_on_end_cb rt_release */
            rt_acquire(rt, tr->name);
            if (call) {
                snprintf(call->rt_name, sizeof(call->rt_name), "%s", tr->name);
                /* 外呼用 display_number 做主叫显示 → 写入 effective_caller_id_num */
                if (caller_display[0])
                    snprintf(call->effective_caller_id_num,
                             sizeof(call->effective_caller_id_num), "%s", caller_display);
            }
            llog(LOG_INFO, "sig", "resolve: queue '%s' member '%s' EXTERNAL → "
                 "trunk '%s' dst=%s display='%s'",
                 qname, ext_user, tr->name, dst_num, caller_display);
            return NSBC_OK;
        }

        /* --- 内部分机：loc_query 走注册地址 --- */
        char aor[256];
        snprintf(aor, sizeof(aor), "%s@%s", ext_user, s->realm);
        loc_binding_t b;
        int n = s->loc ? loc_query(s->loc, aor, &b, 1, nsbc_now_ms()) : 0;
        if (n <= 0) {
            llog(LOG_WARN, "sig", "resolve: queue member '%s' (internal) not registered", ext_user);
            return NSBC_NOENT;
        }
        const char *contact = (b.nat_detected && b.public_contact[0])
                              ? b.public_contact : b.contact;
        if (b.ws_conn && ws_conn_alive(b.ws_conn)) {
            if (sa_hide_ws_conn(dst, b.ws_conn) == NSBC_OK) {
                if (out_ruri && outsz)
                    snprintf(out_ruri, outsz, "sip:%s@%s", ext_user, s->realm);
                if (out_to && out_to_sz)
                    snprintf(out_to, out_to_sz, "<sip:%s@%s>", ext_user, s->realm);
                return NSBC_OK;
            }
        }
        size_t cl = strlen(contact);
        if (uri_to_sa(contact, cl, dst) != NSBC_OK) {
            llog(LOG_WARN, "sig", "resolve: queue member '%s' contact '%s' → sa fail",
                 ext_user, contact);
            return NSBC_NOENT;
        }
        if (out_ruri && outsz)
            snprintf(out_ruri, outsz, "sip:%s@%s", ext_user, s->realm);
        if (out_to && out_to_sz)
            snprintf(out_to, out_to_sz, "<sip:%s@%s>", ext_user, s->realm);
        llog(LOG_INFO, "sig", "resolve: queue '%s' member '%s' INTERNAL → contact=%s",
             qname, ext_user, contact);
        return NSBC_OK;
    }

    if (dp_ok && dres.layer == DP_LAYER_OUT) {
        /* ---- DP_LAYER_OUT 完整实现（REQ-PBX-ROUTE-002 中继选路）----
         * dres.target = trunk name（dialplan rule 配置的中继名，cfg 格式：
         *   "match_pattern:2:trunk_name[:strip][:add_prefix]"）。
         * rt_trunk_get → trunk_t → 构造 dst_sa + out_ruri + out_to。 */
        route_table_t *rt = s->ctx ? s->ctx->rt : NULL;
        if (!rt) {
            llog(LOG_WARN, "sig", "dp out route: route_table NULL");
            return NSBC_NOENT;
        }
        const trunk_t *tr = NULL;
        /* ---- Advisor 2026-10-01k：dialplan target 可能是组名或单 trunk ----
         * 优先 rt_select（组选路，内置 max_channels 检查 + 熔断跳过），
         * rt_select 返回 NULL（target 不是组名）时 fallback rt_trunk_get。 */
        /* Advisor 2026-10-12m：剥 OUT 层前缀（gateway:/trunk:/route:）
         * dp_infer_layer_from_target 用前缀判断 layer，但 rt_select / rt_trunk_get
         * 按纯名字精确匹配——所以这里统一剥掉前缀后再查 */
        const char *lookup = dres.target;
        if (!strncmp(lookup, "gateway:", 8)) lookup += 8;
        else if (!strncmp(lookup, "trunk:", 6)) lookup += 6;
        else if (!strncmp(lookup, "route:", 6)) lookup += 6;

        if (call && call->leg[0].from[0]) {
            tr = rt_select(rt, lookup, call->leg[0].from, 0);
        } else {
            tr = rt_select(rt, lookup, NULL, 0);
        }
        if (!tr) tr = rt_trunk_get(rt, lookup);
        if (!tr) {
            llog(LOG_WARN, "sig", "dp out route: trunk '%s' (raw='%s') not found in route_table",
                 lookup, dres.target);
            return NSBC_NOENT;
        }
        if (!tr->enabled) {
            llog(LOG_WARN, "sig", "dp out route: trunk '%s' disabled", tr->name);
            return NSBC_NOENT;
        }
        if (!rt_is_open(rt, tr->name)) {
            llog(LOG_WARN, "sig", "dp out route: trunk '%s' circuit-broken", tr->name);
            return NSBC_NOENT;
        }
        /* Advisor 2026-10-01l：DNS 预解析加速 OUT 选路
         * trunk_t.cached_sa 在 rt_trunk_add 时预解析（配置加载线程）；
         * len>0 表示预解析成功 → 直接 memcpy 到 dst（零阻塞）；
         * len==0 表示预解析失败（动态 DNS）→ fallback 阻塞 getaddrinfo。 */
        if (tr->cached_sa.len > 0) {
            *dst = tr->cached_sa;
        } else if (nsbc_sa_from(dst, tr->host, tr->port, AF_UNSPEC) != NSBC_OK) {
            llog(LOG_WARN, "sig", "dp out route: trunk '%s' resolve %s:%u failed",
                 tr->name, tr->host, tr->port);
            return NSBC_NOENT;
        }
        /* out_ruri：sip:<变换后被叫>@<trunk host:port> */
        if (out_ruri && outsz) {
            int n = snprintf(out_ruri, outsz, "sip:%s@%s:%u",
                             dres.dst_number[0] ? dres.dst_number : user,
                             tr->host, tr->port);
            if (n < 0 || (size_t)n >= outsz)
                return NSBC_NOMEM;
        }
        /* out_to：<sip:<变换后被叫>@<trunk realm 或 host>> */
        if (out_to && out_to_sz) {
            const char *domain = tr->realm[0] ? tr->realm : tr->host;
            int n = snprintf(out_to, out_to_sz, "<sip:%s@%s>",
                             dres.dst_number[0] ? dres.dst_number : user,
                             domain);
            if (n < 0 || (size_t)n >= out_to_sz)
                return NSBC_NOMEM;
        }
        /* OUT 层 b_key 延后到 rt_select 后填（用选中的 tr->name 而非组名 dres.target） */
        if (call) snprintf(call->b_key, sizeof(call->b_key), "trunk:%s", tr->name);
        /* rt_acquire：中继 in_use++（rt_release 在 sig_on_end_cb 时） */
        rt_acquire(rt, tr->name);
        /* 存 trunk 名 → sig_on_end_cb rt_release 用（常量 32 对齐 RT_NAME_MAX） */
        if (call) snprintf(call->rt_name, sizeof(call->rt_name), "%s", tr->name);
        llog(LOG_INFO, "sig", "dp out route: %s → trunk '%s' %s:%u",
             user, tr->name, tr->host, tr->port);
        return NSBC_OK;
    }

    /* ---- 原有 Location DB / 字面量路径 ----
     * §15 架构加固：如果拨号规则没命中（dp_ok=0），**不允许** fallback 绕过规则直接查 Location DB
     * —— 拨号规则是强制准入层，规则未命中 = 不允许此呼叫，直接 silent drop。
     * 合法 fallback 只有一种：dp_ok=1 但 target 分机未注册（行 476），那时仍可以 loc_query 原 user */
    if (!dp_ok) {
        llog(LOG_NOTICE, "sig",
             "resolve: dp_hit=0 user=%s prof=%s → silent drop (no rule matched, fallback forbidden)",
             user, prof_name);
        return NSBC_ERR;
    }

    loc_binding_t b;
    int n = -1;
    if (in_domain) {
        char aor[256];
        int al = snprintf(aor, sizeof(aor), "%s@%s", user, s->realm);
        if (al > 0 && al < (int)sizeof(aor)) {
            n = loc_query(s->loc, aor, &b, 1, nsbc_now_ms());
            llog(LOG_INFO, "sig", "resolve: loc_query aor=%s n=%d contact=%s expire_ms=%lu now_ms=%lu",
                 aor, n, n > 0 ? b.contact : "(none)",
                 n > 0 ? (unsigned long)b.expire_ms : 0,
                 (unsigned long)nsbc_now_ms());
        }
        if (n <= 0) {
            int al2 = snprintf(aor, sizeof(aor), "%s@%s", user, rhost);
            if (al2 > 0 && al2 < (int)sizeof(aor))
                n = loc_query(s->loc, aor, &b, 1, nsbc_now_ms());
        }
    }
    if (n > 0) {
        /* Loop detection（用原始 contact 看——NAT 改写后的 public_contact 不可能等于 local_host） */
        size_t cl = strlen(b.contact);
        nsbc_sa_t contact_sa;
        if (uri_to_sa(b.contact, cl, &contact_sa) == NSBC_OK) {
            char contact_host[INET6_ADDRSTRLEN] = "";
            void *addrp = NULL;
            if (contact_sa.ss.ss_family == AF_INET6)
                addrp = &((struct sockaddr_in6 *)&contact_sa.ss)->sin6_addr;
            else if (contact_sa.ss.ss_family == AF_INET)
                addrp = &((struct sockaddr_in *)&contact_sa.ss)->sin_addr;
            if (addrp)
                inet_ntop(contact_sa.ss.ss_family, addrp, contact_host,
                          sizeof(contact_host));
            uint16_t cport = nsbc_sa_port(&contact_sa);
            if ((strcmp(contact_host, s->local_host) == 0 ||
                 strcmp(contact_host, "127.0.0.1") == 0 ||
                 strcmp(contact_host, "localhost") == 0 ||
                 strcmp(contact_host, "::1") == 0) &&
                cport == 5060) {
                llog(LOG_WARN, "sig", "resolve: Contact %s:%u matches local SIP "
                     "listener — loop detected, treat as unavailable",
                     contact_host, cport);
                n = 0; /* 强制走 NOENT → 404，避免 B2BUA 递归 */
            }
        }
    }
    if (n > 0) {
        /* REQ-NAT-001：优先 NAT 改写后的 public_contact（可达） */
        const char *route_contact =
            (b.nat_detected && b.public_contact[0])
            ? b.public_contact : b.contact;

        /* WS 注册（RFC 7118）：.invalid Contact 无法 sockaddr 路由，
         * binding 存有 ws_conn* → 直接走 WS 通道（防 UAF：alive 校验）
         * WS 场景一般无 NAT（浏览器用 WebSocket 本来就是直连），
         * 但仍用 route_contact 统一逻辑 */
        if (b.ws_conn && ws_conn_alive(b.ws_conn) &&
            sa_hide_ws_conn(dst, b.ws_conn) == NSBC_OK) {
            ruri_from_contact(route_contact, out_ruri, outsz);
            if (out_to && out_to_sz)
                snprintf(out_to, out_to_sz, "<sip:%s@%s>", user, s->realm);
            return NSBC_OK;
        }
        /* 用 route_contact 覆盖目的与 R-URI */
        size_t cl = strlen(route_contact);
        if (uri_to_sa(route_contact, cl, dst) != NSBC_OK) {
            llog(LOG_WARN, "sig",
                 "resolve: %s@%s route_contact unreachable (%s, %s) → 404",
                 user, s->realm, route_contact,
                 b.ws_conn ? "ws_conn dead" : "host unresolvable");
            return NSBC_NOENT;
        }
        ruri_from_contact(route_contact, out_ruri, outsz);
        if (out_to && out_to_sz)
            snprintf(out_to, out_to_sz, "<sip:%s@%s>", user, s->realm);
        if (b.nat_detected)
            llog(LOG_INFO, "sig", "resolve: NAT route contact=%s → %s",
                 b.contact, route_contact);
        return NSBC_OK;
    }
    /* 域外：字面量地址直连（主机名经 DNS 缓存为 P1） */
    if (in_domain) {
        llog(LOG_WARN, "sig", "resolve: %s@%s unregistered → 404",
             user, s->realm);
        return NSBC_NOENT; /* 本域未注册：404 */
    }
    if (uri_to_sa(buf, ruri_len, dst) != NSBC_OK)
        return NSBC_NOENT;
    if (out_ruri && outsz) {
        size_t copy = ruri_len < outsz - 1 ? ruri_len : outsz - 1;
        memcpy(out_ruri, ruri, copy);
        out_ruri[copy] = '\0';
    }
    if (out_to && out_to_sz)
        snprintf(out_to, out_to_sz, "<sip:%s@%s>", user, s->realm);
    return NSBC_OK;
}

/* ------------------------------------------------------------------ */
/* 媒体挂接：决策在 bridge 内部（offer/answer hook），结束释放          */
/* ------------------------------------------------------------------ */
static int media_confirm_cb(b2b_call_t *call, const char *a, size_t al,
                            const char *b, size_t bl, void *ud)
{
    /* 两侧 SDP 齐备后由 b2b 在 call 锁内调一次：bridge 复核最终模式
     * （已锚定 relay 不降级 bypass；transcode 仅落账，执行引擎 P1）。 */
    return bridge_confirm((bridge_mgr_t *)ud, call, a, al, b, bl);
}

/* OAM 只读视图翻译层（组合根）：bridge 侧下标按 relay 物理侧，
 * [0]=朝 A（fd_rtp[0] 收 A 报文）、[1]=朝 B；side 号与 b2b Leg 同号：
 * leg0(A)→[0]、leg1(B)→[1]（端口方向标准化后无需翻转）。
 * relay[0] 记录收 A 的入向学习 SSRC 与发往 A 的出向重写 SSRC，
 * 恰好是 leg0 视角的 ssrc_in/out。 */
static int media_view_cb(void *call_key, int b_leg, b2b_media_view_t *v,
                         void *ud)
{
    mb_view_t mv;
    int rc = bridge_call_view((bridge_mgr_t *)ud, call_key, &mv);
    if (rc != NSBC_OK)
        return rc;
    int side = b_leg;
    v->valid = mv.valid;
    v->mode = (b2b_media_mode_t)mv.mode; /* 0/1/2 两枚举同序 */
    v->gate_open = mv.gate_open[side];
    v->ssrc_in = mv.ssrc_in[side];
    v->have_ssrc_in = mv.have_ssrc_in[side];
    v->ssrc_out = mv.ssrc_out[side];
    v->have_ssrc_out = mv.have_ssrc_out[side];
    v->vq_fraction = mv.rr_fraction[side];
    v->vq_lost = mv.rr_lost[side];
    v->vq_jitter = mv.rr_jitter[side];
    v->have_vq = mv.have_rr[side];
    /* REQ-MEDIA-SEC-003⑤ P0：SSRC↔CNAME 绑定 + 安全计数 */
    v->have_cname = mv.have_cname[side];
    if (v->have_cname)
        memcpy(v->cname, mv.cname[side], sizeof(v->cname));
    v->ssrc_inject_drops = mv.ssrc_inject_drops[side];
    v->ssrc_collision_drops = mv.ssrc_collision_drops[side];
    return NSBC_OK;
}

static void __attribute__((unused)) media_end_cb(void *call_key, void *ud)
{
    /* call_key 为 b2b 层不透明呼叫键，bridge 侧仅以指针值查表（REQ-ARCH-011） */
    bridge_release_call((bridge_mgr_t *)ud, call_key);
}

/* REQ-REC-003 + REQ-PBX-CDR-001：呼叫结束时组合 cleanup。
 * 先清 bridge（media_end_cb），再兜底停录音（recording_apply_dp_action stop），
 * 最后发 cdr END 事件带 rec_path。
 *
 * REQ-ARCH-011 说明：call_key 声明为 void*（b2b_end_hook_fn 签名），
 * 但本函数为 signaling 层内部 hook，与 b2b_call_t 同属 core/sip 域（非 mod 层），
 * 解引用拿 call_id 合法；真正的"跨模块禁止解引用"适用于 sip→mod 回调。 */

/* 从 SIP URI 提取 user 部分（纯号码）——与 b2bua.c 同名同语义 helper，
 * signaling 层无法直接引用 b2bua.c 的 static 函数，故就地复制一份。 */
static void sig_uri_extract_user(char *out, size_t cap, const char *uri)
{
    if (!uri || !cap) { out[0] = '\0'; return; }
    const char *p = uri;
    if (*p == '<') p++;
    if (!strncasecmp(p, "sip:", 4)) p += 4;
    else if (!strncasecmp(p, "tel:", 4)) p += 4;
    const char *at = NULL;
    const char *end = NULL;
    for (const char *q = p; *q; ++q) {
        if (*q == '@' && !at) at = q;
        if (*q == ';' || *q == '>' || *q == ':' || *q == ' ' || *q == '"') {
            end = q; break;
        }
    }
    if (!end) end = p + strlen(p);
    size_t n = at ? (size_t)(at - p) : (size_t)(end - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static void sig_on_end_cb(void *call_key, void *ud)
{
    sig_runtime_t *s = ud;
    b2b_call_t *c = (b2b_call_t *)call_key;

    /* 1) 采集媒体质量快照（必须在 bridge_release_call 之前！
     *    release 后 relay 被销毁，bridge_call_view 会找不到 call → mv 全零） */
    int32_t vq_loss_pct = -1;   /* -1=无数据 */
    uint32_t vq_jitter_ms = 0;
    int32_t vq_mos = -1;        /* MOS×1000，-1=无效 */
    if (s->bridge && c) {
        mb_view_t mv; memset(&mv, 0, sizeof(mv));
        int rc = bridge_call_view(s->bridge, c, &mv);
        llog(LOG_INFO, "sig", "on_end_vq_pre: rc=%d valid=%d gate0=%d mode=%d callid=%s",
             rc, mv.valid, mv.gate_open[0], mv.mode,
             c->leg[0].callid ? c->leg[0].callid : "?");
        if (rc == NSBC_OK && mv.valid && mv.gate_open[0]) {
            /* 优先用 RTCP RR（有对端报告），退化用 RTP seq-gap 本地推断 */
            uint32_t exp = 0, lost = 0, jitter_ticks = 0;
            int have_rr = 0;
            for (int i = 0; i < 2; i++) {
                if (mv.have_rr[i] && mv.rr_fraction[i]) {
                    /* fraction = 累计丢失比例（/256 取整），exp ≈ rr_lost / (fraction/256) */
                    uint32_t fe = mv.rr_fraction[i];
                    if (fe > 0 && mv.rr_lost[i] > 0) {
                        uint64_t e = (uint64_t)mv.rr_lost[i] * 256u / fe;
                        if (e > 0xFFFFFFFFu) e = 0xFFFFFFFFu;
                        exp += (uint32_t)e;
                        lost += mv.rr_lost[i];
                        jitter_ticks += mv.rr_jitter[i];
                        have_rr = 1;
                    }
                }
                exp += mv.rtp_expected[i];
                lost += mv.rtp_lost[i];
            }
            if (exp > 0) {
                vq_loss_pct = (int32_t)((uint64_t)lost * 1000u / exp); /* 千分比 */
                /* E-model 简化：R = 94.2 - Ie - Id - Ie*loss_pct/(loss_pct+Bpl)
                 * codec=PCMU/G711 时 Ie=0, Bpl=19.0；简化 loss 影响：R_loss = loss_pct * 25.1 */
                int32_t loss_q = vq_loss_pct; /* 千分比 → 直接用 */
                int32_t r_factor = 942 - loss_q * 25 / 100; /* R×100，loss 千分→÷10 */
                if (r_factor < 0)   r_factor = 0;
                if (r_factor > 1000) r_factor = 1000;
                /* R→MOS (ITU-T G.107 简化) */
                if (r_factor > 640)
                    vq_mos = (int32_t)(0.035f * r_factor + 7.0f * r_factor * (1000 - r_factor) * (r_factor - 640) * 1e-8f);
                else
                    vq_mos = (int32_t)(0.0001f * r_factor * r_factor * r_factor);
                /* RR jitter tick→ms：PCMU=8000Hz → 每个 tick=125us */
                if (have_rr && jitter_ticks > 0)
                    vq_jitter_ms = jitter_ticks / (mv.have_rr[0] + mv.have_rr[1]) * 125 / 1000;
            }
            llog(LOG_INFO, "sig",
                 "hangup vq call=%s loss_permille=%d loss_pkt=%u exp_pkt=%u jitter_ms=%u mos_x1000=%d rr=%d",
                 c->leg[0].callid, vq_loss_pct, lost, exp, vq_jitter_ms, vq_mos, have_rr);
            /* VQ 结果存回 b2b_call_t，供后续 b2b_list_calls 和 CDR emit 使用 */
            if (c) {
                c->mos_x1000 = vq_mos;
                if (vq_loss_pct >= 0 && vq_loss_pct <= 1000) /* 千分比→% */
                    c->rtp_loss_pct = (uint8_t)((vq_loss_pct + 5) / 10);
                else
                    c->rtp_loss_pct = 0;
                c->rtp_jitter_ms = (uint16_t)vq_jitter_ms;
            }
        }
    }

    /* 2) bridge 清理（VQ 已采集完，现在可以安全释放） */
    if (s->bridge)
        bridge_release_call(s->bridge, call_key);

    /* 3) 中继 rt_release（REQ-PBX-ROUTE-002：DP_LAYER_OUT rt_acquire 的配对释放） */
    if (c && c->rt_name[0] && s->ctx && s->ctx->rt) {
        rt_release(s->ctx->rt, c->rt_name);
        c->rt_name[0] = '\0';
    }

    /* 4) 兜底停录音（dialplan 没发 stop 时的保险），拿 final_path */
    char rec_path[CDR_REC_MAX];
    rec_path[0] = '\0';
    if (s->ctx && s->ctx->rec_mgr && c) {
        const char *cid = c->leg[0].callid;
        if (cid && cid[0]) {
            recording_apply_dp_action((rec_mgr_t *)s->ctx->rec_mgr, 2,
                                      cid, "hangup",
                                      NULL, NULL, NULL,
                                      rec_path, sizeof(rec_path));
        }
    }

    /* 5) 发 cdr END 事件（billing 模块按 call_id 聚合 START/END，END 触发写库） */
    if (s->ctx && s->ctx->cdr_emit && c) {
        cdr_event_t ev; memset(&ev, 0, sizeof(ev));
        snprintf(ev.call_id, sizeof(ev.call_id), "%s", c->leg[0].callid);
        if (c->leg[0].from[0]) {
            char u[128]; sig_uri_extract_user(u, sizeof(u), c->leg[0].from);
            snprintf(ev.from, sizeof(ev.from), "%s", u);
        }
        if (c->leg[1].to[0]) {
            char u[128]; sig_uri_extract_user(u, sizeof(u), c->leg[1].to);
            snprintf(ev.to, sizeof(ev.to), "%s", u);
        }
        if (rec_path[0])       snprintf(ev.rec_path, sizeof(ev.rec_path), "%s", rec_path);
        ev.dir = CDR_DIR_UNKNOWN; /* P0 简化，按域内/域外用 host 判断 */
        ev.phase = CDR_PHASE_END;
        /* 时间戳：优先用 b2b_call_t 里记账点，END 时 end_ms 取 wall now。
         * b2b_call_t 里 start_ms/answer_ms 是 monotonic（uptime），
         * 这里转成 wall（epoch ms）。 */
        {
            uint64_t now_mon = nsbc_now_ms();
            uint64_t now_wall = nsbc_wall_ms();
            uint64_t sm = c->start_ms ? c->start_ms : now_mon;
            uint64_t sw = now_wall - (now_mon - sm);
            ev.start_ms  = (sw < 946684800000ULL) ? now_wall : sw;
            if (c->answer_ms) {
                uint64_t aw = now_wall - (now_mon - c->answer_ms);
                ev.answer_ms = (aw < 946684800000ULL) ? now_wall : aw;
            }
            ev.end_ms    = now_wall; /* END 时当前 wall 时间 */
        }
        if (ev.answer_ms && ev.answer_ms >= ev.start_ms) {
            ev.billsec = (uint32_t)((ev.answer_ms - ev.start_ms) / 1000);
            ev.duration_s = (uint32_t)((ev.end_ms - ev.start_ms) / 1000);
        } else if (ev.start_ms) {
            ev.duration_s = (uint32_t)((ev.end_ms - ev.start_ms) / 1000);
        }
        /* 媒体质量指标（从 b2b_call_t 读，刚在上面 END 流程存回） */
        if (c->codec_a[0]) snprintf(ev.codec_a, sizeof(ev.codec_a), "%s", c->codec_a);
        if (c->codec_b[0]) snprintf(ev.codec_b, sizeof(ev.codec_b), "%s", c->codec_b);
        ev.rtp_loss_pct = c->rtp_loss_pct;
        ev.rtp_jitter_ms = c->rtp_jitter_ms;
        ev.mos_x1000 = c->mos_x1000;
        /* 网络端点 + codec fallback：从两腿 answer SDP 解析（b2b_call_t 的 codec_a/b 可能没人写）。
         * 答非 0=解析失败/没找到，字段留空（已经 memset 过）。 */
        if (c->leg[0].answer_len)
            sig_sdp_extract_cdr(c->leg[0].answer, c->leg[0].answer_len,
                                ev.codec_a, sizeof(ev.codec_a),
                                ev.a_peer_ip, sizeof(ev.a_peer_ip));
        if (c->leg[1].answer_len)
            sig_sdp_extract_cdr(c->leg[1].answer, c->leg[1].answer_len,
                                ev.codec_b, sizeof(ev.codec_b),
                                ev.b_peer_ip, sizeof(ev.b_peer_ip));
        /* from_ip：A 腿入向 INVITE 的 src（handle_new_invite 已存） */
        if (c->from_ip[0])
            snprintf(ev.from_ip, sizeof(ev.from_ip), "%s", c->from_ip);
        if (c->a_final > 0)
            ev.disposition = (c->a_final >= 200 && c->a_final < 300) ? CDR_DISP_ANSWERED
                           : (c->a_final == 486) ? CDR_DISP_BUSY
                           : (c->a_final == 480) ? CDR_DISP_NOANSWER
                           : CDR_DISP_FAILED;
        s->ctx->cdr_emit(s->ctx->cdr_ud, &ev);

        /* signaling.c CDR END 完成后，把完整 cdr_event_t 序列化成 JSON 存进
         * b2b_call_t.cdr_json——b2bua.c 后续推 UDS SESSION_END 事件时直接 memcpy 复用，
         * 避免另造一份残缺 snprintf 漏掉 codec / IP / 媒体质量等字段。 */
        {
            char *j = cdr_event_to_json(&ev);
            if (j) {
                snprintf(c->cdr_json, sizeof(c->cdr_json), "%s", j);
                free(j);
            }
        }

        /* ---- 6) 实时事件推送：CALL_END（无条件；复用刚才填好的 ev 字段）----
         * 订阅者收到 CDR_PHASE_END 的 cdr 帧 + CALL_END 的 events 帧，两帧互补：
         *   - cdr 帧是完整 JSON 话单
         *   - events 帧是短体事件，用于低延迟 UI 状态刷新
         * duration/billsec 是整数秒，cause 来自 a_final（A 腿最终状态码）。 */
        {
            const char *call_id  = ev.call_id[0] ? ev.call_id : c->leg[0].callid;
            const char *disp     = cdr_disp_name(ev.disposition);
            const char *orig     = c->orig_number[0] ? c->orig_number : "";
            const char *to_uri   = c->leg[1].to[0] ? c->leg[1].to
                                 : (c->leg[0].to[0] ? c->leg[0].to : "");
            const char *trunk    = c->rt_name[0] ? c->rt_name : "";
            obs_event("CALL_END",
                "call_id=%s orig=%s to=%s trunk=%s disp=%s cause=%d duration_s=%u billsec=%u",
                call_id, orig, to_uri, trunk, disp, c->a_final,
                (unsigned)ev.duration_s, (unsigned)ev.billsec);
        }
    } else if (s->ctx && c) {
        /* billing 模块未启用（ctx->cdr_emit 为空）——也要发 CALL_END 事件
         * 给订阅者；此时 duration/cause 直接从 b2b_call_t 取 */
        const char *call_id  = c->leg[0].callid;
        uint32_t  dur = 0;
        if (c->start_ms && c->start_ms <= nsbc_now_ms())
            dur = (uint32_t)((nsbc_now_ms() - c->start_ms) / 1000u);
        obs_event("CALL_END",
            "call_id=%s orig=%s cause=%d duration_s=%u",
            call_id,
            c->orig_number[0] ? c->orig_number : "",
            c->a_final, dur);
    }
}

/* ------------------------------------------------------------------ */
/* 注册绑定过期扫描                                                     */
/* ------------------------------------------------------------------ */
static void sweep_cb(uint64_t now_ms, void *arg);

static void sweep_arm(sig_runtime_t *s)
{
    tw_node_init(&s->sweep_node);
    int rc = tw_add(s->ctx->tw, &s->sweep_node, nsbc_now_ms() + SWEEP_INTERVAL_MS,
                    sweep_cb, s);
    if (rc != NSBC_OK)
        llog(LOG_WARN, "sig", "loc sweep arm failed rc=%d", rc);
}

static void sweep_cb(uint64_t now_ms, void *arg)
{
    sig_runtime_t *s = (sig_runtime_t *)arg;
    int removed = loc_sweep(s->loc, now_ms);
    if (removed > 0)
        llog(LOG_DEBUG, "sig", "loc sweep removed %d expired bindings",
             removed);
    sweep_arm(s);
}

/* Phase 0: outreg_on_register_response_via_b2b 已搬到 outreg.c，
 * outreg_init 自注册 b2b_resp_hook_add。signaling.c 不再硬编码桥接。 */

/* ------------------------------------------------------------------ */
/* 模块生命周期                                                         */
/* ------------------------------------------------------------------ */

/* WS Bridge 前向声明（定义在文件末尾） */
static void sig_ws_bridge_init(sip_transport_t *tr, sip_tr_proto_t proto);
static void sig_ws_bridge_shutdown(void);
static void sig_ws_bridge_rx(void *ud, const char *buf, size_t len, void *conn);
void *sig_ws_bridge_conn_from_sa(const nsbc_sa_t *sa);

static int sig_init(struct nsbc_ctx *ctx)
{
    sig_runtime_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NSBC_NOMEM;
    s->ctx = ctx;

    /* cfg_get_str 共用单个 __thread 临时窗：多个字符串必须各自拷贝，
     * 不能同时持有多个返回指针（会全部别名到最后一次取值）。 */
    char listen_ip[64] = "0.0.0.0";
    char extern_ip[128] = "";
    char realm[128] = "nsbc.local";
    const char *sp = NULL;
    if (cfg_get_str(ctx->cfg, "net.sip_listen_ip", &sp, NULL) == NSBC_OK)
        snprintf(listen_ip, sizeof(listen_ip), "%s", sp);
    sp = NULL;
    if (cfg_get_str(ctx->cfg, "sip.extern_ip", &sp, NULL) == NSBC_OK)
        snprintf(extern_ip, sizeof(extern_ip), "%s", sp);
    sp = NULL;
    if (cfg_get_str(ctx->cfg, "sip.realm", &sp, NULL) == NSBC_OK)
        snprintf(realm, sizeof(realm), "%s", sp);
    snprintf(s->realm, sizeof(s->realm), "%s", realm);
    /* local_host：extern_ip 优先；listen_ip 为通配符时自动探测非 loopback IPv4 */
    if (extern_ip[0] != '\0') {
        snprintf(s->local_host, sizeof(s->local_host), "%s", extern_ip);
    } else if (strcmp(listen_ip, "0.0.0.0") == 0 || strcmp(listen_ip, "::") == 0) {
        char auto_ip[INET_ADDRSTRLEN] = "";
        struct ifaddrs *ifa = NULL;
        if (getifaddrs(&ifa) == 0) {
            for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
                if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
                    continue;
                struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
                uint32_t ip = ntohl(sin->sin_addr.s_addr);
                /* 排除 127.0.0.0/8 loopback 与 224.0.0.0/4 组播 */
                if ((ip & 0xff000000) == 0x7f000000)
                    continue;
                if ((ip & 0xf0000000) == 0xe0000000)
                    continue;
                inet_ntop(AF_INET, &sin->sin_addr, auto_ip,
                          sizeof(auto_ip));
                break;
            }
            freeifaddrs(ifa);
        }
        if (auto_ip[0]) {
            snprintf(s->local_host, sizeof(s->local_host), "%s", auto_ip);
            llog(LOG_INFO, "sig", "local_host auto-detected: %s", auto_ip);
        } else {
            snprintf(s->local_host, sizeof(s->local_host), "%s", listen_ip);
            llog(LOG_WARN, "sig", "local_host fallback to wildcard %s, "
                 "B 腿 Via/Contact 可能畸形", listen_ip);
        }
    } else {
        snprintf(s->local_host, sizeof(s->local_host), "%s", listen_ip);
    }

    long max_calls = 200000;  /* 默认支持 5-10 万活跃呼叫：
                               * txn_slab = max_calls 个事务（每呼叫 2 txn → 10 万呼叫）
                               * call_session/sdp 各 max_calls
                               * sip_msg = max_calls × 2；pktbuf = max_calls × 4
                               * 内存估算：~200MB slab + ~200MB pktbuf ≈ 400MB
                               * 机器 < 4GB 内存请下调，或设 core.max_calls 覆盖 */
    cfg_get_int(ctx->cfg, "core.max_calls", &max_calls, NULL);
    long port_lo = 10000, port_hi = 59999;
    cfg_get_int(ctx->cfg, "rtp.port_start", &port_lo, NULL);
    cfg_get_int(ctx->cfg, "rtp.port_end", &port_hi, NULL);

    /* 端口表 + 媒体桥 */
    ptable_cfg_t pc = {(uint16_t)port_lo, (uint16_t)port_hi, 64};
    if (ptable_create(&pc, &s->ptable) != NSBC_OK)
        goto fail;
    int allow_bypass = 0, trust_priv = 1;
    cfg_get_bool(ctx->cfg, "media.allow_bypass", &allow_bypass, NULL);
    cfg_get_bool(ctx->cfg, "media.trust_private", &trust_priv, NULL);
    mb_cfg_t mc = {
        .ptable = s->ptable,
        .re = ctx->re,
        .local_ip = s->local_host,
        .allow_bypass = allow_bypass,
        .trust_private = trust_priv,
        /* Phase 0: rec_feed_fn 硬编码桥接已移除 —— recording 模块 init 时通过
         * bridge_add_rec_feed_hook 自注册 recording_rec_feed 回调，零跨模块依赖 */
        /* REQ-SIP-PROFILE-001：per-call RTP 端口段（组合根才可解引用
         * call_key；media 层经回调取值，ARCH-011） */
        .call_port_range = sig_call_port_range,
        /* REQ-DTLS-001：B 腿 WebRTC（WS/WSS）判定回调 */
        .call_b_is_ws = sig_call_b_is_ws,
        /* REQ-MEDIA-DP-001：拨号规则级媒体模式 override 回调 */
        .call_media_mode = sig_call_media_mode,
        /* REQ-NAT-002：per-call NAT 公网 RTP IP —— profile 热更新自动生效 */
        .call_external_rtp_ip = sig_call_external_rtp_ip,
    };
    if (bridge_mgr_create(&mc, &s->bridge) != NSBC_OK)
        goto fail;

    /* Location DB / 对话 / 事务 */
    loc_db_cfg_t lc = {ctx->arena[MEM_DOMAIN_SIGNAL], (int)max_calls, 10};
    long max_contacts = 10;
    cfg_get_int(ctx->cfg, "registrar.max_contacts", &max_contacts, NULL);
    lc.max_contacts = (int)max_contacts;
    if (loc_db_create(&lc, &s->loc) != NSBC_OK)
        goto fail;

    dlg_mgr_cfg_t dc = {ctx->arena[MEM_DOMAIN_SIGNAL], NULL,
                        (int)max_calls};
    if (dlg_mgr_create(&dc, &s->dlg) != NSBC_OK)
        goto fail;

    long t1 = 500, t2 = 4000, t4 = 5000, td = 32000;
    cfg_get_int(ctx->cfg, "sip.t1_ms", &t1, NULL);
    cfg_get_int(ctx->cfg, "sip.t2_ms", &t2, NULL);
    cfg_get_int(ctx->cfg, "sip.t4_ms", &t4, NULL);
    cfg_get_int(ctx->cfg, "sip.timer_d_ms", &td, NULL);
    txn_mgr_cfg_t tc = {ctx->arena[MEM_DOMAIN_SIGNAL],
                        ctx->arena[MEM_DOMAIN_PACKET], NULL,
                        (int)max_calls, ctx->tw,
                        (uint32_t)t1, (uint32_t)t2, (uint32_t)t4,
                        (uint32_t)td, tr_send_cb, s, NULL, NULL};
    if (txn_mgr_create(&tc, &s->txn) != NSBC_OK)
        goto fail;

    /* B2BUA 在 start 阶段创建（依赖 transport + registrar） */
    ctx->loc_db = s->loc;
    ctx->binding_count_fn = (uint64_t (*)(void *))loc_db_count;
    ctx->binding_count_ud = s->loc;
    ctx->bridge = s->bridge;
    ctx->txn_mgr = s->txn;
    ctx->ptable = s->ptable;
    ctx->sig_rt = s;
    dp_ctx_init(&s->dp_ctx);
    return NSBC_OK;

fail:
    llog(LOG_CRIT, "sig", "signaling init failed");
    if (s->b2b)
        b2b_core_destroy(s->b2b);
    if (s->txn)
        txn_mgr_destroy(s->txn);
    if (s->dlg)
        dlg_mgr_destroy(s->dlg);
    if (s->loc)
        loc_db_destroy(s->loc);
    if (s->bridge)
        bridge_mgr_destroy(s->bridge);
    if (s->ptable)
        ptable_destroy(s->ptable);
    free(s);
    return NSBC_ERR;
}

/* TLS 证书热重载：nsbc-web 更新 ../tls/cert.pem + key.pem 后 kill SIGHUP 触发。
 * 读新 PEM → tls_ctx_server_new → 替换 s->tls → 更新 transport + WS listener。
 *
 * 安全保证（OpenSSL 1.1+ 引用计数）：
 *   - tls_conn_accept 内部 SSL_new(ctx) 会增加 ctx 的引用计数
 *   - 老连接持有的 SSL* 自己引用 ctx，ctx 引用计数不为 0 不会真正 free
 *   - 新连接从新 accept loop 拿到新 ctx（transport.cfg.tls_ctx 已替换）
 *   - tls_ctx_free(old_ctx) 安全：老连接引用它，内存延迟释放 */
int sig_reload_tls_certs(struct nsbc_ctx *ctx)
{
    sig_runtime_t *s = ctx ? (sig_runtime_t *)ctx->sig_rt : NULL;
    if (!s || !s->tls) {
        llog(LOG_WARN, "sig", "tls reload skipped: sig_runtime or tls not ready");
        return NSBC_NOTSUP;
    }
    cfg_t *cfg = ctx->cfg;

    /* 读 cert/key 路径（与 sig_start 同一段） */
    char cert[256] = {0}, key[256] = {0};
    const char *sp2 = NULL;
    cfg_get_str(cfg, "net.tls_cert", &sp2, NULL);
    if (sp2) cfg_resolve_path(cfg, sp2, cert, sizeof(cert));
    sp2 = NULL;
    cfg_get_str(cfg, "net.tls_key", &sp2, NULL);
    if (sp2) cfg_resolve_path(cfg, sp2, key, sizeof(key));

    if (!cert[0] || !key[0]) {
        llog(LOG_WARN, "sig", "tls reload skipped: cert/key path empty");
        return NSBC_ERR;
    }

    tls_cfg_t tlc = {cert, key, NULL, 0, 1};
    tls_ctx_t *new_tls = tls_ctx_server_new(&tlc);
    if (!new_tls) {
        llog(LOG_ERR, "sig", "tls reload failed: new ctx create failed (cert=%s)", cert);
        return NSBC_ERR;
    }

    /* 原子替换：所有引用点一次性切到新 ctx */
    tls_ctx_t *old = s->tls;
    s->tls = new_tls;
    sip_tr_set_tls_ctx(s->tr, new_tls);
    ws_listener_set_tls_ctx(new_tls);

    /* 释放旧 ctx（OpenSSL 1.1+ 引用计数：老连接还活着就延迟 free） */
    tls_ctx_free(old);

    llog(LOG_NOTICE, "sig", "✅ TLS cert hot-reloaded (cert=%s)", cert);
    return NSBC_OK;
}

static int sig_start(struct nsbc_ctx *ctx)
{
    sig_runtime_t *s = (sig_runtime_t *)ctx->sig_rt;
    cfg_t *cfg = ctx->cfg;

    long udp_port = 5060, tcp_port = 5060, tls_port = 5061;
    cfg_get_int(cfg, "net.sip_udp_port", &udp_port, NULL);
    cfg_get_int(cfg, "net.sip_tcp_port", &tcp_port, NULL);
    cfg_get_int(cfg, "net.sip_tls_port", &tls_port, NULL);
    int en_udp = 1, en_tcp = 1, en_tls = 0, reuseport = 1;
    cfg_get_bool(cfg, "sip.enable_udp", &en_udp, NULL);
    cfg_get_bool(cfg, "sip.enable_tcp", &en_tcp, NULL);
    cfg_get_bool(cfg, "net.tls_enabled", &en_tls, NULL);
    cfg_get_bool(cfg, "net.reuseport", &reuseport, NULL);
    long rcvbuf = 4096, sndbuf = 4096, maxmsg = 16384, connmax = 100000;
    cfg_get_int(cfg, "net.rcvbuf_kb", &rcvbuf, NULL);
    cfg_get_int(cfg, "net.sndbuf_kb", &sndbuf, NULL);
    cfg_get_int(cfg, "net.sip_max_msg", &maxmsg, NULL);
    cfg_get_int(cfg, "net.tcp_conn_max", &connmax, NULL);
    char listen_ip_cpy[64] = "0.0.0.0";
    {
        const char *listen_ip = NULL;
        cfg_get_str(cfg, "net.sip_listen_ip", &listen_ip, NULL);
        if (listen_ip && listen_ip[0])
            snprintf(listen_ip_cpy, sizeof(listen_ip_cpy), "%s", listen_ip);
    }

    if (en_tls) {
        /* cert/key 同为 cfg 字符串：临时窗会互相覆盖，各自拷贝。 */
        char cert[256] = {0}, key[256] = {0};
        const char *sp2 = NULL;
        if (cfg_get_str(cfg, "net.tls_cert", &sp2, NULL) == NSBC_OK)
            cfg_resolve_path(cfg, sp2, cert, sizeof(cert));
        sp2 = NULL;
        if (cfg_get_str(cfg, "net.tls_key", &sp2, NULL) == NSBC_OK)
            cfg_resolve_path(cfg, sp2, key, sizeof(key));
        tls_cfg_t tlc = {cert, key, NULL, 0, 1};
        s->tls = tls_ctx_server_new(&tlc);
        if (!s->tls) {
            llog(LOG_CRIT, "sig", "TLS ctx create failed (cert=%s)", cert);
            return NSBC_ERR;
        }
    }

    sip_tr_cfg_t trc;
    memset(&trc, 0, sizeof(trc));
    trc.re = ctx->re;
    trc.listen_ip = listen_ip_cpy;
    trc.udp_port = (uint16_t)udp_port;
    trc.tcp_port = (uint16_t)tcp_port;
    trc.tls_port = (uint16_t)tls_port;
    trc.enable_udp = en_udp;
    trc.enable_tcp = en_tcp;
    trc.enable_tls = en_tls;
    trc.reuseport = reuseport;
    trc.rcvbuf_kb = (int)rcvbuf;
    trc.sndbuf_kb = (int)sndbuf;
    trc.max_msg = (size_t)maxmsg;
    trc.tcp_conn_max = (int)connmax;
    trc.tls_ctx = s->tls;
    trc.rx = b2b_on_rx;
    trc.rx_ud = NULL; /* b2b 构造后用 sip_tr_set_rx_ud 回填 */
    if (sip_tr_create(&trc, &s->tr) != NSBC_OK) {
        llog(LOG_CRIT, "sig", "sip transport create failed");
        return NSBC_ERR;
    }
    ctx->sip_tr = s->tr; /* 挂载到 nsbc_ctx，供 outreg/b2b 等兄弟模块只读查询 */

    /* 启动时同步 profiles 端口到 transport：profiles 可能在 Web 端启动序列里
     * 先于 transport 创建被推送 reload（此时 ctx->sip_tr 还挂 NULL → 跳过）；
     * transport 创建好后这里补做一次，确保所有 profile 配置的端口都被监听。 */
    {
        int src = sip_tr_sync_from_profiles(s->tr);
        llog(LOG_INFO, "sig", "startup sip_tr_sync_from_profiles rc=%d", src);
    }

    /* registrar 需要 transport 发送应答，在 transport 就绪后创建 */
    const char *secret = NULL;
    cfg_get_str(cfg, "sip.digest_secret", &secret, NULL);
    /* cfg_get_str 用 __thread 临时 buffer，后续 cfg_get_int 会覆盖！必须立即 strdup */
    if (secret) secret = strdup(secret);
    llog(LOG_INFO, "sig", "registrar cfg: digest_secret=%s", secret ? secret : "(null)");
    long minexp = 60, defexp = 3600, maxexp = 3600;
    long ipcps = 100, aorcps = 10, gcps = 2000;
    cfg_get_int(cfg, "registrar.min_expires", &minexp, NULL);
    cfg_get_int(cfg, "registrar.default_expires", &defexp, NULL);
    cfg_get_int(cfg, "registrar.max_expires", &maxexp, NULL);
    cfg_get_int(cfg, "registrar.rps_per_ip", &ipcps, NULL);
    cfg_get_int(cfg, "registrar.rps_per_aor", &aorcps, NULL);
    cfg_get_int(cfg, "registrar.rps_global", &gcps, NULL);
    reg_cfg_t rc;
    memset(&rc, 0, sizeof(rc));
    rc.db = s->loc;
    rc.tr = s->tr;
    rc.ha1_cb = ctx->ha1_cb;
    rc.ha1_ud = ctx->ha1_ud;
    rc.realm = s->realm;
    rc.digest_secret = secret;
    rc.nonce_ttl_ms = 300000;
    rc.min_expires = (int)minexp;
    rc.max_expires = (int)maxexp;
    rc.default_expires = (int)defexp;
    rc.ip_cps = (int)ipcps;
    rc.aor_cps = (int)aorcps;
    rc.global_cps = (int)gcps;
    rc.retry_after_s = 30;
    if (reg_core_create(&rc, &s->reg) != NSBC_OK)
        return NSBC_ERR;
    ctx->reg_core = s->reg;

    /* B2BUA：REGISTER 入口挂 registrar，SDP/end 回调挂 media bridge */
    b2b_cfg_t bc;
    memset(&bc, 0, sizeof(bc));
    bc.tr = s->tr;
    bc.dlg = s->dlg;
    bc.txn = s->txn;
    bc.tw = ctx->tw;
    bc.sig_arena = ctx->arena[MEM_DOMAIN_SIGNAL];
    bc.pkt_arena = ctx->arena[MEM_DOMAIN_PACKET];
    long cap = 100000;
    cfg_get_int(cfg, "core.max_calls", &cap, NULL);
    bc.capacity = (int)cap;
    bc.local_host = s->local_host;
    bc.local_udp_port = (int)udp_port;
    /* REQ-NAT-001：出站 NAT 公网 IP（FreeSWITCH ext-sip-ip / ext-rtp-ip 等效）
     * 空字符串=关闭 NAT 改写，出站 Via/Contact/SDP 走 local_host。
     * cfg_get_str 返回 __thread 临时 buffer，立即 strdup 保证生命周期 */
    const char *ext_sip_ip = NULL;
    cfg_get_str(cfg, "sip.external_sip_ip", &ext_sip_ip, NULL);
    if (ext_sip_ip && ext_sip_ip[0]) bc.external_sip_ip = strdup(ext_sip_ip);
    const char *ext_rtp_ip = NULL;
    cfg_get_str(cfg, "sip.external_rtp_ip", &ext_rtp_ip, NULL);
    if (ext_rtp_ip && ext_rtp_ip[0]) bc.external_rtp_ip = strdup(ext_rtp_ip);
    bc.realm = s->realm;
    bc.resolve = resolve_cb;
    bc.resolve_ud = s;
    bc.media = media_confirm_cb;
    bc.media_ud = s->bridge;
    /* Phase 0: 以下六处 b2b_cfg_t 单槽字段（media_view/offer_hook/answer_hook/
     * on_register/on_unmatched_resp/cdr_emit）已全部移到 b2bua.c 全局多槽 hook 链：
     *   b2b_sdp_hook_add(B2B_SDP_OFFER/ANSWER, ...)    ← signaling init 里注册 bridge_offer/answer_hook
     *   b2b_register_hook_add(reg_on_register, ...)    ← signaling init 里注册
     *   b2b_media_view_hook_add(media_view_cb, ...)    ← signaling init 里注册
     *   b2b_resp_hook_add(..., outreg)                 ← outreg_init 自己注册
     *   b2b_cdr_hook_add(..., billing)                 ← billing_init 自己注册
     * b2bua.h L298-300 注释已声明这些字段移除，signaling 不再桥接它们。 */
    bc.on_end = sig_on_end_cb;
    bc.end_ud = s;  /* sig_on_end_cb 需要 s->bridge + s->ctx */
    /* Ring NoAnswer 超时（P0 PBX 基础）：配置单位秒，内部换算 ms；默认 60s */
    long rna_s = 60;
    cfg_get_int(cfg, "sip.ring_noanswer_timeout_s", &rna_s, NULL);
    bc.ring_noanswer_ms = (uint32_t)rna_s * 1000u;
    /* REQ-SIP-DIV-001 P0：302/380 自动重定向 */
    int ar = 1;
    cfg_get_bool(cfg, "sip.auto_redirect_302", &ar, NULL);
    bc.auto_redirect_302 = ar;
    long max_redir = 5;
    cfg_get_int(cfg, "sip.max_redirects", &max_redir, NULL);
    bc.max_redirects = (int)max_redir;
    /* REQ-ARCH-015：全局并发硬限制（默认 0=不限，生产环境建议设值） */
    long max_cc = 0;
    cfg_get_int(cfg, "sip.max_concurrent_calls", &max_cc, NULL);
    bc.max_concurrent_calls = (uint32_t)(max_cc < 0 ? 0 : max_cc);
    /* REQ-ARCH-015：per-AoR 并发硬限制（默认 0=不限） */
    long max_per_aor = 0;
    cfg_get_int(cfg, "sip.max_inflight_per_aor", &max_per_aor, NULL);
    bc.max_inflight_per_aor = (uint32_t)(max_per_aor < 0 ? 0 : max_per_aor);
    /* REQ-SIP-016：早期媒体超时（默认 30s，0=不限制） */
    long early_max = 30;
    cfg_get_int(cfg, "sip.early_media_max_s", &early_max, NULL);
    bc.early_media_max_s = (uint32_t)(early_max < 0 ? 0 : early_max);
    /* P1-12：route_table 指针 → b2bua.c cli_tu INVITE 401/407 challenge 现算 HA1 */
    bc.rt = ctx->rt;
    if (b2b_core_create(&bc, &s->b2b) != NSBC_OK) {
        llog(LOG_CRIT, "sig", "b2b core create failed");
        return NSBC_ERR;
    }
    ctx->b2b = s->b2b;

    /* Phase 0: 四组多槽自注册 —— 把旧硬编码桥接线换成模块自注册
     * （旧 bc.media_view/offer_hook/answer_hook/on_register 字段已从 b2b_cfg_t 删掉）*/
    {
        /* REGISTER 入口 → registrar */
        int arc1 = b2b_register_hook_add(reg_on_register, s->reg);
        llog(LOG_INFO, "sig", "Phase 0: b2b_register_hook_add registrar arc=%d", arc1);

        /* SDP offer/answer → media_bridge */
        int arc2 = b2b_sdp_hook_add(B2B_SDP_OFFER, bridge_offer_hook, s->bridge);
        llog(LOG_INFO, "sig", "Phase 0: b2b_sdp_hook_add OFFER arc=%d", arc2);
        int arc3 = b2b_sdp_hook_add(B2B_SDP_ANSWER, bridge_answer_hook, s->bridge);
        llog(LOG_INFO, "sig", "Phase 0: b2b_sdp_hook_add ANSWER arc=%d", arc3);

        /* Media View → signaling 自己的 media_view_cb（bridge 内部查状态）*/
        int arc4 = b2b_media_view_hook_add(media_view_cb, s->bridge);
        llog(LOG_INFO, "sig", "Phase 0: b2b_media_view_hook_add arc=%d", arc4);
    }

    /* Phase 5 XL: SIP endpoint 注册到 endpoint_registry
     * b2b_server_tu INVITE 分支会先 try_accept，找到就委托 sip_ep_handle_invite
     * （内部解包 sip_invite_ctx_t 然后调 handle_new_invite）。
     * 未找到 endpoint → fallback 到原有 handle_new_invite 路径。 */
    {
        int rc = endpoint_registry_register(&sip_endpoint_ops);
        if (rc == NSBC_OK)
            llog(LOG_INFO, "sig", "Phase 5 XL: sip_endpoint_ops registered");
        else
            llog(LOG_WARN, "sig", "Phase 5 XL: sip_endpoint_ops register rc=%d (may already exist)", rc);
    }

    /* 至此 b2b 就绪，回填两个后置入口（reactor 尚未运行）：
     * 事务层新服务端事务上送口、transport 收包口 */
    txn_mgr_set_server_tu(s->txn, b2b_server_tu, s->b2b);
    sip_tr_set_rx_ud(s->tr, s->b2b);

    /* ---- 分机预填充（P0） ---- */
    const char *exts = NULL;
    cfg_get_str(ctx->cfg, "sip.extensions", &exts, NULL);
    if (exts && exts[0]) {
        ep_extensions_load(exts);
    }

    /* ---- Dialplan 规则加载（P0 + REQ-REC-003 扩展） ----
     * 格式："match_pattern:layer:target[:strip][:add_prefix][:recording=start|stop]" 多条 '|' 分隔
     * layer: 0=EXT, 1=BIZ, 2=OUT
     * 可选第 6 段：recording=start → 命中后自动开启录音；recording=stop → 命中后停止。
     *              也支持裸 recording（等价 start），大小写不敏感。
     * 例："40XX:0:ext-{called}:2::recording=start|90:2:trunk-mgw:0:recording=start" */
    const char *dp_cfg = NULL;
    cfg_get_str(ctx->cfg, "dialplan.rules", &dp_cfg, NULL);
    if (dp_cfg && dp_cfg[0]) {
        dp_table_t *tbl = calloc(1, sizeof(*tbl));
        if (tbl && dp_table_init(tbl, 64) == NSBC_OK) {
            char *dup = strdup(dp_cfg);
            if (dup) {
                uint32_t rid = 1;
                char *saveptr = NULL;
                for (char *rule = strtok_r(dup, "|", &saveptr); rule;
                     rule = strtok_r(NULL, "|", &saveptr)) {
                    while (*rule == ' ' || *rule == '\t') ++rule;
                    if (!*rule) continue;
                    /* 拆分 3-6 段（冒号分隔） */
                    char *f[6] = {0};
                    int fn = 0;
                    f[fn++] = rule;
                    for (char *p = rule; *p && fn < 6; ++p) {
                        if (*p == ':') {
                            *p = '\0';
                            f[fn++] = p + 1;
                        }
                    }
                    if (fn < 3) continue;
                    dp_rule_t r;
                    memset(&r, 0, sizeof(r));
                    r.id = rid++;
                    r.layer = (dp_layer_t)(f[1][0] - '0');
                    if (r.layer >= DP_LAYER_CNT) continue;
                    r.enabled = 1;
                    r.prio = 100;
                    r.perm = 0;
                    r.immediate = 0;
                    strncpy(r.match.pattern, f[0], sizeof(r.match.pattern) - 1);
                    r.match.type = PM_DIGIT;
                    strncpy(r.target, f[2], sizeof(r.target) - 1);
                    if (fn >= 4 && f[3][0])
                        r.strip = (uint8_t)atoi(f[3]);
                    if (fn >= 5 && f[4][0])
                        strncpy(r.add_prefix, f[4], sizeof(r.add_prefix) - 1);
                    /* 第 6 段可选：recording=start/stop 或裸 recording */
                    if (fn >= 6 && f[5][0])
                        r.rec_action = recording_parse_action(f[5]);
                    dp_rule_add(tbl, &r);
                }
                free(dup);
            }
            dp_table_sort(tbl);
            dp_table_t *old = NULL;
            dp_ctx_publish(&s->dp_ctx, tbl, &old);
            dp_table_free(old);
            llog(LOG_INFO, "sig", "dialplan loaded %zu rules (from cfg dialplan.rules)", tbl->count);
        }
    }

    llog(LOG_INFO, "sig", "dialplan pre-check: active=%p", (void*)s->dp_ctx.active);

    /* §15 Fallback：两条路径都没加载（active 仍 NULL）→ 注入默认 internal 规则表。
     * 规则含义：所有 INVITE → extension:<called>，profile=internal。
     * 保证 C 侧启动后 dp_execute 永不为空，nsbc-web 手动 reload 会 RCU 原子替换掉。 */
    if (s->dp_ctx.active == NULL) {
        llog(LOG_NOTICE, "sig", "dialplan fallback: active was NULL, building default rules");
        dp_table_t *tbl = calloc(1, sizeof(*tbl));
        if (tbl && dp_table_init(tbl, 2) == NSBC_OK) {
            dp_rule_t r;
            memset(&r, 0, sizeof(r));
            r.enabled = 1;
            r.prio = 100;
            r.perm = 0;
            snprintf(r.profile_name, sizeof(r.profile_name), "internal");
            snprintf(r.target, sizeof(r.target), "extension:${called}");
            pm_init(&r.match, PM_RANGE, "", 1, 32);
            dp_rule_add(tbl, &r);

            dp_table_sort(tbl);
            llog(LOG_INFO, "sig", "dialplan fallback table ready: count=%zu rules[0].profile='%s' type=%d min_len=%d max_len=%d",
                 tbl->count, tbl->rules[0].profile_name, tbl->rules[0].match.type,
                 tbl->rules[0].match.min_len, tbl->rules[0].match.max_len);
            dp_table_t *old = NULL;
            dp_ctx_publish(&s->dp_ctx, tbl, &old);
            dp_table_free(old);
            llog(LOG_NOTICE, "sig", "dialplan fallback: published active=%p", (void*)s->dp_ctx.active);
        } else {
            llog(LOG_ERR, "sig", "dialplan fallback FAILED: tbl=%p init_rc=%d",
                 (void*)tbl, tbl ? dp_table_init(tbl, 2) : -999);
            if (tbl) dp_table_free(tbl);
        }
    } else {
        llog(LOG_NOTICE, "sig", "dialplan pre-loaded: active=%p count=%zu first_profile='%s'",
             (void*)s->dp_ctx.active, s->dp_ctx.active->count,
             s->dp_ctx.active->count > 0 ? s->dp_ctx.active->rules[0].profile_name : "");
    }

    sweep_arm(s);
    llog(LOG_NOTICE, "sig",
         "signaling up udp=%s:%ld tcp=%ld tls=%d realm=%s media=%s",
         listen_ip_cpy, en_udp ? udp_port : 0, en_tcp ? tcp_port : 0,
         en_tls ? (int)tls_port : 0, s->realm, s->local_host);

    /* outreg 在此之后绑定 transport + local_host（outreg init 在 level 3，
     * 此时 ctx->sip_tr 还是 NULL；signaling 在 level 4 才创建 transport）。 */
    outreg_bind_transport(s->tr, s->local_host);

    /* ---- STUN Server（REQ-WEBRTC-001 Phase 1） ---- */
    {
        int stun_enabled = 1;
        long stun_port = 3478;
        cfg_get_bool(ctx->cfg, "nat.stun.enabled", &stun_enabled, NULL);
        cfg_get_int(ctx->cfg, "nat.stun.port", &stun_port, NULL);
        stun_server_init((uint16_t)stun_port, stun_enabled);
        stun_server_start();
    }

    /* ---- WebSocket Listener (RFC 7118) ----
     * WS rx 回调：sig_ws_rx → 把 ws_conn* 塞进 nsbc_sa_t padding，
     * proto 标记 SIP_TR_WS/WSS → sip_tr_send 走 WS 发送分支。 */
    {
        int ws_enabled = 1, ws_tls = 1;
        long ws_port = 5062;
        cfg_get_bool(ctx->cfg, "sip.ws.enabled", &ws_enabled, NULL);
        cfg_get_int(ctx->cfg, "sip.ws.port", &ws_port, NULL);
        cfg_get_bool(ctx->cfg, "sip.ws.tls", &ws_tls, NULL);
        void *ws_tls_ctx = ws_tls ? s->tls : NULL;   /* 复用 SIP TLS ctx */
        sig_ws_bridge_init(s->tr, ws_tls ? SIP_TR_WSS : SIP_TR_WS);
        ws_listener_init((uint16_t)ws_port, ws_tls, ws_enabled, ws_tls_ctx,
                         sig_ws_bridge_rx, ctx);
        ws_listener_start();
    }

    return NSBC_OK;
}

static void sig_stop(struct nsbc_ctx *ctx)
{
    sig_runtime_t *s = (sig_runtime_t *)ctx->sig_rt;
    if (!s)
        return;
    tw_del(ctx->tw, &s->sweep_node);

    /* Phase 0: 先卸载所有 hook（阻止新回调进来），再 destroy b2b/reg/bridge */
    {
        b2b_register_hook_rm(reg_on_register, s->reg);
        b2b_sdp_hook_rm(B2B_SDP_OFFER, bridge_offer_hook, s->bridge);
        b2b_sdp_hook_rm(B2B_SDP_ANSWER, bridge_answer_hook, s->bridge);
        b2b_media_view_hook_rm(media_view_cb, s->bridge);
        llog(LOG_INFO, "sig", "Phase 0: all b2b hooks unregistered");
    }

    /* Phase 5 XL: 先 unregister SIP endpoint（阻止新 INVITE 进来），再 destroy b2b */
    {
        int rc = endpoint_registry_unregister("sip");
        if (rc == NSBC_OK)
            llog(LOG_INFO, "sig", "Phase 5 XL: sip_endpoint_ops unregistered");
        else
            llog(LOG_WARN, "sig", "Phase 5 XL: sip_endpoint_ops unregister rc=%d", rc);
    }

    if (s->b2b) {
        fprintf(stderr, "[sig_stop] b2b_core_destroy... "); fflush(stderr);
        b2b_core_destroy(s->b2b);
        s->b2b = NULL;
        ctx->b2b = NULL;
        fprintf(stderr, "done\n"); fflush(stderr);
    }
    if (s->reg) {
        fprintf(stderr, "[sig_stop] reg_core_destroy... "); fflush(stderr);
        reg_core_destroy(s->reg);
        s->reg = NULL;
        ctx->reg_core = NULL;
        fprintf(stderr, "done\n"); fflush(stderr);
    }
    if (s->tr) {
        fprintf(stderr, "[sig_stop] sip_tr_destroy... "); fflush(stderr);
        sip_tr_destroy(s->tr);
        s->tr = NULL;
        fprintf(stderr, "done\n"); fflush(stderr);
    }
    if (s->tls) {
        fprintf(stderr, "[sig_stop] tls_ctx_free... "); fflush(stderr);
        tls_ctx_free(s->tls);
        s->tls = NULL;
        fprintf(stderr, "done\n"); fflush(stderr);
    }
    /* ---- STUN Server 停在 transport 销毁之后 ---- */
    fprintf(stderr, "[sig_stop] stun_server_stop... "); fflush(stderr);
    stun_server_stop();
    fprintf(stderr, "done\n"); fflush(stderr);
    /* ---- WebSocket Listener ---- */
    fprintf(stderr, "[sig_stop] ws_listener_stop... "); fflush(stderr);
    ws_listener_stop();
    fprintf(stderr, "done\n"); fflush(stderr);
    sig_ws_bridge_shutdown();
}

static void sig_fini(struct nsbc_ctx *ctx)
{
    sig_runtime_t *s = (sig_runtime_t *)ctx->sig_rt;
    if (!s)
        return;
    /* start 失败路径下 stop 可能未被调用，此处幂等兜底 */
    if (s->tr || s->reg || s->b2b || s->tls)
        sig_stop(ctx);
    if (s->txn)
        txn_mgr_destroy(s->txn);
    if (s->dlg)
        dlg_mgr_destroy(s->dlg);
    if (s->loc)
        loc_db_destroy(s->loc);
    if (s->bridge)
        bridge_mgr_destroy(s->bridge);
    if (s->ptable)
        ptable_destroy(s->ptable);
    dp_ctx_fini(&s->dp_ctx);
    ctx->sig_rt = NULL;
    ctx->b2b = NULL;
    ctx->loc_db = NULL;
    ctx->bridge = NULL;
    ctx->txn_mgr = NULL;
    ctx->ptable = NULL;
    free(s);
}

/* =====================================================================
 * sig_sdp_extract_cdr —— 从 answer SDP 提取首个 audio media 的 codec 名 + peer addr:port
 *
 * 零拷贝、幂等、P0 降级友好：
 *   - sdp_parse 失败 → 全空，返回 -1
 *   - 无 audio media 但有 video → 取 video（fallback，极少数场景如 T.38）
 *   - 所有 media port=0（hold/rejected）→ peer 为空
 *   - codec build 失败 → codec 为空
 * ===================================================================== */
static int sig_sdp_extract_cdr(const char *sdp_buf, size_t sdp_len,
                                char *codec_out, size_t codec_sz,
                                char *peer_out,  size_t peer_sz)
{
    sdp_t sdp;
    if (!sdp_buf || sdp_len == 0 || !codec_out || !peer_out)
        return -1;
    codec_out[0] = '\0';
    peer_out[0]  = '\0';

    nsbc_rc_t rc = sdp_parse(&sdp, sdp_buf, sdp_len);
    if (rc != NSBC_OK) return -1;

    /* 找首个 audio media（port>0 / !rejected），找不到 fallback video */
    const sdp_media_t *target = NULL;
    for (int i = 0; i < sdp.nmedia; i++) {
        const sdp_media_t *m = &sdp.media[i];
        if (m->port == 0 || m->rejected) continue;
        if (m->type == SDP_M_AUDIO) { target = m; break; }
    }
    if (!target) {
        for (int i = 0; i < sdp.nmedia; i++) {
            const sdp_media_t *m = &sdp.media[i];
            if (m->port == 0 || m->rejected) continue;
            if (m->type == SDP_M_VIDEO || m->type == SDP_M_TEXT) { target = m; break; }
        }
    }
    if (!target) return -1;

    /* codec：lazy build → 取首个 negotiated codec 的 enc name（rtpmap 原始令牌） */
    if (sdp_media_build_codecs(&sdp, (sdp_media_t *)target) == NSBC_OK && target->ncodecs > 0) {
        const sip_tok_t *enc = &target->codecs[0].enc;
        if (enc->p && enc->len > 0 && enc->len < codec_sz) {
            memcpy(codec_out, enc->p, enc->len);
            codec_out[enc->len] = '\0';
        }
    }

    /* peer IP:port —— 先查 media 级 conn（c=IN IP4 ...），缺则回落会话级 */
    const sdp_conn_t *conn = sdp_media_effective_conn(&sdp, (sdp_media_t *)target);
    if (conn && conn->addr.p && conn->addr.len > 0) {
        /* hold 状态：conn.addr 是 0.0.0.0 或 ::，跳过 */
        int is_hold = 0;
        if (conn->addrtype.p && conn->addrtype.len == 3 &&
            strncmp(conn->addrtype.p, "IP6", 3) == 0) {
            /* IPv6：:: */
            if (conn->addr.len == 2 && conn->addr.p[0] == ':' && conn->addr.p[1] == ':')
                is_hold = 1;
        } else {
            /* IPv4：0.0.0.0 */
            if (conn->addr.len == 7 && strncmp(conn->addr.p, "0.0.0.0", 7) == 0)
                is_hold = 1;
        }
        if (!is_hold && target->port > 0) {
            snprintf(peer_out, peer_sz, "%.*s:%d",
                     (int)conn->addr.len, conn->addr.p, target->port);
        }
    }
    return 0;
}

static const char *sig_deps[] = {NULL}; /* P0 去掉 endpoints 依赖（mod/ 未纳入构建） */

/* dp 上下文访问器（stub_mods.c 的 dp_rules_reload_json 经此换表；
 * 非 static：core 层内跨文件，签名不入公共头避免暴露 sig_runtime_t）。
 * sig_rt 未挂载（signaling init 前/fini 后）返回 NULL。 */
dp_ctx_t *sig_dp_ctx(void)
{
    nsbc_ctx_t *c = nsbc_get_ctx();
    sig_runtime_t *s = c ? (sig_runtime_t *)c->sig_rt : NULL;
    return s ? &s->dp_ctx : NULL;
}

/* ================================================================
 * WS Bridge: ws_rx_fn → b2b_on_rx(SIP_TR_WS/WSS, ...)
 *
 * conn 指针藏在 nsbc_sa_t 的 sockaddr_storage padding 里：
 *   ss_family = AF_UNSPEC (0)
 *   最后 8B (__ss_padding 或 __ss_align) 存 ws_conn*
 *
 * sip_tr_send 里看到 proto=SIP_TR_WS && family=AF_UNSPEC →
 * 取出 conn → ws_conn_send()。
 * ================================================================ */

static sip_transport_t *g_ws_bridge_tr = NULL;
sip_tr_proto_t   g_ws_bridge_proto = SIP_TR_WS;

static void sig_ws_bridge_init(sip_transport_t *tr, sip_tr_proto_t proto)
{
    g_ws_bridge_tr = tr;
    g_ws_bridge_proto = proto;
}

static void sig_ws_bridge_shutdown(void)
{
    g_ws_bridge_tr = NULL;
}

static void sig_ws_bridge_rx(void *ud, const char *buf, size_t len, void *conn)
{
    /* ud = ctx（ws_listener_init 传入）。直接用 ctx->b2b（L1447 已赋值），
     * 不绕 ctx->sig_rt→s→b2b（那条链理论上等价，但历史上出现过 b2b core not ready 的 drop）。
     * ctx->b2b 和 TCP SIP 入口用的是同一个 b2b_core_t*。 */
    struct nsbc_ctx *ctx = (struct nsbc_ctx *)ud;
    b2b_core_t *b2b = ctx ? (b2b_core_t *)ctx->b2b : NULL;
    if (!b2b) {
        llog(LOG_WARN, "ws", "rx drop: b2b core not ready (%zuB)", len);
        return;
    }

    /* 构造 src — 藏 ws_conn* */
    nsbc_sa_t src;
    nsbc_sa_clear(&src);
    struct sockaddr_storage *ss = &src.ss;
    ss->ss_family = AF_UNSPEC;
    /* 把 conn 指针塞进 padding 区（sockaddr_storage 至少 128B）
     * 注意：conn 是 void*，必须用 sizeof(conn)（指针本身大小），
     * 不能用 sizeof(*conn) — void* 解引用是 void，sizeof(void)==1！ */
    size_t sz = sizeof(conn);
    memcpy(((char *)ss) + sizeof(struct sockaddr_storage) - sz, &conn, sz);
    llog(LOG_INFO, "ws-bridge", "rx: conn=%p stored, family=%d, sizeof_ss=%zd",
         conn, ss->ss_family, sizeof(*ss));

    /* local 填占位 */
    nsbc_sa_t local;
    nsbc_sa_clear(&local);

    llog(LOG_DEBUG, "ws", "rx %zuB SIP msg via %s", len,
         g_ws_bridge_proto == SIP_TR_WSS ? "WSS" : "WS");

    b2b_on_rx(b2b, g_ws_bridge_proto, &src, &local, buf, len);
}

/* ws_conn* → AF_UNSPEC nsbc_sa_t（藏指针进 padding，与 sig_ws_bridge_rx 对称）。
 * resolve_cb 用它把被叫路由到 WS 注册连接（RFC 7118 .invalid Contact）。
 * 返回值同 uri_to_sa 口径：NSBC_OK / 失败码。 */
static int sa_hide_ws_conn(nsbc_sa_t *sa, void *conn)
{
    if (!sa || !conn)
        return NSBC_ERR;
    nsbc_sa_clear(sa);
    sa->ss.ss_family = AF_UNSPEC; /* sip_tr_send WS 分支的约定标志 */
    size_t sz = sizeof(conn);
    memcpy(((char *)&sa->ss) + sizeof(struct sockaddr_storage) - sz, &conn, sz);
    return NSBC_OK;
}

/* 供 sip_tr_send WS 分支调用：从 dst padding 取出 ws_conn* */
void *sig_ws_bridge_conn_from_sa(const nsbc_sa_t *sa)
{
    if (!sa) {
        llog(LOG_WARN, "ws-bridge", "conn_from_sa: sa=NULL");
        return NULL;
    }
    llog(LOG_INFO, "ws-bridge", "conn_from_sa: ss_family=%d sizeof_sa=%zd",
         sa->ss.ss_family, sizeof(*sa));
    if (sa->ss.ss_family != AF_UNSPEC) {
        llog(LOG_WARN, "ws-bridge", "conn_from_sa: family not AF_UNSPEC (%d), skip",
             sa->ss.ss_family);
        return NULL;
    }
    size_t sz = sizeof(void *);
    void *conn = NULL;
    memcpy(&conn, ((const char *)&sa->ss) + sizeof(struct sockaddr_storage) - sz, sz);
    llog(LOG_INFO, "ws-bridge", "conn_from_sa: extracted conn=%p", conn);
    return conn;
}

/* REQ-AI-001：供 ai_dialog / 未来 audio-bridge 等可选模块拿到 bridge_mgr 实例。
 * signaling 是 REQUIRED STATIC 模块，bridge 在 sig_start 时已创建——ai_dialog 是
 * DYNAMIC 模块 dlopen 在此之后，所以桥已存在。模块 shutdown（进程退出）时
 * signaling 还在，ai_dialog 的 hook 回调不会到已释放的 bridge。 */
bridge_mgr_t *sig_bridge_instance(void)
{
    nsbc_ctx_t *ctx = nsbc_get_ctx();
    if (!ctx || !ctx->sig_rt) return NULL;
    sig_runtime_t *s = (sig_runtime_t *)ctx->sig_rt;
    return s->bridge;
}

NSBC_MOD_CAT(signaling, 1, MOD_L_FINAL,
         MOD_F_CALL_PATH | MOD_F_REQUIRED, MOD_CAT_CORE, sig_deps, sig_init, sig_start,
         sig_stop, sig_fini);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
