---
title: WebRTC 与 SIP 互通
title_en: WebRTC ↔ SIP interoperability
order: 40
date: 2026-11-16
summary: WSS 信令、DTLS-SRTP 终结、ICE 与编解码转换的关键配置与自测清单。
summary_en: Key configuration and a self-test checklist for WSS signaling, DTLS-SRTP termination, ICE and codec conversion.
tags: WebRTC, 互通
tags_en: WebRTC, interoperability
---

浏览器与 SIP 终端不会直接对话，而是各自与 SBC 建立一段独立会话。

## 四类转换

| 平面 | WebRTC 侧 | SBC 处理 | SIP 侧 |
|---|---|---|---|
| 信令 | WSS + 子协议 `sip` | 还原 SIP，补 rport / Session-ID | UDP / TCP / TLS |
| 会话描述 | ICE 候选、DTLS 指纹、BUNDLE | 终结 ICE，改写 `c=` / `m=` | 普通 SDP |
| 媒体 | 强制 DTLS-SRTP | 浏览器侧终结 DTLS，SIP 侧按策略加密 | SRTP 或 RTP |
| 编解码 | Opus、VP8、H.264 Baseline | 取交集，必要时转码 | PCMU / PCMA / G.722 |

## 呼叫过程

1. 浏览器通过 WSS 连接 `/ws` 并完成注册。
2. INVITE 携带 ICE 候选、DTLS 指纹与 Opus。
3. SBC 鉴权、查询注册库、命中拨号计划。
4. 向 SIP 侧重建 B 腿，改写 SDP。
5. 200 OK 回填，浏览器完成 DTLS 握手。
6. 媒体在 SBC 内桥接，生成质量统计与 CDR。

## 配置要点

```yaml
sip:
  ws:
    enabled: true
    port: 5062
    tls: true

media:
  default_mode: relay
  allow_bypass: false
```

外网部署还需在防火墙放行媒体端口段，并配置 STUN / TURN 供浏览器收集候选。

## 自测清单

- 浏览器控制台无 ICE failed，候选包含 `srflx`。
- 抓包可见 DTLS ClientHello / ServerHello。
- answer 中 `m=` 数量与 offer 一致。
- 双向音频正常，DTMF 可识别。
- 切换网络后 30 秒内恢复。
- 客户端在 ICE 失败时只重启一次，避免风暴。

完整的原理与故障表见 [WebRTC 与 SIP 互通](/webrtc-sip)。

<!-- en -->

Browsers and SIP endpoints never talk directly — each establishes an independent leg with the SBC.

## Four classes of translation

| Plane | WebRTC side | SBC action | SIP side |
|---|---|---|---|
| Signaling | WSS + subprotocol `sip` | reconstruct SIP, add rport / Session-ID | UDP / TCP / TLS |
| Session desc. | ICE candidates, DTLS fingerprint, BUNDLE | terminate ICE, rewrite `c=` / `m=` | standard SDP |
| Media | mandatory DTLS-SRTP | terminate DTLS on the browser leg, encrypt SIP side per policy | SRTP or RTP |
| Codecs | Opus, VP8, H.264 Baseline | intersect capabilities, transcode when needed | PCMU / PCMA / G.722 |

## Call flow

1. Browser connects to `/ws` over WSS and registers.
2. INVITE carries ICE candidates, DTLS fingerprint and Opus.
3. SBC authenticates, looks up registration, matches dial plan.
4. Rebuilds the B-leg towards the SIP side, rewrites the SDP.
5. 200 OK propagates back; browser completes the DTLS handshake.
6. Media is bridged inside the SBC, producing quality metrics and CDRs.

## Configuration highlights

```yaml
sip:
  ws:
    enabled: true
    port: 5062
    tls: true

media:
  default_mode: relay
  allow_bypass: false
```

For public deployments also punch through the media port range on the firewall, and configure STUN / TURN so browsers can gather candidates.

## Self-test checklist

- Browser console shows no `ICE failed`; candidates include `srflx`.
- DTLS ClientHello / ServerHello visible in packet capture.
- The answer has the same number of `m=` lines as the offer.
- Two-way audio works and DTMF is recognizable.
- Recovery after a network switch completes within 30 seconds.
- The client restarts ICE only once on failure — no storms.

For the full background and symptom matrix see [WebRTC ↔ SIP interop](/webrtc-sip).
