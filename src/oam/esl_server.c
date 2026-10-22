/* esl_server.c — 统一 ESL/MI Socket（Phase 2 增量 2026-10-15）
 *
 * 设计：
 *   - 复用 obs_control 的 reactor_add 模式（非阻塞 accept + epoll 多路复用）
 *   - 双监听：UDS（本机特权） + TCP（网络，可加防火墙）
 *   - 连接管理：单链表维护，每个连接存 fd + 订阅位图 + 接收缓冲 + 认证状态
 *   - 命令路由：api → cmd_registry（复用 obs_control 的注册表）
 *   - 事件推送：esl_event_fire 遍历连接链表，订阅位图 & 事件位 → 推送
 *   - 非阻塞写：一次 write 完不完继续下一个连接，不阻塞
 *
 * TODO：
 *   - TLS 加密（P1，TCP 口 mTLS）
 *   - 命令哈希优化（当前线性扫描 cmd_registry，256 条内够用）
 */

#include "esl.h"
#include "cmd_registry.h"
#include "log.h"
#include "nsbc_common.h"
#include "reactor.h"
#include "event_bus.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* ================================================================
 * 常量
 * ================================================================ */

#define ESL_BUF_MAX     (16 * 1024)  /* 接收/发送缓冲 */
#define ESL_LINE_MAX    2048          /* 单行命令最大长度 */
#define ESL_MAX_CLIENTS 64            /* 默认最大并发 */
#define ESL_MAX_ARGC    32            /* 单命令最多参数 */

/* ================================================================
 * 连接对象
 * ================================================================ */

typedef struct esl_conn {
    int fd;
    uint32_t subscribed;   /* 事件订阅位图 */
    int authed;            /* 1=已认证 */
    char buf[ESL_BUF_MAX]; /* 接收缓冲 */
    int  buf_len;
    char out[ESL_BUF_MAX * 2]; /* 发送缓冲（可能多条事件） */
    int  out_len;
    struct esl_conn *next;
} esl_conn_t;

/* ================================================================
 * 全局状态
 * ================================================================ */

static void *g_reactor = NULL;
static int   g_uds_fd = -1;
static int   g_tcp_fd = -1;
static char  g_uds_path[256] = "";
static char  g_tcp_addr[128] = "";
static int   g_max_clients = ESL_MAX_CLIENTS;
static const char *g_password = NULL;
static cmd_reg_t *g_cmd_reg = NULL;
static int   g_own_cmd_reg = 0;  /* 1=内部 self-created，需自己 destroy */

static esl_conn_t *g_conn_head = NULL;
static int         g_client_count = 0;

static uint64_t g_api_calls = 0;
static uint64_t g_events_fired = 0;

/* ================================================================
 * 内部工具
 * ================================================================ */

static void conn_send(esl_conn_t *c, const char *data, int len)
{
    if (!c || !data || len <= 0) return;
    if (c->out_len + len > (int)sizeof(c->out)) {
        return; /* 缓冲满，丢弃这次 push，下次再推 */
    }
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
}

static int conn_flush(esl_conn_t *c)
{
    ssize_t n;
    if (!c || c->out_len <= 0) return 0;
    n = write(c->fd, c->out, c->out_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    if (n < c->out_len) {
        memmove(c->out, c->out + n, c->out_len - n);
        c->out_len -= n;
    } else {
        c->out_len = 0;
    }
    return 0;
}

static void conn_close(esl_conn_t *c)
{
    esl_conn_t **pp = &g_conn_head;
    if (!c) return;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) {
        *pp = c->next;
        close(c->fd);
        g_client_count--;
        free(c);
    }
}

/* ================================================================
 * 事件名到位图映射
 * ================================================================ */

