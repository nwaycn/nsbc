/* 实时控制接口（REQ-OAM-004/005/007，P0）
 *
 * UNIX 域 stream socket（oam.control_sock，文件权限 0600：仅本机特权
 * 运维身份接入；TLS/令牌分级鉴权随 TCP 口与 Web 边车在 P1 补齐）。
 * 文本行命令，回车结束：
 *   help / version / stats / events [N] / show calls / show channels
 *   show registrations / uuid_dump <call-id>
 *   config.get <key> / config.set <k> <v> / config.validate <k> <v>
 *   config.export / config.snapshots / config.reload（别名 reload）
 *   config.rollback [snapshot-id]
 *   hangup <call-id> / quit
 * 三入口（CLI/REST/实时）共用同一命令核心，P0 在此提供只读子集（含
 * REQ-OAM-007/010 的 uuid_dump 双侧 Leg 诊断，L0；config.get 生效值查询、
 * config.validate dry-run、config.export 脱敏全量、config.snapshots
 * 快照列举，L0，敏感键 **** 脱敏）、hangup（L1，REQ-OAM-004，全程审计）、
 * config.set（L1，REQ-CFG-011，仅白名单非敏感键，审计不记值）、
 * config.rollback（L1，REQ-CFG-008/011，无参=最新份）与 config.reload
 * （L1，REQ-CFG-011，事务化热加载；黑名单键 need-restart 不静默忽略）
 * 均成败审计；其余高危命令（L1/L2）在 P1 走统一命令注册表+二次授权。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "app_ctx.h"
#include "b2bua.h"
#include "cdr.h"
#include "cfg.h"
#include "cmd_registry.h"
#include "dp_sleep.h"     /* REQ-AI-001: dp_sleep_app_reg → show apps */
#include "app_registry.h" /* REQ-AI-001: app_help / app_count */
#include "location_db.h"
#include "log.h"
#include "media_bridge.h"
#include "metric_reg.h"
#include "module.h"       /* Phase 5: show modules / show event_bus */
#include "event_bus.h"    /* Phase 5: show event_bus 内核状态 */
#include "media_codec.h"  /* Phase 5: show codecs registry */
#include "cdr_wal.h"      /* Phase 6: show cdr_wal */
#include "endpoint_registry.h" /* Phase 审计: show endpoints */
#include "ai_backend_registry.h" /* Phase R-14: show ai_backends */
#include "obs.h"
#include "obs_sub.h"
#include "oam_internal.h"
#include "rating.h"
#include "reactor.h"
#include "cfg_sync.h"     /* P0: cfg_sync_after_reload — plain reload 后拉 DB 同步 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define NSBC_VERSION "2.6.0"
/* CTL_BUF_MAX：UDS 命令行接收缓冲（堆上 calloc，非栈）。
 *  - 原 8192 只够 ~40 条费率的 JSON reload payload（每条 ~200B），
 *    真实规模 200+ 条会触发 "超长无换行丢弃" 静默失败。
 *  - 提至 256KB 与 CTL_OUT_MAX 同口径，支持批量 reload。 */
#define CTL_BUF_MAX (256 * 1024)
#define CTL_OUT_MAX (256 * 1024)
/* uuid_dump：两 Leg × 各 2KB SDP + 头行，16KB 足够 */
#define CTL_DUMP_MAX (16 * 1024)

typedef struct {
    int fd;
    char buf[CTL_BUF_MAX];
    size_t n;
    unsigned sub_topics;   /* 订阅的 topic 位图（1<<obs_topic_t）；0=无订阅 */
} ctl_conn_t;

static int g_lfd = -1;
static char g_path[108];
static nsbc_ctx_t *g_ctx;
static cmd_reg_t *g_cmd_reg;  /* CCMD 统一注册表 */
static obs_sub_hub_t *g_sub_hub;  /* 订阅推送 hub（events/cdr/log/metrics） */

/* obs_sub send 适配：conn = (ctl_conn_t*)，从 c->fd 非阻塞 send。
 * obs_sub flush_one_locked 约定：返回 0 = 全部发送成功，
 * 返回非 0 = break（暂时发不出 / 部分发送 / 连接失效）。
 * ★ EAGAIN 必须返回非 0（-1）让 flush break，帧留在环里等 EPOLLOUT 再试。
 *   同时 reactor_mod 加上 EPOLLOUT，避免卡在环里没触发 flush 的机会。 */
static int obs_control_sub_send(void *conn, const void *data, size_t len)
{
    ctl_conn_t *c = (ctl_conn_t *)conn;
    if (c == NULL || c->fd < 0) return -1;
    ssize_t w = send(c->fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (w < 0) {
        /* EAGAIN/EWOULDBLOCK: kernel buffer 满了 → 帧留在环里 → 加 EPOLLOUT */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (g_ctx && g_ctx->re)
                reactor_mod(g_ctx->re, c->fd, EPOLLIN | EPOLLOUT);
        }
        return -1;
    }
    if ((size_t)w < len) {
        /* 部分发送：kernel buffer 即将满 → 也加 EPOLLOUT 保险 */
        if (g_ctx && g_ctx->re)
            reactor_mod(g_ctx->re, c->fd, EPOLLIN | EPOLLOUT);
        return -1;
    }
    return 0;  /* 全部发送成功 */
}

static void ctl_write(ctl_conn_t *c, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(c->fd, p, n, MSG_NOSIGNAL);
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

static void ctl_printf(ctl_conn_t *c, const char *fmt, ...)
{
    char out[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, sizeof(out), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if ((size_t)n >= sizeof(out))
            n = (int)sizeof(out) - 1; /* 截断：绝不越界读 */
        ctl_write(c, out, (size_t)n);
    }
}

/* cmd_ctx_t.io_printf 适配器（cmd_registry 要求 int 返回；ctl_printf 返回 void） */
static int ctl_io_printf(void *ud, const char *fmt, ...)
{
    ctl_conn_t *c = (ctl_conn_t *)ud;
    char out[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, sizeof(out), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if ((size_t)n >= sizeof(out))
            n = (int)sizeof(out) - 1;
        ctl_write(c, out, (size_t)n);
    }
    return n;
}

static void ctl_help(ctl_conn_t *c)
{
    ctl_printf(c,
        "commands:\n"
        "  help                 this help\n"
        "  version              core version\n"
        "  stats                prometheus exposition snapshot\n"
        "  events [N]           last N events (default 20)\n"
        "  show calls           active/total/failed calls\n"
        "  show channels        per-call list (A/B leg Call-ID and state)\n"
        "  show registrations   current binding count\n"
        "  show apps            registered dialplan applications (answer/ai/bridge/...)\n"
        "  show rates           billing rate table (L0)\n"
        "  show balances        account balances (L0)\n"
        "  show transcode       active media bridge sessions (L0)\n"
        "  rate.reload <json>   full rate table swap from DB (L1, audited)\n"
        "  rate.upsert <json>   single rate entry upsert (L1, audited)\n"
        "  rate.delete <prefix> remove one rate entry (L1, audited)\n"
        "  account.open <key> <balance_uc> <credit_limit_uc> (L1, audited)\n"
        "  account.topup <key> <delta_uc>  top up balance (L1, audited)\n"
        "  accounts.reload <json>  DB accounts full upsert (L1, audited)\n"
        "  uuid_dump <call-id>  per-leg diagnostic dump (L0)\n"
        "  cdr.recent [N]       last N finished CDRs from in-memory ring (default 500, L0)\n"
        "  cdr.flush            flush pending CDR ring to persist/spool (L1, audited)\n"
        "  show codecs          registered audio/video codec table (L0)\n"
        "  show cdr             cdr sink aggregate counters (L0)\n"
        "  log dump [N]         last N log lines from ring buffer (default 200)\n"
        "  log stats            dropped/written counter (debug)\n"
        "  config.get <key>     effective config value (L0, secrets ****)\n"
        "  config.set <k> <v>   hot runtime key set (L1, audited)\n"
        "  config.validate <k> <v>  dry-run value check (L0)\n"
        "  config.export        full redacted effective snapshot (L0)\n"
        "  config.snapshots     list rollback snapshots, newest first (L0)\n"
        "  config.rollback [id] rollback to newest|given snapshot (L1, audited)\n"
        "  config.reload        transactional hot-reload (L1, audited)\n"
        "  hangup <call-id>     force-terminate a call by Call-ID (L1, audited)\n"
        "  reload               alias of config.reload\n"
        "  sub <topics>         订阅实时推送（events/cdr/log/metrics，逗号分隔；如 sub events,cdr）\n"
        "  unsub <topics>       取消订阅（无参取消全部）\n"
        "  sub.stat             查看本连接订阅状态\n"
        "  quit                 close session\n");
    /* 注册表命令（call_commands / reload_commands 启动装配）追加列出 */
    if (g_cmd_reg) {
        /* UDS 控制通道走 root-only socket (0600)，等价 L2 admin 信任边界 */
        cmd_ctx_t ctx = { NULL, 2, 1, ctl_io_printf, c };
        ctl_printf(c, "registry commands:\n");
        cmd_help(g_cmd_reg, &ctx, NULL);
    }
}

static void cmd_stats(ctl_conn_t *c)
{
    char *body = malloc(CTL_OUT_MAX);
    if (!body) {
        ctl_printf(c, "ERR oom\n");
        return;
    }
    int n = metric_render_all(body, CTL_OUT_MAX);
    ctl_write(c, body, (size_t)n);
    free(body);
}

static void cmd_events(ctl_conn_t *c, const char *arg)
{
    int max = 20;
    if (arg && *arg)
        max = atoi(arg);
    if (max <= 0 || max > 500)
        max = 500;
    char *body = malloc((size_t)max * 600 + 16);
    if (!body) {
        ctl_printf(c, "ERR oom\n");
        return;
    }
    int n = obs_events_tail(body, (size_t)max * 600 + 16, max);
    if (n > 0)
        ctl_write(c, body, (size_t)n);
    else
        ctl_printf(c, "(no events)\n");
    free(body);
}

/* Call-ID 白名单（REQ-OAM-004 入校）：非空、长度 1..DLG_CALLID_MAX-1、
 * 仅可见 ASCII（0x21-0x7e）。拒绝空格/控制字符——参数既进审计日志
 * 又拼进响应文本，必须杜绝注入与帧歧义。 */
static int valid_callid(const char *s)
{
    if (!s)
        return 0;
    size_t n = strlen(s);
    if (n == 0 || n >= DLG_CALLID_MAX)
        return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x21 || ch > 0x7e)
            return 0;
    }
    return 1;
}

/* hangup <call-id>（REQ-OAM-004，L1）：同步强制挂断，成败均审计
 * （REQ-OAM-007 参数快照）。命令行身份与细粒度鉴权在 P1 命令注册表补齐。 */
