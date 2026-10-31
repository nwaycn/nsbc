# NSBC IVR + Billing 需求梳理

> **Created**: 2026-10-31
> **Status**: Draft
> **Based on**: `src/mod/applications/ivr.c` + `src/mod/billing/rating.c` + Web DB schema audit

---

## Part A — IVR 增强需求

### A1. 多层 IVR（已实现，确认）

| 能力 | 现状 | 需求 |
|------|------|------|
| 菜单嵌套深度 | **128 nodes / profile**（`IVR_NODE_MAX=128`），choices 支持 SUBMENU 跳转 | ✅ 已满足 |
| 前缀累积多键 | `match_choice()` 支持精确 + 前缀匹配，累积 buf | ✅ 已满足 |
| 超时控制 | 首位 `initial_timeout_s` + 位间 `interdigit_timeout_s` + `max_retries` | ✅ 已满足 |
| 动作类型 | SUBMENU / TRANSFER / PLAY / VOICEMAIL / HTTP_QUERY / HANGUP | ✅ 已满足 |
| Web 管理 | `ivr_menus` 表 + `nodes_json` 完整菜单树 + reload sync | ✅ 已满足 |

### A2. 需新增能力（P0）

#### A2.1 Collect DTMF — 收集号码输入并传递

```
现状: ivr_choice_t 只有 {key, action}，匹配后直接执行 action
需求: 新增 collect 节点类型，按长度或 # 结束收集 DTMF 串

节点 JSON 扩展:
{
  "name": "collect_pin",
  "type": "collect",           // 新增: menu / collect / cond / play_only
  "prompt": "enter_pin.wav",
  "max_digits": 4,
  "min_digits": 4,
  "terminator": "#",           // 可选: 默认按 max_digits 自动结束
  "timeout_s": 10,
  "store_as": "caller_pin",    // 存入 call_vars，供后续 cond / transfer / log 使用
  "on_complete": {"act": "submenu", "arg": "verify_pin"},
  "on_timeout": {"act": "hangup", "arg": ""},
  "on_invalid": {"act": "play", "arg": "invalid_pin.wav"}
}
```

**价值**: PIN 验证、会议号码输入、客户编号、IVR 自助查询（查余额、查通话记录）

#### A2.2 Conditional Branch — 条件分支

```
现状: 所有节点走静态 choices 匹配
需求: cond 节点按 call_vars / 时间 / 主叫 走不同路径

{
  "name": "route_by_pin",
  "type": "cond",
  "prompt": "",
  "conditions": [
    {"if": "${caller_pin} == 1234", "then": {"act": "submenu", "arg": "admin"}},
    {"if": "${caller_pin} == 5678", "then": {"act": "submenu", "arg": "agent"}},
    {"if": "${caller_pin} >= 1000 && ${caller_pin} <= 9999", "then": {"act": "transfer", "arg": "100${caller_pin:0:2}"}},
    {"else": {"act": "play", "arg": "invalid_pin.wav"}}
  ]
}
```

**支持变量**: `${caller}` / `${callee}` / `${caller_pin}` / `${time:hour}` / `${time:weekday}` / `${tenant}` / `${call_duration}` / `${account_balance}`

#### A2.3 Time-based Routing — 时段路由

```
{
  "name": "main",
  "type": "menu",
  "schedule": [
    {"hours": "09:00-18:00", "weekdays": "mon-fri", "entry": "business_hours"},
    {"hours": "18:00-09:00", "weekdays": "mon-fri", "entry": "after_hours"},
    {"weekdays": "sat,sun", "entry": "weekend"}
  ],
  "default_entry": "business_hours",
  "nodes": [...]
}
```

#### A2.4 Context Passing — 子菜单继承父菜单上下文

```
现状: 每个 node 独立，SUBMENU 跳转后丢失之前的 collect 结果
需求: ivr_sess 持有 call_vars 字典，所有节点共享

struct ivr_sess {
    ...
    char call_vars[CALL_VAR_MAX][2][64];  // {"caller_pin": "1234", "queue": "sales"}
    int  n_vars;
};
```

### A3. 需新增能力（P1）

| 能力 | 描述 |
|------|------|
| **Nested menu fallback** | 子菜单找不到 choice 时向上冒泡 |
| **TTS prompt** | `{"act": "tts", "text": "您的余额是 ${balance} 元"}` |
| **Record caller input** | `{"act": "record", "max_seconds": 30, "store_as": "voicemail_file"}` |
| **Parallel play** | 边播 music on hold 边收 DTMF |
| **Multi-language** | `prompt_zh` / `prompt_en` 按 caller locale 选 |

