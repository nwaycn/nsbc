# NSBC — 快速上手

从零构建并运行 NSBC 的"前 10 分钟"实操指南。

完整配置手册和架构说明请参阅 [用户指南](docs/user-guide_cn.md) 与 [架构文档](docs/architecture.md)。

---

## 目录

- [1. 支持的操作系统](#1-支持的操作系统)
- [2. 安装编译依赖](#2-安装编译依赖)
  - [Debian 12 Bookworm（主力编译平台）](#debian-12-bookworm主力编译平台)
  - [Ubuntu 22.04 / 24.04](#ubuntu-2204--2404)
  - [RHEL / Rocky / AlmaLinux 9+](#rhel--rocky--almalinux-9)
  - [通用 Linux（仅包名速查）](#通用-linux仅包名速查)
- [3. 安装 Rust 工具链](#3-安装-rust-工具链)
- [4. 编译 C SIP 守护进程（nsbcd）](#4-编译-c-sip-守护进程nsbcd)
- [5. 编译 Web 管理控制台（nsbc-web）](#5-编译-web-管理控制台nsbc-web)
- [6. 快速运行](#6-快速运行)
- [7. 可选编译形态](#7-可选编译形态)
- [8. 常见问题排查](#8-常见问题排查)

---

## 1. 支持的操作系统

| 操作系统 | 状态 | 备注 |
|----------|------|------|
| **Debian 12 Bookworm** | ✅ **主力** | 内置 OpenSSL 3.0.x，所有开发包在官方源 |
| Debian 13 Trixie | ✅ 可用 | |
| Ubuntu 22.04 LTS | ✅ 可用 | 默认 OpenSSL 3.0.x |
| Ubuntu 24.04 LTS | ✅ 可用 | |
| RHEL / Rocky / AlmaLinux 9 | ✅ 可用 | baseos 自带 OpenSSL 3.0.x，FFmpeg dev 需 EPEL |
| RHEL 8 | ⚠️ 可能可行 | 默认 OpenSSL 1.1.1 过旧，需自行升级 |
| FreeBSD 14 | ⚠️ 未测试 | 欢迎贡献补丁 |
| macOS | ❌ 不支持 | nsbc 基于 Linux epoll 反应堆 |

**内核要求**：4.15 及以上（依赖 `SO_REUSEPORT` 和现代 TLS socket 选项）。

---

## 2. 安装编译依赖

### Debian 12 Bookworm（主力编译平台）

OpenSSL 3.0.x 已随系统预装，无需额外仓库。

```bash
# 更新包索引
sudo apt update

# 基础编译工具链
sudo apt install -y build-essential pkg-config git curl ca-certificates

# OpenSSL 3.0 —— 头文件 + 运行库
# Debian 12 的 libssl-dev 即为 OpenSSL 3.0.13，满足 NSBC >= 3.0 的要求
sudo apt install -y libssl-dev

# libsrtp2 —— DTLS-SRTP 媒体加密（硬依赖）
sudo apt install -y libsrtp2-dev

# libpcre2 —— 正则拨号计划匹配（硬依赖）
sudo apt install -y libpcre2-dev

# libedit —— 可选，nsbc_cli 交互式命令行（Tab 补全 / 历史）
sudo apt install -y libedit-dev

# SQLite —— Rust 边车数据库（C 核心不使用 DB）
sudo apt install -y sqlite3

# FFmpeg —— 可选，仅 mod_av（视频转码/视频桥接）需要
# 如果不需要 WebRTC 视频桥接可跳过；缺少时 mod_av 自动降级为 stub (NOTSUP)
sudo apt install -y libavcodec-dev libavutil-dev libswresample-dev libswscale-dev

# 验证 OpenSSL 版本（应显示 3.0.x）
openssl version
# OpenSSL 3.0.13 30 Jan 2024 (Library: OpenSSL 3.0.13 30 Jan 2024)
```

### Ubuntu 22.04 / 24.04

与 Debian 12 完全相同，所有包均在默认 `main` 仓库中。

```bash
sudo apt update
sudo apt install -y build-essential pkg-config git curl ca-certificates \
                    libssl-dev libsrtp2-dev libpcre2-dev libedit-dev sqlite3 \
                    libavcodec-dev libavutil-dev libswresample-dev libswscale-dev
openssl version    # 22.04 为 OpenSSL 3.0.x / 24.04 为 OpenSSL 3.2.x
```

### RHEL / Rocky / AlmaLinux 9+

OpenSSL 3.0.x 在 `baseos`，FFmpeg dev 需 EPEL / RPM Fusion。

```bash
sudo dnf install -y gcc gcc-c++ make pkg-config git curl ca-certificates \
                    openssl-devel libsrtp2-devel pcre2-devel libedit-devel sqlite-devel

# FFmpeg dev —— 可选
sudo dnf install -y https://download1.rpmfusion.org/free/el/rpmfusion-free-release-9.noarch.rpm
sudo dnf install -y ffmpeg-devel      # 会自动拉起 libavcodec-devel、libavutil-devel 等

openssl version
# OpenSSL 3.0.7 FIPS 15 Aug 2023
```

### 通用 Linux（仅包名速查）

| 用途 | Debian / Ubuntu | RHEL / Fedora / Alma 9 |
|------|-----------------|------------------------|
| C 编译器 + 工具 | `build-essential` | `gcc gcc-c++ make` |
| OpenSSL 3.0 头文件 | `libssl-dev` | `openssl-devel` |
| libsrtp2 | `libsrtp2-dev` | `libsrtp2-devel` |
| PCRE2 | `libpcre2-dev` | `pcre2-devel` |
| libedit（可选 CLI） | `libedit-dev` | `libedit-devel` |
| SQLite（边车） | `sqlite3` | `sqlite-devel` |
| FFmpeg 编解码（可选） | `libavcodec-dev libavutil-dev libswresample-dev libswscale-dev` | `ffmpeg-devel` |
| pkg-config | `pkg-config` | `pkg-config` |
| Git / curl | `git curl` | `git curl` |

---

## 3. 安装 Rust 工具链

NSBC 的 Web 管理控制台（`nsbc-web`）使用 Rust 编写。通过 rustup 安装 stable 工具链：

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal
source "$HOME/.cargo/env"        # 当前 shell 生效
rustc --version                  # rustc 1.78.0 或更新
cargo --version
```

最低支持 Rust 版本（MSRV）：**1.78**（`axum 0.7`、`sqlx 0.8`、`rustls 0.23` 的要求）。

若编译机无法访问 crates.io，见下文 [离线编译](#离线编译) 章节。

---

## 4. 编译 C SIP 守护进程（nsbcd）

```bash
# 克隆仓库
git clone https://github.com/nsbc/nsbc.git
cd nsbc

# configure —— 读取 conf/modules.conf，解析模块依赖拓扑，
# 生成 .config/config.mk + src/core/builtin_modules.gen.inc + build/mod/mod.load
./configure
# 预期输出：
#   ✓ scan src/**/*.c:  28 modules
#   ✓ .config/config.mk   (NPROC=4, CC=gcc, LDLIBS=-lssl -lcrypto -lsrtp2 -lpcre2-8 ...)
#   ✓ src/core/builtin_modules.gen.inc (8 lines, STATIC=signaling oam ...)
#   ✓ build/mod.load      (11 dynamic modules)

# 并行编译
make -j$(nproc)
# 预期：
#   CC  build/src/core/main.o
#   ...
#   LINK  build/nsbcd
#   COPY  nsbcd
#   MOD  build/mod/mod_endpoints.so
#   ...

# 安装（可选）
sudo make install
# 安装 nsbcd、nsbc_cli、mod_*.so、默认配置至 /usr/local
```

验证产物：

```bash
ls -la nsbcd nsbcd_cli 2>/dev/null || ls -la build/nsbcd
ldd build/nsbcd | grep "not found" || echo "✓ 所有共享库已解析"
```

常用 configure 参数：

```bash
./configure --with-modules=billing,dialplan,recording,security      # 白名单模式
./configure --without-modules=hello,ai_dialog,presence              # 黑名单模式
NSBC_WITH_CURL=1 ./configure                                       # 启用真实 ASR/TTS（Whisper / ElevenLabs）
NSBC_WITH_BCG729=0 ./configure                                      # 跳过 G.729 以节省编译时间
```

### 离线编译

若编译机无法访问 crates.io，先在**有互联网的机器**上 vendor 所有依赖：

```bash
# 在有网机器上
cd nsbc/web
make vendor         # 生成 web/vendor/ 并写入 .cargo/config.toml
# 然后把整个仓库（含 vendor/ 和 .cargo/）打包传到隔离编译机
```

隔离机上 `cd web && make build` 完全离线编译。

---

## 5. 编译 Web 管理控制台（nsbc-web）

```bash
cd nsbc/web
make fetch          # 联网重新生成 Cargo.lock（有网时执行一次）
make build          # 离线编译 nsbc-web + 内嵌 SPA 静态资源
# 预期：
#   Compiling nsbc-web v2.5.5
#    Finished release [optimized] target(s) in 2m 15s
#   ✅ build done → target/release/nsbc-web
#   ✅ install done (binary + static copied to parent directory)

# 或带 ACME Let's Encrypt 自动续签（公网部署）
make vendor         # 先 vendor（ACME 依赖 certon crate）
make build-acme
```

产物位于项目根目录：

```
nsbc-web/            # Rust 边车二进制
static/              # 内嵌 SPA（Vue 3 + Element Plus）
```

---

## 6. 快速运行

### 目录布局（懒人版 —— 无需安装）

```bash
cd nsbc

# 创建运行目录
mkdir -p logs run tls recordings data

# 先起 Rust 边车（首次启动在 tls/cert.pem 下自动生成自签证书）
cd web && ./nsbc-web -c ../conf/nsbc.yaml &
sleep 2

# 再起 C SIP 守护进程
cd .. && ./nsbcd -c conf/nsbc.yaml
```

`logs/nsbcd.log` 中看到以下行即为启动成功：

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

### 打开 Web 控制台

浏览器访问 **https://<你的主机>:8443**。

- 默认用户名：`admin`
- 默认密码：`Admin123`（或环境变量 `NSBC_DEFAULT_ADMIN_PW` 的值）
- 自签证书 → 浏览器点"继续访问"

### 用 CLI 验证

```bash
./nsbc_cli
nsbc> version
nsbc> show registrations
nsbc> show calls
nsbc> config.reload
nsbc> help
```

### 第一次 SIP Register

```bash
# 使用 sipsak（apt install sipsak）
sipsak -s sip:2001@<你的主机> -M REGISTER
# 预期：401 Unauthorized → 带 Authorization 的 REGISTER → 200 OK
```

---

## 7. 可选编译形态

### Sanitizer 构建（调试用）

```bash
make clean
make SANITIZE=asan -j$(nproc)       # AddressSanitizer
make SANITIZE=tsan -j$(nproc)       # ThreadSanitizer
make SANITIZE=ubsan -j$(nproc)       # UndefinedBehaviorSanitizer
```

全部开启 `-g` 调试符号、关闭 LTO，使 sanitizer 栈帧可读。

### 纯静态构建（无动态模块）

```bash
./configure --without-modules=hello,presence,asr_tts,ai_dialog,playback,recording,cachedb,formats
make -j$(nproc)
# 产物：单一静态 nsbcd 二进制，运行时不 dlopen mod_*.so
```

### 交叉编译（示例：x86_64 → aarch64）

```bash
sudo apt install gcc-aarch64-linux-gnu libc6-dev-arm64-cross
./configure --host=aarch64-linux-gnu
make -j$(nproc) CC=aarch64-linux-gnu-gcc
```

---

## 8. 常见问题排查

### `ld: undefined reference to pb_engine_start`

`playback` 模块需要被强制并入 STATIC（configure 会自动加进 HARD_STATIC）。重新跑 `./configure && make clean && make`。

### `cargo generate-lockfile: tokio does not have feature 'blocking'`

当前 tokio 为 1.x，`blocking` feature 在 tokio 1.0 已移除。编辑 `web/Cargo.toml`，从 tokio features 列表里去掉 `"blocking"`（见提交历史中对应修复）。

### `ldd nsbcd 显示 libsrtp.so not found`

libsrtp2 未安装，或动态链接器缓存过期：

```bash
sudo apt install libsrtp2-dev
sudo ldconfig
```

### `EACCES: /var/run/nsbc/control.sock`

nsbcd 与 nsbc-web 运行用户不一致。任选其一：

```bash
sudo chown nsbc:nsbc /var/run/nsbc
# 或快速测试时两者同用户运行
sudo ./nsbcd
sudo ./nsbc-web
```

### `mod_av: FFmpeg not found — stub mode`

这是预期行为，说明没装 FFmpeg 开发包。按 [第 2 节](#2-安装编译依赖) 安装后重新 `./configure && make`。

### `openssl version 显示 1.1.1 而非 3.0.x`

当前系统版本过旧。两种解决方式：

1. **升级操作系统**（Debian 12、Ubuntu 22.04+ 均自带 OpenSSL 3.0.x）
2. **源码编译 OpenSSL**（安装至 `/usr/local/openssl3`，然后 `./configure CFLAGS="-I/usr/local/openssl3/include" LDFLAGS="-L/usr/local/openssl3/lib" CPPFLAGS="-I/usr/local/openssl3/include" LDLIBS="-L/usr/local/openssl3/lib -Wl,-rpath,/usr/local/openssl3/lib"`）

---

## 许可证

Apache 2.0 —— 见 [LICENSE](LICENSE)。
