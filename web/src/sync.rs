//! Web DB → C core full configuration push (REQ-DATA-SVC-003, hybrid sync S4/S5).
//!
//! Data flow: Web database (sqlite/postgres/mysql) is the single source of truth →
//! control.sock `*.reload <json>` command → C-side in-memory table whole-table replacement.
//!
//! Triggers:
//!   - UDS HELLO ([`crate::uds_server`]): core process start/reconnect succeeds →
//!     [`on_hello`] pushes all four (extensions → trunks → dialplan-rules → sip-profiles)
//!   - After CRUD save (various routes handlers): [`spawn_push`] single-kind push
//!     (300ms debounced merge)
//!
//! Reliability: each kind retries independently (backoff 500ms→8s, 5 times total);
//! one kind failure does not block others; if all retries are exhausted it logs ERROR
//! and waits for the next trigger (HELLO reconnect / CRUD / manual reload endpoint).
//!
//! Security: extension push goes through [`ExtSyncDto`] (no plaintext password, only
//! HA1 is sent down).

use std::collections::HashMap;
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{SystemTime, UNIX_EPOCH};

use crate::AppState;
use crate::coreclient::{CoreApi, CoreError};

/// Extension sync DTO: DB Extension → C ext_table_reload_json.
/// Password never leaves Web side (C side only needs HA1 for Digest verification).
#[derive(Debug, serde::Serialize)]
pub struct ExtSyncDto {
    pub extension:    String,
    pub realm:        String,
    pub ha1:          String,
    pub enabled:      bool,
    pub clip:         String,
    pub clir:         bool,
    pub max_contacts: i64,
}

impl From<&crate::db::Extension> for ExtSyncDto {
    fn from(x: &crate::db::Extension) -> Self {
        Self {
            extension:    x.extension.clone(),
            realm:        x.realm.clone(),
            ha1:          x.ha1.clone(),
            enabled:      x.enabled != 0,
            clip:         x.clip.clone(),
            clir:         x.clir != 0,
            max_contacts: x.max_contacts,
        }
    }
}

/// Queue sync DTO: DB Queue → C queue_table_reload_json.
/// Field names/types strictly match C-side parsing schema (the reload handler in queue.c).
///
/// | DTO field        | C-side purpose       | Rust source                  |
/// |------------------|----------------------|------------------------------|
/// | tenant           | Match queue slot by tenant | tenant_id:i64 → format!("{id}") |
/// | name             | Queue unique name    | name: String                 |
/// | enabled          | json_truthy bool    | enabled:i64 != 0             |
/// | strategy         | CQ_STRATEGY_* parsing | ring_strategy: String      |
/// | max_wait_s       | Timeout overflow (s) | max_wait_sec: i64            |
/// | wrap_time_s      | Agent after-call wrap-up (s) | currently hardcoded 30
/// | require_skill    | Skill routing filter | currently "" (Rust-side field TBD) |
/// | members          | Member extension list | members_json: String → parse Vec<String> |
#[derive(Debug, serde::Serialize)]
pub struct QueueSyncDto {
    pub tenant:       String,
    pub name:         String,
    pub enabled:      bool,
    pub strategy:     String,
    pub max_wait_s:   i64,
    pub wrap_time_s:  i64,
    pub require_skill: String,
    pub members:      Vec<String>,
}

/// Security-blacklist sync DTO: DB SecurityBlacklist → C security_blacklist_reload_json.
/// Field names strictly match C-side parsing schema (security.c: kind/type/value/comment/enabled).
#[derive(Debug, serde::Serialize)]
pub struct BlacklistSyncDto {
    pub kind:       String,
    #[serde(rename = "type")]
    pub rule_type:  String,
    pub value:      String,
    pub comment:    String,
    pub enabled:    bool,
}

impl From<&crate::db::SecurityBlacklist> for BlacklistSyncDto {
    fn from(x: &crate::db::SecurityBlacklist) -> Self {
        Self {
            kind:       x.kind.clone(),
            rule_type:  x.rule_type.clone(),
            value:      x.value.clone(),
            comment:    x.comment.clone(),
            enabled:    x.enabled != 0,
        }
    }
}

