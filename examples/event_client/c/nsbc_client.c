/* nsbc_client.c — implementation of the NSBC unified C client library.
 *
 * POSIX only. No TLS, no HTTP, no dynamic memory management beyond a few
 * malloc'd call_url strings returned to callers.
 *
 * See nsbc_client.h for full API.
 */

#define _GNU_SOURCE

#include "nsbc_client.h"

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

/* -------------------------------------------------------------------- */
/* Opaque connection                                                     */
/* -------------------------------------------------------------------- */

struct nsbc_conn {
    char  uds_path[256];
    int   fd;
    char  banner[512];
};

/* -------------------------------------------------------------------- */
/* Iter stop flag (set from SIGINT handler)                              */
/* -------------------------------------------------------------------- */

static volatile sig_atomic_t g_stop = 0;

void nsbc_iter_stop(void) { g_stop = 1; }

static void on_sigint(int s) { (void)s; g_stop = 1; }

/* -------------------------------------------------------------------- */
/* Connection                                                            */
/* -------------------------------------------------------------------- */

nsbc_conn_t *nsbc_connect(const char *uds_path)
{
    if (!uds_path || access(uds_path, F_OK) != 0) return NULL;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, uds_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }

    nsbc_conn_t *c = calloc(1, sizeof(*c));
    if (!c) { close(fd); return NULL; }
    strncpy(c->uds_path, uds_path, sizeof(c->uds_path) - 1);
    c->fd = fd;

    /* Probe banner (2s deadline) */
    char buf[2048];
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    size_t got = 0;
    while (got + 1 < sizeof(buf)) {
        ssize_t n = recv(fd, buf + got, sizeof(buf) - got - 1, 0);
        if (n <= 0) break;
        got += (size_t)n;
        buf[got] = '\0';
        if (strstr(buf, NSBC_PROMPT)) break;
    }
    tv.tv_sec = 0; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Extract first NSBC banner line */
    buf[got] = '\0';
    const char *b = strstr(buf, "NSBC ");
    if (b) {
        const char *end = b;
        while (*end && *end != '\n' && *end != '\r') ++end;
        size_t blen = (size_t)(end - b);
        if (blen >= sizeof(c->banner)) blen = sizeof(c->banner) - 1;
        memcpy(c->banner, b, blen);
        c->banner[blen] = '\0';
    }

    return c;
}

void nsbc_close(nsbc_conn_t *c)
{
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c);
}

int nsbc_banner(const nsbc_conn_t *c, char *out, size_t out_cap)
{
    if (!c || !out || out_cap == 0) return 0;
    snprintf(out, out_cap, "%s", c->banner);
    return (int)strlen(out);
}

/* Receive until NSBC_PROMPT or timeout. Returns bytes read.
 * Strips trailing prompt from the in-place buffer. */
static int recv_until_prompt(int fd, int timeout_ms, char *out, size_t out_cap)
{
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t used = 0;
    char buf[65536];
    while (used + 1 < out_cap) {
        if (g_stop) break;
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if ((size_t)n >= out_cap - used) n = (ssize_t)(out_cap - used - 1);
        memcpy(out + used, buf, (size_t)n);
        used += (size_t)n;
        out[used] = '\0';
        if (strstr(out, NSBC_PROMPT)) break;
    }
    tv.tv_sec = 0; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Strip trailing prompt if present */
    size_t plen = strlen(NSBC_PROMPT);
    if (used >= plen && !strcmp(out + used - plen, NSBC_PROMPT)) {
        out[used - plen] = '\0';
        used -= plen;
    }
    /* Also strip mid-buffer prompt (e.g. from an earlier chunk that
     * contained "nsbc>" before we broke out of the loop) */
    char *p = strstr(out, NSBC_PROMPT);
    if (p) *p = '\0';

    return (int)used;
}

