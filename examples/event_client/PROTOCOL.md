# NSBC Event Client Protocol Reference

**单一事实源** — 所有语言 SDK（Python/Go/Rust/PHP/JS/Java/C#）**必须**按此文档实现。
新字段先改此文档，再对齐各语言 `BuildCallUrl` / Frame schema / CDR JSON。

> 版本号策略：新增字段 bump `REQ-XXX-NNN` 编号；破坏性变更 bump `v2`。
> 当前版本：**v1** (2026-11-15, REQ-DP-TIMEOUT-001)

---

## §0 传输层总览

| 场景 | 协议 | 端点 | 备注 |
|---|---|---|---|
| **外部集成（生产）** | HTTPS + SSE | `nsbc-web:443/8080` | 唯一 canonical surface。所有新语言必须实现 |
| 同机调试 | UDS | `/var/run/nsbc/control.sock` | 本地 debug only，权限 `0600` |
| nsbc-web → nsbcd | UDS | `/var/run/nsbc/data.sock` | nsbc-web 内部，不对外暴露 |

**外部 SDK 最低要求：HTTP POST originate + SSE subscribe**。UDS 是可选 debug path。

---

## §1 Originate — 发起外呼

### 1.1 HTTP POST

```
POST {api_base}/ext/v1/originate
Content-Type: application/json
Authorization: Bearer {token}   ← 可选，取决于 nsbc-web 鉴权配置

{"call_url": "{codec_vars}user/{ext} &{app}"}
```

成功：
```json
{"status": "originate-dispatched", "call_id": "abc123...", "call_url": "..."}
```

失败：
```json
{"error": "destination unreachable", "code": "BAD_DESTINATION"}
```

### 1.2 UDS 调试命令

```
originate {call_url}\n
→ OK: dispatched call_id=abc123...\nnsbc>
```

### 1.3 call_url 语法

FS 风格：

```
{key=value,key=value,...}user/<extension>[@<realm>] &<app>
```

brace var 块可以**省略**（空块 `{}` 也行）。`<realm>` 可选，省略时走默认 SIP realm。

### 1.4 Brace Var 字段字典（BuildCallUrl 必须全部支持）

> **新增字段流程**：先改此表（带 REQ-XXX-NNN 编号），再对齐 Python/Go/Rust builder 结构体。

| key | 类型 | 适用场景 | 默认 | 来源 | 说明 |
|---|---|---|---|---|---|
| `origination_caller_id_number` | string | originate | (空) | FS | 覆盖 From 头主叫号 |
| `origination_caller_id_name` | string | originate | (空) | FS | 覆盖 From 头主叫名 |
| `originate_timeout` | int (秒) | originate | (走全局 60s) | FS | B-leg ringing 期超时；**被 `originate_call_timeout` 覆盖** |
| `audio_codecs` | csv | originate | 各语言自定义默认 | REQ-ORIGINATE-004 | `PCMU,PCMA,G722,OPUS` — 空则不设，UA 用自己的 codec |
| `video_codecs` | csv | originate | 各语言自定义默认 | REQ-ORIGINATE-004 | `H264,VP8` — 空则不设 |
| `realm` | string | originate | (走 profile 默认) | REQ-ORIGINATE-003 | SIP realm 覆盖 |
| **`sched_hangup`** | int (秒) | originate | `0`=不限 | **REQ-DP-TIMEOUT-001** | **接通后最大通话时长**，双腿 confirm 后强制 arm；到期 cause=486 Busy Here |
| **`originate_call_timeout`** | int (秒) | originate | `0`=走全局 50s | **REQ-DP-TIMEOUT-001** | **B-leg 等待接通超时**，覆盖 `arm_noanswer_timer` 决策链 |
| `force_prack` / `force_precondition` | 0/1/2 | originate | 0=inherit profile | REQ-IMS-001 | IMS force-override 三态 |
| 其他 key | — | — | — | — | 跳过。C 端 `parse_fs_brace_vars` 白名单处理，未知 key 不报错只忽略 |

### 1.5 决策链（服务端）

**`arm_noanswer_timer` — B-leg ringing 期（4 层）**：
```
1) call->noanswer_ms_override (>0)              ← FS originate_timeout brace var（最高优先）
2) call->originate_call_timeout_ms_override (>0) ← REQ-DP-TIMEOUT-001 / 新 brace var（仅 originate）
3) cfg.originate_call_timeout_ms (>0)            ← 全局默认 50s（仅 originate）
4) cfg.ring_noanswer_ms                           ← 全局 60s（入站 + fallback）
```

