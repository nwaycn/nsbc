# NSBC AI 智能客服系统 — 端到端测试方案

> **状态**：方案草案，待评审后执行
> **版本**：v1.0 — 2026-11-16
> **语言栈**：Go 为主（`examples/event_client/go/ai_agent/`）+ nsbcd C 核心 + nsbc-web Rust 侧车
> **参考协议**：`examples/event_client/PROTOCOL.md` §4 AI Pass-through

## 总体要求：
默认当我们的ai路由触发时应给ai_agent一个带callid，主被叫等信息的一个接入事件，然后主动播放一个语音，如果人在说话，则停止放音，等人说话，人说话的时候，同时还是要接收语音流，而且还有个问题，当停止播放语音时，得给被叫送静音包，不管是哪侧挂机，都应停止这两个收和发语音流。 这个服务应是可以一直启动的，而不是通过watch来监视有来电了才拉取服务，而是服务在监听着，当有新的来电，则启一个ai_session处理。

---

## 0. 架构回顾 — 各层职责边界

```
 ┌──────────── Go AI Agent ────────────┐
 │                                     │
 │  AiAgent 主循环                     │
 │  ├─ ASRProvider.transcribe(pcmu)    │ ← 真实/模拟 ASR
 │  ├─ LLMProvider.chat(transcript)    │ ← 真实/模拟 LLM → LLMReply
 │  │   ├─ action="say" → TTS.send_tx │
 │  │   ├─ action="transfer" → REFER  │
 │  │   └─ action="hangup" → BYE       │
 │  ├─ TTSProvider.synthesize(text)    │ ← 真实/模拟 TTS
 │  ├─ BargeInController (VAD)        │
 │  └─ MediaClient (HTTP / WebSocket)  │
 │                                     │
 └──────────────┬──────────────────────┘
                │ PCMU 8kHz Binary frame (WS) 或 base64 (HTTP)
                ▼
 ┌───────── nsbc-web (Rust) ─────────┐
 │  /ws/ai-media/:call_id            │ ← Binary frame 双向直传
 │  /ext/v1/cmd  (UDS 透明代理)      │ ← ai-* 命令
 └──────────────┬────────────────────┘
                │ Unix Domain Socket (JSON 行协议)
                ▼
 ┌────────── nsbcd (C) ──────────────────────────────┐
 │  ai_dialog 模块：                                  │
 │    ring buffer (per-call) + bridge_send_raw()     │
 │    + 可选 VAD (ai-set-producer 1 自动启停)         │
 │                                                    │
 │  media_bridge:  SRTP relatch + RTP 转发            │
 │  b2bua:          blind REFER (ai-request-transfer) │
 └────────────────────────────────────────────────────┘
```

**ai_dialog 严格只管媒体管道**：PCMU 8kHz 收发 + 可选 VAD。
ASR / LLM / TTS / 转人工决策 100% 在 Go Agent 侧。

---

## 1. 测试环境拓扑（真实三机）

