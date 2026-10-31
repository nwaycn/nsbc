# Billing 模块 Phase 1 交接

**更新时间**: 2026-10-31
**Commits**: `bfd4696` · `1820270` · `3aa0fb8` · `3aff3b6`
**状态**: Phase 1 完成（DB Schema + Sync + C Core 阶梯算法 + reserve/settle CDR hook + HTTP API）
**下一步**: 远端验证 → Phase 2（INVITE pre-auth / trunk_id 映射 / 实时 tick / IVR collect）

---

## 0. 快速开始（接手人首先看）

```bash
# 拉取最新
git pull

# 远端 Debian 12 编译
make -j$(nproc) && systemctl restart nsbcd && systemctl restart nsbc-web

# 验证 DB 建表（auto ensure_all_tables）
sqlite3 /var/lib/nsbc/nsbc.db ".tables" | grep -E "rate_plan|billing_trans"

# curl 测试 API
curl -s -X POST http://localhost:8080/api/v1/billing/rate-plans \
  -H 'Content-Type: application/json' \
  -d '{"tenant_id":1,"name":"test","enabled":true}'

# OAM 手动 reload 验证
echo "show rates" | nc -U /var/run/nsbc/control.sock

# 发起一通 010 通话后看计费日志
grep "billing.*reserved\|billing.*billsec" logs/nsbcd.log
```

详细看 [billing-state.md](./billing-state.md)。

---

## 1. 完成清单

### 1.1 DB Layer（`bfd4696`）

| 项目 | 完成 | 文件 |
|------|------|------|
| 4 张新表 DDL（三后端 tri-branch）| ✅ | web/src/db/mod.rs L3025-3295 |
| accounts/rate_tables 升级迁移（幂等）| ✅ | sqlite PRAGMA + pg/mysql ALTER ADD COLUMN IF NOT EXISTS |
| RatePlan / RatePlanEntry / BillingTransaction / TrunkRateOverride struct | ✅ | L6269-6385 |
| Account struct 扩展新字段 | ✅ | L6231-6243 |
| 全套 CRUD（list/get/create/update/delete）| ✅ | L7562-7930 |
| account_topup（sqlx::Transaction 原子性）| ✅ | L7864-7900 |
| settle_billing（sqlx::Transaction）| ✅ | L7903-7930 |
| cargo check 零错误 | ✅ | 45 预存 warning 未新增 |

### 1.2 Sync Layer（`bfd4696`）

| 项目 | 完成 | 文件 |
|------|------|------|
| Kind 枚举 + RatePlans / RatePlanEntries | ✅ | web/src/sync.rs |
| snapshot + push_kind 完整链路 | ✅ | |
| CoreApi reload_rate_plans / reload_rate_plan_entries | ✅ | web/src/coreclient.rs |
| RealCore + MockCore 两套 impl | ✅ | |
| 自动继承 300ms debounce / 退避 / periodic / HELLO 重连 | ✅ | 框架通用 |

### 1.3 C Core 阶梯算法（`1820270`）

| 项目 | 完成 | 文件 |
|------|------|------|
| rate_entry_t 扩展 9 个字段（向后兼容）| ✅ | src/include/rating.h |
| calc_stair_charge() 逐阶梯分段 + bill_inc_s 取整 + min_bill 兜底 | ✅ | src/mod/billing/rating.c |
| rate_lookup_stair() 前缀最长 + trunk + 时段过滤 + tier 排序 | ✅ | |
| time_slot_match() HH:MM 跨天 + weekdays 匹配 | ✅ | |
| json_parse_entry() jstr/jint 手写 JSON（无私有依赖）| ✅ | |
| json_walk_objects() + walk_upsert_entry() jarray_at 遍历 | ✅ | |
| rate_plan_entries_reload_json() / rate_plans_reload_json() | ✅ | |
| OAM rate_plans.reload / rate_plan_entries.reload | ✅ | src/oam/obs_control.c |
| show rates 输出含 tier/trunk/time/prec | ✅ | |

### 1.4 End-to-End Reserve/Settle（`3aa0fb8`）

| 项目 | 完成 | 文件 |
|------|------|------|
| acct_t 扩展 4 新字段（tenant_id/account_type/rate_plan_id/low_threshold）| ✅ | src/mod/billing/rating.c |
| acct_find_locked() — 已持锁版本（避免嵌套 wrlock）| ✅ | |
| account_reload_json 重写：持 wrlock + 直接 upsert 全部 8 字段 | ✅ | |
| billing_make_key() — 按 dir 推导 account key | ✅ | src/mod/billing/billing.c |
| billing_now_hhmm() — 当前时间 | ✅ | |
| billing_cdr_cb 接 reserve/settle | ✅ | |
| ANSWER → reserve（保守策略：×2 + fee）| ✅ | |
| END → settle（多退少补 + OVERDRAWN 检测）| ✅ | |
| CDR 原有 cdr_sink_emit 保留（始终先做）| ✅ | |

### 1.5 HTTP API（`3aff3b6`）

