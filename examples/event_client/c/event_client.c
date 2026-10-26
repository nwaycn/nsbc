/* event_client.c — NSBC Event/CDR subscription & originate client (C CLI demo).
 *
 * Thin shell — all real API lives in nsbc_client.{h,c}. This file is ONLY
 * flag parsing + mode dispatch + pretty printing.
 *
 * Build (with nsbc_client.c):
 *   gcc -std=gnu99 -O2 -Wall -Wextra -o event_client nsbc_client.c event_client.c
 *
 * Usage:
 *   ./event_client                                         # default originate+subscribe
 *   ./event_client --orig-to 7gja30rf --video              # WebRTC video (default on)
 *   ./event_client --mode subscribe                        # pure observer
 *   ./event_client --mode originate --orig-to 1001 --no-video
 *   ./event_client --uds /tmp/nsbc-e2e/control.sock
 */

#define _GNU_SOURCE

#include "nsbc_client.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_sigint = 0;
static void on_sigint(int s) { (void)s; g_sigint = 1; nsbc_iter_stop(); }

#define RESET "\x1b[0m"
#define BOLD  "\x1b[1m"
#define DIM   "\x1b[2m"

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

static void banner(const char *msg)
{
    fprintf(stdout, "%s[nsbc] %s%s\n", DIM, msg, RESET);
    fflush(stdout);
}

static void print_frame_cb(const nsbc_frame_t *frame, void *userdata)
{
    if (!frame) return;
    const char *short_ts = strlen(frame->ts) >= 12 ? frame->ts + 11 : frame->ts;
    const char *c = domain_color(frame->domain);

    if (!strcmp(frame->domain, "CDR")) {
        fprintf(stdout, "%s %s%12s%s %s\n", short_ts, c, "CDR", RESET, frame->body);
    } else {
        fprintf(stdout, "%s %s%15s%s %s\n", short_ts, c, frame->domain, RESET, frame->body);
    }
    fflush(stdout);
    (void)userdata;
}

static void print_timeline_cb(const nsbc_frame_t *frame, void *userdata)
{
    /* The core C SDK doesn't ship a CallCorrelator (single-file std-only design).
     * We track one simple last-call_id to print a rough end-of-life marker. */
    (void)frame;
    (void)userdata;
}

/* -------------------------------------------------------------------- */
/* Flag parsing                                                          */
/* -------------------------------------------------------------------- */

static void print_help(void)
{
    fprintf(stderr,
        "NSBC Event Client (C)\n"
        "\n"
        "Canonical transport: HTTPS + SSE via nsbc-web (external integrations).\n"
        "UDS path is local-debug only (same host as nsbcd).\n"
        "\n"
        "Build:\n"
        "  # UDS only (local-debug, zero-dep)\n"
        "  gcc -std=gnu99 -O2 -Wall -Wextra -o event_client nsbc_client.c event_client.c\n"
        "  # Full (HTTP originate + SSE stream — canonical external surface)\n"
        "  sudo apt-get install libcurl4-openssl-dev\n"
        "  gcc -DNSBC_WITH_CURL -std=gnu99 -O2 -Wall -Wextra \\\n"
        "      cJSON.c nsbc_client.c event_client.c -o event_client -lcurl\n"
        "\n"
        "Usage:\n"
        "  # Local-debug (UDS, no web sidecar)\n"
        "  ./event_client --uds /var/run/nsbc/control.sock --mode subscribe\n"
        "  # External integration (HTTP + SSE — requires -DNSBC_WITH_CURL)\n"
        "  ./event_client --transport sse --api-base https://nsbc-web:443 \\\n"
        "      --api-token YOUR_BEARER --insecure --mode subscribe\n"
        "  ./event_client --orig-to 7gja30rf --insecure --api-token xxx\n"
        "\n"
        "Flags:\n"
        "  --mode           auto | subscribe | originate | originate+subscribe (default: auto)\n"
        "  --transport       uds | sse | auto (default: auto)\n"
        "                   uds = local-debug only — same host as nsbcd\n"
        "                   sse = canonical external surface — requires -DNSBC_WITH_CURL\n"
        "  --uds            UDS socket path (default: /var/run/nsbc/control.sock)\n"
        "  --topics         subscribe topics (default: events,cdr)\n"
        "  --api-base       nsbc-web HTTP base (default: https://127.0.0.1:443)\n"
        "  --sse-url        SSE stream URL (default: <api-base>/api/v1/calls/stream)\n"
        "  --api-token      Bearer token for HTTP auth (originate + SSE)\n"
        "  --insecure       skip TLS certificate verification\n"
        "  --orig-via       auto | uds | web (default: auto)\n"
        "  --orig-to        destination extension (default: 1000)\n"
        "  --app            post-originate app (default: echo)\n"
        "  --caller-id NUM  origination caller ID\n"
        "  --caller-name NAME origination caller name\n"
        "  --timeout N      ringing timeout seconds\n"
        "  --video / --no-video   video on/off (default on)\n"
        "  --audio / --no-audio   multi-audio on/off (default on)\n"
        "  --no-audio-codecs      skip audio_codecs brace var\n"
        "  --no-video-codecs      skip video_codecs brace var\n"
        "  --help / -h\n");
}