/// Config kinds that can be pushed (one-to-one mapping with C-side reload commands).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Kind {
    Extensions,
    Trunks,
    DialplanRules,
    SipProfiles,
    Queues,
    SecurityBlacklist,
    Gateways,
    HaGroups,
    IvrMenus,
    RouteGroups,
    Rates,
    Accounts,
    RatePlans,
    RatePlanEntries,
}

impl Kind {
    pub const ALL: [Kind; 14] = [
        Kind::Extensions,
        Kind::Trunks,
        Kind::DialplanRules,
        Kind::SipProfiles,
        Kind::Queues,
        Kind::SecurityBlacklist,
        Kind::Gateways,
        Kind::HaGroups,
        Kind::IvrMenus,
        Kind::RouteGroups,
        Kind::Rates,
        Kind::Accounts,
        Kind::RatePlans,
        Kind::RatePlanEntries,
    ];
    fn name(self) -> &'static str {
        match self {
            Kind::Extensions        => "extensions",
            Kind::Trunks            => "trunks",
            Kind::DialplanRules     => "dialplan-rules",
            Kind::SipProfiles       => "sip-profiles",
            Kind::Queues            => "queues",
            Kind::SecurityBlacklist => "security-blacklist",
            Kind::Gateways          => "gateways",
            Kind::HaGroups          => "ha-groups",
            Kind::IvrMenus          => "ivr-menus",
            Kind::RouteGroups       => "route-groups",
            Kind::Rates             => "rates",
            Kind::Accounts          => "accounts",
            Kind::RatePlans         => "rate-plans",
            Kind::RatePlanEntries   => "rate-plan-entries",
        }
    }
}

