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
    snprintf(frame->source, sizeof(frame->source), "uds");

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

/* ===================================================================== */
/* CallCorrelator — groups frames by call_id → timeline on CDR            */
/* Pure data-structure, zero deps, works in BOTH build paths.            */
/* ===================================================================== */

#define NSBC_CC_MAX_ACTIVE 256  /* upper bound on concurrent active calls */

struct nsbc_call_correlator {
    nsbc_correlator_cb_t on_complete;
    void *userdata;
    struct nsbc_cc_entry {
        char call_id[128];
        nsbc_tl_event_t *events;
        char cdr_json[4096];
        int  cdr_present;
        struct nsbc_cc_entry *next;
    } *entries; /* singly-linked list (fast enough; active << 256) */
    int active_count;
};

nsbc_call_correlator_t *nsbc_correlator_new(nsbc_correlator_cb_t on_complete,
                                            void *userdata)
{
    nsbc_call_correlator_t *cc = calloc(1, sizeof(*cc));
    if (!cc) return NULL;
    cc->on_complete = on_complete;
    cc->userdata = userdata;
    return cc;
}

void nsbc_correlator_free(nsbc_call_correlator_t *cc)
{
    if (!cc) return;
    struct nsbc_cc_entry *e = cc->entries;
    while (e) {
        struct nsbc_cc_entry *next = e->next;
        nsbc_tl_event_t *ev = e->events;
        while (ev) {
            nsbc_tl_event_t *enext = ev->next;
            free(ev);
            ev = enext;
        }
        free(e);
        e = next;
    }
    free(cc);
}

/* Extract call_id from frame body. Scan for "call_id":"..." pattern.
 * Caller-provided buf must be >= 128 bytes. Returns 1 if found. */
static int extract_call_id(const nsbc_frame_t *frame, char *out, size_t out_cap)
{
    out[0] = '\0';
    const char *search = frame->body[0] ? frame->body : frame->raw;
    if (!search || !*search) return 0;

    const char *key = "\"call_id\"";
    const char *p = strstr(search, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) ++p;
    if (*p == '"') {
        ++p;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < out_cap) {
            if (*p == '\\' && *(p + 1)) {
                char c = *(p + 1);
                switch (c) {
                    case 'n': out[i++] = '\n'; break;
                    case 'r': out[i++] = '\r'; break;
                    case 't': out[i++] = '\t'; break;
                    default: out[i++] = c;
                }
                p += 2;
            } else {
                out[i++] = *p++;
            }
        }
        out[i] = '\0';
    } else {
        /* unquoted value */
        size_t i = 0;
        while (*p && *p != ',' && *p != '}' && *p != ' ' && i + 1 < out_cap) {
            out[i++] = *p++;
        }
        out[i] = '\0';
    }
    return out[0] != '\0';
}

