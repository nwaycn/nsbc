# NSBC Cookbook — v0.5.0-beta

> **版本**: `v0.5.0-beta` (commit `28f0136`)
> **范围**: 从 `nsbc-web`（Rust sidecar）到 `nsbcd`（C daemon）的完整操作手册
> **读者**: 运维、SIP 开发、二次开发者
> **前置阅读**: [architecture.md](architecture.md) · [user-guide.md](user-guide.md) · [README.md](../README.md)

---

## Table of Contents

1. [Architecture at a Glance](#1-architecture-at-a-glance)
2. [Prerequisites & Build](#2-prerequisites--build)
3. [First Run — 10 Minutes to a Working Call](#3-first-run--10-minutes-to-a-working-call)
4. [Web ↔ Core Communication Deep Dive](#4-web--core-communication-deep-dive)
   - [4.1 Two Unix Domain Sockets](#41-two-unix-domain-sockets)
   - [4.2 UDS Frame Protocol](#42-uds-frame-protocol)
   - [4.3 Config Sync: pull_all + cfg_sync](#43-config-sync-pull_all--cfg_sync)
5. [Dialplan Cookbook](#5-dialplan-cookbook)
   - [5.1 Syntax & Rule Format](#51-syntax--rule-format)
   - [5.2 Literal `sip:host:port` Target (v0.5.0)](#52-literal-siphostport-target-v050)
   - [5.3 Trunk Routing](#53-trunk-routing)
   - [5.4 Route: Dial-Office Hours / After Hours](#54-route-dial-office-hours--after-hours)
   - [5.5 Emergency 911 Bypass](#55-emergency-911-bypass)
   - [5.6 Strip & Add Prefix](#56-strip--add-prefix)
   - [5.7 Ring Groups](#57-ring-groups)
6. [Outbound Trunk Registration](#6-outbound-trunk-registration)
   - [6.1 Via YAML (`outbound.register_trunks`)](#61-via-yaml-outboundregister_trunks)
   - [6.2 Via nsbc-web DB (Recommended)](#62-via-nsbc-web-db-recommended)
   - [6.3 Monitoring Trunk Status](#63-monitoring-trunk-status)
7. [Control Socket Command Reference](#7-control-socket-command-reference)
   - [7.1 Connecting](#71-connecting)
   - [7.2 All Commands](#72-all-commands)
   - [7.3 New in v0.5.0: Diagnostic Commands](#73-new-in-v050-diagnostic-commands)
8. [Module System](#8-module-system)
9. [Observability](#9-observability)
   - [9.1 Prometheus Metrics](#91-prometheus-metrics)
   - [9.2 Structured Logging](#92-structured-logging)
   - [9.3 HEPv3 Mirroring](#93-hepv3-mirroring)
10. [Troubleshooting](#10-troubleshooting)
    - [10.1 nsbcd Starts but No Invite Routes](#101-nsbcd-starts-but-no-invite-routes)
    - [10.2 REGISTER Returns 403](#102-register-returns-403)
    - [10.3 trunk Sinks Are Empty After Reload](#103-trunk-sinks-are-empty-after-reload)
    - [10.4 Media One-Way Audio](#104-media-one-way-audio)
    - [10.5 control.sock `cfg.pull_all` Timeouts](#105-controlsock-cfgpull_all-timeouts)

---

## 1. Architecture at a Glance

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Browser SPA (Vue 3)                         │
│                       https://host:443                              │
└────────────────────────┬────────────────────────────────────────────┘
                         │ HTTPS / JWT
┌────────────────────────▼────────────────────────────────────────────┐
│                   nsbc-web (Rust sidecar)                           │
│  Tokio + Axum · SQLite/Postgres · UDS client · SPA embed            │
│                                                                     │
│   ┌──────────────┐   ┌───────────────┐   ┌────────────────────┐    │
│   │ SQLite / PG  │   │ control.sock  │   │    data.sock       │    │
│   │ trunks table │   │ CMD_REQ →     │   │ ← PUSH events     │    │
│   │ dialplan     │   │ nsbcd CMD_RSP │   │ CDR · REGISTER    │    │
│   └──────┬───────┘   └───────┬───────┘   └──────────▲─────────┘    │
│          │                   │                       │              │
│          └───────────────────┼───────────────────────┘              │
│                              │ UDS (AF_UNIX, /var/run/nsbc/)        │
└──────────────────────────────┼──────────────────────────────────────┘
                               │
┌──────────────────────────────▼──────────────────────────────────────┐
│                        nsbcd (C daemon)                             │
│  Built on FreeSWITCH-inspired module manager · reactor thread pool │
│                                                                     │
│  ┌─ cfg.c (YAML) ───────────────────────────────────────────────┐  │
│  │  cfg.pull_all ← 启动/reload 时从 web 同步 DB 所有配置表       │  │
│  │  cfg_sync_after_reload() → 调各模块 _reload_json()             │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌─ SIP Signaling ──┐  ┌─ Registrar ──┐  ┌─ Media Bridge ──┐      │
│  │  B2BUA core      │  │ location_db  │  │ DTLS-SRTP       │      │
│  │  dialplan route  │  │ HA1 auth     │  │ jitter buffer   │      │
│  │  txn state mach  │  │ rate limit   │  │ SDP rewrite     │      │
│  └───────────────────┘  └──────────────┘  └─────────────────┘      │
│                                                                     │
│  ┌─ Route Table (RCU) ──────────────────────────────────────────┐   │
│  │  trunk_table → rt_trunk_list[] · RTU atomic swap on reload   │   │
│  │  v0.5.0: started via cfg_sync_on_startup + cfg_sync_after_   │   │
│  │          reload from both control.sock "reload" and SIGHUP   │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

**Critical rule**: `nsbc-web` must start FIRST. It creates:
- `/var/run/nsbc/data.sock` and `/var/run/nsbc/control.sock`
- SQLite DB with default tables (`trunks`, `dialplan_rules`, `sip_profiles`, `endpoints`, ...)
- Self-signed TLS certificate for the web console

Then `nsbcd` connects to both UDS sockets and pulls all config tables via `cfg.pull_all`.

---

## 2. Prerequisites & Build

### Dependencies (Debian 12 / Ubuntu 24.04)

```bash
sudo apt-get update && sudo apt-get install -y \
    build-essential pkg-config git cmake \
    libssl-dev libsrtp2-dev libpcre2-dev libedit-dev \
    libsqlite3-dev libpq-dev libcurl4-openssl-dev libopus-dev \
    sipp

# Rust toolchain (nsbc-web only)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source $HOME/.cargo/env
```

### Clone & Build

```bash
git clone https://github.com/nsbc/nsbc.git && cd nsbc
# China mirror: git clone https://gitee.com/nwaycn/nsbc.git

chmod +x configure
./configure                           # auto-scans src/, reads conf/modules.conf
make -j$(nproc)                      # ~3-5 min on 4 cores

# Optional: ./configure --prefix=/opt/nsbc --without-modules=ai_dialog,hello
# Optional: ./configure --reactor N   # force N reactor threads (default=1)
```

### Build nsbc-web

```bash
cd web
make build-acme                      # builds release binary + copies static/ SPA
cd ..
# Result: ./nsbc-web binary + ./tls/ self-signed cert
```

### Layout After Build

```
nsbc/
├── nsbcd                      # C daemon (single binary)
├── nsbc-web                   # Rust sidecar
├── conf/nsbc.yaml             # active YAML config
├── configs/nsbc.yaml          # built-in default (template)
├── tls/                       # nsbc-web TLS certs (auto-generated)
├── build/mod/*.so             # dynamically loaded modules
└── logs/nsbcd.log             # default log file
```

---

## 3. First Run — 10 Minutes to a Working Call

```bash
# Step 1: nsbc-web FIRST (auto-creates DB + TLS + UDS sockets)
sudo mkdir -p /var/run/nsbc
sudo nohup ./nsbc-web > logs/web.log 2>&1 &
sleep 3

# Step 2: nsbcd SECOND (connects to UDS, pulls config via cfg.pull_all)
sudo nohup ./nsbcd -c conf/nsbc.yaml > logs/nsbcd.log 2>&1 &
sleep 3

# Step 3: Verify both running
ps aux | grep -E 'nsbc-web|nsbcd' | grep -v grep
ss -x | grep nsbc     # should show control.sock + data.sock

# Step 4: Open web console
# Browser → https://<your-ip>:443
# Default login: admin / Admin123

# Step 5: Register two extensions
# Default extensions exist: 1000-1019, password 876876, realm nsbc.local
# Register 1001 from MicroSIP and 1002 from a second device

# Step 6: Make a call — that's it!
```

### Verify nsbcd Pulled Config from nsbc-web

```bash
# New in v0.5.0 — diagnostic command
echo "trunks.list" | nc -U /var/run/nsbc/control.sock
echo "dialplan.show" | nc -U /var/run/nsbc/control.sock

# Or grep startup log:
grep 'cfg-sync' logs/nsbcd.log
# Expect: cfg-sync: pull_all: trunks=N, dialplan_rules=N, sip_profiles=N
```

### Verify control.sock is Working

```bash
echo "help" | nc -U /var/run/nsbc/control.sock
echo "status" | nc -U /var/run/nsbc/control.sock
echo "version" | nc -U /var/run/nsbc/control.sock
```

---

## 4. Web ↔ Core Communication Deep Dive

### 4.1 Two Unix Domain Sockets

| Socket | Path | Direction | Owner | Purpose |
|--------|------|-----------|-------|---------|
| `control.sock` | `/var/run/nsbc/control.sock` | **Web → Core** (req/rsp) | nsbcd (listener) | OAM commands: `show calls`, `config.patch`, `trunks.reload <json>`, `reload`, `trunks.list`, ... |
| `data.sock` | `/var/run/nsbc/data.sock` | **Core → Web** (push) | nsbc-web (listener) | Event push: CDR, REGISTER_OK/FAIL, SESSION_CREATE/ANSWER/BYE, runtime stats, audit logs |

**Why two separate sockets?**
- `control.sock` is a command channel (Web asks, Core answers)
- `data.sock` is an event push channel (Core publishes, Web receives)
- Different back-pressure and permission models

### 4.2 UDS Frame Protocol

Both sockets use a unified 12-byte frame header:

```
Offset  Length  Field
  0        1     magic         fixed 0x01
  1        1     frame_type    0x1=HELLO  0x2=HELLO_ACK
                               0x3=CMD_REQ  0x4=CMD_RSP
                               0x5=PUSH  0x6=ACK  0x7=HB  0xF=ERR
  2        4     total_len     big-endian, whole frame incl header
  6        4     ticket        big-endian, REQ→RSP pairing (0 for PUSH/HB)
 10        1     flags        bit0=more_fragment  bit1=compressed
 11        1     rsvd         =0
 12+       N     payload      JSON

HELLO payload:      {"version":"0.5.0"}
CMD_REQ payload:    {"cmd":"cfg.pull_all"}
CMD_RSP payload:    {"ok":true,"trunks":[...],
                       "dialplan_rules":[...],
                       "sip_profiles":[...]}
DATA_PUSH payload:  {"kind":"cdr","payload":{...}}
```

### 4.3 Config Sync: pull_all + cfg_sync

**v0.5.0 unified reload entry — both control.sock `reload` and `kill -HUP` now trigger:**

```
nsbcd reload entry
    │
    ├─ cfg_reload_file()           ← YAML reload (log.level, security.*, ...)
    │
    └─ cfg_sync_after_reload()     ← NEW in v0.5.0
         │
         └─ uds_request("cfg.pull_all")  → nsbc-web
              │
              ├─ Rust side: list_enabled_trunks()      → JSON array
              ├─ Rust side: list_enabled_dialplan_rules() → JSON array
              └─ Rust side: list_enabled_sip_profiles()  → JSON array
              │
              └─ nsbcd cfg_sync.c receives CMD_RSP
                   │
                   ├─ trunk_table_reload_json()     → RTU atomic swap
                   │     └─ outreg_reload_from_route_table() (incremental)
                   ├─ dp_rules_reload_json()        → RCU atomic swap
                   └─ sip_profiles_reload_json()    → RCU + sip_tr_sync_from_profiles()
```

**Startup path:**
```
oam_module start (MOD_L_FINAL)
    └─ uds_client_start()          ← connect to data.sock, HELLO
         └─ cfg_sync_on_startup()  ← NEW in v0.5.0
              └─ retry 10×500ms cfg.pull_all  → wait for nsbc-web to come up
```

**How to test this:**

```bash
# Simulate a "DB change + reload"
# 1. In nsbc-web UI, edit a trunk (change IP)
# 2. Run reload (works via control.sock or SIGHUP):
echo "reload" | nc -U /var/run/nsbc/control.sock
kill -HUP $(pidof nsbcd)

# 3. Verify nsbcd picked it up:
echo "trunks.list" | nc -U /var/run/nsbc/control.sock
grep 'cfg-sync.*trunks OK' logs/nsbcd.log
```

---

## 5. Dialplan Cookbook

### 5.1 Syntax & Rule Format

Two ways to define rules — YAML (startup + reload) or DB (Web UI + JSON reload):

**YAML**:
```yaml
dialplan:
  rules:
    - profile: external           # SIP profile this rule applies to
      match: "^1[0-9]{3,}$"        # PCRE2 regex on Destination Number
      target: "sip:10.0.0.25:5080" # Literal or "trunk:name" / "extension:"
      strip: 0                    # digits to strip from callee
      add_prefix: ""               # prefix to add after strip
      priority: 10                # lower = higher priority
      enabled: true
```

**DB** (nsbc-web UI): same fields, one row per rule in `dialplan_rules` table.

Rule evaluation order: sorted by `(profile ASC, priority ASC, id ASC)` → first match wins.

### 5.2 Literal `sip:host:port` Target (v0.5.0)

**NEW in v0.5.0** — skip route_table entirely, dial directly to a URI. No trunk needed!

```yaml
dialplan:
  rules:
    # Simple literal: bypass route_table, direct INVITE to 10.0.0.25:5080
    - profile: external
      match: "_010"
      target: "sip:10.0.0.25:5080"

    # DNS hostname (nsbcd resolves per INVITE via nsbc_sa_from)
    - profile: external
      match: "^8[0-9]+$"
      target: "sip:sip.example.com:5060"

    # TLS — sips: prefix defaults to port 5061
    - profile: external
      match: "^911$"
      target: "sips:sip.example.com:5061"

    # IPv6
    - profile: external
      match: "^2[0-9]{3}$"
      target: "sip:[::1]:5080"
```

**When to use literal vs trunk:**

| Use literal `sip:host:port` | Use `trunk:name` |
|---------------------------|------------------|
| Quick test call, zero config | Long-term stable relay |
| Temp failover (trunk down → cutover) | Need route_table open/close tracking |
| Simple deployments | Need SIP profile binding / rate limits |
| No HA/failover needed | Integration with outreg registration |

### 5.3 Trunk Routing

```yaml
dialplan:
  rules:
    # Route 010 numbers through trunk named "sip_terminator"
    - profile: external
      match: "_010"
      target: "trunk:sip_terminator"
      strip: 0
      add_prefix: ""
      priority: 10

    # Route international numbers (00+) through international trunk
    - profile: external
      match: "^00"
      target: "trunk:intl_trunk"
      strip: 2             # strip "00"
      add_prefix: "+"      # E.164 format
      priority: 20
```

### 5.4 Route: Dial-Office Hours / After Hours

Dialplan time conditions are evaluated at runtime (UTC):

```yaml
dialplan:
  rules:
    # Office hours (Mon-Fri 09:00-18:00) → ring group
    - profile: external
      match: "^1[0-9]{3}$"
      target: "group:support_desk"
      when:
        days: "Mon,Tue,Wed,Thu,Fri"
        time: "09:00-18:00"
      priority: 10

    # After hours → voicemail
    - profile: external
      match: "^1[0-9]{3}$"
      target: "voicemail:general"
      priority: 20         # lower priority, catches everything else
```

### 5.5 Emergency 911 Bypass

911 should always route to PSTN, bypass all other rules, no stripping:

```yaml
dialplan:
  rules:
    - profile: external
      match: "^911$"
      target: "trunk:pstn_emergency"
      strip: 0
      add_prefix: ""
      priority: 1          # highest priority — always first
      enabled: true
```

### 5.6 Strip & Add Prefix

```yaml
dialplan:
  rules:
    # User dials 01012345678 → strip 3 ("010") → 12345678 → add "86" → 8612345678
    - profile: external
      match: "^010[0-9]{8}$"
      target: "trunk:china_mobile"
      strip: 3
      add_prefix: "86"
```

### 5.7 Ring Groups

```yaml
dialplan:
  rules:
    - profile: external
      match: "^100$"
      target: "group:sales_desk"
      # Ring group defined in YAML modules.applications.ring_groups or DB
```

---

## 6. Outbound Trunk Registration

### 6.1 Via YAML (`outbound.register_trunks`)

Used as startup fallback when nsbc-web DB is empty:

```yaml
outbound:
  register_trunks: "sip_terminator,backup_trunk"

  trunk:
    sip_terminator:
      host: "10.0.0.25"
      port: 5080
      transport: udp
      expires: 3600
      sip_username: "sip_terminator"
      sip_password: "secret123"
      from_user: "sip_terminator"
      from_host: "nsbc.local"

    backup_trunk:
      host: "10.0.0.99"
      port: 5080
      transport: tcp
      expires: 3600
```

### 6.2 Via nsbc-web DB (Recommended)

```bash
# Option A: Web UI → Routing → Trunks → Add Trunk
# Option B: REST API
curl -X POST https://localhost/api/v1/trunks \
  -H "Authorization: Bearer $JWT" \
  -H "Content-Type: application/json" \
  -d '{
    "name": "sip_terminator",
    "host": "10.0.0.25",
    "port": 5080,
    "transport": "udp",
    "enabled": 1,
    "register": 1,
    "sip_username": "sip_terminator",
    "sip_password": "secret123",
    "expires": 3600,
    "profile": "external"
  }'

# Option C: direct sqlite3
sqlite3 data/nsbc.db "INSERT INTO trunks (name,host,port,enabled,register)
  VALUES ('sip_terminator','10.0.0.25',5080,1,1);"

# After any method → reload pulls DB (v0.5.0 unified entry)
echo "reload" | nc -U /var/run/nsbc/control.sock
```

### 6.3 Monitoring Trunk Status

**New in v0.5.0**:

```bash
echo "trunks.list" | nc -U /var/run/nsbc/control.sock

# Output:
# NAME                    KIND         HOST:PORT         ENABLED OPEN   IN_USE   PROFILE
# sip_terminator          register     10.0.0.25:5080    1       open  0        external
# backup_trunk            register     10.0.0.99:5080    1       open  0        external
# -- 2 trunks
```

| Field | Meaning |
|-------|---------|
| `KIND` | `register` = active SIP registration; `static` = static route |
| `OPEN` | trunk's socket state (`open` / `closed`) |
| `IN_USE` | number of active B2BUA calls routed through this trunk |

---

## 7. Control Socket Command Reference

### 7.1 Connecting

```bash
# Method A: netcat
echo "help" | nc -U /var/run/nsbc/control.sock

# Method B: nsbcli (bundled utility)
nsbcli help

# Method C: socat
socat - UNIX-CONNECT:/var/run/nsbc/control.sock
```

### 7.2 All Commands

| Command | Level | Description |
|---------|-------|-------------|
| `help` / `?` | L0 | Show all commands |
| `version` | L0 | Build version, commit, build date |
| `status` | L0 | Uptime, active calls, reactor count, memory |
| `stats` | L0 | Detailed counters (txn in-flight, CPS, registrations) |
| `events [N]` | L0 | Last N events from ring buffer (default=20) |
| `reload` | L1 | **v0.5.0**: reload YAML + pull all DB tables via cfg.pull_all |
| `config.reload` | L1 | Alias for `reload` |
| `config.get <key>` | L0 | Read YAML key (e.g. `sip.profiles.external.port`) |
| `config.set <key> <val>` | L1 | Runtime set YAML key (hot-only keys) |
| `calls list` | L0 | Active B2BUA calls |
| `calls hangup <id>` | L1 | Terminate a call |
| `calls answer <id>` | L1 | Answer a held call |
| `calls hold <id>` | L1 | Put a call on hold |
| `calls transfer <id> <dest>` | L1 | Blind transfer |
| `profiles.list` | L0 | Loaded SIP profiles (port, transport, auth_calls) |
| `profiles.reload <json>` | L1 | Atomic RCU swap of sip_profiles |
| `trunks.reload <json>` | L1 | **Direct** RTU swap (bypasses DB — use `reload` instead) |
| `dialplan-rules.reload <json>` | L1 | **Direct** RCU swap (bypasses DB) |
| `extensions.reload <json>` | L1 | RCU swap of endpoints table |
| `outreg.reload <json>` | L1 | RCU swap of outbound registrations |
| `ivr.reload <json>` | L1 | RCU swap of IVR menus |
| `queues.reload <json>` | L1 | RCU swap of ACD queues |
| `ha.reload <json>` | L1 | HA heartbeat config (no-op — Web DB authoritative) |
| `route_groups.reload <json>` | L1 | Route group config (no-op) |
| `metrics` | L0 | Dump internal metric registry |

### 7.3 New in v0.5.0: Diagnostic Commands

| Command | Level | Description |
|---------|-------|-------------|
| `trunks.list` | L0 | List route_table trunks with name/host/port/enabled/open/in_use |
| `dialplan.show` | L0 | List dp_ctx.active rules sorted by priority |

**Examples:**

```bash
# Verify trunk loaded after reload
echo "trunks.list" | nc -U /var/run/nsbc/control.sock

# Verify dialplan rules loaded
echo "dialplan.show" | nc -U /var/run/nsbc/control.sock

# Combined diagnostic one-liner:
(echo "=== TRUNKS ==="; echo "trunks.list"; echo "=== RULES ==="; echo "dialplan.show") | nc -U /var/run/nsbc/control.sock
```

---

## 8. Module System

### Built-in (statically linked into nsbcd)

| Module | Description |
|--------|-------------|
| `signaling` | SIP B2BUA core — transactions, INVITE/REGISTER handlers |
| `registrar` | REGISTER transaction + location DB |
| `media_bridge` | RTP relay, DTLS-SRTP, SDP rewrite |
| `dialplan` | Dialplan routing, route table (RCU) |
| `outreg` | Outbound SIP trunk registration |
| `endpoints` | Endpoint auth (HA1) |
| `security` | Topology hiding, blacklist, rate limit, auto-ban |
| `applications` | IVR, voicemail, ring group, conference |
| `billing` | CDR, rating, WAL persistence |
| `recording` | Media recording |
| `agent_pool` / `queue` | ACD call center |
| `hep` | HEPv3 capture sink |
| `oam` | Operations & maintenance (control socket, metrics) |
| `ha` | High-availability heartbeat |
| `dbal` | DB abstraction (stubbed — core has **zero** SQLite dependency) |

### Dynamically Loaded (`mod_*.so`)

| Module | Description |
|--------|-------------|
| `codecs` | G.711 ulaw/alaw + G.729 Annex B |
| `playback` | WAV playback (FreeSWITCH-style) |
| `presence` | Presence / BLF |
| `cachedb` | Cache backend |
| `formats` | Media format negotiation |
| `asr_tts` | ASR/TTS interface |
| `ai_dialog` | Real-time AI voice dialog (Whisper + ElevenLabs) |
| `event_handlers` | Event hook dispatch |
| `media_bug` | Passive audio tap |
| `av` | FFmpeg transcoding (stub mode if FFmpeg not found) |

### Disable at Build / Runtime

```bash
# Configure time:
./configure --without-modules=ai_dialog,hello

# Runtime YAML:
modules:
  disable:
    - media_bug
    - presence
```

---

## 9. Observability

### 9.1 Prometheus Metrics

Embedded exporter: `http://127.0.0.1:9100/metrics`

| Metric | Type | Description |
|--------|------|-------------|
| `nsbc_sessions_active` | Gauge | Current B2BUA calls |
| `nsbc_calls_total` | Counter | Calls since start (labeled by disposition: `answered`, `busy`, `no_route`, ...) |
| `nsbc_calls_per_second` | Gauge | 1-second window CPS |
| `nsbc_registrations_active` | Gauge | Active REGISTER contacts |
| `nsbc_rtp_packets_total` | Counter | RTP bridged packets |
| `nsbc_transactions_inflight` | Gauge | SIP transactions in-flight |
| `nsbc_dlopen_mods_loaded` | Gauge | `.so` modules loaded |

### 9.2 Structured Logging

JSON or plain (configurable via `log.format` in YAML). Key fields:

```json
{
  "ts": "2026-10-31T18:42:12.117Z",
  "level": "INFO",
  "module": "sig",
  "trace_id": "call-0xA1B2C3",
  "msg": "INVITE → sip:10.0.0.25:5080 via sip: literal",
  "call_id": "...",
  "from": "sip:1001@nsbc.local",
  "to": "sip:1002@nsbc.local"
}
```

### 9.3 HEPv3 Mirroring

With `hep` module loaded, SIP + RTP traffic mirrors to a HEPv3 collector (Homer / heplify-server / sngrep):

```yaml
hep:
  enabled: true
  collectors:
    - host: "127.0.0.1"
      port: 9060
      capture: all        # sip_only | rtp_only | all
```

---

## 10. Troubleshooting

### 10.1 nsbcd Starts but No Invite Routes

**Symptom**: INVITE → 404 or rings forever, `trunks.list` shows empty.

**Checklist**:
```bash
# 1. Did nsbc-web start FIRST?
ls -la /var/run/nsbc/data.sock /var/run/nsbc/control.sock

# 2. Did nsbcd pull DB config at startup?
grep 'cfg-sync' logs/nsbcd.log
# Expect: "cfg-sync: pull_all: trunks=N, dialplan_rules=N, sip_profiles=N"

# 3. Route table empty?
echo "trunks.list" | nc -U /var/run/nsbc/control.sock

# 4. Dialplan empty?
echo "dialplan.show" | nc -U /var/run/nsbc/control.sock

# 5. Try manual reload
echo "reload" | nc -U /var/run/nsbc/control.sock
kill -HUP $(pidof nsbcd)
```

### 10.2 REGISTER Returns 403

**NEW in v0.5.0**: If your SIP profile has `auth_calls: 0`, REGISTER gets **403 Forbidden** instead of silent timeout. This is **expected behavior**:

```bash
# Check the offending profile
echo "profiles.list" | nc -U /var/run/nsbc/control.sock
# Look for auth_calls=0

# Fix: either enable registrations
#   (nsbc-web UI → SIP Profiles → check "Accept registrations")
# Or route REGISTER to a profile that accepts them
```

### 10.3 trunk Sinks Are Empty After Reload

**Checklist**:
```bash
# Is there an outbound.register_trunks YAML key?
grep register_trunks conf/nsbc.yaml

# Does DB have trunks?
sqlite3 data/nsbc.db "SELECT name,host,port,enabled,register FROM trunks;"

# After reload, did cfg_sync report pulling trunks?
grep 'cfg-sync.*trunks OK' logs/nsbcd.log

# Final check — what's in route_table NOW?
echo "trunks.list" | nc -U /var/run/nsbc/control.sock
```

### 10.4 Media One-Way Audio

```bash
# Check media bridge mode
grep 'bridge_confirm' logs/nsbcd.log | head -5
# Expect: MB_BYPASS (pass-through) or MB_RELAY (anchored) or MB_SRTP_RELAY

# Check nsbcd can reach RTP peer
# From nsbcd host:
sudo tcpdump -ni any udp port range 16384-32768

# Check firewall
sudo iptables -L -n | grep -E 'REJECT|DROP'
```

### 10.5 control.sock `cfg.pull_all` Timeouts

**Symptom**: `cfg_sync` startup retry runs out (10×500ms = 5s).

**Cause**: nsbc-web hasn't come up yet when nsbcd starts.

**Fix**:
```bash
# Ensure nsbc-web starts before nsbcd (systemd ordering below)
# Check nsbc-web logs for errors
tail -f logs/web.log

# Verify DB file exists
ls -la data/nsbc.db
sqlite3 data/nsbc.db ".tables"   # should show trunks, dialplan_rules, ...

# systemd ordering fix:
# [Unit]
# Wants=nsbc-web.service
# After=network.target nsbc-web.service
```

---

## Appendix A: v0.5.0-beta Feature Matrix

| Feature | Description | Key Command / Code Path |
|---------|-------------|------------------------|
| **Unified reload** | Both control.sock "reload" AND `kill -HUP` pull all DB tables via `cfg.pull_all` | `cfg_sync_after_reload()` |
| **Startup DB pull** | nsbc-web not up at nsbcd start? Retry 10×500ms | `cfg_sync_on_startup()` |
| **sip: literal** | Dialplan `sip:host:port` skips route_table | `dp_infer_layer_from_target()` + signaling literal branch |
| **sips: literal** | sips: prefix → default port 5061 + TLS | Same code path |
| **IPv6 literal** | `sip:[::1]:5080` supported | uri_hostport() native |
| **trunks.list** | New control.sock diagnostic command | `rt_trunk_count()` + `rt_trunk_at()` |
| **dialplan.show** | New control.sock diagnostic command | RCU atomic read of `dp_ctx.active` |
| **REGISTER 403** | external profile REGISTER → 403 Forbidden, no 32s timeout | registrar.c silent-drop → respond() |
| **cfg.pull_all** | New Rust UDS CMD_REQ: dump trunks + dialplan_rules + sip_profiles | `uds_server.rs` CMD_REQ handler |

## Appendix B: Open Issues (v0.5.0-beta)

| Priority | Issue | Description |
|----------|-------|-------------|
| P2 | `cfg.pull_all` only pulls 3 tables | `extensions`, `gateways`, `queues`, `ivr_menus`, `ha_groups` have `list_enabled_*` + `_reload_json` ready — +10 lines each |
| P2 | `resolve <user> <ip> <profile>` command | Simulate a number resolution, output matched rule + final target |
| P2 | `help` command should categorize | Currently flat list; should group by OAM / reload / modules |
| P2 | max_calls default too high (100000) | `./build/nsbcd` without `-c` → slab OOM → unclear error message |
| P2 | 5 other silent-drop sites | CIDR blacklist, PERM_DENIED, `no_rule_matched` — could return more specific codes |
| P2 | `config_change_hook` bus | Long-term architecture — replace scatter calls with hook registrations |
| P2 | Test infrastructure | Official sipp XML scenarios + parameterized stress scripts |

---

**NSBC is Apache 2.0 licensed.** Copyright © 2024-2026 李浩 (Li Hao), 上海宁卫信息技术有限公司.

Cite this cookbook as: `NSBC Cookbook — v0.5.0-beta`, 2026-10-31, `docs/cookbook.md`.