/// Query DB → build JSON (single-kind full snapshot).
async fn snapshot<C: CoreApi + Clone + Send + Sync>(
    st: &Arc<AppState<C>>,
    kind: Kind,
) -> Result<(String, usize), String> {
    match kind {
        Kind::Extensions => {
            let rows = st
                .db
                .list_enabled_extensions()
                .await
                .map_err(|e| format!("db: {e}"))?;
            let dtos: Vec<ExtSyncDto> = rows.iter().map(ExtSyncDto::from).collect();
            let n = dtos.len();
            serde_json::to_string(&dtos).map(|j| (j, n)).map_err(|e| e.to_string())
        }
        Kind::Trunks => {
            let rows = st
                .db
                .list_enabled_trunks()
                .await
                .map_err(|e| format!("db: {e}"))?;
            let n = rows.len();
            serde_json::to_string(&rows).map(|j| (j, n)).map_err(|e| e.to_string())
        }
        Kind::DialplanRules => {
            let rows = st
                .db
                .list_enabled_dialplan_rules(None)
                .await
                .map_err(|e| format!("db: {e}"))?;
            let n = rows.len();
            serde_json::to_string(&rows).map(|j| (j, n)).map_err(|e| e.to_string())
        }
        Kind::SipProfiles => {
            let rows = st
                .db
                .list_enabled_sip_profiles()
                .await
                .map_err(|e| format!("db: {e}"))?;
            let n = rows.len();
            serde_json::to_string(&rows).map(|j| (j, n)).map_err(|e| e.to_string())
        }
        Kind::Queues => {
            let rows = st
                .db
                .list_enabled_queues()
                .await
                .map_err(|e| format!("db: {e}"))?;
            // rows is Vec<(Queue, tenant_name)> — realm from JOIN with tenants
            let dtos: Vec<QueueSyncDto> = rows.into_iter().map(|(q, tenant_name)| {
                let members: Vec<String> = serde_json::from_str(&q.members_json).unwrap_or_default();
                QueueSyncDto {
                    tenant:       tenant_name,
                    name:         q.name.clone(),
                    enabled:      q.enabled != 0,
                    strategy:     q.ring_strategy.clone(),
                    max_wait_s:   q.max_wait_sec,
                    wrap_time_s:  30,
                    require_skill: String::new(),
                    members,
                }
            }).collect();
            let n = dtos.len();
            serde_json::to_string(&dtos).map(|j| (j, n)).map_err(|e| e.to_string())
        }
        Kind::SecurityBlacklist => {
            let rows = st
                .db
                .list_enabled_security_blacklist()
                .await
                .map_err(|e| format!("db: {e}"))?;
            let dtos: Vec<BlacklistSyncDto> = rows.iter().map(BlacklistSyncDto::from).collect();
            let n = dtos.len();
            let cfg = st.db.get_security_config().await
                .map_err(|e| format!("db: {e}"))?;
            let obj = serde_json::json!({
                "fence_mode":                cfg.fence_mode,
                "method_blocked":            cfg.method_blocked,
                "auto_ban":                  cfg.auto_ban,
                "authfail_threshold":        cfg.authfail_threshold,
                "authfail_window_s":         cfg.authfail_window_s,
                "ban_duration_s":            cfg.ban_duration_s,
                "ratelimit_regs":            cfg.ratelimit_regs,
                "ratelimit_window_s":        cfg.ratelimit_window_s,
                "ratelimit_invites":         cfg.ratelimit_invites,
                "ratelimit_invites_active":  cfg.ratelimit_invites_active,
                "rules":                     dtos,
            });
            serde_json::to_string(&obj).map(|j| (j, n)).map_err(|e| e.to_string())
        }
        Kind::Gateways => {
            let rows = st.db.list_enabled_gateways().await.map_err(|e| format!("db: {e}"))?;
            let n = rows.len();
            serde_json::to_string(&rows).map(|j| (j, n)).map_err(|e| e.to_string())
        }
        Kind::HaGroups => {
            let rows = st.db.list_enabled_ha_groups().await.map_err(|e| format!("db: {e}"))?;
            let n = rows.len();
            serde_json::to_string(&rows).map(|j| (j, n)).map_err(|e| e.to_string())
        }
        Kind::IvrMenus => {
            let rows = st.db.list_enabled_ivr_menus().await.map_err(|e| format!("db: {e}"))?;
            let n = rows.len();
            serde_json::to_string(&rows).map(|j| (j, n)).map_err(|e| e.to_string())
        }
        Kind::RouteGroups => {
            let rows = st.db.list_enabled_routes().await.map_err(|e| format!("db: {e}"))?;
            let n = rows.len();
            serde_json::to_string(&rows).map(|j| (j, n)).map_err(|e| e.to_string())
        }
        Kind::Rates => {
            let (rows, _total) = st.db.list_rates(None, Some(true), None, i64::MAX, 0).await
                .map_err(|e| format!("db: {e}"))?;
            let n = rows.len();
            serde_json::to_string(&rows).map(|j| (j, n)).map_err(|e| e.to_string())
        }
        Kind::Accounts => {
            /* accounts.reload 全量 upsert — DB 里所有账户。
             * C 侧 account_reload_json 是 upsert-only（不删 JSON 外账户，
             * 因为 resv[] 活跃 reservation 要结算），所以空数组=无新账户，
             * 已存在账户完全保留。 */
            let (rows, _total) = st.db.list_accounts(None, i64::MAX, 0).await
                .map_err(|e| format!("db: {e}"))?;
            let n = rows.len();
            serde_json::to_string(&rows).map(|j| (j, n)).map_err(|e| e.to_string())
        }
        Kind::RatePlans => {
            let rows = st.db.list_enabled_rate_plans().await
                .map_err(|e| format!("db: {e}"))?;
            let n = rows.len();
            serde_json::to_string(&rows).map(|j| (j, n)).map_err(|e| e.to_string())
        }
        Kind::RatePlanEntries => {
            let rows = st.db.list_enabled_rate_plan_entries().await
                .map_err(|e| format!("db: {e}"))?;
            let n = rows.len();
            serde_json::to_string(&rows).map(|j| (j, n)).map_err(|e| e.to_string())
        }
    }
}

