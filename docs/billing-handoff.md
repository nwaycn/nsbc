# Billing 模块 Phase 2 交接

**更新时间**: 2026-11-02
**状态**: Phase 1 + 1.5 + 1.6 资金闭环 + Phase 2 **全部完成**（含审计修复）
**Web cargo check**: ✅ 零错误
**C core Linux 编译**: ✅ 远端 make 通过（手工编译验证）
**Phase 3**: IVR 增强 + 高级特性（未启动）
**详细状态**: 见 [billing-state.md](./billing-state.md)

---

## 0. 快速开始（接手人首先看）

```bash
# 拉取最新
git pull

# 远端 Debian 12 编译（Phase 1 没验证！这步绝对必须）
cd /home/NSBC && make -j$(nproc) && systemctl restart nsbcd && systemctl restart nsbc-web

# 验证 DB 建表
sqlite3 /var/lib/nsbc/nsbc.db ".tables" | grep -E "rate_plan|billing_trans"

# 推 trunk_map（Phase 1.5 L1 依赖）
echo 'trunk_map.reload [{"name":"china_unicom","id":1},{"name":"telecom","id":2}]' \
  | nc -U /var/run/nsbc/control.sock

# 手工充值一个账户（✅ Phase 1.6 已补：写 DB + 自动写 billing_transactions tx_type=initial）
curl -s -X POST http://localhost:8080/api/v1/billing/balances \
  -H 'Content-Type: application/json' \
  -d '{"tenant_id":1,"account_key":"tenant:1:extension:1000","balance_uc":1000000,"credit_limit_uc":0,"account_type":"extension"}'

# 手动 reload 账户到 core
echo 'accounts.reload [{"account_key":"tenant:1:extension:1000","balance":1000000,"credit_limit":0}]' \
  | nc -U /var/run/nsbc/control.sock

# 发起一通 010 通话（3+ 分钟）看日志
grep "billing.*pre-auth\|billing.*reserved\|billing.*billsec" logs/nsbcd.log

# ✅ Phase 1.6 已补：billing_transactions 会有 initial / topup / call_charge / refund 四种 tx_type
sqlite3 /var/lib/nsbc/nsbc.db "SELECT * FROM billing_transactions ORDER BY id DESC LIMIT 5;"
```

详细看 [billing-state.md](./billing-state.md)。

---

## 1. 完成清单

### 1.1 DB Layer（Phase 1，`bfd4696`）

| 项目 | 完成 | 文件 |
|------|------|------|
| 4 张新表 DDL（三后端 tri-branch）| ✅ | web/src/db/mod.rs L3025-3295 |
| accounts/rate_tables 升级迁移（幂等）| ✅ | sqlite PRAGMA + pg/mysql ALTER ADD COLUMN IF NOT EXISTS |
| RatePlan / RatePlanEntry / BillingTransaction / TrunkRateOverride struct | ✅ | L6269-6385 |
| Account struct 扩展新字段 | ✅ | L6231-6243 |
| 全套 CRUD（list/get/create/update/delete）| ✅ | L7562-7930 |
| account_topup（✅ Phase 1.6 G2 已补：sqlx::Transaction + tx_type="topup" 流水）| ✅ | L8302 |
| settle_billing（✅ sqlx::Transaction — 有写流水的路径）| ✅ | L7999-8026 |
| cargo check 零错误 | ✅ | 45 预存 warning 未新增 |

### 1.2 Sync Layer（Phase 1，`bfd4696`）

| 项目 | 完成 | 文件 |
|------|------|------|
| Kind 枚举 + RatePlans / RatePlanEntries | ✅ | web/src/sync.rs |
| snapshot + push_kind 完整链路 | ✅ | |
| CoreApi reload_rate_plans / reload_rate_plan_entries | ✅ | web/src/coreclient.rs |
| RealCore + MockCore 两套 impl | ✅ | |
| 自动继承 300ms debounce / 退避 / periodic / HELLO 重连 | ✅ | 框架通用 |

### 1.3 C Core 阶梯算法（Phase 1，`1820270`）