void nsbc_correlator_feed(nsbc_call_correlator_t *cc, const nsbc_frame_t *frame)
{
    if (!cc || !frame) return;

    char call_id[128];
    if (!extract_call_id(frame, call_id, sizeof(call_id))) return;

    /* Find existing entry or create new (drop oldest if over cap) */
    struct nsbc_cc_entry *e = cc->entries;
    while (e) {
        if (!strcmp(e->call_id, call_id)) break;
        e = e->next;
    }
    if (!e) {
        if (cc->active_count >= NSBC_CC_MAX_ACTIVE) {
            /* Drop oldest (remove head) — keeps memory bounded */
            struct nsbc_cc_entry *old = cc->entries;
            cc->entries = old->next;
            nsbc_tl_event_t *ev = old->events;
            while (ev) { nsbc_tl_event_t *n = ev->next; free(ev); ev = n; }
            free(old);
            cc->active_count--;
        }
        e = calloc(1, sizeof(*e));
        if (!e) return;
        snprintf(e->call_id, sizeof(e->call_id), "%s", call_id);
        e->next = cc->entries;
        cc->entries = e;
        cc->active_count++;
    }

    /* Append event to timeline */
    nsbc_tl_event_t *ev = calloc(1, sizeof(*ev));
    if (!ev) return;
    snprintf(ev->domain, sizeof(ev->domain), "%s", frame->domain ? frame->domain : "?");
    snprintf(ev->ts, sizeof(ev->ts), "%s", frame->ts ? frame->ts : "");
    ev->next = e->events;
    e->events = ev;

    /* CDR → complete! fire callback + cleanup */
    if (!strcmp(frame->domain, "CDR")) {
        if (frame->body[0]) snprintf(e->cdr_json, sizeof(e->cdr_json), "%s", frame->body);
        e->cdr_present = 1;

        /* Build timeline for callback */
        nsbc_call_timeline_t tl;
        memset(&tl, 0, sizeof(tl));
        snprintf(tl.call_id, sizeof(tl.call_id), "%s", e->call_id);
        tl.events = e->events;
        snprintf(tl.cdr_json, sizeof(tl.cdr_json), "%s", e->cdr_json);
        tl.cdr_present = e->cdr_present;

        if (cc->on_complete) cc->on_complete(&tl, cc->userdata);

        /* Remove from active list + free events (head was at most-recent;
         * timeline.events is the same linked list we just passed in) */
        struct nsbc_cc_entry **pp = &cc->entries;
        while (*pp && *pp != e) pp = &(*pp)->next;
        if (*pp) *pp = e->next;
        cc->active_count--;

        /* Walk events from oldest-to-newest (reverse the list we built head-first)
         * — free each after reversing. But timeline.events is already the list
         * pointer; after callback returns we own it again. */
        nsbc_tl_event_t *cur = e->events;
        while (cur) {
            nsbc_tl_event_t *n = cur->next;
            free(cur);
            cur = n;
        }
        free(e);
    }
}

int nsbc_correlator_active(const nsbc_call_correlator_t *cc)
{
    return cc ? cc->active_count : 0;
}

/* ===================================================================== */
/* HTTP + SSE (canonical external integration surface — requires curl)   */
/* ===================================================================== */

#ifdef NSBC_WITH_CURL

#include <curl/curl.h>
#include "cJSON.h"

/* Global TLS verify toggle */
static int g_ssl_verify = 1;

void nsbc_set_ssl_verify(int verify) { g_ssl_verify = verify; }

/* -------------------------------------------------------------------- */
/* libcurl helpers — write callbacks for HTTP originate + SSE streaming  */
/* -------------------------------------------------------------------- */

struct buf { char *data; size_t len; };

static size_t buf_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t n = size * nmemb;
    struct buf *b = (struct buf *)userdata;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* Set common curl opts (URL, TLS verify, timeouts). Caller sets method/body. */
static CURL *curl_common(const char *url, int timeout_s)
{
    CURL *h = curl_easy_init();
    if (!h) return NULL;
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, (long)timeout_s);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, (long)(timeout_s > 10 ? 10 : timeout_s));
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, buf_write_cb);
    if (!g_ssl_verify) {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    return h;
}

/* -------------------------------------------------------------------- */
/* HTTP originate                                                        */
/* -------------------------------------------------------------------- */

int nsbc_originate_via_web(const char *api_base, const char *call_url,
                            int timeout_s, const char *api_token,
                            char **payload_out)
{
    if (!api_base || !call_url) return 0;
    if (payload_out) *payload_out = NULL;

    /* Build URL: api_base + /ext/v1/originate */
    size_t alen = strlen(api_base);
    while (alen > 0 && api_base[alen - 1] == '/') --alen;
    char url[512];
    snprintf(url, sizeof(url), "%.*s/ext/v1/originate", (int)alen, api_base);

    /* Build JSON body: {"call_url": "..."} */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "call_url", call_url);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return 0;

    CURL *h = curl_common(url, timeout_s);
    if (!h) { free(body); return 0; }

    curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    if (api_token && *api_token) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_token);
        hdrs = curl_slist_append(hdrs, auth);
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "POST");

    struct buf resp = { NULL, 0 };
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(h);
    long http_code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(h);
    curl_slist_free_all(hdrs);
    free(body);

    if (rc != CURLE_OK) {
        free(resp.data);
        return 0;
    }
    if (http_code >= 400) {
        free(resp.data);
        return 0;
    }

    if (payload_out) *payload_out = resp.data; else free(resp.data);
    return 1;
}

