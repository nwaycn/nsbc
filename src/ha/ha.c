/* 心跳/主备仲裁（REQ-HA-001/002，P0）；状态复制与 Hitless 为 P1。
 *
 * 仲裁规则（v2，修复双 STANDBY 死锁）：
 *  - 无对端配置 → ACTIVE（单机模式）；
 *  - 对端不可见（从未收到：以本线程启动时刻为基准等满 failover_ms；
 *    或心跳中断超 failover_ms）→ ACTIVE（failover 接管）；
 *  - 对端活着 → priority 大者 ACTIVE；相等用 node_id（hostname）字典序
 *    tie-break（>=0 者胜），保证双机同时启动/重启后恰好一台 ACTIVE；
 *  - 健康门（ha_health）：本机 UNHEALTHY 连续 3 个仲裁周期 → 让位
 *    STANDBY（防病机抢主/带病接管）。单机模式绝不让位（无接管者）。
 *
 * 心跳协议 V1：NSBC-HB/1 seq role prio node\n
 * （sscanf 兼容旧 3 字段格式，旧包按 prio=100/node="" 处理）。
 * VIP 漂移由外部 keepalived/LB 执行（本进程只出角色状态 + 信令门控）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "app_ctx.h"
#include "cfg.h"
#include "ha.h"
#include "ha_bfd.h"
#include "ha_health.h"
#include "ha_repl_transport.h"
#include "log.h"
#include "module.h"
#include "net_sock.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

typedef struct {
    nsbc_ctx_t *ctx;
    pthread_t thr;
    int stop;
    int fd;
    nsbc_sa_t peer;
    int have_peer;
    uint32_t hb_ms;
    uint32_t failover_ms;
    ha_role_t role;      /* ha_role() 跨线程调用：原子读写 */
    int my_prio;
    char node_id[64];    /* hostname；priority 相同时的 tie-break */
    int peer_prio;
    char peer_node[64];
    uint64_t start_ms;   /* 线程启动时刻（last_recv==0 时的等待基准） */
    int unhealthy_run;   /* 健康门防抖计数 */
    ha_hc_t *hc;         /* NULL = 健康门降级关闭（心跳仲裁照常） */
    /* 本地健康采样状态（1s 周期） */
    uint64_t last_sample_ms;
    long last_ticks;     /* /proc/self/stat utime+stime */
    uint64_t last_iter_ms;
    int loop_lag_ms;     /* 心跳线程调度滞后：系统压力粗粒度代理 */
} ha_t;

static ha_t *g_ha;
static ha_repl_trans_t *g_repl_trans;
static ha_bfd_t *g_bfd;

ha_role_t ha_role(void)
{
    ha_t *h = g_ha;
    if (!h)
        return HA_ROLE_ACTIVE; /* 未启用 HA：单机恒 ACTIVE */
    return (ha_role_t)__atomic_load_n(&h->role, __ATOMIC_ACQUIRE);
}

/* === HA P1 状态复制 wrapper ===
 * 业务线程（registrar/b2bua）调用 —— 传输层 ops.send 内部已 SPSC 入队，
 * 零阻塞。role 守卫 + transport 未就绪 → 静默跳过（不影响业务主路径）。 */
int64_t ha_repl_publish(repl_type_t type, uint16_t ver,
                        const void *payload, uint16_t len)
{
    /* role 守卫：只有 ACTIVE 才 publish（STANDBY 不生成话务/注册，防双写） */
    if (ha_role() != HA_ROLE_ACTIVE)
        return 0;
    if (!g_repl_trans)
        return 0; /* transport 未就绪（disabled / degraded / 单机） */
    ha_repl_t *r = ha_repl_trans_repl(g_repl_trans);
    if (!r)
        return 0;
    int64_t rc = repl_publish(r, type, ver, payload, len);
    if (rc == NSBC_NOSPC) {
        /* 环形缓冲满（备端 ACK 长期落后） —— 业务侧应该转全量/告警，
         * 这里只 WARN 日志，业务线程继续跑（P0-1 不阻塞热路径） */
        llog(LOG_WARN, "ha-repl",
             "repl_publish NOSPC type=%d ver=%d → business should trigger FULL",
             (int)type, (int)ver);
    }
    return rc;
}