struct evt_map { const char *name; uint32_t bit; };
static const struct evt_map g_evt_map[] = {
    { "CDR",           ESL_EVT_CDR },
    { "CHANNEL_STATE", ESL_EVT_CHANNEL_STATE },
    { "SIP_REGISTERED", ESL_EVT_SIP_REGISTERED },
    { "SIP_UNREGISTERED", ESL_EVT_SIP_UNREGISTERED },
    { "CONFIG_CHANGED", ESL_EVT_CONFIG_CHANGED },
    { "PLAYBACK_START", ESL_EVT_PLAYBACK_START },
    { "PLAYBACK_STOP",  ESL_EVT_PLAYBACK_STOP },
    { "HANGUP",         ESL_EVT_HANGUP },
    { NULL, 0 }
};

uint32_t esl_event_class_from_name(const char *name)
{
    if (!name) return 0;
    for (const struct evt_map *m = g_evt_map; m->name; m++)
        if (strcasecmp(name, m->name) == 0) return m->bit;
    return 0;
}

const char *esl_event_class_to_name(uint32_t bit)
{
    if (!bit) return NULL;
    for (const struct evt_map *m = g_evt_map; m->name; m++)
        if (m->bit == bit) return m->name;
    return NULL;
}

/* ================================================================
 * 命令执行：tokenize → cmd_execute（用项目注册表 API）
 * ================================================================ */

/* io_printf 回调（给 cmd_ctx_t 用）——命令处理函数的输出通过这个写回连接 */
static int conn_io_cb(void *ud, const char *fmt, ...)
{
    esl_conn_t *c = (esl_conn_t *)ud;
    char tmp[ESL_BUF_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) conn_send(c, tmp, n);
    return n;
}

/* 简易 tokenizer：按空格切分，argv 数组指向 line 内部（就地改写空格为 \0）
 * 最多 ESL_MAX_ARGC 个 token。返回 argc。 */
static int tokenize_cmd(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < ESL_MAX_ARGC) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p == ' ') { *p = '\0'; p++; }
    }
    return argc;
}