/* Returns heap-allocated opts — caller frees. NULL on parse error / help. */
static nsbc_orig_opts_t *parse_opts(int argc, char **argv,
                                    const char **out_mode,
                                    const char **out_transport,
                                    const char **out_uds,
                                    const char **out_topics,
                                    const char **out_api_base,
                                    const char **out_sse_url,
                                    const char **out_api_token,
                                    const char **out_orig_via,
                                    int *out_insecure)
{
    nsbc_orig_opts_t *o = calloc(1, sizeof(*o));
    if (!o) return NULL;

    *out_mode        = "auto";
    *out_transport   = "auto";
    *out_uds         = NSBC_DEFAULT_UDS;
    *out_topics      = NSBC_DEFAULT_TOPICS;
    *out_api_base    = "https://127.0.0.1:443";
    *out_sse_url     = NULL;  /* build from api_base later */
    *out_api_token   = NULL;
    *out_orig_via    = "auto";
    *out_insecure    = 0;

    static char default_audio[] = "PCMU,PCMA,G722,OPUS";
    static char default_video[] = "H264,VP8";
    int audio_on = 1, video_on = 1;
    int skip_audio = 0, skip_video = 0;

    o->orig_to = "1000";
    o->app     = "echo";

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *nxt = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(a, "--mode")          && nxt) { *out_mode = nxt; ++i; }
        else if (!strcmp(a, "--transport") && nxt) { *out_transport = nxt; ++i; }
        else if (!strcmp(a, "--uds")       && nxt) { *out_uds  = nxt; ++i; }
        else if (!strcmp(a, "--topics")   && nxt) { *out_topics = nxt; ++i; }
        else if (!strcmp(a, "--api-base")  && nxt) { *out_api_base = nxt; ++i; }
        else if (!strcmp(a, "--sse-url")   && nxt) { *out_sse_url = nxt; ++i; }
        else if (!strcmp(a, "--api-token") && nxt) { *out_api_token = nxt; ++i; }
        else if (!strcmp(a, "--orig-via")  && nxt) { *out_orig_via = nxt; ++i; }
        else if (!strcmp(a, "--insecure")) { *out_insecure = 1; }
        else if (!strcmp(a, "--orig-to")   && nxt) { o->orig_to = nxt; ++i; }
        else if (!strcmp(a, "--app")       && nxt) { o->app = nxt; ++i; }
        else if (!strcmp(a, "--caller-id") && nxt) { o->caller_id = nxt; ++i; }
        else if (!strcmp(a, "--caller-name") && nxt) { o->caller_name = nxt; ++i; }
        else if (!strcmp(a, "--timeout")   && nxt) { o->timeout = atoi(nxt); ++i; }
        else if (!strcmp(a, "--video"))    { video_on = 1; }
        else if (!strcmp(a, "--no-video")) { video_on = 0; skip_video = 1; }
        else if (!strcmp(a, "--audio"))    { audio_on = 1; }
        else if (!strcmp(a, "--no-audio")) { audio_on = 0; skip_audio = 1; }
        else if (!strcmp(a, "--no-audio-codecs")) { skip_audio = 1; }
        else if (!strcmp(a, "--no-video-codecs")) { skip_video = 1; }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            print_help(); free(o); return NULL;
        } else {
            if (a[0] != '-' && *a) *out_uds = a;
        }
    }

    if (audio_on && !skip_audio) o->audio_codecs = default_audio;
    if (video_on && !skip_video) o->video_codecs = default_video;

    return o;
}

