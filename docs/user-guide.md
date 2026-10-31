# NSBC User Guide

This guide covers installation, configuration, and day-to-day operation of the NSBC SIP platform.

> **See also**:
> · 📘 **[cookbook.md](cookbook.md)** — Operation recipes, control socket command reference,
>   v0.5.0 `cfg.pull_all` reload deep dive, dialplan patterns, outbound trunk monitoring,
>   troubleshooting, and Web↔Core UDS protocol details. Recommended reading for anyone
>   deploying NSBC beyond a single test environment.
> · 🏗️ **[architecture.md](architecture.md)** — Internal design: reactor model, module
>   lifecycle, media bridge pipeline, threading & locking matrix.
> · 📞 **[call-flows.md](call-flows.md)** — SIP & WebRTC call-flows annotated with real
>   function names from the C and Rust sources.

---

## Table of Contents

- [1. System Requirements](#1-system-requirements)
- [2. Building from Source](#2-building-from-source)
  - [2.1 Dependencies](#21-dependencies)
  - [2.2 Configure](#22-configure)
  - [2.3 Compile](#23-compile)
  - [2.4 Build the Web Sidecar](#24-build-the-web-sidecar)
- [3. Installation](#3-installation)
- [4. Directory Layout](#4-directory-layout)
- [5. Configuration](#5-configuration)
  - [5.1 Shared `conf/nsbc.yaml`](#51-shared-confnsbcyaml)
  - [5.2 Core Default `configs/nsbc.yaml`](#52-core-default-configsnsbcyaml)
  - [5.3 Environment Variables](#53-environment-variables)
  - [5.4 Running Without Installation](#54-running-without-installation)
- [6. Starting NSBC](#6-starting-nsbc)
  - [6.1 Startup Order](#61-startup-order)
  - [6.2 systemd Unit (Example)](#62-systemd-unit-example)
  - [6.3 Graceful Restart](#63-graceful-restart)
- [7. Web Management Console](#7-web-management-console)
  - [7.1 Default Login](#71-default-login)
  - [7.2 TLS Certificate Modes](#72-tls-certificate-modes)
  - [7.3 Live Monitoring](#73-live-monitoring)
- [8. Module System](#8-module-system)
  - [8.1 Built-in (Statically Linked)](#81-built-in-statically-linked)
  - [8.2 Dynamic (mod_*.so)](#82-dynamic-mod_so)
  - [8.3 Runtime Enable/Disable](#83-runtime-enabledisable)
  - [8.4 Writing a Custom Module](#84-writing-a-custom-module)
- [9. Dialplan](#9-dialplan)
  - [9.1 Rule Format](#91-rule-format)
  - [9.2 Dialplan Actions](#92-dialplan-actions)
- [10. Observability](#10-observability)
  - [10.1 Prometheus Metrics](#101-prometheus-metrics)
  - [10.2 Control Socket (UDS)](#102-control-socket-uds)
  - [10.3 Event Bus / Subscriptions](#103-event-bus--subscriptions)
- [11. Event Client Examples](#11-event-client-examples)
- [12. Troubleshooting](#12-troubleshooting)
  - [12.1 Core Not Starting](#121-core-not-starting)
  - [12.2 401 Challenges Not Returning](#122-401-challenges-not-returning)
  - [12.3 Media Not Relaying (one-way audio)](#123-media-not-relaying-one-way-audio)
  - [12.4 TLS/WS Not Working](#124-tlsws-not-working)

---

## 1. System Requirements

| Item | Minimum | Recommended |
|------|---------|-------------|
| OS | Linux x86_64, kernel 4.15+ | Linux x86_64, kernel 5.1+ (Ubuntu 22.04 LTS / RHEL 9+) |
| CPU | 2 cores | 4+ cores |
| RAM | 256 MB | 512 MB – 2 GB depending on concurrency |
| Disk | 200 MB | 1 GB (includes recordings and audit logs) |
| GCC | 4.8 (C11) | GCC 8+ or Clang 10+ |
| Rust | — | stable toolchain (for `nsbc-web`) |
| OpenSSL | 1.1.1+ | 3.0+ |
| libsrtp2 | 2.3+ | 2.5+ |
| libpcre2 | 10.30+ | 10.40+ |
| libedit | (optional, for CLI) | present |
| pkg-config | required | required |

Build packages (Debian/Ubuntu):

```bash
sudo apt install build-essential pkg-config libssl-dev libsrtp2-dev \
                 libpcre2-dev libedit-dev sqlite3 libcurl4-openssl-dev
# Rust sidecar (optional):
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

Build packages (RHEL/CentOS 8+):

```bash
sudo dnf install gcc gcc-c++ make pkg-config openssl-devel libsrtp2-devel \
                 pcre2-devel libedit-devel sqlite-devel curl-devel
```

---

## 2. Building from Source

### 2.1 Dependencies

Third-party libraries vendored or auto-downloaded:

| Library | Source | Purpose |
|---------|--------|---------|
| bcg729 | `3rd/bcg729/` (vendored) | G.729 Annex B codec (`mod_codecs.so`) |
| cJSON | `3rd/cjson/` (vendored) | JSON parsing in core |
| libsrtp2 | system package | DTLS-SRTP media encryption |
| OpenSSL | system package | TLS / DTLS / certificate handling |
| libpcre2 | system package | Regex dialplan matching |

### 2.2 Configure

The configure script reads `conf/modules.conf`, resolves module topological dependencies, and generates:

- `.config/config.mk` — compile variables
- `src/core/builtin_modules.gen.inc` — static module registration table
- `build/mod.load` — dynamic module manifest

```bash
./configure
# Optional: select a custom module set
./configure --with-modules=billing,dialplan,recording,endpoints
./configure --without-modules=asr_tts,presence
```

### 2.3 Compile

```bash
make -j$(nproc)        # build nsbcd + dynamic .so modules
make check             # unit tests (requires test sources)
sudo make install      # install to /usr/local
```

Build with sanitizers (for debugging):

```bash
make SANITIZE=asan -j$(nproc)     # AddressSanitizer
make SANITIZE=tsan -j$(nproc)     # ThreadSanitizer
make SANITIZE=ubsan -j$(nproc)    # UndefinedBehaviorSanitizer
```

Key compile-time switches (`NSBC_WITH_*`, auto-detected, override in `./configure`):

| Switch | Default | Purpose |
|--------|---------|---------|
| `NSBC_WITH_OPENSSL` | 1 | SIP TLS + DTLS-SRTP (hard requirement — always on) |
| `NSBC_WITH_SRTP` | 1 | SRTP media encryption (paired with OpenSSL — always on) |
| `NSBC_WITH_BCG729` | 1 | G.729 Annex B codec |
| `NSBC_WITH_DLOPEN` | 1 | Dynamic module loading via `dlopen()` |
| `NSBC_WITH_CURL` | 0 | Real ASR/TTS in `mod_ai_dialog` (Whisper / ElevenLabs) |
| `NSBC_WITH_SQLITE` | 0 | **Architecture constraint** — C core never touches DB. SQLite lives only in the Rust sidecar. |

### 2.4 Build the Web Sidecar

```bash
cd web
cargo build --release
# Output: target/release/nsbc-web
# Optional ACME feature (public TLS):
cargo build --features acme --release
```

The Rust sidecar (`nsbc-web`) is a separate process that provides the management console and stores configuration in SQLite/Postgres/MySQL. It communicates with `nsbcd` over Unix domain sockets (`/var/run/nsbc/control.sock` for commands, `/var/run/nsbc/data.sock` for real-time events).

---

## 3. Installation

```bash
sudo make install
# Installs:
#   /usr/local/sbin/nsbcd           — SIP daemon
#   /usr/local/sbin/nsbc_cli        — interactive CLI (optional)
#   /usr/local/lib/nsbc/mod_*.so    — dynamic modules
#   /usr/local/etc/nsbc/nsbc.yaml   — default configuration
```

Manual deployment (no make install):

```bash
# Typical layout
/opt/nsbc/
├── nsbcd
├── nsbc_cli
├── conf/
│   └── nsbc.yaml
├── web/
│   ├── nsbc-web
│   └── static/            # SPA assets
├── tls/                   # cert.pem + key.pem (auto-generated by nsbc-web)
├── logs/                  # nsbcd.log + audit.jsonl
├── run/                   # /var/run/nsbc/ symlinked here or actual UDS dir
├── recordings/
└── data/snapshots/
```

Create runtime user (recommended):

```bash
sudo useradd -r -s /usr/sbin/nologin nsbc
sudo mkdir -p /var/run/nsbc /var/log/nsbc /var/lib/nsbc
sudo chown -R nsbc:nsbc /var/run/nsbc /var/log/nsbc /var/lib/nsbc
```

---

## 4. Directory Layout

| Directory | Contents | Notes |
|-----------|----------|-------|
| `src/` | C source tree | All `.c` / `.h` for the core |
| `src/core/` | Core daemon | `main.c`, `reactor.c`, `module.c`, `cfg.c`, `signaling.c`, `event_bus.c`, `worker.c`, `cfg_crypto.c` |
| `src/sip/` | SIP protocol stack | `sip_txn.c`, `sip_transport.c`, `sip_dialog.c`, `b2bua.c`, `sip_build.c`, `sip_event.c`, `stir_shaken.c`, etc. |
| `src/media/` | Media pipeline | `bridge/`, `crypto/`, `codec/`, `dsp/`, `rtp/`, `nat/`, `play/`, `rec/`, `mix/`, `video/` |
| `src/mod/` | Optional modules | Each subdir → one `mod_*.so` (or static-linked via configure) |
| `src/parse/` | Parsers | `sip/`, `sdp/`, `rtp/`, `xml/` |
| `src/oam/` | Operations & Maintenance | Control socket, ESL server, observability, reload commands |
| `src/registrar/` | Registration | `registrar.c`, `location_db.c`, `reg_enhance.c` |
| `src/net/` | Networking | Socket pool, TCP I/O group, TLS, DNS cache, WebSocket listener |
| `src/include/` | Public headers | One header per major subsystem |
| `src/db/` | DBAL | **Architecture note**: C core never links against DB. SQLite is only used by the Rust sidecar. |
| `src/ha/` | High Availability | Active/standby replication, health checks |
| `src/mem/` | Memory manager | Arena, slab, RCU deferral, ring buffer, pool |
| `src/base/` | Common primitives | Clock, hash, ID, logging, timer wheel, trace ID |
| `web/src/` | Rust sidecar sources | Axum + Tokio HTTP API + embedded SPA |
| `web/static/` | SPA assets | Vue 3 + Element Plus + Tailwind, bundled in `nsbc-web` |
| `conf/` | Runtime configuration | `nsbc.yaml` (shared between nsbcd and nsbc-web), `modules.conf` |
| `configs/` | Default core config | Reference configuration with all keys documented |
| `examples/` | Code samples | Event client in C, Go, Python, Rust |
| `3rd/` | Vendored libraries | bcg729, cJSON |
| `build/` | Build artifacts | Compiled objects, dynamic modules, config.mk |
| `docs/` | Documentation | Architecture, user guide |

---

## 5. Configuration

### 5.1 Shared `conf/nsbc.yaml`

This is the primary runtime configuration. The file is placed at `<deploy-root>/conf/nsbc.yaml`. Both `nsbcd` and `nsbc-web` read it but ignore sections that don't belong to them:

- **nsbcd reads**: `log.*`, `net.*`, `sip.*`, `registrar.*`, `oam.*`, `ha.*`, `hep.*`, `mem.*`, `rtp.*`, `media.*`, `core.*`, `dialplan.*`, `recording.*`, `system.*`, `modules.*`
- **nsbc-web reads**: `web.*`, `auth.*`, `db.*`, `audit.*`, `core.*`

All relative paths resolve against the directory containing the YAML file (the `conf/` directory). So `"../tls/cert.pem"` in `conf/nsbc.yaml` resolves to `<deploy-root>/tls/cert.pem`. This means the entire deployment tree can be moved without editing the config.

**Search order for `nsbcd`:**
1. Explicit `-c <path>` argument
2. `<binary-dir>/conf/nsbc.yaml`
3. `/etc/nsbc/nsbc.yaml`

**Search order for `nsbc-web`:**
1. Explicit `-c <path>` argument
2. `<binary-dir>/../conf/nsbc.yaml`
3. `<binary-dir>/conf/nsbc.yaml`
4. `./conf/nsbc.yaml`
5. `/etc/nsbc/nsbc.yaml`

### 5.2 Core Default `configs/nsbc.yaml`

This is a comprehensive reference showing every configuration key with comments. Useful as a template when you want to understand all available options. **It is not normally copied to production** — the `conf/nsbc.yaml` at your deployment root is what actually runs.

### 5.3 Environment Variables

| Variable | Scope | Purpose |
|----------|-------|---------|
| `NSBC_MASTER_KEY` | nsbcd | Master key for decrypting `enc:` prefixed secrets |
| `NSBC_DEFAULT_ADMIN_PW` | nsbc-web | Override default admin password (DB empty case) |
| `NSBC_RESET_ADMIN` | nsbc-web | Set to `1` to reset admin password to `NSBC_DEFAULT_ADMIN_PW` or hardcoded default |
| `NSBC_DB_DSN` | nsbc-web | Override `db.dsn` in YAML |
| `NSBC_NO_DB_MODULE=1` | compile-time | Architecture — never link DB into C core |

### 5.4 Running Without Installation

```bash
# Build first (see Section 2)
cd /path/to/nsbc

# Run the Rust web sidecar first (it generates TLS certificates if configured)
cd web && ./target/release/nsbc-web &
sleep 2

# Run the C SIP daemon
cd .. && ./nsbcd -c conf/nsbc.yaml
```

---

## 6. Starting NSBC

### 6.1 Startup Order

1. **Rust sidecar (`nsbc-web`) first** — binds HTTPS on port 443 (or 8443 with `-p` override), auto-generates self-signed TLS cert, creates SQLite DB, and opens `/var/run/nsbc/control.sock` + `data.sock`.
2. **C daemon (`nsbcd`) second** — connects to both UDS sockets, pulls **all config tables** via `cfg.pull_all` (v0.5.0 unified entry), then starts SIP transports and media bridge.

> ⚠️ If you skip this order — start `nsbcd` when `nsbc-web` isn't up yet — `nsbcd` will retry the DB pull 10×500ms and proceed with only YAML-configured trunks. **All non-YAML config (trunks, dialplan rules, SIP profiles from the web DB) will be missing** until you run `echo "reload" | nc -U /var/run/nsbc/control.sock` or `kill -HUP $(pidof nsbcd)` after `nsbc-web` is up.

Both processes share the same `conf/nsbc.yaml`. Both `control.sock "reload"` and SIGHUP reload YAML **and** pull all DB tables — they behave identically (v0.5.0).

### 6.2 systemd Unit (Example)

**`/etc/systemd/system/nsbc-web.service`:**

```ini
[Unit]
Description=NSBC Web Management Console
After=network.target

[Service]
Type=simple
User=nsbc
Group=nsbc
ExecStart=/opt/nsbc/web/nsbc-web -c /opt/nsbc/conf/nsbc.yaml
WorkingDirectory=/opt/nsbc
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
```

**`/etc/systemd/system/nsbcd.service`:**

```ini
[Unit]
Description=NSBC SIP Daemon
After=network.target nsbc-web.service
Requires=nsbc-web.service

[Service]
Type=simple
User=nsbc
Group=nsbc
ExecStart=/opt/nsbc/nsbcd -c /opt/nsbc/conf/nsbc.yaml
WorkingDirectory=/opt/nsbc
Restart=on-failure
RestartSec=3
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl start nsbc-web
sudo systemctl start nsbcd
sudo systemctl enable nsbc-web nsbcd
```

### 6.3 Graceful Restart

Send `SIGHUP` to the nsbcd process to trigger a config reload:

```bash
kill -HUP $(pidof nsbcd)
# Or via CLI:
nsbc_cli control config.reload
```

The following keys support **hot reload** (marked `[hot]` in the reference config):

- `log.level`
- `net.rcvbuf_kb`, `net.sndbuf_kb`, `net.tcp_conn_max`, `net.dns_ttl_s`
- `sip.t1_ms`, `sip.t2_ms`, `sip.t4_ms`, `sip.timer_d_ms`, `sip.udp_cl_strict`, `sip.user_agent`
- `rtp.dscp`, `rtp.ssrc_grace_ms`, `rtp.rtcp_interval_ms`
- `media.default_mode`, `media.allow_bypass`, `media.trust_private`
- `registrar.min_expires`, `registrar.default_expires`, `registrar.max_expires`, `registrar.max_contacts`, `registrar.rps_*`
- `oam.metrics_enabled`, `oam.metrics_bind`
- `ha.heartbeat_ms`, `ha.failover_ms`

Non-hot keys require a full process restart.

---

## 7. Web Management Console

### 7.1 Default Login

- Username: `admin`
- Password: **random** — set via `NSBC_DEFAULT_ADMIN_PW` env or hardcoded fallback `Admin123` when the users table is empty
- Realm: matches your tenant (or `nsbc.local` default)

Immediately after first login, **change the admin password** from the console (Security → Users) or clear the DB and set `NSBC_RESET_ADMIN=1`.

### 7.2 TLS Certificate Modes

Three certificate sources supported (auto-selected by priority):

| Mode | How to enable | Use case |
|------|---------------|----------|
| **Pre-existing** | Point `cert_path` and `key_path` to your files | Bring your own cert (Let's Encrypt manual, internal CA) |
| **Self-signed (auto-generated)** | `auto_generate: true` | Internal networks, testing — certs rotated hourly |
| **ACME Let's Encrypt** | `public: true` + domain | Public deployments — needs DNS A record to this host |

```yaml
web:
  tls:
    enabled: true
    cert_path: "../tls/cert.pem"
    key_path:  "../tls/key.pem"
    # Self-signed (internal / lab)
    auto_generate: true
    validity_days: 2
    # Or public ACME (production)
    public: true
    domain: "sip.example.com"
    acme_email: "ops@example.com"
```

### 7.3 Live Monitoring

The web console provides real-time panels:

- **Overview** — CPS, active calls, failed calls, registrations
- **Active Calls** — per-leg state, codec, RTP stats, MOS score
- **Call Records** — CDRs with call/download recordings
- **Registrations** — extension online/offline, remaining expires
- **Security Center** — dynamic ban list, fence rules, global config
- **Metrics** — Prometheus scrape endpoint display

Data flows from nsbcd → nsbc-web over `data.sock` using a broadcast channel. Default polling interval 5 seconds.

---

## 8. Module System

NSBC uses a two-level module system, inspired by FreeSWITCH:

### 8.1 Built-in (Statically Linked)

Modules in the static table are compiled directly into `nsbcd`. The configure script determines which are required based on call-graph analysis:

- **REQUIRED** (cannot be disabled): `signaling`, `dbal`, `oam`, `endpoints`, `agent_pool`, `queue`
- **Hard-statically linked** (called directly by `b2bua.c` — linker would fail if missing): `billing`, `dialplans`, `outbound_reg`, `recording`, `applications`, `ha`, `media_bug`, `security`

### 8.2 Dynamic (mod_*.so)

Dynamically loaded modules are `dlopen()`ed at startup from `<prefix>/lib/nsbc/` or the path specified by `modules.conf`. The configure script builds these as `.so`:

`ai_dialog`, `asr_tts`, `av`, `cachedb`, `codecs`, `event_handlers`, `formats`, `hello`, `playback`, `presence`

Dynamic modules can be individually selected or excluded:

```bash
./configure --without-modules=ai_dialog,asr_tts,hello
```

### 8.3 Runtime Enable/Disable

`conf/nsbc.yaml` supports two runtime controls (hot-reload after `SIGHUP`):

```yaml
modules:
  # enable:        # whitelist — only listed modules are loaded (REQUIRED forced)
  #   - signaling
  #   - oam
  disable:          # blacklist — these modules are skipped
    - hello
    - asr_tts
```

If both `enable` and `disable` are present, `enable` takes priority.

### 8.4 Writing a Custom Module

1. Create `src/mod/my_module/my_module.c`
2. Implement the standard descriptor:

```c
#include "module.h"

static int my_start(nsbc_modmgr_t *m);
static int my_stop(nsbc_modmgr_t *m);
static int my_reload(nsbc_modmgr_t *m);

NSBC_MOD_DEFINE(my_module,
    .version  = "1.0.0",
    .init_level = MOD_L_FINAL,
    .deps     = NULL,          // no dependencies
    .start    = my_start,
    .stop     = my_stop,
    .reload   = my_reload,
);
```

3. Run `./configure` — the module is picked up automatically.
4. `make` builds it as both static and dynamic.

---

## 9. Dialplan

NSBC has a lightweight YAML-based dialplan plus a structured JSON action builder in the web console. Dialplan rules match inbound calls (called number pattern + optional caller pattern) and execute a chain of routing / media / recording / number-transform actions.

> 📘 **Full dialplan patterns** (literal `sip:host:port`, trunk routing, office-hours routing, 911 bypass, strip/add prefix, ring groups) with YAML examples → see [cookbook.md §5](cookbook.md#5-dialplan-cookbook).

### 9.1 Rule Format

```yaml
dialplan:
  rules: "alice:0:alice|bob:0:bob|^[0-9]+$:0:$0"
```

Compact format: `called_pattern:priority:target` — pipe-separated. Alternatively, use the web console → Dialplan Rules for structured editing.

Valid target prefixes (v0.5.0):

| Prefix | Layer | Example | Notes |
|--------|-------|---------|-------|
| *(empty / pure digits)* | EXT (location DB) | `1001`, `2001` | Routes to registered extension |
| `extension:` | EXT | `extension:1001` | Explicit |
| `gateway:` | OUT | `gateway:acme` | Gateway from `outbound.gateways` |
| `trunk:` | OUT | `trunk:sip_terminator` | Trunk from `outbound.trunks` or DB |
| `sip:` **(NEW v0.5.0)** | OUT | `sip:10.0.0.25:5080` | Literal URI — skips route_table, direct INVITE. No trunk needed |
| `sips:` **(NEW v0.5.0)** | OUT | `sips:example.com:5061` | TLS variant, default port 5061 |

> `sip:` / `sips:` literals are ideal for quick test deployments, temp failover (trunk down → cutover), and zero-trunk routing. IPv6 `sip:[::1]:5080` is supported. See [cookbook.md §5.2](cookbook.md#52-literal-siphostport-target-v050) for full examples.

### 9.2 Dialplan Actions

Common actions configured per rule in the web UI:

| Action | Description |
|--------|-------------|
| Route extension | Forward to an internal extension (`2001`) |
| Route trunk | Forward to an external SIP trunk (`trunk = ACME-IMS`) |
| Route group | Forward to a gateway group with strategy (`least_busy`) |
| Play IVR | Route to an IVR menu |
| Start recording | Begin recording (`mode=mixed`, `format=wav`) |
| Number strip | Strip N leading digits before routing |
| Called override | Replace called number with a captured group |
| SIP header rewrite | Set `sip_from_display`, `sip_from_uri`, R-URI, Contact params |
| IMS PAI/PPID/PCPI injection | Auto-build IMS identity headers |

Time-of-day routing: combine a rule with a time group (`Time Strategy` menu → create a group with M-F 09:00–18:00, then reference it from the rule).

---

## 10. Observability

### 10.1 Prometheus Metrics

nsbcd exposes metrics on `127.0.0.1:9100` by default:

```bash
curl http://127.0.0.1:9100/metrics | head -40
```

Key metrics:

| Metric | Description |
|--------|-------------|
| `nsbc_active_calls` | Current concurrent calls |
| `nsbc_calls_total` | Total call attempts |
| `nsbc_calls_failed_total` | Failed calls |
| `nsbc_registered_extensions` | Online registrations |
| `nsbc_rtp_relayed_packets_total` | RTP packets bridged |
| `nsbc_rtp_relayed_bytes_total` | RTP bytes bridged |
| `nsbc_txn_active` | Active SIP transactions |
| `nsbc_event_bus_queue_depth` | Event bus pending queue |
| `nsbc_mem_arena_used_bytes` | Per-memory-domain arena usage |
| `nsbc_recording_active` | Active recordings |
| `nsbc_ha_role` | HA role (0=standby, 1=active) |

### 10.2 Control Socket (UDS)

Run-time control via Unix domain socket (default `/var/run/nsbc/control.sock`):

```bash
# Method A: netcat (recommended for scripts)
echo "help"         | nc -U /var/run/nsbc/control.sock
echo "version"      | nc -U /var/run/nsbc/control.sock
echo "status"       | nc -U /var/run/nsbc/control.sock
echo "calls list"   | nc -U /var/run/nsbc/control.sock
echo "reload"       | nc -U /var/run/nsbc/control.sock   # v0.5.0: pulls DB too
echo "trunks.list"  | nc -U /var/run/nsbc/control.sock   # NEW v0.5.0
echo "dialplan.show"| nc -U /var/run/nsbc/control.sock   # NEW v0.5.0

# Method B: bundled CLI
nsbcli help           # shows all commands
nsbcli calls hangup <call-id>
nsbcli metrics        # dump internal metric registry

# Method C: socat for interactive mode
socat - UNIX-CONNECT:/var/run/nsbc/control.sock
```

> 📘 **Complete command reference** (20+ commands with level/permission matrix, including all v0.5.0 diagnostic commands) → see [cookbook.md §7](cookbook.md#7-control-socket-command-reference).

### 10.3 Event Bus / Subscriptions

The C core publishes structured events on the event bus. Clients subscribe via ESL or data.sock.

Core event types:

| Event | Description |
|-------|-------------|
| `SESSION_CREATE` | New call leg created |
| `SESSION_ANSWER` | Call answered |
| `SESSION_END` | Call torn down |
| `REGISTER_SUCCESS` | Extension registered |
| `REGISTER_EXPIRE` | Registration expired |
| `BAN_ADD` | IP blocked |
| `BAN_REMOVE` | IP unblocked |
| `RECORDING_START` | Recording started |
| `RECORDING_STOP` | Recording stopped |
| `CFG_CHANGE` | Configuration hot-reloaded |

See `src/include/event_bus.h` for the full list.

---

## 11. Event Client Examples

Ready-to-run event client samples are in `examples/event_client/`:

| Language | File | Build/Run |
|----------|------|-----------|
| C | `c/event_client.c` | `gcc event_client.c -o event_client && ./event_client` |
| Go | `go/main.go` | `cd go && go run main.go` |
| Python | `python/event_client.py` | `python3 event_client.py` |
| Rust | `rust/src/main.rs` | `cd rust && cargo run` |

All clients connect to the UDS control socket and subscribe to `SESSION_CREATE`, `SESSION_ANSWER`, `SESSION_END` events. They print to stdout in real time.

```text
[SUB] EVENT SESSION_CREATE call=abc123 leg=A from=sip:alice@nsbc.local to=sip:bob@nsbc.local
[SUB] EVENT SESSION_ANSWER call=abc123 leg=B codec=PCMU/8000
[SUB] EVENT SESSION_END   call=abc123 leg=A duration=42s cause=NORMAL_CLEARING
```

---

## 12. Troubleshooting

> 📘 **More troubleshooting scenarios** (INVITE no route, trunk sinks empty after reload, cfg.pull_all timeouts) with full shell checklists → see [cookbook.md §10](cookbook.md#10-troubleshooting).

### 12.1 Core Not Starting

```bash
# Check logs
tail -f logs/nsbcd.log

# Common causes:
# 1. Port conflict (5060 already bound) — check: ss -tulpn | grep 5060
# 2. Permission denied on /var/run/nsbc or /var/log/nsbc
# 3. Missing TLS certs when tls_enabled=true
# 4. libsrtp2/OpenSSL not installed — check ldd:
ldd nsbcd | grep "not found"

# Validate config syntax
./nsbcd -c conf/nsbc.yaml --dry-run   # if supported, else look at first log lines
```

### 12.2 401 Challenges Not Returning

Check that the registrar module is enabled and `authz` module is not blocking:

```bash
# Test with a register attempt
sipsak -s sip:test@nsbc.local -M REGISTER

# Expected:
# 1. REGISTER → 401 with WWW-Authenticate header
# 2. REGISTER + Authorization → 200 OK
```

> **v0.5.0 behavior change**: If your SIP profile has `auth_calls: 0` (external profile does not accept registrations), REGISTER now returns **403 Forbidden** immediately instead of silently dropping (which caused a 32s client timeout). This is expected — see `profiles.list` to confirm auth_calls value. See also [cookbook.md §10.2](cookbook.md#102-register-returns-403).

Also verify `registrar.digest_secret` is consistent and `security.blacklist.reload` has no catch-all deny.

### 12.3 Media Not Relaying (one-way audio)

```bash
# Check media mode in config
grep media configs/nsbc.yaml
# default_mode should be 'relay' (bypass skips SBC relay entirely)
# trust_private should be true for private-peer calls

# Verify RTP port range available
ss -ulpn | grep -E "10000|20000" | head
# Nothing should be bound in 10000-59999

# Check NAT/external IP configuration
grep extern_ip configs/nsbc.yaml

# Monitor active call RTP bridge
nsbc_cli show calls      # look for ACodec/BCodec, RTP IP columns
nsbc_cli metrics         # watch nsb_rtp_relayed_packets_total
```

### 12.4 TLS/WS Not Working

```bash
# Verify TLS cert paths exist and PEM-valid
openssl x509 -in tls/cert.pem -text -noout | head
openssl rsa  -in tls/key.pem  -check -noout

# nsbcd start-up log should show [tls] cert=... key=... — inspect for "失败" failures

# Test with OpenSSL client
openssl s_client -connect 127.0.0.1:5061 </dev/null

# WebSocket (WSS on port 5062)
openssl s_client -connect 127.0.0.1:5062 -alpn sip.wss </dev/null
```

---

## License

Apache 2.0 — see [LICENSE](../LICENSE).
