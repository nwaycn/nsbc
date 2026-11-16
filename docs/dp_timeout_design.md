# Dialplan Per-Call Timeout 设计

**2026-11-15**

---

## 1. 现状速查

| Timer | 作用域 | 谁 arm | 谁 disarm | 默认值 | 现状 |
|---|---|---|---|---|---|
| `noanswer_node` | ringing 期 | `arm_noanswer_timer()` 每条 INVITE leg 创建时 | `call_destroy` catch-all + 腿 answer 时 | `sip.ring_noanswer_timeout_s=60` | ✅ 已有，全局 + `originate_timeout` brace var override |
| Session Timer `expire_node` | 接通后（RFC4028） | 200 OK 协商后 arm | `call_destroy` | `def_se_s=1800`（30min） | ✅ 已有，但 B-leg arm 缺 `max_se_s` 裁剪（已修） |
| `call_limit_node`（刚加） | 接通后 | originate 创建的 call answer 后 arm | `call_destroy` | `sip.originate_caller_timeout_s=0` | ✅ 已有全局 cfg，但 **originate 专属**，dialplan 规则设不了 |
| **sched_hangup**（本次新增） | 接通后 | 接入路径 + originate 路径 | `call_destroy` | `0`（不限） | ❌ 缺失 |
| **originate_call_timeout**（本次新增） | 发起 B-leg 等待接通 | originate 创建 B-leg INVITE 时 | `call_destroy` + 腿 answer 时 | `50s` | ❌ 缺失，目前 B-leg 也走全局 ring_noanswer |

### 现有 timeout 覆盖矩阵

| 场景 | ringing 期超时 | 接通后超时 |
|---|---|---|
| **入站 INVITE → 分机** | ✅ 全局 `ring_noanswer_timeout_s` | ✅ Session Timer（UA 自报 90–1800s） |
| **originate → B2BUA B-leg** | ✅ 全局 + `originate_timeout` brace var override | ✅ 全局 `originate_caller_timeout_s` |
| **bridge（A leg answer → B leg）** | ✅ 全局（B-leg arm 时又 arm 一次） | ⚠️ call 级共享 —— 双腿都 confirm 后算接通 |
| **dialplan 规则指定不同业务线不同 timeout** | ❌ 无 per-rule override | ❌ 无 per-rule override |

---

## 2. 需求拆解

### 2.1 `originate_call_timeout` — 发起 B-leg 的呼叫等待超时

**语义**：originate / bridge 路径里，对 B-leg 发出 INVITE 后，在 **B-leg 接通前** 等待多久没有 answer 就停止 ringing，直接 486 Busy Here 终止整个 call。

**生命周期**：

```
originate 创建 call
    │
    ├─ arm call_limit_ring_noanswer(B-leg)  ──  ← 新逻辑
    │   时间 = originate_call_timeout_ms (优先)
    │         else cfg.ring_noanswer_ms
    │
    ├── B-leg 200 OK → disarm，call 继续 → maybe_start_media → arm sched_hangup
    │
    └── 超时 → B-leg 腿 480 / B-leg 腿 CANCEL → b2b_terminate(c, 486)
```

**默认值**：50 秒（比全局 `ring_noanswer_timeout_s=60` 短一点，符合外呼语义——外呼通常比入站需要更快失败）

**覆盖矩阵**：
| 来源 | 优先级 |
|---|---|
| 1. OAM CLI brace var `{originate_call_timeout=30}` | 最高 |
| 2. Dialplan rule `originate_call_timeout` 字段 | 中 |
| 3. 全局 cfg（`0` 时 fallback 全局 `ring_noanswer_ms`） | 底 |

**FS 对应**：`originate_timeout`（FS 的这个变量既控制 ringing 超时又控制 originate 命令等待，我们这里把 per-call ringing override 独立为 `originate_call_timeout`）

### 2.2 `sched_hangup` — 接通后最大通话时长

**语义**：call 双腿都接通（或 originate 单腿接通）后，强制挂断整个 call 的最大时长（秒）。`0` = 不限制。

**生命周期**：

```
腿 confirm / maybe_start_media()
    │
    ├─ arm sched_hangup_timer(c)
    │   时间 = sched_hangup_ms (优先)
    │         else (入站) Session Timer 继续生效
    │         else (originate) cfg.originate_caller_timeout_ms（全局）
    │
    ├── B-leg 200 OK → 续 arm，到期自动顺延
    │
    └── 超时 → disarm all → b2b_terminate(c, 486)
```

