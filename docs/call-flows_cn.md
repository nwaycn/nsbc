# NSBC 调用链参考 — SIP 与 WebRTC

两条主媒体路径的函数级调用链，全部取自当前源码。文中每个函数名都是真实的，可直接跳转。
模块级视图请配合 [architecture.md](architecture.md) 阅读。

- **第一部分 — SIP 中继呼叫**（RTP 路径）：信令收发、拨号计划路由、RTP 中继。
- **第二部分 — WebRTC 呼叫**（DTLS-SRTP 路径）：WSS 信令、ICE、DTLS 握手、SRTP 桥接。
- **第三部分 — Web 控制面**：nsbc-web 通过 UDS 驱动上述链路。

---

## 第一部分 — SIP 信令调用链（RTP 路径）

### 1.1 总览

```mermaid
flowchart TD
    subgraph NET["网络 I/O"]
        EPOLL[epoll 事件循环<br/>main.c]
    end

    subgraph TRANS["SIP 传输层 — src/sip/transport.c"]
        TR["sip_tr_send()<br/>UDP / TCP / TLS / WS / WSS"]
    end

    subgraph B2B["B2BUA 核心 — src/sip/b2bua.c"]
        RX["b2b_on_rx() :4685"]
        HNI["handle_new_invite() :3175"]
        SIB["send_invite_b()"]
    end

    subgraph SIG["信令层 — src/core/signaling.c"]
        EPREG["endpoint_registry.c :177<br/>accept / 分发"]
        SEHI["sip_ep_handle_invite() :317"]
    end

    subgraph REG["注册服务器 — src/registrar/registrar.c"]
        ROR["reg_on_register() :470"]
    end

    subgraph DP["拨号计划 — src/mod/dialplans/"]
        DPE["dp_execute() :211"]
        APPR["app_execute()<br/>app_registry.c :108"]
    end

    subgraph MB["媒体桥 — src/media/bridge/media_bridge.c"]
        BOH["bridge_offer_hook() :2348"]
        BAH["bridge_answer_hook() :2707"]
        BCF["bridge_confirm() :3175"]
        RR["rtp_relay_start() :2144"]
    end

    EPOLL -->|"SIP 字节流"| RX
    RX -->|"REGISTER"| ROR
    RX -->|"INVITE"| EPREG
    EPREG --> SEHI --> HNI
    HNI --> DPE
    DPE -->|"路由结果"| HNI
    HNI --> BOH
    HNI -->|"B-leg INVITE"| SIB --> TR
    BOH --> RR
    SIB --> BAH
    BAH --> BCF
```

### 1.2 逐步调用时序

```mermaid
sequenceDiagram
    autonumber
    participant UAC as SIP 主叫
    participant TR as transport.c
    participant B2 as b2bua.c
    participant DP as dialplan.c
    participant BR as media_bridge.c
    participant UAS as SIP 被叫

    UAC->>TR: INVITE (UDP/TCP/TLS)
    TR->>B2: b2b_on_rx() :4685
    B2->>B2: profile / ACL / 解析
    B2->>B2: sip_ep_handle_invite() → handle_new_invite() :3175

    Note over B2,DP: 路由决策
    B2->>DP: dp_execute() :211
    DP->>DP: 按 profile/CIDR/号码模式/权限 过滤
    DP-->>B2: dst_number + media_mode + apps

    Note over B2,BR: A-leg 媒体准备（SDP offer 钩子）
    B2->>BR: bridge_offer_hook() :2348
    BR->>BR: rtp_relay_start() :2144
    BR-->>B2: 改写后的 SDP（SBC 的 RTP IP:port）

    B2->>UAS: send_invite_b() — B-leg INVITE
    UAS-->>B2: 200 OK（SDP answer）

    Note over B2,BR: B-leg 应答处理
    B2->>BR: bridge_answer_hook() :2707
    B2->>BR: bridge_confirm() :3175
    BR->>BR: 编解码交集 → relay/transcode 模式
    BR-->>B2: 最终 SDP

    B2-->>UAC: 200 OK（A-leg 应答）
    Note over UAC,UAS: RTP 经 rtp_relay fd 转发<br/>每包走 relay_recv_one() :376
```

### 1.3 函数速查表

