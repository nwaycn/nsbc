---
title: 快速开始
title_en: Quick start
order: 10
date: 2026-11-16
summary: 从源码构建 nsbcd 与 nsbc-web，启动后完成第一次注册与互拨。
summary_en: Build nsbcd and nsbc-web from source, then complete your first registration and test call.
tags: 入门
tags_en: getting started
---

## 环境要求

- Linux x86_64（生产环境建议），内核支持 epoll。
- 构建工具：`gcc` 或 `clang`、`make`、`pkg-config`、`cmake`。
- Rust 工具链：用于构建 Web 控制台边车 `nsbc-web`。

## 获取源码

```bash
git clone https://github.com/nwaycn/nsbc.git
# 国内镜像
git clone https://gitee.com/nwaycn/nsbc.git
cd nsbc
```

## 构建

```bash
# C 核心
make -j$(nproc)

# Rust 边车（会把二进制与 static/ 复制到上级目录）
cd web
make vendor
make build-acme
cd ..
```

## 启动

必须先启动 `nsbc-web`：它会在首次运行时生成 SQLite 数据库与自签名 TLS 证书，`nsbcd` 只读取这些文件。

```bash
./nsbc-web &
sleep 2
./nsbcd -c conf/nsbc.yaml &
```

## 端口

| 端口 | 协议 | 用途 |
|---|---|---|
| 5060 | UDP / TCP | SIP |
| 5061 | TLS | SIP over TLS |
| 5062 | WSS | SIP over WebSocket（WebRTC） |
| 443 | HTTPS | Web 控制台与 REST API |
| 9100 | HTTP | Prometheus 指标（默认仅本机） |

## 第一次呼叫

1. 打开 `https://<服务器地址>/`，接受自签名证书。
2. 创建两个分机，例如 `1001` 与 `1002`。
3. 用软电话或 WebRTC 客户端注册，互相拨打。
4. 在 Web 控制台查看注册、会话与 CDR。

> 排障阶段可临时设置 `registrar.force_register_ok: 1` 跳过注册鉴权；验证完成后务必改回 `0`。

<!-- en -->

## Prerequisites

- Linux x86_64 (recommended for production) with epoll support in the kernel.
- Build tools: `gcc` or `clang`, `make`, `pkg-config`, `cmake`.
- Rust toolchain — required to build the web console sidecar `nsbc-web`.

## Get the source

```bash
git clone https://github.com/nwaycn/nsbc.git
cd nsbc
```

## Build

```bash
# C core
make -j$(nproc)

# Rust sidecar (copies the binary and static/ into the parent directory)
cd web
make vendor
make build-acme
cd ..
```

## Start

You must start `nsbc-web` first: on its first run it creates the SQLite database and self-signed TLS certificates that `nsbcd` later reads.

```bash
./nsbc-web &
sleep 2
./nsbcd -c conf/nsbc.yaml &
```

## Ports

| Port | Protocol | Purpose |
|---|---|---|
| 5060 | UDP / TCP | SIP |
| 5061 | TLS | SIP over TLS |
| 5062 | WSS | SIP over WebSocket (WebRTC) |
| 443 | HTTPS | Web console and REST API |
| 9100 | HTTP | Prometheus metrics (localhost only by default) |

## Your first call

1. Open `https://<server-address>/` and accept the self-signed certificate.
2. Create two extensions, e.g. `1001` and `1002`.
3. Register with a softphone or WebRTC client and call each other.
4. Inspect registrations, sessions and CDRs in the web console.

> During troubleshooting you can temporarily set `registrar.force_register_ok: 1` to skip registration auth; remember to revert it to `0` once you are done.
