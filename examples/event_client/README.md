# NSBC Event Client — Multi-Language SDK

Reference client libraries and CLI demos for NSBC realtime event/CDR
subscription + originate over **Unix Domain Socket** (same-host) and
**nsbc-web HTTP API** (remote deployments).

## Architecture — Library vs Application (every language)

Every language here ships **two** artifacts:

| Artifact          | Role                                                                   |
|-------------------|------------------------------------------------------------------------|
| **Library / SDK** | Pure API, no print-to-terminal, no argparse, main program agnostic     |
| **CLI demo**      | argparse + mode dispatch + pretty printing — *consumes* the library     |

**Caller-facing contract (the SDK part)** — every language implements these
building blocks. See [API contract](#api-contract) below.

```
python/
├── nsbc_client.py        # Library (NsbccClient, NsbccClientSse, originate(),
│                         #          build_call_url(), parse_frame(),
│                         #          CallCorrelator, WebhookSink)
└── event_client.py       # CLI demo — imports nsbc_client

go/
├── nsbc_sdk/sdk.go       # Library (Client, Originate, parse_frame, CallCorrelator, WebhookSink)
├── go.mod
└── main.go               # CLI demo — imports ./nsbc_sdk

rust/
├── Cargo.toml            # [lib] nsbc_sdk + [[bin]] event_client
├── src/lib.rs            # Library (ClientUds, Originate, parse_frame, CallCorrelator)
└── src/main.rs           # CLI demo

c/
├── nsbc_client.h         # Public C API (opaque nsbc_conn_t, nsbc_build_call_url, …)
├── nsbc_client.c         # Implementation
└── event_client.c        # CLI demo — #include "nsbc_client.h"
```

## API Contract

### 1. Connection

| Python               | Go                      | Rust               | C                          |
|----------------------|-------------------------|--------------------|----------------------------|
| `uds = NsbccClientUds(path).connect()` | `c := nsbc_sdk.NewClientUds(path).Connect()` | `ClientUds::new(path)?.connect()?` | `nsbc_conn_t *c = nsbc_connect(path);` |
| `uds.close()`        | `c.Close()`             | `c.close()`        | `nsbc_close(c);`           |

### 2. Subscribe + receive frames

| Python                     | Go                      | Rust                 | C                               |
|----------------------------|-------------------------|----------------------|---------------------------------|
| `uds.subscribe("events,cdr")` | `c.Subscribe("events,cdr")` | `c.subscribe("events,cdr")?` | `nsbc_subscribe(c, "events,cdr", resp, sizeof(resp));` |
| `for f in uds.iter_frames():` | `ch := make(chan *Frame); go c.IterFrames(ch); for f := range ch { ... }` | `for f in c.iter_frames() { ... }` | `nsbc_iter_frames(c, my_cb, userdata);` |

### 3. Build call_url

| Python | Go | Rust | C |
|--------|----|------|---|
| `build_call_url(orig_to="7gja30rf", app="echo", audio=["PCMU",…], video=["H264",…])` | `BuildCallUrl(OriginateOpts{…})` | `build_call_url(OriginateOpts{…})` | `char *cu = nsbc_build_call_url(&(nsbc_orig_opts_t){…}); free(cu);` |

Output:
```
{audio_codecs=PCMU,PCMA,G722,OPUS,video_codecs=H264,VP8}user/7gja30rf &echo
```

### 4. Originate (dual path — UDS preferred, HTTP fallback)

| Python | Go | Rust | C |
|--------|----|------|---|
| `ok, payload, via = originate(call_url, via="auto", uds_path=…, api_base=…)` | `result := nsbc_sdk.Originate(call_url, "auto", uds_path, api_base, token)` | `originate(call_url, "auto", uds, api_base, token)` | `nsbc_originate(c, call_url, resp, sizeof(resp))` — UDS path only |

### 5. Frame parsing

| Python | Go | Rust | C |
|--------|----|------|---|
| `frame = parse_frame(line)` | `f := ParseFrame(line)` | `parse_frame(line)` | `nsbc_frame_t f; nsbc_parse_frame(line, &f);` |

Every language produces a struct/dict with these fields:

| Field   | Type      | Meaning                                                  |
|---------|-----------|----------------------------------------------------------|
| `ts`    | string    | `YYYY-MM-DDTHH:MM:SS.mmm`                                |
| `domain`| string    | `CALL_INCOMING / CALL_ANSWERED / CALL_END / CDR / REGISTER / ERR / OK / subscribed:` |
| `kind`  | string    | `call / cdr / register / cmd / other`                    |
| `fields`| dict/struct | `domain == "CDR"` → full JSON; else `k=v` pairs          |
| `raw`   | string    | original line                                             |

### 6. Correlator / WebhookSink (Python + Go + Rust)

| Python                 | Go                              | Rust                          | C |
|------------------------|---------------------------------|-------------------------------|---|
| `c = CallCorrelator(on_complete=timeline_cb)` | `c := nsbc_sdk.NewCallCorrelator(timeline_cb)` | `CallCorrelator::new(timeline_cb)` | (zero-dep design choice) |
| `c.feed(frame)`        | `c.Feed(f)`                     | `c.feed(&frame)`              |   |

| Python                | Go                            | Rust                                    | C |
|-----------------------|-------------------------------|-----------------------------------------|---|
| `w = WebhookSink(url, timeout=5, retries=2)` | `w := nsbc_sdk.NewWebhookSink(url, 5, 2)` | `WebhookSink::new(url, 5, 2)` | (zero-dep design choice) |
| `w.send(frame)`       | `w.Send(f)`                   | `w.send(&frame)`                        |   |

### 7. HTTP-only features (Python + Go + Rust)

| Feature       | Python | Go | Rust | C  |
|---------------|--------|----|------|----|
| `originate_via_web()` | ✅     | ✅  | ✅   | ❌ |
| `WebhookSink`          | ✅     | ✅  | ✅   | ❌ |
| `NsbccClientSse` / `SseClient` | ✅ | ✅  | ✅   | ❌ |
| `sse_event_to_frame()` | ✅   | ✅  | ✅   | ❌ |

All three now cover SSE remote deployments — same Python SSE protocol spec
(empty-line dispatch, `data:` leading-space trim, multi-line join, auto-reconnect).

## Codec Defaults

The Python / Go / Rust CLI demos all default to **WebRTC-safe** codec sets
— `--video` and `--audio` default *on* (matching real-world WebRTC originate).

| Flag              | Python default | Go default | Rust default | C default |
|-------------------|----------------|------------|--------------|-----------|
| `--video`         | True (H264,VP8)| True       | True         | True      |
| `--audio`         | True (PCMU,PCMA,G722,OPUS) | True | True | True |
| `--no-video`      | off            | off        | off          | off       |
| `--no-audio`      | off            | off        | off          | off       |

Explicit codec lists override via `--video-codecs H264,VP8` / `--audio-codecs PCMU,PCMA`
(Python only — Go/Rust/C CLI flags follow the CLI args you give).

## Build + Run

### Python (stdlib only, no install)
```bash
python3 event_client.py --orig-to 7gja30rf --app echo           # default video on
python3 event_client.py --mode subscribe                          # pure observer
python3 event_client.py --mode originate --orig-to 1001 --no-video
python3 event_client.py --mode subscribe --transport sse \
    --sse-url http://nsbc-web:8080/calls/stream --webhook-url ...
```

### Go
```bash
cd go && go build ./...
./main --orig-to 7gja30rf                                        # Linux
```

### Rust
```bash
cd rust && cargo run --release -- --orig-to 7gja30rf
```
Note: Rust binary compiles on Windows (Cargo verifies), but Unix stream
transport only functions on Linux / macOS / WSL.

### C
```bash
cd c
gcc -std=gnu99 -O2 -Wall -Wextra -o event_client nsbc_client.c event_client.c
./event_client --orig-to 7gja30rf                                # Linux only (UnixStream)
```

## Write Your Own Caller — Example Snippets

### Python — own minimal integration (library-only, no CLI)
```python
from nsbc_client import NsbccClientUds, parse_frame, CallCorrelator, originate, build_call_url

uds = NsbccClientUds("/var/run/nsbc/control.sock").connect()
uds.subscribe("events,cdr")

cu = build_call_url(orig_to="7gja30rf", app="echo", video=["H264","VP8"])
ok, payload, via = originate(cu)
print(f"dispatched via={via}: {payload}")

corr = CallCorrelator(on_complete=lambda tl: print(tl))
for raw in uds.iter_frames():
    f = parse_frame(raw)
    corr.feed(f)
uds.close()
```

### Go — own minimal integration
```go
import "event_client/nsbc_sdk"

c, _ := nsbc_sdk.NewClientUds("/var/run/nsbc/control.sock").Connect()
c.Subscribe("events,cdr")

cu := nsbc_sdk.BuildCallUrl(nsbc_sdk.OriginateOpts{
    OrigTo: "7gja30rf", App: "echo",
    Video: []string{"H264", "VP8"},
})
r := nsbc_sdk.Originate(cu, "auto", "/var/run/nsbc/control.sock",
                        "https://127.0.0.1:443", "")
fmt.Printf("ok=%v via=%s\n", r.OK, r.Via)

corr := nsbc_sdk.NewCallCorrelator(timelineCb)
frameCh := make(chan *nsbc_sdk.Frame)
go c.IterFrames(frameCh)
for f := range frameCh { corr.Feed(f) }
```

### C — own minimal integration
```c
#include "nsbc_client.h"

nsbc_conn_t *c = nsbc_connect("/var/run/nsbc/control.sock");
nsbc_subscribe(c, "events,cdr", NULL, 0);

nsbc_orig_opts_t opts = {
    .orig_to = "7gja30rf", .app = "echo",
    .video_codecs = "H264,VP8", .audio_codecs = "PCMU,PCMA,G722,OPUS",
};
char *cu = nsbc_build_call_url(&opts);
char resp[1024];
nsbc_originate(c, cu, resp, sizeof(resp)); free(cu);

nsbc_iter_frames(c, my_frame_cb, NULL);
nsbc_close(c);
```

## Known Limits

| Item                   | Python | Go    | Rust | C     | Root cause                                    |
|------------------------|--------|-------|------|-------|-----------------------------------------------|
| UDS subscribe + frame iter | ✅    | ✅     | ✅    | ✅    |                                               |
| Originate dual-path (UDS+HTTP) | ✅  | ✅     | ✅    | ⚠️ UDS | C: zero-dep design choice                      |
| CallCorrelator         | ✅      | ✅     | ✅    | ❌    | C: zero-dep design choice                      |
| WebhookSink (HTTP)     | ✅      | ✅     | ✅    | ❌    | C: zero-dep design choice                      |
| SSE transport (remote) | ✅      | ✅     | ✅    | ❌    | C: zero-dep design choice                      |
| TLS verify toggle      | ✅      | ✅     | ✅    | ❌    | C: no TLS layer                               |
| Non-Unix (Windows)     | ✅      | ✅ stub | stub | stub | UnixStream is Unix-only; stubs return clear err |
| Third-party deps       | 0       | 0      | reqwest+serde_json | 0      | Rust opts into reqwest for HTTP               |

### Root-cause legend

- **"❌" = deliberately omitted** (C zero-dep design, changes the build story from `gcc single-file` to `pkg-config -lcurl`).
- **"❌" = fundamental platform constraint** (UnixStream on Windows — std doesn't ship it).

Python / Go / Rust all provide the full **dual-path originate → UDS preferred, HTTP fallback**
stack + SSE remote deployments + CallCorrelator + WebhookSink + TLS verify toggle. C stays lean.

**Unix Domain Socket**: `AF_UNIX` stream. Text protocol:
- connect → nsbcd sends banner + `nsbc> ` prompt
- send `sub <topics>\n` → server echoes, then `nsbc> `
- server pushes `YYYY-MM-DDTHH:MM:SS.mmm <DOMAIN> <body>\n` lines
- any other command follows `cmd\n` → response lines → `nsbc> `

**nsbc-web HTTP**: `POST {api_base}/ext/v1/originate` JSON `{"call_url":"..."}`.
SSE stream: `GET {api_base}/calls/stream` emits `event: session` JSON payloads.

Frame body is either `k=v k=v ...` (events) or full JSON (CDR). Every SDK
parses both into the same `Frame` struct.
