# NSBC IVR + Billing 需求梳理

> **Created**: 2026-10-31
> **Updated**: 2026-11-02
> **Status**: Phase 1 + Phase 1.5 + Phase 1.6 资金闭环 + **Phase 2 全局开关 / L4 tick / L5 kill / OAM / Web / 前端全部完成（含审计修复）**；IVR 增强未启动
> **Based on**: [ivr.c](file:///d:/github/nsbc/src/mod/applications/ivr.c) + [rating.c](file:///d:/github/nsbc/src/mod/billing/rating.c) + [rating.h](file:///d:/github/nsbc/src/include/rating.h) + Web DB schema audit + Phase 1.5 实现 + Phase 1.6 资金闭环 + Phase 2 全部实现
> **Related**: [billing-state.md](./billing-state.md) · [billing-handoff.md](./billing-handoff.md)

---

## Part A — IVR 增强需求（Draft，未启动）

> IVR 侧 Phase 1/1.5 未改动，以下需求全部仍在 Draft 状态。

### A1. 多层 IVR（已实现，确认）

| 能力 | 现状 | 需求 |
|------|------|------|
| 菜单嵌套深度 | **128 nodes / profile**（`IVR_NODE_MAX=128`），choices 支持 SUBMENU 跳转 | ✅ 已满足 |
| 前缀累积多键 | `match_choice()` 支持精确 + 前缀匹配，累积 buf | ✅ 已满足 |
| 超时控制 | 首位 `initial_timeout_s` + 位间 `interdigit_timeout_s` + `max_retries` | ✅ 已满足 |
| 动作类型 | SUBMENU / TRANSFER / PLAY / VOICEMAIL / HTTP_QUERY / HANGUP | ✅ 已满足 |
| Web 管理 | `ivr_menus` 表 + `nodes_json` 完整菜单树 + reload sync | ✅ 已满足 |

### A2. 需新增能力（P0，Phase 2）

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

## Part B — Billing 增强需求（Phase 1 + 1.5 + 1.6 全部完成）

### B1. 现状 vs 目标（更新至 Phase 1.6 完成）

| 能力 | Phase 1 前 | Phase 1 完成 | Phase 1.5 完成 | Phase 1.6 完成 |
|------|-----------|-------------|---------------|---------------|
| 费率粒度 | per 前缀唯一价格 | ✅ per 前缀 × per 中继 × per 时段 × 阶梯 | ✅ trunk_name→id 映射 | ✅ 完整闭环 |
| 计费方式 | 单一 bill_inc_s + min_bill_s | ✅ 阶梯计费（前 N 分钟 X 元/分）+ 不同块 | ✅ pre-auth 入口拦截 | ✅ 完整闭环 |
| 客户余额 | 内存 acct_t[] | ✅ DB 持久化 + 实时 sync + Web 充值 API | ✅ reserve 绑定 call_id 精确匹配 | ✅ 完整闭环 |
| 透支控制 | credit_limit 全局字段 | ✅ per 客户可配置 | ✅ pre-auth 查 credit_limit | L5 OVERDRAWN kill（Phase 2）|
| 退款/流水 | **无** | ⚠️ billing_transactions 表建了但只有 settle_billing 会写 | ⚠️ 还是只有 settle_billing 会写 | ✅ **G1-G4 全部补完**：initial/topup/call_charge/refund 四种 tx_type 全有写入路径 |

### B2. 数据库 Schema（Phase 1 已建表）

```sql
-- ============ rate_plans（费率方案模板）— Phase 1 ✅ ============
CREATE TABLE rate_plans (
    id              BIGSERIAL PRIMARY KEY,
    tenant_id       BIGINT NOT NULL DEFAULT 1,
    name            VARCHAR(64) NOT NULL,
    description     TEXT,
    enabled         BOOLEAN NOT NULL DEFAULT TRUE,
    UNIQUE(tenant_id, name)
);

-- ============ rate_plan_entries（阶梯 + 时段条目）— Phase 1 ✅ ============
CREATE TABLE rate_plan_entries (
    id              BIGSERIAL PRIMARY KEY,
    plan_id         BIGINT NOT NULL REFERENCES rate_plans(id),
    prefix          VARCHAR(24) NOT NULL,
    trunk_id        BIGINT REFERENCES trunks(id),    -- NULL = 所有中继
    enabled         BOOLEAN NOT NULL DEFAULT TRUE,
    time_from       TIME,                             -- "09:00:00"
    time_to         TIME,                             -- "18:00:00"
    weekdays        VARCHAR(16),                      -- "1,2,3,4,5" 或 NULL=每天
    tier_min_sec    BIGINT NOT NULL DEFAULT 0,        -- 0 = 第一阶梯
    tier_max_sec    BIGINT,                           -- NULL = 无上限
    price_per_min_uc BIGINT NOT NULL,                 -- 微元/分钟
    bill_inc_s      INT NOT NULL DEFAULT 60,          -- 6=6秒计费, 60=分钟
    min_bill_s      INT NOT NULL DEFAULT 60,           -- 最小收费秒数
    connect_fee_uc  BIGINT NOT NULL DEFAULT 0,         -- 接通费
    precedence      INT NOT NULL DEFAULT 0
);

-- ============ trunk_rate_overrides（中继费率覆盖）— Phase 1 ✅ ============
CREATE TABLE trunk_rate_overrides (
    id              BIGSERIAL PRIMARY KEY,
    trunk_id        BIGINT NOT NULL REFERENCES trunks(id),
    prefix          VARCHAR(24) NOT NULL,
    price_per_min_uc BIGINT NOT NULL,
    bill_inc_s      INT NOT NULL DEFAULT 60,
    UNIQUE(trunk_id, prefix)
);

-- ============ billing_transactions（充值/扣费/退款流水）— Phase 1 ✅ 建表，Phase 1.6 补全写入 ============
CREATE TABLE billing_transactions (
    id              BIGSERIAL PRIMARY KEY,
    account_id      BIGINT NOT NULL REFERENCES accounts(id),
    call_id         VARCHAR(127),
    tx_type         VARCHAR(16) NOT NULL,
                   -- "initial"     ✅ Phase 1.6 G4: 开户开账（balance_uc>0 时）
                   -- "topup"       ✅ Phase 1.6 G2: 充值
                   -- "call_charge" ✅ Phase 1 + Phase 1.6 G1: settle_billing 内部写 + C→Rust 自动落库
                   -- "refund"      ✅ Phase 1.6 G3: 退款
                   -- "credit_adjust" / "manual" — Phase 1 已有
    amount_uc       BIGINT NOT NULL,                -- 正=充值/退款, 负=扣费
    balance_after_uc BIGINT NOT NULL,                -- 交易后余额（快照，用于对账）
    operator        VARCHAR(64),
    remark          TEXT,
    created_at      TEXT NOT NULL
);

CREATE INDEX idx_bt_account ON billing_transactions(account_id);
CREATE INDEX idx_bt_call    ON billing_transactions(call_id);
CREATE INDEX idx_bt_created ON billing_transactions(created_at);
```

**注意**：原需求文档用的表名是 `customer_accounts`，Phase 1 实现时复用了已有 `accounts` 表 + 新增 4 列（tenant_id, account_type, rate_plan_id, low_threshold_uc）。三后端 DDL 在 [web/src/db/mod.rs](file:///d:/github/nsbc/web/src/db/mod.rs) 里统一管理。

### B3. 阶梯计费算法（Phase 1 ✅ 已实现，Phase 1.5 ✅ 修正了 reserve 签名 bug）

> **实现位置**: [rating.c L791](file:///d:/github/nsbc/src/mod/billing/rating.c#L791) — `calc_stair_charge()`

```c
// Phase 1 calc_stair_charge — [rating.c L791+](file:///d:/github/nsbc/src/mod/billing/rating.c#L791)
// 一条 call 180 秒 (3分钟) 打 010xxxxxxxx，走 china_unicom 中继（trunk_id=1）
// rate_plan_entries 匹配到 3 条：
//   prefix="010", trunk=1, tier [0, 180): price=6000 微元/分, min_bill=180, bill_inc=60
//   prefix="010", trunk=1, tier [180, 3600): price=3000 微元/分, bill_inc=6
//   prefix="010", trunk=1, tier [3600, ∞): price=1500 微元/分

int64_t calc_stair_charge(rate_entry_t *tiers, int n_tiers, uint32_t billsec) {
    int64_t total = 0;
    uint32_t remaining = billsec;
    int64_t connect_fee = billsec > 0 ? tiers[0].connect_fee_uc : 0;

    for (int i = 0; i < n_tiers && remaining > 0; ++i) {
        rate_entry_t *t = tiers[i];
        uint32_t tier_end = t->tier_max_sec > 0 ? (uint32_t)t->tier_max_sec : 0xFFFFFFFFu;
        uint32_t span = tier_end - (uint32_t)t->tier_min_sec;
        /* 单费率兼容: tier_min=0 && tier_max=0 → 全覆盖 */
        if (t->tier_min_sec == 0 && t->tier_max_sec == 0) span = 0xFFFFFFFFu;

        uint32_t this_tier = (remaining < span) ? remaining : span;
        if (i == 0 && this_tier < (uint32_t)t->min_bill_s && billsec > 0)
            this_tier = (uint32_t)t->min_bill_s;
        if (t->bill_inc_s > 1 && this_tier % (uint32_t)t->bill_inc_s != 0)
            this_tier = (this_tier / (uint32_t)t->bill_inc_s + 1) * (uint32_t)t->bill_inc_s;

        total += t->price_per_min_uc * (int64_t)this_tier / 60;  // 先乘后除防溢出
        remaining -= this_tier;
    }
    return total + connect_fee;
}
```

### B4. 不同计费场景映射（Phase 1 ✅ 全支持）

| 场景 | 配置 | 状态 |
|------|------|------|
| **按分钟计费** | `bill_inc_s=60, min_bill_s=60` | ✅ |
| **按 6 秒计费** | `bill_inc_s=6, min_bill_s=6` | ✅ |
| **前 3 分钟 0.3 元/分，之后 0.1 元/分** | tier1: `tier_min=0, tier_max=180, price=30000, min_bill=180`; tier2: `tier_min=180, price=10000, bill_inc=6` | ✅ |
| **接通费 + 按分钟** | `connect_fee_uc=50000 (5分), price_per_min_uc=150000 (1.5元/分)` | ✅ |
| **工作日白天 0.2 元/分，夜间/周末 0.1 元/分** | 两条：`time_from=09:00, time_to=18:00, weekdays=1-5, price=200000`；`time_from=18:00, weekdays="", price=100000` | ✅ |
| **同一个 010，走 unicom 比 telecom 便宜** | Phase 1.5 L1: 推 `trunk_map.reload [{"name":"china_unicom","id":1},{"name":"telecom","id":2}]`；rate_plan_entries 配 `trunk_id=1 vs trunk_id=2` | ✅ |

### B5. C 侧内存结构（Phase 1.5 更新）

> **定义位置**: [rating.c](file:///d:/github/nsbc/src/mod/billing/rating.c) · 头文件 [rating.h](file:///d:/github/nsbc/src/include/rating.h)

```c
// src/mod/billing/rating.c — Phase 1.5 完整内存结构

struct billing {
    pthread_rwlock_t lock;
    rate_entry_t rates[4096];       /* 展平的费率条目（rate_tables + rate_plan_entries upsert 到同一个数组）*/
    int          n_rates;
    acct_t       accts[200000];
    resv_t       resv[4096];
    uint64_t     next_resv;

    /* Phase 1.5 L1: trunk name → id 映射 */
    trunk_map_entry_t trunk_map[BILL_TRUNK_MAP_MAX];   // 定义在 [rating.c L60](file:///d:/github/nsbc/src/mod/billing/rating.c#L60)
};

/* Phase 1.5 L3: resv_t 加 call_id — 同账户多通话并发不会错乱
 * 定义在 [rating.c L34](file:///d:/github/nsbc/src/mod/billing/rating.c#L34) */
typedef struct {
    int      used;
    uint64_t id;
    size_t   ai;              /* acct index into b->accts[] */
    int64_t  amount;
    char     call_id[40];     /* SIP Call-ID 精确匹配 */
} resv_t;

/* rate_lookup_stair — Phase 1 实现
 * 输入: called + trunk_id（Phase 1.5 从 billing_lookup_trunk_id 来，[rating.c L947](file:///d:/github/nsbc/src/mod/billing/rating.c#L947)）+ now_hhmm
 * 输出: 匹配到的所有 tier（排序后）
 * 实现在 [rating.c L721](file:///d:/github/nsbc/src/mod/billing/rating.c#L721) */
int rate_lookup_stair(billing_t *b,
                      const char *called,
                      int64_t    trunk_id,
                      const char *now_hhmm, const char *weekdays,
                      rate_entry_t *tiers_out, int *n_tiers_out,
                      int max_tiers);
```

### B6. 完整呼叫计费时序（Phase 1.5 更新）

```
INVITE ──► b2bua.c
  │
  ├── security_check_fence ✅
  ├── ★ Phase 1.5 L2: billing_pre_auth(caller, callee)
  │   ├── caller 是短 extension → 推 acct_key = "tenant:1:extension:1000"
  │   ├── rate_lookup_stair(callee, trunk_id=0, now_hhmm) → tiers[]
  │   ├── reserve = calc_stair(tiers, max(min_bill, 120)) × 2 + connect_fee
  │   └── balance + credit_limit - reserved >= reserve ?
  │       ├── ✅ → 放行
  │       └── ❌ → PERM_DENIED（静默丢）
  │
  ├── b2b_call_t 分配 → 拨号规则 → 180 Ringing → 200 OK (ANSWER)
  │
  ▼
ANSWER ──► billing_cdr_cb()
  │
  ├── ★ Phase 1.5 L1: trunk_id = billing_lookup_trunk_id(ev->trunk)  // "china_unicom" → 1
  ├── rate_lookup_stair(ev->to, trunk_id, now_hhmm) → tiers[]
  ├── reserve = calc_stair_charge(tiers, reserve_sec) × 2 + connect_fee
  ├── ★ Phase 1.5 L3: bill_reserve(b, acct_key, reserve, ev->call_id, &resv_id)  // 绑定 call_id
  └── log_info("ANSWER reserved=%lld ...")
  │
  ▼
BYE (END) ──► billing_cdr_cb()
  │
  ├── trunk_id = billing_lookup_trunk_id(ev->trunk)
  ├── rate_lookup_stair(ev->to, trunk_id, now_hhmm) → tiers[]
  ├── charge = calc_stair_charge(tiers, billsec)
  ├── ★ Phase 1.5 L3: bill_settle(b, acct_key, ev->call_id, charge)  // 精确匹配 reservation
  │   └── resv_find_call_id(key, call_id) → 唯一 reservation
  │   └── a->reserved -= resv.amount; a->balance -= charge  // 多退少补
  ├── if bal < 0: log_warn("OVERDRAWN ...")
  └── ✅ Phase 1.6 G1: uds_push("billing_settle", json)
      └── Rust uds_server dispatch_push → get_account_by_key → settle_billing() DB
          └── sqlx::Transaction: UPDATE accounts balance_uc -= charge
                               + INSERT billing_transactions (tx_type=call_charge, amount_uc=-charge)
```

---

## Part C — 实施进度（更新）

### Phase 1 — Billing 基础设施（✅ 全部完成，Phase 1.5 已补遗留）

| ID | 任务 | 状态 | 说明 |
|----|------|------|------|
| B-DB-1 | accounts / rate_plans / rate_plan_entries / billing_transactions 建表 | ✅ | accounts 复用已有表 + 4 列升级 |
| B-DB-2 | trunk_rate_overrides 建表 | ✅ | |
| B-SYNC-1 | Web → nsbcd rate_plans / rate_plan_entries JSON push | ✅ | sync.rs Kind 枚举 + debounce |
| B-SYNC-2 | Web → nsbcd accounts JSON push（扩展 8 字段）| ✅ | accounts.reload UDS |
| B-CORE-1 | C 侧 billing_t 扩展：rates[] + rate_lookup_stair() | ✅ | |
| B-CORE-2 | 阶梯计费 calc_stair_charge() 实现 | ✅ | |
| B-CORE-3 | reserve/settle 接入 rate_lookup_stair | ✅ | Phase 1.5 补了 call_id 精确匹配 |
| B-CORE-4 | trunk_rate_overrides 中继维度覆盖 | ✅ | Phase 1.5 L1 billing_lookup_trunk_id |
| B-WEB-1 | Web 管理界面：费率方案编辑、阶梯可视化 | ✅ | |
| B-WEB-2 | 交易流水查询 API | ✅ | 写入 Phase 1.6 已补（G1-G4 四种 tx_type 全覆盖）|
| B-OAM-1 | OAM 命令：show rates / show balances / accounts.reload / rates.reload | ✅ | Phase 1.5 加 trunk_map.reload |

### Phase 1.6 — 资金闭环（✅ 全部完成）

| ID | 任务 | 复杂度 | 代码位置 |
|----|------|--------|---------|
| **G1** | **END settle 自动写 DB billing_transactions** | M | [billing.c L200-L220](file:///d:/github/nsbc/src/mod/billing/billing.c#L200-L220) uds_push("billing_settle", json) → [uds_server.rs L287-L306](file:///d:/github/nsbc/web/src/uds_server.rs#L287-L306) dispatch_push handler → settle_billing() |
| **G2** | **topup_account 写 billing_transactions** | S | [db/mod.rs topup_account L8302](file:///d:/github/nsbc/web/src/db/mod.rs#L8302) sqlx::Transaction + tx_type="topup" |
| **G3** | **退款接口 refund_account + /balances/:id/refund** | M | [db/mod.rs refund_account L8373](file:///d:/github/nsbc/web/src/db/mod.rs#L8373) + [billing.rs refund route L65-67 + handler L379-L424](file:///d:/github/nsbc/web/src/routes/billing.rs#L65) |
| **G4** | **open_account 写 initial 流水** | XS | [db/mod.rs open_account L8248](file:///d:/github/nsbc/web/src/db/mod.rs#L8248) sqlx::Transaction + balance_uc>0 时写 tx_type="initial" |

### Phase 2 — Billing 生产必备（✅ 全部完成 + 审计修复）

| ID | 任务 | 状态 | 说明 |
|----|------|------|------|
| P0 | `billing.enabled` 全局开关（YAML + CGF_HOT 热更新）| ✅ | cfg_schema.c 注册 + 所有计费入口 guard |
| L4 | 实时扣费 tick → **L4v2 精确费率追加 + L4v3 收缩 + L4v4 批量维护** | ✅ | 3s 周期，预分配 scratch buffer 零 malloc |
| L5 | OVERDRAWN 强制 kill 同账户所有通话 | ✅ | 边沿检测 + 滞回防抖动 |
| OAM | show resv 命令 | ✅ | obs_control.c + bill_foreach_reservation |
| Web | GET/PUT /enabled + GET /reservations/core | ✅ | config_get/config_set + UDS show resv |
| 前端 | Billing 管理页面 | ✅ | 启停开关 + Reservation 实时表格 |
| 审计 P0 | alloca 栈溢出 + LOG_INFO 热路径 | ✅ | 预分配 scratch + 三条降级 LOG_DEBUG |
| 审计 P1 | tick strlen(called) | ✅ | called_len 预存，tick 零 strlen |

### Phase 2 IVR 增强（未启动）

| ID | 任务 | 复杂度 | 依赖 |
|----|------|--------|------|
| IVR-1 | `collect` 节点类型 + DTMF 收集 + store_as 存入 call_vars | M | 无 |
| IVR-2 | `cond` 条件分支节点 | L | IVR-1 |
| IVR-3 | ivr_sess 加 call_vars 字典 | S | IVR-1 |
| IVR-4 | 时段路由（schedule 字段） | S | 无 |
| IVR-5 | 扩展 ivr_node_t 结构 + JSON 解析 | M | IVR-1 |
| IVR-6 | Web 节点编辑器：collect / cond 可视化 | L | IVR-5 |

### Phase 3 — 高级特性（P1，按需）

| ID | 任务 | 复杂度 |
|----|------|--------|
| IVR-P1-1 | TTS prompt（HTTP 回调合成语音） | M |
| IVR-P1-2 | 录音留言（IVR 内 record action） | M |
| IVR-P1-3 | 多语言菜单（按 caller locale 选 prompt 文件）| S |
| B-P1-1 | 周期性扣费（包月套餐 / 60s 一次 tick）| L |
| B-P1-2 | 费率模板市场（预设方案一键应用）| M |
| B-P1-3 | 反欺诈：高额通话实时告警 + 自动拦截 | L |

---

## Part D — 关键设计决策

### D1. 金额单位：微元（1e-6 元）int64 — Phase 1 ✅

**理由**：避免浮点精度问题。1 元 = 1,000,000 微元，int64 范围 ±9.2×10¹² ≈ ±9.2 万亿元，足够覆盖任何客户余额。

**保持一致**：所有字段（price_per_min_uc / connect_fee_uc / balance_uc / reserve / charge）统一用 uc 后缀 + int64。

### D2. 费率匹配优先级 — Phase 1 ✅ 实现，Phase 1.5 ✅ trunk_map 补全

```
1. trunk_rate_overrides[trunk_id + prefix]          ← DB 层已展平
2. rate_plan_entries[account.rate_plan_id + trunk_id + prefix + time]
3. rate_plan_entries[account.rate_plan_id + trunk_id + prefix]
4. rate_plan_entries[account.rate_plan_id + prefix + time]
5. rate_plan_entries[account.rate_plan_id + prefix]
6. rate_plan_entries[account.rate_plan_id + "" + time]   ← 默认
7. rate_plan_entries[account.rate_plan_id + ""]          ← 兜底
8. rate_tables[prefix]                              ← 旧表兜底
```

**Phase 1.5 trunk_id 来源**：billing_lookup_trunk_id(ev->trunk) 做 name→id 映射，没推 trunk_map 时返回 0（不限兜底）。

每条按 **长前缀优先** + **trunk 精确优先** + **precedence 大优先** + **tier_min_sec 升序** 展开。

### D3. reserve 保守策略 — Phase 1 ✅，Phase 1.5 ✅ call_id 精确 settle

```
reserve_sec = max(tiers[0].min_bill_s, 120)   // 至少 2 分钟
reserve     = calc_stair_charge(tiers, reserve_sec) × 2 + connect_fee
```

保守原因：长通话跨过阶梯，费用会超过 reserve × 2 兜底。

**Phase 1.5 修正**：Phase 1.5 之前 bill_settle 扫账户下所有 reservation（根因：resv_t 没 call_id）。现在 resv_t 加 call_id，bill_reserve 绑定，bill_settle 按 call_id 精确匹配 —— 同账户多通话并发不会互相多退少补。

### D4. 多租户隔离 — Phase 1 ✅

- 所有表都有 `tenant_id` 列
- `rate_plan_entries` 通过 `rate_plan_id` → `rate_plans.tenant_id` 关联
- C 侧 `account_key` 格式 `"tenant:<tid>:extension:<ext>"` —— tenant 在前，防止跨租户串账

### D5. CDR 与计费解耦 — Phase 1 ⚠️ 部分实现，Phase 1.6 ✅ G1 补完

原始需求：CDR 表加 3 列（charge_uc / rate_plan_id / billing_status）+ 计费模块独立写 billing_transactions。

**现状**：
- ✅ billing_transactions 表存在（Phase 1）
- ✅ settle_billing() DB 方法用 `sqlx::Transaction` 包着 UPDATE + INSERT（Phase 1）
- ✅ Phase 1.6 G1 已补：C 侧 END settle 成功后 `uds_push("billing_settle", json)` → Rust dispatch_push handler → `get_account_by_key` + `settle_billing()` 自动落库
- ❌ CDR 表没加 charge_uc / rate_plan_id / billing_status 3 列（P2 后续）

---

## Appendix — vs 竞品对比（更新）

| 能力 | FreeSWITCH | Asterisk | OpenSIPS | NSBC Phase 1.5 | NSBC Phase 1.6 |
|------|-----------|----------|----------|---------------|----------------|
| 多层 IVR | ✅ dialplan + Lua | ✅ dialplan + AGI | ❌ 无内建 | ✅ 6 种 action + 128 节点 | ✅ |
| 阶梯计费 | ❌ 用外部系统 | ⚠️ 用 cgrates | ❌ | ✅ 内置 stair 算法 | ✅ |
| 时段费率 | ❌ dialplan case | ⚠️ Time-based routing | ❌ | ✅ time_from/to + weekdays | ✅ |
| 中继维度费率 | ❌ | ⚠️ 用 cgrates | ❌ | ✅ trunk_rate_overrides + trunk_map | ✅ |
| 6 秒计费块 | ⚠️ 外部 | ⚠️ | ❌ | ✅ 6s + 阶梯组合 | ✅ |
| **INVITE pre-auth** | ❌ | ⚠️ 用 cgrates | ❌ | ✅ **Phase 1.5 实现** | ✅ |
| **settle 精确匹配** | ❌ | ⚠️ 用 cgrates | ❌ | ✅ **Phase 1.5 修复**（call_id 绑定）| ✅ |
| 客户充值 | ❌ | ❌ | ❌ | ⚠️ DB + API 但**不写流水** | ✅ **G2 已补** |
| **退款** | ❌ | ❌ | ❌ | ❌ **完全缺失** | ✅ **G3 已补** |
| **通话扣费自动落账** | ⚠️ 外部 | ⚠️ 外部 | ❌ | ❌ | ✅ **G1 已补** |
| 热 reload | ✅ `reloadxml` | ❌ 重启 | ✅ `reload` | ✅ 全部配置热 reload | ✅ |
| 透支控制 | ❌ | ❌ | ❌ | ✅ per 客户 + pre-auth 拦截 | ✅ + L5 OVERDRAWN kill（Phase 2）|

**差异化**：NSBC Phase 1.6 已实现完整计费闭环——阶梯算法 + reserve/settle CDR hook + Web 管理 + pre-auth 拦截 + 资金闭环四条 Transaction 原子路径（settle_billing / topup / refund / open_account）。Phase 2 推进实时扣费 tick 和 IVR 增强。

---

## Changelog

| 日期 | 版本 | 变更 |
|------|------|------|
| 2026-10-31 | Draft | 初始需求梳理，Phase 1 未开始 |
| 2026-11-01 | Phase 1 + 1.5 对齐 | Part B 现状 vs 目标表更新（Phase 1 前 → Phase 1 → Phase 1.5）；B3 代码示例对齐实现；B5 内存结构加 trunk_map + resv_t call_id；B6 时序图补 pre-auth + trunk_id + call_id 绑定；Part C 实施进度更新（Phase 1 ✅ / Phase 1.6 🔴 阻塞 / Phase 2 未启动）；Part D D3 补 Phase 1.5 修正；Part D D5 标注 G1 缺口；Appendix 更新竞品对比；新增资金闭环 G1-G4 缺口描述 |
| 2026-11-02 | Phase 1.6 资金闭环完成 | 更新时间戳；头部状态从"Phase 1.6 待补"改为"全部完成"；Part B B1 现状 vs 目标表 Phase 1.6 列从"待补"改为"完成"；Part B B2 billing_transactions tx_type 注释全部打 ✅；Part B B6 时序图 G1 缺口改成已补；Part C Phase 1.6 表格从 🔴 阻塞改为 ✅ 已完成 + 代码链接；Part D D5 CDR 解耦标注 G1 已补；竞品对比表 Phase 1.6 列全部 ✅ |
| 2026-11-02 | Phase 2 全部完成 + 审计修复 | 头部状态加 Phase 2；Part C Phase 2 表格拆分 Billing（✅ 完成）和 IVR（未启动）；新增 billing.enabled 全局开关、L4v2 精确 tick 追加、L4v3 reserve 收缩、L4v4 批量维护（预分配 scratch buffer）、L5 OVERDRAWN kill、OAM show resv、Web 3 新 API、前端 Billing 管理页面；审计修复 P0 alloca 栈溢出 + P0 LOG_INFO 热路径 + P1 strlen 热路径 |