**默认值**：0（不限）

**覆盖矩阵**：
| 来源 | 优先级 | 适用场景 |
|---|---|---|
| 1. OAM CLI brace var `{sched_hangup=3600}` | 最高 | originate |
| 2. Dialplan rule `sched_hangup` 字段 | 中 | 入站 + originate |
| 3. Session Timer | 底 | 入站（UA 自报）|
| 4. cfg `originate_caller_timeout_s` | 底 | originate（全局）|

**FS 对应**：

```xml
<!-- FS 原生写法：接通后 60 分钟挂断 -->
<action application="sched_hangup" data="+3600000 normal_hangup"/>
<!-- 或变量 -->
<action application="set" data="sched_hangup=+3600000"/>
```

**我们的简化**：只接一个正整数秒（接通后 N 秒挂断），不支持 FS 的 `+` / `-` / `time+offset` / hangup cause 字符串。

---

## 3. 设计方案

### 3.1 dialplan 规则 schema 扩展

在 `dp_rule_t` / `dp_result_t` 里加两个字段：

```c
// src/include/dialplan.h

typedef struct {
    ...                       // 现有字段保持不动
    uint8_t force_prack;
    uint8_t force_precondition;
    uint8_t force_timer;
    uint8_t force_gruu;

    /* REQ-DP-TIMEOUT-001：dialplan 规则级超时覆盖 */
    uint32_t sched_hangup_s;          /* 接通后最大通话时长（秒，0=不限） */
    uint32_t originate_call_timeout_s;/* B-leg 发起等待接通超时（秒，0=走默认 50s） */
} dp_result_t;
```

### 3.2 b2b_call_t 扩展

```c
// src/include/b2bua.h

struct b2b_call {
    ...
    tw_node_t noanswer_node;
    tw_node_t call_limit_node;       /* 刚加 */

    /* REQ-DP-TIMEOUT-001：per-call 超时 override（毫秒，0=走上层） */
    int sched_hangup_ms_override;
    int noanswer_ms_override;        /* 已存在 */

    /* originate 来源标记（刚加）*/
    uint8_t originate_created;
    ...
};
```

### 3.3 `b2b_orig_t` 扩展（OAM/CLI 透传）

```c
typedef struct {
    ...
    int timeout_ms;                  /* 已存在：originate_timeout brace var 解析 */

    /* REQ-DP-TIMEOUT-001：OAM brace var 新支持 */
    int sched_hangup_s;              /* brace {sched_hangup=3600} → 3600s */
    int originate_call_timeout_s;    /* brace {originate_call_timeout=50} → 50s；0=走全局默认 */
    ...
} b2b_orig_t;
```

### 3.4 新增 cfg schema 行（全局默认）

```c
// src/core/cfg_schema.c

{"sip.originate_call_timeout_s", CGT_INT, CGF_HOT, "50", NULL, 0, 3600},
/* REQ-DP-TIMEOUT-001：originate 发起 B-leg 等待接通的超时（秒）。
 * 0=走全局 ring_noanswer_timeout_s（60s）；dialplan rule 或 OAM brace var 可覆盖。
 * 注意：这是 originate 专属的 ringing 期超时，入站 INVITE 走 ring_noanswer_timeout_s。 */
```

### 3.5 代码路径修改点

| 位置 | 文件 | 改什么 |
|---|---|---|
| dialplan schema | `src/include/dialplan.h` | `dp_rule_t` / `dp_result_t` 加 2 字段 |
| dialplan rule 解析 | `src/mod/dialplans/dialplan.c` | 规则字符串解析时读 `sched_hangup=` / `originate_call_timeout=` |
| dp_result 装配 | `src/core/signaling.c` | handle_new_invite 从 dp_result 填 b2b_call |
| originate create | `src/sip/b2bua.c:7200` | 从 `b2b_orig_t` 填 `sched_hangup_ms_override` + `noanswer_ms_override`（B-leg 专属）|
| **新建** | `src/sip/b2bua.c` | `arm_sched_hangup_timer(c)` / `disarm_sched_hangup_timer(c)` / `sched_hangup_expire_cb` |
| maybe_start_media | `src/sip/b2bua.c:2239` | 双腿 confirm 后 arm `sched_hangup_timer` |
| arm_noanswer_timer | `src/sip/b2bua.c:2515` | originate 创建的 B-leg 用 `originate_call_timeout_s` 覆盖 |
| call_ctor | `src/sip/b2bua.c:493` | call_ctor 里 `tw_node_init(&c->sched_hangup_node)` |
| call_destroy | `src/sip/b2bua.c:1020` | disarm catch-all |
| OAM brace 解析 | `src/oam/call_commands.c:863` | 新增 `sched_hangup` / `originate_call_timeout` key |

