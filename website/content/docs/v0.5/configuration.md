---
title: 配置文件结构
title_en: Configuration file structure
order: 20
date: 2026-11-16
summary: nsbcd 与 nsbc-web 共用一份 YAML；理解相对路径、核心段与热重载范围。
summary_en: nsbcd and nsbc-web share a single YAML; understand relative paths, the main sections and what can be hot-reloaded.
tags: 配置
tags_en: configuration
---

`nsbcd` 与 `nsbc-web` 读取同一份 `conf/nsbc.yaml`，各自只解析自己关心的段，未知键会被忽略。

## 路径约定

YAML 中的所有相对路径都以**该文件所在目录**为基准。例如 `../tls/cert.pem` 实际指向部署根目录下的 `tls/cert.pem`，整个部署目录可以直接搬移。

## 主要段落

| 段落 | 读取方 | 作用 |
|---|---|---|
| `core` | 双方 | worker 数、reactor 数、最大呼叫数、定时器精度 |
| `mem` | nsbcd | 信令、报文、媒体、转码、录音五域内存上限 |
| `log` | nsbcd | 日志级别与路径 |
| `net` | nsbcd | SIP UDP/TCP/TLS 端口与证书 |
| `sip` | nsbcd | 监听地址与 WebSocket 接入 |
| `registrar` | nsbcd | 注册有效期、鉴权开关、同一 AoR 的注册策略 |
| `security` | nsbcd | 限速、自动封禁、GeoIP 白名单 |
| `recording` | nsbcd | 录音开关、策略、格式与并发上限 |
| `billing` | nsbcd | 计费开关；关闭时相关路径全部旁路 |
| `web` | nsbc-web | HTTPS 监听、TLS 证书来源 |
| `db` / `auth` | nsbc-web | 数据库与登录鉴权 |

## 最小示例

```yaml
core:
  workers: 1
  reactors: 2
  max_calls: 2000

log:
  level: info
  path: "../logs/nsbcd.log"

net:
  sip_udp_port: 5060
  sip_tcp_port: 5060
  sip_tls_port: 5061
  tls_enabled: true
  tls_cert: "../tls/cert.pem"
  tls_key: "../tls/key.pem"

sip:
  bind: "0.0.0.0:5060"
  ws:
    enabled: true
    port: 5062
    tls: true

registrar:
  force_register_ok: 0
  min_expires: 60
  default_expires: 600
  max_expires: 3600
```

## 注册策略

- `force_expires`：大于 0 时强制覆盖客户端请求的有效期，适合公网部署。
- `expires_jitter_s`：在强制值上下随机抖动，避免整点注册风暴。
- `multiple_registrations`：是否允许同一分机多终端同时在线。
- `max_registrations_per_ext`：配合上一项限制单分机 binding 数量。

## 热重载

修改配置后执行 `nsbc_cli config.reload` 或发送 `SIGHUP`。日志级别、注册策略、拨号计划与安全规则可热加载；监听端口与内存上限的变更需要重启进程。

<!-- en -->

`nsbcd` and `nsbc-web` both read `conf/nsbc.yaml`. Each process only parses the sections it cares about; unknown keys are silently ignored.

## Path convention

All relative paths in the YAML are resolved relative to **the directory containing the YAML file**. For example, `../tls/cert.pem` actually points to `tls/cert.pem` under the deployment root, so the whole deployment directory can be moved as a unit.

## Main sections

| Section | Read by | Purpose |
|---|---|---|
| `core` | both | worker count, reactor count, max calls, timer resolution |
| `mem` | nsbcd | memory limits for signaling, messages, media, transcoding and recording |
| `log` | nsbcd | log level and path |
| `net` | nsbcd | SIP UDP/TCP/TLS ports and certificates |
| `sip` | nsbcd | bind address and WebSocket access |
| `registrar` | nsbcd | registration expiry, auth toggle, per-AoR registration policy |
| `security` | nsbcd | rate limits, auto-ban, GeoIP whitelist |
| `recording` | nsbcd | recording toggle, policy, format and concurrency limit |
| `billing` | nsbcd | billing toggle; related paths are bypassed when off |
| `web` | nsbc-web | HTTPS listener, TLS certificate source |
| `db` / `auth` | nsbc-web | database and login authentication |

## Minimal example

```yaml
core:
  workers: 1
  reactors: 2
  max_calls: 2000

log:
  level: info
  path: "../logs/nsbcd.log"

net:
  sip_udp_port: 5060
  sip_tcp_port: 5060
  sip_tls_port: 5061
  tls_enabled: true
  tls_cert: "../tls/cert.pem"
  tls_key: "../tls/key.pem"

sip:
  bind: "0.0.0.0:5060"
  ws:
    enabled: true
    port: 5062
    tls: true

registrar:
  force_register_ok: 0
  min_expires: 60
  default_expires: 600
  max_expires: 3600
```

## Registration policy

- `force_expires`: when greater than 0, overrides the client-requested expiry; useful for public deployments.
- `expires_jitter_s`: random jitter around the forced value to avoid top-of-minute registration storms.
- `multiple_registrations`: whether multiple terminals of the same extension may be online simultaneously.
- `max_registrations_per_ext`: pair with the previous item to cap bindings per extension.

## Hot reload

After changing the config run `nsbc_cli config.reload` or send `SIGHUP`. Log level, registration policy, dial plan and security rules are hot-reloadable; changes to listening ports or memory limits require a process restart.