| 项目 | 完成 | 文件 |
|------|------|------|
| rate_entry_t 扩展 9 个字段（向后兼容）| ✅ | src/include/rating.h |
| calc_stair_charge() 逐阶梯分段 + bill_inc_s 取整 + min_bill 兜底 | ✅ | src/mod/billing/rating.c |
| rate_lookup_stair() 前缀最长 + trunk + 时段过滤 + tier 排序 | ✅ | |
| time_slot_match() HH:MM 跨天 + weekdays 匹配 | ✅ | |
| json_parse_entry() jstr/jint 手写 JSON | ✅ | |
| rate_plan_entries_reload_json() / rate_plans_reload_json() | ✅ | |
| OAM rate_plans.reload / rate_plan_entries.reload | ✅ | src/oam/obs_control.c |
| show rates 输出含 tier/trunk/time/prec | ✅ | |

### 1.4 End-to-End Reserve/Settle（Phase 1，`3aa0fb8`）

| 项目 | 完成 | 文件 |
|------|------|------|
| acct_t 扩展 4 新字段（tenant_id/account_type/rate_plan_id/low_threshold）| ✅ | src/mod/billing/rating.c |
| acct_find_locked() — 已持锁版本 | ✅ | |
| account_reload_json 重写：持 wrlock + 直接 upsert 全部 8 字段 | ✅ | |
| billing_make_key() — 按 dir 推导 account key | ✅ | src/mod/billing/billing.c |
| billing_now_hhmm() — 当前时间 | ✅ | |
| billing_cdr_cb 接 reserve/settle | ✅ | |
| ANSWER → reserve（保守策略：×2 + fee）| ✅ | |
| END → settle（多退少补 + OVERDRAWN 检测）| ✅ | |
| CDR 原有 cdr_sink_emit 保留 | ✅ | |

### 1.5 HTTP API（Phase 1，`3aff3b6`）

| 项目 | 完成 | 文件 |
|------|------|------|
| Rate Plans CRUD | ✅ | web/src/routes/billing.rs |
| Rate Plan Entries | ✅ | |
| Billing Transactions GET 查询 | ✅ | |
| reload 命令 | ✅ | |
| delete_rate_plan 前置检查 | ✅ | |
| cargo check 零错误 | ✅ | |

### 1.6 Phase 1.5 — 三项 P0 遗留补完（本轮）

| 项目 | 状态 | 说明 |
|------|------|------|
| **L3** resv_t 加 call_id + bill_settle 精确匹配 | ✅ 已完成 | **修复了根本 bug**：billing.c 把 acct_key 当 resv_id 传给旧 bill_settle —— settle 永远找不到 reservation。现在 bill_reserve 绑定 call_id，bill_settle(key, call_id, actual_uc) 精确匹配 |
| **L2** INVITE pre-auth | ✅ 已完成 | 在 b2bua.c security_check_fence 之后接 billing_pre_auth(caller, callee, tenant, src_ip)。caller 是短 extension + 全数字才检查；PERM_DENIED → 静默丢（与 security fence 同一惯例）；Emergency 跳过 |
| **L1** trunk_name → trunk_id 映射 | ✅ 已完成 | billing_t 加 trunk_map[64]；OAM 新增 `trunk_map.reload <json>`；billing.c ANSWER/END 用 billing_lookup_trunk_id(ev->trunk) 替换硬编码 trunk_id=0 |

### 1.7 Phase 2 — 全局开关 + L4 tick + L5 kill + OAM + Web + 前端（✅ 全部完成 + 审计修复）

| 项目 | 状态 | 说明 |
|------|------|------|
| **P0** `billing.enabled` 全局开关（YAML + CGF_HOT 热更新）| ✅ 已完成 | cfg_schema.c 注册 + billing_t.enabled + bill_is_enabled/set_enabled + 所有计费入口 guard |
| **L4v1→v2** 精确费率 tick 追加 reserve | ✅ 已完成 | bill_resv_fill 补填 called/trunk_id → tick_lookup_tiers_locked → calc_stair_charge |
| **L4v3** reserve 收缩（费率变化退款） | ✅ 已完成 | 安全 guard = connect_fee + price × 120s/60 |
| **L4v4** 批量 reserve 维护（预分配 scratch）| ✅ 已完成 | bill_create 预分配 per_acct_delta → tick 零 malloc |
| **L5** OVERDRAWN 强制 kill（边沿检测 + 滞回）| ✅ 已完成 | overdrawn_gate + kill_list 收集 + b2b_terminate_callid |
| **OAM** `show resv` 命令 | ✅ 已完成 | obs_control.c 注册 + bill_foreach_reservation |
| **Web** 3 新 REST API | ✅ 已完成 | GET/PUT /enabled + GET /reservations/core |
| **前端** Billing 管理页面 | ✅ 已完成 | Vue 3 CDN + 启停开关 + Reservation 实时表格 |
| **审计** alloca(200000×8) 栈溢出 → 预分配 | ✅ 已修复 | P0 红线 |
| **审计** tick LOG_INFO → LOG_DEBUG | ✅ 已修复 | P0 红线 |
| **审计** tick strlen(called) → called_len 预存 | ✅ 已修复 | P1 性能 |
| **审计** L4 保守模式计数器 | ✅ 已修复 | P2 可观测 |

