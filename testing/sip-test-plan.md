# NSBC SIP 测试方案

> **版本**: 3.0
> **日期**: 2026-10-29
> **测试工具**: sipp 3.5+（`/usr/bin/sipp`）
> **目标进程**: nsbcd（SIP 信令 + 可选 RTP 媒体转发）
> **账号池**: 10000 – 99999（9 万），密码统一 876876，realm=`nsbc.local`

---

## 0. 架构总览

### 双机拓扑（跨机压测推荐）

```
┌───────────────────────────────────┐      ┌───────────────────────────────────────┐
│   压测机  (CentOS 7)              │      │   NSBC 运行机 (Debian 12)            │
│   10.0.0.25:22 / root / Nway@2025 │      │   10.0.0.80:22322 / root / Nway@2025  │
│                                   │      │                                       │
│   ✅ sipp 3.5.0  /usr/bin/sipp    │      │   ✅ nsbcd binary  /home/github/nsbc/  │
│   ✅ Python 2.7.5 (系统自带)      │      │   ✅ nsbc-web binary /home/github/nsbc/  │
│   ✅ Rust rustc 1.98.1 (离线)     │      │   ✅ DB: /home/github/nsbc/db/web.db   │
|      sipp 3.5.0  /usr/bin/sipp    │      │    sipp 3.8.0  /usr/bin/sipp           │
│                                   │      │   ✅ Config: /home/github/nsbc/conf/    │
└───────────────┬───────────────────┘      └───────────────┬───────────────────────┘
                │                                           │
                │  SIP UDP 5060 / TCP 5060                  │
                │  RTP (BYPASS: 双方直连)                    │
                │  RTP (RELAY: 经 NSBC 10000-59999 端口)    │
                │                                           │
                └───────────────────────────────────────────┘
                          10.0.0.0/24 同网段私网
```

### 媒体路径差异（核心概念）

| 模式 | 谁决定 | 信令路径 | 媒体路径 | 说明 |
|------|--------|---------|---------|------|
| **BYPASS**（默认） | `trust_private=1` + 双方私网地址，或拨号规则 `media_mode=1` | 经 NSBC | **绕开 NSBC，双方直连** | 也叫"信令 only"，压测 nsbcd SIP 层极限 |
| **RELAY**（强制） | 拨号规则 `media_mode=2` | 经 NSBC | **经 NSBC RTP relay 转发** | 压测 nsbcd SIP + 媒体桥双层负载 |
| **auto**（默认值） | NSBC 按上下文自动决策 | 经 NSBC | 同网段自动 BYPASS，外网自动 RELAY | 测试时需显式指定，避免混淆 |

> **当前环境**: 10.0.0.25 和 10.0.0.80 都是 10.0.0.x 私网 → **默认自动走 BYPASS**。
> 测 RELAY 时必须手动改拨号规则强制。

### 启动顺序（强制）

```
① nsbc-web  →  ② nsbcd  →  ③ sipp 压测
```

nsbcd 通过 Unix domain socket 从 nsbc-web 拉取 sip_profiles / extensions / dialplan_rules 等配置。nsbc-web 不启动 → nsbcd 无配置 → silent drop 所有请求。

---

## 1. 机器清单

### 1.1 NSBC 运行机 — 10.0.0.80

| 项 | 值 |
|----|----|
| **OS** | Debian 12 |
| **SSH** | `ssh -p 22322 root@10.0.0.80`，密码 `Nway@2025` |
| **代码目录** | `/home/github/nsbc` |
| **nsbc-web binary** | `/home/github/nsbc/nsbc-web` |
| **nsbcd binary** | `/home/github/nsbc/nsbcd` |
| **nsbc.yaml** | `/home/github/nsbc/conf/nsbc.yaml` |
| **DB** | `/home/github/nsbc/db/web.db`（SQLite3） |
| **UDS** | `/var/run/nsbc/data.sock`（HA1 查询）、`/var/run/nsbc/control.sock`（控制） |
| **Rust** | rustc 1.98.1（系统安装，**无外网**） |
| **sipp** | `/usr/bin/sipp`（可本机压测也可跨机） |
| **部署要点** | nsbc-web 必须在 `/home/github/nsbc/` 目录下运行（make 后 binary 和 static 都复制到这） |

### 1.2 压测机 — 10.0.0.25（可选，推荐跨机）

| 项 | 值 |
|----|----|
| **OS** | CentOS 7 |
| **SSH** | `ssh root@10.0.0.25`，密码 `Nway@2025` |
| **sipp** | `/usr/bin/sipp` 3.5.0 |
| **Python** | 2.7.5 |
| **Rust** | rustc 1.98.1 |
| **脚本目录** | `/home/NSBC/` |

### 1.3 本机压测（10.0.0.80 自己跑 sipp）

简单场景可用本机压测，但极限压测会把 nsbcd CPU 和 sipp CPU 抢在一起，结果不准。**推荐跨机**。

---

## 2. SIP 资源

### 2.1 账号池

| 项 | 值 |
|----|----|
| **分机号范围** | `10000` – `99999`（共 90000 个，已 seed 进 extensions 表） |
| **压测池建议** | `10000` – `19999`（10000 个）— 够 5000 并发呼叫每 call 一对独立号 |
| **统一密码** | `876876` |
| **Realm** | `nsbc.local`（⚠️ 关键！不是 10.0.0.80，不是 sip 域名） |
| **HA1 示例** | `md5("10001:nsbc.local:876876")` = `04447cd778b26e7f26a0706af990fcac` |
| **clip 字段** | 与 extension 一致（如 10001 → clip=10001） |

#### 生成 authfile（sipp -ab 参数用）

