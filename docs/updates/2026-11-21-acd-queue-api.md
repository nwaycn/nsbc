# 【技术文档 2026-11-21】ACD 队列 · 分机状态 · 拨号规则 · AI 转接队列

对应版本：**v0.6.2-beta**（commit `b3adca9`）
前置阅读：[cookbook.md §5 拨号规则](../cookbook.md)、[architecture.md](../architecture.md)

---

## 0. 能力总览

v0.6.2-beta 把呼叫中心三条关键链打通了：

| 链 | 核心入口 | 关键文件 |
|---|---|---|
| **入呼 → 队列** | dialplan `target: "queue:xxx"` → resolve_cb → `cq_pick_member` / `cq_enqueue` | [signaling.c#L1060-L1169](../../src/core/signaling.c#L1060-L1169) |
| **全忙排队** | `b2b_queue_hold` 建 hold relay + 183+MoH → `cq_tick` 1s 派发 → `b2b_queue_dispatch` 复用 relay | [b2bua.c#L9240-L9325](../../src/sip/b2bua.c#L9240-L9325)、[queue.c](../../src/mod/callcenter/queue.c) |
| **AI 转队列** | AI 对话请求 `dest="queue:xxx"` → `cq_pick_member` 选一个可振铃座席 → REFER | [ai_dialog.c#L1255-L1296](../../src/mod/ai_dialog/ai_dialog.c#L1255-L1296) |

分机状态由两套表维护：
- **call_table**（64 分片读写锁）：通话中引用计数 `slot0=被叫 / slot1=主叫`
- **ext_table**（64 分片读写锁）：跨 reload 保留的 `busy / paused / pause_reason / since_ms`

锁序：`call_lock → cq_lock → ext_lock`，**不同时反向持有**。

---

## 1. 分机队列 API

### 1.1 REST 端点

入口文件：[queues.rs#L21-L34](../../web/src/routes/queues.rs#L21-L34)，基路径 `/api/queues`。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET    | `/queues?tenant_id=&enabled=&keyword=&limit=&offset=` | 列表（分页） |
| POST   | `/queues`                              | 创建 |
| GET    | `/queues/:id`                          | 单条 |
| PUT    | `/queues/:id`                          | 更新 |
| DELETE | `/queues/:id`                          | 删除 |
| POST   | `/queues/reload`                       | 从 DB 推送到 nsbcd 核心 |
| GET    | `/queues/agents?tenant=&ext=`          | 座席状态列表（透传 OAM 命令 `cc.agent_list`） |
| GET    | `/queues/agents/state?tenant=&ext=`    | 单个分机状态 |
| POST   | `/queues/agents/pause`                 | 置忙（break/training/other） |
| POST   | `/queues/agents/resume`                | 恢复 |

### 1.2 请求/响应示例

**创建队列** `POST /queues`

```jsonc
{
  "tenant_id": 1,
  "name": "support",
  "enabled": true,
  "ring_strategy": "least_recent",          // ringall / least_recent / round_robin / fewest_calls
  "max_wait_sec": 300,                      // 全忙排队上限；`<=0` 关闭排队（保持 486 语义）
  "wrap_time_s": 30,                        // 话后整理（WRAP）
  "require_skill": "",                      // 空=不筛技能
  "overflow": "voicemail:support",          // max_wait 溢出目标（队列名 / ivr / voicemail / sip URI）
  "moh_sound": "default",                   // "default" / "" → ./sounds/default/moh.wav；也可写绝对/相对路径
  "members_json": "[\"1007\",\"1008\",\"1009\"]",
  "description": "技术支持队列"
}
```

**响应**（201 Created）：
```json
{ "id": 7, "name": "support", "enabled": true, "ring_strategy": "least_recent",
  "max_wait_sec": 300, "wrap_time_s": 30, "require_skill": "",
  "overflow": "voicemail:support", "moh_sound": "default",
  "members_json": "[\"1007\",\"1008\",\"1009\"]", "description": "技术支持队列",
  "created_at": "2026-11-21T10:00:00Z", "updated_at": "2026-11-21T10:00:00Z" }
```

**热加载** `POST /queues/reload`

从 DB 读所有 `enabled=1` 的队列，转成 `QueueSyncDto` 数组经 UDS 推送给核心。关键设计：[queue.c](../../src/mod/callcenter/queue.c) 不再 `m->n=0`，而是 `seen[]` 位图原地更新同名队列、保留有 waiter 的消失队列至 max_wait 溢出——**排队中的 183 呼叫跨 reload 不丢**。

```json
{ "loaded": 3 }
```

### 1.3 DB 表结构（节选）

[mod.rs#L3723-L3771](../../web/src/db/mod.rs#L3723-L3771)

```sql
CREATE TABLE IF NOT EXISTS queues (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    tenant_id     INTEGER NOT NULL,
    name          VARCHAR(64)  NOT NULL,
    enabled       INTEGER NOT NULL DEFAULT 1,
    ring_strategy VARCHAR(32)  NOT NULL DEFAULT 'ringall',
    max_wait_sec  INTEGER NOT NULL DEFAULT 600,
    wrap_time_s   INTEGER NOT NULL DEFAULT 30,
    require_skill VARCHAR(64)  NOT NULL DEFAULT '',
    overflow      VARCHAR(128) NOT NULL DEFAULT '',
    moh_sound     VARCHAR(128) NOT NULL DEFAULT 'default',
    members_json  TEXT    NOT NULL DEFAULT '[]',
    description   TEXT    NOT NULL DEFAULT '',
    UNIQUE(tenant_id, name)
);
```

### 1.4 振铃策略枚举

[queue.h#L30-L35](../../src/include/queue.h#L30-L35)

| DB 值 | 枚举 | 行为 |
|---|---|---|
| `ringall`      | `CQ_STRATEGY_RING_ALL`     | 所有空闲座席同振，首接听赢 |
| `least_recent` | `CQ_STRATEGY_LEAST_RECENT` | 选最久未通话的 |
| `round_robin`  | `CQ_STRATEGY_ROUND_ROBIN`  | 轮询上次接通的下一位 |
| `fewest_calls` | `CQ_STRATEGY_FEWEST_CALLS` | 累计接起次数最少的 |

### 1.5 默认 MoH 素材

[queue.h#L27-L28](../../src/include/queue.h#L27-L28)、[b2bua.c#L9228-L9255](../../src/sip/b2bua.c#L9228-L9255)

- `moh_sound` 为空字符串或 `"default"` → 使用编译期常量 `CQ_DEFAULT_MOH_WAV = "./sounds/default/moh.wav"`
- 路径按 **nsbcd 启动目录**解析（systemd `WorkingDirectory=/opt/nsbc` 时 → `/opt/nsbc/sounds/default/moh.wav`）
- `access(moh, R_OK)` 失败 → 降级为 **183 无 SDP**，主叫听本地回铃；**不会出现静音**
- WAV 解码：PCM 8/16-bit，引擎自动重采样为 8k 单声道

### 1.6 溢出目标写法

`overflow` 字段支持：
- 队列名（不含前缀）：呼叫释放后入该队列
- `ivr:<name>`：转 IVR
- `voicemail:<name>`：语音信箱
- `sip:user@host:port`：字面量 URI

溢出触发条件：`cq_tick` 每秒检查 waiter 的 `enqueue_ms + max_wait_s`，到期调用 `ovf_cb` → `b2b_queue_abort(call, 480)`，A 腿回 480 Temporarily Unavailable。

---

## 2. 分机状态 API

### 2.1 两套表

| 表 | 分片 | 容量 | 字段 | 生命周期 |
|---|---|---|---|---|
| **call_table** | 64 × 256 | 8192 并发 | `(tenant, ext) → slot0=被叫引用计数 / slot1=主叫引用计数` | 通话期间，结束释放 |
| **ext_table**  | 64 × 512 | 32768 分机 | `busy, paused, pause_reason, since_ms` | **跨 reload 保留**，永不删除 |

`pause_reason` 取值（[reload_commands.c#L306-L316](../../src/oam/reload_commands.c#L306-L316)）：
- `1` = break（小休/午餐）
- `2` = training（培训）
- `3` = other（其他）

busy 是**引用计数**（多个同时振铃会 +N，全部挂断才归 0），pause 是布尔 + reason。

### 2.2 REST

入口：[queues.rs#L182-L228](../../web/src/routes/queues.rs#L182-L228)

**置忙** `POST /queues/agents/pause`
```jsonc
{ "tenant": "nway", "ext": "1007", "reason": 1 }
```
响应：`{ "ok": true, "tenant": "nway", "ext": "1007", "paused": true, "reason": 1 }`

**恢复** `POST /queues/agents/resume`
```jsonc
{ "tenant": "nway", "ext": "1007" }
```

**查询单个** `GET /queues/agents/state?tenant=nway&ext=1007`
响应：`{ "ok": true, "raw": "OK tenant=nway ext=1007 busy=0 paused=1 reason=1 since_ms=1732138800000" }`

**列表** `GET /queues/agents/agents?tenant=nway`
响应：透传 OAM `cc.agent_list` 的 JSON 输出。

### 2.3 OAM 命令（UDS / systemd socket）

直接打到 `/var/run/nsbc/control.sock`：

| 命令 | 参数 | 说明 |
|---|---|---|
| `cc.agent_pause  <tenant|*> <ext> [reason]` | tenant **必填**（通配符只查有效） | 置忙；`*` 会报错，必须落到具体租户 |
| `cc.agent_resume <tenant|*> <ext>` | tenant 必填 | 恢复 |
| `cc.agent_state  <tenant|*> <ext>` | — | 打印 `busy / paused / reason / since_ms` |
| `cc.agent_list   [tenant]` | — | JSON 输出所有追踪分机 |

示例：
```bash
echo 'cc.agent_pause nway 1007 1' | nc -U /var/run/nsbc/control.sock
# → OK ext 1007 paused reason=1
echo 'cc.agent_state nway 1007' | nc -U /var/run/nsbc/control.sock
# → OK tenant=nway ext=1007 busy=0 paused=1 reason=1 since_ms=1732138800000
```

> ⚠️ 置忙命令强制要求 `tenant` 参数。队列选路按 `q->tenant` 查 ext_table，tenant 为空会写进 `""` 条目，永远匹配不上，成为静默无效操作。

### 2.4 busy 引用计数的增减点

- **+1**：`cq_pick_member` 抢到座席 → `call_table[tenant:ext].slot0++`；同时 B 腿建腿时 `slot1++`
- **-1**：B 腿最终态（2xx 接通 / 4xx 拒绝 / Timer B 超时）→ `b2b_queue_requeue` 或 `finish_with_a_response` 调 `perkey_dec` 后清 slot；`sig_on_end_cb` 调 `cq_settle_call` 统一结算

队列表条目在 slot0/slot1 都归 0 时才删除。

---

## 3. 拨号规则把呼叫送入队列

### 3.1 target 写法

dialplan 的 `target` 字段新增 **`queue:<queue_name>`** 前缀（[dialplan.h#L65](../../src/include/dialplan.h#L65)、[dialplan.c#L88](../../src/mod/dialplans/dialplan.c#L88)）。

所有配置统一通过 **Web UI → 拨号规则** 管理，nsbc-web 写 SQLite `dialplan_rules` 表后自动推送给核心（`dialplan_rules.reload`）。

在 Web UI 新建/编辑拨号规则时填写：

| 字段 | 示例值 |
|---|---|
| Profile | `external` |
| CalleeMatch (PCRE2) | `^4008000001$` |
| Target | `queue:support` |
| Priority | `10` |
| Enabled | ✅ |

两条典型规则：
- **客户热线 `4008000001` → `support` 队列**（全忙入队等待 + MoH）
- **销售专线 `4008000002` → `sales_immediate` 队列**（`max_wait_sec=0`，全忙立即 486）

### 3.2 resolve_cb 队列选路流程

[signaling.c#L1060-L1169](../../src/core/signaling.c#L1060-L1169)

```
handle_new_invite
  → dp_execute → target="queue:support"
  → resolve_cb 剥前缀拿 queue name
  → cq_pick_member(mgr, tenant, qname, avail_cb, ...)
      ├── rc==0：抢到成员 → sig_route_queue_member → b2b_start_b_leg
      └── rc!=0：全忙/置忙/未注册
            ├── call==NULL → NSBC_BUSY（originate 无上下文不能排队）
            ├── `q->max_wait_s<=0` → NSBC_BUSY（队列关闭排队）
            └── cq_enqueue → NSBC_QUEUED
                  → 上层收到 NSBC_QUEUED → b2b_queue_hold 建 hold relay + 183 + MoH
```

### 3.3 排队后的媒体链

[b2bua.c#L9240-L9325](../../src/sip/b2bua.c#L9240-L9325)

`b2b_queue_hold` 做四件事：
1. `bridge_local_start(call, &a_rtp)` — 建 **hold relay**，A 腿 RTP 收到转发到本地 MoH，同时回 **183 Session Progress + SDP**（端口就是 relay 端口）
2. `pb_start_ex(call, wav, loop=1, stop_on_answer=1, ...)` — 循环放 MoH，B 腿 2xx 接通自动停
3. 选默认素材：`moh_sound` 空或 `"default"` → `./sounds/default/moh.wav`；`access(R_OK)` 失败降级为 `moh=""` → 183 无 SDP，主叫听本地回铃
4. `call->queue_wait = 1` — **这里才置位**，之前 resolve_cb 不提前置位（避免 hold 幂等返回）

派发时 `bridge_offer_hook` 检测到 relay 存在 → 复用同一 relay 端口给 B 腿，**183/200 SDP 端口一致、媒体不断流**。

### 3.4 派发/退回排队/溢出

[cq_tick 1s](../../src/core/signaling.c#L106) → `cq_peek_pending` → `cq_pick_member` → `cq_mark_dispatched` → `reactor_call(ctx->re, sig_cc_dispatch_task)` 投递到 SIP 线程 → `b2b_queue_dispatch` → `b2b_start_b_leg`。

| B 腿结果 | 行为 |
|---|---|
| 2xx 接通 | `c->queue_wait=0`，`cq_settle_call` 结算统计 |
| 486 Busy Here / 480 Temporarily Unavailable / 408 Timer B | `b2b_queue_requeue` 清 dlg/inv_txn/按 key 并发槽 → `cq_requeue` 重置 `dispatch_ms`，下轮 tick 继续派 |
| max_wait 到期 | `ovf_cb → b2b_queue_abort(call, 480)`，主叫收 480 |

### 3.5 reload 语义

`queues.reload` 不再清空 `m->n` 重建；改为 `seen[]` 位图原地覆盖同名队列。**有 waiter 的消失队列**（DB 里删掉或 disabled=0）保留在内存中直到 max_wait 溢出，避免排队中 183 呼叫被静默丢弃。

---

## 4. ai_backends 下配置分机队列

### 4.1 ai_backends 结构

DB 表 `ai_backends`（[mod.rs#L6541-L6574](../../web/src/db/mod.rs#L6541-L6574)），CRUD 路由 `/api/ai-backends`：

| 字段 | 说明 |
|---|---|
| `name` | 后端标识，唯一 |
| `llm_provider / llm_model / llm_api_key / llm_endpoint` | LLM 配置 |
| `asr_provider / asr_addr` | ASR |
| `tts_provider / tts_addr` | TTS |
| `transfer_dest` | **默认转接目标**（分机号 / SIP URI）。AI 对话中没指定 dest 时用 |
| `barge_in` | 是否允许用户打断 AI |
| `vad_threshold / vad_mode` | VAD 门限和模式 |
| `enabled` | 是否启用 |

**注意**：`transfer_dest` 字段不直接引用队列名。AI 转队列的路径是对话内部走 **`dest="queue:xxx"`**，不是 ai_backends 配置字段。

### 4.2 AI 对话中转队列

[ai_dialog.c#L1255-L1296](../../src/mod/ai_dialog/ai_dialog.c#L1255-L1296)

AI Agent 调用 transfer API 时，`dest` 参数按以下前缀分支：

| dest 写法 | 行为 |
|---|---|
| `queue:support` | `cq_pick_member` 从 support 队列挑一个**当前可振铃**的座席（不排队）→ REFER |
| `extension:1007` | 剥前缀补 realm → REFER |
| 纯分机号 `1007` | 同上，补 realm |
| `sip:user@host:port` / 含 `@` | 完整 SIP URI，原样 REFER |

**与拨号规则的区别**：AI 转队列是「**转队列当前可用的一个座席**」，不做排队等待。没挑到可用成员直接返回 `NSBC_NOENT`，调用方收到明确失败而不是 REFER 到一个永远接不通的目标。

伪代码：
```c
if (!strncmp(dest, "queue:", 6)) {
    qname = dest + 6;
    prc = cq_pick_member(mgr, realm, qname, 0,
                         ai_queue_member_avail, &av, call_key,
                         member, sizeof(member));
    if (prc != 0) return NSBC_NOENT;     // 没可用座席 → 失败
    snprintf(full_dest, "sip:%s@%s", member, realm);  // 选中的那个座席
}
b2b_transfer_call(call_key, full_dest, ...);
```

### 4.3 推荐集成模式（全 Web UI 配置）

所有配置都从 Web UI 写 SQLite，nsbc-web 自动推送给核心，**`nsbc.yaml` 不需要任何 dialplan 或 ai_backends 字段**。

Web UI 中按以下 4 步搭建：
1. 「分机队列」页面建队列 `support` / `sales`，填成员分机号，`ring_strategy` 选 `least_recent`
2. 「AI Backends」页面建 backend `default`，`transfer_dest` 留空（让对话意图路由动态决定）
3. 「拨号规则」页面新建规则，把客户热线 `^4008000001$` 的 `target` 填 `ai:default`，Priority `10`
4. AI Agent 对话脚本里用 `queue:xxx` 语法转队列，例如：
   - 意图 `human_handoff` + 槽位 `dept=support` → transfer `dest="queue:support"`
   - 意图 `human_handoff` + 槽位 `dept=sales`   → transfer `dest="queue:sales"`
   - 意图 `end_conversation`                  → hangup

### 4.4 AI 转队列 vs 拨号规则入队

| 维度 | 拨号规则 `target: queue:xxx` | AI transfer `dest: queue:xxx` |
|---|---|---|
| 语义 | 入队等待，全忙排队 + 183 + MoH | 立即转当前可用座席，无排队 |
| 全忙时 | 入队 / 486（看 max_wait_s） | 返回 NOENT，AI 自己决定话术 |
| 媒体 | 主叫已在队列听 MoH | AI 正在通话中，REFER 后媒体切换 |
| 调用时机 | INVITE 入口 resolve | 对话中途（用户说“找人工”） |

---

## 5. 运维 checklist

### 5.1 部署前

- [ ] 确认 `/opt/nsbc/sounds/default/moh.wav` 存在且 nsbcd 用户可读（WAV 需 PCM 8/16-bit）
- [ ] 启动顺序：先 `nsbc-web`（SQLite 自动建表），再 `nsbcd`
- [ ] `WorkingDirectory=/opt/nsbc`（systemd unit）
- [ ] 首次启动后手动 `queues.reload` 推一次 DB 配置到核心

### 5.2 验证命令

```bash
# 推队列到核心
echo 'queues.reload' | nc -U /var/run/nsbc/control.sock

# 查看队列成员状态
echo 'cc.agent_list nway' | nc -U /var/run/nsbc/control.sock

# 置忙一个分机测试排队
echo 'cc.agent_pause nway 1007 1' | nc -U /var/run/nsbc/control.sock

# 查单分机
echo 'cc.agent_state nway 1007' | nc -U /var/run/nsbc/control.sock
```

### 5.3 日志关键字

| 关键字 | 含义 |
|---|---|
| `resolve: queue 'xxx' → member '1007'` | 队列选路命中 |
| `resolve: queue 'xxx' ... → QUEUED (max_wait=300s moh='default')` | 全忙入队 |
| `hold MoH OK ... wav=./sounds/default/moh.wav` | hold relay + MoH 建好 |
| `hold: moh '...' not readable → ringback` | 素材缺失降级回铃 |
| `cq_tick dispatch xxx → agent 1008` | tick 派发成功 |
| `queue_wait ... b2b_queue_requeue` | B 腿失败退回排队 |
| `queue_wait ... b2b_queue_abort status=480` | max_wait 溢出 |

### 5.4 已知边界（v0.6.2-beta 未实现）

- **WRAP 同步路径**：resolve 抢占座席时不跳过 WRAP，需等下次 tick 才会判 WRAP 结束
- **中继显撞号**：未注册分机可能被误判为置忙（call_table slot0 残留）
- **盲转后 busy 保持**：B 腿盲转后原分机 busy 引用计数未正确归位
- **486 后座席冷却**：同一座席刚 486 可能立刻再次振铃，无 per-agent 冷却

### 5.5 排查：队列接通后"一会就自动挂断"

这是 v0.6.1 生产环境出现过的症状——A 腿 183 → B 腿 200 OK 接通 → 双方通话几秒 ~ 几十秒后，nsbcd 主动发 BYE 或双方同时断流。v0.6.2-beta 已修 4 处根因，下面是诊断顺序。

#### 症状定位

```bash
# 步骤 1：拿到具体 callid，grep 整条链
grep 'hold MoH OK' logs/nsbcd.log | tail -5
grep '<callid>' logs/nsbcd.log
```

接通后必过的三个日志（**按序出现**）：
```
resolve: queue 'support' → member '1007'       ← resolve_cb 选路
hold MoH OK ... wav=./sounds/default/moh.wav    ← b2b_queue_hold 建 relay
media BRIDGE_CONFIRM ... offer_len=... answer_len=...  ← bridge_confirm ACK 后
```

如果 `BRIDGE_CONFIRM` 之后紧接着出现下列任一日志 → 定位方向确定：

| 后续日志 | 原因 | 定位 |
|---|---|---|
| `b2b_queue_requeue` / `queue_wait ... retry` | B 腿 4xx/408 被误判退回排队 | [b2bua.c#L3800-L3870](../../src/sip/b2bua.c#L3800-L3870) 的 B 腿非 2xx 分支 |
| `pong miss MAX -> close fd=NNN` | ws_listener 认为 WS 连接死了，关 fd → 注册状态丢失或 session 结束 | [ws_listener.c#L821-L827](../../src/net/ws_listener.c#L821-L827) |
| `SESSION TIMEOUT EXPIRED` / `sess_timer_expire` | Session Timer 超时未收到 re-INVITE/UPDATE | [b2bua.c 末尾 sess_timer_expire_cb](../../src/sip/b2bua.c) |
| `call_destroy BEGIN ... a_final=0 leg0=1 leg1=2` | 正常挂断（a_final=0 表示非 A 主动 BYE），看前面一条谁先发 BYE | [b2bua.c#L1058](../../src/sip/b2bua.c#L1058) |
| `BRIDGE_CONFIRM ... relay=1 local_media=1`（**relay 存在却 local_media=1**） | hold relay 没被复用，offer_hook 被 BYPASS 降级 → 183/200 端口跳变，媒体断 | [media_bridge.c offer_hook](../../src/media/bridge/media_bridge.c) |

#### 最常见 4 个根因（v0.6.2-beta 已修）

**① ws_listener pong miss MAX 误杀**

WS/WSS 注册连接空闲 90s（3 × WS_PING_TIMEOUT_MS=15s）没回 pong 就关 fd。WebRTC 分机浏览器标签页在后台时 ping/pong 可能延迟；或者 TLS 缓冲读阻塞导致 ping 帧发不出去 → 连杀带断，已注册分机变成"幽灵空闲态"，被队列当作可振铃座席选中 → 振铃后立即 408。

```bash
# 诊断：看有没有 pong miss MAX
grep 'pong miss MAX' logs/nsbcd.log
# 修复（v0.6.2-beta 已合入）：ws_listener.c 4 处修复——
#   - EPOLLERR/EPOLLHUP 时 continue 不跳出 worker 循环（原来会 break）
#   - PING payload 必须 echo 完整，不能截断（部分浏览器拒绝空 pong）
#   - TLS 场景下用非阻塞缓冲读避免 ping 发送被卡住
#   - 动态 read_buf 64KB 上限避免大 payload 时 malloc 失败
```

**② queue_wait 提前置位导致 hold 幂等返回**

resolve_cb 在返回 NSBC_QUEUED 前就把 `call->queue_wait = 1`，而 b2b_queue_hold 以 `queue_wait==0` 判定首次进入 → 幂等直接 return，183/SDP/relay/MoH **全部没建**，主叫只收到 486 或提前结束。

```c
/* 正确顺序：resolve_cb 只填 queue_tenant/queue_name/queue_moh 三元组，
 * queue_wait=1 留到 b2b_queue_hold 内部设置 */
/* 修复（v0.6.2-beta 已合入）：signaling.c#L1142-L1149 删掉提前置位 */
```

**③ bridge_offer_hook 没复用 hold relay**

183 已建了 hold relay（端口 X），200 OK 到达 B 腿 offer 时如果 offer_hook 判定 BYPASS（默认），会新建 relay（端口 Y）→ 183/200 SDP 端口不一致，WebRTC DTLS 握手失败 → 媒体断流几秒后 SIP 层被动挂断。

```c
/* 修复（v0.6.2-beta 已合入）：offer_hook 检测到 e->relay 存在时强制 MB_RELAY，
 * 复用 rtp_relay_port(e->relay, 0/1)，local_media=0 跳过 rtp_relay_start */
/* 见 src/media/bridge/media_bridge.c bridge_offer_hook */
```

**④ B 腿 4xx/408 后 b2b_queue_requeue 误触发**

B 腿接通后的 2xx 响应处理里有一行 `c->queue_wait = 0`，确保已接起不再退回排队。如果这个标志没清干净（被别的路径残留），后续 B 腿因会话内原因（如 hold 时的 REFER 失败）触发非 2xx 响应时，会走到 `if (c->queue_wait) b2b_queue_requeue` 分支 → 清掉已接通的 B 腿 dlg → 对话被动结束。

```c
/* 修复（v0.6.2-beta 已合入）：b2bua.c#L3588 在 B 腿 !reinv 2xx 分支显式
 * c->queue_wait = 0; —— 已接起后所有后续异常走 finish_with_a_response，
 * 不再回 b2b_queue_requeue */
```

#### 仍然可能的边界（未全修）

| 边界 | 现象 | 临时规避 |
|---|---|---|
| Session Timer 协商但 ACK 丢失 | 接通 60s（session-expires 默认）后 nsbcd 主动 BYE | 关闭 sess_timer_enabled：`co->cfg.sess_timer_enabled = 0` |
| 中继 BYE 提前到达 | B 腿中继侧先挂，nsbcd 透传到 A 腿 → 双方断 | 查 trunk 的 `inband_detect` 和 codecs 是否匹配 |
| hold relay 在 B 腿接通后未释放 | 媒体面仍走 hold relay → 听到双方对话但有延迟/杂音 | v0.6.2-beta 的 bridge_confirm 会正确设 e->local_media=1，确认 `BRIDGE_CONFIRM ... relay=1 local_media=1` 日志 |

#### 一条命令抓全链路

```bash
# 把某个 callid 的完整生命周期按时间线拉出来
CID="<callid>"
grep -E "${CID}|pong miss|BRIDGE_CONFIRM|b2b_queue|SESSION TIMEOUT|call_destroy BEGIN" \
  logs/nsbcd.log | sort | less
```

---

## 6. 文件速查

| 模块 | 路径 |
|---|---|
| 队列核心 | [src/mod/callcenter/queue.c](../../src/mod/callcenter/queue.c) |
| 队列头 | [src/include/queue.h](../../src/include/queue.h) |
| resolve_cb 队列选路 | [src/core/signaling.c](../../src/core/signaling.c) |
| b2b_queue_hold/dispatch/abort | [src/sip/b2bua.c](../../src/sip/b2bua.c) |
| bridge_offer_hook relay 复用 | [src/media/bridge/media_bridge.c](../../src/media/bridge/media_bridge.c) |
| AI 转队列 | [src/mod/ai_dialog/ai_dialog.c](../../src/mod/ai_dialog/ai_dialog.c) |
| OAM 命令 | [src/oam/reload_commands.c](../../src/oam/reload_commands.c) |
| REST 路由 | [web/src/routes/queues.rs](../../web/src/routes/queues.rs)、[web/src/routes/ai_backends.rs](../../web/src/routes/ai_backends.rs) |
| DB 模型 | [web/src/db/mod.rs](../../web/src/db/mod.rs) |
| C←Web 同步 DTO | [web/src/sync.rs](../../web/src/sync.rs) |
| 前端表单 | [web/static/js/pages-crud.js](../../web/static/js/pages-crud.js) |