// ---------------------------------------------------------------------------
// Per-kind cooldown — one kind's failure (e.g. extensions large payload framing
// overflow) must NOT block other kinds (dialplan-rules / sip-profiles / trunks)
// from retrying immediately. Previously a single global AtomicU64 cooldown
// caused a single kind's CoreError to skip all 14 kinds for 15s, which is the
// root cause of "extensions.reload broke, then dialplan-rules.reload never retried".
// ---------------------------------------------------------------------------
fn kind_cooldowns() -> &'static Mutex<HashMap<Kind, u64>> {
    static CDS: OnceLock<Mutex<HashMap<Kind, u64>>> = OnceLock::new();
    CDS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

fn kind_in_cooldown(kind: Kind) -> bool {
    let cds = kind_cooldowns().lock().unwrap();
    match cds.get(&kind) {
        Some(&until) if until > now_ms() => true,
        _ => false,
    }
}

fn mark_kind_down(kind: Kind) {
    let until = now_ms() + 15_000;
    let mut cds = kind_cooldowns().lock().unwrap();
    cds.insert(kind, until);
}

fn clear_kind_cooldown(kind: Kind) {
    let mut cds = kind_cooldowns().lock().unwrap();
    cds.remove(&kind);
}

/// C core 重新连上时，清除所有 kind 的 cooldown，让后续的 push_all 立即生效。
/// （否则 Web 启动后周期性同步先跑失败会让所有 kind 进入 15s cooldown，
///  C 连上触发的 push_all 会全部被跳过）。
pub fn clear_all_cooldowns() {
    let mut cds = kind_cooldowns().lock().unwrap();
    if !cds.is_empty() {
        let n = cds.len();
        cds.clear();
        tracing::info!(n, "C 侧连接 —— 清除了 {n} 个 kind cooldown");
    }
}

