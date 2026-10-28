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

use crate::AppState;
use crate::coreclient::CoreApi;

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
}

impl Kind {
    pub const ALL: [Kind; 12] = [
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
            let (rows, _total) = st.db.list_rates(None, Some(true), i64::MAX, 0).await
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
    }
}

/// Single-kind push: snapshot → CoreApi.reload_*; retry on failure with [500ms,1s,2s,4s,8s].
async fn push_kind<C: CoreApi + Clone + Send + Sync>(
    st: &Arc<AppState<C>>,
    kind: Kind,
) -> Result<(), String> {
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
        };
        match r {
            Ok(()) => {
                tracing::info!(kind = kind.name(), count = n, attempt, "pushed to core");
                return Ok(());
            }
            Err(e) => {
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

/// Full push (four kinds in sequence; one kind failure does not block others — log ERROR,
/// the next trigger will retry).
pub async fn push_all<C: CoreApi + Clone + Send + Sync>(st: Arc<AppState<C>>) {
    tracing::info!("full config push begin (hello)");
    let mut failed = Vec::new();
    for kind in Kind::ALL {
        if let Err(e) = push_kind(&st, kind).await {
            tracing::error!(kind = kind.name(), error = %e, "full push kind failed");
            failed.push(kind.name());
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
        if let Err(e) = push_kind(&st, kind).await {
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
                if let Err(e) = push_kind(&st, kind).await {
                    tracing::warn!(kind = kind.name(), error = %e, "periodic push failed");
                }
            }
        }
    });
}
