---
title: NSBC 0.5.6-beta 发布
title_en: NSBC 0.5.6-beta released
date: 2026-11-10
summary: 当前 beta 版本：SIP 注册与代理、Digest 鉴权、RTP/RTCP 中继、SRTP 与 WebRTC 接入，以及三端软电话 SDK。
summary_en: The current beta brings SIP registration and proxying, Digest authentication, RTP/RTCP relay, SRTP and WebRTC access, plus softphone SDKs for three platforms.
tags: 版本发布
tags_en: release
---

0.5.6-beta 是 NSBC 当前的 beta 版本。它把 SBC、PBX 与 SIP 代理三类能力放在同一套 C 核心中，对外只需部署 `nsbcd` 与 `nsbc-web` 两个进程。

## 本版本包含

- **信令**：SIP 注册与代理、Digest 鉴权、注册保活、B2BUA 拆腿。
- **媒体**：RTP/RTCP 中继、SRTP 与 DTLS-SRTP、按需转码。
- **管理面**：注册、会话与 CDR 查询，REST API 与 `nsbc_cli`。
- **客户端**：Android、Desktop 与 iOS 三端软电话 SDK。

三端 SDK 在 Via、Contact、`Session-ID` 与 SDP 编解码策略上保持一致，可作为集成参考。

## 升级建议

新部署请先从源码构建，按[快速开始](/docs/quickstart)完成第一次注册与互拨；生产环境请把 `registrar.force_register_ok` 改回 `0`，并为分机设置密码。

<!-- en -->

0.5.6-beta is the current beta release of NSBC. It unifies SBC, PBX and SIP proxy capabilities into a single C core; only two processes — `nsbcd` and `nsbc-web` — need to be deployed externally.

## What's in this release

- **Signaling**: SIP registration and proxying, Digest authentication, registration keepalive, B2BUA leg splitting.
- **Media**: RTP/RTCP relay, SRTP and DTLS-SRTP, on-demand transcoding.
- **Control plane**: Registration, session and CDR queries, REST API and `nsbc_cli`.
- **Clients**: Softphone SDKs for Android, Desktop and iOS.

The three SDKs share the same Via, Contact, `Session-ID` and SDP codec strategy, and can serve as integration references.

## Upgrade notes

For a fresh deployment build from source first, then complete your first registration and test call following the [Quick Start](/en/docs/quickstart). In production set `registrar.force_register_ok` back to `0` and assign passwords to each extension.
