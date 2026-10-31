# Billing 模块当前状态（Phase 1 完成）

**更新时间**: 2026-10-31
**Commits**: `bfd4696` · `1820270` · `3aa0fb8` · `3aff3b6`
**Web cargo check**: ✅ 零错误
**C core Linux 编译**: 未验证（Phase 1.5）

---

## 1. 三层架构总览

```
┌──────────────────────────────────────────────────────────┐
│ Web DB (Rust sqlx — sqlite / postgres / mysql)            │
│                                                           │
│ rate_plans / rate_plan_entries / trunk_rate_overrides /   │
│ billing_transactions                                      │
│ accounts (升级 +4 列) / rate_tables (升级 +2 列)          │
│                                                           │
│ HTTP:  /api/v1/billing/*                                  │
│ Sync:  sync.rs Kind::RatePlans / RatePlanEntries          │
│ UDS:   reload_rate_plans / reload_rate_plan_entries       │
└────────────────────┬─────────────────────────────────────┘
                     │
                     ▼
┌──────────────────────────────────────────────────────────┐
│ OAM (obs_control.c — UDS 控制命令)                        │
│                                                           │
│ rate.reload          ← 旧 rate_tables JSON                │
│ rate_plans.reload    ← 新 rate_plans JSON                 │
│ rate_plan_entries.reload ← 新 rate_plan_entries JSON      │
│ accounts.reload      ← 扩展解析 8 字段                    │
│ show rates           ← 输出 tier/trunk/time/prec 全字段   │
│ show balances        ← 账户列表                           │
│ account.open / account.topup                              │
└────────────────────┬─────────────────────────────────────┘
                     │
                     ▼
┌──────────────────────────────────────────────────────────┐
│ C Core (rating.c + billing.c)                             │
│                                                           │
│ billing_t { rates[4096] rate_entry_t; accts[200000];       │
│            resv[4096] resv_t; }                           │
│                                                           │
│ rate_entry_t (24 fields, backward compat)                 │
│   prefix, name, price_per_min_uc, connect_fee_uc,         │
│   bill_inc_s, min_bill_s, initial_s, enabled,             │
│   tier_min_sec, tier_max_sec,  ← Phase 1 新增阶梯         │
│   trunk_id, rate_plan_id,      ← Phase 1 新增维度          │
│   time_from, time_to, weekdays, precedence                │
│                                                           │
│ acct_t (10 fields, backward compat)                        │
│   used, key, balance, reserved, credit_limit,             │
│   tenant_id, account_type, rate_plan_id, low_threshold_uc  │
│                                                           │
│ API: rate_lookup_stair(), calc_stair_charge(),            │
│      bill_reserve(), bill_settle(), bill_release(),       │
│      account_open(), account_reload_json()                 │
└────────────────────┬─────────────────────────────────────┘
                     │
                     ▼
┌──────────────────────────────────────────────────────────┐
│ b2b_cdr_hook_add — billing_cdr_cb() 自动接线               │
│                                                           │
│ CDR_PHASE_ANSWER → rate_lookup_stair → reserve × 2 + fee  │
│ CDR_PHASE_END    → rate_lookup_stair → calc_stair → settle│
└──────────────────────────────────────────────────────────┘
```

---

## 2. Web DB Schema（三后端 DDL 全量）

### 2.1 新表