static void cmd_hangup(ctl_conn_t *c, const char *arg)
{
    if (!valid_callid(arg)) {
        ctl_printf(c, "ERR invalid call-id\n");
        log_audit("oam", "control-hangup",
                  "rejected: invalid call-id arg");
        return;
    }
    b2b_core_t *b = (b2b_core_t *)g_ctx->b2b;
    int rc = b ? b2b_terminate_callid(b, arg, strlen(arg), 487)
               : NSBC_NOENT;
    if (rc == NSBC_OK) {
        ctl_printf(c, "OK hungup %s\n", arg);
        log_audit("oam", "control-hangup", "call-id=%s result=ok", arg);
    } else if (rc == NSBC_NOENT) {
        ctl_printf(c, "ERR notfound\n");
        log_audit("oam", "control-hangup",
                  "call-id=%s result=notfound", arg);
    } else {
        ctl_printf(c, "ERR hangup failed\n");
        log_audit("oam", "control-hangup",
                  "call-id=%s result=err rc=%d", arg, rc);
    }
}

/* ---- show rates/balances/transcode 的迭代回调 ---- */

/* 极简 JSON 字符串查找：在 json[0..len) 中找 "key":"..." 的值，
 * 拷贝到 out（out[0]='\0' 表示未找到）。返回 1 找到，0 未找到/格式错。
 * 处理转义字符 \"。 */
static int json_str_get(const char *json, size_t len, const char *key,
                        char *out, size_t out_sz)
{
    size_t klen = strlen(key);
    const char *p = json;
    const char *end = json + len;

    /* 长度守卫：len < klen + 4 时 end-klen-3 会 wrap（size_t 无符号） */
    if (len < klen + 4) { out[0] = '\0'; return 0; }

    while (p < end - klen - 3) {
        if (*p == '"' && memcmp(p + 1, key, klen) == 0 && p[klen + 1] == '"') {
            /* 找冒号（允许空格） */
            p += klen + 2;
            while (p < end && (*p == ' ' || *p == ':')) ++p;
            if (*p != '"') return 0;
            ++p; /* 跳过值起始引号 */
            /* 收集值字符串 */
            size_t n = 0;
            while (p < end && *p != '"' && n < out_sz - 1) {
                if (*p == '\\' && p + 1 < end) {
                    char c = *(p + 1);
                    if (c == '"' || c == '\\' || c == '/') { out[n++] = c; p += 2; continue; }
                    if (c == 'n') { out[n++] = '\n'; p += 2; continue; }
                    if (c == 't') { out[n++] = '\t'; p += 2; continue; }
                    if (c == 'r') { out[n++] = '\r'; p += 2; continue; }
                    /* \uXXXX：简化跳过 */
                    if (c == 'u') { p += 6; continue; }
                }
                out[n++] = *p++;
            }
            out[n] = '\0';
            return 1;
        }
        ++p;
    }
    out[0] = '\0';
    return 0;
}

/* 极简 JSON 数字查找：返回 1 找到，0 未找到。 */
static int json_i64_get(const char *json, size_t len, const char *key, int64_t *out)
{
    size_t klen = strlen(key);
    const char *p = json;
    const char *end = json + len;

    if (len < klen + 4) { *out = 0; return 0; }

    while (p < end - klen - 3) {
        if (*p == '"' && memcmp(p + 1, key, klen) == 0 && p[klen + 1] == '"') {
            p += klen + 2;
            while (p < end && (*p == ' ' || *p == ':')) ++p;
            if (*p != '-' && (*p < '0' || *p > '9')) return 0;
            *out = strtoll(p, NULL, 10);
            return 1;
        }
        ++p;
    }
    *out = 0;
    return 0;
}

/* 极简 JSON bool 查找：返回 1 找到，0 未找到。 */
static int json_bool_get(const char *json, size_t len, const char *key, int *out)
{
    size_t klen = strlen(key);
    const char *p = json;
    const char *end = json + len;

    if (len < klen + 4) { *out = 0; return 0; }

    while (p < end - klen - 3) {
        if (*p == '"' && memcmp(p + 1, key, klen) == 0 && p[klen + 1] == '"') {
            p += klen + 2;
            while (p < end && (*p == ' ' || *p == ':')) ++p;
            if (end - p >= 4 && strncmp(p, "true", 4) == 0) { *out = 1; return 1; }
            if (end - p >= 5 && strncmp(p, "false", 5) == 0) { *out = 0; return 1; }
            return 0;
        }
        ++p;
    }
    *out = 0;
    return 0;
}

/* 解析单个 JSON 对象到 rate_entry_t。返回 0 成功，-1 格式错。 */
static int json_rate_entry(const char *json, size_t len, rate_entry_t *r)
{
    char buf[256];

    if (json_str_get(json, len, "prefix", buf, sizeof(buf)) != 1) return -1;
    if (strlen(buf) >= sizeof(r->prefix)) return -1;
    snprintf(r->prefix, sizeof(r->prefix), "%s", buf);

    if (json_str_get(json, len, "name", buf, sizeof(buf)) == 1)
        snprintf(r->name, sizeof(r->name), "%s", buf);

    json_i64_get(json, len, "price_per_min_uc", &r->price_per_min_uc);
    json_i64_get(json, len, "connect_fee_uc",   &r->connect_fee_uc);
    {
        /* int 字段经 int64_t 中转（json_i64_get 出参类型） */
        int64_t iv = 0;
        json_i64_get(json, len, "bill_inc_s", &iv);  r->bill_inc_s = (int)iv;
        iv = 0; json_i64_get(json, len, "min_bill_s", &iv);  r->min_bill_s = (int)iv;
        iv = 0; json_i64_get(json, len, "initial_s", &iv);  r->initial_s = (int)iv;
    }
    json_bool_get(json, len, "enabled",         &r->enabled);

    /* bill_inc_s 默认 1 */
    if (r->bill_inc_s <= 0) r->bill_inc_s = 1;

    return 0;
}

static int rate_show_cb(const rate_entry_t *r, void *ud)
{
    ctl_conn_t *c = (ctl_conn_t *)ud;
    if (!c || !r) return NSBC_INVAL;
    ctl_printf(c,
        "rate prefix=%s name=%s price_per_min_uc=%lld connect_fee_uc=%lld "
        "bill_inc_s=%d min_bill_s=%d initial_s=%d enabled=%d\n",
        r->prefix, r->name,
        (long long)r->price_per_min_uc, (long long)r->connect_fee_uc,
        r->bill_inc_s, r->min_bill_s, r->initial_s, r->enabled);
    return NSBC_OK;
}

static int acct_show_cb(const char *key, int64_t balance,
                        int64_t reserved, int64_t credit_limit, void *ud)
{
    ctl_conn_t *c = (ctl_conn_t *)ud;
    if (!c || !key) return NSBC_INVAL;
    ctl_printf(c,
        "account key=%s balance_uc=%lld reserved_uc=%lld credit_limit_uc=%lld\n",
        key, (long long)balance, (long long)reserved, (long long)credit_limit);
    return NSBC_OK;
}

static int bridge_show_cb(void *call_key, mb_mode_t mode,
                          uint16_t a_rtp, uint16_t b_rtp,
                          uint64_t pkts, uint64_t drops, void *ud)
{
    /* 结构体 ud：ctl_conn_t* + int idx */
    struct { ctl_conn_t *c; int idx; } *s = ud;
    const char *mode_s;
    if (!s || !s->c) return NSBC_INVAL;
    switch (mode) {
        case MB_BYPASS:    mode_s = "bypass"; break;
        case MB_RELAY:     mode_s = "relay"; break;
        case MB_TRANSCODE: mode_s = "transcode"; break;
        default:           mode_s = "?"; break;
    }
    ctl_printf(s->c,
        "bridge idx=%d call_key=%p mode=%s a_rtp=%u b_rtp=%u pkts=%llu drops=%llu\n",
        s->idx++, call_key, mode_s, a_rtp, b_rtp,
        (unsigned long long)pkts, (unsigned long long)drops);
    return NSBC_OK;
}

/* show registrations 每一行格式化回调：
 *   reg aor=1000@nsbc.local contact=sip:xxx.invalid... expires_sec=3580
 * expires_sec = expire_ms(绝对到期 monotonic) - now_ms(当前 monotonic)，剩余有效秒。 */
int cmd_reg_line_cb(const loc_binding_t *b, void *ud)
{
    typedef struct { char *out; size_t cap; size_t pos; } reg_ud_t;
    reg_ud_t *u = (reg_ud_t *)ud;
    char tmp[LOC_AOR_MAX]; /* 提前声明，避免 GCC 12 dangling-pointer 警告 */
    if (!u || !b) return 1; /* stop */
    if (u->pos + 256 >= u->cap) return 1; /* 缓冲满，截断 */
    /* 提取 AOR 里的纯号码（去掉 @domain） */
    const char *user = b->aor;
    const char *at = strchr(b->aor, '@');
    if (at) {
        size_t ul = at - b->aor;
        if (ul >= sizeof(tmp)) ul = sizeof(tmp) - 1;
        memcpy(tmp, b->aor, ul); tmp[ul] = '\0';
        user = tmp;
    }
    /* 剩余有效秒 */
    int64_t rem = b->expire_ms ? (int64_t)(b->expire_ms - nsbc_now_ms()) / 1000 : 0;
    int64_t alive = 3600 - rem; /* 默认 expires=3600s → 已注册时长 */
    if (alive < 0) alive = 0;
    int64_t q = b->q;
    u->pos += snprintf(u->out + u->pos, u->cap - u->pos,
                       "reg aor=%s user=%s contact=%s expires_sec=%lld alive_sec=%lld q=%lld"
                       " nat=%d transport=%s received_ip=%s received_port=%u pub_contact=%s\n",
                       b->aor, user, b->contact,
                       (long long)rem, (long long)alive, (long long)q,
                       /* REQ-NAT-001：NAT 元数据，拆分 ip/port 独立 kv 便于 Rust split_whitespace 解析 */
                       b->nat_detected,
                       b->transport_proto == 4 ? "ws" :
                       b->transport_proto == 5 ? "wss" :
                       b->transport_proto == 3 ? "tls" :
                       b->transport_proto == 2 ? "tcp" : "udp",
                       b->received_ip[0] ? b->received_ip : "-",
                       (unsigned)b->received_port,
                       b->public_contact[0] ? b->public_contact : b->contact);
    return 0; /* continue */
}

/* show channels（REQ-OAM-007，L0 只读）：每呼叫一行快照+尾行汇总。 */
static void cmd_show_channels(ctl_conn_t *c)
{
    char *body = malloc(CTL_OUT_MAX);
    if (!body) {
        ctl_printf(c, "ERR oom\n");
        return;
    }
    b2b_core_t *b = (b2b_core_t *)g_ctx->b2b;
    if (!b) {
        /* 核心装配未就绪：返回空清单而非错误（与 show calls 计数同口径） */
        ctl_printf(c, "channels=0 truncated=0\n");
        free(body);
        return;
    }
    int n = b2b_list_calls(b, body, CTL_OUT_MAX);
    if (n == NSBC_OK)
        ctl_write(c, body, strlen(body));
    else
        ctl_printf(c, "ERR channels failed\n");
    free(body);
}

