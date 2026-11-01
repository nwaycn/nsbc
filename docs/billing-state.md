# Billing 模块当前状态（Phase 2 全部完成 + 审计修复）

**更新时间**: 2026-11-02
**状态**: Phase 1 + 1.5 + 1.6 资金闭环 + Phase 2 **全部完成**（含审计修复）
**Web cargo check**: ✅ 零错误
**C core Linux 编译**: ✅ 远端 make 通过（手工编译验证）
**关键文件**: [rating.h](file:///d:/github/nsbc/src/include/rating.h) · [rating.c](file:///d:/github/nsbc/src/mod/billing/rating.c) · [billing.c](file:///d:/github/nsbc/src/mod/billing/billing.c) · [b2bua.c](file:///d:/github/nsbc/src/sip/b2bua.c) · [obs_control.c](file:///d:/github/nsbc/src/oam/obs_control.c) · [uds_server.rs](file:///d:/github/nsbc/web/src/uds_server.rs) · [billing.rs](file:///d:/github/nsbc/web/src/routes/billing.rs) · [uDS_client.h](file:///d:/github/nsbc/src/include/uDS_client.h) · [uds_client.c](file:///d:/github/nsbc/src/oam/uds_client.c)

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
│                                                           │
│ ⚠️ 旧: topup/settle/open_account 不写 billing_transactions        │
│ ✅ Phase 1.6 已补: G1-G4 全部 Transaction 原子写入                 │
└────────────────────┬─────────────────────────────────────┘
                     │
                     ▼
┌──────────────────────────────────────────────────────────┐
│ OAM (obs_control.c — UDS 控制命令)                        │
│                                                           │
│ rate.reload          ← 旧 rate_tables JSON                │
│ rate_plans.reload    ← 新 rate_plans JSON                 │
│ rate_plan_entries.reload ← 新 rate_plan_entries JSON      │
│ trunk_map.reload     ← trunk name→id 映射（Phase 1.5 新增）│
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
│ billing_t { rates[4096]; accts[200000]; resv[4096];       │
│            trunk_map[64]; ← Phase 1.5                      │
│            enabled;  ← Phase 2 全局开关（CGF_HOT 热更新）   │
│            per_acct_delta; ← Phase 2 L4v4 预分配 scratch    │
│                                                           │
│ resv_t { used, id, ai, amount, call_id[40],               │
│         answer_ms, last_bill_sec, called[48],             │
│         called_len(uint8_t), trunk_id } ← Phase 2         │
│                                                           │
│ API: rate_lookup_stair(), calc_stair_charge(),            │
│      bill_reserve(key, amount, call_id, &resv_id),        │
│      bill_settle(key, call_id, actual_uc),                │
│      bill_is_enabled/set_enabled,  ← Phase 2              │
│      bill_resv_fill,  ← Phase 2                           │
│      billing_tick_internal/tick,  ← Phase 2 L4/L5          │
│      bill_foreach_reservation,  ← Phase 2 OAM             │
│      billing_pre_auth(),  ← Phase 1.5                     │
│      billing_lookup_trunk_id()                             │
└────────────────────┬─────────────────────────────────────┘
                     │
                     ▼
┌──────────────────────────────────────────────────────────┐
│ INVITE 入口 — b2bua.c (security_check_fence 之后)          │
│                                                           │
│ billing_pre_auth(caller, callee, tenant, src_ip)          │
│   → caller 是短 extension + 全数字 → 推 acct_key           │
│   → rate_lookup_stair(callee, trunk_id=0, now_hhmm, ...)  │
│   → account_balance + account_credit_limit                 │
│   → balance + credit_limit - reserved >= reserve → OK     │
│   → PERM_DENIED → 静默丢弃（与 security fence 同惯例）    │
│                                                           │
│ 绕过: Emergency URN:service:sos / 中继付费（caller 非数字）│
└────────────────────┬─────────────────────────────────────┘
                     │
                     ▼
┌──────────────────────────────────────────────────────────┐
│ b2b_cdr_hook_add — billing_cdr_cb() 自动接线               │
│                                                           │
│ Phase 1.5: trunk_name → billing_lookup_trunk_id → trunk_id│
│ Phase 1.5: bill_reserve 绑定 call_id → bill_settle 精确匹配│
│                                                           │
│ CDR_PHASE_ANSWER → lookup_stair(trunk_id) → reserve(call_id)│
│ CDR_PHASE_END    → lookup_stair(trunk_id) → settle(key, call_id)│
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
    tx_type         VARCHAR(16) NOT NULL,  -- "initial" / "topup" / "call_charge" / "refund" / "credit_adjust" / "manual"
    amount_uc       BIGINT NOT NULL,       -- 正=充值/退款, 负=扣费
    balance_after_uc BIGINT NOT NULL,
    operator        VARCHAR(64),
    remark          TEXT,
    created_at      TEXT NOT NULL,
    INDEX idx_bt_account (account_id),
    INDEX idx_bt_call (call_id),
    INDEX idx_bt_created (created_at)
);
```

**tx_type 枚举**：`initial`（开户开账） / `topup`（充值） / `refund`（退款） / `call_charge`（通话扣费） / `credit_adjust`（信用额度调整） / `manual`（人工调整）。Phase 1.5 之前只有 `topup` / `call_charge` / `credit_adjust` / `manual`，缺 `initial` 和 `refund`。

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

### 2.4 Db CRUD 方法（资金闭环审计）

| 方法 | 功能 | 写 billing_transactions？ | 状态 |
|---|---|---|---|
| `open_account` | INSERT accounts + 新行 | ✅ **有写** `tx_type=initial`（balance_uc>0 时） | ✅ G4 已闭环 |
| `topup_account(id, delta_uc)` | UPDATE balance_uc += delta | ✅ **有写** `tx_type=topup` | ✅ G2 已闭环 |
| `settle_billing(account_id, call_id, charge_uc)` | UPDATE balance_uc -= charge + INSERT | ✅ **有写** `tx_type=call_charge` | ✅ 已闭环 |
| `delete_account` | DELETE accounts | ❌ 应写 `tx_type=close` 或保留流水 | P1 |
| `refund_account(id, delta_uc, operator, remark)` | UPDATE balance_uc += delta（退款加余额）+ INSERT | ✅ **有写** `tx_type=refund` | ✅ G3 已闭环 |

---

## 3. Sync 层

```rust
// web/src/sync.rs — Kind 枚举扩展
pub enum Kind {
    // ... 旧 12 种 ...
    RatePlans,         // ← 新增
    RatePlanEntries,   // ← 新增
    // trunk_map 暂不进 sync（trunk_name↔trunk_id 是运行时映射，不需要 DB 表）
}

// push_kind() — 调 UDS 命令
Kind::RatePlans        → core.reload_rate_plans(&json)
Kind::RatePlanEntries  → core.reload_rate_plan_entries(&json)
Kind::Rates            → core.reload_rates(&json)            // 旧
Kind::Accounts         → core.reload_accounts(&json)         // 已扩展新字段
```

sync.rs 有 debounce（300ms）、退避重试（5 级）、周期性（60s）、HELLO 重连。

trunk_map 不走 sync —— 它是运行时 name→id 映射，由 OAM `trunk_map.reload <json>` 手动推送。可以后续扩展 trunks.reload 时自动附带推 trunk_map。

---

## 4. C Core 数据结构（Phase 1.5 更新）

### 4.1 resv_t（Phase 1.5 扩展 call_id）

```c
typedef struct {
    int      used;
    uint64_t id;
    size_t   ai;              /* acct index into b->accts[] */
    int64_t  amount;          /* reserved amount in uc */
    char     call_id[40];     /* SIP Call-ID — bind to a single call */
} resv_t;
```

**为什么加 call_id**：Phase 1 的 `bill_settle(b, acct_key, charge)` 会释放该账户**所有** reservation —— 同分机打两通电话，第一通 END 会把两通的 reserve 一起退掉，第二通 reserve 被提前释放。Phase 1.5 改为 `bill_settle(b, key, call_id, actual_uc)` 精确匹配。

### 4.2 billing_t（Phase 1.5 加 trunk_map）

```c
struct billing {
    pthread_rwlock_t lock;
    rate_entry_t rates[RATE_PLAN_MAX];
    int          n_rates;
    acct_t       accts[BILL_ACCT_MAX];
    resv_t       resv[4096];
    uint64_t     next_resv;
    int64_t      low_threshold;
    bill_low_cb  low_cb;
    void        *low_ud;

    /* Phase 1.5 L1: trunk name → id 映射 */
    trunk_map_entry_t trunk_map[64];
};
```

### 4.3 rate_entry_t（24 fields，Phase 1）

```c
// src/include/rating.h
typedef struct {
    char     prefix[24];
    char     name[32];
    int64_t  price_per_min_uc;
    int64_t  connect_fee_uc;
    int      bill_inc_s;
    int      min_bill_s;
    int      initial_s;
    int      enabled;

    /* Phase 1 新增 — 向后兼容：旧条目这些字段全零 */
    int64_t  tier_min_sec;
    int64_t  tier_max_sec;
    int64_t  trunk_id;
    int64_t  rate_plan_id;
    char     time_from[8];
    char     time_to[8];
    char     weekdays[16];
    int      precedence;
} rate_entry_t;
```

### 4.4 acct_t（10 fields，Phase 1）

```c
typedef struct {
    int      used;
    char     key[128];
    int64_t  balance;
    int64_t  reserved;
    int64_t  credit_limit;

    /* Phase 1 新增 */
    int64_t  tenant_id;
    char     account_type[16];
    int64_t  rate_plan_id;
    int64_t  low_threshold_uc;
} acct_t;
```

### 4.5 账户 Key 推导（billing_make_key）

```c
switch (ev->dir) {
case CDR_DIR_OUTBOUND:
    snprintf(key, "tenant:%s:extension:%s", tenant, ev->from);   // 主叫付费
case CDR_DIR_INBOUND:
    snprintf(key, "tenant:%s:trunk:%s", tenant, ev->trunk ?: ev->to); // 中继付费
case CDR_DIR_INTERNAL:
    snprintf(key, "tenant:%s:extension:%s", tenant, ev->from);   // 主叫付费
}
```

---

## 5. C Core API（Phase 1.5 更新）

> **头文件**: [rating.h](file:///d:/github/nsbc/src/include/rating.h#L110-L170) · **实现**: [rating.c](file:///d:/github/nsbc/src/mod/billing/rating.c)

### 5.1 费率

| API | 功能 | 代码位置 |
|-----|------|---------|
| `rate_upsert(b, entry)` | upsert 到 rates[]（prefix 覆盖更新）| [rating.c L83](file:///d:/github/nsbc/src/mod/billing/rating.c#L83) |
| `rate_delete(b, prefix)` | 按前缀删除 | [rating.c L108](file:///d:/github/nsbc/src/mod/billing/rating.c#L108) |
| `rate_lookup(b, called, out)` | 单费率查找（旧，兼容）| [rating.c L128](file:///d:/github/nsbc/src/mod/billing/rating.c#L128) |
| `rate_quote(r, billsec, q)` | 单费率报价（旧，兼容）| [rating.c L171](file:///d:/github/nsbc/src/mod/billing/rating.c#L171) |
| `rate_lookup_stair(b, called, trunk_id, now_hhmm, weekdays, tiers, &n, max)` | 多维阶梯查找 — 前缀最长匹配 + trunk + 时段 + precedence 排序 | [rating.c L721](file:///d:/github/nsbc/src/mod/billing/rating.c#L721) |
| `calc_stair_charge(tiers, n_tiers, billsec)` | 阶梯计费 — 逐段 bill_inc_s 取整、min_bill 兜底、接通费一次性 | [rating.c L791](file:///d:/github/nsbc/src/mod/billing/rating.c#L791) |
| `rate_plan_entries_reload_json(b, json)` | 从 JSON 数组 upsert | [rating.c L699](file:///d:/github/nsbc/src/mod/billing/rating.c#L699) |
| `rate_plans_reload_json(b, json)` | 从 JSON 数组 upsert plan 元数据 | [rating.c L706](file:///d:/github/nsbc/src/mod/billing/rating.c#L706) |
| `time_slot_match(time_from, time_to, weekdays, hhmm, wd)` | 时段/星期匹配 | [rating.c L623](file:///d:/github/nsbc/src/mod/billing/rating.c#L623) |

### 5.2 账户 + 计费（Phase 1.5 签名变更标 ★）

| API | 功能 | Phase | 代码位置 |
|-----|------|-------|---------|
| `account_open(b, key, balance, credit)` | 3 参数，内部填新字段默认值 | 旧 | [rating.c L207](file:///d:/github/nsbc/src/mod/billing/rating.c#L207) |
| `account_balance(b, key, &bal, &resv)` | 查余额 + 预占 | 旧 | [rating.c L249](file:///d:/github/nsbc/src/mod/billing/rating.c#L249) |
| `account_topup(b, key, delta)` | 充值 | 旧 | [rating.c L269](file:///d:/github/nsbc/src/mod/billing/rating.c#L269) |
| `account_reload_json(b, json)` | 持 wrlock + 直接 upsert accts[] 所有 8 字段 | Phase 1 | [rating.c L513](file:///d:/github/nsbc/src/mod/billing/rating.c#L513) |
| ★ `bill_reserve(b, key, amount, call_id, &resv_id)` | 预占，绑定 call_id（call_id 传 NULL = 向后兼容）| Phase 1.5 | [rating.h L114](file:///d:/github/nsbc/src/include/rating.h#L114) · [rating.c L287](file:///d:/github/nsbc/src/mod/billing/rating.c#L287) |
| ★ `bill_settle(b, key, call_id, actual_uc)` | 结算，按 key+call_id 精确匹配 reservation | Phase 1.5 | [rating.h L120](file:///d:/github/nsbc/src/include/rating.h#L120) · [rating.c L386](file:///d:/github/nsbc/src/mod/billing/rating.c#L386) |
| `bill_release(b, resv_id)` | 释放（按 resv_id，不关心 call_id） | 旧 | [rating.c L368](file:///d:/github/nsbc/src/mod/billing/rating.c#L368) |
| ★ `billing_pre_auth(b, caller, callee, tenant, src_ip)` | INVITE 入口余额检查 — 返回 PERM_DENIED 静默丢 | Phase 1.5 | [rating.h L162](file:///d:/github/nsbc/src/include/rating.h#L162) · [rating.c L846](file:///d:/github/nsbc/src/mod/billing/rating.c#L846) |
| ★ `billing_lookup_trunk_id(b, trunk_name)` | trunk name → int64 trunk_id（没找到返回 0 兜底）| Phase 1.5 | [rating.h L169](file:///d:/github/nsbc/src/include/rating.h#L169) · [rating.c L947](file:///d:/github/nsbc/src/mod/billing/rating.c#L947) |
| ★ `trunk_map_reload_json(b, json)` | 从 JSON 数组 upsert trunk_map（OAM trunk_map.reload 调）| Phase 1.5 | [rating.h L170](file:///d:/github/nsbc/src/include/rating.h#L170) · [rating.c L963](file:///d:/github/nsbc/src/mod/billing/rating.c#L963) |
| `resv_find_call_id(b, key, call_id)` | **内部辅助函数** — 按 key+call_id 找唯一 reservation | Phase 1.5 | [rating.c L340](file:///d:/github/nsbc/src/mod/billing/rating.c#L340) |

### 5.3 内部锁

```c
static acct_t *acct_find_locked(b, key);   // 假设调用方已持 wrlock（account_reload_json 用）
static acct_t *acct_find(b, key);          // 内部自己加 rdlock
```

**教训（K1）**：`account_reload_json` 持 wrlock 调 `account_open`（内部也 wrlock）→ 死锁。修复：提供 locked 版本。`billing_pre_auth` 内部用 `acct_find`（自带 rdlock/unlock），**不能**外部先持锁再调。

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

**Phase 1.5 trunk_id 来源**：
- 有 trunk_map → `billing_lookup_trunk_id(ev->trunk)` → 精确匹配 trunk 维度费率
- 无 trunk_map → 传 0 → rate_entry.trunk_id=0 条目匹配（"不限"兜底）
- 两者共存：trunk_id=unicom_id 精确条目 + trunk_id=0 不限条目都进 tiers[]，rate_lookup_stair 长前缀优先排序

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
            this_tier = min_bill_s
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
| 同一 010 走 unicom 比 telecom 便宜 | trunk_id=unicom(1), price=100000 vs trunk_id=telecom(2), price=150000 |

### 6.4 Reserve 保守策略

```
reserve_sec = max(tiers[0].min_bill_s, 120)   // 至少 2 分钟
reserve     = calc_stair_charge(tiers, reserve_sec) × 2 + connect_fee
```

保守原因：长通话跨过阶梯，费用会超过 reserve × 2 兜底。

### 6.5 Settle 结算（Phase 1.5 修正后）

```
charge     = calc_stair_charge(tiers, actual_billsec)
resv       = resv_find_call_id(b, acct_key, call_id)  // Phase 1.5: 精确匹配 reservation
reserved   = resv.amount

// 多退少补：
a->reserved -= reserved      // 释放预占
a->balance  -= charge        // 扣实际费用

// charge < reserved → 多的退回余额（balance 比 reserve 时多了 reserved - charge）
// charge > reserved → 透支（余额变负，受 credit_limit 约束）
```

### 6.6 Pre-auth 逻辑（Phase 1.5）

```
billing_pre_auth(caller, callee, tenant, src_ip):
  1. caller 长度 ≤ 8 且全数字 → 推 acct_key = "tenant:<tid>:extension:<caller>"
  2. 否则跳过（中继付费等 ANSWER reserve 兜底）
  3. rate_lookup_stair(callee, trunk_id=0, now_hhmm, ...) → tiers
  4. 保守 reserve = calc_stair_charge(tiers, max(min_bill, 120)) × 2 + connect_fee
  5. account_balance + account_credit_limit - account_reserved >= reserve → OK
  6. 余额不够 → PERM_DENIED（静默丢）
  7. 账户不存在 / 没匹配费率 → NOENT（放行，让 ANSWER 兜底）
```

---

## 7. Billing CDR Hook（Phase 1.5 更新）

> **代码位置**: [billing.c L114-L219](file:///d:/github/nsbc/src/mod/billing/billing.c#L114-L219)

```
billing_init() 内部:
    b2b_cdr_hook_add(billing_cdr_cb, g_sink)
    ctx->billing = g_billing

billing_cdr_cb(ud, ev):                              // billing.c ~L114
    cdr_sink_emit(sink, ev)                           // 始终先写 CDR（原有路径）
    if (g_billing == NULL) return

    acct_key = billing_make_key(ev)                   // 按 dir 推导: tenant:X:extension:Y 或 tenant:X:trunk:Z

    switch (ev->phase):
    CDR_PHASE_ANSWER:                                  // billing.c ~L140
        ★ trunk_id = billing_lookup_trunk_id(g_billing, ev->trunk)  // Phase 1.5 L1: name→id
        rate_lookup_stair(g_billing, ev->to, trunk_id, hhmm, NULL, tiers, &n, 8)
        reserve_sec = max(tiers[0].min_bill_s, 120)
        reserve = calc_stair_charge(tiers, reserve_sec) × 2 + connect_fee
        ★ bill_reserve(g_billing, acct_key, reserve, ev->call_id, &resv_id)  // L160: 绑定 call_id
        log_info("billing", "ANSWER %s reserved=%lld uc acct=%s resv_id=%llu", ...)

    CDR_PHASE_END:                                      // billing.c ~L174
        ★ trunk_id = billing_lookup_trunk_id(g_billing, ev->trunk)  // L182-184
        rate_lookup_stair(g_billing, ev->to, trunk_id, hhmm, NULL, tiers, &n, 8)
        charge = calc_stair_charge(tiers, ev->billsec)
        ★ bill_settle(g_billing, acct_key, ev->call_id, charge)     // L198: 按 call_id 精确匹配
        account_balance → if (bal < 0):
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
| GET/POST | `/api/v1/billing/rate-plans` | 费率方案 CRUD |
| GET/PUT/DELETE | `/api/v1/billing/rate-plans/:id` | 方案 CRUD |
| POST | `/api/v1/billing/rate-plans/reload` | 推方案元数据到 core |
| GET/POST | `/api/v1/billing/rate-plan-entries` | 阶梯/时段/中继条目 |
| GET/DELETE | `/api/v1/billing/rate-plan-entries/:id` | 条目 CRUD |
| POST | `/api/v1/billing/rate-plan-entries/reload` | 推阶梯条目到 core |
| GET/POST | `/api/v1/billing/balances` | 账户（已扩展新字段） |
| PUT/DELETE | `/api/v1/billing/balances/:id` | 账户更新/关闭 |
| POST | `/api/v1/billing/balances/:id/topup` | 充值（DB + core 同步 + 安全网） ✅ 写 billing_transactions(G2) |
| POST | `/api/v1/billing/balances/reload` | accounts.reload UDS |
| ✅ **POST** | **`/api/v1/billing/balances/:id/refund`** | **退款（Phase 1.6 G3）— delta_uc 正数 + operator/remark + tx_type="refund"** |
| GET | `/api/v1/billing/transactions` | 交易流水查询（account_id/call_id/tx_type 过滤） |
| ✅ **自动** | **C→Rust UDS billing_settle** | **END settle 触发 settle_billing() 写 tx_type="call_charge"（G1）** |
| ✅ **GET** | **`/api/v1/billing/reservations/core`** | **Phase 2 — show resv 活跃 Reservation 实时快照（L4v2 tick 调试）** |
| ✅ **GET** | **`/api/v1/billing/enabled`** | **Phase 2 — 查 billing.enabled 当前值（config.get）** |
| ✅ **PUT** | **`/api/v1/billing/enabled`** | **Phase 2 — 热切换计费开关（config.set，CGF_HOT 自动生效）** |

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
| **旧 rate.reload 命令** | 完全不变，和 rate_plan_entries.reload 共存 |
| **旧 accounts 表数据** | ALTER TABLE ADD COLUMN IF NOT EXISTS + DEFAULT 值 |
| **旧 rate_tables 表数据** | 同上 |
| **trunk_map 空** | billing_lookup_trunk_id 没找到返回 0 → trunk_id=0 兜底（Phase 1 行为）|
| **billing_pre_auth 跳过 call_id 绑定** | bill_reserve(call_id=NULL) 存空串；bill_settle(call_id=NULL) 退化按 key 找第一个 reservation |

---

## 10. 关键数据流向图（Phase 1.5 更新）

```
用户/管理员
  │
  ├── curl POST /trunk_map.reload [{"name":"unicom","id":1}]
  │                                  ▼
  │                          OAM trunk_map.reload → billing.trunk_map upsert
  │                                  │
  ├── curl POST /balances/:id/topup  │
  │                                  │
  │                          ⚠️ DB accounts.balance 更新
  │                          ❌ 不写 billing_transactions  ← G2 缺口
  │                                  │
  │                          sync accounts.reload → core.balance
  │                                  │
  └── 发起 010xxxxxxxx 通话          │
      │                              │
      │                              ▼
  INVITE ──► b2bua.c
      │
      ├── security_check_fence ✅
      ├── ★ billing_pre_auth(caller, callee)  ← Phase 1.5 L2
      │   ├── caller 是 1000 → 推 acct_key=tenant:1:extension:1000
      │   ├── rate_lookup_stair("010...", trunk_id=0, now) → tiers
      │   ├── reserve = calc_stair(tiers, 120×2+fee)
      │   └── balance + credit_limit - reserved >= reserve?
      │       ├── ✅ → 放行
      │       └── ❌ → 静默丢（PERM_DENIED）
      │
      ├── b2b_call_t 分配 → 拨号规则 → 180 Ringing
      │
      ▼
  200 OK (ANSWER) ──► billing_cdr_cb()
      │
      ├── ★ billing_lookup_trunk_id("china_unicom") → trunk_id=1  ← Phase 1.5 L1
      ├── rate_lookup_stair(to, trunk_id=1, now, tiers, &n)
      ├── reserve = calc_stair_charge(tiers, max(min_bill, 120)) × 2 + fee
      ├── ★ bill_reserve(acct_key, reserve, call_id, &resv_id)  ← Phase 1.5 L3 绑定 call_id
      └── log_info("ANSWER reserved=%lld ...")
      │
      ▼
  BYE (END) ──► billing_cdr_cb()
      │
      ├── billing_lookup_trunk_id(trunk) → trunk_id
      ├── rate_lookup_stair(to, trunk_id, now, tiers, &n)
      ├── charge = calc_stair_charge(tiers, billsec=185)
      ├── ★ bill_settle(acct_key, call_id, charge)  ← Phase 1.5 L3 精确匹配
      │   └── resv_find_call_id → resv[call_id] 唯一
      │   └── a->reserved -= resv.amount
      │   └── a->balance -= charge    (多退少补)
      │   └── resv.used = 0
      ├── log_info("END billsec=185 charge=xxx settle=OK")
      └── if bal < 0: log_warn("OVERDRAWN")
      │
      └── ✅ Phase 1.6 G1: uds_push("billing_settle", {...})
         │        ↓ Rust uds_server dispatch_push
         │   get_account_by_key(ak) → account_id
         │   settle_billing(id, call_id, charge)
         │   → Transaction: UPDATE balance + INSERT billing_transactions(call_charge) ✅
```

---

## 11. ✅ 资金闭环 — Phase 1.6 全部补完

```
开户 → 充值 → 退款 → 通话扣费 → 计费明细 → 对账
  ✅      ✅       ✅       ✅        ✅        ✅
 G4      G2       G3       G1        ✅        ✅
```

| # | 缺口 | 修复 | 改动文件 |
|---|------|------|---------|
| **G1** | 通话扣费不落 DB | C 侧 `billing.c` END settle 成功后 `uds_push("billing_settle", {...})` → Rust `uds_server.rs` dispatch_push handler → `get_account_by_key` + `settle_billing` | [billing.c L200-220](file:///d:/github/nsbc/src/mod/billing/billing.c#L200-L220) · [uDS_client.h:62](file:///d:/github/nsbc/src/include/uDS_client.h#L62) · [uds_client.c:477](file:///d:/github/nsbc/src/oam/uds_client.c#L477) · [uds_server.rs L287-306](file:///d:/github/nsbc/web/src/uds_server.rs#L287-L306) · [db/mod.rs get_account_by_key](file:///d:/github/nsbc/web/src/db/mod.rs#L8240) |
| **G2** | topup 不写流水 | `topup_account` 用 `sqlx::Transaction` 包 UPDATE + INSERT `tx_type="topup"` | [db/mod.rs topup_account](file:///d:/github/nsbc/web/src/db/mod.rs#L8302) |
| **G3** | 退款接口缺失 | 新增 `refund_account(id, delta_uc, operator, remark)` DB 方法 + `POST /balances/:id/refund` route | [db/mod.rs refund_account](file:///d:/github/nsbc/web/src/db/mod.rs#L8373) · [billing.rs L65-67](file:///d:/github/nsbc/web/src/routes/billing.rs#L65-L67) · [billing.rs refund_account handler](file:///d:/github/nsbc/web/src/routes/billing.rs#L379-L424) |
| **G4** | 开户没 initial 流水 | `open_account` 改成 `sqlx::Transaction`：INSERT accounts + 条件性 INSERT `tx_type="initial"` | [db/mod.rs open_account](file:///d:/github/nsbc/web/src/db/mod.rs#L8248) |

### 跨进程 G1 完整链路

```
nsbcd billing.c END settle 成功
  │  构建 JSON: {"account_key","call_id","charge_uc","billsec",...}
  ▼ uds_push("billing_settle", json)     ← uDS_client.h 新增通用推送 API
  │  ring_put → FT_DATA_PUSH 异步帧发送
  ▼ nsbc-web uds_server.rs dispatch_push("billing_settle")
  │  get_account_by_key(ak) → account_id（中继付费无账户则 debug 跳过）
  │  settle_billing(acct.id, call_id, -charge_uc, None)
  ▼ settle_billing 内部 sqlx::Transaction:
     UPDATE accounts SET balance_uc -= charge
     INSERT billing_transactions(tx_type="call_charge", ...)
     → commit 原子性保证余额 + 流水一致
```

### Transaction 原子性保证

G1（settle_billing）/ G2（topup）/ G3（refund）/ G4（open initial）四条写入路径**全部**用 `sqlx::Transaction` 包着 balance 更新 + billing_transactions INSERT。任何一步失败 → 回滚，Core 内存余额与 DB 余额可能暂时不一致（Core 内存已扣/已加，DB 回滚），但 accounts.reload 安全网会在下一轮 sync 周期用 DB 绝对余额覆盖 Core。

---

## 12. 文件变更完整清单（累计）

> 所有文件点击可跳转 GitHub 工作区对应路径。

```
Phase 1 (4 commits):
  [rating.h](file:///d:/github/nsbc/src/include/rating.h)          (rate_entry_t 扩展 + 4 新 API)
  [rating.c](file:///d:/github/nsbc/src/mod/billing/rating.c)      (阶梯算法 + 账户扩展 + JSON 解析 + acct_find_locked)
  [billing.c](file:///d:/github/nsbc/src/mod/billing/billing.c)    (billing_cdr_cb reserve/settle 接线)
  [obs_control.c](file:///d:/github/nsbc/src/oam/obs_control.c)    (rate_plans.reload + rate_plan_entries.reload + show rates 新字段)
  web/src/db/mod.rs             (4 新表 DDL + 升级迁移 + structs + CRUD + Account 扩展)
  web/src/sync.rs               (Kind::RatePlans / RatePlanEntries)
  web/src/coreclient.rs         (reload_rate_plans / reload_rate_plan_entries trait + impl)
  web/src/routes/billing.rs     (Rate Plans / Rate Plan Entries / Transactions HTTP API)

Phase 1.5 (本轮增量):
  [rating.h](file:///d:/github/nsbc/src/include/rating.h#L110-L170)
    bill_reserve/bill_settle 签名变更（加 call_id）
    新增 billing_pre_auth / billing_lookup_trunk_id / trunk_map_reload_json

  [rating.c](file:///d:/github/nsbc/src/mod/billing/rating.c)
    resv_t 加 call_id[40]（L34）
    billing_t 加 trunk_map[64]（L60）
    新增 resv_find_call_id（L340）— Phase 1.5 L3 核心匹配函数
    重写 bill_settle（L386）— 按 call_id 精确匹配
    新增 billing_pre_auth（L846）— Phase 1.5 L2 INVITE 入口检查
    新增 billing_lookup_trunk_id（L947）+ trunk_map_reload_json（L963）— Phase 1.5 L1

  [billing.c](file:///d:/github/nsbc/src/mod/billing/billing.c#L114-L219)
    ANSWER L160: bill_reserve(acct_key, reserve, ev->call_id, &resv_id)
    END L198:   bill_settle(acct_key, ev->call_id, charge)
    ANSWER/END 都用 billing_lookup_trunk_id(ev->trunk) 替换硬编码 trunk_id=0

  [b2bua.c](file:///d:/github/nsbc/src/sip/b2bua.c#L3545-L3571)
    #include "rating.h"（L29）
    security_check_fence 之后接 billing_pre_auth（L3563）
    PERM_DENIED → 静默 return（L3568）

  [obs_control.c](file:///d:/github/nsbc/src/oam/obs_control.c#L1464-L1475)
    新增 trunk_map.reload <json> 命令处理器（L1464）

Phase 1.6 — 资金闭环（本轮增量）:
  [uDS_client.h](file:///d:/github/nsbc/src/include/uDS_client.h#L62)
    新增 int uds_push(const char *kind, const char *json) — 通用推送 API

  [uds_client.c](file:///d:/github/nsbc/src/oam/uds_client.c#L477-L482)
    实现 uds_push() — 所有 uds_send_xxx 都改成调它（向后兼容）

  [billing.c](file:///d:/github/nsbc/src/mod/billing/billing.c#L200-L220)
    END settle 成功后构建 billing_settle JSON → uds_push("billing_settle", json)
    加 #include "uDS_client.h" + #include <cjson/cJSON.h>

  [rating.c](file:///d:/github/nsbc/src/mod/billing/rating.c#L594-L600)
    Werror 修复: (void)mask / (void)weekdays_cache / 修正 json_walk_objects 调用参数

  [db/mod.rs](file:///d:/github/nsbc/web/src/db/mod.rs)
    open_account: sqlx::Transaction 包装 + balance_uc>0 写 tx_type="initial" 流水
    topup_account: sqlx::Transaction 包装 + 写 tx_type="topup" 流水
    新增 refund_account(id, delta_uc, operator, remark) — tx_type="refund"
    新增 get_account_by_key(account_key) — billing_settle handler 用

  [billing.rs](file:///d:/github/nsbc/web/src/routes/billing.rs#L65-L67)
    新增 POST /balances/:id/refund route + RefundBody struct + refund_account handler

  [uds_server.rs](file:///d:/github/nsbc/web/src/uds_server.rs#L287-L306)
    dispatch_push 新增 "billing_settle" kind → get_account_by_key + settle_billing
```