### 3.6 优先级决策函数（写一处，用多处）

```c
/* REQ-DP-TIMEOUT-001：统一的 sched_hangup 决策链
 * 返回 0 = 不限；>0 = 毫秒数 */
static uint32_t decide_sched_hangup_ms(const b2b_call_t *c)
{
    if (c->sched_hangup_ms_override > 0)    return (uint32_t)c->sched_hangup_ms_override;
    /* 入站 INVITE 走 Session Timer，originate 走全局 */
    if (c->originate_created && co->cfg.originate_caller_timeout_ms > 0)
        return co->cfg.originate_caller_timeout_ms;
    return 0;  /* 不限制 */
}

/* REQ-DP-TIMEOUT-001：B-leg ringing 期超时决策链
 * 返回 0 = 不 arm；>0 = 毫秒数 */
static uint32_t decide_b_leg_noanswer_ms(const b2b_call_t *c)
{
    if (c->originate_call_timeout_ms_override > 0)
        return (uint32_t)c->originate_call_timeout_ms_override;
    /* 没 override → 0 = 走默认 50s（cfg） */
    return co->cfg.originate_call_timeout_ms;
}
```

### 3.7 dialplan 规则格式扩展

```
# 格式：match:layer:target[:flags]
# 新增 flags 内可带两个超时字段（逗号分隔）
# 未提供的字段保持全局默认
#
# 例1：普通分机（接通后 60 分钟挂断）
100[0-9]:ext:100:sched_hangup=3600

# 例2：业务号专线（外呼 B-leg 只等 25 秒，接通后 30 分钟挂断）
4000:ext:1001:originate_call_timeout=25,sched_hangup=1800

# 例3：trunk 组拨号（外呼走 B-leg，等待 40 秒，接通后 2 小时）
97800000[0-9]:trunk:sip.mytrunk:sched_hangup=7200,originate_call_timeout=40
```

flags 解析器复用现有 parse_dp_flags 框架，加两个 key。

### 3.8 OAM CLI brace var 新支持

```bash
# 接通后 1 小时挂断，B-leg 只等 30 秒
originate {sched_hangup=3600,originate_call_timeout=30} user/1001 &echo

# 通配 URL
originate {sched_hangup=7200} user/1002 &bridge(user/1003)
```

`parse_fs_brace_vars` 扩展两个 key。

### 3.9 Web 表单（可选）

dialplan rule 编辑页（Web UI）新增两个输入框，类型 number，单位秒，默认 0，help 说明 "0 = 不限 / 走全局默认"。

---

## 4. 决策树（每个 timer 的 arm/disarm）

### 4.1 `sched_hangup_node`（接通后最大通话时长）

```
arm 触发点：maybe_start_media()
            ├─ bridge 模式：legs[0].offer_len && legs[1].answer_len → arm
            └─ originate 单腿：legs[1].offer_len && legs[1].answer_len → arm

decision = decide_sched_hangup_ms(c)
if (decision > 0) {
    tw_add(core->cfg.tw, &c->sched_hangup_node, now + decision, expire_cb, c);
    llog(LOG_NOTICE, "b2b", "sched_hangup armed callid=%s timeout_ms=%u", callid, decision);
}

disarm 触发点：
    call_destroy() catch-all（所有终止路径都走）
    tw_del(core->cfg.tw, &c->sched_hangup_node);

expire 回调：
    lock → 检查 dead/leg state → unlock → b2b_terminate(c, 486)
    （cause=486 "Busy Here" 是最接近的语义——对端被系统强制挂断）
```

### 4.2 `originate_call_timeout`（B-leg ringing 期 override）

```
arm 触发点：arm_noanswer_timer(c) 已经在 originate 路径会被调
            → 只需要改 arm_noanswer_timer 里的优先级链：

决策：ms = (originate 路径 && originate_call_timeout_ms_override > 0)
            ? override
          : originate 路径 && cfg.originate_call_timeout_ms > 0
            ? cfg
          : co->cfg.ring_noanswer_ms;   // 入站 + fallback

（现有代码只支持 noanswer_ms_override → ring_noanswer_ms 两档）
```