/// Single-kind push: snapshot → CoreApi.reload_*; retry on failure with [500ms,1s,2s,4s,8s].
/// Per-kind cooldown: a kind that fails at attempt==0 (CoreError) marks ONLY itself
/// cooldown 15s — other kinds are unaffected. This prevents one kind's large-payload
/// framing overflow from blacking out the entire config push pipeline.
///
/// `bypass_cooldown=true` 用于 push_all（C 连接触发的最高优先级路径）—— 它必须
/// 绕过 cooldown 检查，且 attempt==0 失败也不能标记 cooldown（可能是 extensions
/// 仍在处理中，60s timeout 还没到）。
async fn push_kind<C: CoreApi + Clone + Send + Sync>(
    st: &Arc<AppState<C>>,
    kind: Kind,
    bypass_cooldown: bool,
) -> Result<(), String> {
    // Per-kind cooldown bail — save CPU and disk I/O for THIS kind only.
    // 但 push_all (C 连接) 必须绕过！否则周期同步先跑失败 → 14 kinds cooldown →
    // C 刚连上触发的 push_all 立即全部被挡。
    if !bypass_cooldown && kind_in_cooldown(kind) {
        return Err(format!("core unreachable ({kind:?} cooldown)"));
    }

    const DELAYS_MS: [u64; 5] = [500, 1_000, 2_000, 4_000, 8_000];

    for attempt in 0..=DELAYS_MS.len() {
        let (json, n) = match snapshot(st, kind).await {
            Ok(v) => v,
            Err(e) => {
                // DB read failure also goes through retry (connection pool transient jitter)
                tracing::warn!(kind = kind.name(), attempt, error = %e, "sync snapshot failed");
                if attempt == DELAYS_MS.len() {
                    return Err(e);
                }
                tokio::time::sleep(std::time::Duration::from_millis(DELAYS_MS[attempt])).await;
                continue;
            }
        };
        let r = match kind {
            Kind::Extensions    => st.core.reload_extensions(&json).await,
            Kind::Trunks        => st.core.reload_trunks(&json).await,
            Kind::DialplanRules => st.core.reload_dialplan_rules(&json).await,
            Kind::SipProfiles   => st.core.reload_sip_profiles(&json).await,
            Kind::Queues        => st.core.reload_queues(&json).await,
            Kind::SecurityBlacklist => st.core.reload_security_blacklist(&json).await,
            Kind::Gateways          => st.core.reload_gateways(&json).await,
            Kind::HaGroups          => st.core.reload_ha_groups(&json).await,
            Kind::IvrMenus          => st.core.reload_ivr_menus(&json).await,
            Kind::RouteGroups       => st.core.reload_route_groups(&json).await,
            Kind::Rates             => st.core.reload_rates(&json).await,
            Kind::Accounts          => st.core.reload_accounts(&json).await,
            Kind::RatePlans         => st.core.reload_rate_plans(&json).await,
            Kind::RatePlanEntries   => st.core.reload_rate_plan_entries(&json).await,
        };
        match r {
            Ok(()) => {
                tracing::info!(kind = kind.name(), count = n, attempt, "pushed to core");
                // Success clears this kind's cooldown (if any) so next trigger retries immediately
                clear_kind_cooldown(kind);
                return Ok(());
            }
            Err(e) => {
                // ★ Per-kind cooldown on ANY CoreError at attempt==0.
                // Only mark THIS kind as cooldown — other kinds must keep retrying
                // (e.g. extensions large-payload overflow must not black out dialplan-rules).
                //
                // 但 push_all (bypass_cooldown) 绝不能标记 cooldown —— 可能只是
                // extensions 还在 41s 处理中，后续 retry 或下一次 periodic push 会成功。
                if attempt == 0 && !bypass_cooldown {
                    let reason = match &e {
                        CoreError::Connect(_)     => "connect refused",
                        CoreError::CoreRejected(_)=> "command rejected",
                        CoreError::Timeout        => "timeout",
                        CoreError::Io(_)          => "io error",
                        CoreError::NotFound(_)    => "notfound (unexpected for reload)",
                        CoreError::Protocol(_)   => "protocol error",
                        CoreError::Unsupported(_)=> "unsupported",
                    };
                    tracing::warn!(kind = kind.name(), error = ?e,
                        "kind reload failed ({reason}) — entering 15s per-kind cooldown");
                    mark_kind_down(kind);
                    return Err(format!("core channel broken ({reason}): {e:?}"));
                }
                tracing::warn!(kind = kind.name(), attempt, error = ?e, "reload cmd failed");
                if attempt == DELAYS_MS.len() {
                    return Err(format!("reload failed after retries: {e:?}"));
                }
                tokio::time::sleep(std::time::Duration::from_millis(DELAYS_MS[attempt])).await;
            }
        }
    }
    unreachable!()
}

/// Full push —— 14 kinds **并发**发送。
///
/// 为什么必须并发：C 侧 recv_cmd_once 是单线程 poll 循环，所有 CMD_REQ 串行处理
/// （extensions.reload 90K entries 实测 ~41s，其他 kinds 毫秒级）。如果 Web 串行发，
/// 第一个 extensions 会 60s timeout 才返回（成功），但后续 13 种在 Rust 侧各自独立
/// timeout（旧 2s 不够），先超时放弃。并发 send 让 14 个 datagram 同时进 C 侧
/// kernel queue，单线程 poll 串行处理后，所有 RSP 在 extensions 完后密集返回。
///
/// 每种 kind 内部仍保留独立的 retry backoff (500ms→8s) 和 per-kind cooldown 逻辑。
pub async fn push_all<C: CoreApi + Clone + Send + Sync + 'static>(st: Arc<AppState<C>>) {
    tracing::info!("full config push begin (concurrent x{})", Kind::ALL.len());

    // 每种 kind spawn 独立 task —— 14 种同时 sendto C 侧 kernel queue
    let mut handles: Vec<(Kind, tokio::task::JoinHandle<Result<(), String>>)> = Vec::new();
    for kind in Kind::ALL {
        let st_clone = st.clone();
        let handle = tokio::spawn(async move {
            push_kind(&st_clone, kind, true).await  // bypass cooldown — C 连接最高优先级
        });
        handles.push((kind, handle));
    }

    let mut failed = Vec::new();
    for (kind, handle) in handles {
        match handle.await {
            Ok(Ok(())) => {},
            Ok(Err(e)) => {
                tracing::error!(kind = kind.name(), error = %e, "full push kind failed");
                failed.push(kind.name().to_string());
            }
            Err(join_err) => {
                tracing::error!(kind = kind.name(), error = %join_err, "push task panicked");
                failed.push(kind.name().to_string());
            }
        }
    }
    if failed.is_empty() {
        tracing::info!("full config push done");
    } else {
        tracing::error!(kinds = ?failed, "full config push finished with failures");
    }
}

