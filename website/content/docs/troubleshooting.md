---
title: 常见故障排查
title_en: Troubleshooting
order: 50
date: 2026-11-16
summary: 注册失败、呼叫 4xx、单向媒体、ICE 失败与自动封禁的处理顺序。
summary_en: Step-by-step handling for registration failures, 4xx call errors, one-way media, ICE failures and auto-bans.
tags: 排障
tags_en: troubleshooting
---

排障的第一步是分清“信令”还是“媒体”：能振铃说明 SIP 路径正常，问题通常落在 SDP 地址、端口放行或编解码。

## 注册失败

| 现象 | 可能原因 | 处理 |
|---|---|---|
| 401 循环 | 密码错误或 realm 不匹配 | 核对分机密码与 `realm` |
| 403 | 分机不存在、被禁用或达到 binding 上限 | 检查分机状态与 `max_registrations_per_ext` |
| 423 | 请求的有效期低于 `min_expires` | 提高客户端 Expires 或调整下限 |
| 超时无响应 | 端口未放行或服务未启动 | 检查 5060/5062 与进程状态 |

短时间内多次认证失败会触发自动封禁（默认 30 秒内 3 次，封禁 1 小时）。确认是误封后，在 Web 控制台解除或临时关闭 `security.auto_ban`。

## 呼叫失败

- **404 Not Found**：被叫未注册，或拨号计划未命中。
- **488 / 415**：双方编解码无交集，检查 SDP 与转码配置。
- **486 Busy**：被叫忙或并发上限已到。
- **CANCEL 后仍振铃**：检查事务与对话释放日志。

## 媒体问题

| 现象 | 排查方向 |
|---|---|
| 接通无声 | SDP 中 `c=` 是否为可达地址；媒体端口是否放行 |
| 单向音频 | 呼入时是否正确复用 offer 的 m-line；ICE 是否完成 |
| 声音断续 | 丢包、抖动缓冲、带宽；查看 RTCP 统计 |
| DTMF 无效 | 双方事件方式不一致，统一为 RFC 4733 或 INFO |

## ICE 与 DTLS

- ICE failed：候选不足或 UDP 被拦；配置 STUN / TURN 后重试。
- DTLS 握手失败：检查 `a=fingerprint` 是否被中间设备改写。
- 媒体中断后不恢复：确认客户端只调用一次 `restartIce()`。

## 观测点

1. `logs/nsbcd.log`：按 `trace_id` 串联一次呼叫的全部事务。
2. Web 控制台：注册状态、活动会话、CDR 与恶意呼叫标记。
3. Prometheus：注册数、活跃呼叫、失败原因分布。
4. 抓包：`tcpdump -i any -n port 5060 or portrange 20000-30000`。

<!-- en -->

The first step in troubleshooting is to separate "signaling" from "media": if the phone rings, the SIP path is fine; the problem usually lies in SDP addresses, open ports or codecs.

## Registration failures

| Symptom | Possible cause | Action |
|---|---|---|
| 401 loop | wrong password or realm mismatch | verify extension password and `realm` |
| 403 | extension missing, disabled, or at binding limit | check extension status and `max_registrations_per_ext` |
| 423 | requested expiry below `min_expires` | raise client Expires or lower the floor |
| timeout, no response | port blocked or service not running | check 5060/5062 and process state |

Multiple auth failures in a short window trigger the auto-ban (default: 3 strikes in 30 seconds → 1-hour ban). Once you confirm it is a false positive, unban through the web console or temporarily set `security.auto_ban` to `false`.

## Call failures

- **404 Not Found**: callee not registered, or dial plan missed.
- **488 / 415**: no codec intersection — inspect SDP and transcoding config.
- **486 Busy**: callee busy or concurrency cap reached.
- **rings after CANCEL**: check transaction and dialog teardown logs.

## Media issues

| Symptom | What to check |
|---|---|
| silent after connect | SDP `c=` reachable? media ports open? |
| one-way audio | inbound call reuses offer's m-line correctly? ICE completed? |
| choppy audio | packet loss, jitter buffer, bandwidth — inspect RTCP stats |
| DTMF not working | event methods differ — standardize on RFC 4733 or INFO |

## ICE and DTLS

- ICE failed: insufficient candidates or UDP blocked; retry after configuring STUN / TURN.
- DTLS handshake failed: verify `a=fingerprint` was not rewritten by an intermediary.
- Media never recovers after outage: confirm the client calls `restartIce()` only once.

## Where to look

1. `logs/nsbcd.log` — correlate every transaction of a single call by `trace_id`.
2. Web console — registration state, active sessions, CDRs and malformed-call flags.
3. Prometheus — registrations, active calls, failure-reason breakdown.
4. Packet capture: `tcpdump -i any -n port 5060 or portrange 20000-30000`.