---

## 2. 遗留 / 未实现

### Phase 2 — L4/L5 + 开关 + OAM + Web 全部完成 ✅

| # | 项 | 复杂度 | 完成版本 |
|---|----|--------|---------|
| **L4** | 实时扣费 tick → **L4v2 精确费率追加 + L4v3 收缩 + L4v4 批量维护** | L | Phase 2 |
| **L5** | OVERDRAWN 强制 kill 同账户所有通话（边沿检测 + 滞回）| L | Phase 2 |
| **P0** | billing.enabled 全局开关（CGF_HOT 热更新）| S | Phase 2 |
| **OAM** | show resv 命令 + bill_foreach_reservation | S | Phase 2 |
| **Web** | GET/PUT /enabled + GET /reservations/core | S | Phase 2 |
| **前端** | Billing 管理页面（启停开关 + Reservation 实时表格）| S | Phase 2 |
| **审计 P0** | alloca 栈溢出 + LOG_INFO 热路径 | S | Phase 2 审计 |
| **审计 P1** | tick strlen 热路径 | S | Phase 2 审计 |

### P2 — 高级（未启动）

| # | 项 | 复杂度 |
|---|----|--------|
| **L7** | 周期性扣费（包月/按天 cron）| L |
| **L8** | 反欺诈：高额/高频通话实时告警 | L |
| **L9** | IVR collect / cond / 时段路由 | XL |
| **CDR** | CDR 表加 charge_uc / rate_plan_id / billing_status 3 列 | M |

---

## 3. 踩过的坑（别重蹈覆辙）

### K1: `account_reload_json` 嵌套 wrlock

**现象**：原实现调 `account_open(b, key, balance, credit)`，后者内部 `pthread_rwlock_wrlock(&b->lock)` —— 但 account_reload_json 调用前**已经** wrlock，导致死锁。

**修复**：抽出 `acct_find_locked()` —— 不获取锁版本。account_reload_json 自己持 wrlock，内部直接调 locked 版本。

**教训**：函数级 API（account_open 带锁）和批量操作（account_reload_json 持锁）混用 → 必须提供 locked 内部版本。

### K2: `rate_lookup_stair` trunk_id=0 匹配陷阱（Phase 1.5 已补）

**现象**：billing_cdr_cb 传 trunk_id=0（从 cdr_event_t.trunk 字符串转不了 int64）。rate_plan_entries 如果配置了 trunk_id=unicom_id 就跳过了。

**修复（Phase 1.5）**：billing_lookup_trunk_id() 做 name→id 映射。没推 trunk_map 时返回 0（仍兜底），推了就精确匹配。

### K3: `bill_settle` 签名变更 —— **Phase 1.5 修复了根本 bug**

**现象**：billing.c 写的是 `bill_settle(g_billing, acct_key, charge)` —— 但旧 API 签名是 `bill_settle(b, resv_id, actual_uc)`。acct_key 是 `const char*`，强转成 `uint64_t resv_id` 传入 —— 指针值当 ID 用，`resv_find` 永远找不到。

**根因**：resv_t 没有 call_id 关联，旧设计是 `bill_settle(account_key, amount)` 扫账户下所有 reservation —— 但代码根本没实现这个版本！

**修复（Phase 1.5）**：重写 bill_settle 签名为 `(b, key, call_id, actual_uc)`，用 resv_find_call_id(key, call_id) 精确匹配。resv_t 加 `call_id[40]`；bill_reserve 存 call_id。

### K4: cargo check 通不代表生产可用

**一直**：Windows 不编译 C 代码。远端 `make clean && make -j$(nproc)` 是必须的。