/// UDS HELLO entry point: core connects/reconnects → full push (background task, does not
/// block HELLO_ACK).
pub fn on_hello<C: CoreApi + Clone + Send + Sync + 'static>(st: Arc<AppState<C>>) {
    tokio::spawn(async move {
        push_all(st).await;
    });
}

// ---------------------------------------------------------------------------
// Single-kind push after CRUD save (S5): 300ms debounce + merge same-kind tasks
// ---------------------------------------------------------------------------

type GateMap = Mutex<HashMap<Kind, tokio::task::JoinHandle<()>>>;

fn gates() -> &'static GateMap {
    static GATES: OnceLock<GateMap> = OnceLock::new();
    GATES.get_or_init(|| Mutex::new(HashMap::new()))
}

/// Called after CRUD succeeds: schedule a single-kind push.
///
/// Debounce: the task sleeps 300ms first to merge consecutive modifications within the
/// window (after waking up, the DB snapshot already contains all of them).
/// Merge: if there is already a pending/running task for the same kind, skip — that
/// task will cover this change.
pub fn spawn_push<C: CoreApi + Clone + Send + Sync + 'static>(
    st: Arc<AppState<C>>,
    kind: Kind,
) {
    let mut g = gates().lock().unwrap();
    if let Some(h) = g.get(&kind) {
        if !h.is_finished() {
            return; // Already a pending push, will cover this change
        }
    }
    let h = tokio::spawn(async move {
        tokio::time::sleep(std::time::Duration::from_millis(300)).await;
        if let Err(e) = push_kind(&st, kind, false).await {
            tracing::error!(kind = kind.name(), error = %e, "spawned push failed");
        }
    });
    g.insert(kind, h);
}

// ---------------------------------------------------------------------------
// Periodic full re-push (HA dual-node config consistency guarantee)
// ---------------------------------------------------------------------------

/// Every 60 seconds, full re-push five kinds of config to the local nsbcd.
///
/// Why this is needed: CRUD pushes (spawn_push) only happen on the Web instance that
/// handled that HTTP request — in a dual-node deployment VIP traffic only goes to one,
/// the other's nsbcd only ever gets the HELLO snapshot, so runtime config changes never
/// reach it. After failover it would serve with stale profile / dialplan rules /
/// extension table. This also covers the "Web restart while core did not (no new HELLO)"
/// timing scenario.
///
/// Idempotent: C-side reload does whole-table RCU atomic replace, repeated pushes have
/// no side effects; also harmless in single-node deployments. Success is silent
/// (avoid per-minute log spam); failures are WARN and retried next cycle. Interval
/// hardcoded at 60s, no config knob introduced.
pub fn spawn_periodic_push<C: CoreApi + Clone + Send + Sync + 'static>(
    st: Arc<AppState<C>>,
) {
    tokio::spawn(async move {
        // interval's first tick completes immediately: push full set once at startup
        let mut tick = tokio::time::interval(std::time::Duration::from_secs(60));
        tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        loop {
            tick.tick().await;
            for kind in Kind::ALL {
                if let Err(e) = push_kind(&st, kind, false).await {
                    // Cooldown bail is noisy when core is down — debug only.
                    // Real push failures (attempted and failed) stay WARN.
                    let is_cooldown = e.contains("cooldown") || e.contains("channel broken");
                    if is_cooldown {
                        tracing::debug!(kind = kind.name(), error = %e, "periodic push skipped (cooldown)");
                    } else {
                        tracing::warn!(kind = kind.name(), error = %e, "periodic push failed");
                    }
                }
            }
        }
    });
}