/* cdr.recent [N]（REQ-OBS-006 P1，L0 只读）：拉取核心侧最近 N 条已完成 CDR。
 * mod/billing.c 在模块 init 时把 cdr_sink 接到 ctx->cdr_ud；此处强转拿到 sink，
 * 调 cdr_sink_recent() 从内存环（ring）倒序格式化成 Web 侧 parse_cdr_line
 * 能直接解析的帧。环头不挪动（只读），flush 另有独立命令/周期任务。 */
#define CDR_RECENT_MAX 500
static void cmd_cdr_recent(ctl_conn_t *c, char *arg)
{
    int limit = CDR_RECENT_MAX;
    if (arg && *arg) {
        int v = atoi(arg);
        if (v > 0 && v <= CDR_RECENT_MAX)
            limit = v;
    }

    char *body = malloc(CTL_OUT_MAX);
    if (!body) {
        ctl_printf(c, "ERR oom\n");
        return;
    }

    /* ctx->cdr_ud 是 billing 模块塞进来的 cdr_sink_t*；模块未加载则 NULL */
    cdr_sink_t *sink = (cdr_sink_t *)(g_ctx ? g_ctx->cdr_ud : NULL);
    cdr_sink_recent(sink, limit, body, CTL_OUT_MAX);
    ctl_write(c, body, strlen(body));
    free(body);
}

/* cdr.flush（REQ-PBX-CDR-001，L1，审计）：手动触发一次 cdr_sink_flush，
 * 把 ring 里的待持久化 CDR 推给 persist_batch / spool。ring_head 不变
 * 就是空（原子幂等）；flush 本身已加锁，与 emit 并发安全。 */
static void cmd_cdr_flush(ctl_conn_t *c)
{
    cdr_sink_t *sink = (cdr_sink_t *)(g_ctx ? g_ctx->cdr_ud : NULL);
    if (sink == NULL) {
        ctl_printf(c, "ERR cdr sink not initialized\n");
        log_audit("oam", "control-cdr-flush", "result=not-initialized");
        return;
    }
    int rc = cdr_sink_flush(sink);
    if (rc > 0) {
        ctl_printf(c, "OK flushed %d cdrs\n", rc);
        log_audit("oam", "control-cdr-flush", "result=ok n=%d", rc);
    } else if (rc == 0) {
        ctl_printf(c, "OK ring empty, nothing to flush\n");
        log_audit("oam", "control-cdr-flush", "result=empty");
    } else {
        ctl_printf(c, "ERR flush failed rc=%d\n", rc);
        log_audit("oam", "control-cdr-flush", "result=err rc=%d", rc);
    }
}

/* show cdr（REQ-PBX-CDR-001，L0 只读）：聚合统计。 */
static void cmd_show_cdr(ctl_conn_t *c)
{
    cdr_sink_t *sink = (cdr_sink_t *)(g_ctx ? g_ctx->cdr_ud : NULL);
    if (sink == NULL) {
        ctl_printf(c, "cdr=off\n");
        return;
    }
    ctl_printf(c,
        "cdr ring=%d open=%d emitted=%llu persisted=%llu spooled=%llu dropped=%llu\n",
        cdr_sink_pending_count(sink),  /* ring 内 recent 条数（flush 不清 ring，poller 兜底用）*/
        cdr_sink_open_count(sink),
        (unsigned long long)cdr_sink_total_emitted(sink),
        (unsigned long long)cdr_sink_total_persisted(sink),
        (unsigned long long)cdr_sink_total_spooled(sink),
        (unsigned long long)cdr_sink_total_dropped(sink));
}

/* uuid_dump <call-id>（REQ-OAM-007/010，L0 只读）：双侧 Leg 诊断视图。
 * 不审计（只读命令，与 show/stats 同口径）；参数仍走白名单防帧注入。 */
static void cmd_uuid_dump(ctl_conn_t *c, const char *arg)
{
    if (!valid_callid(arg)) {
        ctl_printf(c, "ERR invalid call-id\n");
        return;
    }
    char *body = malloc(CTL_DUMP_MAX);
    if (!body) {
        ctl_printf(c, "ERR oom\n");
        return;
    }
    b2b_core_t *b = (b2b_core_t *)g_ctx->b2b;
    int rc = b ? b2b_dump_call(b, arg, strlen(arg), body, CTL_DUMP_MAX)
               : NSBC_NOENT;
    if (rc == NSBC_OK) {
        ctl_write(c, body, strlen(body));
    } else if (rc == NSBC_NOENT) {
        ctl_printf(c, "ERR notfound\n");
    } else if (rc == NSBC_INVAL) {
        ctl_printf(c, "ERR invalid call-id\n");
    } else {
        ctl_printf(c, "ERR dump failed\n");
    }
    free(body);
}

/* 配置键白名单（REQ-CFG-011 入校）：长度 1..127，字符集 [A-Za-z0-9._-]，
 * 不以点起/止、不含 ".."。键既进响应帧又用于层查找，必须杜绝空白/控制
 * 字符造成的帧注入。 */
static int valid_cfg_key(const char *s)
{
    if (!s)
        return 0;
    size_t n = strlen(s);
    if (n == 0 || n >= 128 || s[0] == '.' || s[n - 1] == '.')
        return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)s[i];
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                 ch == '-';
        if (!ok)
            return 0;
    }
    return strstr(s, "..") == NULL;
}

/* config.reload（REQ-CFG-011，L1）：同步事务化热加载，与 SIGHUP 同源
 * （cfg_reload_file 内部完成 解析→黑名单→原子发布，失败整体回滚）。
 * 黑名单键被拒时 cfg 发 CFG_RELOAD_FAILED 事件（detail 带具体键）并返回
 * PERM_DENIED；此处明确回 need-restart，不静默忽略（REQ-CFG-007）。 */
static void cmd_reload(ctl_conn_t *c)
{
    if (!g_ctx->cfg || !g_ctx->config_file) {
        ctl_printf(c, "ERR reload unavailable\n");
        log_audit("oam", "control-reload", "rejected: config source unavailable");
        return;
    }
    const char *bad = NULL;
    int rc = cfg_reload_file(g_ctx->cfg, g_ctx->config_file, &bad);
    if (rc == NSBC_OK) {
        ctl_printf(c, "OK reload success %s\n", g_ctx->config_file);
        log_audit("oam", "control-reload", "result=ok file=%s",
                  g_ctx->config_file);

        /* P0: YAML reload 成功后，主动从 nsbc-web DB 拉全量配置同步。
         * 解决两套 reload 通道（YAML vs DB）互不关联的架构问题——
         * DBA 手工 INSERT 后 plain reload 也能同步 route_table/dialplan/sip_profiles。
         * 如果 web 侧 data.sock 还没 ready（超时 2s），这里只打 WARN 不中断。 */
        int sync_rc = cfg_sync_after_reload();
        if (sync_rc != NSBC_OK) {
            ctl_printf(c, "WARN YAML reloaded but DB sync failed (web not ready?)\n");
            log_audit("oam", "control-reload", "db_sync=fail");
        } else {
            ctl_printf(c, "OK DB config synced from web\n");
            log_audit("oam", "control-reload", "db_sync=ok");
        }
    } else if (rc == NSBC_PERM_DENIED) {
        ctl_printf(c, "ERR need-restart %s\n", bad ? bad : "E_NEED_RESTART");
        log_audit("oam", "control-reload",
                  "result=need-restart reason=%s", bad ? bad : "-");
    } else {
        ctl_printf(c, "ERR reload failed\n");
        log_audit("oam", "control-reload", "result=err rc=%d", rc);
    }
}

/* config.get <key>（REQ-CFG-011，L0 只读不审计）：取生效值+命中层；
 * reveal=0——敏感键恒 ****（揭示密文需 L2 授权+审计，P1 随命令注册表）。
 * 帧两行：`config key=.. layer=.. secret=0|1` + 值原文一行。 */
static void cmd_config_get(ctl_conn_t *c, const char *arg)
{
    if (!valid_cfg_key(arg)) {
        ctl_printf(c, "ERR invalid key\n");
        return;
    }
    if (!g_ctx->cfg) {
        ctl_printf(c, "ERR get unavailable\n");
        return;
    }
    /* cfg_get_str 标量拷贝窗 512B，加层注记 1024 足够 */
    char buf[1024];
    int rc = cfg_show(g_ctx->cfg, arg, buf, sizeof(buf), 0);
    if (rc == NSBC_NOENT) {
        ctl_printf(c, "ERR notfound\n");
        return;
    }
    if (rc != NSBC_OK) {
        ctl_printf(c, "ERR get failed\n");
        return;
    }
    /* cfg_show 渲染："<value>\t# layer=<name>"（敏感值 value=****） */
    char *tab = strchr(buf, '\t');
    const char *layer = "?";
    if (tab) {
        *tab = '\0';
        const char *p = strstr(tab + 1, "layer=");
        if (p)
            layer = p + 6;
    }
    int secret = strcmp(buf, "****") == 0;
    ctl_printf(c, "config key=%s layer=%s secret=%d\n%s\n",
               arg, layer, secret, buf);
}

/* 值白名单（与键同属入校）：1..511 字节（cfg_get_str 拷贝窗 512B），
 * 仅可见 ASCII（0x20..0x7e，含空格）；杜绝控制字符造成帧注入。 */
static int valid_cfg_value(const char *s)
{
    if (!s)
        return 0;
    size_t n = strlen(s);
    if (n == 0 || n > 511)
        return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch > 0x7e)
            return 0;
    }
    return 1;
}

/* 就地拆分 "<key> <value>"：value 为首个空白后的原文（保留内部空格）。
 * 成功返回 1 并写出指针；无值/空值返回 0。 */
static int split_kv(char *arg, char **key, char **value)
{
    *key = NULL;
    *value = NULL;
    char *p = arg;
    while (*p && *p != ' ' && *p != '\t')
        ++p;
    if (!*p)
        return 0;
    *p++ = '\0';
    while (*p == ' ' || *p == '\t')
        ++p;
    if (!*p)
        return 0;
    *key = arg;
    *value = p;
    return 1;
}

/* config.set（apply=1，L1，审计）/ config.validate（apply=0，L0 dry-run，
 * 不审计）共用同一校验链（REQ-CFG-011 同源）。审计只记 key+结果，绝不记录
 * value 明文（防止敏感参数经审计通道泄漏）。 */