static int exec_api(esl_conn_t *c, const char *line)
{
    /* 解析 "api <cmd> [args...]" */
    const char *p = line;
    if (strncmp(p, "api ", 4) != 0) return -1;

    /* 拷贝一份就地 tokenize（line 来自接收缓冲，只读） */
    char work[ESL_LINE_MAX];
    strncpy(work, p + 4, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    char *argv[ESL_MAX_ARGC];
    int argc = tokenize_cmd(work, argv);
    if (argc == 0) {
        conn_send(c, "-ERR empty command\n\n", 20);
        return 0;
    }

    if (!g_cmd_reg) {
        conn_send(c, "-ERR no command registry\n\n", 26);
        return 0;
    }

    cmd_ctx_t cx;
    memset(&cx, 0, sizeof(cx));
    cx.uid = c;
    cx.authed_level = c->authed ? CMD_L2_ADMIN : CMD_L1_MONITOR;
    cx.l2_confirmed = c->authed;
    cx.io_printf = conn_io_cb;
    cx.io_ud = c;

    int rc = cmd_execute(g_cmd_reg, &cx, argc, argv);
    g_api_calls++;

    /* cmd_execute 返回值 */
    if (rc == NSBC_OK) {
        conn_send(c, "+OK\n", 4);
    } else if (rc == NSBC_NOENT) {
        char err[128];
        snprintf(err, sizeof(err), "-ERR unknown command: %s\n\n", argv[0]);
        conn_send(c, err, strlen(err));
    } else if (rc == NSBC_INVAL) {
        conn_send(c, "-ERR permission denied (auth first)\n\n", 38);
    } else {
        char err[128];
        snprintf(err, sizeof(err), "-ERR command failed rc=%d\n\n", rc);
        conn_send(c, err, strlen(err));
    }
    return 0;
}

/* ================================================================
 * 事件订阅/推送
 * ================================================================ */

static int parse_event_cmd(esl_conn_t *c, const char *line)
{
    if (strncmp(line, "event plain", 11) == 0) {
        uint32_t bits = 0;
        const char *p = line + 11;
        while (*p) {
            while (*p == ' ') p++;
            char tok[64] = "";
            int i = 0;
            while (*p && *p != ' ' && i < 63) tok[i++] = *p++;
            uint32_t b = esl_event_class_from_name(tok);
            if (b) bits |= b;
        }
        if (bits == 0) {
            conn_send(c, "-ERR unknown event classes\n", 27);
            return 0;
        }
        c->subscribed |= bits;
        conn_send(c, "+OK event subscribed\n\n", 24);
        return 0;
    }
    if (strncmp(line, "noevents", 8) == 0) {
        c->subscribed = 0;
        conn_send(c, "+OK noevents\n\n", 14);
        return 0;
    }
    return -1;
}

static int parse_auth_cmd(esl_conn_t *c, const char *line)
{
    if (strncmp(line, "auth", 4) != 0) return -1;
    if (!g_password || g_password[0] == '\0') {
        c->authed = 1;
        conn_send(c, "+OK accepted (no password configured)\n\n", 39);
        return 0;
    }
    const char *pw = line + 4;
    while (*pw == ' ') pw++;
    if (strcmp(pw, g_password) == 0) {
        c->authed = 1;
        conn_send(c, "+OK accepted\n\n", 14);
    } else {
        conn_send(c, "-ERR invalid password\n\n", 23);
    }
    return 0;
}

/* 事件推送（实现） */
void esl_event_fire(const char *name, const char *json_body, size_t json_len)
{
    if (!name || !json_body) return;
    uint32_t bit = esl_event_class_from_name(name);
    if (!bit) return;
    g_events_fired++;

    esl_conn_t *c = g_conn_head;
    while (c) {
        esl_conn_t *next = c->next;
        if (c->subscribed & bit) {
            char hdr[256];
            int hl = snprintf(hdr, sizeof(hdr),
                "Event-Name: %s\n"
                "Content-Length: %zu\n"
                "\n",
                name, json_len);
            conn_send(c, hdr, hl);
            conn_send(c, json_body, (int)json_len);
            conn_send(c, "\n", 1);
            conn_flush(c);
        }
        c = next;
    }
}

/* ================================================================
 * Phase 4: event_bus 订阅通配 "*" — 把 event_handler_trigger 事件转推 ESL
 *
 * 绑定后，内核所有同步事件（SESSION_CREATE/ANSWER/END, REGISTER_OK/FAIL,
 * AUTH_FAIL, MEDIA_BUG 等）自动通过 esl_event_fire 推给订阅的 ESL 客户端。
 * 与 ctx->esl_event_fire 直接调用的路径（CDR/PLAYBACK）双轨共存，互不干扰。
 * ================================================================ */

static int g_esl_bound_evtbus = 0;

/* event_handler_trigger 回调 — 直接转推，不过滤，非阻塞 */
static int esl_event_bus_cb(const event_context_t *ev, void *ud)
{
    (void)ud;
    if (!ev || !ev->event_name || !ev->event_name[0]) return NSBC_OK;

    const char *payload = ev->payload ? ev->payload : "";
    size_t plen = ev->payload ? strlen(ev->payload) : 0;
    esl_event_fire(ev->event_name, payload, plen);
    return NSBC_OK;   /* PERM_DENIED 由其他 handler 决定，ESL 不干涉 */
}

int esl_bind_event_bus(void)
{
    if (g_esl_bound_evtbus) return NSBC_OK;
    int rc = event_handler_register("*", esl_event_bus_cb, NULL);
    if (rc == NSBC_OK) {
        g_esl_bound_evtbus = 1;
        llog(LOG_NOTICE, "esl", "bound event_handler '*' → all kernel events fire to ESL");
    } else {
        llog(LOG_WARN, "esl", "event_handler_register '*' failed rc=%d (ESL still works via ctx direct fire)", rc);
    }
    return rc;
}

void esl_unbind_event_bus(void)
{
    if (!g_esl_bound_evtbus) return;
    event_handler_unregister("*", esl_event_bus_cb, NULL);
    g_esl_bound_evtbus = 0;
    llog(LOG_NOTICE, "esl", "unbound event_handler '*'");
}

/* ================================================================
 * 命令解析主入口
 * ================================================================ */

static void handle_line(esl_conn_t *c, const char *line)
{
    if (!line || !*line) return;

    if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
        conn_send(c, "Bye!\n", 5);
        return;
    }

    if (parse_auth_cmd(c, line) == 0) return;
    if (parse_event_cmd(c, line) == 0) return;
    if (exec_api(c, line) == 0) return;

    char err[128];
    snprintf(err, sizeof(err), "-ERR unknown command: %s\n\n", line);
    conn_send(c, err, strlen(err));
}

