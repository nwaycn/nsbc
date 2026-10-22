# NSBC 用户指南

本指南涵盖 NSBC SIP 平台的安装、配置及日常运维。

---

## 目录

- [1. 系统要求](#1-系统要求)
- [2. 从源码构建](#2-从源码构建)
  - [2.1 依赖项](#21-依赖项)
  - [2.2 配置](#22-配置)
  - [2.3 编译](#23-编译)
  - [2.4 构建 Web 旁车进程](#24-构建-web-旁车进程)
- [3. 安装](#3-安装)
- [4. 目录结构](#4-目录结构)
- [5. 配置](#5-配置)
  - [5.1 共享 `conf/nsbc.yaml`](#51-共享-confnsbcyaml)
  - [5.2 核心默认 `configs/nsbc.yaml`](#52-核心默认-configsnsbcyaml)
  - [5.3 环境变量](#53-环境变量)
  - [5.4 免安装运行](#54-免安装运行)
- [6. 启动 NSBC](#6-启动-nsbc)
  - [6.1 启动顺序](#61-启动顺序)
  - [6.2 systemd 单元（示例）](#62-systemd-单元示例)
  - [6.3 优雅重启](#63-优雅重启)
- [7. Web 管理控制台](#7-web-管理控制台)
  - [7.1 默认登录](#71-默认登录)
  - [7.2 TLS 证书模式](#72-tls-证书模式)
  - [7.3 实时监控](#73-实时监控)
- [8. 模块系统](#8-模块系统)
  - [8.1 内置（静态链接）](#81-内置静态链接)
  - [8.2 动态（mod_*.so）](#82-动态-mod_so)
  - [8.3 运行时启用/禁用](#83-运行时启用禁用)
  - [8.4 编写自定义模块](#84-编写自定义模块)
- [9. 拨号计划](#9-拨号计划)
  - [9.1 规则格式](#91-规则格式)
  - [9.2 拨号计划动作](#92-拨号计划动作)
- [10. 可观测性](#10-可观测性)
  - [10.1 Prometheus 指标](#101-prometheus-指标)
  - [10.2 控制套接字（UDS）](#102-控制套接字uds)
  - [10.3 事件总线 / 订阅](#103-事件总线--订阅)
- [11. 事件客户端示例](#11-事件客户端示例)
- [12. 故障排查](#12-故障排查)
  - [12.1 核心无法启动](#121-核心无法启动)
  - [12.2 未返回 401 质询](#122-未返回-401-质询)
  - [12.3 媒体未转发（单通语音）](#123-媒体未转发单通语音)
  - [12.4 TLS/WS 无法工作](#124-tlsws-无法工作)

---

## 1. 系统要求

| 项目 | 最低要求 | 推荐配置 |
|------|---------|-------------|
| 操作系统 | Linux x86_64，内核 4.15+ | Linux x86_64，内核 5.1+（Ubuntu 22.04 LTS / RHEL 9+） |
| CPU | 2 核 | 4+ 核 |
| 内存 | 256 MB | 512 MB – 2 GB（取决于并发量） |
| 磁盘 | 200 MB | 1 GB（包含录音和审计日志） |
| GCC | 4.8（C11） | GCC 8+ 或 Clang 10+ |
| Rust | — | stable 工具链（用于 `nsbc-web`） |
| OpenSSL | 1.1.1+ | 3.0+ |
| libsrtp2 | 2.3+ | 2.5+ |
| libpcre2 | 10.30+ | 10.40+ |
| libedit | （可选，用于 CLI） | 已安装 |
| pkg-config | 必需 | 必需 |

构建依赖包（Debian/Ubuntu）：

```bash
sudo apt install build-essential pkg-config libssl-dev libsrtp2-dev \
                 libpcre2-dev libedit-dev sqlite3 libcurl4-openssl-dev
# Rust 旁车（可选）：
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

构建依赖包（RHEL/CentOS 8+）：

```bash
sudo dnf install gcc gcc-c++ make pkg-config openssl-devel libsrtp2-devel \
                 pcre2-devel libedit-devel sqlite-devel curl-devel
```

---

## 2. 从源码构建

### 2.1 依赖项

第三方库以 vendored 形式内置或自动下载：

| 库 | 来源 | 用途 |
|---------|--------|---------|
| bcg729 | `3rd/bcg729/`（vendored） | G.729 Annex B 编解码器（`mod_codecs.so`） |
| cJSON | `3rd/cjson/`（vendored） | 核心中的 JSON 解析 |
| libsrtp2 | 系统包 | DTLS-SRTP 媒体加密 |
| OpenSSL | 系统包 | TLS / DTLS / 证书处理 |
| libpcre2 | 系统包 | 正则拨号计划匹配 |

### 2.2 配置

configure 脚本读取 `conf/modules.conf`，解析模块的拓扑依赖，并生成：

- `.config/config.mk` — 编译变量
- `src/core/builtin_modules.gen.inc` — 静态模块注册表
- `build/mod.load` — 动态模块清单

```bash
./configure
# 可选：选择自定义模块集合
./configure --with-modules=billing,dialplan,recording,endpoints
./configure --without-modules=asr_tts,presence
```

### 2.3 编译

```bash
make -j$(nproc)        # 构建 nsbcd 以及动态 .so 模块
make check             # 单元测试（需要测试源码）
sudo make install      # 安装到 /usr/local
```

使用清理器构建（用于调试）：

```bash
make SANITIZE=asan -j$(nproc)     # AddressSanitizer
make SANITIZE=tsan -j$(nproc)     # ThreadSanitizer
make SANITIZE=ubsan -j$(nproc)    # UndefinedBehaviorSanitizer
```

关键编译期开关（`NSBC_WITH_*`，自动检测，可在 `./configure` 中覆盖）：

| 开关 | 默认值 | 用途 |
|--------|---------|---------|
| `NSBC_WITH_OPENSSL` | 1 | SIP TLS + DTLS-SRTP（硬性要求 — 始终开启） |
| `NSBC_WITH_SRTP` | 1 | SRTP 媒体加密（与 OpenSSL 配对 — 始终开启） |
| `NSBC_WITH_BCG729` | 1 | G.729 Annex B 编解码器 |
| `NSBC_WITH_DLOPEN` | 1 | 通过 `dlopen()` 动态加载模块 |
| `NSBC_WITH_CURL` | 0 | `mod_ai_dialog` 中的真实 ASR/TTS（Whisper / ElevenLabs） |
| `NSBC_WITH_SQLITE` | 0 | **架构约束** — C 核心从不直接操作数据库。SQLite 仅在 Rust 旁车中使用。 |

### 2.4 构建 Web 旁车进程

```bash
cd web
cargo build --release
# 输出：target/release/nsbc-web
# 可选的 ACME 功能（公网 TLS）：
cargo build --features acme --release
```

Rust 旁车进程（`nsbc-web`）是一个独立进程，提供管理控制台并将配置存储在 SQLite/Postgres/MySQL 中。它通过 Unix 域套接字与 `nsbcd` 通信（`/var/run/nsbc/control.sock` 用于命令，`/var/run/nsbc/data.sock` 用于实时事件）。

---

## 3. 安装

```bash
sudo make install
# 安装内容：
#   /usr/local/sbin/nsbcd           — SIP 守护进程
#   /usr/local/sbin/nsbc_cli        — 交互式 CLI（可选）
#   /usr/local/lib/nsbc/mod_*.so    — 动态模块
#   /usr/local/etc/nsbc/nsbc.yaml   — 默认配置
```

手动部署（不使用 make install）：

```bash
# 典型目录结构
/opt/nsbc/
├── nsbcd
├── nsbc_cli
├── conf/
│   └── nsbc.yaml
├── web/
│   ├── nsbc-web
│   └── static/            # SPA 静态资源
├── tls/                   # cert.pem + key.pem（由 nsbc-web 自动生成）
├── logs/                  # nsbcd.log + audit.jsonl
├── run/                   # /var/run/nsbc/ 软链接至此或作为实际 UDS 目录
├── recordings/
└── data/snapshots/
```

创建运行用户（推荐）：

```bash
sudo useradd -r -s /usr/sbin/nologin nsbc
sudo mkdir -p /var/run/nsbc /var/log/nsbc /var/lib/nsbc
sudo chown -R nsbc:nsbc /var/run/nsbc /var/log/nsbc /var/lib/nsbc
```

---

## 4. 目录结构

| 目录 | 内容 | 说明 |
|-----------|----------|-------|
| `src/` | C 源码树 | 核心的所有 `.c` / `.h` 文件 |
| `src/core/` | 核心守护进程 | `main.c`、`reactor.c`、`module.c`、`cfg.c`、`signaling.c`、`event_bus.c`、`worker.c`、`cfg_crypto.c` |
| `src/sip/` | SIP 协议栈 | `sip_txn.c`、`sip_transport.c`、`sip_dialog.c`、`b2bua.c`、`sip_build.c`、`sip_event.c`、`stir_shaken.c` 等 |
| `src/media/` | 媒体管线 | `bridge/`、`crypto/`、`codec/`、`dsp/`、`rtp/`、`nat/`、`play/`、`rec/`、`mix/`、`video/` |
| `src/mod/` | 可选模块 | 每个子目录 → 一个 `mod_*.so`（或通过 configure 静态链接） |
| `src/parse/` | 解析器 | `sip/`、`sdp/`、`rtp/`、`xml/` |
| `src/oam/` | 运维维护 | 控制套接字、ESL 服务器、可观测性、重载命令 |
| `src/registrar/` | 注册服务 | `registrar.c`、`location_db.c`、`reg_enhance.c` |
| `src/net/` | 网络层 | 套接字池、TCP I/O 组、TLS、DNS 缓存、WebSocket 监听器 |
| `src/include/` | 公共头文件 | 每个主要子系统一个头文件 |
| `src/db/` | DBAL | **架构说明**：C 核心从不链接数据库。SQLite 仅由 Rust 旁车使用。 |
| `src/ha/` | 高可用 | 主备复制、健康检查 |
| `src/mem/` | 内存管理器 | Arena、slab、RCU deferral、环形缓冲区、内存池 |
| `src/base/` | 通用原语 | 时钟、哈希、ID、日志、定时器轮、trace ID |
| `web/src/` | Rust 旁车源码 | Axum + Tokio HTTP API + 嵌入式 SPA |
| `web/static/` | SPA 静态资源 | Vue 3 + Element Plus + Tailwind，打包进 `nsbc-web` |
| `conf/` | 运行时配置 | `nsbc.yaml`（nsbcd 与 nsbc-web 共享）、`modules.conf` |
| `configs/` | 默认核心配置 | 参考配置，所有键均有注释说明 |
| `examples/` | 代码示例 | C、Go、Python、Rust 事件客户端 |
| `3rd/` | Vendored 库 | bcg729、cJSON |
| `build/` | 构建产物 | 编译目标文件、动态模块、config.mk |
| `docs/` | 文档 | 架构说明、用户指南 |

---

## 5. 配置

### 5.1 共享 `conf/nsbc.yaml`

这是主要的运行时配置文件，放置在 `<deploy-root>/conf/nsbc.yaml`。`nsbcd` 和 `nsbc-web` 都会读取它，但会忽略不属于自身的配置段：

- **nsbcd 读取**：`log.*`、`net.*`、`sip.*`、`registrar.*`、`oam.*`、`ha.*`、`hep.*`、`mem.*`、`rtp.*`、`media.*`、`core.*`、`dialplan.*`、`recording.*`、`system.*`、`modules.*`
- **nsbc-web 读取**：`web.*`、`auth.*`、`db.*`、`audit.*`、`core.*`

所有相对路径均以包含该 YAML 文件的目录（即 `conf/` 目录）为基准进行解析。因此 `conf/nsbc.yaml` 中的 `"../tls/cert.pem"` 将解析为 `<deploy-root>/tls/cert.pem`。这意味着整个部署目录树可以整体移动，而无需修改配置文件。

**`nsbcd` 的搜索顺序：**
1. 显式指定的 `-c <path>` 参数
2. `<binary-dir>/conf/nsbc.yaml`
3. `/etc/nsbc/nsbc.yaml`

**`nsbc-web` 的搜索顺序：**
1. 显式指定的 `-c <path>` 参数
2. `<binary-dir>/../conf/nsbc.yaml`
3. `<binary-dir>/conf/nsbc.yaml`
4. `./conf/nsbc.yaml`
5. `/etc/nsbc/nsbc.yaml`

### 5.2 核心默认 `configs/nsbc.yaml`

这是一份完整的参考配置，展示了每一个配置键及其注释说明，可在你需要了解所有可用选项时作为模板使用。**该文件通常不会直接复制到生产环境** —— 实际运行时使用的是部署根目录下的 `conf/nsbc.yaml`。

### 5.3 环境变量

| 变量 | 作用范围 | 用途 |
|----------|-------|---------|
| `NSBC_MASTER_KEY` | nsbcd | 解密 `enc:` 前缀密钥的主密钥 |
| `NSBC_DEFAULT_ADMIN_PW` | nsbc-web | 覆盖默认管理员密码（数据库为空时） |
| `NSBC_RESET_ADMIN` | nsbc-web | 设为 `1` 可将管理员密码重置为 `NSBC_DEFAULT_ADMIN_PW` 或硬编码默认值 |
| `NSBC_DB_DSN` | nsbc-web | 覆盖 YAML 中的 `db.dsn` |
| `NSBC_NO_DB_MODULE=1` | 编译期 | 架构约束 — 永远不将数据库链接到 C 核心 |

### 5.4 免安装运行

```bash
# 先完成构建（见第 2 节）
cd /path/to/nsbc

# 先启动 Rust web 旁车（如配置了自动生成，则它会生成 TLS 证书）
cd web && ./target/release/nsbc-web &
sleep 2

# 启动 C SIP 守护进程
cd .. && ./nsbcd -c conf/nsbc.yaml
```

---

## 6. 启动 NSBC

### 6.1 启动顺序

1. **先启动 Rust 旁车** — 启动 TLS 证书自动生成，监听 8443 端口。
2. **再启动 C 守护进程** — 读取旁车刚刚生成的 TLS 证书。

两个进程共享同一个 `conf/nsbc.yaml` 和同一套 TLS 证书。旁车还会监听来自 Web 控制台的 SIGHUP 信号，触发配置热重载。

### 6.2 systemd 单元（示例）

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

### 6.3 优雅重启

向 nsbcd 进程发送 `SIGHUP` 信号触发配置热重载：

```bash
kill -HUP $(pidof nsbcd)
# 或通过 CLI：
nsbc_cli control config.reload
```

以下配置键支持**热重载**（在参考配置中标记为 `[hot]`）：

- `log.level`
- `net.rcvbuf_kb`、`net.sndbuf_kb`、`net.tcp_conn_max`、`net.dns_ttl_s`
- `sip.t1_ms`、`sip.t2_ms`、`sip.t4_ms`、`sip.timer_d_ms`、`sip.udp_cl_strict`、`sip.user_agent`
- `rtp.dscp`、`rtp.ssrc_grace_ms`、`rtp.rtcp_interval_ms`
- `media.default_mode`、`media.allow_bypass`、`media.trust_private`
- `registrar.min_expires`、`registrar.default_expires`、`registrar.max_expires`、`registrar.max_contacts`、`registrar.rps_*`
- `oam.metrics_enabled`、`oam.metrics_bind`
- `ha.heartbeat_ms`、`ha.failover_ms`

不支持热重载的配置键需要完整进程重启。

---

## 7. Web 管理控制台

### 7.1 默认登录

- 用户名：`admin`
- 密码：**随机生成** — 通过 `NSBC_DEFAULT_ADMIN_PW` 环境变量设置，或在用户表为空时使用硬编码后备值 `Admin123`
- Realm：与您的租户匹配（或使用默认值 `nsbc.local`）

首次登录后，请立即在控制台中（安全 → 用户）**修改管理员密码**，或清空数据库并设置 `NSBC_RESET_ADMIN=1`。

### 7.2 TLS 证书模式

支持三种证书来源（按优先级自动选择）：

| 模式 | 启用方式 | 适用场景 |
|------|---------------|----------|
| **预置证书** | 将 `cert_path` 和 `key_path` 指向您自己的证书文件 | 自带证书（Let's Encrypt 手动、内部 CA） |
| **自签名（自动生成）** | `auto_generate: true` | 内网、测试环境 — 证书每小时轮换 |
| **ACME Let's Encrypt** | `public: true` + 域名 | 公网部署 — 需要将 DNS A 记录指向本机 |

```yaml
web:
  tls:
    enabled: true
    cert_path: "../tls/cert.pem"
    key_path:  "../tls/key.pem"
    # 自签名（内部 / 实验环境）
    auto_generate: true
    validity_days: 2
    # 或公网 ACME（生产环境）
    public: true
    domain: "sip.example.com"
    acme_email: "ops@example.com"
```

### 7.3 实时监控

Web 控制台提供以下实时面板：

- **总览** — CPS、活动呼叫、失败呼叫、注册数
- **活动呼叫** — 每路的状态、编解码器、RTP 统计、MOS 评分
- **呼叫记录** — 带呼叫和下载录音功能的 CDR
- **注册信息** — 分机在线/离线状态、剩余过期时间
- **安全中心** — 动态封禁列表、围栏规则、全局配置
- **指标** — Prometheus 抓取端点显示

数据通过 `data.sock` 使用广播通道从 nsbcd → nsbc-web 传输，默认轮询间隔为 5 秒。

---

## 8. 模块系统

NSBC 采用借鉴自 FreeSWITCH 的两级模块系统：

### 8.1 内置（静态链接）

静态表中的模块会被直接编译进 `nsbcd`。configure 脚本基于调用图分析来确定哪些模块是必需的：

- **必需模块**（不可禁用）：`signaling`、`dbal`、`oam`、`endpoints`、`agent_pool`、`queue`
- **硬性静态链接**（被 `b2bua.c` 直接调用 — 若缺失链接器会报错）：`billing`、`dialplans`、`outbound_reg`、`recording`、`applications`、`ha`、`media_bug`、`security`

### 8.2 动态（mod_*.so）

动态加载的模块在启动时通过 `dlopen()` 从 `<prefix>/lib/nsbc/` 或 `modules.conf` 指定的路径加载。configure 脚本会将这些模块构建为 `.so`：

`ai_dialog`、`asr_tts`、`av`、`cachedb`、`codecs`、`event_handlers`、`formats`、`hello`、`playback`、`presence`

动态模块可单独选择或排除：

```bash
./configure --without-modules=ai_dialog,asr_tts,hello
```

### 8.3 运行时启用/禁用

`conf/nsbc.yaml` 支持两个运行时控制项（`SIGHUP` 后热重载生效）：

```yaml
modules:
  # enable:        # 白名单 — 仅加载列出的模块（必需模块强制加载）
  #   - signaling
  #   - oam
  disable:          # 黑名单 — 跳过这些模块
    - hello
    - asr_tts
```

若同时存在 `enable` 和 `disable`，以 `enable` 为准。

### 8.4 编写自定义模块

1. 创建 `src/mod/my_module/my_module.c`
2. 实现标准描述符：

```c
#include "module.h"

static int my_start(nsbc_modmgr_t *m);
static int my_stop(nsbc_modmgr_t *m);
static int my_reload(nsbc_modmgr_t *m);

NSBC_MOD_DEFINE(my_module,
    .version  = "1.0.0",
    .init_level = MOD_L_FINAL,
    .deps     = NULL,          // 无依赖
    .start    = my_start,
    .stop     = my_stop,
    .reload   = my_reload,
);
```

3. 运行 `./configure` — 模块会被自动识别。
4. `make` 会将其同时构建为静态和动态两种形式。

---

## 9. 拨号计划

NSBC 具有轻量级的 YAML 拨号计划，以及 Web 控制台中结构化的 JSON 动作构建器。拨号计划规则匹配入呼被叫号码模式（以及可选的主叫号码模式），并执行一串路由 / 媒体 / 录音 / 号码变换动作。

### 9.1 规则格式

```yaml
dialplan:
  rules: "alice:0:alice|bob:0:bob|^[0-9]+$:0:$0"
```

紧凑格式：`called_pattern:priority:target` — 以竖线分隔。或者，使用 Web 控制台 → 拨号计划规则 进行结构化编辑。

### 9.2 拨号计划动作

在 Web UI 中为每条规则配置的常用动作：

| 动作 | 说明 |
|--------|-------------|
| 路由到分机 | 转发到内部分机（`2001`） |
| 路由到中继 | 转发到外部 SIP 中继（`trunk = ACME-IMS`） |
| 路由到组 | 按策略转发到网关组（`least_busy`） |
| 播放 IVR | 路由到 IVR 菜单 |
| 开始录音 | 开始录音（`mode=mixed`、`format=wav`） |
| 号码剥离 | 在路由前去掉前 N 位数字 |
| 被叫号覆盖 | 用捕获组替换被叫号码 |
| SIP 头重写 | 设置 `sip_from_display`、`sip_from_uri`、R-URI、Contact 参数 |
| IMS PAI/PPID/PCPI 注入 | 自动构建 IMS 身份头 |

时段路由：将规则与时间组结合（`Time Strategy` 菜单 → 创建一个周一至周五 09:00–18:00 的组，然后在规则中引用）。

---

## 10. 可观测性

### 10.1 Prometheus 指标

nsbcd 默认在 `127.0.0.1:9100` 暴露指标：

```bash
curl http://127.0.0.1:9100/metrics | head -40
```

关键指标：

| 指标 | 说明 |
|--------|-------------|
| `nsbc_active_calls` | 当前并发呼叫数 |
| `nsbc_calls_total` | 总呼叫尝试数 |
| `nsbc_calls_failed_total` | 失败呼叫数 |
| `nsbc_registered_extensions` | 在线注册数 |
| `nsbc_rtp_relayed_packets_total` | 桥接的 RTP 包数 |
| `nsbc_rtp_relayed_bytes_total` | 桥接的 RTP 字节数 |
| `nsbc_txn_active` | 活动的 SIP 事务数 |
| `nsbc_event_bus_queue_depth` | 事件总线待处理队列深度 |
| `nsbc_mem_arena_used_bytes` | 每个内存域的 arena 使用量 |
| `nsbc_recording_active` | 活动的录音数量 |
| `nsbc_ha_role` | HA 角色（0=备，1=主） |

### 10.2 控制套接字（UDS）

通过 Unix 域套接字进行运行时控制（默认 `/var/run/nsbc/control.sock`）：

```bash
nsbc_cli                              # 交互式 shell
nsbc_cli show calls                   # 列出活动呼叫
nsbc_cli show registrations           # 列出注册信息
nsbc_cli config.reload                # 配置热重载
nsbc_cli config.get log.level
nsbc_cli config.set log.level debug
nsbc_cli hangup <call-id>             # 挂断一个呼叫
nsbc_cli metrics                      # 指标快照
nsbc_cli security reload              # 重载安全规则
nsbc_cli help                         # 显示所有命令
```

### 10.3 事件总线 / 订阅

C 核心在事件总线上发布结构化事件，客户端可通过 ESL 或 data.sock 订阅。

核心事件类型：

| 事件 | 说明 |
|-------|-------------|
| `SESSION_CREATE` | 新建呼叫支路 |
| `SESSION_ANSWER` | 呼叫应答 |
| `SESSION_END` | 呼叫拆除 |
| `REGISTER_SUCCESS` | 分机注册成功 |
| `REGISTER_EXPIRE` | 注册过期 |
| `BAN_ADD` | IP 被封禁 |
| `BAN_REMOVE` | IP 被解除封禁 |
| `RECORDING_START` | 录音开始 |
| `RECORDING_STOP` | 录音停止 |
| `CFG_CHANGE` | 配置热重载完成 |

完整列表见 `src/include/event_bus.h`。

---

## 11. 事件客户端示例

可直接运行的事件客户端示例位于 `examples/event_client/`：

| 语言 | 文件 | 构建/运行 |
|----------|------|-----------|
| C | `c/event_client.c` | `gcc event_client.c -o event_client && ./event_client` |
| Go | `go/main.go` | `cd go && go run main.go` |
| Python | `python/event_client.py` | `python3 event_client.py` |
| Rust | `rust/src/main.rs` | `cd rust && cargo run` |

所有客户端都连接到 UDS 控制套接字并订阅 `SESSION_CREATE`、`SESSION_ANSWER`、`SESSION_END` 事件，实时打印到标准输出。

```text
[SUB] EVENT SESSION_CREATE call=abc123 leg=A from=sip:alice@nsbc.local to=sip:bob@nsbc.local
[SUB] EVENT SESSION_ANSWER call=abc123 leg=B codec=PCMU/8000
[SUB] EVENT SESSION_END   call=abc123 leg=A duration=42s cause=NORMAL_CLEARING
```

---

## 12. 故障排查

### 12.1 核心无法启动

```bash
# 检查日志
tail -f logs/nsbcd.log

# 常见原因：
# 1. 端口冲突（5060 已被占用） — 检查：ss -tulpn | grep 5060
# 2. /var/run/nsbc 或 /var/log/nsbc 权限不足
# 3. tls_enabled=true 时 TLS 证书缺失
# 4. libsrtp2/OpenSSL 未安装 — 用 ldd 检查：
ldd nsbcd | grep "not found"

# 校验配置语法
./nsbcd -c conf/nsbc.yaml --dry-run   # 若支持，否则查看日志前几行
```

### 12.2 未返回 401 质询

检查 registrar 模块是否已启用，以及 `authz` 模块是否未拦截请求：

```bash
# 用一次 REGISTER 尝试进行测试
sipsak -s sip:test@nsbc.local -M REGISTER

# 预期结果：
# 1. REGISTER → 401 并带有 WWW-Authenticate 头
# 2. REGISTER + Authorization → 200 OK
```

同时验证 `registrar.digest_secret` 配置一致，且 `security.blacklist.reload` 没有 catch-all deny 规则。

### 12.3 媒体未转发（单通语音）

```bash
# 检查配置中的媒体模式
grep media configs/nsbc.yaml
# default_mode 应为 'relay'（bypass 会完全跳过 SBC 转发）
# 对于内网对等呼叫，trust_private 应为 true

# 验证 RTP 端口范围可用
ss -ulpn | grep -E "10000|20000" | head
# 10000-59999 范围内不应有任何服务绑定

# 检查 NAT/外部 IP 配置
grep extern_ip configs/nsbc.yaml

# 监控活动呼叫的 RTP 桥接
nsbc_cli show calls      # 查看 ACodec/BCodec、RTP IP 列
nsbc_cli metrics         # 观察 nsb_rtp_relayed_packets_total
```

### 12.4 TLS/WS 无法工作

```bash
# 验证 TLS 证书路径存在且 PEM 格式正确
openssl x509 -in tls/cert.pem -text -noout | head
openssl rsa  -in tls/key.pem  -check -noout

# nsbcd 启动日志应显示 [tls] cert=... key=... — 检查是否有 "失败" 相关错误

# 使用 OpenSSL 客户端测试
openssl s_client -connect 127.0.0.1:5061 </dev/null

# WebSocket（端口 5062 上的 WSS）
openssl s_client -connect 127.0.0.1:5062 -alpn sip.wss </dev/null
```

---

## 许可证

Apache 2.0 — 见 [LICENSE](../LICENSE)。