static void cmd_config_propose(ctl_conn_t *c, char *arg, int apply)
{
    char *key = NULL, *val = NULL;
    if (!split_kv(arg, &key, &val) || !valid_cfg_key(key) ||
        !valid_cfg_value(val)) {
        ctl_printf(c, "ERR invalid key\n");
        if (apply)
            log_audit("oam", "control-config-set", "result=invalid");
        return;
    }
    if (!g_ctx->cfg) {
        ctl_printf(c, "ERR %s unavailable\n", apply ? "set" : "validate");
        if (apply)
            log_audit("oam", "control-config-set",
                      "key=%s result=unavailable", key);
        return;
    }
    const char *reason = NULL;
    int changed = 0;
    int rc = cfg_runtime_propose(g_ctx->cfg, key, val, apply, &reason,
                                 &changed);
    if (rc == NSBC_OK) {
        if (apply)
            ctl_printf(c, changed ? "OK set %s\n" : "OK unchanged %s\n", key);
        else
            ctl_printf(c, "OK valid %s\n", key);
        if (apply)
            log_audit("oam", "control-config-set", "key=%s result=%s", key,
                      changed ? "ok" : "unchanged");
        return;
    }
    if (rc == NSBC_NOENT)
        ctl_printf(c, "ERR notfound\n");
    else if (rc == NSBC_PERM_DENIED && reason &&
             strcmp(reason, "E_NEED_RESTART") == 0)
        ctl_printf(c, "ERR need-restart E_NEED_RESTART\n");
    else if (rc == NSBC_PERM_DENIED && reason && strcmp(reason, "secret") == 0)
        ctl_printf(c, "ERR rejected secret-not-settable\n");
    else if (rc == NSBC_INVAL)
        ctl_printf(c, "ERR invalid value\n");
    else
        ctl_printf(c, "ERR %s failed\n", apply ? "set" : "validate");
    if (apply)
        log_audit("oam", "control-config-set", "key=%s result=rejected reason=%s",
                  key, reason ? reason : "-");
}

/* config.export（REQ-CFG-013/011，L0 只读不审计）：脱敏全量生效视图。
 * 帧：首行 `config-export keys=N redact=1`，其后每键一行
 * `<dotted.key>: <value|****>`（引擎保证注释仅首行，值区自首个换行起）。 */
static void cmd_config_export(ctl_conn_t *c)
{
    if (!g_ctx->cfg) {
        ctl_printf(c, "ERR export unavailable\n");
        return;
    }
    char *body = malloc(CTL_OUT_MAX);
    if (!body) {
        ctl_printf(c, "ERR oom\n");
        return;
    }
    int rc = cfg_export_yaml(g_ctx->cfg, body, CTL_OUT_MAX, 1);
    if (rc != NSBC_OK) {
        ctl_printf(c, "ERR export failed\n");
        free(body);
        return;
    }
    char *vals = strchr(body, '\n');
    vals = vals ? vals + 1 : body;
    unsigned keys = 0;
    for (const char *q = vals; *q;) {
        const char *nl = strchr(q, '\n');
        ++keys;
        if (!nl)
            break;
        q = nl + 1;
    }
    ctl_printf(c, "config-export keys=%u redact=1\n", keys);
    ctl_write(c, vals, strlen(vals));
    free(body);
}

/* config.snapshots（L0 不审计）：首行 `config-snapshots count=N`，
 * 其后每行一个快照名（新→旧）；目录不存在按 count=0。 */
static void cmd_config_snapshots(ctl_conn_t *c)
{
    if (!g_ctx->cfg) {
        ctl_printf(c, "ERR snapshots unavailable\n");
        return;
    }
    char *body = malloc(CTL_OUT_MAX);
    if (!body) {
        ctl_printf(c, "ERR oom\n");
        return;
    }
    int count = 0;
    int rc = cfg_snapshot_list(g_ctx->cfg, body, CTL_OUT_MAX, &count);
    if (rc != NSBC_OK && rc != NSBC_NOENT) {
        ctl_printf(c, "ERR snapshots failed\n");
        free(body);
        return;
    }
    ctl_printf(c, "config-snapshots count=%d\n", count);
    if (rc == NSBC_OK && count > 0)
        ctl_write(c, body, strlen(body));
    free(body);
}

/* 快照 id 白名单（引擎 snap_name_ok 同口径）：键字符合集 + .yaml 后缀。 */
static int valid_snapshot_id(const char *s)
{
    if (!s || !valid_cfg_key(s))
        return 0;
    size_t n = strlen(s);
    return n >= 6 && strcmp(s + n - 5, ".yaml") == 0;
}

/* config.rollback [snapshot-id]（REQ-CFG-008/011，L1，全程审计）：
 * 无参=最新份，带 id=--to 指定快照；审计只记 id（文件名）+结果。 */
static void cmd_config_rollback(ctl_conn_t *c, const char *arg)
{
    const char *id = (*arg ? arg : NULL);
    if (id && !valid_snapshot_id(id)) {
        ctl_printf(c, "ERR invalid snapshot\n");
        log_audit("oam", "control-config-rollback", "result=invalid");
        return;
    }
    if (!g_ctx->cfg) {
        ctl_printf(c, "ERR rollback unavailable\n");
        log_audit("oam", "control-config-rollback",
                  "result=unavailable");
        return;
    }
    char name[128];
    int rc = cfg_rollback(g_ctx->cfg, id, name, sizeof(name));
    if (rc == NSBC_OK) {
        ctl_printf(c, "OK rollback %s\n", name);
        log_audit("oam", "control-config-rollback", "result=ok id=%s", name);
        return;
    }
    if (rc == NSBC_INVAL) {
        ctl_printf(c, "ERR invalid snapshot\n");
        log_audit("oam", "control-config-rollback", "result=invalid");
    } else if (rc == NSBC_NOENT) {
        ctl_printf(c, "ERR no-snapshot\n");
        log_audit("oam", "control-config-rollback",
                  "result=no-snapshot id=%s", id ? id : "-");
    } else if (rc == NSBC_PERM_DENIED) {
        ctl_printf(c, "ERR need-restart E_NEED_RESTART\n");
        log_audit("oam", "control-config-rollback",
                  "result=need-restart id=%s", id ? id : "-");
    } else {
        ctl_printf(c, "ERR rollback failed\n");
        log_audit("oam", "control-config-rollback",
                  "result=err rc=%d id=%s", rc, id ? id : "-");
    }
}