/* === HA P1 备端 apply 订阅链 ===
 * 线程安全：订阅/遍历 不在并发路径
 *   subscribe_apply: 业务模块 init 时调（此时 transport 线程尚未 recv）
 *   apply_proxy:     transport recv 线程调
 * init 时序保证 subscribe 发生在 proxy 第一次被调之前。 */

#define HA_REPL_APPLY_MAX 4
typedef struct {
    int  (*apply)(void *ud, const repl_rec_t *rec);
    void *ud;
} ha_repl_sub_t;

static ha_repl_sub_t g_repl_subs[HA_REPL_APPLY_MAX];
static int g_repl_sub_count;

int ha_repl_subscribe_apply(int (*apply)(void *ud, const repl_rec_t *rec),
                            void *ud)
{
    if (!apply) return NSBC_INVAL;
    /* 幂等：相同 apply+ud 不重复注册 */
    for (int i = 0; i < g_repl_sub_count; i++)
        if (g_repl_subs[i].apply == apply && g_repl_subs[i].ud == ud)
            return NSBC_OK;
    if (g_repl_sub_count >= HA_REPL_APPLY_MAX) {
        llog(LOG_WARN, "ha-repl",
             "apply subscriber FULL (max=%d) apply=%p", HA_REPL_APPLY_MAX, (void *)apply);
        return NSBC_NOSPC;
    }
    g_repl_subs[g_repl_sub_count].apply = apply;
    g_repl_subs[g_repl_sub_count].ud     = ud;
    g_repl_sub_count++;
    llog(LOG_NOTICE, "ha-repl",
         "apply subscriber #%d apply=%p ud=%p",
         g_repl_sub_count, (void *)apply, ud);
    return NSBC_OK;
}

int ha_repl_apply_proxy(void *ud, const repl_rec_t *rec)
{
    (void)ud;   /* proxy 层不感知具体业务模块 ud —— 各订阅者自带 ud */
    if (!rec) return NSBC_INVAL;
    if (g_repl_sub_count == 0) return NSBC_OK;  /* 无订阅者：骨架期 STANDBY 不重建 */
    int first_err = NSBC_OK;
    for (int i = 0; i < g_repl_sub_count; i++) {
        int rc = g_repl_subs[i].apply(g_repl_subs[i].ud, rec);
        if (rc != NSBC_OK && first_err == NSBC_OK) {
            first_err = rc;
            llog(LOG_WARN, "ha-repl",
                 "apply_proxy sub[%d] rc=%d type=%d seq=%llu",
                 i, rc, (int)rec->type, (unsigned long long)rec->seq);
        }
    }
    return first_err;
}

/* === HA P1 角色切换回调订阅链 ===
 * 心跳线程 role transition 时调 proxy — 通知 b2bua 等业务模块重建内存状态
 * （特别是 STANDBY→ACTIVE failover 时 b2bua 需要重建对话锚点）。
 *
 * 线程安全：subscribe 只在业务 init 时调；role_switch_proxy 在心跳线程（ha_main）调。
 * init 时序保证 subscribe 先于任何 role_switch。数组遍历无锁。 */

#define HA_ROLE_MAX 4
typedef struct {
    ha_role_switch_fn cb;
    void *ud;
} ha_role_sub_t;

static ha_role_sub_t g_role_subs[HA_ROLE_MAX];
static int g_role_sub_count;

int ha_repl_subscribe_role_switch(ha_role_switch_fn cb, void *ud)
{
    if (!cb) return NSBC_INVAL;
    /* 幂等：相同 cb+ud 不重复注册 */
    for (int i = 0; i < g_role_sub_count; i++)
        if (g_role_subs[i].cb == cb && g_role_subs[i].ud == ud)
            return NSBC_OK;
    if (g_role_sub_count >= HA_ROLE_MAX) {
        llog(LOG_WARN, "ha-repl",
             "role subscriber FULL (max=%d) cb=%p", HA_ROLE_MAX, (void *)cb);
        return NSBC_NOSPC;
    }
    g_role_subs[g_role_sub_count].cb = cb;
    g_role_subs[g_role_sub_count].ud = ud;
    g_role_sub_count++;
    llog(LOG_NOTICE, "ha-repl",
         "role subscriber #%d cb=%p ud=%p",
         g_role_sub_count, (void *)cb, ud);
    return NSBC_OK;
}

