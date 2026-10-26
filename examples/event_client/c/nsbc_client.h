/* nsbc_client.h — NSBC Unified Client Library (C)
 *
 * Public API for NSBC realtime event/CDR subscription + originate over
 * Unix Domain Socket. Pure POSIX, no third-party deps.
 *
 * Build:
 *   gcc -std=gnu99 -O2 -Wall -Wextra -c nsbc_client.c -o nsbc_client.o
 *   gcc -std=gnu99 -O2 -Wall -Wextra -c event_client.c -o event_client.o
 *   gcc nsbc_client.o event_client.o -o event_client
 *
 * Thread model: single-threaded. Callbacks fire on the iter loop thread.
 * No mutexes, no async I/O, no closures — plain C.
 *
 * Usage (library-only, no CLI):
 *   #include "nsbc_client.h"
 *   nsbc_conn_t *c = nsbc_connect("/var/run/nsbc/control.sock");
 *   nsbc_subscribe(c, "events,cdr");
 *   char *cu = nsbc_build_call_url(&opts);
 *   nsbc_originate(c, cu);   // synchronous, waits until nsbc> prompt
 *   nsbc_iter_frames(c, my_frame_cb, NULL);  // blocking
 *   nsbc_close(c);
 *   free(cu);
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
    NSBC_ERR_NO_UDS       = -8, /* Non-Unix platform — HTTP path not implemented */
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
/* HTTP originate — intentionally not implemented (std-only library).
 * Python / Go clients provide the full dual-path stack.                */
/* -------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* NSBC_CLIENT_H */
