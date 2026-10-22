/* Media Bug — 按需 RTP payload 探针（REQ-MEDIA-BUG-001，P0）
 *
 * 类似 FreeSWITCH `uuid_bug`：对指定呼叫的某一 leg（A=0 / B=1）挂监听，
 * 抓取该 leg 的 RTP payload（PCMU/PCMA/G.729 等原始音频帧，不含 RTP 头）
 * 通过 UDP 发往外部分析器（pcap/录音/ASR 前置/关键词检测/窃听取证）。
 *
 * 架构：
 *   bridge_add_rec_feed_hook  —— 只注册**一次全局 hook**（媒体 recv 方向）
 *   listener 链表（per-call） —— bug.add 动态挂 / bug.del 动态摘
 *   hook 内 per-回调查链表     —— 匹配才 sendto，不命中快速返回（<1μs）
 *
 * 为何全局 hook + per-call listener，而不是 per-call hook？
 *   bridge hook 链表每元素回调一次，per-call hook 会 O(listener_count)，
 *   多 bug 叠加时媒体线程成本线性增长。全局 hook 一次回调，内部自己做
 *   call_key+leg 匹配 → 命中 sendto，不命中快速返回。
 *
 * UDS 控制命令（注册到 obs_control CCMD 注册表）：
 *   bug.add  <call_id> <leg> <host:port> [pt]     attach listener (L2 ADMIN)
 *   bug.del  <call_id> [leg]                       detach listener (L2 ADMIN)
 *   bug.list                                        list active bugs (L1 MONITOR)
 *
 * 线程模型：
 *   hook 回调（媒体线程）持 g_bug.lock 读 listener 链表
 *   bug.add/del（UDS reactor 线程）持锁写链表
 *   sendto 在锁外执行（不阻塞其他 listener）
 *
 * 降级策略：
 *   sendto EAGAIN：立即丢帧（MSG_DONTWAIT，不阻塞媒体线程）
 *   sendto 其他 errno：记 drop++，继续跑（不自动关闭 listener，让 admin 清理）
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "module.h"
#include "log.h"
#include "media_bridge.h"
#include "b2bua.h"          /* b2b_call_t —— call_key 类型 + leg[].callid 字段 */
#include "obs.h"            /* obs_control_get_cmd_reg() 拿 CCMD 注册表 */
#include "cmd_registry.h"   /* cmd_register / cmd_desc_t / cmd_ctx_t */

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* sig_bridge_instance() 定义在 signaling.c（core 层），无独立 header ——
 * extern 声明让这个 dlopen 模块能 link 到（主二进制 -export-dynamic 已加）*/
struct nsbc_ctx;
extern bridge_mgr_t *sig_bridge_instance(struct nsbc_ctx *ctx);

/* ================================================================== */
/* 数据结构                                                             */
/* ================================================================== */

#define BUG_LISTENER_MAX 64    /* 单进程最多 64 个活跃 bug 会话 */

typedef struct bug_listener {
    char     call_id[36];      /* b2b_call_t.leg[0].callid（32 字符 hex） */
    void    *call_key;         /* b2b_call_t* 指针（hook 回调 O(1) 匹配） */
    int      leg;              /* 0=A / 1=B；-1=不限（所有 leg）*/
    int      pt;               /* payload type 过滤；-1=不限 */
    int      sockfd;           /* UDP socket（已 connect 对端）*/
    char     host[128];
    uint16_t port;
    uint64_t sent;             /* 已发帧数 */
    uint64_t dropped;          /* EAGAIN 丢帧数 */
    struct bug_listener *next;
} bug_listener_t;

typedef struct {
    bridge_mgr_t *bridge;
    void         *bridge_hook_ptr;
    bug_listener_t *listeners;
    int            n_listeners;
    pthread_mutex_t lock;
} bug_store_t;

static bug_store_t g_bug;

/* ================================================================== */
/* hook 回调（媒体线程，每帧 RTP recv 都进）                             */
/* ================================================================== */