static void dispatch(ctl_conn_t *c, char *line)
{
    while (*line == ' ' || *line == '\t')
        ++line;
    char *e = line + strlen(line);
    while (e > line && (e[-1] == '\r' || e[-1] == '\n' ||
                        e[-1] == ' ' || e[-1] == '\t'))
        *--e = '\0';
    if (*line == '\0')
        return;
    char *arg = line;
    while (*arg && *arg != ' ')
        ++arg;
    if (*arg == ' ') {
        *arg++ = '\0';
        while (*arg == ' ')
            ++arg;
    }

    /* quit/exit：只置标记，由调用方 on_conn_event 统一释放——
     * dispatch 内 free(c) 会让调用方 L1217 读 c->fd 变成 UAF
     * （free 后 tcache 覆写首字段，垃圾值非负则 memmove 写已释放堆，
     *  实测远端 segfault @ obs_control.c:1217）。 */
    if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
        ctl_printf(c, "bye\n");
        c->fd = -1;
        return;
    }

    /* ---- 多 token 命令：show/log 家族保持硬编码 ----
     *
     * cmd_registry 命令均为单 token（"uuid_kill" / "status" /
     * "profiles.reload" 等）。多 token 的 "show calls"/"show channels"/
     * "show registrations"/"show cdr"/"sofia profile <name> rescan"
     * 在下面 if 链里处理。sofia 家族的 rescan 虽然在 cmd_registry
     * 里注册了 sofia 但它只收一个 cmd_line="sofia"，参数 rescan
     * 作为后续 token 由函数内部再切——简化处理：sofia 也让走
     * cmd_registry（内部再切 argv 数组）。 */

    /* ---- 单 token：注册表命令走 cmd_registry，其余走内建硬编码 ----
     * 预判（cmd_registered 只读查询）先行：下面 token 化会就地把空格
     * 改写为 '\0'，仅对确在注册表的命令做；内建命令的原始 arg（如
     * rate.reload 的整段 JSON）必须保持完整，否则 JSON 被切词截断。 */
    if (g_cmd_reg && cmd_registered(g_cmd_reg, line)) {
        /* 构造 argc/argv：argv[0]=line, argv[1]=arg 再切出后续 token
         *
         * ⚠️ .reload 命令的 argv[1] 是 JSON（含空格/引号/花括号），
         *    任何按空格切词都会把 JSON 截断导致 cJSON_Parse 报
         *    "invalid json: expected array"。
         *    → 检测命令名以 ".reload" 结尾时，把 arg 整段作为
         *    argv[1]、argc=2，不做空格切分。 */
        char *argv[16];     /* 最多 16 个 token */
        int argc = 0;
        argv[argc++] = line;
        int is_reload = (strlen(line) >= 7 &&
                         strcmp(line + strlen(line) - 7, ".reload") == 0);
        if (is_reload) {
            argv[argc++] = arg;  /* JSON 整段，不切 */
        } else {
            char *p = arg;
            while (argc < 16) {
                while (*p == ' ') ++p;
                if (*p == '\0') break;
                argv[argc++] = p;
                while (*p && *p != ' ') ++p;
                if (*p == ' ') *p++ = '\0';
            }
        }
        /* UDS 控制通道走 root-only socket (0600)，等价 L2 admin 信任边界 */
        cmd_ctx_t ctx = { NULL, 2, 1, ctl_io_printf, c };
        int rc = cmd_execute(g_cmd_reg, &ctx, argc, argv);
        if (rc == NSBC_PERM_DENIED) {
            /* L2 命令未二次授权（REQ-OAM-008 P1）：提示走重认证 */
            ctl_printf(c, "ERR permission denied (L2 re-auth required)\n");
            log_audit("oam", "control-cmd",
                      "cmd=%s result=perm-denied", line);
        } else if (rc != NSBC_OK) {
            ctl_printf(c, "ERR cmd failed rc=%d\n", rc);
        }
        /* rc == NSBC_OK：命令函数内已 ctl_printf 输出结果 */
        ctl_printf(c, "nsbc> ");
        return;
    }

    /* ---- 内建硬编码命令（注册表未注册的历史命令 + 多 token）---- */
    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        ctl_help(c);
    } else if (strcmp(line, "version") == 0) {
        ctl_printf(c, "nsbcd %s\n", NSBC_VERSION);
    } else if (strcmp(line, "stats") == 0) {
        cmd_stats(c);
    } else if (strcmp(line, "events") == 0) {
        cmd_events(c, arg);
    } else if (strcmp(line, "show") == 0 && strcmp(arg, "calls") == 0) {
        b2b_core_t *b = (b2b_core_t *)g_ctx->b2b;
        ctl_printf(c,
                   "active=%llu total=%llu failed=%llu\n",
                   (unsigned long long)(b ? b2b_active_calls(b) : 0),
                   (unsigned long long)(b ? b2b_total_calls(b) : 0),
                   (unsigned long long)(b ? b2b_failed_calls(b) : 0));
    } else if (strcmp(line, "show") == 0 && strcmp(arg, "channels") == 0) {
        cmd_show_channels(c);
    } else if (strcmp(line, "show") == 0 && strcmp(arg, "apps") == 0) {
        /* ---- show apps：dialplan app 注册表（REQ-DP-APP-001 + REQ-AI-001）----
         * ans = 2 表示应用级（不是 CCMD 命令级）；dp_sleep_app_reg() 返回
         * 全局 app_reg_t 实例；app_help 逐行输出 "name  help" */
        app_reg_t *reg = dp_sleep_app_reg();
        if (!reg) {
            ctl_printf(c, "(no app registry — dialplan module not loaded)\n");
        } else {
            ctl_printf(c, "apps=%d\n", app_count(reg));
            app_help(reg, NULL, ctl_io_printf, c);
        }
    } else if (strcmp(line, "show") == 0 &&
               strcmp(arg, "registrations") == 0) {
        /* ---- show registrations：列出所有有效 binding（REQ-OAM-011 P1）---- */
        loc_db_t *db = g_ctx->binding_count_ud
                       ? (loc_db_t *)g_ctx->binding_count_ud : NULL;
        char *body = malloc(CTL_OUT_MAX);
        if (!body) {
            ctl_printf(c, "ERR oom\n");
        } else if (db) {
            struct { char *out; size_t cap; size_t pos; } ud;
            ud.out = body; ud.cap = CTL_OUT_MAX; ud.pos = 0;
            uint64_t now = nsbc_now_ms();
            extern int cmd_reg_line_cb(const loc_binding_t *b, void *ud);
            int shown = loc_db_foreach(db, now, cmd_reg_line_cb, &ud);
            if (ud.pos + 64 <= CTL_OUT_MAX) {
                ud.pos += snprintf(ud.out + ud.pos, CTL_OUT_MAX - ud.pos,
                                   "registrations=%d truncated=%d\n",
                                   shown, ud.pos >= CTL_OUT_MAX);
            }
            ctl_write(c, body, strlen(body));
        } else {
            ctl_printf(c, "registrations=0\n");
        }
        free(body);
    } else if (strcmp(line, "show") == 0 &&
               strcmp(arg, "rates") == 0) {
        /* ---- show rates：列出所有费率条目（OAM L0 只读，REQ-OAM-009）---- */
        billing_t *bil = g_ctx ? (billing_t *)g_ctx->billing : NULL;
        if (!bil) {
            ctl_printf(c, "ERR billing not initialized\n");
        } else {
            int total = bill_rate_count(bil);
            ctl_printf(c, "rates=%d\n", total);
            if (total > 0) {
                /* 用迭代回调输出每一行：
                 * rate prefix=X name=Y price_per_min_uc=N connect_fee_uc=F
                 * bill_inc_s=I min_bill_s=M initial_s=S enabled=B */
                struct { ctl_conn_t *c; } ud = { c };
                bill_foreach_rate(bil, (rate_iter_cb)rate_show_cb, &ud);
            }
        }
    } else if (strcmp(line, "show") == 0 &&
               strcmp(arg, "balances") == 0) {
        /* ---- show balances：列出所有账户余额（OAM L0 只读，REQ-OAM-009）---- */
        billing_t *bil = g_ctx ? (billing_t *)g_ctx->billing : NULL;
        if (!bil) {
            ctl_printf(c, "ERR billing not initialized\n");
        } else {
            int total = bill_account_count(bil);
            ctl_printf(c, "accounts=%d\n", total);
            if (total > 0) {
                struct { ctl_conn_t *c; } ud = { c };
                bill_foreach_account(bil, (acct_iter_cb)acct_show_cb, &ud);
            }
        }
    } else if (strcmp(line, "show") == 0 &&
               strcmp(arg, "transcode") == 0) {
        /* ---- show transcode：活跃媒体 bridge 会话（OAM L0 只读，REQ-OAM-010）---- */
        bridge_mgr_t *bm = g_ctx ? (bridge_mgr_t *)g_ctx->bridge : NULL;
        if (!bm) {
            ctl_printf(c, "ERR bridge not initialized\n");
        } else {
            int total = bridge_active_count(bm);
            ctl_printf(c, "sessions=%d\n", total);
            if (total > 0) {
                struct { ctl_conn_t *c; int idx; } ud = { c, 0 };
                bridge_foreach_call(bm, (bridge_iter_cb)bridge_show_cb, &ud);
            }
        }
    } else if (strcmp(line, "show") == 0 && strcmp(arg, "modules") == 0) {
        /* ---- show modules：按 category 分组列出全部模块 ---- */
        nsbc_modmgr_t *mm = modmgr_get_global();
        if (!mm) {
            ctl_printf(c, "ERR module manager not initialized\n");
        } else {
            unsigned total = modmgr_count(mm);
            ctl_printf(c, "modules=%u\n", total);

            /* 两轮遍历：先按 category 分组计数，再逐类输出。
             * 每个 category 单独 ctl_printf，无 buffer 拼接 bug。*/

            /* pass 1: 统计每个 category 的模块数 */
            int counts[MOD_CAT__COUNT] = {0};
            for (unsigned idx = 0; idx < total; idx++) {
                const nsbc_module_t *d = modmgr_get_nth(mm, idx);
                if (!d) continue;
                mod_category_t cat = d->category;
                if (cat >= MOD_CAT__COUNT) cat = MOD_CAT_UNKNOWN;
                counts[cat]++;
            }

            /* pass 2: 按 category 顺序输出。先打 category 标题行，
             * 再扫一遍找出该类的模块逐个打印 */
            for (int ci = 0; ci < MOD_CAT__COUNT; ci++) {
                if (counts[ci] == 0) continue;
                ctl_printf(c, "  [%s]\n", mod_category_name((mod_category_t)ci));
                for (unsigned idx = 0; idx < total; idx++) {
                    const nsbc_module_t *d = modmgr_get_nth(mm, idx);
                    if (!d) continue;
                    mod_category_t cat = d->category;
                    if (cat >= MOD_CAT__COUNT) cat = MOD_CAT_UNKNOWN;
                    if ((int)cat != ci) continue;
                    mod_state_t st = modmgr_get_state_at(mm, idx);
                    const char *st_name =
                        st == MOD_ST_STARTED  ? "STARTED"  :
                        st == MOD_ST_INITED   ? "INITED"   :
                        st == MOD_ST_DISABLED ? "DISABLED" : "REG";
                    unsigned flags = d->flags;
                    const char *fl =
                        (flags & MOD_F_CALL_PATH) ? " [CALL_PATH]" :
                        (flags & MOD_F_REQUIRED)  ? " [REQUIRED]"  : "";
                    ctl_printf(c, "    %-20s %-8s %s\n",
                               d->name, st_name, fl);
                }
            }
        }
    } else if (strcmp(line, "show") == 0 && strcmp(arg, "event_bus") == 0) {
        /* ---- show event_bus：每 core ring 状态（Phase 5）----
         *
         * 输出格式：
         *   cores=N
         *   core 0: write=1234 read=1200 size=34 dropped=0
         *   core 1: write=5678 read=5600 size=78 dropped=0
         */
        int cores = event_bus_core_count();
        if (cores <= 0) {
            ctl_printf(c, "ERR event_bus not initialized\n");
        } else {
            per_core_ring_stat_t *stats = malloc((size_t)cores * sizeof(*stats));
            if (!stats) {
                ctl_printf(c, "ERR oom\n");
            } else {
                ctl_printf(c, "cores=%d\n", cores);
                event_bus_collect_all_stats(stats, cores);
                for (int i = 0; i < cores; i++) {
                    ctl_printf(c,
                        "core %d: write=%llu read=%llu size=%llu dropped=%llu\n",
                        stats[i].core_id,
                        (unsigned long long)stats[i].write_pos,
                        (unsigned long long)stats[i].read_pos,
                        (unsigned long long)stats[i].current_size,
                        (unsigned long long)stats[i].dropped_count);
                }
                free(stats);
            }
        }
    } else if (strcmp(line, "show") == 0 && strcmp(arg, "codecs") == 0) {
        /* ---- show codecs：codec_registry 已注册条目（v0.5.0 升级）----
         *
         * 输出格式（分音频 / 视频，表格对齐）：
         *   codecs=N (audio=M, video=K)
         *   --- Audio Codecs ---
         *   NAME          PT   RATE   CH   PTIME   DEC   ENC   SOURCE
         *   g711a          8    8000    1    20ms    ✓     ✓     built-in
         *   g711u          0    8000    1    20ms    ✓     ✓     built-in
         *   g729          18    8000    1    20ms    ✓     ✓     bcg729
         *   g722           9   16000    1    20ms    ✓     ✓     FFmpeg
         *   opus          96   48000    1    20ms    ✓     ✓     FFmpeg
         *   --- Video Codecs ---
         *   NAME          PT   RATE   CH   PTIME   DEC   ENC   SOURCE
         *   h264          96   90000    0     0ms    ✓     ✓     FFmpeg
         */
        unsigned total = codec_registry_count();
        unsigned aud_n = 0, vid_n = 0;

        /* Pass 1: count audio vs video */
        for (int i = 1; i < CODEC__MAX; i++) {
            const codec_ops_t *ops = codec_registry_find((codec_id_t)i);
            if (!ops) continue;
            if (ops->id == CODEC_H264 || ops->id == CODEC_VP8 || ops->id == CODEC_VP9)
                vid_n++;
            else
                aud_n++;
        }

        ctl_printf(c, "codecs=%u (audio=%u, video=%u)\n", total, aud_n, vid_n);

        if (total == 0) {
            ctl_printf(c, "  (no codecs registered — mod_codecs/mod_av may not be loaded)\n");
        }

        /* Audio pass */
        if (aud_n > 0) {
            const char *src;
            ctl_printf(c, "\n--- Audio Codecs ---\n");
            ctl_printf(c, "%-16s %4s %7s %4s %7s %5s %5s %s\n",
                       "NAME", "PT", "RATE", "CH", "PTIME", "DEC", "ENC", "SOURCE");
            for (int i = 1; i < CODEC__MAX; i++) {
                const codec_ops_t *ops = codec_registry_find((codec_id_t)i);
                if (!ops) continue;
                if (ops->id == CODEC_H264 || ops->id == CODEC_VP8 || ops->id == CODEC_VP9)
                    continue;

                /* classify source by codec_id range */
                switch (ops->id) {
                case CODEC_PCMA:
                case CODEC_PCMU:
                case CODEC_TELEPHONE_EVENT:
                case CODEC_CN:
                    src = "built-in"; break;
                case CODEC_G729:
                case CODEC_AMR_NB:
                case CODEC_AMR_WB:
                    src = "bcg729/vendor"; break;
                default:
                    src = "FFmpeg (mod_av)"; break;
                }

                /* PT=-1 (dynamic) → "dyn"; channels=0 (video) → "-" */
                char pt_str[8], ch_str[8];
                if (ops->default_pt < 0)      snprintf(pt_str, sizeof(pt_str), "%-4s", "dyn");
                else                          snprintf(pt_str, sizeof(pt_str), "%-4d", ops->default_pt);
                if (ops->channels == 0)       snprintf(ch_str, sizeof(ch_str), "%-4s", "-");
                else                          snprintf(ch_str, sizeof(ch_str), "%-4d", ops->channels);

                ctl_printf(c, "%-16s %s %7d %s %5dms %5s %5s %s\n",
                    ops->name,
                    pt_str,
                    ops->clock_rate,
                    ch_str,
                    ops->ptime_ms,
                    ops->decode     ? "\xe2\x9c\x93" : "-",
                    ops->encode     ? "\xe2\x9c\x93" : "-",
                    src);
            }
        }

        /* Video pass */
        if (vid_n > 0) {
            ctl_printf(c, "\n--- Video Codecs ---\n");
            ctl_printf(c, "%-16s %4s %7s %4s %7s %5s %5s %s\n",
                       "NAME", "PT", "RATE", "CH", "PTIME", "DEC", "ENC", "SOURCE");
            for (int i = 1; i < CODEC__MAX; i++) {
                const codec_ops_t *ops = codec_registry_find((codec_id_t)i);
                if (!ops) continue;
                if (ops->id != CODEC_H264 && ops->id != CODEC_VP8 && ops->id != CODEC_VP9)
                    continue;

                char pt_str[8], ch_str[8];
                if (ops->default_pt < 0)      snprintf(pt_str, sizeof(pt_str), "%-4s", "dyn");
                else                          snprintf(pt_str, sizeof(pt_str), "%-4d", ops->default_pt);
                snprintf(ch_str, sizeof(ch_str), "%-4s", "-"); /* 视频 codec 无声道 */

                ctl_printf(c, "%-16s %s %7d %s %5dms %5s %5s %s\n",
                    ops->name,
                    pt_str,
                    ops->clock_rate,
                    ch_str,
                    ops->ptime_ms,
                    ops->decode     ? "\xe2\x9c\x93" : "-",
                    ops->encode     ? "\xe2\x9c\x93" : "-",
                    "FFmpeg (mod_av)");
            }
        }
    } else if (strcmp(line, "show") == 0 && strcmp(arg, "cdr_wal") == 0) {
        /* ---- show cdr_wal：WAL 文件未提交事件数（Phase 6）----
         *
         * 输出格式：
         *   enabled=1
         *   uncommitted=N
         */
        cdr_wal_t *w = cdr_wal_get_global();
        if (!w) {
            ctl_printf(c, "enabled=0\n");
        } else {
            int n = cdr_wal_uncommitted_count(w);
            ctl_printf(c, "enabled=1 uncommitted=%d\n", n);
        }
    } else if (strcmp(line, "show") == 0 && strcmp(arg, "endpoints") == 0) {
        /* ---- show endpoints：endpoint_registry 已注册条目（Phase 审计）----
         *
         * 输出格式：
         *   total=N
         *     sip(slot=0, accept=yes, handle_invite=yes, refcnt=0)
         */
        int total = endpoint_registry_count();
        ctl_printf(c, "endpoints=%d\n", total);
        for (int i = 0; i < ENDPOINT_MAX; ++i) {
            int unloading = 0, refcnt = 0;
            const endpoint_ops_t *ops = endpoint_registry_find_by_slot(i, &unloading, &refcnt);
            if (!ops) continue;
            ctl_printf(c,
                "  %s(slot=%d, accept=%s, handle_invite=%s, handle_destroy=%s, unloading=%d, refcnt=%d)\n",
                ops->name, i,
                ops->accept ? "yes" : "no",
                ops->handle_invite ? "yes" : "no",
                ops->handle_destroy ? "yes" : "no",
                unloading, refcnt);
        }
    } else if (strcmp(line, "show") == 0 && strcmp(arg, "ai_backends") == 0) {
        /* Phase R-14: show ai_backends —— ai_backend_registry 遍历 */
        int total = ai_backend_registry_count();
        ctl_printf(c, "ai_backends=%d\n", total);
        for (int i = 0; i < AI_BACKEND_MAX; ++i) {
            const ai_backend_ops_t *op = ai_backend_registry_find_by_slot(i);
            if (!op || !op->name) continue;
            ctl_printf(c, "  %s(tts=%s, asr=%s, llm=%s)\n",
                op->name, op->tts ? "yes" : "no",
                op->asr ? "yes" : "no", op->llm ? "yes" : "no");
        }
    } else if (strcmp(line, "rate.reload") == 0) {
        /* ---- rate.reload <json>：DB enabled 费率全量推入核心（REQ-OAM-009 L1）----
         *
         * 命令行 arg 是整个 JSON 数组：
         *   [{prefix, name, price_per_min_uc, connect_fee_uc, bill_inc_s,
         *     min_bill_s, initial_s, enabled, ...}, ...]
         *
         * RCU 原子替换：建新表 → 填充条目 → 指针 swap → 销毁旧表。
         * 整个过程持有新表锁，旧表在 uniq 后释放（无并发安全问题）。 */
        billing_t *bil = g_ctx ? (billing_t *)g_ctx->billing : NULL;
        billing_t *newb = NULL;
        int n_loaded = 0;

        if (!arg || !*arg) {
            ctl_printf(c, "ERR missing json payload\n");
            goto rate_done;
        }
        if (!bil) {
            ctl_printf(c, "ERR billing not initialized\n");
            goto rate_done;
        }

        if (bill_create(&newb) != NSBC_OK) {
            ctl_printf(c, "ERR oom\n");
            goto rate_done;
        }

        /* 简易 JSON 数组解析：找 '{' 块 → 解析对象 → rate_upsert */
        {
            const char *p = arg;
            const char *end = arg + strlen(arg);

            while (p < end) {
                /* 找 '{' — JSON 对象起点 */
                const char *obj_start = NULL;
                int depth = 0;
                int in_str = 0;
                const char *cur;

                for (cur = p; cur < end; ++cur) {
                    if (*cur == '"' && (cur == arg || cur[-1] != '\\'))
                        in_str = !in_str;
                    if (in_str) continue;
                    if (*cur == '{') {
                        obj_start = cur;
                        depth = 1;
                        break;
                    }
                }
                if (!obj_start) break;

                /* 找匹配的 '}'（处理嵌套对象和字符串内的） */
                ++cur;
                in_str = 0;
                while (cur < end && depth > 0) {
                    if (*cur == '"' && *(cur - 1) != '\\')
                        in_str = !in_str;
                    if (in_str) { ++cur; continue; }
                    if (*cur == '{') ++depth;
                    else if (*cur == '}') --depth;
                    ++cur;
                }

                /* 现在 obj_start..cur-1 是完整 JSON 对象 */
                {
                    size_t obj_len = (size_t)(cur - obj_start);
                    rate_entry_t r;
                    memset(&r, 0, sizeof(r));

                    if (json_rate_entry(obj_start, obj_len, &r) == 0) {
                        if (rate_upsert(newb, &r) == NSBC_OK)
                            ++n_loaded;
                    }
                }
                p = cur;
            }
        }

        /* RCU：原子指针替换（旧表无并发访问了，直接 destroy） */
        g_ctx->billing = newb;
        bill_destroy(bil);
        bil = NULL;   /* 标记旧表已释放 */
        ctl_printf(c, "OK loaded=%d\n", n_loaded);
        log_audit("oam", "rate.reload", "loaded=%d", n_loaded);

    rate_done:
        /* 如果 newb 没成功挂到 g_ctx->billing（RCU 未执行），释放它 */
        if (newb && g_ctx && g_ctx->billing != newb)
            bill_destroy(newb);

    } else if (strcmp(line, "accounts.reload") == 0) {
        /* ---- accounts.reload <json>：DB 全量账户 upsert 推入核心（REQ-OAM-009 L1）----
         *
         * 与 rate.reload 不同：upsert-only，不删除 JSON 里没有的账户
         * （那些账户在 resv[] 里可能有活跃 reservation，bill_settle 还要找）。
         * 命令分发层已识别 ".reload" 后缀，argv[1] 是整段 JSON 数组不空格切分。*/
        billing_t *bil = g_ctx ? (billing_t *)g_ctx->billing : NULL;
        if (!arg || !*arg) { ctl_printf(c, "ERR missing json payload\n"); goto acc_done; }
        if (!bil) { ctl_printf(c, "ERR billing not initialized\n"); goto acc_done; }

        int rc = account_reload_json(bil, arg);
        if (rc == NSBC_OK) {
            ctl_printf(c, "OK accounts reloaded\n");
            log_audit("oam", "accounts.reload", "ok");
        } else {
            ctl_printf(c, "ERR reload failed\n");
        }
    acc_done:

    } else if (strcmp(line, "rate.upsert") == 0) {
        /* ---- rate.upsert <json>：单条费率 upsert（REQ-OAM-009 L1）---- */
        billing_t *bil = g_ctx ? (billing_t *)g_ctx->billing : NULL;
        if (!bil) { ctl_printf(c, "ERR billing not initialized\n"); goto rate_upsert_end; }
        if (!arg || !*arg || *arg != '{') { ctl_printf(c, "ERR missing json object\n"); goto rate_upsert_end; }
        {
            rate_entry_t r;
            memset(&r, 0, sizeof(r));
            if (json_rate_entry(arg, strlen(arg), &r) != 0) {
                ctl_printf(c, "ERR bad json\n");
                goto rate_upsert_end;
            }
            int rc = rate_upsert(bil, &r);
            if (rc != NSBC_OK) {
                ctl_printf(c, "ERR rate_upsert rc=%d\n", rc);
            } else {
                ctl_printf(c, "OK prefix=%s\n", r.prefix);
                log_audit("oam", "rate.upsert", "prefix=%s", r.prefix);
            }
        }
    rate_upsert_end:
        ;

    } else if (strcmp(line, "rate.delete") == 0) {
        /* ---- rate.delete <prefix>：按前缀删除费率（REQ-OAM-009 L1）---- */
        billing_t *bil = g_ctx ? (billing_t *)g_ctx->billing : NULL;
        if (!bil) { ctl_printf(c, "ERR billing not initialized\n"); }
        else if (!arg || !*arg) { ctl_printf(c, "ERR missing prefix\n"); }
        else {
            int rc = rate_delete(bil, arg);
            if (rc == NSBC_NOENT) ctl_printf(c, "ERR notfound\n");
            else if (rc != NSBC_OK) ctl_printf(c, "ERR rc=%d\n", rc);
            else { ctl_printf(c, "OK\n"); log_audit("oam", "rate.delete", "prefix=%s", arg); }
        }

    } else if (strcmp(line, "account.open") == 0) {
        /* ---- account.open <key> <balance_uc> <credit_limit_uc> ---- */
        billing_t *bil = g_ctx ? (billing_t *)g_ctx->billing : NULL;
        char *arg_copy;
        char *key, *bal_s, *cred_s;
        int64_t bal, cred;
        int rc;

        if (!bil) { ctl_printf(c, "ERR billing not initialized\n"); goto acc_open_end; }
        if (!arg || !*arg) { ctl_printf(c, "ERR usage: account.open <key> <balance_uc> <credit_limit_uc>\n"); goto acc_open_end; }

        arg_copy = strdup(arg);
        if (!arg_copy) { ctl_printf(c, "ERR oom\n"); goto acc_open_end; }

        key = arg_copy;
        bal_s = strchr(key, ' ');
        if (!bal_s) { free(arg_copy); ctl_printf(c, "ERR missing balance\n"); goto acc_open_end; }
        *bal_s++ = '\0';
        cred_s = strchr(bal_s, ' ');
        if (!cred_s) { free(arg_copy); ctl_printf(c, "ERR missing credit_limit\n"); goto acc_open_end; }
        *cred_s++ = '\0';
        /* 跳过 cred_s 后续空格 */
        while (*cred_s == ' ') ++cred_s;

        bal = strtoll(bal_s, NULL, 10);
        cred = strtoll(cred_s, NULL, 10);

        rc = account_open(bil, key, bal, cred);
        if (rc == NSBC_OK) { ctl_printf(c, "OK\n"); log_audit("oam", "account.open", "key=%s balance=%lld credit=%lld", key, (long long)bal, (long long)cred); }
        else if (rc == NSBC_NOSPC) ctl_printf(c, "ERR capacity full\n");
        else ctl_printf(c, "ERR rc=%d\n", rc);
        free(arg_copy); /* key 指向 arg_copy 内，必须在使用完后再 free */

    acc_open_end:
        ;

    } else if (strcmp(line, "account.topup") == 0) {
        /* ---- account.topup <key> <delta_uc> ---- */
        billing_t *bil = g_ctx ? (billing_t *)g_ctx->billing : NULL;
        char *arg_copy;
        char *key, *delta_s;
        int64_t delta;
        int rc;

        if (!bil) { ctl_printf(c, "ERR billing not initialized\n"); goto acc_topup_end; }
        if (!arg || !*arg) { ctl_printf(c, "ERR usage: account.topup <key> <delta_uc>\n"); goto acc_topup_end; }

        arg_copy = strdup(arg);
        if (!arg_copy) { ctl_printf(c, "ERR oom\n"); goto acc_topup_end; }

        key = arg_copy;
        delta_s = strchr(key, ' ');
        if (!delta_s) { free(arg_copy); ctl_printf(c, "ERR missing delta\n"); goto acc_topup_end; }
        *delta_s++ = '\0';
        while (*delta_s == ' ') ++delta_s;

        delta = strtoll(delta_s, NULL, 10);
        rc = account_topup(bil, key, delta);
        if (rc == NSBC_OK) { ctl_printf(c, "OK\n"); log_audit("oam", "account.topup", "key=%s delta=%lld", key, (long long)delta); }
        else if (rc == NSBC_NOENT) ctl_printf(c, "ERR notfound\n");
        else ctl_printf(c, "ERR rc=%d\n", rc);
        free(arg_copy); /* key 指向 arg_copy 内，必须在使用完后再 free */

    acc_topup_end:
        ;

    } else if (strcmp(line, "log") == 0) {
        if (strncmp(arg, "dump", 4) == 0 &&
            (arg[4] == '\0' || arg[4] == ' ')) {
            int max = 200;
            char *p = arg + 4;
            while (*p == ' ') ++p;
            if (*p) max = atoi(p);
            if (max <= 0 || max > 5000) max = 200;
            char *body = malloc(CTL_OUT_MAX);
            if (!body) { ctl_printf(c, "ERR oom\n"); }
            else {
                size_t n = log_ring_tail(body, CTL_OUT_MAX, max);
                if (n > 0) ctl_write(c, body, n);
                else ctl_printf(c, "(no pending log lines in ring)\n");
                free(body);
            }
        } else if (strncmp(arg, "stats", 5) == 0 && arg[5] == '\0') {
            ctl_printf(c, "log dropped=%llu written=%llu\n",
                       (unsigned long long)log_dropped(),
                       (unsigned long long)log_written());
        } else {
            ctl_printf(c, "ERR unknown log subcommand (try 'log dump [N]' or 'log stats')\n");
        }
    } else if (strcmp(line, "uuid_dump") == 0) {
        cmd_uuid_dump(c, arg);
    } else if (strcmp(line, "config.get") == 0) {
        cmd_config_get(c, arg);
    } else if (strcmp(line, "config.set") == 0) {
        cmd_config_propose(c, arg, 1);
    } else if (strcmp(line, "config.validate") == 0) {
        cmd_config_propose(c, arg, 0);
    } else if (strcmp(line, "config.export") == 0) {
        cmd_config_export(c);
    } else if (strcmp(line, "config.snapshots") == 0) {
        cmd_config_snapshots(c);
    } else if (strcmp(line, "config.rollback") == 0) {
        cmd_config_rollback(c, arg);
    } else if (strcmp(line, "config.reload") == 0 ||
               strcmp(line, "reload") == 0) {
        cmd_reload(c);
    } else if (strcmp(line, "cdr.recent") == 0) {
        cmd_cdr_recent(c, arg);
    } else if (strcmp(line, "cdr.flush") == 0) {
        cmd_cdr_flush(c);
    } else if (strcmp(line, "show") == 0 && arg && strcmp(arg, "cdr") == 0) {
        cmd_show_cdr(c);
    } else if (strcmp(line, "hangup") == 0) {
        cmd_hangup(c, arg);
    } else if (strcmp(line, "sub") == 0) {
        /* sub <topics> — 订阅实时推送
         * topics 逗号分隔：events / cdr / log / metrics */
        if (!g_sub_hub) {
            ctl_printf(c, "ERR sub hub not initialized\n");
        } else if (!arg || !*arg) {
            ctl_printf(c, "ERR usage: sub events,cdr[,log,metrics]\n");
        } else {
            unsigned added = 0;
            char *dup = strdup(arg);
            if (dup) {
                char *save = NULL;
                for (char *tok = strtok_r(dup, ",", &save); tok;
                     tok = strtok_r(NULL, ",", &save)) {
                    while (*tok == ' ') ++tok;
                    char *end = tok + strlen(tok);
                    while (end > tok && *(end - 1) == ' ') *--end = '\0';
                    obs_topic_t t;
                    if (obs_topic_parse(tok, &t) == NSBC_OK) {
                        obs_sub_add(g_sub_hub, c, t);
                        c->sub_topics |= 1u << (unsigned)t;
                        ++added;
                    } else {
                        ctl_printf(c, "ERR unknown topic '%s'\n", tok);
                    }
                }
                free(dup);
            }
            ctl_printf(c, "OK sub added=%u total_topics=0x%x\n",
                       added, c->sub_topics);
        }
    } else if (strcmp(line, "unsub") == 0) {
        /* unsub [topics] — 取消订阅；无参取消全部 */
        if (!g_sub_hub) {
            ctl_printf(c, "ERR sub hub not initialized\n");
        } else if (!arg || !*arg) {
            for (unsigned t = 0; t < OBS_TOPIC_MAX; ++t)
                if (c->sub_topics & (1u << t))
                    obs_sub_del(g_sub_hub, c, (obs_topic_t)t);
            c->sub_topics = 0;
            ctl_printf(c, "OK unsubscribed all\n");
        } else {
            unsigned removed = 0;
            char *dup = strdup(arg);
            if (dup) {
                char *save = NULL;
                for (char *tok = strtok_r(dup, ",", &save); tok;
                     tok = strtok_r(NULL, ",", &save)) {
                    while (*tok == ' ') ++tok;
                    char *end = tok + strlen(tok);
                    while (end > tok && *(end - 1) == ' ') *--end = '\0';
                    obs_topic_t t;
                    if (obs_topic_parse(tok, &t) == NSBC_OK) {
                        obs_sub_del(g_sub_hub, c, t);
                        c->sub_topics &= ~(1u << (unsigned)t);
                        ++removed;
                    }
                }
                free(dup);
            }
            ctl_printf(c, "OK unsub removed=%u remaining_topics=0x%x\n",
                       removed, c->sub_topics);
        }
    } else if (strcmp(line, "sub.stat") == 0) {
        if (!c->sub_topics) {
            ctl_printf(c, "subscribed: (none)\n");
        } else {
            ctl_printf(c, "subscribed:");
            for (unsigned t = 0; t < OBS_TOPIC_MAX; ++t)
                if (c->sub_topics & (1u << t))
                    ctl_printf(c, " %s", obs_topic_name((obs_topic_t)t));
            ctl_printf(c, "\n");
        }
        if (g_sub_hub)
            ctl_printf(c, "drop_count=%llu\n",
                       (unsigned long long)obs_sub_drop_count(g_sub_hub));
    } else {
        ctl_printf(c, "ERR unknown command: %s (try 'help')\n", line);
        /* 未知命令可能是探测/暴力尝试 → fail2ban 可抓 */
        llog(LOG_WARN, "oam",
             "key=fail2ban reason=uds_unknown_cmd cmd=%s", line);
        log_audit("oam", "control-unknown-cmd", "cmd=%s", line);
    }
    ctl_printf(c, "nsbc> ");
    /* dispatch 末尾 flush 订阅残留帧（obs_sub_publish 里 EAGAIN 未发完的） */
    if (g_sub_hub && c->sub_topics)
        obs_sub_flush(g_sub_hub, c);
}