---

## 5. 与现有 `originate_caller_timeout_s` 全局配置的关系

刚加的 `sip.originate_caller_timeout_s`（全局、originate 专属、接通后控制）**可以保留**，也可以**让 `sched_hangup` 完全取代它**。

建议：**保留**，原因：
- `sched_hangup` 是 per-call / per-rule override
- `originate_caller_timeout_s` 是全局默认（所有 originate 都生效，作为兜底）
- `sched_hangup > cfg.originate_caller_timeout_s > 0 = 不限制`

优先级链写在 `decide_sched_hangup_ms()` 里统一决策，一处维护。

---

## 6. 与现有 Session Timer 的关系

Session Timer 是 **RFC 4028 协议层**，由 UA 自报（通常 90-1800s），B2BUA 负责协商、刷新、超时 terminate。

`sched_hangup` 是 **应用层硬上限**，语义是"即使双方 Session Timer 说可以一直打，但打到这个时间我也强制挂"。

| | Session Timer | sched_hangup |
|---|---|---|
| 层级 | SIP 协议 | 应用/dialplan |
| 谁设 | UA 自报（min-se/session-expires） | dialplan rule / OAM brace |
| 默认 | min-se=90, def_se=1800 | 0（不限） |
| 到期后 | 双方都没刷新 → 任何一方可发 BYE | B2BUA 主动发 BYE |
| 谁刷新 | B2BUA refresher 定时 re-INVITE | — |
| 关系 | sched_hangup < Session Timer（sched_hangup 更小，先到先挂） |

如果 `sched_hangup = 3600` 但 Session Timer = 1800，30 分钟时 Session Timer 先到 → BYE。此时 `sched_hangup` 没机会触发。

**dialplan 规则加 `sched_hangup` 时建议值 ≤ Session Timer 上限**。我们可以在 arm 时打个 WARN 日志提醒。

---

## 7. 改动量估算

| 文件 | 改动 |
|---|---|
| `src/include/dialplan.h` | `dp_rule_t` / `dp_result_t` 各加 2 字段（8 行） |
| `src/include/b2bua.h` | `b2b_call_t` 加 `sched_hangup_node` + 2 个 override 字段（12 行） |
| `src/include/b2bua.h` | `b2b_orig_t` 加 2 brace var 字段（4 行） |
| `src/core/cfg_schema.c` | 新 schema 行（3 行） |
| `src/core/signaling.c` | cfg_get_int 读新值 + dp_result → b2b_call 透传（10 行） |
| `src/mod/dialplans/dialplan.c` | flags 解析器加 2 key（30 行） |
| `src/sip/b2bua.c` | call_ctor / originate create / maybe_start_media / call_destroy + 3 新函数（~100 行） |
| `src/oam/call_commands.c` | brace var 解析加 2 key（20 行） |
| 合计 | ~190 行 |

---

## 8. 不在本次范围

- 入站 INVITE 走 Session Timer，不额外 arm `sched_hangup`（除非后续想强制 override Session Timer —— 这需要更强的 B2BUA refresher 语义改造，另案）
- dialplan rule 生效范围：当前只打算从 flags 里读 key-value，不改现有 match:layer:target 格式
- Web 表单：本次不做，dialplan rule 用 flags 填，CLI 用 brace var 填即可

---

## 9. 决策清单（等你拍板）

| # | 问题 | 我的建议 |
|---|---|---|
| 1 | `originate_call_timeout` 默认值多少？ | **50s**（比全局 ring_noanswer=60s 短，外呼更积极失败） |
| 2 | `originate_call_timeout=0` 时 fallback 到全局 `ring_noanswer_timeout_s=60`？ | **是**（0=不指定，走全局） |
| 3 | `sched_hangup` 默认 0（不限）保留刚加的全局 `originate_caller_timeout_s`？ | **保留**，作为 originate 路径兜底，dialplan 规则可覆盖它 |
| 4 | `sched_hangup` 到期 cause code？ | **486 Busy Here**（语义最近） |
| 5 | dialplan rule flags 用什么分隔符？ | 沿用现有逗号分隔的 k=v |
| 6 | `sched_hangup` 入站 INVITE 也生效？ | **是**（接入路径里 dp_result 也有，双腿 confirm 后统一 arm） |