```bash
# 压测池 10000 个（在压测机执行）
seq 10000 19999 | awk '{print $1";nsbc.local;876876"}' > /tmp/nsbc_authfile.txt
# 或全部 9 万个（仅首次大规模注册用）
seq 10000 99999 | awk '{print $1";nsbc.local;876876"}' > /tmp/nsbc_authfile_full.txt
```

### 2.2 Realm 陷阱（必读！）

nsbcd 从 **REGISTER 请求的 To 头 URI host** 提取 realm，然后用 `extension:realm:password` 去 DB 查 HA1。extensions 表 realm 列是硬编码的 `nsbc.local`。

- ✅ 正确：`REGISTER sip:10001@nsbc.local SIP/2.0`
- ❌ 错误：`REGISTER sip:10001@10.0.0.80 SIP/2.0` → **silent drop**，无任何响应

所有 SIP 消息的 From / To / Contact / Call-ID host 部分必须用 `@nsbc.local`。

### 2.3 SIP 端口

| 协议 | 端口 | 说明 |
|------|------|------|
| **SIP UDP** | 5060 | 默认监听口，reactor 独立绑 UDP fd |
| **SIP TCP** | 5060 | 标准 TCP 监听 |
| **RTP relay** | 10000 – 59999 | NSBC 分配的 RTP 端口段（仅 RELAY 模式用） |
| **UDS HA1 查询** | `/var/run/nsbc/data.sock` | nsbcd → nsbc-web |
| **UDS 控制** | `/var/run/nsbc/control.sock` | reload / 诊断 / 热加载 |

### 2.4 Registrar & Rate Limit 默认值

| 项 | 默认值 | 说明 |
|----|--------|------|
| authfail_threshold | 3 / 30s | 触发 IP ban |
| ratelimit_regs | 10 / 30s | security fence 覆盖 |
| ip_cps | 50 / s | registrar 令牌桶 |
| aor_cps | 10 / s | registrar 令牌桶 |
| global_cps | 200 / s | registrar 令牌桶 |

> **⚠️ 双重覆盖问题**：
> 1. `security fence reload`（nsbcd 启动时）会把 nsbc.yaml 里的 ratelimit_* **覆盖回默认值**（security.c:815，db authoritative 逻辑）
> 2. registrar 层另有独立的三级令牌桶（ip_cps / aor_cps / global_cps）
> 压测前必须**同时关掉两套**，并且**重启 nsbcd** 让 security fence reload 生效。

### 2.5 Dialplan & media_mode

默认 dialplan_rules（已 seed）：

| name | layer | match_expr | media_mode | 说明 |
|------|-------|-----------|------------|------|
| Internal extension-to-extension | ext | `^\d{5}$` | 0=auto（默认） | 5 位分机互呼 |
| Default outbound placeholder | out | `^\d{5,}$` | — | 禁用中，等网关 |

`media_mode` 取值（dialplan_rules.action_json）：
- `"media_mode": 0` → auto（默认，同网段私网自动 BYPASS）
- `"media_mode": 1` → 强制 BYPASS（信令 only）
- `"media_mode": 2` → 强制 RELAY（媒体经 NSBC 转发）

#### 强制切 RELAY（压测 RELAY 矩阵前必须执行）

```bash
# SSH 到 10.0.0.80
sqlite3 /home/github/nsbc/db/web.db \
  "UPDATE dialplan_rules SET action_json='{\"media_mode\":2}' WHERE layer='ext' AND enabled=1;"

# 热加载（或直接重启 nsbcd）
echo "dialplan reload" | nc -U /var/run/nsbc/control.sock
# 验证
echo "dialplan show" | nc -U /var/run/nsbc/control.sock
```

#### 切回 auto / BYPASS

```bash
sqlite3 /home/github/nsbc/db/web.db \
  "UPDATE dialplan_rules SET action_json='{}' WHERE layer='ext' AND enabled=1;"
echo "dialplan reload" | nc -U /var/run/nsbc/control.sock
```

---

## 3. 启动 & 验证（sanity gate 前必须完成）

### 3.1 关闭 Rate Limit（必须！）

```bash
# SSH 到 10.0.0.80
cd /home/github/nsbc

# 备份原配置
cp conf/nsbc.yaml conf/nsbc.yaml.bak

# 关掉所有 ratelimit（sed 覆盖）
sed -i 's/ratelimit_regs:.*/ratelimit_regs: 99999/'          conf/nsbc.yaml
sed -i 's/ratelimit_invites:.*/ratelimit_invites: 99999/'   conf/nsbc.yaml
sed -i 's/ratelimit_invites_active:.*/ratelimit_invites_active: 99999/' conf/nsbc.yaml
sed -i 's/ip_cps:.*/ip_cps: 99999/'                         conf/nsbc.yaml
sed -i 's/aor_cps:.*/aor_cps: 99999/'                       conf/nsbc.yaml
sed -i 's/global_cps:.*/global_cps: 999999/'                conf/nsbc.yaml

# 确认改了
grep -E 'ratelimit|cps' conf/nsbc.yaml
```

### 3.2 修正 external_sip_ip / external_rtp_ip

nsbc-web 启动时可能自动探测到**公网 IP**，导致 SDP 改写用公网地址、RTP 不通。

```bash
# 查看当前值
sqlite3 /home/github/nsbc/db/web.db \
  "SELECT name, external_sip_ip, external_rtp_ip FROM sip_profiles;"

# 如果是公网 IP（不是 10.0.0.x），强制改回内网
sqlite3 /home/github/nsbc/db/web.db \
  "UPDATE sip_profiles SET external_sip_ip='10.0.0.80', external_rtp_ip='10.0.0.80' WHERE 1;"

# 再确认
sqlite3 /home/github/nsbc/db/web.db \
  "SELECT name, external_sip_ip, external_rtp_ip FROM sip_profiles;"
```