### A4. 数据模型变更

```sql
-- ivr_menus 表不需要改 — nodes_json 已经是完整 JSON，前端扩展即可
-- C 侧 ivr_node_t 需要加:
typedef struct {
    ...existing fields...
    char      node_type[16];        -- "menu" / "collect" / "cond" / "play_only"
    int       collect_max_digits;   -- A2.1
    int       collect_min_digits;   -- A2.1
    char      collect_terminator;   -- A2.1
    char      store_as[32];         -- A2.1
    char      cond_expressions[4][256];  -- A2.2（4 条条件）
    char      cond_targets[4][64];       -- A2.2
} ivr_node_t;
```

---

## Part B — Billing 增强需求

### B1. 现状 vs 目标

| 能力 | 现状 | 目标 |
|------|------|------|
| 费率粒度 | **per 前缀唯一价格** | per 前缀 × per 客户 × per 中继 × per 时段 |
| 计费方式 | **单一 bill_inc_s + min_bill_s** | 阶梯计费（前 N 分钟 X 元/分，之后 Y 元/分）+ 不同块（6秒/分钟/3分钟起） |
| 客户余额 | **内存 acct_t[200000]** | DB 持久化 + 实时 sync + Web 充值 API |
| 透支控制 | **credit_limit 全局字段** | per 客户可配置 + 黑名单 + 强制拆线 |
| 数据库表 | **零** — 只有内存 + JSON reload | rate_plans / rate_plan_entries / customer_accounts / trunk_rate_overrides / billing_transactions |

### B2. 数据库 Schema（P0）

```sql
-- ============ 客户账户表 ============
CREATE TABLE customer_accounts (
    id              BIGSERIAL PRIMARY KEY,
    tenant_id       BIGINT NOT NULL DEFAULT 1,
    account_key     VARCHAR(64) NOT NULL UNIQUE,  -- "tenant:extension:1000" 或 "trunk:china_unicom"
    account_type    VARCHAR(16) NOT NULL,          -- "extension" / "trunk" / "tenant" / "reseller"
    balance_uc      BIGINT NOT NULL DEFAULT 0,     -- 微元
    credit_limit_uc BIGINT NOT NULL DEFAULT 0,     -- 微元（允许透支）
    low_threshold_uc BIGINT NOT NULL DEFAULT 0,     -- 低于此值告警
    rate_plan_id    BIGINT REFERENCES rate_plans(id),
    enabled         BOOLEAN NOT NULL DEFAULT TRUE,
    created_at      TIMESTAMP NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMP NOT NULL DEFAULT NOW()
);

-- ============ 费率方案（可复用模板） ============
CREATE TABLE rate_plans (
    id              BIGSERIAL PRIMARY KEY,
    tenant_id       BIGINT NOT NULL DEFAULT 1,
    name            VARCHAR(64) NOT NULL,           -- "默认方案" / "经济套餐" / "VIP 方案"
    description     TEXT,
    enabled         BOOLEAN NOT NULL DEFAULT TRUE,
    UNIQUE(tenant_id, name)
);

-- ============ 费率条目（阶梯 + 时段） ============
CREATE TABLE rate_plan_entries (
    id              BIGSERIAL PRIMARY KEY,
    plan_id         BIGINT NOT NULL REFERENCES rate_plans(id),
    prefix          VARCHAR(24) NOT NULL,           -- "010" / "86138" / "00" / "" (默认)
    trunk_id        BIGINT REFERENCES trunks(id),    -- NULL = 所有中继
    enabled         BOOLEAN NOT NULL DEFAULT TRUE,

    -- 时段（可选，NULL = 全时段）
    time_from       TIME,                           -- "09:00:00"
    time_to         TIME,                           -- "18:00:00"
    weekdays        VARCHAR(16),                    -- "1,2,3,4,5" (mon-fri) 或 NULL=每天

    -- 阶梯（多个条目共享 prefix，按 tier_min_sec 排序）
    tier_min_sec    BIGINT NOT NULL DEFAULT 0,      -- 0 = 第一阶梯
    tier_max_sec    BIGINT,                         -- NULL = 无上限
    price_per_min_uc BIGINT NOT NULL,               -- 微元/分钟

    -- 计费块规则
    bill_inc_s      INT NOT NULL DEFAULT 60,        -- 计费进位块（6=6秒计费, 60=分钟）
    min_bill_s      INT NOT NULL DEFAULT 60,        -- 最小收费秒数
    connect_fee_uc  BIGINT NOT NULL DEFAULT 0,      -- 接通费

    -- 排序权重（长前缀优先，同前缀按时段 specificity）
    precedence      INT NOT NULL DEFAULT 0
);

-- ============ 中继费率覆盖（中继维度的特殊费率） ============
CREATE TABLE trunk_rate_overrides (
    id              BIGSERIAL PRIMARY KEY,
    trunk_id        BIGINT NOT NULL REFERENCES trunks(id),
    prefix          VARCHAR(24) NOT NULL,
    price_per_min_uc BIGINT NOT NULL,
    bill_inc_s      INT NOT NULL DEFAULT 60,
    UNIQUE(trunk_id, prefix)
);

-- ============ 充值/扣费交易流水 ============
CREATE TABLE billing_transactions (
    id              BIGSERIAL PRIMARY KEY,
    account_id      BIGINT NOT NULL REFERENCES customer_accounts(id),
    call_id         VARCHAR(127),                   -- 关联 CDR（通话扣费）
    tx_type         VARCHAR(16) NOT NULL,           -- "topup" / "call_charge" / "credit_adjust" / "manual"
    amount_uc       BIGINT NOT NULL,                -- 正=充值, 负=扣费
    balance_after_uc BIGINT NOT NULL,               -- 交易后余额
    operator        VARCHAR(64),                    -- 操作人（manual 时）
    remark          TEXT,
    created_at      TIMESTAMP NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_billing_tx_account ON billing_transactions(account_id);
CREATE INDEX idx_billing_tx_call ON billing_transactions(call_id);
CREATE INDEX idx_billing_tx_created ON billing_transactions(created_at);
```