| 项目 | 完成 | 文件 |
|------|------|------|
| Rate Plans CRUD (GET/POST/PUT/DELETE /rate-plans) | ✅ | web/src/routes/billing.rs |
| Rate Plan Entries (GET/POST/DELETE /rate-plan-entries) | ✅ | |
| Billing Transactions (GET /transactions + filters) | ✅ | |
| reload /rate-plans/reload /rate-plan-entries/reload | ✅ | |
| delete_rate_plan 前置检查（是否有 entries）| ✅ | |
| 所有路由走 sync::spawn_push 安全网 | ✅ | |
| cargo check 零错误 | ✅ | |

---

## 2. 遗留 / 未实现

### P0 — 阻塞实际计费生效

| # | 项 | 复杂度 | 为什么阻塞 |
|---|----|--------|-----------|
| **L1** | trunk_name → trunk_id 映射 | S | CDR `trunk` 是字符串（如 "china_unicom"），rate_lookup_stair 要 int64 trunk_id。rate_plan_entries 里 trunk_id=0 = 不限。现在 trunk_id 匹配永远走 0 兜底 |
| **L2** | INVITE pre-auth（b2b_guard_hook_add）| L | 现在 reserve 走 post-hoc（ANSWER 后才 reserve）。余额 0 的账户仍然能接通、用完话费、OVERDRAWN 再告警。需要在 INVITE 入口查 rate_lookup_stair + account_balance → balance + credit_limit 不够 → PERM_DENIED |
| **L3** | bill_settle 多账户 reservation 匹配 | M | bill_reserve 记录的 resv_t.ai = account index（不是 call_id）。SETTLE 时调 `bill_settle(b, acct_key, charge)` 会释放该账户**所有** reservation 并只保留 charge —— 如果同账户多通话并发，会互相多退少补错乱。需要 resv_t 加 call_id 字段 + settle 按 call_id 匹配 |

### P1 — 生产必备

| # | 项 | 复杂度 | 说明 |
|---|----|--------|------|
| **L4** | 实时扣费 tick（60s 周期）| L | 长通话可能 2 小时没人管，费用越积越高。需要每秒 tick 遍历所有活跃 CALLID → billsec 变化 → calc_stair → balance 不够 → kill call |
| **L5** | OVERDRAWN 强制 kill 同账户其他通话 | L | account_balance < 0 只 log_warn，需要遍历 b2bua.calls → 同 acct_key 全部 uuid_kill |
| **L6** | settle_billing Web API 接入 | S | 现在 settle_billing 是 DB method，但没 HTTP route。CDR 落库时应自动触发 settle |

### P2 — 高级

| # | 项 | 复杂度 |
|---|----|--------|
| **L7** | 周期性扣费（包月/按天 cron）| L |
| **L8** | 反欺诈：高额/高频通话实时告警 | L |
| **L9** | IVR collect / cond / 时段路由（独立需求）| XL |

---

## 3. 踩过的坑（别重蹈覆辙）

### K1: `account_reload_json` 嵌套 wrlock

**现象**：原实现调 `account_open(b, key, balance, credit)`，后者内部 `pthread_rwlock_wrlock(&b->lock)` —— 但 account_reload_json 调用前**已经** wrlock，导致死锁。

**修复**：抽出 `acct_find_locked()` —— 不获取锁版本。account_reload_json 自己持 wrlock，内部直接调 locked 版本查找/创建槽位。

**教训**：函数级 API（account_open 带锁）和批量操作（account_reload_json 持锁）混用 → 必须提供 locked 内部版本。

### K2: `rate_lookup_stair` trunk_id=0 匹配陷阱

**现象**：billing_cdr_cb 传 trunk_id=0（从 cdr_event_t.trunk 字符串转不了 int64）。rate_plan_entries 如果配置了 trunk_id=unicom_id 就跳过了。

**当前**： trunk_id=0 条目能匹配（"不限"语义），trunk_id≠0 条目只有精确匹配才命中。

**修复方向**：见遗留 L1 — trunk_name → trunk_id 映射。

### K3: `bill_reserve` / `bill_settle` 跨通话错乱

**现象**：同分机 1000 同时打两通电话。第一通 reserve 100000 微元，第二通 reserve 100000 微元。第一通 END → settle 50000 微元 → balance 被退回 150000（两通的 reserve 总和），第二通 reserve 被提前释放。

**根因**：resv_t 没有 call_id 关联，bill_settle(account_key, amount) 会释放该 key 下**所有** reservation。

**修复方向**：resv_t 加 `call_id[40]`；bill_reserve 接收 call_id；bill_settle(account_key, call_id, actual_amount) 只释放匹配 call_id 的 reservation。

### K4: cargo check 通不代表生产可用

本次 cargo check 零错误，但 **Linux C 编译没跑过**！Windows 不编译 C 代码。远端 `cd /home/NSBC && make clean && make -j$(nproc)` 是必须的验证步骤。

### K5: rate_lookup_stair 返回顺序与 tier 插入顺序

**现象**：plan_entries reload 时数据库按 creation order upsert。同一个 prefix 的 tier 可能乱序进 rates[]。如果 calc_stair_charge 按错误的 tier 顺序算，第二段覆盖第一段。