void ha_repl_role_switch_proxy(ha_role_t new_role)
{
    if (g_role_sub_count == 0) return;
    for (int i = 0; i < g_role_sub_count; i++) {
        llog(LOG_INFO, "ha-repl",
             "role_switch_proxy sub[%d] → %s",
             i, new_role == HA_ROLE_ACTIVE ? "ACTIVE" : "STANDBY");
        g_role_subs[i].cb(g_role_subs[i].ud, new_role);
    }
}

int ha_repl_connected(void)
{
    return ha_repl_trans_connected(g_repl_trans);
}

uint64_t ha_repl_last_ack(void)
{
    return ha_repl_trans_last_ack(g_repl_trans);
}

/* === HA P1 Hitless Upgrade 冻结 stub（骨架实现：纯 atomic flag）===
 * 正式接线时 b2bua.c INVITE 门控会读 ha_is_frozen()，为 1 时主机也拒绝新呼。 */
static int g_frozen;

void ha_freeze_input(int freeze_ms)
{
    __atomic_store_n(&g_frozen, 1, __ATOMIC_RELEASE);
    llog(LOG_NOTICE, "ha", "hitless freeze input freeze_ms=%d", freeze_ms);
    (void)freeze_ms; /* P1 skeleton: auto-unfreeze 定时器由 shm_seg publish path 触发 */
}

void ha_unfreeze_input(void)
{
    __atomic_store_n(&g_frozen, 0, __ATOMIC_RELEASE);
    llog(LOG_NOTICE, "ha", "hitless unfreeze input");
}

int ha_is_frozen(void)
{
    return __atomic_load_n(&g_frozen, __ATOMIC_ACQUIRE);
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* /proc/self/stat 的 utime+stime（时钟滴答）；失败返回 -1 */
static long proc_cpu_ticks(void)
{
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f)
        return -1;
    char buf[1024];
    long ticks = -1;
    if (fgets(buf, sizeof(buf), f)) {
        char *p = strrchr(buf, ')'); /* comm 可含空格，从 ')' 后重定位 */
        if (p) {
            unsigned long ut = 0, st = 0;
            /* ') ' 之后：state ppid pgrp session tty tpgid flags
             * minflt cminflt majflt cmajflt | utime stime */
            if (sscanf(p + 2,
                       "%*s %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %lu %lu",
                       &ut, &st) == 2)
                ticks = (long)(ut + st);
        }
    }
    fclose(f);
    return ticks;
}

/* /proc/self/status 的 VmRSS（MB）；失败返回 0 */
static int proc_rss_mb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return 0;
    char line[128];
    long kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "VmRSS:", 6)) {
            sscanf(line + 6, "%ld", &kb);
            break;
        }
    }
    fclose(f);
    return (int)(kb / 1024);
}

/* 1s 周期本地健康采样：CPU%（进程占全机核数比）、RSS、调度滞后 */
static void sample_local_health(ha_t *h)
{
    ha_hc_sample_t s;
    memset(&s, 0, sizeof(s));

    long ticks = proc_cpu_ticks();
    uint64_t now = mono_ms();

    if (h->last_sample_ms && ticks >= 0 && now > h->last_sample_ms) {
        long hz = sysconf(_SC_CLK_TCK);
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        long dt = ticks - h->last_ticks;
        if (hz <= 0)
            hz = 100;
        if (ncpu < 1)
            ncpu = 1;
        if (dt >= 0) {
            double busy_s = (double)dt / (double)hz;
            double pct = busy_s /
                         ((double)(now - h->last_sample_ms) / 1000.0) *
                         100.0 / (double)ncpu;
            if (pct < 0)
                pct = 0;
            if (pct > 100)
                pct = 100;
            s.cpu_pct = (int)(pct + 0.5);
        }
    }
    s.rss_mb = proc_rss_mb();
    s.evloop_lag_ms = h->loop_lag_ms; /* ha 线程周期超期：调度压力代理 */
    s.overload_reject_rate_pct = 0;   /* OAM 过载拒绝率 P1 接入 */

    /* === HA P1: 状态复制水位差 ===
     * ACTIVE: repl_lag = published_seq - last_ack_seq（主端看备端落尾）
     * STANDBY: repl_lag = repl_apply_proxy 推进的 applied_seq vs 主端发的 published_seq
     *          简化：用 transport last_ack 本身（备端只负责 apply，没有 published）
     *          所以备端 lag=0 合理 — repl lag 只在 ACTIVE 有意义 */
    if (g_repl_trans) {
        ha_repl_t *r = ha_repl_trans_repl(g_repl_trans);
        if (r) {
            uint64_t pub = repl_published_seq(r);
            uint64_t ack = ha_repl_trans_last_ack(g_repl_trans);
            /* published_seq 和 ack_seq 都是单调增的 64-bit：
             * ACTIVE publish 后 published_seq 立刻++；STANDBY apply 后 repl_ack() 推 ack
             * 如果 pub < ack（wrap 或备端更超前）→ lag=0 */
            s.repl_lag = (pub > ack) ? (pub - ack) : 0;
        }
    }

    h->last_sample_ms = now;
    h->last_ticks = ticks;
    ha_hc_sample_local(h->hc, &s);
}