### B3. 阶梯计费算法（核心）

```c
// 一条 call 180 秒 (3分钟) 打 010xxxxxxxx，走 china_unicom 中继
// rate_plan_entries 匹配到 3 条：
//   prefix="010", trunk=unicom, tier [0, 180): price=6000 微元/分, min_bill=180, bill_inc=60
//   prefix="010", trunk=unicom, tier [180, 3600): price=3000 微元/分, bill_inc=6
//   prefix="010", trunk=unicom, tier [3600, ∞): price=1500 微元/分

int64_t calc_stair_charge(rate_entry_t **tiers, int n_tiers, uint32_t billsec) {
    int64_t total = 0;
    uint32_t remaining = billsec;

    for (int i = 0; i < n_tiers && remaining > 0; ++i) {
        rate_entry_t *t = tiers[i];
        uint32_t tier_span = t->tier_max_sec - t->tier_min_sec;
        uint32_t this_tier = (remaining < tier_span) ? remaining : tier_span;

        // 第一阶梯用 min_bill_s 兜底
        if (i == 0 && this_tier < (uint32_t)t->min_bill_s)
            this_tier = t->min_bill_s;

        // 按 bill_inc_s 向上取整
        if (t->bill_inc_s > 1 && this_tier % t->bill_inc_s)
            this_tier = (this_tier / t->bill_inc_s + 1) * t->bill_inc_s;

        total += t->price_per_min_uc * (int64_t)this_tier / 60;
        remaining -= tier_span;
    }
    return total;
}
```

### B4. 不同计费场景映射

| 场景 | rate_plan_entries 配置 |
|------|----------------------|
| **按分钟计费** | `bill_inc_s=60, min_bill_s=60` |
| **按 6 秒计费** | `bill_inc_s=6, min_bill_s=6` |
| **前 3 分钟 0.3 元/分，之后 0.1 元/分** | tier1: `tier_min=0, tier_max=180, price=30000, min_bill=180`; tier2: `tier_min=180, price=10000, bill_inc=6` |
| **接通费 + 按分钟** | `connect_fee_uc=50000 (5分), price_per_min_uc=150000 (1.5元/分)` |
| **工作日白天 0.2 元/分，夜间/周末 0.1 元/分** | 两条：`time_from=09:00, time_to=18:00, weekdays=1-5, price=200000`；`time_from=18:00, weekdays=all, price=100000` |
| **同一个 010，走 unicom 比 telecom 便宜** | `prefix=010, trunk=unicom, price=100000`; `prefix=010, trunk=telecom, price=150000` |

### B5. C 侧内存结构变更

