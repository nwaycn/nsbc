/* nsbc_client.h — NSBC Unified Client Library (C)
 *
 * Public API for NSBC realtime event/CDR subscription + originate.
 *
 * Two build paths:
 *
 *   1) ZERO-DEP (UDS only, local-debug same-host):
 *      gcc -std=gnu99 -O2 -Wall -Wextra nsbc_client.c event_client.c -o event_client
 *      No external deps. Works on any Unix with AF_UNIX.
 *
 *   2) HTTP/SSE (full canonical surface — requires libcurl):
 *      sudo apt-get install libcurl4-openssl-dev   # Debian/Ubuntu
 *      gcc -DNSBC_WITH_CURL -std=gnu99 -O2 -Wall -Wextra \
 *          cJSON.c nsbc_client.c event_client.c \
 *          -o event_client -lcurl
 *      cJSON (public-domain single-file JSON parser) is vendored in c/.
 *      libcurl provides HTTPS + SSE. Build on any OS with libcurl (Unix + Windows).
 *
 * Thread model: single-threaded. Callbacks fire on the iter loop thread.
 * No mutexes, no async I/O, no closures — plain C.
 *
 * Architecture: HTTP/SSE via nsbc-web HTTPS + Bearer token is the canonical
 * external integration surface. UDS (path 1) is a local-debug convenience for
 * operators on the nsbcd host. All external call-center integrations should
 * use path 2.
 */

#ifndef NSBC_CLIENT_H
#define NSBC_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- */
/* Constants                                                             */
/* -------------------------------------------------------------------- */

#define NSBC_DEFAULT_UDS      "/var/run/nsbc/control.sock"
#define NSBC_DEFAULT_API_BASE "https://127.0.0.1:443"
#define NSBC_DEFAULT_TOPICS   "events,cdr"

#define NSBC_PROMPT           "nsbc> "

/* Frame body / command response buffer sizes */
#define NSBC_BODY_LEN         4096
#define NSBC_CMD_LEN          8192
#define NSBC_RESP_LEN         8192

/* -------------------------------------------------------------------- */
/* Opaque connection handle                                              */
/* -------------------------------------------------------------------- */

typedef struct nsbc_conn nsbc_conn_t;

/* -------------------------------------------------------------------- */
/* Frame — result of nsbc_parse_frame                                    */
/* -------------------------------------------------------------------- */

typedef struct {
    char  ts[24];          /* "YYYY-MM-DDTHH:MM:SS.mmm\0" */
    char  domain[64];     /* "CALL_INCOMING" / "CDR" / "REGISTER" ... */
    char  kind[16];       /* "call" / "cdr" / "register" / "cmd" / "other" */
    char  body[NSBC_BODY_LEN];
    char  raw[NSBC_BODY_LEN];
    char  source[16];      /* "uds" | "sse" | "http" */
} nsbc_frame_t;

/* Callback for iter_frames. userdata opaque. */
typedef void (*nsbc_frame_cb_t)(const nsbc_frame_t *frame, void *userdata);

/* -------------------------------------------------------------------- */
/* Originate options — input to nsbc_build_call_url                      */
/* All char* fields: NULL or empty → skip that brace var                 */
/* -------------------------------------------------------------------- */

typedef struct {
    const char *orig_to;       /* "7gja30rf" or NULL → "1000" */
    const char *app;           /* "echo" / "park" / "hangup" or NULL → "echo" */
    const char *audio_codecs;  /* "PCMU,PCMA,G722,OPUS" or NULL */
    const char *video_codecs;  /* "H264,VP8" or NULL */
    const char *caller_id;
    const char *caller_name;
    int         timeout;       /* 0 → skip */
    const char *call_url;      /* explicit call_url → bypass builder */
} nsbc_orig_opts_t;

/* -------------------------------------------------------------------- */
/* Errors                                                                */
/* -------------------------------------------------------------------- */