/* 3 处释放路径共用的订阅者清理 */
static void ctl_detach_sub(ctl_conn_t *c)
{
    if (g_sub_hub && c)
        obs_sub_detach(g_sub_hub, c);
}

static void on_conn_event(int fd, uint32_t ev, void *arg)
{
    ctl_conn_t *c = (ctl_conn_t *)arg;
    if (ev & (EPOLLHUP | EPOLLERR)) {
        ctl_detach_sub(c);
        reactor_del(g_ctx->re, c->fd);
        close(c->fd);
        free(c);
        return;
    }

    /* ★ EPOLLOUT：订阅帧 EAGAIN 后 kernel buffer 变空，flush 积压帧。
     * flush 到环空 → 减 EPOLLOUT，避免 UDS 永久可写导致 reactor 空转。 */
    if (ev & EPOLLOUT) {
        if (g_sub_hub && c->sub_topics) {
            obs_sub_flush(g_sub_hub, c);
            if (obs_sub_ring_empty(g_sub_hub, c) && g_ctx && g_ctx->re)
                reactor_mod(g_ctx->re, c->fd, EPOLLIN);
        }
    }

    if (!(ev & EPOLLIN))
        return;

    for (;;) {
        ssize_t n = recv(c->fd, c->buf + c->n, sizeof(c->buf) - c->n, 0);
        if (n <= 0) {
            if (n == 0) {
                llog(LOG_NOTICE, "oam", "control client fd=%d closed (EOF)", c->fd);
                ctl_detach_sub(c);
                reactor_del(g_ctx->re, c->fd);
                close(c->fd);
                free(c);
            }
            return; /* EAGAIN */
        }
        c->n += (size_t)n;
        /* 逐行分发 */
        for (;;) {
            char *lf = memchr(c->buf, '\n', c->n);
            if (!lf) {
                if (c->n == sizeof(c->buf))
                    c->n = 0; /* 超长无换行：丢弃 */
                break;
            }
            size_t llen = (size_t)(lf - c->buf) + 1;
            *lf = '\0';
            dispatch(c, c->buf);
            if (c->fd < 0) { /* quit：就地释放 */
                ctl_detach_sub(c);
                reactor_del(g_ctx->re, fd);
                close(fd);
                free(c);
                return;
            }
            memmove(c->buf, c->buf + llen, c->n - llen);
            c->n -= llen;
        }
    }
}