```c
// billing_t 新增
typedef struct {
    // ... existing ...
    
    // 费率方案（替代单一 rates[]）
    plan_entry_t plan_entries[MAX_PLAN_ENTRIES];  // 展开后的 (plan × prefix × trunk × time × tier) 扁平数组
    int n_plan_entries;
    
    // 账户 ↔ 方案 绑定
    // acct_t 新增 rate_plan_id
    
    // 中继维度 lookup 加速
    // trunk_rate_map[trunk_id_hash] → plan_entry_t*
} billing_t;

// rate_lookup() 新签名
// 输入: called_number + account_key + trunk_id + now()
// 输出: 匹配到的费率条目（含所有匹配的 tier）
int rate_lookup_stair(billing_t *b, 
                      const char *called,
                      const char *account_key,
                      int trunk_id,
                      time_t now,
                      rate_entry_t **tiers_out, int *n_tiers_out);
```

### B6. 完整呼叫计费时序

```
INVITE → 180 Ringing → 200 OK → BYE
  │          │              │        │
  │          │              │        └─ bill_settle: 按 stair 算法算实际费用
  │          │              │           balance -= actual (多退少补)
  │          │              │
  │          │              └─ bill_reserve: 预占最大可能费用
  │          │                 rate_lookup_stair(called, account, trunk, now)
  │          │                 reserve = max_tier_charge × 2 (保守)
  │          │
  │          └─ (可选) low_water callback: balance < threshold → 强制拆线
  │
  └─ 呼叫前 rate_lookup_stair 失败 → 返回 PERM_DENIED → 呼叫被拒 (pre-paid)
     或 reserve 不足但 credit_limit 够 → 允许 (post-paid)
```

---

## Part C — 实施优先级

### Phase 1 — Billing 基础设施（P0，阻塞后续）

| ID | 任务 | 复杂度 | 依赖 |
|----|------|--------|------|
| B-DB-1 | `customer_accounts` / `rate_plans` / `rate_plan_entries` / `billing_transactions` 建表 | S | 无 |
| B-DB-2 | `trunk_rate_overrides` 建表 | XS | 无 |
| B-SYNC-1 | Web → nsbcd 的 rate_plans / rate_plan_entries JSON push | M | B-DB-1 |
| B-SYNC-2 | Web → nsbcd 的 customer_accounts JSON push（扩展现有 account_reload_json） | M | B-DB-1 |
| B-CORE-1 | C 侧 billing_t 扩展：plan_entries[] + rate_lookup_stair() | L | B-SYNC-1 |
| B-CORE-2 | 阶梯计费 calc_stair_charge() 实现 | L | B-CORE-1 |
| B-CORE-3 | reserve/settle 接入 rate_lookup_stair | M | B-CORE-2 |
| B-CORE-4 | trunk_rate_overrides 中继维度覆盖 | S | B-CORE-1 |
| B-WEB-1 | Web 管理界面：客户充值、费率方案编辑、阶梯可视化 | XL | B-DB-1 |
| B-WEB-2 | 交易流水查询 API + 导出 | M | B-DB-1 |
| B-OAM-1 | OAM 命令：`show rates` / `show accounts` / `accounts.reload` / `rates.reload` | S | B-CORE-1 |

### Phase 2 — IVR 增强（P0）

| ID | 任务 | 复杂度 | 依赖 |
|----|------|--------|------|
| IVR-1 | `collect` 节点类型 + DTMF 收集 + store_as 存入 call_vars | M | 无 |
| IVR-2 | `cond` 条件分支节点 | L | IVR-1 |
| IVR-3 | ivr_sess 加 call_vars 字典 | S | IVR-1 |
| IVR-4 | 时段路由（schedule 字段） | S | 无 |
| IVR-5 | 扩展 ivr_node_t 结构 + JSON 解析 | M | IVR-1 |
| IVR-6 | Web 节点编辑器：collect / cond 可视化 | L | IVR-5 |

### Phase 3 — 高级特性（P1）

| ID | 任务 | 复杂度 |
|----|------|--------|
| IVR-P1-1 | TTS prompt（HTTP 回调合成语音） | M |
| IVR-P1-2 | 录音留言（IVR 内 record action） | M |
| IVR-P1-3 | 多语言菜单（按 caller locale 选 prompt 文件） | S |
| B-P1-1 | 周期性扣费（包月套餐 / 60s 一次 tick） | L |
| B-P1-2 | 费率模板市场（预设方案一键应用） | M |
| B-P1-3 | 反欺诈：高额通话实时告警 + 自动拦截 | L |

