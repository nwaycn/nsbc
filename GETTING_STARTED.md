# NSBC — Getting Started

A practical "first 10 minutes" guide to building and running NSBC from source.

For full configuration reference and operational details, see the [User Guide](docs/user-guide.md) and [Architecture](docs/architecture.md).

---

## Table of Contents

- [1. Supported Operating Systems](#1-supported-operating-systems)
- [2. Install Build Dependencies](#2-install-build-dependencies)
  - [Debian 12 Bookworm (primary build target)](#debian-12-bookworm-primary-build-target)
  - [Ubuntu 22.04 / 24.04](#ubuntu-2204--2404)
  - [RHEL / Rocky / AlmaLinux 9+](#rhel--rocky--almalinux-9)
  - [Generic Linux (package names only)](#generic-linux-package-names-only)
- [3. Install Rust Toolchain](#3-install-rust-toolchain)
- [4. Build the C SIP Daemon (nsbcd)](#4-build-the-c-sip-daemon-nsbcd)
- [5. Build the Web Management Sidecar (nsbc-web)](#5-build-the-web-management-sidecar-nsbc-web)
- [6. Quick Run](#6-quick-run)
- [7. Optional Build Flavors](#7-optional-build-flavors)
- [8. Troubleshooting](#8-troubleshooting)

---

## 1. Supported Operating Systems

| OS | Status | Notes |
|----|--------|-------|
| **Debian 12 Bookworm** | ✅ **Primary** | OpenSSL 3.0.x is preinstalled; all dev packages available in base repos |
| Debian 13 Trixie | ✅ Works | |
| Ubuntu 22.04 LTS | ✅ Works | Default OpenSSL 3.0.x |
| Ubuntu 24.04 LTS | ✅ Works | |
| RHEL / Rocky / AlmaLinux 9 | ✅ Works | OpenSSL 3.0.x from baseos; FFmpeg dev from EPEL |
| RHEL 8 | ⚠️ Possible | OpenSSL 1.1.1 (too old). Upgrade OpenSSL manually or rebuild from source. |
| FreeBSD 14 | ⚠️ Untested | Patches welcome |
| macOS | ❌ Not supported | nsbc is Linux-only (epoll-based reactor) |

**Kernel requirement**: 4.15 or newer (for `SO_REUSEPORT` and modern TLS socket options).

---

## 2. Install Build Dependencies

### Debian 12 Bookworm (primary build target)

OpenSSL 3.0.x ships by default — no extra repo needed.

```bash
# update apt indexes
sudo apt update

# core build toolchain
sudo apt install -y build-essential pkg-config git curl ca-certificates

# OpenSSL 3.0 — headers + runtime
# Debian 12's libssl-dev is OpenSSL 3.0.13, which satisfies NSBC's >= 3.0 requirement
sudo apt install -y libssl-dev

# libsrtp2 — DTLS-SRTP media encryption (hard dependency)
sudo apt install -y libsrtp2-dev

# libpcre2 — regex dialplan matching (hard dependency)
sudo apt install -y libpcre2-dev

# libedit — optional, for nsbc_cli interactive shell (Tab completion / history)
sudo apt install -y libedit-dev

# SQLite — Rust sidecar DB (not used by the C core)
sudo apt install -y sqlite3

# FFmpeg — OPTIONAL, only needed for mod_av (video transcoding)
# Skip this step if you do not need WebRTC video bridges; mod_av falls back to
# stub mode (NOTSUP) automatically when FFmpeg is missing
sudo apt install -y libavcodec-dev libavutil-dev libswresample-dev libswscale-dev

# Verify OpenSSL version (should show 3.0.x)
openssl version
# OpenSSL 3.0.13 30 Jan 2024 (Library: OpenSSL 3.0.13 30 Jan 2024)
```

### Ubuntu 22.04 / 24.04

Same as Debian 12 — all packages are in the default `main` archive.

```bash
sudo apt update
sudo apt install -y build-essential pkg-config git curl ca-certificates \
                    libssl-dev libsrtp2-dev libpcre2-dev libedit-dev sqlite3 \
                    libavcodec-dev libavutil-dev libswresample-dev libswscale-dev
openssl version    # should be OpenSSL 3.0.x on 22.04 / 3.2.x on 24.04
```

### RHEL / Rocky / AlmaLinux 9+

OpenSSL 3.0.x is in `baseos`. FFmpeg dev lives in EPEL/RPM Fusion.

```bash
sudo dnf install -y gcc gcc-c++ make pkg-config git curl ca-certificates \
                    openssl-devel libsrtp2-devel pcre2-devel libedit-devel sqlite-devel

# FFmpeg dev — optional
sudo dnf install -y https://download1.rpmfusion.org/free/el/rpmfusion-free-release-9.noarch.rpm
sudo dnf install -y ffmpeg-devel      # pulls libavcodec-devel, libavutil-devel, ...

openssl version
# OpenSSL 3.0.7 FIPS 15 Aug 2023
```

### Generic Linux (package names only)

| Purpose | Debian/Ubuntu | RHEL/Fedora/Alma 9 |
|---------|--------------|---------------------|
| C compiler + tools | `build-essential` | `gcc gcc-c++ make` |
| OpenSSL 3.0 headers | `libssl-dev` | `openssl-devel` |
| libsrtp2 | `libsrtp2-dev` | `libsrtp2-devel` |
| PCRE2 | `libpcre2-dev` | `pcre2-devel` |
| libedit (optional CLI) | `libedit-dev` | `libedit-devel` |
| SQLite (sidecar) | `sqlite3` | `sqlite-devel` |
| FFmpeg codecs (optional) | `libavcodec-dev libavutil-dev libswresample-dev libswscale-dev` | `ffmpeg-devel` |
| pkg-config | `pkg-config` | `pkg-config` |
| Git / curl | `git curl` | `git curl` |

---

## 3. Install Rust Toolchain

NSBC's web management console (`nsbc-web`) is written in Rust. Install the stable toolchain via rustup:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal
source "$HOME/.cargo/env"        # load cargo into PATH for current shell
rustc --version                  # rustc 1.78.0 or newer
cargo --version
```

Minimum supported Rust version (MSRV): **1.78** (for `axum 0.7`, `sqlx 0.8`, and `rustls 0.23`).

If you are building on an isolated machine without internet, see the [Offline Build](#offline-build) section below.

---

## 4. Build the C SIP Daemon (nsbcd)

```bash
# clone the repo
git clone https://github.com/nsbc/nsbc.git
cd nsbc

# configure — reads conf/modules.conf, resolves module deps,
# generates .config/config.mk + src/core/builtin_modules.gen.inc + build/mod/mod.load
./configure
# expected output includes:
#   ✓ scan src/**/*.c:  28 modules
#   ✓ .config/config.mk   (NPROC=4, CC=gcc, LDLIBS=-lssl -lcrypto -lsrtp2 -lpcre2-8 ...)
#   ✓ src/core/builtin_modules.gen.inc (8 lines, STATIC=signaling oam ...)
#   ✓ build/mod.load      (11 dynamic modules)

# compile
make -j$(nproc)
# expected:
#   CC  build/src/core/main.o
#   ...
#   LINK  build/nsbcd
#   COPY  nsbcd
#   MOD  build/mod/mod_endpoints.so
#   ...

# install (optional)
sudo make install
# installs nsbcd, nsbc_cli, mod_*.so, default config under /usr/local
```

Verify the binary was built:

```bash
ls -la nsbcd nsbcd_cli 2>/dev/null || ls -la build/nsbcd
ldd build/nsbcd | grep "not found" || echo "✓ all shared libs resolved"
```

Common configure flags:

```bash
./configure --with-modules=billing,dialplan,recording,security      # whitelist
./configure --without-modules=hello,ai_dialog,presence              # blacklist
NSBC_WITH_CURL=1 ./configure                                       # enable real ASR/TTS
NSBC_WITH_BCG729=0 ./configure                                      # disable G.729 to save time
```

### Offline Build

If your build machine cannot reach crates.io, first vendor all dependencies on a machine **with** internet:

```bash
# on a machine WITH internet
cd nsbc/web
make vendor         # creates web/vendor/ and sets .cargo/config.toml vendor path
# then ship the whole repo (including vendor/ and .cargo/) to the isolated build box
```

On the isolated box, `cd web && make build` works fully offline.

---

## 5. Build the Web Management Sidecar (nsbc-web)

```bash
cd nsbc/web
make fetch          # rebuild Cargo.lock with online access
make build          # offline-compile nsbc-web + bundle SPA static assets
# expected:
#   Compiling nsbc-web v2.5.5
#    Finished release [optimized] target(s) in 2m 15s
#   ✅ build done → target/release/nsbc-web
#   ✅ install done (binary + static copied to parent directory)

# or with ACME Let's Encrypt auto-renewal (for public deployments)
make vendor         # cargo vendor — needed once before ACME build
make build-acme
```

Artifacts land in the project root:

```
nsbc-web/            # Rust sidecar binary
static/              # embedded SPA (Vue 3 + Element Plus)
```

---

## 6. Quick Run

### Directory layout (the lazy way — no install)

```bash
cd nsbc

# create runtime dirs
mkdir -p logs run tls recordings data

# Rust sidecar first (generates self-signed TLS cert at tls/cert.pem on first boot)
cd web && ./nsbc-web -c ../conf/nsbc.yaml &
sleep 2

# C SIP daemon
cd .. && ./nsbcd -c conf/nsbc.yaml
```

Success indicators in `logs/nsbcd.log`:

```
[notice] nsbc 2.5.5 starting (pid=12345, core=C11, openssl=3.0.13, srtp=2.6.0)
[notice] reactor listening UDP 0.0.0.0:5060
[notice] reactor listening TCP 0.0.0.0:5060
[notice] [tls] cert=tls/cert.pem key=tls/key.pem (TLS SIP on :5061, WSS on :5062)
[notice] oam_control: UDS /var/run/nsbc/control.sock
[notice] mod playback INIT OK (engine started)
[notice] mod security INIT OK (blacklist loaded 0)
[notice] mod signaling FINAL OK (all modules up)
```

### Open the Web Console

Point your browser at **https://<your-host>:8443**.

- Default username: `admin`
- Default password: `Admin123` (or value of `NSBC_DEFAULT_ADMIN_PW` env var)
- Self-signed cert — proceed past the browser warning

### Test with the CLI

```bash
./nsbc_cli
nsbc> version
nsbc> show registrations
nsbc> show calls
nsbc> config.reload
nsbc> help
```

### First SIP Register

```bash
# using sipsak (apt install sipsak)
sipsak -s sip:2001@<your-host> -M REGISTER
# expected: 401 Unauthorized → REGISTER with auth → 200 OK
```

---

## 7. Optional Build Flavors

### Sanitizer Builds (for debugging)

```bash
make clean
make SANITIZE=asan -j$(nproc)       # AddressSanitizer
make SANITIZE=tsan -j$(nproc)       # ThreadSanitizer
make SANITIZE=ubsan -j$(nproc)       # UndefinedBehaviorSanitizer
```

All build with `-g` debug symbols and no LTO so sanitizer stacks are readable.

### Build Without Dynamic Modules (static-only)

```bash
./configure --without-modules=hello,presence,asr_tts,ai_dialog,playback,recording,cachedb,formats
make -j$(nproc)
# result: single static nsbcd binary, no mod_*.so dlopen at runtime
```

### Cross-Compile (example: x86_64 → aarch64)

```bash
sudo apt install gcc-aarch64-linux-gnu libc6-dev-arm64-cross
./configure --host=aarch64-linux-gnu
make -j$(nproc) CC=aarch64-linux-gnu-gcc
```

---

## 8. Troubleshooting

### `ld: undefined reference to pb_engine_start`

`playback` needs to be STATIC (configure injects it into HARD_STATIC). Re-run `./configure` then `make clean && make`.

### `cargo generate-lockfile: tokio does not have feature 'blocking'`

You are on tokio 1.x — the `blocking` feature was removed in tokio 1.0. Edit `web/Cargo.toml` and remove it from the tokio features list (see commit history for the exact fix).

### `ldd nsbcd shows libsrtp.so not found`

libsrtp2 not installed, or the linker cache is stale:

```bash
sudo apt install libsrtp2-dev
sudo ldconfig
```

### `EACCES: /var/run/nsbc/control.sock`

nsbcd runs as a different user than nsbc-web. Either:

```bash
sudo chown nsbc:nsbc /var/run/nsbc
# or run both as the same user for quick testing
sudo ./nsbcd
sudo ./nsbc-web
```

### `mod_av: FFmpeg not found — stub mode`

Expected if you skipped FFmpeg dev packages. Install them as described in [Section 2](#2-install-build-dependencies) and re-run `./configure && make`.

### `openssl version shows 1.1.1 instead of 3.0.x`

You are on an older OS. Options:

1. **Upgrade OS** (Debian 12, Ubuntu 22.04+ — both have OpenSSL 3.0.x out of the box)
2. **Build OpenSSL from source** (install to `/usr/local/openssl3`, then `./configure CFLAGS="-I/usr/local/openssl3/include" LDFLAGS="-L/usr/local/openssl3/lib" CPPFLAGS="-I/usr/local/openssl3/include" LDLIBS="-L/usr/local/openssl3/lib -Wl,-rpath,/usr/local/openssl3/lib"`)

---

## License

Apache 2.0 — see [LICENSE](LICENSE).