/* ============================================================================
 * cmd_registry 命令：模块管理（Phase 6 从 dispatch 硬编码 if 链迁到注册表）
 *
 * 迁移收益：
 *   1. help 自动列出 — cmd_help() 遍历注册表，不再手动同步 ctl_help 字符串
 *   2. 权限分级自动生效 — CMD_L2_ADMIN 需要 L2 二次授权
 *   3. 审计自动触发 — cmd_registry 有统一审计钩子
 *   4. 模块卸载可联动 — cmd_unregister 支持动态移除
 * ==========================================================================*/

static int cmd_mod_load(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    (void)argc;  /* cmd_registry 已按空格切词，但 mod.load 只需 argv[1] */
    (void)ud;    /* modmgr 全局可达 */
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        c->io_printf(c->io_ud,
                     "ERR usage: mod.load <modname> (e.g. mod.load av)\n");
        return NSBC_INVAL;
    }
    nsbc_modmgr_t *mm = modmgr_get_global();
    if (!mm) {
        c->io_printf(c->io_ud, "ERR modmgr not initialized\n");
        return NSBC_ERR;
    }
    int rc = modmgr_load_by_name(mm, argv[1], g_ctx);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK mod.load '%s' success\n", argv[1]);
        log_audit("oam", "control-mod-load",
                  "name=%s result=ok", argv[1]);
    } else {
        c->io_printf(c->io_ud, "ERR mod.load '%s' rc=%d\n", argv[1], rc);
        log_audit("oam", "control-mod-load",
                  "name=%s result=err rc=%d", argv[1], rc);
    }
    return rc == NSBC_OK ? NSBC_OK : NSBC_ERR;
}

