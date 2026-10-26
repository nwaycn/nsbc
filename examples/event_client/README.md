# NSBC Event Client — Multi-Language SDK

Reference client libraries and CLI demos for NSBC realtime event/CDR
subscription + originate.

## Canonical Integration Surface (architectural decision — do not deviate)

**All external language SDKs MUST communicate with nsbc-web over HTTPS.**

```
Call-Center (any language)     nsbc-web (Rust sidecar)      nsbcd (C core)
    │                                │                              │
    │── HTTPS POST /ext/v1/originate ─►│                              │
    │── HTTPS GET  /api/v1/calls/stream (SSE) ─►│                     │
    │                                ├── UDS /control.sock ─────────►│
    │                                ├── UDS /data.sock ◄─────────────┤
    │                                │  UDS is nsbc-web INTERNAL      │
```

**Why this is non-negotiable:**
- nsbc-web HTTPS + Bearer token is the *only* documented, stable, versioned external API.
- UDS (`/var/run/nsbc/control.sock`) has permissions `0600` — only works as
  **same-host same-user**. External call-center integrations live on different
  hosts and cannot reach it.
- UDS is nsbc-web's *internal* transport to nsbcd. nsbc-web itself translates
  HTTP → UDS. SDKs should not skip the sidecar — it exists for a reason
  (TLS termination, auth, rate-limiting, payload normalization).

**UDS remains in Python/Go/Rust SDKs as a LOCAL DEBUG convenience only**
(`--transport uds`). It lets operators on the nsbcd host subscribe to events
without starting the nsbc-web sidecar. This is a debugging tool, not an
integration path.

**For any new language SDK: implement HTTP + SSE only.**
Use this Java SDK as the template — zero deps, stdlib only, correct production interface.

## Which transport should you use?

| Who you are                 | Path                 | Notes                                                    |
|-----------------------------|----------------------|----------------------------------------------------------|
| **External integration**    | **HTTPS + SSE**     | ✅ Canonical surface. Every new language MUST implement. |
| (call-center, CRM, etc.)    | HTTP POST originate  |                                                          |
| Same-host operator debug    | UDS (Python/Go/Rust) | ⚠️ Local debug only. `--transport uds` CLI flag.        |
|                             |                      | Zero-auth, socket perms = system trust.                  |
| nsbc-web → nsbcd (internal) | UDS always           | nsbc-web implementation detail, not a public path.        |

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
├── nsbc_client.py        # Library (stdlib only)
└── event_client.py       # CLI demo

go/
├── nsbc_sdk/sdk.go       # Library (stdlib only)
├── go.mod
└── main.go               # CLI demo

rust/
├── Cargo.toml            # [lib] nsbc_sdk + [[bin]] event_client
├── src/lib.rs            # Library (reqwest+serde_json for HTTP)
└── src/main.rs           # CLI demo

c/
├── nsbc_client.h         # Public C API (opaque nsbc_conn_t)
├── nsbc_client.c         # Implementation (UDS only, zero-dep embedded)
└── event_client.c        # CLI demo

java/                     # HTTP/SSE ONLY (Java 8 stdlib; UDS needs jnr-unixsocket)
├── src/nsbc/NsbcClient.java   # Library (hand-rolled JSON parser, no deps)
├── src/nsbc/EventClient.java # CLI demo
├── build.sh              # Unix compile: javac -source 1.8 -target 1.8
└── build.bat             # Windows compile
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

### Java (Java 8 LTS+, pure stdlib — no external deps)
```bash
cd java
bash build.sh                          # Linux / macOS
# or: build.bat                        # Windows
java -cp out nsbc.EventClient --mode subscribe \
    --transport sse --insecure \
    --api-token YOUR_BEARER_TOKEN \
    --api-base https://nsbc-web:443
java -cp out nsbc.EventClient --orig-to 7gja30rf --insecure
```
Compiled with `javac -source 1.8 -target 1.8`. Works on every host (Unix + Windows).

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

