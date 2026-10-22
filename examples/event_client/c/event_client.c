/* NSBC realtime event/CDR subscription client (Unix Domain Socket / C)
 *
 * Pure POSIX, no third-party deps. Builds with GCC 4.8+ / Clang / MSYS2.
 *
 * Build:
 *   gcc -std=gnu99 -O2 -Wall -Wextra -o event_client event_client.c
 *   clang -O2 -Wall -o event_client event_client.c
 *
 * Usage:
 *   ./event_client                              # default /tmp/nsbc-e2e/control.sock
 *   ./event_client /var/run/nsbc/control.sock
 *   ./event_client --topics events,cdr,log
 *   ./event_client --pull-only
 *
 * Frame format (server-side unified):
 *   events: YYYY-MM-DDTHH:MM:SS.mmm CALL_INCOMING  k=v k=v ...\n
 *   events: YYYY-MM-DDTHH:MM:SS.mmm CALL_ANSWERED  k=v k=v ...\n
 *   events: YYYY-MM-DDTHH:MM:SS.mmm CALL_END       k=v k=v ...\n
 *   cdr:    YYYY-MM-DDTHH:MM:SS.mmm CDR { ... JSON ... }\n
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE  (64 * 1024)
#define FRAME_LEN 4096
#define TS_LEN    23    /* "YYYY-MM-DDTHH:MM:SS.mmm" */

static volatile sig_atomic_t g_sigint = 0;
static void on_sigint(int s) { (void)s; g_sigint = 1; }

static const char *BANNER = "NSBC ";
static const char *PROMPT = "nsbc> ";
static const char *RESET  = "\x1b[0m";

static const char *domain_color(const char *d)
{
    if (!d) return "\x1b[37m";
    if (!strcmp(d, "CALL_INCOMING")) return "\x1b[36m";
    if (!strcmp(d, "CALL_ANSWERED")) return "\x1b[32m";
    if (!strcmp(d, "CALL_END"))      return "\x1b[31m";
    if (!strcmp(d, "REGISTER"))      return "\x1b[33m";
    if (!strcmp(d, "CDR"))           return "\x1b[35m";
    return "\x1b[37m";
}

/* Parse frame: "YYYY-MM-DDTHH:MM:SS.mmm DOMAIN body"
 * Returns 1=push frame; 0=non-push frame */
static int parse_frame(const char *line, char *ts_out, char *domain_out, char *body_out, size_t body_cap)
{
    if (!line || strlen(line) < TS_LEN + 2) return 0;

    /* Check timestamp prefix (lenient: only require 'T' and '.' present) */
    const char *p = line;
    int valid_ts = 1;
    for (int i = 0; i < TS_LEN; ++i, ++p) {
        if (*p == '\0') { valid_ts = 0; break; }
    }
    if (!valid_ts) return 0;
    if (*p != ' ') return 0; ++p;

    /* Copy timestamp */
    memcpy(ts_out, line, TS_LEN);
    ts_out[TS_LEN] = '\0';

    /* Split DOMAIN (until next space or \n) */
    const char *dstart = p;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r') ++p;
    size_t dlen = (size_t)(p - dstart);
    if (dlen == 0 || dlen > 63) return 0;
    memcpy(domain_out, dstart, dlen);
    domain_out[dlen] = '\0';

    /* Skip space */
    if (*p == ' ') ++p;

    /* body */
    size_t bi = 0;
    while (*p && *p != '\n' && *p != '\r' && bi + 1 < body_cap) {
        body_out[bi++] = *p++;
    }
    body_out[bi] = '\0';

    return 1;
}

static void print_frame(const char *line)
{
    char ts[TS_LEN + 1], domain[64], body[FRAME_LEN];
    if (!parse_frame(line, ts, domain, body, sizeof(body))) {
        /* Non-push frame — banner/OK/ERR/prompt */
        if (strncmp(line, BANNER, strlen(BANNER)) == 0 ||
            strncmp(line, "OK ", 3) == 0 ||
            strncmp(line, "ERR ", 4) == 0 ||
            strncmp(line, "subscribed:", 11) == 0) {
            fprintf(stdout, "\x1b[2m[cmd] %s%s\n", line, RESET);
        }
        return;
    }

    /* Shorten timestamp: 15:04:05.000 */
    const char *ts_short = (strlen(ts) >= 12) ? ts + 11 : ts;

    const char *c = domain_color(domain);
    if (!strcmp(domain, "CDR")) {
        fprintf(stdout, "%s %s%12s%s %s\n", ts_short, c, "CDR", RESET, body);
    } else {
        fprintf(stdout, "%s %s%15s%s %s\n", ts_short, c, domain, RESET, body);
    }
    fflush(stdout);
}