/* -------------------------------------------------------------------- */
/* Modes                                                                 */
/* -------------------------------------------------------------------- */

static void run_subscribe(nsbc_conn_t *c, const char *topics)
{
    banner("subscribe mode");
    char resp[1024];
    nsbc_subscribe(c, topics, resp, sizeof(resp));
    /* Print non-empty response lines */
    char *saveptr = NULL;
    for (char *ln = strtok_r(resp, "\n", &saveptr); ln; ln = strtok_r(NULL, "\n", &saveptr)) {
        if (ln[0]) banner(ln);
    }
    banner("waiting for events — Ctrl-C to quit");
    nsbc_iter_frames(c, print_frame_cb, NULL);
}

static void run_originate_only(nsbc_conn_t *c, nsbc_orig_opts_t *opts)
{
    char *cu = nsbc_build_call_url(opts);
    if (!cu) { fprintf(stderr, "[FAIL] build_call_url\n"); return; }

    banner(cu);
    char resp[NSBC_RESP_LEN];
    int ok = nsbc_originate(c, cu, resp, sizeof(resp));
    if (ok) {
        fprintf(stdout, "[OK] %s\n", resp);
    } else {
        fprintf(stderr, "[FAIL] %s\n", resp);
    }
    free(cu);
}

static void run_originate_and_subscribe(nsbc_conn_t *c, nsbc_orig_opts_t *opts, const char *topics)
{
    char *cu = nsbc_build_call_url(opts);
    if (!cu) { fprintf(stderr, "[FAIL] build_call_url\n"); return; }

    fprintf(stdout, "%s[1/2] originate: %s%s\n", DIM, cu, RESET);
    fflush(stdout);
    char resp[NSBC_RESP_LEN];
    int ok = nsbc_originate(c, cu, resp, sizeof(resp));
    free(cu);

    if (!ok) {
        fprintf(stderr, "%s[FAIL] originate: %s%s\n", "\x1b[31m", resp, RESET);
        return;
    }
    fprintf(stdout, "[OK] %s\n", resp);
    banner("[2/2] subscribe + iter");

    char sresp[1024];
    nsbc_subscribe(c, topics, sresp, sizeof(sresp));
    char *saveptr = NULL;
    for (char *ln = strtok_r(sresp, "\n", &saveptr); ln; ln = strtok_r(NULL, "\n", &saveptr)) {
        if (ln[0]) banner(ln);
    }
    banner("waiting for events — Ctrl-C to quit");
    nsbc_iter_frames(c, print_frame_cb, NULL);
}

/* -------------------------------------------------------------------- */
/* main                                                                  */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* Modes — HTTP (nsbc-web) path (requires -DNSBC_WITH_CURL)              */
/* -------------------------------------------------------------------- */

static void run_originate_http(nsbc_orig_opts_t *opts, const char *api_base,
                               const char *api_token)
{
    char *cu = nsbc_build_call_url(opts);
    if (!cu) { fprintf(stderr, "[FAIL] build_call_url\n"); return; }

    banner(cu);
    char *payload = NULL;
    int ok = nsbc_originate_via_web(api_base, cu, 10, api_token, &payload);
    free(cu);
    if (ok) {
        fprintf(stdout, "[OK] via=web  %s\n", payload ? payload : "");
    } else {
        fprintf(stderr, "%s[FAIL] via=web  %s%s\n", "\x1b[31m",
                payload ? payload : "HTTP originate failed", RESET);
    }
    free(payload);
}