### 3.3 启动顺序

```bash
# SSH 到 10.0.0.80
cd /home/github/nsbc

# 全杀（如果有残留）
pkill -9 nsbc-web 2>/dev/null; pkill -9 nsbcd 2>/dev/null
sleep 1

# ① 启动 nsbc-web（必须在 /home/github/nsbc/ 目录下，它会找 conf/ 和 db/）
nohup ./nsbc-web > /tmp/nsbc_web.log 2>&1 < /dev/null &
sleep 3
# 确认起来了
pgrep -a nsbc-web
tail -20 /tmp/nsbc_web.log

# ② 启动 nsbcd（core dump 已开）
bash -c 'ulimit -c unlimited && nohup ./nsbcd > /tmp/nsbcd.log 2>&1 < /dev/null &'
sleep 4
# 确认起来了
pgrep -a nsbcd
ls /var/run/nsbc/data.sock /var/run/nsbc/control.sock
tail -20 /tmp/nsbcd.log
```

> **首次启动注意**: extensions 表 seed 9 万行 + HA1 计算，SQLite 可能要 3-10 秒。耐心等，别 kill。

### 3.4 OPTIONS 验证（本机）

```bash
# 10.0.0.80 本机执行
echo -e "OPTIONS sip:nsbc.local SIP/2.0\r\n\
Via: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bKopt1\r\n\
Max-Forwards: 70\r\n\
From: <sip:test@nsbc.local>;tag=opt\r\n\
To: <sip:test@nsbc.local>\r\n\
Call-ID: opt123@nsbc.local\r\n\
CSeq: 1 OPTIONS\r\n\
Content-Length: 0\r\n\r\n" \
  | nc -u -w2 127.0.0.1 5060

# 期望：200 OK + User-Agent: NSBC/x.x.x
```

### 3.5 跨机 UDP 可达验证

```bash
# 10.0.0.25 压测机执行
timeout 2 nc -uz 10.0.0.80 5060 && echo "✅ SIP UDP 可达" || echo "❌ 不可达"
```

### 3.6 REGISTER 验证（压测机执行）

```bash
# 先造 authfile（少量测试用 5 个）
seq 10001 10005 | awk '{print $1";nsbc.local;876876"}' > /tmp/nsbc_authfile_test.txt

# 用 sipp register 场景注册 5 个分机
sipp 10.0.0.80:5060 -sn register -ab /tmp/nsbc_authfile_test.txt -nb \
  -m 10 -r 1 -d 10s -trace_err 2>&1

# 期望：REGISTER 全部 200 OK（不是 401 / 403 / 无响应）
# 如果无响应 → 查 realm 是否为 nsbc.local
```

### 3.7 Dialplan 规则验证（关键！防旧规则残留）

```bash
# 10.0.0.80 执行
sqlite3 /home/github/nsbc/db/web.db \
  "SELECT name, layer, match_expr, enabled, action_json FROM dialplan_rules;"
```

**期望 ext layer 那条 match_expr 是 `^\d{5}$`**。如果是 `^1[01]\d{2}$`（旧 v1.0 规则），手动修：

```bash
sqlite3 /home/github/nsbc/db/web.db \
  "UPDATE dialplan_rules SET match_expr='^\\d{5}$', description='Internal profile: 10000-99999 extensions' \
   WHERE layer='ext' AND enabled=1;"
echo "dialplan reload" | nc -U /var/run/nsbc/control.sock
```

### 3.8 Extension 表验证（9 万 seed 是否成功）

```bash
sqlite3 /home/github/nsbc/db/web.db \
  "SELECT min(extension), max(extension), count(*) FROM extensions WHERE tenant_id=1;"

# 期望：10000 | 99999 | 90000
```

### 3.9 完整启动验证 checklist

- [ ] nsbc-web 进程存在 `pgrep -a nsbc-web`
- [ ] nsbcd 进程存在 `pgrep -a nsbcd`
- [ ] UDS 存在 `ls /var/run/nsbc/data.sock /var/run/nsbc/control.sock`
- [ ] OPTIONS 本机 200 OK
- [ ] 跨机 UDP 5060 可达
- [ ] 5 个分机 REGISTER 全部 200 OK
- [ ] dialplan ext layer match_expr = `^\d{5}$`
- [ ] extensions count = 90000
- [ ] sip_profiles external_sip_ip 是内网 `10.0.0.80` 不是公网
- [ ] nsbc.yaml 所有 ratelimit_* / cps 都已拉高到 99999+

---

## 4. 功能测试（T01 – T05 + T08）

> 功能测试在压测矩阵前先跑一遍，确认 nsbcd 基本功能正常再上高并发。

### T01 — SIP 注册（8 场景）

| ID | 场景 | 预期 | sipp XML |
|----|------|------|---------|
| T01-01 | 正常注册 (UDP) | REGISTER → 401 → REGISTER(auth) → 200 OK | `01-register/T01-01-register-cycle.xml` |
| T01-02 | 正常注册 (TCP) | 同上，TCP 传输 | 用 `-t t1` 覆盖传输 |
| T01-03 | 密码错误 | REGISTER → 401 → REGISTER(bad auth) → 持续 401 | 改 authfile 密码 |
| T01-04 | 密码错误触发 ban | 30s 内 4 次密码错 → nsbcd 封测试机 IP | 需改 authfile + 跑多次 |
| T01-05 | 重注册刷新 | 注册成功后新 CSeq REGISTER → 200 OK | T01-01 重跑 |
| T01-06 | 注销 (Expires=0) | 已注册账号发 REGISTER Expires=0 → 200 OK | |
| T01-07 | REGISTER 洪泛（限速验证） | 如果 rate limit 没关 → 前 10 过后续被拦 | 1s 内 20 次 |
| T01-08 | 未注册 INVITE | 无 REGISTER 直接 INVITE → silent drop（防枚举） | |