---

## Part D — 关键设计决策

### D1. 金额单位：微元（1e-6 元）int64

**理由**：避免浮点精度问题。1 元 = 1,000,000 微元，int64 范围 ±9.2×10¹² ≈ ±9.2 万亿元，足够覆盖任何客户余额。

**保持一致**：现有 `price_per_min_uc` / `connect_fee_uc` / `balance_uc` 已用此单位，继续沿用。

### D2. 费率匹配优先级

```
1. trunk_rate_overrides[trunk_id + prefix]          ← 最高
2. rate_plan_entries[account.rate_plan_id + trunk + prefix + time]
3. rate_plan_entries[account.rate_plan_id + trunk + prefix]
4. rate_plan_entries[account.rate_plan_id + prefix + time]
5. rate_plan_entries[account.rate_plan_id + prefix]
6. rate_plan_entries[account.rate_plan_id + "" + time]   ← 默认
7. rate_plan_entries[account.rate_plan_id + ""]          ← 兜底
8. rate_plan_entries[tenant_default_plan + ""]            ← 租户默认
```

每条按 **长前缀优先** + **时段 specificity 优先** + **tier_min_sec 升序** 展开。

### D3. reserve 保守策略

呼叫前 reserve = 匹配费率的 **第一阶梯 2 分钟费用**（`max(price_per_min_uc × min_bill_s, price_per_min_uc × 120)`）。实际通话中如果跨过阶梯、实际费用超过 reserve：

- balance 够 → `bill_settle` 直接扣
- balance 不够但 credit_limit 够 → 扣到负（透支）
- balance + credit_limit 都不够 → **强制拆线**（组合根调 `uuid_kill`）

### D4. 多租户隔离

- 所有表都有 `tenant_id` 列
- `rate_plan_entries` 通过 `rate_plan_id` → `rate_plans.tenant_id` 关联
- C 侧 `account_key` 格式 `"tenant:<tid>:extension:<ext>"` —— tenant 在前，防止跨租户串账

### D5. CDR 与计费解耦

现有 `cdr` 表增加 3 列：
```sql
ALTER TABLE cdr ADD COLUMN charge_uc BIGINT;        -- 本次通话实际扣费
ALTER TABLE cdr ADD COLUMN rate_plan_id BIGINT;     -- 使用的费率方案
ALTER TABLE cdr ADD COLUMN billing_status VARCHAR(16);  -- "pending" / "charged" / "failed" / "refunded"
```

计费模块只负责算钱、改余额、写 billing_transactions。CDR 模块独立记录通话元数据 + 最终 charge_uc。

---

## Appendix — vs 竞品对比

| 能力 | FreeSWITCH | Asterisk | OpenSIPS | NSBC 现状 | NSBC 目标 |
|------|-----------|----------|----------|-----------|----------|
| 多层 IVR | ✅ dialplan + Lua | ✅ dialplan + AGI | ❌ 无内建 | ✅ 6 种 action + 128 节点 | ✅ + collect + cond + schedule + TTS |
| 阶梯计费 | ❌ 用外部系统 | ⚠️ 用 cgrates | ❌ | ❌ 单费率 | ✅ 内置 stair 算法 |
| 时段费率 | ❌ dialplan case | ⚠️ Time-based routing | ❌ | ❌ | ✅ time_from/to + weekdays |
| 中继维度费率 | ❌ | ⚠️ 用 cgrates | ❌ | ❌ | ✅ trunk_rate_overrides |
| 6 秒计费块 | ⚠️ 外部 | ⚠️ | ❌ | ⚠️ bill_inc_s 支持但阶梯未实现 | ✅ 6s + 阶梯组合 |
| 客户充值 | ❌ | ❌ | ❌ | ❌ 内存 account | ✅ DB + Web API + 流水 |
| 热 reload | ✅ `reloadxml` | ❌ 重启 | ✅ `reload` | ✅ mod.reload av / accounts.reload / rates.reload | ✅ 全部配置热 reload |
| 透支控制 | ❌ | ❌ | ❌ | ✅ credit_limit 字段 | ✅ per 客户 + 强制拆线 |

**差异化**：NSBC 目标是把计费做成 C core 内建能力（不像 FreeSWITCH/Asterisk 要外挂系统），同时保留模块热 reload 的运维便利性。