/* -------------------------------------------------------------------- */
/* Dual-path originate                                                   */
/* -------------------------------------------------------------------- */

int nsbc_originate_dual(const char *call_url, const char *via,
                        const char *uds_path, const char *api_base,
                        const char *api_token, char **via_out)
{
    if (!call_url) return 0;
    if (via_out) *via_out = NULL;

    int ok = 0;
    const char *used_via = "web";

    if (!via || !strcmp(via, "auto") || !strcmp(via, "uds")) {
        /* Try UDS first */
        if (uds_path && !access(uds_path, F_OK)) {
            nsbc_conn_t *c = nsbc_connect(uds_path);
            if (c) {
                char resp[NSBC_RESP_LEN];
                ok = nsbc_originate(c, call_url, resp, sizeof(resp));
                nsbc_close(c);
                used_via = "uds";
            }
        }
        /* UDS failed → HTTP fallback (unless via was explicitly "uds") */
        if (!ok && (!via || !strcmp(via, "auto"))) {
            char *pl = NULL;
            ok = nsbc_originate_via_web(api_base, call_url, 10, api_token, &pl);
            free(pl);
            used_via = "web";
        }
    } else if (!strcmp(via, "web")) {
        char *pl = NULL;
        ok = nsbc_originate_via_web(api_base, call_url, 10, api_token, &pl);
        free(pl);
        used_via = "web";
    }

    if (via_out) *via_out = strdup(used_via);
    return ok;
}

/* -------------------------------------------------------------------- */
/* SSE stream consumer (auto-reconnect, callback per frame)             */
/* -------------------------------------------------------------------- */

/* Accumulated SSE parse state across multiple curl write callback invocations */
struct sse_state {
    char *buf;                /* growing line buffer */
    size_t len;
    char event_name[64];      /* "message" is default */
    char *data_lines[32];     /* up to 32 data lines */
    int   data_line_count;
    int   retry_ms;
    int   retry_set;
    nsbc_frame_cb_t cb;
    void *userdata;
};

static void sse_state_init(struct sse_state *s)
{
    memset(s, 0, sizeof(*s));
    s->event_name[0] = '\0';
}

static void sse_state_free(struct sse_state *s)
{
    free(s->buf);
    for (int i = 0; i < s->data_line_count; i++) free(s->data_lines[i]);
}

static void sse_state_reset_event(struct sse_state *s)
{
    s->event_name[0] = '\0';
    for (int i = 0; i < s->data_line_count; i++) free(s->data_lines[i]);
    s->data_line_count = 0;
    s->retry_set = 0;
}

/* Parse one complete SSE event and call cb. data_lines joined by "\n". */
static void sse_dispatch_event(struct sse_state *s)
{
    char ev_name[64] = "message";
    if (s->event_name[0]) snprintf(ev_name, sizeof(ev_name), "%s", s->event_name);

    if (s->cb && (s->data_line_count > 0 || strcmp(ev_name, "message") != 0)) {
        /* Join data lines */
        size_t total = 0;
        for (int i = 0; i < s->data_line_count; i++) total += strlen(s->data_lines[i]) + 1;
        char *joined = malloc(total + 1);
        if (joined) {
            joined[0] = '\0';
            for (int i = 0; i < s->data_line_count; i++) {
                if (i > 0) strcat(joined, "\n");
                strcat(joined, s->data_lines[i]);
            }
            nsbc_frame_t f;
            if (nsbc_sse_event_to_frame(joined, &f) && s->cb) s->cb(&f, s->userdata);
            free(joined);
        }
    }
    sse_state_reset_event(s);
}