/* ================================================================
 * reactor 回调
 * ================================================================ */

static void on_conn_event(int fd, uint32_t ev, void *arg)
{
    esl_conn_t *c = (esl_conn_t *)arg;
    (void)fd;
    (void)ev;
    if (!c) return;

    conn_flush(c);

    char tmp[ESL_BUF_MAX];
    ssize_t n = recv(c->fd, tmp, sizeof(tmp), 0);
    if (n <= 0) {
        conn_close(c);
        return;
    }

    if (c->buf_len + n > ESL_BUF_MAX) {
        conn_close(c);
        return;
    }
    memcpy(c->buf + c->buf_len, tmp, n);
    c->buf_len += n;

    char *start = c->buf;
    char *end = c->buf + c->buf_len;
    char *nl;
    while ((nl = memchr(start, '\n', end - start)) != NULL) {
        *nl = '\0';
        if (nl > start && nl[-1] == '\r') nl[-1] = '\0';
        handle_line(c, start);
        start = nl + 1;
    }
    if (start > c->buf) {
        int remain = (int)(end - start);
        memmove(c->buf, start, remain);
        c->buf_len = remain;
    } else {
        c->buf_len = 0;
    }
}

static void on_accept_event(int fd, uint32_t ev, void *arg)
{
    (void)ev;
    (void)arg;
    for (;;) {
        int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) return;
        if (g_client_count >= g_max_clients) {
            close(cfd);
            continue;
        }
        esl_conn_t *c = (esl_conn_t *)calloc(1, sizeof(*c));
        if (!c) { close(cfd); return; }
        c->fd = cfd;
        if (reactor_add(g_reactor, cfd, EPOLLIN, on_conn_event, c) != 0) {
            close(cfd); free(c); return;
        }
        c->next = g_conn_head;
        g_conn_head = c;
        g_client_count++;
        const char banner[] = "+OK\nContent-Type: command/reply\n\n";
        conn_send(c, banner, sizeof(banner) - 1);
        llog(LOG_INFO, "esl", "client connected (fd=%d total=%d)", cfd, g_client_count);
    }
}

/* ================================================================
 * 监听初始化
 * ================================================================ */

static int make_listen_unix(const char *path)
{
    /* 自动创建父目录（与 obs_control 一致：/var/run/nsbc 等） */
    char dup[512];
    snprintf(dup, sizeof(dup), "%s", path);
    char *slash = strrchr(dup, '/');
    if (slash) { *slash = '\0'; mkdir_p(dup); }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0600);
    if (listen(fd, 16) != 0) { close(fd); unlink(path); return -1; }
    return fd;
}

static int make_listen_tcp(const char *addr)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    const char *colon = strrchr(addr, ':');
    uint16_t port = colon ? (uint16_t)atoi(colon + 1) : 8021;
    sa.sin_port = htons(port);
    if (colon) {
        /* 用户指定了 IP:port（非 0.0.0.0），用 inet_pton 解析 */
        char ip[INET_ADDRSTRLEN] = "0.0.0.0";
        size_t ip_len = (size_t)(colon - addr);
        if (ip_len >= sizeof(ip)) ip_len = sizeof(ip) - 1;
        memcpy(ip, addr, ip_len);
        ip[ip_len] = '\0';
        if (strcmp(ip, "0.0.0.0") != 0)
            inet_pton(AF_INET, ip, &sa.sin_addr);
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
    if (listen(fd, 16) != 0) { close(fd); return -1; }
    return fd;
}

/* ================================================================
 * 公开生命周期
 * ================================================================ */

