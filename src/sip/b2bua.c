/*
 * NSBC B2BUA core — SIP dialog state machine, call session lifecycle.
 * Copyright 2024-2026 李浩 (Li Hao), 上海宁卫信息技术有限公司
 * Contact: 1354608370@qq.com
 *
 * Licensed under the Apache License, Version 2.0. See LICENSE in project root.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "b2bua.h"
#include "callctrl.h"
#include "chan_var.h"
#include "endpoint_registry.h"  /* Phase 5 XL: INVITE 入站 endpoint_registry 分发 */
#include "event_bus.h"          /* 白皮书 §10.3: SESSION_CREATE/ANSWER/END 事件触发 */
#include "ha.h"                 /* REQ-HA-001: 主备角色门控（ha_role，主二进制内符号） */
#include "log.h"
#include "media_bug.h"
#include "media_bridge.h"   /* REQ-ORIGINATE-003 &echo: bridge_lookup_relay + rtp_relay_set_echo */
#include "metric_reg.h"
#include "nsbc_id.h"
#include "recorder.h"
#include "obs.h"          /* CALL_INCOMING/ANSWERED 事件经 obs_event 同时写 ring + 推订阅 */
#include "recording.h"   /* REQ-REC-006: teardown 走统一 registry+stop 路径 */
#include "realtime.h"
#include "security.h"    /* 静态黑名单（IP CIDR / UA）三接入点统一检查 */
#include "sip_build.h"
#include "sip_digest.h"    /* P1-12 INVITE 401/407 challenge 解析 + HA1 现算 */
#include "sip_profile.h"
#include "route_table.h"   /* P1-12 rt_trunk_get 拿 auth_user/realm/password + P2-14 rt_match_inbound_trunk */
#include "outbound_reg.h"  /* P2-14 outreg_find_by_auth_user 层 1 register 反向关联 */
#include "app_ctx.h"       /* nsbc_get_ctx() —— 每次取最新 ctx->rt（RCU reload 原子换表） */
#include "uDS_client.h"    /* REQ-UDS-002: uds_send_session（精确大小写，Linux 编译区分） */
#include <unistd.h>        /* usleep — _GNU_SOURCE 已在上部定义，此处显式 include 声明 */

/* ---------- rt 访问 helper ----------
 *
 * b2b_rt(co) 是启动时的快照指针 —— trunk reload 会换 ctx->rt（RCU 原子换表），
 * 但 b2b_core 不会收到通知，快照永远停在旧表（可能是空的）。
 * 所有 rt 查询必须经此 helper，每次从全局 ctx 取最新指针。 */
static inline route_table_t *b2b_rt(b2b_core_t *co)
{
    (void)co;
    nsbc_ctx_t *ctx = nsbc_get_ctx();
    return ctx ? ctx->rt : NULL;
}

/* ---------- reactor 线程内 local port 传递 ----------
 * transport rx callback（b2b_on_rx）拿到入向 local 端口，但 handle_new_invite
 * 在 txn_server_input 更深层调用，签名没有 local 参数。用 __thread 临时变量
 * 在 reactor 线程栈内传递（全流程同步在同一 epoll 线程执行）。
 * 定义已移到 sip_transport.c（sip_transport 是公共依赖模块，方便 sip_tr_send 引用）。 */
extern __thread uint16_t g_rx_local_port;

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>   /* strpbrk / strchr / strstr 等 */

/* SDP 单腿 offer/answer 上限（WebRTC SDP 3~8KB；超限回 488） */
#ifndef B2B_SDP_MAX
#define B2B_SDP_MAX    8192
#endif
#ifndef B2B_HDR_MAX
#define B2B_HDR_MAX    256
#endif
#define B2B_CALL_SHARDS 1024   /* 64 → 1024: tbl_find 链表扫描从 ~780→~49 节点，延迟降 16 倍 */

/* REQ-AI-001 / REQ-EVENTS-001：call_destroy 多槽 hook 数组（B2B_END_HOOK_MAX=32）
 * pthread_mutex_t hook_lock 保护注册/卸载；遍历时持锁（add/rm 都是小操作）。 */
static struct { b2b_call_destroy_cb fn; void *ud; } g_end_hooks[B2B_END_HOOK_MAX];
static int g_end_hook_n = 0;
static pthread_mutex_t g_end_hook_lock = PTHREAD_MUTEX_INITIALIZER;

/* SDP hook 多槽链：offer + answer 分开两张表 */
static struct { b2b_sdp_hook_fn fn; void *ud; } g_sdp_hooks[2][B2B_SDP_HOOK_MAX];
static int g_sdp_hook_n[2] = {0, 0};
static pthread_mutex_t g_sdp_hook_lock = PTHREAD_MUTEX_INITIALIZER;

/* REGISTER 多槽链 */
static struct { b2b_register_fn fn; void *ud; } g_reg_hooks[B2B_REGISTER_HOOK_MAX];
static int g_reg_hook_n = 0;
static pthread_mutex_t g_reg_hook_lock = PTHREAD_MUTEX_INITIALIZER;

/* 无主响应多槽链 */
static struct { b2b_resp_hook_fn fn; void *ud; } g_resp_hooks[B2B_RESP_HOOK_MAX];
static int g_resp_hook_n = 0;
static pthread_mutex_t g_resp_hook_lock = PTHREAD_MUTEX_INITIALIZER;

/* CDR 多槽链 */
static struct { b2b_cdr_hook_fn fn; void *ud; } g_cdr_hooks[B2B_CDR_HOOK_MAX];
static int g_cdr_hook_n = 0;
static pthread_mutex_t g_cdr_hook_lock = PTHREAD_MUTEX_INITIALIZER;

/* Media View 多槽链 */
static struct { b2b_media_view_fn fn; void *ud; } g_mv_hooks[B2B_REGISTER_HOOK_MAX];
static int g_mv_hook_n = 0;
static pthread_mutex_t g_mv_hook_lock = PTHREAD_MUTEX_INITIALIZER;

/* b2b_leg_t / call_link_t / struct b2b_call 已在 b2bua.h 完整定义 */

static void disarm_noanswer_timer(b2b_call_t *c); /* 前置声明 */
static void b2b_emit_cdr(b2b_call_t *c, cdr_phase_t phase);

/* nsbc_sa_t → 纯 IP 字符串（无端口；nsbc_sa_fmt 带 ":port" 不能喂 CIDR 匹配）。
 * AF_UNSPEC（WS 桥 padding 藏 ws_conn*）→ 空串（profile ACL 跳过，WS 有
 * 自己的来源控制）；失败输出空串。 */
static void sa_ip_str(const nsbc_sa_t *sa, char *out, size_t outsz)
{
    if (!out || !outsz)
        return;
    out[0] = '\0';
    if (!sa)
        return;
    if (sa->ss.ss_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)&sa->ss;
        inet_ntop(AF_INET, &s->sin_addr, out, outsz);
    } else if (sa->ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)&sa->ss;
        inet_ntop(AF_INET6, &s->sin6_addr, out, outsz);
    }
}

/* Session Timer 时间轮回调前置声明（定义在文件末尾） */
static void sess_timer_refresh_cb(uint64_t cb_now, void *arg);
static void sess_timer_expire_cb(uint64_t cb_now, void *arg);
static void extract_sess_timer_hdrs(const sip_msg_t *m,
                                    char *se, size_t sesz,
                                    char *minse, size_t msz,
                                    int *timer_ok);

/* struct b2b_call / struct b2b_core 已在 b2bua.h 完整定义 */

typedef struct {
    pthread_mutex_t lock;
    call_link_t *head;
} call_shard_t;

struct b2b_core {
    b2b_cfg_t cfg;
    mem_slab_t *slab;
    call_shard_t sh[B2B_CALL_SHARDS];
    uint64_t active, total, failed;
    uint32_t atomic_concurrent; /* 原子计数：全局同时在处理的呼叫数（并发硬限制） */
    uint32_t dump_gen; /* 枚举代数：每次 list 自增，配合 dump_mark 去重 */
    /* REQ-ORIGINATE-003 待补：核心/进程停机标志（b2b_core_destroy 置位）。
     * bridge 顺序外呼的阻塞 wait loop 轮询此标志，避免 shutdown 时挂住
     * 最长 timeout_ms（默认 60s）。volatile 保证 OAM 轮询线程能看到。 */
    volatile int stopping;
    /* ---- per-key 并发计数（REQ-ARCH-015：AoR + trunk 共用）----
     * 固定 4096 桶，key=AoR 字符串（"user@domain"）或 trunk_name。
     * 同一 AoR/trunk 的并发呼叫数硬限制（cfg.max_inflight_per_aor）。
     * 仿 registrar.c tbucket 模式：桶内 pthread_mutex + count + key。 */
#define B2B_PERKEY_N 4096
    struct {
        pthread_mutex_t lock;
        uint32_t count;
        char key[128];
    } pk[B2B_PERKEY_N];
};

/* ------------------------------------------------------------------ */
/* 小工具                                                               */
/* ------------------------------------------------------------------ */
static uint64_t now_ms(void) { return nsbc_now_ms(); }

/* === NAT SDP 改写（REQ-NAT-001）===
 * 扫描 SDP o=/c= 行，将 IN IP4 后的 RFC1918/CGNAT/回环/LinkLocal 私网 IP
 * 替换为 new_rtp_ip（FreeSWITCH ext-rtp-ip 等效）。IPv6 暂不改写。
 * in-place 改写，new_ip 更长时跳过并 warn。返回改写后长度（无 NAT 时不变）。 */
static int b2b_is_private_ip(const char *ip)
{
    if (!ip || !ip[0]) return 0;
    struct in_addr a;
    if (inet_pton(AF_INET, ip, &a) != 1) return 0;
    uint32_t h = ntohl(a.s_addr);
    if ((h & 0xFF000000u) == 0x0A000000u) return 1;  /* 10.0.0.0/8 */
    if ((h & 0xFFF00000u) == 0xAC100000u) return 1;  /* 172.16.0.0/12 */
    if ((h & 0xFFFF0000u) == 0xC0A80000u) return 1;  /* 192.168.0.0/16 */
    if ((h & 0xFFC00000u) == 0x64400000u) return 1;  /* 100.64.0.0/10 CGNAT */
    if ((h & 0xFF000000u) == 0x7F000000u) return 1;  /* 127.0.0.0/8 回环 */
    if ((h & 0xFFFF0000u) == 0xA9FE0000u) return 1;  /* 169.254.0.0/16 LinkLocal */
    return 0;
}

static size_t nat_sdp_rewrite(char *sdp, size_t sdp_len, size_t cap,
                              const char *new_rtp_ip, const char *new_sip_ip)
{
    if (!sdp || !sdp_len || !new_rtp_ip || !new_rtp_ip[0])
        return sdp_len;
    (void)cap; (void)new_sip_ip;

    char *p = sdp;
    char *end = sdp + sdp_len;
    int changed = 0;

    while (p < end) {
        char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        size_t line_sz = (size_t)(line_end - p);

        int is_o = (line_sz >= 2 && p[0] == 'o' && p[1] == '=');
        int is_c = (line_sz >= 2 && p[0] == 'c' && p[1] == '=');

        if (is_o || is_c) {
            char *ip4_marker = memmem(p, line_sz, "IN IP4", 6);
            if (ip4_marker) {
                char *addr_start = ip4_marker + 6;
                char *addr_end = addr_start;
                while (addr_end < line_end && *addr_end != ' ' && *addr_end != '\r')
                    ++addr_end;
                size_t addr_len = (size_t)(addr_end - addr_start);
                if (addr_len > 0 && addr_len < INET_ADDRSTRLEN) {
                    char old_ip[INET_ADDRSTRLEN] = {0};
                    memcpy(old_ip, addr_start, addr_len);
                    if (b2b_is_private_ip(old_ip)) {
                        size_t new_ip_len = strlen(new_rtp_ip);
                        if (new_ip_len <= addr_len) {
                            memcpy(addr_start, new_rtp_ip, new_ip_len);
                            if (new_ip_len < addr_len)
                                memset(addr_start + new_ip_len, ' ', addr_len - new_ip_len);
                            changed = 1;
                        } else {
                            llog(LOG_WARN, "b2b", "nat_sdp: new_ip(%zuB) > old_ip(%zuB), skip",
                                 new_ip_len, addr_len);
                        }
                    }
                }
            }
        }

        p += line_sz;
        if (p < end && *p == '\r') ++p;
        if (p < end && *p == '\n') ++p;
    }

    if (changed)
        llog(LOG_INFO, "b2b", "nat_sdp_rewrite: o=/c= private IP → %s", new_rtp_ip);
    return sdp_len;
}

static void add_atomic(uint64_t *p, int64_t v)
{
    __sync_fetch_and_add(p, v);
}

/* 并发硬限制：try_inc 检查并原子递增，超限返回 0；dec 递减
 * （atomic_concurrent 用 __atomic_* 以匹配 uint32_t 的显式宽度） */
static int concurrent_try_inc(b2b_core_t *co)
{
    uint32_t lim = co->cfg.max_concurrent_calls;
    uint32_t cur;
    for (;;) {
        cur = __atomic_load_n(&co->atomic_concurrent, __ATOMIC_ACQUIRE);
        if (lim != 0 && cur >= lim)
            return 0; /* 超限，拒绝新呼叫 */
        if (__atomic_compare_exchange_n(&co->atomic_concurrent, &cur, cur + 1,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 1;
    }
}

static void concurrent_dec(b2b_core_t *co)
{
    __atomic_fetch_sub(&co->atomic_concurrent, 1u, __ATOMIC_ACQ_REL);
}

/* ---- per-key 并发（REQ-ARCH-015：AoR / trunk 共用同一套 helper）----
 * key 为 AoR 字符串（"user@domain"）或 trunk_name，hash 到固定 4096 桶。
 * per-key try_inc：检查并原子递增，超限返回 0；dec：递减（key 不匹配跳过）。
 * 限制值为 cfg.max_inflight_per_aor（0=不限）。
 * 桶内 pthread_mutex 保护 count + key，key 不同（哈希冲突）时覆盖桶。 */

static uint32_t perkey_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h & (B2B_PERKEY_N - 1);
}

static int perkey_try_inc(b2b_core_t *co, const char *key)
{
    if (!key || !key[0]) return 1; /* 无 key 不限制 */
    uint32_t lim = co->cfg.max_inflight_per_aor;
    if (lim == 0) return 1; /* 不限 */
    uint32_t idx = perkey_hash(key);
    pthread_mutex_lock(&co->pk[idx].lock);
    int ok;
    if (co->pk[idx].key[0] == '\0') {
        /* 空桶 → 初始化 */
        snprintf(co->pk[idx].key, sizeof(co->pk[idx].key), "%s", key);
        co->pk[idx].count = 1;
        ok = 1;
    } else if (strcmp(co->pk[idx].key, key) == 0) {
        /* 同 key → 检查上限 */
        if (co->pk[idx].count < lim) {
            co->pk[idx].count++;
            ok = 1;
        } else {
            ok = 0; /* 超限 */
        }
    } else {
        /* 哈希冲突：桶已被另一个 key 占用
         * Advisor 2026-10-01i 审计：不能静默覆盖——否则旧 key 的 count 永远无法 dec（strcmp 失败）
         * → 永久泄漏 per-key 槽。安全优先：old_count > 0 时拒绝新 key（返回 0），
         * 让调用方 fallback 直连或 486，牺牲一个桶的精度换无泄漏。 */
        if (co->pk[idx].count > 0) {
            ok = 0; /* old key 活跃中 → 拒绝新 key（防泄漏） */
        } else {
            /* old count=0：安全覆盖（旧 key 已完全释放） */
            snprintf(co->pk[idx].key, sizeof(co->pk[idx].key), "%s", key);
            co->pk[idx].count = 1;
            ok = 1;
        }
    }
    pthread_mutex_unlock(&co->pk[idx].lock);
    return ok;
}

static void perkey_dec(b2b_core_t *co, const char *key)
{
    if (!key || !key[0]) return;
    uint32_t idx = perkey_hash(key);
    pthread_mutex_lock(&co->pk[idx].lock);
    if (co->pk[idx].key[0] != '\0' &&
        strcmp(co->pk[idx].key, key) == 0 &&
        co->pk[idx].count > 0) {
        co->pk[idx].count--;
    }
    pthread_mutex_unlock(&co->pk[idx].lock);
}

static uint32_t shard_of(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h & (B2B_CALL_SHARDS - 1);
}

static void copy_tok(char *dst, size_t cap, sip_tok_t t)
{
    size_t n = t.len < cap - 1 ? t.len : cap - 1;
    memcpy(dst, t.p, n);
    dst[n] = '\0';
}

/* 复制头值并截掉既有 ;tag= 参数（用于重写 From/To） */
static void copy_strip_tag(char *dst, size_t cap, sip_tok_t t)
{
    size_t n = t.len < cap - 1 ? t.len : cap - 1;
    for (size_t i = 0; i + 4 < n; ++i) {
        if (!strncasecmp(t.p + i, ";tag=", 5)) {
            n = i;
            break;
        }
    }
    memcpy(dst, t.p, n);
    dst[n] = '\0';
}

/* 同 copy_strip_tag，但参数是 C 字符串（dialog 字段） */
static void strip_tag_str(char *dst, size_t cap, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src) < cap - 1 ? strlen(src) : cap - 1;
    for (size_t i = 0; i + 4 < n; ++i) {
        if (!strncasecmp(src + i, ";tag=", 5)) {
            n = i;
            break;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void call_lock(b2b_call_t *c) { pthread_mutex_lock(&c->lock); }
void call_unlock(b2b_call_t *c) { pthread_mutex_unlock(&c->lock); }

/* ------------------------------------------------------------------ */
/* 呼叫表                                                               */
/* ------------------------------------------------------------------ */
static void tbl_link(b2b_core_t *co, b2b_call_t *c, int leg)
{
    b2b_leg_t *l = &c->leg[leg];
    if (!l->callid[0])
        return;
    call_shard_t *s = &co->sh[shard_of(l->callid, strlen(l->callid))];
    c->link[leg].call = c;
    c->link[leg].leg = leg;
    pthread_mutex_lock(&s->lock);
    c->link[leg].next = s->head;
    s->head = &c->link[leg];
    pthread_mutex_unlock(&s->lock);
}

static void tbl_unlink(b2b_core_t *co, b2b_call_t *c, int leg)
{
    b2b_leg_t *l = &c->leg[leg];
    if (!l->callid[0])
        return;
    call_shard_t *s = &co->sh[shard_of(l->callid, strlen(l->callid))];
    pthread_mutex_lock(&s->lock);
    call_link_t **pp = &s->head;
    while (*pp) {
        if (*pp == &c->link[leg]) {
            *pp = (*pp)->next;
            c->link[leg].next = NULL;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&s->lock);
}

static b2b_call_t *tbl_find(b2b_core_t *co, const char *callid, size_t len,
                            int *leg_out)
{
    if (len >= DLG_CALLID_MAX)
        len = DLG_CALLID_MAX - 1;
    call_shard_t *s = &co->sh[shard_of(callid, len)];
    b2b_call_t *found = NULL;
    pthread_mutex_lock(&s->lock);
    for (call_link_t *p = s->head; p; p = p->next) {
        b2b_leg_t *l = &p->call->leg[p->leg];
        if (strlen(l->callid) == len && !memcmp(l->callid, callid, len)) {
            found = p->call;
            if (leg_out)
                *leg_out = p->leg;
            break;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return found;
}

static void call_ctor(void *obj, void *ud)
{
    (void)ud;
    b2b_call_t *c = obj;
    pthread_mutex_init(&c->lock, NULL);
    tw_node_init(&c->noanswer_node);
    c->prof = NULL;
    c->out_prof = NULL;
}

/* OBS 自埋点（REQ-OBS-001）：呼叫计数器 render，ud=b2b_core；
 * 2026-09-27 A3 自 oam/obs_metrics.c 原样迁入，指标名保持不变。 */
static int r_calls(char *b, size_t cap, void *ud)
{
    b2b_core_t *c = (b2b_core_t *)ud;
    if (!c)
        return snprintf(b, cap, "nsbc_calls_active 0\nnsbc_calls_total 0\n"
                               "nsbc_calls_failed 0\n");
    return snprintf(b, cap,
                    "nsbc_calls_active %llu\nnsbc_calls_total %llu\n"
                    "nsbc_calls_failed %llu\n",
                    (unsigned long long)b2b_active_calls(c),
                    (unsigned long long)b2b_total_calls(c),
                    (unsigned long long)b2b_failed_calls(c));
}

int b2b_core_create(const b2b_cfg_t *cfg, b2b_core_t **out)
{
    b2b_core_t *co = calloc(1, sizeof(*co));
    if (!co)
        return NSBC_NOMEM;
    co->cfg = *cfg;
    if (co->cfg.capacity <= 0)
        co->cfg.capacity = 50000;
    /* P0：没传 sig_arena 就自己建一个（mem_slab 要求 arena 非空）
     * arena 大小 = capacity * stride + slack（arena_alloc 用） */
    if (!co->cfg.sig_arena) {
        size_t stride = (sizeof(b2b_call_t) + 7u) & ~7u; /* 8 字节对齐 */
        size_t need = (size_t)co->cfg.capacity * stride;
        if (need < 4ull * 1024 * 1024)
            need = 4ull * 1024 * 1024;
        mem_arena_cfg_t acfg = {
            MEM_DOMAIN_SIGNAL, need * 3 / 2, 0, 0 /* 1.5x slack */
        };
        llog(LOG_INFO, "b2b",
             "arena plan: sizeof(b2b_call_t)=%zu stride=%zu cap=%u "
             "need=%zu arena_bytes=%zu slack=1.5x",
             sizeof(b2b_call_t), stride, co->cfg.capacity,
             need, acfg.bytes);
        nsbc_rc_t arc = mem_arena_create(&acfg, &co->cfg.sig_arena);
        if (arc != NSBC_OK) {
            llog(LOG_CRIT, "b2b", "arena create failed rc=%d bytes=%zu",
                 arc, acfg.bytes);
            free(co);
            return arc;
        }
        size_t free_payload = mem_arena_free_payload(co->cfg.sig_arena);
        size_t largest = mem_arena_largest_free(co->cfg.sig_arena);
        llog(LOG_INFO, "b2b",
             "arena created ok: total=%zu free_payload=%zu largest_free=%zu",
             acfg.bytes, free_payload, largest);
    }

    /* --- slab 预期用量诊断 --- */
    {
        size_t obj_size = (sizeof(b2b_call_t) + 15u) & ~15u;  /* 16 对齐 */
        size_t hdr = 48; /* MEM_OBJ_HDR_SIZE 估算 */
        size_t total_stride = hdr + obj_size;
        size_t slab_obj_bytes = (size_t)co->cfg.capacity * total_stride;
        size_t slab_stack_bytes = (size_t)co->cfg.capacity * sizeof(int);
        size_t free_now = mem_arena_free_payload(co->cfg.sig_arena);
        size_t largest_now = mem_arena_largest_free(co->cfg.sig_arena);
        llog(LOG_INFO, "b2b",
             "slab expect: obj_size=%zu hdr=%zu stride=%zu slab_obj=%zu "
             "slab_stack=%zu arena_free=%zu arena_largest=%zu",
             obj_size, hdr, total_stride, slab_obj_bytes, slab_stack_bytes,
             free_now, largest_now);
    }
    for (int i = 0; i < B2B_CALL_SHARDS; ++i)
        pthread_mutex_init(&co->sh[i].lock, NULL);
    for (int i = 0; i < B2B_PERKEY_N; ++i)
        pthread_mutex_init(&co->pk[i].lock, NULL);

    mem_slab_cfg_t sc = {"b2b_call", sizeof(b2b_call_t),
                         co->cfg.capacity, 8, co->cfg.sig_arena,
                         call_ctor, NULL};
    nsbc_rc_t rc = mem_slab_create(&sc, &co->slab);
    if (rc != NSBC_OK) {
        size_t free_n = mem_arena_free_payload(co->cfg.sig_arena);
        size_t largest_n = mem_arena_largest_free(co->cfg.sig_arena);
        llog(LOG_CRIT, "b2b",
             "slab create failed rc=%d cap=%u arena_free=%zu arena_largest=%zu "
             "arena_frag=%.2f — 可能是 arena 容量不足，请增大 1.5x slack "
             "或调小 sip.max_concurrent_calls",
             rc, co->cfg.capacity, free_n, largest_n,
             mem_arena_frag_rate(co->cfg.sig_arena));
        free(co);
        return rc;
    }
    /* Session Timer 默认配置 */
    sess_timer_cfg_default(&co->cfg.sess_timer_cfg);
    co->cfg.sess_timer_enabled = 1; /* 开启 P1 完整模式：协商 + 定时刷新/超时 */
    metric_source_register("nsbc_calls", "call counters", r_calls, co);
    *out = co;
    return NSBC_OK;
}

void b2b_core_destroy(b2b_core_t *co)
{
    if (!co)
        return;
    co->stopping = 1;   /* REQ-ORIGINATE-003 待补：唤醒 bridge wait loop 尽快退出 */
    mem_slab_destroy(co->slab);
    for (int i = 0; i < B2B_CALL_SHARDS; ++i)
        pthread_mutex_destroy(&co->sh[i].lock);
    for (int i = 0; i < B2B_PERKEY_N; ++i)
        pthread_mutex_destroy(&co->pk[i].lock);
    free(co);
}

/* ------------------------------------------------------------------ */
/* 发送原语                                                             */
/* ------------------------------------------------------------------ */
static int tr_send_leg(b2b_leg_t *l, const char *buf, size_t len,
                       b2b_core_t *co)
{
    return sip_tr_send(co->cfg.tr, l->proto, buf, len, &l->dst);
}

static void detach_txn(sip_txn_t *t)
{
    if (t)
        txn_set_ud(t, NULL);
}

/* 呼叫销毁：摘除表、关对话、断开关联后归还 slab */
static void call_destroy(b2b_call_t *c)
{
    b2b_core_t *co = c->core;
    if (c->dead)
        return;
    c->dead = 1;
    llog(LOG_INFO, "b2b", "call_destroy BEGIN callid=%s a_state=%d b_state=%d a_final=%d",
         c->leg[0].callid, c->leg[0].state, c->leg[1].state, c->a_final);
    /* SIP Profile active_calls 对称递减（handle_new_invite 归属时 +1 配对）。
     * ★ 注意：不能在这里置 c->prof=NULL —— 后面 g_end_hooks / on_end / b2b_emit_cdr(END)
     * 都要读 c->prof->name 发事件。真正的 NULL 赋值在 mem_obj_release 之前。 */
    if (c->prof) {
        __sync_fetch_and_sub(&c->prof->active_calls, 1u);
    }
    disarm_noanswer_timer(c); /* catch-all：所有终止路径最终走到这里 */
    /* Session Timer: disarm all timer nodes */
    if (c->core->cfg.tw) {
        tw_del(c->core->cfg.tw, &c->leg[0].refresh_node);
        tw_del(c->core->cfg.tw, &c->leg[0].expire_node);
        tw_del(c->core->cfg.tw, &c->leg[1].refresh_node);
        tw_del(c->core->cfg.tw, &c->leg[1].expire_node);
    }
    tbl_unlink(co, c, 0);
    tbl_unlink(co, c, 1);
    concurrent_dec(co); /* REQ-ARCH-015：call 生命周期结束，归还全局并发槽 */
    /* REQ-ARCH-015：per-key 并发槽释放
     * key_held=1 表示 resolve 成功后 perkey_try_inc 通过（占了槽）；
     * key_held=0 可能是：resolve 失败（404 路径直接走 call_destroy），
     *   或 perkey 超限返回 486（try_inc 失败所以没占槽）。
     * perkey_dec 带空 key/key 不匹配保护，安全可多次调用。 */
    if (c->key_held) {
        perkey_dec(co, c->b_key);
        c->key_held = 0;
    }
    for (int i = 0; i < 2; ++i) {
        detach_txn(c->leg[i].inv_txn);
        detach_txn(c->leg[i].srv_txn);
        detach_txn(c->leg[i].out_txn);
        detach_txn(c->leg[i].cancel_txn);
        if (c->leg[i].dlg)
            dlg_close(co->cfg.dlg, c->leg[i].dlg);
    }
    detach_txn(c->refer_txn);
    /* REQ-REC-006：call 结束时自动停录制——走 recording 模块统一 registry+stop 路径
     * Advisor 2026-10-01l：先 atomic NULL call->rec[leg] 消除媒体线程 UAF 窗口
     * （recording_apply_dp_action 内部 reg_get 还能找到 sess，因为 registry reg_del 还没跑） */
    if (co->cfg.rec_mgr && c->leg[0].callid[0]) {
        __atomic_store_n((void **)&c->rec[0], NULL, __ATOMIC_RELEASE);
        __atomic_store_n((void **)&c->rec[1], NULL, __ATOMIC_RELEASE);
        /* 内联提取 caller/callee（sip_uri_extract_user 定义在文件底部 static，
         * 此处提前不可见——与 signaling.c 的做法保持一致） */
        char call_destroy_caller[64] = {0};
        char call_destroy_callee[64] = {0};
        const char *from_uri = c->leg[0].from;
        const char *ruri_uri = c->leg[0].ruri;
        if (from_uri[0]) {
            const char *p = from_uri;
            if (*p == '<') p++;
            if (!strncasecmp(p, "sip:", 4)) p += 4;
            else if (!strncasecmp(p, "tel:", 4)) p += 4;
            const char *at = strchr(p, '@');
            const char *end = at ? at : (p + strlen(p));
            while (end > p && (*(end-1) == '>' || *(end-1) == ';')) --end;
            size_t n = (size_t)(end - p);
            if (n >= sizeof(call_destroy_caller)) n = sizeof(call_destroy_caller) - 1;
            memcpy(call_destroy_caller, p, n); call_destroy_caller[n] = '\0';
        }
        if (ruri_uri[0]) {
            const char *p = ruri_uri;
            if (!strncasecmp(p, "sip:", 4)) p += 4;
            else if (!strncasecmp(p, "tel:", 4)) p += 4;
            const char *at = strchr(p, '@');
            const char *end = at ? at : (p + strlen(p));
            while (end > p && (*(end-1) == '>' || *(end-1) == ';')) --end;
            size_t n = (size_t)(end - p);
            if (n >= sizeof(call_destroy_callee)) n = sizeof(call_destroy_callee) - 1;
            memcpy(call_destroy_callee, p, n); call_destroy_callee[n] = '\0';
        }
        char destroy_rec_path[CDR_REC_MAX] = {0};
        recording_apply_dp_action(co->cfg.rec_mgr, 2,
                                  c->leg[0].callid, "call_destroy",
                                  NULL,
                                  call_destroy_caller[0] ? call_destroy_caller : NULL,
                                  call_destroy_callee[0] ? call_destroy_callee : NULL,
                                  destroy_rec_path, sizeof(destroy_rec_path));
        /* 录音终态路径存进 call 对象：b2b_emit_cdr(END) 会把它写进 CDR，
         * 之前传 NULL 导致 Web 端“录音”列永远是 '-'。 */
        if (destroy_rec_path[0] && !c->rec_path[0])
            snprintf(c->rec_path, sizeof(c->rec_path), "%s", destroy_rec_path);
    }
    /* REQ-MEDIA-BUG-001：per-call bugs 自动清理
     * ——global consumers 不依赖 call_key，不受 call_destroy 影响；
     * per-call 手动 attach 的必须在 call 结束时 detach，避免悬挂指针。 */
    media_bug_detach_all_for_call(c);
    /* REQ-AI-001 / REQ-EVENTS-001：多模块 call_destroy hook 遍历
     * ——独立于 b2b_cfg_t.on_end 单槽，让 ai_dialog / event_handlers / ...
     * 都能在呼叫销毁前拿到通知（ai_dialog 要在这里关 per-call session 线程）。
     * 回调必须尽快返回，禁阻塞。
     *
     * 白皮书 §10.3：hook 链内部禁止发 Event。聚合 Event 在所有 hook 跑完后统一发。*/
    for (int i = 0; i < g_end_hook_n; ++i)
        g_end_hooks[i].fn(g_end_hooks[i].ud ? c : c,
                          c->a_final,
                          g_end_hooks[i].ud);

    if (c->active)
        add_atomic(&co->active, -1);
    /* on_end（sig_on_end_cb）必须先于 b2b_emit_cdr(END) 执行：
     * 里面从 bridge 视图计算 rtp_loss_pct/rtp_jitter_ms/mos_x1000 并存回 c，
     * 兜底停录音产生 c->rec_path——这些字段是 END ev 的数据源。
     * 旧顺序（emit → push → on_end）导致 END ev 拿不到 VQ/rec_path。 */
    if (co->cfg.on_end)
        co->cfg.on_end(c, co->cfg.end_ud);

    /* REQ-PBX-CDR-001：END emit — 所有 end hooks 跑完后统一发，
     * 此时 a_final/cause 等终态字段已确定，billing 模块按 call_id 聚合 END 写库。
     * b2b_emit_cdr END 分支同时把完整 ev 序列化进 c->cdr_json（供下方 SESSION_END 推送）。 */
    b2b_emit_cdr(c, CDR_PHASE_END);

    /* 白皮书 §10.3：所有 hook 跑完后统一发聚合 SESSION_END 事件。
     * payload 携带完整 CDR 快照（b2b_emit_cdr END 已写入 c->cdr_json）。
     * event_handler_trigger 是同步订阅链（SESSION_CREATE/ANSWER/END），
     * nsbc_event_push 是异步 Per-Core Ring（双通道）。
     *
     * REQ-UDS-002：UDS 推送的 payload 必须含 event/state/dir/cause，
     * Rust 侧 dispatch_session_push 解析这些字段写入 calls 表。缺字段会
     * 被 Rust 侧空串保护拦住，但正确补齐是规范。dir 由 orig_caller_id_num
     * 是否非空区分（originate 路专用字段，入向 INVITE 不会设置 → memset 初始全 0）。
     * cause 取 A 腿最终状态码。*/
    {
        int dir_out = c->orig_caller_id_num[0] != '\0';
        char end_json[1024];
        if (c->cdr_json[0]) {
            /* b2b_emit_cdr(END) 产出的完整 CDR JSON，直接透传 UDS */
            snprintf(end_json, sizeof(end_json), "%s", c->cdr_json);
        } else {
            /* P0 降级：CDR END 链路异常（c->cdr_json 没填上）——
             * 最简 snprintf 保底，UDS/Rust 侧空串保护兜底 */
            snprintf(end_json, sizeof(end_json),
                "{\"event\":\"end\",\"call_id\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
                "\"state\":\"terminated\",\"dir\":\"%s\",\"cause\":%d,"
                "\"duration\":%d,\"billsec\":0,\"disposition\":%d}",
                c->leg[0].callid,
                c->leg[0].from, c->leg[0].ruri,
                dir_out ? "outbound" : "inbound",
                c->a_final,
                (int)(c->leg[1].state - c->leg[0].state),
                c->a_final);
        }
        uint64_t seq = 0;
        int rc = event_handler_trigger("SESSION_END", end_json, &c->leg[0].callid, &seq);
        if (rc < 0) llog(LOG_WARN, "b2b", "SESSION_END handler rc=%d callid=%s", rc, c->leg[0].callid);
        /* 同时推异步 Ring Buffer（双通道，白皮书 §5.2）*/
        nsbc_event_push("SESSION_END", end_json);
        /* 同时推 UDS session 通道给 Web 端 */
        uds_send_session(end_json);
    }

    /* REQ-CHAN-VAR-001：per-call 通道变量销毁（必须在 mem_obj_release 之前，
     * 因为 slab 复用会把槽归零——不提前 destroy 会泄漏 arena-backed 哈希表）。
     * 懒初始化：vars 可能一直是 NULL（呼叫全程没 setvar）。 */
    if (c->vars) {
        chan_var_destroy(c->vars);
        c->vars = NULL;
    }
    /* 所有事件/回调跑完后再置 NULL —— 保证 g_end_hooks / on_end / b2b_emit_cdr(END)
     * / SESSION_END 发事件时 c->prof / c->out_prof 还有效。mem_obj_release 之后
     * 内存就被 slab 回收复用了。 */
    c->prof = NULL;
    c->out_prof = NULL;
    mem_obj_release(c);
}

/* 由服务端事务缓存的原始请求构造应答并经事务发出 */
static int respond_txn(b2b_core_t *co, sip_txn_t *t, int status,
                       const char *contact, const char *extra,
                       const char *body, size_t bodylen)
{
    if (!t)
        return NSBC_INVAL;
    size_t rlen = 0;
    const char *raw = txn_first_request(t, &rlen);
    if (!raw)
        return NSBC_ERR;
    sip_msg_t req;
    if (sip_parse(&req, raw, rlen) != NSBC_OK)
        return NSBC_PROTO;
    char out[SIP_MAX_MSG];
    /* 取本侧 to-tag：服务端事务对应的腿 */
    const char *tag = NULL;
    (void)co;
    int n = sip_build_response(out, sizeof(out), &req, status, NULL, tag,
                               contact, extra, body, bodylen);
    if (n < 0)
        return NSBC_NOSPC;
    return txn_send_response(t, out, (size_t)n, now_ms());
}

/* Route 头块（对话路由集，UAC 逆序已在 dlg 层处理） */
static void route_block(const b2b_leg_t *l, char *out, size_t outsz)
{
    out[0] = '\0';
    size_t used = 0;
    /* IMS Service-Route（RFC 3608）：最前——UE 后续 INVITE Route 链的第一个
     * hop 必须是 registrar 返回的 Service-Route 值。
     * ★ registrar.c L659 生成的 service_route 值已经带尖括号 "<sip:tenant;lr>"，
     *   这里直接拼完整头行，不要再 snprintf 包一层尖括号！ */
    if (l->service_route[0]) {
        int n = snprintf(out + used, outsz - used,
                         "Route: %s\r\n", l->service_route);
        if (n > 0 && (size_t)n < outsz - used) used += (size_t)n;
    }
    for (int i = 0; i < l->nroutes; ++i) {
        /* routes[i] 存的是原始头 value——可能带尖括号（从 Route/Record-Route
         * 头直接拷的）或不带（历史遗留的 host-only 值）。智能判断：已经带
         * "< >" 就不再包尖括号，否则包。同时跳过逗号（多段只取第一段） */
        const char *v = l->routes[i];
        /* 跳过前导逗号（多值 comma-separated 场景） */
        while (*v == ' ' || *v == ',') v++;
        if (*v == '<') {
            /* 已经是完整 "<uri>" 格式 */
            int n = snprintf(out + used, outsz - used, "Route: %s\r\n", v);
            if (n < 0 || (size_t)n >= outsz - used) break;
            used += (size_t)n;
        } else if (*v != '\0') {
            /* 纯 URI（历史遗留 host-only）→ 包尖括号 */
            int n = snprintf(out + used, outsz - used, "Route: <%s>\r\n", v);
            if (n < 0 || (size_t)n >= outsz - used) break;
            used += (size_t)n;
        }
    }
}

/* REQ-SIP-DIALOG-TAG-001：dialog remote tag = 对端 From 头的 tag。
 * INVITE 进来（UAS）→ 取 INVITE From 的 tag；
 * 200 OK 回来（UAC）→ 取 200 OK From 的 tag（即 UAS 的 From）。 */
static void tok_from_tag(const sip_msg_t *m, char *dst, size_t cap)
{
    sip_hdr_t *h = sip_hdr_get(m, SIP_H_FROM);
    if (!h || sip_hdr_parse_addr(h) != NSBC_OK) {
        dst[0] = '\0';
        return;
    }
    copy_tok(dst, cap, h->u.addr.tag);
}

/* ACK 专用：对端 dialog local tag = 响应的 To 头 tag。
 * 200 OK / 终响应的 To 头 tag 是 UAS 在 dialog 里的 local tag。
 * ACK 必须用这个 tag 才能被 UAS 正确识别（RFC 3261 §12.2）。 */
static void tok_to_tag(const sip_msg_t *m, char *dst, size_t cap)
{
    sip_hdr_t *h = sip_hdr_get(m, SIP_H_TO);
    if (!h || sip_hdr_parse_addr(h) != NSBC_OK) {
        dst[0] = '\0';
        return;
    }
    copy_tok(dst, cap, h->u.addr.tag);
}

/* 从 From header URI 提取主叫 user part → call->orig_number
 * e.g. From: "Alice" <sip:1000@nsbc.local>;tag=abc → "1000"
 * 用于呼叫表/CDR 显示真实主叫（之前错用 R-URI user = 被叫）。 */
static void tok_from_user(const sip_msg_t *m, char *dst, size_t cap)
{
    sip_hdr_t *h = sip_hdr_get(m, SIP_H_FROM);
    if (!h || sip_hdr_parse_addr(h) != NSBC_OK) {
        dst[0] = '\0';
        return;
    }
    /* 优先取 URI 的 user part（<sip:user@host> 里的 user） */
    if (h->u.addr.uri.user.p && h->u.addr.uri.user.len > 0) {
        copy_tok(dst, cap, h->u.addr.uri.user);
        return;
    }
    /* 无尖括号形式：From: 1000@host → 整个值就是 URI，再 parse 一次 */
    sip_uri_t u2;
    if (sip_parse_uri(&h->value, &u2) == NSBC_OK && u2.user.p && u2.user.len > 0) {
        copy_tok(dst, cap, u2.user);
        return;
    }
    dst[0] = '\0';
}

static void cli_tu(sip_txn_t *t, const sip_msg_t *m, txn_event_t ev,
                   void *ud);

/* 新随机 branch（含 magic cookie） */
static void new_branch(char *out, size_t outsz)
{
    uint8_t r[8];
    nsbc_rand_fill(r, sizeof(r));
    snprintf(out, outsz, "z9hG4bKnsbc%02x%02x%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
}

/* 在本腿对话内构造请求（CANCEL/ACK/BYE/re-INVITE） */
static int build_dlg_req(b2b_core_t *co, b2b_leg_t *l,
                         sip_method_t method, long cseq,
                         const char *ruri, const char *to_with_tag,
                         const char *branch, const char *extra,
                         const char *body, size_t bodylen,
                         char *out, size_t outsz)
{
    const char *mn = "BYE";
    switch (method) {
    case SIP_M_ACK: mn = "ACK"; break;
    case SIP_M_CANCEL: mn = "CANCEL"; break;
    case SIP_M_BYE: mn = "BYE"; break;
    case SIP_M_INVITE: mn = "INVITE"; break;
    case SIP_M_OPTIONS: mn = "OPTIONS"; break;
    case SIP_M_REFER: mn = "REFER"; break;
    case SIP_M_UPDATE: mn = "UPDATE"; break;
    case SIP_M_PRACK: mn = "PRACK"; break;
    case SIP_M_NOTIFY: mn = "NOTIFY"; break;
    case SIP_M_SUBSCRIBE: mn = "SUBSCRIBE"; break;
    case SIP_M_MESSAGE: mn = "MESSAGE"; break;
    case SIP_M_INFO: mn = "INFO"; break;
    default: break;
    }
    char rt[DLG_URI_MAX * DLG_ROUTE_MAX + 64];
    if (extra)
        rt[0] = '\0';
    else
        route_block(l, rt, sizeof(rt));
    /* REQ-NAT-001：dialog 内请求（BYE/PRACK/UPDATE 等）Via host 也走 external_sip_ip */
    /* REQ-NAT-002：优先 per-call profile 的 external_sip_ip，fallback cfg */
    /* build_dlg_req 只有 b2b_core_t*co、b2b_leg_t*l——通过 l->dlg->ud 反查 b2b_call_t */
    b2b_call_t *call_ref = (l && l->dlg) ? (b2b_call_t *)l->dlg->ud : NULL;
    const char *prof_ext = (call_ref && call_ref->prof && call_ref->prof->external_sip_ip[0])
                           ? call_ref->prof->external_sip_ip : co->cfg.external_sip_ip;
    const char *adv_host = (prof_ext && prof_ext[0]) ? prof_ext : co->cfg.local_host;
    const char *ex = extra ? extra : rt;
    const char *tr = l->proto == SIP_TR_TCP ? "TCP"
                    : l->proto == SIP_TR_TLS ? "TLS" : "UDP";
    /* REQ-NAT-003：dialog 内请求也走 per-call profile 的 sip_udp_port */
    char contact[B2B_HDR_MAX];
    uint16_t dlg_port = (call_ref && call_ref->prof && call_ref->prof->sip_udp_port)
                        ? call_ref->prof->sip_udp_port : (uint16_t)co->cfg.local_udp_port;
    /* GRUU（RFC 5627）：全局唯一 instance-id 拼到 Contact URI 参数 */
    if (co->cfg.gruu_instance_id[0]) {
        snprintf(contact, sizeof(contact),
                 "<sip:%s:%d;+sip.instance=\"%s\">",
                 adv_host, dlg_port, co->cfg.gruu_instance_id);
    } else {
        snprintf(contact, sizeof(contact), "<sip:%s:%d>", adv_host, dlg_port);
    }
    sip_req_spec_t s; memset(&s, 0, sizeof(s));
    /* REQ-SIP-RURI-001：Request-URI 不允许 <>（name-addr 只用于 From/To/Contact 头）。
     * dlg->target 来自 200 OK Contact 头（<sip:...>），会带 <>，
     * 如果不剥掉 → SIP.js Parser 解析首行 "BYE <sip:...> SIP/2.0" 失败丢弃整条消息。 */
    char ruri_stripped[DLG_URI_MAX];
    if (ruri && ruri[0]) {
        size_t rl = strlen(ruri);
        if (rl >= 2 && ruri[0] == '<' && ruri[rl - 1] == '>') {
            snprintf(ruri_stripped, sizeof(ruri_stripped), "%.*s",
                     (int)(rl - 2), ruri + 1);
            s.ruri = ruri_stripped;
        } else {
            s.ruri = ruri;
        }
    } else {
        s.ruri = ruri;
    }
    s.method = mn;
    /* REQ-SIP-DIALOG-FROMTO-001：dialog 内请求 From/To 必须按对话方向选（RFC 3261 §12）。
     *
     * DLG_UAC (SBC 发起 INVITE，= B 腿)：
     *   Local From = l->from（send_invite_b 构造的正确值：主叫 URI + b->loc_tag）
     *   Remote To  = l->to（被叫 URI）+ ;tag= + l->rem_tag（被叫的 From tag）
     *
     * DLG_UAS (SBC 应答 INVITE，= A 腿)：
     *   Local From = l->to（INVITE To = 被叫 URI）+ ;tag= + l->loc_tag（SBC 的本地 tag）
     *   Remote To  = strip_tag(l->from)（INVITE From 的 URI 部分 = 主叫 URI）+ ;tag= + l->rem_tag（主叫的 From tag）
     *
     * ❌ 之前直接 s.from = l->from — 对于 UAS 腿，l->from 存的是 INVITE From（对端的 URI + 对端 tag），
     *    根本不是 SBC 在对话里的 Local From！导致 BYE From/To 完全反 + tag 方向错 → 对端 481！
     */
    char dlg_local_from[B2B_HDR_MAX + DLG_TAG_MAX + 8];
    char dlg_remote_to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
    int is_uas = (l->dlg && l->dlg->dir == DLG_UAS);
    if (is_uas) {
        /* UAS Local From = INVITE To（被叫 URI，无 tag）+ SBC 的 loc_tag */
        snprintf(dlg_local_from, sizeof(dlg_local_from), "%s;tag=%s",
                 l->to, l->loc_tag);
        s.from = dlg_local_from;
    } else {
        s.from = l->from;
    }
    /* to_with_tag：调用者（send_bye_leg/send_ack_leg 等）传入正确的 To+tag；
     * CANCEL 传 NULL（CANCEL 非 dialog 内请求，To 与 INVITE 相同无 tag） */
    if (to_with_tag && to_with_tag[0]) {
        if (is_uas) {
            /* UAS 调用者错误地用了 l->to（被叫 URI）+ rem_tag — 修正为
             * strip_tag(l->from)（主叫 URI）+ rem_tag */
            char from_uri_only[B2B_HDR_MAX];
            strip_tag_str(from_uri_only, sizeof(from_uri_only), l->from);
            snprintf(dlg_remote_to, sizeof(dlg_remote_to), "%s;tag=%s",
                     from_uri_only, l->rem_tag);
            s.to = dlg_remote_to;
        } else {
            s.to = to_with_tag;
        }
    } else if (is_uas) {
        char from_uri_only[B2B_HDR_MAX];
        strip_tag_str(from_uri_only, sizeof(from_uri_only), l->from);
        snprintf(dlg_remote_to, sizeof(dlg_remote_to), "%s;tag=%s",
                 from_uri_only, l->rem_tag);
        s.to = dlg_remote_to;
    } else {
        s.to = (to_with_tag && to_with_tag[0]) ? to_with_tag : l->to;
    }
    s.contact = contact; s.callid = l->callid; s.cseq = cseq;
    s.via_host = adv_host; s.via_port = dlg_port;
    s.transport = tr; s.branch = branch; s.max_forwards = 70;
    s.user_agent = co->cfg.user_agent;
    s.extra = ex[0] ? ex : NULL; s.body = body; s.bodylen = bodylen;
    s.content_type = bodylen ? "application/sdp" : NULL;
    return sip_build_request(out, outsz, &s);
}

/* ====================================================================== */
/* P2-16：呼出 From 覆盖 helper（文档 §4.1.1 + §4.1.3）                    */
/* ====================================================================== */

/* 专家 2.5：from_user 空值回退链（显式图）
 *   tr->from_user != "" ? tr->from_user : tr->auth_user */
static const char *p216_resolve_from_user(const trunk_t *tr, int caller_id_in_from,
                                          const char *orig_number)
{
    /* 专家 2.4：caller_id_in_from=1 只替换 URI user part（FS 语义）
     * host 域仍用 resolve_from_host(tr)，Display name 也用主叫号码 */
    if (caller_id_in_from && orig_number && orig_number[0])
        return orig_number;
    return tr->from_user[0] ? tr->from_user : tr->auth_user;
}

/* 专家 2.5：from_domain 回退链（对齐 FreeSWITCH + FS 默认漏的 proxy 层）
 *   tr->from_domain → tr->realm → tr->proxy → tr->host */
static const char *p216_resolve_from_host(const trunk_t *tr)
{
    if (tr->from_domain[0]) return tr->from_domain;
    if (tr->realm[0])       return tr->realm;
    if (tr->proxy[0])       return tr->proxy;
    return tr->host;
}

/* ------------------------------------------------------------------ */
/* B 腿外发                                                             */
/* ------------------------------------------------------------------ */

/* REQ-TRUNK-HDR-001：trunk 自定义头变量替换
 *
 * 支持占位符：
 *   ${callid}    — 完整 Call-ID（b->callid）
 *   ${caller}    — 最终生效的主叫号（c->effective_caller_id_num，fallback a->from_user）
 *   ${callee}    — 最终被叫号码（c->dst_number，拨号规则改写后）
 *   ${from_user} — B 腿 From URI 的 user part（@ 之前）
 *   ${to_user}   — B 腿 To URI 的 user part（@ 之前）
 *   ${from_ip}   — A 腿源 IP（handle_new_invite 存入 c->from_ip，纯 IP 无端口）
 *   ${to_ip}     — B 腿目标 IP（从 b->dst 提取纯 IP 部分）
 * 未识别的占位符原样保留（日志打 WARN）。
 * 扫描 input 逐字符拷贝，遇到 ${...} 查表替换，输出到 result。 */
static void substitute_custom_vars(const char *input, char *result, size_t result_sz,
                                   const b2b_call_t *c, const b2b_leg_t *a, const b2b_leg_t *b)
{
    if (!input || !result || result_sz == 0) return;
    result[0] = '\0';
    size_t ri = 0;
    const char *p = input;
    while (*p && ri + 1 < result_sz) {
        if (p[0] == '$' && p[1] == '{') {
            const char *end = strchr(p + 2, '}');
            if (!end) {
                /* 没找到闭合 } → 原样拷贝 $ 继续 */
                if (ri + 2 < result_sz) { result[ri++] = *p++; continue; }
                else break;
            }
            size_t vlen = (size_t)(end - p - 2);
            if (vlen == 0 || vlen > 32) {
                /* 空变量或超长 → 原样拷贝 */
                if (ri + 1 < result_sz) { result[ri++] = *p++; continue; }
                else break;
            }
            char vname[40]; memcpy(vname, p + 2, vlen); vname[vlen] = '\0';
            const char *val = NULL;
            if (strcmp(vname, "callid") == 0) {
                val = b->callid[0] ? b->callid : "";
            } else if (strcmp(vname, "caller") == 0) {
                val = c->effective_caller_id_num[0] ? c->effective_caller_id_num : NULL;
                if (!val) {
                    /* fallback：从 A 腿 from 头里提取 user part（跳过 display name 和 < ） */
                    static __thread char a_from_user[B2B_HDR_MAX];
                    a_from_user[0] = '\0';
                    const char *lt = strchr(a->from, '<');
                    const char *start = lt ? lt + 1 : a->from;
                    const char *at = strchr(start, '@');
                    if (at) {
                        size_t ulen = (size_t)(at - start);
                        if (ulen >= sizeof(a_from_user)) ulen = sizeof(a_from_user) - 1;
                        memcpy(a_from_user, start, ulen); a_from_user[ulen] = '\0';
                    }
                    val = a_from_user;
                }
            } else if (strcmp(vname, "callee") == 0) {
                val = c->dst_number[0] ? c->dst_number : "";
            } else if (strcmp(vname, "from_user") == 0) {
                /* 从 b->from 里提取 user part（跳过 display name 和 < ，找 sip:user@host 里的 @ 前半） */
                static __thread char from_user_buf[B2B_HDR_MAX];
                from_user_buf[0] = '\0';
                const char *lt = strchr(b->from, '<');
                const char *start = lt ? lt + 1 : b->from;
                const char *colon = strchr(start, ':');
                if (colon) start = colon + 1; /* 跳过 sip: */
                const char *at = strchr(start, '@');
                if (at) {
                    size_t ulen = (size_t)(at - start);
                    if (ulen >= sizeof(from_user_buf)) ulen = sizeof(from_user_buf) - 1;
                    memcpy(from_user_buf, start, ulen); from_user_buf[ulen] = '\0';
                }
                val = from_user_buf[0] ? from_user_buf : "";
            } else if (strcmp(vname, "to_user") == 0) {
                static __thread char to_user_buf[B2B_HDR_MAX];
                to_user_buf[0] = '\0';
                const char *lt = strchr(b->to, '<');
                const char *start = lt ? lt + 1 : b->to;
                const char *colon = strchr(start, ':');
                if (colon) start = colon + 1; /* 跳过 sip: */
                const char *at = strchr(start, '@');
                if (at) {
                    size_t ulen = (size_t)(at - start);
                    if (ulen >= sizeof(to_user_buf)) ulen = sizeof(to_user_buf) - 1;
                    memcpy(to_user_buf, start, ulen); to_user_buf[ulen] = '\0';
                }
                val = to_user_buf[0] ? to_user_buf : "";
            } else if (strcmp(vname, "from_ip") == 0) {
                /* A 腿源 IP（handle_new_invite 存入 c->from_ip） */
                val = c->from_ip[0] ? c->from_ip : "";
            } else if (strcmp(vname, "to_ip") == 0) {
                /* B 腿目标 IP（从 b->dst 提取纯 IP 部分） */
                static __thread char to_ip_buf[INET6_ADDRSTRLEN];
                sa_ip_str(&b->dst, to_ip_buf, sizeof(to_ip_buf));
                val = to_ip_buf[0] ? to_ip_buf : "";
            } else {
                llog(LOG_WARN, "b2b", "trunk custom_headers: unknown variable '${%s}' callid=%s",
                     vname, b->callid);
                /* 未知变量 → 原样保留 ${xxx} */
                size_t plen = (size_t)(end - p + 1);
                if (ri + plen < result_sz) {
                    memcpy(result + ri, p, plen); ri += plen;
                }
                p = end + 1;
                continue;
            }
            if (val) {
                size_t vallen = strlen(val);
                if (ri + vallen < result_sz) {
                    memcpy(result + ri, val, vallen); ri += vallen;
                } else {
                    /* 空间不够 → 截断 */
                    size_t room = result_sz - ri - 1;
                    memcpy(result + ri, val, room); ri += room;
                }
            }
            p = end + 1;
        } else {
            result[ri++] = *p++;
        }
    }
    result[ri] = '\0';
}

/* 发 INVITE（初始 / re-INVITE / 302 follow 重定向）。
 * leg_idx=0 → bridge 模式外呼 A 腿；leg_idx=1 → 常规 B 腿。
 * extra 为已格式化好的额外头块（如 History-Info 累积），每个头自带 CRLF，可 NULL。 */
static sip_txn_t *send_invite_b(b2b_call_t *c, int leg_idx,
                                const char *body, size_t bodylen, int reinv,
                                const char *extra)
{
    b2b_core_t *co = c->core;
    b2b_leg_t *a = &c->leg[0];
    b2b_leg_t *b = &c->leg[leg_idx];
    char branch[40];
    new_branch(branch, sizeof(branch));

    /* REQ-NAT-001：出站 SIP 层宣告 host = external_sip_ip（空则走 local_host）
     * FreeSWITCH ext-sip-ip 等效。Call-ID 后缀不改（标识符非寻址）。
     * REQ-NAT-002：优先 per-call profile 的 external_sip_ip，fallback cfg
     * ★ 2026-10-19：用 out_prof（B 腿出站 profile，resolve 按 dp_layer 设），
     *   OUT 层自动走 external profile；EXT/BIZ 走 c->prof */
    struct sip_profile *op = c->out_prof ? c->out_prof : c->prof;
    const char *prof_ext = (op && op->external_sip_ip[0])
                           ? op->external_sip_ip : co->cfg.external_sip_ip;
    const char *adv_host = (prof_ext && prof_ext[0]) ? prof_ext : co->cfg.local_host;

    char rt[DLG_URI_MAX * DLG_ROUTE_MAX + 64];
    route_block(b, rt, sizeof(rt));
    /* REQ-DP-SIP-001：Contact 头支持 sip_invite_contact_params 追加 */
    /* REQ-NAT-003：per-call profile 优先用 sip_udp_port，fallback cfg 默认 5060 */
    uint16_t adv_port = (op && op->sip_udp_port)
                        ? op->sip_udp_port : (uint16_t)co->cfg.local_udp_port;
    char contact[B2B_HDR_MAX];
    /* GRUU（RFC 5627）：全局唯一 instance-id 拼到 Contact URI 参数链末尾 */
    const char *gruu_suffix = co->cfg.gruu_instance_id[0]
        ? co->cfg.gruu_instance_id : NULL;
    if (c->hdr_rw.sip_invite_contact_params[0]) {
        if (gruu_suffix) {
            snprintf(contact, sizeof(contact),
                     "<sip:%s:%d;%s;+sip.instance=\"%s\">",
                     adv_host, adv_port,
                     c->hdr_rw.sip_invite_contact_params, gruu_suffix);
        } else {
            snprintf(contact, sizeof(contact), "<sip:%s:%d;%s>",
                     adv_host, adv_port,
                     c->hdr_rw.sip_invite_contact_params);
        }
    } else {
        if (gruu_suffix) {
            snprintf(contact, sizeof(contact),
                     "<sip:%s:%d;+sip.instance=\"%s\">",
                     adv_host, adv_port, gruu_suffix);
        } else {
            snprintf(contact, sizeof(contact), "<sip:%s:%d>",
                     adv_host, adv_port);
        }
    }
    /* REQ-DP-SIP-001：sip_invite_domain 全局 host 替换（From/To/RURI/Contact 统一处理） */
    if (c->hdr_rw.sip_invite_domain[0]) {
        const char *dom = c->hdr_rw.sip_invite_domain;
        #define APPLY_DOMAIN(field) do { \
            char *_s = (field); \
            char *_at = strchr(_s, '@'); \
            char *_end = _at ? strpbrk(_at, ">;:? \t") : NULL; \
            if (_at && _end && _end > _at) { \
                char _tail[128] = {0}; \
                strncpy(_tail, _end, sizeof(_tail) - 1); \
                *_end = '\0'; \
                snprintf(_at + 1, sizeof(*_s) - (size_t)(_at - _s + 1), "%s%s", dom, _tail); \
            } \
        } while(0)
        APPLY_DOMAIN(b->from);
        APPLY_DOMAIN(b->to);
        APPLY_DOMAIN(b->ruri);
        APPLY_DOMAIN(contact);
        #undef APPLY_DOMAIN
    }
    long cseq;
    if (reinv) {
        cseq = ++b->cseq;
    } else {
        /* 初始 INVITE / 302 follow：用调用方已设的 b->cseq，无值才兜底 1 */
        cseq = b->cseq ? b->cseq : (b->cseq = 1);
    }
    sip_req_spec_t s; memset(&s, 0, sizeof(s));
    s.method = "INVITE"; s.ruri = b->target[0] ? b->target : b->ruri;
    s.from = b->from;
    if (reinv && b->rem_tag[0]) {
        /* re-INVITE：To 头必须带对端 dialog tag */
        static __thread char reinv_to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
        snprintf(reinv_to, sizeof(reinv_to), "%s;tag=%s", b->to, b->rem_tag);
        s.to = reinv_to;
    } else {
        s.to = b->to;
    }
    s.contact = contact; s.callid = b->callid; s.cseq = cseq;
    s.via_host = adv_host; s.via_port = adv_port;
    s.transport = "UDP"; s.branch = branch; s.max_forwards = 70;
    s.user_agent = co->cfg.user_agent;
    /* Session Timer (RFC 4028)：把 A-leg 的 SE/Min-SE/Supported:timer 注入 B-leg INVITE */
    char st_extra[256]; st_extra[0] = '\0';
    /* 函数级 extra 合成 buffer — 关键：不能在 if 块内声明 merged（块结束后
     * 被后续函数调用的栈帧覆盖，导致 s.extra 指向的内存变成垃圾，典型症状：
     * CSeq:2 INVITE 出现两份头块（残留的旧 INVITE 行被当作 extra 追加）。 */
    char _extra_work[B2B_HDR_MAX * 4 * 2 + 2048]; _extra_work[0] = '\0';
    char *extra_ptr = NULL;
    size_t extra_off = 0;
    /* ★ Supported 头：三层优先级链 cfg→profile→dialplan route 动态拼接
     *   replaces=REFER attend/blind 必需；100rel=PRACK/precondition 必需；
     *   precondition/timer/gruu 为 3GPP IMS 可选。
     *   force_* 三态：0=AUTO(继承 profile) / 1=强制开 / 2=强制关。 */
    {
        sip_profile_t *pr = c->prof;
        int do_prack, do_precond, do_timer, do_gruu;
        /* helper 三态解析 */
        #define DP_FORCE(f, prof_val)  \
            (c->force_##f == 1 ? 1 : (c->force_##f == 2 ? 0 : (prof_val)))
        int prof_prack   = pr ? pr->enable_prack        : 0;  /* profile NULL → 默认关（REQ: IMS 三态默认 OFF） */
        int prof_precond = pr ? pr->enable_precondition : 0;
        int prof_timer   = pr ? pr->enable_timer        : 0;
        int prof_gruu    = pr ? pr->enable_gruu         : 0;
        do_prack   = DP_FORCE(prack,        prof_prack);
        do_precond = DP_FORCE(precondition, prof_precond);
        do_timer   = DP_FORCE(timer,        prof_timer);
        do_gruu    = DP_FORCE(gruu,         prof_gruu);
        #undef DP_FORCE
        int n = snprintf(st_extra, sizeof(st_extra),
                         "Supported: replaces%s%s%s%s, path, outbound, sec-agree, answermode, eventlist\r\n",
                         do_prack      ? ", 100rel"       : "",
                         do_precond    ? ", precondition" : "",
                         do_timer      ? ", timer"        : "",
                         do_gruu       ? ", gruu"         : "");
        if (n > 0) { st_extra[n] = '\0'; }
    }
    if (a->session_expires[0] && !co->cfg.sess_timer_enabled) {
        /* P0 透传模式：extract_sess_timer_hdrs 存纯值 "120;refresher=uac"，
         * 需拼 "Session-Expires: xxx\r\n" 头行 */
        char se_line[96];
        int sn = snprintf(se_line, sizeof(se_line), "Session-Expires: %s\r\n", a->session_expires);
        if (sn > 0 && (size_t)sn < sizeof(se_line)) {
            size_t cur = strlen(st_extra);
            if (cur + sn < sizeof(st_extra)) { memcpy(st_extra + cur, se_line, sn); cur += sn; st_extra[cur] = '\0'; }
        }
    } else if (a->session_expires[0] && co->cfg.sess_timer_enabled) {
        /* P1 完整模式：sess_timer_uas_negotiate 存的是完整头行
         * "Session-Expires: N;refresher=xxx\r\n"，直接拼 */
        size_t cur = strlen(st_extra);
        size_t slen = strlen(a->session_expires);
        if (cur + slen < sizeof(st_extra)) { memcpy(st_extra + cur, a->session_expires, slen); cur += slen; st_extra[cur] = '\0'; }
    }
    if (a->min_se[0]) {
        /* extract_sess_timer_hdrs 存 Min-SE 纯值 */
        char mse_line[64];
        int mn = snprintf(mse_line, sizeof(mse_line), "Min-SE: %s\r\n", a->min_se);
        if (mn > 0 && (size_t)mn < sizeof(mse_line)) {
            size_t cur = strlen(st_extra);
            if (cur + mn < sizeof(st_extra)) { memcpy(st_extra + cur, mse_line, mn); cur += mn; st_extra[cur] = '\0'; }
        }
    }
    /* 合入 s.extra（用函数级 _extra_work buffer，避免块内 merged 被后续栈覆盖） */
    if (st_extra[0]) {
        size_t cur = extra_off;
        size_t slen = strlen(st_extra);
        if (cur + slen < sizeof(_extra_work)) {
            memcpy(_extra_work + cur, st_extra, slen); cur += slen; _extra_work[cur] = '\0';
            extra_off = cur;
            extra_ptr = _extra_work;
        }
    }
    s.route = rt[0] ? rt : NULL;
    s.body = body; s.bodylen = bodylen;
    s.content_type = bodylen ? "application/sdp" : NULL;
    /* History-Info 等：与 Route 互斥——302 follow 时 b->dlg 已重置，
     * rt 应为空；extra 里自带完整头行含 CRLF，直接拼在 extra 槽。
     * 若 rt 和 extra 同时存在（罕见），extra 在前（History-Info 应早于 Route）。 */
    if (extra && extra[0]) {
        size_t cur = extra_off;
        size_t elen = strlen(extra);
        if (cur + elen < sizeof(_extra_work)) {
            memcpy(_extra_work + cur, extra, elen); cur += elen; _extra_work[cur] = '\0';
            extra_off = cur;
            extra_ptr = _extra_work;
        }
    }
    /* REQ-TRUNK-HDR-001：trunk 级自定义头 + 变量替换
     * 从 route_table 查当前 trunk 的 custom_headers，替换 ${xxx} 变量后拼入 extra。
     * 拼入位置：最靠上（在 st_extra / extra 参数之前）——自定义头应最早出现。 */
    char custom_resolved[2048]; custom_resolved[0] = '\0';
    if (c->rt_name[0] && b2b_rt(co)) {
        const trunk_t *tr = rt_trunk_get((route_table_t *)b2b_rt(co), c->rt_name);
        if (tr) {
            /* --- 3GPP IMS 自动头注入（REQ-TRUNK-IMS-001） ---
             * ims_enabled=1 且 ims_domain 非空时，自动拼 PAI/PPID/PCPI/PANI/Privacy 头。
             * 用函数级 _ims_work 临时 buffer（最大 512B），再合并进 custom_resolved。
             * 优先级：custom_headers 用户自定义值 > IMS 自动头（若 custom_headers 里
             * 已经有完整头行，IMS 自动头仍会拼但被 action_json 规则级覆盖，不冲突）。 */
            if (tr->ims_enabled && tr->ims_domain[0]) {
                char _ims_work[512]; _ims_work[0] = '\0';
                size_t io = 0;
                const char *auth_user = tr->auth_user[0] ? tr->auth_user
                                        : (c->effective_caller_id_num[0] ? c->effective_caller_id_num : "");
                /* P-Asserted-Identity（必需，运营商计费/路由用） */
                io += (size_t)snprintf(_ims_work + io, sizeof(_ims_work) - io,
                    "P-Asserted-Identity: <sip:%s@%s>\r\n",
                    auth_user, tr->ims_domain);
                /* P-Preferred-Identity（同 PAI，展示主叫） */
                io += (size_t)snprintf(_ims_work + io, sizeof(_ims_work) - io,
                    "P-Preferred-Identity: <sip:%s@%s>\r\n",
                    auth_user, tr->ims_domain);
                /* P-Called-Party-ID（被叫 E.164，IMS 必需；用拨号规则改写后的 dst_number，
                 * 再 fallback B 腿 To 头 user part —— 对齐 custom_headers ${callee} 解析逻辑） */
                {
                    const char *pcpi = c->dst_number[0] ? c->dst_number : "";
                    if (!pcpi[0]) {
                        static __thread char _b_to_user[B2B_HDR_MAX]; _b_to_user[0] = '\0';
                        const char *lt = strchr(b->to, '<');
                        const char *s = lt ? lt + 1 : b->to;
                        const char *at = strchr(s, '@');
                        if (at) { size_t ul = (size_t)(at - s); if (ul >= sizeof(_b_to_user)) ul = sizeof(_b_to_user) - 1;
                                  memcpy(_b_to_user, s, ul); _b_to_user[ul] = '\0'; }
                        pcpi = _b_to_user;
                    }
                    io += (size_t)snprintf(_ims_work + io, sizeof(_ims_work) - io,
                        "P-Called-Party-ID: <sip:%s@%s>\r\n", pcpi, tr->ims_domain);
                }
                /* P-Access-Network-Info（可选，默认 E-UTRAN-TDD，用户可配 ims_pani_value 覆盖） */
                if (tr->ims_auto_pani) {
                    const char *pani = tr->ims_pani_value[0] ? tr->ims_pani_value : "E-UTRAN-TDD";
                    io += (size_t)snprintf(_ims_work + io, sizeof(_ims_work) - io,
                        "P-Access-Network-Info: %s\r\n", pani);
                }
                /* Privacy: id（可选，CLIR 自动加） */
                if (tr->ims_auto_privacy) {
                    io += (size_t)snprintf(_ims_work + io, sizeof(_ims_work) - io,
                        "Privacy: id\r\n");
                }
                /* P-Charging-Function-Addresses + P-Charging-Vector（RFC 7315，可选）
                 * 运营商在线计费系统必需——CCF/ECF 地址 + 计费向量标识本次会话。 */
                if (tr->ims_auto_charging) {
                    if (tr->ims_pcfa[0]) {
                        io += (size_t)snprintf(_ims_work + io, sizeof(_ims_work) - io,
                            "P-Charging-Function-Addresses: %s\r\n", tr->ims_pcfa);
                    }
                    if (tr->ims_pcv[0]) {
                        io += (size_t)snprintf(_ims_work + io, sizeof(_ims_work) - io,
                            "P-Charging-Vector: %s\r\n", tr->ims_pcv);
                    }
                }
                llog(LOG_INFO, "b2b", "send_invite_b trunk '%s' IMS auto "
                     "injected %zu bytes (domain=%s)", c->rt_name, io, tr->ims_domain);
                /* IMS 头**最前**——运营商需要 PAI 在 extra 区首位 */
                if (_ims_work[0] && io + strlen(custom_resolved) < sizeof(custom_resolved)) {
                    size_t ims_len = strlen(_ims_work);
                    memmove(custom_resolved + ims_len, custom_resolved, strlen(custom_resolved));
                    memcpy(custom_resolved, _ims_work, ims_len);
                }
            }
            /* custom_headers 用户自定义头（变量替换）——拼在 IMS 自动头之后 */
            if (tr->custom_headers[0]) {
                size_t before = strlen(custom_resolved);
                substitute_custom_vars(tr->custom_headers, custom_resolved + before,
                                       sizeof(custom_resolved) - before, c, a, b);
                if (custom_resolved[before]) {
                    llog(LOG_INFO, "b2b", "send_invite_b trunk '%s' custom_headers "
                         "injected", c->rt_name);
                }
            }
        }
    }
    /* 把 custom_resolved prepend 到 _extra_work 最前面（自定义头应早于 st_extra 和 extra 参数） */
    if (custom_resolved[0] && extra_ptr) {
        size_t clen = strlen(custom_resolved);
        if (extra_off + clen < sizeof(_extra_work)) {
            /* 把已有内容往后挪 clen 字节，腾出位置 */
            memmove(_extra_work + clen, _extra_work, extra_off);
            memcpy(_extra_work, custom_resolved, clen);
            extra_off += clen;
            _extra_work[extra_off] = '\0';
        }
    } else if (custom_resolved[0]) {
        /* 没有 st_extra 也没有 extra → 直接放 custom_resolved */
        size_t clen = strlen(custom_resolved);
        if (clen < sizeof(_extra_work)) {
            memcpy(_extra_work, custom_resolved, clen);
            extra_off = clen;
            _extra_work[extra_off] = '\0';
            extra_ptr = _extra_work;
        }
    }
    /* 最终：所有 extra 合成完毕，s.extra 指向函数级 _extra_work（一直有效到 sip_build_request 调用） */
    s.extra = extra_ptr;
    /* --- P0 关键修复：out buffer 必须清零 ---
     * 栈上分配的 out[SIP_MAX_MSG*2] 不会自动清零。sip_build_request 用 sipb_catf
     * 从 len=0 开始构建，正确返回 n（构建长度）。但如果任何后续发送环节误用
     * strlen(out) 而非 len，就会越过 n 读到 CSeq:1 残留在栈上的旧数据（整个旧报文），
     * 造成典型的"头重复 + SDP 片段"损坏包（用户实测 CSeq:2 从 1200B 膨胀到 1781B）。
     * 清零 + NUL 终止 → 即使 strlen 误用也只会返回 n。 */
    char out[SIP_MAX_MSG * 2];
    memset(out, 0, sizeof(out));
    int n = sip_build_request(out, sizeof(out), &s);
    if (n >= 0 && (size_t)n < sizeof(out)) out[n] = '\0';
    if (n < 0) {
        llog(LOG_WARN, "b2b", "build B INVITE failed");
        return NULL;
    }
    /* 诊断：完整 SIP 报文 dump（407 challenge 重发 / 初始 INVITE 各打一次）
     * 用于定位"头重复 + SDP 截断"等构建层损坏问题。 */
    {
        /* 前 512B 头区 + SDP 起始，足够看到头重复和 m= 行状态 */
        size_t dump_len = (size_t)n < 1024 ? (size_t)n : 1024;
        llog(LOG_INFO, "b2b", "★send_invite_b built %dB reinv=%d cseq=%ld "
             "branch=%s extra_len=%zu bodylen=%zu body_head=%.64s",
             n, reinv, cseq, branch,
             extra ? strlen(extra) : 0,
             bodylen, body && bodylen ? body : "(null)");
        llog(LOG_INFO, "b2b", "★send_invite_b MSG_DUMP_BEGIN total=%zu show=%zu",
             (size_t)n, dump_len);
        /* 分段打（每 256B 一行），避免单行过长被日志系统截断 */
        for (size_t off = 0; off < dump_len; off += 256) {
            size_t chunk = dump_len - off;
            if (chunk > 256) chunk = 256;
            llog(LOG_INFO, "b2b", "★MSG[%zu..%zu]: %.*s",
                 off, off + chunk, (int)chunk, out + off);
        }
        llog(LOG_INFO, "b2b", "★send_invite_b MSG_DUMP_END");
    }
    snprintf(b->last_branch, sizeof(b->last_branch), "%s", branch);
    /* 设本地发送端口 = adv_port（与 Via/Contact 宣告一致）
     * —— WS 入向场景 g_rx_local_port 为 0，必须显式设 */
    g_rx_local_port = adv_port;
    return txn_client_new(co->cfg.txn, TXN_ICT, branch, strlen(branch),
                          SIP_M_INVITE, &b->dst, b->reliable,
                          out, (size_t)n, cli_tu, c, now_ms());
}

/* ------------------------------------------------------------------ */
/* 前向声明                                                             */
/* ------------------------------------------------------------------ */
void b2b_server_tu(sip_txn_t *t, const sip_msg_t *m, txn_event_t ev,
                   void *ud);
static void cli_tu(sip_txn_t *t, const sip_msg_t *m, txn_event_t ev,
                   void *ud);
static void finish_with_a_response(b2b_call_t *c, int status,
                                   const char *body, size_t bodylen);
static void send_cancel_req(b2b_call_t *c, int leg);
static void send_bye_leg(b2b_call_t *c, int leg);
static int build_dlg_req(b2b_core_t *co, b2b_leg_t *l,
                         sip_method_t method, long cseq, const char *ruri,
                         const char *to_with_tag, const char *branch,
                         const char *extra, const char *body,
                         size_t bodylen, char *out, size_t outsz);
static sip_txn_t *send_invite_b(b2b_call_t *c, int leg_idx,
                                const char *body, size_t bodylen, int reinv,
                                const char *extra /* History-Info 等额外头块，自带 CRLF */);

/* ------------------------------------------------------------------ */
/* REQ-SIP-DIV-001：Diversion 头（RFC 5806）构造                          */
/* ------------------------------------------------------------------ */
/* 输出一行完整的 Diversion 头（含 CRLF）。
 * URI 为原始被叫（<sip:...> 尖括号格式）；reason 可为 NULL（默认 "unknown"）；
 * counter 为当前重定向计数（0 时不输出）。
 * 返回写出长度（不含 NUL），空间不足返回 -1。 */
static int b2b_build_diversion_header(char *out, size_t outsz,
                                      const char *original_uri,
                                      const char *reason, int counter)
{
    if (counter <= 0 || !original_uri || !original_uri[0])
        return 0; /* 无重定向，Diversion 无意义 */
    if (!reason)
        reason = "unknown";
    /* RFC 5806: Diversion: <sip:...>;reason=<reason>;counter=<n>\r\n
     * 最大估算：64(uri) + 32(reason) + "Diversion: \r\n;reason=;counter="(28) + 4(counter) ≈ 130B */
    int n = snprintf(out, outsz,
                     "Diversion: <%s>;reason=%s;counter=%d\r\n",
                     original_uri, reason, counter);
    if (n < 0 || (size_t)n >= outsz)
        return -1;
    return n;
}

/* ------------------------------------------------------------------ */
/* A 腿应答                                                             */
/* ------------------------------------------------------------------ */
static void respond_a(b2b_call_t *c, int status, const char *body,
                      size_t bodylen)
{
    b2b_core_t *co = c->core;
    b2b_leg_t *a = &c->leg[0];
    sip_txn_t *t = a->srv_txn ? a->srv_txn : a->inv_txn;
    if (!t)
        return;
    /* REQ-NAT-001：respond_a 里构造的 Contact 也走 external_sip_ip
     * REQ-NAT-002：优先 per-call profile，fallback cfg */
    const char *prof_ext = (c->prof && c->prof->external_sip_ip[0])
                           ? c->prof->external_sip_ip : co->cfg.external_sip_ip;
    const char *adv_host = (prof_ext && prof_ext[0]) ? prof_ext : co->cfg.local_host;
    /* extra 头块：Content-Type(28) + History-Info(≤1024) + Diversion(≤130) → 1300 够装 */
    char extra[1300];
    size_t used = 0;
    extra[0] = '\0';
    if (bodylen) {
        int n = snprintf(extra, sizeof(extra), "Content-Type: application/sdp\r\n");
        if (n > 0 && (size_t)n < sizeof(extra)) used = (size_t)n;
    }
    /* REQ-SIP-DIV-001：302 follow 后累积的 History-Info 链透传给 A 腿应答 */
    if (c->history_info[0] && !c->privacy_history) {
        size_t hi_len = strlen(c->history_info);
        if (used + hi_len < sizeof(extra)) {
            memcpy(extra + used, c->history_info, hi_len);
            used += hi_len;
            extra[used] = '\0';
        }
    }
    /* REQ-SIP-DIV-001：Diversion 头（RFC 5806）——有重定向且非 privacy=history 时透传 */
    if (c->redirect_count > 0 && c->orig_to[0] && !c->privacy_history &&
        used + 256 < sizeof(extra)) {
        int dn = b2b_build_diversion_header(extra + used,
                                            sizeof(extra) - used,
                                            c->orig_to, NULL,
                                            c->redirect_count);
        if (dn > 0) {
            used += (size_t)dn;
            extra[used] = '\0';
        }
    }
    /* Session Timer (RFC 4028)：注入 Alice 200 OK */
    b2b_leg_t *b_leg = &c->leg[1];
    if (a->timer_supported && used + 256 < sizeof(extra)) {
        /* Supported:timer 合并 */
        const char *sup = "Supported: timer,precondition,gruu,path,outbound,sec-agree\r\n";
        int sn = snprintf(extra + used, sizeof(extra) - used, "%s", sup);
        if (sn > 0) { used += (size_t)sn; }
        /* Session-Expires */
        const char *se_val = NULL;
        if (co->cfg.sess_timer_enabled && a->session_expires[0]) {
            /* P1 完整模式：用协商后的值（格式已含完整头行） */
            se_val = a->session_expires;
            int sen = snprintf(extra + used, sizeof(extra) - used, "%s", se_val);
            if (sen > 0) used += (size_t)sen;
        } else if (!co->cfg.sess_timer_enabled && b_leg->session_expires[0]) {
            /* P0 透传模式：从 Bob 200 OK 存的纯值拼头行 */
            int sen = snprintf(extra + used, sizeof(extra) - used,
                               "Session-Expires: %s\r\n", b_leg->session_expires);
            if (sen > 0) used += (size_t)sen;
        }
        /* Min-SE：RFC 4028 §5.3 若 UAS 提高了 Min-SE 必须回显；
         * P1 模式用 B2BUA cfg 的 min_se_s（UAS 真实下限）；
         * P0 透传模式用 Alice 原始提议的 Min-SE 原样回显 */
        {
            int p1 = co->cfg.sess_timer_enabled;
            int mse = p1 ? co->cfg.sess_timer_cfg.min_se_s : atoi(a->min_se);
            if (mse > 0) {
                int mn = snprintf(extra + used, sizeof(extra) - used,
                                  "Min-SE: %d\r\n", mse);
                if (mn > 0) used += (size_t)mn;
            }
        }
        extra[used] = '\0';
    }
    /* ---- 100rel 可靠临时响应：注入 RSeq + Require:100rel ---- */
    if (status < 200 && a->local_rseq != 0 && used + 128 < sizeof(extra)) {
        int rn = snprintf(extra + used, sizeof(extra) - used,
                          "RSeq: %u\r\nRequire: 100rel\r\n", a->local_rseq);
        if (rn > 0) used += (size_t)rn;
        extra[used] = '\0';
    }
    size_t rlen = 0;
    const char *raw = txn_first_request(t, &rlen);
    if (!raw)
        return;
    sip_msg_t req;
    if (sip_parse(&req, raw, rlen) != NSBC_OK)
        return;
    /* REQ-NAT-003：dialog 内请求也走 per-call profile 的 sip_udp_port */
    char contact[B2B_HDR_MAX];
    uint16_t dlg_port = (c->prof && c->prof->sip_udp_port)
                        ? c->prof->sip_udp_port : (uint16_t)co->cfg.local_udp_port;
    snprintf(contact, sizeof(contact), "<sip:%s:%d>",
             adv_host, dlg_port);
    /* 初始 INVITE：to_tag 为本侧 tag；re-INVITE：已带 tag 传 NULL 不追加 */
    const char *tag = a->srv_txn ? NULL : a->loc_tag;
    char out[SIP_MAX_MSG];
    int n = sip_build_response(out, sizeof(out), &req, status, NULL,
                               tag, contact, extra[0] ? extra : NULL,
                               body, bodylen);
    if (n < 0)
        return;
    txn_send_response(t, out, (size_t)n, now_ms());
}

/* 媒体决策一次 */
static void maybe_start_media(b2b_call_t *c)
{
    b2b_core_t *co = c->core;
    if (c->media_done || !co->cfg.media)
        return;
    if (!c->leg[0].answer_len && !c->leg[0].offer_len)
        return;
    /* A 侧 offer 已随 INVITE 收到，B 侧 answer 在 2xx 中 */
    if (!c->leg[1].answer_len)
        return;
    c->media_done = 1;
    co->cfg.media(c, c->leg[0].offer, c->leg[0].offer_len,
                  c->leg[1].answer, c->leg[1].answer_len,
                  co->cfg.media_ud);
}

/* B 2xx 后向 B 逐跳发 ACK（含 2xx 重传吸收） */
/* 2xx 终响应 ACK：用新 Via branch（RFC3261 §17.1.1——INVITE 的 2xx ACK 必须
 * 新 branch，同事务的非 2xx ACK 用 send_ack_non2xx 同 branch）。
 * leg=0 或 1：B-腿初始/hold re-INVITE、A-腿 hold re-INVITE 都用。 */
static void send_ack_leg(b2b_call_t *c, int leg)
{
    b2b_core_t *co = c->core;
    b2b_leg_t *l = &c->leg[leg];
    /* ACK To tag 必须是 peer_tag（= 被确认的 200 OK 的 To 头 tag = 对端 UAS 的 local tag），
     * 绝对不能用 rem_tag（= From 头 tag = dialog remote tag）！
     * Bug 历史：用 rem_tag 导致 ACK To tag 错误 → SIP.js 不认 → Timer H 触发 200 OK 重传死循环。
     * peer_tag 为空（理论上不应发生，200 OK 必有 To tag）时 fallback 到 rem_tag 兜底。 */
    const char *ack_tag = l->peer_tag[0] ? l->peer_tag
                       : (l->rem_tag[0] ? l->rem_tag : "");
    char to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
    snprintf(to, sizeof(to), "%s;tag=%s", l->to, ack_tag);
    char branch[40];
    new_branch(branch, sizeof(branch));
    char out[SIP_MAX_MSG];
    int n = build_dlg_req(co, l, SIP_M_ACK, l->cseq,
                          l->target[0] ? l->target : l->ruri, to, branch,
                          NULL, NULL, 0, out, sizeof(out));
    if (n > 0) {
        /* 设本地发送端口：ACK 走对话已建立的路径，用 out_prof */
        struct sip_profile *op = c->out_prof ? c->out_prof : c->prof;
        g_rx_local_port = (op && op->sip_udp_port)
                          ? op->sip_udp_port
                          : (uint16_t)co->cfg.local_udp_port;
        tr_send_leg(l, out, (size_t)n, co);
    }
}

/* 非 2xx 最终响应的 ACK：同事务同 Via branch（RFC3261 §17.1.1.2）。
 * 本仓事务层约定该 ACK 由 TU 构造，响应重传时重复调用即可。 */
static void send_ack_non2xx(b2b_call_t *c, int leg)
{
    b2b_core_t *co = c->core;
    b2b_leg_t *l = &c->leg[leg];
    if (!l->last_branch[0])
        return;
    /* 同 send_ack_leg：ACK To tag 必须是 peer_tag（= 终响应 To 头 tag）。 */
    const char *ack_tag = l->peer_tag[0] ? l->peer_tag
                       : (l->rem_tag[0] ? l->rem_tag : "");
    char to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
    snprintf(to, sizeof(to), "%s;tag=%s", l->to, ack_tag);
    char out[SIP_MAX_MSG];
    int n = build_dlg_req(co, l, SIP_M_ACK, l->cseq,
                          l->target[0] ? l->target : l->ruri, to,
                          l->last_branch, NULL, NULL, 0,
                          out, sizeof(out));
    if (n > 0) {
        /* 设本地发送端口：ACK 走对话已建立的路径，用 out_prof */
        struct sip_profile *op = c->out_prof ? c->out_prof : c->prof;
        g_rx_local_port = (op && op->sip_udp_port)
                          ? op->sip_udp_port
                          : (uint16_t)co->cfg.local_udp_port;
        tr_send_leg(l, out, (size_t)n, co);
    }
}

/* ------------------------------------------------------------------ */
/* Ring NoAnswer 定时器（P0 PBX 基础）                                  */
/*                                                                    */
/* 设计：B 腿返回 180/183 后 arm；任何终态触发 disarm。              */
/*   - 180/183 ringing → arm 60s → 触发回调 → 向 A 腿发 480           */
/*   - 2xx 应答 / 4xx/5xx/6xx 任何响应 / A 腿 CANCEL / call_destroy → disarm */
/*   - disarm 在 call_destroy 里 catch-all，保证零悬挂回调             */
/* ------------------------------------------------------------------ */
static void noanswer_cb(uint64_t now_ms, void *arg)
{
    (void)now_ms;
    b2b_call_t *c = arg;
    if (!c)
        return;
    b2b_core_t *co = c->core;
    call_lock(c);
    if (c->dead || c->a_final) {
        call_unlock(c);
        return;
    }
    /* 仅在仍处于 EARLY 时才算超时；CONFIRMED/TERMINATING 是竞态（极罕见） */
    if (c->leg[0].state == LEG_CONFIRMED ||
        c->leg[1].state == LEG_CONFIRMED) {
        call_unlock(c);
        return;
    }
    llog(LOG_INFO, "b2b", "ring_noanswer: call timed out, send 480 + CANCEL B-leg");
    /* ★ 同时终止两腿：
     *   A 腿（leg[0]，主叫/WebRTC）：我们是 server → respond_a(480)
     *   B 腿（leg[1]，被叫/WebRTC）：我们是 client → send_cancel_req 发 CANCEL
     * 顺序关键：send_cancel_req 检查 state==LEG_CALLING/EARLY 才发，
     * 必须在设 LEG_TERMINATED 之前调。 */
    respond_a(c, 480, NULL, 0);
    c->a_final = 480;
    c->leg[0].state = LEG_TERMINATED;
    send_cancel_req(c, 1);        // ★ 新增：对 B 腿 INVITE 发 CANCEL
    c->leg[1].state = LEG_TERMINATED;
    add_atomic(&co->failed, 1);
    call_unlock(c);
    call_destroy(c);
}

/* REQ-SIP-DIV-001 P0：从 302/380 响应的 Contact 头提取新目的地 URI。
 * 从原始 Contact 值中找尖括号内（有）或分号前（无）的 URI 原文，
 * 拷贝到 out 缓冲并 NUL 终止。返回 NSBC_OK / NOENT / INVAL / NOSPC。 */
static int b2b_extract_contact_uri(const sip_msg_t *m, char *out, size_t outsz)
{
    sip_hdr_t *h = sip_hdr_get((sip_msg_t *)m, SIP_H_CONTACT);
    if (!h) return NSBC_NOENT;
    const char *p = h->value.p;
    size_t n = h->value.len;
    if (!p || n == 0) return NSBC_INVAL;
    if (n == 1 && *p == '*') return NSBC_INVAL; /* Contact: * */

    /* 找尖括号内 URI */
    const char *lt = memchr(p, '<', n);
    const char *gt;
    const char *uri_b, *uri_e;
    if (lt) {
        uri_b = lt + 1;
        gt = memchr(uri_b, '>', p + n - uri_b);
        if (!gt) return NSBC_ERR;
        uri_e = gt;
    } else {
        uri_b = p;
        const char *semi = memchr(p, ';', n);
        uri_e = semi ? semi : p + n;
    }
    size_t urilen = (size_t)(uri_e - uri_b);
    if (urilen == 0) return NSBC_INVAL;
    if (urilen >= outsz) return NSBC_NOSPC;
    memcpy(out, uri_b, urilen);
    out[urilen] = '\0';
    return NSBC_OK;
}

/* REQ-SIP-DIV-001 P0：累积 History-Info 链（RFC 4244/7044）。
 * 每次重定向在头部追加新条目（index=1 最新），rc=触发码（如 302）。
 * 输出格式：History-Info: <sip:target@host>;index=N;rc=302,\r\n...\r\n */
static void b2b_append_history_info(b2b_call_t *c, const char *target_uri,
                                    int rc)
{
    if (!target_uri || !target_uri[0]) return;
    char entry[512];
    /* index 从当前 redirect_count+1 开始（这次成功的会在后面 ++） */
    int idx = c->redirect_count + 1;
    int n = snprintf(entry, sizeof(entry),
                     "<%s>;index=%d;rc=%d", target_uri, idx, rc);
    if (n < 0 || (size_t)n >= sizeof(entry)) return;
    /* 如果 history_info 是空的，建立头；否则在前条目前插入新条目（index=1 永远是最新） */
    if (c->history_info[0] == '\0') {
        int nb = snprintf(c->history_info, sizeof(c->history_info),
                          "History-Info: %s\r\n", entry);
        if (nb < 0 || (size_t)nb >= sizeof(c->history_info)) {
            c->history_info[0] = '\0';
        }
    } else {
        /* 在 History-Info: 后、已有条目前插入新条目，加逗号 */
        char existing[1024];
        snprintf(existing, sizeof(existing), "%s", c->history_info);
        /* 找到 "History-Info: " 后面的内容起始位置 */
        const char *p = strchr(existing, ':');
        if (!p) return; /* 防御 */
        p += 2; /* 跳过 ": " */
        /* history_info 当前行尾有 \r\n，把它替换成新条目+逗号+原有内容+\r\n */
        char new_line[1536];
        int nn = snprintf(new_line, sizeof(new_line),
                          "History-Info: %s,%s", entry, p);
        /* guard 检查目标缓冲（history_info[1024]），不是中间 new_line[1536]：
         * 防止 new_line 通过 1536 检查但 snprintf 到 1024 时 silent truncation
         * 产生缺少 CRLF 的 malformed header */
        if (nn > 0 && (size_t)nn < sizeof(c->history_info)) {
            snprintf(c->history_info, sizeof(c->history_info), "%s", new_line);
        }
    }
}

static void arm_noanswer_timer(b2b_call_t *c)
{
    b2b_core_t *co = c->core;
    if (!co->cfg.tw)
        return;
    if (c->dead || c->a_final)
        return;
    /* REQ-ORIGINATE-001：per-call override 优先（>0），否则走全局。
     * 注意 memset 初始化后 noanswer_ms_override=0，入站 INVITE 路径
     * 自然走全局 ring_noanswer_ms，originate 显式设 >0 才覆盖。 */
    uint32_t ms = (c->noanswer_ms_override > 0)
                  ? (uint32_t)c->noanswer_ms_override
                  : co->cfg.ring_noanswer_ms;
    if (ms == 0)
        return;
    tw_del(co->cfg.tw, &c->noanswer_node); /* 续约先摘再挂（幂等） */
    tw_add(co->cfg.tw, &c->noanswer_node,
           now_ms() + ms,
           noanswer_cb, c);
}

static void disarm_noanswer_timer(b2b_call_t *c)
{
    b2b_core_t *co = c->core;
    if (!co->cfg.tw)
        return;
    tw_del(co->cfg.tw, &c->noanswer_node);
}

/* ==================================================================== */
/* REQ-ORIGINATE-003 待补：bridge 顺序外呼 A 腿（leg[0]，UAC）遇到的            */
/*   - 401/407 challenge 鉴权重试                                        */
/*   - 302/380 redirect 跟随                                             */
/* 语义与 B 腿（leg[1]）inline 逻辑一致，参数化 leg 索引以复用。            */
/* 前置：调用方持有 call->lock（在 cli_tu 内调用）。                        */
/* 返回：1 = 已重发 INVITE（调用方直接 return）；0 = 未处理；-1 = 需 486。   */

static int uac_invite_handle_challenge(b2b_call_t *c, int lg,
                                       const sip_msg_t *m, int st)
{
    b2b_core_t *co = c->core;
    b2b_leg_t *b = &c->leg[lg];
    if (st != 401 && st != 407) return 0;
    if (b->auth_attempts >= 3) return 0;
    if (!c->rt_name[0] || !b2b_rt(co)) return 0;

    const trunk_t *tr = rt_trunk_get((route_table_t *)b2b_rt(co), c->rt_name);
    if (!tr || !tr->password[0]) {
        llog(LOG_WARN, "b2b", "bridge A leg 401/407 but trunk %s unusable",
             c->rt_name);
        return 0;
    }
    sip_hdr_t *ch_hdr = sip_hdr_get(m, st == 407 ? SIP_H_PROXY_AUTHENTICATE
                                                : SIP_H_WWW_AUTHENTICATE);
    if (!ch_hdr || !ch_hdr->value.p) return 0;

    digest_challenge_t ch; memset(&ch, 0, sizeof(ch));
    if (digest_parse_challenge(ch_hdr->value.p, &ch) != NSBC_OK) return 0;

    b->auth_attempts++;
    if (st == 407) {
        snprintf(b->proxy_ch_realm, sizeof(b->proxy_ch_realm), "%s",
                 ch.realm[0] ? ch.realm : tr->realm);
        snprintf(b->proxy_ch_nonce, sizeof(b->proxy_ch_nonce), "%s", ch.nonce);
        snprintf(b->proxy_ch_opaque, sizeof(b->proxy_ch_opaque), "%s", ch.opaque);
        snprintf(b->proxy_ch_qop, sizeof(b->proxy_ch_qop), "%s", ch.qop);
        if (ch.stale) b->nc_counter = 0;
        b->auth_flags |= 0x01;
    } else {
        snprintf(b->www_ch_realm, sizeof(b->www_ch_realm), "%s",
                 ch.realm[0] ? ch.realm : tr->realm);
        snprintf(b->www_ch_nonce, sizeof(b->www_ch_nonce), "%s", ch.nonce);
        snprintf(b->www_ch_opaque, sizeof(b->www_ch_opaque), "%s", ch.opaque);
        snprintf(b->www_ch_qop, sizeof(b->www_ch_qop), "%s", ch.qop);
        if (ch.stale) b->nc_counter = 0;
        b->auth_flags |= 0x02;
    }

    char ha1[33];
    const char *ha1_realm = NULL;
    if (b->auth_flags & 0x01) ha1_realm = b->proxy_ch_realm[0] ? b->proxy_ch_realm : tr->realm;
    if (b->auth_flags & 0x02) ha1_realm = b->www_ch_realm[0]  ? b->www_ch_realm  : tr->realm;
    if (!ha1_realm)           ha1_realm = tr->realm[0] ? tr->realm : tr->host;
    digest_make_ha1(tr->auth_user, ha1_realm, tr->password, ha1);

    if (b->auth_flags & 0x01) {
        uint32_t nc; char nc_buf[16], cnonce[16];
        b->nc_counter++; nc = b->nc_counter;
        snprintf(nc_buf, sizeof(nc_buf), "%08x", (unsigned)nc);
        nsbc_rand_token(cnonce, sizeof(cnonce), 8);
        digest_client_build_auth(tr->auth_user,
            b->proxy_ch_realm[0] ? b->proxy_ch_realm : tr->realm,
            b->proxy_ch_nonce,
            b->proxy_ch_opaque[0] ? b->proxy_ch_opaque : NULL,
            b->proxy_ch_qop[0] ? b->proxy_ch_qop : "auth",
            nc_buf, cnonce, "INVITE", b->ruri, ha1, 1,
            b->proxy_auth_hdr, sizeof(b->proxy_auth_hdr));
    }
    if (b->auth_flags & 0x02) {
        uint32_t nc; char nc_buf[16], cnonce[16];
        b->nc_counter++; nc = b->nc_counter;
        snprintf(nc_buf, sizeof(nc_buf), "%08x", (unsigned)nc);
        nsbc_rand_token(cnonce, sizeof(cnonce), 8);
        digest_client_build_auth(tr->auth_user,
            b->www_ch_realm[0] ? b->www_ch_realm : tr->realm,
            b->www_ch_nonce,
            b->www_ch_opaque[0] ? b->www_ch_opaque : NULL,
            b->www_ch_qop[0] ? b->www_ch_qop : "auth",
            nc_buf, cnonce, "INVITE", b->ruri, ha1, 1,
            b->www_auth_hdr, sizeof(b->www_auth_hdr));
    }

    char auth_extra[512]; size_t ax = 0;
    if (b->auth_flags & 0x01)
        ax += (size_t)snprintf(auth_extra + ax, sizeof(auth_extra) - ax,
                               "Proxy-Authorization: %s\r\n", b->proxy_auth_hdr);
    if (b->auth_flags & 0x02)
        ax += (size_t)snprintf(auth_extra + ax, sizeof(auth_extra) - ax,
                               "Authorization: %s\r\n", b->www_auth_hdr);

    if (b->dlg) { dlg_close(co->cfg.dlg, b->dlg); b->dlg = NULL; }
    b->cseq++;
    b->inv_txn = send_invite_b(c, lg, b->offer, b->offer_len, 0,
                               auth_extra[0] ? auth_extra : NULL);
    if (b->inv_txn) {
        llog(LOG_INFO, "b2b",
             "bridge A leg INVITE re-sent after %d (attempts=%u) callid=%s",
             st, b->auth_attempts, b->callid);
        return 1;
    }
    b->cseq--;
    b->auth_attempts--;
    return 0;
}

static int uac_invite_follow_redirect(b2b_call_t *c, int lg,
                                      const sip_msg_t *m, int st)
{
    b2b_core_t *co = c->core;
    b2b_leg_t *b = &c->leg[lg];
    if (st != 302 && st != 380) return 0;
    if (!co->cfg.auto_redirect_302) return 0;
    if (c->redirect_count >= co->cfg.max_redirects) return 0;

    char new_target[256];
    if (b2b_extract_contact_uri(m, new_target, sizeof(new_target)) != NSBC_OK)
        return 0;

    b2b_append_history_info(c, new_target, st);
    c->redirect_count++;

    if (b->dlg) { dlg_close(co->cfg.dlg, b->dlg); b->dlg = NULL; }
    b->nroutes = 0;
    b->target[0] = '\0';
    b->ruri[0] = '\0';
    b->rem_tag[0] = '\0';
    b->peer_tag[0] = '\0';
    if (c->key_held) {
        perkey_dec(co, c->b_key);
        c->key_held = 0;
        c->b_key[0] = '\0';
    }
    if (co->cfg.resolve) {
        int rr = co->cfg.resolve(c, new_target, strlen(new_target), &b->dst,
                                 b->ruri, sizeof(b->ruri), b->to, sizeof(b->to),
                                 co->cfg.resolve_ud);
        if (rr != NSBC_OK) {
            snprintf(b->target, sizeof(b->target), "%s", new_target);
            snprintf(b->ruri, sizeof(b->ruri), "%s", new_target);
            snprintf(b->to, sizeof(b->to), "%s", new_target);
        } else if (c->b_key[0] && !perkey_try_inc(co, c->b_key)) {
            llog(LOG_WARN, "b2b", "bridge A 302 perkey_limit for %s, 486",
                 c->b_key);
            return -1;
        } else if (c->b_key[0]) {
            c->key_held = 1;
        }
    } else {
        snprintf(b->target, sizeof(b->target), "%s", new_target);
        snprintf(b->ruri, sizeof(b->ruri), "%s", new_target);
        snprintf(b->to, sizeof(b->to), "%s", new_target);
    }
    b->cseq++;
    b->state = LEG_CALLING;
    b->dlg = dlg_uac_new(co->cfg.dlg, b->callid, strlen(b->callid), b->loc_tag,
                         strlen(b->loc_tag), b->target, b->cseq, SIP_M_INVITE);
    if (b->dlg) b->dlg->ud = c;

    b->inv_txn = send_invite_b(c, lg, b->offer, b->offer_len, 0, NULL);
    if (!b->inv_txn) {
        c->redirect_count--;
        llog(LOG_WARN, "b2b", "bridge A 302 send failed target=%s", new_target);
        return 0;
    }
    llog(LOG_INFO, "b2b", "bridge A redirected count=%d target=%s",
         c->redirect_count, new_target);
    return 1;
}

/* ------------------------------------------------------------------ */
/* 客户端事务事件（B 腿 / 转发请求的响应）                               */
/* ------------------------------------------------------------------ */
static void cli_tu(sip_txn_t *t, const sip_msg_t *m, txn_event_t ev,
                   void *ud)
{
    b2b_call_t *c = ud;
    if (!c)
        return;
    b2b_core_t *co = c->core;
    call_lock(c);
    if (c->dead) {
        call_unlock(c);
        return;
    }

    llog(LOG_INFO, "b2b",
         "cli_tu: ev=%d cid=%s s0=%d s1=%d",
         ev, c->leg[0].callid,
         c->leg[0].state, c->leg[1].state);

    /* Session Timer: refresh_cb 发出的 re-INVITE 收到 2xx → reset + arm 下次
     * 此块无 t==inv_txn 守卫，因为 A-leg/B-leg re-INVITE 都用独立 txn。
     * 初始 INVITE 的 2xx 不会误触发（refresh_pending 初始化 = 0）。
     * 关键：用 tw_add 不用 tw_mod — refresh_node 触发后已被 timer_wheel 移除！ */
    if (ev == TXN_EV_FINAL && m && m->status >= 200) {
        for (int li = 0; li < 2; li++) {
            b2b_leg_t *lg = &c->leg[li];
            if (lg->refresh_pending && lg->st.active) {
                uint64_t nm = now_ms();
                sess_timer_reset(&lg->st, nm);
                int64_t half_ms = (int64_t)lg->st.interval_s * 500 + 200;
                tw_add(co->cfg.tw, &lg->refresh_node, (uint64_t)((int64_t)nm + half_ms),
                       sess_timer_refresh_cb, c);
                tw_mod(co->cfg.tw, &lg->expire_node,
                       nm + (uint64_t)lg->st.interval_s * 1000u + SESS_TIMER_GRACE_MS);
                lg->refresh_pending = 0;
            }
        }
    }

    /* bridge 外呼 A 腿响应（leg[0] 作为 UAC 发 INVITE 的场景）——
     * 简化处理：只做 state 更新 + dlg 注册 + answer 存储，跳过 respond_a
     * （bridge 模式没有真实入向 A 腿，只有顺序外呼 B 腿）。 */
    if (t == c->leg[0].inv_txn) {
        b2b_leg_t *a = &c->leg[0];
        if (ev == TXN_EV_PROVISIONAL && m && m->status < 200) {
            tok_from_tag(m, a->rem_tag, DLG_TAG_MAX);
            tok_to_tag(m, a->peer_tag, DLG_TAG_MAX);
            dlg_uac_on_response(co->cfg.dlg, m);
            a->state = LEG_EARLY;
            call_unlock(c);
            return;
        }
        if (ev == TXN_EV_FINAL && m) {
            tok_from_tag(m, a->rem_tag, DLG_TAG_MAX);
            tok_to_tag(m, a->peer_tag, DLG_TAG_MAX);
            dlg_uac_on_response(co->cfg.dlg, m);
            if (a->dlg) {
                const char *dt = a->dlg->target;
                size_t dl = strlen(dt);
                if (dl >= 2 && dt[0] == '<' && dt[dl - 1] == '>') {
                    snprintf(a->target, sizeof(a->target), "%.*s", (int)(dl - 2), dt + 1);
                } else {
                    snprintf(a->target, sizeof(a->target), "%s", dt);
                }
                a->nroutes = a->dlg->nroutes;
                for (int i = 0; i < a->dlg->nroutes && i < DLG_ROUTE_MAX; ++i)
                    snprintf(a->routes[i], sizeof(a->routes[i]), "%s", a->dlg->routes[i]);
            }
            disarm_noanswer_timer(c);
            int st = m->status;
            if (st >= 200 && st < 300) {
                if (m->body.len && m->body.len > B2B_SDP_MAX) {
                    send_ack_non2xx(c, 0);
                    a->state = LEG_TERMINATED;
                    call_unlock(c);
                    return;
                }
                if (m->body.len) {
                    memcpy(a->answer, m->body.p, m->body.len);
                    a->answer_len = m->body.len;
                }
                a->state = LEG_CONFIRMED;
                send_ack_leg(c, 0);
                call_unlock(c);
                return;
            }
            /* REQ-ORIGINATE-003 待补：bridge A 腿 401/407 鉴权重试 + 302/380 跟随。
             * 与 B 腿（leg[1]）语义一致：重发成功直接 return（走新事务，不在此回
             * 非 2xx ACK），仅终态才 send_ack_non2xx。 */
            if (uac_invite_handle_challenge(c, 0, m, st) == 1) {
                call_unlock(c);
                return;
            }
            if (uac_invite_follow_redirect(c, 0, m, st) == 1) {
                call_unlock(c);
                return;
            }
            /* 401/407 超限、302 超限/重发失败、其他 3xx-6xx：bridge A 腿直接终止 */
            send_ack_non2xx(c, 0);
            a->state = LEG_TERMINATED;
            call_unlock(c);
            return;
        }
        if (ev == TXN_EV_TIMEOUT) {
            disarm_noanswer_timer(c);
            a->state = LEG_TERMINATED;
            call_unlock(c);
            return;
        }
        call_unlock(c);
        return;
    }

    /* B 腿初始 INVITE / re-INVITE 响应 */
    if (t == c->leg[1].inv_txn) {
        int reinv = c->leg[1].reinv;
        if (ev == TXN_EV_PROVISIONAL && m && m->status < 200) {
            tok_from_tag(m, c->leg[1].rem_tag, DLG_TAG_MAX);
            tok_to_tag(m, c->leg[1].peer_tag, DLG_TAG_MAX);
            dlg_uac_on_response(co->cfg.dlg, m);
            c->leg[1].state = LEG_EARLY;

            /* ---- 100rel：检测 B-leg 可靠临时响应（RFC 3262） ---- */
            int is_reliable = 0;
            unsigned b_peer_rseq = 0;
            sip_hdr_t *rseq_hdr = sip_hdr_get(m, SIP_H_RSEQ);
            sip_hdr_t *req_hdr = sip_hdr_get(m, SIP_H_REQUIRE);
            if (rseq_hdr && req_hdr && req_hdr->value.p) {
                /* Require: 100rel （逗号分隔列表） */
                const char *rv = req_hdr->value.p;
                size_t rl = req_hdr->value.len;
                for (size_t i = 0; i + 5 <= rl; ++i) {
                    if (!strncasecmp(rv + i, "100rel", 6)) {
                        /* 边界检查：前后是逗号/空格/换行或起始/结尾 */
                        int prev_ok = (i == 0) || rv[i-1] == ',' || rv[i-1] == ' ' || rv[i-1] == '\t';
                        int next_ok = (i + 6 >= rl) || rv[i+6] == ',' || rv[i+6] == ' ' || rv[i+6] == '\t';
                        if (prev_ok && next_ok) { is_reliable = 1; break; }
                    }
                }
            }
            /* REQ-SIP-PROFILE-001：profile 关 PRACK → B 腿 reliable 1xx
             * 降级为普通 1xx 透传（不设 rseq，respond_a 不会注入
             * RSeq/Require:100rel；B 侧重传由 ICT 层吸收）。 */
            if (is_reliable && c->prof && !c->prof->enable_prack)
                is_reliable = 0;
            if (is_reliable) {
                b_peer_rseq = (unsigned)atol(rseq_hdr->value.p);
                c->leg[1].peer_rseq = b_peer_rseq;
                /* A-leg 生成新 RSeq：简单自增（1, 2, 3...） */
                c->leg[0].local_rseq = (c->leg[0].local_rseq ? c->leg[0].local_rseq : 0) + 1;
            } else {
                /* 非 reliable 1xx：清 pending 状态 */
                c->leg[1].peer_rseq = 0;
                c->leg[0].local_rseq = 0;
            }

            if (!reinv) {
                /* 透传 180/183（100 本地已发不透传） */
                if (m->status >= 180) {
                    /* Provisional 去重 guard：
                     * RFC 3261 §17.1.1 — ICT 在 PROCEEDING 状态对每个收到的
                     * provisional 都回调 TU；UDP 上对端会重传相同的 180/183，
                     * 如果 B2BUA 每次都 respond_a，就会给 A-leg 发重复的 180。
                     * 同一 status 的重复只发一次；不同 status（180→183）放行；
                     * 100rel reliable 1xx 也放行（RSeq 变化代表新消息）。 */
                    int skip_dup = (c->leg[1].last_prov_status == m->status)
                                   && !is_reliable;
                    if (!skip_dup) {
                        respond_a(c, m->status,
                                  m->body.p ? m->body.p : NULL,
                                  m->body.len);
                        c->leg[1].last_prov_status = m->status;
                        /* P0 PBX：Ring NoAnswer 超时器 arm（首次 180/183 才 arm） */
                        arm_noanswer_timer(c);
                    } else {
                        llog(LOG_DEBUG, "b2b", "skip dup provisional %d callid=%s",
                             m->status, c->leg[0].callid);
                    }
                }
            } else if (c->leg[0].srv_txn) {
                /* B 发起 re-INVITE 的响应？B 的 inv_txn 是我们发出，
                 * reinv 时源在 A，因此应答给 A 的 srv_txn */
                respond_a(c, m->status,
                          m->body.p ? m->body.p : NULL, m->body.len);
            }
            call_unlock(c);
            return;
        }
        if (ev == TXN_EV_FINAL && m) {
            b2b_leg_t *b = &c->leg[1];  /* 外层作用域——P1-12 challenge 块也用 */
            tok_from_tag(m, c->leg[1].rem_tag, DLG_TAG_MAX);
            tok_to_tag(m, c->leg[1].peer_tag, DLG_TAG_MAX);
            dlg_uac_on_response(co->cfg.dlg, m);
            disarm_noanswer_timer(c); /* 任何终态都 disarm ring timer */
            c->leg[1].last_prov_status = 0; /* 终态后重置去重标记 */
            if (c->leg[1].dlg) {
                /* b 已在外层声明 */
                /* dlg->target 来自 200 OK Contact 头（<sip:...> name-addr 格式），
                 * Request-URI 不允许 <>，必须剥掉尖括号再存入 b->target，
                 * 否则后续 BYE/ACK/INFO 等 dialog 内请求都会带 <> → SIP.js Parser 崩。 */
                const char *dt = b->dlg->target;
                size_t dl = strlen(dt);
                if (dl >= 2 && dt[0] == '<' && dt[dl - 1] == '>') {
                    snprintf(b->target, sizeof(b->target), "%.*s",
                             (int)(dl - 2), dt + 1);
                } else {
                    snprintf(b->target, sizeof(b->target), "%s", dt);
                }
                b->nroutes = b->dlg->nroutes;
                for (int i = 0; i < b->dlg->nroutes &&
                                i < DLG_ROUTE_MAX; ++i)
                    snprintf(b->routes[i], sizeof(b->routes[i]), "%s",
                             b->dlg->routes[i]);
            }
            int st = m->status;
            if (st >= 200 && st < 300) {
                const char *ab = m->body.p;
                size_t abl = m->body.len;
                if (m->body.len && m->body.len > B2B_SDP_MAX) {
                    finish_with_a_response(c, 488, NULL, 0);
                    call_unlock(c);
                    return;
                }
                /* answer_hook 多槽链：各模块依次改写 */
                if (m->body.len && g_sdp_hook_n[B2B_SDP_ANSWER] > 0) {
                    char buf[B2B_SDP_MAX]; buf[0] = '\0';
                    pthread_mutex_lock(&g_sdp_hook_lock);
                    for (int i = 0; i < g_sdp_hook_n[B2B_SDP_ANSWER]; ++i) {
                        char tbuf[B2B_SDP_MAX]; tbuf[0] = '\0';
                        const char *in = buf[0] ? buf : m->body.p;
                        size_t inlen = buf[0] ? strlen(buf) : m->body.len;
                        int n = g_sdp_hooks[B2B_SDP_ANSWER][i].fn(c, in, inlen, tbuf,
                                                                   sizeof(tbuf),
                                                                   g_sdp_hooks[B2B_SDP_ANSWER][i].ud);
                        if (n >= 0) {
                            memcpy(buf, tbuf, (size_t)n);
                            buf[n] = '\0';
                        }
                    }
                    pthread_mutex_unlock(&g_sdp_hook_lock);
                    if (buf[0]) { ab = buf; abl = strlen(buf); }
                }
                if (abl) {
                    memcpy(c->leg[1].answer, ab, abl);
                    c->leg[1].answer_len = abl;
                }
                c->leg[1].state = LEG_CONFIRMED;

                /* 存 Bob 200 OK 的 Session-Expires 到 B leg（P0 透传 + P1 协商都需要） */
                {
                    sip_hdr_t *bose = sip_hdr_get(m, SIP_H_SESSION_EXPIRES);
                    if (bose)
                        snprintf(c->leg[1].session_expires, sizeof(c->leg[1].session_expires),
                                 "%.*s", (int)bose->value.len, bose->value.p);
                }

                /* Session Timer: extract Bob 200 OK + arm both legs
                 * ★ 三层守卫：cfg.sess_timer_enabled (全局) → force_timer (dialplan 三态) → profile.enable_timer */
                {
                    sip_profile_t *pr = c->prof;
                    /* force_timer 三态：0=AUTO(继承 profile) / 1=强制开 / 2=强制关 */
                    int prof_en = pr ? pr->enable_timer : 0;  /* profile NULL → 默认关 */
                    int force_en = (c->force_timer == 1) ? 1 : (c->force_timer == 2 ? 0 : prof_en);
                    if (co->cfg.sess_timer_enabled && force_en) {
                    sess_timer_nego_t nb;
                    b2b_leg_t *al = &c->leg[0];
                    uint64_t nm = now_ms();
                    /* A-leg arm（协商后值格式 "Session-Expires: N;refresher=xxx\r\n"） */
                    if (al->session_expires[0]) {
                        sess_timer_nego_t na;
                        const char *v = al->session_expires;
                        if ((v = strchr(v, ':')) != NULL) v++;
                        na.interval_s = atoi(v);
                        /* 从协商后的值解析 refresher */
                        na.refresher = SIP_REF_NONE;
                        {
                            const char *rp = strstr(v, "refresher=");
                            if (rp) { rp += 10; if (!strncmp(rp, "uac", 3)) na.refresher = SIP_REF_UAC;
                                else if (!strncmp(rp, "uas", 3)) na.refresher = SIP_REF_UAS; }
                        }
                        if (na.interval_s >= co->cfg.sess_timer_cfg.min_se_s) {
                            /* A-leg: B2BUA=UAS → 仅当 refresher=uas 时 B2BUA 是刷新方 → arm st + refresh_node；
                             * 否则只 arm expire_node 做超时监控 */
                            if (na.refresher == SIP_REF_UAS) {
                                sess_timer_arm(&al->st, &na, nm);
                                tw_del(co->cfg.tw, &al->refresh_node);
                                int64_t half_ms = (int64_t)na.interval_s * 500 + 200;
                                tw_add(co->cfg.tw, &al->refresh_node, (uint64_t)((int64_t)nm + half_ms), sess_timer_refresh_cb, c);
                            }
                            tw_del(co->cfg.tw, &al->expire_node);
                            tw_add(co->cfg.tw, &al->expire_node, nm + (uint64_t)na.interval_s * 1000u + SESS_TIMER_GRACE_MS, sess_timer_expire_cb, c);
                        }
                    }
                    /* B-leg arm（从 Bob 200 OK） */
                    sip_hdr_t *bose = sip_hdr_get(m, SIP_H_SESSION_EXPIRES);
                    if (bose) {
                        nb.interval_s = atoi(bose->value.p);
                        nb.refresher = SIP_REF_NONE;
                        const char *rp = strstr(bose->value.p, "refresher=");
                        if (rp) { rp += 10; if (!strncmp(rp, "uac", 3)) nb.refresher = SIP_REF_UAC; else if (!strncmp(rp, "uas", 3)) nb.refresher = SIP_REF_UAS; }
                        if (nb.interval_s >= co->cfg.sess_timer_cfg.min_se_s) {
                            /* B-leg: B2BUA=UAC → 仅当 refresher=uac 时 B2BUA 是刷新方 */
                            if (nb.refresher == SIP_REF_UAC) {
                                sess_timer_arm(&c->leg[1].st, &nb, nm);
                                tw_del(co->cfg.tw, &c->leg[1].refresh_node);
                                int64_t half_ms2 = (int64_t)nb.interval_s * 500 + 200;
                                tw_add(co->cfg.tw, &c->leg[1].refresh_node, (uint64_t)((int64_t)nm + half_ms2), sess_timer_refresh_cb, c);
                            }
                            tw_del(co->cfg.tw, &c->leg[1].expire_node);
                            tw_add(co->cfg.tw, &c->leg[1].expire_node, nm + (uint64_t)nb.interval_s * 1000u + SESS_TIMER_GRACE_MS, sess_timer_expire_cb, c);
                        }
                    }
                }
                } /* closes profile guard block */
                send_ack_leg(c, 1);
                if (!reinv) {
                    /* 向 A 透传 2xx，等 A 腿 ACK 后确认 */
                    respond_a(c, st, c->leg[1].answer,
                              c->leg[1].answer_len);
                    c->a_final = st;
                    {
                        char sess[384];
                        snprintf(sess, sizeof(sess),
                                 "{\"event\":\"answer\",\"call_id\":\"%s\",\"state\":\"confirmed\","
                                 "\"from\":\"%s\",\"to\":\"%s\",\"dir\":\"inbound\",\"cause\":0}",
                                 c->leg[0].callid,
                                 c->leg[0].from, c->leg[0].ruri);
                        uds_send_session(sess);
                    }
                } else {
                    sip_upd_end(&c->leg[1].guard);
                    c->leg[1].reinv = 0;
                    if (c->leg[0].srv_txn) {
                        /* A 发起的 re-INVITE：应答给 A */
                        respond_a(c, st, c->leg[1].answer,
                                  c->leg[1].answer_len);
                        c->leg[0].srv_txn = NULL;
                    }
                }
                call_unlock(c);
                return;
            }

            /* === P1-12：INVITE 401/407 Challenge 处理（非 re-INVITE 场景） ===
             *
             * 两轮挑战合并：
             *   407 Proxy Authentication Required → 存 proxy_ch_* + proxy_auth_hdr
             *   401 Unauthorized                 → 存 www_ch_* + www_auth_hdr
             *   两轮都 set（auth_flags bit0+bit1）→ 下一次重发同时拼两种头
             *
             * 重发走 send_invite_b 新事务（CSeq+1，新 branch），extra 里拼鉴权头。
             * 防死循环：auth_attempts >= 3 放弃，走下面 3xx-6xx 路径给 A 腿透传。
             *
             * 独立存储（存 b2b_leg_t.per-call 字段）——**绝对不污染 outreg_trunk_t.ch_* 缓存**。 */
            if (!reinv && (st == 401 || st == 407) && b->auth_attempts < 3
                && c->rt_name[0] && b2b_rt(co)) {
                llog(LOG_INFO, "b2b", "P1-12 INVITE challenge handling st=%d auth_attempts=%u rt_name=%s",
                     st, b->auth_attempts, c->rt_name);


                const trunk_t *tr = rt_trunk_get((route_table_t *)b2b_rt(co), c->rt_name);
                if (!tr) {
                    llog(LOG_WARN, "b2b", "P1-12 401/407 but trunk %s not in route_table", c->rt_name);
                } else if (!tr->password[0]) {
                    llog(LOG_WARN, "b2b", "P1-12 trunk %s has no password → cannot respond to 401/407", c->rt_name);
                } else {
                    b->auth_attempts++;

                    /* 1. 解析 challenge 头（按 status 分流 WWW-Authenticate / Proxy-Authenticate） */
                    const char *hdr_name = (st == 407) ? "Proxy-Authenticate" : "WWW-Authenticate";
                    sip_hdr_t *ch_hdr = sip_hdr_get(m, st == 407 ? SIP_H_PROXY_AUTHENTICATE : SIP_H_WWW_AUTHENTICATE);
                    if (!ch_hdr || !ch_hdr->value.p) {
                        llog(LOG_WARN, "b2b", "P1-12 401/407 but no %s header found, callid=%s",
                             hdr_name, c->leg[0].callid);
                    } else {
                        digest_challenge_t ch; memset(&ch, 0, sizeof(ch));
                        int rc = digest_parse_challenge(ch_hdr->value.p, &ch);
                        if (rc != NSBC_OK) {
                            llog(LOG_WARN, "b2b", "P1-12 digest_parse_challenge failed rc=%d callid=%s",
                                 rc, c->leg[0].callid);
                        } else {
                            /* 2. 存 challenge 参数到 per-call 字段 */
                            if (st == 407) {
                                snprintf(b->proxy_ch_realm, sizeof(b->proxy_ch_realm), "%s",
                                         ch.realm[0] ? ch.realm : tr->realm);
                                snprintf(b->proxy_ch_nonce, sizeof(b->proxy_ch_nonce), "%s", ch.nonce);
                                snprintf(b->proxy_ch_opaque, sizeof(b->proxy_ch_opaque), "%s", ch.opaque);
                                snprintf(b->proxy_ch_qop, sizeof(b->proxy_ch_qop), "%s", ch.qop);
                                if (ch.stale) b->nc_counter = 0;
                                b->auth_flags |= 0x01;
                                log_debug("b2b", "P1-12 407 Proxy challenge cached callid=%s realm=%s",
                                          c->leg[0].callid, b->proxy_ch_realm);
                            } else {
                                snprintf(b->www_ch_realm, sizeof(b->www_ch_realm), "%s",
                                         ch.realm[0] ? ch.realm : tr->realm);
                                snprintf(b->www_ch_nonce, sizeof(b->www_ch_nonce), "%s", ch.nonce);
                                snprintf(b->www_ch_opaque, sizeof(b->www_ch_opaque), "%s", ch.opaque);
                                snprintf(b->www_ch_qop, sizeof(b->www_ch_qop), "%s", ch.qop);
                                if (ch.stale) b->nc_counter = 0;
                                b->auth_flags |= 0x02;
                                log_debug("b2b", "P1-12 401 WWW challenge cached callid=%s realm=%s",
                                          c->leg[0].callid, b->www_ch_realm);
                            }

                            /* 3. HA1 现算 + digest_client_build_auth 构建头值
                             *    两轮挑战合并：auth_flags 两个 bit 都 set → 同时构建两种头
                             *
                             * !!! HA1 必须用 challenge 里给的 realm（RFC 2617 §3.2.1），
                             *     绝对不能用 tr->realm 替代 —— Digest HA1 = MD5(user:realm:pw)，
                             *     realm 不对 HA2 就永远对不上。 */
                            char ha1[33];
                            const char *ha1_realm = NULL;
                            if (b->auth_flags & 0x01) ha1_realm = b->proxy_ch_realm[0] ? b->proxy_ch_realm : tr->realm;
                            if (b->auth_flags & 0x02) ha1_realm = b->www_ch_realm[0]  ? b->www_ch_realm  : tr->realm;
                            if (!ha1_realm)           ha1_realm = tr->realm[0]       ? tr->realm       : tr->host;
                            digest_make_ha1(tr->auth_user, ha1_realm, tr->password, ha1);

                            /* 407 → Proxy-Authorization（专家 2.1：注意不是 Authorization！） */
                            if (b->auth_flags & 0x01) {
                                uint32_t nc; char nc_buf[16], cnonce[16];
                                b->nc_counter++; nc = b->nc_counter;
                                snprintf(nc_buf, sizeof(nc_buf), "%08x", (unsigned)nc);
                                nsbc_rand_token(cnonce, sizeof(cnonce), 8);
                                digest_client_build_auth(
                                    tr->auth_user,
                                    b->proxy_ch_realm[0] ? b->proxy_ch_realm : tr->realm,
                                    b->proxy_ch_nonce,
                                    b->proxy_ch_opaque[0] ? b->proxy_ch_opaque : NULL,
                                    b->proxy_ch_qop[0] ? b->proxy_ch_qop : "auth",
                                    nc_buf, cnonce, "INVITE",
                                    b->ruri,
                                    ha1, 1,
                                    b->proxy_auth_hdr, sizeof(b->proxy_auth_hdr));
                            }
                            /* 401 → Authorization */
                            if (b->auth_flags & 0x02) {
                                uint32_t nc; char nc_buf[16], cnonce[16];
                                b->nc_counter++; nc = b->nc_counter;
                                snprintf(nc_buf, sizeof(nc_buf), "%08x", (unsigned)nc);
                                nsbc_rand_token(cnonce, sizeof(cnonce), 8);
                                digest_client_build_auth(
                                    tr->auth_user,
                                    b->www_ch_realm[0] ? b->www_ch_realm : tr->realm,
                                    b->www_ch_nonce,
                                    b->www_ch_opaque[0] ? b->www_ch_opaque : NULL,
                                    b->www_ch_qop[0] ? b->www_ch_qop : "auth",
                                    nc_buf, cnonce, "INVITE",
                                    b->ruri,
                                    ha1, 1,
                                    b->www_auth_hdr, sizeof(b->www_auth_hdr));
                            }

                            /* 4. 拼 extra 头块（Proxy-Authorization + Authorization，按两轮是否都 set 决定） */
                            char auth_extra[512]; size_t ax = 0;
                            if (b->auth_flags & 0x01) {
                                ax += (size_t)snprintf(auth_extra + ax, sizeof(auth_extra) - ax,
                                    "Proxy-Authorization: %s\r\n", b->proxy_auth_hdr);
                            }
                            if (b->auth_flags & 0x02) {
                                ax += (size_t)snprintf(auth_extra + ax, sizeof(auth_extra) - ax,
                                    "Authorization: %s\r\n", b->www_auth_hdr);
                            }

                            /* 5. 重发 INVITE：清旧 dlg（新 branch 必须），CSeq+1，extra 带鉴权头 */
                            llog(LOG_INFO, "b2b",
                                 "P1-12 pre-re-send offer_len=%zu offer_head=%.64s",
                                 b->offer_len, b->offer ? b->offer : "(null)");
                            if (b->dlg) { dlg_close(co->cfg.dlg, b->dlg); b->dlg = NULL; }
                            b->cseq++;
                            b->inv_txn = send_invite_b(c, 1, b->offer, b->offer_len, 0,
                                                       auth_extra);
                            if (b->inv_txn) {
                                llog(LOG_INFO, "b2b",
                                     "P1-12 INVITE re-sent trunk=%s auth_attempts=%u auth_flags=0x%02x callid=%s",
                                     c->rt_name, b->auth_attempts, b->auth_flags, c->leg[0].callid);
                                call_unlock(c);
                                return; /* 成功重发 → 不走 send_ack_non2xx */
                            } else {
                                llog(LOG_WARN, "b2b", "P1-12 INVITE re-send FAILED trunk=%s", c->rt_name);
                                /* txn 创建失败 → 继续走 3xx-6xx 路径透传给 A 腿 */
                                b->cseq--; /* 回滚 CSeq */
                                b->auth_attempts--; /* 回滚计数让下次可能重试？不，直接放弃 */
                            }
                        }
                    }
                }
            }

            /* 3xx-6xx（含 401/407 challenge 处理失败时的透传 fallback） */
            if (st == 401 || st == 407) {
                llog(LOG_WARN, "b2b", "P1-12 SKIP INVITE challenge st=%d reinv=%d attempts=%u rt_name=%s rt=%p",
                     st, reinv, b->auth_attempts, c->rt_name[0] ? c->rt_name : "(empty)",
                     b2b_rt(co));
            }
            tok_from_tag(m, c->leg[1].rem_tag, DLG_TAG_MAX);
            tok_to_tag(m, c->leg[1].peer_tag, DLG_TAG_MAX);
            send_ack_non2xx(c, 1);
            /* REQ-SIP-DIV-001 P0：302/380 自动重定向（非 re-INVITE 场景） */
            if (!reinv && (st == 302 || st == 380) &&
                co->cfg.auto_redirect_302 &&
                c->redirect_count < co->cfg.max_redirects) {
                char new_target[256];
                int rc = b2b_extract_contact_uri(m, new_target,
                                                  sizeof(new_target));
                if (rc == NSBC_OK) {
                    /* 追加 History-Info 链（RFC 4244/7044） */
                    b2b_append_history_info(c, new_target, st);
                    c->redirect_count++;

                    /* B 腿旧 dialog 不再有效，重定向到新目标
                     * b 已在外层（TXN_EV_FINAL 块开头）声明 */
                    if (b->dlg) {
                        dlg_close(co->cfg.dlg, b->dlg);
                        b->dlg = NULL;
                    }
                    b->nroutes = 0;    /* 清空旧 Route */
                    b->target[0] = '\0';
                    b->ruri[0] = '\0';
                    b->rem_tag[0] = '\0';  /* 旧响应 tag 已废弃（新 dialog 尚未握手） */
                    b->peer_tag[0] = '\0';
                    /* REQ-ARCH-015：302 follow 先释放旧 key 并发槽
                     * （resolve_cb 会预填新 b_key，之后再 try_inc） */
                    if (c->key_held) {
                        perkey_dec(co, c->b_key);
                        c->key_held = 0;
                        c->b_key[0] = '\0'; /* 清空前值，防止 try_inc 失败后残留 */
                    }
                    /* resolve 新目的地（复用 dialplan 注册的 resolve_cb，输出 b->to） */
                    if (co->cfg.resolve) {
                        int rr = co->cfg.resolve(c, new_target, strlen(new_target),
                                                 &b->dst, b->ruri, sizeof(b->ruri),
                                                 b->to, sizeof(b->to),
                                                 co->cfg.resolve_ud);
                        if (rr != NSBC_OK) {
                            /* resolve 失败 → 退回原始 URI（假设 Contact 已可直连） */
                            snprintf(b->target, sizeof(b->target), "%s", new_target);
                            snprintf(b->ruri, sizeof(b->ruri), "%s", new_target);
                            snprintf(b->to, sizeof(b->to), "%s", new_target);
                        } else {
                            /* resolve 成功 → 新 key 已由 resolve_cb 预填，try_inc */
                            if (c->b_key[0] && !perkey_try_inc(co, c->b_key)) {
                                /* REQ-ARCH-015：302 follow per-key 超限 → 硬拒绝 486
                                 * Advisor 2026-10-01i 审计：不能 fallback Contact 直连——
                                 * 攻击者可构造 302 redirect 绕过 per-AoR 并发限制。
                                 * A 腿已 up，486 Busy Here 语义正确（上游可再尝试/走 CFNR）。 */
                                llog(LOG_WARN, "b2b", "302 perkey_limit for %s, 486",
                                     c->b_key);
                                b2b_terminate(c, 486);
                                return;
                            } else if (c->b_key[0]) {
                                c->key_held = 1; /* 新 key 占槽成功 */
                            }
                        }
                    } else {
                        snprintf(b->target, sizeof(b->target), "%s", new_target);
                        snprintf(b->ruri, sizeof(b->ruri), "%s", new_target);
                        snprintf(b->to, sizeof(b->to), "%s", new_target);
                    }
                    /* CSeq 递增（重定向是新 INVITE，必须新 CSeq） */
                    b->cseq++;
                    b->state = LEG_CALLING;
                    /* 重建 UAC dialog（新 target，新 cseq，同一 callid/local_tag） */
                    b->dlg = dlg_uac_new(co->cfg.dlg, b->callid,
                                         strlen(b->callid), b->loc_tag,
                                         strlen(b->loc_tag), b->target,
                                         b->cseq, SIP_M_INVITE);
                    if (b->dlg) {
                        b->dlg->ud = c;
                    } else {
                        /* 内存压力 / dialog 表满：事务仍能走，但 dialog 状态追踪丢失
                         * — 后续 re-INVITE / BYE-by-dialog-match 会失败 */
                        llog(LOG_WARN, "b2b",
                             "302 redirect dlg_uac_new FAILED (dialog tracking lost) "
                             "callid=%s target=%s redirect_count=%d",
                             c->leg[0].callid, new_target,
                             c->redirect_count);
                    }
                    /* 清旧 offer（重定向可能用不同的 SDP offer——但我们 A 的 offer
                     * 不变，所以用原 offer 即可） */
                    /* REQ-SIP-DIV-001：extra 头块 = History-Info + Diversion(RFC 5806)
                     * 头块大小：history_info[1024] + Diversion[130] ≈ 1154 → 1200 够装
                     * privacy=history 时不拼接（截断内部路由轨迹） */
                    char extra_hi_dv[1200];
                    size_t extra_used = 0;
                    extra_hi_dv[0] = '\0';
                    /* REQ-PBX-DP-003：CLIR Privacy: id → 放在 History-Info/Diversion 之前 */
                    if (c->privacy_id) {
                        int pn = snprintf(extra_hi_dv, sizeof(extra_hi_dv),
                                          "Privacy: id\r\n");
                        if (pn > 0 && (size_t)pn < sizeof(extra_hi_dv))
                            extra_used = (size_t)pn;
                    }
                    if (!c->privacy_history && c->history_info[0]) {
                        size_t hi_len = strlen(c->history_info);
                        if (hi_len < sizeof(extra_hi_dv)) {
                            memcpy(extra_hi_dv, c->history_info, hi_len);
                            extra_used = hi_len;
                        }
                    }
                    if (!c->privacy_history) {
                        int dn = b2b_build_diversion_header(
                            extra_hi_dv + extra_used,
                            sizeof(extra_hi_dv) - extra_used,
                            c->orig_to[0] ? c->orig_to : b->to, NULL,
                            c->redirect_count);
                        if (dn > 0) extra_used += (size_t)dn;
                    }
                    extra_hi_dv[extra_used] = '\0';
                    b->inv_txn = send_invite_b(c, 1, b->offer, b->offer_len, 0,
                                               extra_hi_dv[0] ? extra_hi_dv : NULL);
                    if (!b->inv_txn) {
                        /* 重定向 INVITE 发失败 → 透传给 A 腿原码 */
                        llog(LOG_WARN, "b2b", "302 redirect send failed, target=%s",
                             new_target);
                        c->redirect_count--; /* 失败不计数 */
                        b->state = LEG_TERMINATED;
                        finish_with_a_response(c, st, NULL, 0);
                    } else {
                        llog(LOG_INFO, "b2b", "302 redirected count=%d target=%s",
                             c->redirect_count, new_target);
                    }
                    call_unlock(c);
                    return;
                }
                /* Contact 解析失败 → 降级透传 */
                llog(LOG_WARN, "b2b", "302 redirect: Contact parse failed");
            }
            if (!reinv) {
                c->leg[1].state = LEG_TERMINATED;
                {
                    char sess[384];
                    snprintf(sess, sizeof(sess),
                             "{\"event\":\"hangup\",\"call_id\":\"%s\",\"state\":\"terminated\","
                             "\"from\":\"%s\",\"to\":\"%s\",\"dir\":\"inbound\",\"cause\":%d}",
                             c->leg[0].callid,
                             c->leg[0].from, c->leg[0].ruri, st);
                    uds_send_session(sess);
                }
                finish_with_a_response(c, st, NULL, 0);
                call_unlock(c);
                return;
            }
            sip_upd_end(&c->leg[1].guard);
            c->leg[1].reinv = 0;
            c->leg[1].inv_txn = NULL;
            if (c->leg[0].srv_txn) {
                respond_a(c, st, NULL, 0);
                c->leg[0].srv_txn = NULL;
            }
            call_unlock(c);
            return;
        }
        if (ev == TXN_EV_TIMEOUT) {
            /* Timer B/F：B 腿完全无响应（含 1xx 都没收到）。
             * sip_txn 在收到 provisional 时会停 Timer B，所以此处与
             * Ring NoAnswer 定时器是**互斥**的：收到 1xx 才 arm
             * ring_noanswer，timer B 永远不会在收到 1xx 后触发。
             * 响应码应为 408 Request Timeout（被叫超时）而非 504 Server
             * Time-out（上游超时——语义错，504 表示"我方到上游超时"）。 */
            llog(LOG_WARN, "b2b",
                 "timer B/F: B leg no response (not even 1xx), callid=%s → 408",
                 c->leg[0].callid);
            disarm_noanswer_timer(c); /* 防御：若 Timer B 先触发，清 ring timer */
            if (reinv) {
                sip_upd_end(&c->leg[1].guard);
                c->leg[1].reinv = 0;
                c->leg[1].inv_txn = NULL;
                if (c->leg[0].srv_txn) {
                    respond_a(c, 408, NULL, 0);
                    c->leg[0].srv_txn = NULL;
                }
            } else {
                finish_with_a_response(c, 408, NULL, 0);
            }
        }
        call_unlock(c);
        return;
    }

    /* 发向 A 腿的在轮事务（B 发起 re-INVITE/MESSAGE/INFO 或 API 触发） */
    if (t == c->leg[0].out_txn) {
        if (ev == TXN_EV_FINAL && m) {
            int st = m->status;
            if (c->leg[1].srv_txn) {
                /* B 发起 re-INVITE/MESSAGE/INFO：把 A 的应答转回 B。
                 * 响应 body 的 Content-Type 原样透传（MESSAGE/INFO
                 * 200 带非 SDP body 时不能强标 application/sdp）。 */
                if (st >= 200 && st < 300) {
                    char ctbuf[160];
                    ctbuf[0] = '\0';
                    if (m->body.len && m->body.p) {
                        sip_hdr_t *ct = sip_hdr_get(m, SIP_H_CONTENT_TYPE);
                        if (ct && ct->value.p && ct->value.len)
                            snprintf(ctbuf, sizeof(ctbuf),
                                     "Content-Type: %.*s\r\n",
                                     (int)ct->value.len, ct->value.p);
                    }
                    respond_txn(co, c->leg[1].srv_txn, st, NULL,
                                ctbuf[0] ? ctbuf : NULL,
                                m->body.p, m->body.len);
                } else {
                    send_ack_non2xx(c, 0);
                    respond_txn(co, c->leg[1].srv_txn, st, NULL, NULL,
                                NULL, 0);
                }
                c->leg[1].srv_txn = NULL;
                c->leg[0].reinv = 0;
            } else if (st >= 200 && st < 300 &&
                       c->leg[1].state == LEG_TERMINATING) {
                /* 转发 BYE 完成，收尾 */
                c->leg[0].state = LEG_TERMINATED;
                c->leg[1].state = LEG_TERMINATED;
                c->leg[0].out_txn = NULL;
                call_unlock(c);
                call_destroy(c);
                return;
            }
            /* 双腿 guard 释放：srv_txn 有无都要放（reinv 路径上面已放
             * 过，但 sip_upd_end 幂等；API 发起 MESSAGE/INFO 无 srv_txn，
             * guard 必须在此处释放）。 */
            sip_upd_end(&c->leg[0].guard);
            sip_upd_end(&c->leg[1].guard);
        } else if (ev == TXN_EV_TIMEOUT) {
            /* Timer B/F：A 腿无终响应（B 发起的 re-INVITE/MESSAGE/INFO
             * 或 API 触发）→ 408 回 B 腿（若有 srv_txn）+ 释放 guard */
            llog(LOG_WARN, "b2b",
                 "timer B/F: A leg no response, callid=%s → 408",
                 c->leg[0].callid);
            if (c->leg[1].srv_txn) {
                respond_txn(co, c->leg[1].srv_txn, 408, NULL, NULL, NULL, 0);
                c->leg[1].srv_txn = NULL;
            }
            sip_upd_end(&c->leg[0].guard);
            sip_upd_end(&c->leg[1].guard);
            c->leg[0].reinv = 0;
            c->leg[0].inv_txn = NULL;
        }
        c->leg[0].out_txn = NULL;
        call_unlock(c);
        return;
    }

    /* 发向 B 腿的 NICT（CANCEL/BYE/PRACK/UPDATE/MESSAGE/INFO 转发） */
    if (t == c->leg[1].out_txn) {
        if (ev == TXN_EV_FINAL && m) {
            /* CANCEL 得 487 由 B 腿 INVITE 事务分支收尾；BYE 200 收尾 */
            if (m->status >= 200 && m->status < 300 &&
                c->leg[0].state == LEG_TERMINATING) {
                llog(LOG_INFO, "b2b", "hangup ok -> call_destroy!");
                c->leg[0].state = LEG_TERMINATED;
                c->leg[1].state = LEG_TERMINATED;
                c->leg[1].out_txn = NULL;
                call_unlock(c);
                call_destroy(c);
                return;
            }
            /* MESSAGE/INFO 延迟响应：handler 存了 A 腿 srv_txn，此处把
             * B 腿终响应回给 A（PRACK/UPDATE 已立即回过，srv_txn 为空
             * 不触发）。响应 body 的 Content-Type 原样透传。 */
            if (c->leg[0].srv_txn) {
                char ctbuf[160];
                ctbuf[0] = '\0';
                if (m->body.len && m->body.p) {
                    sip_hdr_t *ct = sip_hdr_get(m, SIP_H_CONTENT_TYPE);
                    if (ct && ct->value.p && ct->value.len)
                        snprintf(ctbuf, sizeof(ctbuf),
                                 "Content-Type: %.*s\r\n",
                                 (int)ct->value.len, ct->value.p);
                }
                respond_txn(co, c->leg[0].srv_txn, m->status, NULL,
                            ctbuf[0] ? ctbuf : NULL,
                            m->body.len ? m->body.p : NULL,
                            m->body.len);
                c->leg[0].srv_txn = NULL;
            }
            /* PRACK/UPDATE/MESSAGE/INFO 转发终响应：释放双 leg 的 upd guard
             * （转发时 begin 了 l/o 两腿，否则后续 in-dialog 请求会 491）。
             * sip_upd_end 幂等，BYE/CANCEL 场景无害。 */
            sip_upd_end(&c->leg[0].guard);
            sip_upd_end(&c->leg[1].guard);
        } else if (ev == TXN_EV_TIMEOUT) {
            /* Timer F：B 腿无终响应 → 408 回 A 腿 + 释放 guard（防死锁） */
            llog(LOG_WARN, "b2b",
                 "timer F: B leg no final response, callid=%s → 408",
                 c->leg[0].callid);
            if (c->leg[0].srv_txn) {
                respond_txn(co, c->leg[0].srv_txn, 408, NULL, NULL, NULL, 0);
                c->leg[0].srv_txn = NULL;
            }
            sip_upd_end(&c->leg[0].guard);
            sip_upd_end(&c->leg[1].guard);
        }
        c->leg[1].out_txn = NULL;
        call_unlock(c);
        return;
    }

    /* re-INVITE 的 CANCEL 应答（200/481）：仅清理 */
    if (t == c->leg[0].cancel_txn) {
        c->leg[0].cancel_txn = NULL;
        call_unlock(c);
        return;
    }
    if (t == c->leg[1].cancel_txn) {
        c->leg[1].cancel_txn = NULL;
        call_unlock(c);
        return;
    }
    call_unlock(c);
}

/* ------------------------------------------------------------------ */
/* 呼叫失败/结束                                                         */
/* ------------------------------------------------------------------ */
static void finish_with_a_response(b2b_call_t *c, int status,
                                   const char *body, size_t bodylen)
{
    b2b_leg_t *a = &c->leg[0];
    if (a->callid[0] && !c->a_final) {
        respond_a(c, status, body, bodylen);
        c->a_final = status;
    }
    a->state = LEG_TERMINATED;
    /* ★ B 腿还在 CALLING/EARLY → 先发 CANCEL，再设 TERMINATED
     * send_cancel_req 内部 guard：只对 CALLING/EARLY 发，CONFIRMED/已结束则跳过 */
    send_cancel_req(c, 1);
    c->leg[1].state = LEG_TERMINATED;
    add_atomic(&c->core->failed, 1);
    /* ACK 到非 2xx 由 A 侧 IST 吸收；对象立即回收，重传由事务缓存应答 */
    call_destroy(c);
}

/* 在指定腿发 CANCEL（B 腿在轮 INVITE） */
static void send_cancel_req(b2b_call_t *c, int leg)
{
    b2b_core_t *co = c->core;
    b2b_leg_t *l = &c->leg[leg];
    if (l->state != LEG_CALLING && l->state != LEG_EARLY && !l->reinv)
        return;
    char branch[40];
    new_branch(branch, sizeof(branch));
    char out[SIP_MAX_MSG];
    int n = build_dlg_req(co, l, SIP_M_CANCEL, l->cseq,
                          l->target[0] ? l->target : l->ruri,
                          NULL /*To 无 tag 与 INVITE 同*/, branch,
                          NULL, NULL, 0, out, sizeof(out));
    if (n <= 0)
        return;
    /* 设本地发送端口：与初始 INVITE 一致，用 out_prof */
    {
        struct sip_profile *op = c->out_prof ? c->out_prof : c->prof;
        g_rx_local_port = (op && op->sip_udp_port)
                          ? op->sip_udp_port
                          : (uint16_t)co->cfg.local_udp_port;
    }
    sip_txn_t *t = txn_client_new(co->cfg.txn, TXN_NICT, branch,
                                  strlen(branch), SIP_M_CANCEL, &l->dst,
                                  l->reliable, out, (size_t)n, cli_tu, c,
                                  now_ms());
    if (t) {
        if (l->reinv)
            l->cancel_txn = t;
        else
            l->out_txn = t;
    }
}

static void send_bye_leg(b2b_call_t *c, int leg)
{
    b2b_core_t *co = c->core;
    b2b_leg_t *l = &c->leg[leg];
    if (l->state != LEG_CONFIRMED)
        return;

    /* ★ BYE 转发诊断日志：在 build_dlg_req 之前把所有关键 tag 打出来，
     * 方便定位 481 Call/Transaction Does Not Exist。 */
    int dlg_dir = l->dlg ? (int)l->dlg->dir : -1;
    llog(LOG_INFO, "b2b",
         "★BYE_PREP leg=%d dlg_dir=%d loc_tag=[%s] rem_tag=[%s] "
         "peer_tag=[%s] from=[%s] to=[%s] ruri=[%s]",
         leg, dlg_dir, l->loc_tag, l->rem_tag, l->peer_tag,
         l->from, l->to, l->target[0] ? l->target : l->ruri);

    /* REQ-SIP-DIALOG-001：dialog 内请求的 To 必须带对端 tag。
     * 与 ACK 同理（send_ack_leg 已有修复）：To tag 必须是 peer_tag，
     * 即 200 OK 的 To 头 tag（= 对端 UAS 的 local tag）。
     * 绝对不能用 rem_tag（= From 头 tag = dialog remote tag）！
     * Bug 历史：之前 BYE 用 rem_tag，在 B leg 上 rem_tag 被错误设置为
     * A leg 的 loc_tag，导致 From/To tag 一样 → 对端 481。
     * peer_tag 为空时 fallback 到 rem_tag 兜底。 */
    const char *bye_tag = l->peer_tag[0] ? l->peer_tag
                       : (l->rem_tag[0] ? l->rem_tag : "");
    char to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
    if (bye_tag[0]) {
        snprintf(to, sizeof(to), "%s;tag=%s", l->to, bye_tag);
    } else {
        llog(LOG_WARN, "b2b", "send_bye_leg: leg=%d peer_tag&rem_tag EMPTY! to=%s",
             leg, l->to);
        snprintf(to, sizeof(to), "%s", l->to);
    }
    long cs = ++l->cseq;
    char branch[40];
    new_branch(branch, sizeof(branch));
    char out[SIP_MAX_MSG];
    int n = build_dlg_req(co, l, SIP_M_BYE, cs,
                          l->target[0] ? l->target : l->ruri, to, branch,
                          NULL, NULL, 0, out, sizeof(out));
    if (n <= 0) {
        llog(LOG_WARN, "b2b", "★BYE build_dlg_req FAIL leg=%d n=%d", leg, n);
        return;
    }

    /* ★ 从生成的 BYE 消息里提取实际 From/To tag，验证 build_dlg_req 是否正确组装 */
    char check_from_tag[DLG_TAG_MAX + 4] = "(no-from)";
    char check_to_tag[DLG_TAG_MAX + 4] = "(no-to)";
    const char *from_line = strstr(out, "From:");
    const char *to_line = strstr(out, "\r\nTo:");
    if (from_line) {
        const char *ft = strstr(from_line, "tag=");
        if (ft) {
            const char *ft_end = strpbrk(ft, "\r\n");
            size_t flen = ft_end ? (size_t)(ft_end - ft - 4) : strlen(ft + 4);
            if (flen > DLG_TAG_MAX) flen = DLG_TAG_MAX;
            memcpy(check_from_tag, ft + 4, flen);
            check_from_tag[flen] = '\0';
        }
    }
    if (to_line) {
        const char *tt = strstr(to_line, "tag=");
        if (tt) {
            const char *tt_end = strpbrk(tt, "\r\n");
            size_t tlen = tt_end ? (size_t)(tt_end - tt - 4) : strlen(tt + 4);
            if (tlen > DLG_TAG_MAX) tlen = DLG_TAG_MAX;
            memcpy(check_to_tag, tt + 4, tlen);
            check_to_tag[tlen] = '\0';
        }
    }

    llog(LOG_INFO, "b2b",
         "★BYE_SEND leg=%d cseq=%ld branch=%s from_tag=[%s] to_tag=[%s] "
         "dst=%s:%d reliable=%d",
         leg, cs, branch, check_from_tag, check_to_tag,
         co->cfg.local_host, co->cfg.local_udp_port, l->reliable);

    /* ★ 关键一致性检查：From tag 和 To tag 不应该相同！
     * 如果相同说明 build_dlg_req 里 tag 方向搞反了 → 对端 481 */
    if (strcmp(check_from_tag, check_to_tag) == 0) {
        llog(LOG_WARN, "b2b",
             "★BYE_TAG_WARN leg=%d: from_tag == to_tag == [%s] — "
             "这会导致对端 481 Call/Transaction Does Not Exist！",
             leg, check_from_tag);
    }

    /* 设本地发送端口：与初始 INVITE 一致，用 out_prof */
    {
        struct sip_profile *op = c->out_prof ? c->out_prof : c->prof;
        g_rx_local_port = (op && op->sip_udp_port)
                          ? op->sip_udp_port
                          : (uint16_t)co->cfg.local_udp_port;
    }

    sip_txn_t *t = txn_client_new(co->cfg.txn, TXN_NICT, branch,
                                  strlen(branch), SIP_M_BYE, &l->dst,
                                  l->reliable, out, (size_t)n, cli_tu, c,
                                  now_ms());
    if (t) {
        l->out_txn = t;
        l->state = LEG_TERMINATING;
        llog(LOG_INFO, "b2b", "★BYE_TXN_CREATED leg=%d txn=%p", leg, (void *)t);
    } else {
        llog(LOG_WARN, "b2b", "★BYE_TXN_NEW FAIL leg=%d txn_client_new returned NULL", leg);
    }
}

/* 从 SIP URI 提取 user 部分（纯号码）。
 * 支持格式：
 *   "<sip:user@host>;tag=xxx" → "user"
 *   "sip:user@host"           → "user"
 *   "<sip:user@host>"         → "user"
 *   "user@host"               → "user"
 *   "user"                    → "user"（原样返回）
 *   ""                        → ""
 * out 必须至少 cap 字节。 */
static void sip_uri_extract_user(char *out, size_t cap, const char *uri)
{
    if (!uri || !cap) { out[0] = '\0'; return; }
    const char *p = uri;
    /* 跳过前导 '<' */
    if (*p == '<') p++;
    /* 跳过 "sip:" 或 "tel:" scheme */
    if (!strncasecmp(p, "sip:", 4)) p += 4;
    else if (!strncasecmp(p, "tel:", 4)) p += 4;
    /* 找到 '@' 或第一个 ';'、'>'、':'、空格 */
    const char *at = NULL;
    const char *end = NULL;
    for (const char *q = p; *q; ++q) {
        if (*q == '@' && !at) at = q;
        if (*q == ';' || *q == '>' || *q == ':' || *q == ' ' || *q == '"') {
            end = q; break;
        }
    }
    if (!end) end = p + strlen(p);
    const char *start = p;
    if (at) start = p;           /* user 就是 '@' 之前 */
    /* 但如果没有 '@'（纯号码如 "1000"），start=p, end=end 就是整个串 */
    size_t n;
    if (at) {
        n = (size_t)(at - start);
    } else {
        n = (size_t)(end - start);
    }
    if (n >= cap) n = cap - 1;
    memcpy(out, start, n);
    out[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* REQ-PBX-CDR-001：话单事件 emit 辅助                                 */
/* ------------------------------------------------------------------ */
/* signaling.c 跨文件导出（同 g_ws_bridge_proto 先例，签名不入公共头）：
 * 从 answer SDP 提取 codec 名 + peer addr:port，b2b_emit_cdr END 填 ev 用。 */
extern int sig_sdp_extract_cdr(const char *sdp_buf, size_t sdp_len,
                               char *codec_out, size_t codec_sz,
                               char *peer_out,  size_t peer_sz);

/* 两腿 answer SDP → c->codec_a/b（peer 丢弃）。
 * ANSWER 阶段调用：SESSION_ANSWER 实时推送（读 c->codec_a/b）与
 * END ev 兜底都能拿到 codec，Web 端"接通"状态即刻显示编码。 */
static void call_fill_codecs(b2b_call_t *c)
{
    char peer[CDR_IP_MAX];
    if (c->leg[0].answer_len && !c->codec_a[0])
        sig_sdp_extract_cdr(c->leg[0].answer, c->leg[0].answer_len,
                            c->codec_a, sizeof(c->codec_a),
                            peer, sizeof(peer));
    if (c->leg[1].answer_len && !c->codec_b[0])
        sig_sdp_extract_cdr(c->leg[1].answer, c->leg[1].answer_len,
                            c->codec_b, sizeof(c->codec_b),
                            peer, sizeof(peer));
}

static void b2b_emit_cdr(b2b_call_t *c, cdr_phase_t phase)
{
    /* ---- 1. 实时事件推送（无条件：billing 关掉也要让订阅者看到呼叫状态）----
     * obs_event 内部同时写事件环（obs_events_tail 可拉取）+ 推订阅读端，
     * 两路径都带时间戳；body 附 call 侧的原始字段（src_ip / orig_number / trunk）。
     *
     * CDR END 已经在 signaling.c 的 hangup 统一出口 emit（那时候 b2b_call 里
     * 的 cause/disposition 才最终确认），所以此处只发 START / ANSWER。 */
    {
        const char *call_id = c->leg[0].callid;
        const char *from_ip = c->from_ip[0] ? c->from_ip : "";
        const char *trunk   = c->rt_name[0] ? c->rt_name : "";
        const char *orig    = c->orig_number[0] ? c->orig_number : "";
        /* leg[1].to 在 START 时还没赋值——fallback 到 A 腿原始 To */
        const char *to_uri  = c->leg[1].to[0] ? c->leg[1].to
                           : (c->leg[0].to[0] ? c->leg[0].to : "");
        /* Profile 上下文：A 腿入站 profile + B 腿出站 profile（out_prof=中继/域外） */
        const char *prof    = (c->prof && c->prof->name[0])    ? c->prof->name    : "-";
        const char *oprof   = (c->out_prof && c->out_prof->name[0]) ? c->out_prof->name : prof;
        const char *dp_tag  = (c->out_prof) ? "OUT" : "LOCAL";
        /* 呼叫方向：orig_caller_id_num 非空=originate（外呼），空=INVITE（入呼） */
        const char *dir_tag = (c->orig_caller_id_num[0] != '\0') ? "OUTBOUND" : "INBOUND";

        if (phase == CDR_PHASE_START) {
            obs_event(6, "CALL_INCOMING",
                "call_id=%s orig=%s to=%s src_ip=%s trunk=%s prof=%s oprof=%s dp=%s dir=%s",
                call_id, orig, to_uri, from_ip, trunk, prof, oprof, dp_tag, dir_tag);
        } else if (phase == CDR_PHASE_ANSWER) {
            obs_event(6, "CALL_ANSWERED",
                "call_id=%s orig=%s to=%s trunk=%s prof=%s oprof=%s dp=%s dir=%s",
                call_id, orig, to_uri, trunk, prof, oprof, dp_tag, dir_tag);
        } else if (phase == CDR_PHASE_END) {
            /* ★ term 精确区分挂断发起方：
             *   hangup_by=1 (A腿收到BYE) → term=A_UA 主叫/用户侧主动挂机
             *   hangup_by=2 (B腿收到BYE) → term=B_UA 被叫/对端侧主动挂机
             *   hangup_by=0 且 a_final=200 → term=LOCAL nbs本地收尾(双腿都收BYE)
             *   hangup_by=0 且 a_final!=200 → term=LOCAL nbs内部处理(拒接/超时/404) */
            const char *term;
            if (c->hangup_by == 1)      term = "A_UA";
            else if (c->hangup_by == 2) term = "B_UA";
            else                        term = "LOCAL";
            obs_event(c->a_final >= 400 && c->a_final < 500 ? 4 : 6,  /* 4xx client-error → WARN */
                "CALL_END",
                "call_id=%s orig=%s to=%s trunk=%s prof=%s oprof=%s dp=%s final=%d term=%s dir=%s",
                call_id, orig, to_uri, trunk, prof, oprof, dp_tag, c->a_final, term, dir_tag);
        }
    }

    /* ---- 2. 话单 emit（多槽链，billing/recording/event_handlers 都能收到）---- */
    if (g_cdr_hook_n == 0)
        return;
    cdr_event_t ev; memset(&ev, 0, sizeof(ev));
    snprintf(ev.call_id, sizeof(ev.call_id), "%s", c->leg[0].callid);
    if (c->leg[0].from[0]) {
        char user[128]; sip_uri_extract_user(user, sizeof(user), c->leg[0].from);
        snprintf(ev.from, sizeof(ev.from), "%s", user);
    }
    /* leg[1].to 在 CDR_PHASE_START 时尚未赋值（send_invite_b 在 emit 之后），
     * fallback 到 A 腿原始 To（已在 L1681 copy_strip_tag 赋值） */
    const char *to = c->leg[1].to[0] ? c->leg[1].to
                   : (c->leg[0].to[0] ? c->leg[0].to : "");
    if (to[0]) {
        char user[128]; sip_uri_extract_user(user, sizeof(user), to);
        snprintf(ev.to, sizeof(ev.to), "%s", user);
    }
    /* dir 判定与 SESSION_* 推送点同逻辑：originate 路设置 orig_caller_id_num，
     * 入向 INVITE 不会 → inbound；internal（分机互拨）细分后续需要 host 域判定再补 */
    ev.dir = c->orig_caller_id_num[0] ? CDR_DIR_OUTBOUND : CDR_DIR_INBOUND;
    ev.phase = phase;
    /* from_ip 全阶段填充：billing 的 START ev 是整体拷贝进聚合槽，
     * 从 START 起就带上源 IP（END 分支也会重填兜底）。 */
    if (c->from_ip[0])
        snprintf(ev.from_ip, sizeof(ev.from_ip), "%s", c->from_ip);
    /* uptime → wall 转换：nsbc_now_ms() 是单调时钟（CLOCK_MONOTONIC），
     * nsbc_wall_ms() 是墙上时间（CLOCK_REALTIME epoch ms）。
     * sanity：算出来 < 2000-01-01 epoch ms → 无效（进程重启/未赋值/溢出），
     * 兜底用当前 wall 时间。 */
    {
        uint64_t now_mon = nsbc_now_ms();
        uint64_t now_wall = nsbc_wall_ms();
        uint64_t s = c->start_ms ? c->start_ms : now_mon;
        uint64_t w = now_wall - (now_mon - s);
        ev.start_ms = (w < 946684800000ULL) ? now_wall : w; /* 2000-01-01 = 946684800000 */
    }
    if (phase == CDR_PHASE_ANSWER) {
        uint64_t now_mon = nsbc_now_ms();
        uint64_t now_wall = nsbc_wall_ms();
        uint64_t a = c->answer_ms ? c->answer_ms : now_mon;
        uint64_t start_wall = ev.start_ms; /* 已算好 */
        uint64_t w = now_wall - (now_mon - a);
        uint64_t ans_wall = (w < 946684800000ULL) ? now_wall : w;
        ev.answer_ms = ans_wall;
        ev.billsec = ans_wall >= start_wall
                     ? (uint32_t)((ans_wall - start_wall) / 1000) : 0;
        /* answer SDP 已到 → 就地填 c->codec_a/b：
         * 紧随其后的 SESSION_ANSWER UDS 推送直接读这两个字段，
         * Web 端"接通"实时状态即显示编码（不用等 END 落库）。 */
        call_fill_codecs(c);
    }
    if (phase == CDR_PHASE_END) {
        uint64_t now_mon = nsbc_now_ms();
        uint64_t now_wall = nsbc_wall_ms();
        uint64_t sm = c->start_ms ? c->start_ms : now_mon;
        uint64_t sw = now_wall - (now_mon - sm);
        ev.start_ms  = (sw < 946684800000ULL) ? now_wall : sw;
        if (c->answer_ms) {
            uint64_t aw = now_wall - (now_mon - c->answer_ms);
            ev.answer_ms = (aw < 946684800000ULL) ? now_wall : aw;
        }
        ev.end_ms    = now_wall;
        /* duration_s: START→END 总时长（含振铃/早期媒体），billsec 由 billing 模块内部重算 */
        uint64_t dur_ms = ev.end_ms >= ev.start_ms ? (ev.end_ms - ev.start_ms) : 0;
        ev.duration_s = (uint32_t)(dur_ms / 1000);
        /* disposition 映射：a_final 是 SIP 最终状态码 */
        switch (c->a_final) {
            case 200: ev.disposition = ev.answer_ms ? CDR_DISP_ANSWERED : CDR_DISP_NOANSWER; break;
            case 486: ev.disposition = CDR_DISP_BUSY;     break;
            case 480: ev.disposition = CDR_DISP_NOANSWER;  break;
            case 404:
            case 481: ev.disposition = CDR_DISP_FAILED;    break;
            default:  ev.disposition = CDR_DISP_FAILED;    break;
        }
        /* ---- security threat 标记（REQ-SEC-005 + REQ-SEC-006）----
         * 两层判定，任一命中即恶意（仅对有来源 IP 的 SIP INVITE 生效；
         * originate 外呼 / bridge 无 from_ip → 跳过，保持合法）：
         *   层 1：源 IP 命中 auto-ban / blacklist → is_malicious + threat_reason
         *   层 2：拨号规则全走完没人认领（dp_matched=0）→ No matching dialplan rule
         * 让正常通话和恶意攻击在 CDR 层分流 —— 前端可以按 is_malicious
         * Tab 切换（全部 / 正常 / 恶意），运维可以单独审计攻击流量。 */
        int malicious = 0;
        if (c->from_ip[0]) {
            char reason[48];
            if (security_get_threat(c->from_ip, reason, sizeof(reason))) {
                ev.is_malicious = 1;
                snprintf(ev.threat_reason, sizeof(ev.threat_reason),
                         "SecurityBan: %s", reason);
                malicious = 1;
            }
            if (!malicious && c->dp_matched == 0) {
                /* dp 全规则走完无人认领 → 扫号/未知来源 —— 即使 IP 没触发 ban
                 * 阈值（扫号器可能只刷 3-4 次就换 IP），也标记恶意 */
                ev.is_malicious = 1;
                snprintf(ev.threat_reason, sizeof(ev.threat_reason),
                         "No matching dialplan rule");
                malicious = 1;
            }
        }
        /* ---- 网络端点 + codec + 媒体质量 + 录音（REQ-CDR-002 补全）----
         * 旧版只有 signaling.c 的死路径填这些字段（ctx->cdr_emit 从未被赋值），
         * 导致 Web 端 Source IP / A-B Codec / Threat / 录音列永远是 '-'。
         * on_end（sig_on_end_cb）已在 call_destroy 里先于本函数执行，
         * VQ 指标与 rec_path 均已存进 c。 */
        if (c->from_ip[0])
            snprintf(ev.from_ip, sizeof(ev.from_ip), "%s", c->from_ip);
        if (c->leg[0].answer_len)
            sig_sdp_extract_cdr(c->leg[0].answer, c->leg[0].answer_len,
                                ev.codec_a, sizeof(ev.codec_a),
                                ev.a_peer_ip, sizeof(ev.a_peer_ip));
        if (c->leg[1].answer_len)
            sig_sdp_extract_cdr(c->leg[1].answer, c->leg[1].answer_len,
                                ev.codec_b, sizeof(ev.codec_b),
                                ev.b_peer_ip, sizeof(ev.b_peer_ip));
        /* c->codec_a/b 由 call_fill_codecs（ANSWER 阶段）填充，
         * SDP 直接解析失败时兜底 */
        if (!ev.codec_a[0] && c->codec_a[0])
            snprintf(ev.codec_a, sizeof(ev.codec_a), "%s", c->codec_a);
        if (!ev.codec_b[0] && c->codec_b[0])
            snprintf(ev.codec_b, sizeof(ev.codec_b), "%s", c->codec_b);
        ev.rtp_loss_pct = c->rtp_loss_pct;
        ev.rtp_jitter_ms = c->rtp_jitter_ms;
        ev.mos_x1000 = c->mos_x1000;
        if (c->rec_path[0])
            snprintf(ev.rec_path, sizeof(ev.rec_path), "%s", c->rec_path);
        if (c->rt_name[0])
            snprintf(ev.trunk, sizeof(ev.trunk), "%s", c->rt_name);
        ev.emergency = c->emergency;
    }
    llog(LOG_INFO, "b2b", "cdr emit phase=%s callid=%s from=%s to=%s",
         cdr_phase_name(phase), ev.call_id, ev.from, ev.to);

    /* ---- 白皮书 §5.2：双通道 CDR ----
     * 通道 A：同步多槽链（billing/recording 等业务模块自注册，走 g_cdr_hooks）
     * 通道 B：异步 Per-Core Ring（nsbc_event_push("CDR", ...)，ESL/WebSocket 客户端实时订阅）
     *
     * 设计原则：CDR 双通道与 SESSION_* 会话双通道同源互补——SESSION_CREATE/ANSWER/END
     * 是会话生命周期粗粒度事件，payload 偏路由信息；CDR 是精细计费事件，payload
     * 带完整 start_ms/answer_ms/billsec/disposition。两条都推，订阅者按需消费。
     *
     * §10.3 合规：nsbc_event_push 在 g_cdr_hooks 循环**之后**，不在任何 hook 回调内部发。
     * 就算某个 hook 回调内部误调 nsbc_event_push，由于 event_bus 内部是 TLS buffer
     * + Per-Core Ring，与 b2bua 的 mutex 互不影响，不会死锁（但仍违反 §10.3 设计约束）。*/

    /* 通道 B：异步 Ring — 构造完整 CDR JSON（含计费字段）并推 */
    {
        char cdr_json[768];
        int cn = snprintf(cdr_json, sizeof(cdr_json),
            "{\"call_id\":\"%s\",\"from\":\"%s\",\"to\":\"%s\",\"phase\":%d,"
            "\"start_ms\":%llu,\"answer_ms\":%llu,\"billsec\":%u,"
            "\"disposition\":%d,\"duration\":%u}",
            ev.call_id, ev.from, ev.to, (int)phase,
            (unsigned long long)ev.start_ms,
            (unsigned long long)ev.answer_ms,
            (unsigned)ev.billsec,
            (int)ev.disposition,
            (unsigned)ev.duration_s);
        if (cn > 0 && (size_t)cn < sizeof(cdr_json)) {
            nsbc_event_push("CDR", cdr_json);
        }
    }

    /* 通道 A：同步多槽链（billing_cdr_cb → cdr_sink_emit → persist_batch → WAL/DB）*/
    pthread_mutex_lock(&g_cdr_hook_lock);
    for (int i = 0; i < g_cdr_hook_n; ++i)
        g_cdr_hooks[i].fn(g_cdr_hooks[i].ud, &ev);
    pthread_mutex_unlock(&g_cdr_hook_lock);

    /* END：把完整 ev 序列化进 c->cdr_json——call_destroy 的 SESSION_END UDS
     * 推送直接复用（含 from_ip/codec/threat/VQ/rec_path 全字段），
     * 不再落进下方残缺 snprintf 降级路径。仅 END 存（START/ANSWER 的
     * UDS 推送点各自有专用 JSON）。 */
    if (phase == CDR_PHASE_END) {
        char *j = cdr_event_to_json(&ev);
        if (j) {
            snprintf(c->cdr_json, sizeof(c->cdr_json), "%s", j);
            free(j);
        }
    }
}

/* ---------- 安全黑名单接入 helpers ----------
 * sa_ip_copy : nsbc_sa_t → 点分 IP 字符串（NULL/未知族 → 空串）
 * msg_ua_copy: 顶层 User-Agent 头值拷贝（无头 → 空串）。原始 token
 *              指向报文缓冲且非 NUL 结尾，必须拷贝后才能用于日志/匹配 */
static void sa_ip_copy(const nsbc_sa_t *a, char *out, size_t outsz)
{
    if (!out || outsz == 0) return;
    out[0] = 0;
    if (!a) return;
    if (a->ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((const struct sockaddr_in *)&a->ss)->sin_addr,
                  out, outsz);
    } else if (a->ss.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)&a->ss)->sin6_addr,
                  out, outsz);
    }
}

static void msg_ua_copy(const sip_msg_t *m, char *out, size_t outsz)
{
    if (!out || outsz == 0) return;
    out[0] = 0;
    if (!m || outsz < 2) return;
    sip_hdr_t *h = sip_hdr_get(m, SIP_H_USER_AGENT);
    if (!h || !h->value.p || h->value.len == 0) return;
    size_t n = (size_t)h->value.len;
    if (n > outsz - 1) n = outsz - 1;
    memcpy(out, h->value.p, n);
    out[n] = 0;
}

/* 提取 From 头 URI 的 user part（主叫号码/分机）。
 * From 头原始 token 含 Display Name + URI（可含 <sip:user@host>）。
 * 无 From / URI 里无 user → out[0] = 0。 */
static void msg_caller_copy(const sip_msg_t *m, char *out, size_t outsz)
{
    if (!out || outsz < 2) return;
    out[0] = 0;
    if (!m) return;
    sip_hdr_t *h = sip_hdr_get(m, SIP_H_FROM);
    if (!h || !h->value.p || h->value.len == 0) return;
    /* 先找 URI 部分（可能在 <...> 里，或者裸 sip:...） */
    const char *uri_start = NULL;
    const char *uri_end = NULL;
    const char *p = h->value.p;
    size_t len = h->value.len;
    /* 找 '<' */
    const char *lt = memchr(p, '<', len);
    if (lt) {
        uri_start = lt + 1;
        const char *gt = memchr(uri_start, '>', p + len - uri_start);
        uri_end = gt ? gt : p + len;
    } else {
        uri_start = p;
        uri_end = p + len;
    }
    /* 跳过 sip: / tel: */
    const char *u = uri_start;
    while (u < uri_end && (*u == 's' || *u == 'S') && (uri_end - u >= 4)
           && strncasecmp(u, "sip:", 4) == 0) u += 4;
    while (u < uri_end && (*u == 't' || *u == 'T') && (uri_end - u >= 4)
           && strncasecmp(u, "tel:", 4) == 0) u += 4;
    /* 找 '@' 终止（user@host）；无 '@' 则直到 URI 末尾 */
    const char *at = memchr(u, '@', uri_end - u);
    const char *end = at ? at : uri_end;
    /* 去掉末尾 ';' 等参数 */
    while (end > u && (end[-1] == ';' || end[-1] == ' ')) --end;
    if (end > u) {
        size_t n = (size_t)(end - u);
        if (n > outsz - 1) n = outsz - 1;
        memcpy(out, u, n);
        out[n] = 0;
    }
}

/* 提取 Request-URI 的 user part（被叫号码/分机）。
 * 用 m->ruri.user（sip_uri_t 深解析后的 user token，含 .p/.len）。
 * 无 user → out[0] = 0。 */
static void msg_callee_copy(const sip_msg_t *m, char *out, size_t outsz)
{
    if (!out || outsz < 2) return;
    out[0] = 0;
    if (!m) return;
    if (m->ruri.user.p && m->ruri.user.len > 0) {
        const char *p = m->ruri.user.p;
        size_t len = m->ruri.user.len;
        size_t n = len;
        if (n > outsz - 1) n = outsz - 1;
        memcpy(out, p, n);
        out[n] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Phase 5 XL: 去掉 static，暴露为 public（signaling endpoint_registry
 * 分发路径会调它，ctx 构造好所有参数后内部委托） */
void handle_new_invite(b2b_core_t *co, sip_txn_t *t,
                              const sip_msg_t *m, sip_tr_proto_t proto,
                              const nsbc_sa_t *src, uint16_t local_port)
{
    /* Emergency URN 检测（RFC 6881 / ETSI TS 183 041）：
     * urn:service:sos / urn:service:sos.fire / urn:service:sos.medical 等
     * 绕过静态黑名单（auto-ban 仍生效——防攻击者冒充 emergency flood），
     * 同时绕过 HA standby（备机也接应急呼叫双保险）。 */
    int is_emergency = 0;
    if (m->ruri.scheme.p && m->ruri.scheme.len == 3
        && strncasecmp(m->ruri.scheme.p, "urn", 3) == 0
        && m->ruri.user.p && m->ruri.user.len >= 12
        && !strncmp(m->ruri.user.p, "service:sos", 11)) {
        is_emergency = 1;
        llog(LOG_INFO, "b2b", "Emergency INVITE: ruri=%.*s@%.*s",
             (int)m->ruri.user.len, m->ruri.user.p ? m->ruri.user.p : "",
             (int)m->ruri.host.len, m->ruri.host.p ? m->ruri.host.p : "");
    }

    /* 安全门（最前置）：被动态封禁的 IP（REGISTER 暴力失败超阈值）的
     * 新 INVITE 静默丢弃——不回任何响应，防 SBC 指纹暴露。
     * 静态黑名单（IP CIDR / UA，security 模块）次之。优先级：
     * 动态 auto-ban > 静态黑名单 > HA 门控 > 拨号规则。 */
    {
        char gip[INET6_ADDRSTRLEN] = {0};
        sa_ip_copy(src, gip, sizeof(gip));
        if (gip[0] && b2b_guard_check(gip, B2B_GUARD_INVITE) == NSBC_PERM_DENIED)
            return;

        /* NIP 完整围栏：method + IP + UA + 主叫 + 被叫
         * 优先级：method 禁用 → deny 全表 → allowlist 未命中
         * 命中一律静默丢弃（不回任何 SIP 响应，防 SBC 指纹暴露）。 */
        if (!is_emergency) {
            char f_ua[256] = {0}, f_caller[128] = {0}, f_callee[128] = {0};
            msg_ua_copy(m, f_ua, sizeof(f_ua));
            msg_caller_copy(m, f_caller, sizeof(f_caller));
            msg_callee_copy(m, f_callee, sizeof(f_callee));
            security_fence_ctx_t fctx = {0};
            fctx.src = (const struct sockaddr *)src;
            fctx.method = SIP_M_INVITE;
            fctx.ua = f_ua;
            fctx.caller = f_caller;
            fctx.callee = f_callee;
            security_check_fence(&fctx);
            if (fctx.hit != BLACKLIST_OK) {
                security_fence_log_drop(fctx.hit, gip, f_ua, "INVITE",
                                        f_caller, f_callee);
                return;
            }
        }
    }

    /* HA 门控（REQ-HA-001）：备机不接新呼叫，静默丢弃（与 CIDR 拒绝
     * 同一惯例，不回响应防 SBC 指纹暴露）。VIP 已漂移时正常流量不会
     * 到达备机；keepalived 收敛窗口内的漏网直连流量在此兜底。
     * 例外：Emergency URN:service:sos —— 备机也接应急呼叫（双保险）。
     * 仅拦新对话：in-dialog BYE/re-INVITE 走上面的分支不受影响。 */
    if (ha_role() != HA_ROLE_ACTIVE && !is_emergency) {
        /* 限频 WARN：STANDBY 丢呼叫若只打 DEBUG（默认级别不可见），
         * 主备仲裁异常时表现为"呼叫静默消失 + CDR 停止"且无日志可查。 */
        static unsigned drop_invite_n = 0;
        if (drop_invite_n++ % 50 == 0)
            llog(LOG_WARN, "b2b",
                 "HA standby: drop new INVITE (dropped=%u)", drop_invite_n);
        return;
    }

    /* REQ-ARCH-015 并发硬限制：try_inc 超限返回 0 → 503 Busy */
    if (!concurrent_try_inc(co)) {
        llog(LOG_WARN, "b2b", "concurrent_call_limit reached (%u), 503",
             co->cfg.max_concurrent_calls);
        respond_txn(co, t, 503, NULL, "Retry-After: 10\r\n", NULL, 0);
        return;
    }

    b2b_call_t *c = mem_slab_alloc_direct(co->slab);
    if (!c) {
        /* slab 耗尽：撤回并发计数 */
        concurrent_dec(co);
        respond_txn(co, t, 503, NULL, "Retry-After: 10\r\n", NULL, 0);
        return;
    }
    memset(c, 0, sizeof(*c));
    c->core = co;
    /* Emergency URN 标记进 call 对象：b2b_emit_cdr 写进 CDR emergency 位 */
    c->emergency = (uint8_t)is_emergency;

    /* SIP Profile 归属（REQ-SIP-PROFILE-001）：
     * 先按入端口匹配（多端口场景最高效——从 5080 进来的包直接归属 external），
     * 端口没匹配或为 0 再 fallback 到源 IP 白名单匹配。
     * active_calls +1（call_destroy 对称 -1；b2b_originate 外呼无源 IP 不归属）。
     * 指针存 call 生命周期安全（数组不可变+墓地）。 */
    {
        char ip[INET6_ADDRSTRLEN];
        sa_ip_str(src, ip, sizeof(ip));
        c->prof = NULL;
        if (local_port > 0) {
            c->prof = sip_profile_match_port(
                (proto == SIP_TR_UDP) ? local_port : 0,
                (proto != SIP_TR_UDP) ? local_port : 0);
            if (c->prof)
                llog(LOG_DEBUG, "b2b", "call profile by port %u → %s",
                     local_port, c->prof->name);
        }
        if (!c->prof) {
            c->prof = sip_profile_match_src(ip, NULL);
            if (c->prof)
                llog(LOG_DEBUG, "b2b", "call profile by ip → %s", c->prof->name);
        }
        /* ★ fallback：WS 呼入（local_port=0、src=:0）两个匹配全挂时，
         *  取 enabled 的 profile 中 name 字典序最大的（internal > external，
         *  确保命中 internal——拨号规则默认归属它）。
         *  必须确定归属，否则 dp lookup 因 profile 对不上全部 skip → 404。 */
        if (!c->prof) {
            unsigned n = sip_profile_count();
            sip_profile_t *best = NULL;
            for (unsigned i = 0; i < n; ++i) {
                sip_profile_t *p = sip_profile_at(i);
                if (p && p->enabled) {
                    if (!best || strcmp(p->name, best->name) > 0)
                        best = p;
                }
            }
            c->prof = best;
            if (c->prof)
                llog(LOG_INFO, "b2b", "call profile fallback → %s", c->prof->name);
            else
                llog(LOG_WARN, "b2b", "call profile: NO enabled profile found!");
        }
        if (c->prof)
            __sync_fetch_and_add(&c->prof->active_calls, 1u);

        /* === P2-14：呼入 trunk 四层匹配（文档 §4.2.1） ===
         *
         * 层 1（register 反向关联）：To/PAI → outreg auth_user（仅 state==OR_REGISTERED）
         * 层 2（auth_out CIDR 白名单）：src_ip ∈ trunk.peer_ip
         * 层 3（static_peer host IP）：src_ip == trunk.host
         * 层 4（回退）：任意 trunk host IP，多 trunk 取 priority 最小
         *
         * 匹配结果存 c->rt_name —— 供后续 P2-15 inbound_extension 使用。
         * 全部未命中 → c->rt_name 保持空（走原有 DP → EXT 层正常分机路由）。 */
        char src_ip_str[INET6_ADDRSTRLEN];
        sa_ip_str(src, src_ip_str, sizeof(src_ip_str));
        snprintf(c->from_ip, sizeof(c->from_ip), "%s", src_ip_str);
        char matched_name[RT_NAME_MAX] = {0};

        /* 1. 层 1：register 反向关联——先试 To user，再试 PAI user */
        if (b2b_rt(co)) {
            const char *auth_user = NULL;
            /* To header user */
            sip_hdr_t *to_h = sip_hdr_get(m, SIP_H_TO);
            if (to_h && sip_hdr_parse_addr(to_h) == NSBC_OK
                && to_h->u.addr.uri.user.p && to_h->u.addr.uri.user.len > 0) {
                char tmp[64]; copy_tok(tmp, sizeof(tmp), to_h->u.addr.uri.user);
                if (outreg_find_by_auth_user(tmp, matched_name, sizeof(matched_name))) {
                    auth_user = tmp;
                }
            }
            /* To 没命中 → P-Accessed-Identity */
            if (!matched_name[0]) {
                sip_hdr_t *pai = sip_hdr_get(m, SIP_H_P_ASSERTED_IDENTITY);
                if (pai && sip_hdr_parse_addr(pai) == NSBC_OK
                    && pai->u.addr.uri.user.p && pai->u.addr.uri.user.len > 0) {
                    char tmp[64]; copy_tok(tmp, sizeof(tmp), pai->u.addr.uri.user);
                    outreg_find_by_auth_user(tmp, matched_name, sizeof(matched_name));
                }
            }

            /* 2. 层 2/3/4：IP 匹配（src_ip 为空跳过——WS 桥 AF_UNSPEC） */
            if (!matched_name[0] && src_ip_str[0]) {
                const trunk_t *tr = rt_match_inbound_trunk((route_table_t *)b2b_rt(co), src_ip_str);
                if (tr) snprintf(matched_name, sizeof(matched_name), "%s", tr->name);
            }

            if (matched_name[0]) {
                snprintf(c->rt_name, sizeof(c->rt_name), "%s", matched_name);
                llog(LOG_INFO, "b2b", "P2-14 inbound INVITE trunk matched: name=%s src_ip=%s auth_user=%s callid=%s",
                     matched_name, src_ip_str, auth_user ? auth_user : "-", c->leg[0].callid);
            } else {
                /* 未命中 trunk：正常场景（内网分机对打走 EXT 层），只打 DEBUG */
                log_debug("b2b", "P2-14 inbound INVITE no trunk match: src_ip=%s callid=%s",
                          src_ip_str, c->leg[0].callid);
            }
        }
    }

    b2b_leg_t *a = &c->leg[0];
    a->state = LEG_CALLING;
    a->proto = proto;
    a->reliable = proto != SIP_TR_UDP;
    a->dst = *src;
    a->inv_txn = t;

    sip_hdr_t *cid = sip_hdr_get(m, SIP_H_CALL_ID);
    sip_hdr_t *from = sip_hdr_get(m, SIP_H_FROM);
    sip_hdr_t *to = sip_hdr_get(m, SIP_H_TO);
    /* REQ-SIP-DIV-001：Privacy: history → 后续 History-Info/Diversion 截断（内部路由轨迹不外泄）
     * REQ-PBX-DP-003：Privacy: id → B 腿 From anonymize + 加 Privacy: id 头 */
    sip_hdr_t *priv = sip_hdr_get(m, SIP_H_PRIVACY);
    if (priv) {
        if (sip_hdr_has_option(priv, "history")) c->privacy_history = 1;
        if (sip_hdr_has_option(priv, "id"))      c->privacy_id = 1;
    }
    if (!cid || !from || !to) {
        llog(LOG_WARN, "b2b", "INVITE rejected 400: missing Call-ID/From/To hdr");
        respond_txn(co, t, 400, NULL, NULL, NULL, 0);
        mem_obj_release(c);
        return;
    }
    copy_tok(a->callid, sizeof(a->callid), cid->value);
    copy_tok(a->from, sizeof(a->from), from->value);
    copy_strip_tag(a->to, sizeof(a->to), to->value);
    /* REQ-SIP-DIALOG-TAG-001：A 腿 (UAS) 的 dialog remote tag = INVITE From 头的 tag（主叫 1000 的 tag）。
     * 之前漏设 → 发给 A 腿的 BYE/INFO 的 To 头 ;tag= 为空 → SIP.js Parser 崩整条消息！ */
    tok_from_tag(m, a->rem_tag, DLG_TAG_MAX);
    /* peer_tag = INVITE To 头 tag（主叫发 INVITE 时 To 头通常无 tag，
     * 所以这里大概率是空。但如果主叫发的是 re-INVITE（dialog 内请求），To 头会带 tag，
     * 此时需要 peer_tag 来构造 ACK 的 To 头。空也没关系 — send_ack_leg 会 fallback。 */
    tok_to_tag(m, a->peer_tag, DLG_TAG_MAX);
    copy_tok(a->ruri, sizeof(a->ruri), m->ruri_raw);

    /* ★ REQ-OBS-010 呼叫表显示：从 From header 提取主叫 user
     * （之前 signaling.c resolve_cb 错把 R-URI user = 被叫 塞进 orig_number
     *  → 呼叫表显示 "1001 / 1001"，两个都是被叫） */
    tok_from_user(m, c->orig_number, sizeof(c->orig_number));
    llog(LOG_INFO, "b2b", "new_invite: from_user=%s ruri_user=?",
         c->orig_number);
    sip_hdr_t *cseq = sip_hdr_get(m, SIP_H_CSEQ);
    if (cseq && sip_hdr_parse_cseq(cseq) == NSBC_OK)
        a->cseq = cseq->u.cseq.number;
    sip_hdr_t *contact = sip_hdr_get(m, SIP_H_CONTACT);
    if (contact && sip_hdr_parse_addr(contact) == NSBC_OK)
        copy_tok(a->target, sizeof(a->target), contact->value);
    if (m->body.len) {
        if (m->body.len > B2B_SDP_MAX) {
            llog(LOG_WARN, "b2b",
                 "INVITE rejected 488: sdp too large %zuB > %zuB (callid=%.*s)",
                 m->body.len, (size_t)B2B_SDP_MAX,
                 (int)(cid->value.len < 127 ? cid->value.len : 127),
                 cid->value.p);
            respond_txn(co, t, 488, NULL, NULL, NULL, 0);
            mem_obj_release(c);
            return;
        }
        memcpy(a->offer, m->body.p, m->body.len);
        a->offer_len = m->body.len;
    }
    sip_upd_guard_init(&a->guard);
    sip_upd_guard_init(&c->leg[1].guard);

    /* Session Timer (RFC 4028) */
    /* extract 必须无条件执行（P0 透传 + P1 完整都需要原始头值） */
    extract_sess_timer_hdrs(m, a->session_expires, sizeof(a->session_expires),
                             a->min_se, sizeof(a->min_se), &a->timer_supported);
    if (co->cfg.sess_timer_enabled) {
        sess_timer_init(&a->st);
        sess_timer_init(&c->leg[1].st);
        tw_node_init(&a->refresh_node);
        tw_node_init(&a->expire_node);
        tw_node_init(&c->leg[1].refresh_node);
        tw_node_init(&c->leg[1].expire_node);
        /* 协商 + 422 */
        if (a->timer_supported && a->session_expires[0]) {
            sess_timer_nego_t nin, nout;
            nin.interval_s = atoi(a->session_expires);
            nin.refresher = SIP_REF_NONE;
            /* 解析原始值里的 refresher 参数，尊重请求方意图 */
            {
                const char *rp = strstr(a->session_expires, "refresher=");
                if (rp) { rp += 10; if (!strncmp(rp, "uac", 3)) nin.refresher = SIP_REF_UAC;
                    else if (!strncmp(rp, "uas", 3)) nin.refresher = SIP_REF_UAS; }
            }
            if (nin.interval_s > 0) {
                if (nin.interval_s < co->cfg.sess_timer_cfg.min_se_s) {
                    char msehdr[64]; snprintf(msehdr, sizeof(msehdr), "Min-SE: %d\r\n", co->cfg.sess_timer_cfg.min_se_s);
                    respond_txn(co, t, 422, NULL, msehdr, NULL, 0);
                    mem_obj_release(c);
                    return;
                }
                sess_timer_uas_negotiate(&co->cfg.sess_timer_cfg, &nin, &nout);
                snprintf(a->session_expires, sizeof(a->session_expires), "Session-Expires: %d;refresher=%s\r\n",
                         nout.interval_s, nout.refresher == SIP_REF_UAC ? "uac" : "uas");
            }
        }
    }

    /* UAS 对话骨架（生成本地 tag） */
    a->dlg = dlg_uas_new(co->cfg.dlg, m);
    if (a->dlg) {
        snprintf(a->loc_tag, sizeof(a->loc_tag), "%s", a->dlg->local_tag);
        a->dlg->ud = c;
    }

    txn_set_ud(t, c);
    txn_set_tu(t, b2b_server_tu); /* Timer H（2xx 无 ACK）上送 */
    tbl_link(co, c, 0);
    add_atomic(&co->total, 1);

    /* 白皮书 §5.2 + §10.3：call 正式激活后统一发 SESSION_CREATE 事件。
     * 双通道：event_handler_trigger（同步订阅链）+ nsbc_event_push（异步 Ring）。
     * security 的 SESSION_CREATE handler 可以在这里做 IP 信誉检查。*/
    {
        char create_json[512];
        snprintf(create_json, sizeof(create_json),
            "{\"call_id\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
            "\"ruri\":\"%s\",\"profile\":\"%s\",\"proto\":\"%d\"}",
            c->leg[0].callid, c->leg[0].from, c->leg[0].ruri,
            a->ruri, c->prof ? c->prof->name : "unknown", (int)SIP_TR_UDP);
        uint64_t seq = 0;
        event_handler_trigger("SESSION_CREATE", create_json, &c->leg[0].callid, &seq);
        nsbc_event_push("SESSION_CREATE", create_json);
    }

    /* REQ-PBX-CDR-001：START emit — 入呼 INVITE 到达即记账 */
    c->start_ms = now_ms();
    b2b_emit_cdr(c, CDR_PHASE_START);

    /* 100 Trying 立即发（本地，不透传 B 的 100） */
    respond_txn(co, t, 100, NULL, NULL, NULL, 0);
    llog(LOG_INFO, "b2b", "new_invite: 100 Trying sent, a.ruri=%s", a->ruri);

    /* === P2-15：inbound_extension 完整生效（文档 §4.2.2） ===
     *
     * 前置条件：P2-14 trunk 匹配成功（c->rt_name[0] != '\0'）
     *          + trunk_t.inbound_extension 非空
     * 改 3 件事：
     *   [1] Request-URI user 替换为 inbound_extension → a->ruri 改写
     *   [2] CDR 双记录存 c->original_called / c->trunk_called
     *   [3] To header 同步改写（重建 sip:new_user@host 不带 tag，tag 由 dialog 后续管理）
     * 未注册分机 → 走原有 resolve 流程 → resolve_rc != NSBC_OK → 404 Not Found
     * （不需要额外处理，resolve_cb 已经做了） */
    if (c->rt_name[0] && b2b_rt(co)) {
        const trunk_t *tr = rt_trunk_get((route_table_t *)b2b_rt(co), c->rt_name);
        if (tr && tr->inbound_extension[0]) {
            sip_tok_t ruri_tok;
            ruri_tok.p = a->ruri;
            ruri_tok.len = strlen(a->ruri);
            sip_uri_t ruri_u;
            if (sip_parse_uri(&ruri_tok, &ruri_u) == NSBC_OK && ruri_u.host.p) {
                /* [1] 存原始 RURI user → CDR */
                if (ruri_u.user.p && ruri_u.user.len > 0) {
                    copy_tok(c->original_called, sizeof(c->original_called), ruri_u.user);
                }
                snprintf(c->trunk_called, sizeof(c->trunk_called), "%s",
                         tr->inbound_extension);

                /* [2] 重建 a->ruri：sip:inbound_extension@host(:port) */
                char new_ruri[B2B_HDR_MAX];
                uint16_t port = (ruri_u.port.p && ruri_u.port.len > 0)
                                ? (uint16_t)strtoul(ruri_u.port.p, NULL, 10) : 0;
                if (port > 0) {
                    snprintf(new_ruri, sizeof(new_ruri), "sip:%s@%s:%u",
                             tr->inbound_extension, ruri_u.host.p, port);
                } else {
                    snprintf(new_ruri, sizeof(new_ruri), "sip:%s@%s",
                             tr->inbound_extension, ruri_u.host.p);
                }
                snprintf(a->ruri, sizeof(a->ruri), "%s", new_ruri);

                /* [3] a->to 同步改写（To header 是"远端分机"，user part 必须匹配改后 DID） */
                sip_hdr_t *to_h = sip_hdr_get(m, SIP_H_TO);
                if (to_h && sip_hdr_parse_addr(to_h) == NSBC_OK) {
                    char new_to[B2B_HDR_MAX];
                    const sip_uri_t *turi = &to_h->u.addr.uri;
                    uint16_t tport = (turi->port.p && turi->port.len > 0)
                                     ? (uint16_t)strtoul(turi->port.p, NULL, 10) : 0;
                    const char *thost = turi->host.p ? turi->host.p : "";
                    if (tport > 0) {
                        snprintf(new_to, sizeof(new_to), "<sip:%s@%s:%u>",
                                 tr->inbound_extension, thost, tport);
                    } else {
                        snprintf(new_to, sizeof(new_to), "<sip:%s@%s>",
                                 tr->inbound_extension, thost);
                    }
                    snprintf(a->to, sizeof(a->to), "%s", new_to);
                }

                llog(LOG_INFO, "b2b",
                     "P2-15 inbound_extension rewrite: trunk=%s orig_ruri_user=%s → new=%s a.ruri=%s a.to=%s callid=%s",
                     tr->name, c->original_called, tr->inbound_extension,
                     a->ruri, a->to, c->leg[0].callid);
            } else {
                log_warn("b2b", "P2-15 inbound_extension but a->ruri parse failed ruri=%s", a->ruri);
            }
        }
    }

    /* 号码解析 → B 腿目的 */
    nsbc_sa_t bd;
    char nruri[B2B_HDR_MAX];
    char bto[B2B_HDR_MAX];
    nsbc_sa_clear(&bd);
    int resolve_rc = co->cfg.resolve
        ? co->cfg.resolve(c, a->ruri, strlen(a->ruri), &bd, nruri,
                          sizeof(nruri), bto, sizeof(bto),
                          co->cfg.resolve_ud)
        : NSBC_NOTSUP;
    if (resolve_rc == NSBC_OK) {
        llog(LOG_INFO, "b2b", "new_invite resolve ok: callid=%s b_ruri=%s",
             a->callid, nruri);
    } else {
        /* 失败的具体原因（unregistered / contact unreachable / no user
         * part ...）由 signaling.c resolve_cb 各分支打 WARN，此处只记
         * R-URI 与返回码，不再打印误导性的布尔值 */
        llog(LOG_WARN, "b2b",
             "new_invite resolve failed: callid=%s ruri=%s rc=%d → 404",
             a->callid, a->ruri, resolve_rc);
    }
    /* §15 安全加固：DP 层 NSBC_PERM_DENIED → silent drop（只记日志不回包）
     * 恶意扫描者收到 404 就知道"这里有 SBC 但号码不存在"，silent drop 让对方超时放弃
     * NSBC_ERR / NSBC_NOTSUP 等继续走原有 404（合法的"号码不存在"） */
    if (resolve_rc == NSBC_PERM_DENIED) {
        llog(LOG_NOTICE, "b2b",
             "new_invite dp PERM_DENIED callid=%s → silent drop (no response)",
             a->callid);
        a->state = LEG_TERMINATED;
        call_destroy(c);
        return;
    }
    if (resolve_rc != NSBC_OK) {
        c->a_final = 404;
        respond_txn(co, t, 404, NULL, NULL, NULL, 0);
        a->state = LEG_TERMINATED;
        add_atomic(&co->failed, 1);
        call_destroy(c);
        return;
    }

    /* REQ-ARCH-015 per-key 并发硬限制：b_key 由 resolve_cb（signaling.c）预填
     * EXT 层 → AoR，OUT 层 → trunk_name；空 b_key 不限制。
     * 超限返回 486 Busy Here（SIP 标准，语义比 503 更准确）。 */
    if (c->b_key[0] && !perkey_try_inc(co, c->b_key)) {
        llog(LOG_WARN, "b2b", "perkey_concurrent_limit reached (%u) for %s, 486",
             co->cfg.max_inflight_per_aor, c->b_key);
        c->a_final = 486;
        respond_txn(co, t, 486, NULL, NULL, NULL, 0);
        a->state = LEG_TERMINATED;
        add_atomic(&co->failed, 1);
        call_destroy(c);
        return;
    }
    if (c->b_key[0]) c->key_held = 1; /* AoR/trunk key 已占槽，call_destroy 时释放 */

    /* 构造 B 腿 */
    b2b_leg_t *b = &c->leg[1];
    b->state = LEG_CALLING;
    /* proto 按 resolve 结果推断（tr_send_cb 同款约定）：WS/WSS 注册的
     * dst 是 AF_UNSPEC（padding 藏 ws_conn*），必须用 WS/WSS proto —
     * 否则 send_ack_leg/tr_send_leg 直调 sip_tr_send 会走 UDP 分支，
     * 无法寻址 .invalid Contact 的 WS 连接 */
    if (bd.ss.ss_family == AF_UNSPEC) {
        extern sip_tr_proto_t g_ws_bridge_proto; /* signaling.c */
        b->proto = g_ws_bridge_proto;
        b->reliable = 1;   /* WS 可靠传输 */
    } else {
        b->proto = SIP_TR_UDP;
        b->reliable = 0;
    }
    b->dst = bd;
    if (nsbc_sa_port(&bd) == 0)
        nsbc_sa_set_port(&b->dst, 5060);
    nsbc_rand_token(b->loc_tag, sizeof(b->loc_tag), 16);

    /* ==============================================================
     * REQ-DP-SIP-001：统一 B-leg INVITE 头构建
     *
     * 所有 DP 命中的 SIP 头 override 都在 c->hdr_rw 里，统一在此处合成最终值。
     * 优先级（从高到低）：
     *   privacy_id → sip_invite_from → sip_invite_from_uri → (sip_from_uri 可选 + sip_from_display) → sip_from_uri
     *   → origination_caller_id_number + origination_caller_id_name（FreeSWITCH 风格，纯 user+name，host 自动）
     *   → trunk 默认构造（P2-16，trunk resolve_from_user/host 回退链 + caller_id_in_from）
     *   → clip(legacy CLIP，已预填 sip_from_uri) → 透传 A-腿 From
     *   To / R-URI / Contact 各自独立覆盖 + params 追加
     *   sip_invite_domain 强制覆盖所有 URI 的 host:port（最后一步）
     *   sip_invite_pid_params + sip_invite_params → 拼入 extra 头块
     * ============================================================== */
    const sip_hdr_rewrite_t *rw = &c->hdr_rw;

    /* --- 1. From 头构建 --- */
    char from_base[B2B_HDR_MAX];
    int from_has_tag = 0;
    if (c->privacy_id) {
        snprintf(from_base, sizeof(from_base), "Anonymous");
    } else if (rw->sip_invite_from[0]) {
        /* 完整 From 值覆盖（可能自带 tag） */
        snprintf(from_base, sizeof(from_base), "%s", rw->sip_invite_from);
        from_has_tag = !!strstr(from_base, ";tag=");
    } else if (rw->sip_invite_from_uri[0]) {
        /* 纯 URI 覆盖（FS sip_invite_from_uri）→ 叠加 display name（若有） */
        if (rw->sip_from_display[0])
            snprintf(from_base, sizeof(from_base), "\"%s\" <%s>",
                     rw->sip_from_display, rw->sip_invite_from_uri);
        else
            snprintf(from_base, sizeof(from_base), "<%s>", rw->sip_invite_from_uri);
    } else if (rw->sip_from_uri[0]) {
        /* 组件级覆盖 → 叠加 display name（若有）；
         * REQ-PBX-CLIP-001：legacy clip 已由 DP 预填到 rw->sip_from_uri */
        if (rw->sip_from_display[0])
            snprintf(from_base, sizeof(from_base), "\"%s\" <%s>",
                     rw->sip_from_display, rw->sip_from_uri);
        else
            snprintf(from_base, sizeof(from_base), "<%s>", rw->sip_from_uri);
    /* === FreeSWITCH 层 4.5：origination_caller_id_number + origination_caller_id_name ===
     * 纯 user part（host 自动填 trunk.resolve_from_host 或 local_host），
     * 比 sip_from_uri（完整 URI）更灵活——DP 只设 user，host 由系统/运营商域保证。
     * 优先级：低于 sip_invite_from/_uri/sip_from_uri，高于 trunk 默认构造 */
    } else if (c->orig_caller_id_num[0]) {
        const char *u = c->orig_caller_id_num;
        const char *h = co->cfg.local_host;
        if (c->rt_name[0] && b2b_rt(co)) {
            const trunk_t *tr = rt_trunk_get((route_table_t *)b2b_rt(co), c->rt_name);
            if (tr && tr->enabled) h = p216_resolve_from_host(tr);
        }
        const char *disp = c->orig_caller_id_name[0] ? c->orig_caller_id_name : u;
        snprintf(from_base, sizeof(from_base), "\"%s\" <sip:%s@%s>", disp, u, h);
        llog(LOG_INFO, "b2b",
             "origination_caller_id: num=%s name=%s host=%s disp=%s",
             u, c->orig_caller_id_name, h, disp);
    /* === P2-16：层 5 trunk 默认构造（文档 §4.1.1 + §4.1.3） ===
     * 优先级：低于 DP 显式 override（sip_invite_from/_uri/sip_from_uri[含clip]），
     *        高于纯透传 A-腿 From。
     * 前置条件：c->rt_name 已填（呼入 P2-14 或呼出 OUT resolve）
     *        + trunk 存在且 enabled 且 kind != TRUNK_EXT
     * 效果：From: "<caller>" <sip:<from_user>@<from_host>>;tag=...
     * 其中 from_user / from_host 走完整回退链（p216_resolve_from_user/host）
     * caller_id_in_from=1 → URI user + Display name 都用 c->orig_number（主叫号码）
     * host 始终用 resolve_from_host(tr) 保留运营商域（专家 2.4 明确）
     * trunk 不可用 → 继续下一行 fallback 透传 A-腿 From */
    } else if (c->rt_name[0] && b2b_rt(co)) {
        const trunk_t *tr = rt_trunk_get((route_table_t *)b2b_rt(co), c->rt_name);
        if (tr && tr->enabled) {
            const char *u = p216_resolve_from_user(tr, tr->caller_id_in_from, c->orig_number);
            const char *h = p216_resolve_from_host(tr);
            /* 专家 2.4：caller_id_in_from=1 时 Display name 也用主叫号码（FS 行为） */
            const char *disp = tr->caller_id_in_from ? c->orig_number : u;
            snprintf(from_base, sizeof(from_base), "\"%s\" <sip:%s@%s>",
                     disp, u, h);
            llog(LOG_INFO, "b2b",
                 "P2-16 trunk From default: trunk=%s user=%s host=%s caller_id_in_from=%d disp=%s",
                 tr->name, u, h, tr->caller_id_in_from, disp);
        } else {
            copy_strip_tag(from_base, sizeof(from_base), from->value);
        }
    } else {
        copy_strip_tag(from_base, sizeof(from_base), from->value);
    }
    /* From params：在 ;tag= 之前插入 */
    {
        if (from_has_tag) {
            /* 已有 tag，拆 body 和 tag，params 加在 body 后 */
            char tag_bak[64] = {0};
            char *p = strstr(from_base, ";tag=");
            if (p) {
                strncpy(tag_bak, p + 5, sizeof(tag_bak) - 1);
                *p = '\0';
            }
            if (rw->sip_invite_from_params[0])
                snprintf(b->from, sizeof(b->from), "%s;%s;tag=%s",
                         from_base, rw->sip_invite_from_params, tag_bak);
            else
                snprintf(b->from, sizeof(b->from), "%s;tag=%s", from_base, tag_bak);
        } else {
            if (rw->sip_invite_from_params[0])
                snprintf(b->from, sizeof(b->from), "%s;%s;tag=%s",
                         from_base, rw->sip_invite_from_params, b->loc_tag);
            else
                snprintf(b->from, sizeof(b->from), "%s;tag=%s", from_base, b->loc_tag);
        }
    }

    /* === FreeSWITCH effective_caller_id_*：运行时合成值 ===
     * 优先级链全部跑完后，从 from_base 提取最终 user part + display name
     * 供 CDR emit 记录"真正生效的主叫号/名"（不是 DP 原始配置值）
     *
     * 提取规则：
     *   from_base = "\"Display\" <sip:user@host>"  → effective_num=user, effective_name=Display
     *   from_base = "<sip:user@host>"              → effective_num=user, effective_name=""
     *   from_base = "Anonymous"                   → effective_num="Anonymous", effective_name=""
     * effective 值与 DP 配置值（orig_caller_id_*）独立：
     *   - orig_caller_id_* 是"我想要"
     *   - effective_caller_id_* 是"最终拿到"
     *   - 两者不同意味着优先级链有更高层覆盖了（sip_invite_from/_uri/sip_from_uri/privacy_id） */
    {
        /* 先找 Display name（尖括号之前的 "xxx" 部分） */
        const char *disp_start = NULL;
        const char *disp_end = NULL;
        const char *angle = strchr(from_base, '<');
        if (angle) {
            /* "Display" <sip:...> 模式 */
            disp_start = strchr(from_base, '"');
            if (disp_start && disp_start < angle) {
                disp_end = strchr(disp_start + 1, '"');
                if (disp_end && disp_end < angle) {
                    size_t dlen = (size_t)(disp_end - disp_start - 1);
                    if (dlen >= sizeof(c->effective_caller_id_name))
                        dlen = sizeof(c->effective_caller_id_name) - 1;
                    memcpy(c->effective_caller_id_name, disp_start + 1, dlen);
                    c->effective_caller_id_name[dlen] = '\0';
                }
            }
            /* 提取 <sip:user@host> 中的 user part */
            const char *sip = strstr(angle, "sip:");
            if (sip) {
                const char *user_start = sip + 4;
                const char *at = strchr(user_start, '@');
                const char *user_end = at ? at : strchr(user_start, '>');
                if (at || user_end) {
                    size_t ulen = (size_t)((user_end ? user_end : at) - user_start);
                    if (ulen >= sizeof(c->effective_caller_id_num))
                        ulen = sizeof(c->effective_caller_id_num) - 1;
                    memcpy(c->effective_caller_id_num, user_start, ulen);
                    c->effective_caller_id_num[ulen] = '\0';
                }
            }
        } else {
            /* 无尖括号（privacy_id=1 → "Anonymous"） */
            snprintf(c->effective_caller_id_num, sizeof(c->effective_caller_id_num),
                     "%s", from_base);
            c->effective_caller_id_name[0] = '\0';
        }
    }

    /* --- 2. To 头构建 --- */
    if (rw->sip_invite_to_uri[0]) {
        snprintf(b->to, sizeof(b->to), "%s", rw->sip_invite_to_uri);
    } else if (bto[0]) {
        snprintf(b->to, sizeof(b->to), "%s", bto);
    } else {
        snprintf(b->to, sizeof(b->to), "%s", a->to);
    }
    if (rw->sip_invite_to_params[0]) {
        /* snprintf 源/目标同缓冲区会触发 -Werror=restrict，用 tmp 串接 */
        char tmp[B2B_HDR_MAX];
        snprintf(tmp, sizeof(tmp), "%s;%s", b->to, rw->sip_invite_to_params);
        snprintf(b->to, sizeof(b->to), "%s", tmp);
    }
    /* REQ-SIP-DIV-001：保存 B 腿初始被叫 URI（供 Diversion RFC 5806） */
    snprintf(c->orig_to, sizeof(c->orig_to), "%s", b->to);

    /* --- 3. Request-URI 构建 --- */
    if (rw->sip_invite_req_uri[0]) {
        snprintf(b->ruri, sizeof(b->ruri), "%s", rw->sip_invite_req_uri);
        snprintf(b->target, sizeof(b->target), "%s", rw->sip_invite_req_uri);
    } else {
        snprintf(b->ruri, sizeof(b->ruri), "%s", nruri);
        snprintf(b->target, sizeof(b->target), "%s", nruri);
    }
    if (rw->sip_invite_tel_params[0] && strncmp(b->ruri, "tel:", 4) == 0) {
        /* snprintf 源/目标同缓冲区会触发 -Werror=restrict */
        char tmp[B2B_HDR_MAX];
        snprintf(tmp, sizeof(tmp), "%s;%s", b->ruri, rw->sip_invite_tel_params);
        snprintf(b->ruri, sizeof(b->ruri), "%s", tmp);
    }

    /* --- 4/5. Contact 头构建 + Domain override 统一移到 send_invite_b 里处理 --- */

    nsbc_callid_make(b->callid, sizeof(b->callid), co->cfg.local_host);
    b->cseq = 1;
    sip_upd_guard_init(&b->guard);
    b->dlg = dlg_uac_new(co->cfg.dlg, b->callid, strlen(b->callid),
                         b->loc_tag, strlen(b->loc_tag), b->target,
                         b->cseq, SIP_M_INVITE);
    if (b->dlg)
        b->dlg->ud = c;
    tbl_link(co, c, 1);

    /* 媒体出口重写（多槽 SDP offer hook 链 + NAT 改写） */
    const char *out_sdp = a->offer;
    size_t out_sdp_len = a->offer_len;
    char rewritten[B2B_SDP_MAX]; rewritten[0] = '\0';
    /* offer_hook 多槽链：各模块（media_bridge/security/...）依次改写 */
    if (a->offer_len && g_sdp_hook_n[B2B_SDP_OFFER] > 0) {
        pthread_mutex_lock(&g_sdp_hook_lock);
        for (int i = 0; i < g_sdp_hook_n[B2B_SDP_OFFER]; ++i) {
            char buf[B2B_SDP_MAX]; buf[0] = '\0';
            const char *in = (rewritten[0] ? rewritten : a->offer);
            size_t inlen = rewritten[0] ? strlen(rewritten) : a->offer_len;
            int n = g_sdp_hooks[B2B_SDP_OFFER][i].fn(c, in, inlen, buf,
                                                     sizeof(buf),
                                                     g_sdp_hooks[B2B_SDP_OFFER][i].ud);
            if (n >= 0) {
                memcpy(rewritten, buf, (size_t)n);
                rewritten[n] = '\0';
            }
        }
        pthread_mutex_unlock(&g_sdp_hook_lock);
        if (rewritten[0]) {
            out_sdp = rewritten;
            out_sdp_len = strlen(rewritten);
        }
    }
    /* REQ-NAT-001：出站 SDP o=/c= 私网 IP → external_rtp_ip（FreeSWITCH ext-rtp-ip）
     * 无论 offer_hook 是否改写过，都需要一份可写 buffer 做 in-place NAT rewrite。
     * REQ-NAT-002：优先 per-call profile 的 external_rtp_ip，fallback cfg */
    const char *prof_rtp = (c->prof && c->prof->external_rtp_ip[0])
                           ? c->prof->external_rtp_ip : co->cfg.external_rtp_ip;
    const char *prof_sip = (c->prof && c->prof->external_sip_ip[0])
                           ? c->prof->external_sip_ip : co->cfg.external_sip_ip;
    const char *nat_rtp_ip = (prof_rtp && prof_rtp[0]) ? prof_rtp
                             : (prof_sip && prof_sip[0]) ? prof_sip : NULL;
    if (nat_rtp_ip && nat_rtp_ip[0] && a->offer_len) {
        /* 确保 rewritten 有当前 out_sdp 的完整拷贝（offer_hook 可能没写 rewritten） */
        if (out_sdp != rewritten)
            memcpy(rewritten, out_sdp, out_sdp_len);
        rewritten[out_sdp_len] = '\0';
        out_sdp_len = nat_sdp_rewrite(rewritten, out_sdp_len, sizeof(rewritten),
                                      nat_rtp_ip, prof_sip);
        out_sdp = rewritten;
    }

    /* --- 6. 拼 extra 头块（PID + invite_params + CLIR Privacy） --- */
    /* 2026-10-14 扩到 2048：3GPP IMS 场景 sip_invite_params 装 PAI+PPID+PANI+PCPI
     * 多行头块 ≈ 350 字节，加 pid_params(256) + privacy_id(14) + 留足余量 */
    char b_extra[2048]; b_extra[0] = '\0';
    size_t eo = 0;
    if (c->privacy_id) {
        eo += (size_t)snprintf(b_extra + eo, sizeof(b_extra) - eo,
                                "Privacy: id\r\n");
    }
    if (rw->sip_invite_pid_params[0]) {
        /* sip_invite_pid_params 允许完整头行 "P-Asserted-Identity: <sip:...>"
         * 或简写 "sip:user@host"（自动包 P-Asserted-Identity 头） */
        const char *pid = rw->sip_invite_pid_params;
        if (strstr(pid, ": ")) {
            eo += (size_t)snprintf(b_extra + eo, sizeof(b_extra) - eo, "%s\r\n", pid);
        } else {
            eo += (size_t)snprintf(b_extra + eo, sizeof(b_extra) - eo,
                                    "P-Asserted-Identity: <%s>\r\n", pid);
        }
    }
    if (rw->sip_invite_params[0]) {
        /* sip_invite_params：已格式化头块，每行自带 \r\n */
        size_t remain = sizeof(b_extra) - eo;
        strncat(b_extra + eo, rw->sip_invite_params, remain - 1);
        eo = strlen(b_extra);
        /* 确保结尾有 \r\n */
        if (eo >= 2 && (b_extra[eo-2] != '\r' || b_extra[eo-1] != '\n'))
            eo += (size_t)snprintf(b_extra + eo, sizeof(b_extra) - eo, "\r\n");
    }
    const char *b_extra_ptr = b_extra[0] ? b_extra : NULL;
    /* ★ 存 offer 到 leg —— 407 challenge 重发 / 302 follow 路径会用
     * 之前只靠 send_invite_b 栈上 buffer，txn 回调回来后已失效 */
    if (out_sdp_len > 0 && out_sdp_len <= B2B_SDP_MAX) {
        memcpy(b->offer, out_sdp, out_sdp_len);
        b->offer_len = out_sdp_len;
    } else {
        b->offer_len = 0;
    }
    b->inv_txn = send_invite_b(c, 1, out_sdp, out_sdp_len, 0, b_extra_ptr);
    char dstbuf[128];
    nsbc_sa_fmt(&b->dst, dstbuf, sizeof(dstbuf));
    llog(LOG_INFO, "b2b", "send_invite_b rc=%p dst=%s", b->inv_txn, dstbuf);
    if (!b->inv_txn) {
        respond_txn(co, t, 500, NULL, NULL, NULL, 0);
        call_destroy(c);
    } else {
        char sess[512];
        snprintf(sess, sizeof(sess),
                 "{\"event\":\"create\",\"call_id\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
                 "\"dir\":\"inbound\",\"state\":\"calling\"}",
                 c->leg[0].callid,
                 c->leg[0].from,
                 c->dst_number[0] ? c->dst_number : c->leg[0].to);
        uds_send_session(sess);
    }
}

/* 对话内 re-INVITE/UPDATE */
static void handle_indialog_invite(b2b_call_t *c, int leg, sip_txn_t *t,
                                   const sip_msg_t *m)
{
    b2b_core_t *co = c->core;
    b2b_leg_t *l = &c->leg[leg];
    int other = leg ^ 1;
    b2b_leg_t *o = &c->leg[other];
    if (o->state != LEG_CONFIRMED) {
        respond_txn(co, t, 491, NULL, NULL, NULL, 0);
        return;
    }
    if (!sip_upd_begin(&l->guard)) {
        respond_txn(co, t, 491, NULL, NULL, NULL, 0);
        return;
    }
    l->srv_txn = t;
    txn_set_ud(t, c);
    txn_set_tu(t, b2b_server_tu); /* re-INVITE 2xx 无 ACK：Timer H */
    if (m->body.len) {
        if (m->body.len > B2B_SDP_MAX) {
            sip_upd_end(&l->guard);
            l->srv_txn = NULL;
            respond_txn(co, t, 488, NULL, NULL, NULL, 0);
            return;
        }
        memcpy(l->offer, m->body.p, m->body.len);
        l->offer_len = m->body.len;
    }
    /* 向另一条腿发 re-INVITE */
    if (!sip_upd_begin(&o->guard)) {
        sip_upd_end(&l->guard);
        l->srv_txn = NULL;
        respond_txn(co, t, 491, NULL, NULL, NULL, 0);
        return;
    }
    o->reinv = 1;

    /* Session Timer: re-INVITE received → reset both leg timers */
    if (co->cfg.sess_timer_enabled) {
        uint64_t nm = now_ms();
        for (int i = 0; i < 2; ++i) {
            if (c->leg[i].st.active) {
                sess_timer_reset(&c->leg[i].st, nm);
                tw_mod(co->cfg.tw, &c->leg[i].refresh_node, nm + (uint64_t)c->leg[i].st.interval_s * 500u);
                tw_mod(co->cfg.tw, &c->leg[i].expire_node, nm + (uint64_t)c->leg[i].st.interval_s * 1000u + SESS_TIMER_GRACE_MS);
            }
        }
    }
    if (leg == 0) {
        /* A→B：走 B 腿出向 INVITE */
        o->inv_txn = send_invite_b(c, 1, l->offer, l->offer_len, 1, NULL);
    } else {
        /* B→A：朝 A 对话目标发 re-INVITE（A 腿 ICT） */
        char branch[40];
        new_branch(branch, sizeof(branch));
        long cs = ++o->cseq;
        char to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
        snprintf(to, sizeof(to), "%s;tag=%s", o->to, o->rem_tag);
        char out[SIP_MAX_MSG * 2];
        int n = build_dlg_req(co, o, SIP_M_INVITE, cs,
                              o->target[0] ? o->target : o->ruri, to,
                              branch, NULL, l->offer, l->offer_len,
                              out, sizeof(out));
        if (n > 0) {
            /* 设本地发送端口：对话已建立，用 out_prof 端口 */
            struct sip_profile *op = c->out_prof ? c->out_prof : c->prof;
            g_rx_local_port = (op && op->sip_udp_port)
                              ? op->sip_udp_port
                              : (uint16_t)co->cfg.local_udp_port;
            o->out_txn = txn_client_new(
                co->cfg.txn, TXN_ICT, branch, strlen(branch), SIP_M_INVITE,
                &o->dst, o->reliable, out, (size_t)n, cli_tu, c,
                now_ms());
            if (o->out_txn)
                snprintf(o->last_branch, sizeof(o->last_branch), "%s",
                         branch);
        }
    }
    if ((leg == 0 && !o->inv_txn) || (leg == 1 && !o->out_txn)) {
        sip_upd_end(&l->guard);
        sip_upd_end(&o->guard);
        l->srv_txn = NULL;
        o->reinv = 0;
        respond_txn(co, t, 500, NULL, NULL, NULL, 0);
    }
}

/* ------------------------------------------------------------------ */
/* 服务端事务统一入口                                                    */
/* ------------------------------------------------------------------ */
void b2b_server_tu(sip_txn_t *t, const sip_msg_t *m, txn_event_t ev,
                   void *ud)
{
    b2b_core_t *co = ud;

    if (ev == TXN_EV_TIMEOUT) {
        /* Timer H：2xx 发出后始终收不到 ACK（RFC3261 §13.3.1 应 BYE） */
        b2b_call_t *c = txn_ud(t);
        if (!c)
            return;
        call_lock(c);
        if (c->dead) {
            call_unlock(c);
            return;
        }
        llog(LOG_WARN, "b2b",
             "Timer H: IST timeout state=%d kind=%d callid=%s "
             "→ sending BYE to both legs",
             (int)txn_state(t), (int)txn_kind(t), c->leg[0].callid);
        for (int i = 0; i < 2; ++i) {
            if (c->leg[i].state == LEG_CONFIRMED)
                send_bye_leg(c, i);
            c->leg[i].state = LEG_TERMINATED;
        }
        call_unlock(c);
        call_destroy(c);
        return;
    }

    if (ev == TXN_EV_ACK) {
        /* 服务端 INVITE 事务的 ud 在首请求时已指向呼叫 */
        b2b_call_t *c = txn_ud(t);
        if (!c)
            return;
        call_lock(c);
        if (c->dead || (c->leg[0].inv_txn != t &&
                        c->leg[0].srv_txn != t)) {
            call_unlock(c);
            return;
        }
        if (c->a_final >= 200 && c->a_final < 300) {
            c->leg[0].state = LEG_CONFIRMED;
            /* REQ-PBX-CDR-001：ANSWER emit — A 腿 ACK 收到即记账（幂等） */
            if (c->answer_ms == 0) {
                c->answer_ms = now_ms();
                b2b_emit_cdr(c, CDR_PHASE_ANSWER);
            }
            if (c->leg[1].state == LEG_CONFIRMED && !c->active) {
                c->active = 1;
                add_atomic(&co->active, 1);
                maybe_start_media(c);
            }
        } else if (c->a_final) {
            /* 非 2xx 的 ACK：呼叫彻底结束 */
            call_unlock(c);
            call_destroy(c);
            return;
        }
        call_unlock(c);
        return;
    }

    if (ev != TXN_EV_NEW_REQUEST || !m)
        return;

    /* REGISTER 多槽链：遍历所有注册模块直到某个返回非 0
     * （security 可返回 PERM_DENIED → 403 静默丢弃；registrar 返回 OK → 已处理） */
    if (m->method == SIP_M_REGISTER) {
        /* HA 门控：备机不受理注册（防 AoR 双写/主备状态漂移），静默丢弃 */
        if (ha_role() != HA_ROLE_ACTIVE) {
            static unsigned drop_reg_n = 0;
            llog(LOG_WARN, "b2b",
                 "HA standby: drop REGISTER (dropped=%u)", drop_reg_n++);
            return;
        }
        extern sip_tr_proto_t g_ws_bridge_proto; /* signaling.c */
        const nsbc_sa_t *rp = txn_peer(t);
        sip_tr_proto_t reg_proto =
            (rp && rp->ss.ss_family == AF_UNSPEC) ? g_ws_bridge_proto
                                                  : SIP_TR_UDP;
        int handled = 0;
        llog(LOG_INFO, "b2b", "REGISTER hook chain start: n=%d ha_active=%d src_fam=%d",
             g_reg_hook_n, ha_role() == HA_ROLE_ACTIVE,
             rp ? rp->ss.ss_family : -1);
        pthread_mutex_lock(&g_reg_hook_lock);
        for (int i = 0; i < g_reg_hook_n; ++i) {
            int rc = g_reg_hooks[i].fn(reg_proto, rp, (sip_msg_t*)m, NULL,
                                       0, t, g_reg_hooks[i].ud);
            llog(LOG_INFO, "b2b", "REGISTER hook[%d] rc=%d", i, rc);
            if (rc == NSBC_PERM_DENIED) { handled = 1; break; }
            if (rc == NSBC_OK) handled = 1;
        }
        pthread_mutex_unlock(&g_reg_hook_lock);
        llog(LOG_INFO, "b2b", "REGISTER hook chain end: handled=%d", handled);
        if (!handled)
            respond_txn(co, t, 501, NULL, NULL, NULL, 0);
        return;
    }

    if (m->method == SIP_M_OPTIONS) {
        /* 静态黑名单（最前置）：OPTIONS 是扫描探测主力，命中静默丢弃
         * ——跳过所有响应路径（不回 200），防 SBC 指纹暴露。
         * NIP 完整围栏：method 禁用优先（全位图 O(1)）→ deny → allowlist */
        {
            const nsbc_sa_t *osrc = txn_peer(t);
            char oip[INET6_ADDRSTRLEN] = {0};
            sa_ip_copy(osrc, oip, sizeof(oip));
            char f_ua[256] = {0}, f_caller[128] = {0}, f_callee[128] = {0};
            msg_ua_copy(m, f_ua, sizeof(f_ua));
            msg_caller_copy(m, f_caller, sizeof(f_caller));
            msg_callee_copy(m, f_callee, sizeof(f_callee));
            security_fence_ctx_t fctx = {0};
            fctx.src = (const struct sockaddr *)osrc;
            fctx.method = SIP_M_OPTIONS;
            fctx.ua = f_ua;
            fctx.caller = f_caller;
            fctx.callee = f_callee;
            security_check_fence(&fctx);
            if (fctx.hit != BLACKLIST_OK) {
                security_fence_log_drop(fctx.hit, oip, f_ua, "OPTIONS",
                                        f_caller, f_callee);
                return;
            }
        }
        respond_txn(co, t, 200, NULL,
                    "Allow: INVITE,ACK,CANCEL,BYE,OPTIONS,REFER\r\n"
                    "Accept: application/sdp\r\n",
                    NULL, 0);
        return;
    }

    if (m->method == SIP_M_INVITE) {
        if (sip_msg_in_dialog(m)) {
            int leg = -1;
            sip_hdr_t *cid = sip_hdr_get(m, SIP_H_CALL_ID);
            b2b_call_t *c = NULL;
            if (cid)
                c = tbl_find(co, cid->value.p, cid->value.len, &leg);
            if (c && leg >= 0) {
                call_lock(c);
                if (!c->dead)
                    handle_indialog_invite(c, leg, t, m);
                call_unlock(c);
                return;
            }
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }

        /* Phase 5 XL: endpoint_registry 分发
         *
         * 遍历所有已注册 endpoint，第一个 accept(m) 返回 OK/PERM_DENIED 的就接管。
         * 当前只有 sip endpoint 注册（永远 accept），但架构预留 ai/webrtc 等 endpoint。
         *
         * ctx 构造：如果 accept 返回 out_ctx!=NULL 就直接用；否则自己构造 sip_invite_ctx_t
         * （包含 co/t/proto/src/local_port）塞给 handle_invite。
         *
         * PERM_DENIED = 安全 endpoint（security 模块）静默拦截，直接 return 不回包 */
        {
            int slot = -1;
            void *ep_ctx = NULL;
            const endpoint_ops_t *ep = endpoint_registry_try_accept((sip_msg_t*)m, &slot, &ep_ctx);

            if (ep) {
                sip_invite_ctx_t ictx = {
                    .co = co, .t = t, .m = m,
                    .proto = SIP_TR_UDP, .src = txn_peer(t),
                    .local_port = g_rx_local_port
                };
                int rc = 0;
                if (ep->handle_invite) {
                    /* accept 已返回 PERM_DENIED → 安全拦截：不调 handle_invite，直接静默 */
                    if (ep_ctx) {
                        rc = ep->handle_invite(ep_ctx, NULL);
                    } else {
                        rc = ep->handle_invite(&ictx, NULL);
                    }
                }
                endpoint_registry_dec(slot);
                /* call 还没创建，从 msg 里取 Call-ID */
                {
                    sip_hdr_t *_cid = sip_hdr_get(m, SIP_H_CALL_ID);
                    const char *_cids = (_cid && _cid->value.p) ? _cid->value.p : "-";
                    llog(LOG_INFO, "b2b",
                         "Phase 5 XL: INVITE dispatched to endpoint '%s' rc=%d callid=%s",
                         ep->name, rc, _cids);
                }
                return;
            }
            /* 没 endpoint 接管 → 走原有路径 */
        }
        handle_new_invite(co, t, m, SIP_TR_UDP, txn_peer(t), g_rx_local_port);
        return;
    }

    if (m->method == SIP_M_CANCEL) {
        sip_hdr_t *cid = sip_hdr_get(m, SIP_H_CALL_ID);
        int leg = -1;
        b2b_call_t *c = cid ? tbl_find(co, cid->value.p, cid->value.len, &leg)
                            : NULL;
        /* 200 给 CANCEL 本身 */
        respond_txn(co, t, 200, NULL, NULL, NULL, 0);
        if (!c)
            return;
        call_lock(c);
        if (c->dead) {
            call_unlock(c);
            return;
        }
        b2b_leg_t *target = &c->leg[leg >= 0 ? leg : 0];
        if (target->srv_txn) {
            /* re-INVITE 取消：487 该 re-INVITE，呼叫保留 */
            respond_txn(co, target->srv_txn, 487, NULL, NULL, NULL, 0);
            target->srv_txn = NULL;
            sip_upd_end(&target->guard);
            int other = (leg >= 0 ? leg : 0) ^ 1;
            if (c->leg[other].reinv) {
                send_cancel_req(c, other);
                c->leg[other].reinv = 0;
            }
            call_unlock(c);
            return;
        }
        if (!c->a_final) {
            respond_a(c, 487, NULL, 0);
            c->a_final = 487;
            /* B 腿 EARLY/CALLING：发 CANCEL */
            if (c->leg[1].state == LEG_CALLING ||
                c->leg[1].state == LEG_EARLY)
                send_cancel_req(c, 1);
            call_unlock(c);
            return;
        }
        call_unlock(c);
        return;
    }

    if (m->method == SIP_M_BYE) {
        sip_hdr_t *cid = sip_hdr_get(m, SIP_H_CALL_ID);
        int leg = -1;
        b2b_call_t *c = cid ? tbl_find(co, cid->value.p, cid->value.len, &leg)
                            : NULL;
        if (!c || leg < 0) {
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }
        respond_txn(co, t, 200, NULL, NULL, NULL, 0);
        call_lock(c);
        if (c->dead) {
            call_unlock(c);
            return;
        }
        c->leg[leg].state = LEG_TERMINATING;
        /* ★ 记录谁发起的挂断：leg=0→A腿(主叫)发BYE，leg=1→B腿(被叫)发BYE */
        c->hangup_by = leg + 1;
        int other = leg ^ 1;
        if (c->leg[other].state == LEG_CONFIRMED)
            send_bye_leg(c, other);
        else {
            c->leg[other].state = LEG_TERMINATED;
            c->leg[leg].state = LEG_TERMINATED;
            call_unlock(c);
            call_destroy(c);
            return;
        }
        /* 注意：不设 leg[leg].state = LEG_TERMINATED — 保留 TERMINATING
         * 状态让 cli_tu 在对端 BYE 200 OK 回来时能通过条件检查
         * （cli_tu line 1352: leg[0].state == LEG_TERMINATING 才 call_destroy）。 */
        call_unlock(c);
        return;
    }

    /* ---- re-INVITE（RFC 3261 §14.2）：对话内媒体重协商 ----
     * 和 UPDATE(RFC3311) 不同：re-INVITE 是 dialog 级 INVITE，会跟对话内
     * 的 1xx/2xx/Bye 冲突——用 sip_upd guard 串行互斥（同 UPDATE 模式）。
     * UA 发 re-INVITE 做 hold/unhold 是最常见场景，也可以是 codec 切换。 */
    if (m->method == SIP_M_INVITE && sip_msg_in_dialog(m)) {
        int leg = -1;
        sip_hdr_t *cid = sip_hdr_get(m, SIP_H_CALL_ID);
        b2b_call_t *c = cid ? tbl_find(co, cid->value.p, cid->value.len, &leg) : NULL;
        if (!c || leg < 0) {
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }
        b2b_leg_t *l = &c->leg[leg];
        int other = leg ^ 1;
        b2b_leg_t *o = &c->leg[other];

        /* re-INVITE 需要 dialog 至少有一条腿 CONFIRMED/EARLY（初始 INVITE 完成了） */
        if (o->state != LEG_CONFIRMED && o->state != LEG_EARLY) {
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        if (!sip_upd_begin(&l->guard)) {
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        txn_set_ud(t, c);
        txn_set_tu(t, b2b_server_tu);

        /* 存 SDP：入站 re-INVITE 的 offer 可能带 hold 标志，
         * 我们的 answer 要跟它匹配（cli_tu 回 200 OK 时自动用 l->offer） */
        if (m->body.len && m->body.len <= B2B_SDP_MAX) {
            memcpy(l->offer, m->body.p, m->body.len);
            l->offer_len = m->body.len;
            /* legacy hold：c=IN IP4 0.0.0.0；direction hold：a=sendonly */
            const char *body = m->body.p;
            if (memmem(body, m->body.len, "0.0.0.0", 7)
                || memmem(body, m->body.len, "a=sendonly", 10)
                || memmem(body, m->body.len, "a=inactive", 10)) {
                llog(LOG_INFO, "b2b",
                     "re-INVITE hold detected leg=%d callid=%s",
                     leg, cid->value.p);
                c->hold_legs |= (1u << leg);
                /* 对端腿也被 hold（hold 语义：双方媒体都停）
                 * 只做标志位，具体 MOH（Music on Hold）播放由 dialplan hook 触发 */
            } else {
                int was = c->hold_legs & (1u << leg);
                if (was) {
                    llog(LOG_INFO, "b2b",
                         "re-INVITE unhold detected leg=%d callid=%s",
                         leg, cid->value.p);
                    c->hold_legs &= ~(1u << leg);
                }
            }
        }

        /* 转发 re-INVITE 到对端 leg（UPDATE 同模式） */
        if (!sip_upd_begin(&o->guard)) {
            sip_upd_end(&l->guard);
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        o->reinv = 1;
        o->out_txn = NULL;

        char extra[512];
        extra[0] = '\0';
        if (m->body.len)
            snprintf(extra, sizeof(extra), "Content-Type: application/sdp\r\n");

        char branch[40];
        new_branch(branch, sizeof(branch));
        long cs = ++o->cseq;
        char to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
        snprintf(to, sizeof(to), "%s;tag=%s", o->to, o->rem_tag);
        char ruri[DLG_URI_MAX + 8];
        snprintf(ruri, sizeof(ruri), "%s",
                 o->target[0] ? o->target : o->ruri);
        char out[SIP_MAX_MSG * 2];
        int n = build_dlg_req(co, o, SIP_M_INVITE, cs,
                              ruri, to, branch,
                              extra[0] ? extra : NULL,
                              m->body.len ? m->body.p : NULL,
                              m->body.len,
                              out, sizeof(out));
        if (n > 0) {
            o->out_txn = txn_client_new(
                co->cfg.txn, TXN_ICT, branch, strlen(branch), SIP_M_INVITE,
                &o->dst, o->reliable, out, (size_t)n, cli_tu, c, now_ms());
            if (o->out_txn)
                snprintf(o->last_branch, sizeof(o->last_branch), "%s", branch);
        }
        if (!o->out_txn) {
            sip_upd_end(&l->guard);
            sip_upd_end(&o->guard);
            respond_txn(co, t, 500, NULL, NULL, NULL, 0);
        }
        /* cli_tu 在 o->out_txn 终响应时回 A 腿 200 OK 并释放 guard */
        return;
    }

    /* ---- REFER (RFC 3515)：入站 transfer 请求 ----
     * blind transfer：UA 发 REFER + Refer-To（不带 Replaces），
     *   我们对 B-腿发 BYE，然后对 Refer-To 目标发新 INVITE（重建桥）。
     * attended transfer（带 Replaces）：暂不做，回 481 + log。 */
    if (m->method == SIP_M_REFER && sip_msg_in_dialog(m)) {
        int leg = -1;
        sip_hdr_t *cid = sip_hdr_get(m, SIP_H_CALL_ID);
        b2b_call_t *c = cid ? tbl_find(co, cid->value.p, cid->value.len, &leg) : NULL;
        if (!c || leg < 0) {
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }
        b2b_leg_t *l = &c->leg[leg];     /* 发起 REFER 的腿（UA） */
        int other = leg ^ 1;

        /* REFER 时发起腿必须 CONFIRMED（在通话中） */
        if (l->state != LEG_CONFIRMED && l->state != LEG_EARLY) {
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }

        sip_hdr_t *rt_hdr = sip_hdr_get(m, SIP_H_REFER_TO);
        sip_hdr_t *repl_hdr = sip_hdr_get(m, SIP_H_REPLACES);

        /* 无 Refer-To → 481 Invalid REFER */
        if (!rt_hdr || !rt_hdr->value.p || !rt_hdr->value.len) {
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }

        /* 先解析 Replaces（如果有），后解析 Refer-To 目标 */
        int is_attended = (repl_hdr && repl_hdr->value.p && repl_hdr->value.len);
        char replaces[DLG_CALLID_MAX + DLG_TAG_MAX * 4 + 64];
        replaces[0] = '\0';
        if (is_attended) {
            size_t rl = repl_hdr->value.len < sizeof(replaces) - 1
                        ? repl_hdr->value.len : sizeof(replaces) - 1;
            memcpy(replaces, repl_hdr->value.p, rl);
            replaces[rl] = '\0';
            /* 基本合法性：必须包含 call-id= 和 from-tag= */
            if (!strstr(replaces, "call-id=") || !strstr(replaces, "from-tag=")) {
                llog(LOG_WARN, "b2b",
                     "attended transfer invalid Replaces format callid=%s",
                     cid->value.p);
                respond_txn(co, t, 400, NULL, NULL, NULL, 0);
                return;
            }
            llog(LOG_INFO, "b2b",
                 "REFER attended transfer callid=%s replaces=%s",
                 cid->value.p, replaces);
        }

        /* 解析 Refer-To SIP URI → 提取目标号码 */
        char refer_to[DLG_URI_MAX];
        size_t rtl = rt_hdr->value.len < sizeof(refer_to) - 1
                     ? rt_hdr->value.len : sizeof(refer_to) - 1;
        memcpy(refer_to, rt_hdr->value.p, rtl);
        refer_to[rtl] = '\0';
        /* 去掉 <> 尖括号（Refer-To 通常是 "<sip:123@host>"） */
        if (refer_to[0] == '<') {
            char *end = strchr(refer_to, '>');
            if (end) *end = '\0';
            memmove(refer_to, refer_to + 1, strlen(refer_to));
        }
        char *at = strchr(refer_to, '@');
        if (at) *at = '\0'; /* 留下 user 部分（目标号码） */

        llog(LOG_INFO, "b2b",
             "REFER %s transfer leg=%d target=%s callid=%s",
             is_attended ? "attended" : "blind",
             leg, refer_to, cid->value.p);

        /* 1. 先对 REFER 发起腿回 202 Accepted（RFC 3515 要求必须立刻响应） */
        respond_txn(co, t, 202, NULL, NULL, NULL, 0);

        /* 2. b2b_transfer_call 内部持锁 + b2b_send_refer + 重建桥（异步）。
         *    blind 模式传 replaces=NULL，attended 模式传解析好的 Replaces 串。 */
        {
            int rc = b2b_transfer_call(
                c, other, refer_to,
                is_attended ? CCF_XFER_CONSULTED : CCF_XFER_BLIND,
                NULL,
                is_attended ? replaces : NULL);
            if (rc != NSBC_OK) {
                llog(LOG_WARN, "b2b",
                     "REFER transfer failed rc=%d kind=%d callid=%s",
                     rc, is_attended, cid->value.p);
            }
        }
        return;
    }

    /* ---- UPDATE (RFC 3311)：对话内，转发到对端 leg ---- */
    if (m->method == SIP_M_UPDATE && sip_msg_in_dialog(m)) {
        int leg = -1;
        sip_hdr_t *cid = sip_hdr_get(m, SIP_H_CALL_ID);
        b2b_call_t *c = cid ? tbl_find(co, cid->value.p, cid->value.len, &leg) : NULL;
        if (!c || leg < 0) {
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }
        /* REQ-SIP-PROFILE-001：profile 关 UPDATE → 501（对端应改用
         * re-INVITE 做媒体重协商；呼叫不受影响） */
        if (c->prof && !c->prof->enable_update) {
            llog(LOG_INFO, "b2b",
                 "UPDATE rejected: profile '%s' enable_update=0",
                 c->prof->name);
            respond_txn(co, t, 501, NULL, NULL, NULL, 0);
            return;
        }
        b2b_leg_t *l = &c->leg[leg];
        int other = leg ^ 1;
        b2b_leg_t *o = &c->leg[other];
        /* UPDATE 可在 EARLY 或 CONFIRMED 状态发送（RFC 3311） */
        if (o->state != LEG_CONFIRMED && o->state != LEG_EARLY) {
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        if (!sip_upd_begin(&l->guard)) {
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        /* 注意：不要改 l->srv_txn —— 那是 INVITE 的 server txn */
        txn_set_ud(t, c);
        txn_set_tu(t, b2b_server_tu);

        /* 保存 UPDATE body（SDP），发 200 OK 时可作为 answer */
        if (m->body.len && m->body.len <= B2B_SDP_MAX) {
            memcpy(l->offer, m->body.p, m->body.len);
            l->offer_len = m->body.len;
        }

        /* 构建 extra：透传 Content-Type (UPDATE 可能带 SDP) */
        char extra[512];
        extra[0] = '\0';
        if (m->body.len) {
            snprintf(extra, sizeof(extra), "Content-Type: application/sdp\r\n");
        }

        /* 转发 UPDATE 到对端 leg */
        if (!sip_upd_begin(&o->guard)) {
            sip_upd_end(&l->guard);
            l->srv_txn = NULL;
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        o->reinv = 1;
        o->out_txn = NULL;

        char branch[40];
        new_branch(branch, sizeof(branch));
        long cs = ++o->cseq;
        char to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
        snprintf(to, sizeof(to), "%s;tag=%s", o->to, o->rem_tag);
        char ruri[DLG_URI_MAX + 8];
        snprintf(ruri, sizeof(ruri), "%s",
                 o->target[0] ? o->target : o->ruri);
        char out[SIP_MAX_MSG * 2];
        int n = build_dlg_req(co, o, SIP_M_UPDATE, cs,
                              ruri, to, branch,
                              extra[0] ? extra : NULL,
                              m->body.len ? m->body.p : NULL,
                              m->body.len,
                              out, sizeof(out));
        if (n > 0) {
            o->out_txn = txn_client_new(
                co->cfg.txn, TXN_NICT, branch, strlen(branch), SIP_M_UPDATE,
                &o->dst, o->reliable, out, (size_t)n, cli_tu, c, now_ms());
            if (o->out_txn)
                snprintf(o->last_branch, sizeof(o->last_branch), "%s", branch);
        }
        if (!o->out_txn) {
            sip_upd_end(&l->guard);
            sip_upd_end(&o->guard);
            l->srv_txn = NULL;
            o->reinv = 0;
            respond_txn(co, t, 500, NULL, NULL, NULL, 0);
        }
        return;
    }

    /* ---- PRACK (RFC 3262)：对话内可靠临时响应确认 ---- */
    if (m->method == SIP_M_PRACK && sip_msg_in_dialog(m)) {
        int leg = -1;
        sip_hdr_t *cid = sip_hdr_get(m, SIP_H_CALL_ID);
        b2b_call_t *c = cid ? tbl_find(co, cid->value.p, cid->value.len, &leg) : NULL;
        if (!c || leg < 0) {
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }
        b2b_leg_t *l = &c->leg[leg];      /* 入向腿（收到 PRACK 的腿） */
        int other = leg ^ 1;
        b2b_leg_t *o = &c->leg[other];     /* 出向腿（有 peer_rseq） */
        /* REQ-SIP-PROFILE-001：profile 关 PRACK → 本侧从不发 reliable 1xx
         * （local_rseq 恒 0），到达的 PRACK 一律 481（防误配/恶意注入） */
        if (c->prof && !c->prof->enable_prack) {
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }
        if (o->state != LEG_EARLY) {
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }
        if (!sip_upd_begin(&l->guard)) {
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        /* 注意：不要改 l->srv_txn —— 那是 INVITE 的 server txn，
         * 后面 respond_a 要用来发 200 OK！PRACK/UPDATE 各自是独立 txn。 */
        txn_set_ud(t, c);
        txn_set_tu(t, b2b_server_tu);

        /* RAck 重写：A-leg RSeq → B-leg 原始 RSeq，然后转发 */
        char extra[256];
        extra[0] = '\0';
        unsigned acked_a_rseq = 0;
        {
            /* 从原消息遍历找 RAck header */
            const char *rack_val = NULL;
            size_t rack_len = 0;
            for (int i = 0; i < m->nhdrs; ++i) {
                const sip_hdr_t *h = &m->hdrs[i];
                if (h->type == SIP_H_UNKNOWN && h->name.p && h->name.len >= 4 &&
                    !strncasecmp(h->name.p, "RAck", 4)) {
                    rack_val = h->value.p;
                    rack_len = h->value.len;
                    break;
                }
            }
            if (rack_val && rack_len) {
                /* RAck 格式: "1 1 INVITE" （rseq cseq method） */
                char rbuf[64];
                size_t rlen = rack_len < sizeof(rbuf) - 1 ? rack_len : sizeof(rbuf) - 1;
                memcpy(rbuf, rack_val, rlen);
                rbuf[rlen] = '\0';
                unsigned a_rseq = 0;
                long a_cseq = 0;
                char method_str[32] = {0};
                /* 简单解析：%u %ld %31s */
                if (sscanf(rbuf, "%u %ld %31s", &a_rseq, &a_cseq, method_str) >= 1) {
                    acked_a_rseq = a_rseq;
                }
                /* 如果 ack 的是我们发的 pending reliable 1xx → 用 B-leg 原始 RSeq 重写 */
                unsigned b_peer_rseq = o->peer_rseq;
                if (acked_a_rseq != 0 && l->local_rseq != 0 &&
                    acked_a_rseq == l->local_rseq && b_peer_rseq != 0) {
                    snprintf(extra, sizeof(extra), "RAck: %u %ld %s\r\n",
                             b_peer_rseq, a_cseq, method_str[0] ? method_str : "INVITE");
                    /* PRACK 已确认 pending reliable 1xx → 清零 pending 状态 */
                    l->local_rseq = 0;
                    o->peer_rseq = 0;
                } else {
                    /* 透传（可能是重传的 PRACK） */
                    snprintf(extra, sizeof(extra), "RAck: %s\r\n", rbuf);
                }
            }
        }

        /* ★ 先回 A-leg PRACK 200 OK（RFC 3262：收到即回，不等下游） */
        respond_txn(co, t, 200, NULL, NULL, NULL, 0);

        /* 转发 PRACK 到对端 leg（early dialog） */
        if (!sip_upd_begin(&o->guard)) {
            sip_upd_end(&l->guard);
            l->srv_txn = NULL;
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        o->out_txn = NULL;

        char branch[40];
        new_branch(branch, sizeof(branch));
        long cs = ++o->cseq;
        char to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
        snprintf(to, sizeof(to), "%s;tag=%s", o->to, o->rem_tag);
        char ruri[DLG_URI_MAX + 8];
        snprintf(ruri, sizeof(ruri), "%s",
                 o->target[0] ? o->target : o->ruri);
        char out[SIP_MAX_MSG * 2];
        int n = build_dlg_req(co, o, SIP_M_PRACK, cs,
                              ruri, to, branch,
                              extra[0] ? extra : NULL, NULL, 0,
                              out, sizeof(out));
        if (n > 0) {
            o->out_txn = txn_client_new(
                co->cfg.txn, TXN_NICT, branch, strlen(branch), SIP_M_PRACK,
                &o->dst, o->reliable, out, (size_t)n, cli_tu, c, now_ms());
            if (o->out_txn)
                snprintf(o->last_branch, sizeof(o->last_branch), "%s", branch);
        }
        if (!o->out_txn) {
            sip_upd_end(&l->guard);
            sip_upd_end(&o->guard);
            l->srv_txn = NULL;
            respond_txn(co, t, 500, NULL, NULL, NULL, 0);
        }
        return;
    }

    /* ---- MESSAGE (RFC 3428)：对话内即时消息，转发到对端 leg ---- */
    if (m->method == SIP_M_MESSAGE && sip_msg_in_dialog(m)) {
        int leg = -1;
        sip_hdr_t *cid = sip_hdr_get(m, SIP_H_CALL_ID);
        b2b_call_t *c = cid ? tbl_find(co, cid->value.p, cid->value.len, &leg) : NULL;
        if (!c || leg < 0) {
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }
        b2b_leg_t *l = &c->leg[leg];
        int other = leg ^ 1;
        b2b_leg_t *o = &c->leg[other];
        if (o->state != LEG_CONFIRMED && o->state != LEG_EARLY) {
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        if (!sip_upd_begin(&l->guard)) {
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }

        /* 延迟响应（B2BUA 端到端语义）：存 srv_txn，等 B 腿终响应
         * 由 cli_tu 回 A 腿——A 收到 200 时 guard 已释放，连续
         * MESSAGE/INFO 不会 491。CONFIRMED 后 INVITE 的 inv_txn 已
         * 固定，srv_txn 此时空闲（re-INVITE 场景由 guard 串行互斥）。 */
        l->srv_txn = t;
        txn_set_ud(t, c);

        /* 构建 extra：透传 Content-Type + Content-Length（如有 body） */
        char extra[512];
        extra[0] = '\0';
        if (m->body.len && m->body.p) {
            sip_hdr_t *ct = sip_hdr_get(m, SIP_H_CONTENT_TYPE);
            if (ct && ct->value.p && ct->value.len) {
                int n = snprintf(extra, sizeof(extra), "Content-Type: %.*s\r\n",
                                 (int)ct->value.len, ct->value.p);
                if (n > 0 && (size_t)n < sizeof(extra)) {
                    int cl = snprintf(extra + n, sizeof(extra) - (size_t)n,
                                      "Content-Length: %zu\r\n", m->body.len);
                    if (cl > 0) n += cl;
                }
            }
        }

        /* 转发 MESSAGE 到对端 leg */
        if (!sip_upd_begin(&o->guard)) {
            sip_upd_end(&l->guard);
            respond_txn(co, l->srv_txn, 491, NULL, NULL, NULL, 0);
            l->srv_txn = NULL;
            return;
        }
        o->out_txn = NULL;

        char branch[40];
        new_branch(branch, sizeof(branch));
        long cs = ++o->cseq;
        char to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
        snprintf(to, sizeof(to), "%s;tag=%s", o->to, o->rem_tag);
        char ruri[DLG_URI_MAX + 8];
        snprintf(ruri, sizeof(ruri), "%s",
                 o->target[0] ? o->target : o->ruri);
        char out[SIP_MAX_MSG * 2];
        int n = build_dlg_req(co, o, SIP_M_MESSAGE, cs,
                              ruri, to, branch,
                              extra[0] ? extra : NULL,
                              m->body.len ? m->body.p : NULL,
                              m->body.len,
                              out, sizeof(out));
        if (n > 0) {
            o->out_txn = txn_client_new(
                co->cfg.txn, TXN_NICT, branch, strlen(branch), SIP_M_MESSAGE,
                &o->dst, o->reliable, out, (size_t)n, cli_tu, c, now_ms());
            if (o->out_txn)
                snprintf(o->last_branch, sizeof(o->last_branch), "%s", branch);
        }
        if (!o->out_txn) {
            sip_upd_end(&l->guard);
            sip_upd_end(&o->guard);
            respond_txn(co, l->srv_txn, 500, NULL, NULL, NULL, 0);
            l->srv_txn = NULL;
            return;
        }
        /* cli_tu 在 o->out_txn 终响应时回 A 腿并释放 guard */
        return;
    }

    /* ---- INFO (RFC 6086)：对话内信令承载，转发到对端 leg ---- */
    if (m->method == SIP_M_INFO && sip_msg_in_dialog(m)) {
        int leg = -1;
        sip_hdr_t *cid = sip_hdr_get(m, SIP_H_CALL_ID);
        b2b_call_t *c = cid ? tbl_find(co, cid->value.p, cid->value.len, &leg) : NULL;
        if (!c || leg < 0) {
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }
        b2b_leg_t *l = &c->leg[leg];
        int other = leg ^ 1;
        b2b_leg_t *o = &c->leg[other];
        if (o->state != LEG_CONFIRMED && o->state != LEG_EARLY) {
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        if (!sip_upd_begin(&l->guard)) {
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }

        /* 延迟响应：同 MESSAGE，存 srv_txn 等 B 腿终响应再回 A */
        l->srv_txn = t;
        txn_set_ud(t, c);

        /* 构建 extra：透传 Content-Type/Content-Length（如有 body） */
        char extra[512];
        extra[0] = '\0';
        if (m->body.len && m->body.p) {
            sip_hdr_t *ct = sip_hdr_get(m, SIP_H_CONTENT_TYPE);
            if (ct && ct->value.p && ct->value.len) {
                int n = snprintf(extra, sizeof(extra), "Content-Type: %.*s\r\n",
                                 (int)ct->value.len, ct->value.p);
                if (n > 0 && (size_t)n < sizeof(extra)) {
                    int cl = snprintf(extra + n, sizeof(extra) - (size_t)n,
                                      "Content-Length: %zu\r\n", m->body.len);
                    if (cl > 0) n += cl;
                }
            }
        }

        /* 转发 INFO 到对端 leg */
        if (!sip_upd_begin(&o->guard)) {
            sip_upd_end(&l->guard);
            respond_txn(co, l->srv_txn, 491, NULL, NULL, NULL, 0);
            l->srv_txn = NULL;
            return;
        }
        o->out_txn = NULL;

        char branch[40];
        new_branch(branch, sizeof(branch));
        long cs = ++o->cseq;
        char to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
        snprintf(to, sizeof(to), "%s;tag=%s", o->to, o->rem_tag);
        char ruri[DLG_URI_MAX + 8];
        snprintf(ruri, sizeof(ruri), "%s",
                 o->target[0] ? o->target : o->ruri);
        char out[SIP_MAX_MSG * 2];
        int n = build_dlg_req(co, o, SIP_M_INFO, cs,
                              ruri, to, branch,
                              extra[0] ? extra : NULL,
                              m->body.len ? m->body.p : NULL,
                              m->body.len,
                              out, sizeof(out));
        if (n > 0) {
            o->out_txn = txn_client_new(
                co->cfg.txn, TXN_NICT, branch, strlen(branch), SIP_M_INFO,
                &o->dst, o->reliable, out, (size_t)n, cli_tu, c, now_ms());
            if (o->out_txn)
                snprintf(o->last_branch, sizeof(o->last_branch), "%s", branch);
        }
        if (!o->out_txn) {
            sip_upd_end(&l->guard);
            sip_upd_end(&o->guard);
            respond_txn(co, l->srv_txn, 500, NULL, NULL, NULL, 0);
            l->srv_txn = NULL;
            return;
        }
        /* cli_tu 在 o->out_txn 终响应时回 A 腿并释放 guard */
        return;
    }

    /* ---- PUBLISH (RFC 3903)：presence 发布，dialog 内透传到对端 leg ----
     * 场景：Bria/MicroSIP 等软电话 publish 自己的在线状态到 presence 服务器，
     * B2BUA 只做中间转发。模式同 SUBSCRIBE/NOTIFY：guard 串行互斥 + body 透传。 */
    if (m->method == SIP_M_PUBLISH && sip_msg_in_dialog(m)) {
        int leg = -1;
        sip_hdr_t *cid = sip_hdr_get(m, SIP_H_CALL_ID);
        b2b_call_t *c = cid ? tbl_find(co, cid->value.p, cid->value.len, &leg) : NULL;
        if (!c || leg < 0) {
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }
        b2b_leg_t *l = &c->leg[leg];
        int other = leg ^ 1;
        b2b_leg_t *o = &c->leg[other];
        if (o->state != LEG_CONFIRMED && o->state != LEG_EARLY) {
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        if (!sip_upd_begin(&l->guard)) {
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        l->srv_txn = t;
        txn_set_ud(t, c);

        /* 透传 Content-Type + Content-Length（PUBLISH 带 application/pidf+xml） */
        char extra[512];
        extra[0] = '\0';
        if (m->body.len && m->body.p) {
            sip_hdr_t *ct = sip_hdr_get(m, SIP_H_CONTENT_TYPE);
            if (ct && ct->value.p && ct->value.len) {
                int n = snprintf(extra, sizeof(extra), "Content-Type: %.*s\r\n",
                                 (int)ct->value.len, ct->value.p);
                if (n > 0 && (size_t)n < sizeof(extra)) {
                    int cl = snprintf(extra + n, sizeof(extra) - (size_t)n,
                                      "Content-Length: %zu\r\n", m->body.len);
                    if (cl > 0) n += cl;
                }
            }
        }

        /* 转发到对端 leg（NICT：non-INVITE client txn） */
        if (!sip_upd_begin(&o->guard)) {
            sip_upd_end(&l->guard);
            respond_txn(co, l->srv_txn, 491, NULL, NULL, NULL, 0);
            l->srv_txn = NULL;
            return;
        }
        o->out_txn = NULL;

        char branch[40];
        new_branch(branch, sizeof(branch));
        long cs = ++o->cseq;
        char to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
        snprintf(to, sizeof(to), "%s;tag=%s", o->to, o->rem_tag);
        char ruri[DLG_URI_MAX + 8];
        snprintf(ruri, sizeof(ruri), "%s",
                 o->target[0] ? o->target : o->ruri);
        char out[SIP_MAX_MSG * 2];
        int n = build_dlg_req(co, o, m->method, cs,
                              ruri, to, branch,
                              extra[0] ? extra : NULL,
                              m->body.len ? m->body.p : NULL,
                              m->body.len,
                              out, sizeof(out));
        if (n > 0) {
            o->out_txn = txn_client_new(
                co->cfg.txn, TXN_NICT,
                branch, strlen(branch), m->method,
                &o->dst, o->reliable, out, (size_t)n, cli_tu, c, now_ms());
            if (o->out_txn)
                snprintf(o->last_branch, sizeof(o->last_branch), "%s", branch);
        }
        if (!o->out_txn) {
            sip_upd_end(&l->guard);
            sip_upd_end(&o->guard);
            respond_txn(co, l->srv_txn, 500, NULL, NULL, NULL, 0);
            l->srv_txn = NULL;
            return;
        }
        /* cli_tu 在 o->out_txn 终响应时回 A 腿并释放 guard */
        return;
    }

    /* ---- SUBSCRIBE / NOTIFY (RFC 3265)：对话内订阅/通知，转发到对端 leg ----
     * MESSAGE/INFO 上面已各自处理，SUBSCRIBE/NOTIFY 也用同样的 guard 串行互斥。 */
    if ((m->method == SIP_M_SUBSCRIBE || m->method == SIP_M_NOTIFY)
        && sip_msg_in_dialog(m)) {
        int leg = -1;
        sip_hdr_t *cid = sip_hdr_get(m, SIP_H_CALL_ID);
        b2b_call_t *c = cid ? tbl_find(co, cid->value.p, cid->value.len, &leg) : NULL;
        if (!c || leg < 0) {
            respond_txn(co, t, 481, NULL, NULL, NULL, 0);
            return;
        }
        b2b_leg_t *l = &c->leg[leg];
        int other = leg ^ 1;
        b2b_leg_t *o = &c->leg[other];
        if (o->state != LEG_CONFIRMED && o->state != LEG_EARLY) {
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        if (!sip_upd_begin(&l->guard)) {
            respond_txn(co, t, 491, NULL, NULL, NULL, 0);
            return;
        }
        l->srv_txn = t;
        txn_set_ud(t, c);

        /* 透传 Content-Type + Content-Length（SUBSCRIBE/NOTIFY 带 application/sipxml） */
        char extra[512];
        extra[0] = '\0';
        if (m->body.len && m->body.p) {
            sip_hdr_t *ct = sip_hdr_get(m, SIP_H_CONTENT_TYPE);
            if (ct && ct->value.p && ct->value.len) {
                int n = snprintf(extra, sizeof(extra), "Content-Type: %.*s\r\n",
                                 (int)ct->value.len, ct->value.p);
                if (n > 0 && (size_t)n < sizeof(extra)) {
                    int cl = snprintf(extra + n, sizeof(extra) - (size_t)n,
                                      "Content-Length: %zu\r\n", m->body.len);
                    if (cl > 0) n += cl;
                }
            }
        }

        /* 转发到对端 leg */
        if (!sip_upd_begin(&o->guard)) {
            sip_upd_end(&l->guard);
            respond_txn(co, l->srv_txn, 491, NULL, NULL, NULL, 0);
            l->srv_txn = NULL;
            return;
        }
        o->out_txn = NULL;

        char branch[40];
        new_branch(branch, sizeof(branch));
        long cs = ++o->cseq;
        char to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
        snprintf(to, sizeof(to), "%s;tag=%s", o->to, o->rem_tag);
        char ruri[DLG_URI_MAX + 8];
        snprintf(ruri, sizeof(ruri), "%s",
                 o->target[0] ? o->target : o->ruri);
        char out[SIP_MAX_MSG * 2];
        int n = build_dlg_req(co, o, m->method, cs,
                              ruri, to, branch,
                              extra[0] ? extra : NULL,
                              m->body.len ? m->body.p : NULL,
                              m->body.len,
                              out, sizeof(out));
        if (n > 0) {
            o->out_txn = txn_client_new(
                co->cfg.txn, TXN_NICT,
                branch, strlen(branch), m->method,
                &o->dst, o->reliable, out, (size_t)n, cli_tu, c, now_ms());
            if (o->out_txn)
                snprintf(o->last_branch, sizeof(o->last_branch), "%s", branch);
        }
        if (!o->out_txn) {
            sip_upd_end(&l->guard);
            sip_upd_end(&o->guard);
            respond_txn(co, l->srv_txn, 500, NULL, NULL, NULL, 0);
            l->srv_txn = NULL;
            return;
        }
        /* cli_tu 在 o->out_txn 终响应时回 A 腿并释放 guard */
        return;
    }

    /* 非对话内的 NOTIFY/SUBSCRIBE/PUBLISH：服务端主动发的（如 FS message-summary），
     * 我们作为 B2BUA 不处理语音邮件/presence 等，但必须回 200 OK 表示收到
     * （RFC 3265 §3.2.2 / RFC 3903 §6.1 要求订阅者/接收方回 200）。 */
    if (m->method == SIP_M_NOTIFY || m->method == SIP_M_SUBSCRIBE
        || m->method == SIP_M_PUBLISH
        || m->method == SIP_M_MESSAGE || m->method == SIP_M_INFO) {
        respond_txn(co, t, 200, NULL, NULL, NULL, 0);
        return;
    }

    /* 未识别的非对话内方法 */
    respond_txn(co, t, 501, NULL, NULL, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* 入向总入口（挂 transport rx）                                         */
/* ------------------------------------------------------------------ */
void b2b_on_rx(void *ud, sip_tr_proto_t proto, const nsbc_sa_t *src,
               const nsbc_sa_t *local, const char *buf, size_t len)
{
    b2b_core_t *co = ud;
    sip_msg_t m;
    char src_ip[128] = {0};
    nsbc_sa_fmt(src, src_ip, sizeof(src_ip));
    /* Guard/auto-ban key MUST be the pure source IP (no port).
     * Keying by ip:port scatters one attacker across thousands of entries
     * (a new UDP source port per packet), so the INVITE rate threshold is
     * never reached and ban_is_banned(pure_ip) never matches. */
    char src_gip[INET6_ADDRSTRLEN] = {0};
    sa_ip_str(src, src_gip, sizeof(src_gip));

    /* 存入向本地端口到 __thread 变量（handle_new_invite 深层读） */
    g_rx_local_port = 0;
    if (local && local->ss.ss_family == AF_INET)
        g_rx_local_port = nsbc_sa_port(local);
    llog(LOG_DEBUG, "b2b",
         "b2b_on_rx: local_fam=%d local_port_direct=%u g_rx_local_port=%u",
         local ? local->ss.ss_family : 0,
         local ? nsbc_sa_port(local) : 0,
         g_rx_local_port);

    if (sip_parse_ex(&m, buf, len, SIP_F_COMPAT_ALL) != NSBC_OK) {
        /* 短包 keepalive 检测：
         *   - "\r\n\r\n" (4B) — 标准 SIP NAT keepalive
         *   - 全 \r/\n 的 2–8B 包 — 同类探测
         *   - len<=4 的任何包 — 不可能是有效 SIP，静默 drop
         * 对非纯 CRLF 但短于 16B 的包打 DEBUG hex dump，帮确认 peer 发了什么 */
        if (len <= 4) return;
        if (len <= 16) {
            int all_crlf = 1;
            for (size_t i = 0; i < len; ++i)
                if (buf[i] != '\r' && buf[i] != '\n') { all_crlf = 0; break; }
            if (all_crlf) return;
            /* 不是纯 CRLF 但很短 — 打 DEBUG hex，观察一次即可 */
            unsigned char *hb = (unsigned char *)buf;
            llog(LOG_DEBUG, "b2b",
                 "rx short non-sip len=%zu hex=%02x%02x%02x%02x%02x%02x%02x%02x from %s",
                 len,
                 len > 0 ? hb[0] : 0, len > 1 ? hb[1] : 0,
                 len > 2 ? hb[2] : 0, len > 3 ? hb[3] : 0,
                 len > 4 ? hb[4] : 0, len > 5 ? hb[5] : 0,
                 len > 6 ? hb[6] : 0, len > 7 ? hb[7] : 0,
                 src_ip);
            return;
        }
        llog(LOG_WARN, "b2b", "rx BAD sip from %s len=%zu", src_ip, len);
        return;
    }

    /* SIP Profile ACL 数据面（REQ-SIP-PROFILE-001）：
     * 多端口场景（external=5080 / internal=5060）下，先按入端口匹配 profile
     * ——最高效的快捷路径：从 5080 进来的包直接归属 external profile；
     * 端口没匹配（WS/WSS 桥、端口共享）再 fallback 到源 IP 白名单匹配。
     * 黑名单 → silent drop；白名单有值但无归属 → silent drop（§15 安全加固）。 */
    if (m.is_request) {
        char ip[INET6_ADDRSTRLEN];
        sa_ip_str(src, ip, sizeof(ip));

        /* ---- Phase 1: 归属判定（端口优先，IP 兜底） ---- */
        sip_profile_t *prof = NULL;
        if (local && local->ss.ss_family == AF_INET) {
            uint16_t in_port = nsbc_sa_port(local);
            if (in_port > 0) {
                prof = sip_profile_match_port(
                    (proto == SIP_TR_UDP) ? in_port : 0,
                    (proto != SIP_TR_UDP) ? in_port : 0);
                if (prof)
                    llog(LOG_DEBUG, "b2b", "profile match by port %u → %s",
                         in_port, prof->name);
            }
        }
        if (!prof) {
            prof = sip_profile_match_src(ip, NULL);
            if (prof)
                llog(LOG_DEBUG, "b2b", "profile match by ip → %s", prof->name);
        }

        /* ---- Phase 2: ACL 判定 ---- */
        int verdict = SIP_IP_ACCEPT;
        (void)sip_profile_match_src(ip, &verdict); /* 黑名单/白名单全域检查 */
        if (verdict == SIP_IP_DROP) {
            llog(LOG_INFO, "b2b",
                 "profile ACL drop %.*s from %s (silent)",
                 (int)m.method_str.len, m.method_str.p, ip);
            return;
        }
        if (verdict == SIP_IP_REJECT) {
            llog(LOG_NOTICE, "b2b",
                 "profile ACL reject %.*s from %s (silent, no response)",
                 (int)m.method_str.len, m.method_str.p, ip);
            return;
        }
    }

    if (m.is_request) {

        llog(LOG_DEBUG, "b2b", "-------- rx REQ %.*s uri=%.*s from %s",
             (int)m.method_str.len, m.method_str.p,
             (int)m.ruri_raw.len, m.ruri_raw.p, src_ip);
        /* ★ 完整 SIP request dump（REGISTER/SUBSCRIBE 等也需要看完整消息体） */
        if (m.buf && m.buflen > 0) {
            size_t show = m.buflen > 1024 ? 1024 : m.buflen;
            llog(LOG_DEBUG, "b2b", "★REQ_DUMP %s total=%zu show=%zu",
                 m.method_str.p ? m.method_str.p : "?", m.buflen, show);
            llog(LOG_DEBUG, "b2b", "★REQ[0..%zu]: %.*s",
                 show, (int)show, m.buf);
        }
    } else {
        llog(LOG_DEBUG, "b2b", "----- rx RESP %d %.*s from %s",
             m.status, (int)m.reason.len, m.reason.p, src_ip);
        if (m.buf && m.buflen > 0) {
            size_t show = m.buflen > 1024 ? 1024 : m.buflen;
            llog(LOG_DEBUG, "b2b", "★RESP_DUMP %d %s total=%zu show=%zu",
                 m.status, m.reason.p ? m.reason.p : "?", m.buflen, show);
            llog(LOG_DEBUG, "b2b", "★RESP[0..%zu]: %.*s",
                 show, (int)show, m.buf);
        }
    }

    int reliable = proto != SIP_TR_UDP;
    if (m.is_request) {
        /* ---- FIND-3: method 白名单 reject（未知 method 立即 silent drop）----
         * txn_server_input 不区分已知/未知 method，任何请求都会创建 IST/NIST。
         * 攻击者发 SUBSCRIBE/REFER/自定义 X-FOO 能无限制打 txn slab，
         * 且绕过 P1 主动防护（INVITE_COUNT 之前只对 INVITE/REGISTER 计数）。
         * 这里做白名单 reject：RFC3261 + 常用扩展 = 13 种。 */
        switch (m.method) {
        case SIP_M_INVITE:
        case SIP_M_ACK:
        case SIP_M_BYE:
        case SIP_M_CANCEL:
        case SIP_M_OPTIONS:
        case SIP_M_REGISTER:
        case SIP_M_SUBSCRIBE:
        case SIP_M_NOTIFY:
        case SIP_M_PUBLISH:
        case SIP_M_REFER:
        case SIP_M_INFO:
        case SIP_M_MESSAGE:
        case SIP_M_UPDATE:
        case SIP_M_PRACK:
            break; /* 合法 method，继续处理 */
        default: {
            static unsigned unk_drop_n = 0;
            if (unk_drop_n++ % 512 == 0)
                llog(LOG_WARN, "b2b",
                     "reject unknown method=%d (%.*s) from %s (silent drop)",
                     m.method, (int)m.method_str.len,
                     m.method_str.p ? m.method_str.p : "?", src_ip);
            return;
        }
        }

        /* 主动请求频率计数（per-IP 窗口内次数）——对 ALL 合法 method 计数，
         * 不仅 INVITE/REGISTER。攻击者换 SUBSCRIBE/REFER 也会触发 auto-ban。
         * key 用纯 IP（src_gip）：带端口会把同一攻击者打散成上千条记录，
         * 阈值永远凑不齐，且 ban_is_banned(纯IP) 永远查不到带端口的 key。 */
        b2b_guard_notify(src_gip, B2B_GUARD_INVITE_COUNT);

        llog(LOG_DEBUG, "b2b", "txn_input msg.buf=%p msg.buflen=%zu txn_mgr=%p src_af=%d",
             m.buf, m.buflen, (void *)co->cfg.txn, src->ss.ss_family);
        int rc = txn_server_input(co->cfg.txn, &m, src, reliable, now_ms());
        llog(LOG_DEBUG, "b2b", "txn_srv_input method=%d rc=%d", m.method, rc);
        if (rc == TXN_STATELESS) {
            /* RFC2543 无 cookie / 2xx ACK（新 branch 不匹配 IST） */
            if (m.method == SIP_M_ACK) {
                /* 2xx ACK 对话层吸收（RFC3261 §13.3.1.4）：
                 * 停 A 腿 IST Timer G/H，转 CONFIRMED，记 CDR ANSWER。
                 * 匹配键：Call-ID 查表得 A 腿 + CSeq method=ACK
                 * （2xx ACK 的 CSeq method 由 INVITE 改为 ACK，
                 * 序列号不变——RFC3261 §13.2.4）。 */
                sip_hdr_t *cid = sip_hdr_get(&m, SIP_H_CALL_ID);
                sip_hdr_t *cs = sip_hdr_get(&m, SIP_H_CSEQ);
                if (cid && cs &&
                    sip_hdr_parse_cseq(cs) == NSBC_OK &&
                    cs->u.cseq.method == SIP_M_ACK) {
                    int leg = -1;
                    b2b_call_t *c = tbl_find(co, cid->value.p,
                                             cid->value.len, &leg);
                    if (c && leg == 0) {
                        call_lock(c);
                        /* 首 INVITE 的 server txn 在 inv_txn（srv_txn 是
                         * re-INVITE/MESSAGE/INFO 的延迟响应槽），与
                         * respond_a 相同的 fallback 顺序。 */
                        sip_txn_t *it = c->leg[0].srv_txn
                                      ? c->leg[0].srv_txn
                                      : c->leg[0].inv_txn;
                        if (c->dead || !it ||
                            c->a_final < 200 || c->a_final >= 300) {
                            llog(LOG_WARN, "b2b",
                                 "ack guard fail: dead=%d it=%p a_final=%d "
                                 "callid=%s",
                                 c->dead, (void *)it, c->a_final,
                                 c->leg[0].callid);
                        } else {
                            int rc = txn_ist_absorb_ack(it, now_ms());
                            if (!rc) {
                                /* absorb_ack 因状态不匹配返回 0（竞态 /
                                 * 已 TERMINATED 等），但 Timer H 可能还在
                                 * 32 秒后触发 BYE —— 强制解除以防意外 */
                                txn_ist_force_disarm_h(it);
                                llog(LOG_WARN, "b2b",
                                     "ack absorb_ack failed, force-disarm "
                                     "Timer H callid=%s",
                                     c->leg[0].callid);
                            }
                            c->leg[0].state = LEG_CONFIRMED;
                            /* CDR ANSWER 幂等 */
                            if (c->answer_ms == 0) {
                                c->answer_ms = now_ms();
                                b2b_emit_cdr(c, CDR_PHASE_ANSWER);

                                /* 白皮书 §5.2 + §10.3：ANSWER 后统一发 SESSION_ANSWER 事件
                                 * 三通道：event_handler_trigger（同步订阅链）+ nsbc_event_push（异步 Ring）
                                 * + uds_send_session（UDS data.sock 推 Web 端 calls 表）
                                 *
                                 * REQ-UDS-002：UDS payload 必须含 event/state/from/to/dir/cause，
                                 * Rust 侧 dispatch_session_push 写入 calls 表。缺字段虽有
                                 * Rust 侧空串保护，但正确补齐是规范。*/
                                {
                                    char ans_json[768];
                                    snprintf(ans_json, sizeof(ans_json),
                                        "{\"event\":\"answer\",\"call_id\":\"%s\","
                                        "\"from\":\"%s\",\"to\":\"%s\","
                                        "\"state\":\"confirmed\",\"dir\":\"outbound\",\"cause\":0,"
                                        "\"answer_ms\":%llu,"
                                        "\"codec_a\":\"%s\",\"codec_b\":\"%s\","
                                        "\"active\":%d}",
                                        c->leg[0].callid,
                                        c->leg[0].from,
                                        c->leg[0].ruri,
                                        (unsigned long long)c->answer_ms,
                                        c->codec_a[0] ? c->codec_a : "-",
                                        c->codec_b[0] ? c->codec_b : "-",
                                        c->active);
                                    uint64_t seq = 0;
                                    event_handler_trigger("SESSION_ANSWER", ans_json,
                                                         &c->leg[0].callid, &seq);
                                    nsbc_event_push("SESSION_ANSWER", ans_json);
                                    uds_send_session(ans_json);
                                }
                            }
                            if (c->leg[1].state == LEG_CONFIRMED &&
                                !c->active) {
                                c->active = 1;
                                add_atomic(&co->active, 1);
                                maybe_start_media(c);
                            }
                        }
                        call_unlock(c);
                    } else {
                        llog(LOG_WARN, "b2b",
                             "ack tbl_find fail: c=%p leg=%d callid=%.*s",
                             (void *)c, leg,
                             (int)cid->value.len, cid->value.p);
                    }
                }
                return;
            }
            if (m.method == SIP_M_OPTIONS) {
                char out[SIP_MAX_MSG];
                int n = sip_build_response(
                    out, sizeof(out), &m, 200, NULL, NULL, NULL,
                    "Allow: INVITE,ACK,CANCEL,BYE,OPTIONS\r\n", NULL, 0);
                if (n > 0)
                    sip_tr_send(co->cfg.tr, proto, out, (size_t)n, src);
                return;
            }
            char out[SIP_MAX_MSG];
            int n = sip_build_response(out, sizeof(out), &m, 400, NULL,
                                       NULL, NULL, NULL, NULL, 0);
            if (n > 0)
                sip_tr_send(co->cfg.tr, proto, out, (size_t)n, src);
        }
        /* 事务层分配失败（slab 满 / NOMEM / NOSPC 等负错误码）：
         * 显式静默丢弃 + 上报 security，累计该 IP 的失败次数，超阈值触发 auto-ban。
         * 攻击者的 INVITE/REGISTER flood 打满 slab 后，自己先被封，
         * 正常合法呼叫不受影响（阈值由 security 模块 ratelimit_invites 控制）。 */
        if (rc < 0 && (m.method == SIP_M_INVITE || m.method == SIP_M_REGISTER)) {
            static unsigned fail_drop_n = 0;
            if (fail_drop_n++ % 32 == 0)
                llog(LOG_WARN, "b2b",
                     "txn alloc FAIL rc=%d method=%d ip=%s "
                     "(silent drop, no response, dropped=%u)",
                     rc, m.method, src_ip, fail_drop_n);
            b2b_guard_notify(src_gip, B2B_GUARD_INVITE_FAIL);
        }
        return;
    }

    /* 响应：交客户端事务；无主 2xx 重传（B 腿）补 ACK */
    int rc = txn_client_input(co->cfg.txn, &m, src, reliable, now_ms());
    int matched_invite_2xx = 0; /* 标志：2xx 是 B 腿 INVITE 的正常响应 → 不进 unmatched_resp */
    if (rc == TXN_DROP && m.status >= 200 && m.status < 300) {
        sip_hdr_t *cid = sip_hdr_get(&m, SIP_H_CALL_ID);
        sip_hdr_t *cseq = sip_hdr_get(&m, SIP_H_CSEQ);
        if (cid && cseq && sip_hdr_parse_cseq(cseq) == NSBC_OK &&
            cseq->u.cseq.method == SIP_M_INVITE) {
            int leg = -1;
            b2b_call_t *c = tbl_find(co, cid->value.p, cid->value.len, &leg);
            if (c && leg == 1 && c->leg[1].state == LEG_CONFIRMED) {
                matched_invite_2xx = 1;
                call_lock(c);
                if (!c->dead)
                    send_ack_leg(c, 1);
                call_unlock(c);
            }
        }
    }

    /* 无主响应旁路多槽链：txn_client_input DROP 但可能是我们发出的 UAC 请求的响应
     * （如 outreg REGISTER）。广播给所有注册的 resp hook 模块。
     * 已被 B2BUA INVITE 匹配的 2xx 响应跳过（matched_invite_2xx=1）。 */
    if (rc == TXN_DROP && !matched_invite_2xx && g_resp_hook_n > 0) {
        sip_hdr_t *cid = sip_hdr_get(&m, SIP_H_CALL_ID);
        if (cid && cid->value.p && cid->value.len > 0) {
            char cid_buf[DLG_CALLID_MAX] = {0};
            size_t cid_len = cid->value.len < sizeof(cid_buf) - 1
                                 ? cid->value.len : sizeof(cid_buf) - 1;
            memcpy(cid_buf, cid->value.p, cid_len);
            sip_hdr_t *www = sip_hdr_get(&m, SIP_H_WWW_AUTHENTICATE);
            sip_hdr_t *pauth = sip_hdr_get(&m, SIP_H_PROXY_AUTHENTICATE);
            const char *www_val = NULL;
            if (www && www->value.p && www->value.len > 0) {
                static __thread char www_buf[512] = {0};
                size_t wlen = www->value.len < sizeof(www_buf) - 1
                                  ? www->value.len : sizeof(www_buf) - 1;
                memcpy(www_buf, www->value.p, wlen);
                www_buf[wlen] = '\0';
                www_val = www_buf;
            } else if (pauth && pauth->value.p && pauth->value.len > 0) {
                static __thread char pauth_buf[512] = {0};
                size_t plen = pauth->value.len < sizeof(pauth_buf) - 1
                                  ? pauth->value.len : sizeof(pauth_buf) - 1;
                memcpy(pauth_buf, pauth->value.p, plen);
                pauth_buf[plen] = '\0';
                www_val = pauth_buf;
            }
            pthread_mutex_lock(&g_resp_hook_lock);
            for (int i = 0; i < g_resp_hook_n; ++i)
                g_resp_hooks[i].fn(cid_buf, m.status, www_val, g_resp_hooks[i].ud);
            pthread_mutex_unlock(&g_resp_hook_lock);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 主动外呼（originate）                                                 */
/* ------------------------------------------------------------------ */
int b2b_resolve_dst(b2b_core_t *co, const char *ruri, size_t ruri_len,
                    nsbc_sa_t *dst, char *out_ruri, size_t outsz,
                    char *out_to, size_t out_to_sz)
{
    if (!co || !ruri || !dst) return NSBC_INVAL;
    if (!co->cfg.resolve) return NSBC_ERR;
    return co->cfg.resolve(NULL, ruri, ruri_len, dst, out_ruri, outsz,
                           out_to, out_to_sz, co->cfg.resolve_ud);
}

int b2b_originate(b2b_core_t *co, const b2b_orig_t *o, b2b_call_t **out)
{
    /* HA 门控：备机拒绝控制面外呼（originate 统一入口，队列/CLI/ESL
     * 外呼都走这里）。保持单主语义——备机不产生呼叫/CDR/媒体，
     * 避免主备各自外呼导致话务分裂。 */
    if (ha_role() != HA_ROLE_ACTIVE) {
        llog(LOG_WARN, "b2b", "HA standby: originate refused");
        return NSBC_PERM_DENIED;
    }

    /* REQ-ARCH-015 并发硬限制：超限返回 NOSPC */
    if (!concurrent_try_inc(co)) {
        llog(LOG_WARN, "b2b", "concurrent_call_limit reached (%u), originate NOSPC",
             co->cfg.max_concurrent_calls);
        return NSBC_NOSPC;
    }

    /* b2b_call_t 预分配（bridge 模式用同一 call_t 承载双腿） */
    b2b_call_t *c = mem_slab_alloc_direct(co->slab);
    if (!c) {
        concurrent_dec(co);
        return NSBC_NOSPC;
    }
    memset(c, 0, sizeof(*c));
    c->core = co;
    sip_upd_guard_init(&c->leg[0].guard);
    sip_upd_guard_init(&c->leg[1].guard);

    /* REQ-ORIGINATE-001：per-call noanswer override（>0 覆盖全局，否则走全局）
     * 在 alloc 之后立即设，方便 bridge 模式两次 arm_noanswer_timer 复用 */
    c->noanswer_ms_override = o->timeout_ms;
    /* REQ-ORIGINATE-003 待补：携带尾随 app（&park/&hangup/&echo）到呼叫上下文 */
    c->post_app = o->post_app;

    /* REQ-ORIGINATE-001：orig_caller_id 覆盖 from_user（主叫号）
     * orig_caller_id_name → From 显示名（Caller-Id name） */
    const char *caller = (o->orig_caller_id && o->orig_caller_id[0])
                         ? o->orig_caller_id
                         : o->from_user;
    const char *cname = (o->orig_caller_id_name && o->orig_caller_id_name[0])
                        ? o->orig_caller_id_name : NULL;

    /* ==================================================================
     * 模式 1：单腿外呼（bridge_url == NULL）—— 原有逻辑
     * ==================================================================*/
    if (!o->bridge_url || !o->bridge_url[0]) {
        b2b_leg_t *b = &c->leg[1];
        b->state = LEG_CALLING;
        b->proto = SIP_TR_UDP;
        b->dst = o->dst;
        if (nsbc_sa_port(&b->dst) == 0)
            nsbc_sa_set_port(&b->dst, 5060);
        nsbc_rand_token(b->loc_tag, sizeof(b->loc_tag), 16);

        /* From 头：有显示名 → "\"name\" <sip:user@host>"；否则退化为 user@host */
        if (cname) {
            snprintf(b->from, sizeof(b->from), "\"%s\" <sip:%s@%s>;tag=%s",
                     cname, caller ? caller : "", co->cfg.local_host, b->loc_tag);
        } else {
            snprintf(b->from, sizeof(b->from), "\"%s\" <sip:%s@%s>;tag=%s",
                     caller ? caller : "", caller ? caller : "",
                     co->cfg.local_host, b->loc_tag);
        }
        snprintf(b->to, sizeof(b->to), "%s", o->to_uri);
        snprintf(b->ruri, sizeof(b->ruri), "%s", o->to_uri);
        snprintf(b->target, sizeof(b->target), "%s", o->to_uri);
        nsbc_callid_make(b->callid, sizeof(b->callid), co->cfg.local_host);
        b->cseq = 1;
        if (o->sdp_len) {
            if (o->sdp_len > B2B_SDP_MAX) {
                call_destroy(c);
                return NSBC_NOSPC;
            }
            memcpy(b->offer, o->sdp, o->sdp_len);
            b->offer_len = o->sdp_len;
        }
        b->dlg = dlg_uac_new(co->cfg.dlg, b->callid, strlen(b->callid),
                             b->loc_tag, strlen(b->loc_tag), b->target,
                             b->cseq, SIP_M_INVITE);
        if (b->dlg)
            b->dlg->ud = c;
        tbl_link(co, c, 1);
        add_atomic(&co->total, 1);

        b->inv_txn = send_invite_b(c, 1, b->offer, b->offer_len, 0, NULL);
        if (!b->inv_txn) {
            call_destroy(c);
            return NSBC_ERR;
        }

        arm_noanswer_timer(c);
        {
            char sess[512];
            snprintf(sess, sizeof(sess),
                     "{\"event\":\"create\",\"call_id\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
                     "\"dir\":\"outbound\",\"state\":\"calling\"}",
                     b->callid, b->from, b->to[0] ? b->to : b->ruri);
            uds_send_session(sess);
        }

        /* REQ-ORIGINATE-003 &park / &hangup / &echo 后置 app —— 等 B 腿 answer 后执行。
         * 在 OAM 线程执行（b2b_hold_call/b2b_terminate 自身加锁，安全）。
         * &echo 现在有环回媒体实现：设置 relay->echo_mode=1，让两侧各自
         * 环回自己的 RTP peer（side→side），通话方听到自己回声。 */
        if (o->post_app == B2B_APP_PARK || o->post_app == B2B_APP_HANGUP
            || o->post_app == B2B_APP_ECHO) {
            int wait_ms = (o->timeout_ms > 0) ? o->timeout_ms : 60000;
            int waited = 0;
            while (waited < wait_ms) {
                if (co->stopping) { call_destroy(c); return NSBC_ERR; }
                if (b->state == LEG_CONFIRMED) break;
                if (b->state == LEG_TERMINATED || b->state == LEG_TERMINATING) {
                    llog(LOG_INFO, "b2b", "originate post-app: B leg failed state=%d",
                         b->state);
                    call_destroy(c);
                    return NSBC_ERR;
                }
                usleep(20000); waited += 20;
            }
            if (b->state != LEG_CONFIRMED) {
                llog(LOG_WARN, "b2b", "originate post-app timeout waited=%dms", waited);
                call_destroy(c);
                return NSBC_TIMEOUT;
            }
            if (o->post_app == B2B_APP_PARK) {
                int hrc = b2b_hold_call(c, -1, 1, NULL, 0);
                llog(LOG_INFO, "b2b", "originate &park applied rc=%d callid=%s",
                     hrc, b->callid);
            } else if (o->post_app == B2B_APP_HANGUP) {
                llog(LOG_INFO, "b2b", "originate &hangup terminating callid=%s", b->callid);
                b2b_terminate(c, 0);
            } else { /* B2B_APP_ECHO */
                nsbc_ctx_t *ctx = nsbc_get_ctx();
                if (ctx && ctx->bridge) {
                    bridge_mgr_t *m = (bridge_mgr_t *)ctx->bridge;
                    rtp_relay_t *relay = NULL;
                    nsbc_sa_t peer_unused[2];
                    /* REQ-ORIGINATE-003 race 修复：WebRTC/DTLS 呼叫 LEG_CONFIRMED
                     * 到了但 relay 还没建好（async setup），加重试循环。
                     * 每 50ms 一次，最多 2s —— DTLS 通常 200-500ms 内就绪。 */
                    int lrc = NSBC_NOENT;
                    for (int retry = 0; retry < 40; retry++) {
                        lrc = bridge_lookup_relay(m, c, &relay, peer_unused);
                        if (lrc == NSBC_OK && relay) break;
                        if (co->stopping) break;
                        usleep(50000); /* 50ms */
                    }
                    if (lrc == NSBC_OK && relay) {
                        rtp_relay_set_echo(relay, 1);
                        llog(LOG_INFO, "b2b",
                             "originate &echo loopback ON callid=%s", b->callid);
                    } else {
                        llog(LOG_WARN, "b2b",
                             "originate &echo bridge_lookup_relay failed rc=%d "
                             "(relay not yet built or bypass mode), callid=%s",
                             lrc, b->callid);
                    }
                } else {
                    llog(LOG_WARN, "b2b",
                         "originate &echo no bridge_mgr (media bridge not loaded) callid=%s",
                         b->callid);
                }
                /* echo 模式：保持呼叫在线，不 park/hangup */
            }
        }

        if (out)
            *out = c;
        return NSBC_OK;
    }

    /* ==================================================================
     * 模式 2：bridge 顺序外呼 —— leg[0] 先呼主 dest，等 answer 后 leg[1] 呼 bridge_url
     * ==================================================================*/
    llog(LOG_INFO, "b2b", "originate bridge mode: dest=%s bridge=%s",
         o->to_uri, o->bridge_url);

    b2b_leg_t *a = &c->leg[0];
    b2b_leg_t *b = &c->leg[1];

    /* ---- A 腿（主 dest） ---- */
    a->state = LEG_CALLING;
    a->proto = SIP_TR_UDP;
    a->dst = o->dst;
    if (nsbc_sa_port(&a->dst) == 0)
        nsbc_sa_set_port(&a->dst, 5060);
    nsbc_rand_token(a->loc_tag, sizeof(a->loc_tag), 16);
    if (cname) {
        snprintf(a->from, sizeof(a->from), "\"%s\" <sip:%s@%s>;tag=%s",
                 cname, caller ? caller : "", co->cfg.local_host, a->loc_tag);
    } else {
        snprintf(a->from, sizeof(a->from), "\"%s\" <sip:%s@%s>;tag=%s",
                 caller ? caller : "", caller ? caller : "",
                 co->cfg.local_host, a->loc_tag);
    }
    snprintf(a->to, sizeof(a->to), "%s", o->to_uri);
    snprintf(a->ruri, sizeof(a->ruri), "%s", o->to_uri);
    snprintf(a->target, sizeof(a->target), "%s", o->to_uri);
    nsbc_callid_make(a->callid, sizeof(a->callid), co->cfg.local_host);
    a->cseq = 1;
    a->dlg = dlg_uac_new(co->cfg.dlg, a->callid, strlen(a->callid),
                         a->loc_tag, strlen(a->loc_tag), a->target,
                         a->cseq, SIP_M_INVITE);
    if (a->dlg) a->dlg->ud = c;
    tbl_link(co, c, 0);
    add_atomic(&co->total, 1);

    arm_noanswer_timer(c);
    a->inv_txn = send_invite_b(c, 0, a->offer, a->offer_len, 0, NULL);
    if (!a->inv_txn) {
        llog(LOG_WARN, "b2b", "bridge A leg INVITE send failed");
        call_destroy(c);
        return NSBC_ERR;
    }

    /* ---- 阻塞等 A 腿 answer（最长 timeout_ms，默认 60s） ----
     * 线程安全：cli_tu 回调在 SIP 线程里改 state，我们在 OAM 线程里轮询，
     * state 是 leg_state_t（整数），原子读写（x86）。
     * 停机：b2b_core_destroy 置 co->stopping，循环每 20ms 检测一次立即退出，
     * 避免 shutdown 时在此挂住最长 timeout_ms（默认 60s）。 */
    int wait_ms = (o->timeout_ms > 0) ? o->timeout_ms : 60000;
    int waited = 0;
    while (waited < wait_ms) {
        if (co->stopping) {
            llog(LOG_INFO, "b2b", "bridge A leg wait aborted: core stopping");
            call_destroy(c);
            return NSBC_ERR;
        }
        if (a->state == LEG_CONFIRMED) break;
        if (a->state == LEG_TERMINATED || a->state == LEG_TERMINATING) {
            llog(LOG_INFO, "b2b", "bridge A leg failed state=%d waited=%dms",
                 a->state, waited);
            call_destroy(c);
            return (a->state == LEG_TERMINATED) ? NSBC_ERR : NSBC_TIMEOUT;
        }
        usleep(20000); waited += 20;
    }
    if (a->state != LEG_CONFIRMED) {
        llog(LOG_WARN, "b2b", "bridge A leg timeout waited=%dms state=%d",
             waited, a->state);
        call_destroy(c);
        return NSBC_TIMEOUT;
    }
    llog(LOG_INFO, "b2b", "bridge A leg answered in %dms, callid=%s",
         waited, a->callid);

    /* ---- B 腿（bridge_url）—— 复用 A 腿 From 头 ---- */
    b->state = LEG_CALLING;
    b->proto = a->proto;
    b->dst = o->bridge_dst;
    if (nsbc_sa_port(&b->dst) == 0)
        nsbc_sa_set_port(&b->dst, 5060);
    nsbc_rand_token(b->loc_tag, sizeof(b->loc_tag), 16);
    /* B 腿 From 头复用 A 腿（相同主叫号/显示名，新 tag） */
    if (cname) {
        snprintf(b->from, sizeof(b->from), "\"%s\" <sip:%s@%s>;tag=%s",
                 cname, caller ? caller : "", co->cfg.local_host, b->loc_tag);
    } else {
        snprintf(b->from, sizeof(b->from), "\"%s\" <sip:%s@%s>;tag=%s",
                 caller ? caller : "", caller ? caller : "",
                 co->cfg.local_host, b->loc_tag);
    }
    snprintf(b->to, sizeof(b->to), "%s", o->bridge_url);
    snprintf(b->ruri, sizeof(b->ruri), "%s", o->bridge_url);
    snprintf(b->target, sizeof(b->target), "%s", o->bridge_url);
    nsbc_callid_make(b->callid, sizeof(b->callid), co->cfg.local_host);
    b->cseq = 1;
    b->dlg = dlg_uac_new(co->cfg.dlg, b->callid, strlen(b->callid),
                         b->loc_tag, strlen(b->loc_tag), b->target,
                         b->cseq, SIP_M_INVITE);
    if (b->dlg) b->dlg->ud = c;
    tbl_link(co, c, 1);

    /* ★★★ bridge 模式 SDP 关键修复（REQ-ORIGINATE-003）★★★
     *
     * bridge 模式里：
     *   1. A 腿（leg[0]）是 UAC 发 INVITE —— 没带 offer → leg[0].offer_len=0
     *      但 A 腿收到 200 OK answer → leg[0].answer 有 SDP
     *   2. B 腿（leg[1]）是我们再发起的 INVITE —— 必须带 SDP offer
     *      FreeSWITCH 语义：B 腿 offer = A 腿 answer（二次 offer/answer 交换）
     *   3. maybe_start_media 检查 leg[0].offer_len 和 leg[1].answer_len 非零才桥接
     *
     * 所以：把 leg[0].answer 同时填到 leg[0].offer 和 leg[1].offer
     *       → B 腿 INVITE 带 offer → leg[1] 收到 200 OK → leg[1].answer 有 SDP
     *       → maybe_start_media 触发 → 媒体桥接成功
     */
    if (a->answer_len && a->answer_len <= B2B_SDP_MAX) {
        /* B 腿 INVITE 必须带 SDP offer */
        memcpy(b->offer, a->answer, a->answer_len);
        b->offer_len = a->answer_len;
        /* A 腿 offer 也要有值（maybe_start_media 检查）—— 同一份 answer */
        if (a->offer_len == 0) {
            memcpy(a->offer, a->answer, a->answer_len);
            a->offer_len = a->answer_len;
        }
        llog(LOG_INFO, "b2b", "bridge SDP wired: B-offer len=%zu, A-offer len=%zu",
             b->offer_len, a->offer_len);
    } else {
        llog(LOG_WARN, "b2b", "bridge A leg has no SDP answer — B leg INVITE will lack offer");
    }

    /* B 腿重新 arm timer（bridge 模式下第二次外呼，用 timeout_ms 覆盖） */
    arm_noanswer_timer(c);
    b->inv_txn = send_invite_b(c, 1, b->offer, b->offer_len, 0, NULL);
    if (!b->inv_txn) {
        llog(LOG_WARN, "b2b", "bridge B leg INVITE send failed");
        call_destroy(c);
        return NSBC_ERR;
    }

    llog(LOG_INFO, "b2b", "bridge B leg INVITE sent callid=%s", b->callid);
    if (out)
        *out = c;
    return NSBC_OK;
}

int b2b_terminate(b2b_call_t *c, int cause_code)
{
    (void)cause_code;
    if (!c)
        return NSBC_INVAL;
    call_lock(c);
    if (c->dead) {
        call_unlock(c);
        return NSBC_NOENT;
    }

    /* ★ 终止顺序倒转：先发 A 腿（主叫/WebRTC/WSS，通常慢速）再 B 腿（被叫/UDP，通常快速）。
     * 原顺序先发 B 腿 → B 腿 BYE 200 OK 快回 → cli_tu 触发 call_destroy → dialog 提前 close。
     * 虽然 send_bye_leg 已经同步发出 BYE 入 txn 队列，call_destroy 不会阻止已发消息到网络，
     * 但倒转顺序让慢腿先发，对端先收 BYE 结束通话，避免"bleg 挂断了 aleg 还在"的时序观感。 */

    int a_path = 0, b_path = 0;   /* 0=skip, 1=BYE, 2=CANCEL, 3=487 */
    if (c->leg[0].state == LEG_CONFIRMED) {
        send_bye_leg(c, 0);          a_path = 1;
    } else if (c->leg[0].state == LEG_CALLING ||
               c->leg[0].state == LEG_EARLY) {
        send_cancel_req(c, 0);       a_path = 2;
    } else if (c->leg[0].callid[0] && !c->a_final) {
        respond_a(c, 487, NULL, 0);  a_path = 3;
        c->a_final = 487;
    }

    if (c->leg[1].state == LEG_CONFIRMED) {
        send_bye_leg(c, 1);          b_path = 1;
    } else if (c->leg[1].state == LEG_CALLING ||
               c->leg[1].state == LEG_EARLY) {
        send_cancel_req(c, 1);       b_path = 2;
    }

    llog(LOG_INFO, "b2b",
         "b2b_terminate: cause=%d a_path=%d b_path=%d s0=%d s1=%d",
         cause_code, a_path, b_path,
         c->leg[0].state, c->leg[1].state);
    call_unlock(c);
    return NSBC_OK;
}

int b2b_terminate_callid(b2b_core_t *co, const char *callid, size_t len,
                         int cause_code)
{
    if (!co || !callid || len == 0 || len >= DLG_CALLID_MAX)
        return NSBC_INVAL;
    b2b_call_t *c = tbl_find(co, callid, len, NULL);
    if (!c)
        return NSBC_NOENT;
    return b2b_terminate(c, cause_code);
}

leg_state_t b2b_leg_state(b2b_call_t *c, int b_leg)
{
    if (!c || b_leg < 0 || b_leg > 1)
        return LEG_IDLE;
    return c->leg[b_leg].state;
}

int b2b_respond_leg(b2b_call_t *c, int b_leg, int status,
                    const char *extra, const char *body, size_t bodylen)
{
    (void)extra;
    if (!c || b_leg < 0 || b_leg > 1)
        return NSBC_INVAL;
    b2b_leg_t *l = &c->leg[b_leg];
    if (l->srv_txn)
        return respond_txn(c->core, l->srv_txn, status, NULL, NULL, body,
                           bodylen);
    return NSBC_NOENT;
}

uint64_t b2b_active_calls(b2b_core_t *c)
{
    return c ? __sync_add_and_fetch(&c->active, 0) : 0;
}
uint64_t b2b_total_calls(b2b_core_t *c)
{
    return c ? __sync_add_and_fetch(&c->total, 0) : 0;
}
uint64_t b2b_failed_calls(b2b_core_t *c)
{
    return c ? __sync_add_and_fetch(&c->failed, 0) : 0;
}

/* ------------------------------------------------------------------ */
/* 在线诊断快照（REQ-OAM-007 / REQ-OBS-010，OAM 低频路径）              */
/* ------------------------------------------------------------------ */

/* 媒体层无此呼叫记录（未确认/已释放/无回调）：五字段整行 UNAVAILABLE。
 * bypass Leg 有 mode/gate 真值但 SSRC/VQ 逐字段 UNAVAILABLE（设备不过媒体，
 * REQ-OBS-010 严禁估算）。 */
#define DUMP_UNAVAIL \
    "mode=UNAVAILABLE gate=UNAVAILABLE ssrc_in=UNAVAILABLE " \
    "ssrc_out=UNAVAILABLE vq=UNAVAILABLE"

static const char *leg_state_name(leg_state_t s)
{
    switch (s) {
    case LEG_IDLE:        return "IDLE";
    case LEG_CALLING:     return "CALLING";
    case LEG_EARLY:       return "EARLY";
    case LEG_CONFIRMED:   return "CONFIRMED";
    case LEG_TERMINATING: return "TERMINATING";
    case LEG_TERMINATED:  return "TERMINATED";
    }
    return "?";
}

static const char *proto_name(sip_tr_proto_t p)
{
    switch (p) {
    case SIP_TR_UDP: return "udp";
    case SIP_TR_TCP: return "tcp";
    case SIP_TR_TLS: return "tls";
    case SIP_TR_WS:  return "ws";
    case SIP_TR_WSS: return "wss";
    }
    return "?";
}

/* 网络来的 Call-ID/tag 可能含空格/控制字符：只回显可见 ASCII，否则 '?'，
 * 杜绝响应帧注入；空值回显 '-'。 */
static const char *safe_tok(const char *s)
{
    if (!s[0])
        return "-";
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p < 0x21 || *p > 0x7e)
            return "?";
    }
    return s;
}

/* 有界格式化拼接：空间不足置 pos=cap 并返回 NSBC_NOSPC。 */
static int buf_put(char *out, size_t cap, size_t *pos, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*pos >= cap)
        return NSBC_NOSPC;
    va_start(ap, fmt);
    n = vsnprintf(out + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n < 0)
        return NSBC_ERR;
    if ((size_t)n >= cap - *pos) {
        *pos = cap;
        return NSBC_NOSPC;
    }
    *pos += (size_t)n;
    return NSBC_OK;
}

/* 有界原始字节拼接（长度分帧的 SDP 段用）。 */
static int buf_raw(char *out, size_t cap, size_t *pos, const char *p, size_t n)
{
    if (n > cap - *pos)
        return NSBC_NOSPC;
    memcpy(out + *pos, p, n);
    *pos += n;
    return NSBC_OK;
}

/* 单 Leg 快照（持 call 锁时填充；含 SDP 故较大，OAM 栈可接受）。 */
typedef struct {
    int have;
    leg_state_t state;
    sip_tr_proto_t proto;
    int reliable;
    long cseq;
    int nroutes;
    char callid[DLG_CALLID_MAX];
    char loc_tag[DLG_TAG_MAX];
    char rem_tag[DLG_TAG_MAX];
    char offer[B2B_SDP_MAX];
    size_t offer_len;
    char answer[B2B_SDP_MAX];
    size_t answer_len;
    b2b_media_view_t mv; /* W5：持 call 锁经 media_view 回调拍照 */
} leg_snap_t;

static void snap_leg(const b2b_leg_t *l, leg_snap_t *s)
{
    memset(s, 0, sizeof(*s));
    s->have = l->callid[0] != '\0';
    s->state = l->state;
    s->proto = l->proto;
    s->reliable = l->reliable;
    s->cseq = l->cseq;
    s->nroutes = l->nroutes;
    snprintf(s->callid, sizeof(s->callid), "%s", l->callid);
    snprintf(s->loc_tag, sizeof(s->loc_tag), "%s", l->loc_tag);
    snprintf(s->rem_tag, sizeof(s->rem_tag), "%s", l->rem_tag);
    if (l->offer_len) {
        s->offer_len = l->offer_len < sizeof(s->offer)
                           ? l->offer_len : sizeof(s->offer);
        memcpy(s->offer, l->offer, s->offer_len);
    }
    if (l->answer_len) {
        s->answer_len = l->answer_len < sizeof(s->answer)
                            ? l->answer_len : sizeof(s->answer);
        memcpy(s->answer, l->answer, s->answer_len);
    }
}

static const char *media_mode_name(b2b_media_mode_t m)
{
    switch (m) {
    case B2B_M_BYPASS:    return "BYPASS";
    case B2B_M_RELAY:     return "RELAY";
    case B2B_M_TRANSCODE: return "TRANSCODE";
    }
    return "?";
}

/* 媒体行渲染（帧格式不变：leg N media mode=.. gate=.. ssrc_in=..
 * ssrc_out=.. vq=..；vq 单 token，斜杠分隔 fl/lost/jit）。 */
static int render_media(char *out, size_t outsz, size_t *pos, int leg,
                        const b2b_media_view_t *mv)
{
    if (!mv->valid)
        return buf_put(out, outsz, pos, "leg %d media %s\n", leg,
                       DUMP_UNAVAIL);
    char sin[16], sout[16], cname[64], vq[40];
    if (mv->have_ssrc_in)
        snprintf(sin, sizeof(sin), "%08x", mv->ssrc_in);
    else
        snprintf(sin, sizeof(sin), "UNAVAILABLE");
    if (mv->have_ssrc_out)
        snprintf(sout, sizeof(sout), "%08x", mv->ssrc_out);
    else
        snprintf(sout, sizeof(sout), "UNAVAILABLE");
    /* REQ-MEDIA-SEC-003⑤ P0：CNAME（SDES）↔入向 SSRC 绑定 */
    if (mv->have_cname && mv->cname[0])
        snprintf(cname, sizeof(cname), "%s", mv->cname);
    else
        snprintf(cname, sizeof(cname), "UNAVAILABLE");
    if (mv->have_vq)
        snprintf(vq, sizeof(vq), "fl=%u/lost=%u/jit=%u",
                 (unsigned)mv->vq_fraction, (unsigned)mv->vq_lost,
                 (unsigned)mv->vq_jitter);
    else
        snprintf(vq, sizeof(vq), "UNAVAILABLE");
    /* 前 5 个 key 保持 DUMP_UNAVAIL 的 mode/gate/ssrc_in/ssrc_out/vq 序，
     * Web 现有解析零改兼容；cname/unauthorized/collision 为 P0 增量。
     * unauthorized ≈ ssrc_unauthorized（白名单外陌生 SSRC 注入丢弃数，
     * 取 ssrc_policy.inject_drops 近似）；collision 为 end 侧 SSRC 与对端
     * 撞号后自恢复次数。 */
    return buf_put(
        out, outsz, pos,
        "leg %d media mode=%s gate=%s ssrc_in=%s cname=%s ssrc_out=%s "
        "vq=%s unauthorized=%llu collision=%llu\n",
        leg, media_mode_name(mv->mode), mv->gate_open ? "OPEN" : "LOCKED",
        sin, cname, sout, vq,
        (unsigned long long)mv->ssrc_inject_drops,
        (unsigned long long)mv->ssrc_collision_drops);
}

int b2b_list_calls(b2b_core_t *co, char *out, size_t outsz)
{
    if (!co || !out || outsz == 0)
        return NSBC_INVAL;
    size_t pos = 0;
    unsigned shown = 0;
    int truncated = 0;
    /* 单调代数去重：一个呼叫经 A/B 两条 link 出现在两个分片。
     * 锁序恒为 shard→call（tbl_link/unlink 从不反向持锁）。 */
    uint32_t gen = (uint32_t)__sync_add_and_fetch(&co->dump_gen, 1);
    /* 单行上限：2×DLG_CALLID_MAX(80) + caller/callee(96) + start/ans/dur(60)
     * + codec 成对(64) + VQ loss/jitter/mos + 键名/状态名/空白 ≈ 640；
     * 预留 768，保证任何截断点之后尾行（≤32B）必能完整落帧。 */
    const size_t row_reserve = 768;
    for (int si = 0; si < B2B_CALL_SHARDS; ++si) {
        call_shard_t *sh = &co->sh[si];
        pthread_mutex_lock(&sh->lock);
        for (call_link_t *p = sh->head; p; p = p->next) {
            b2b_call_t *c = p->call;
            call_lock(c);
            if (c->dead || c->dump_mark == gen) {
                call_unlock(c);
                continue;
            }
            if (outsz - pos < row_reserve) {
                /* 不足一整行+尾行：停在此处，尾行仍完整，帧可解析 */
                call_unlock(c);
                truncated = 1;
                break;
            }
            c->dump_mark = gen;
            const char *a_id = safe_tok(c->leg[0].callid);
            const char *b_id = safe_tok(c->leg[1].callid);
            const char *codec_a = c->codec_a[0] ? c->codec_a : "-";
            const char *codec_b = c->codec_b[0] ? c->codec_b : "-";
            /* uptime → wall 转换（同 emit CDR） */
            uint64_t now_mon = nsbc_now_ms();
            uint64_t now_wall = nsbc_wall_ms();
            uint64_t sm = c->start_ms ? c->start_ms : now_mon;
            uint64_t start_wall = now_wall - (now_mon - sm);
            uint64_t ans_wall = 0;
            if (c->answer_ms)
                ans_wall = now_wall - (now_mon - c->answer_ms);
            uint64_t dur_ms = start_wall ? (now_wall - start_wall) : 0;
            int rc = buf_put(
                out, outsz, &pos,
                "call a_callid=%s a_state=%s b_callid=%s b_state=%s "
                "caller=%s callee=%s "
                "start_ms=%llu answer_ms=%llu duration_ms=%llu "
                "codec_a=%s codec_b=%s "
                "rtp_loss_pct=%u rtp_jitter_ms=%u mos_x1000=%d\n",
                c->leg[0].callid[0] ? a_id : "-",
                leg_state_name(c->leg[0].state),
                c->leg[1].callid[0] ? b_id : "-",
                leg_state_name(c->leg[1].state),
                c->orig_number[0] ? c->orig_number : "-",
                c->dst_number[0]  ? c->dst_number  : "-",
                (unsigned long long)start_wall,
                (unsigned long long)ans_wall,
                (unsigned long long)dur_ms,
                codec_a, codec_b,
                (unsigned)c->rtp_loss_pct,
                (unsigned)c->rtp_jitter_ms,
                (int)c->mos_x1000);
            call_unlock(c);
            if (rc == NSBC_NOSPC) {
                truncated = 1;
                break;
            }
            ++shown;
        }
        pthread_mutex_unlock(&sh->lock);
        if (truncated)
            break;
    }
    /* 汇总在尾行（行数先渲染后才知道，避免两遍持锁）；缓冲不足时
     * truncated=1，channels 为实际写出的呼叫行数。 */
    buf_put(out, outsz, &pos, "channels=%u truncated=%d\n", shown, truncated);
    return NSBC_OK;
}

int b2b_dump_call(b2b_core_t *co, const char *callid, size_t len,
                  char *out, size_t outsz)
{
    if (!co || !callid || len == 0 || len >= DLG_CALLID_MAX || !out ||
        outsz == 0)
        return NSBC_INVAL;
    b2b_call_t *c = tbl_find(co, callid, len, NULL);
    if (!c)
        return NSBC_NOENT;

    leg_snap_t s[2];
    call_lock(c);
    if (c->dead) {
        call_unlock(c);
        return NSBC_NOENT;
    }
    snap_leg(&c->leg[0], &s[0]);
    snap_leg(&c->leg[1], &s[1]);
    /* 媒体视图必须在 call 锁内拍：回调经 call_key 查媒体表，呼叫存活
     * 保证 relay 记录有效；NOENT/无回调 → mv.valid=0 整行 UNAVAILABLE。
     * Phase 0：media_view 从 cfg 单槽改为全局多槽链 g_mv_hooks。 */
    if (g_mv_hook_n > 0) {
        pthread_mutex_lock(&g_mv_hook_lock);
        for (int i = 0; i < 2; ++i) {
            if (!s[i].have) continue;
            int any_ok = 0;
            for (int k = 0; k < g_mv_hook_n; ++k) {
                if (g_mv_hooks[k].fn(c, i, &s[i].mv, g_mv_hooks[k].ud) == NSBC_OK)
                    any_ok = 1;
            }
            if (!any_ok)
                memset(&s[i].mv, 0, sizeof(s[i].mv));
        }
        pthread_mutex_unlock(&g_mv_hook_lock);
    }
    call_unlock(c);

    size_t pos = 0;
    int rc = buf_put(
        out, outsz, &pos, "call a_callid=%s a_state=%s b_callid=%s b_state=%s\n",
        s[0].have ? safe_tok(s[0].callid) : "-",
        s[0].have ? leg_state_name(s[0].state) : "-",
        s[1].have ? safe_tok(s[1].callid) : "-",
        s[1].have ? leg_state_name(s[1].state) : "-");
    if (rc != NSBC_OK)
        return rc == NSBC_NOSPC ? NSBC_NOSPC : NSBC_ERR;

    for (int i = 0; i < 2; ++i) {
        if (!s[i].have)
            continue;
        rc = buf_put(out, outsz, &pos,
                     "leg %d state=%s proto=%s reliable=%d cseq=%ld routes=%d\n",
                     i, leg_state_name(s[i].state), proto_name(s[i].proto),
                     s[i].reliable, s[i].cseq, s[i].nroutes);
        if (rc != NSBC_OK)
            return NSBC_NOSPC;
        rc = buf_put(out, outsz, &pos, "leg %d tags local=%s remote=%s\n", i,
                     safe_tok(s[i].loc_tag), safe_tok(s[i].rem_tag));
        if (rc != NSBC_OK)
            return NSBC_NOSPC;
        /* REQ-MEDIA-SEC-003⑤/REQ-OBS-010：mode/gate/SSRC 映射/VQ 真值；
         * 无媒体记录或 bypass Leg 的缺失字段逐值 UNAVAILABLE（禁估算）。 */
        rc = render_media(out, outsz, &pos, i, &s[i].mv);
        if (rc != NSBC_OK)
            return NSBC_NOSPC;
        if (s[i].offer_len) {
            rc = buf_put(out, outsz, &pos, "leg %d sdp_offer %zu\n", i,
                         s[i].offer_len);
            if (rc != NSBC_OK)
                return NSBC_NOSPC;
            if (buf_raw(out, outsz, &pos, s[i].offer, s[i].offer_len) !=
                    NSBC_OK ||
                buf_put(out, outsz, &pos, "\n") != NSBC_OK)
                return NSBC_NOSPC;
        }
        if (s[i].answer_len) {
            rc = buf_put(out, outsz, &pos, "leg %d sdp_answer %zu\n", i,
                         s[i].answer_len);
            if (rc != NSBC_OK)
                return NSBC_NOSPC;
            if (buf_raw(out, outsz, &pos, s[i].answer, s[i].answer_len) !=
                    NSBC_OK ||
                buf_put(out, outsz, &pos, "\n") != NSBC_OK)
                return NSBC_NOSPC;
        }
    }
    return NSBC_OK;
}

/* ==================================================================
 * REQ-CCMD-001 呼叫级原语：Call-ID 白名单 + 定位（全函数共用）
 * ==================================================================*/

int b2b_valid_callid(const char *s, size_t len)
{
    if (!s || len == 0 || len >= DLG_CALLID_MAX)
        return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x21 || ch > 0x7e)
            return 0;
    }
    return 1;
}

b2b_call_t *b2b_find_callid(b2b_core_t *co, const char *callid, size_t len)
{
    if (!co || !b2b_valid_callid(callid, len))
        return NULL;
    return tbl_find(co, callid, len, NULL);
}

/* ==================================================================
 * uuid_answer（REQ-CCMD-001 + REQ-DP-APP-001 共用）
 *   answer=200 OK：对早期腿发 200；已 confirm/终止腿返回 NOENT。
 *   P0 版本：优先 A 腿（入呼侧总是可 answer 的腿），fallback B 腿。
 * ==================================================================*/

int b2b_answer_call(b2b_call_t *call, int b_leg,
                    const char *sdp, size_t sdp_len)
{
    if (!call)
        return NSBC_INVAL;
    int leg[2] = {0, 1}, nleg = 1;
    if (b_leg >= 0 && b_leg <= 1) {
        leg[0] = b_leg;
        nleg = 1;
    } else {
        /* 自动：优先 A 腿（0） */
        nleg = 2;
    }
    b2b_call_t *c = call;
    for (int i = 0; i < nleg; ++i) {
        int l = leg[i];
        call_lock(c);
        if (c->dead || c->leg[l].state == LEG_TERMINATED ||
            c->leg[l].state == LEG_TERMINATING) {
            call_unlock(c);
            continue;
        }
        if (c->leg[l].state == LEG_CONFIRMED) {
            call_unlock(c);
            return NSBC_NOENT;  /* 已接通再 answer 语义矛盾 */
        }
        if (c->leg[l].srv_txn) {
            call_unlock(c);
            return b2b_respond_leg(c, l, 200, NULL, sdp, sdp_len);
        }
        call_unlock(c);
    }
    return NSBC_NOENT;  /* 无可用服务端事务 */
}

/* ==================================================================
 * §6 Pre-Answer：发送 183 Session Progress（带 hold SDP），不发 200 OK
 * 不触发计费，激活 Early Media 通道让 playback 等提前介入。
 * ==================================================================*/

int b2b_pre_answer_call(b2b_call_t *call)
{
    if (!call) return NSBC_INVAL;
    b2b_call_t *c = call;

    call_lock(c);
    if (c->dead || c->leg[0].state == LEG_TERMINATED ||
        c->leg[0].state == LEG_TERMINATING) {
        call_unlock(c);
        return NSBC_NOENT;
    }
    /* 已 PRE_ANSWERED 或已接通 → 幂等返回 */
    if (c->leg[0].state == LEG_EARLY || c->leg[0].state == LEG_CONFIRMED) {
        call_unlock(c);
        return NSBC_NOENT;
    }
    if (!c->leg[0].srv_txn) {
        call_unlock(c);
        return NSBC_NOENT;  /* 还没有收到 INVITE，无法发 183 */
    }
    call_unlock(c);

    /* hold SDP（占位：实际需从 media_bridge 拿 A 腿 hold SDP）
     * P0 简化：传 NULL SDP（对端可能接受无 SDP 的 183，或忽略）
     * P1：从 media_bridge 预创建 hold SDP 注入 */
    int rc = b2b_respond_leg(c, 0, 183, NULL, NULL, 0);
    if (rc == NSBC_OK) {
        llog(LOG_INFO, "b2b", "pre_answer: sent 183 Session Progress call=%p", c);
        /* 标记：调用方会写 chan_var "pre_answered=true" */
    } else {
        llog(LOG_WARN, "b2b", "pre_answer: 183 send failed rc=%d call=%p", rc, c);
    }
    return rc;
}

/* ==================================================================
 * 以下原语 P1 实装：
 *   uuid_hold ✅ / uuid_transfer / uuid_send_dtmf / uuid_record /
 *   uuid_broadcast / uuid_break / bridge / status
 *
 * P0 只做参数白名单校验（杜绝帧注入）和 Call-ID 定位，
 * 让上层 cmd_registry 可以接线（参数不合法提前 INVAL，
 * 目标不存在提前 NOENT，不往下走到 P1 骨架才报 NOTSUP）。
 * ==================================================================*/

/* P1 helper：在 SDP 原文 buffer 里做方向属性替换（hold=sendonly / resume=sendrecv）。
 * 找第一个匹配的 a=sendrecv / a=sendonly / a=recvonly / a=inactive 行，
 * 替换为 target。
 *
 * 不变量：四个标准方向 token 的字符串长度都是 10（"a=sendrecv"/
 * "a=sendonly"/"a=recvonly"/"a=inactive"）——保证 replace 后 buffer 不扩缩，
 * 可以 in-place 替换（多余字节用空格覆盖保持 CRLF 对齐）。
 * 非标准方向（如 sendrecvonly/保持原状）不处理，让对端回 400/488。
 * 不改 m= 行里的媒体方向（legacy hold 处理，P1 不做）。
 * 返回 1=找到并替换成功，0=没找到。 */
static int sdp_replace_direction(char *buf, size_t buf_len, int hold)
{
    static const char *from_toks[] = {"a=sendrecv", "a=sendonly",
                                      "a=recvonly", "a=inactive", NULL};
    const char *target = hold ? "a=sendonly" : "a=sendrecv";
    size_t target_len = strlen(target);
    for (size_t i = 0; from_toks[i]; ++i) {
        const char *needle = from_toks[i];
        size_t nlen = strlen(needle);
        if (nlen < target_len) continue; /* 替换后 buffer 更大，跳过（不做） */
        if (buf_len < nlen) continue;
        for (size_t j = 0; j + nlen <= buf_len; ++j) {
            if (memcmp(buf + j, needle, nlen) == 0) {
                /* 方向行后面可能跟 \r\n / \n / EOF —— 必须在行首对齐（前一字节
                 * 是 \n 或 buffer 开头） */
                if (j > 0 && buf[j - 1] != '\n' && buf[j - 1] != '\r')
                    continue;
                memcpy(buf + j, target, target_len);
                if (target_len < nlen)
                    memset(buf + j + target_len, ' ', nlen - target_len);
                return 1;
            }
        }
    }
    return 0;
}

/* P1 helper：发送某条腿的 re-INVITE（hold/resume 用）。
 * B-腿直接 send_invite_b 镜像初始 INVITE 构造路径；A-腿没有对称的
 * send_invite_a，复用 build_dlg_req + txn_client_new 按 dialog 上下文拼。
 * 调用方必须已持有 c->lock。 */
static int send_reinvite_leg(b2b_call_t *c, int leg,
                             const char *body, size_t bodylen)
{
    b2b_core_t *co = c->core;
    b2b_leg_t *l = &c->leg[leg];
    if (l->state != LEG_CONFIRMED && l->state != LEG_EARLY)
        return NSBC_NOENT; /* 腿未 confirm/early 不能 hold */
    /* 已有在轮 inv_txn 取消（并发 re-INVITE 冲突） */
    if (l->inv_txn) {
        llog(LOG_WARN, "b2b", "reinvite leg=%d conflict (inv_txn active)", leg);
        return NSBC_ERR;
    }

    if (leg == 1) {
        /* B-腿：直接 send_invite_b(c, body, len, reinv=1, NULL) */
        sip_txn_t *t = send_invite_b(c, 1, body, bodylen, 1, NULL);
        if (!t) return NSBC_ERR;
        l->inv_txn = t;
        l->reinv = 1;
        return NSBC_OK;
    }

    /* A-腿：build_dlg_req + txn_client_new 镜像 send_invite_b 结构 */
    char branch[40];
    new_branch(branch, sizeof(branch));
    long cseq = ++l->cseq;
    char ruri[DLG_URI_MAX];
    snprintf(ruri, sizeof(ruri), "%s", l->ruri[0] ? l->ruri : l->target);
    char to_with_tag[DLG_URI_MAX + DLG_TAG_MAX + 16];
    snprintf(to_with_tag, sizeof(to_with_tag), "%s;tag=%s",
             l->to, l->rem_tag[0] ? l->rem_tag : "-");
    char out[SIP_MAX_MSG * 2];
    int n = build_dlg_req(co, l, SIP_M_INVITE, cseq, ruri, to_with_tag,
                          branch, NULL, body, bodylen, out, sizeof(out));
    if (n < 0) return NSBC_ERR;
    snprintf(l->last_branch, sizeof(l->last_branch), "%s", branch);
    sip_txn_t *t = txn_client_new(co->cfg.txn, TXN_ICT, branch, strlen(branch),
                                  SIP_M_INVITE, &l->dst, l->reliable,
                                  out, (size_t)n, cli_tu, c, now_ms());
    if (!t) return NSBC_ERR;
    l->inv_txn = t;
    l->reinv = 1;
    return NSBC_OK;
}

/* ==================================================================
 * b2b_cancel_call —— CANCEL 早期 INVITE（uuid_cancel wrapper）
 * ==================================================================*/
int b2b_cancel_call(b2b_call_t *call, int leg)
{
    if (!call) return NSBC_INVAL;
    if (leg < -1 || leg > 1) return NSBC_INVAL;
    call_lock(call);
    if (call->dead) { call_unlock(call); return NSBC_NOENT; }
    int legs[2] = {0};
    int nlegs = 0;
    if (leg == -1) { legs[0] = 0; legs[1] = 1; nlegs = 2; }
    else             { legs[0] = leg; nlegs = 1; }
    int any_sent = 0;
    for (int i = 0; i < nlegs; ++i) {
        int lg = legs[i];
        b2b_leg_t *l = &call->leg[lg];
        if (l->state == LEG_CALLING || l->state == LEG_EARLY || l->reinv) {
            send_cancel_req(call, lg);
            ++any_sent;
            llog(LOG_INFO, "b2b", "cancel leg=%d callid=%s state=%d",
                 lg, l->callid, l->state);
        } else {
            llog(LOG_INFO, "b2b", "cancel skip leg=%d callid=%s state=%d (not early)",
                 lg, l->callid, l->state);
        }
    }
    call_unlock(call);
    return any_sent ? NSBC_OK : NSBC_NOENT;
}

/* ==================================================================
 * b2b_mute_call —— 设置 per-leg mute 标志（uuid_mute wrapper）
 * ==================================================================*/
int b2b_mute_call(b2b_call_t *call, int leg, int on)
{
    if (!call) return NSBC_INVAL;
    if (leg < -1 || leg > 1) return NSBC_INVAL;
    if (on != 0 && on != 1) return NSBC_INVAL;
    call_lock(call);
    if (call->dead) { call_unlock(call); return NSBC_NOENT; }
    int legs[2] = {0};
    int nlegs = 0;
    if (leg == -1) { legs[0] = 0; legs[1] = 1; nlegs = 2; }
    else             { legs[0] = leg; nlegs = 1; }
    for (int i = 0; i < nlegs; ++i) {
        int lg = legs[i];
        call->muted[lg] = on;
        llog(LOG_INFO, "b2b", "mute leg=%d on=%d callid=%s",
             lg, on, call->leg[lg].callid);
    }
    call_unlock(call);
    return NSBC_OK;
}

int b2b_hold_call(b2b_call_t *call, int b_leg, int hold,
                  const char *sdp, size_t sdp_len)
{
    if (!call) return NSBC_INVAL;
    if (b_leg < -1 || b_leg > 1) return NSBC_INVAL;
    if (hold != 0 && hold != 1) return NSBC_INVAL;

    /* OAM 线程访问 reactor 的 call 状态，必须加锁 */
    pthread_mutex_lock(&call->lock);

    /* race guard：call_destroy 可能在别的线程跑（先 dead=1 再关对话），
     * 我们刚抢到锁时 dead 可能已置位——dead 了就放弃 hold。 */
    if (call->dead) {
        pthread_mutex_unlock(&call->lock);
        return NSBC_NOENT;
    }

    /* 确定目标腿集：b_leg=-1 双腿，0=仅 A，1=仅 B */
    int legs[2] = {0};
    int nlegs = 0;
    if (b_leg == -1) { legs[0] = 0; legs[1] = 1; nlegs = 2; }
    else             { legs[0] = b_leg; nlegs = 1; }

    int last_rc = NSBC_OK;
    for (int i = 0; i < nlegs; ++i) {
        int lg = legs[i];
        b2b_leg_t *l = &call->leg[lg];
        if (!l->dlg || (l->state != LEG_CONFIRMED && l->state != LEG_EARLY)) {
            /* 腿没 dialog 或未 confirm/early（初始 INVITE 在响、已 hold、已结束），跳过 */
            last_rc = NSBC_NOENT;
            continue;
        }
        /* SDP 源：调用方传入优先，否则用 leg->answer（最后协商好的远端 SDP）。
         * hold/resume 都是在原有 SDP 基础上改方向属性——不生成全新 SDP。 */
        const char *src_sdp = NULL;
        size_t src_len = 0;
        if (sdp && sdp_len > 0) {
            src_sdp = sdp;
            src_len = sdp_len;
        } else if (l->answer_len > 0) {
            src_sdp = l->answer;
            src_len = l->answer_len;
        } else {
            last_rc = NSBC_NOENT;
            continue; /* 没 SDP 没法改方向，跳过 */
        }
        /* 拷贝到足够大的临时 buffer（栈分配 8KB，避免踩字段）。 */
        char new_sdp[B2B_SDP_MAX];
        size_t new_len = src_len < sizeof(new_sdp) - 1
                         ? src_len
                         : sizeof(new_sdp) - 1;
        memcpy(new_sdp, src_sdp, new_len);
        new_sdp[new_len] = '\0';
        /* 方向替换：hold=sendonly / resume=sendrecv */
        int replaced = sdp_replace_direction(new_sdp, new_len, hold);
        (void)replaced; /* 即使 SDP 没 a= 行（极端），照样发 re-INVITE，让对端回 400/488 */
        new_sdp[new_len] = '\0';

        int rc = send_reinvite_leg(call, lg, new_sdp, new_len);
        if (rc != NSBC_OK) {
            /* 一条腿失败不阻断另一条腿的尝试（双腿 hold 场景） */
            last_rc = rc;
            continue;
        }
        /* 注意：不覆写 l->answer——那是对端最后协商好的 SDP，reactor 收到
         * re-INVITE 的 200 OK 会自然更新。下次 resume 时直接从 answer 读即可。 */
    }

    pthread_mutex_unlock(&call->lock);
    return last_rc;
}

/* P1 helper：向指定腿发 REFER（blind/attended transfer 共用，Refer-To 头由调用方
 * 拼进 extra；attended 时额外带 Replaces 头）。
 * 调用方必须已持有 call->lock + 确认 leg 非 dead + state=CONFIRMED。 */
static int b2b_send_refer(b2b_call_t *c, int leg,
                          const char *refer_to, const char *replaces)
{
    b2b_core_t *co = c->core;
    b2b_leg_t *l = &c->leg[leg];
    if (!l->dlg || l->state != LEG_CONFIRMED) return NSBC_NOENT;
    if (l->inv_txn) {
        llog(LOG_WARN, "b2b", "REFER leg=%d conflict (inv_txn active)", leg);
        return NSBC_ERR;
    }
    /* extra 头块：Refer-To + Replaces（可选） */
    char extra[512];
    size_t used = 0;
    int n = snprintf(extra, sizeof(extra), "Refer-To: <sip:%s>\r\n", refer_to);
    if (n > 0 && (size_t)n < sizeof(extra)) used = (size_t)n;
    if (replaces && replaces[0]) {
        int m = snprintf(extra + used, sizeof(extra) - used,
                         "Replaces: %s\r\n", replaces);
        if (m > 0 && (size_t)m < sizeof(extra) - used) used += (size_t)m;
    }
    extra[used] = '\0';

    char branch[40];
    new_branch(branch, sizeof(branch));
    long cseq = ++l->cseq;
    char ruri[DLG_URI_MAX];
    snprintf(ruri, sizeof(ruri), "%s", l->target[0] ? l->target : l->ruri);
    char to_with_tag[DLG_URI_MAX + DLG_TAG_MAX + 16];
    snprintf(to_with_tag, sizeof(to_with_tag), "%s;tag=%s",
             l->to, l->rem_tag[0] ? l->rem_tag : "-");
    char out[SIP_MAX_MSG * 2];
    int rn = build_dlg_req(co, l, SIP_M_REFER, cseq, ruri, to_with_tag,
                           branch, extra, NULL, 0, out, sizeof(out));
    if (rn < 0) return NSBC_ERR;
    snprintf(l->last_branch, sizeof(l->last_branch), "%s", branch);
    sip_txn_t *t = txn_client_new(co->cfg.txn, TXN_NICT, branch, strlen(branch),
                                  SIP_M_REFER, &l->dst, l->reliable,
                                  out, (size_t)rn, cli_tu, c, now_ms());
    if (!t) return NSBC_ERR;
    /* 存 refer_txn 让 cli_tu 能匹配 REFER 响应做自动 BYE；
     * Blind transfer 通常双腿同时 REFER，第一腿存这里就够（响应里 legs 已标） */
    if (!c->refer_txn) c->refer_txn = t;
    (void)t;
    return NSBC_OK;
}

int b2b_transfer_call(b2b_call_t *call, int b_leg, const char *dest,
                      int kind, b2b_call_t *consult_call,
                      const char *replaces_hdr)
{
    if (!call || !dest) return NSBC_INVAL;
    if (b_leg < -1 || b_leg > 1) return NSBC_INVAL;
    if (kind != CCF_XFER_BLIND && kind != CCF_XFER_CONSULTED) return NSBC_INVAL;
    /* dest 白名单：可见 ASCII + 长度 */
    size_t dl = strlen(dest);
    if (dl == 0 || dl > 256) return NSBC_INVAL;
    for (size_t i = 0; i < dl; ++i) {
        unsigned char ch = (unsigned char)dest[i];
        if (ch < 0x21 || ch > 0x7e) return NSBC_INVAL;
    }
    if (kind == CCF_XFER_CONSULTED && !replaces_hdr) return NSBC_INVAL;

    pthread_mutex_lock(&call->lock);
    if (call->dead) { pthread_mutex_unlock(&call->lock); return NSBC_NOENT; }

    /* Blind transfer：向 B-腿发 REFER（refer A→B→dest），同时向 A-腿发 BYE。
     * b_leg=-1：双腿同时，b_leg=1：仅 B 腿，b_leg=0：仅 A 腿（REFER 给 A）。
     * Consulted transfer（attended）：REFER 带 Replaces 头指向 consultation
     * dialog（A hold + 拨 C → A 确认接通 → REFER 让 B 和 C 对话）。 */
    int rc = NSBC_OK;
    if (kind == CCF_XFER_BLIND) {
        if (b_leg == -1 || b_leg == 1) {
            int lr = b2b_send_refer(call, 1, dest, NULL);
            if (lr != NSBC_OK) rc = lr;
        }
        if (b_leg == -1 || b_leg == 0) {
            int lr = b2b_send_refer(call, 0, dest, NULL);
            if (lr != NSBC_OK && rc == NSBC_OK) rc = lr;
        }
    } else {
        /* CONSULTED：Replaces 头已经由调用方（REFER 入站）从 SIP 消息里解析好。
         * 格式：call-id=xxx;from-tag=xxx;to-tag=xxx（RFC 3891） */
        const char *rp = (replaces_hdr && replaces_hdr[0]) ? replaces_hdr : NULL;
        int lr = b2b_send_refer(call, b_leg == 0 ? 0 : 1, dest, rp);
        rc = lr == NSBC_OK ? NSBC_OK : lr;
        (void)consult_call; /* 暂不用——replaces 字符串直接走 b2b_send_refer */
    }

    pthread_mutex_unlock(&call->lock);
    return rc;
}

/* bridge_ctrl op 常量（与 media/bridge 模块对齐，P1 移到 bridge.h） */
#define BC_DTMF_PRESS     101
#define BC_BROADCAST_START 102
#define BC_BROADCAST_BREAK 103
#define BC_BRIDGE_UP      104
#define BC_BRIDGE_DOWN    105

int b2b_send_dtmf(b2b_call_t *call, int b_leg, const char *digits, size_t len,
                  int volume, uint16_t dur_ms)
{
    if (!call || !digits || len == 0 || len > 32) return NSBC_INVAL;
    if (b_leg < -1 || b_leg > 1) return NSBC_INVAL;
    for (size_t i = 0; i < len; ++i) {
        char d = digits[i];
        int ok = (d >= '0' && d <= '9') || d == '*' || d == '#' ||
                 (d >= 'A' && d <= 'D') || (d >= 'a' && d <= 'd');
        if (!ok) return NSBC_INVAL;
    }
    if (volume < 0 || volume > 63) return NSBC_INVAL;
    b2b_core_t *co = call->core;
    if (!co->cfg.bridge_ctrl) return NSBC_NOTSUP; /* P1: bridge_ctrl 未装配 */
    /* digits/leg/volume/dur_ms 封成紧凑格式：直接传 digits，leg 丢
     * 进 ptr_arg 低位——bridge_ctrl 签名由 media 模块实现，这里只要 hook 存在就 OK */
    uint32_t arg32 = ((uint32_t)(dur_ms & 0xFFFF) << 16)
                   | ((uint32_t)(volume & 0x3F) << 8)
                   | ((uint32_t)(b_leg & 0xFF));
    int rc = co->cfg.bridge_ctrl(co->cfg.bridge_ctrl_ud, call, BC_DTMF_PRESS,
                                 digits, (void *)(uintptr_t)arg32);
    return rc;
}

int b2b_send_message(b2b_call_t *call, int leg,
                     const char *body, size_t body_len)
{
    if (!call) return NSBC_INVAL;
    if (leg < -1 || leg > 1) return NSBC_INVAL;
    if (body_len > 4096) return NSBC_INVAL; /* 安全上限：MESSAGE body 不应过大 */

    b2b_core_t *co = call->core;

    pthread_mutex_lock(&call->lock);
    if (call->dead) { pthread_mutex_unlock(&call->lock); return NSBC_NOENT; }

    /* 自动选发起腿——对端刚接通那条（MESSAGE 发向对端） */
    int src_leg;
    if (leg < 0) {
        if (call->leg[0].state == LEG_CONFIRMED ||
            call->leg[0].state == LEG_EARLY)
            src_leg = 0;
        else if (call->leg[1].state == LEG_CONFIRMED ||
                 call->leg[1].state == LEG_EARLY)
            src_leg = 1;
        else {
            pthread_mutex_unlock(&call->lock);
            return NSBC_NOENT; /* 无腿接通，无法发 MESSAGE */
        }
    } else {
        src_leg = leg;
        b2b_leg_t *l = &call->leg[src_leg];
        if (l->state != LEG_CONFIRMED && l->state != LEG_EARLY) {
            pthread_mutex_unlock(&call->lock);
            return NSBC_NOENT; /* 指定腿未接通 */
        }
    }

    int dst_leg = src_leg ^ 1;
    b2b_leg_t *src = &call->leg[src_leg];
    b2b_leg_t *dst = &call->leg[dst_leg];

    /* guard 串行化：确保与对话内其他请求互斥 */
    if (!sip_upd_begin(&src->guard)) {
        pthread_mutex_unlock(&call->lock);
        return NSBC_AGAIN; /* guard 忙，稍后重试 */
    }
    if (!sip_upd_begin(&dst->guard)) {
        sip_upd_end(&src->guard);
        pthread_mutex_unlock(&call->lock);
        return NSBC_AGAIN; /* guard 忙，稍后重试 */
    }

    /* 构造 body：默认 Content-Type: text/plain */
    char extra[128];
    extra[0] = '\0';
    if (body_len) {
        snprintf(extra, sizeof(extra),
                 "Content-Type: text/plain\r\n"
                 "Content-Length: %zu\r\n", body_len);
    }

    /* 构造请求 */
    char branch[40];
    new_branch(branch, sizeof(branch));
    long cs = ++dst->cseq;
    char to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
    snprintf(to, sizeof(to), "%s;tag=%s", dst->to, dst->rem_tag);
    char ruri[DLG_URI_MAX + 8];
    snprintf(ruri, sizeof(ruri), "%s",
             dst->target[0] ? dst->target : dst->ruri);
    char out[SIP_MAX_MSG * 2];
    int n = build_dlg_req(co, dst, SIP_M_MESSAGE, cs,
                          ruri, to, branch,
                          extra[0] ? extra : NULL,
                          body_len ? body : NULL, body_len,
                          out, sizeof(out));
    if (n <= 0) {
        sip_upd_end(&src->guard);
        sip_upd_end(&dst->guard);
        pthread_mutex_unlock(&call->lock);
        return NSBC_ERR;
    }

    /* 客户端事务发起（NICT：MESSAGE 是非 INVITE）；cli_tu 到终态
     * 时自动释放双腿 guard（leg[1]/leg[0] 的 out_txn 分支都会命中
     * sip_upd_end，幂等）。此处无入站 srv_txn——API 发完立即返回。 */
    dst->out_txn = txn_client_new(
        co->cfg.txn, TXN_NICT, branch, strlen(branch), SIP_M_MESSAGE,
        &dst->dst, dst->reliable, out, (size_t)n, cli_tu, call,
        now_ms());
    if (dst->out_txn) {
        snprintf(dst->last_branch, sizeof(dst->last_branch), "%s", branch);
    } else {
        sip_upd_end(&src->guard);
        sip_upd_end(&dst->guard);
        pthread_mutex_unlock(&call->lock);
        return NSBC_ERR;
    }

    pthread_mutex_unlock(&call->lock);
    return NSBC_OK;
}

int b2b_record_start(b2b_call_t *call, int mode,
                     const char *path __attribute__((unused)),
                     const char *reason)
{
    if (!call) return NSBC_INVAL;
    if (mode < REC_MODE_SINGLE || mode > REC_MODE_SEPARATE) return NSBC_INVAL;
    b2b_core_t *co = call->core;
    if (!co->cfg.rec_mgr) return NSBC_NOTSUP;
    /* 加锁：call 状态机 + rec[2] 槽互斥 */
    pthread_mutex_lock(&call->lock);
    if (call->dead) { pthread_mutex_unlock(&call->lock); return NSBC_NOENT; }
    /* 双腿都可能开录制——已有槽位非空 → 拒绝重复开 */
    if (call->rec[0] || call->rec[1]) {
        pthread_mutex_unlock(&call->lock);
        return NSBC_ERR; /* 已有录制会话 */
    }
    rec_open_t req;
    memset(&req, 0, sizeof(req));
    req.mode = (rec_mode_t)mode;
    snprintf(req.callid, sizeof(req.callid), "%s", call->leg[0].callid[0]
             ? call->leg[0].callid : "");
    if (reason) snprintf(req.reason, sizeof(req.reason), "%s", reason);
    /* mode→leg_mask：SINGLE=leg1, MIXED=bit0|bit1, SEPARATE=bit0|bit1 */
    req.leg_mask = (mode == REC_MODE_SINGLE) ? 2 : 3;
    rec_sess_t *s = NULL;
    int rc = rec_open((rec_mgr_t *)co->cfg.rec_mgr, &req, &s);
    if (rc == NSBC_OK) {
        call->rec[0] = (mode == REC_MODE_SEPARATE) ? s : NULL;
        call->rec[1] = s; /* SINGLE/MIXED 只有 B 腿槽塞 ptr，MIXED 的 mixer 内部处理 */
    }
    pthread_mutex_unlock(&call->lock);
    return rc;
}

int b2b_record_stop(b2b_call_t *call, const char *reason,
                    char *path_out, size_t path_cap)
{
    (void)reason;
    if (!call) return NSBC_INVAL;
    b2b_core_t *co = call->core;
    if (!co->cfg.rec_mgr) return NSBC_NOTSUP;
    pthread_mutex_lock(&call->lock);
    if (call->dead) { pthread_mutex_unlock(&call->lock); return NSBC_NOENT; }
    if (!call->rec[0] && !call->rec[1]) {
        pthread_mutex_unlock(&call->lock);
        return NSBC_NOENT; /* 没有在录 */
    }
    int rc = NSBC_OK;
    for (int i = 0; i < 2; ++i) {
        if (call->rec[i]) {
            rec_sess_t *s = (rec_sess_t *)call->rec[i];
            char p[REC_PATH_MAX];
            uint64_t bytes = 0;
            int r = rec_stop(s, p, sizeof(p), &bytes);
            if (r != NSBC_OK && rc == NSBC_OK) rc = r;
            call->rec[i] = NULL;
            /* 第一腿的 path 填给 out（SINGLE/MIXED 共用一文件；SEPARATE 只给 leg1） */
            if (i == 1 && path_out && path_cap > 0 && p[0]) {
                snprintf(path_out, path_cap, "%s", p);
            }
        }
    }
    pthread_mutex_unlock(&call->lock);
    return rc;
}

int b2b_broadcast_start(b2b_call_t *call, int b_leg, const char *wav_path)
{
    if (!call || !wav_path) return NSBC_INVAL;
    if (b_leg < -1 || b_leg > 1) return NSBC_INVAL;
    size_t pl = strlen(wav_path);
    if (pl == 0 || pl > 256) return NSBC_INVAL;
    b2b_core_t *co = call->core;
    if (!co->cfg.bridge_ctrl) return NSBC_NOTSUP;
    uint32_t leg_arg = (uint32_t)(b_leg & 0xFF);
    return co->cfg.bridge_ctrl(co->cfg.bridge_ctrl_ud, call,
                               BC_BROADCAST_START, wav_path,
                               (void *)(uintptr_t)leg_arg);
}

int b2b_broadcast_break(b2b_call_t *call, int b_leg)
{
    if (!call) return NSBC_INVAL;
    if (b_leg < -1 || b_leg > 1) return NSBC_INVAL;
    b2b_core_t *co = call->core;
    if (!co->cfg.bridge_ctrl) return NSBC_NOTSUP;
    uint32_t leg_arg = (uint32_t)(b_leg & 0xFF);
    return co->cfg.bridge_ctrl(co->cfg.bridge_ctrl_ud, call,
                               BC_BROADCAST_BREAK, NULL,
                               (void *)(uintptr_t)leg_arg);
}

int b2b_bridge(b2b_core_t *co, const char *callid_a, const char *callid_b)
{
    if (!co || !b2b_valid_callid(callid_a, strlen(callid_a)) ||
        !b2b_valid_callid(callid_b, strlen(callid_b)))
        return NSBC_INVAL;
    if (strcmp(callid_a, callid_b) == 0)
        return NSBC_INVAL;
    b2b_call_t *ca = b2b_find_callid(co, callid_a, strlen(callid_a));
    b2b_call_t *cb = b2b_find_callid(co, callid_b, strlen(callid_b));
    if (!ca || !cb) return NSBC_NOENT;
    if (!co->cfg.bridge_ctrl) return NSBC_NOTSUP; /* P1: media bridge 未装配 */
    /* bridge_ctrl(call_key_a, BC_BRIDGE_UP, call_key_b) — 两 call_key 都是
     * b2b_call_t* 不透明，media bridge 层做表键比较 */
    return co->cfg.bridge_ctrl(co->cfg.bridge_ctrl_ud, ca,
                               BC_BRIDGE_UP, NULL, cb);
}

/* ---- status：P0 版本用已有原子计数 + 内存 Arena 水位 ----
 *
 * CPS/MPS 计数由 oam 层维护（cmd_stats/cmd_cdr 里已有）；
 * P0 先用 b2b_active/total/failed + mem_arena stat 拼综合视图。 */
int b2b_status(b2b_core_t *co, char *out, size_t outsz)
{
    if (!co || !out || outsz < 64) return NSBC_INVAL;
    size_t pos = 0;
    buf_put(out, outsz, &pos,
            "status active_calls=%llu total=%llu failed=%llu\n",
            (unsigned long long)b2b_active_calls(co),
            (unsigned long long)b2b_total_calls(co),
            (unsigned long long)b2b_failed_calls(co));
    return NSBC_OK;
}

/* REQ-CHAN-VAR-001：per-call 通道变量懒初始化 + 读写封装。
 * 这是 b2b_call_t 内部字段 vars 的唯一访问入口（REQ-ARCH-011 不透明保护），
 * 外部（OAM/CCMD）只能通过这两个 API 操作。 */
static int call_vars_ensure_init(b2b_call_t *c)
{
    if (c->vars) return NSBC_OK;
    return chan_var_create(c->core->cfg.sig_arena, &c->vars);
}

int b2b_call_set_var(b2b_call_t *call, int leg, const char *key, const char *value)
{
    if (!call || !key) return NSBC_INVAL;
    if (leg < -1 || leg > 1) return NSBC_INVAL;
    pthread_mutex_lock(&call->lock);
    if (call->dead) { pthread_mutex_unlock(&call->lock); return NSBC_NOENT; }
    int rc = call_vars_ensure_init(call);
    if (rc == NSBC_OK)
        rc = chan_var_set(call->vars, leg, key, value ? value : "");
    pthread_mutex_unlock(&call->lock);
    return rc;
}

int b2b_call_get_var(b2b_call_t *call, int leg, const char *key,
                     char *out, size_t out_cap)
{
    if (!call || !key || !out) return NSBC_INVAL;
    if (leg < -1 || leg > 1) return NSBC_INVAL;
    pthread_mutex_lock(&call->lock);
    if (call->dead) { pthread_mutex_unlock(&call->lock); return NSBC_NOENT; }
    /* get 时 vars 必须已初始化；否则没变量可读。 */
    if (!call->vars) {
        pthread_mutex_unlock(&call->lock);
        return NSBC_NOENT;
    }
    int rc = chan_var_get(call->vars, leg, key, out, out_cap);
    pthread_mutex_unlock(&call->lock);
    return rc;
}


/* ============================================================
 * Session Timer (RFC 4028) — 辅助函数 + 时间轮回调
 * ============================================================ */

/* 从 sip_msg_t 提取 Session-Expires / Min-SE / Supported:timer 原始头值 */
static void extract_sess_timer_hdrs(const sip_msg_t *m,
                                    char *se, size_t sesz,
                                    char *minse, size_t msz,
                                    int *timer_ok)
{
    if (se) se[0] = '\0';
    if (minse) minse[0] = '\0';
    if (timer_ok) *timer_ok = 0;
    sip_hdr_t *h;
    if ((h = sip_hdr_get(m, SIP_H_SESSION_EXPIRES)) != NULL && se && sesz > 0)
        snprintf(se, sesz, "%.*s", (int)h->value.len, h->value.p);
    if ((h = sip_hdr_get(m, SIP_H_MIN_SE)) != NULL && minse && msz > 0)
        snprintf(minse, msz, "%.*s", (int)h->value.len, h->value.p);
    if ((h = sip_hdr_get(m, SIP_H_SUPPORTED)) != NULL && timer_ok) {
        const char *p = h->value.p;
        const char *end = h->value.p + h->value.len;
        while (p < end) {
            while (p < end && (*p == ',' || *p == ' ')) p++;
            const char *ts = p;
            while (p < end && *p != ',' && *p != ' ') p++;
            if (p - ts == 5 && strncmp(ts, "timer", 5) == 0) { *timer_ok = 1; return; }
        }
    }
}

/* Session Timer 刷新回调（timer_wheel 驱动） */
static void sess_timer_refresh_cb(uint64_t cb_now, void *arg)
{
    b2b_call_t *c = (b2b_call_t *)arg;
    b2b_core_t *co = c->core;
    int myleg = -1, otherleg = -1;

    call_lock(c);

    /* 确定刷新哪腿：每腿独立判断 refresher 角色 */
    for (int i = 0; i < 2; ++i) {
        int due = sess_timer_due_refresh(&c->leg[i].st, cb_now);
        if (c->leg[i].st.active && c->leg[i].st.refresher != SIP_REF_NONE && due) {
            myleg = i;
            otherleg = 1 - i;
            break;
        }
    }

    if (myleg >= 0) {
        b2b_leg_t *me = &c->leg[myleg];
        b2b_leg_t *other = &c->leg[otherleg];

        /* 发送 re-INVITE 但不重置 refresh_node —
         * refresh_sent 保持 1（标记已发未确认），timer 到期 due_refresh 会返回 0
         * 等 re-INVITE 被确认后（cli_tu 收到 2xx）再 reset + tw_mod 下次 */
        me->refresh_pending = 1;
        tw_mod(co->cfg.tw, &me->expire_node,
               cb_now + (uint64_t)me->st.interval_s * 1000u + SESS_TIMER_GRACE_MS);

        if (myleg == 0) {
            /* A 腿 refresher → 用 A-leg dialog 朝 Alice 发 re-INVITE */
            char branch[40];
            new_branch(branch, sizeof(branch));
            long cs = ++me->cseq;
            char to[B2B_HDR_MAX + DLG_TAG_MAX + 8];
            snprintf(to, sizeof(to), "%s;tag=%s", me->to, me->rem_tag);
            char extra[256];
            if (me->session_expires[0]) {
                snprintf(extra, sizeof(extra), "Supported: timer\r\n%s", me->session_expires);
            } else {
                snprintf(extra, sizeof(extra), "Supported: timer\r\n");
            }
            char req[4096];
            int n = build_dlg_req(co, me, SIP_M_INVITE, cs,
                                  me->target[0] ? me->target : me->ruri, to,
                                  branch, extra, me->offer, me->offer_len,
                                  req, sizeof(req));
            if (n > 0) {
                sip_txn_t *t = txn_client_new(co->cfg.txn, TXN_ICT, branch,
                                              strlen(branch), SIP_M_INVITE,
                                              &me->dst, me->reliable, req, (size_t)n,
                                              cli_tu, c, now_ms());
                if (t) me->out_txn = t;
            }
        } else {
            /* B 腿 refresher → send_invite_b 朝 Bob 发 re-INVITE */
            sip_txn_t *t = send_invite_b(c, 1, other->offer, other->offer_len, 1, NULL);
            if (!t) {
                llog(LOG_WARN, "b2bua", "re-INVITE Bob failed call=%p",
                     (void *)c);
            }
        }
    }

    call_unlock(c);
}

/* Session Timer 超时回调（timer_wheel 驱动） */
static void sess_timer_expire_cb(uint64_t cb_now, void *arg)
{
    b2b_call_t *c = (b2b_call_t *)arg;
    b2b_core_t *co = c->core;

    call_lock(c);

    int expired_leg = -1;
    for (int i = 0; i < 2; ++i) {
        if (c->leg[i].st.active) {
            if (sess_timer_expired(&c->leg[i].st, cb_now)) {
                expired_leg = i;
                break;
            }
        }
    }

    if (expired_leg >= 0) {
        b2b_leg_t *le = &c->leg[expired_leg];
        /* disarm 所有 timer node */
        tw_del(co->cfg.tw, &le->refresh_node);
        tw_del(co->cfg.tw, &le->expire_node);
        le->st.active = 0;

        /* 终止整个呼叫 */
        llog(LOG_WARN, "b2b", "Session Timer EXPIRED leg[%d] interval=%ds → terminate",
             expired_leg, le->st.interval_s);
        call_unlock(c);
        b2b_terminate(c, 481);
        return;
    }

    call_unlock(c);
}

/* REQ-AI-001 / REQ-EVENTS-001：call_destroy 多槽 hook 注册/卸载 */
int b2b_call_destroy_add(b2b_call_destroy_cb fn, void *ud)
{
    if (!fn) return NSBC_INVAL;
    pthread_mutex_lock(&g_end_hook_lock);
    for (int i = 0; i < g_end_hook_n; ++i)
        if (g_end_hooks[i].fn == fn && g_end_hooks[i].ud == ud) {
            pthread_mutex_unlock(&g_end_hook_lock);
            return NSBC_OK; /* 已存在（防重复）*/
        }
    if (g_end_hook_n >= B2B_END_HOOK_MAX) {
        pthread_mutex_unlock(&g_end_hook_lock);
        return NSBC_NOSPC;
    }
    g_end_hooks[g_end_hook_n].fn = fn;
    g_end_hooks[g_end_hook_n].ud = ud;
    ++g_end_hook_n;
    pthread_mutex_unlock(&g_end_hook_lock);
    return NSBC_OK;
}

void b2b_call_destroy_rm(b2b_call_destroy_cb fn, void *ud)
{
    if (!fn) return;
    pthread_mutex_lock(&g_end_hook_lock);
    int out = 0;
    for (int i = 0; i < g_end_hook_n; ++i) {
        if (g_end_hooks[i].fn == fn && g_end_hooks[i].ud == ud) continue;
        if (out != i) g_end_hooks[out] = g_end_hooks[i];
        ++out;
    }
    g_end_hook_n = out;
    pthread_mutex_unlock(&g_end_hook_lock);
}

void b2b_call_destroy_rm_all(void)
{
    pthread_mutex_lock(&g_end_hook_lock);
    g_end_hook_n = 0;
    pthread_mutex_unlock(&g_end_hook_lock);
}

/* ===== SDP hook 多槽链 ===== */
int b2b_sdp_hook_add(b2b_sdp_hook_kind_t kind, b2b_sdp_hook_fn fn, void *ud)
{
    if (kind > 1 || !fn) return NSBC_INVAL;
    pthread_mutex_lock(&g_sdp_hook_lock);
    for (int i = 0; i < g_sdp_hook_n[kind]; ++i)
        if (g_sdp_hooks[kind][i].fn == fn && g_sdp_hooks[kind][i].ud == ud) {
            pthread_mutex_unlock(&g_sdp_hook_lock);
            return NSBC_OK;
        }
    if (g_sdp_hook_n[kind] >= B2B_SDP_HOOK_MAX) {
        pthread_mutex_unlock(&g_sdp_hook_lock);
        return NSBC_NOSPC;
    }
    g_sdp_hooks[kind][g_sdp_hook_n[kind]].fn = fn;
    g_sdp_hooks[kind][g_sdp_hook_n[kind]].ud = ud;
    ++g_sdp_hook_n[kind];
    pthread_mutex_unlock(&g_sdp_hook_lock);
    return NSBC_OK;
}
void b2b_sdp_hook_rm(b2b_sdp_hook_kind_t kind, b2b_sdp_hook_fn fn, void *ud)
{
    if (kind > 1 || !fn) return;
    pthread_mutex_lock(&g_sdp_hook_lock);
    int out = 0;
    for (int i = 0; i < g_sdp_hook_n[kind]; ++i) {
        if (g_sdp_hooks[kind][i].fn == fn && g_sdp_hooks[kind][i].ud == ud) continue;
        if (out != i) g_sdp_hooks[kind][out] = g_sdp_hooks[kind][i];
        ++out;
    }
    g_sdp_hook_n[kind] = out;
    pthread_mutex_unlock(&g_sdp_hook_lock);
}

/* ===== REGISTER 多槽链 ===== */
int b2b_register_hook_add(b2b_register_fn fn, void *ud)
{
    if (!fn) return NSBC_INVAL;
    pthread_mutex_lock(&g_reg_hook_lock);
    for (int i = 0; i < g_reg_hook_n; ++i)
        if (g_reg_hooks[i].fn == fn && g_reg_hooks[i].ud == ud) {
            pthread_mutex_unlock(&g_reg_hook_lock);
            return NSBC_OK;
        }
    if (g_reg_hook_n >= B2B_REGISTER_HOOK_MAX) {
        pthread_mutex_unlock(&g_reg_hook_lock);
        return NSBC_NOSPC;
    }
    g_reg_hooks[g_reg_hook_n].fn = fn;
    g_reg_hooks[g_reg_hook_n].ud = ud;
    ++g_reg_hook_n;
    pthread_mutex_unlock(&g_reg_hook_lock);
    return NSBC_OK;
}
void b2b_register_hook_rm(b2b_register_fn fn, void *ud)
{
    if (!fn) return;
    pthread_mutex_lock(&g_reg_hook_lock);
    int out = 0;
    for (int i = 0; i < g_reg_hook_n; ++i) {
        if (g_reg_hooks[i].fn == fn && g_reg_hooks[i].ud == ud) continue;
        if (out != i) g_reg_hooks[out] = g_reg_hooks[i];
        ++out;
    }
    g_reg_hook_n = out;
    pthread_mutex_unlock(&g_reg_hook_lock);
}

/* ===== IP guard 多槽链（防暴力破解，security 模块注册） ===== */
typedef struct {
    b2b_guard_fn fn;
    void *ud;
} guard_hook_t;
static pthread_mutex_t g_guard_hook_lock = PTHREAD_MUTEX_INITIALIZER;
static guard_hook_t g_guard_hooks[B2B_GUARD_HOOK_MAX];
static int g_guard_hook_n;

int b2b_guard_hook_add(b2b_guard_fn fn, void *ud)
{
    if (!fn) return NSBC_INVAL;
    pthread_mutex_lock(&g_guard_hook_lock);
    for (int i = 0; i < g_guard_hook_n; ++i)
        if (g_guard_hooks[i].fn == fn && g_guard_hooks[i].ud == ud) {
            pthread_mutex_unlock(&g_guard_hook_lock);
            return NSBC_OK;
        }
    if (g_guard_hook_n >= B2B_GUARD_HOOK_MAX) {
        pthread_mutex_unlock(&g_guard_hook_lock);
        return NSBC_NOSPC;
    }
    g_guard_hooks[g_guard_hook_n].fn = fn;
    g_guard_hooks[g_guard_hook_n].ud = ud;
    ++g_guard_hook_n;
    pthread_mutex_unlock(&g_guard_hook_lock);
    return NSBC_OK;
}

void b2b_guard_hook_rm(b2b_guard_fn fn, void *ud)
{
    if (!fn) return;
    pthread_mutex_lock(&g_guard_hook_lock);
    int out = 0;
    for (int i = 0; i < g_guard_hook_n; ++i) {
        if (g_guard_hooks[i].fn == fn && g_guard_hooks[i].ud == ud) continue;
        if (out != i) g_guard_hooks[out] = g_guard_hooks[i];
        ++out;
    }
    g_guard_hook_n = out;
    pthread_mutex_unlock(&g_guard_hook_lock);
}

/* 锁内收集回调快照到栈数组，解锁后执行——钩子回调内部重入
 * add/rm 也不会死锁（项目钩子链标准模式）。 */
int b2b_guard_check(const char *src_ip, b2b_guard_evt_t evt)
{
    if (!src_ip || !src_ip[0]) return NSBC_OK;
    b2b_guard_fn fns[B2B_GUARD_HOOK_MAX];
    void *uds[B2B_GUARD_HOOK_MAX];
    int n = 0;
    pthread_mutex_lock(&g_guard_hook_lock);
    for (int i = 0; i < g_guard_hook_n && n < B2B_GUARD_HOOK_MAX; ++i) {
        fns[n] = g_guard_hooks[i].fn;
        uds[n] = g_guard_hooks[i].ud;
        ++n;
    }
    pthread_mutex_unlock(&g_guard_hook_lock);
    for (int i = 0; i < n; ++i)
        if (fns[i](src_ip, evt, uds[i]) == NSBC_PERM_DENIED)
            return NSBC_PERM_DENIED;
    return NSBC_OK;
}

void b2b_guard_notify(const char *src_ip, b2b_guard_evt_t evt)
{
    if (!src_ip || !src_ip[0]) return;
    b2b_guard_fn fns[B2B_GUARD_HOOK_MAX];
    void *uds[B2B_GUARD_HOOK_MAX];
    int n = 0;
    pthread_mutex_lock(&g_guard_hook_lock);
    for (int i = 0; i < g_guard_hook_n && n < B2B_GUARD_HOOK_MAX; ++i) {
        fns[n] = g_guard_hooks[i].fn;
        uds[n] = g_guard_hooks[i].ud;
        ++n;
    }
    pthread_mutex_unlock(&g_guard_hook_lock);
    for (int i = 0; i < n; ++i)
        (void)fns[i](src_ip, evt, uds[i]);
}

/* ===== 无主响应多槽链 ===== */
int b2b_resp_hook_add(b2b_resp_hook_fn fn, void *ud)
{
    if (!fn) return NSBC_INVAL;
    pthread_mutex_lock(&g_resp_hook_lock);
    for (int i = 0; i < g_resp_hook_n; ++i)
        if (g_resp_hooks[i].fn == fn && g_resp_hooks[i].ud == ud) {
            pthread_mutex_unlock(&g_resp_hook_lock);
            return NSBC_OK;
        }
    if (g_resp_hook_n >= B2B_RESP_HOOK_MAX) {
        pthread_mutex_unlock(&g_resp_hook_lock);
        return NSBC_NOSPC;
    }
    g_resp_hooks[g_resp_hook_n].fn = fn;
    g_resp_hooks[g_resp_hook_n].ud = ud;
    ++g_resp_hook_n;
    pthread_mutex_unlock(&g_resp_hook_lock);
    return NSBC_OK;
}
void b2b_resp_hook_rm(b2b_resp_hook_fn fn, void *ud)
{
    if (!fn) return;
    pthread_mutex_lock(&g_resp_hook_lock);
    int out = 0;
    for (int i = 0; i < g_resp_hook_n; ++i) {
        if (g_resp_hooks[i].fn == fn && g_resp_hooks[i].ud == ud) continue;
        if (out != i) g_resp_hooks[out] = g_resp_hooks[i];
        ++out;
    }
    g_resp_hook_n = out;
    pthread_mutex_unlock(&g_resp_hook_lock);
}

/* ===== CDR 多槽链 ===== */
int b2b_cdr_hook_add(b2b_cdr_hook_fn fn, void *ud)
{
    if (!fn) return NSBC_INVAL;
    pthread_mutex_lock(&g_cdr_hook_lock);
    for (int i = 0; i < g_cdr_hook_n; ++i)
        if (g_cdr_hooks[i].fn == fn && g_cdr_hooks[i].ud == ud) {
            pthread_mutex_unlock(&g_cdr_hook_lock);
            return NSBC_OK;
        }
    if (g_cdr_hook_n >= B2B_CDR_HOOK_MAX) {
        pthread_mutex_unlock(&g_cdr_hook_lock);
        return NSBC_NOSPC;
    }
    g_cdr_hooks[g_cdr_hook_n].fn = fn;
    g_cdr_hooks[g_cdr_hook_n].ud = ud;
    ++g_cdr_hook_n;
    pthread_mutex_unlock(&g_cdr_hook_lock);
    return NSBC_OK;
}
void b2b_cdr_hook_rm(b2b_cdr_hook_fn fn, void *ud)
{
    if (!fn) return;
    pthread_mutex_lock(&g_cdr_hook_lock);
    int out = 0;
    for (int i = 0; i < g_cdr_hook_n; ++i) {
        if (g_cdr_hooks[i].fn == fn && g_cdr_hooks[i].ud == ud) continue;
        if (out != i) g_cdr_hooks[out] = g_cdr_hooks[i];
        ++out;
    }
    g_cdr_hook_n = out;
    pthread_mutex_unlock(&g_cdr_hook_lock);
}

/* ===== Media View 多槽链 ===== */
int b2b_media_view_hook_add(b2b_media_view_fn fn, void *ud)
{
    if (!fn) return NSBC_INVAL;
    pthread_mutex_lock(&g_mv_hook_lock);
    for (int i = 0; i < g_mv_hook_n; ++i)
        if (g_mv_hooks[i].fn == fn && g_mv_hooks[i].ud == ud) {
            pthread_mutex_unlock(&g_mv_hook_lock);
            return NSBC_OK;
        }
    if (g_mv_hook_n >= B2B_REGISTER_HOOK_MAX) {
        pthread_mutex_unlock(&g_mv_hook_lock);
        return NSBC_NOSPC;
    }
    g_mv_hooks[g_mv_hook_n].fn = fn;
    g_mv_hooks[g_mv_hook_n].ud = ud;
    ++g_mv_hook_n;
    pthread_mutex_unlock(&g_mv_hook_lock);
    return NSBC_OK;
}
void b2b_media_view_hook_rm(b2b_media_view_fn fn, void *ud)
{
    if (!fn) return;
    pthread_mutex_lock(&g_mv_hook_lock);
    int out = 0;
    for (int i = 0; i < g_mv_hook_n; ++i) {
        if (g_mv_hooks[i].fn == fn && g_mv_hooks[i].ud == ud) continue;
        if (out != i) g_mv_hooks[out] = g_mv_hooks[i];
        ++out;
    }
    g_mv_hook_n = out;
    pthread_mutex_unlock(&g_mv_hook_lock);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
