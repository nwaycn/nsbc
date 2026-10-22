# NSBC Event/CDR Realtime Subscription Client (Multi-language)

This directory contains **four language implementations** of the NSBC control socket
(Unix Domain Socket) realtime event/CDR subscription client; all four behave
identically:

| Language | Directory | Dependencies | Compile / Run |
|---|---|---|---|
| **Python** | `python/` | stdlib only | `python3 event_client.py` |
| **Go** | `go/` | stdlib only | `go run main.go` or `go build -o event_client .` |
| **Rust** | `rust/` | stdlib only | `cargo run --release --` |
| **C** | `c/` | POSIX only | `gcc -O2 -Wall -o event_client event_client.c && ./event_client` |

Default socket path is `/tmp/nsbc-e2e/control.sock`; override with `--topics` or a
positional argument.

---

## Protocol (server frame format)

All frames pushed by nsbcd through `obs_sub` are **single-line text terminated by
`\n`** — clients just `readline`.

### Event frames (`sub events`)

```
YYYY-MM-DDTHH:MM:SS.mmm CALL_INCOMING  call_id=... orig=1001 to=1002 src_ip=10.0.0.50 trunk=
YYYY-MM-DDTHH:MM:SS.mmm CALL_ANSWERED  call_id=... orig=1001 to=1002 trunk=
YYYY-MM-DDTHH:MM:SS.mmm CALL_END       call_id=... orig=1001 to=1002 trunk= disp=ANSWERED cause=200 duration_s=120 billsec=116
YYYY-MM-DDTHH:MM:SS.mmm REGISTER       ...
```

Format: `ISO8601-timestamp  DOMAIN  k=v k=v ...\n`

### CDR frames (`sub cdr`)

```
YYYY-MM-DDTHH:MM:SS.mmm CDR {"call_id":"...","from":"1001","to":"1002","phase":"end",
                             "disposition":"ANSWERED","duration_s":120,"billsec":116,...}
```

Format: `ISO8601-timestamp CDR <JSON>\n`

The JSON contract matches the Web REST response exactly (see `cdr_event_to_json` in
[signaling.c](../../../src/core/signaling.c#L892)).

### Log / Metrics frames (`sub log` / `sub metrics`)

- `log`: direct passthrough of the log ring buffer line
- `metrics`: periodic Prometheus exposition (default 5s)

### Command protocol

After the client connects, nsbcd sends a banner + prompt:

```
NSBC 2.6.0 control interface (try 'help')
nsbc> 
```

Commands are terminated by `\n`; the prompt `nsbc> ` indicates the command has finished.

Realtime subscribe commands:

| Command | Description |
|---|---|
| `sub <topics>` | Subscribe to topics (comma-separated: events,cdr,log,metrics) |
| `unsub [topics]` | Unsubscribe (no arg = unsubscribe all) |
| `sub.stat` | Inspect subscription state for this connection |

Common pull commands (available in `pull-only` mode):

| Command | Description |
|---|---|
| `events [N]` | Pull the most recent N events (default 20) |
| `show calls` | Active call table |
| `cdr.recent [N]` | Pull the most recent N completed CDRs (default 500) |
| `show cdr` | CDR sink aggregate counters |

---

## Server-side performance design

| Item | Strategy |
|---|---|
| **Zero back-pressure on call thread** | `obs_sub_publish` memcpy frames into each subscriber's independent frame ring (`OBS_SUB_RING=16` per subscriber) inside the lock, drop oldest on full — **never block** |
| **Slow consumer** | Per-connection independent frame ring; `obs_sub_publish` does an immediate `try flush` (non-blocking `send`, `EAGAIN` → frame ring) |
| **Frame truncation** | Single frame capped at `OBS_FRAME_MAX=2048` bytes — avoids huge JSON blowing buffers |
| **Subscriber cap** | `OBS_SUBSCRIBER_MAX=256` concurrent subscribers |
| **Pull mode** | `obs_event` also writes a ring buffer (`OBS_EVENT_RING=1024`) to support `events N` pull |

### Lock order (deadlock-free)

```
call_lock (b2bua shard)
  → obs_event: g_ev.lock → obs_sub_hub.lock
s->lock (cdr sink) — unlock before publish —
  → obs_sub_hub.lock
```

The send_fn callback only does `send()`, never recurses into b2b/cdr code — lock order
is one-directional and safe.

---

## Running steps

```bash
# 1. start nsbcd
nsbcd -c /tmp/nsbc-e2e/nsbc.yaml

# 2. pick any client (on the same machine or forward UDS over TCP across hosts)
cd examples/event_client/python   && python3 event_client.py
cd examples/event_client/go       && go run main.go
cd examples/event_client/rust     && cargo run --release --
cd examples/event_client/c        && gcc -O2 -o event_client event_client.c && ./event_client

# 3. register a soft phone with nsbcd and place a call
#    alice (10.0.0.50) → dial bob
#    client should see CALL_INCOMING → CALL_ANSWERED → CDR → CALL_END
```

---

## Comparison with FreeSWITCH event_socket

| Item | FreeSWITCH | NSBC |
|---|---|---|
| Transport | TCP (TLS optional) | **Unix Domain Socket** (P0); TCP later |
| Auth | `auth <password>` | File permissions 0600 (root-only socket); TCP later adds token |
| Subscribe command | `event plain ALL` | `sub events,cdr` |
| Push frame format | `Content-Type: ... \n\nBODY` | Single-line `YYYY..SS.mmm DOMAIN body\n` |
| Slow-consumer policy | Back-pressure (blocks call thread) | **Drop-oldest frame ring, never back-pressure** |
| Event set | CHANNEL_CREATE/BEGIN/ANSWER/HANGUP/HANGUP_COMPLETE + 200+ | **Minimal set** CALL_INCOMING/ANSWERED/END + REGISTER |
| CDR | Separate CDR socket or `cdr_csv` | **Same channel** `sub cdr`, with ISO8601 timestamp header |

NSBC intentionally keeps the event set minimal — 99% of real operations only need the
three call-lifecycle events + final CDR record.
