//! Web DB → C 核心配置全量推送（REQ-DATA-SVC-003，混合式同步 S4/S5）。
//!
//! 数据流：Web 数据库（sqlite/postgres/mysql）为唯一数据源 →
//! control.sock `*.reload <json>` 命令 → C 侧内存表整表替换。
//!
//! 触发点：
//!   - UDS HELLO（[`crate::uds_server`]）：核心进程启动/重连成功 → [`on_hello`]
//!     全量推四类（extensions → trunks → dialplan-rules → sip-profiles）
//!   - CRUD 保存后（routes 各 handler）：[`spawn_push`] 单类推送（300ms 防抖合并）
//!
//! 可靠性：每类独立重试（backoff 500ms→8s 共 5 次）；单类失败不阻断其余类；
//! 重试耗尽记 ERROR，等下一次触发（HELLO 重连/CRUD/手动 reload 端点）补推。
//!
//! 安全：分机推送走 [`ExtSyncDto`]（不含明文 password，只下发 HA1）。

use std::collections::HashMap;
use std::sync::{Arc, Mutex, OnceLock};

use crate::AppState;
use crate::coreclient::CoreApi;

/// 分机同步 DTO：DB Extension → C ext_table_reload_json。
/// 密码永不离开 Web 端（C 侧只需 HA1 做 Digest 校验）。
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

/// 队列同步 DTO：DB Queue → C queue_table_reload_json。
/// 字段名/类型严格匹配 C 侧解析 schema（queue.c 中的 reload handler）。
///
/// | DTO 字段        | C 侧用途             | Rust 来源                     |
/// |-----------------|---------------------|-------------------------------|
/// | tenant          | 按租户匹配队列槽     | tenant_id:i64 → format!("{id}") |
/// | name            | 队列唯一名           | name: String                  |
/// | enabled         | json_truthy 布尔     | enabled:i64 != 0              |
/// | strategy        | CQ_STRATEGY_* 解析   | ring_strategy: String         |
/// | max_wait_s      | 超时溢出（秒）       | max_wait_sec: i64             |
/// | wrap_time_s     | 座席话后整理（秒）   | 暂固定 30（Rust 侧字段待加）    |
/// | require_skill   | 技能路由过滤         | 暂 ""（Rust 侧字段待加）       |
/// | members         | 成员分机列表         | members_json: String → 解析 Vec<String> |
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

/// 安全围栏同步 DTO：DB SecurityBlacklist → C security_blacklist_reload_json。
/// 字段名严格匹配 C 侧解析 schema（security.c：kind/type/value/comment/enabled）。
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

/// 推送的配置类别（与 C 侧 reload 命令一一对应）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Kind {
    Extensions,
    Trunks,
    DialplanRules,
    SipProfiles,
    Queues,
    SecurityBlacklist,
}

impl Kind {
    pub const ALL: [Kind; 6] = [
        Kind::Extensions,
        Kind::Trunks,
        Kind::DialplanRules,
        Kind::SipProfiles,
        Kind::Queues,
        Kind::SecurityBlacklist,
    ];
    fn name(self) -> &'static str {
        match self {
            Kind::Extensions   => "extensions",
            Kind::Trunks       => "trunks",
            Kind::DialplanRules => "dialplan-rules",
            Kind::SipProfiles  => "sip-profiles",
            Kind::Queues       => "queues",
            Kind::SecurityBlacklist => "security-blacklist",
        }
    }
}

/// 查 DB → 组 JSON（单类全量快照）。
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
            // rows 是 Vec<(Queue, tenant_name)> —— JOIN tenants 拿的 realm
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
                "fence_mode":         cfg.fence_mode,
                "method_blocked":     cfg.method_blocked,
                "auto_ban":           cfg.auto_ban,
                "authfail_threshold": cfg.authfail_threshold,
                "authfail_window_s":  cfg.authfail_window_s,
                "ban_duration_s":     cfg.ban_duration_s,
                "ratelimit_regs":     cfg.ratelimit_regs,
                "ratelimit_window_s": cfg.ratelimit_window_s,
                "rules":              dtos,
            });
            serde_json::to_string(&obj).map(|j| (j, n)).map_err(|e| e.to_string())
        }
    }
}

/// 单类推送：快照 → CoreApi.reload_*，失败按 [500ms,1s,2s,4s,8s] 重试。
async fn push_kind<C: CoreApi + Clone + Send + Sync>(
    st: &Arc<AppState<C>>,
    kind: Kind,
) -> Result<(), String> {
    const DELAYS_MS: [u64; 5] = [500, 1_000, 2_000, 4_000, 8_000];

    for attempt in 0..=DELAYS_MS.len() {
        let (json, n) = match snapshot(st, kind).await {
            Ok(v) => v,
            Err(e) => {
                // DB 读失败也走重试（连接池瞬时抖动）
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

/// 全量推送（四类串行；单类失败不阻断，记 ERROR 等下次触发补推）。
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

/// UDS HELLO 入口：核心连接建立/重连 → 全量推送（后台任务，不阻塞 HELLO_ACK）。
pub fn on_hello<C: CoreApi + Clone + Send + Sync + 'static>(st: Arc<AppState<C>>) {
    tokio::spawn(async move {
        push_all(st).await;
    });
}

// ---------------------------------------------------------------------------
// CRUD 保存后单类推送（S5）：300ms 防抖 + 同类任务合并
// ---------------------------------------------------------------------------

type GateMap = Mutex<HashMap<Kind, tokio::task::JoinHandle<()>>>;

fn gates() -> &'static GateMap {
    static GATES: OnceLock<GateMap> = OnceLock::new();
    GATES.get_or_init(|| Mutex::new(HashMap::new()))
}

/// CRUD 成功后调用：安排一次单类推送。
///
/// 防抖：任务先睡 300ms 合并窗口内连续修改（唤醒后查的 DB 快照已含全部）；
/// 合并：同类已有 pending/running 任务时跳过（该任务会覆盖本次变更）。
pub fn spawn_push<C: CoreApi + Clone + Send + Sync + 'static>(
    st: Arc<AppState<C>>,
    kind: Kind,
) {
    let mut g = gates().lock().unwrap();
    if let Some(h) = g.get(&kind) {
        if !h.is_finished() {
            return; // 已有 pending 推送，将覆盖本次变更
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
// 周期性全量重推（HA 双机部署的配置一致性保障）
// ---------------------------------------------------------------------------

/// 每 60s 向本机 nsbcd 全量重推五类配置。
///
/// 为什么需要：CRUD 推送（spawn_push）只发生在「处理该次 HTTP 请求」
/// 的 Web 实例上——双机部署时 VIP 流量只到一台，另一台的 nsbcd 若只
/// 靠 HELLO 快照，运行期配置变更永远到不了它，failover 后就会拿旧
/// profile/拨号规则/分机表服务。也覆盖「Web 重启而核心未重启（无新
/// HELLO）」的时序。
///
/// 幂等：C 侧 reload 是整表 RCU 原子替换，重复推送无副作用；单机
/// 部署同样无害。成功静默（避免每分钟刷日志），失败记 WARN 由下一
/// 周期补推。间隔硬编码 60s，不引入配置项。
pub fn spawn_periodic_push<C: CoreApi + Clone + Send + Sync + 'static>(
    st: Arc<AppState<C>>,
) {
    tokio::spawn(async move {
        // interval 首个 tick 立即完成：启动即推一次全量
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
