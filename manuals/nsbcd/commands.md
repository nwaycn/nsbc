# nsbcd UDS 控制命令手册

nsbcd 通过 Unix Domain Socket (`control.sock`) 接受来自 nsbcd-web 和 CLI 的行协议命令。
连接后先发 banner，然后逐行发送命令，每命令返回 `OK ...` 或 `ERR ...`。

> 连接示例：`socat - UNIX-CONNECT:/home/github/nsbc/control.sock`
> 或 Rust Web 端：`web/src/coreclient.rs::send_cmd`

## 命令架构

命令分两类：

| 类型 | 定义位置 | 注册方式 | 典型 |
|---|---|---|---|
| **内建命令** | `src/oam/obs_control.c::dispatch()` | 硬编码 if 链（多 token） | `show calls`, `config.get`, `hangup` |
| **注册表命令** | `src/oam/cmd_registry.c` + 各模块 register 函数 | `cmd_register` 注册 | `profiles.reload`, `uuid_kill`, `sofia`, `console` |

注册表命令统一走参数切分（空格分隔），argc/argv 自动就绪；内建命令在 dispatch 里手动切 token。

## 权限分级

| 级别 | 含义 | 需 L2 授权 |
|---|---|---|
| L1 MONITOR | 只读查询 | 否 |
| L2 ADMIN | 写操作 / reload / kill | 是（命令执行前会检查） |

## 日志与诊断命令（新增）

### `console` — 控制台诊断

| 用法 | 级别 | 说明 |
|---|---|---|
| `console` | L2 | 显示当前 loglevel 和 siptrace 状态 |
| `console loglevel` | L2 | 显示当前日志级别 |
| `console loglevel <level>` | L2 | 切换日志级别 |
| `console siptrace on\|off` | L2 | SIP 包收发 trace 开关 |

**level 取值**（类似 FreeSWITCH console loglevel）：

| 数字 | 名称 | 说明 |
|---|---|---|
| 0 | EMERG | 系统不可用（仅崩溃前） |
| 1 | ALERT | 需立即处理 |
| 2 | CRIT | 严重错误 |
| 3 | ERR | 错误 |
| 4 | WARN | 警告 |
| 5 | NOTICE | 正常但值得注意 |
| 6 | INFO | 正常操作信息（默认） |
| 7 | DEBUG | 调试信息（排障时开） |

也支持字符串形式：`emerg` / `alert` / `crit` / `err` / `warn` / `notice` / `info` / `debug`

**示例**：
```
console loglevel 7          ← 开 DEBUG（siptrace 需要）
console loglevel info       ← 切回 INFO
console loglevel            ← 回显当前
console siptrace on         ← 开 SIP 包 trace
console                     ← 看当前状态
```

### `sofia siptrace` — SIP 包 trace（类似 FreeSWITCH sofia global siptrace）

| 用法 | 级别 | 说明 |
|---|---|---|
| `sofia siptrace on\|off` | L2 | 简写 |
| `sofia global siptrace on\|off` | L2 | 全写（等价） |

**效果**：打开后，`sip_tr_send`（TX 出站）和 `on_udp_event`（RX 入站）会把完整 SIP 包打日志 + 推送给所有订阅者。长包（>4KB）自动截断。

**开 siptrace on 时自动做三件事**（一条命令全搞定）：
1. `g_sip_trace = 1` — 打开 trace 开关
2. 自动把**当前 UDS 连接**加到 `OBS_TOPIC_LOG` 订阅列表 — CLI 实时收推送
3. 如果 loglevel < DEBUG 自动升到 DEBUG — 日志文件也能看到

```
sofia global siptrace on       ← 开 trace（自动订阅 + 自动升 loglevel）
sofia siptrace off             ← 关 trace
console siptrace on            ← 简写等价
```

> siptrace 关闭后是 **零开销**（volatile 读 + 立即 return）。关闭 trace 不会自动退订 log topic（避免误伤其他订阅者），如需退订手动 `unsub log`。

**双通道输出**（不用额外配置，开 siptrace 自动生效）：

```bash
# === 方式 1：UDS 实时推送（CLI 自动收）===
nsbc_cli
nsbc> sofia siptrace on
→ (auto) loglevel 6(?) → 7(DEBUG)        ← 自动升 loglevel
→ OK sofia siptrace on                     ← 命令返回
→ siptrace <-RX UDP 10.0.0.163:5060 (389B) ← 自动推送到 CLI 屏幕
→ REGISTER sip:nsbc.local SIP/2.0
→ Via: SIP/2.0/UDP 10.0.0.163:5060;branch=z9hG4bK...
→ From: <sip:1001@nsbc.local>;tag=xxx
→ ...
→ siptrace TX-> UDP 10.0.0.163:5060 (152B)
→ SIP/2.0 200 OK
→ Via: SIP/2.0/UDP 10.0.0.163:5060;branch=z9hG4bK...
→ ...

# === 方式 2：日志文件 ===
tail -f logs/nsbcd.log | grep siptrace
```

---

## Show 命令（内建，L1 只读）

| 命令 | 说明 |
|---|---|
| `help` 或 `?` | 显示所有已注册命令列表 |
| `version` | 版本信息 |
| `show calls` | 当前所有通话 |
| `show channels` | 当前所有通道（calls 等价别名） |
| `show apps` | 注册的应用（IVR / park 等） |
| `show registrations` / `show regs` | 在线注册分机 |
| `show event_bus` | 事件总线状态 |
| `show codecs` | 支持的编解码器 |
| `show cdr_wal` | CDR 预写日志状态 |
| `show cdr` | CDR 最后记录 |
| `show endpoints` | 已注册分机详情 |
| `show modules` | 已加载模块 |
| `show ai_backends` | AI 后端状态 |