static int cmd_mod_unload(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    (void)argc;
    (void)ud;
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        c->io_printf(c->io_ud,
                     "ERR usage: mod.unload <modname> (e.g. mod.unload av)\n");
        return NSBC_INVAL;
    }
    nsbc_modmgr_t *mm = modmgr_get_global();
    if (!mm) {
        c->io_printf(c->io_ud, "ERR modmgr not initialized\n");
        return NSBC_ERR;
    }
    int rc = modmgr_unload(mm, argv[1], g_ctx);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK mod.unload '%s' success\n", argv[1]);
        log_audit("oam", "control-mod-unload",
                  "name=%s result=ok", argv[1]);
    } else {
        c->io_printf(c->io_ud, "ERR mod.unload '%s' rc=%d\n", argv[1], rc);
        log_audit("oam", "control-mod-unload",
                  "name=%s result=err rc=%d", argv[1], rc);
    }
    return rc == NSBC_OK ? NSBC_OK : NSBC_ERR;
}

static int cmd_mod_reload(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    if (!c || !c->io_printf) return NSBC_INVAL;
    (void)argc;
    (void)ud;
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        c->io_printf(c->io_ud,
                     "ERR usage: mod.reload <modname> (e.g. mod.reload av)\n");
        return NSBC_INVAL;
    }
    nsbc_modmgr_t *mm = modmgr_get_global();
    if (!mm) {
        c->io_printf(c->io_ud, "ERR modmgr not initialized\n");
        return NSBC_ERR;
    }
    int rc = modmgr_reload(mm, argv[1], g_ctx);
    if (rc == NSBC_OK) {
        c->io_printf(c->io_ud, "OK mod.reload '%s' success\n", argv[1]);
        log_audit("oam", "control-mod-reload",
                  "name=%s result=ok", argv[1]);
    } else {
        c->io_printf(c->io_ud, "ERR mod.reload '%s' rc=%d\n", argv[1], rc);
        log_audit("oam", "control-mod-reload",
                  "name=%s result=err rc=%d", argv[1], rc);
    }
    return rc == NSBC_OK ? NSBC_OK : NSBC_ERR;
}

/* obs_control 核心命令注册到 cmd_registry（init + start 各调用一次，
 * 均有幂等保护——obs_cmd_registry_init 先跑，obs_control_start 发现
 * g_cmd_reg != NULL 就跳过 create 但仍调本函数补注册） */
void obs_core_commands_register(cmd_reg_t *reg, nsbc_ctx_t *ctx)
{
    if (!reg) return;
    cmd_desc_t descs[] = {
        { "mod.load",   "动态加载 mod_<name>.so（L2，审计）",
          "mod", CMD_L2_ADMIN, cmd_mod_load, ctx },
        { "mod.unload", "stop + fini + dlclose 模块（L2，审计）",
          "mod", CMD_L2_ADMIN, cmd_mod_unload, ctx },
        { "mod.reload", "unload + load + init + start 一键热更（L2，审计）",
          "mod", CMD_L2_ADMIN, cmd_mod_reload, ctx },
    };
    for (size_t i = 0; i < sizeof(descs) / sizeof(descs[0]); ++i) {
        int rc = cmd_register(reg, &descs[i]);
        if (rc != NSBC_OK)
            llog(LOG_WARN, "oam", "cmd '%s' register rc=%d",
                 descs[i].name, rc);
    }
}

static void on_accept_event(int fd, uint32_t ev, void *arg)
{
    (void)ev;
    (void)arg;
    for (;;) {
        int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0)
            return;
        ctl_conn_t *c = calloc(1, sizeof(*c));
        if (!c) {
            close(cfd);
            return;
        }
        c->fd = cfd;
        /* ★ 只用 EPOLLIN——UDS 连接永久可写，EPOLLOUT 会让 reactor 空转。
         * 需要监听可写时由 obs_control_sub_send 的 EAGAIN/partial 路径
         * reactor_mod 动态加上，flush 到环空后由 EPOLLOUT 回调再减掉。 */
        if (reactor_add(g_ctx->re, cfd, EPOLLIN, on_conn_event, c) !=
            NSBC_OK) {
            close(cfd);
            free(c);
            return;
        }
        /* 注册到订阅 hub（attach 不占 topic 位图；sub 命令才加 topic） */
        if (g_sub_hub)
            obs_sub_attach(g_sub_hub, c);
        log_audit("oam", "control-connect", "control client connected");
        ctl_printf(c, "NSBC %s control interface (try 'help')\nnsbc> ",
                   NSBC_VERSION);
    }
}

int obs_control_start(nsbc_ctx_t *ctx)
{
    g_ctx = ctx;
    /* UDS 路径硬编码（不读配置）：C/Web/cli 统一 /var/run/nsbc/，
     * 避免配置文件两边不一致导致 Web poller 连不上 */
    static const char *spec = "unix:/var/run/nsbc/control.sock";
    const char *path = spec + 5;
    if (strlen(path) >= sizeof(g_path))
        return NSBC_INVAL;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return NSBC_ERR;
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);
    unlink(path); /* 上一进程残留（启动单实例由 systemd 保证） */
    mkdir_p(path); /* 确保 /var/run/nsbc/ 等父目录存在 */
    if (bind(fd, (struct sockaddr *)&un, sizeof(un)) != 0) {
        llog(LOG_WARN, "oam", "control bind %s failed errno=%d (%s)",
             path, errno, strerror(errno));
        close(fd);
        return NSBC_ERR;
    }
    chmod(path, 0600); /* REQ-OAM-005：仅本机特权用户 */
    if (listen(fd, 16) != 0) {
        close(fd);
        unlink(path);
        return NSBC_ERR;
    }
    g_lfd = fd;
    snprintf(g_path, sizeof(g_path), "%s", path);
    reactor_add(ctx->re, g_lfd, EPOLLIN, on_accept_event, NULL);
    /* CCMD 统一注册表：幂等——oam_init 已创建并注册过 reload + call 命令组
     * （其他模块如 security 的 init 阶段依赖它注册自己的命令）。
     * 若 g_cmd_reg 已由 init 创建，此处跳过 create/register，保持 security 等模块
     * 在 init 阶段注册的命令不丢失。 */
    if (!g_cmd_reg) {
        cmd_reg_create(&g_cmd_reg);
        reload_commands_register(g_cmd_reg);
        call_commands_register(g_cmd_reg, ctx);
        obs_core_commands_register(g_cmd_reg, ctx);
    }
    /* ---- 订阅推送 hub（REQ-OBS-005/006）----
     * send 函数让 obs_sub_publish_events 在调用线程（可能是 worker）
     * 能直接往订阅者帧环推帧，flush 在锁内非阻塞 send。 */
    if (obs_sub_global_init(obs_control_sub_send) == NSBC_OK)
        g_sub_hub = obs_sub_global();

    llog(LOG_NOTICE, "oam", "control socket on %s (registered %d commands)",
         path, cmd_count(g_cmd_reg));
    return NSBC_OK;
}

void obs_control_stop(nsbc_ctx_t *ctx)
{
    (void)ctx;
    if (g_sub_hub) {
        /* 所有订阅者随 control 停止一起注销 */
        obs_sub_global_destroy();
        g_sub_hub = NULL;
    }
    if (g_lfd >= 0) {
        reactor_del(g_ctx->re, g_lfd);
        close(g_lfd);
        g_lfd = -1;
    }
    if (g_path[0]) {
        unlink(g_path);
        g_path[0] = '\0';
    }
    if (g_cmd_reg) {
        cmd_reg_destroy(g_cmd_reg);
        g_cmd_reg = NULL;
    }
}

/* §8 / REQ-OAM-008：暴露 cmd_reg 给 ESL / playback 等模块 */
cmd_reg_t *obs_control_get_cmd_reg(void) { return g_cmd_reg; }

/* init 阶段创建 cmd_registry（security/billing 等模块 init 阶段依赖它注册命令）。
 * start 阶段 obs_control_start 里有幂等检查（g_cmd_reg != NULL 则跳过 create）。 */
int obs_cmd_registry_init(struct nsbc_ctx *ctx)
{
    if (g_cmd_reg)
        return NSBC_OK;  /* 已初始化 */
    cmd_reg_create(&g_cmd_reg);
    reload_commands_register(g_cmd_reg);
    call_commands_register(g_cmd_reg, ctx);
    obs_core_commands_register(g_cmd_reg, ctx);
    llog(LOG_INFO, "oam", "cmd_registry init ready (registered %d commands)",
         cmd_count(g_cmd_reg));
    return NSBC_OK;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