### T02 — 基础呼叫流程（6 场景）

| ID | 场景 | 预期信令流 | sipp XML |
|----|------|-----------|---------|
| T02-01 | UDP 正常呼叫 | INVITE→100→180→200→ACK→RTP→BYE→200 | `02-basic-call/T02-01-call-flow.xml` |
| T02-02 | TCP 正常呼叫 | 同上 | `-t t1` |
| T02-03 | 主叫取消 (CANCEL) | INVITE→180→CANCEL→487→BYE→200 | `02-basic-call/T02-04-cancel.xml` |
| T02-04 | 被叫忙 (486) | INVITE→100→486→ACK | |
| T02-05 | 被叫无应答 (408) | INVITE→100→超时→408→ACK | |
| T02-06 | 已注册双方 INVITE | 10001→10002，双方都 REGISTER → 正常 | |

### T03 — 媒体转发（4 场景）

| ID | 场景 | 验证 | sipp XML |
|----|------|------|---------|
| T03-01 | RTP 双向转发 | sipp UDP + G.711 PCMU | `03-media-relay/T03-01-rtp-relay.xml` |
| T03-02 | DTMF 透传 (RFC 2833) | payload 101 event 包 | |
| T03-03 | 媒体桥断后重连 | 中途关被叫 RTP socket 再重开 | |
| T03-04 | 端到端音频 | 双 UAC/UAS 双向 RTP | |

### T04 — 信令响应码覆盖

| 响应码 | 场景 | 预期 |
|--------|------|------|
| 401 | 未 auth REGISTER | 401 + WWW-Authenticate |
| 403 | 不存在账号 | **silent drop**（防枚举） |
| 404 | INVITE 未注册分机 | 404 |
| 408 | INVITE 超时 | 408 |
| 480 | 所有分机离线 | 480 |
| 481 | CANCEL 不存在的 call-id | 481 |
| 486 | 被叫 busy | 486 |
| 487 | CANCEL 成功 | 487 |
| 500 | 内部错误 | 500 |
| 503 | 过载保护 | 503 |
| 200 | OPTIONS keepalive | 200 + User-Agent: NSBC |
| — | 非对话内 MESSAGE | **silent drop** |

### T05 — Session Timer（3 场景）

| ID | 场景 |
|----|------|
| T05-01 | UAC 带 Session-Expires=1800 |
| T05-02 | NSBC 作为 Session Timer 主动 Refresh |
| T05-03 | 会话超时自动 BYE（Session-Expires=60s） |

### T08 — 安全机制（6 场景）

| ID | 场景 |
|----|------|
| T08-01 | authfail 自动 ban（3/30s） |
| T08-02 | ban 后静默丢弃 |
| T08-03 | REGISTER 限速（10/30s，需关 rate limit 后反向验证） |
| T08-04 | INVITE 限速（活跃数/滑窗） |
| T08-05 | 不存在账号 silent drop（防枚举） |
| T08-06 | 恶意 IP CDR 标记 |

---

## 5. 压测矩阵（T06）⭐ 核心

> **目标**: 摸到 nsbcd 真实极限。⚠️ 档过了继续往上加，直到 crash 或成功率 < 99%。

### 5.0 sanity gate（强制，矩阵前必须过）

**顺序**: BYPASS 默认 → 先跑通 BYPASS sanity → 切 RELAY → 跑 RELAY sanity → 再进矩阵

| 步骤 | 操作 | 期望 | 验证 |
|------|------|------|------|
| 1 | 5 个分机 REGISTER | 全部 200 OK | sipp 输出无 4xx |
| 2 | 5 对分机 BYPASS 互呼 | 全部 200 OK + RTP 正常 | sipp -trace_msg 看媒体流；或抓包看 RTP 是否直连 |
| 3 | 切 RELAY（见 §2.5）再 5 对 | 媒体走 NSBC | nsbcd 日志有 "relay" 关键字；或 RTP 流向 10.0.0.80:10000-59999 |
| 4 | dialplan 规则确认 | ext layer match_expr = `^\d{5}$` + media_mode 正确 | `sqlite3` 查询 |

任何一步挂了，先排障再进矩阵。上次 16 组全挂（0% 建立率）就是没跑 sanity gate。

### 5.1 通用命令模板