### K5: rate_lookup_stair 返回顺序与 tier 插入顺序

**修复**：rate_lookup_stair 末尾有插入排序 `tier_min_sec asc, precedence desc`。

### K6: 时段跨天

**修复**：`time_in_range()` 检测 `from > to` → 跨天 → `now >= from || now < to`。

### K7: rate_plan_entries 和 rate_tables 共存

**OK**：同一个 prefix 可能有多条（单费率旧条目 + 多条阶梯新条目）。rate_lookup_stair 全部收集 + 排序。

### K8: billing_pre_auth 不能外部先持锁再调 acct_find（Phase 1.5 新增）

**教训**：acct_find() 内部自己拿 rdlock/unlock。如果外部已经持了 rdlock 再调 acct_find → 嵌套锁（rwlock 是递归锁还是非递归？取决于 pthread 实现，不可靠）。

**修复**：billing_pre_auth 里先 account_balance（自带 rdlock/unlock），再 acct_find（自带 rdlock/unlock）—— 两次独立加锁，无嵌套。

### K9: trunk_map_reload_json extern 声明签名不一致（Phase 1.5 新增）

**教训**：ivr.h 公开的 `jstr/jint/jarray_at` 签名和我 extern 声明的不一致（jstr 有 dst+cap 参数，jint 返回 int 不是 int64）。直接用 ivr.h 头文件声明或 include ivr.h 而不是手写 extern。

**修复**：trunk_map_reload_json 正确使用 `extern int jstr(const char*, const char*, char*, size_t)` 等。

### K10: `alloca(200000*8) = 1.6MB` 栈溢出（Phase 2 审计）

**现象**：BILL_ACCT_MAX=200000，tick_lookup_tiers_locked + alloca(BILL_ACCT_MAX × 8) 当 per_acct_delta buffer。tick 跑在 worker 线程上（栈 1-2MB），直接 segfault。

**修复**：billing_t 结构体加 `int64_t *per_acct_delta` 字段，bill_create 时 calloc(BILL_ACCT_MAX × 8) 和 accts[] 同生命周期，bill_destroy free。tick 零 malloc。

**教训**：热路径栈分配看清楚上限——BILL_ACCT_MAX 是 20w 不是 4096。用预分配 scratch buffer 替代 alloca。

### K11: tick 热路径 LOG_INFO（Phase 2 审计）

**现象**：L4v2+/L4v3-/L4v4 done 三条 tick 日志用 LOG_INFO，每 3s × 活跃通话数输出 → 生产日志爆炸。

**修复**：全部降级 LOG_DEBUG。L5 OVERDRAWN kill 是偶发状态变更，保留 LOG_INFO。

**教训**：热路径（每 3s 或更快）日志只能 LOG_DEBUG / LOG_TRACE。LOG_INFO 用于状态变更。

### K12: `struct nsbc_ctx` 参数列表内声明不可见（Phase 2 编译错误）

**现象**：rating.h 函数声明 `int billing_tick(struct nsbc_ctx *ctx)` —— C 里 struct nsbc_ctx 在参数列表内声明不会向外传播类型。billing.c 用 `nsbc_ctx_t *` 签名冲突。

**修复**：rating.h 顶部加 `typedef struct nsbc_ctx nsbc_ctx_t;` 前向声明。

**教训**：跨模块类型引用用 typedef 前向声明，不要强制 include 头文件。

### K13: `strlen(called)` 在 tick 热路径每 resv 重算（Phase 2 审计）

**现象**：tick_lookup_tiers_locked 每 resv 调 `size_t clen = strlen(called)` —— called 是固定 48 字节 buffer，每 3s × 4096 槽重扫一次。

**修复**：resv_t 加 `uint8_t called_len`，bill_resv_fill 时一次性算好存进去。tick_lookup_tiers_locked 改成接收 `size_t clen` 参数，零 strlen。

**教训**：热路径上固定长度字符串的 strlen 可以在写入点一次性算好存字段。

---

## 4. 关键路径代码索引