**`arm_sched_hangup_timer` — 接通后硬上限**：
```
1) call->sched_hangup_ms_override (>0)    ← brace var / dialplan rule override
2) cfg.originate_caller_timeout_ms (>0)    ← originate 路径兜底（0=不限）
3) 0 = 不 arm（入站 INVITE 走 Session Timer）
```

### 1.6 build_call_url 跨语言对照

| 语言 | 函数签名 | 默认 codec 行为 |
|---|---|---|
| Python | `build_call_url(orig_to, app, audio, video, timeout, caller_id, caller_name, sched_hangup, originate_call_timeout, call_url)` | audio/video 默认 True → 各自 DEFAULT |
| Go | `BuildCallUrl(OriginateOpts{...})` | Audio/Video nil → 跳过 |
| Rust | `build_call_url(OriginateOpts{...})` | audio/video None → 跳过 |
| C | `nsbc_build_call_url(&(nsbc_orig_opts_t){...})` | NULL → 跳过 |
| Java | `NsbcClient.buildCallUrl(OriginateOpts)` | audio/video null → 用默认常量（与 Python 行为一致） |

所有语言 **`call_url` 非空则原样返回**（绕过 builder）。

---

## §2 Subscribe — 接收事件

### 2.1 UDS 协议

```
connect → banner + "nsbc> "
sub <topics>\n → "OK subscribed to topics: <t>\nnsbc> "
nsbcd 推送: "YYYY-MM-DDTHH:MM:SS.mmm <DOMAIN> <body>\n"   ← 每条独立 \n 结尾
nsbc> prompt 每帧之后不单独出现（server push 不需要 client 命令回应）
```

### 2.2 SSE 协议

```
GET {api_base}/calls/stream
Headers: Authorization: Bearer {token}   ← 可选

→ event: CALL_ANSWERED
  data: {"event":"answer","ts":"2026-11-15T10:00:01.123",...}\n
```

SSE `event:` 映射到 UDS `domain`：

| SSE event | UDS domain |
|---|---|
| `ring` | `CALL_INCOMING` |
| `answer` | `CALL_ANSWERED` |
| `hangup` | `CALL_END` |
| `originate` | `CALL_OUTGOING` |

### 2.3 Frame Schema（每语言 SDK 必须输出相同结构）

```
Frame {
    ts:      string      // ISO8601 "2026-11-15T10:00:01.123"
    ts_str:  string      // 同上（parse_frame 直接存，避免重复 strptime）
    domain:  string      // CALL_INCOMING / CALL_ANSWERED / CALL_OUTGOING / CALL_END / REGISTER / CDR / OK / ERR / subscribed:
    kind:    string      // call / cdr / register / cmd / other  （由 domain 推断，SDK 内部函数）
    fields:  dict/struct // domain≠CDR → k=v pairs 字典；domain=CDR → 完整 JSON 对象
    raw:     string      // 原始行
}
```

### 2.4 Domain 字典 + kind 推断

| domain token | kind | 触发时机 |
|---|---|---|
| `CALL_INCOMING` | `call` | INVITE 到达 nsbcd |
| `CALL_OUTGOING` | `call` | originate 创建 B-leg INVITE |
| `CALL_ANSWERED` | `call` | 200 OK 到达（任一条腿 answer） |
| `CALL_END` | `call` | 任一条腿 BYE / CANCEL → 完整 call 终止 |
| `REGISTER` | `register` | 端点注册/注销 |
| `CDR` | `cdr` | call 完整终止后，同一条 UDS/SSE 通道推送完整 CDR JSON |
| `OK` | `cmd` | UDS 命令成功 |
| `ERR` | `cmd` | UDS 命令失败 |
| `subscribed:` | `cmd` | UDS `sub` 命令回执 |

`kind` 由 SDK 内部 `_classify_domain(domain)` 推断——**不是服务端推送的字段**。

### 2.5 Event Frame k=v 字段字典（domain≠CDR 时）

> 按事件时序列出，字段顺序**不保证**（C 端 snprintf 顺序就是这里列出的顺序）。新字段加在**末尾**。