/* Write callback for SSE stream — line-buffers, dispatches on empty line */
static size_t sse_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t n = size * nmemb;
    struct sse_state *s = (struct sse_state *)userdata;

    /* Append to line buffer */
    char *p = realloc(s->buf, s->len + n + 1);
    if (!p) return 0;
    s->buf = p;
    memcpy(s->buf + s->len, ptr, n);
    s->len += n;
    s->buf[s->len] = '\0';

    /* Process complete lines (those terminated by \n or \r\n) */
    char *start = s->buf;
    char *eol;
    while ((eol = strchr(start, '\n')) != NULL) {
        *eol = '\0';
        size_t ll = (size_t)(eol - start);
        /* Strip trailing \r */
        if (ll > 0 && start[ll - 1] == '\r') start[ll - 1] = '\0';

        if (start[0] == '\0') {
            /* Empty line → dispatch if we accumulated anything */
            sse_dispatch_event(s);
        } else if (start[0] == ':') {
            /* SSE comment → skip */
        } else if (!strncmp(start, "event:", 6)) {
            const char *v = start + 6;
            while (*v == ' ') ++v;
            snprintf(s->event_name, sizeof(s->event_name), "%s", *v ? v : "message");
        } else if (!strncmp(start, "data:", 5)) {
            const char *v = start + 5;
            if (*v == ' ') ++v;
            if (s->data_line_count < 32) {
                s->data_lines[s->data_line_count++] = strdup(v);
            }
        } else if (!strncmp(start, "retry:", 6)) {
            const char *v = start + 6;
            while (*v == ' ') ++v;
            s->retry_ms = (int)atol(v);
            s->retry_set = 1;
        }
        start = eol + 1;
    }
    /* Remove consumed bytes from buffer */
    if (start > s->buf) {
        size_t consumed = (size_t)(start - s->buf);
        memmove(s->buf, start, s->len - consumed + 1);
        s->len -= consumed;
    }
    return n;
}

nsbc_rc_t nsbc_sse_iter(const char *sse_url, int timeout_s,
                        const char *api_token,
                        nsbc_frame_cb_t cb, void *userdata)
{
    if (!sse_url || !cb) return NSBC_ERR_INVALID_ARG;
    if (timeout_s <= 0) timeout_s = 30;

    signal(SIGINT, on_sigint);

    while (!g_stop) {
        /* One connection — run until disconnect or signal */
        CURL *h = curl_common(sse_url, timeout_s);
        if (!h) return NSBC_ERR_HTTP;

        curl_slist *hdrs = NULL;
        hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
        hdrs = curl_slist_append(hdrs, "Cache-Control: no-cache");
        if (api_token && *api_token) {
            char auth[512];
            snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_token);
            hdrs = curl_slist_append(hdrs, auth);
        }
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "GET");

        struct sse_state s;
        sse_state_init(&s);
        s.cb = cb;
        s.userdata = userdata;
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &s);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, sse_write_cb);

        CURLcode rc = curl_easy_perform(h);
        curl_easy_cleanup(h);
        curl_slist_free_all(hdrs);
        sse_state_free(&s);

        if (g_stop) break;
        if (rc != CURLE_OK) {
            fprintf(stderr, "[sse] curl error: %s — reconnecting in 3s\n", curl_easy_strerror(rc));
        }
        sleep(3);
    }
    return NSBC_OK;
}

/* -------------------------------------------------------------------- */
/* SSE event JSON → canonical frame                                       */
/* -------------------------------------------------------------------- */