| 功能 | 文件 | 函数/行 | 备注 |
|------|------|---------|------|
| DB DDL (4 新表) | [db/mod.rs](file:///d:/github/nsbc/web/src/db/mod.rs) | L3025-3295 | 三后端 tri-branch |
| DB upgrade migrations | 同上 | L3217-3295 | sqlite PRAGMA + pg/mysql ALTER |
| **topup_account**（✅ Phase 1.6 G2 已补：Transaction + tx_type=topup 流水）| 同上 | L8302 | sqlx::Transaction 包 UPDATE + INSERT billing_transactions |
| **settle_billing**（✅ 唯一有流水的路径）| 同上 | L7999-8026 | sqlx::Transaction 包 UPDATE + INSERT |
| sync Kind 枚举 | [sync.rs](file:///d:/github/nsbc/web/src/sync.rs) | L101+ | RatePlans / RatePlanEntries |
| OAM rate_plans.reload | [obs_control.c](file:///d:/github/nsbc/src/oam/obs_control.c) | ~L1420 | |
| **OAM trunk_map.reload（Phase 1.5）** | 同上 | [L1464-L1475](file:///d:/github/nsbc/src/oam/obs_control.c#L1464-L1475) | 新增命令 |
| rate_entry_t struct | [rating.h](file:///d:/github/nsbc/src/include/rating.h) | L26+ | |
| **resv_t call_id + billing_t trunk_map（Phase 1.5）** | [rating.c](file:///d:/github/nsbc/src/mod/billing/rating.c) | [L34](file:///d:/github/nsbc/src/mod/billing/rating.c#L34) · [L60](file:///d:/github/nsbc/src/mod/billing/rating.c#L60) | |
| **bill_reserve（Phase 1.5 加 call_id）** | 同上 | [L287](file:///d:/github/nsbc/src/mod/billing/rating.c#L287) | |
| **resv_find_call_id（Phase 1.5 核心匹配）** | 同上 | [L340](file:///d:/github/nsbc/src/mod/billing/rating.c#L340) | |
| **bill_settle（Phase 1.5 重写）** | 同上 | [L386](file:///d:/github/nsbc/src/mod/billing/rating.c#L386) | 按 call_id 精确匹配 |
| **billing_pre_auth（Phase 1.5 新增）** | 同上 | [L846](file:///d:/github/nsbc/src/mod/billing/rating.c#L846) | INVITE 入口余额检查 |
| **billing_lookup_trunk_id / trunk_map_reload_json** | 同上 | [L947](file:///d:/github/nsbc/src/mod/billing/rating.c#L947) · [L963](file:///d:/github/nsbc/src/mod/billing/rating.c#L963) | Phase 1.5 L1 |
| **billing_cdr_cb（Phase 1.5 用 trunk_id + call_id）** | [billing.c](file:///d:/github/nsbc/src/mod/billing/billing.c) | [L114-L219](file:///d:/github/nsbc/src/mod/billing/billing.c#L114-L219) | ANSWER L160 / END L198 |
| **b2bua.c pre-auth 接入口（Phase 1.5）** | [b2bua.c](file:///d:/github/nsbc/src/sip/b2bua.c) | [L3545-L3571](file:///d:/github/nsbc/src/sip/b2bua.c#L3545-L3571) | security fence 之后、HA 之前 |
| HTTP routes | [billing.rs](file:///d:/github/nsbc/web/src/routes/billing.rs) | router() + handlers | |

---

## 5. 远端验证 Checklist

### Phase 1 基础验证

- [ ] `git pull && make -j$(nproc) && systemctl restart nsbcd && systemctl restart nsbc-web`
- [ ] DB 自动建表：`sqlite3 /var/lib/nsbc/nsbc.db ".tables"` 有 4 张新表
- [ ] accounts 表 4 个新列存在
- [ ] curl POST /api/v1/billing/rate-plans → 201
- [ ] curl POST /api/v1/billing/rate-plan-entries (2 条 tier) → 201
- [ ] curl POST /api/v1/billing/rate-plan-entries/reload → `{loaded: 2}`
- [ ] `show rates` 能看到 tier/trunk/time/prec 字段
- [ ] 发起一通 010 通话（3+ 分钟）
- [ ] `grep "billing.*reserved"` 有 ANSWER reserve 日志
- [ ] `grep "billing.*settle=OK"` 有 END settle 日志

### Phase 1.5 新增验证

- [ ] `trunk_map.reload [{"name":"china_unicom","id":1}]` → OK loaded
- [ ] 发起一通走 china_unicom 中继的 010 通话
- [ ] `grep "billing.*reserved"` 日志里能看到 trunk_id 匹配（或至少没错误提示）
- [ ] 分机 1000 余额设 0 → 发起 INVITE → 看是否被静默丢
- [ ] `grep "billing.*pre-auth DENY"` 有日志

### Phase 1.6 必须验证（远端跑通前）

- [ ] topup 10 元后 `SELECT * FROM billing_transactions WHERE tx_type='topup'` 有记录
- [ ] 通话结束后 `SELECT * FROM billing_transactions WHERE tx_type='call_charge'` 有记录
- [ ] 两者 balance_after_uc 能串起来对账

---

## 6. 推进路线

```
✅ Phase 1.6 — 资金闭环（已完成）
  ✅ G1 C→Rust UDS billing_settle notify → settle_billing DB 流水
  ✅ G2 topup_account Transaction + tx_type="topup"
  ✅ G3 refund_account + /balances/:id/refund route
  ✅ G4 open_account 写 tx_type="initial"
  ✅ L6 settle_billing 自动触发（G1 连带）

✅ Phase 2 — 生产必备 + 审计修复（已完成）
  ✅ billing.enabled 全局开关（YAML + CGF_HOT 热更新）
  ✅ L4v2 精确费率 tick 追加 reserve（tick_lookup_tiers_locked + calc_stair_charge）
  ✅ L4v3 reserve 收缩（费率变化退款，guard=connect_fee+price×120s）
  ✅ L4v4 批量维护（billing_t.per_acct_delta 预分配 scratch buffer，tick 零 malloc）
  ✅ L5 OVERDRAWN kill（边沿检测 + 滞回防抖动，kill_list ≤ 256）
  ✅ OAM show resv（bill_foreach_reservation）
  ✅ Web 3 新 API（GET/PUT /enabled + GET /reservations/core）
  ✅ 前端 Billing 页面（启停开关 + Reservation 实时表格，5s 刷新）
  ✅ 审计 P0: alloca 栈溢出 → 预分配
  ✅ 审计 P0: tick LOG_INFO → LOG_DEBUG
  ✅ 审计 P1: tick strlen(called) → called_len 预存
  ✅ 审计 P2: 保守模式计数器

Phase 3 — IVR 增强 + 高级（未启动）
  IVR-1 collect 节点 + DTMF 收集 + store_as call_vars
  IVR-2 cond 条件分支节点
  IVR-3 ivr_sess call_vars 字典
  IVR-4 时段路由 schedule
  IVR-5 ivr_node_t 扩展 + JSON 解析
  IVR-6 Web collect/cond 可视化编辑器
  L7 周期性扣费 cron
  L8 反欺诈
```

---

## 7. 变更的完整文件清单（累计）

```
Phase 1:
C Core:
  [rating.h](file:///d:/github/nsbc/src/include/rating.h)               (rate_entry_t 扩展 + 4 新 API)
  [rating.c](file:///d:/github/nsbc/src/mod/billing/rating.c)           (阶梯算法 + 账户扩展 + JSON 解析 + acct_find_locked)
  [billing.c](file:///d:/github/nsbc/src/mod/billing/billing.c)         (billing_cdr_cb reserve/settle 接线)
  [obs_control.c](file:///d:/github/nsbc/src/oam/obs_control.c)         (rate_plans.reload + rate_plan_entries.reload + show rates)

Web Rust:
  web/src/db/mod.rs                  (4 新表 DDL + 升级迁移 + structs + CRUD + Account 扩展)
  web/src/sync.rs                    (Kind::RatePlans / RatePlanEntries)
  web/src/coreclient.rs              (reload_rate_plans / reload_rate_plan_entries)
  web/src/routes/billing.rs          (Rate Plans / Rate Plan Entries / Transactions HTTP API)

Phase 1.5（本轮增量）:
C Core:
  [rating.h](file:///d:/github/nsbc/src/include/rating.h#L110-L170)
    bill_reserve / bill_settle 改签名（加 call_id 参数）
    新增 billing_pre_auth / billing_lookup_trunk_id / trunk_map_reload_json

  [rating.c](file:///d:/github/nsbc/src/mod/billing/rating.c)
    L34: resv_t 加 call_id[40]
    L60: billing_t 加 trunk_map[BILL_TRUNK_MAP_MAX]
    L340: 新增 resv_find_call_id() — Phase 1.5 L3 核心
    L386: 重写 bill_settle() — 按 call_id 精确匹配
    L846: 新增 billing_pre_auth() — Phase 1.5 L2
    L947: 新增 billing_lookup_trunk_id() — Phase 1.5 L1
    L963: 新增 trunk_map_reload_json() — Phase 1.5 L1
    #include <ctype.h>/<string.h>/<time.h>

  [billing.c](file:///d:/github/nsbc/src/mod/billing/billing.c#L114-L219)
    L160: bill_reserve(acct_key, reserve, ev->call_id, &resv_id)
    L198: bill_settle(acct_key, ev->call_id, charge)
    ANSWER/END 都加 billing_lookup_trunk_id(ev->trunk)

  [b2bua.c](file:///d:/github/nsbc/src/sip/b2bua.c#L3545-L3571)
    L29: #include "rating.h"
    L3563: billing_pre_auth(cctx->billing, caller, callee, NULL, srcip)
    L3568: PERM_DENIED → return（静默丢）

  [obs_control.c](file:///d:/github/nsbc/src/oam/obs_control.c#L1464-L1475)
    L1464: trunk_map.reload <json> 命令处理器

Phase 2（本轮增量 + 审计修复）:
C Core:
  [cfg_schema.c](file:///d:/github/nsbc/src/core/cfg_schema.c#L171-L175)
    注册 `billing.enabled`（CGT_BOOL, CGF_HOT, 默认 false）

  [rating.h](file:///d:/github/nsbc/src/include/rating.h)
    L70: typedef struct nsbc_ctx nsbc_ctx_t（前向声明）
    新增 bill_is_enabled / bill_set_enabled / bill_resv_fill / billing_tick_internal / billing_tick
    新增 resv_iter_cb 回调类型 + bill_foreach_reservation / bill_reservation_count

  [rating.c](file:///d:/github/nsbc/src/mod/billing/rating.c)
    resv_t 加 answer_ms / last_bill_sec / called[48] / called_len(uint8_t) / trunk_id
    billing_t 加 enabled + per_acct_delta（bill_create calloc, bill_destroy free）
    bill_resv_fill（补填 called/trunk_id + called_len）
    tick_lookup_tiers_locked（加 clen 参数，tick 零 strlen）
    calc_stair_charge 内部复用（精度优先，不预存 price_per_second）
    billing_tick_internal（L4v2+L4v3+L4v4 批量维护 + L5 OVERDRAWN kill 门闩）
    bill_foreach_reservation / bill_reservation_count（OAM show resv）

  [billing.c](file:///d:/github/nsbc/src/mod/billing/billing.c)
    CDR hook 入口加 enabled guard
    ANSWER reserve 后调 bill_resv_fill 补填 called/trunk_id
    billing_init 读 cfg + set_enabled
    新增 billing_tick() 对外入口（3s 节流 + 每 tick 同步 enabled）

  [b2bua.c](file:///d:/github/nsbc/src/sip/b2bua.c)
    pre-auth 调用点加 bill_is_enabled guard

  [main.c](file:///d:/github/nsbc/src/core/main.c)
    #include "rating.h"
    ticker_main 每 ~1s 调 billing_tick()

  [obs_control.c](file:///d:/github/nsbc/src/oam/obs_control.c)
    新增 show resv 命令 + resv_show_cb 回调

  [conf/nsbc.yaml](file:///d:/github/nsbc/conf/nsbc.yaml) + [configs/nsbc.yaml](file:///d:/github/nsbc/configs/nsbc.yaml)
    加 billing.enabled: false 默认条目

Web Rust:
  [coreclient.rs](file:///d:/github/nsbc/web/src/coreclient.rs)
    新增 ResvEntry / ResvList 结构体
    新增 show_resv CoreApi trait 方法 + parse_show_resv + 真实/mock 实现

  [routes/billing.rs](file:///d:/github/nsbc/web/src/routes/billing.rs)
    新增 GET /reservations/core + GET/PUT /enabled 三个 endpoint

前端:
  [app-core.js](file:///d:/github/nsbc/web/static/js/app-core.js)
    菜单加 /billing 路由

  [index.html](file:///d:/github/nsbc/web/static/index.html)
    模板加 billing-page 挂载点

  [pages-live.js](file:///d:/github/nsbc/web/static/js/pages-live.js)
    新增 billing-page 组件（启停开关 + Reservation 实时表格，5s 刷新）
```