| k=v key | 类型 | 出现 domain | 说明 |
|---|---|---|---|
| `call_id` | UUID | 所有 CALL_* | 全局唯一，事件关联主键 |
| `orig` | string | 所有 CALL_* | 主叫号 |
| `to` | string | 所有 CALL_* | 被叫号 |
| `src_ip` | IP | `CALL_INCOMING` | INVITE 来源 IP |
| `trunk` | string | `CALL_INCOMING` / `CALL_OUTGOING` | 中继名（外线才有，内线为空） |
| `profile` | string | 所有 CALL_* | 走的 SIP profile |
| `duration_s` | int | `CALL_ANSWERED` | 从 INVITE 到 answer 的秒数 |
| `billsec` | int | `CALL_ANSWERED` | billsec（通常 0，answer 即时） |
| `disp` | string | `CALL_END` | 最终处置：`ANSWERED` / `NO_ANSWER` / `BUSY` / `NORMAL_CLEARING` / `REJECTED` / ... |
| `cause` | int | `CALL_END` | SIP BYE cause code |
| `hangup_by` | string | `CALL_END` | `A_UA`=主叫挂断 / `B_UA`=被叫挂断 |

### 2.6 CDR JSON Schema（domain=CDR 时，fields 是完整 JSON）

```json
{
    "call_id":   "uuid",
    "from":      "主叫号",
    "to":        "被叫号",
    "phase":     "end",
    "disposition": "NORMAL_CLEARING | NO_ANSWER | BUSY | ...",
    "duration_s":   3,
    "billsec":      2,
    "hangup_by":    "A_UA | B_UA",
    "src_ip":     "10.0.0.50",
    "profile":    "internal",
    "trunk":      ""
}
```

REST API `/api/cdrs/recent` 返回同样结构。

---

## §3 高级组件（Python / Go / Rust 实现，外部集成直接用）

### 3.1 CallCorrelator

按 `call_id` 聚合 CALL_INCOMING → CALL_ANSWERED → CALL_END → CDR 成完整时间线。
`on_complete(timeline_cb)` 在 CDR 到达时触发。

```
timeline = {
    "call_id": "abc123",
    "events":  [ {ts, domain, ...}, ... ],   // 按时间序
    "cdr":     { ... },                      // 完整 CDR JSON
    "start_ts": datetime,
    "end_ts":   datetime,
}
```

### 3.2 WebhookSink

异步 POST 每个 Frame JSON 到外部 webhook。**不阻塞事件循环**（fire-and-forget，慢消费者 webhook 挂了也不丢其他事件）。

参数：`url`、`timeout_s`（默认 5）、`retries`（默认 2，退避重试）。

webhook POST body：
```json
Headers: X-NSBC-Event: CALL_ANSWERED

{
  "ts":    "2026-11-15T10:00:01.123",
  "domain": "CALL_ANSWERED",
  "kind":   "call",
  "fields": { "call_id": "abc123", "orig": "8001", "to": "1001", "duration_s": "2" },
  "raw":    "2026-11-15T10:00:01.123 CALL_ANSWERED  call_id=abc123 orig=8001 to=1001 duration_s=2"
}
```

### 3.3 SSE → Frame 映射（SDK 内部）

`domain` 大写化 + `sse_event` 小写（ring/answer/hangup/originate）。payload JSON 直接存 `fields`。

---

## §4 AI Pass-through（REQ-AI-MEDIA-001）

### 4.1 架构分层

```
┌──────────────────────────────────────────────────────────┐
│ ai_dialog (C 核心) — 媒体管道层                            │
│   • ai_rec_feed hook → ring buffer (256KB PCMU 8kHz)     │
│   • ring buffer ← ai_get_rx_audio()                      │
│   • ai_send_tx_audio() → bridge_send_raw → A 腿 RTP       │
│   • ai_request_transfer() → b2b_transfer_call (blind)    │
│   • 内建 demo 管道：do_asr→do_llm→do_tts（cfg.llm_enabled 默认 0 关闭）│
└──────────────────────────────────────────────────────────┘
           ↕  /ext/v1/cmd HTTP 透明代理（Admin scope）
┌──────────────────────────────────────────────────────────┐
│ 外部 AI agent（Python/Go/Rust）                            │
│   • HTTP/SSE 订阅 CALL_ANSWERED → 知道哪通电话要接管          │
│   • ai-set-producer <call-id> 1 —— 让 nsbcd 停内建管道      │
│   • ai-get-rx → ASR（第三方：Whisper/云）                    │
│   • LLM 对话管理（多轮/function call/知识库检索）              │
│   • TTS（云或本地 VITS/GPT-SoVITS）                        │
│   • ai-send-tx —— 推回复 → 用户听                           │
│   • ai-request-transfer —— 复杂意图触发转人工座席              │
└──────────────────────────────────────────────────────────┘
```