```sql
-- rate_plans: 费率方案模板
CREATE TABLE rate_plans (
    id              BIGSERIAL PRIMARY KEY,
    tenant_id       BIGINT NOT NULL DEFAULT 1,
    name            VARCHAR(64) NOT NULL,
    description     TEXT,
    enabled         INT NOT NULL DEFAULT 1,
    UNIQUE(tenant_id, name)
);

-- rate_plan_entries: 阶梯条目（一 prefix 可多条，按 tier_min_sec 升序）
CREATE TABLE rate_plan_entries (
    id              BIGSERIAL PRIMARY KEY,
    plan_id         BIGINT NOT NULL REFERENCES rate_plans(id),
    prefix          VARCHAR(24) NOT NULL,
    trunk_id        BIGINT NULL REFERENCES trunks(id),
    enabled         INT NOT NULL DEFAULT 1,
    time_from       TEXT NULL,     -- "HH:MM"
    time_to         TEXT NULL,
    weekdays        VARCHAR(16),   -- "1,2,3,4,5" 或 NULL=每天
    tier_min_sec    BIGINT NOT NULL DEFAULT 0,
    tier_max_sec    BIGINT NULL,   -- NULL = 无上限
    price_per_min_uc BIGINT NOT NULL,
    bill_inc_s      INT NOT NULL DEFAULT 60,
    min_bill_s      INT NOT NULL DEFAULT 60,
    connect_fee_uc   BIGINT NOT NULL DEFAULT 0,
    precedence      INT NOT NULL DEFAULT 0,
    INDEX idx_rpe_plan_prefix (plan_id, prefix)
);

-- trunk_rate_overrides: 中继维度特殊费率
CREATE TABLE trunk_rate_overrides (
    id              BIGSERIAL PRIMARY KEY,
    trunk_id        BIGINT NOT NULL REFERENCES trunks(id),
    prefix          VARCHAR(24) NOT NULL,
    price_per_min_uc BIGINT NOT NULL,
    bill_inc_s      INT NOT NULL DEFAULT 60,
    UNIQUE(trunk_id, prefix)
);

-- billing_transactions: 充值/扣费流水
CREATE TABLE billing_transactions (
    id              BIGSERIAL PRIMARY KEY,
    account_id      BIGINT NOT NULL REFERENCES accounts(id),
    call_id         VARCHAR(127),
    tx_type         VARCHAR(16) NOT NULL,  -- "topup" / "call_charge" / "credit_adjust" / "manual"
    amount_uc       BIGINT NOT NULL,       -- 正=充值, 负=扣费
    balance_after_uc BIGINT NOT NULL,
    operator        VARCHAR(64),
    remark          TEXT,
    created_at      TEXT NOT NULL,
    INDEX idx_bt_account (account_id),
    INDEX idx_bt_call (call_id),
    INDEX idx_bt_created (created_at)
);
```

### 2.2 升级迁移（幂等）

```sql
-- accounts: +4 列
ALTER TABLE accounts ADD COLUMN IF NOT EXISTS tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE accounts ADD COLUMN IF NOT EXISTS account_type VARCHAR(16) NOT NULL DEFAULT 'extension';
ALTER TABLE accounts ADD COLUMN IF NOT EXISTS rate_plan_id BIGINT NULL;
ALTER TABLE accounts ADD COLUMN IF NOT EXISTS low_threshold_uc BIGINT NOT NULL DEFAULT 0;

-- rate_tables: +2 列
ALTER TABLE rate_tables ADD COLUMN IF NOT EXISTS tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE rate_tables ADD COLUMN IF NOT EXISTS rate_plan_id BIGINT NULL;
```

SQLite 用 PRAGMA table_info + 条件 ALTER（逐列检查是否存在），PostgreSQL 用 `ADD COLUMN IF NOT EXISTS`，MySQL 用 `information_schema.COLUMNS` 检查后 ALTER。

### 2.3 Rust Structs

```rust
// web/src/db/mod.rs

pub struct RatePlan {
    pub id: i64, pub tenant_id: i64, pub name: String,
    pub description: String, pub enabled: i64,
    pub created_at: String, pub updated_at: String,
}

pub struct RatePlanEntry {
    pub id: i64, pub plan_id: i64, pub prefix: String,
    pub trunk_id: Option<i64>, pub enabled: i64,
    pub time_from: Option<String>, pub time_to: Option<String>,
    pub weekdays: Option<String>,
    pub tier_min_sec: i64, pub tier_max_sec: Option<i64>,
    pub price_per_min_uc: i64, pub bill_inc_s: i64,
    pub min_bill_s: i64, pub connect_fee_uc: i64, pub precedence: i64,
}

pub struct BillingTransaction {
    pub id: i64, pub account_id: i64, pub call_id: Option<String>,
    pub tx_type: String, pub amount_uc: i64, pub balance_after_uc: i64,
    pub operator: Option<String>, pub remark: Option<String>, pub created_at: String,
}

// Account 已扩展: + tenant_id / account_type / rate_plan_id / low_threshold_uc
// AccountCreate 已扩展: 同名字段 + serde(default)
```