typedef enum {
    NSBC_OK               =  0,
    NSBC_ERR_UDS_NOT_FOUND = -1,
    NSBC_ERR_UDS_CONNECT  = -2,
    NSBC_ERR_SEND         = -3,
    NSBC_ERR_RECV         = -4,
    NSBC_ERR_TIMEOUT      = -5,
    NSBC_ERR_INVALID_ARG  = -6,
    NSBC_ERR_PARSE        = -7,
    NSBC_ERR_NO_UDS       = -8, /* Non-Unix platform — UDS path unavailable */
    NSBC_ERR_NO_CURL      = -9, /* Built without -DNSBC_WITH_CURL — HTTP path unavailable */
    NSBC_ERR_HTTP         = -10,
    NSBC_ERR_TLS          = -11,
    NSBC_ERR_JSON         = -12,
} nsbc_rc_t;

/* -------------------------------------------------------------------- */
/* Connection                                                            */
/* -------------------------------------------------------------------- */

nsbc_conn_t *nsbc_connect(const char *uds_path);
void         nsbc_close(nsbc_conn_t *c);

/* Receive banner/probe on connect (called inside nsbc_connect) */
int          nsbc_banner(const nsbc_conn_t *c, char *out, size_t out_cap);

/* Send a single \n-terminated command and block until nsbc> prompt or timeout_ms.
 * out: buffer that receives the response (without trailing prompt)
 * Returns NSBC_OK or NSBC_ERR_* */
nsbc_rc_t    nsbc_send_command(nsbc_conn_t *c, const char *cmd,
                               int timeout_ms, char *out, size_t out_cap);

int          nsbc_subscribe(nsbc_conn_t *c, const char *topics,
                            char *resp_out, size_t resp_cap);
int          nsbc_unsubscribe(nsbc_conn_t *c, const char *topics,
                              char *resp_out, size_t resp_cap);

/* -------------------------------------------------------------------- */
/* Frame parsing                                                         */
/* -------------------------------------------------------------------- */

/* Parse one raw obs_sub line → *frame. Returns 1 on success, 0 if not a frame */
int          nsbc_parse_frame(const char *line, nsbc_frame_t *frame);

/* Call the given callback for every frame pushed by nsbcd, forever
 * until the connection closes or nsbc_iter_stop() is called from a signal handler.
 * userdata is forwarded verbatim to cb.
 *
 * On non-Unix (Windows): immediately returns NSBC_ERR_NO_UDS (no UnixStream). */
nsbc_rc_t    nsbc_iter_frames(nsbc_conn_t *c, nsbc_frame_cb_t cb, void *userdata);

/* Signal-safe cooperative stop — set an atomic flag that next iter reads */
void         nsbc_iter_stop(void);

/* -------------------------------------------------------------------- */
/* Originate (UDS path)                                                  */
/* -------------------------------------------------------------------- */

/* Build an FS-style call_url with brace-var codec block.
 * Returns malloc'd string — caller frees(). */
char        *nsbc_build_call_url(const nsbc_orig_opts_t *opts);

/* Synchronous originate — sends "originate <call_url>" and waits for OK/ERR.
 * *response_out receives the raw nsbcd response (caller-provided buffer).
 * Returns 1 on OK, 0 on ERR/timeout */
int          nsbc_originate(nsbc_conn_t *c, const char *call_url,
                            char *response_out, size_t resp_cap);

/* -------------------------------------------------------------------- */
/* CallCorrelator — groups frames by call_id → timeline on CDR           */
/* Pure data-structure logic — no deps, works in BOTH build paths.      */
/* -------------------------------------------------------------------- */

/* Event in a call timeline. */
typedef struct nsbc_tl_event {
    char domain[32];
    char ts[32];
    struct nsbc_tl_event *next;
} nsbc_tl_event_t;

/* Completed call timeline — passed to on_complete callback.
 * Caller owns this pointer ONLY during the callback; correlator frees
 * it immediately after callback returns. */
typedef struct nsbc_call_timeline {
    char call_id[128];
    nsbc_tl_event_t *events;    /* linked list */
    char cdr_json[4096];        /* CDR record as compact JSON (if any) */
    int  cdr_present;
} nsbc_call_timeline_t;

/* Opaque correlator handle. */
typedef struct nsbc_call_correlator nsbc_call_correlator_t;

/* Callback signature: called when a call's CDR arrives.
 * timeline is valid for the duration of the callback only — do NOT hold
 * onto the pointer beyond it. */