**为什么这么分**：C 核心只做媒体桥接（零 HTTP/curl 副作用）；第三方 AI 想用什么云 ASR/本地 LLM 都能接；转人工座席是 B2BUA 业务层能力，留在 C 核心。

### 4.2 OAM 命令字典（经 /ext/v1/cmd 透明代理）

| 命令 | 格式 | 作用 | 返回 | Scope |
|---|---|---|---|---|
| `ai-set-producer` | `<call-id> <0\|1>` | 切换 producer 模式（1=外部接管，**自动启 VAD barge-in**） | `OK producer=N vad_start...` / `ERR` | Admin |
| `ai-request-transfer` | `<call-id> <dest>` | 请求转人工座席（blind REFER，非阻塞） | `OK transfer-pending` / `ERR` | Admin |
| `ai-get-rx` | `<call-id> [max-bytes]` | 拉用户语音（PCMU 8kHz base64，非阻塞） | `OK rx_bytes=N\nb64:<base64>` | Readonly |
| `ai-send-tx` | `<call-id> <0\|1> <base64>` | 推回复音频（0=PCMU，1=PCM16 自动转）；>3KB 建议分片 | `OK tx_bytes=N format=F` | Admin |
| **`ai-vad-state`** | `<call-id>` | **REQ-AI-BARGEIN-001**: 查 A/B 腿 VAD 状态（SPEECH/SILENCE + RMS） | `OK A_leg=SPEECH rms=1200 vad_active=1` | Readonly |

**音频格式约定**：
- RX（用户→AI）：PCMU 8kHz，每次 ai_rec_feed 入队完整 RTP 载荷（通常 160 字节 / 20ms）
- TX（AI→用户）：format=0 PCMU 8kHz；format=1 PCM16（函数内 linear_to_mulaw 自动转）
- 最大命令行 4KB（UDS/HTTP 文本实际极限），建议每次 tx 喂 ≤ 3KB（≈ 1920 字节 PCMU）；更大 payload 在调用方分片发送即可（base64 每 chunk 独立命令）

**外部 producer 循环节奏**：ai_get_rx 每 50ms 调一次（和媒体桥接节奏对齐），空返回 nanosleep；累计 2s+ 触发 ASR→LLM→TTS→ai_send_tx。

**REQ-AI-BARGEIN-001: barge-in 语义**：
- `ai-set-producer <call-id> 1` **自动启动 VAD**（双腿，threshold 默认 800 RMS）；producer=0 时自动停
- VAD 引擎（vad_engine.c）注册为 media_bug global consumer，**无锁热路径**：查状态表 → g711_decode → RMS → 状态机（SILENCE↔SPEECH 去抖 hangover=60ms SPEECH / 200ms SILENCE）
- 外部 AI 每 100ms 轮询 `ai-vad-state`：A_leg=SPEECH 且 rms≥800 → 用户在打断 → AI agent 暂停 TTS → waitUntilSilence（500ms 连续静默）→ 继续对话
- barge-in 不阻塞 C 核心：VAD_EVENT 也推 JSON 到 data.sock Event Bus（`{"event":"VAD_EVENT","call_key":"%p","leg":0,"state":"SPEECH","rms":1200}`），SSE 订阅同样能收到

### 4.3 Python 外部 AI agent 骨架

见 [examples/event_client/python/ai_agent.py](python/ai_agent.py)（真可跑，mock ASR/LLM/TTS，生产替换）：

```bash
# 1) 先发起外呼
originate {sched_hangup=3600} user/1001 &echo

# 2) 等 CALL_ANSWERED 事件拿到 call_id，然后：
python3 ai_agent.py \
    --api-base https://nsbc-web.local:8080 \
    --api-token <admin-ext-key> \
    --call-id <从 SSE 拿到的 call_id> \
    --transfer-dest 1001
```

agent 自动：producer 模式开启 → 循环拉 rx → ASR → LLM → TTS → tx → LLM 检测到"转人工"关键词 → ai-request-transfer → break。

### 4.4 WebSocket 媒体流（REQ-AI-WS-001，零 base64 开销）

**为什么 WebSocket**：HTTP polling（`ai-get-rx` 50ms 一次）每次都 base64 编解码，带宽开销 ~33% + 轮询延迟抖动。WebSocket 提供 **Binary frame 直传 PCMU 8kHz 原始字节**，零编码开销，延迟 < 5ms。

**端点**：
```
GET /ws/ai-media/:call_id?token=<admin-ext-key>
```