### 2.4 Db CRUD 方法

| 表 | list | get | create | update | delete | 特殊 |
|---|---|---|---|---|---|---|
| rate_plans | ✅ + enabled/tenant filter | ✅ | ✅ | ✅ | ✅ + entries 前置检查 | — |
| rate_plan_entries | ✅ + plan_id filter | ✅ | ✅ | — | ✅ | — |
| trunk_rate_overrides | ✅ | — | ✅ | — | ✅ | — |
| billing_transactions | ✅ + account_id/call_id/tx_type | — | — | — | — | **只读查询**（settle_billing 内部写入） |
| accounts | ✅ + keyword | ✅ | ✅ | ✅ | ✅ | account_topup / settle_billing |
| rate_tables | ✅ + prefix/enabled | ✅ | ✅ | ✅ | ✅ | 旧表，保持兼容 |

`account_topup(id, delta_uc)` 和 `settle_billing(account_id, call_id, charge_uc)` 包在 `sqlx::Transaction` 里，余额和流水要么一起成要么一起败。

---

## 3. Sync 层

```rust
// web/src/sync.rs — Kind 枚举扩展
pub enum Kind {
    // ... 旧 12 种 ...
    RatePlans,         // ← 新增
    RatePlanEntries,   // ← 新增
}

// ALL 数组: 14 种（旧 12 + 新 2）
// name() → "rate_plans" / "rate_plan_entries"

// snapshot() — 遍历 enabled 条目
Kind::RatePlans        → db.list_enabled_rate_plans()
Kind::RatePlanEntries  → db.list_enabled_rate_plan_entries()

// push_kind() — 调 UDS 命令
Kind::RatePlans        → core.reload_rate_plans(&json)      // "rate_plans.reload <json>"
Kind::RatePlanEntries  → core.reload_rate_plan_entries(&json)// "rate_plan_entries.reload <json>"
Kind::Rates            → core.reload_rates(&json)            // "rate.reload <json>"（旧）
Kind::Accounts         → core.reload_accounts(&json)         // "accounts.reload <json>"（已扩展新字段）
```

sync.rs 已经有 debounce（300ms）、退避重试（5 级）、周期性（60s）、HELLO 重连。新增的两个 Kind 自动享受同样的可靠性保障。

---

## 4. C Core 数据结构

### 4.1 rate_entry_t（24 fields）

```c
// src/include/rating.h
typedef struct {
    char     prefix[24];
    char     name[32];
    int64_t  price_per_min_uc;     // 微元/分钟
    int64_t  connect_fee_uc;       // 接通费
    int      bill_inc_s;           // 计费块（1/6/60秒）
    int      min_bill_s;           // 最小收费秒数
    int      initial_s;            // 首块秒数
    int      enabled;

    // Phase 1 新增 — 向后兼容：旧条目这些字段全零
    int64_t  tier_min_sec;         // 本阶梯起点（0=第一阶梯）
    int64_t  tier_max_sec;         // 本阶梯终点（0=无上限）
    int64_t  trunk_id;             // 中继维度（0=不限）
    int64_t  rate_plan_id;         // 关联 DB rate_plans.id（0=旧 rate_tables）
    char     time_from[8];         // "HH:MM"
    char     time_to[8];
    char     weekdays[16];         // "1,2,3,4,5" 或 ""
    int      precedence;           // 同 prefix + trunk + time 优先级
} rate_entry_t;
```

**向后兼容逻辑**：`tier_min_sec=0, tier_max_sec=0` → calc_stair_charge 把它当"全覆盖单费率"。旧 rate_tables reload JSON 不含这些字段 → jint 默认 0 → 行为完全不变。

### 4.2 acct_t（10 fields）

```c
// src/mod/billing/rating.c
typedef struct {
    int      used;
    char     key[128];
    int64_t  balance;
    int64_t  reserved;
    int64_t  credit_limit;

    // Phase 1 新增
    int64_t  tenant_id;
    char     account_type[16];     // "extension" / "trunk" / "tenant" / "reseller"
    int64_t  rate_plan_id;
    int64_t  low_threshold_uc;
} acct_t;
```

