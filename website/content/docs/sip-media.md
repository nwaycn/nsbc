---
title: SIP 与媒体
title_en: SIP and media
order: 30
date: 2026-11-16
summary: WebSocket 接入、SDP 编解码策略、NAT 改写与媒体中继的选择。
summary_en: WebSocket access, SDP codec policy, NAT rewriting and the choice between media relay and bypass.
tags: SIP, 媒体
tags_en: SIP, media
---

## SIP over WebSocket

WebRTC 客户端不直接使用 UDP 5060，而是通过 WSS 承载 SIP：

```yaml
sip:
  ws:
    enabled: true
    port: 5062
    tls: true
```

- 连接地址：`wss://<host>:5062/ws`
- 子协议：`sip`
- 每条 WebSocket 文本帧装载一条完整 SIP 报文

SBC 侧会补齐 `Via` 的 `rport`、`Contact` 与 RFC 7989 `Session-ID`，便于把两段会话关联起来。

## SDP 编解码策略

| 媒体 | 参数 | 说明 |
|---|---|---|
| H.264 | `profile-level-id=42e01f` | 裁剪为 Baseline，兼容浏览器与老旧话机 |
| H.264 | `packetization-mode=1` | 缺失时补齐，避免部分终端拒绝 |
| Opus | `maxplaybackrate=8000` | 与 PSTN 侧采样率对齐，减少转码压力 |
| G.711 | PCMU / PCMA | 默认保留，作为兜底编解码 |

Offer/Answer 阶段只做交集裁剪，不强制改写 answer 的 profile；需要转码时启用对应模块。

## NAT 与地址改写

每个 SIP profile 可以配置独立的对外地址：

```yaml
# 概念示意，实际键位于 profile 配置中
external_sip_ip: "203.0.113.10"
external_rtp_ip: "203.0.113.10"
rtp_port_start: 20000
rtp_port_end: 30000
```

- `external_sip_ip`：写入 Via、Contact 与 Record-Route。
- `external_rtp_ip`：写入 SDP 的 `c=` 行。
- `rtp_port_start/end`：该 profile 的媒体端口段，便于防火墙放行。

## 中继与直通

| 模式 | 路径 | 适用 |
|---|---|---|
| `relay` | 终端 → SBC → 终端 | 公网接入、WebRTC、需要录音或统计 |
| `bypass` | 终端 → 终端 | 内网可信对等，追求低延迟 |

WebRTC 接入应使用 `relay`：只有媒体经过 SBC，DTLS-SRTP 才能被终结，质量指标与 CDR 才有数据。

## 注册与保活

- 客户端在 `Expires / 2` 时刷新注册。
- `OPTIONS` 可作为在线探测，建议 30 秒一次。
- 连续 3 次失败后重新注册；服务端按 `max_expires` 清理过期 binding。

<!-- en -->

## SIP over WebSocket

WebRTC clients do not use UDP 5060 directly; SIP is carried over WSS instead:

```yaml
sip:
  ws:
    enabled: true
    port: 5062
    tls: true
```

- Endpoint: `wss://<host>:5062/ws`
- Subprotocol: `sip`
- Each WebSocket text frame carries exactly one complete SIP message

The SBC side fills in `Via` `rport`, `Contact` and RFC 7989 `Session-ID` so the two call legs can be correlated.

## SDP codec policy

| Media | Parameter | Notes |
|---|---|---|
| H.264 | `profile-level-id=42e01f` | constrain to Baseline for browser and legacy-phone compatibility |
| H.264 | `packetization-mode=1` | add when missing to avoid rejection by some endpoints |
| Opus | `maxplaybackrate=8000` | align with PSTN sampling rate, reduce transcoding load |
| G.711 | PCMU / PCMA | kept as default fallback codec |

During Offer/Answer NSBC only intersects capabilities and does not force-rewrite the answer profile; enable the transcoding module when conversion is needed.

## NAT and address rewriting

Each SIP profile can have its own externally visible addresses:

```yaml
# conceptual — the actual keys live inside the profile block
external_sip_ip: "203.0.113.10"
external_rtp_ip: "203.0.113.10"
rtp_port_start: 20000
rtp_port_end: 30000
```

- `external_sip_ip` is written into Via, Contact and Record-Route.
- `external_rtp_ip` is written into the SDP `c=` line.
- `rtp_port_start/end` defines the media port range for that profile, which you can punch through the firewall.

## Relay vs. bypass

| Mode | Path | Use case |
|---|---|---|
| `relay` | endpoint → SBC → endpoint | public access, WebRTC, recording or analytics required |
| `bypass` | endpoint → endpoint | trusted in-network peers, minimal latency |

WebRTC access **must** use `relay`: only when media traverses the SBC can DTLS-SRTP be terminated, and only then do quality metrics and CDRs have data to record.

## Registration and keepalive

- Clients refresh their registration at `Expires / 2`.
- `OPTIONS` serves as an availability probe; every 30 seconds is recommended.
- Three consecutive probe failures trigger a re-registration; the server cleans up expired bindings based on `max_expires`.