static void bug_rec_feed(void *ud, void *call_key, int leg, int pt,
                         const void *payload, size_t len)
{
    bug_store_t *st = (bug_store_t *)ud;
    /* 快速路径：没有活跃 listener → 立即返回（<1μs，媒体线程零成本）*/
    pthread_mutex_lock(&st->lock);
    if (st->n_listeners == 0) { pthread_mutex_unlock(&st->lock); return; }

    /* 从 b2b_call_t* 拿 call_id 字符串（call_key 就是 b2b_call_t*）
     * hook 只给了 call_key，call_id 存 leg[0].callid 字段里（公开结构）*/
    const char *cid = NULL;
    if (call_key) {
        b2b_call_t *c = (b2b_call_t *)call_key;
        if (c->leg[0].callid[0]) cid = c->leg[0].callid;
    }

    /* 快照匹配 listener 数组（锁内复制指针，锁外 sendto —— 不阻塞写侧）*/
    bug_listener_t *matches[BUG_LISTENER_MAX];
    int match_n = 0;
    for (bug_listener_t *l = st->listeners; l; l = l->next) {
        int call_match = (l->call_key == call_key) ||
                         (cid && l->call_id[0] && strcmp(l->call_id, cid) == 0);
        if (!call_match) continue;
        if (l->leg >= 0 && l->leg != leg) continue;
        if (l->pt >= 0 && l->pt != pt) continue;
        matches[match_n++] = l;
    }
    pthread_mutex_unlock(&st->lock);
    if (match_n == 0) return;

    /* 锁外 sendto —— 每个 listener 独立 EAGAIN/drop，互不阻塞 */
    for (int i = 0; i < match_n; ++i) {
        bug_listener_t *l = matches[i];
        ssize_t w = sendto(l->sockfd, payload, len, MSG_NOSIGNAL | MSG_DONTWAIT,
                           NULL, 0); /* UDP connect 已绑对端 */
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                l->dropped++;
            }
            /* EBADF/ENOTCONN 等其他 errno 不自动关闭 —— 让 admin bug.del 清理 */
        } else {
            l->sent++;
        }
    }
}

/* ================================================================== */
/* listener 链表操作（持 g_bug.lock）                                   */
/* ================================================================== */

static int bug_add_listener(const char *call_id, void *call_key, int leg, int pt,
                            const char *host, uint16_t port)
{
    /* 防重复：同 call_id+leg+host+port+pt → 返回 OK（幂等）*/
    for (bug_listener_t *l = g_bug.listeners; l; l = l->next) {
        if (strcmp(l->call_id, call_id) == 0 && l->leg == leg && l->pt == pt &&
            strcmp(l->host, host) == 0 && l->port == port) {
            return NSBC_OK;
        }
    }
    if (g_bug.n_listeners >= BUG_LISTENER_MAX) {
        llog(LOG_WARN, "bug", "listener table full (%d)", BUG_LISTENER_MAX);
        return NSBC_NOSPC;
    }

    /* UDP socket + connect 对端（简化 sendto，后续 sendto 不需要再给地址）*/
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { llog(LOG_WARN, "bug", "socket(): %s", strerror(errno)); return NSBC_ERR; }
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sin.sin_addr) != 1) {
        /* 不是 IPv4：尝试 DNS（只 AF_INET，保持简单）*/
        struct addrinfo *ai = NULL;
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
        if (getaddrinfo(host, NULL, &hints, &ai) != 0 || !ai) {
            llog(LOG_WARN, "bug", "inet_pton+DNS fail host=%s", host);
            close(fd); return NSBC_ERR;
        }
        memcpy(&sin, ai->ai_addr, sizeof(sin));
        freeaddrinfo(ai); sin.sin_port = htons(port);
    }
    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        llog(LOG_WARN, "bug", "connect %s:%u: %s", host, port, strerror(errno));
        close(fd); return NSBC_ERR;
    }

    bug_listener_t *nl = (bug_listener_t *)calloc(1, sizeof(*nl));
    if (!nl) { close(fd); return NSBC_NOMEM; }
    snprintf(nl->call_id, sizeof(nl->call_id), "%s", call_id);
    nl->call_key = call_key; nl->leg = leg; nl->pt = pt;
    nl->sockfd = fd;
    snprintf(nl->host, sizeof(nl->host), "%s", host);
    nl->port = port;
    nl->next = g_bug.listeners;
    g_bug.listeners = nl;
    ++g_bug.n_listeners;

    llog(LOG_INFO, "bug", "ADDED call=%s key=%p leg=%d pt=%d → %s:%u",
         call_id, call_key, leg, pt, host, port);
    return NSBC_OK;
}