### 4.3 账户 Key 推导（billing_make_key）

```c
// billing_cdr_cb 内部自动推
switch (ev->dir) {
case CDR_DIR_OUTBOUND: // 内部分机 → 出中继
    snprintf(key, "tenant:%s:extension:%s", tenant, ev->from);   // 主叫付费
case CDR_DIR_INBOUND:  // 入中继 → 内部分机
    snprintf(key, "tenant:%s:trunk:%s", tenant, ev->trunk ?: ev->to); // 中继付费
case CDR_DIR_INTERNAL: // 分机互拨
    snprintf(key, "tenant:%s:extension:%s", tenant, ev->from);   // 主叫付费
}
```

---

## 5. C Core API（完整）

### 5.1 费率

| API | 功能 |
|-----|------|
| `rate_upsert(b, entry)` | upsert 到 rates[]（prefix 覆盖更新）|
| `rate_delete(b, prefix)` | 按前缀删除 |
| `rate_lookup(b, called, out)` | 单费率查找（旧，兼容）|
| `rate_quote(r, billsec, q)` | 单费率报价（旧，兼容）|
| **`rate_lookup_stair(b, called, trunk_id, now_hhmm, weekdays, tiers, &n, max)`** | **多维阶梯查找** — 前缀最长匹配 + trunk + 时段 + precedence 排序 |
| **`calc_stair_charge(tiers, n_tiers, billsec)`** | **阶梯计费** — 逐段 bill_inc_s 取整、min_bill 兜底、接通费一次性 |
| `rate_plan_entries_reload_json(b, json)` | 从 JSON 数组 upsert（新）|
| `rate_plans_reload_json(b, json)` | 从 JSON 数组 upsert plan 元数据（新，stub）|
| `time_slot_match(time_from, time_to, weekdays, hhmm, wd)` | 时段/星期匹配 |

### 5.2 账户

| API | 功能 |
|-----|------|
| `account_open(b, key, balance, credit)` | 旧 3 参数，内部填新字段默认值 |
| `account_balance(b, key, &bal, &resv)` | 查余额 + 预占 |
| `account_topup(b, key, delta)` | 充值（内部锁 + 更新 balance）|
| `account_reload_json(b, json)` | **重写** — 持 wrlock + 直接 upsert accts[] 所有 8 字段（避免嵌套锁）|
| `bill_reserve(b, key, amount, &resv_id)` | 预占（扣 reserved）|
| `bill_settle(b, key, actual_amount)` | 结算（多退少补 + 释放 reservation）|
| `bill_release(b, resv_id)` | 释放 |

### 5.3 内部锁

```c
static acct_t *acct_find_locked(b, key);   // 假设调用方已持 wrlock
static acct_t *acct_find(b, key);          // 内部自己加 rdlock → 调 locked 版本
```

---

## 6. 计费算法

### 6.1 多维匹配优先级

```
1. trunk_rate_overrides[trunk_id + prefix]          ← DB 层已展平
2. rate_plan_entries[account.rate_plan_id + trunk_id + prefix + time]
3. rate_plan_entries[account.rate_plan_id + trunk_id + prefix]
4. rate_plan_entries[account.rate_plan_id + prefix + time]
5. rate_plan_entries[account.rate_plan_id + prefix]
6. rate_plan_entries[account.rate_plan_id + "" + time]
7. rate_plan_entries[account.rate_plan_id + ""]
8. rate_tables[prefix]                              ← 旧表兜底
```

每条按 **长前缀优先** → **trunk 精确优先** → **precedence 大优先** → **tier_min_sec 升序** 过滤+排序。

### 6.2 阶梯计费

```c
int64_t calc_stair_charge(rate_entry_t *tiers, int n_tiers, uint32_t billsec) {
    total = 0
    remaining = billsec
    connect_fee = tiers[0].connect_fee_uc if billsec > 0 else 0

    for each tier:
        span = tier_max_sec - tier_min_sec   // tier_max=0 → 无上限
        this_tier = min(remaining, span)
        // 单费率兼容: tier_min=0 && tier_max=0 → span 覆盖全部
        if first_tier && this_tier < min_bill_s && billsec > 0:
            this_tier = min_bill_s            // 首段兜底
        if bill_inc_s > 1 && this_tier % bill_inc_s != 0:
            this_tier = ceil_align_up(this_tier, bill_inc_s)
        total += price_per_min_uc * this_tier / 60  // 先乘后除防溢出
        remaining -= this_tier

    return total + connect_fee
}
```