| 层 | 函数 | 文件:行 | 职责 |
|---|---|---|---|
| 传输 | `sip_tr_send` | src/sip/transport.c:836 | 出站 SIP（UDP/TCP/TLS/WS/WSS） |
| B2BUA 入口 | `b2b_on_rx` | src/sip/b2bua.c:4685 | 入站 SIP 总入口：解析、profile/ACL 路由 |
| 端点注册 | （分发循环） | src/core/endpoint_registry.c:177 | 遍历端点 → `handle_invite` |
| 信令 | `sip_ep_handle_invite` | src/core/signaling.c:317 | 解包 invite ctx → `handle_new_invite` |
| 呼叫建立 | `handle_new_invite` | src/sip/b2bua.c:3175 | A-leg：鉴权、路由、App 链 |
| 拨号计划 | `dp_execute` | src/mod/dialplans/dialplan.c:211 | 规则匹配：profile/CIDR/模式/权限 |
| App 执行 | `app_execute` | src/mod/dialplans/app_registry.c:108 | 执行 App 链（bridge/playback/answer…） |
| 注册 | `reg_on_register` | src/registrar/registrar.c:470 | REGISTER 处理、位置绑定 |
| 媒体 offer | `bridge_offer_hook` | src/media/bridge/media_bridge.c:2348 | A-leg SDP 改写、中继分配 |
| 媒体 answer | `bridge_answer_hook` | src/media/bridge/media_bridge.c:2707 | B-leg SDP 改写、对端地址锁定 |
| 媒体确认 | `bridge_confirm` | src/media/bridge/media_bridge.c:3175 | 编解码决策：bypass/relay/transcode |
| 中继 | `rtp_relay_start` | src/media/bridge/media_bridge.c:2144 | 每侧分配 RTP/RTCP fd 对 |
| 中继循环 | `relay_recv_one` | src/media/bridge/media_bridge.c:376 | 逐包转发（需要时 SRTP 解密） |
| 出站 | `send_invite_b` | src/sip/b2bua.c:3820（路径） | 构造并发送 B-leg INVITE |

---

## 第二部分 — WebRTC 调用链（DTLS-SRTP 路径）

### 2.1 总览

WebRTC 终端通过 **WSS**（WebSocket Secure）注册，经 SDP 协商 DTLS-SRTP，
SBC 在自己的 UDP fd 上终结 DTLS，完成 SRTP ↔ 普通 RTP 的桥接。

```mermaid
flowchart TD
    subgraph FE["浏览器 — web/static/js"]
        JSW["pages-webrtc.js:317<br/>JsSIP.WebSocketInterface"]
        UA["JsSIP UA.register()"]
        PC["RTCPeerConnection<br/>setRemoteDescription :386"]
    end

    subgraph WSSL["WSS 监听器 — src/net/ws_listener.c"]
        WLI["ws_listener_init() :861"]
        WLS["ws_listener_start() :896"]
        UPG["RFC 6455 升级握手<br/>:332 Sec-WebSocket-Accept"]
    end

    subgraph SIGW["信令 WS 桥 — src/core/signaling.c"]
        SWL[":1910 ws_listener_init/start"]
        TRW["sip_tr_send() SIP_TR_WSS<br/>transport.c:836"]
    end

    subgraph B2W["B2BUA — src/sip/b2bua.c"]
        RXW["b2b_on_rx() :4685<br/>（proto = SIP_TR_WSS）"]
        REGW["registrar.c:715<br/>.invalid Contact 兼容"]
    end

    subgraph MBW["媒体桥 — src/media/bridge/media_bridge.c"]
        BOH["bridge_offer_hook() :2348<br/>a_ws/b_ws 标志"]
        BRS["bridge_rewrite_sdp() :1888<br/>识别 DTLS-SRTP + ICE"]
        BAH["bridge_answer_hook() :2707<br/>注入 ICE/DTLS 凭证"]
    end

    subgraph CRY["加密层 — src/media/crypto/ + src/ssl/"]
        DSC["dtls_server_create() :228<br/>dtls_srtp.c"]
        DOR["dtls_server_on_read() :440"]
        SSD["nsbc_ssl_dtls_server_new()<br/>nsbc_ssl.c:267"]
        SSE["nsbc_ssl_dtls_export_srtp()<br/>nsbc_ssl.c:480"]
        SSC["nsbc_srtp_session_create()<br/>srtp_session.c:109"]
        SP["nsbc_srtp_protect_rtp() :257<br/>nsbc_srtp_unprotect_rtp() :279"]
    end

    JSW -->|"WSS 连接"| UPG
    UA -->|"REGISTER over WS"| RXW
    RXW --> REGW
    PC -->|"INVITE + SDP offer（WSS）"| RXW
    RXW --> BOH --> BRS
    BRS -->|"WebRTC 侧"| BAH
    BAH -->|"UDP 媒体 fd"| DSC
    DSC --> SSD
    DOR -->|"握手完成"| SSE --> SSC --> SP
    SP -->|"SRTP ↔ RTP"| RR2["relay_recv_one() :376"]
```