## Config 命令（内建）

| 命令 | 级别 | 说明 |
|---|---|---|
| `config.get <key>` | L1 | 读配置值 |
| `config.set <key> <value>` | L2 | 写配置值 |
| `config.validate` | L1 | 校验当前配置 |
| `config.export` | L1 | 导出全部配置（JSON） |
| `config.snapshots` | L1 | 列配置快照历史 |
| `config.rollback <id>` | L2 | 回滚到指定快照 |
| `config.reload` / `config.save` | L2 | 从磁盘重新加载 / 保存当前到磁盘 |

## Reload 系列（注册表，L2）

> 触发源：Web DB（权威）→ Rust `sync.rs` → `*.reload <json>` → C 端 RCU 原子替换

| 命令 | 说明 |
|---|---|
| `extensions.reload <json>` | 分机表全量替换 |
| `gateways.reload <json>` | 网关表 |
| `trunks.reload <json>` | Trunk 表 |
| `dialplan-rules.reload <json>` | 拨号规则表 |
| `profiles.reload <json>` | SIP Profile 表（成功后自动调 `sip_tr_sync_from_profiles` 同步监听端口） |
| `profiles.list` | **L1 只读** — 列当前加载的 Profile |
| `queues.reload <json>` | ACD 队列表 |
| `security.blacklist.reload <json>` | 安全黑名单 + fence 配置 |
| `ivr.reload <json>` | IVR 菜单表 |
| `ha.reload <json>` | HA 组（Web DB 权威，noop） |
| `route_groups.reload <json>` | 路由组（Web DB 权威，noop） |

## UUID 呼叫级命令（注册表，L2）

| 命令 | 说明 |
|---|---|
| `uuid_dump <call-id>` | **L1** — 通话 dump（已废弃，内建命令） |
| `uuid_getvar <call-id> <var>` | **L1** — 读通道变量 |
| `uuid_kill <call-id> [cause]` | 强制挂断 |
| `uuid_cancel <call-id> [leg=-1\|0\|1]` | CANCEL 早期 INVITE |
| `uuid_hold <call-id> on\|off` | hold / resume |
| `uuid_mute <call-id> on\|off [leg]` | mute / resume |
| `uuid_vad_start <call-id> [threshold]` | 启动 VAD 监控 |
| `uuid_vad_stop <call-id>` | 停止 VAD 监控 |
| `uuid_transfer <call-id> <dest>` | 盲转 REFER |
| `uuid_displace <call-id>` | B-leg answer + A-leg unhold 接管 |
| `uuid_park <call-id>` | hold 双腿暂停媒体 |
| `uuid_unpark <call-id>` | unhold 双腿恢复 |
| `uuid_deflect <call-id> <dest>` | 盲转（FreeSWITCH 别名） |
| `uuid_wait_for_answer <call-id> [timeout_ms]` | 阻塞等待 B-leg 200 OK |
| `uuid_bridge <call-id1> <call-id2>` | 桥接两呼叫 |
| `uuid_broadcast <call-id> <audio>` | 向通话注入音频 |
| `uuid_break <call-id>` | 打断当前媒体流 |
| `uuid_record <call-id> start\|stop [path]` | 录制控制 |
| `uuid_answer <call-id>` | 200 OK 应答 |
| `uuid_send_dtmf <call-id> <digits>` | 发送 DTMF 序列 |
| `uuid_setvar <call-id> <var> <value>` | 设置通道变量 |

## 全局命令（注册表）

| 命令 | 级别 | 说明 |
|---|---|---|
| `status` | L1 | 系统综合状态（活跃通话、注册、Profile 等） |
| `originate <call-url> [&app]` | L2 | 发起外呼 |
| `sofia profile <name> rescan` | L2 | 沿用 FreeSWITCH 别名 → 返回 not-supported（请用 `profiles.reload`） |
| `sofia siptrace on\|off` | L2 | SIP 包 trace（见上文） |
| `console loglevel <level>` | L2 | 日志级别切换（见上文） |
| `console siptrace on\|off` | L2 | SIP trace 简写（见上文） |

## 媒体与 AI

| 命令 | 级别 | 说明 |
|---|---|---|
| `uuid_media_bug <call-id> start\|stop\|list [type] [filter]` | L2 | 附加媒体 bug（录音抓包） |
| `media_bug purge_all` | L2 | 强制清理所有 media_bug |
| `hep status\|flush\|reload` | L2 | HEP3 推流控制 |
| `send_msg <call-id> <body>` | L2 | 对话内发送 MESSAGE |
| `hangup <call-id> [cause]` | L2 | 挂断（内建命令，等价 `uuid_kill`） |
| `test_push_cdr` | L2 | 往 data.sock 推模拟 CDR 验证 write-through |

## 典型调试流程

```
# 1. 连接控制 socket
socat - UNIX-CONNECT:/home/github/nsbc/control.sock

# 2. 看当前状态
console
→ console loglevel=6(INFO) siptrace=off

# 3. 开 SIP trace 抓包（需要 DEBUG）
console loglevel 7
sofia siptrace on

# 4. 等几秒让通话发生，然后回查日志
tail -f logs/nsbcd.log | grep siptrace

# 5. 关闭 trace，恢复正常级别
sofia siptrace off
console loglevel info

# 6. 看通话状态
status
show calls
show registrations
```

---

```
Author: 李浩 (Li Hao)
Company: 上海宁卫信息技术有限公司
Version: 2026-10-24 (nsbcd loglevel + siptrace added)
```