### 6.3 场景映射

| 场景 | 配置 |
|------|------|
| 按分钟计费 | bill_inc_s=60, min_bill_s=60 |
| 按 6 秒计费 | bill_inc_s=6, min_bill_s=6 |
| 前3分钟 0.3元/分，之后 0.1元/分 | tier1: tier_min=0, tier_max=180, price=30000, min_bill=180; tier2: tier_min=180, price=10000, bill_inc=6 |
| 接通费 + 按分钟 | connect_fee_uc=50000(5分), price_per_min_uc=150000(1.5元/分) |
| 工作日白天 0.2元/分，夜间/周末 0.1元/分 | 两条 entries: time_from=09:00, time_to=18:00, weekdays=1-5, price=200000; weekdays="", price=100000 |
| 同一 010 走 unicom 比 telecom 便宜 | trunk_id=unicom, price=100000 vs trunk_id=telecom, price=150000 |

### 6.4 Reserve 保守策略

```
reserve_sec = max(tiers[0].min_bill_s, 120)   // 至少 2 分钟
reserve     = calc_stair_charge(tiers, reserve_sec) × 2 + connect_fee
```

保守原因：长通话跨过阶梯，费用会超过 reserve × 2 兜底。

### 6.5 Settle 结算

```
charge    = calc_stair_charge(tiers, actual_billsec)
reserved  = resv[resv_id].amount

// 多退少补
balance += reserved - charge
// balance -= charge  → 等价于 balance += (reserved - charge)
// bill_reserve 时 balance -= reserved（已扣）
// bill_settle 时 balance += (reserved - charge)（先退 reserved，再扣 charge）
```

---

## 7. Billing CDR Hook（端到端接线）

```
billing_init() 内部:
    b2b_cdr_hook_add(billing_cdr_cb, g_sink)
    ctx->billing = g_billing

billing_cdr_cb(ud, ev):
    cdr_sink_emit(sink, ev)           // 始终先写 CDR（原有路径）

    if (g_billing == NULL) return      // billing 未初始化时 no-op

    acct_key = billing_make_key(ev)   // 推导账户 key

    switch (ev->phase):
    CDR_PHASE_ANSWER:
        rate_lookup_stair(g_billing, ev->to, 0, now_hhmm, NULL, tiers, &n, 8)
        reserve = calc_stair_charge(tiers, max(min_bill, 120)) × 2 + connect_fee
        bill_reserve(g_billing, acct_key, reserve, &resv_id)
        log_info("billing", "ANSWER %s reserved=%lld uc acct=%s", ...)

    CDR_PHASE_END:
        rate_lookup_stair(g_billing, ev->to, 0, now_hhmm, NULL, tiers, &n, 8)
        charge = calc_stair_charge(tiers, ev->billsec)
        bill_settle(g_billing, acct_key, charge)
        if (account_balance < 0)
            log_warn("billing", "END %s account OVERDRAWN ...")

    CDR_PHASE_START:  // 暂不做
```

---

## 8. HTTP API 完整清单

| Method | Path | 功能 |
|--------|------|------|
| GET/POST | `/api/v1/billing/rates` | 旧单费率表 |
| GET/PUT/DELETE | `/api/v1/billing/rates/:id` | 单费率条目 CRUD |
| POST | `/api/v1/billing/rates/reload` | 推 rate_tables 到 core |
| GET | `/api/v1/billing/rates/core` | core 实时快照 |
| **GET/POST** | **`/api/v1/billing/rate-plans`** | **费率方案（新）** |
| **GET/PUT/DELETE** | **`/api/v1/billing/rate-plans/:id`** | 方案 CRUD（删除前检查是否有 entries） |
| **POST** | **`/api/v1/billing/rate-plans/reload`** | 推方案元数据到 core |
| **GET/POST** | **`/api/v1/billing/rate-plan-entries`** | **阶梯/时段/中继条目（新）** |
| **GET/DELETE** | **`/api/v1/billing/rate-plan-entries/:id`** | 条目 CRUD |
| **POST** | **`/api/v1/billing/rate-plan-entries/reload`** | 推阶梯条目到 core |
| GET/POST | `/api/v1/billing/balances` | 账户（已扩展新字段） |
| PUT/DELETE | `/api/v1/billing/balances/:id` | 账户更新/关闭 |
| POST | `/api/v1/billing/balances/:id/topup` | 充值（DB + core 同步 + 安全网 fallback）|
| POST | `/api/v1/billing/balances/reload` | accounts.reload UDS |
| **GET** | **`/api/v1/billing/transactions`** | **交易流水查询（新，按 account_id/call_id/tx_type 过滤）** |

