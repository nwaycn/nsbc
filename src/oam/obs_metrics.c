/* Prometheus C-Exporter（REQ-OBS-003，P0）
 *
 * 极简 HTTP/1.0：reactor 线程 accept + 单次请求应答后关连接；
 * 只支持 GET /metrics（与 /health），其余 404。
 * 鉴权/TLS/限流为 P1（REQ-OBS-008）；绑定地址默认 127.0.0.1，
 * oam.metrics_bind 是唯一"先开后关"热加载例外（CGF_PROM_MIGRATE）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "app_ctx.h"
#include "cfg.h"
#include "log.h"
#include "metric_reg.h"
#include "net_sock.h"
#include "oam_internal.h"
#include "reactor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define METRICS_BODY_MAX (256 * 1024)
#define HTTP_REQ_MAX 2048

typedef struct {
    int fd;
    char buf[HTTP_REQ_MAX];
    size_t n;
} http_conn_t;

static int g_lfd = -1;
static nsbc_ctx_t *g_ctx;

/* ------------------------------------------------------------------ */
/* 指标 render（拉取时才执行，REQ-OBS-002 采集开销 <1%）                */
/* ------------------------------------------------------------------ */
static int r_up(char *b, size_t cap, void *ud)
{
    (void)ud;
    return snprintf(b, cap, "nsbc_up %d\n", g_ctx ? 1 : 0);
}

/* 业务指标源（calls/reg/txn/rtp_ports）自 A3 起由各属主模块 create 时
 * 经 metric_reg.h 自注册（REQ-OBS-001 代码内埋点）；exporter 只登记
 * 进程活性并负责 HTTP 拉取。 */
static void register_sources(void)
{
    metric_source_register("nsbc_up", "1 if the NSBC core process is up",
                           r_up, NULL);
}

/* ------------------------------------------------------------------ */
/* HTTP 连接处理                                                        */
/* ------------------------------------------------------------------ */
static void conn_close(http_conn_t *c)
{
    if (c->fd >= 0) {
        reactor_del(g_ctx->re, c->fd);
        close(c->fd);
    }
    free(c);
}

static void send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

static const char *http_reason(int status)
{
    if (status == 200)
        return "OK";
    if (status == 500)
        return "Internal Server Error";
    return "Not Found"; /* 404 及其他 */
}

static void respond(http_conn_t *c, int status, const char *ctype,
                    const char *body, size_t blen)
{
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n\r\n",
                      status, http_reason(status), ctype, blen);
    send_all(c->fd, hdr, (size_t)hn);
    send_all(c->fd, body, blen);
}

static void on_conn_event(int fd, uint32_t ev, void *arg)
{
    (void)fd;
    http_conn_t *c = (http_conn_t *)arg;
    if (ev & (EPOLLHUP | EPOLLERR)) {
        conn_close(c);
        return;
    }
    for (;;) {
        ssize_t n = recv(c->fd, c->buf + c->n, sizeof(c->buf) - c->n, 0);
        if (n > 0) {
            c->n += (size_t)n;
            if (c->n >= sizeof(c->buf)) {
                respond(c, 404, "text/plain", "request too large\n", 18);
                conn_close(c);
                return;
            }
            if (!memmem(c->buf, c->n, "\r\n\r\n", 4))
                continue;
        } else if (n < 0)
            return; /* EAGAIN：等更多数据（P0 浏览器一次性发来） */
        /* 请求行：GET <path> HTTP/1.x */
        int is_metrics = (c->n > 18 &&
                          strncmp(c->buf, "GET /metrics", 12) == 0 &&
                          (c->buf[12] == ' ' || c->buf[12] == '?'));
        int is_health = (c->n > 15 &&
                         strncmp(c->buf, "GET /health", 11) == 0 &&
                         c->buf[11] == ' ');
        if (is_health) {
            respond(c, 200, "text/plain", "OK\n", 3);
        } else if (is_metrics) {
            char *body = malloc(METRICS_BODY_MAX);
            if (!body) {
                respond(c, 500, "text/plain", "oom\n", 4);
            } else {
                int bl = metric_render_all(body, METRICS_BODY_MAX);
                respond(c, 200,
                        "text/plain; version=0.0.4; charset=utf-8", body,
                        (size_t)bl);
                free(body);
            }
        } else {
            respond(c, 404, "text/plain", "not found\n", 10);
        }
        conn_close(c);
        return;
    }
}

static void on_accept_event(int fd, uint32_t ev, void *arg)
{
    (void)ev;
    (void)arg;
    for (;;) {
        nsbc_sa_t peer;
        int cfd = net_tcp_accept(fd, &peer);
        if (cfd < 0)
            return;
        http_conn_t *c = calloc(1, sizeof(*c));
        if (!c) {
            close(cfd);
            return;
        }
        c->fd = cfd;
        if (reactor_add(g_ctx->re, cfd, EPOLLIN, on_conn_event, c) !=
            NSBC_OK) {
            close(cfd);
            free(c);
            return;
        }
    }
}

int obs_metrics_start(nsbc_ctx_t *ctx)
{
    g_ctx = ctx;
    register_sources();

    int enabled = 1;
    cfg_get_bool(ctx->cfg, "oam.metrics_enabled", &enabled, NULL);
    if (!enabled)
        return NSBC_OK;
    const char *bind = NULL;
    cfg_get_str(ctx->cfg, "oam.metrics_bind", &bind, NULL);
    if (!bind)
        return NSBC_OK;
    char host[128];
    snprintf(host, sizeof(host), "%s", bind);
    char *colon = strrchr(host, ':');
    if (!colon)
        return NSBC_INVAL;
    *colon = '\0';
    long port = atol(colon + 1);
    g_lfd = net_tcp_listen(host[0] ? host : NULL, (uint16_t)port, 64, 0);
    if (g_lfd < 0) {
        llog(LOG_WARN, "oam", "metrics bind %s failed", bind);
        return NSBC_ERR;
    }
    if (reactor_add(ctx->re, g_lfd, EPOLLIN, on_accept_event, NULL) !=
        NSBC_OK) {
        close(g_lfd);
        g_lfd = -1;
        return NSBC_ERR;
    }
    llog(LOG_NOTICE, "oam", "prometheus exporter on http://%s/metrics",
         bind);
    return NSBC_OK;
}

void obs_metrics_stop(nsbc_ctx_t *ctx)
{
    if (g_lfd >= 0) {
        reactor_del(ctx->re, g_lfd);
        close(g_lfd);
        g_lfd = -1;
    }
}
