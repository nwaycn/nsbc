---
title: 与主流 PBX 完成互通测试
title_en: Interop tests completed with mainstream PBXes
date: 2026-08-18
summary: 完成与多款开源 PBX 的注册、互拨与媒体中继测试，配置样例已合并进文档。
summary_en: Registration, cross-calling and media relay tests completed with several open-source PBXes; configuration examples have been merged into the docs.
tags: 生态
tags_en: ecosystem
---

我们完成了 NSBC 与多款开源 PBX 的互通测试，覆盖注册、互拨、媒体中继与挂断释放。

## 测试内容

- 分机注册与刷新：确认 Contact 改写与过期清理正常。
- 域内互拨：验证拨号计划命中与主叫号码透传。
- 媒体中继：双向音频、DTMF 与静音抑制。
- 异常释放：被叫忙、无应答与主叫提前挂断。

## 已知差异

不同 PBX 对 SDP 属性顺序与会话定时器实现不同，个别设备会忽略 `a=ptime`。NSBC 默认按能力取交集，不做强制改写；如需收紧，可在 SIP profile 中裁剪编解码。

配置样例已合并进[配置手册](/handbook)的典型场景章节。

<!-- en -->

We have finished interop tests between NSBC and several open-source PBXes, covering registration, cross-calling, media relay and teardown.

## What we tested

- Extension registration and refresh — Contact rewriting and expiry cleanup were verified to work correctly.
- In-domain calls — dial-plan matching and caller-ID transparency validated.
- Media relay — two-way audio, DTMF and silence suppression.
- Abnormal teardown — callee busy, no answer, and caller early hangup.

## Known differences

PBXes vary in SDP attribute ordering and session timer behavior, and some devices ignore `a=ptime`. NSBC defaults to taking the capability intersection without forcing rewrites; if you need a tighter set, constrain the codecs in the SIP profile.

Configuration examples have been merged into the typical-scenarios chapter of the [configuration handbook](/handbook).