### 路由注册位置

`web/src/main.rs L184`: `.merge(routes::billing::router())`

### reload 安全网模式（所有 POST reload 命令通用）

```
DB 写入 → sync::spawn_push(st, Kind::Rates)  // 300ms debounce + 5 级退避
→ sync 线程: db.list_enabled_rates() → reload_rates(json) UDS
→ nsbc 忙时丢帧也不阻塞 API（60s periodic 兜底 + HELLO 重连自愈）
```

---

## 9. 向后兼容性

| 路径 | 兼容方式 |
|------|---------|
| **旧 rate_tables JSON** | rate_lookup_stair 匹配到（tier_min=0, tier_max=0 全覆盖）→ calc_stair_charge 走单费率逻辑 |
| **旧 accounts.reload JSON** | jstr/jint safe default（tenant_id=1, account_type="extension", rate_plan_id=0） |
| **旧 account.open 命令** | account_open 内部 memset + 填默认值 |
| **旧 rate.reload 命令** | 完全不变，和 rate_plan_entries.reload 共存（rate_lookup_stair 会一起匹配）|
| **旧 accounts 表数据** | ALTER TABLE ADD COLUMN IF NOT EXISTS + DEFAULT 值，旧数据不受影响 |
| **旧 rate_tables 表数据** | 同上 |

---

## 10. 关键数据流向图

```
用户/管理员
  │
  ├── curl POST /rate-plan-entries ─┐
  │                                  ▼
  ├── curl POST /balances ───────► nsbc-web (axum)
  │                                  │
  │                                  ▼
  │                              sqlx::Transaction
  │                                  │
  │                    ┌─────────────┼─────────────┐
  │                    ▼             ▼             ▼
  │              rate_plan_entries   accounts   billing_transactions
  │                    │             │             │
  │                    └──────┬──────┘             │
  │                           ▼                      │
  │              sync::spawn_push                     │
  │              (300ms debounce)                     │
  │                           │                      │
  │                           ▼                      │
  │                    UDS socket                     │
  │              ┌────────────┼────────────┐         │
  │              ▼            ▼            ▼         │
  │   rate_plans.reload  rate_plan_entries.reload  accounts.reload
  │                           │                      │
  │                           ▼                      │
  │                  billing.rates[] (upsert)         │
  │                  billing.accts[] (upsert)         │
  │                                                   │
  ▼                                                   ▼
nsbcd core                                     billing_transactions
  │                                            (DB 持久化，
  │  来一通 010xxxxxxxx 通话                       由 settle_billing
  │  ────────── INVITE ─────────────              Web API 调用)
  │  ────────── 200 OK (ANSWER) ──┐
  │                               │
  │                    billing_cdr_cb()
  │                               │
  │                    rate_lookup_stair(to, now)
  │                    → tiers[0..1]
  │                               │
  │                    calc_stair_charge(reserve_sec)
  │                    → reserve = xxx × 2 + fee
  │                               │
  │                    bill_reserve(acct_key, reserve)
  │                               │
  │  ────────── BYE (END) ─────────┐
  │                               │
  │                    billing_cdr_cb()
  │                               │
  │                    rate_lookup_stair(to, now)
  │                    calc_stair_charge(billsec=185)
  │                    → charge = tier1(0-180) + tier2(180-185)
  │                               │
  │                    bill_settle(acct_key, charge)
  │                    balance -= (charge - reserved)
  │                               │
  └───────────────────────────────┘
```
