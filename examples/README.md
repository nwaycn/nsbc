# NSBC Module Examples and Enabling Guide

This directory holds writing templates, enablement examples, and documentation for
each NSBC SIP B2BUA module.

---

## Directory

| File | Description |
|------|-------------|
| [module_template.c](module_template.c) | **New-module writing starter template** (with three writing-style examples and full comments) |
| [01_enable_builtins.yaml](01_enable_builtins.yaml) | YAML config template for built-in registered modules |
| [02_enable_new_mods.cdiff](02_enable_new_mods.cdiff) | Code diff needed to enable modules under `src/mod/` that are written but not yet registered |
| [03_makefile_srcs_hack.md](03_makefile_srcs_hack.md) | Notes on appending to Makefile SRCS whitelist |

---

## Quick start: three steps to add a new module

### 1. Write the `.c` file

Copy the template to `src/mod/myfeature/myfeature.c`, then edit per the template comments.
The key is the trailing one-line macro:

```c
NSBC_MOD0(myfeature, 1, MOD_L_FINAL, MOD_F_REQUIRED, myf_init, myf_fini);
```

- `NSBC_MOD0` = minimal style with no dependencies and no start/stop; use `NSBC_MOD` when
  you need start/stop or deps
- `level` values are defined in `src/include/module.h` `mod_level_t`; `MOD_L_FINAL` is
  the usual choice
- `flags` can be ORed together: `MOD_F_CALL_PATH` (call-plane module), `MOD_F_REQUIRED`
  (startup failure if module fails)

### 2. Register in builtin.c

Open `src/core/builtin.c` and add two lines:

```c
extern const nsbc_module_t nsbcm_myfeature;   // add to the existing extern list

// append to the nsbc_builtin_modules[] array:
&nsbcm_myfeature,
```

The table terminates with `(const nsbc_module_t *)0`; ordering does not matter (the
module manager does an internal topological sort).

### 3. Add one line to the Makefile

Open the root `Makefile`, find line 24 `SRCS := $(shell (find ...`, and append after
the existing whitelist directories:

```makefile
find src/mod/myfeature -name '*.c';
```

Then `make && ./nsbcd -T` to verify config loads.

---

## Module layering and dependency order

Refer to `src/include/module.h` `mod_level_t`:

```
MOD_L_EARLY          ← self-check / register only (example: dbal)
  ↓
MOD_L_MEM_READY      ← memory five-domains / RCU ready
  ↓
MOD_L_NET_READY      ← reactor / sockets ready
  ↓
MOD_L_WORKER_READY   ← worker pool / wheel ready (examples: endpoints, outreg, dialplan)
  ↓
MOD_L_FINAL          ← last layer before accepting traffic (examples: signaling, billing, recording)
```

The module manager initializes by level; within a level it topologically sorts by deps.

---

## Currently registered vs. unregistered modules

### Built-in modules registered in builtin.c

| Module | Source | Level | Flags | Description |
|--------|--------|-------|-------|-------------|
| `dbal` | `src/db/dbal_module.c` | EARLY | REQUIRED | Database abstraction layer |
| `endpoints` | `src/mod/endpoints/endpoints.c` | WORKER_READY | REQUIRED | Extension / trunk credential DB |
| `outreg` | `src/mod/outbound_reg/outbound_reg.c` | WORKER_READY | - | Outbound registration |
| `dialplan` | `src/mod/dialplans/dialplan.c` | WORKER_READY | - | Dialplan |
| `signaling` | `src/sip/signaling.c` | FINAL | CALL_PATH\|REQUIRED | SIP core signaling |
| `oam` | `src/oam/oam_module.c` | FINAL | - | Ops management (UDS commands) |
| `ha` | `src/ha/ha.c` | FINAL | - | High availability |
| `hep` | `src/media/bridge/hep_sink.c` | FINAL | - | HEP packet capture |
| `recording` | `src/mod/recording/recording.c` | FINAL | - | Recording module |
| `billing` | `src/mod/billing/billing.c` | FINAL | - | Billing / CDR |

### Modules written but **not registered** (stub init only, P1 real implementation)

These module files live under `src/mod/`, `NSBC_MOD0` macros are written, but they are
neither externed + added to the table in builtin.c nor whitelisted in the Makefile SRCS.
Their init is a no-op:

| Module | Directory | Purpose | Effort to enable |
|--------|-----------|---------|------------------|
| `applications` | `src/mod/applications/` | IVR / conference / voicemail / ring-group orchestration | Medium (wires into dialplan) |
| `asr_tts` | `src/mod/asr_tts/` | ASR/TTS AI bridge | Low (stub init, placeholder) |
| `cachedb` | `src/mod/cachedb/` | External cache (Redis/Memcached) | Low |
| `codecs` | `src/mod/codecs/` | External codec plugin | Low |
| `event_handlers` | `src/mod/event_handlers/` | Business event handler registry | Low |
| `formats` | `src/mod/formats/` | File-format plugins (e.g. .wav/.mp3) | Low |
| `presence` | `src/mod/presence/` | Presence subscription | Medium |
| `callcenter` | `src/mod/callcenter/` | Call center (agent/queue) | High |
| `security` | `src/mod/security/` | Toll-fraud / risk control | High |

For enablement steps, see `02_enable_new_mods.cdiff`.

---

## Build switches (Makefile optional)

| Switch | Default | Meaning |
|--------|---------|---------|
| `NSBC_WITH_OPENSSL=1` | On | SIP TLS + DTLS-SRTP |
| `NSBC_WITH_SRTP=1` | On | libsrtp2 media encryption |
| `NSBC_WITH_SQLITE=1` | Off | SQLite WAL driver |
| `NSBC_WITH_DLOPEN=1` | Off | Development dynamic module loading |

Example:
```bash
make NSBC_WITH_OPENSSL=0            # disable TLS
make NSBC_WITH_DLOPEN=1             # enable dlopen dev mode
```

When `NSBC_WITH_DLOPEN` is on you can `modmgr_load()` a `.so` at runtime, but
`MOD_F_CALL_PATH` modules are rejected for dlopen (the call plane is never unloaded).

---

## Verification

```bash
make                                 # compile
./nsbcd -T -c configs/nsbc.yaml      # config check (dry-run mode)
./nsbcd -c configs/nsbc.yaml          # normal start
./nsbc_cli module list                # inspect loaded modules at runtime
```

You should see something like this in the startup log:
```
[INFO] core: registered 10 modules
[INFO] mod: inited dbal (level 0)
[INFO] mod: inited endpoints (level 3)
...
```