int nsbc_sse_event_to_frame(const char *sse_json, nsbc_frame_t *frame)
{
    if (!sse_json || !frame) return 0;
    memset(frame, 0, sizeof(*frame));

    cJSON *root = cJSON_Parse(sse_json);
    if (!root) return 0;

    cJSON *kind_outer = cJSON_GetObjectItemCaseSensitive(root, "kind");
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!payload) payload = root; /* some deployments put payload directly */

    /* Map sse event → domain */
    cJSON *evt = cJSON_GetObjectItemCaseSensitive(payload, "event");
    const char *sse_evt = (evt && evt->valuestring) ? evt->valuestring : "";
    const char *domain_map[] = { "ring","CALL_INCOMING", "answer","CALL_ANSWERED",
                                  "hangup","CALL_END", "originate","CALL_OUTGOING", NULL };
    const char *domain = NULL;
    for (int i = 0; domain_map[i]; i += 2) {
        if (!strcasecmp(sse_evt, domain_map[i])) { domain = domain_map[i + 1]; break; }
    }
    if (!domain) {
        if (sse_evt[0]) domain = sse_evt;
        else if (kind_outer && kind_outer->valuestring) domain = kind_outer->valuestring;
        else domain = "EVENT";
    }
    snprintf(frame->domain, sizeof(frame->domain), "%s", domain);
    classify(frame->domain, frame->kind, sizeof(frame->kind));
    snprintf(frame->source, sizeof(frame->source), "sse");

    /* ts */
    cJSON *tsj = cJSON_GetObjectItemCaseSensitive(payload, "ts");
    if (!tsj) tsj = cJSON_GetObjectItemCaseSensitive(payload, "timestamp");
    if (tsj && tsj->valuestring && strlen(tsj->valuestring) >= 23) {
        memcpy(frame->ts, tsj->valuestring, 23);
        frame->ts[23] = '\0';
    } else {
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        snprintf(frame->ts, sizeof(frame->ts),
                 "%04d-%02d-%02dT%02d:%02d:%02d.000",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    /* body = compact payload JSON */
    char *pstr = cJSON_PrintUnformatted(payload);
    if (pstr) {
        snprintf(frame->body, sizeof(frame->body), "%s", pstr);
        free(pstr);
    }

    /* raw = full SSE JSON */
    char *rstr = cJSON_PrintUnformatted(root);
    if (rstr) {
        snprintf(frame->raw, sizeof(frame->raw), "%s", rstr);
        free(rstr);
    }

    cJSON_Delete(root);
    return 1;
}

/* ===================================================================== */
/* WebhookSink — synchronous POST every frame (requires curl + cJSON)     */
/* ===================================================================== */

struct nsbc_webhook_sink {
    char url[512];
    int  timeout_s;
    int  retries;
    int  ok_count;
    int  fail_count;
};

/* Build JSON body from frame — uses cJSON for safety.
 * Caller frees returned string. */
static char *frame_to_json(const nsbc_frame_t *frame)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "ts", frame->ts);
    cJSON_AddStringToObject(root, "domain", frame->domain);
    cJSON_AddStringToObject(root, "kind", frame->kind);
    cJSON_AddStringToObject(root, "body", frame->body);
    cJSON_AddStringToObject(root, "raw", frame->raw);
    cJSON_AddStringToObject(root, "source", frame->source[0] ? frame->source : "");

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

nsbc_webhook_sink_t *nsbc_webhook_new(const char *url, int timeout_s, int retries)
{
    if (!url) return NULL;
    nsbc_webhook_sink_t *wh = calloc(1, sizeof(*wh));
    if (!wh) return NULL;
    snprintf(wh->url, sizeof(wh->url), "%s", url);
    wh->timeout_s = timeout_s > 0 ? timeout_s : 5;
    wh->retries = retries > 0 ? retries : 1;
    return wh;
}

void nsbc_webhook_free(nsbc_webhook_sink_t *wh)
{
    free(wh);
}

int nsbc_webhook_send(nsbc_webhook_sink_t *wh, const nsbc_frame_t *frame)
{
    if (!wh || !frame) return 0;

    char *body = frame_to_json(frame);
    if (!body) { wh->fail_count++; return 0; }

    int max_attempts = wh->retries + 1;
    int ok = 0;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        CURL *h = curl_common(wh->url, wh->timeout_s);
        if (!h) break;

        curl_slist *hdrs = NULL;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        char evhdr[128];
        snprintf(evhdr, sizeof(evhdr), "X-NSBC-Event: %s", frame->domain);
        hdrs = curl_slist_append(hdrs, evhdr);
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "POST");

        struct buf resp = { NULL, 0 };
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp);

        CURLcode rc = curl_easy_perform(h);
        long http_code = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(h);
        curl_slist_free_all(hdrs);
        free(resp.data);

        if (rc == CURLE_OK && http_code >= 200 && http_code < 300) {
            ok = 1;
            break;
        }
        if (attempt < max_attempts - 1) sleep(1);
    }

    free(body);
    if (ok) wh->ok_count++; else wh->fail_count++;
    return ok;
}