**修复**：rate_lookup_stair 末尾有插入排序 `tier_min_sec asc, precedence desc` —— 不管输入顺序，输出保证升序。

### K6: 时段跨天

**现象**："22:00 - 06:00"（夜班时段）用普通 `from <= now && now < to` 匹配，会错判。

**修复**：`time_in_range()` 检测 `from > to` → 跨天 → `now >= from || now < to`。

### K7: rate_plan_entries 和 rate_tables 共存

**现象**：旧 rate.reload 和新 rate_plan_entries.reload 都往 billing.rates[] 写，同一个 prefix 可能有多条（一条单费率旧条目 + 多条阶梯新条目）。

**OK**：rate_lookup_stair 把所有匹配的都收集起来，按 tier_min_sec 排序。单费率条目 tier_min=0,tier_max=0 全覆盖，阶梯条目 tier_min_sec=0,180,3600 升序。输出数组顺序正确。

---

## 4. 关键路径代码索引

| 功能 | 文件 | 函数/行 |
|------|------|---------|
| DB DDL (4 新表) | web/src/db/mod.rs | L3025-3295 |
| DB upgrade migrations | 同上 | L3217-3295 |
| CRUD 方法 | 同上 | L7562-7930 |
| sync Kind 枚举 | web/src/sync.rs | L101+ |
| CoreApi trait | web/src/coreclient.rs | L760+ |
| Reload UDS 命令 | 同上 RealCore | L1071-1078 |
| OAM rate_plans.reload | src/oam/obs_control.c | ~L1420 |
| OAM rate_plan_entries.reload | 同上 | ~L1440 |
| show rates 扩展输出 | 同上 | rate_show_cb |
| rate_entry_t struct | src/include/rating.h | L26+ |
| acct_t struct | src/mod/billing/rating.c | L12+ |
| rate_lookup_stair | 同上 | ~L650 |
| calc_stair_charge | 同上 | ~L730 |
| time_slot_match | 同上 | ~L530 |
| account_reload_json (重写)| 同上 | ~L435 |
| billing_cdr_cb (reserve/settle)| src/mod/billing/billing.c | ~L113 |
| billing_make_key | 同上 | ~L86 |
| HTTP routes | web/src/routes/billing.rs | router() + handlers |

---

## 5. 远端验证 Checklist

- [ ] `git pull && make -j$(nproc) && systemctl restart nsbcd && systemctl restart nsbc-web`
- [ ] DB 自动建表：`sqlite3 /var/lib/nsbc/nsbc.db ".tables"` 有 `rate_plans / rate_plan_entries / trunk_rate_overrides / billing_transactions`
- [ ] accounts 表 4 个新列存在：`PRAGMA table_info(accounts)`
- [ ] curl POST /api/v1/billing/rate-plans 返回 201
- [ ] curl POST /api/v1/billing/rate-plan-entries (2 条 tier) 返回 201
- [ ] curl POST /api/v1/billing/rate-plan-entries/reload 返回 `{loaded: 2}`
- [ ] `echo "show rates" | nc -U /var/run/nsbc/control.sock` 能看到 tier/trunk/time/prec 字段
- [ ] 发起一通 010 通话（3+ 分钟）
- [ ] `grep "billing.*reserved" logs/nsbcd.log` 有 ANSWER reserve 日志
- [ ] `grep "billing.*billsec" logs/nsbcd.log` 有 END settle 日志
- [ ] 查 billing_transactions 有 `tx_type=call_charge` + `amount_uc=<负>` + `balance_after_uc=<正确余额>`

---

## 6. 推进路线

```
Phase 1.5 — 补齐遗留（1-2 周）
  L1 trunk_name → trunk_id 映射
  L2 INVITE pre-auth (b2b_guard_hook_add)
  L3 resv_t 加 call_id — settle 按 call_id 精确匹配

Phase 2 — 生产必备（2-3 周）
  L4 实时扣费 tick
  L5 OVERDRAWN kill
  L6 settle_billing Web API + CDR auto-settle
  + IVR collect / cond / 时段路由（独立模块）

Phase 3 — P1 高级（按需）
  L7 周期性扣费 cron
  L8 反欺诈
```

---

## 7. 变更的完整文件清单

```
C Core:
  src/include/rating.h               (rate_entry_t 扩展 + 4 新 API)
  src/mod/billing/rating.c           (阶梯算法 + 账户扩展 + JSON 解析 + acct_find_locked)
  src/mod/billing/billing.c          (billing_cdr_cb reserve/settle 接线)
  src/oam/obs_control.c             (rate_plans.reload + rate_plan_entries.reload + show rates 新字段)

Web Rust:
  web/src/db/mod.rs                  (4 新表 DDL + 升级迁移 + structs + CRUD + Account 扩展)
  web/src/sync.rs                    (Kind::RatePlans / RatePlanEntries)
  web/src/coreclient.rs              (reload_rate_plans / reload_rate_plan_entries trait + impl)
  web/src/routes/billing.rs          (Rate Plans / Rate Plan Entries / Transactions HTTP API)
```