> 机器清单来源：[sip-test-plan.md#L133-170](file:///d:/github/nsbc/testing/sip-test-plan.md#L133-L170)

```
 ┌─── 10.0.0.80 (Debian 12) ──────┐
 │  nsbcd:5060 (SIP UDP)           │
 │  nsbc-web:443 (HTTPS)          │
 │  /var/run/nsbc/control.sock     │── VLAN 直连 1Gbps
 │  SSH: ssh -p 22322 root@10.0.0.80  │
 │  代码: /home/github/nsbc/        │
 └───────┬────────────┬────────────┘
         │            │
 ┌────────┴──┐   ┌────┴──────────┐
 │ 10.0.0.25 │   │ 10.0.0.120    │
 │ CentOS 7  │   │ CentOS 7      │
 │ sipp 3.5  │   │ sipp 3.5      │
 │ SSH root@ │   │ SSH root@     │
 │ 密码 2025 │   │ 密码 20266    │
 └───────────┘   └───────────────┘

┌─── SIP 角色分配（灵活切换）────┐
│                                │
│  UAC（主叫 / 发 INVITE）      │
│   = 10.0.0.25 或 10.0.0.120    │
│                                │
│  UAS（被叫 / 座席 / 转人工）  │
│   = 10.0.0.25 或 10.0.0.120    │
│   （另一个端口/另一个进程）    │
│                                │
│  可同时在同一压测机跑两个 SIPp │
│  进程（不同端口）模拟 AB 腿   │
└────────────────────────────────┘

 Go Agent 跑在开发机（Windows 本机），跨网访问 https://10.0.0.80:8080
 或跑在 10.0.0.80 同机（localhost 最快）
```

### 1.1 真实机器清单

| 机器 | OS | 角色 | SSH | SIPp |
|---|---|---|---|---|
| **10.0.0.80** | Debian 12 | nsbcd + nsbc-web + **可选 Go Agent** | `ssh -p 22322 root@10.0.0.80` 密码 `Nway@2025` | `/usr/bin/sipp` |
| **10.0.0.25** | CentOS 7 | SIPp UAS/UAC（**跨机推荐**）| `ssh root@10.0.0.25` 密码 `Nway@2025` | `/usr/bin/sipp` 3.5.0 |
| **10.0.0.120** | CentOS 7 | SIPp UAS/UAC（**跨机推荐**）| `ssh root@10.0.0.120` 密码 `Nway@20266` | `/usr/bin/sipp` 3.5.0 |

### 1.2 nsbcd / nsbc-web 强制启动命令

```bash
# 登录 10.0.0.80 后执行（⚠️ 顺序不能反，nsbcd 必须带 -c）
ssh -p 22322 root@10.0.0.80
cd /home/github/nsbc
nohup ./nsbc-web > /tmp/web.log 2>&1 &
sleep 25
nohup ./nsbcd > /tmp/bcd.log 2>&1 &   无需build/目录下的，我们make时会自动更新到/home/github/nsbc/

# 日志
tailf /home/github/nsbc/logs/nsbcd.log
tailf /tmp/web.log

# DB: /home/github/nsbc/db/web.db（SQLite3 — 拿 Admin-ext-key）
sqlite3 /home/github/nsbc/db/web.db \
  "SELECT ext_key FROM ext_keys WHERE scope='admin' LIMIT 1;"
```

### 1.3 SIPp 启动（跨机 SIPp 验证真实媒体延迟）

```bash
# === 方案 A：10.0.0.25 跑 UAS，10.0.0.120 跑 UAC（两机独立 → 最真实）===

# 终端 1 — 登录 10.0.0.25 → UAS 被叫座席（B 腿）
ssh root@10.0.0.25
sipp -sn uas -p 5060 -m 99999

# 终端 2 — 登录 10.0.0.120 → UAC 主叫（A 腿）→ 发 INVITE 到 10.0.0.80
ssh root@10.0.0.120
sipp -sn uac -d 10000 -s 2001 10.0.0.80:5060 -trace_msg
# （nsbcd 从 UAS 取被叫 2001 → 桥接 AB 腿）

# === 方案 B：同一压测机两个 SIPp 进程（不同端口）===
# 适合先快速验证，媒体延迟是 loopback，但 SIP 信令完全真实
ssh root@10.0.0.25
# 终端 1 — UAS on 5061
sipp -sn uas -p 5061 -m 99999 &
# 终端 2 — UAC → 10.0.0.80
sipp -sn uac -d 10000 -s 2001 10.0.0.80:5060 -trace_msg

# === 确认通话建立 ===
# nsbcd.log 应该看到 relay FWD 双向 send_len>0
# SIPp 控制台应该看到 200 OK + RTP 收发
```

### 1.4 10.0.0.80 本机额外服务（ASR / TTS / LLM）

切记：
本机如果要编译nsbcd，则
cd /home/github/nsbc
make
这样后，会由Makefile中的nsbcd及相关模块复制到/home/github/nsbc目录
/home/github/nsbc/下的nsbcd和nsbc-web都在这个目录下统一使用db/web.db数据库和conf/nsbc.yaml配置文件


> **关键约束**：nway-asr / nway-tts 服务端只允许 **127.0.0.1** 连接。
> 所以 Go Agent 必须 **部署在 10.0.0.80 同机** 才能访问。

| 服务 | 进程 | 端口 | 协议 | 路径 |
|---|---|---|---|---|
| **NWAY ASR** | `./nasr-server` | **TCP 8089** 仅 loopback | 裸 TCP v1.0：`4B_LE(samples)+PCM16_LE → 4B_LE(text_len)+UTF-8` | `/home/lihao/nway_asr/nasr-server` |
| **NWAY TTS** | `./ntts-server` | **TCP 8090** 仅 loopback | 裸 TCP v2：`4B_LE(len)+text → 循环包 {1B type + 4B LE plen + payload}` | `/home/lihao/nway_asr/ntts-server` |
| Ollama LLM | `ollama serve` | 默认 11434 | HTTP JSON mode |（可选）同机跑 |

**已验证的命令**（在 10.0.0.80 本机）：
```bash
# ASR — wav 文件 → transcript
/home/lihao/nway_asr/nasr-client 127.0.0.1:8089 d.wav
# 输出: "我想知道一加一等于几我的手机号是..."

# TTS — 文本 → wav（HEAD sample_rate=8000 Hz）
/home/lihao/nway_asr/ntts-client --input "你好，我是李浩" 127.0.0.1:8090 --output out.wav
# 输出: "Connecting... HEAD sample_rate=8000 Hz ... Saved 42164 bytes to out.wav"
```

**Go Agent 协议实现要点**（providers.go）：
- ASR 输入：NSBC 给的 **PCMU 8kHz** → `PcmuToPcm16` 解码 → `Upsample8kTo16k` 翻倍 → 发 16kHz PCM16
- ASR 输出：transcript UTF-8 文本
- TTS 输入：UTF-8 文本 → 发服务端
- TTS 输出：服务端 HEAD sample_rate=8000 → `Pcm16ToPcmu` 直接编码（无需重采样！）
- **零新依赖**：G.711 μ-law codec（查表）+ 重采样（整数倍）+ `net.Dial`

### 1.5 前置条件（一次性）

| 项 | 值 |
|---|---|
| nsbcd 版本 | commit `8cee04b` 或更新 |
| nsbc-web 版本 | 同 commit 编译 |
| nway-asr | 已在 10.0.0.80 运行（nasr-server pid=3781210 / ntts-server pid=3782338）|
| Go Agent | **编译后放到 10.0.0.80 同机**（`scp ai_agent root@10.0.0.80:/home/github/nsbc/`）|
| Go | ≥ 1.21（Agent 纯 stdlib） |
| Admin-ext-key | SQLite `ext_keys` 表 scope=`admin` 的 key |
| 日志级别 | `log.level=INFO`（media_bridge FWD 诊断需要 INFO 可见）|

---

## 2. 测试矩阵（按优先级排序）

### T1 — 基础媒体管道（P0 必过）

| # | 场景 | 步骤 | 预期 |
|---|---|---|---|
| T1.1 | **Go Agent mock 接管** | 1. 发起 INVITE 建立通话<br>2. 从 SSE `CALL_ANSWERED` 拿 `call_id`<br>3. `--transport ws` 启动 Go Agent<br>4. Agent 自动 `ai-set-producer 1` | WS 连接成功 → `handler 自动 producer=1 + VAD on`<br>stderr 打印 handler 消息 |
| T1.2 | **ai-get-rx 用户语音** | T1.1 后对着电话说"转人工"<br>Agent 主循环拉 PCMU | `ai-get-rx` 返回 base64 非空<br>Go `rx` 字节数 > 0<br>nsbcd.log 有 `relay FWD side=0→1 send_len>0` |
| T1.3 | **ai-send-tx 回复音频** | T1.2 ASR 得到"转人工" → LLM reply="say" → TTS 合成 → Agent 调 `ai-send-tx 0 <b64>` | Go 打 `[agent] ← TTS 回复 (N bytes)`<br>电话里听到 AI 说话<br>nsbcd.log 有 `bridge_send_raw pt=8 sent=...` |
| T1.4 | **HTTP transport 降级** | Go Agent 用 `--transport http` 重跑 T1.1 | 无 `websockets` 库也能工作<br>媒体流延迟略高（50ms × 2 往返）但无功能差异 |
| T1.5 | **大分片 ai-send-tx** | TTS 回复 20KB+ PCMU | Agent 自动按 chunk_cap 分片 send<br>每片独立 base64 → `ai-send-tx` 命令<br>电话里完整听到，无截断 |

### T2 — VAD Barge-In（P0 必过）

| # | 场景 | 步骤 | 预期 |
|---|---|---|---|
| T2.1 | **producer=1 自动启 VAD** | `ai-set-producer 1` 后调 `ai-vad-state` | 返回 `OK A_leg=SILENCE rms=0 vad_active=1` |
| T2.2 | **barge-in 打断 AI 说话** | AI 正在说欢迎语时，用户抢着说话 | `ai-vad-state` 很快变 `A_leg=SPEECH rms>800` |
| T2.3 | **BargeInController.waitUntilSilence** | 用户说完后 Agent 等静默再 ASR | `waitUntilSilence(2s)` 返回 true<br>ASR 只处理静默后的稳定帧，不含用户插话尾音 |
| T2.4 | **producer=0 停 VAD** | 通话结束前 `ai-set-producer 0` | `ai-vad-state` 返回 `vad_active=0`<br>VAD 资源释放，无残留 thread |

### T3 — 转人工（P0 必过）

| # | 场景 | 步骤 | 预期 |
|---|---|---|---|
| T3.1 | **LLM 返回 transfer → Agent 发 REFER** | LLMReply=`transfer dest=1001`<br>Go Agent 调 `ai-request-transfer call_id 1001` | 1. `ai-request-transfer` 返回 `OK transfer-pending`<br>2. SIPp UAS 收到 REFER → 202 Accepted<br>3. SIPp UAS 发 NOTIFY sipfrags<br>4. 原 call B leg 接新 INVITE 到 1001<br>5. 座席 1001 振铃 → 接听 → 媒体桥接 |
| T3.2 | **盲转后媒体切断** | T3.1 成功后，Agent 端不再有 rx/tx | `ai-get-rx` 返回空（原 call 已 close）<br>Agent 主循环 break 退出 |
| T3.3 | **transfer 失败（dest 不存在）** | 转不存在的分机号 `9999` | REFER 失败 → Agent fallback say<br>AI 回复"转接失败，请稍后再试"<br>通话不中断 |
| T3.4 | **非阻塞 API 语义** | Go agent 调 `request_transfer` 后立刻 return | Agent 不等 REFER 完成，立刻 break<br>Agent 退出 → WS handler 自动 producer=0 |

### T4 — AI 机器人端到端（**P0 必过**，以 nway 本地服务为主）

| # | 场景 | 步骤 | 预期 |
|---|---|---|---|
| **T4.1** | **nway 本地 ASR + TTS（零云 API key）** | Go Agent `-asr nway -tts nway -llm mock`<br>部署在 10.0.0.80 同机 | `[NWAYASR] transcript="转人工"`<br>`[NWAYTTS] HEAD sample_rate=8000 Hz`<br>`synthesized N PCMU bytes`<br>电话里听到 AI 说"好的，正在为您转接..." |
| **T4.2** | **Ollama 本地 LLM + nway ASR/TTS** | Go Agent `-llm ollama -asr nway -tts nway`<br>Ollama 同机跑 llama3.1:8b | Ollama JSON mode → parse → LLMReply<br>function call `transfer` → `ai-request-transfer`<br>完整转人工流程 |
| T4.3 | Mock 全链路（零依赖快速验证） | `-asr mock -llm mock -tts mock` | MockASR 能量词 → MockLLM 关键词匹配 → MockTTS 静音<br>每步 stderr 有日志 |
| T4.4 | Whisper / ElevenLabs 外部云（可选） | 需要外网 + API key | ASR/TTS 真实云服务（**低优先级**）|
| **T4.5** | **LLM function call → transfer** | 用户说"转人工"<br>LLM 返回 `{"action":"transfer","dest":"1001"}` | Agent dispatch → `request_transfer`<br>blind REFER 成功（和 T3.1 联动）|
| **T4.6** | **LLM function call → hangup** | 用户说"再见"<br>LLM 返回 `{"action":"hangup"}` | Agent 调 `uuid-kill call_id`<br>通话 graceful 结束 |
| **T4.7** | **Barge-in 真实打断（nway VAD）** | AI 正在说欢迎语时，用户抢着说话 | BargeInController 检测到能量 > 阈值<br>打断 TTS → 重新 ASR → LLM 处理新输入 |
| **T4.8** | **跨通道路由正确（ASR→LLM→TTS）** | 完整链路 | PCMU 8k → nway-asr TCP → transcript → mock/ollama → nway-tts TCP → PCMU → ai-send-tx<br>每段延迟日志可追踪（stderr）|

### T5 — 稳定性 / 压力（P1）

| # | 场景 | 步骤 | 预期 |
|---|---|---|---|
| T5.1 | **Go Agent crash → WS 断 → producer 自动切回 demo** | Agent 进程 kill -9 | WS handler 10s pong timeout 检测断开<br>handler 退出前调 `ai-set-producer 0`<br>nsbcd 切回内建 demo 模式 |
| T5.2 | **nsbcd log rotate 期间通话不断** | 通话进行中 nsbcd.log 到 50MB | do_rotate_locked 在 drain 完所有 ring 后锁内切换 fd<br>通话媒体持续 relay，无丢包（drop counter 不变）<br>stderr 打 `[log-rotate] nsbcd.log → nsbcd.log.1` |
| T5.3 | **nsbcd log rotate 期间新 Agent 接管** | 通话进行中 rotate<br>然后 Go Agent attach | producer=1 成功<br>媒体管道正常 |
| T5.4 | **连续 10 通 INVITE → 每通都有 Go Agent mock** | SIPp 发 10 通，每通 attach 独立 Go Agent 进程 | 10 通都能 AI 对话 → LLM say → TTS 回放<br>audit.jsonl 有 10 条 `ai-set-producer` 记录<br>nsbcd.log 的 relay FWD 不丢包 |

### T6 — Video AI 播放（P2 待实现）

> **状态**：nsbcd 已有 `media_video.c`（video_fwd + keyframe + PLI + FIR），但 **缺少 AI 视频播放命令**
> 本测试组先占位，REQ-AI-VIDEO-001 落地后再激活

| # | 场景 | 前置 REQ | 步骤 | 预期 |
|---|---|---|---|---|
| T6.1 | **AI 播视频 clip 到 B-leg** | REQ-AI-VIDEO-001 `ai-send-video` | 1. 通话建立<br>2. Agent 推 H.264 base64<br>3. Bridge 封 RTP 发 B-leg | B-leg 终端收到视频流<br>video_fwd keyframe 检测 → PLI/FIR 正确响应 |
| T6.2 | **音频 + 视频 AI 同步播放** | 同上 | Agent 同时调 `ai-send-tx` 和 `ai-send-video` | 音视频时间对齐（Agent 侧自己控制节奏）|

---

## 3. Go Agent 快速启动命令（以 nway 本地 ASR/TTS 为主）

```bash
# ========== Step 1: Windows 本地编译 ==========
cd examples/event_client/go/ai_agent
go build -o ai_agent .

# ========== Step 2: scp 到 10.0.0.80（nway 服务端只放行 localhost）==========
scp -P 22322 ai_agent root@10.0.0.80:/home/github/nsbc/
ssh -p 22322 root@10.0.0.80 "chmod +x /home/github/nsbc/ai_agent"

# ========== Step 3: 从 nsbc-web 拿 Admin-ext-key ==========
ssh -p 22322 root@10.0.0.80
sqlite3 /home/github/nsbc/db/web.db \
  "SELECT ext_key FROM ext_keys WHERE scope='admin' LIMIT 1;"
# → 复制粘贴到 $ADMIN_EXT_KEY

# ========== Step 4: 发起 INVITE（SIPp UAC → 10.0.0.80:5060）==========
# 另开终端 SIPp UAC:
ssh root@10.0.0.120
sipp -sn uac -d 30000 -s 2001 10.0.0.80:5060 -trace_msg -trace_rtp

# ========== Step 5: 从 nsbcd log 拿 call_id ==========
# 同机:
tailf /home/github/nsbc/logs/nsbcd.log | grep CALL_ANSWERED

# ========== Step 6: Go Agent 接管通话（在 10.0.0.80 本机！）==========
cd /home/github/nsbc

# ---- 方案 A: nway 本地 ASR + nway 本地 TTS（推荐，最快最稳定）----
# ASR: PCMU → PCM16 16k → nway-asr (TCP 8089) → transcript
# LLM: mock (关键词匹配，可换 Ollama 本地)
# TTS: 文本 → nway-tts (TCP 8090) → PCM16 8k → PCMU
./ai_agent \
  -api-base https://127.0.0.1:8080 \
  -api-token $ADMIN_EXT_KEY \
  -call-id <从 log 拿到的 call_id> \
  -transport ws \
  -vad-barge-in \
  -asr nway -nway-asr-addr 127.0.0.1:8089 \
  -llm mock \
  -tts nway -nway-tts-addr 127.0.0.1:8090 \
  -transfer-dest 1001

# ---- 方案 B: mock 全链路（最快验证管道，零依赖）----
./ai_agent ... -asr mock -llm mock -tts mock ...

# ---- 方案 C: Ollama 本地 LLM + nway ASR/TTS ----
# 先在 10.0.0.80 起 Ollama:
#   ollama serve &  ollama pull llama3.1:8b
./ai_agent ... \
  -asr nway -tts nway \
  -llm ollama -llm-endpoint http://127.0.0.1:11434/api/chat -llm-model llama3.1:8b \
  ...
```

### 3.1 Go Agent 输出示例（nway 全链路）

```
======================================================================
[ai_agent Go] call=call-abc123 transfer_dest=1001
  transport=ws  barge_in=true  chunk=160
  ASR=nway  LLM=mock  TTS=nway
======================================================================

[handler] WS connected. Auto producer=1 (VAD on)
[handler] ai-set-producer 1 OK vad_start
[handler] WS media relay loop started (rx→ASR→LLM→TTS→tx)

[ASR] NWAYASR transcript="转人工" (12800 bytes input → 3 chars)
[LLM] mock action=transfer dest=1001
[handler] ← action=transfer → ai-request-transfer 1001
[handler] ai-request-transfer OK transfer-pending dest=1001
[agent] transfer sent, breaking loop
[handler] auto producer=0 (VAD off)
[agent] done. reason=transfer
```

### 3.2 协议参数速查

| 参数 | ASR (nway) | TTS (nway) |
|---|---|---|
| 服务地址 | `127.0.0.1:8089` | `127.0.0.1:8090` |
| 协议版本 | v1.0 | v2 |
| 输入格式 | 4B_LE(samples) + PCM16_LE 16kHz | 4B_LE(text_len) + UTF-8 文本 |
| 输出格式 | 4B_LE(text_len) + UTF-8 transcript | 循环包：`{type(1) + plen(4) + payload}`<br>`type=0x01 HEAD(4B sr u32 LE)`<br>`type=0x02 DATA(PCM16_LE)`<br>`type=0x03 END`<br>`type=0xFF ERR` |
| 服务端采样率 | 16kHz 必须 | **8kHz**（HEAD 返回） |
| 客户端需要做 | PCMU 8k → PCM16 8k → upsample 16k → 发 | 收 PCM16 8k → PCMU 直接编码 |

---

## 4. 验收检查点（通过标准）

### 4.1 媒体管道正确性

- [ ] T1.2 `ai-get-rx` 持续返回非空 base64，每秒 ≥ 50 帧（20ms × 50 = 1s）
- [ ] T1.3 `ai-send-tx` 返回 OK，电话另一端听到 AI 语音
- [ ] T5.4 10 通并发无丢包：`drop` counter = 0（或 ≤ 1 偶发）

### 4.2 转人工语义

- [ ] T3.1 REFER 202 Accepted → NOTIFY → 媒体切换到 1001
- [ ] T3.3 transfer 失败 → Agent fallback say，通话不中断
- [ ] Audit 日志有 `ai-request-transfer call-id=<id> dest=1001`

### 4.3 Log 系统

- [ ] T5.2 通话中 nsbcd.log ≥ 50MB → rotate 自动发生，通话无感知
- [ ] `tailf nsbcd.log` 看到 `I relay FWD side=0→1` 默认 INFO 级别的转发日志
- [ ] Audit 日志 `audit.jsonl` 每条命令都有 actor/action/timestamp

### 4.4 进程稳定性

- [ ] Go Agent crash → nsbcd 自动 producer=0，不卡住
- [ ] nsbcd crash → SIPp UAS 收到 BYE，通话 graceful 结束

---

## 5. 工具包（快速验证单个 REQ）

### 5.1 快速拿 call_id（UDS 命令）

```bash
# 在 nsbcd CLI 里（或 nsbc-web /ext/v1/cmd）
cdr list --json 2>&1 | python3 -c "import sys,json; d=json.load(sys.stdin); print([r['call_id'] for r in d if r['state']=='answered'])"
```

### 5.2 手动测试 ai-send-tx（base64 编码）

```bash
# 生成 1 秒静音 PCMU + base64 + ai-send-tx
python3 -c "
import base64, sys
pcmu = b'\x00' * 8000  # 1 秒静音 PCMU 8kHz
print(base64.b64encode(pcmu).decode())
" | xargs -I{} curl -sk -H "Authorization: Bearer $EXT_KEY" \
  -X POST https://10.0.0.80:8080/ext/v1/cmd \
  -d '{"cmd":"ai-send-tx <call-id> 0 {}"}'
```

### 5.3 查看 VAD 状态

```bash
curl -sk -H "Authorization: Bearer $EXT_KEY" \
  -X POST https://10.0.0.80:8080/ext/v1/cmd \
  -d '{"cmd":"ai-vad-state <call-id>"}'
# 预期：OK A_leg=SILENCE rms=0 vad_active=1
#       OK B_leg=SILENCE rms=0 vad_active=1
```

### 5.4 手动 request-transfer

```bash
curl -sk -H "Authorization: Bearer $EXT_KEY" \
  -X POST https://10.0.0.80:8080/ext/v1/cmd \
  -d '{"cmd":"ai-request-transfer <call-id> 1001"}'
# 预期：OK transfer-pending dest=1001 call=<call-id>
```

---

## 6. REQ 状态追踪

| REQ | 描述 | 状态 | 测试组 |
|---|---|---|---|
| REQ-AI-DIALOG-001 | Go 版 ai_dialog 完整 Agent 抽象 | ✅ 已实现（commit `8cee04b`） | T1-T6 |
| REQ-AI-MEDIA-001 | PCMU 双向媒体管道 | ✅ 已实现 | T1 |
| REQ-AI-BARGEIN-001 | VAD barge-in | ✅ 已实现 | T2 |
| REQ-AI-TRANSFER-001 | blind REFER 转人工 | ✅ 已实现 | T3 |
| REQ-AI-WS-001 | WebSocket Binary frame + 心跳 | ✅ 已实现 | T1.1, T1.4 |
| REQ-LOG-ROTATE-001 | 自管理 size-based rotate（O_DSYNC + 锁内原子切换）| ✅ 已实现 | T5.2, T5.3 |
| REQ-LOG-ROBUST-001 | PTHREAD_MUTEX_ROBUST crash-safe mutex | ✅ 已实现 | T5.1 |
| **REQ-AI-NWAY-001** | Go NWAYASR/NWAYTTS provider + PCMU↔PCM16 + 8k↔16k resample | ✅ 刚实现（commit pending） | T4.1, T4.2, T4.7, T4.8 |
| REQ-AI-VIDEO-001 | AI 视频播放（ai-send-video 命令 + video_fwd）| ❌ 待实现 | T6 |
| REQ-AI-BOT-001 | 生产级 Go AI 机器人（多轮对话 / 上下文记忆 / 知识库 RAG）| 📋 方案设计中 | 后续 |

---

## 7. 下一步

1. **评审本方案** —— 确认 T1-T6 测试矩阵完整，没有遗漏
2. **跑通 T1-T3** —— Go mock Agent 端到端接管通话 + ASR → LLM say → TTS → 转人工
3. **T4 真实云对接** —— Whisper + Ollama + ElevenLabs（用各自 free tier 快速验证）
4. **T5 稳定性** —— 10 通并发 + log rotate 压力
5. **T6 视频 AI 前置 REQ 实现** —— `ai-send-video <call-id> <base64>` 命令 + video_fwd 桥接
6. **REQ-AI-BOT-001 方案** —— 基于验证过的媒体管道 + Provider 抽象，设计生产级 Go AI 机器人

---

```
Author: AI Engineer (auto-generated)
Date: 2026-11-16
Change: v1.0 初始版本 — 以 Go 为准的测试方案
```