/* Receive until PROMPT seen or timeout. Uses SO_RCVTIMEO. */
static int recv_until_prompt(int fd, int timeout_ms, char *out, size_t out_cap)
{
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t used = 0;
    char buf[BUF_SIZE];
    while (used + 1 < out_cap) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;  /* EOF / timeout / error */
        if ((size_t)n >= out_cap - used) n = (ssize_t)(out_cap - used - 1);
        memcpy(out + used, buf, (size_t)n);
        used += (size_t)n;
        out[used] = '\0';
        if (strstr(out, PROMPT)) break;
    }
    /* Restore infinite wait */
    tv.tv_sec = 0; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return (int)used;
}

static int uds_connect(const char *path)
{
    if (access(path, F_OK) != 0) {
        fprintf(stderr, "ERR: UDS socket not found: %s\n", path);
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "ERR connect %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    const char *uds_path  = "/tmp/nsbc-e2e/control.sock";
    const char *topics    = "events,cdr";
    int         pull_only = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--topics") && i + 1 < argc) {
            topics = argv[++i];
        } else if (!strcmp(argv[i], "--pull-only")) {
            pull_only = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fprintf(stderr,
                "Usage: event_client [uds_path] [--topics events,cdr] [--pull-only]\n");
            return 0;
        } else if (argv[i][0] != '-' && argv[i][0] != '\0') {
            uds_path = argv[i];
        }
    }

    signal(SIGINT, on_sigint);

    int fd = uds_connect(uds_path);
    if (fd < 0) return 2;

    printf("\x1b[2m[connected to %s]%s\n", uds_path, RESET);
    fflush(stdout);

    /* Receive banner + prompt (short 2s timeout) */
    char tmp[4096];
    recv_until_prompt(fd, 2000, tmp, sizeof(tmp));
    /* Print banner line */
    char *saveptr = NULL;
    for (char *ln = strtok_r(tmp, "\n", &saveptr); ln; ln = strtok_r(NULL, "\n", &saveptr)) {
        if (!strncmp(ln, BANNER, strlen(BANNER))) {
            printf("\x1b[2m%s%s\n", ln, RESET);
        }
    }

    if (!pull_only) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "sub %s\n", topics);
        if (send(fd, cmd, strlen(cmd), 0) < 0) {
            perror("send sub"); close(fd); return 1;
        }

        recv_until_prompt(fd, 2000, tmp, sizeof(tmp));
        saveptr = NULL;
        for (char *ln = strtok_r(tmp, "\n", &saveptr); ln; ln = strtok_r(NULL, "\n", &saveptr)) {
            /* Strip possible trailing nsbc> */
            size_t llen = strlen(ln);
            if (llen > strlen(PROMPT) && !strcmp(ln + llen - strlen(PROMPT), PROMPT)) {
                ln[llen - strlen(PROMPT)] = '\0';
            }
            if (ln[0] && strcmp(ln, PROMPT) != 0) {
                printf("\x1b[2m[sub] %s%s\n", ln, RESET);
            }
        }
        printf("\x1b[2m[subscribed: %s — Ctrl-C to quit]%s\n", topics, RESET);
        fflush(stdout);
    }

    /* Main loop: blocking read + cumulative line buffer + parse on \n */
    char line_buf[FRAME_LEN];
    size_t line_used = 0;
    char net_buf[BUF_SIZE];

    while (!g_sigint) {
        ssize_t n = recv(fd, net_buf, sizeof(net_buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "\n\x1b[31m[recv error: %s]%s\n", strerror(errno), RESET);
            break;
        }
        if (n == 0) {
            fprintf(stderr, "\n\x1b[2m[server closed]%s\n", RESET);
            break;
        }

        for (ssize_t i = 0; i < n && !g_sigint; ++i) {
            char c = net_buf[i];
            if (c == '\n') {
                line_buf[line_used] = '\0';
                print_frame(line_buf);
                line_used = 0;
            } else if (line_used + 1 < sizeof(line_buf)) {
                line_buf[line_used++] = c;
            }
        }
    }

    if (g_sigint) {
        printf("\n\x1b[2m[quit]%s\n", RESET);
    }
    close(fd);
    return 0;
}