### 2.2 时序 — WebRTC 终端呼叫 SIP 中继

```mermaid
sequenceDiagram
    autonumber
    participant B as 浏览器（JsSIP）
    participant W as ws_listener.c（WSS）
    participant B2 as b2bua.c
    participant BR as media_bridge.c
    participant DT as dtls_srtp.c
    participant S as SIP 中继（RTP）

    Note over B,W: 信令走 WSS
    B->>W: WSS 连接 + REGISTER
    W->>B2: b2b_on_rx() proto=SIP_TR_WSS
    B2->>B2: reg_on_register() — .invalid Contact 兼容 :715
    B-->>B2: 注册成功

    B->>B2: INVITE（SDP offer：DTLS-SRTP + ICE）
    B2->>BR: bridge_offer_hook() :2348 — a_ws=1
    BR->>BR: bridge_rewrite_sdp() :1888<br/>识别 fingerprint → SRTP_RELAY 模式
    BR->>S: send_invite_b() — 普通 RTP SDP（无 DTLS）

    S-->>B2: 200 OK（RTP answer）
    B2->>BR: bridge_answer_hook() :2707
    Note over BR,DT: 仅 WebRTC 侧创建 DTLS server
    BR->>DT: dtls_server_create() :228<br/>（nsbc_ssl_dtls_server_new）
    BR-->>B2: WebRTC answer SDP<br/>（ICE 凭证 + DTLS fingerprint）

    B2-->>B: 200 OK over WSS
    Note over B,DT: 媒体 fd 上进行 ICE → DTLS 握手
    B->>DT: DTLS ClientHello（UDP）
    DT->>DT: dtls_server_on_read() :440
    DT->>DT: nsbc_ssl_dtls_export_srtp()<br/>label = EXTRACTOR-dtls_srtp
    DT->>DT: nsbc_srtp_session_create() :109

    Note over B,S: 媒体面激活
    B->>DT: SRTP 包
    DT->>BR: nsbc_srtp_unprotect_rtp() → RTP
    BR->>S: 普通 RTP 中继（relay_recv_one）
    S->>BR: RTP
    BR->>DT: nsbc_srtp_protect_rtp() → SRTP
    DT->>B: SRTP 发往浏览器
```

### 2.3 函数速查表