static int bug_del_listener(const char *call_id, int leg)
{
    bug_listener_t **pp = &g_bug.listeners;
    int removed = 0;
    while (*pp) {
        bug_listener_t *l = *pp;
        if (strcmp(l->call_id, call_id) == 0 && (leg < 0 || l->leg == leg)) {
            *pp = l->next;
            close(l->sockfd);
            llog(LOG_INFO, "bug", "DELETED call=%s leg=%d sent=%lu dropped=%lu",
                 call_id, l->leg, (unsigned long)l->sent, (unsigned long)l->dropped);
            free(l); ++removed; --g_bug.n_listeners;
        } else {
            pp = &l->next;
        }
    }
    return removed ? NSBC_OK : NSBC_NOENT;
}

static void bug_del_all(void)
{
    bug_listener_t *l = g_bug.listeners;
    while (l) { bug_listener_t *nx = l->next; close(l->sockfd); free(l); l = nx; }
    g_bug.listeners = NULL; g_bug.n_listeners = 0;
}

/* ================================================================== */
/* CCMD 命令（cmd_fn_t 风格：argc/argv 自动切词 + cmd_ctx_t.io_printf 输出）*/
/* ================================================================== */

static int cmd_bug_add(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)ud;
    if (argc < 3 || argc > 4) {
        c->io_printf(c->io_ud, "usage: bug.add <call_id> <leg> <host:port> [pt]\n");
        return NSBC_INVAL;
    }
    const char *call_id = argv[0];
    int leg = atoi(argv[1]);
    char *hp = argv[2];
    int pt  = (argc == 4) ? atoi(argv[3]) : -1;
    char *colon = strrchr(hp, ':');
    if (!colon) {
        c->io_printf(c->io_ud, "ERR host:port expected\n");
        return NSBC_INVAL;
    }
    *colon = '\0';
    const char *host = hp;
    uint16_t port = (uint16_t)atoi(colon + 1);

    pthread_mutex_lock(&g_bug.lock);
    int rc = bug_add_listener(call_id, NULL, leg, pt, host, port);
    pthread_mutex_unlock(&g_bug.lock);
    if (rc == NSBC_OK)
        c->io_printf(c->io_ud, "OK bug.add call=%s leg=%d pt=%d → %s:%u\n",
                     call_id, leg, pt, host, port);
    else
        c->io_printf(c->io_ud, "ERR bug.add rc=%d\n", rc);
    return rc;
}

static int cmd_bug_del(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)ud;
    if (argc < 1 || argc > 2) {
        c->io_printf(c->io_ud, "usage: bug.del <call_id> [leg]\n");
        return NSBC_INVAL;
    }
    int leg = (argc == 2) ? atoi(argv[1]) : -1;

    pthread_mutex_lock(&g_bug.lock);
    int rc = bug_del_listener(argv[0], leg);
    pthread_mutex_unlock(&g_bug.lock);
    if (rc == NSBC_OK)
        c->io_printf(c->io_ud, "OK bug.del call=%s leg=%d removed\n", argv[0], leg);
    else
        c->io_printf(c->io_ud, "ERR bug.del rc=%d\n", rc);
    return rc;
}

