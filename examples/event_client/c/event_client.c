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
        "Usage: event_client [flags]\n"
        "\n"
        "  --mode auto|originate+subscribe|subscribe|originate\n"
        "  --uds PATH               Unix Domain Socket path\n"
        "  --topics STRING          subscribe topics (default events,cdr)\n"
        "  --orig-to EXT            destination extension (default 1000)\n"
        "  --app echo|park|hangup   post-originate app (default echo)\n"
        "  --caller-id NUM          origination caller ID\n"
        "  --caller-name NAME       origination caller name\n"
        "  --timeout N              ringing timeout seconds\n"
        "  --video / --no-video     video on/off (default on)\n"
        "  --audio / --no-audio     multi-audio on/off (default on)\n"
        "  --no-audio-codecs        explicitly skip audio_codecs brace var\n"
        "  --no-video-codecs        explicitly skip video_codecs brace var\n"
        "  --help / -h\n");
}

/* Returns heap-allocated opts — caller frees. NULL on parse error. */
static nsbc_orig_opts_t *parse_opts(int argc, char **argv,
                                    const char **out_mode,
                                    const char **out_uds,
                                    const char **out_topics)
{
    nsbc_orig_opts_t *o = calloc(1, sizeof(*o));
    if (!o) return NULL;

    *out_mode = "auto";
    *out_uds  = NSBC_DEFAULT_UDS;
    *out_topics = NSBC_DEFAULT_TOPICS;

    /* Default codec lists (default on — matches Python Go Rust) */
    static char default_audio[] = "PCMU,PCMA,G722,OPUS";
    static char default_video[] = "H264,VP8";
    int audio_on = 1, video_on = 1;
    int skip_audio = 0, skip_video = 0;

    o->orig_to = "1000";
    o->app     = "echo";

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *nxt = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(a, "--mode")       && nxt) { *out_mode = nxt; ++i; }
        else if (!strcmp(a, "--uds")  && nxt) { *out_uds  = nxt; ++i; }
        else if (!strcmp(a, "--topics") && nxt) { *out_topics = nxt; ++i; }
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
            /* Positional arg → treat as UDS path */
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

int main(int argc, char **argv)
{
    const char *mode, *uds_path, *topics;
    nsbc_orig_opts_t *opts = parse_opts(argc, argv, &mode, &uds_path, &topics);
    if (!opts) return 0; /* help was shown */

    signal(SIGINT, on_sigint);

    nsbc_conn_t *c = nsbc_connect(uds_path);
    if (!c) {
        fprintf(stderr, "%s[FAIL] connect: %s%s\n", "\x1b[31m", strerror(errno), RESET);
        free(opts);
        return 2;
    }
    banner(c->banner[0] ? c->banner : uds_path);

    if (!strcmp(mode, "subscribe")) {
        run_subscribe(c, topics);
    } else if (!strcmp(mode, "originate")) {
        run_originate_only(c, opts);
    } else {
        /* auto / originate+subscribe */
        run_originate_and_subscribe(c, opts, topics);
    }

    free(opts);
    nsbc_close(c);
    return 0;
}