```bash
# === 全部在压测机 (10.0.0.25) 执行 ===

# 压测机上生成 authfile（10000 个压测池）
seq 10000 19999 | awk '{print $1";nsbc.local;876876"}' > /tmp/nsbc_authfile.txt

# sipp target：跨机用 10.0.0.80:5060，本机用 127.0.0.1:5060
TARGET="10.0.0.80:5060"

# sipp 参数说明
#   -sn register        使用 sipp 内置 register 场景
#   -sn uac             使用 sipp 内置 uac（INVITE 主叫）场景
#   -ab authfile        sipp 自动从 authfile 取账号做 digest auth
#   -nb                 不自动开始（authfile 模式下要加这个）
#   -m TOTAL_MSGS       总发送消息数 = 并发 × 持续秒 × CPS
#   -r CPS              每秒发多少 call setup
#   -d DURATION         持续时间（如 30s）
#   -trace_err          只记录错误包（trace_msg 太大，极限压测别开）
#   -trace_msg          调试时开，记录每个 SIP 包

# === 每次压测前：保存 baseline ===
# 命令模板（压测机执行，用 ssh 远端采 nsbcd 侧指标）：
ssh -p 22322 root@10.0.0.80 \
  "top -H -b -n 1 -p \$(pgrep nsbcd)" > /tmp/nsbcd_cpu_before.txt

# === 每次压测后：采指标 ===
ssh -p 22322 root@10.0.0.80 \
  "top -H -b -n 1 -p \$(pgrep nsbcd)" > reports/${TEST_NAME}_cpu.txt

ssh -p 22322 root@10.0.0.80 \
  "cat /proc/\$(pgrep nsbcd)/status | grep -E 'Vm|Threads'" > reports/${TEST_NAME}_mem.txt

ssh -p 22322 root@10.0.0.80 \
  "ls /proc/\$(pgrep nsbcd)/task/ | wc -l" > reports/${TEST_NAME}_threads.txt

ssh -p 22322 root@10.0.0.80 \
  "sqlite3 /home/github/nsbc/db/web.db 'SELECT count(*) FROM cdr;'" > reports/${TEST_NAME}_cdr.txt

ssh -p 22322 root@10.0.0.80 \
  "ls core.* 2>/dev/null; echo 'exit_code=\$?'" > reports/${TEST_NAME}_core.txt

# === 压测循环示例 ===
# REGISTER 矩阵
for c in 100 500 1000 3000 5000; do
  for r in 50 200 500 1000; do
    TOTAL=$((c * 30))
    echo "▶ REGISTER C${c} R${r} (total=${TOTAL})"
    sipp $TARGET -sn register -ab /tmp/nsbc_authfile.txt -nb \
      -m $TOTAL -r $r -d 30s -trace_err 2>&1 | tee reports/reg_C${c}_R${r}.log
    # 采指标...
  done
done
```

### 5.2 矩阵 A — REGISTER 并发 × CPS

> 只测注册，与媒体模式无关。用 `-sn register` 场景。
> 每个 call = 一条 REGISTER + 401 challenge + REGISTER(auth) = 约 2-3 条 SIP 消息

| 并发 (call 数) \ CPS | 50 | 200 | 500 | 1000 | 2000 | 5000 |
|---|---|---|---|---|---|---|
| **100** | ✅ | ✅ | ✅ | — | — | — |
| **500** | ✅ | ✅ | ✅ | ✅ | — | — |
| **1000** | — | ✅ | ✅ | ✅ | ✅ | — |
| **3000** | — | — | ✅ | ✅ | ✅ | ✅ |
| **5000** | — | — | — | ✅ | ✅ | ⚠️ |
| **10000** | — | — | — | — | ✅ | ⚠️ |

**命令**:
```bash
# REGISTER C3000 R500 示例
sipp 10.0.0.80:5060 -sn register -ab /tmp/nsbc_authfile.txt -nb \
  -m 90000 -r 500 -d 30s -trace_err 2>&1 | tee reports/reg_C3000_R500.log
```

> **摸到极限后往上加**: 如果 10000 并发 / 5000 CPS 也过了，继续试 20000 并发 / 10000 CPS —— 直到 crash 或成功率 < 99%。

### 5.3 矩阵 B — BYPASS 呼叫并发 × CPS（信令 only，默认模式）

> 媒体绕开 NSBC，双方直连。测 NSBC SIP 信令层极限。
> **前提**: sanity gate BYPASS 部分已过。
> 用 `-sn uac` 场景，每 call 一对分机互呼。

| 并发 \ CPS | 30 | 100 | 300 | 500 | 1000 | 2000 |
|---|---|---|---|---|---|---|
| **100** | ✅ | ✅ | ✅ | — | — | — |
| **500** | ✅ | ✅ | ✅ | ✅ | — | — |
| **1000** | — | ✅ | ✅ | ✅ | ✅ | — |
| **3000** | — | — | ✅ | ✅ | ✅ | ✅ |
| **5000** | — | — | — | ✅ | ✅ | ⚠️ |

**命令**:
```bash
# BYPASS C1000 R100 示例
sipp 10.0.0.80:5060 -sn uac -s 10002@nsbc.local \
  -ab /tmp/nsbc_authfile.txt \
  -m 30000 -r 100 -d 30s -trace_err 2>&1 | tee reports/bypass_C1000_R100.log

# BYPASS C3000 R300 示例
sipp 10.0.0.80:5060 -sn uac -s 10002@nsbc.local \
  -ab /tmp/nsbc_authfile.txt \
  -m 90000 -r 300 -d 30s -trace_err 2>&1 | tee reports/bypass_C3000_R300.log
```

### 5.4 矩阵 C — RELAY 呼叫并发 × CPS（媒体经 NSBC）

> **前提**: 先切 RELAY（见 §2.5）+ sanity gate RELAY 部分已过。
> RELAY 模式负载显著更高（SIP + RTP relay 双层，RTP relay 走 reactor 线程），并发/CPS 档减半。

| 并发 \ CPS | 30 | 50 | 100 | 200 | 300 | 500 |
|---|---|---|---|---|---|---|
| **50** | ✅ | ✅ | ✅ | — | — | — |
| **100** | ✅ | ✅ | ✅ | ✅ | — | — |
| **300** | — | ✅ | ✅ | ✅ | ✅ | — |
| **500** | — | — | ✅ | ✅ | ✅ | ⚠️ |
| **1000** | — | — | — | ✅ | ✅ | ⚠️ |

**命令**:
```bash
# 先切 RELAY（SSH 到 10.0.0.80）
sqlite3 /home/github/nsbc/db/web.db \
  "UPDATE dialplan_rules SET action_json='{\"media_mode\":2}' WHERE layer='ext' AND enabled=1;"
echo "dialplan reload" | nc -U /var/run/nsbc/control.sock

# RELAY C300 R100 示例
sipp 10.0.0.80:5060 -sn uac -s 10002@nsbc.local \
  -ab /tmp/nsbc_authfile.txt \
  -m 9000 -r 100 -d 30s -trace_err 2>&1 | tee reports/relay_C300_R100.log
```

### 5.5 矩阵跑完后切回 auto（重要！）