**鉴权**：token query 参数 → DB 查 `ext_api_keys.key_hash` → scope 必须 `admin`。握手时 axum middleware 链不跑，handler 内直接查。

**帧格式**（RFC 6455 子集，最小集）：

| Opcode | 值 | 方向 | 用途 |
|---|---|---|---|
| Binary | 0x2 | 双向 | PCMU 8kHz 原始字节（每帧通常 20ms = 160 bytes） |
| Ping | 0x9 | Server→Client | 心跳，30s 间隔 |
| Pong | 0xA | Client→Server | 心跳响应，payload 原样回传 |
| Close | 0x8 | 双向 | 优雅断开，nsbcd 自动 producer=0 切回 demo 模式 |

**服务端→客户端帧 MASK=0**；**客户端→服务端必须 MASK=1**（RFC 6455 强制）。

**桥接架构**（串行单 task，避免并发 mut）：
```
外部 AI Agent                    nsbc-web (Rust)                      nsbcd (C)
     │  Binary(PCMU) ──► ws_read() ──► ai-send-tx ──► ring buffer ──► A腿
     │  ◄── Binary(PCMU) ── ws_write() ◄── ai-get-rx ◄── ring buffer ◄── A腿
     │  Ping/Pong ◄──► ws_write/read 心跳 (30s)
     │  Close ──► 优雅断开 + producer=0 清理
```

**关键实现细节**（ws.rs self-implemented，零新 crate）：
- `hyper::upgrade::on(req)` 需 **move** Request（hyper 1.12 `CanUpgrade` trait 对 owned Request 实现），先取 header 再 move
- `hyper_util::rt::TokioIo::new(upgraded)` 包装成 tokio AsyncRead/Write，**无需 downcast TcpStream**，HTTP/HTTPS 通用
- `ws_read()` 带 50ms timeout，不阻塞 UDS poll（`ai-get-rx` 50ms 轮询）
- UDS 命令仍走 base64（行式文本协议不能传二进制），WebSocket Binary frame 是 base64 之上的**零开销快速路径**

**为什么 self-implemented**：vendor/ 没有 tungstenite/tokio-tungstenite；nsbc-web 离线构建（vendor/ + replace-with）不能加新 crate。本实现只依赖已有的 ring(SHA1) + base64 0.22 + hyper 1.12 + hyper_util 0.1。

**客户端示例**（Python websockets 库）：
```python
import asyncio, websockets

async def ws_media_loop(call_id: str, token: str):
    uri = f"ws://nsbc-web.local:8080/ws/ai-media/{call_id}?token={token}"
    async with websockets.connect(uri) as ws:
        # 开启 producer 模式（WS handler 自动调 ai-set-producer 1）
        while True:
            pcmu = await asyncio.wait_for(ws.recv(), timeout=5.0)
            # pcmu 是 PCMU 8kHz 原始字节，送 ASR
            asr_text = await asr(pcmu)
            reply = await llm(asr_text)
            tts_pcmu = await tts(reply)
            await ws.send(tts_pcmu)  # Binary frame，零编码
```

---

## §5 Build + Run 速查

见 [README.md §Build + Run](README.md)。

---

## §6 字段生命周期（REQ-XXX-NNN）

| 编号 | 标题 | 新增 | 影响 | 对齐状态 |
|---|---|---|---|---|
| REQ-ORIGINATE-001 | FS originate_timeout brace var | — | 所有语言 BuildCallUrl `timeout` 参数 | ✅ Python/Go/Rust/C/Java |
| REQ-ORIGINATE-003 | realm brace var | — | BuildCallUrl `realm` 参数 | ✅ Python/Go/Rust |
| REQ-ORIGINATE-004 | audio_codecs / video_codecs | — | BuildCallUrl codec 参数 | ✅ 全语言 |
| **REQ-DP-TIMEOUT-001** | **sched_hangup / originate_call_timeout** | **2026-11-15** | BuildCallUrl 加 2 参数；C 端 parse_fs_brace_vars；b2b_call_t override | ✅ Python/Go/Rust/C/Java + C core |
| **REQ-AI-WS-001** | **WebSocket 媒体流 /ws/ai-media/:call_id** | **2026-11-16** | web/src/ws.rs self-implemented WS；ai_agent.py 加 WS transport | ✅ Rust web + C core + Python agent |

新字段（非破坏性）遵循：改此文档 → Python reference → 全语言 builder 对齐 → commit message 带 REQ 编号。
破坏性变更（删字段、改类型）bumps 协议版本 + 同步 README。