### Java — own minimal integration (library-only, no CLI — external call-center pattern)
```java
import nsbc.NsbcClient.*;
import java.util.*;

// 1. Originate
OriginateOpts opts = new OriginateOpts();
opts.origTo = "7gja30rf"; opts.app = "echo";
opts.videoCodecs = "H264,VP8";
String callUrl = NsbcClient.buildCallUrl(opts);
OriginateResult r = NsbcClient.originate(callUrl, "web",
                         "https://nsbc-web:443", "YOUR_BEARER_TOKEN");
System.out.println("ok=" + r.ok + " via=" + r.via);

// 2. Subscribe events via SSE (reconnects forever on disconnect)
CallCorrelator cc = new CallCorrelator(tl -> {
    System.out.println("Call completed: " + tl.callId);
    for (Map<String, Object> ev : tl.events) System.out.println("  " + ev);
});

Map<String, String> headers = new HashMap<>();
headers.put("Authorization", "Bearer YOUR_BEARER_TOKEN");
SseClient sse = new SseClient("https://nsbc-web:443/api/v1/calls/stream",
                              30, headers);
NsbcClient.setSslVerify(false); // for self-signed cert

sse.runForeach(frame -> {
    if (frame.domain.equals("CDR")) cc.feed(frame);
    System.out.println(frame.ts + " " + frame.domain + " " + frame.fields);
});

// 3. Forward every event to external webhook
WebhookSink wh = new WebhookSink("http://cc.example.com/nsbc", 5, 2);
wh.send(frame);
wh.stop();
```

## Known Limits

| Item                   | Python | Go    | Rust | C     | Java  | Root cause                                    |
|------------------------|--------|-------|------|-------|-------|-----------------------------------------------|
| UDS subscribe + iter  | ✅      | ✅     | ✅    | ✅     | ❌     | Java 8 stdlib has no Unix Socket API            |
| Originate dual-path    | ✅      | ✅     | ✅    | ⚠️ UDS | ⚠️ HTTP only | C/Java: each picks path matching its target    |
| CallCorrelator         | ✅      | ✅     | ✅    | ❌     | ✅     | C: zero-dep design choice                      |
| WebhookSink (HTTP)     | ✅      | ✅     | ✅    | ❌     | ✅     | C: zero-dep design choice                      |
| SSE transport (remote) | ✅      | ✅     | ✅    | ❌     | ✅     | C: zero-dep design choice                      |
| TLS verify toggle      | ✅      | ✅     | ✅    | ❌     | ✅     | C: no TLS layer; Java: Trust-all trustmanager |
| Non-Unix (Windows)     | ✅      | ✅ stub | stub | stub   | ✅    | UnixStream / AF_UNIX are platform-only; Java no UDS so clean |
| Third-party deps       | 0       | 0      | reqwest+serde_json | 0 | 0 | Rust opts into reqwest for HTTP/TLS           |

### Root-cause legend

- **"❌" = deliberately omitted** (C stays zero-dep `gcc single-file`; Java targets call-center integrations that use nsbc-web HTTP/SSE exclusively — no host filesystem access to UDS).
- **"❌" = platform constraint** (Unix Domain Socket is Unix kernel feature, not available on Windows; Java 16+ added std UnixDomainSocketAddress but Java 8 LTS is still the most common deployment target).

**Python / Go / Rust** cover both local-debug (UDS) and remote-integration (HTTP/SSE) paths.
**C** covers local-debug / embedded same-host (UDS only, zero-dep build story).
**Java** covers the correct production interface for external call-center integrations (HTTP/SSE only, zero third-party deps). All three languages share the nsbc-web HTTP + SSE surface that external integrations actually use.

**Unix Domain Socket**: `AF_UNIX` stream. Text protocol:
- connect → nsbcd sends banner + `nsbc> ` prompt
- send `sub <topics>\n` → server echoes, then `nsbc> `
- server pushes `YYYY-MM-DDTHH:MM:SS.mmm <DOMAIN> <body>\n` lines
- any other command follows `cmd\n` → response lines → `nsbc> `

**nsbc-web HTTP**: `POST {api_base}/ext/v1/originate` JSON `{"call_url":"..."}`.
SSE stream: `GET {api_base}/calls/stream` emits `event: session` JSON payloads.

Frame body is either `k=v k=v ...` (events) or full JSON (CDR). Every SDK
parses both into the same `Frame` struct.