typedef void (*nsbc_correlator_cb_t)(const nsbc_call_timeline_t *timeline,
                                     void *userdata);

nsbc_call_correlator_t *nsbc_correlator_new(nsbc_correlator_cb_t on_complete,
                                            void *userdata);
void nsbc_correlator_free(nsbc_call_correlator_t *cc);

/* Feed a frame into the correlator. Extracts call_id from frame->body
 * (searching for "call_id":"..." pattern). Silently ignores frames with
 * no call_id. */
void nsbc_correlator_feed(nsbc_call_correlator_t *cc, const nsbc_frame_t *frame);

/* Snapshot of active (not-yet-completed) call count. */
int  nsbc_correlator_active(const nsbc_call_correlator_t *cc);

/* -------------------------------------------------------------------- */
/* HTTP originate + SSE (canonical external integration surface)          */
/* Enabled only when compiled with -DNSBC_WITH_CURL -lcurl              */
/* -------------------------------------------------------------------- */

/* TLS verify toggle — default on (secure). */
void  nsbc_set_ssl_verify(int verify);

/* Originate via nsbc-web HTTPS POST /ext/v1/originate.
 * *payload_out: malloc'd JSON response (caller frees with free()).
 * Returns 1 on OK, 0 on HTTP error / network error. */
int   nsbc_originate_via_web(const char *api_base, const char *call_url,
                              int timeout_s, const char *api_token,
                              char **payload_out);

/* Unified originate — picks path by 'via'.
 *   "auto" → try UDS first, HTTP fallback (requires -DNSBC_WITH_CURL for fallback)
 *   "uds"  → nsbc_originate on a UDS conn (requires caller to have open UDS conn)
 *   "web"  → nsbc_originate_via_web (requires -DNSBC_WITH_CURL)
 * Returns 1 on OK, 0 otherwise. *via_out tells you which path was used. */
int   nsbc_originate_dual(const char *call_url, const char *via,
                          const char *uds_path, const char *api_base,
                          const char *api_token, char **via_out);

/* SSE stream client — consumes nsbc-web /api/v1/calls/stream forever.
 * cb fires per parsed frame (source="sse"). Auto-reconnects on disconnect.
 * Requires -DNSBC_WITH_CURL. Returns NSBC_ERR_NO_CURL otherwise.
 *
 *   nsbc_sse_iter(url, timeout_s, api_token, cb, userdata);
 *   ← blocks forever; SIGINT handler calls nsbc_iter_stop() to exit. */
nsbc_rc_t nsbc_sse_iter(const char *sse_url, int timeout_s,
                        const char *api_token,
                        nsbc_frame_cb_t cb, void *userdata);

/* Convert raw SSE event JSON → canonical frame (source="sse").
 * Requires -DNSBC_WITH_CURL for full cJSON parsing; fallback returns 0. */
int   nsbc_sse_event_to_frame(const char *sse_json, nsbc_frame_t *frame);

/* -------------------------------------------------------------------- */
/* WebhookSink — fire-and-forget POST every frame to external URL       */
/* Requires -DNSBC_WITH_CURL -lcurl.                                    */
/* In single-threaded C: send() is synchronous (blocks briefly per frame);
 * this is deliberate — simplicity over concurrency. Callers can run
 * send() from a dedicated thread if they need true non-blocking.       */
/* -------------------------------------------------------------------- */

typedef struct nsbc_webhook_sink nsbc_webhook_sink_t;

nsbc_webhook_sink_t *nsbc_webhook_new(const char *url, int timeout_s, int retries);
void nsbc_webhook_free(nsbc_webhook_sink_t *wh);

/* Send one frame as JSON POST. Blocks until success or retries exhausted.
 * Returns 1 on HTTP 2xx, 0 otherwise. */
int nsbc_webhook_send(nsbc_webhook_sink_t *wh, const nsbc_frame_t *frame);

/* Stats counters — atomic reads (not thread-safe, but informative in CLI). */
int nsbc_webhook_ok(const nsbc_webhook_sink_t *wh);
int nsbc_webhook_fail(const nsbc_webhook_sink_t *wh);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_CLIENT_H */