int nsbc_webhook_ok(const nsbc_webhook_sink_t *wh)   { return wh ? wh->ok_count : 0; }
int nsbc_webhook_fail(const nsbc_webhook_sink_t *wh) { return wh ? wh->fail_count : 0; }

/* ===================================================================== */
/* Stub implementations when compiled WITHOUT libcurl                    */
/* ===================================================================== */

#else /* !NSBC_WITH_CURL */

void nsbc_set_ssl_verify(int verify) { (void)verify; /* no-op without curl */ }

int nsbc_originate_via_web(const char *api_base, const char *call_url,
                            int timeout_s, const char *api_token,
                            char **payload_out)
{
    (void)api_base; (void)call_url; (void)timeout_s;
    (void)api_token; (void)payload_out;
    fprintf(stderr, "[nsbc] HTTP path requires building with -DNSBC_WITH_CURL -lcurl\n");
    return 0;
}

int nsbc_originate_dual(const char *call_url, const char *via,
                        const char *uds_path, const char *api_base,
                        const char *api_token, char **via_out)
{
    if (!call_url) return 0;
    if (via_out) *via_out = NULL;

    /* Without curl: only UDS path works. "web" or "auto-without-uds" fails. */
    if (via && !strcmp(via, "web")) {
        fprintf(stderr, "[nsbc] HTTP path requires building with -DNSBC_WITH_CURL -lcurl\n");
        return 0;
    }
    if (!uds_path || access(uds_path, F_OK) != 0) {
        fprintf(stderr, "[nsbc] no UDS at %s and no curl built in\n",
                uds_path ? uds_path : "(null)");
        return 0;
    }

    nsbc_conn_t *c = nsbc_connect(uds_path);
    if (!c) return 0;
    char resp[NSBC_RESP_LEN];
    int ok = nsbc_originate(c, call_url, resp, sizeof(resp));
    nsbc_close(c);
    if (via_out) *via_out = strdup("uds");
    return ok;
}

nsbc_rc_t nsbc_sse_iter(const char *sse_url, int timeout_s,
                        const char *api_token,
                        nsbc_frame_cb_t cb, void *userdata)
{
    (void)sse_url; (void)timeout_s; (void)api_token;
    (void)cb; (void)userdata;
    fprintf(stderr, "[nsbc] SSE requires building with -DNSBC_WITH_CURL -lcurl\n");
    return NSBC_ERR_NO_CURL;
}

int nsbc_sse_event_to_frame(const char *sse_json, nsbc_frame_t *frame)
{
    (void)sse_json; (void)frame;
    fprintf(stderr, "[nsbc] SSE JSON parsing requires building with -DNSBC_WITH_CURL\n");
    return 0;
}

/* WebhookSink stubs — all fail with clear error */
nsbc_webhook_sink_t *nsbc_webhook_new(const char *url, int timeout_s, int retries)
{
    (void)url; (void)timeout_s; (void)retries;
    fprintf(stderr, "[nsbc] WebhookSink requires building with -DNSBC_WITH_CURL -lcurl\n");
    return NULL;
}
void nsbc_webhook_free(nsbc_webhook_sink_t *wh) { (void)wh; }
int nsbc_webhook_send(nsbc_webhook_sink_t *wh, const nsbc_frame_t *frame)
{
    (void)wh; (void)frame;
    fprintf(stderr, "[nsbc] WebhookSink requires building with -DNSBC_WITH_CURL -lcurl\n");
    return 0;
}
int nsbc_webhook_ok(const nsbc_webhook_sink_t *wh)   { (void)wh; return 0; }
int nsbc_webhook_fail(const nsbc_webhook_sink_t *wh) { (void)wh; return 0; }

#endif /* NSBC_WITH_CURL */