static void run_subscribe_sse(const char *sse_url, const char *api_token)
{
    fprintf(stdout, "%s[nsbc] [transport=SSE] url=%s%s\n", DIM, sse_url, RESET);
    fprintf(stdout, "%s[nsbc] waiting for events — Ctrl-C to quit%s\n", DIM, RESET);
    fflush(stdout);
    signal(SIGINT, on_sigint);

    while (!g_sigint) {
        nsbc_rc_t rc = nsbc_sse_iter(sse_url, 30, api_token, print_frame_cb, NULL);
        if (rc == NSBC_ERR_NO_CURL) {
            fprintf(stderr, "\n[FATAL] SSE requires building with -DNSBC_WITH_CURL -lcurl\n");
            break;
        }
        if (g_sigint) break;
        sleep(3); /* auto-reconnect */
    }
}

/* -------------------------------------------------------------------- */
/* main                                                                  */
/* -------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *mode, *transport, *uds_path, *topics;
    const char *api_base, *sse_url, *api_token, *orig_via;
    int insecure;
    nsbc_orig_opts_t *opts = parse_opts(argc, argv, &mode, &transport, &uds_path,
                                         &topics, &api_base, &sse_url,
                                         &api_token, &orig_via, &insecure);
    if (!opts) return 0;

    signal(SIGINT, on_sigint);

    if (insecure) nsbc_set_ssl_verify(0);

    /* Resolve sse_url default: <api_base>/api/v1/calls/stream */
    char sse_buf[512];
    if (!sse_url) {
        size_t alen = strlen(api_base);
        while (alen > 0 && api_base[alen - 1] == '/') --alen;
        snprintf(sse_buf, sizeof(sse_buf), "%.*s/api/v1/calls/stream", (int)alen, api_base);
        sse_url = sse_buf;
    }

    /* Resolve transport: "auto" → prefer SSE if no UDS path explicitly given */
    const char *actual_transport = transport;
    if (!strcmp(transport, "auto")) {
        if (!access(uds_path, F_OK) == 0) {
            actual_transport = "uds";
        } else {
            /* no UDS socket → go SSE (canonical external path) */
            actual_transport = "sse";
        }
    }

    /* ---- UDS path (local-debug) ---- */
    if (!strcmp(actual_transport, "uds")) {
        nsbc_conn_t *c = nsbc_connect(uds_path);
        if (!c) {
            fprintf(stderr, "%s[FAIL] UDS connect: %s%s\n", "\x1b[31m", strerror(errno), RESET);
            free(opts);
            return 2;
        }
        banner(c->banner[0] ? c->banner : uds_path);

        if (!strcmp(mode, "subscribe")) {
            run_subscribe(c, topics);
        } else if (!strcmp(mode, "originate")) {
            run_originate_only(c, opts);
        } else {
            run_originate_and_subscribe(c, opts, topics);
        }
        nsbc_close(c);
    }
    /* ---- HTTP/SSE path (canonical external surface) ---- */
    else {
        fprintf(stdout, "%s[nsbc] [transport=SSE/HTTP] api_base=%s%s\n", DIM, api_base, RESET);
        fflush(stdout);

        if (!strcmp(mode, "subscribe")) {
            run_subscribe_sse(sse_url, api_token);
        } else if (!strcmp(mode, "originate")) {
            run_originate_http(opts, api_base, api_token);
        } else {
            /* auto / originate+subscribe */
            fprintf(stdout, "%s[1/2] originate (HTTP)...%s\n", DIM, RESET);
            run_originate_http(opts, api_base, api_token);
            fprintf(stdout, "%s[2/2] subscribe (SSE)...%s\n", DIM, RESET);
            run_subscribe_sse(sse_url, api_token);
        }
    }

    free(opts);
    return 0;
}