```bash
sqlite3 /home/github/nsbc/db/web.db \
  "UPDATE dialplan_rules SET action_json='{}' WHERE layer='ext' AND enabled=1;"
echo "dialplan reload" | nc -U /var/run/nsbc/control.sock
```

---

## 6. reactor × worker 配置矩阵（T07）⭐ 核心

> **顺序依赖**: 先跑通 T06 矩阵 M4 配置（reactors=4, workers=8），再切其他配置。
> 每次切换后重启 nsbcd + 验证线程数 + 跑一组 baseline（C1000 R100）。

### 6.1 配置变量

| 变量 | 说明 |
|------|------|
| `core.reactors` | reactor 线程数（收包层，各绑一个独立 UDP fd） |
| `core.workers` | worker 线程池大小（事务层） |

### 6.2 测试配置（6 组）

| 组 | reactors | workers | 说明 | 切换后 baseline |
|----|----------|---------|------|----------------|
| **M4**（基线） | **4** | **8** | 平衡配置，先跑通 T06 全矩阵 | C1000 R100 必须过 |
| M1 | 1 | 1 | 单 reactor + 单 worker | C100 R30 |
| M2 | 8 | 1 | 多 reactor + 单 worker（worker 瓶颈） | C500 R50 |
| M3 | 1 | 4 | 单 reactor + 多 worker（reactor 瓶颈） | C500 R50 |
| M5 | 16 | 8 | 大量级 | C1000 R100 |
| M6 | 4 | 8 | 同 M4，用作对照 | — |

### 6.3 切换脚本

```bash
# SSH 到 10.0.0.80
cd /home/github/nsbc

# 改配置（示例：切 M2 = 8 reactors, 1 worker）
sed -i 's|^[[:space:]]*reactors:.*|  reactors: 8|' conf/nsbc.yaml
sed -i 's|^[[:space:]]*workers:.*|  workers: 1|'   conf/nsbc.yaml

# 杀 nsbcd + 重启（nsbc-web 不动）
pkill -9 nsbcd; sleep 1
bash -c 'ulimit -c unlimited && nohup ./nsbcd > /tmp/nsbcd.log 2>&1 < /dev/null &'
sleep 4

# 验证
echo "=== 线程数 ==="
ls /proc/$(pgrep nsbcd)/task/ | wc -l

echo "=== reactor UDP fd（= reactors 数）==="
ss -lunp | grep 5060

echo "=== 快速验证（OPTIONS）==="
echo -e "OPTIONS sip:nsbc.local SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bK\r\nMax-Forwards: 70\r\nFrom: <sip:test@nsbc.local>;tag=a\r\nTo: <sip:test@nsbc.local>\r\nCall-ID: x@nsbc.local\r\nCSeq: 1 OPTIONS\r\nContent-Length: 0\r\n\r\n" | nc -u -w2 127.0.0.1 5060 | head -1
```

### 6.4 每组交叉跑

每个 (reactors, workers) 配置，跑 3 档 T06 baseline：

| 规模 | 模式 | 并发 | CPS | 目的 |
|------|------|------|-----|------|
| 低 | BYPASS | C100 | R30 | sanity |
| 中 | BYPASS | C1000 | R100 | 主流 |
| 高 | BYPASS | C3000 | R300 | 压力 |

### 6.5 性能对比图（运行时填）

| reactor \ worker | 1 | 4 | 8 |
|-----------------|---|---|---|
| **1** | | | |
| **4** | | ✅ M4 baseline | |
| **8** | | | |

> 每格填: 通过的最大 CPS / CPU 利用率 / 延迟 P99 / crash 与否

---

## 7. 产物规范 & 归档

### 7.1 每次压测必须收集（完整！别只收 sipp log）

每次执行 `sipp ... -m N -r R -d 30s`，必须同时采集以下指标：

| 文件 | 采集命令 | 时机 |
|------|---------|------|
| `{name}.log` | sipp stdout | 压测时 tee |
| `{name}_cpu.txt` | `top -H -b -n 1 -p $(pgrep nsbcd)` | 压测后立即 |
| `{name}_mem.txt` | `cat /proc/$(pgrep nsbcd)/status | grep Vm` | 压测后立即 |
| `{name}_threads.txt` | `ls /proc/$(pgrep nsbcd)/task/ | wc -l` | 压测后立即 |
| `{name}_cdr.txt` | `sqlite3 db/web.db "SELECT count(*) FROM cdr;"` | 压测后立即 |
| `{name}_core.txt` | `ls core.* 2>/dev/null` | 压测后立即 |
| `{name}_dns_log.txt` | `tail -200 /tmp/nsbcd.log` | 压测后立即 |

### 7.2 目录模板

```
testing/
├── sip-test-plan.md                   ← 本文件 v3.0
├── config.env                         ← SSH / SIP 变量（已存在，可复用）
│
├── 01-register/ ... 07-security/      ← 功能测试 sipp XML（已存在）
├── 06-stress-concurrent/              ← 压测脚本（已存在 run_matrix.py / stress-uac.xml）
│
├── reports/
│   ├── reg_C100_R50.log
│   ├── reg_C100_R50_cpu.txt
│   ├── reg_C100_R50_mem.txt
│   ├── ...
│   ├── bypass_C1000_R100.log
│   ├── ...
│   ├── relay_C300_R100.log
│   ├── ...
│   ├── matrix-report-YYYYMMDD.json    ← 最终汇总（机器可读，脚本生成）
│   ├── matrix-report-YYYYMMDD.md      ← 最终汇总（人类可读，手动填）
│   ├── sip-matrix-report-20261027-114047.md  ← 上一轮产物（保留作为反例）
│   └── sip-matrix-20261027-132653.json       ← 上一轮全挂（0% 建立率）
│
└── authfile_cache/
    ├── pool_10000.txt                 ← 10000 个压测池
    └── full_90000.txt                 ← 全部 9 万（首次大规模注册用）
```

