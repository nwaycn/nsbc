---
title: NSBC 控制面命令 & API 使用说明书
summary: CLI / Web HTTP API / 外部 API 三条控制面入口，uuid_* 通话控制、OAM、reload、security、Dialplan App 与 HTTP 路由总览。
slug: control-api
order: 60
---

# NSBC 控制面命令 & API 使用说明书

**版本**: v2.5.5 (2026-11-14)
**架构**: C Core (cmd_registry) → Rust web (axum HTTP + UDS proxy) → JS Frontend

---

## 目录

- [1. 连接方式](#1-连接方式)
- [2. 权限分级](#2-权限分级)
- [3. uuid_\* 通话控制命令](#3-uuid_-通话控制命令)
- [4. 全局 OAM 命令](#4-全局-oam-命令)
- [5. show/list/status 只读命令](#5-showliststatus-只读命令)
- [6. reload\* 热加载命令](#6-reload-热加载命令)
- [7. security.\* 安全命令](#7-security-安全命令)
- [8. bug.\* 媒体 Bug 命令](#8-bug-媒体-bug-命令)
- [9. Dialplan App 清单](#9-dialplan-app-清单)
- [10. HTTP API 路由总览](#10-http-api-路由总览)
- [11. 附录：错误码对照](#11-附录错误码对照)

---

## 1. 连接方式

系统暴露**三条独立的控制面入口**，面向不同用户群：

| 方式 | 鉴权 | 入口 | 示例 |
|------|------|------|------|
| **CLI 本地** | 无（本机 root） | `nsbc_cli` → Unix Domain Socket `/var/run/nsbc/control.sock` | `nsbc_cli uuid_kill 550e...` |
| **Web HTTP API** | JWT Bearer（Web 登录态） | `http://<host>:8088/api/v1/` | `POST /api/v1/calls/{id}/hangup` |
| **外部 API** | API Key Bearer（`nsbc_live_<32hex>`） | `http://<host>:8088/ext/v1/` | `POST /ext/v1/calls/{id}/hangup` |

三者最终都经 UDS 代理到 C core 的 cmd_registry 分发执行。

### CLI 参数

```bash
nsbc_cli [OPTIONS]

  -s, --sock   <path>      指定 control.sock 路径（默认 /var/run/nsbc/control.sock）
  -c, --config <path>      从 nsbc-web 配置文件（toml/json）读取 control_sock 路径
  -e, --exec   "cmd args"  非交互模式：执行一条命令后退出
  -h, --help               帮助
  -V, --version            版本

# 交互模式
nsbc_cli -s /tmp/nsbc.sock

# 单次命令
nsbc_cli -e "uuid_kill 550e8400-e29b-41d4-a716-446655440000"
```

### Web HTTP `/api/v1/`

面向 Web UI / 内部运维系统。需要先登录拿 JWT：
```bash
curl -X POST http://host:8088/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"xxx"}'
# 返回 {"access_token": "eyJ...", ...}

curl http://host:8088/api/v1/calls \
  -H "Authorization: Bearer eyJ..."
```

### 外部 API `/ext/v1/`

**面向呼叫中心 CRM / AI Agent / IVR 平台**，与 Web UI 完全隔离。
API Key 格式：`nsbc_live_<32bytes hex>`（首次部署自动生成 seed admin key）。

#### Scope 分级

| Scope | 允许操作 | 端点 |
|-------|----------|------|
| `readonly` | 查询 | `GET /calls`, `GET /system/status`, `GET /system/version`, `GET /docs`, `GET /cmd/help` |
| `readwrite` | 查询 + 写操作 | 上述 + `POST /originate`, `POST /calls/{id}/hangup`, `GET /events/stream` |
| `admin` | 所有 | 上述 + `POST /cmd` 任意命令代理 |

#### curl 完整示例

```bash
# ===== 准备：保存 Key =====
export NSBC_KEY="nsbc_live_ab12cd34ef56789012345678901234ab"
export NSBC_HOST="http://10.0.0.80:8088"

# ===== 1. 系统信息（readonly）=====
# 版本
curl -s "$NSBC_HOST/ext/v1/system/version" \
  -H "Authorization: Bearer $NSBC_KEY" | jq .
# → {"web":"2.5.5","core":"2.5.5-73abc4d","api_major":1}

# 综合状态
curl -s "$NSBC_HOST/ext/v1/system/status" \
  -H "Authorization: Bearer $NSBC_KEY" | jq .
# → {"active_calls":3,"uptime_s":86400,"mem_mb":128,"sock_connected":true,...}

# 动态自描述（外部系统可用此发现 API）
curl -s "$NSBC_HOST/ext/v1/docs" \
  -H "Authorization: Bearer $NSBC_KEY" | jq '.endpoints | length'

# ===== 2. 通话查询（readonly）=====
# 活动通话列表
curl -s "$NSBC_HOST/ext/v1/calls" \
  -H "Authorization: Bearer $NSBC_KEY" | jq '.calls[] | {id, from, to, state}'
# → [{"id":"550e8400-...","from":"sip:1001@...","to":"sip:5001@...","state":"confirmed"}]

# ===== 3. 写操作（readwrite）=====
# 外呼
curl -s -X POST "$NSBC_HOST/ext/v1/originate" \
  -H "Authorization: Bearer $NSBC_KEY" \
  -H "Content-Type: application/json" \
  -d '{"call_url":"sip:1001@10.0.0.80:5060"}' | jq .
# → {"status":"originate-dispatched","call_url":"sip:1001@..."}

# 强制挂断
curl -s -X POST "$NSBC_HOST/ext/v1/calls/550e8400-e29b-41d4-a716-446655440000/hangup" \
  -H "Authorization: Bearer $NSBC_KEY" | jq .
# → {"status":"ok"}

# ===== 4. 事件流 SSE（readwrite）=====
# 接收所有 session 事件（CALL_START/CALL_ANSWER/CALL_END），按 Ctrl+C 退出
curl -N "$NSBC_HOST/ext/v1/events/stream" \
  -H "Authorization: Bearer $NSBC_KEY"
# → data: {"kind":"session","event":"CALL_START","call_id":"...","from":"...","to":"..."}
# → data: {"kind":"session","event":"CALL_ANSWER","call_id":"...",...}

# ===== 5. 透明命令代理（admin，★ 最强大）=====
# 绕过结构化路由，直接发任意 nsbcd 命令
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" \
  -H "Authorization: Bearer $NSBC_KEY" \
  -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_playback 550e... \"9.wav|5.wav|2.wav|serve.wav\""}' | jq .
# → {"cmd":"uuid_playback ...","output":"OK playback chain ...\n"}

# 查播放状态
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" \
  -H "Authorization: Bearer $NSBC_KEY" \
  -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_playback_status 550e..."}' | jq -r .output
# → OK playback active call=550e... side=-1 loop=0

# 查所有命令帮助
curl -s "$NSBC_HOST/ext/v1/cmd/help" \
  -H "Authorization: Bearer $NSBC_KEY" | jq '.commands[].name'

# 错误处理示例
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" \
  -H "Authorization: Bearer $NSBC_KEY" \
  -H "Content-Type: application/json" \
  -d '{"cmd":"reload"}' | jq -r .output
# → ERR not-allowed (admin 命令被前缀白名单拒)
```

#### Shell 辅助函数

```bash
# 放到 ~/.bashrc 里，之后直接 nsbc uuid_kill xxx
nsbc() {
  curl -s -X POST "$NSBC_HOST/ext/v1/cmd" \
    -H "Authorization: Bearer $NSBC_KEY" \
    -H "Content-Type: application/json" \
    -d "{\"cmd\":\"$*\"}" | jq -r .output
}

nsbc_status()  { curl -s "$NSBC_HOST/ext/v1/system/status"  -H "Authorization: Bearer $NSBC_KEY" | jq .; }
nsbc_calls()   { curl -s "$NSBC_HOST/ext/v1/calls"          -H "Authorization: Bearer $NSBC_KEY" | jq '.calls[]'; }
nsbc_hangup()  { curl -s -X POST "$NSBC_HOST/ext/v1/calls/$1/hangup" -H "Authorization: Bearer $NSBC_KEY" | jq .; }
nsbc_originate(){ curl -s -X POST "$NSBC_HOST/ext/v1/originate" \
                   -H "Authorization: Bearer $NSBC_KEY" \
                   -H "Content-Type: application/json" \
                   -d "{\"call_url\":\"$1\"}" | jq .; }
```

管理 API Key：`POST /api/v1/ext-keys`（Admin scope JWT）

---

## 2. 权限分级

| 级别 | 含义 | HTTP Scope | 示例 |
|------|------|------------|------|
| **L1_MONITOR** | 只读查询 | `read` / `readwrite` | `uuid_playback_status`, `profiles.list` |
| **L2_ADMIN** | 管理操作 | `readwrite` / `admin` | `uuid_playback`, `mod.reload` |
| **审计** | L2 写操作自动写 audit log | — | 所有 `uuid_*` 写命令 |

---

## 3. uuid_\* 通话控制命令

所有命令首参数为 `<call-id>`（UUID 字符串，如 `550e8400-e29b-41d4-a716-446655440000`）。

### 3.1 强制挂断

```
uuid_kill <call-id> [cause]
  cause: SIP cause code，默认 487 REQUEST_TERMINATED。
         403=FORBIDDEN, 486=BUSY HERE, 480=TEMPORARILY UNAVAILABLE, 603=DECLINE
HTTP: POST /calls/:id/hangup   POST /calls/:id/kill
```
**返回**: `OK hangup dispatched` / `ERR notfound`

### 3.2 CANCEL 早期 INVITE

```
uuid_cancel <call-id> [leg]
  leg: -1=both(默认), 0=A, 1=B。对已接通腿无效（需用 kill）。
HTTP: POST /calls/:id/cancel
```
**返回**: `OK cancel dispatched leg=...` / `ERR no-early-leg`

### 3.3 HOLD / RESUME

```
uuid_hold <call-id> [on|off]
  默认 on；自动发 Re-INVITE hold；unhold 发 Re-INVITE 恢复。
HTTP: POST /calls/:id/hold
  ⚠️ 字段名 on 不是 hold！Rust body: HoldBody { on: bool }
  body: { "on": true } 或 { "on": false }
```
**返回**: `OK hold=1 <callid>` / `OK hold=0 <callid>`（UDS 推 `{"event":"hold"}`）

### 3.4 MUTE / UNMUTE

```
uuid_mute <call-id> [on|off] [leg]
  默认 on；leg=-1 双腿(默认)。媒体线程下一帧自动清零 payload。
HTTP: POST /calls/:id/mute
  ⚠️ 字段名 on 不是 mute！Rust body: MuteBody { on: bool, leg: Option<i32> }
  body: { "on": true, "leg": -1 }
```

### 3.5 VAD 语音活动检测（AI Barge-in 前置）

```
uuid_vad_start  <call-id> [threshold]    # threshold: 100..30000，默认 VAD_DEFAULT_THRESHOLD
uuid_vad_stop   <call-id>
HTTP: POST /calls/:id/vad-start   POST /calls/:id/vad-stop
```

### 3.6 BLIND TRANSFER（盲转）

```
uuid_transfer <call-id> <dest>
  dest: SIP URI 或号码，如 "sip:1001@10.0.0.1:5060" / "1001"
  底层发 REFER + replaces 头。FreeSWITCH 兼容。
HTTP: POST /calls/:id/transfer
  body: { "dest": "sip:1001@10.0.0.1:5060" }
```

### 3.7 DEFLECT（别名 transfer）

```
uuid_deflect <call-id> <dest>
  同 uuid_transfer，FreeSWITCH deflect 别名。
HTTP: POST /calls/:id/deflect
  body: { "dest": "..." }
```

### 3.8 DISPLACE（B-leg 接管）

```
uuid_displace <call-id>
  动作序列：
    1) 若 B 腿未接通 → uuid_answer B-leg
    2) A 腿 unhold → 媒体恢复
  场景：坐席接听保留中的 A 腿 + B 腿接通 → A 腿 unhold 完成接管。
HTTP: POST /calls/:id/displace
```

### 3.9 PARK / UNPARK

```
uuid_park   <call-id>   # 双腿 hold，媒体暂停
uuid_unpark <call-id>   # 双腿 unhold，媒体恢复
HTTP: POST /calls/:id/park   POST /calls/:id/unpark
```

### 3.10 BRIDGE（两呼叫互相桥接）

```
uuid_bridge <call-id-A> <call-id-B>
  动作：relay_create(relay_both) → RTP 互转发
  警告：bridge 后两 call 的 leg 角色不变，但媒体已互通。
HTTP: POST /calls/:id/bridge
  body: { "call_id_b": "other-uuid" }
```

### 3.11 BROADCAST（注入音频）

```
uuid_broadcast <call-id> <wav_path>
  向通话注入 WAV 音频流（替换实时 RTP）。底层调 playback_engine。
HTTP: POST /calls/:id/broadcast
  ⚠️ 字段名 wav_path 不是 wav！Rust body: BroadcastBody { wav_path: String }
  body: { "wav_path": "/audio/welcome.wav" }
```

### 3.12 ★ PLAYBACK（支持多文件 chain，工号播报）

```
uuid_playback <call-id> <wav|chain> [side] [loop]
  wav|chain: 单文件 或 管道符拼接多文件
    单文件示例: "/audio/welcome.wav"
    chain 示例: "9.wav|5.wav|2.wav|7.wav|serve.wav"  (工号播报场景)
  side: -1=both(默认), 0=A leg, 1=B leg
  loop: 0=播一次(默认), 1=循环
  多文件 chain 模式：按序依次播放，每个 EOF 自动 start 下一个，
  最后一个 EOF 自动 stop。loop 参数被忽略（chain 每个文件播一次）。
  硬上限 PB_CHAIN_MAX=8 文件。

uuid_playback_stop  <call-id>    # 立即停止（chain 全停）
uuid_playback_status <call-id>   # 查询是否在播
HTTP: POST /calls/:id/playback   POST /calls/:id/playback-stop  GET /calls/:id/playback-status
```

**HTTP 示例（工号播报）**:
```json
POST /api/v1/calls/550e8400.../playback
{
  "wav": "9.wav|5.wav|2.wav|7.wav|为您服务.wav",
  "side": -1,
  "loop": 0
}
# 响应: { "status": "ok", "is_chain": true, "call_id": "...", "core_response": "OK playback chain ..." }
```

### 3.13 BREAK（打断 playback / sleep）

```
uuid_break <call-id>
  打断当前 call 的 playback 播放 + dp_sleep 睡眠。
  双模：OAM 命令 + Dialplan app 同名。
HTTP: POST /calls/:id/break
```

### 3.14 RECORD（录制）

```
uuid_record <call-id> start|stop [path]
  start: 开始混合录制（默认 MIXED 模式）；path 可选（默认 /var/log/nsbc/recordings/）
  stop: 停止录制，返回实际文件路径
HTTP: POST /calls/:id/record
  body: { "action": "start", "path": "/custom/dir/" }
        { "action": "stop" }
```

### 3.15 ANSWER（200 OK）

```
uuid_answer <call-id>
  对 A 腿发 200 OK（通常 B-leg answer 后自动触发；此命令手动兜底）。
HTTP: POST /calls/:id/answer
```

### 3.16 SEND DTMF

```
uuid_send_dtmf <call-id> <digits> [volume] [dur_ms]
  digits: 0-9, *, #, A-D 序列（最多 32 字符）
  默认 volume=26 dBFS, dur_ms=100ms
HTTP: POST /calls/:id/send-dtmf
  ⚠️ Rust SendDtmfBody 只有 digits！无 duration_ms/volume 字段（C 层走默认值）
  body: { "digits": "123#" }
```

### 3.17 SEND MESSAGE（SIP MESSAGE）

```
send_msg <call-id> <message-body>
  对 A 腿发 SIP MESSAGE（text/plain 内容）。
HTTP: POST /calls/:id/send-msg
  ⚠️ 字段名 text 不是 body！Rust body: SendMessageBody { text: String }
  body: { "text": "text/plain: hello" }
```

### 3.18 GETVAR / SETVAR（通道变量）

```
uuid_getvar  <call-id> <name>      # 读通道变量（L1，只读）
uuid_setvar  <call-id> <name> [value]  # 写通道变量（L2）
  底层调 b2b_call_get_var / b2b_call_set_var → chan_var 表
  leg 参数内部统一 -1（双腿通用）。
HTTP: POST /calls/:id/getvar   POST /calls/:id/setvar
  getvar body:  { "name": "caller_id_number" }
  setvar body:  { "name": "agent_id", "value": "A1001" }
```

### 3.19 WAIT FOR ANSWER（阻塞）

```
uuid_wait_for_answer <call-id> [timeout_ms]
  轮询 B 腿 state 是否 LEG_CONFIRMED。默认 30s，最大 1h。
  超时返回 ERR timeout。
HTTP: POST /calls/:id/wait-for-answer
  body: { "timeout_ms": 10000 }
```

### 3.20 MEDIA BUG（媒体抓包附加器）

```
uuid_media_bug <call-id> start|stop|list [mask|bug_id]
  start [mask]:  mask=0x0F(默认全开), 0x01=RAW_RTP, 0x02=RTCP, 0x04=HEADER, 0x08=SIP
  stop [bug_id]: 停止指定 bug（不传则所有）
  list: 列出当前 call 上挂的所有 bug
HTTP: 暂无结构化路由；通过 ext.cmd 代理
  POST /ext/cmd  { "cmd": "uuid_media_bug xxxx start 0x0F" }
```

---

## 4. 全局 OAM 命令

### 4.1 originate（外呼）

```
originate <call-url> [&app]
  call-url: "sip:1001@10.0.0.1:5060" / "1001"
  app: 接通后执行的 Dialplan app，如 "&playback welcome.wav"
HTTP: POST /originate
  body: { "call_url": "sip:1001@10.0.0.1:5060" }
```

### 4.2 sofia（SIP 栈控制）

```
sofia siptrace on|off             # 全局 SIP 抓包开关
sofia profile <name> rescan       # 重新扫描 profile 配置
sofia profile <name> restart      # 重启 profile
```

### 4.3 console（日志控制）

```
console loglevel <0-7|name>       # 全局日志级别 0=EMERG...7=DEBUG
console siptrace on|off           # 控制台 SIP trace 开关
```

### 4.4 hep（HEP 告警）

```
hep status   # HEP sink 当前状态（队列深度、drop 计数）
hep flush    # 清空 HEP 发送队列
hep reload   # 重读 HEP 配置（target host/port）
```

### 4.5 mod（动态模块管理）

```
mod.load   <name>     # dlopen + init + start mod_<name>.so
mod.unload <name>     # stop + fini + dlclose
mod.reload <name>     # 一键热更（unload + load + init + start）
```

### 4.6 events / cdr / log / color / sub

```
events on|off            # 事件推送开关
cdr    on|off            # CDR 写文件开关
log    on|off|status     # 日志总开关
color  on|off            # 控制台彩色开关
sub    log|events|cdr|metrics   # 订阅子系统
```

### 4.7 test_push_cdr（测试工具）

```
test_push_cdr            # 往 data.sock 推一条模拟 CDR 验证 write-through
```

---

## 5. show/list/status 只读命令

### 5.1 核心 show

```
show calls          # 当前活动通话列表（call_id / from / to / state / leg）
show channels       # 当前 RTP channel 列表（ssrc / peer / codec / jitter）
show registrations  # 注册端点列表（username / contact / expire）
show endpoints      # 已配置端点
show rates          # 费率表
show rate_plans     # 费率方案
show balances       # 账户余额
show trunks         # SIP trunk 状态
show gateways       # gateway 状态
```

### 5.2 reload 组 list

```
profiles.list       # 已加载 SIP profiles
trunks.list         # 已加载 trunks 和 route_table
dialplan.show       # 当前生效 Dialplan rules（按优先级排序）
dialplan-apps       # 支持的 Dialplan app 清单（HTTP: GET /dialplan-apps）
```

### 5.3 status

```
status              # 系统综合状态（核心线程数、active_calls、uptime、内存）
```

---

## 6. reload\* 热加载命令

全部 L2_ADMIN，无参数或参数是模块名。多数 reload 为 Web DB authoritative（实际数据已在 DB，reload 只触发推送）。

| 命令 | 说明 | 真实实装 |
|------|------|----------|
| `extensions.reload` | 重加载号码 | ✅ JSON→内存 |
| `gateways.reload` | 重加载 gateway | ✅ |
| `trunks.reload` | 重加载 trunk_t | ✅ |
| `dialplan-rules.reload` | 重加载 dp_rule | ✅ |
| `queues.reload` | 重加载 ACD queue | ✅ |
| `profiles.reload` | RCU 原子替换 SIP profile | ✅ |
| `ha.reload` | 重加载 HA group | ⚠️ no-op（Web DB authoritative） |
| `ivr.reload` | 重加载 IVR menu | ✅ |
| `route_groups.reload` | 重加载 route group | ⚠️ no-op |
| `rate.reload` / `accounts.reload` / `rate_plans.reload` / `rate_plan_entries.reload` | 计费表 | ⚠️ no-op（Web DB authoritative） |

---

## 7. security.\* 安全命令

| 命令 | 说明 | 示例 |
|------|------|------|
| `security.ban.list [ip-substring]` | 列出 fail2ban ban/watch | `security.ban.list 192.168` |
| `security.ban.unban <ip>` | 解 ban | `security.ban.unban 1.2.3.4` |
| `security.ban.clear` | 清空 ban 表 | |
| `security.blacklist.list` | 列出静态黑名单（CIDR+UA） | |
| `security.blacklist.reload` | 从 JSON 重加载 | |
| `security.cfg.reload` | 重加载 fail2ban YAML 配置 | |
| `security.geoip.load_deny  <file>` | 追加 deny CIDR 批量文件 | |
| `security.geoip.load_allow <file>` | 追加 allow CIDR 批量文件 | |
| `security.geoip.reload` | 重读 seed + 所有 geoip 文件 | |
| `security.geoip.list` | 当前 deny/allow 数量 + allowlist 模式 | |

---

## 8. bug.\* 媒体 Bug 命令

| 命令 | 说明 |
|------|------|
| `bug.add <call-id> <leg_mask> <mask>` | 挂 RTP payload listener（`media_bug.c`） |
| `bug.del <bug_id>` | 拆除 listener |
| `bug.list` | 列出活跃 bug + counter |

---

## 9. Dialplan App 清单

Dialplan app 是 JSON rule 的 `"app"` 字段值，在 A-leg 接通后依次执行。**注意：桥接/routing 已由 rule target 自动处理，`bridge` app 已是 DEPRECATED no-op。**

| App | 说明 | data 示例 |
|-----|------|-----------|
| `answer` | 发 200 OK 应答来电 | — |
| `pre_answer` | Early Media 预应答（183 Session Progress） | — |
| `playback` | 播放单个或多文件音频 | `"welcome.wav"` 或 `"a.wav\|b.wav"` |
| `post_playback` | 接通后放音（语义同 playback） | `"welcome.wav"` |
| `endless_playback` | 循环播放（uuid_break 可打断） | `"moh.wav"` |
| `ringback` | 回铃音（B-leg 接通时自动停） | `"ring.wav"` |
| `play_and_get_digits` | 播提示音 + 收 DTMF | `"welcome.wav\|4\|#"` |
| `record` | 录制通话 | `""` 或 `"/path/to/out.wav"` |
| `transfer` | REFER 转移 | `"sip:1001@..."` |
| `att_xfer` | 出席/咨询转移 | `"sip:1001@..."` |
| `sleep` | 暂停（毫秒） | `"2000"` |
| `park` | 保持 + MoH | — |
| `set` | 设置通道变量 | `"agent_id=A1001"` |
| `conference` | 加入会议 | `"room123@conf-prof"` |
| `gentones` | 生成音调序列 | `"2600-2600-1100"` |
| `echo` | RTP 环回（self→self） | — |
| `ivr` | 启动 IVR 菜单 | `"main_menu"` |
| `uuid_break` | 打断当前 call 的 playback/sleep | — |
| `read` | DTMF 收号（P0 占位） | — |
| `bridge` | ⚠️ DEPRECATED — 已由 rule target 处理 | — |

---

## 10. HTTP API 路由总览

### 10.1 通话控制 `/api/v1/calls/*`

所有 body 字段名以 Rust `struct XxxBody` 真实定义为准（见 [calls.rs](file:///d:/github/nsbc/web/src/routes/calls.rs)）。**⚠️ 下表 body 列旧字段名已修正**。

| Method | Route | Body struct | Body JSON | C 命令 | 前端按钮 |
|--------|-------|-------------|-----------|--------|----------|
| GET | `/calls` | — | — | `show channels` | channels-live 数据源 |
| GET | `/calls/stream` | — | — | — (UDS sub) | SSE 自动刷新 |
| GET | `/vad/stream` | — | — | — (UDS sub) | SSE 自动推送 |
| GET | `/calls/:id` | — | — | — | 详情查询 |
| **POST** | `/calls/:id/hangup` | — | `{}` | `uuid_kill` (cause=487) | 🔴 **Hang up** |
| **POST** | `/calls/:id/kill` | `KillBody` | `{ "cause": 487 }` （可选） | `uuid_kill <id> <cause>` | ⋮ → Kill |
| **POST** | `/calls/:id/cancel` | `CancelBody` | `{ "leg": -1 \| 0 \| 1 }` （可选，默认 -1） | `uuid_cancel <id> [leg]` | ⋮ → Cancel |
| **POST** | `/calls/:id/answer` | — | `{}` | `uuid_answer <id>` | ⋮ → Answer |
| **POST** | `/calls/:id/hold` | `HoldBody` | `{ "on": true \| false }` ✅ **字段名 `on`** | `uuid_hold <id> on\|off` | ⏸️ Hold on / ▶️ Hold off |
| **POST** | `/calls/:id/mute` | `MuteBody` | `{ "on": true, "leg": -1 }` ✅ **字段名 `on`** | `uuid_mute <id> on\|off [leg]` | 🔇 Mute on / 🎙️ Mute off |
| **POST** | `/calls/:id/transfer` | `TransferBody` | `{ "dest": "sip:1001@..." }` | `uuid_transfer <id> <dest>` | ⋮ → Transfer |
| **POST** | `/calls/:id/deflect` | `DeflectBody` | `{ "dest": "..." }` | `uuid_deflect <id> <dest>` | ⋮ → Deflect |
| **POST** | `/calls/:id/displace` | — | `{}` | `uuid_displace <id>` | ⋮ → Displace |
| **POST** | `/calls/:id/park` | — | `{}` | `uuid_park <id>` | ⋮ → Park |
| **POST** | `/calls/:id/unpark` | — | `{}` | `uuid_unpark <id>` | ⋮ → Unpark |
| **POST** | `/calls/:id/bridge` | `BridgeBody` | `{ "call_id_b": "<uuid>" }` | `uuid_bridge <id-A> <id-B>` | ⋮ → Bridge |
| **POST** | `/calls/:id/broadcast` | `BroadcastBody` | `{ "wav_path": "/abs/path/xxx.wav" }` ✅ **字段名 `wav_path`** | `uuid_broadcast <id> <path>` | ⋮ → Broadcast |
| **POST** | `/calls/:id/playback` | `PlaybackBody` | `{ "wav": "welcome.wav \| 9.wav\|5.wav\|...", "side": -1, "loop": 0 }` | `uuid_playback <id> <wav\|chain> [side] [loop]` | 🔊 **Playback** |
| **POST** | `/calls/:id/playback-stop` | — | `{}` | `uuid_playback_stop <id>` | ⏹️ Playback Stop |
| **GET** | `/calls/:id/playback-status` | — | — | `uuid_playback_status <id>` | ⋮ → Playback Status |
| **POST** | `/calls/:id/break` | — | `{}` | `uuid_break <id>` | ✂️ Break |
| **POST** | `/calls/:id/record` | `RecordBody` | `{ "action": "start"\|"stop", "path": "/optional/dir/" }` | `uuid_record <id> start\|stop [path]` | 🔴● Record |
| **POST** | `/calls/:id/send-dtmf` | `SendDtmfBody` | `{ "digits": "123#" }` ✅ **只有 digits，无 duration_ms** | `uuid_send_dtmf <id> <digits>` | 📡 Send DTMF |
| **POST** | `/calls/:id/send-msg` | `SendMessageBody` | `{ "text": "Hello" }` ✅ **字段名 `text` 不是 `body`** | `send_msg <id> <text>` | ⋮ → Send MESSAGE |
| **POST** | `/calls/:id/getvar` | `GetvarBody` | `{ "name": "caller_id_number" }` | `uuid_getvar <id> <name>` | ⋮ → GetVar |
| **POST** | `/calls/:id/setvar` | `SetvarBody` | `{ "name": "agent_id", "value": "A1001" }` | `uuid_setvar <id> <name> [value]` | ⋮ → SetVar |
| **POST** | `/calls/:id/wait-for-answer` | `WaitForAnswerBody` | `{ "timeout_ms": 10000 }` （可选，默认 10s，最大 60s） | `uuid_wait_for_answer <id> [ms]` | ⋮ → Wait for Answer |
| **POST** | `/calls/:id/vad-start` | `VadStartBody` | `{ "threshold": 800 }` （可选，默认 800） | `uuid_vad_start <id> [thr]` | ⋮ → VAD Start |
| **POST** | `/calls/:id/vad-stop` | — | `{}` | `uuid_vad_stop <id>` | ⋮ → VAD Stop |
| **POST** | `/originate` | `OriginateBody` | `{ "call_url": "sip:1001@nsbc.local" }` | `originate <call-url>` | 顶部 Originate 按钮 |

#### ⚠️ 字段名修正汇总（对照 Rust 定义）

| 旧文档字段 | 正确字段 | Rust struct |
|-----------|----------|-------------|
| `{ "hold": true }` | **`{ "on": true }`** | `HoldBody { on: bool }` |
| `{ "mute": true, "leg": -1 }` | **`{ "on": true, "leg": -1 }`** | `MuteBody { on: bool, leg: Option<i32> }` |
| `{ "wav": "...", "side": -1 }` (broadcast) | **`{ "wav_path": "..." }`** | `BroadcastBody { wav_path: String }` 无 side 字段 |
| `{ "digits": "123#", "duration_ms": 100 }` | **`{ "digits": "123#" }`** | `SendDtmfBody` 只有 digits |
| `{ "body": "..." }` (send-msg) | **`{ "text": "..." }`** | `SendMessageBody { text: String }` |
| `{ "side": -1, "loop": 0 }` (playback) | **保留正确** | `PlaybackBody { wav, side, loop }` |

#### 前端按钮 → Rust 路由 → C 命令精确映射

（按 `app-core.js` window.nsbcXxx 函数定义 + `pages-live.js` actionCol / openCallMenu 渲染）

| 前端按钮 | JS 函数 | 路由 | Body |
|----------|---------|------|------|
| 🔴 Hang up | `nsbcHangup` | `POST /calls/:id/hangup` | `{}` |
| ⏸️ Hold on | `nsbcHold(id, true)` | `POST /calls/:id/hold` | `{ on: true }` |
| ▶️ Hold off | `nsbcHold(id, false)` | `POST /calls/:id/hold` | `{ on: false }` |
| 🔇 Mute on | `nsbcMute(id, true)` | `POST /calls/:id/mute` | `{ on: true, leg: -1 }` |
| 🎙️ Mute off | `nsbcMute(id, false)` | `POST /calls/:id/mute` | `{ on: false, leg: -1 }` |
| 🔊 Playback | `nsbcPlayback` | `POST /calls/:id/playback` | `{ wav, side, loop }` |
| ⏹️ Playback Stop | `nsbcPlaybackStop` | `POST /calls/:id/playback-stop` | `{}` |
| 🔴● Record | `nsbcRecord` | `POST /calls/:id/record` | `{ action, path? }` |
| ✂️ Break | `nsbcBreak` | `POST /calls/:id/break` | `{}` |
| 📡 Send DTMF | `nsbcSendDtmf` | `POST /calls/:id/send-dtmf` | `{ digits }` |
| ⋮ → Kill | `nsbcKill` | `POST /calls/:id/kill` | `{ cause? }` |
| ⋮ → Cancel | `nsbcCancel` | `POST /calls/:id/cancel` | `{ leg? }` |
| ⋮ → Answer | `nsbcAnswer` | `POST /calls/:id/answer` | `{}` |
| ⋮ → Park | `nsbcPark` | `POST /calls/:id/park` | `{}` |
| ⋮ → Unpark | `nsbcUnpark` | `POST /calls/:id/unpark` | `{}` |
| ⋮ → Displace | `nsbcDisplace` | `POST /calls/:id/displace` | `{}` |
| ⋮ → Transfer | `nsbcTransfer` | `POST /calls/:id/transfer` | `{ dest }` |
| ⋮ → Deflect | `nsbcDeflect` | `POST /calls/:id/deflect` | `{ dest }` |
| ⋮ → Bridge | `nsbcBridge` | `POST /calls/:id/bridge` | `{ call_id_b }` |
| ⋮ → Wait-for-answer | `nsbcWaitForAnswer` | `POST /calls/:id/wait-for-answer` | `{ timeout_ms? }` |
| ⋮ → Broadcast | `nsbcBroadcast` | `POST /calls/:id/broadcast` | `{ wav_path }` |
| ⋮ → Playback Status | `nsbcPlaybackStatus` | `GET /calls/:id/playback-status` | — |
| ⋮ → VAD Start | `nsbcVadStart` | `POST /calls/:id/vad-start` | `{ threshold? }` |
| ⋮ → VAD Stop | `nsbcVadStop` | `POST /calls/:id/vad-stop` | `{}` |
| ⋮ → Send MESSAGE | `nsbcSendMsg` | `POST /calls/:id/send-msg` | `{ text }` |
| ⋮ → GetVar | `nsbcGetVar` | `POST /calls/:id/getvar` | `{ name }` |
| ⋮ → SetVar | `nsbcSetVar` | `POST /calls/:id/setvar` | `{ name, value? }` |
| (global) Originate | `nsbcOriginate` | `POST /originate` | `{ call_url }` |

#### 典型错误响应（所有 action 通用）

```json
// 404 — call 不存在
{ "error": "notfound", "message": "call not found: 550e8400..." }

// 502 — C core 拒绝命令
{ "error": "protocol", "message": "core returned -2 (NSBC_NOENT)" }

// 400 — body 字段缺失/类型错
{ "error": "invalid_body", "message": "missing required field 'dest'" }
```

#### curl 完整示例（Web JWT `/api/v1/calls/*`）

```bash
# ===== 准备 =====
export NSBC_TOKEN="eyJhbGciOiJIUzI1NiIs..."       # POST /auth/login 获得
export NSBC_HOST="http://10.0.0.80:8088"
export CID="550e8400-e29b-41d4-a716-446655440000"

# ==== 查询类（GET，可选拼 tenant_id= 参数）====
curl -s "$NSBC_HOST/api/v1/calls" -H "Authorization: Bearer $NSBC_TOKEN" | jq .
curl -s "$NSBC_HOST/api/v1/calls/$CID" -H "Authorization: Bearer $NSBC_TOKEN" | jq .

# ==== 挂断类（POST，body {} 或带 cause）====
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/hangup" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/kill"   -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"cause":486}'

# ==== Hold / Mute（⚠️ 字段名 on，不是 hold/mute）====
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/hold" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"on":true}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/hold" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"on":false}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/mute" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"on":true,"leg":-1}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/mute" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"on":false,"leg":-1}'

# ==== Park / Unpark / Displace / Answer / Cancel / Break（无 body）====
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/park"      -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/unpark"    -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/displace"  -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/answer"    -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/break"     -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/cancel"    -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"leg":-1}'

# ==== Transfer / Deflect / Bridge（带 dest 或 call_id_b）====
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/transfer" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"dest":"sip:1001@nsbc.local"}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/deflect"  -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"dest":"sip:1001@nsbc.local"}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/bridge"   -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"call_id_b":"aaaa-bbbb-cccc-dddd"}'

# ==== ★ Playback（支持 chain，工号播报）====
# 单文件
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/playback" \
  -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" \
  -d '{"wav":"/audio/welcome.wav","side":-1,"loop":0}'
# 多文件 chain（9.wav|5.wav|2.wav|7.wav|serve.wav 按序依次播报工号）
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/playback" \
  -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" \
  -d '{"wav":"9.wav|5.wav|2.wav|7.wav|serve.wav","side":-1,"loop":0}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/playback-stop"  -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{}'
curl -s     "$NSBC_HOST/api/v1/calls/$CID/playback-status"   -H "Authorization: Bearer $NSBC_TOKEN" | jq .

# ==== Broadcast（⚠️ 字段名 wav_path，无 side）====
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/broadcast" \
  -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" \
  -d '{"wav_path":"/audio/hold_music.wav"}'

# ==== Record ====
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/record" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"action":"start","path":"/var/log/nsbc/recordings/"}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/record" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"action":"stop"}'

# ==== Send DTMF（只有 digits！无 duration_ms）====
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/send-dtmf" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"digits":"123#"}'

# ==== Send MESSAGE（⚠️ 字段名 text，不是 body）====
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/send-msg" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"text":"Hello from Web UI"}'

# ==== VAD ====
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/vad-start" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"threshold":800}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/vad-stop"  -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{}'

# ==== Wait for answer（阻塞直到 B-leg 200 OK 或超时）====
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/wait-for-answer" \
  -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" \
  -d '{"timeout_ms":10000}'

# ==== GetVar / SetVar（通道变量）====
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/getvar" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"name":"caller_id_number"}'
curl -s -X POST "$NSBC_HOST/api/v1/calls/$CID/setvar" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"name":"agent_id","value":"A1001"}'

# ==== Originate（全局，无 callId）====
curl -s -X POST "$NSBC_HOST/api/v1/originate" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d '{"call_url":"sip:1001@nsbc.local"}'
```

#### Shell 辅助函数（放到 ~/.bashrc）

```bash
# 一行调用任意 calls 动作
nsbc_call() {
  local path="/api/v1/calls/$1/$2"; shift 2
  local body="${1:-{}}"
  curl -s -X POST "$NSBC_HOST$path" \
    -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d "$body" \
    | jq .
}
# 用法: nsbc_call 550e... playback '{"wav":"9.wav|5.wav|serve.wav","side":-1,"loop":0}'
#       nsbc_call 550e... hold    '{"on":true}'
nsbc_list() { curl -s "$NSBC_HOST/api/v1/calls" -H "Authorization: Bearer $NSBC_TOKEN" | jq '.calls[]'; }
nsbc_orig() { curl -s -X POST "$NSBC_HOST/api/v1/originate" -H "Authorization: Bearer $NSBC_TOKEN" -H "Content-Type: application/json" -d "{\"call_url\":\"$1\"}" | jq .; }
```

### 10.2 外部 API `/ext/v1/*`（API Key `nsbc_live_*` 鉴权）

与 Web UI 的 `/api/v1/*`（JWT 鉴权）完全隔离。面向 CRM / AI Agent / IVR 平台。

| Method | Route | Scope | 说明 |
|--------|-------|-------|------|
| POST | `/ext/v1/cmd` | admin | **原始 OAM 命令代理**（绕过前缀白名单的 reload/config/acl 类需 admin）。body: `{"cmd":"uuid_playback xxxx \\\"9.wav\\|5.wav\\\""}` |
| POST | `/ext/v1/originate` | readwrite | 外呼。body: `{"call_url":"sip:1001@..."}` |
| POST | `/ext/v1/calls/{id}/hangup` | readwrite | 强制挂断 |
| GET | `/ext/v1/calls` | readonly | 活动通话列表 |
| GET | `/ext/v1/events/stream` | readwrite | 全局事件 SSE（CALL_START/CALL_ANSWER/CALL_END/CDR） |
| GET | `/ext/v1/system/status` | readonly | 综合状态（active_calls/uptime/mem） |
| GET | `/ext/v1/system/version` | readonly | 版本 + Git commit + web 版本 |
| GET | `/ext/v1/docs` | readonly | 动态自描述（外部系统可发现 API） |
| GET | `/ext/v1/cmd/help` | readonly | 返回所有命令帮助文本 |

**重要**：ext/v1 **只有 1 个 calls 结构化写端点**（hangup）。其他 23 个 uuid_* 动作**全部走 `/ext/v1/cmd` 透明代理**——这是设计选择，不是缺失。cmd 代理要求 admin scope；结构化端点是 readwrite。

#### curl 完整示例（API Key `Authorization: Bearer nsbc_live_<32hex>`）

```bash
# ===== 准备 =====
export NSBC_KEY="nsbc_live_3f9a7b2c1d4e5f8a9b0c1d2e3f4a5b6c"    # seed key 自动生成
export NSBC_HOST="http://10.0.0.80:8088"
export CID="550e8400-e29b-41d4-a716-446655440000"
export OTHER="aaaa-bbbb-cccc-dddd"
# helper: 所有 ext/v1 命令（不管结构化还是 cmd 代理）都用这组 header
H="-H Authorization: Bearer $NSBC_KEY"

# ==== 系统只读（readonly scope）====
curl -s "$NSBC_HOST/ext/v1/system/status"       $H | jq .
curl -s "$NSBC_HOST/ext/v1/system/version"      $H | jq .
curl -s "$NSBC_HOST/ext/v1/calls"                $H | jq '.calls[]'
curl -s "$NSBC_HOST/ext/v1/docs"                 $H | jq '.endpoints[] | {route,method,scope}'
curl -s "$NSBC_HOST/ext/v1/cmd/help"            $H    # 返回纯文本帮助

# ==== 结构化端点（readwrite scope）====
curl -s -X POST "$NSBC_HOST/ext/v1/calls/$CID/hangup" $H -H "Content-Type: application/json" -d '{}'
curl -s -X POST "$NSBC_HOST/ext/v1/originate"         $H -H "Content-Type: application/json" \
  -d '{"call_url":"sip:1001@nsbc.local"}'

# ==== ★ SSE 事件流（readwrite scope）====
# 持续接收 CALL_START / CALL_ANSWER / CALL_HOLD / CALL_MUTE / CALL_END / CDR / PLAYBACK_START / PLAYBACK_END ...
curl -sN "$NSBC_HOST/ext/v1/events/stream" $H &
# Ctrl+C 或 kill 后台进程结束；每条事件格式: data: {"event":"CALL_ANSWER","call_id":"...","ts":"2026-11-14T10:00:00Z"}
```

#### ★ `/ext/v1/cmd` 透明代理 — 覆盖所有 uuid_* 动作（admin scope）

这是 ext/v1 外部 API 面的**主力写路径**。绕过 Rust handler 直接把命令字符串透传给 C core 的 `cmd_registry`。所有 §3 的 uuid_* 命令都能用。

**⚠️ 命令字符串里的管道符 / 引号必须正确转义**：JSON body 里 `"9.wav|5.wav"` 不需要额外转义管道，但双引号要 `\"`。

```bash
# 基础模板
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" \
  -H "Authorization: Bearer $NSBC_KEY" \
  -H "Content-Type: application/json" \
  -d '{"cmd":"<完整命令字符串>"}'

# ==== 核心通话控制 ====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_kill '"$CID"' 487"}'
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_cancel '"$CID"' -1"}'
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_answer '"$CID"'"}'

# ==== Hold / Mute ====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" -d '{"cmd":"uuid_hold '"$CID"' on"}'
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" -d '{"cmd":"uuid_hold '"$CID"' off"}'
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" -d '{"cmd":"uuid_mute '"$CID"' on -1"}'
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" -d '{"cmd":"uuid_mute '"$CID"' off -1"}'

# ==== Park / Unpark / Displace ====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" -d '{"cmd":"uuid_park '"$CID"'"}'
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" -d '{"cmd":"uuid_unpark '"$CID"'"}'
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" -d '{"cmd":"uuid_displace '"$CID"'"}'

# ==== Transfer / Deflect / Bridge ====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_transfer '"$CID"' sip:1001@nsbc.local"}'
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_deflect '"$CID"' sip:1001@nsbc.local"}'
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_bridge '"$CID"' '"$OTHER"'"}'

# ==== ★ Playback 单文件 ====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_playback '"$CID"' welcome.wav"}'
# side=both, loop=0
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_playback '"$CID"' /audio/welcome.wav -1 0"}'

# ==== ★ Playback 多文件 chain —— 工号播报 ====
# 9.wav|5.wav|2.wav|7.wav|serve.wav 按序依次播放
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_playback '"$CID"' \"9.wav|5.wav|2.wav|7.wav|serve.wav\""}'
# 带 side=-1 both, loop=0
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_playback '"$CID"' \"9.wav|5.wav|2.wav|7.wav|serve.wav\" -1 0"}'
# 停止
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_playback_stop '"$CID"'"}'
# 查询状态
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_playback_status '"$CID"'"}'

# ==== Broadcast ====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_broadcast '"$CID"' /audio/hold_music.wav"}'

# ==== Break ====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_break '"$CID"'"}'

# ==== Record ====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_record '"$CID"' start /var/log/nsbc/recordings/"}'
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_record '"$CID"' stop"}'

# ==== Send DTMF ====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_send_dtmf '"$CID"' 123#"}'

# ==== Send MESSAGE ====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"send_msg '"$CID"' \"Hello from CRM\""}'

# ==== VAD ====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_vad_start '"$CID"' 800"}'
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_vad_stop '"$CID"'"}'

# ==== Wait for answer ====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_wait_for_answer '"$CID"' 10000"}'

# ==== GetVar / SetVar ====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_getvar '"$CID"' caller_id_number"}'
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"uuid_setvar '"$CID"' agent_id A1001"}'

# ==== 重载命令（reload/config 类也能走 cmd 代理 —— admin scope 必需）====
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"mod.reload profile-sip"}'
curl -s -X POST "$NSBC_HOST/ext/v1/cmd" $H -H "Content-Type: application/json" \
  -d '{"cmd":"profiles.reload"}'
```

#### Shell 辅助函数（放到 ~/.bashrc）

```bash
# ext/v1 cmd 代理一行调用任意 OAM 命令
nsbc_cmd() {
  curl -s -X POST "$NSBC_HOST/ext/v1/cmd" \
    -H "Authorization: Bearer $NSBC_KEY" -H "Content-Type: application/json" \
    -d "{\"cmd\":\"$1\"}"
}
# 用法: nsbc_cmd "uuid_hold $CID on"
#       nsbc_cmd "uuid_playback $CID \"9.wav|5.wav|serve.wav\" -1 0"

# ext/v1 结构化端点（只有 hangup/originate/calls/system/events 几个）
nsbc_ext_hangup() { curl -s -X POST "$NSBC_HOST/ext/v1/calls/$1/hangup" $H -H "Content-Type: application/json" -d '{}' | jq .; }
nsbc_ext_orig()   { curl -s -X POST "$NSBC_HOST/ext/v1/originate" $H -H "Content-Type: application/json" -d "{\"call_url\":\"$1\"}" | jq .; }
nsbc_ext_list()   { curl -s "$NSBC_HOST/ext/v1/calls" $H | jq '.calls[]'; }
nsbc_ext_status() { curl -s "$NSBC_HOST/ext/v1/system/status" $H | jq .; }
nsbc_ext_stream() { curl -sN "$NSBC_HOST/ext/v1/events/stream" $H; }  # 前台持续接收
```

#### ext/v1 vs /api/v1 路径选择指南

| 场景 | 推荐路径 | 原因 |
|------|----------|------|
| CRM/IVR/AI Agent 集成 | **`/ext/v1/cmd`** | 一个 API Key 管所有 uuid_* 动作，不用为每个 action 单独开发 JSON body |
| 高频只读状态查询 | `/ext/v1/calls` + `/ext/v1/system/*` | readonly scope 足够，限流宽松 |
| 需要 SSE 事件流 | `/ext/v1/events/stream` | 一条连接收所有 CALL_* + CDR 事件 |
| Web UI 内部运维 | `/api/v1/calls/*` | JWT rbac + 前端 toast/SSE 已对接 |
| 需要细粒度 rbac（tenant 隔离） | `/api/v1/calls/*` | 带 tenant_id 参数的多租户路由 |
| 只需要 hangup/originate 两个写动作 | `/ext/v1/originate` + `/ext/v1/calls/{id}/hangup` | 结构化端点不需要 admin scope |

### 10.3 配置管理 `/api/v1/`

| Method | Route | 说明 |
|--------|-------|------|
| GET/PUT/POST/DELETE | `/sip-profiles` | SIP profile CRUD + `/reload` |
| GET/PUT/POST/DELETE | `/gateways` | Gateway CRUD + `/reload` |
| GET/PUT/POST/DELETE | `/extensions` | 号码规则 CRUD + `/reload` |
| GET/PUT/POST/DELETE | `/dialplan-rules` | Dialplan rule CRUD + `/reload` |
| GET/PUT/POST/DELETE | `/routes` | 路由组 CRUD + `/reload` |
| GET/PUT/POST/DELETE | `/queues` | ACD queue CRUD + `/reload` |
| GET/PUT/POST/DELETE | `/ivr-menus` | IVR 菜单 CRUD + `/reload` |
| GET/PUT/POST/DELETE | `/ha-groups` | HA 组 CRUD + `/reload` |
| GET | `/codecs` | 当前支持的 codec 清单 |
| GET | `/dialplan-apps` | Dialplan app 清单（等价 `dialplan-apps`） |

### 10.4 计费 `/api/v1/`

| Method | Route | 说明 |
|--------|-------|------|
| GET/POST | `/rates` | 费率 CRUD |
| POST | `/rates/reload` | 重加载 |
| GET | `/rates/core` | C core 端当前生效费率 |
| GET/POST | `/balances` | 账户余额 CRUD |
| GET | `/balances/core` | C core 端当前余额视图 |
| GET/PUT/POST/DELETE | `/balances/:id` | 单个账户管理 |
| POST | `/balances/:id/topup` | 充值 |
| POST | `/balances/:id/refund` | 退款 |
| GET | `/reservations/core` | 预留额度视图 |
| GET/PUT | `/enabled` | 计费开关 |
| GET/POST | `/rate-plans` | 费率方案 CRUD + `/reload` |
| GET/POST | `/rate-plan-entries` | 费率方案条目 CRUD + `/reload` |
| GET | `/transactions` | 交易流水 |

### 10.5 监控 `/api/v1/`

| Method | Route | 说明 |
|--------|-------|------|
| GET | `/metrics` | Prometheus metrics |
| GET | `/cdrs` | CDR 查询 |
| GET | `/recordings` | 录音列表 + `/recordings/*path` 下载 |
| GET | `/registrations` | 注册状态 |
| GET | `/security/bans` | fail2ban ban 表 |

### 10.6 认证 `/api/v1/`

| Method | Route | 说明 |
|--------|-------|------|
| GET | `/auth/captcha` | 获取验证码 |
| POST | `/auth/login` | 登录返回 JWT |
| POST | `/auth/refresh` | 刷新 JWT |
| POST | `/auth/logout` | 登出 |
| POST | `/auth/change-password` | 改密码 |
| POST | `/auth/step-up` | 提权（扩展 key） |

---

## 11. 附录：错误码对照

| C 宏 | 值 | 含义 | HTTP 映射 |
|------|----|------|-----------|
| `NSBC_OK` | 0 | 成功 | 200 |
| `NSBC_INVAL` | -1 | 参数无效 | 400 |
| `NSBC_NOENT` | -2 | 资源不存在（call not found / var not found） | 404 |
| `NSBC_ERR` | -3 | 通用错误 | 500 |
| `NSBC_NOMEM` | -4 | 内存不足 | 500 |
| `NSBC_NOTSUP` | -5 | 功能不支持（codec / 平台） | 501 |
| `NSBC_AGAIN` | -6 | 重试 | 503 |
| `NSBC_BUSY` | -7 | 资源忙 | 503 |

### 典型 HTTP 响应体格式

```json
// 成功（写操作）
{ "status": "ok", "call_id": "550e8400-...", "side": -1, "loop": 0 }

// 成功（查操作）
{ "call_id": "550e...", "core_response": "OK ..." }

// 错误
{ "error": "notfound", "message": "call not found" }     // 404
{ "error": "timeout", "message": "B-leg did not answer" } // 408
{ "error": "protocol", "message": "core rejected command" } // 502
```

---

*文档由代码自动扫描生成，精确反映当前仓库命令注册状态。*
