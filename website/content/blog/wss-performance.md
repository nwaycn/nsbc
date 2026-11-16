---
title: WSS 接入路径优化，单节点容量提升约 60%
title_en: Optimized WSS access path — single-node capacity up ~60%
date: 2026-09-30
summary: 重写 WebSocket 帧解析与缓冲管理，减少大帧场景的内存拷贝；4 核 8G 虚拟机上注册容量从约 6000 提升到约 9600。
summary_en: Rewrote WebSocket frame parsing and buffer management to reduce copies on large frames; registration capacity on a 4-core 8 GB VM rose from ~6000 to ~9600.
tags: 性能
tags_en: performance
---

WebRTC 客户端通过 WSS 接入时，SIP 报文被封装在 WebSocket 帧中。旧实现在大帧场景下会多次拷贝缓冲区，注册风暴时内存与 CPU 都会快速上升。

## 改动

- 帧解析改为按需扩容，避免为每条消息预留最大缓冲。
- 合并读缓冲与 SIP 解析之间的复制步骤。
- WSS 写路径按连接维度批处理，减少小包系统调用。

## 结果

在 4 核 8G 的虚拟机上，使用标准 REGISTER 流程压测：

| 指标 | 优化前 | 优化后 |
|---|---|---|
| WSS 注册容量 | ~6000 | ~9600 |
| 平均注册时延 | 4.1 ms | 2.6 ms |
| 峰值内存 | 1.9 GB | 1.5 GB |

压测方法与配置参考[配置手册](/handbook)，`core.reactors` 建议设为 2，`mem.signal_mb` 按呼叫量调整。

<!-- en -->

When WebRTC clients connect over WSS, SIP messages are wrapped in WebSocket frames. The previous implementation copied buffers multiple times on large frames, causing memory and CPU usage to spike during registration storms.

## Changes

- Frame parsing now grows buffers on demand instead of pre-allocating the maximum size for every message.
- Merged copy steps between the read buffer and the SIP parser.
- Batched the WSS write path per connection to reduce small-packet syscalls.

## Results

Benchmarked with the standard REGISTER flow on a 4-core 8 GB VM:

| Metric | Before | After |
|---|---|---|
| WSS registration capacity | ~6000 | ~9600 |
| Average registration latency | 4.1 ms | 2.6 ms |
| Peak memory | 1.9 GB | 1.5 GB |

For the benchmark methodology and configuration see the [configuration handbook](/handbook). `core.reactors` is recommended to be set to 2; tune `mem.signal_mb` according to your call volume.