| 层 | 函数 | 文件:行 | 职责 |
|---|---|---|---|
| 前端 | `WebSocketInterface` / `ua.register` | web/static/js/pages-webrtc.js:317 | WSS 连接 + SIP REGISTER |
| 前端 | `setRemoteDescription` 处理器 | web/static/js/pages-webrtc.js:386 | SDP answer → RTCPeerConnection |
| WSS | `ws_listener_init` | src/net/ws_listener.c:861 | 绑定 WSS 端口 + TLS ctx |
| WSS | `ws_listener_start` | src/net/ws_listener.c:896 | accept 循环（epoll） |
| WSS | （升级握手） | src/net/ws_listener.c:332 | RFC 6455：校验 key，回 accept |
| 信令 | （WS 监听器启动） | src/core/signaling.c:1910 | 桥接 WS ↔ SIP 协议栈 |
| 传输 | `sip_tr_send`（WSS 分支） | src/sip/transport.c:836 | SIP 帧 → WebSocket 发送 |
| 注册 | （`.invalid` 兼容） | src/registrar/registrar.c:715 | 接受 WS 场景的 `.invalid` Contact |
| 媒体桥 | `bridge_offer_hook` | src/media/bridge/media_bridge.c:2348 | 置 `a_ws/b_ws`，选 BYPASS/RELAY |
| 媒体桥 | `bridge_rewrite_sdp` | src/media/bridge/media_bridge.c:1888 | 识别 SDP 中的 DTLS-SRTP/ICE |
| 媒体桥 | `bridge_answer_hook` | src/media/bridge/media_bridge.c:2707 | 仅 WebRTC 侧建 DTLS server，注入 ICE 凭证 |
| DTLS | `dtls_server_create` | src/media/crypto/dtls_srtp.c:228 | 每媒体 fd 的 DTLS 角色/证书/Profile |
| DTLS | `dtls_server_on_read` | src/media/crypto/dtls_srtp.c:440 | 握手泵 + 记录分发 |
| DTLS | `dtls_server_take_srtp` | src/media/crypto/dtls_srtp.c | ESTABLISHED 后导出 SRTP 材料 |
| SSL | `nsbc_ssl_dtls_server_new` | src/ssl/nsbc_ssl.c:267 | OpenSSL DTLS：自签证书、指纹校验 |
| SSL | `nsbc_ssl_dtls_export_srtp` | src/ssl/nsbc_ssl.c:480 | 用 `EXTRACTOR-dtls_srtp` 导出密钥材料 |
| SRTP | `nsbc_srtp_session_create` | src/media/crypto/srtp_session.c:109 | 由导出材料建 libsrtp 会话 |
| SRTP | `nsbc_srtp_protect_rtp` / `unprotect` | src/media/crypto/srtp_session.c:257/:279 | 逐包加解密 |
| 中继 | `relay_recv_one` | src/media/bridge/media_bridge.c:376 | SRTP 侧解密 → 转发 RTP |

### 2.4 媒体模式决策（WebRTC 感知）

```
bridge_offer_hook() + bridge_rewrite_sdp() 按呼叫决策：

  WebRTC ↔ WebRTC  →  BYPASS    （P2P，SBC 只改写 SDP）
  WebRTC ↔ RTP     →  SRTP_RELAY（SBC 终结 DTLS，桥接 SRTP↔RTP）
  RTP ↔ RTP        →  RELAY     （普通 RTP 中继，bridge_confirm 可选转码）
```

---

## 第三部分 — Web（nsbc-web）→ 核心控制面

Web 边车通过两条 UDS 通道驱动上述链路（C 核心不访问数据库）：

```mermaid
flowchart LR
    subgraph WEB["nsbc-web（Rust）"]
        CLI["coreclient.rs<br/>RealCore"]
        RT["routes/*.rs<br/>（calls.rs 等）"]
    end
    subgraph OAM["C 核心 OAM — src/oam/"]
        OAMS["oam_start() :23"]
        OBSC["obs_control<br/>control.sock 服务端"]
        RL["reload_commands.c :47<br/>dp_rules_reload_json 等"]
    end
    subgraph EV["事件推送"]
        DS["data.sock<br/>C → Web SSE"]
        SSE["/calls/stream SSE<br/>app-core.js:1143"]
    end

    RT --> CLI -->|"JSON 命令<br/>list / hangup / reload"| OBSC
    OBSC --> OAMS
    OBSC --> RL
    DS --> SSE
```

---

## 快速导航

| 想追踪… | 起点 |
|---|---|
| SIP 消息到达 | `b2b_on_rx` — src/sip/b2bua.c:4685 |
| 路由决策 | `dp_execute` — src/mod/dialplans/dialplan.c:211 |
| App 执行（bridge/playback） | `app_execute` — src/mod/dialplans/app_registry.c:108 |
| 媒体分配 | `bridge_offer_hook` — src/media/bridge/media_bridge.c:2348 |
| WebRTC 检测 | `bridge_rewrite_sdp` — src/media/bridge/media_bridge.c:1888 |
| DTLS 握手 | `dtls_server_on_read` — src/media/crypto/dtls_srtp.c:440 |
| SRTP 密钥 | `nsbc_ssl_dtls_export_srtp` — src/ssl/nsbc_ssl.c:480 |
| 逐包媒体处理 | `relay_recv_one` — src/media/bridge/media_bridge.c:376 |
| Web → 核心命令 | `coreclient.rs`（web/src）→ `obs_control`（src/oam/） |