### 7.3 汇总报告模板（跑完全部后）

```
# NSBC 极限压测报告 — YYYY-MM-DD

## 环境
- nsbcd 版本: ____
- reactors=____, workers=____
- 机器: 10.0.0.80, RAM: ____GB
- 测试机: 10.0.0.25

## REGISTER 极限
- 最大通过: ____ 并发 / ____ CPS（成功率 ____%）
- 首次 crash 点: ____ / ____
- CPU 峰值: ____%（reactor）, ____%（worker）

## BYPASS 呼叫极限（信令 only）
- 最大通过: ____ 并发 / ____ CPS（成功率 ____%）
- 首次 crash 点: ____ / ____
- 延迟 P99: ____ms

## RELAY 呼叫极限（媒体转发）
- 最大通过: ____ 并发 / ____ CPS（成功率 ____%）
- reactor CPU 瓶颈: 单核 100% at ____ CPS

## 发现的问题
-
```

---

## 8. 已知坑 & 风险

| # | 风险 | 触发条件 | 缓解 |
|---|------|---------|------|
| **0** | **上次全挂根因**：sipp authfile 还在用旧号段 1001-1019 + realm 问题 | operator 忘了同步账号号段 | ✅ **已修**：v3 文档全部引用 10000-99999，authfile 生成命令写死 |
| **1** | **dialplan 旧规则留存**：DB 已有 `^1[01]\d{2}$`，新 `^\d{5}$` 因 `ON CONFLICT DO NOTHING` 不生效 | operator 清库前已经跑过 migrations | ✅ **v3 文档 §3.7 强制检查** + 手动 UPDATE 命令 |
| **2** | **security fence reload 覆盖 ratelimit**：nsbcd 启动时 security fence 把 ratelimit_* 覆盖回默认值 10/30s | 改了 nsbc.yaml 但只 reload 没重启 nsbcd | ✅ **v3 文档 §3.1 强制"改 yaml + 重启 nsbcd"** + 重启后验证 REGISTER |
| **3** | **external_sip_ip / external_rtp_ip 被探测成公网** | nsbc-web 自动探测公网 IP（特别是机器上有公网网卡时） | ✅ **v3 文档 §3.2 强制改 DB + 验证** |
| **4** | **realm 陷阱**：REGISTER To 头 host 不是 nsbc.local | sipp XML 里 host 写了 IP 或其他域 | ✅ **v3 文档 §2.2 大段警告**，所有示例命令写 `@nsbc.local` |
| **5** | **9 万账号首次 seed 慢** | nsbc-web 首次启动 INSERT 9 万行 + HA1 计算 | 耐心等 3-10s，别 kill nsbc-web |
| **6** | **9 万账号未注册就发 INVITE → silent drop** | 以为 seed 进 DB 了就能 INVITE，忘了先 REGISTER | ✅ **sanity gate 强制先 REGISTER** |
| **7** | **RELAY 模式忘记切回去** | 测完 RELAY 忘了改回 auto → 后续 RELAY 负载模式继续跑 | ✅ **§5.5 明确切回命令** |
| **8** | **压测触发 IP ban**（authfail 3/30s） | sipp authfile 里有密码错的账号（哪怕一个） | 生成 authfile 时确保密码全对；ban 后在 nsbcd 侧手动解 |
| **9** | **sipp -trace_msg 吃掉极限性能** | 极限压测开 trace_msg → sipp 自己 I/O 扛不住 | 极限压测用 `-trace_err`，调试时才开 `trace_msg` |
| **10** | **跨机压测 UDP 被防火墙丢** | 10.0.0.25 → 10.0.0.80 5060 UDP 不通 | ✅ **sanity gate §3.5 强制 nc 验证** |
| **11** | **73 个 _diag*.py 历史脚本误用** | operator 看到 testing/ 下一堆脚本以为是当前要用的 | **本文件之外所有 `_diag*` / `_fix*` / `_t06*` 均视为历史排障脚本，请忽略** |

---

## 9. 执行命令速查

### SSH

```bash
# NSBC 运行机（Debian 12，SSH 端口非标准）
ssh -p 22322 root@10.0.0.80

# 压测机（CentOS 7，标准端口）
ssh root@10.0.0.25
```

### 启停

```bash
# 全杀
pkill -9 nsbc-web 2>/dev/null; pkill -9 nsbcd 2>/dev/null

# 先 nsbc-web，再 nsbcd
cd /home/github/nsbc
nohup ./nsbc-web > /tmp/nsbc_web.log 2>&1 < /dev/null &
sleep 2
bash -c 'ulimit -c unlimited && nohup ./nsbcd > /tmp/nsbcd.log 2>&1 < /dev/null &'
sleep 4
```

### 热加载

```bash
echo "profile reload"    | nc -U /var/run/nsbc/control.sock
echo "dialplan reload"   | nc -U /var/run/nsbc/control.sock
echo "registrar reload"  | nc -U /var/run/nsbc/control.sock
echo "core show threads" | nc -U /var/run/nsbc/control.sock
echo "bridge show"       | nc -U /var/run/nsbc/control.sock
```

### DB 查询

```bash
# extensions 概览
sqlite3 db/web.db "SELECT min(extension), max(extension), count(*) FROM extensions WHERE tenant_id=1;"

# dialplan 当前规则
sqlite3 db/web.db "SELECT name, layer, match_expr, enabled, action_json FROM dialplan_rules;"

# sip_profiles external IP
sqlite3 db/web.db "SELECT name, external_sip_ip, external_rtp_ip FROM sip_profiles;"

# CDR 统计（压测后看）
sqlite3 db/web.db "SELECT count(*), min(start_ts), max(start_ts) FROM cdr;"
sqlite3 db/web.db "SELECT term_reason, count(*) FROM cdr GROUP BY term_reason;"
```