static int cmd_bug_list(cmd_ctx_t *c, int argc, char **argv, void *ud)
{
    (void)argc; (void)argv; (void)ud;
    pthread_mutex_lock(&g_bug.lock);
    c->io_printf(c->io_ud, "bug.listeners=%d\n", g_bug.n_listeners);
    for (bug_listener_t *l = g_bug.listeners; l; l = l->next) {
        c->io_printf(c->io_ud,
          "  call=%s leg=%d pt=%d → %s:%u sock=%d sent=%lu dropped=%lu\n",
          l->call_id, l->leg, l->pt, l->host, l->port, l->sockfd,
          (unsigned long)l->sent, (unsigned long)l->dropped);
    }
    pthread_mutex_unlock(&g_bug.lock);
    return NSBC_OK;
}

/* ================================================================== */
/* init / fini                                                          */
/* ================================================================== */

static int bug_init(struct nsbc_ctx *ctx)
{
    memset(&g_bug, 0, sizeof(g_bug));
    pthread_mutex_init(&g_bug.lock, NULL);

    /* 拿全局 bridge_mgr（core/signaling 层导出）*/
    g_bug.bridge = sig_bridge_instance(ctx);
    if (!g_bug.bridge) {
        llog(LOG_WARN, "bug", "sig_bridge_instance() null — media_bug disabled");
        return -1;   /* 非 REQUIRED 模块：init 失败只禁用，不阻断启动 */
    }
    g_bug.bridge_hook_ptr = bridge_add_rec_feed_hook(
        g_bug.bridge, bug_rec_feed, &g_bug);
    if (!g_bug.bridge_hook_ptr) {
        llog(LOG_WARN, "bug", "bridge_add_rec_feed_hook failed");
        return -1;
    }
    llog(LOG_INFO, "bug", "init OK hook registered bridge=%p", (void *)g_bug.bridge);

    /* 注册到 obs_control CCMD 注册表（动态模块，init 时注册表已就绪）*/
    cmd_reg_t *reg = obs_control_get_cmd_reg();
    if (!reg) {
        llog(LOG_WARN, "bug", "obs_control_get_cmd_reg() null — commands not registered");
        return 0; /* hook 仍然有效，只是 CLI 不可控 */
    }
    static const cmd_desc_t descs[] = {
        { "bug.add",  "attach RTP payload listener",     CMD_L2_ADMIN, cmd_bug_add,  NULL },
        { "bug.del",  "detach listener",                 CMD_L2_ADMIN, cmd_bug_del,  NULL },
        { "bug.list", "list active bugs with counters",  CMD_L1_MONITOR, cmd_bug_list, NULL },
    };
    for (size_t i = 0; i < sizeof(descs)/sizeof(descs[0]); ++i) {
        int rc = cmd_register(reg, &descs[i]);
        llog(LOG_INFO, "bug", "cmd_register('%s') rc=%d", descs[i].name, rc);
    }
    return 0;
}

static void bug_fini(struct nsbc_ctx *ctx)
{
    (void)ctx;
    llog(LOG_INFO, "bug", "fini");

    /* 卸载 CCMD 命令 */
    cmd_reg_t *reg = obs_control_get_cmd_reg();
    if (reg) {
        cmd_unregister(reg, "bug.add");
        cmd_unregister(reg, "bug.del");
        cmd_unregister(reg, "bug.list");
    }

    /* 摘 hook + 关所有 listener */
    pthread_mutex_lock(&g_bug.lock);
    bug_del_all();
    pthread_mutex_unlock(&g_bug.lock);
    if (g_bug.bridge && g_bug.bridge_hook_ptr) {
        bridge_remove_rec_feed_hook(g_bug.bridge, g_bug.bridge_hook_ptr);
        g_bug.bridge_hook_ptr = NULL;
    }
    pthread_mutex_destroy(&g_bug.lock);
}

static const char *bug_deps[] = { "signaling", NULL };

NSBC_MOD_CAT(media_bug, "1.0", MOD_L_WORKER_READY, 0, MOD_CAT_MEDIA_BUG,
          bug_deps,
          bug_init, NULL, NULL, bug_fini);
