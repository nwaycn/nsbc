---
title: 三端软电话完成 SIP 与媒体行为对齐
title_en: Three softphone SDKs aligned on SIP and media behavior
date: 2026-10-22
summary: 统一注册刷新、OPTIONS 心跳、Via rport 与 Session-ID，并修正呼入场景复用远端 m-line，避免单向媒体。
summary_en: Unified registration refresh, OPTIONS keepalive, Via rport and Session-ID, and fixed the inbound reuse of remote m-lines to avoid one-way audio.
tags: 客户端
tags_en: client
---

本次更新覆盖 Android、Desktop 与 iOS 三端 SDK，重点是让三端在同一套 NSBC 配置下表现一致。

## 信令

- 统一注册刷新时机为 `Expires / 2`。
- OPTIONS 保活间隔 30 秒，连续 3 次失败触发重新注册。
- Via 统一携带 `;rport`，并生成 RFC 7989 `Session-ID`。

## 媒体

修正了呼入场景下错误新建 transceiver 的问题：`setRemoteDescription(offer)` 之后应按媒体类型查找已有 transceiver 再绑定轨道，否则 answer 不会包含新建的 m-line，表现为单向媒体。

编解码方面，三端统一将 H.264 裁剪为 Baseline（`profile-level-id=42e01f`、`packetization-mode=1`），Opus 限定 `maxplaybackrate=8000`，提升与老旧终端的互通成功率。

## 验证

使用 `client/threeway_test` 的互呼脚本可以复现六对组合的注册与呼叫流程，并校验 SDP 中的 H.264 与 Opus 参数。

<!-- en -->

This update covers the Android, Desktop and iOS SDKs, with the goal of making all three behave consistently under the same NSBC configuration.

## Signaling

- Registration refresh is unified at `Expires / 2`.
- OPTIONS keepalive interval is 30 seconds; three consecutive failures trigger a re-registration.
- Via always carries `;rport` and RFC 7989 `Session-ID`.

## Media

Fixed an issue where a new transceiver was incorrectly created in the inbound call path: after `setRemoteDescription(offer)` the client should look up an existing transceiver by media kind before attaching a track, otherwise the answer will not contain the newly created m-line, resulting in one-way audio.

On the codec side, all three SDKs now constrain H.264 to Baseline (`profile-level-id=42e01f`, `packetization-mode=1`) and limit Opus to `maxplaybackrate=8000`, improving interoperability with legacy endpoints.

## Verification

The `client/threeway_test` call script reproduces the registration and call flow for all six pairwise combinations and validates the H.264 and Opus parameters in the SDP.