nsbc_rc_t nsbc_send_command(nsbc_conn_t *c, const char *cmd,
                            int timeout_ms, char *out, size_t out_cap)
{
    if (!c || c->fd < 0 || !cmd || !out) return NSBC_ERR_INVALID_ARG;

    size_t clen = strlen(cmd);
    char *send_buf = malloc(clen + 2);
    if (!send_buf) return NSBC_ERR_INVALID_ARG;
    memcpy(send_buf, cmd, clen);
    if (send_buf[clen - 1] != '\n') send_buf[clen++] = '\n';
    send_buf[clen] = '\0';

    ssize_t nw = send(c->fd, send_buf, clen, 0);
    free(send_buf);
    if (nw < 0) return NSBC_ERR_SEND;

    out[0] = '\0';
    int got = recv_until_prompt(c->fd, timeout_ms, out, out_cap);
    if (got < 0) return NSBC_ERR_RECV;

    out[got] = '\0';
    return NSBC_OK;
}

int nsbc_subscribe(nsbc_conn_t *c, const char *topics, char *resp_out, size_t resp_cap)
{
    if (!topics) topics = NSBC_DEFAULT_TOPICS;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "sub %s", topics);
    return (nsbc_send_command(c, cmd, 2000, resp_out, resp_cap) == NSBC_OK) ? 1 : 0;
}

int nsbc_unsubscribe(nsbc_conn_t *c, const char *topics, char *resp_out, size_t resp_cap)
{
    char cmd[256];
    if (topics) snprintf(cmd, sizeof(cmd), "unsub %s", topics);
    else        snprintf(cmd, sizeof(cmd), "unsub");
    return (nsbc_send_command(c, cmd, 2000, resp_out, resp_cap) == NSBC_OK) ? 1 : 0;
}

/* -------------------------------------------------------------------- */
/* Frame parsing                                                         */
/* -------------------------------------------------------------------- */

static void classify(const char *domain, char *kind_out, size_t kind_cap)
{
    if (strncmp(domain, "CALL_", 5) == 0 || !strcmp(domain, "CALL_OUTGOING")) {
        snprintf(kind_out, kind_cap, "call");
    } else if (!strcmp(domain, "CDR")) {
        snprintf(kind_out, kind_cap, "cdr");
    } else if (!strcmp(domain, "REGISTER")) {
        snprintf(kind_out, kind_cap, "register");
    } else if (!strcmp(domain, "OK") || !strcmp(domain, "ERR") || strncmp(domain, "subscribed:", 11) == 0) {
        snprintf(kind_out, kind_cap, "cmd");
    } else {
        snprintf(kind_out, kind_cap, "other");
    }
}

int nsbc_parse_frame(const char *line, nsbc_frame_t *frame)
{
    if (!line || !frame) return 0;
    memset(frame, 0, sizeof(*frame));

    size_t ln = strlen(line);
    if (ln < 25) return 0; /* "YYYY-MM-DDTHH:MM:SS.mmm X ..." */

    /* Timestamp: 23 chars */
    memcpy(frame->ts, line, 23);
    frame->ts[23] = '\0';

    if (line[23] != ' ') return 0;
    const char *p = line + 24;

    /* DOMAIN until space/EOL */
    const char *dstart = p;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r') ++p;
    size_t dlen = (size_t)(p - dstart);
    if (dlen == 0 || dlen >= sizeof(frame->domain)) return 0;
    memcpy(frame->domain, dstart, dlen);
    frame->domain[dlen] = '\0';

    while (*p == ' ') ++p;

    /* body */
    size_t bi = 0;
    while (*p && *p != '\n' && *p != '\r' && bi + 1 < sizeof(frame->body)) {
        frame->body[bi++] = *p++;
    }
    frame->body[bi] = '\0';

    /* raw (with original line content) */
    size_t rl = ln;
    if (rl >= sizeof(frame->raw)) rl = sizeof(frame->raw) - 1;
    memcpy(frame->raw, line, rl);
    frame->raw[rl] = '\0';

    classify(frame->domain, frame->kind, sizeof(frame->kind));

    return 1;
}