nsbc_rc_t esl_start(void *reactor, const esl_cfg_t *cfg, cmd_reg_t *reg)
{
    if (!reactor) return NSBC_INVAL;

    g_reactor = reactor;
    g_cmd_reg = reg;
    g_own_cmd_reg = 0;

    /* 没传注册表——内部创建一个空的（后续可以加 show commands 之类） */
    if (!g_cmd_reg) {
        if (cmd_reg_create(&g_cmd_reg) == NSBC_OK) {
            g_own_cmd_reg = 1;
            llog(LOG_INFO, "esl", "created internal empty cmd_registry");
        } else {
            llog(LOG_WARN, "esl", "cmd_reg_create failed — api commands disabled");
        }
    }

    if (cfg) {
        if (cfg->uds_path) snprintf(g_uds_path, sizeof(g_uds_path), "%s", cfg->uds_path);
        if (cfg->tcp_addr) snprintf(g_tcp_addr, sizeof(g_tcp_addr), "%s", cfg->tcp_addr);
        if (cfg->max_clients > 0) g_max_clients = cfg->max_clients;
        g_password = cfg->password;
    } else {
        snprintf(g_uds_path, sizeof(g_uds_path), "/var/run/nsbc/esl.sock");
        snprintf(g_tcp_addr, sizeof(g_tcp_addr), "0.0.0.0:8021");
    }

    /* UDS */
    if (g_uds_path[0]) {
        g_uds_fd = make_listen_unix(g_uds_path);
        if (g_uds_fd >= 0) {
            reactor_add(g_reactor, g_uds_fd, EPOLLIN, on_accept_event, NULL);
            llog(LOG_NOTICE, "esl", "UDS listener on %s", g_uds_path);
        } else {
            llog(LOG_WARN, "esl", "UDS bind failed: %s", g_uds_path);
        }
    }

    /* TCP */
    if (g_tcp_addr[0]) {
        g_tcp_fd = make_listen_tcp(g_tcp_addr);
        if (g_tcp_fd >= 0) {
            reactor_add(g_reactor, g_tcp_fd, EPOLLIN, on_accept_event, NULL);
            llog(LOG_NOTICE, "esl", "TCP listener on %s", g_tcp_addr);
        } else {
            llog(LOG_WARN, "esl", "TCP bind failed: %s", g_tcp_addr);
        }
    }

    if (g_uds_fd < 0 && g_tcp_fd < 0) return NSBC_ERR;
    return NSBC_OK;
}

void esl_stop(void)
{
    /* 关所有连接 */
    esl_conn_t *c = g_conn_head;
    while (c) {
        esl_conn_t *next = c->next;
        close(c->fd);
        free(c);
        c = next;
    }
    g_conn_head = NULL;
    g_client_count = 0;

    if (g_uds_fd >= 0) {
        reactor_del(g_reactor, g_uds_fd);
        close(g_uds_fd);
        g_uds_fd = -1;
        if (g_uds_path[0]) { unlink(g_uds_path); g_uds_path[0] = '\0'; }
    }
    if (g_tcp_fd >= 0) {
        reactor_del(g_reactor, g_tcp_fd);
        close(g_tcp_fd);
        g_tcp_fd = -1;
        g_tcp_addr[0] = '\0';
    }

    if (g_own_cmd_reg && g_cmd_reg) {
        cmd_reg_destroy(g_cmd_reg);
        g_cmd_reg = NULL;
        g_own_cmd_reg = 0;
    }

    llog(LOG_NOTICE, "esl", "stopped (events_fired=%llu api_calls=%llu)",
         (unsigned long long)g_events_fired, (unsigned long long)g_api_calls);
}

/* ================================================================
 * 查询接口
 * ================================================================ */

int esl_client_count(void) { return g_client_count; }
uint64_t esl_event_total_fired(void) { return g_events_fired; }
uint64_t esl_api_total_calls(void) { return g_api_calls; }

int esl_register_cmd(cmd_reg_t *reg, const cmd_desc_t *desc)
{
    if (!reg || !desc) return NSBC_INVAL;
    return cmd_register(reg, desc);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