static void *ha_main(void *arg)
{
    ha_t *h = (ha_t *)arg;
    uint64_t seq = 0;
    uint64_t last_send = 0;
    uint64_t last_recv = 0; /* 0=从未收到 */
    h->start_ms = mono_ms();
    h->role = HA_ROLE_STANDBY;
    char in[256];

    while (!__atomic_load_n(&h->stop, __ATOMIC_ACQUIRE)) {
        uint64_t now = mono_ms();

        /* 循环周期超期 = 线程被调度延迟（系统压力粗粒度代理） */
        if (h->last_iter_ms) {
            int expect = (int)(h->hb_ms + h->hb_ms / 4);
            int lag = (int)(now - h->last_iter_ms) - expect;
            h->loop_lag_ms = lag > 0 ? lag : 0;
        }
        h->last_iter_ms = now;

        if (now - last_send >= h->hb_ms) {
            char pkt[192];
            int n = snprintf(pkt, sizeof(pkt), "NSBC-HB/1 %llu %d %d %s\n",
                             (unsigned long long)++seq,
                             (int)__atomic_load_n(&h->role,
                                                  __ATOMIC_RELAXED),
                             h->my_prio, h->node_id);
            if (h->have_peer) {
                ssize_t sent = sendto(h->fd, pkt, (size_t)n, MSG_NOSIGNAL,
                                      (struct sockaddr *)&h->peer.ss, h->peer.len);
                if (sent < 0) {
                    llog(LOG_WARN, "ha", "heartbeat sendto failed errno=%d peer=%s",
                         errno, h->node_id);
                } else {
                    last_send = now;
                }
            } else {
                last_send = now;
            }
        }
        struct timeval tv = {h->hb_ms / 1000,
                             (int)(h->hb_ms % 1000) * 1000};
        if (tv.tv_sec == 0 && tv.tv_usec == 0)
            tv.tv_usec = 10000;
        setsockopt(h->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        /* 仅接受已配置对端的心跳，防伪造保活把本机钉在 STANDBY */
        nsbc_sa_t src;
        socklen_t sl = sizeof(src.ss);
        ssize_t r = recvfrom(h->fd, in, sizeof(in) - 1, 0,
                             (struct sockaddr *)&src.ss, &sl);
        src.len = sl;
        int got_hb = 0;
        if (r > 0 &&
            (!h->have_peer || nsbc_sa_eq_addr(&src, &h->peer))) {
            in[r] = '\0';
            last_recv = mono_ms();
            got_hb = 1;
            unsigned long long pseq;
            int prole, pprio = 100;
            char pnode[64] = "";
            /* V1 四字段；旧 3 字段包 sscanf 返回 2，按默认值处理 */
            if (sscanf(in, "NSBC-HB/1 %llu %d %d %63s",
                       &pseq, &prole, &pprio, pnode) >= 3) {
                (void)pseq;
                h->peer_prio = pprio;
                snprintf(h->peer_node, sizeof(h->peer_node), "%s", pnode);
                /* 双主瞬间（如网络分区愈合）：留痕一拍，随仲裁收敛 */
                if (prole == (int)HA_ROLE_ACTIVE &&
                    __atomic_load_n(&h->role, __ATOMIC_RELAXED) ==
                        HA_ROLE_ACTIVE)
                    llog(LOG_WARN, "ha",
                         "dual-ACTIVE seen (peer node=%s prio=%d); arbitrating",
                         h->peer_node, h->peer_prio);
            }
        }

        /* 健康检查接线：对端探测（连续 3 次空手 → 不可见）+ 1s 本地采样 */
        if (h->hc) {
            ha_hc_peer_probe(h->hc, got_hb, got_hb ? 0 : -1);
            if (mono_ms() - h->last_sample_ms >= 1000)
                sample_local_health(h);
        }

        /* ---- 仲裁 ----
         * 1) 无对端 → ACTIVE（单机）
         * 2) 对端不可见 → failover 窗口后接管（修 v1 死锁：从未收到时
         *    以 start_ms 为基准，而非误用系统开机时间）
         * 3) 对端活着 → priority 大者主；相等 hostname 字典序 tie-break
         *    （v1 缺陷：双方互见即双双 STANDBY，集群无主）
         * 注意：recvfrom 可能阻塞一个心跳周期，必须重取时钟，否则
         * now(旧) - last_recv(新) 无符号下溢会误判对端死亡。 */
        now = mono_ms();
        int peer_alive = h->have_peer && last_recv != 0 &&
                         (now - last_recv) <= h->failover_ms;
        ha_role_t want;
        if (!h->have_peer) {
            want = HA_ROLE_ACTIVE;
        } else if (!peer_alive) {
            uint64_t base = (last_recv != 0) ? last_recv : h->start_ms;
            want = (now - base) > h->failover_ms ? HA_ROLE_ACTIVE
                                                 : HA_ROLE_STANDBY;
        } else if (h->my_prio != h->peer_prio) {
            want = h->my_prio > h->peer_prio ? HA_ROLE_ACTIVE
                                             : HA_ROLE_STANDBY;
        } else {
            want = strcmp(h->node_id, h->peer_node) >= 0
                       ? HA_ROLE_ACTIVE
                       : HA_ROLE_STANDBY;
        }

        /* 健康门：集群模式下本机 UNHEALTHY 连续 3 个仲裁周期让位 */
        if (want == HA_ROLE_ACTIVE && h->have_peer && h->hc &&
            ha_hc_local(h->hc) == HA_HC_UNHEALTHY) {
            if (++h->unhealthy_run >= 3) {
                want = HA_ROLE_STANDBY;
                llog(LOG_WARN, "ha",
                     "local UNHEALTHY x%d (lag=%dms), yield ACTIVE to peer",
                     h->unhealthy_run, h->loop_lag_ms);
            }
        } else {
            h->unhealthy_run = 0;
        }

        if (want != h->role) {
            ha_role_t old = h->role;
            __atomic_store_n(&h->role, want, __ATOMIC_RELEASE);
            llog(LOG_NOTICE, "ha",
                 "role %s -> %s (peer %s, me prio=%d node=%s, peer prio=%d node=%s)",
                 old == HA_ROLE_ACTIVE ? "ACTIVE" : "STANDBY",
                 want == HA_ROLE_ACTIVE ? "ACTIVE" : "STANDBY",
                 peer_alive ? "alive" : "down/absent",
                 h->my_prio, h->node_id, h->peer_prio, h->peer_node);
            /* A4：经 ctx 事件槽上报，不横向 include oam/obs.h */
            if (h->ctx->event_fn)
                h->ctx->event_fn(want == HA_ROLE_ACTIVE ? 4 : 6,  /* FAILOVER=WARN, STANDBY=INFO */
                                 want == HA_ROLE_ACTIVE ? "HA_FAILOVER"
                                                        : "HA_STANDBY",
                                 "role transition (%s)",
                                 want == HA_ROLE_ACTIVE ? "peer lost"
                                                        : "peer seen");
            log_audit("ha", want == HA_ROLE_ACTIVE ? "failover" : "standby",
                      "HA role transition");
            /* === HA P1 状态复制：角色切换通知 transport 重连方向 === */
            ha_repl_trans_notify_role(g_repl_trans, want);
            /* === HA P1 角色切换多路通知 — 业务模块重建内存状态 ===
             * b2bua: STANDBY→ACTIVE 时遍历 ha_dlg_buf 重建对话锚点
             * registrar: 预留（可选重建 loc_db 过期注销） */
            ha_repl_role_switch_proxy(want);
        }
        usleep((useconds_t)h->hb_ms * 1000 / 4);
    }
    return NULL;
}

static int ha_init(struct nsbc_ctx *ctx)
{
    int enabled = 0;
    cfg_get_bool(ctx->cfg, "ha.enabled", &enabled, NULL);
    if (!enabled)
        return NSBC_OK;

    ha_t *h = calloc(1, sizeof(*h));
    if (!h)
        return NSBC_NOMEM;
    h->ctx = ctx;
    long hb = 500, fo = 3000;
    cfg_get_int(ctx->cfg, "ha.heartbeat_ms", &hb, NULL);
    cfg_get_int(ctx->cfg, "ha.failover_ms", &fo, NULL);
    if (hb < 50)
        hb = 500;
    if (fo < hb)
        fo = 3000;
    h->hb_ms = (uint32_t)hb;
    h->failover_ms = (uint32_t)fo;

    long prio = 100;
    cfg_get_int(ctx->cfg, "ha.priority", &prio, NULL);
    if (prio < 1)
        prio = 100; /* 未配置/非法 → 默认 */
    h->my_prio = (int)prio;
    if (gethostname(h->node_id, sizeof(h->node_id) - 1) != 0 ||
        !h->node_id[0])
        snprintf(h->node_id, sizeof(h->node_id), "node-%d", h->my_prio);
    h->node_id[sizeof(h->node_id) - 1] = '\0';
    h->peer_prio = 100; /* 对端旧格式/未收到心跳时的默认 */

    if (ha_hc_create(NULL, &h->hc) != NSBC_OK)
        h->hc = NULL; /* 健康门降级关闭，不影响心跳仲裁 */

    h->role = HA_ROLE_STANDBY;

    h->fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (h->fd < 0) {
        /* ha 已是 STATIC 模块：init 返回 NSBC_ERR 会走 main.c 致命路径
         * （shutdown_failed → 进程退出）。心跳链路是可用性增强而非
         * 必要条件——降级 solo-active（g_ha 保持 NULL，ha_role() 恒
         * ACTIVE），绝不因 HA 自身故障拖死整机。 */
        llog(LOG_WARN, "ha",
             "socket create failed errno=%d; degrade to solo-active", errno);
        if (h->hc)
            ha_hc_destroy(h->hc);
        free(h);
        return NSBC_OK;
    }
    long local_port = 5070;
    const char *peer = NULL;
    cfg_get_str(ctx->cfg, "ha.peer", &peer, NULL);
    if (peer && *peer) {
        char host[128];
        snprintf(host, sizeof(host), "%s", peer);
        char *colon = strrchr(host, ':');
        long port = 5070;
        if (colon) {
            *colon = '\0';
            port = atol(colon + 1);
        }
        if (nsbc_sa_from(&h->peer, host, (uint16_t)port, AF_INET) ==
            NSBC_OK) {
            h->have_peer = 1;
            local_port = port; /* 对称心跳：双方同端口互发 */
        } else {
            llog(LOG_WARN, "ha", "bad ha.peer %s; running solo-active",
                 peer);
        }
    }
    /* 绑定固定本地端口，否则对端无法把心跳发到本 socket */
    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_ANY);
    la.sin_port = htons((uint16_t)local_port);
    if (bind(h->fd, (struct sockaddr *)&la, sizeof(la)) != 0) {
        /* 端口被占（常见：残留的旧 nsbcd 未退干净、或对端进程还在跑）。
         * 同上：降级 solo-active 而非致命退出——STANDBY 仲裁不可用时
         * 让本机无条件 ACTIVE，呼叫与 CDR 链路不受影响。 */
        llog(LOG_WARN, "ha",
             "bind udp/%ld failed errno=%d; degrade to solo-active",
             local_port, errno);
        close(h->fd);
        if (h->hc)
            ha_hc_destroy(h->hc);
        free(h);
        return NSBC_OK;
    }
    if (pthread_create(&h->thr, NULL, ha_main, h) != 0) {
        llog(LOG_WARN, "ha",
             "heartbeat thread create failed errno=%d; degrade to solo-active",
             errno);
        close(h->fd);
        if (h->hc)
            ha_hc_destroy(h->hc);
        free(h);
        return NSBC_OK;
    }
    g_ha = h;
    llog(LOG_NOTICE, "ha",
         "heartbeat enabled peer=%s hb=%ums failover=%ums prio=%d node=%s",
         h->have_peer ? peer : "-", h->hb_ms, h->failover_ms, h->my_prio,
         h->node_id);

    /* === HA P1 状态复制传输层 ===
     * 默认 ha.replica.enabled=false → transport 自己返回 NSBC_NOENT，不浪费资源。
     * 显式开启 + have_peer → 尝试创建。
     * 失败降级：transport 内部已兜底（listen/socket 失败→返回 NSBC_OK 让调用方继续）
     * 本层不再额外降级，保持心跳仲裁正常运行。 */
    {
        int rep_en = 0;
        cfg_get_bool(ctx->cfg, "ha.replica.enabled", &rep_en, NULL);
        if (rep_en && h->have_peer) {
            long rep_port = 5071;
            long ring_cap = 8192;
            long payload_max = 4096;
            cfg_get_int(ctx->cfg, "ha.replica.port", &rep_port, NULL);
            cfg_get_int(ctx->cfg, "ha.replica.ring_cap", &ring_cap, NULL);
            cfg_get_int(ctx->cfg, "ha.replica.payload_max", &payload_max, NULL);
            ha_repl_trans_cfg_t tcfg;
            memset(&tcfg, 0, sizeof(tcfg));
            tcfg.enabled = 1;
            tcfg.port = (uint16_t)rep_port;
            tcfg.ring_cap = (uint32_t)ring_cap;
            tcfg.payload_max = (uint32_t)payload_max;
            tcfg.is_active = 0;   /* 角色稍后通知；init 时 role 尚未仲裁 */
            tcfg.have_peer = 1;
            tcfg.peer_addr = h->peer;
            int rc = ha_repl_trans_create(&tcfg, &g_repl_trans);
            if (rc == NSBC_OK && g_repl_trans) {
                /* transport 初始化后立即通知当前 role（避免首次仲裁前角色=STANDBY） */
                ha_repl_trans_notify_role(g_repl_trans, HA_ROLE_STANDBY);
            }
        }
    }

    /* === HA P1 BFD 亚秒探测（与 UDP 5070 心跳互补）===
     * 默认 ha.bfd.enabled=false → 跳过。显式开启 + have_peer → 创建独立 BFD Control socket。
     * ha_bfd 线程每收到 Control 调 ha_hc_peer_probe()，ha_health 仲裁层统一判定。 */
    {
        int bfd_en = 0;
        cfg_get_bool(ctx->cfg, "ha.bfd.enabled", &bfd_en, NULL);
        if (bfd_en && h->have_peer) {
            long local_disc = 1;
            long tx_ms = 200, rx_ms = 200, mult = 3;
            cfg_get_int(ctx->cfg, "ha.bfd.local_discriminator", &local_disc, NULL);
            cfg_get_int(ctx->cfg, "ha.bfd.desired_min_tx_interval", &tx_ms, NULL);
            cfg_get_int(ctx->cfg, "ha.bfd.required_min_rx_interval", &rx_ms, NULL);
            cfg_get_int(ctx->cfg, "ha.bfd.detect_multiplier", &mult, NULL);
            ha_bfd_cfg_t bcfg;
            memset(&bcfg, 0, sizeof(bcfg));
            bcfg.enabled = 1;
            bcfg.local_discriminator = (uint32_t)local_disc;
            bcfg.desired_min_tx_interval = (uint32_t)tx_ms;
            bcfg.required_min_rx_interval = (uint32_t)rx_ms;
            bcfg.detect_multiplier = (uint8_t)mult;
            bcfg.have_peer = 1;
            bcfg.peer_addr = h->peer;
            bcfg.hc = h->hc;     /* BFD 线程通过 ha_hc_peer_probe 更新健康门探测结果 */
            ha_bfd_create(&bcfg, &g_bfd);
        }
    }

    return NSBC_OK;
}

static void ha_fini(struct nsbc_ctx *ctx)
{
    (void)ctx;
    ha_t *h = g_ha;
    if (!h)
        return;
    __atomic_store_n(&h->stop, 1, __ATOMIC_RELEASE);
    pthread_join(h->thr, NULL);
    close(h->fd);
    if (h->hc)
        ha_hc_destroy(h->hc);
    /* === HA P1 状态复制传输层 ===
     * 心跳线程已停（pthread_join 已返回），此时 destroy transport 安全 */
    if (g_repl_trans) {
        ha_repl_trans_destroy(g_repl_trans);
        g_repl_trans = NULL;
    }
    /* === HA P1 BFD === */
    if (g_bfd) {
        ha_bfd_destroy(g_bfd);
        g_bfd = NULL;
    }
    g_ha = NULL;
    free(h);
}

NSBC_MOD0_CAT(ha, 1, MOD_L_FINAL, 0, MOD_CAT_CORE, ha_init, ha_fini);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