nsbc_rc_t nsbc_iter_frames(nsbc_conn_t *c, nsbc_frame_cb_t cb, void *userdata)
{
    if (!c || c->fd < 0) return NSBC_ERR_INVALID_ARG;
    signal(SIGINT, on_sigint);

    char line_buf[NSBC_BODY_LEN];
    size_t line_used = 0;
    char net_buf[65536];

    while (!g_stop) {
        ssize_t n = recv(c->fd, net_buf, sizeof(net_buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return NSBC_ERR_RECV;
        }
        if (n == 0) return NSBC_OK; /* clean close */

        for (ssize_t i = 0; i < n && !g_stop; ++i) {
            char ch = net_buf[i];
            if (ch == '\n') {
                line_buf[line_used] = '\0';
                nsbc_frame_t f;
                if (nsbc_parse_frame(line_buf, &f) && cb) cb(&f, userdata);
                line_used = 0;
            } else if (line_used + 1 < sizeof(line_buf)) {
                line_buf[line_used++] = ch;
            }
        }
    }
    return NSBC_OK;
}

/* -------------------------------------------------------------------- */
/* Originate                                                             */
/* -------------------------------------------------------------------- */

char *nsbc_build_call_url(const nsbc_orig_opts_t *opts)
{
    if (!opts) return NULL;
    if (opts->call_url && *opts->call_url) {
        /* Caller gave explicit call_url — just strdup */
        return strdup(opts->call_url);
    }

    const char *ext = opts->orig_to && *opts->orig_to ? opts->orig_to : "1000";
    const char *app = opts->app && *opts->app ? opts->app : "echo";

    /* Build brace var block */
    char vars[NSBC_CMD_LEN];
    size_t vi = 0;

    /* helper: append "key=value," */
    #define APPENDV(k, v) do {                                              \
        if ((v) && *(v)) {                                                  \
            vi += (size_t)snprintf(vars + vi, sizeof(vars) - vi,            \
                                   "%s=%s,", (k), (v));                     \
        }                                                                   \
    } while (0)

    APPENDV("origination_caller_id_number", opts->caller_id);
    APPENDV("origination_caller_id_name",    opts->caller_name);
    if (opts->timeout > 0) {
        vi += (size_t)snprintf(vars + vi, sizeof(vars) - vi,
                                "originate_timeout=%d,", opts->timeout);
    }
    APPENDV("audio_codecs", opts->audio_codecs);
    APPENDV("video_codecs", opts->video_codecs);

    #undef APPENDV

    /* Strip trailing comma */
    if (vi > 0 && vars[vi - 1] == ',') vars[--vi] = '\0';

    /* Assemble: {vars}user/<ext> &<app> */
    char *out = malloc(NSBC_CMD_LEN);
    if (!out) return NULL;

    if (vi > 0) {
        snprintf(out, NSBC_CMD_LEN, "{%s}user/%s &%s", vars, ext, app);
    } else {
        snprintf(out, NSBC_CMD_LEN, "user/%s &%s", ext, app);
    }
    return out;
}

int nsbc_originate(nsbc_conn_t *c, const char *call_url,
                   char *response_out, size_t resp_cap)
{
    if (!c || !call_url) return 0;

    char cmd[NSBC_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "originate %s", call_url);

    nsbc_rc_t rc = nsbc_send_command(c, cmd, 10000, response_out, resp_cap);
    if (rc != NSBC_OK) return 0;

    const char *r = response_out;
    /* Remove trailing prompt if any */
    size_t plen = strlen(NSBC_PROMPT);
    size_t rlen = strlen(r);
    if (rlen >= plen && !strcmp(r + rlen - plen, NSBC_PROMPT)) {
        r[rlen - plen] = '\0';
    }
    /* Trim trailing > */
    size_t last = strlen(r);
    while (last > 0 && (r[last - 1] == '>' || r[last - 1] == ' ')) {
        r[--last] = '\0';
    }

    if (strncmp(r, "OK ", 3) == 0 || strstr(r, "\nOK ") || !strcmp(r, "OK")) return 1;
    if (strncmp(r, "ERR", 3) == 0 || strstr(r, "ERR ")) return 0;
    return 1; /* no OK/ERR but also no timeout → treat as dispatched (async) */
}