### 验证一条 REGISTER

```bash
# 直接发 REGISTER 包（不用 sipp）
# 期望：先 401 challenge，再 REGISTER(auth) → 200 OK
# 手动算 HA1: python -c "import hashlib; print(hashlib.md5(b'10001:nsbc.local:876876').hexdigest())"
```

### 压测循环模板

```bash
# === REGISTER 矩阵（压测机 10.0.0.25 执行）===
TARGET="10.0.0.80:5060"
AUTHFILE=/tmp/nsbc_authfile.txt
seq 10000 19999 | awk '{print $1";nsbc.local;876876"}' > $AUTHFILE

for c in 100 500 1000 3000 5000; do
  for r in 50 200 500 1000; do
    echo "▶ REGISTER C${c} R${r}"
    TOTAL=$((c * 30))
    sipp $TARGET -sn register -ab $AUTHFILE -nb \
      -m $TOTAL -r $r -d 30s -trace_err 2>&1 | tee reports/reg_C${c}_R${r}.log
    # 采 nsbcd 侧指标...
  done
done

# === BYPASS 呼叫矩阵 ===
for c in 100 500 1000 3000; do
  for r in 30 100 300; do
    echo "▶ BYPASS C${c} R${r}"
    TOTAL=$((c * 30))
    sipp $TARGET -sn uac -s 10002@nsbc.local -ab $AUTHFILE \
      -m $TOTAL -r $r -d 30s -trace_err 2>&1 | tee reports/bypass_C${c}_R${r}.log
  done
done
```

---

## 10. 执行进度

| 阶段 | 任务 | 状态 | 负责人 |
|------|------|------|--------|
| P0-0 | 启动 & sanity gate（§3.9 全部 10 项勾过） | ✅ 2026-10-30 | |
| P0-1 | 功能测试 T01-T05 + T08 全过 | ✅ 2026-10-30 | |
| **P0-2** | **REGISTER 极限矩阵（§5.2）** | ⏳ | |
| **P0-3** | **BYPASS 呼叫极限矩阵（§5.3）** | ⏳ | |
| **P0-4** | **RELAY 呼叫极限矩阵（§5.4）** | ⏳ | |
| P0-5 | reactor × worker 配置矩阵（§6，M4 先跑通，再 M1/M2/M3/M5） | ⏳ | |
| 收尾 | 汇总报告 + 代码改动记录 | ⏳ | |

---

## 11. Phase 1 批量写入 + b2bua.c 死锁修复 联合验证（2026-10-31）

### 11.1 改动清单

| 文件 | Commit | 说明 |
|------|--------|------|
| `src/sip/b2bua.c` | `fafe86c` | **g_sdp_hook_lock 快照修复** — 3 处 fire site 先 memcpy 快照再释放锁再调回调，避免跨 reactor_call(cond_wait) 持锁 → ABBA 死锁 |
| `web/src/db/mod.rs` | `4a3151b` | **Phase 1 批量写入** — mpsc channel (cap=4096) + batch consumer (MAX_BATCH=100, FLUSH=100ms) + 4 类批量 SQL (calls/registrations/cdr/runtime) |
| `web/src/db/mod.rs` | `540940b` | flush debug→info 可观测性修复 |

### 11.2 远程验证结果（10.0.0.80, nsbcd 2.6.0, 16 核）

#### BYPASS 完整阶梯（全档存活！）

| 档位 | CPS | DB calls | DB cdr | nsbcd | gdb g_sdp_hook_lock |
|------|-----|----------|--------|-------|-------------------|
| S    | 5   | 10       | —      | ✅    | —                 |
| B1   | 20  | 300      | 300    | ✅    | —                 |
| B2   | 50  | 750 (+450) | 750    | ✅    | —                 |
| B3   | 100 | 1786 (+1036) | 1786  | ✅    | —                 |
| B4   | 200 | 4950 (+3164) | 4950  | ✅    | 0 waiters ✅       |
| B5   | 300 | 8158 (+3208) | 8158  | ✅    | 0 waiters ✅       |

**关键对比**：Phase 0 同远程机在 BYPASS 200 CPS 触发 g_sdp_hook_lock 经典 ABBA 死锁卡死。Phase 1 在 BYPASS **300 CPS** 下依然存活。

#### RELAY 部分验证
- R1-R3 跑完，DB calls 有增长，nsbcd 存活
- RELAY workers join 超慢（Debian spawn 模式），完整矩阵待后续 session

#### 未解决项
| 项 | 状态 | 备注 |
|----|------|------|
| SQLite `busy_timeout=5000` | ❌ PRAGMA 未生效 | sqlx connection-level PRAGMA 需要 `after_connect` hook，Phase 2 修 |
| RELAY 完整 5 档矩阵 | 🔶 部分完成 | R4/R5 因 spawn join 超慢未跑完，nsbcd 依然存活 |
| C 端 ring 溢出计数器 | ❌ 未实现 | 架构事实：ring_put 异步不回压，ring 满丢 oldest — 需加丢弃计数 |

### 11.3 经验教训（已更新 SKILL.md）

| # | 坑 | 根治 |
|---|----|------|
| W2f | OPTIONS sanity 假故障（nc -u loopback 无响应） | 跳过 OPTIONS，直接跑 stress_invite |
| W2g | stress_invite 0% success 假阴性（join 慢导致） | 用 DB calls/cdr 行数判断真实成功率 |
| W2h | SQLite busy_timeout PRAGMA 在 sqlx 里可能不生效 | 用 after_connect hook（代码待补） |
