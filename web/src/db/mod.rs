//! 数据库持久化层（REQ-WEB-013）。
//!
//! - 从 `NSBC_DB_DSN` 环境变量初始化 sqlx 连接池；支持 sqlite / postgres / mysql 三后端。
//! - 启动期自动执行 DDL（`CREATE TABLE IF NOT EXISTS` + 索引），幂等，可重复执行。
//! - 所有写入均为追加（INSERT），CDR/audit_log 永不 UPDATE/DELETE。
//! - 连接池失败不阻断主业务（核心 UDS 控制通道不受 DB 影响）。
//!
//! sqlite DSN:  `sqlite://./nsbc-web.db`（或 `sqlite::memory:` 用于测试）
//! pg DSN:      `postgres://user:pass@host:5432/nsbc`
//! mysql DSN:   `mysql://user:pass@host:3306/nsbc`

use std::env;
use std::sync::Arc;

use md5::{Md5, Digest};
use sqlx::{Any, Pool, Row};
use tracing::{info, warn};

// sqlx Any driver 把 SQLite INTEGER 读为 i64，但业务需要 bool。
// 解决方案：FromRow struct 用 i64 存储，serde 输出自动转 bool。
mod bool_i64 {
    use serde::ser::Serializer;
    pub fn serialize<S: Serializer>(v: &i64, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_bool(*v != 0)
    }
}


fn scheme(dsn: &str) -> &str {
    if let Some((s, _)) = dsn.split_once("://") { s }
    else if let Some((s, _)) = dsn.split_once(':') { s }
    else { "?" }
}

/// 从 sqlite DSN 提取完整 db 文件路径（绝对或相对）。
/// 规则（与 resolve_dsn_path 一致）：
///   "sqlite:" 后总斜杠 ≥ 3 → 绝对路径（补回前导 /）
///   "sqlite:" 后总斜杠 < 3 → 相对路径
fn sqlite_file_path(dsn: &str) -> Option<std::path::PathBuf> {
    if dsn.contains(":memory:") { return None; }

    let rest = dsn.strip_prefix("sqlite:")?;
    let leading_slashes = rest.chars().take_while(|c| *c == '/').count();
    let path_part = rest.trim_start_matches('/');
    if path_part.is_empty() { return None; }

    // 总斜杠 ≥ 3 → 绝对路径（补前导 /）
    let file_path = if leading_slashes >= 3 {
        format!("/{}", path_part)
    } else {
        path_part.to_string()
    };
    Some(std::path::PathBuf::from(file_path))
}

fn sqlite_parent_dir(dsn: &str) -> Option<std::path::PathBuf> {
    tracing::info!("🔧 [sqlite_parent_dir] input dsn = {:?}", dsn);
    let fp = sqlite_file_path(dsn)?;
    tracing::info!("🔧 [sqlite_parent_dir] → file_path={:?}", fp.display());
    let parent = fp.parent().map(|d| d.to_path_buf());
    tracing::info!("🔧 [sqlite_parent_dir] → parent = {:?}", parent.as_ref().map(|d| d.display().to_string()));
    parent
}

/// SIP Digest HA1 = md5(username:realm:password) — 全模块通用
pub fn ha1_of(username: &str, realm: &str, password: &str) -> String {
    let h = Md5::digest(format!("{username}:{realm}:{password}").as_bytes());
    h.iter().map(|b| format!("{b:02x}")).collect()
}

/// Web 边车持久化池；None 表示未配置 NSBC_DB_DSN（CDR/audit 降级为 503）。
#[derive(Clone)]
pub struct Db {
    pool: Option<Arc<Pool<Any>>>,
    backend: Option<String>,
}

impl Db {
    pub fn new() -> Self { Db { pool: None, backend: None } }

    /// 从环境变量初始化。失败返回 `Db(None)` 并记录日志，不阻断主业务启动。
    pub async fn from_env() -> Self {
        let dsn = env::var("NSBC_DB_DSN").unwrap_or_default();
        Self::from_dsn(&dsn).await
    }

    /// 从 WebConfig::DbSection 初始化（dsn 空或连接失败均降级）。
    pub async fn from_config(cfg: &super::config::DbSection) -> Self {
        let dsn = if let Ok(v) = env::var("NSBC_DB_DSN") {
            tracing::info!("🔧 [db::from_config] NSBC_DB_DSN env override active: {:?}", v);
            v
        } else {
            tracing::info!("🔧 [db::from_config] using cfg.dsn: {:?}", cfg.dsn);
            cfg.dsn.clone()
        };
        Self::from_dsn(&dsn).await
    }

    pub async fn from_dsn(dsn: &str) -> Self {
        tracing::info!("🔧 [db::from_dsn] final dsn = {:?}", dsn);
        if dsn.is_empty() {
            info!("db dsn not set — persistence disabled");
            return Self { pool: None, backend: None };
        }
        let backend = scheme(dsn).to_string();

        // SQLite：父目录不存在会导致 code 14 (unable to open database file)
        // sqlx 只会自动创建 db 文件，不会自动 mkdir parent dir
        if backend == "sqlite" {
            if let Some(dir) = sqlite_parent_dir(dsn) {
                if !dir.as_os_str().is_empty() {
                    match std::fs::create_dir_all(&dir) {
                        Ok(()) => info!(dir = %dir.display(), "sqlite parent dir ensured"),
                        Err(e) => warn!(dir = %dir.display(), error = %e,
                                        "sqlite parent dir create failed"),
                    }
                }
            }

            // 🔍 写入探针：sqlx 连接前直接写一次 db 文件，把 errno 打出来。
            // code 14 除路径错误外最常见根因是目录无写权限 / SELinux 拦截；
            // create_dir_all 在目录已存在时不检查写权限，"dir ensured" 不能证明可写。
            if let Some(fp) = sqlite_file_path(dsn) {
                // 进程上下文：uid + cwd（权限诊断第一手信息）
                let uid_line = std::fs::read_to_string("/proc/self/status").ok()
                    .and_then(|s| s.lines().find(|l| l.starts_with("Uid:"))
                              .map(|l| l.trim().to_string()));
                let cwd = std::env::current_dir().ok();
                info!(uid = ?uid_line, cwd = ?cwd.as_ref().map(|c| c.display().to_string()),
                      "🔍 [probe] process context");

                // 文件现状
                match std::fs::metadata(&fp) {
                    Ok(m) => info!(file = %fp.display(), exists = true,
                                   is_file = m.is_file(), len = m.len(),
                                   "🔍 [probe] db file metadata"),
                    Err(_) => info!(file = %fp.display(), exists = false,
                                    "🔍 [probe] db file metadata"),
                }

                // 实际写入（create+append；文件不存在则创建，等同 SQLite 建库动作）
                match std::fs::OpenOptions::new().create(true).append(true).open(&fp) {
                    Ok(_) => info!(file = %fp.display(),
                                   "🔍 [probe] db file WRITABLE — filesystem OK, code 14 来自 sqlx 层"),
                    Err(e) => warn!(file = %fp.display(), error = %e,
                                    os_error = e.raw_os_error().unwrap_or(-1),
                                    "🔍 [probe] db file NOT WRITABLE — 权限/SELinux 问题 \
                                     (13=EACCES 权限不足; 1=EPERM; 30=EROFS 只读盘)"),
                }
            }
        }

        // 先直接连一次；如果是"库不存在"的错误，自动 CREATE DATABASE 后重试
        let pool_opt: Option<Pool<Any>> = match connect_and_migrate(dsn).await {
            Ok(p) => Some(p),
            Err(e) => {
                // 只有 Postgres/MySQL 才需要 CREATE DATABASE（SQLite 文件不存在会自动创建）
                let needs_create = matches!(backend.as_str(), "postgres" | "mysql")
                    && db_missing(&e);
                if needs_create {
                    match auto_create_database(dsn, &backend).await {
                        Ok(()) => {
                            info!("auto-created database, retrying connect...");
                            connect_and_migrate(dsn).await.ok()
                        }
                        Err(e2) => {
                            warn!(error = %e2, "auto-create database failed");
                            None
                        }
                    }
                } else {
                    warn!(error = %e, "db pool init failed — continuing without persistence");
                    None
                }
            }
        };
        match pool_opt {
            Some(p) => {
                info!(scheme = backend.as_str(), "db pool initialized");
                Self { pool: Some(Arc::new(p)), backend: Some(backend) }
            }
            None => Self { pool: None, backend: None },
        }
    }

    /// 取底层池引用。未配置时返回 None。
    pub fn pool(&self) -> Option<&Pool<Any>> { self.pool.as_ref().map(|a| a.as_ref()) }

    /// 后端类型名：sqlite / postgres / mysql / none。
    pub fn backend(&self) -> &'static str {
        match self.backend.as_deref() {
            Some("sqlite") => "sqlite",
            Some("postgres") => "postgres",
            Some("mysql") => "mysql",
            _ => "none",
        }
    }

    /// 从 web_users 表查询 Web 管理用户。
    /// 未配置 DB（pool=None）或用户不存在 → 返回 None。
    /// DB 查询失败 → 打 warn 日志并返回 None（不阻断登录流程，由上层 fallback）。
    pub async fn get_web_user(&self, username: &str) -> Option<WebUser> {
        let pool = self.pool()?;
        let row = sqlx::query_as::<_, WebUser>(
            "SELECT id, username, role, password_phc, enabled FROM web_users WHERE username = $1 LIMIT 1"
        )
        .bind(username)
        .fetch_optional(pool)
        .await;
        match row {
            Ok(Some(u)) => Some(u),
            Ok(None) => None,
            Err(e) => {
                tracing::warn!(user = %username, error = %e,
                    "web_users query failed — falling back to in-memory accounts");
                None
            }
        }
    }

    /// 确保 web_users 表中至少有一个启用的 admin 用户。
    /// 启动时调用：检查表中是否已有 enabled=1 的用户，没有则注入默认 admin。
    /// 默认密码从环境变量 NSBC_DEFAULT_ADMIN_PW 读取，未设则用 "Admin123"。
    /// PHC 由调用方预先计算并传入（Db 不依赖 auth 模块）。
    pub async fn ensure_default_web_user(
        &self,
        username: &str,
        role: &str,
        password_phc: &str,
    ) {
        let pool = match self.pool() {
            Some(p) => p,
            None => return, // 无 DB，跳过 seed
        };

        // 检查是否需要强制重置 admin 密码（运维应急用）
        let force_reset = std::env::var("NSBC_RESET_ADMIN").ok()
            .map(|v| v == "1" || v.eq_ignore_ascii_case("true"))
            .unwrap_or(false);

        // 查询现有记录（拿到 phc 判断是否为空）
        let existing: Result<Option<(i64, String)>, _> = sqlx::query_as(
            "SELECT id, password_phc FROM web_users WHERE username = $1 LIMIT 1"
        )
        .bind(username)
        .fetch_optional(pool)
        .await;

        match existing {
            Ok(Some((id, old_phc))) => {
                // 已存在：如果 phc 为空 或 显式要求重置 → UPDATE
                let need_update = force_reset || old_phc.is_empty();
                if !need_update {
                    tracing::info!(user = %username,
                        "web_users: default admin already exists (phc set), skip seed; \
                         set NSBC_RESET_ADMIN=1 to force reset");
                    return;
                }
                let sql = match self.backend() {
                    "postgres" =>
                        "UPDATE web_users SET role = $1, password_phc = $2, enabled = 1, \
                         updated_at = to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') \
                            || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z' \
                         WHERE id = $3",
                    _ =>
                        "UPDATE web_users SET role = $1, password_phc = $2, enabled = 1 \
                         WHERE id = $3",
                };
                match sqlx::query(sql)
                    .bind(role).bind(password_phc).bind(id)
                    .execute(pool).await
                {
                    Ok(r) => tracing::info!(user = %username, rows = r.rows_affected(),
                        "web_users: default admin password {}updated",
                        if force_reset { "force-re" } else { "" }),
                    Err(e) => tracing::warn!(error = %e, user = %username,
                        "web_users: {}default admin failed",
                        if force_reset { "force-reset " } else { "update " }),
                }
                return;
            }
            Ok(None) => {} // 需要 INSERT
            Err(e) => {
                tracing::warn!(error = %e,
                    "web_users: check existing failed, skip seed");
                return;
            }
        }

        // INSERT OR IGNORE / ON CONFLICT DO NOTHING 幂等插入
        let sql = match self.backend() {
            "postgres" =>
                "INSERT INTO web_users (username, role, password_phc, enabled, description) \
                 VALUES ($1, $2, $3, 1, 'Default admin — bootstrapped at startup') \
                 ON CONFLICT (username) DO NOTHING",
            "mysql" =>
                "INSERT IGNORE INTO web_users (username, role, password_phc, enabled, description) \
                 VALUES (?, ?, ?, 1, 'Default admin — bootstrapped at startup')",
            _ =>
                "INSERT OR IGNORE INTO web_users (username, role, password_phc, enabled, description) \
                 VALUES ($1, $2, $3, 1, 'Default admin — bootstrapped at startup')",
        };

        match sqlx::query(sql)
            .bind(username).bind(role).bind(password_phc)
            .execute(pool).await
        {
            Ok(r) => {
                if r.rows_affected() > 0 {
                    tracing::info!(user = %username,
                        "web_users: seeded default admin user");
                }
            }
            Err(e) => tracing::warn!(error = %e, user = %username,
                "web_users: seed default admin failed"),
        }
    }

    /// 追加一行配置变更历史（config_history 表）。DB 未配置 → 静默跳过
    /// 返回 Ok(())；后端写入失败 → 向上返回 sqlx::Error（由调用方决定降级）。
    /// `actor` 通常是 Web 侧 API 调用占位 "api"（操作人已由 rbac middleware
    /// 独立审计）；`action` 如 "set" / "set-noop" / "reload"；`old_value` 为空
    /// 时传 None；`new_value` 必填。时间戳由列 DEFAULT 自动填充（避免跨后端
    /// NOW()/datetime('now') 差异）。
    pub async fn insert_config_history(
        &self,
        actor: &str,
        action: &str,
        key: &str,
        old_value: Option<&str>,
        new_value: &str,
    ) -> Result<(), sqlx::Error> {
        let Some(pool) = self.pool.as_ref() else { return Ok(()); };
        // 注意：config_history DDL 列名是 `key`（不是 cfg_key）。
        // changed_at 由列 DEFAULT 自动填（SQLite datetime('now') / PG NOW() /
        // MySQL CURRENT_TIMESTAMP(3)），此处不传。
        sqlx::query(
            "INSERT INTO config_history (actor, action, key, old_value, new_value) \
             VALUES ($1, $2, $3, $4, $5)",
        )
        .bind(actor)
        .bind(action)
        .bind(key)
        .bind(old_value)
        .bind(new_value)
        .execute(pool.as_ref())
        .await?;
        Ok(())
    }

    /// 追加一行话单（cdr 表）。DB 未配置 → 静默跳过 Ok；call_id 唯一键冲突
    /// → 忽略；其他错误返回 sqlx::Error。
    ///
    /// 字段映射（新 cdr_event_t → 旧 DDL 列）：
    ///   call_id → call_id (UNIQUE 冲突即幂等)
    ///   from_num → caller；to_num → callee
    ///   start_ms/end_ms → start_time/end_time (ISO 毫秒字符串，三后端 TEXT/时间戳都兼容)
    ///   duration_s → duration_sec；cause → cause_code；disposition → cause_text
    ///   扩展字段（tenant/trunk/rec_path/dir/phase/emergency/answer_ms/billsec）
    ///     打包进 metadata JSON（TEXT/TEXT/JSON 三后端都能吃字符串 JSON）
    ///   a_call_id/b_call_id/leg_a_final/leg_b_final → NULL（旧 B2B 腿诊断遗留列）。
    ///
    /// 跨后端冲突忽略语法：pg/sqlite 用 ON CONFLICT DO NOTHING，mysql 用 INSERT IGNORE。
    pub async fn insert_cdr(
        &self,
        call_id: &str,
        tenant: Option<&str>,
        from_num: &str,
        to_num: &str,
        trunk: Option<&str>,
        rec_path: Option<&str>,
        dir: &str,
        phase: &str,
        disposition: &str,
        start_ms: u64,
        answer_ms: u64,
        end_ms: u64,
        billsec: u32,
        duration_s: u32,
        cause: i32,
        emergency: bool,
        from_ip: Option<&str>,
        codec_a: Option<&str>,
        codec_b: Option<&str>,
        a_peer_ip: Option<&str>,
        b_peer_ip: Option<&str>,
    ) -> Result<(), sqlx::Error> {
        let Some(pool) = self.pool.as_ref() else { return Ok(()); };

        // 时间戳（毫秒 epoch → ISO 8601 字符串，所有后端 TEXT/时间戳列都能存）。
        let start_iso = ms_to_iso(start_ms);
        let answer_iso = if answer_ms > 0 { ms_to_iso(answer_ms) } else { None };
        let end_iso = if end_ms > 0 { ms_to_iso(end_ms) } else { None };
        let billsec_i64 = billsec as i64;

        // metadata JSON 字符串（三后端都能接受）。codec/IP 等扩展字段塞进 metadata——
        // 不碰 cdr 表 schema（最小侵入），前端解析 metadata JSON 展示。
        let mut meta_obj = serde_json::json!({
            "tenant": tenant,
            "trunk": trunk,
            "rec_path": rec_path,
            "dir": dir,
            "phase": phase,
            "answer_ms": answer_ms,
            "billsec": billsec,
            "emergency": emergency,
        });
        // codec / IP 字段（空串时跳过——signaling.c END 链路填完整，START/ANSWER 早期事件可能空）
        // 兼容旧 Rust：let chain guard（if let ... if COND）需 1.65+，改嵌套 if
        if let Some(s) = from_ip   { if !s.is_empty() { meta_obj["from_ip"]   = serde_json::Value::String(s.into()); } }
        if let Some(s) = codec_a   { if !s.is_empty() { meta_obj["codec_a"]   = serde_json::Value::String(s.into()); } }
        if let Some(s) = codec_b   { if !s.is_empty() { meta_obj["codec_b"]   = serde_json::Value::String(s.into()); } }
        if let Some(s) = a_peer_ip { if !s.is_empty() { meta_obj["a_peer_ip"] = serde_json::Value::String(s.into()); } }
        if let Some(s) = b_peer_ip { if !s.is_empty() { meta_obj["b_peer_ip"] = serde_json::Value::String(s.into()); } }
        let metadata = meta_obj.to_string();

        let sql = match self.backend() {
            "mysql" =>
                "INSERT IGNORE INTO cdr \
                 (call_id, caller, callee, start_time, answer_time, end_time, \
                  duration_sec, billsec, cause_code, cause_text, metadata) \
                 VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11)",
            _ =>
                "INSERT INTO cdr \
                 (call_id, caller, callee, start_time, answer_time, end_time, \
                  duration_sec, billsec, cause_code, cause_text, metadata) \
                 VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11) \
                 ON CONFLICT DO NOTHING",
        };
        sqlx::query(sql)
            .bind(call_id)
            .bind(from_num)
            .bind(to_num)
            .bind(start_iso)
            .bind(answer_iso)
            .bind(end_iso)
            .bind(duration_s as i64)
            .bind(billsec_i64)
            .bind(cause as i64)
            .bind(disposition)
            .bind(metadata)
            .execute(pool.as_ref())
            .await?;
        Ok(())
    }

    /// register.write：UPSERT 注册快照（REQ-DATA-SVC-002 C→Web write-through）。
    ///
    /// 唯一键 user_name + instance_id（多终端/GRUU 场景区分设备），重复推送原地
    /// 刷新 contact/agent/expires_at/updated_at；expires_at 由 now + expires_sec
    /// 计算为 ISO 8601 字符串（与 cdr 时间列同模式）。
    /// MySQL 用 ON DUPLICATE KEY UPDATE，sqlite/pg 用 ON CONFLICT DO UPDATE。
    /// NAT 元数据（REQ-NAT-001）：received_ip/received_port/nat_detected/
    /// public_contact/transport — C 侧 registrar.c 检测填充，Web 端原样落库展示。
    pub async fn upsert_registration(
        &self,
        user_name: &str,
        contact: Option<&str>,
        instance_id: &str,
        agent: Option<&str>,
        expires_sec: u32,
        // NAT 元数据（REQ-NAT-001）
        received_ip: Option<&str>,
        received_port: Option<i64>,
        nat_detected: Option<i64>,
        public_contact: Option<&str>,
        transport: Option<&str>,
    ) -> Result<(), sqlx::Error> {
        let Some(pool) = self.pool.as_ref() else { return Ok(()); };

        let now_ms = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_millis() as u64)
            .unwrap_or(0);
        let expires_at = ms_to_iso(now_ms + expires_sec as u64 * 1000);
        let updated_at = ms_to_iso(now_ms);

        // 空字符串 option 归一（C 侧 push 的 JSON 空字符串 → Web None）
        let received_ip = received_ip.and_then(|s| if s.is_empty() { None } else { Some(s) });
        let public_contact = public_contact.and_then(|s| if s.is_empty() { None } else { Some(s) });
        let transport = transport.and_then(|s| if s.is_empty() { None } else { Some(s) });

        let sql = match self.backend() {
            "mysql" =>
                "INSERT INTO registrations \
                 (user_name, contact, instance_id, agent, expires_at, updated_at, \
                  received_ip, received_port, nat_detected, public_contact, transport) \
                 VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11) \
                 ON DUPLICATE KEY UPDATE \
                 contact = VALUES(contact), agent = VALUES(agent), \
                 expires_at = VALUES(expires_at), updated_at = VALUES(updated_at), \
                 received_ip = VALUES(received_ip), received_port = VALUES(received_port), \
                 nat_detected = VALUES(nat_detected), public_contact = VALUES(public_contact), \
                 transport = VALUES(transport)",
            _ =>
                "INSERT INTO registrations \
                 (user_name, contact, instance_id, agent, expires_at, updated_at, \
                  received_ip, received_port, nat_detected, public_contact, transport) \
                 VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11) \
                 ON CONFLICT (user_name, instance_id) DO UPDATE SET \
                 contact = excluded.contact, agent = excluded.agent, \
                 expires_at = excluded.expires_at, updated_at = excluded.updated_at, \
                 received_ip = excluded.received_ip, received_port = excluded.received_port, \
                 nat_detected = excluded.nat_detected, public_contact = excluded.public_contact, \
                 transport = excluded.transport",
        };
        sqlx::query(sql)
            .bind(user_name)
            .bind(contact)
            .bind(instance_id)
            .bind(agent)
            .bind(expires_at)
            .bind(updated_at)
            .bind(received_ip)
            .bind(received_port)
            .bind(nat_detected)
            .bind(public_contact)
            .bind(transport)
            .execute(pool.as_ref())
            .await?;
        Ok(())
    }

    /// runtime.write：追加呼叫状态变迁轨迹（runtime_state 表，纯 INSERT）。
    /// state：DIALING/EARLY/ANSWERED/HANGUP 等；detail 为可选 JSON 字符串。
    pub async fn insert_runtime_state(
        &self,
        call_id: &str,
        state: &str,
        detail: Option<&str>,
    ) -> Result<(), sqlx::Error> {
        let Some(pool) = self.pool.as_ref() else { return Ok(()); };
        sqlx::query(
            "INSERT INTO runtime_state (call_id, state, detail) VALUES ($1, $2, $3)",
        )
        .bind(call_id)
        .bind(state)
        .bind(detail)
        .execute(pool.as_ref())
        .await?;
        Ok(())
    }

    /// audit.write：C 侧 CLI/OAM 操作审计（audit_log 表，Web 侧认证审计同表）。
    /// C 核心经 UDS 推送的场景无来源 IP，ip 传 None。
    pub async fn insert_audit_event(
        &self,
        event: &str,
        actor: Option<&str>,
        ip: Option<&str>,
        target: Option<&str>,
        status: Option<&str>,
        detail: Option<&str>,
    ) -> Result<(), sqlx::Error> {
        let Some(pool) = self.pool.as_ref() else { return Ok(()); };
        sqlx::query(
            "INSERT INTO audit_log (event, actor, ip, target, status, detail) \
             VALUES ($1, $2, $3, $4, $5, $6)",
        )
        .bind(event)
        .bind(actor)
        .bind(ip)
        .bind(target)
        .bind(status)
        .bind(detail)
        .execute(pool.as_ref())
        .await?;
        Ok(())
    }

    /// session.write：UPSERT 实时呼叫会话快照（REQ-DATA-SVC-002 C→Web write-through）。
    ///
    /// call_id 作为唯一键；C 侧经 data.sock 推送 SESSION 事件时调用。
    /// from_num / to_num 为空串时保留已有值（会话早期事件可能还没拿到号码）。
    /// 三后端 upsert 语法差异：mysql ON DUPLICATE KEY UPDATE，pg/sqlite ON CONFLICT DO UPDATE。
    pub async fn upsert_call(
        &self,
        call_id: &str,
        from_num: &str,
        to_num: &str,
        dir: &str,
        state: &str,
        event: &str,
        cause: i32,
        from_ip: &str,
        codec_a: &str,
        codec_b: &str,
        a_peer_ip: &str,
        b_peer_ip: &str,
    ) -> Result<(), sqlx::Error> {
        let Some(pool) = self.pool.as_ref() else { return Ok(()); };

        let now_ms = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_millis() as u64)
            .unwrap_or(0);
        let last_seen = ms_to_iso(now_ms).unwrap_or_else(|| "1970-01-01T00:00:00.000Z".into());

        let sql = match self.backend() {
            "mysql" =>
                "INSERT INTO calls (call_id, from_num, to_num, dir, state, event, cause, \
                     from_ip, codec_a, codec_b, a_peer_ip, b_peer_ip, last_seen) \
                 VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13) \
                 ON DUPLICATE KEY UPDATE \
                     state     = IF(VALUES(state) = '',     calls.state,     VALUES(state)), \
                     event     = IF(VALUES(event) = '',     calls.event,     VALUES(event)), \
                     cause     = VALUES(cause), \
                     last_seen = VALUES(last_seen), \
                     from_num  = IF(VALUES(from_num) = '',  calls.from_num,  VALUES(from_num)), \
                     to_num    = IF(VALUES(to_num)   = '',  calls.to_num,    VALUES(to_num)), \
                     dir       = IF(VALUES(dir)      = '',  calls.dir,       VALUES(dir)), \
                     from_ip   = IF(VALUES(from_ip)  = '',  calls.from_ip,   VALUES(from_ip)), \
                     codec_a   = IF(VALUES(codec_a)  = '',  calls.codec_a,   VALUES(codec_a)), \
                     codec_b   = IF(VALUES(codec_b)  = '',  calls.codec_b,   VALUES(codec_b)), \
                     a_peer_ip = IF(VALUES(a_peer_ip)= '',  calls.a_peer_ip, VALUES(a_peer_ip)), \
                     b_peer_ip = IF(VALUES(b_peer_ip)= '',  calls.b_peer_ip, VALUES(b_peer_ip))",
            _ =>
                "INSERT INTO calls (call_id, from_num, to_num, dir, state, event, cause, \
                     from_ip, codec_a, codec_b, a_peer_ip, b_peer_ip, last_seen) \
                 VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13) \
                 ON CONFLICT(call_id) DO UPDATE SET \
                     state     = CASE WHEN excluded.state    = '' THEN calls.state     ELSE excluded.state     END, \
                     event     = CASE WHEN excluded.event    = '' THEN calls.event     ELSE excluded.event     END, \
                     cause     = excluded.cause, \
                     last_seen = excluded.last_seen, \
                     from_num  = CASE WHEN excluded.from_num = '' THEN calls.from_num  ELSE excluded.from_num  END, \
                     to_num    = CASE WHEN excluded.to_num   = '' THEN calls.to_num    ELSE excluded.to_num    END, \
                     dir       = CASE WHEN excluded.dir      = '' THEN calls.dir       ELSE excluded.dir       END, \
                     from_ip   = CASE WHEN excluded.from_ip   = '' THEN calls.from_ip   ELSE excluded.from_ip   END, \
                     codec_a   = CASE WHEN excluded.codec_a   = '' THEN calls.codec_a   ELSE excluded.codec_a   END, \
                     codec_b   = CASE WHEN excluded.codec_b   = '' THEN calls.codec_b   ELSE excluded.codec_b   END, \
                     a_peer_ip = CASE WHEN excluded.a_peer_ip = '' THEN calls.a_peer_ip ELSE excluded.a_peer_ip END, \
                     b_peer_ip = CASE WHEN excluded.b_peer_ip = '' THEN calls.b_peer_ip ELSE excluded.b_peer_ip END",
        };
        sqlx::query(sql)
            .bind(call_id)
            .bind(from_num)
            .bind(to_num)
            .bind(dir)
            .bind(state)
            .bind(event)
            .bind(cause as i64)
            .bind(from_ip)
            .bind(codec_a)
            .bind(codec_b)
            .bind(a_peer_ip)
            .bind(b_peer_ip)
            .bind(&last_seen)
            .execute(pool.as_ref())
            .await?;
        Ok(())
    }
}

/// 毫秒 epoch → ISO 8601 UTC 字符串（YYYY-MM-DDTHH:MM:SS.sssZ）；0 输入返回 None。
/// 纯 std 实现（无需 chrono 依赖），毫秒精度。SQLite/MySQL TEXT 列直接存字符串；
/// PostgreSQL TIMESTAMPTZ 列也接受 ISO 字符串。
fn ms_to_iso(ms: u64) -> Option<String> {
    if ms == 0 { return None; }
    let secs = (ms / 1000) as u64;
    let ms_rem = ms % 1000;

    // Unix epoch → UTC 年月日时分秒。
    // 用简化 Zeller 推导：epoch 以 1970-01-01 为起点。
    let days = secs / 86_400;
    let rem_sec = secs % 86_400;
    let h = rem_sec / 3_600;
    let m = (rem_sec % 3_600) / 60;
    let s = rem_sec % 60;

    // 1970-01-01 是 Thursday（day_of_week=4, 周日=0），day 0 是 Thu。
    // 简化版：累加年、月天数，考虑闰年。
    let mut y: u32 = 1970;
    let mut d_rem = days as u32;
    loop {
        let year_days = if (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0) { 366 } else { 365 };
        if d_rem < year_days { break; }
        d_rem -= year_days;
        y += 1;
    }
    let leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    let months_days: [u32; 12] = if leap {
        [31,29,31,30,31,30,31,31,30,31,30,31]
    } else {
        [31,28,31,30,31,30,31,31,30,31,30,31]
    };
    let mut mo: u32 = 1;
    for d in months_days {
        if d_rem < d { break; }
        d_rem -= d;
        mo += 1;
    }
    let day = d_rem + 1;

    Some(format!(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
        y, mo, day, h, m, s, ms_rem
    ))
}

/// 判断 sqlx 错误是否是 "database does not exist"
/// 三后端不同：pg "does not exist"，mysql "Unknown database"
fn db_missing(e: &sqlx::Error) -> bool {
    let msg = format!("{}", e).to_ascii_lowercase();
    msg.contains("does not exist")        // PostgreSQL 42P01
        || msg.contains("unknown database") // MySQL 1049
        || msg.contains("no such database")
}

/// 统一占位符转换——任何 SQL 先过这个函数再丢给 sqlx。
/// 接受两种输入风格，统一输出后端要求的格式：
///
///   postgres:  `?` → `$1, $2...`   `$N` 保持不变
///   sqlite:    `?` → `$1, $2...`   `$N` 保持不变
///   mysql:     `$N` → `?`           `?` 保持不变
///
/// 规则：只要 SQL 里有 `?` 就当"原生 `?` 风格"处理；只要有 `${digit}` 就当"pg 风格"处理。
fn fix_ph(sql: &str, backend: &str) -> String {
    let has_q = sql.contains('?');
    let has_dollar = sql.chars().zip(sql.chars().skip(1))
        .any(|(a, b)| a == '$' && b.is_ascii_digit());
    if backend == "mysql" {
        if has_dollar {
            let mut out = String::with_capacity(sql.len());
            let mut chars = sql.chars().peekable();
            while let Some(c) = chars.next() {
                if c == '$' {
                    let mut num = String::new();
                    while let Some(&nc) = chars.peek() {
                        if nc.is_ascii_digit() { num.push(chars.next().unwrap()); }
                        else { break; }
                    }
                    if !num.is_empty() { out.push('?'); } else { out.push(c); }
                } else { out.push(c); }
            }
            out
        } else {
            sql.to_string()
        }
    } else if has_q {
        let mut out = String::with_capacity(sql.len());
        let mut n = 0u32;
        for chunk in sql.split('?') {
            if n > 0 { out.push_str(&format!("${n}")); }
            out.push_str(chunk);
            n += 1;
        }
        out
    } else {
        sql.to_string()
    }
}

/// 剥离 RETURNING 子句（MySQL 不支持 RETURNING）。
/// 只处理末尾的 `RETURNING *` 或 `RETURNING col1, col2...`，大小写不敏感。
fn strip_returning(sql: &str) -> String {
    let lower = sql.to_ascii_lowercase();
    if let Some(idx) = lower.rfind(" returning ") {
        // 找到 'returning '（前有空格避免误匹配），截掉
        let trimmed = sql[..idx].trim_end();
        trimmed.to_string()
    } else if let Some(idx) = lower.rfind("returning ") {
        // 行首或无前导空格的情况
        let trimmed = sql[..idx].trim_end();
        trimmed.to_string()
    } else {
        sql.to_string()
    }
}

/// 从 DSN 里把数据库名抽出来，再替换成默认库（postgres/mysql），
/// 连上去跑 CREATE DATABASE。成功返回 Ok(())。
///
///   postgres://user:pass@host:5432/nsbcdb  → 先连 postgres → CREATE DATABASE nsbcdb
///   mysql://user:pass@host:3306/nsbcdb     → 先连 mysql    → CREATE DATABASE nsbcdb
async fn auto_create_database(dsn: &str, backend: &str) -> Result<(), sqlx::Error> {
    // 从 DSN 解析出目标库名（先 clone 出来，避免 parsed 被借用导致后面 match 无法 move）
    let parsed = parse_dsn(dsn);
    let db_name: String = parsed
        .as_ref()
        .and_then(|d| d.path.strip_prefix('/').map(|s| s.to_string()))
        .ok_or_else(|| sqlx::Error::Configuration("cannot parse database name from DSN".into()))?;

    // 替换路径为默认管理库，建临时连接
    let admin_default = match backend {
        "postgres" => "postgres",
        "mysql"    => "mysql",
        _ => return Err(sqlx::Error::Configuration(format!("unsupported backend: {backend}").into())),
    };
    let admin_dsn = match parsed {
        Some(mut p) => {
            p.path = format!("/{admin_default}");
            rebuild_dsn(&p, backend)
        }
        None => {
            // parse 失败兜底：手搓（只支持最简单 host:port 无 query 的情况）
            let scheme_end = dsn.find("://").unwrap_or(0);
            let path_start = dsn[scheme_end + 3..].find('/').map(|i| scheme_end + 3 + i).unwrap_or(dsn.len());
            let before_path = dsn[..path_start].to_string();
            format!("{before_path}/{admin_default}")
        }
    };

    info!(admin_dsn = %admin_dsn, "connecting admin db to create database '{db_name}'");
    let admin_pool = Pool::<Any>::connect(&admin_dsn).await?;
    // CREATE DATABASE 不能用 prepared statement（多数后端不支持）
    let escaped = db_name.replace('`', "").replace('"', "").replace('\'', "");
    if escaped.is_empty() || escaped.len() > 64 {
        return Err(sqlx::Error::Configuration(format!("invalid database name: {db_name}").into()));
    }
    let sql = match backend {
        "postgres" => format!("CREATE DATABASE \"{escaped}\" WITH ENCODING 'UTF8'"),
        "mysql"    => format!("CREATE DATABASE `{escaped}` DEFAULT CHARSET utf8mb4"),
        _ => unreachable!(),
    };
    sqlx::query(&sql).execute(&admin_pool).await?;
    Ok(())
}

// ---------- DSN 解析（极简，只覆盖我们用到的字段）----------
#[derive(Default)]
struct DsnParts {
    scheme: String,
    userinfo: String, // "user:pass" 或空
    host: String,
    port: String,     // 带冒号 ":5432" 或空
    path: String,     // "/dbname"
    query: String,    // "?a=b" 或空
}

fn parse_dsn(dsn: &str) -> Option<DsnParts> {
    // scheme
    let s_end = dsn.find("://")?;
    let scheme = dsn[..s_end].to_string();
    let rest = &dsn[s_end + 3..];
    // userinfo@host[:port]/path[?query]
    let (authority, tail) = match rest.find('/') {
        Some(i) => (&rest[..i], &rest[i..]),
        None => (rest, ""),
    };
    let (path, query) = match tail.find('?') {
        Some(i) => (tail[..i].to_string(), tail[i..].to_string()),
        None => (tail.to_string(), String::new()),
    };

    let (userinfo, host_port) = match authority.find('@') {
        Some(i) => (authority[..i].to_string(), authority[i + 1..].to_string()),
        None => (String::new(), authority.to_string()),
    };
    let (host, port) = match host_port.rfind(':') {
        Some(i) => (host_port[..i].to_string(), format!(":{}", &host_port[i + 1..])),
        None => (host_port.to_string(), String::new()),
    };

    Some(DsnParts { scheme, userinfo, host, port, path, query })
}

fn rebuild_dsn(p: &DsnParts, backend: &str) -> String {
    let auth = if p.userinfo.is_empty() { String::new() } else { format!("{}@", p.userinfo) };
    let path: String = if p.path.is_empty() {
        match backend {
            "postgres" => "/postgres".to_string(),
            "mysql"    => "/mysql".to_string(),
            _ => "/".to_string(),
        }
    } else { p.path.clone() };
    format!("{}://{}{}{}{}{}", p.scheme, auth, p.host, p.port, path, p.query)
}

async fn connect_and_migrate(dsn: &str) -> Result<Pool<Any>, sqlx::Error> {
    sqlx::any::install_default_drivers();
    let backend = scheme(dsn);

    // SQLite：内存库每连接独立实例 → pool size 锁 1；
    // 外加 journal_mode=MEMORY + synchronous=OFF 绕沙箱文件限制。
    // 文件库也受益（journal 不写磁盘）。
    let pool = if backend == "sqlite" {
        use sqlx::pool::PoolOptions;
        PoolOptions::<Any>::new()
            .max_connections(1)
            .connect(dsn)
            .await?
    } else {
        Pool::<Any>::connect(dsn).await?
    };

    if backend == "sqlite" {
        sqlx::query("PRAGMA journal_mode=MEMORY").execute(&pool).await?;
        sqlx::query("PRAGMA synchronous=OFF").execute(&pool).await?;
    }

    run_migrations(&pool, backend).await?;
    Ok(pool)
}

/// 自动探测机器公网 IPv4 地址。
///
/// 优先级（快 → 慢）：
///   1) `NSBC_PUBLIC_IP` 环境变量（运维显式指定，最高优先，零延迟）
///   2) `NSBC_SKIP_PUBLIC_IP_DETECT=1` → 跳过探测返回 None（纯内网/完全 NAT 场景）
///   3) 国内优先：ip.sb (10ms) → ip138 (50ms) → myip.ipip.net (30ms)
///      全部 1.5s 超时并行，第一个返回胜出
///   4) 国际 fallback：ifconfig.me / api.ipify.org / ident.me（各 1.5s 超时）
///
/// 国内服务器走阿里云国际出口时，ifconfig.me 等海外服务可达但 >500ms；
/// ip.sb / ipip.net 走国内线路 <20ms 稳定。先跑国内轮次，国内全挂才 fallback。
///
/// 实现用 `std::process::Command` 调 `curl`（云服务器必有），零新 crate 依赖。
async fn detect_public_ip() -> Option<String> {
    // 1) env var 最高优先
    if let Ok(v) = std::env::var("NSBC_PUBLIC_IP") {
        let v = v.trim().to_string();
        if is_valid_ipv4(&v) {
            tracing::info!(ip = %v, "🌐 NSBC_PUBLIC_IP env 生效");
            return Some(v);
        }
    }

    // 2) 显式跳过探测
    if std::env::var("NSBC_SKIP_PUBLIC_IP_DETECT").ok()
        .map(|v| v == "1" || v.eq_ignore_ascii_case("true"))
        .unwrap_or(false)
    {
        tracing::info!("🌐 NSBC_SKIP_PUBLIC_IP_DETECT=1 — 跳过公网 IP 探测");
        return None;
    }

    // 3) 国内优先（阿里云/腾讯云国内出口访问 <20ms；各 1.5s 超时）
    let domestic: &[&str] = &[
        "https://ip.sb/ip",          // 七牛云 IP 检测，国内最快
        "https://myip.ipip.net",       // ipip.net 国内节点
        "https://api.ip.sb/ip",        // ip.sb API 路径变体
        "https://ip138.com/ips138.asp?action=ip", // ip138
    ];

    if let Some(ip) = probe_batch(domestic, 1500).await {
        return Some(ip);
    }

    // 4) 国际 fallback（国内全挂才跑；不常见但覆盖特殊网络场景）
    let international: &[&str] = &[
        "https://ifconfig.me/ip",
        "https://api.ipify.org",
        "https://ident.me",
    ];

    probe_batch(international, 1500).await
}

/// 并行探测一批 IP 服务，**第一个返回合法 IPv4 的立刻胜出**（不等其他）。
/// 全部失败/超时 → None。单次请求最大超时 ms。
///
/// 关键：所有 curl 同时用 `spawn_blocking` 并行启动，用 tokio select! 真·抢跑——
/// 最快的那个（国内 ip.sb <20ms）返回合法 IP 就立即返回，不等最慢的超时。
async fn probe_batch(services: &[&str], timeout_ms: u64) -> Option<String> {
    let timeout_s = (timeout_ms as f64 / 1000.0).ceil() as u64;

    // 并行启动所有 curl
    let mut handles: Vec<tokio::task::JoinHandle<Option<String>>> = Vec::with_capacity(services.len());
    for url in services {
        let url = url.to_string();
        let t = timeout_s;
        handles.push(tokio::task::spawn_blocking(move || {
            let out = std::process::Command::new("curl")
                .arg("-s")
                .arg("--max-time").arg(t.to_string())
                .arg("-4")
                .arg("--ipv4")
                .arg("--connect-timeout").arg(t.to_string())
                .arg(&url)
                .output();
            match out {
                Ok(o) if o.status.success() => {
                    let s = String::from_utf8_lossy(&o.stdout).trim().to_string();
                    if is_valid_ipv4(&s) { Some(s) } else { None }
                }
                _ => None,
            }
        }));
    }

    // 真·抢跑：只对已完成的 handle 取结果，没完成的 yield 让出后下一轮再看
    loop {
        let mut all_done = true;
        let mut found = None;
        for h in handles.iter_mut() {
            if !h.is_finished() {
                all_done = false;
                continue;  // 还在跑，跳过不阻塞
            }
            // 已完成 → 瞬间 await（不阻塞）
            if let Ok(Some(ip)) = h.await {
                found = Some(ip);
                break;
            }
        }
        if let Some(ip) = found { return Some(ip); }
        if all_done { break; }
        tokio::task::yield_now().await;  // 让出 CPU，blocking 线程跑完后再来
    }
    None
}

/// 极简 IPv4 验证：四段 0-255 数字，点分隔。
/// 不依赖 std::net::Ipv4Addr（其实也可以用，但保持零依赖风格）。
fn is_valid_ipv4(s: &str) -> bool {
    let mut parts = 0u32;
    for seg in s.split('.') {
        if parts >= 4 { return false; }
        parts += 1;
        if seg.is_empty() || seg.len() > 3 { return false; }
        if !seg.chars().all(|c| c.is_ascii_digit()) { return false; }
        if let Ok(n) = seg.parse::<u16>() {
            if n > 255 { return false; }
        } else {
            return false;
        }
    }
    parts == 4
}

/// 执行 DDL（幂等，所有语句带 IF NOT EXISTS）。
/// 三后端语法差异在各自分支内处理。
async fn run_migrations(pool: &Pool<Any>, backend: &str) -> Result<(), sqlx::Error> {
    // --- cdr 表（CDR 通话记录，承载千万~亿级数据）---
    // answer_time / billsec 是 INSERT 必需列（之前手动 ALTER 补过，新 DB 必须建在表结构里）。
    // ================================================================
    // 租户（REQ-TENANT-001）——租户维度隔离
    // tenant_id 是所有业务表的外键；tenant_name 是人类可读标识，日志/CDR/API 全带。
    // 识别规则（呼入时匹配）：domain 匹配 SIP Realm、did_prefix 匹配入站 DID、trunk 匹配入中继名。
    // 默认租户 (id=1, name='default') 在 run_migrations 末尾 INSERT，兼容零租户时代的老数据。
    // ================================================================
    let _ = sqlx::query(&match backend {
        "sqlite" =>
            "CREATE TABLE IF NOT EXISTS tenants (
                id             INTEGER PRIMARY KEY AUTOINCREMENT,
                tenant_name    TEXT    NOT NULL UNIQUE,
                description    TEXT    NOT NULL DEFAULT '',
                enabled        INTEGER NOT NULL DEFAULT 1,
                match_domain   TEXT    NOT NULL DEFAULT '',
                match_did_prefix TEXT  NOT NULL DEFAULT '',
                match_trunk    TEXT    NOT NULL DEFAULT '',
                created_at     TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at     TEXT    NOT NULL DEFAULT (datetime('now'))
            )",
        "postgres" =>
            "CREATE TABLE IF NOT EXISTS tenants (
                id             BIGSERIAL PRIMARY KEY,
                tenant_name    VARCHAR(64)  NOT NULL UNIQUE,
                description    VARCHAR(256) NOT NULL DEFAULT '',
                enabled        INTEGER     NOT NULL DEFAULT 1,
                match_domain   VARCHAR(128) NOT NULL DEFAULT '',
                match_did_prefix VARCHAR(64) NOT NULL DEFAULT '',
                match_trunk    VARCHAR(64)  NOT NULL DEFAULT '',
                created_at     TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                updated_at     TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z')
            )",
        _ =>
            "CREATE TABLE IF NOT EXISTS tenants (
                id             BIGINT AUTO_INCREMENT PRIMARY KEY,
                tenant_name    VARCHAR(64)  NOT NULL UNIQUE,
                description    VARCHAR(256) NOT NULL DEFAULT '',
                enabled        TINYINT(1)   NOT NULL DEFAULT 1,
                match_domain   VARCHAR(128) NOT NULL DEFAULT '',
                match_did_prefix VARCHAR(64) NOT NULL DEFAULT '',
                match_trunk    VARCHAR(64)  NOT NULL DEFAULT '',
                created_at     TEXT NOT NULL DEFAULT (CAST(CURRENT_TIMESTAMP(3) AS CHAR)),
                updated_at     TEXT NOT NULL DEFAULT (CAST(CURRENT_TIMESTAMP(3) AS CHAR))
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
    }).execute(pool).await;

    // 确保默认租户存在（id=1, name='nsbc.local'——即 SIP 默认 realm）
    {
        let cnt: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM tenants")
            .fetch_one(pool).await.unwrap_or(0);
        if cnt == 0 {
            let _ = sqlx::query(
                "INSERT INTO tenants (id, tenant_name, description, enabled) VALUES (1, 'nsbc.local', 'Default SIP realm', 1)"
            ).execute(pool).await;
            // 自增 ID 对齐（PG 用 setval、SQLite AUTOINCREMENT 不管、MySQL AUTO_INCREMENT 不管）
            let _ = sqlx::query("SELECT setval(pg_get_serial_sequence('tenants','id'), GREATEST(COALESCE((SELECT MAX(id) FROM tenants),1),1), true)").execute(pool).await;
        }
    }

    // ================================================================
    // CDR（呼叫详细记录）
    // ================================================================
    // 索引策略：单 start_time 索引已覆盖时间范围查询；caller+callee 各单独索引覆盖号码过滤；
    // 复合索引 (start_time, caller) / (start_time, callee) 覆盖最常见的"按时间范围+号码"查询模式。
    // SQLite：无原生分区，靠 WAL + auto_vacuum + 好索引；单表 1-2 亿行实测可用。
    // PostgreSQL：推荐按月分区（RANGE PARTITION BY start_time_date），见 run_cdr_partitioning()。
    // MySQL：推荐 PARTITION BY RANGE (TO_DAYS(start_time))，见 run_cdr_partitioning()。
    let cdr_ddl = match backend {
        "sqlite" =>
            "CREATE TABLE IF NOT EXISTS cdr (
                id           INTEGER PRIMARY KEY AUTOINCREMENT,
                call_id      TEXT    NOT NULL UNIQUE,
                a_call_id    TEXT,
                b_call_id    TEXT,
                caller       TEXT,
                callee       TEXT,
                start_time   TEXT    NOT NULL,
                answer_time  TEXT,
                end_time     TEXT,
                duration_sec INTEGER,
                billsec      INTEGER,
                cause_code   INTEGER,
                cause_text   TEXT,
                leg_a_final  INTEGER,
                leg_b_final  INTEGER,
                metadata     TEXT,
                created_at   TEXT    NOT NULL DEFAULT (datetime('now'))
            )",
        "postgres" =>
            "CREATE TABLE IF NOT EXISTS cdr (
                id           BIGSERIAL PRIMARY KEY,
                call_id      VARCHAR(127) NOT NULL UNIQUE,
                a_call_id    VARCHAR(127),
                b_call_id    VARCHAR(127),
                caller       VARCHAR(64),
                callee       VARCHAR(64),
                start_time   TEXT         NOT NULL,
                answer_time  TEXT,
                end_time     TEXT,
                duration_sec INTEGER,
                billsec      INTEGER,
                cause_code   INTEGER,
                cause_text   VARCHAR(64),
                leg_a_final  INTEGER,
                leg_b_final  INTEGER,
                metadata     TEXT,
                created_at   TEXT    NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z')
            )",
        "mysql" =>
            "CREATE TABLE IF NOT EXISTS cdr (
                id           BIGINT AUTO_INCREMENT PRIMARY KEY,
                call_id      VARCHAR(127) NOT NULL UNIQUE,
                a_call_id    VARCHAR(127),
                b_call_id    VARCHAR(127),
                caller       VARCHAR(64),
                callee       VARCHAR(64),
                start_time   TEXT         NOT NULL,
                answer_time  TEXT,
                end_time     TEXT,
                duration_sec INT,
                billsec      INT,
                cause_code   INT,
                cause_text   VARCHAR(64),
                leg_a_final  INT,
                leg_b_final  INT,
                metadata     JSON,
                created_at   DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        _ => {
            return Err(sqlx::Error::Configuration(
                format!("unsupported NSBC_DB_DSN scheme: {backend}").into()
            ));
        }
    };
    sqlx::query(cdr_ddl).execute(pool).await?;

    // --- cdr 索引（幂等，IF NOT EXISTS）---
    // 单列索引：caller / callee / start_time / cause_code 覆盖基础过滤；
    // 复合索引 (start_time, caller) / (start_time, callee) 覆盖"时间范围+号码"查询模式。
    // 注意：复合索引不能走 format! 循环（会拼成 ON cdr((a,b)) → SQLite row value misused）。
    let cdr_single_indexes: &[(&str, &str)] = &[
        ("idx_cdr_start",  "start_time"),
        ("idx_cdr_caller", "caller"),
        ("idx_cdr_callee", "callee"),
        ("idx_cdr_cause",  "cause_code"),
    ];
    for (idx, col) in cdr_single_indexes {
        sqlx::query(&format!(
            "CREATE INDEX IF NOT EXISTS {idx} ON cdr({col})"
        )).execute(pool).await?;
    }
    // 复合索引：完整 SQL，避免双括号
    let cdr_composite_indexes: &[&str] = &[
        "CREATE INDEX IF NOT EXISTS idx_cdr_start_caller  ON cdr(start_time, caller)",
        "CREATE INDEX IF NOT EXISTS idx_cdr_start_callee  ON cdr(start_time, callee)",
        "CREATE INDEX IF NOT EXISTS idx_cdr_cause_start   ON cdr(cause_code, start_time)",
    ];
    for sql in cdr_composite_indexes {
        sqlx::query(sql).execute(pool).await?;
    }

    // --- 升级迁移：旧表追加 answer_time / billsec（幂等）---
    match backend {
        "sqlite" => {
            let cols: Vec<(i64, String)> = sqlx::query_as("PRAGMA table_info(cdr)")
                .fetch_all(pool).await.unwrap_or_default();
            let col_names: Vec<&str> = cols.iter().map(|(_, c)| c.as_str()).collect();
            if !col_names.contains(&"answer_time") {
                let _ = sqlx::query("ALTER TABLE cdr ADD COLUMN answer_time TEXT").execute(pool).await;
            }
            if !col_names.contains(&"billsec") {
                let _ = sqlx::query("ALTER TABLE cdr ADD COLUMN billsec INTEGER").execute(pool).await;
            }
        }
        _ => {
            let _ = sqlx::query("ALTER TABLE cdr ADD COLUMN IF NOT EXISTS answer_time TEXT").execute(pool).await;
            let _ = sqlx::query("ALTER TABLE cdr ADD COLUMN IF NOT EXISTS billsec INTEGER").execute(pool).await;
        }
    }

    // SQLite：打开 WAL + auto_vacuum=INCREMENTAL（千万级数据必备）
    if backend == "sqlite" {
        let _ = sqlx::query("PRAGMA journal_mode=WAL").execute(pool).await;
        let _ = sqlx::query("PRAGMA auto_vacuum=INCREMENTAL").execute(pool).await;
    }

    // --- config_history ---
    let cfg_ddl = match backend {
        "sqlite" =>
            "CREATE TABLE IF NOT EXISTS config_history (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                changed_at  TEXT    NOT NULL DEFAULT (datetime('now')),
                actor       TEXT    NOT NULL,
                action      TEXT    NOT NULL,
                key         TEXT,
                old_value   TEXT,
                new_value   TEXT,
                session_id  TEXT
            )",
        "postgres" =>
            "CREATE TABLE IF NOT EXISTS config_history (
                id          BIGSERIAL PRIMARY KEY,
                changed_at  TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                actor       VARCHAR(64) NOT NULL,
                action      VARCHAR(16) NOT NULL,
                key         VARCHAR(256),
                old_value   TEXT,
                new_value   TEXT,
                session_id  VARCHAR(128)
            )",
        "mysql" =>
            "CREATE TABLE IF NOT EXISTS config_history (
                id          BIGINT AUTO_INCREMENT PRIMARY KEY,
                changed_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                actor       VARCHAR(64) NOT NULL,
                action      VARCHAR(16) NOT NULL,
                key         VARCHAR(256),
                old_value   TEXT,
                new_value   TEXT,
                session_id  VARCHAR(128)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        _ => unreachable!(),
    };
    sqlx::query(cfg_ddl).execute(pool).await?;

    // --- audit_log（Web 侧认证/操作事件）---
    let audit_ddl = match backend {
        "sqlite" =>
            "CREATE TABLE IF NOT EXISTS audit_log (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                ts          TEXT    NOT NULL DEFAULT (datetime('now')),
                event       TEXT    NOT NULL,
                actor       TEXT,
                ip          TEXT,
                target      TEXT,
                status      TEXT,
                detail      TEXT
            )",
        "postgres" =>
            "CREATE TABLE IF NOT EXISTS audit_log (
                id          BIGSERIAL PRIMARY KEY,
                ts          TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                event       VARCHAR(32) NOT NULL,
                actor       VARCHAR(64),
                ip          INET,
                target      VARCHAR(256),
                status      VARCHAR(16),
                detail      TEXT
            )",
        "mysql" =>
            "CREATE TABLE IF NOT EXISTS audit_log (
                id          BIGINT AUTO_INCREMENT PRIMARY KEY,
                ts          DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                event       VARCHAR(32) NOT NULL,
                actor       VARCHAR(64),
                ip          VARCHAR(64),
                target      VARCHAR(256),
                status      VARCHAR(16),
                detail      JSON
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        _ => unreachable!(),
    };
    sqlx::query(audit_ddl).execute(pool).await?;

    // --- registrations（REQ-DATA-SVC-002 C→Web register.write 的落库目标）---
    // C 核心经 UDS 推送的注册快照（分机 + register 型中继出站注册状态）。
    // user_name + instance_id 唯一（多终端/GRUU 场景），重复推送 UPSERT 刷新。
    // 时间列统一 TEXT 存 ISO 8601 字符串（与 cdr.start_time 同模式，三后端兼容）。
    // NAT 元数据（REQ-NAT-001）：received_ip/received_port/nat_detected/
    // public_contact/transport 由 C 侧 registrar.c 检测填充，Web 端原样落库。
    let reg_ddl = match backend {
        "sqlite" =>
            "CREATE TABLE IF NOT EXISTS registrations (
                id             INTEGER PRIMARY KEY AUTOINCREMENT,
                user_name      TEXT    NOT NULL,
                contact        TEXT,
                instance_id    TEXT    NOT NULL DEFAULT '',
                agent          TEXT,
                expires_at     TEXT,
                registered_at  TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at     TEXT,
                received_ip    TEXT    NOT NULL DEFAULT '',
                received_port  INTEGER NOT NULL DEFAULT 0,
                nat_detected   INTEGER NOT NULL DEFAULT 0,
                public_contact TEXT    NOT NULL DEFAULT '',
                transport      TEXT    NOT NULL DEFAULT '',
                UNIQUE (user_name, instance_id)
            )",
        "postgres" =>
            "CREATE TABLE IF NOT EXISTS registrations (
                id             BIGSERIAL PRIMARY KEY,
                user_name      VARCHAR(64) NOT NULL,
                contact        TEXT,
                instance_id    VARCHAR(128) NOT NULL DEFAULT '',
                agent          TEXT,
                expires_at     TEXT,
                registered_at  TEXT    NOT NULL DEFAULT NOW()::text,
                updated_at     TEXT,
                received_ip    VARCHAR(64) NOT NULL DEFAULT '',
                received_port  INTEGER NOT NULL DEFAULT 0,
                nat_detected   INTEGER NOT NULL DEFAULT 0,
                public_contact TEXT    NOT NULL DEFAULT '',
                transport      VARCHAR(16) NOT NULL DEFAULT '',
                UNIQUE (user_name, instance_id)
            )",
        "mysql" =>
            "CREATE TABLE IF NOT EXISTS registrations (
                id             BIGINT AUTO_INCREMENT PRIMARY KEY,
                user_name      VARCHAR(64) NOT NULL,
                contact        TEXT,
                instance_id    VARCHAR(128) NOT NULL DEFAULT '',
                agent          TEXT,
                expires_at     TEXT,
                registered_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                updated_at     TEXT,
                received_ip    VARCHAR(64) NOT NULL DEFAULT '',
                received_port  INT NOT NULL DEFAULT 0,
                nat_detected   TINYINT(1) NOT NULL DEFAULT 0,
                public_contact TEXT NOT NULL DEFAULT '',
                transport      VARCHAR(16) NOT NULL DEFAULT '',
                UNIQUE KEY uq_reg_user_inst (user_name, instance_id)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        _ => unreachable!(),
    };
    sqlx::query(reg_ddl).execute(pool).await?;

    // --- registrations NAT 字段 ALTER TABLE 迁移（REQ-NAT-001）---
    // 对已有 registrations 表补 5 个 NAT 列；新表（CREATE TABLE IF NOT EXISTS）已包含，
    // 此 ALTER 仅对"旧表升级"场景生效。三后端各自 IF NOT EXISTS。
    {
        let nat_cols: &[(&str, &str, &str)] = &[
            ("received_ip",    "TEXT NOT NULL DEFAULT ''",          "VARCHAR(64) NOT NULL DEFAULT ''"),
            ("received_port",  "INTEGER NOT NULL DEFAULT 0",        "INTEGER NOT NULL DEFAULT 0"),
            ("nat_detected",   "INTEGER NOT NULL DEFAULT 0",        "INTEGER NOT NULL DEFAULT 0"),
            ("public_contact", "TEXT NOT NULL DEFAULT ''",          "TEXT NOT NULL DEFAULT ''"),
            ("transport",      "TEXT NOT NULL DEFAULT ''",          "VARCHAR(16) NOT NULL DEFAULT ''"),
        ];
        if backend == "sqlite" {
            let cols: Vec<(i64, String)> =
                sqlx::query_as("PRAGMA table_info(registrations)")
                    .fetch_all(pool).await.unwrap_or_default();
            let col_names: Vec<&str> = cols.iter().map(|(_, c)| c.as_str()).collect();
            for (name, _, _) in nat_cols {
                if !col_names.contains(name) {
                    let _ = sqlx::query(&format!(
                        "ALTER TABLE registrations ADD COLUMN {} {}", name, "TEXT NOT NULL DEFAULT ''"
                    )).execute(pool).await;
                }
            }
        } else if backend == "postgres" {
            for (name, _, pg_def) in nat_cols {
                let _ = sqlx::query(&format!(
                    "ALTER TABLE registrations ADD COLUMN IF NOT EXISTS {} {}", name, pg_def
                )).execute(pool).await;
            }
        } else if backend == "mysql" {
            let mut sql = String::from("ALTER TABLE registrations ");
            for (i, (name, _, _)) in nat_cols.iter().enumerate() {
                let comma = if i > 0 { ", " } else { "" };
                let def = match *name {
                    "received_port" => "INT NOT NULL DEFAULT 0",
                    "nat_detected"  => "TINYINT(1) NOT NULL DEFAULT 0",
                    "transport"     => "VARCHAR(16) NOT NULL DEFAULT ''",
                    _               => "VARCHAR(256) NOT NULL DEFAULT ''",
                };
                sql.push_str(&format!("{}ADD COLUMN {} {}", comma, name, def));
            }
            let _ = sqlx::query(&sql).execute(pool).await;
        }
    }

    // --- runtime_state（REQ-DATA-SVC-002 runtime.write 的落库目标）---
    // 呼叫状态变迁轨迹（DIALING/EARLY/ANSWERED/HANGUP...），CDR 之外的实时轨迹。
    // 注意：建表与索引必须拆成两条语句（sqlx prepared statement 不支持 `;` 链）。
    let rt_ddl = match backend {
        "sqlite" =>
            "CREATE TABLE IF NOT EXISTS runtime_state (
                id       INTEGER PRIMARY KEY AUTOINCREMENT,
                call_id  TEXT    NOT NULL,
                state    TEXT    NOT NULL,
                detail   TEXT,
                ts       TEXT    NOT NULL DEFAULT (datetime('now'))
            )",
        "postgres" =>
            "CREATE TABLE IF NOT EXISTS runtime_state (
                id       BIGSERIAL PRIMARY KEY,
                call_id  VARCHAR(128) NOT NULL,
                state    VARCHAR(32)  NOT NULL,
                detail   TEXT,
                ts       TEXT    NOT NULL DEFAULT NOW()::text
            )",
        "mysql" =>
            "CREATE TABLE IF NOT EXISTS runtime_state (
                id       BIGINT AUTO_INCREMENT PRIMARY KEY,
                call_id  VARCHAR(128) NOT NULL,
                state    VARCHAR(32)  NOT NULL,
                detail   JSON,
                ts       VARCHAR(40)  NOT NULL,
                INDEX idx_rt_call (call_id)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        _ => unreachable!(),
    };
    sqlx::query(rt_ddl).execute(pool).await?;
    if backend != "mysql" {
        sqlx::query("CREATE INDEX IF NOT EXISTS idx_rt_call ON runtime_state(call_id)")
            .execute(pool).await?;
    }

    // --- calls（REQ-DATA-SVC-002 session.write 的落库目标）---
    // 实时会话快照（UPDATE ON CONFLICT），call_id 唯一键；每次 session 事件
    // 刷新 state/event/cause/last_seen；from_num/to_num 采用 CASE WHEN 空值保护
    // （session 事件可能只带 call_id + state，号码字段留空，避免初始化阶段
    // 把已有的号码覆盖成空）。
    let calls_ddl = match backend {
        "sqlite" =>
            "CREATE TABLE IF NOT EXISTS calls (
                id        INTEGER PRIMARY KEY AUTOINCREMENT,
                call_id   TEXT    NOT NULL UNIQUE,
                from_num  TEXT    NOT NULL DEFAULT '',
                to_num    TEXT    NOT NULL DEFAULT '',
                dir       TEXT    NOT NULL DEFAULT 'inbound',
                state     TEXT    NOT NULL DEFAULT '',
                event     TEXT    NOT NULL DEFAULT '',
                cause     INTEGER NOT NULL DEFAULT 0,
                from_ip   TEXT    NOT NULL DEFAULT '',
                codec_a   TEXT    NOT NULL DEFAULT '',
                codec_b   TEXT    NOT NULL DEFAULT '',
                a_peer_ip TEXT    NOT NULL DEFAULT '',
                b_peer_ip TEXT    NOT NULL DEFAULT '',
                last_seen TEXT    NOT NULL DEFAULT (datetime('now'))
            )",
        "postgres" =>
            "CREATE TABLE IF NOT EXISTS calls (
                id        BIGSERIAL PRIMARY KEY,
                call_id   VARCHAR(128) NOT NULL UNIQUE,
                from_num  VARCHAR(64)  NOT NULL DEFAULT '',
                to_num    VARCHAR(64)  NOT NULL DEFAULT '',
                dir       VARCHAR(16)  NOT NULL DEFAULT 'inbound',
                state     VARCHAR(32)  NOT NULL DEFAULT '',
                event     VARCHAR(32)  NOT NULL DEFAULT '',
                cause     INTEGER      NOT NULL DEFAULT 0,
                from_ip   VARCHAR(64)  NOT NULL DEFAULT '',
                codec_a   VARCHAR(32)  NOT NULL DEFAULT '',
                codec_b   VARCHAR(32)  NOT NULL DEFAULT '',
                a_peer_ip VARCHAR(64)  NOT NULL DEFAULT '',
                b_peer_ip VARCHAR(64)  NOT NULL DEFAULT '',
                last_seen TEXT    NOT NULL DEFAULT NOW()::text
            )",
        "mysql" =>
            "CREATE TABLE IF NOT EXISTS calls (
                id        BIGINT AUTO_INCREMENT PRIMARY KEY,
                call_id   VARCHAR(128) NOT NULL UNIQUE,
                from_num  VARCHAR(64)  NOT NULL DEFAULT '',
                to_num    VARCHAR(64)  NOT NULL DEFAULT '',
                dir       VARCHAR(16)  NOT NULL DEFAULT 'inbound',
                state     VARCHAR(32)  NOT NULL DEFAULT '',
                event     VARCHAR(32)  NOT NULL DEFAULT '',
                cause     INT          NOT NULL DEFAULT 0,
                from_ip   VARCHAR(64)  NOT NULL DEFAULT '',
                codec_a   VARCHAR(32)  NOT NULL DEFAULT '',
                codec_b   VARCHAR(32)  NOT NULL DEFAULT '',
                a_peer_ip VARCHAR(64)  NOT NULL DEFAULT '',
                b_peer_ip VARCHAR(64)  NOT NULL DEFAULT '',
                last_seen DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        _ => unreachable!(),
    };
    sqlx::query(calls_ddl).execute(pool).await?;
    if backend != "mysql" {
        sqlx::query("CREATE INDEX IF NOT EXISTS idx_calls_state ON calls(state)")
            .execute(pool).await?;
    }

    // --- 自动迁移：已有 calls 表缺新列时补（SQLite ALTER TABLE ADD COLUMN 安全幂等）---
    // from_ip / codec_a / codec_b / a_peer_ip / b_peer_ip —— 本次新增列
    for (col, def) in &[
        ("from_ip",   "TEXT NOT NULL DEFAULT ''"),
        ("codec_a",   "TEXT NOT NULL DEFAULT ''"),
        ("codec_b",   "TEXT NOT NULL DEFAULT ''"),
        ("a_peer_ip", "TEXT NOT NULL DEFAULT ''"),
        ("b_peer_ip", "TEXT NOT NULL DEFAULT ''"),
    ] {
        let _ = sqlx::query(&format!(
            "ALTER TABLE calls ADD COLUMN {} {}", col, def
        )).execute(pool).await; // 列已存在 → 静默忽略（sqlite / pg / mysql 皆不报错）
    }

    // --- extensions（REQ-WEB-016 分机管理 CRUD）---
    // 每个分机：extension（分机号）+ ha1（SIP 认证 hash）+ realm（SIP 域）
    // + enabled（是否启用）+ clip（强制 CLIP）+ clir（CLIR Privacy:id）+
    // max_contacts（同时注册数）+ dialplan_match（DP 匹配规则 JSON）+
    // description（备注）。extension + realm 联合唯一键（同一域内分机号唯一）。
    let ext_ddl = match backend {
        "sqlite" =>
            "CREATE TABLE IF NOT EXISTS extensions (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                tenant_id       INTEGER NOT NULL DEFAULT 1,
                extension       TEXT    NOT NULL,
                realm           TEXT    NOT NULL DEFAULT '',
                password        TEXT    NOT NULL DEFAULT '',
                ha1             TEXT    NOT NULL DEFAULT '',
                ha1b            TEXT    NOT NULL DEFAULT '',
                enabled         INTEGER NOT NULL DEFAULT 1,
                clip            TEXT    NOT NULL DEFAULT '',
                clir            INTEGER NOT NULL DEFAULT 0,
                max_contacts    INTEGER NOT NULL DEFAULT 10,
                dialplan_match  TEXT    NOT NULL DEFAULT '{}',
                description     TEXT    NOT NULL DEFAULT '',
                ext_type        TEXT    NOT NULL DEFAULT 'internal',
                external_number TEXT    NOT NULL DEFAULT '',
                trunk_name      TEXT    NOT NULL DEFAULT '',
                display_number  TEXT    NOT NULL DEFAULT '',
                created_at      TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at      TEXT    NOT NULL DEFAULT (datetime('now')),
                UNIQUE(tenant_id, extension, realm)
            )",
        "postgres" =>
            "CREATE TABLE IF NOT EXISTS extensions (
                id              BIGSERIAL PRIMARY KEY,
                tenant_id       BIGINT     NOT NULL DEFAULT 1,
                extension       VARCHAR(64)  NOT NULL,
                realm           VARCHAR(128) NOT NULL DEFAULT '',
                password        TEXT         NOT NULL DEFAULT '',
                ha1             VARCHAR(128) NOT NULL DEFAULT '',
                ha1b            VARCHAR(128) NOT NULL DEFAULT '',
                enabled         INTEGER     NOT NULL DEFAULT 1,
                clip            VARCHAR(64)  NOT NULL DEFAULT '',
                clir            INTEGER     NOT NULL DEFAULT 0,
                max_contacts    INTEGER     NOT NULL DEFAULT 10,
                dialplan_match  TEXT       NOT NULL DEFAULT '{}',
                description     VARCHAR(256) NOT NULL DEFAULT '',
                ext_type        VARCHAR(16)  NOT NULL DEFAULT 'internal',
                external_number VARCHAR(64)  NOT NULL DEFAULT '',
                trunk_name      VARCHAR(64)  NOT NULL DEFAULT '',
                display_number  VARCHAR(64)  NOT NULL DEFAULT '',
                created_at      TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                updated_at      TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                UNIQUE(tenant_id, extension, realm)
            )",
        "mysql" =>
            "CREATE TABLE IF NOT EXISTS extensions (
                id              BIGINT AUTO_INCREMENT PRIMARY KEY,
                tenant_id       BIGINT       NOT NULL DEFAULT 1,
                extension       VARCHAR(64)  NOT NULL,
                realm           VARCHAR(128) NOT NULL DEFAULT '',
                password        TEXT         NOT NULL DEFAULT '',
                ha1             VARCHAR(128) NOT NULL DEFAULT '',
                ha1b            VARCHAR(128) NOT NULL DEFAULT '',
                enabled         TINYINT(1)   NOT NULL DEFAULT 1,
                clip            VARCHAR(64)  NOT NULL DEFAULT '',
                clir            TINYINT(1)   NOT NULL DEFAULT 0,
                max_contacts    INT          NOT NULL DEFAULT 10,
                dialplan_match  TEXT         NOT NULL DEFAULT '{}',
                description     VARCHAR(256) NOT NULL DEFAULT '',
                ext_type        VARCHAR(16)  NOT NULL DEFAULT 'internal',
                external_number VARCHAR(64)  NOT NULL DEFAULT '',
                trunk_name      VARCHAR(64)  NOT NULL DEFAULT '',
                display_number  VARCHAR(64)  NOT NULL DEFAULT '',
                created_at      TEXT         NOT NULL DEFAULT (CAST(CURRENT_TIMESTAMP(3) AS CHAR)),
                updated_at      TEXT         NOT NULL DEFAULT (CAST(CURRENT_TIMESTAMP(3) AS CHAR)),
                UNIQUE KEY uq_ext_tenant_realm (tenant_id, extension, realm)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        _ => unreachable!(),
    };
    sqlx::query(ext_ddl).execute(pool).await?;

    // --- extensions 升级（旧表追加外部分机列 + tenant_id，幂等） ---
    {
        const EXT_REQUIRED_COLS: &[(&str, &str, &str)] = &[
            ("tenant_id",        "INTEGER NOT NULL DEFAULT 1",          "BIGINT NOT NULL DEFAULT 1"),
            ("ext_type",         "TEXT NOT NULL DEFAULT 'internal'",    "VARCHAR(16) NOT NULL DEFAULT 'internal'"),
            ("external_number",  "TEXT NOT NULL DEFAULT ''",             "VARCHAR(64) NOT NULL DEFAULT ''"),
            ("trunk_name",       "TEXT NOT NULL DEFAULT ''",             "VARCHAR(64) NOT NULL DEFAULT ''"),
            ("display_number",   "TEXT NOT NULL DEFAULT ''",             "VARCHAR(64) NOT NULL DEFAULT ''"),
        ];
        if backend == "sqlite" {
            let cols: Vec<(i64, String)> = sqlx::query_as(
                "PRAGMA table_info(extensions)"
            ).fetch_all(pool).await.unwrap_or_default();
            let col_names: Vec<&str> = cols.iter().map(|(_, c)| c.as_str()).collect();
            for (name, sql_type, _) in EXT_REQUIRED_COLS {
                if !col_names.contains(name) {
                    let _ = sqlx::query(&format!(
                        "ALTER TABLE extensions ADD COLUMN {} {}", name, sql_type
                    )).execute(pool).await;
                }
            }
        } else if backend == "postgres" {
            for (name, _pg_type, pg_type) in EXT_REQUIRED_COLS {
                let _ = sqlx::query(&format!(
                    "ALTER TABLE extensions ADD COLUMN IF NOT EXISTS {} {}", name, pg_type
                )).execute(pool).await;
            }
        } else if backend == "mysql" {
            let mut sql = String::from("ALTER TABLE extensions ");
            let mut first = true;
            for (name, _sqlite_type, _pg_type) in EXT_REQUIRED_COLS {
                let comma = if first { "" } else { ", " };
                first = false;
                let def = match *name {
                    "tenant_id" => "BIGINT NOT NULL DEFAULT 1",
                    "ext_type"  => "VARCHAR(16) NOT NULL DEFAULT 'internal'",
                    _           => "VARCHAR(64) NOT NULL DEFAULT ''",
                };
                sql.push_str(&format!("{} ADD COLUMN {} {}", comma, name, def));
            }
            let _ = sqlx::query(&sql).execute(pool).await;
        }
    }

    // extensions 索引
    for (idx, col) in [
        ("idx_ext_enabled", "enabled"),
        ("idx_ext_clip",    "clip"),
    ] {
        sqlx::query(&format!(
            "CREATE INDEX IF NOT EXISTS {idx} ON extensions({col})"
        )).execute(pool).await?;
    }

    // --- gateways（REQ-WEB-016 外线网关逻辑分组，对应 route_group_t）---
    let gw_ddl = match backend {
        "sqlite" =>
            "CREATE TABLE IF NOT EXISTS gateways (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                name        TEXT    NOT NULL UNIQUE,
                description TEXT    NOT NULL DEFAULT '',
                enabled     INTEGER NOT NULL DEFAULT 1,
                created_at  TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at  TEXT    NOT NULL DEFAULT (datetime('now'))
            )",
        "postgres" =>
            "CREATE TABLE IF NOT EXISTS gateways (
                id          BIGSERIAL PRIMARY KEY,
                name        VARCHAR(64)  NOT NULL UNIQUE,
                description VARCHAR(256) NOT NULL DEFAULT '',
                enabled     INTEGER     NOT NULL DEFAULT 1,
                created_at  TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                updated_at  TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z')
            )",
        "mysql" =>
            "CREATE TABLE IF NOT EXISTS gateways (
                id          BIGINT AUTO_INCREMENT PRIMARY KEY,
                name        VARCHAR(64)  NOT NULL UNIQUE,
                description VARCHAR(256) NOT NULL DEFAULT '',
                enabled     TINYINT(1)   NOT NULL DEFAULT 1,
                created_at  DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                updated_at  DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
                UNIQUE KEY uq_gw_name (name)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        _ => unreachable!(),
    };
    sqlx::query(gw_ddl).execute(pool).await?;

    // --- trunks（REQ-WEB-016 中继，对应 trunk_t）---
    let tr_ddl = match backend {
        "sqlite" =>
            "CREATE TABLE IF NOT EXISTS trunks (
                id                      INTEGER PRIMARY KEY AUTOINCREMENT,
                tenant_id               INTEGER NOT NULL DEFAULT 1,
                name                    TEXT    NOT NULL,
                gateway_name            TEXT    NOT NULL DEFAULT '',
                profile_name            TEXT    NOT NULL DEFAULT '',
                kind                    TEXT    NOT NULL DEFAULT 'static_peer',
                proto                   INTEGER NOT NULL DEFAULT 0,
                host                    TEXT    NOT NULL DEFAULT '',
                port                    INTEGER NOT NULL DEFAULT 5060,
                auth_user               TEXT    NOT NULL DEFAULT '',
                realm                   TEXT    NOT NULL DEFAULT '',
                codec_set               TEXT    NOT NULL DEFAULT '',
                weight                  INTEGER NOT NULL DEFAULT 1,
                priority                INTEGER NOT NULL DEFAULT 1,
                enabled                 INTEGER NOT NULL DEFAULT 1,
                max_channels            INTEGER NOT NULL DEFAULT 0,
                failover_code           INTEGER NOT NULL DEFAULT 0,
                description             TEXT    NOT NULL DEFAULT '',
                password                TEXT    NOT NULL DEFAULT '',
                from_user               TEXT    NOT NULL DEFAULT '',
                from_domain             TEXT    NOT NULL DEFAULT '',
                proxy                   TEXT    NOT NULL DEFAULT '',
                register_proxy          TEXT    NOT NULL DEFAULT '',
                expire_seconds          INTEGER NOT NULL DEFAULT 3600,
                retry_seconds           INTEGER NOT NULL DEFAULT 5,
                caller_id_in_from       INTEGER NOT NULL DEFAULT 0,
                contact_params          TEXT    NOT NULL DEFAULT '',
                extension_in_contact    INTEGER NOT NULL DEFAULT 0,
                ping_seconds            INTEGER NOT NULL DEFAULT 0,
                inbound_extension       TEXT    NOT NULL DEFAULT '',
                cid_type                TEXT    NOT NULL DEFAULT '',
                rfc5626                 INTEGER NOT NULL DEFAULT 0,
                reg_id                  TEXT    NOT NULL DEFAULT '',
                peer_ip                 TEXT    NOT NULL DEFAULT '',
                caller_id_pool          TEXT    NOT NULL DEFAULT '',
                per_number_max_channels INTEGER NOT NULL DEFAULT 0,
                custom_headers          TEXT    NOT NULL DEFAULT '',
                created_at              TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at              TEXT    NOT NULL DEFAULT (datetime('now')),
                UNIQUE(tenant_id, name)
            )",
        "postgres" =>
            "CREATE TABLE IF NOT EXISTS trunks (
                id                      BIGSERIAL PRIMARY KEY,
                tenant_id               BIGINT     NOT NULL DEFAULT 1,
                name                    VARCHAR(64) NOT NULL,
                gateway_name            VARCHAR(64)  NOT NULL DEFAULT '',
                profile_name            VARCHAR(64)  NOT NULL DEFAULT '',
                kind                    VARCHAR(16)  NOT NULL DEFAULT 'static_peer',
                proto                   INTEGER      NOT NULL DEFAULT 0,
                host                    VARCHAR(256) NOT NULL DEFAULT '',
                port                    INTEGER      NOT NULL DEFAULT 5060,
                auth_user               VARCHAR(64)  NOT NULL DEFAULT '',
                realm                   VARCHAR(128) NOT NULL DEFAULT '',
                codec_set               VARCHAR(32)  NOT NULL DEFAULT '',
                weight                  INTEGER      NOT NULL DEFAULT 1,
                priority                INTEGER      NOT NULL DEFAULT 1,
                enabled                 INTEGER      NOT NULL DEFAULT 1,
                max_channels            INTEGER      NOT NULL DEFAULT 0,
                failover_code           INTEGER      NOT NULL DEFAULT 0,
                description             VARCHAR(256) NOT NULL DEFAULT '',
                password                VARCHAR(256) NOT NULL DEFAULT '',
                from_user               VARCHAR(64)  NOT NULL DEFAULT '',
                from_domain             VARCHAR(256) NOT NULL DEFAULT '',
                proxy                   VARCHAR(256) NOT NULL DEFAULT '',
                register_proxy          VARCHAR(256) NOT NULL DEFAULT '',
                expire_seconds          INTEGER      NOT NULL DEFAULT 3600,
                retry_seconds           INTEGER      NOT NULL DEFAULT 5,
                caller_id_in_from       INTEGER      NOT NULL DEFAULT 0,
                contact_params          VARCHAR(256) NOT NULL DEFAULT '',
                extension_in_contact    INTEGER      NOT NULL DEFAULT 0,
                ping_seconds            INTEGER      NOT NULL DEFAULT 0,
                inbound_extension       VARCHAR(64)  NOT NULL DEFAULT '',
                cid_type                VARCHAR(32)  NOT NULL DEFAULT '',
                rfc5626                 INTEGER      NOT NULL DEFAULT 0,
                reg_id                  VARCHAR(32)  NOT NULL DEFAULT '',
                peer_ip                 VARCHAR(256) NOT NULL DEFAULT '',
                caller_id_pool          VARCHAR(2048) NOT NULL DEFAULT '',
                per_number_max_channels INTEGER      NOT NULL DEFAULT 0,
                custom_headers          VARCHAR(2048) NOT NULL DEFAULT '',
                created_at      TEXT    NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                updated_at      TEXT    NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                UNIQUE(tenant_id, name)
            )",
        "mysql" =>
            "CREATE TABLE IF NOT EXISTS trunks (
                id                      BIGINT AUTO_INCREMENT PRIMARY KEY,
                tenant_id               BIGINT       NOT NULL DEFAULT 1,
                name                    VARCHAR(64)  NOT NULL,
                gateway_name            VARCHAR(64)  NOT NULL DEFAULT '',
                profile_name            VARCHAR(64)  NOT NULL DEFAULT '',
                kind                    VARCHAR(16)  NOT NULL DEFAULT 'static_peer',
                proto                   INT          NOT NULL DEFAULT 0,
                host                    VARCHAR(256) NOT NULL DEFAULT '',
                port                    INT          NOT NULL DEFAULT 5060,
                auth_user               VARCHAR(64)  NOT NULL DEFAULT '',
                realm                   VARCHAR(128) NOT NULL DEFAULT '',
                codec_set               VARCHAR(32)  NOT NULL DEFAULT '',
                weight                  INT          NOT NULL DEFAULT 1,
                priority                INT          NOT NULL DEFAULT 1,
                enabled                 TINYINT(1)   NOT NULL DEFAULT 1,
                max_channels            INT          NOT NULL DEFAULT 0,
                failover_code           INT          NOT NULL DEFAULT 0,
                description             VARCHAR(256) NOT NULL DEFAULT '',
                password                VARCHAR(256) NOT NULL DEFAULT '',
                from_user               VARCHAR(64)  NOT NULL DEFAULT '',
                from_domain             VARCHAR(256) NOT NULL DEFAULT '',
                proxy                   VARCHAR(256) NOT NULL DEFAULT '',
                register_proxy          VARCHAR(256) NOT NULL DEFAULT '',
                expire_seconds          INT          NOT NULL DEFAULT 3600,
                retry_seconds           INT          NOT NULL DEFAULT 5,
                caller_id_in_from       TINYINT(1)   NOT NULL DEFAULT 0,
                contact_params          VARCHAR(256) NOT NULL DEFAULT '',
                extension_in_contact    TINYINT(1)   NOT NULL DEFAULT 0,
                ping_seconds            INT          NOT NULL DEFAULT 0,
                inbound_extension       VARCHAR(64)  NOT NULL DEFAULT '',
                cid_type                VARCHAR(32)  NOT NULL DEFAULT '',
                rfc5626                 TINYINT(1)   NOT NULL DEFAULT 0,
                reg_id                  VARCHAR(32)  NOT NULL DEFAULT '',
                peer_ip                 VARCHAR(256) NOT NULL DEFAULT '',
                caller_id_pool          VARCHAR(2048) NOT NULL DEFAULT '',
                per_number_max_channels INT          NOT NULL DEFAULT 0,
                custom_headers          VARCHAR(2048) NOT NULL DEFAULT '',
                created_at              DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                updated_at              DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
                UNIQUE KEY uq_tr_name (name)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        _ => unreachable!(),
    };
    sqlx::query(tr_ddl).execute(pool).await?;

    // 升级：旧表追加 FreeSWITCH 兼容 + 用户新增列（幂等）
    // 专家 3.7：明列 REQUIRED_COLS 的 (name, sqlite_type, pg_type) 三元组，避免实现时遗漏
    {
        const TRUNK_REQUIRED_COLS: &[(&str, &str, &str)] = &[
            ("tenant_id",              "INTEGER NOT NULL DEFAULT 1",         "BIGINT NOT NULL DEFAULT 1"),
            // (列名, SQLite 类型, PG 类型) — profile_name 之前版本已在单独块加过，这里保留做幂等
            ("profile_name",          "TEXT NOT NULL DEFAULT ''",          "VARCHAR(64) NOT NULL DEFAULT ''"),
            ("password",               "TEXT NOT NULL DEFAULT ''",          "VARCHAR(256) NOT NULL DEFAULT ''"),
            ("from_user",              "TEXT NOT NULL DEFAULT ''",          "VARCHAR(64) NOT NULL DEFAULT ''"),
            ("from_domain",            "TEXT NOT NULL DEFAULT ''",          "VARCHAR(256) NOT NULL DEFAULT ''"),
            ("proxy",                  "TEXT NOT NULL DEFAULT ''",          "VARCHAR(256) NOT NULL DEFAULT ''"),
            ("register_proxy",         "TEXT NOT NULL DEFAULT ''",          "VARCHAR(256) NOT NULL DEFAULT ''"),
            ("expire_seconds",         "INTEGER NOT NULL DEFAULT 3600",     "INTEGER NOT NULL DEFAULT 3600"),
            ("retry_seconds",          "INTEGER NOT NULL DEFAULT 5",        "INTEGER NOT NULL DEFAULT 5"),
            ("caller_id_in_from",      "INTEGER NOT NULL DEFAULT 0",        "INTEGER NOT NULL DEFAULT 0"),
            ("contact_params",         "TEXT NOT NULL DEFAULT ''",          "VARCHAR(256) NOT NULL DEFAULT ''"),
            ("extension_in_contact",   "INTEGER NOT NULL DEFAULT 0",        "INTEGER NOT NULL DEFAULT 0"),
            ("ping_seconds",           "INTEGER NOT NULL DEFAULT 0",        "INTEGER NOT NULL DEFAULT 0"),
            ("inbound_extension",      "TEXT NOT NULL DEFAULT ''",          "VARCHAR(64) NOT NULL DEFAULT ''"),
            ("cid_type",               "TEXT NOT NULL DEFAULT ''",          "VARCHAR(32) NOT NULL DEFAULT ''"),
            ("rfc5626",                "INTEGER NOT NULL DEFAULT 0",        "INTEGER NOT NULL DEFAULT 0"),
            ("reg_id",                 "TEXT NOT NULL DEFAULT ''",          "VARCHAR(32) NOT NULL DEFAULT ''"),
            ("peer_ip",                "TEXT NOT NULL DEFAULT ''",          "VARCHAR(256) NOT NULL DEFAULT ''"),
            ("caller_id_pool",         "TEXT NOT NULL DEFAULT ''",          "VARCHAR(2048) NOT NULL DEFAULT ''"),
            ("per_number_max_channels","INTEGER NOT NULL DEFAULT 0",        "INTEGER NOT NULL DEFAULT 0"),
            ("custom_headers",         "TEXT NOT NULL DEFAULT ''",          "VARCHAR(2048) NOT NULL DEFAULT ''"),
            ("ims_enabled",            "INTEGER NOT NULL DEFAULT 0",        "INTEGER NOT NULL DEFAULT 0"),
            ("ims_domain",             "TEXT NOT NULL DEFAULT ''",          "VARCHAR(128) NOT NULL DEFAULT ''"),
            ("ims_auto_privacy",       "INTEGER NOT NULL DEFAULT 0",        "INTEGER NOT NULL DEFAULT 0"),
            ("ims_auto_pani",          "INTEGER NOT NULL DEFAULT 0",        "INTEGER NOT NULL DEFAULT 0"),
            ("ims_pani_value",         "TEXT NOT NULL DEFAULT ''",          "VARCHAR(128) NOT NULL DEFAULT ''"),
        ];

        if backend == "sqlite" {
            let cols: Vec<(i64, String)> = sqlx::query_as(
                "PRAGMA table_info(trunks)"
            ).fetch_all(pool).await.unwrap_or_default();
            let col_names: Vec<&str> = cols.iter().map(|(_, c)| c.as_str()).collect();
            for (name, sql_type, _pg_type) in TRUNK_REQUIRED_COLS {
                if !col_names.contains(name) {
                    let _ = sqlx::query(&format!(
                        "ALTER TABLE trunks ADD COLUMN {} {}", name, sql_type
                    )).execute(pool).await;
                }
            }
        } else if backend == "postgres" {
            for (name, _sqlite_type, pg_type) in TRUNK_REQUIRED_COLS {
                let _ = sqlx::query(&format!(
                    "ALTER TABLE trunks ADD COLUMN IF NOT EXISTS {} {}", name, pg_type
                )).execute(pool).await;
            }
        } else if backend == "mysql" {
            let mut sql = String::from("ALTER TABLE trunks ");
            let mut first = true;
            for (name, _sqlite_type, _pg_type) in TRUNK_REQUIRED_COLS {
                let comma = if first { "" } else { ", " };
                first = false;
                let def = match *name {
                    "tenant_id"              => "BIGINT NOT NULL DEFAULT 1",
                    "profile_name"          => "VARCHAR(64) NOT NULL DEFAULT ''",
                    "expire_seconds"        => "INT NOT NULL DEFAULT 3600",
                    "retry_seconds"         => "INT NOT NULL DEFAULT 5",
                    "caller_id_in_from" | "extension_in_contact" | "rfc5626"
                    | "per_number_max_channels"
                    | "ims_enabled" | "ims_auto_privacy" | "ims_auto_pani"
                                            => "TINYINT(1) NOT NULL DEFAULT 0",
                    "ping_seconds"          => "INT NOT NULL DEFAULT 0",
                    "caller_id_pool"        => "VARCHAR(2048) NOT NULL DEFAULT ''",
                    "ims_domain" | "ims_pani_value"
                                            => "VARCHAR(128) NOT NULL DEFAULT ''",
                    _ => "VARCHAR(256) NOT NULL DEFAULT ''",
                };
                sql.push_str(&format!("{}ADD COLUMN {} {}", comma, name, def));
            }
            let _ = sqlx::query(&sql).execute(pool).await;
        }
    }

    // trunks 索引
    for (idx, col) in [
        ("idx_tr_enabled",     "enabled"),
        ("idx_tr_gateway",     "gateway_name"),
        ("idx_tr_profile",     "profile_name"),
        ("idx_tr_priority",    "priority"),
    ] {
        sqlx::query(&format!(
            "CREATE INDEX IF NOT EXISTS {idx} ON trunks({col})"
        )).execute(pool).await?;
    }

    // ================ routes（策略组） ================
    let rt_ddl = match backend {
        "sqlite" => r#"
            CREATE TABLE IF NOT EXISTS routes (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                name          TEXT    NOT NULL UNIQUE,
                strategy      TEXT    NOT NULL DEFAULT 'priority',
                enabled       INTEGER NOT NULL DEFAULT 1,
                description   TEXT    NOT NULL DEFAULT '',
                created_at    TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at    TEXT    NOT NULL DEFAULT (datetime('now'))
            )
        "#,
        "postgres" => r#"
            CREATE TABLE IF NOT EXISTS routes (
                id            BIGSERIAL PRIMARY KEY,
                name          TEXT    NOT NULL UNIQUE,
                strategy      TEXT    NOT NULL DEFAULT 'priority',
                enabled       INTEGER NOT NULL DEFAULT 1,
                description   TEXT    NOT NULL DEFAULT '',
                created_at    TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                updated_at    TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z')
            )
        "#,
        _ => r#"
            CREATE TABLE IF NOT EXISTS routes (
                id            BIGINT AUTO_INCREMENT PRIMARY KEY,
                name          VARCHAR(128) NOT NULL,
                strategy      VARCHAR(32)  NOT NULL DEFAULT 'priority',
                enabled       TINYINT(1)   NOT NULL DEFAULT 1,
                description   VARCHAR(512) NOT NULL DEFAULT '',
                created_at    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                updated_at    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
                UNIQUE KEY uq_routes_name (name)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        "#,
    };
    sqlx::query(rt_ddl).execute(pool).await?;

    // ================ dialplan_rules（分机/DID/出局规则） ================
    let dp_ddl = match backend {
        "sqlite" => r#"
            CREATE TABLE IF NOT EXISTS dialplan_rules (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                tenant_id     INTEGER NOT NULL DEFAULT 1,
                name          TEXT    NOT NULL,
                layer         TEXT    NOT NULL DEFAULT 'ext',  -- §15: 保留但废弃，由 target 前缀自动推断
                /* ====== §15 新增 ====== */
                profile_name  TEXT    NOT NULL DEFAULT 'internal',
                src_ip_cidr   TEXT    NOT NULL DEFAULT '10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,127.0.0.0/8',
                /* ===================== */
                match_expr    TEXT    NOT NULL,
                caller_match  TEXT    NOT NULL DEFAULT '',
                prio          INTEGER NOT NULL DEFAULT 1,
                strip         INTEGER NOT NULL DEFAULT 0,
                target        TEXT    NOT NULL,
                rec_action    INTEGER NOT NULL DEFAULT 0,  /* 0=none 1=start 2=stop，由 C 侧 dp_execute 透传 */
                rec_path_template TEXT NOT NULL DEFAULT '',  /* 可选录音路径模板，空=系统默认 yyyymmdd/callid_mix_seq.wav */
                action_json   TEXT    NOT NULL DEFAULT '{}',
                app_chain     TEXT    NOT NULL DEFAULT '[]',  /* REQ-DP-APP-001: Application 链 JSON 数组 */
                enabled       INTEGER NOT NULL DEFAULT 1,
                description   TEXT    NOT NULL DEFAULT '',
                created_at    TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at    TEXT    NOT NULL DEFAULT (datetime('now')),
                UNIQUE(tenant_id, name)
            )
        "#,
        "postgres" => r#"
            CREATE TABLE IF NOT EXISTS dialplan_rules (
                id            BIGSERIAL PRIMARY KEY,
                tenant_id     BIGINT  NOT NULL DEFAULT 1,
                name          TEXT    NOT NULL,
                layer         TEXT    NOT NULL DEFAULT 'ext',  -- §15: 保留但废弃，由 target 前缀自动推断
                /* ====== §15 新增 ====== */
                profile_name  TEXT    NOT NULL DEFAULT 'internal',
                src_ip_cidr   TEXT    NOT NULL DEFAULT '10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,127.0.0.0/8',
                /* ===================== */
                match_expr    TEXT    NOT NULL,
                caller_match  TEXT    NOT NULL DEFAULT '',
                prio          INTEGER NOT NULL DEFAULT 1,
                strip         INTEGER NOT NULL DEFAULT 0,
                target        TEXT    NOT NULL,
                rec_action    INTEGER NOT NULL DEFAULT 0,
                rec_path_template TEXT NOT NULL DEFAULT '',
                action_json   TEXT   NOT NULL DEFAULT '{}',
                app_chain     TEXT   NOT NULL DEFAULT '[]',  /* REQ-DP-APP-001: Application 链 JSON 数组 */
                enabled       INTEGER NOT NULL DEFAULT 1,
                description   TEXT    NOT NULL DEFAULT '',
                created_at    TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                updated_at    TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                UNIQUE(tenant_id, name)
            )
        "#,
        _ => r#"
            CREATE TABLE IF NOT EXISTS dialplan_rules (
                id            BIGINT AUTO_INCREMENT PRIMARY KEY,
                tenant_id     BIGINT       NOT NULL DEFAULT 1,
                name          VARCHAR(128) NOT NULL,
                layer         VARCHAR(16)  NOT NULL DEFAULT 'ext',  -- §15: 保留但废弃，由 target 前缀自动推断
                /* ====== §15 新增 ====== */
                profile_name  VARCHAR(32)  NOT NULL DEFAULT 'internal',
                src_ip_cidr   VARCHAR(128) NOT NULL DEFAULT '10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,127.0.0.0/8',
                /* ===================== */
                match_expr    VARCHAR(256) NOT NULL,
                caller_match  VARCHAR(256) NOT NULL DEFAULT '',
                prio          INT          NOT NULL DEFAULT 1,
                strip         INT          NOT NULL DEFAULT 0,
                target        VARCHAR(256) NOT NULL,
                rec_action    INT          NOT NULL DEFAULT 0,
                rec_path_template VARCHAR(256) NOT NULL DEFAULT '',
                action_json   JSON         NOT NULL,
                app_chain     JSON         NOT NULL,  /* REQ-DP-APP-001: Application 链 JSON 数组 */
                enabled       TINYINT(1)   NOT NULL DEFAULT 1,
                description   VARCHAR(512) NOT NULL DEFAULT '',
                created_at    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                updated_at    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
                UNIQUE KEY uq_dp_tenant_name (tenant_id, name)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        "#,
    };
    sqlx::query(dp_ddl).execute(pool).await?;

    // dialplan_rules 索引
    for (idx, col) in [
        ("idx_dp_layer",   "layer"),
        ("idx_dp_enabled", "enabled"),
        ("idx_dp_prio",    "prio"),
    ] {
        sqlx::query(&format!(
            "CREATE INDEX IF NOT EXISTS {idx} ON dialplan_rules({col})"
        )).execute(pool).await?;
    }

    /* ====== §15 增量 ALTER TABLE：profile_name + src_ip_cidr ======
     * 运行时（表已存在且有数据）：ALTER TABLE ADD COLUMN 自动填 DEFAULT 值
     *   → 旧规则 profile_name='internal'、src_ip_cidr='0.0.0.0/0'
     *   → 行为完全不变（向后兼容）
     * 新建 DB：CREATE TABLE 已包含新列（见 §15.3 DDL）
     *
     * SQLite 不支持一次 ALTER 多列，分开两条
     * MySQL ALTER 支持一次多列
     * PostgreSQL ALTER 支持一次多列 */
    let dp_alter = match backend {
        "mysql" => "ALTER TABLE dialplan_rules \
                    ADD COLUMN profile_name VARCHAR(32) NOT NULL DEFAULT 'internal' AFTER layer, \
                    ADD COLUMN src_ip_cidr  VARCHAR(128) NOT NULL DEFAULT '10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,127.0.0.0/8' AFTER profile_name, \
                    ADD COLUMN rec_action  INT NOT NULL DEFAULT 0 AFTER target, \
                    ADD COLUMN rec_path_template VARCHAR(256) NOT NULL DEFAULT '' AFTER rec_action, \
                    ADD COLUMN app_chain  JSON NOT NULL AFTER action_json",
        _ => {
            // SQLite / PostgreSQL 各自一条
            sqlx::query("ALTER TABLE dialplan_rules ADD COLUMN profile_name TEXT NOT NULL DEFAULT 'internal'")
                .execute(pool).await.ok();  // 列已存在时忽略错误
            sqlx::query("ALTER TABLE dialplan_rules ADD COLUMN src_ip_cidr TEXT NOT NULL DEFAULT '10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,127.0.0.0/8'")
                .execute(pool).await.ok();
            sqlx::query("ALTER TABLE dialplan_rules ADD COLUMN rec_action INTEGER NOT NULL DEFAULT 0")
                .execute(pool).await.ok();
            sqlx::query("ALTER TABLE dialplan_rules ADD COLUMN rec_path_template TEXT NOT NULL DEFAULT ''")
                .execute(pool).await.ok();
            /* REQ-DP-APP-001：Application 链列（存量库补） */
            sqlx::query("ALTER TABLE dialplan_rules ADD COLUMN app_chain TEXT NOT NULL DEFAULT '[]'")
                .execute(pool).await.ok();
            ""  // 返回空串，避免下方重复执行
        }
    };
    if !dp_alter.is_empty() {
        sqlx::query(dp_alter).execute(pool).await.ok();
    }
    // §15 新增：(profile_name, prio) 联合索引——dialplan 热路径高频访问点
    sqlx::query("CREATE INDEX IF NOT EXISTS idx_dp_profile_prio \
                 ON dialplan_rules(profile_name, prio)")
        .execute(pool).await.ok();

    // ================ ha_groups（高可用组） ================
    let ha_ddl = match backend {
        "sqlite" => r#"
            CREATE TABLE IF NOT EXISTS ha_groups (
                id                INTEGER PRIMARY KEY AUTOINCREMENT,
                name              TEXT    NOT NULL UNIQUE,
                enabled           INTEGER NOT NULL DEFAULT 1,
                node_list_json    TEXT    NOT NULL DEFAULT '[]',
                quorum            INTEGER NOT NULL DEFAULT 2,
                election_timeout  INTEGER NOT NULL DEFAULT 5000,
                state             TEXT    NOT NULL DEFAULT 'initializing',
                description       TEXT    NOT NULL DEFAULT '',
                created_at        TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at        TEXT    NOT NULL DEFAULT (datetime('now'))
            )
        "#,
        "postgres" => r#"
            CREATE TABLE IF NOT EXISTS ha_groups (
                id                BIGSERIAL PRIMARY KEY,
                name              TEXT    NOT NULL UNIQUE,
                enabled           INTEGER NOT NULL DEFAULT 1,
                node_list_json    TEXT   NOT NULL DEFAULT '[]',
                quorum            INTEGER NOT NULL DEFAULT 2,
                election_timeout  INTEGER NOT NULL DEFAULT 5000,
                state             TEXT    NOT NULL DEFAULT 'initializing',
                description       TEXT    NOT NULL DEFAULT '',
                created_at        TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                updated_at        TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z')
            )
        "#,
        _ => r#"
            CREATE TABLE IF NOT EXISTS ha_groups (
                id                BIGINT AUTO_INCREMENT PRIMARY KEY,
                name              VARCHAR(128) NOT NULL,
                enabled           TINYINT(1)   NOT NULL DEFAULT 1,
                node_list_json    JSON         NOT NULL,
                quorum            INT          NOT NULL DEFAULT 2,
                election_timeout  INT          NOT NULL DEFAULT 5000,
                state             VARCHAR(32)  NOT NULL DEFAULT 'initializing',
                description       VARCHAR(512) NOT NULL DEFAULT '',
                created_at        DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                updated_at        DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
                UNIQUE KEY uq_ha_name (name)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        "#,
    };
    sqlx::query(ha_ddl).execute(pool).await?;
    sqlx::query(
        "CREATE INDEX IF NOT EXISTS idx_ha_enabled ON ha_groups(enabled)"
    ).execute(pool).await?;

// ================ rate_tables (REQ-OAM-009) ================
let rt_ddl = match backend {
    "sqlite" => r#"
        CREATE TABLE IF NOT EXISTS rate_tables (
            id                 INTEGER PRIMARY KEY AUTOINCREMENT,
            prefix             TEXT    NOT NULL UNIQUE,
            name               TEXT    NOT NULL DEFAULT '',
            price_per_min_uc   INTEGER NOT NULL DEFAULT 0,
            connect_fee_uc     INTEGER NOT NULL DEFAULT 0,
            bill_inc_s         INTEGER NOT NULL DEFAULT 1,
            min_bill_s         INTEGER NOT NULL DEFAULT 0,
            initial_s          INTEGER NOT NULL DEFAULT 0,
            enabled            INTEGER NOT NULL DEFAULT 1,
            description        TEXT    NOT NULL DEFAULT '',
            created_at         TEXT    NOT NULL DEFAULT (datetime('now')),
            updated_at         TEXT    NOT NULL DEFAULT (datetime('now'))
        )
    "#,
    "postgres" => r#"
        CREATE TABLE IF NOT EXISTS rate_tables (
            id                 BIGSERIAL PRIMARY KEY,
            prefix             VARCHAR(24)  NOT NULL UNIQUE,
            name               VARCHAR(48)  NOT NULL DEFAULT '',
            price_per_min_uc   BIGINT       NOT NULL DEFAULT 0,
            connect_fee_uc     BIGINT       NOT NULL DEFAULT 0,
            bill_inc_s         INTEGER      NOT NULL DEFAULT 1,
            min_bill_s         INTEGER      NOT NULL DEFAULT 0,
            initial_s          INTEGER      NOT NULL DEFAULT 0,
            enabled            INTEGER      NOT NULL DEFAULT 1,
            description        VARCHAR(256) NOT NULL DEFAULT '',
            created_at         TEXT    NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
            updated_at         TEXT    NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z')
        )
    "#,
    _ => r#"
        CREATE TABLE IF NOT EXISTS rate_tables (
            id                 BIGINT AUTO_INCREMENT PRIMARY KEY,
            prefix             VARCHAR(24)  NOT NULL UNIQUE,
            name               VARCHAR(48)  NOT NULL DEFAULT '',
            price_per_min_uc   BIGINT       NOT NULL DEFAULT 0,
            connect_fee_uc     BIGINT       NOT NULL DEFAULT 0,
            bill_inc_s         INT          NOT NULL DEFAULT 1,
            min_bill_s         INT          NOT NULL DEFAULT 0,
            initial_s          INT          NOT NULL DEFAULT 0,
            enabled            INTEGER      NOT NULL DEFAULT 1,
            description        VARCHAR(256) NOT NULL DEFAULT '',
            created_at         DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
            updated_at         DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
        )
    "#,
};
sqlx::query(rt_ddl).execute(pool).await?;
sqlx::query("CREATE INDEX IF NOT EXISTS idx_rt_enabled ON rate_tables(enabled)").execute(pool).await?;

// ================ accounts (REQ-OAM-009) ================
let acct_ddl = match backend {
    "sqlite" => r#"
        CREATE TABLE IF NOT EXISTS accounts (
            id               INTEGER PRIMARY KEY AUTOINCREMENT,
            account_key      TEXT    NOT NULL UNIQUE,
            balance_uc       INTEGER NOT NULL DEFAULT 0,
            credit_limit_uc  INTEGER NOT NULL DEFAULT 0,
            description      TEXT    NOT NULL DEFAULT '',
            created_at       TEXT    NOT NULL DEFAULT (datetime('now')),
            updated_at       TEXT    NOT NULL DEFAULT (datetime('now'))
        )
    "#,
    "postgres" => r#"
        CREATE TABLE IF NOT EXISTS accounts (
            id               BIGSERIAL PRIMARY KEY,
            account_key      VARCHAR(64)  NOT NULL UNIQUE,
            balance_uc       BIGINT       NOT NULL DEFAULT 0,
            credit_limit_uc  BIGINT       NOT NULL DEFAULT 0,
            description      VARCHAR(256) NOT NULL DEFAULT '',
            created_at       TEXT    NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
            updated_at       TEXT    NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z')
        )
    "#,
    _ => r#"
        CREATE TABLE IF NOT EXISTS accounts (
            id               BIGINT AUTO_INCREMENT PRIMARY KEY,
            account_key      VARCHAR(64)  NOT NULL UNIQUE,
            balance_uc       BIGINT       NOT NULL DEFAULT 0,
            credit_limit_uc  BIGINT       NOT NULL DEFAULT 0,
            description      VARCHAR(256) NOT NULL DEFAULT '',
            created_at       DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
            updated_at       DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
        )
    "#,
};
sqlx::query(acct_ddl).execute(pool).await?;
    // ================ time_groups（时间策略组） ================
    let tg_ddl = match backend {
        "sqlite" => r#"
            CREATE TABLE IF NOT EXISTS time_groups (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                name          TEXT    NOT NULL UNIQUE,
                enabled       INTEGER NOT NULL DEFAULT 1,
                time_ranges   TEXT    NOT NULL DEFAULT '[]',
                fallback_to   TEXT    NOT NULL DEFAULT '',
                description   TEXT    NOT NULL DEFAULT '',
                created_at    TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at    TEXT    NOT NULL DEFAULT (datetime('now'))
            )
        "#,
        "postgres" => r#"
            CREATE TABLE IF NOT EXISTS time_groups (
                id            BIGSERIAL PRIMARY KEY,
                name          TEXT    NOT NULL UNIQUE,
                enabled       INTEGER NOT NULL DEFAULT 1,
                time_ranges   TEXT   NOT NULL DEFAULT '[]',
                fallback_to   TEXT    NOT NULL DEFAULT '',
                description   TEXT    NOT NULL DEFAULT '',
                created_at    TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                updated_at    TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z')
            )
        "#,
        _ => r#"
            CREATE TABLE IF NOT EXISTS time_groups (
                id            BIGINT AUTO_INCREMENT PRIMARY KEY,
                name          VARCHAR(128) NOT NULL,
                enabled       TINYINT(1)   NOT NULL DEFAULT 1,
                time_ranges   JSON         NOT NULL,
                fallback_to   VARCHAR(128) NOT NULL DEFAULT '',
                description   VARCHAR(512) NOT NULL DEFAULT '',
                created_at    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                updated_at    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
                UNIQUE KEY uq_tg_name (name)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        "#,
    };
    sqlx::query(tg_ddl).execute(pool).await?;

    // ================ queues（分机队列） ================
    let q_ddl = match backend {
        "sqlite" => r#"
            CREATE TABLE IF NOT EXISTS queues (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                tenant_id     INTEGER NOT NULL DEFAULT 1,
                name          TEXT    NOT NULL,
                enabled       INTEGER NOT NULL DEFAULT 1,
                ring_strategy TEXT    NOT NULL DEFAULT 'ringall',
                max_wait_sec  INTEGER NOT NULL DEFAULT 600,
                moh_sound     TEXT    NOT NULL DEFAULT 'default',
                members_json  TEXT    NOT NULL DEFAULT '[]',
                description   TEXT    NOT NULL DEFAULT '',
                created_at    TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at    TEXT    NOT NULL DEFAULT (datetime('now')),
                UNIQUE(tenant_id, name)
            )
        "#,
        "postgres" => r#"
            CREATE TABLE IF NOT EXISTS queues (
                id            BIGSERIAL PRIMARY KEY,
                tenant_id     BIGINT  NOT NULL DEFAULT 1,
                name          TEXT    NOT NULL,
                enabled       INTEGER NOT NULL DEFAULT 1,
                ring_strategy TEXT    NOT NULL DEFAULT 'ringall',
                max_wait_sec  INTEGER NOT NULL DEFAULT 600,
                moh_sound     TEXT    NOT NULL DEFAULT 'default',
                members_json  TEXT   NOT NULL DEFAULT '[]',
                description   TEXT    NOT NULL DEFAULT '',
                created_at    TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                updated_at    TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                UNIQUE(tenant_id, name)
            )
        "#,
        _ => r#"
            CREATE TABLE IF NOT EXISTS queues (
                id            BIGINT AUTO_INCREMENT PRIMARY KEY,
                tenant_id     BIGINT       NOT NULL DEFAULT 1,
                name          VARCHAR(128) NOT NULL,
                enabled       TINYINT(1)   NOT NULL DEFAULT 1,
                ring_strategy VARCHAR(32)  NOT NULL DEFAULT 'ringall',
                max_wait_sec  INT          NOT NULL DEFAULT 600,
                moh_sound     VARCHAR(128) NOT NULL DEFAULT 'default',
                members_json  JSON         NOT NULL,
                description   VARCHAR(512) NOT NULL DEFAULT '',
                created_at    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                updated_at    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
                UNIQUE KEY uq_q_tenant_name (tenant_id, name)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        "#,
    };
    sqlx::query(q_ddl).execute(pool).await?;

    // ================ ivr_menus（IVR 菜单） ================
    let ivr_ddl = match backend {
        "sqlite" => r#"
            CREATE TABLE IF NOT EXISTS ivr_menus (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                name            TEXT    NOT NULL UNIQUE,
                enabled         INTEGER NOT NULL DEFAULT 1,
                prompt_audio    TEXT    NOT NULL DEFAULT '',
                invalid_audio   TEXT    NOT NULL DEFAULT '',
                timeout_sec     INTEGER NOT NULL DEFAULT 10,
                max_attempts    INTEGER NOT NULL DEFAULT 3,
                options_json    TEXT    NOT NULL DEFAULT '[]',
                timeout_target  TEXT    NOT NULL DEFAULT '',
                invalid_target  TEXT    NOT NULL DEFAULT '',
                description     TEXT    NOT NULL DEFAULT '',
                created_at      TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at      TEXT    NOT NULL DEFAULT (datetime('now'))
            )
        "#,
        "postgres" => r#"
            CREATE TABLE IF NOT EXISTS ivr_menus (
                id              BIGSERIAL PRIMARY KEY,
                name            TEXT    NOT NULL UNIQUE,
                enabled         INTEGER NOT NULL DEFAULT 1,
                prompt_audio    TEXT    NOT NULL DEFAULT '',
                invalid_audio   TEXT    NOT NULL DEFAULT '',
                timeout_sec     INTEGER NOT NULL DEFAULT 10,
                max_attempts    INTEGER NOT NULL DEFAULT 3,
                options_json    TEXT   NOT NULL DEFAULT '[]',
                timeout_target  TEXT    NOT NULL DEFAULT '',
                invalid_target  TEXT    NOT NULL DEFAULT '',
                description     TEXT    NOT NULL DEFAULT '',
                created_at      TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                updated_at      TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z')
            )
        "#,
        _ => r#"
            CREATE TABLE IF NOT EXISTS ivr_menus (
                id              BIGINT AUTO_INCREMENT PRIMARY KEY,
                name            VARCHAR(128) NOT NULL,
                enabled         TINYINT(1)   NOT NULL DEFAULT 1,
                prompt_audio    VARCHAR(256) NOT NULL DEFAULT '',
                invalid_audio   VARCHAR(256) NOT NULL DEFAULT '',
                timeout_sec     INT          NOT NULL DEFAULT 10,
                max_attempts    INT          NOT NULL DEFAULT 3,
                options_json    JSON         NOT NULL,
                timeout_target  VARCHAR(256) NOT NULL DEFAULT '',
                invalid_target  VARCHAR(256) NOT NULL DEFAULT '',
                description     VARCHAR(512) NOT NULL DEFAULT '',
                created_at      DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                updated_at      DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
                UNIQUE KEY uq_ivr_name (name)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        "#,
    };
    sqlx::query(ivr_ddl).execute(pool).await?;

    // ================ sip_profiles（多 SIP 传输 Profile） ================
    // 字段与 C 侧 src/include/sip_profile.h sip_profile_t 1:1 对齐
    let sp_ddl = match backend {
        "sqlite" => r#"
            CREATE TABLE IF NOT EXISTS sip_profiles (
                id                   INTEGER PRIMARY KEY AUTOINCREMENT,
                name                 TEXT    NOT NULL UNIQUE,
                enabled              INTEGER NOT NULL DEFAULT 1,
                sip_ip               TEXT    NOT NULL DEFAULT '0.0.0.0',
                external_sip_ip      TEXT    NOT NULL DEFAULT '',
                rtp_ip               TEXT    NOT NULL DEFAULT '0.0.0.0',
                external_rtp_ip      TEXT    NOT NULL DEFAULT '',
                sip_udp_port         INTEGER NOT NULL DEFAULT 5060,
                sip_tcp_port         INTEGER NOT NULL DEFAULT 5060,
                sip_tls_port         INTEGER NOT NULL DEFAULT 5061,
                enable_udp           INTEGER NOT NULL DEFAULT 1,
                enable_tcp           INTEGER NOT NULL DEFAULT 1,
                enable_tls           INTEGER NOT NULL DEFAULT 0,
                rtp_port_start       INTEGER NOT NULL DEFAULT 10000,
                rtp_port_end         INTEGER NOT NULL DEFAULT 59999,
                enable_prack         INTEGER NOT NULL DEFAULT 0,
                enable_update        INTEGER NOT NULL DEFAULT 1,
                dtmf_mode            TEXT    NOT NULL DEFAULT 'rfc4733',
                realm                TEXT    NOT NULL DEFAULT 'nsbc.local',
                auth_calls           INTEGER NOT NULL DEFAULT 0,
                ip_whitelist         TEXT    NOT NULL DEFAULT '',
                ip_blacklist         TEXT    NOT NULL DEFAULT '',
                description          TEXT    NOT NULL DEFAULT '',
                created_at           TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at           TEXT    NOT NULL DEFAULT (datetime('now'))
            )
        "#,
        "postgres" => r#"
            CREATE TABLE IF NOT EXISTS sip_profiles (
                id                   BIGSERIAL PRIMARY KEY,
                name                 TEXT    NOT NULL UNIQUE,
                enabled              INTEGER NOT NULL DEFAULT 1,
                sip_ip               TEXT    NOT NULL DEFAULT '0.0.0.0',
                external_sip_ip      TEXT    NOT NULL DEFAULT '',
                rtp_ip               TEXT    NOT NULL DEFAULT '0.0.0.0',
                external_rtp_ip      TEXT    NOT NULL DEFAULT '',
                sip_udp_port         INTEGER NOT NULL DEFAULT 5060,
                sip_tcp_port         INTEGER NOT NULL DEFAULT 5060,
                sip_tls_port         INTEGER NOT NULL DEFAULT 5061,
                enable_udp           INTEGER NOT NULL DEFAULT 1,
                enable_tcp           INTEGER NOT NULL DEFAULT 1,
                enable_tls           INTEGER NOT NULL DEFAULT 0,
                rtp_port_start       INTEGER NOT NULL DEFAULT 10000,
                rtp_port_end         INTEGER NOT NULL DEFAULT 59999,
                enable_prack         INTEGER NOT NULL DEFAULT 0,
                enable_update        INTEGER NOT NULL DEFAULT 1,
                dtmf_mode            TEXT    NOT NULL DEFAULT 'rfc4733',
                realm                TEXT    NOT NULL DEFAULT 'nsbc.local',
                auth_calls           INTEGER NOT NULL DEFAULT 0,
                ip_whitelist         TEXT    NOT NULL DEFAULT '',
                ip_blacklist         TEXT    NOT NULL DEFAULT '',
                description          TEXT    NOT NULL DEFAULT '',
                created_at           TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                updated_at           TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z')
            )
        "#,
        _ => r#"
            CREATE TABLE IF NOT EXISTS sip_profiles (
                id                   BIGINT AUTO_INCREMENT PRIMARY KEY,
                name                 VARCHAR(32)  NOT NULL,
                enabled              TINYINT(1)   NOT NULL DEFAULT 1,
                sip_ip               VARCHAR(64)  NOT NULL DEFAULT '0.0.0.0',
                external_sip_ip      VARCHAR(64)  NOT NULL DEFAULT '',
                rtp_ip               VARCHAR(64)  NOT NULL DEFAULT '0.0.0.0',
                external_rtp_ip      VARCHAR(64)  NOT NULL DEFAULT '',
                sip_udp_port         INT          NOT NULL DEFAULT 5060,
                sip_tcp_port         INT          NOT NULL DEFAULT 5060,
                sip_tls_port         INT          NOT NULL DEFAULT 5061,
                enable_udp           TINYINT(1)   NOT NULL DEFAULT 1,
                enable_tcp           TINYINT(1)   NOT NULL DEFAULT 1,
                enable_tls           TINYINT(1)   NOT NULL DEFAULT 0,
                rtp_port_start       INT          NOT NULL DEFAULT 10000,
                rtp_port_end         INT          NOT NULL DEFAULT 59999,
                enable_prack         TINYINT(1)   NOT NULL DEFAULT 0,
                enable_update        TINYINT(1)   NOT NULL DEFAULT 1,
                dtmf_mode            VARCHAR(16)  NOT NULL DEFAULT 'rfc4733',
                realm                VARCHAR(64)  NOT NULL DEFAULT 'nsbc.local',
                auth_calls           TINYINT(1)   NOT NULL DEFAULT 0,
                ip_whitelist         VARCHAR(512) NOT NULL DEFAULT '',
                ip_blacklist         VARCHAR(512) NOT NULL DEFAULT '',
                description          VARCHAR(512) NOT NULL DEFAULT '',
                created_at           DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                updated_at           DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
                UNIQUE KEY uq_sp_name (name)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        "#,
    };
    sqlx::query(sp_ddl).execute(pool).await?;

    // 升级：已存在的表追加 auth_calls / ip_whitelist / ip_blacklist（幂等）
    match backend {
        "sqlite" => {
            let cols: Vec<(i64, String)> = sqlx::query_as("PRAGMA table_info(sip_profiles)")
                .fetch_all(pool).await.unwrap_or_default();
            let col_names: Vec<&str> = cols.iter().map(|(_, c)| c.as_str()).collect();
            if !col_names.contains(&"auth_calls") {
                let _ = sqlx::query("ALTER TABLE sip_profiles ADD COLUMN auth_calls INTEGER NOT NULL DEFAULT 0").execute(pool).await;
            }
            if !col_names.contains(&"ip_whitelist") {
                let _ = sqlx::query("ALTER TABLE sip_profiles ADD COLUMN ip_whitelist TEXT NOT NULL DEFAULT ''").execute(pool).await;
            }
            if !col_names.contains(&"ip_blacklist") {
                let _ = sqlx::query("ALTER TABLE sip_profiles ADD COLUMN ip_blacklist TEXT NOT NULL DEFAULT ''").execute(pool).await;
            }
        }
        _ => {
            // PG/MySQL：ADD COLUMN IF NOT EXISTS 幂等
            let _ = sqlx::query("ALTER TABLE sip_profiles ADD COLUMN IF NOT EXISTS auth_calls INTEGER NOT NULL DEFAULT 0").execute(pool).await;
            let _ = sqlx::query("ALTER TABLE sip_profiles ADD COLUMN IF NOT EXISTS ip_whitelist TEXT NOT NULL DEFAULT ''").execute(pool).await;
            let _ = sqlx::query("ALTER TABLE sip_profiles ADD COLUMN IF NOT EXISTS ip_blacklist TEXT NOT NULL DEFAULT ''").execute(pool).await;
        }
    }

    sqlx::query(
        "CREATE INDEX IF NOT EXISTS idx_sp_enabled ON sip_profiles(enabled)"
    ).execute(pool).await?;

    // 默认 seed：internal（auth_calls=true 必须 Digest 认证）+ external（auth_calls=false FreeSWITCH external 模式）
    // 只 INSERT IF NOT EXISTS，不覆盖用户已改
    let seed = match backend {
        "sqlite" => r#"
            INSERT OR IGNORE INTO sip_profiles
                (name, enabled, sip_ip, rtp_ip, sip_udp_port, sip_tcp_port, sip_tls_port,
                 enable_udp, enable_tcp, enable_tls, rtp_port_start, rtp_port_end,
                 enable_update, realm, auth_calls, ip_whitelist, ip_blacklist, description)
            VALUES
                ('internal', 1, '0.0.0.0', '0.0.0.0', 5060, 5060, 5061, 1, 1, 0,
                 10000, 59999, 1, 'nsbc.local', 1, '', '',
                 'Internal profile — all endpoints must Digest-authenticate (类 FreeSWITCH internal)'),
                ('external', 1, '0.0.0.0', '0.0.0.0', 5080, 5080, 5081, 1, 1, 0,
                 10000, 59999, 1, 'nsbc.local', 0, '', '',
                 'External profile — 类 FreeSWITCH external 模式，默认不强制认证；用 IP blacklist 静默丢弃扫描')
        "#,
        "postgres" => r#"
            INSERT INTO sip_profiles
                (name, enabled, sip_ip, rtp_ip, sip_udp_port, sip_tcp_port, sip_tls_port,
                 enable_udp, enable_tcp, enable_tls, rtp_port_start, rtp_port_end,
                 enable_update, realm, auth_calls, ip_whitelist, ip_blacklist, description)
            VALUES
                ('internal', 1, '0.0.0.0', '0.0.0.0', 5060, 5060, 5061, 1, 1, 0,
                 10000, 59999, 1, 'nsbc.local', 1, '', '',
                 'Internal profile — all endpoints must Digest-authenticate (类 FreeSWITCH internal)'),
                ('external', 1, '0.0.0.0', '0.0.0.0', 5080, 5080, 5081, 1, 1, 0,
                 10000, 59999, 1, 'nsbc.local', 0, '', '',
                 'External profile — 类 FreeSWITCH external 模式，默认不强制认证；用 IP blacklist 静默丢弃扫描')
            ON CONFLICT (name) DO NOTHING
        "#,
        _ => r#"
            INSERT IGNORE INTO sip_profiles
                (name, enabled, sip_ip, rtp_ip, sip_udp_port, sip_tcp_port, sip_tls_port,
                 enable_udp, enable_tcp, enable_tls, rtp_port_start, rtp_port_end,
                 enable_update, realm, auth_calls, ip_whitelist, ip_blacklist, description)
            VALUES
                ('internal', 1, '0.0.0.0', '0.0.0.0', 5060, 5060, 5061, 1, 1, 0,
                 10000, 59999, 1, 'nsbc.local', 1, '', '',
                 'Internal profile — all endpoints must Digest-authenticate (类 FreeSWITCH internal)'),
                ('external', 1, '0.0.0.0', '0.0.0.0', 5080, 5080, 5081, 1, 1, 0,
                 10000, 59999, 1, 'nsbc.local', 0, '', '',
                 'External profile — 类 FreeSWITCH external 模式，默认不强制认证；用 IP blacklist 静默丢弃扫描')
        "#,
    };
    let _ = sqlx::query(seed).execute(pool).await;

    // ===== 公网 IP 自动探测 + 回填（P1 增强）=====
    // 只在 profile 的 external_sip_ip / external_rtp_ip 为空时 UPDATE ——
    // 不覆盖用户手工配置的值；探测失败静默跳过（纯 optional 增强）。
    match detect_public_ip().await {
        Some(pub_ip) => {
            info!(ip = %pub_ip, "🌐 探测到公网 IP — 回填 sip_profiles.external_sip_ip/external_rtp_ip");
            let upd_sql = match backend {
                "postgres" =>
                    "UPDATE sip_profiles \
                     SET external_sip_ip = CASE WHEN external_sip_ip = '' THEN $1 ELSE external_sip_ip END, \
                         external_rtp_ip  = CASE WHEN external_rtp_ip  = '' THEN $1 ELSE external_rtp_ip  END",
                "mysql" =>
                    "UPDATE sip_profiles \
                     SET external_sip_ip = IF(external_sip_ip = '', ?, external_sip_ip), \
                         external_rtp_ip  = IF(external_rtp_ip  = '', ?, external_rtp_ip)",
                _ =>
                    "UPDATE sip_profiles \
                     SET external_sip_ip = CASE WHEN external_sip_ip = '' THEN $1 ELSE external_sip_ip END, \
                         external_rtp_ip  = CASE WHEN external_rtp_ip  = '' THEN $1 ELSE external_rtp_ip  END",
            };
            match sqlx::query(upd_sql).bind(&pub_ip).execute(pool).await {
                Ok(r) => {
                    if r.rows_affected() > 0 {
                        info!(rows = r.rows_affected(), pub_ip = %pub_ip,
                              "✅ sip_profiles external_sip_ip/external_rtp_ip 已自动设置")
                    } else {
                        info!("所有 sip_profiles 已配置 external_ip — 跳过自动探测回填");
                    }
                }
                Err(e) => warn!(error = %e, "sip_profiles external_ip auto-update 失败（非致命）"),
            }
        }
        None => {
            warn!("⚠️ 未探测到公网 IP — sip_profiles.external_sip_ip/external_rtp_ip 保持空值；\
                  公网部署请手工填或设 NSBC_PUBLIC_IP env");
        }
    }

    // 默认 extensions seed：1000-1019，密码统一 876876，realm=nsbc.local
    // HA1 = md5(extension:realm:password)，SIP Digest 认证用
    let default_realm = "nsbc.local";
    let default_pass  = "876876";
    for n in 1000..1020u32 {
        let ext = format!("{n}");
        let clip = format!("2{:03}", n % 1000);
        let password = default_pass;
        let ha1  = ha1_of(&ext, default_realm, default_pass);
        let ha1b = ha1.clone();
        let sql = match backend {
            "postgres" =>
                "INSERT INTO extensions (tenant_id, extension, realm, password, ha1, ha1b, enabled, clip, description) \
                 VALUES (1, $1, $2, $3, $4, $5, 1, $6, $7) \
                 ON CONFLICT (tenant_id, extension, realm) DO NOTHING",
            "mysql" =>
                "INSERT IGNORE INTO extensions (tenant_id, extension, realm, password, ha1, ha1b, enabled, clip, description) \
                 VALUES (1, ?, ?, ?, ?, ?, 1, ?, ?)",
            _ =>
                "INSERT OR IGNORE INTO extensions (tenant_id, extension, realm, password, ha1, ha1b, enabled, clip, description) \
                 VALUES (1, $1, $2, $3, $4, $5, 1, $6, $7)",
        };
        let _ = sqlx::query(sql)
            .bind(&ext).bind(default_realm).bind(password)
            .bind(&ha1).bind(&ha1b)
            .bind(&clip).bind(format!("Default ext {ext}"))
            .execute(pool).await;
    }

    // 默认 dialplan_rules seed：Internal 分机对打 + 出局占位规则
    // 幂等（ON CONFLICT DO NOTHING / INSERT OR IGNORE）
    //
    // 规则设计：
    // 1. Internal 分机对打（prio=10，最高优先）
    //    layer=ext, match=^1[01]\d{2}$  → 直接送到分机 `${1}`
    //    匹配 1000-1099 / 1100-1199（保留未来扩到几千的空间）
    //    ⚠️ External profile 不接受 REGISTER（silent drop），因此不再需要独立的 ext 层规则。
    // 2. 默认出局占位（prio=99）
    //    layer=out, match=^\d{5,}$      → 送 default gateway（后续网关配置好改）
    //
    // run_migrations 已经知道 backend（postgresql/mysql/sqlite），直接 match
    let rules: &[(&str, &str, &str, i64, i64, &str, &str, i64, &str)] = &[
        // name, layer, match_expr, prio, strip, target, action_json, enabled, description
        ("Internal 分机对打",      "ext",  "^1[01]\\d{2}$", 10, 0, "${1}", "{}", 1, "Internal profile：1000-1199 分机号直接对打"),
        ("默认出局占位",            "out",  "^\\d{5,}$",     99, 0, "gateway:default", "{}", 0, "匹配 5+ 位被叫，暂关；配好网关后启用"),
    ];
    for (name, layer, mx, prio, strip, target, action, en, desc) in rules {
        let sql = match backend {
            "postgres" =>
                "INSERT INTO dialplan_rules \
                 (tenant_id, name, layer, match_expr, caller_match, prio, strip, target, action_json, enabled, description) \
                 VALUES (1, $1, $2, $3, '', $4, $5, $6, $7, $8, $9) \
                 ON CONFLICT (tenant_id, name) DO NOTHING",
            "mysql" =>
                "INSERT IGNORE INTO dialplan_rules \
                 (tenant_id, name, layer, match_expr, caller_match, prio, strip, target, action_json, enabled, description) \
                 VALUES (1, ?, ?, ?, '', ?, ?, ?, ?, ?, ?)",
            _ =>
                "INSERT OR IGNORE INTO dialplan_rules \
                 (tenant_id, name, layer, match_expr, caller_match, prio, strip, target, action_json, enabled, description) \
                 VALUES (1, $1, $2, $3, '', $4, $5, $6, $7, $8, $9)",
        };
        let _ = sqlx::query(sql)
            .bind(*name).bind(*layer).bind(*mx)
            .bind(prio).bind(strip).bind(*target)
            .bind(*action).bind(en).bind(*desc)
            .execute(pool).await;
    }

    // ================ web_users (Web 管理登录用户) ================
    // Web 管理后台登录账号表：username + role(L0/L1/L2) + argon2id PHC。
    // 与 extensions（SIP 分机）完全独立：extensions 存 SIP Digest ha1，
    // web_users 存 Web 管理登录 argon2id 密码哈希。
    // 账号来源优先级：DB 中 web_users 表 → 若空则 fallback 到 AuthSvc 内存 AccountStore。
    let wu_ddl = match backend {
        "sqlite" => r#"
            CREATE TABLE IF NOT EXISTS web_users (
                id               INTEGER PRIMARY KEY AUTOINCREMENT,
                username         TEXT    NOT NULL UNIQUE,
                role             TEXT    NOT NULL DEFAULT 'L2',
                password_phc     TEXT    NOT NULL DEFAULT '',
                enabled          INTEGER NOT NULL DEFAULT 1,
                description      TEXT    NOT NULL DEFAULT '',
                created_at       TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at       TEXT    NOT NULL DEFAULT (datetime('now'))
            )
        "#,
        "postgres" => r#"
            CREATE TABLE IF NOT EXISTS web_users (
                id               BIGSERIAL PRIMARY KEY,
                username         VARCHAR(64)  NOT NULL UNIQUE,
                role             VARCHAR(4)   NOT NULL DEFAULT 'L2',
                password_phc     TEXT         NOT NULL DEFAULT '',
                enabled          INTEGER      NOT NULL DEFAULT 1,
                description      VARCHAR(256) NOT NULL DEFAULT '',
                created_at       TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                updated_at       TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z')
            )
        "#,
        _ => r#"
            CREATE TABLE IF NOT EXISTS web_users (
                id               BIGINT AUTO_INCREMENT PRIMARY KEY,
                username         VARCHAR(64)  NOT NULL UNIQUE,
                role             VARCHAR(4)   NOT NULL DEFAULT 'L2',
                password_phc     TEXT         NOT NULL DEFAULT '',
                enabled          TINYINT(1)   NOT NULL DEFAULT 1,
                description      VARCHAR(256) NOT NULL DEFAULT '',
                created_at       TEXT         NOT NULL DEFAULT (CAST(CURRENT_TIMESTAMP(3) AS CHAR)),
                updated_at       TEXT         NOT NULL DEFAULT (CAST(CURRENT_TIMESTAMP(3) AS CHAR))
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        "#,
    };
    sqlx::query(wu_ddl).execute(pool).await?;

    // ================ security_blacklist（静态安全围栏：NIP 三层 + deny/allow 双模式） ================
    // rule_type: 'ip_cidr' | 'ua' | 'called_prefix' | 'caller_prefix'
    // kind:      'deny'（命中即丢） | 'allow'（命中才放；配了 allow 后未命中一律丢）
    // Web 安全中心管理 → spawn_push → C security.blacklist.reload <json> 整表替换。
    let bl_ddl = match backend {
        "sqlite" => r#"
            CREATE TABLE IF NOT EXISTS security_blacklist (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                kind        TEXT    NOT NULL DEFAULT 'deny',
                rule_type   TEXT    NOT NULL,
                value       TEXT    NOT NULL,
                comment     TEXT    NOT NULL DEFAULT '',
                enabled     INTEGER NOT NULL DEFAULT 1,
                created_at  TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at  TEXT    NOT NULL DEFAULT (datetime('now')),
                UNIQUE(kind, rule_type, value)
            )
        "#,
        "postgres" => r#"
            CREATE TABLE IF NOT EXISTS security_blacklist (
                id          BIGSERIAL PRIMARY KEY,
                kind        VARCHAR(16)  NOT NULL DEFAULT 'deny',
                rule_type   VARCHAR(16)  NOT NULL,
                value       VARCHAR(128) NOT NULL,
                comment     VARCHAR(64)  NOT NULL DEFAULT '',
                enabled     INTEGER      NOT NULL DEFAULT 1,
                created_at  TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                updated_at  TEXT NOT NULL DEFAULT (to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD') || 'T' || to_char(NOW() AT TIME ZONE 'UTC', 'HH24:MI:SS.MS') || 'Z'),
                UNIQUE(kind, rule_type, value)
            )
        "#,
        _ => r#"
            CREATE TABLE IF NOT EXISTS security_blacklist (
                id          BIGINT AUTO_INCREMENT PRIMARY KEY,
                kind        VARCHAR(16)  NOT NULL DEFAULT 'deny',
                rule_type   VARCHAR(16)  NOT NULL,
                value       VARCHAR(128) NOT NULL,
                comment     VARCHAR(64)  NOT NULL DEFAULT '',
                enabled     TINYINT(1)   NOT NULL DEFAULT 1,
                created_at  DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                updated_at  DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
                UNIQUE KEY uq_bl_kind_type_value (kind, rule_type, value)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        "#,
    };
    sqlx::query(bl_ddl).execute(pool).await?;

    // --- security_blacklist schema 升级（老表没有 kind 列时补） ---
    match backend {
        "sqlite" => {
            // SQLite: 查 PRAGMA → 无 kind 列则 ADD COLUMN
            let has_kind: String = sqlx::query_scalar(
                "SELECT COUNT(*) FROM pragma_table_info('security_blacklist') WHERE name='kind'"
            ).fetch_one(pool).await.unwrap_or_default();
            if has_kind == "0" {
                sqlx::query(
                    "ALTER TABLE security_blacklist ADD COLUMN kind TEXT NOT NULL DEFAULT 'deny'"
                ).execute(pool).await?;
            }
            // 老 UNIQUE(rule_type, value) 约束无法直接改成 UNIQUE(kind, rule_type, value)，
            // SQLite 3.35+ 支持 ALTER TABLE ... RENAME + CREATE TABLE，但简单起见：
            // 运行时检查冲突（kind='deny', rule_type='ip_cidr', value='X' 与 kind='allow' 同值允许共存）
        }
        "postgres" => {
            sqlx::query(
                "ALTER TABLE security_blacklist ADD COLUMN IF NOT EXISTS kind VARCHAR(16) NOT NULL DEFAULT 'deny'"
            ).execute(pool).await?;
            sqlx::query(
                "ALTER TABLE security_blacklist ALTER COLUMN rule_type TYPE VARCHAR(16)"
            ).execute(pool).await.ok();  // 改大列宽，已大则忽略
            sqlx::query(
                "ALTER TABLE security_blacklist DROP CONSTRAINT IF EXISTS security_blacklist_rule_type_value_key"
            ).execute(pool).await.ok();
            sqlx::query(
                "ALTER TABLE security_blacklist DROP CONSTRAINT IF EXISTS security_blacklist_rule_type_value_unique"
            ).execute(pool).await.ok();
            sqlx::query(
                "ALTER TABLE security_blacklist ADD CONSTRAINT security_blacklist_kind_rule_type_value_key \
                 UNIQUE (kind, rule_type, value)"
            ).execute(pool).await.ok();
        }
        _ => {
            // MySQL: ALTER TABLE ADD COLUMN IF NOT EXISTS（MySQL 8.0+ 支持 IF NOT EXISTS）
            sqlx::query(
                "ALTER TABLE security_blacklist ADD COLUMN kind VARCHAR(16) NOT NULL DEFAULT 'deny'"
            ).execute(pool).await.ok();
            sqlx::query(
                "ALTER TABLE security_blacklist MODIFY COLUMN rule_type VARCHAR(16) NOT NULL"
            ).execute(pool).await.ok();
            sqlx::query(
                "ALTER TABLE security_blacklist DROP INDEX uq_bl_type_value"
            ).execute(pool).await.ok();
            sqlx::query(
                "ALTER TABLE security_blacklist ADD UNIQUE KEY uq_bl_kind_type_value (kind, rule_type, value)"
            ).execute(pool).await.ok();
        }
    }

    // security_config（单行：围栏模式 + fail2ban 五维配置）
    // fence_mode: 1=security（完整围栏，默认）/ 0=performance（全关）
    // method_blocked: 位图 (1<<(method-1))，uint32
    // auto_ban=1/0, authfail_threshold=1-1000, authfail_window_s=1-86400,
    // ban_duration_s=1-604800(7d), ratelimit_regs=1-1000, ratelimit_window_s=1-3600
    let sc_ddl = match backend {
        "sqlite" => r#"
            CREATE TABLE IF NOT EXISTS security_config (
                id                    INTEGER PRIMARY KEY CHECK (id = 1),
                fence_mode            INTEGER NOT NULL DEFAULT 1,
                method_blocked        INTEGER NOT NULL DEFAULT 0,
                auto_ban              INTEGER NOT NULL DEFAULT 1,
                authfail_threshold    INTEGER NOT NULL DEFAULT 5,
                authfail_window_s     INTEGER NOT NULL DEFAULT 60,
                ban_duration_s        INTEGER NOT NULL DEFAULT 3600,
                ratelimit_regs        INTEGER NOT NULL DEFAULT 20,
                ratelimit_window_s    INTEGER NOT NULL DEFAULT 60
            )
        "#,
        "postgres" => r#"
            CREATE TABLE IF NOT EXISTS security_config (
                id                    INTEGER PRIMARY KEY CHECK (id = 1),
                fence_mode            INTEGER NOT NULL DEFAULT 1,
                method_blocked        INTEGER NOT NULL DEFAULT 0,
                auto_ban              INTEGER NOT NULL DEFAULT 1,
                authfail_threshold    INTEGER NOT NULL DEFAULT 5,
                authfail_window_s     INTEGER NOT NULL DEFAULT 60,
                ban_duration_s        INTEGER NOT NULL DEFAULT 3600,
                ratelimit_regs        INTEGER NOT NULL DEFAULT 20,
                ratelimit_window_s    INTEGER NOT NULL DEFAULT 60
            )
        "#,
        _ => r#"
            CREATE TABLE IF NOT EXISTS security_config (
                id                    TINYINT PRIMARY KEY,
                fence_mode            TINYINT(1) NOT NULL DEFAULT 1,
                method_blocked        INT UNSIGNED NOT NULL DEFAULT 0,
                auto_ban              TINYINT(1) NOT NULL DEFAULT 1,
                authfail_threshold    INT NOT NULL DEFAULT 5,
                authfail_window_s     INT NOT NULL DEFAULT 60,
                ban_duration_s        INT NOT NULL DEFAULT 3600,
                ratelimit_regs        INT NOT NULL DEFAULT 20,
                ratelimit_window_s    INT NOT NULL DEFAULT 60
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        "#,
    };
    sqlx::query(sc_ddl).execute(pool).await?;

    // --- security_config 增量 ALTER TABLE 迁移 ---
    // 旧表：仅 fence_mode + method_blocked 两列。新增 fail2ban 六维。
    // SQLite: ALTER TABLE 不支持 IF NOT EXISTS，try + 忽略重复列错误即可
    // Postgres: 9.6+ 支持 ADD COLUMN IF NOT EXISTS
    // MySQL: 8.0+ 支持 ADD COLUMN IF NOT EXISTS
    for (name, col_def) in [
        ("auto_ban",           "INTEGER NOT NULL DEFAULT 1"),
        ("authfail_threshold", "INTEGER NOT NULL DEFAULT 5"),
        ("authfail_window_s",  "INTEGER NOT NULL DEFAULT 60"),
        ("ban_duration_s",     "INTEGER NOT NULL DEFAULT 3600"),
        ("ratelimit_regs",     "INTEGER NOT NULL DEFAULT 20"),
        ("ratelimit_window_s", "INTEGER NOT NULL DEFAULT 60"),
    ] {
        match backend {
            "sqlite" => {
                let _ = sqlx::query(&format!(
                    "ALTER TABLE security_config ADD COLUMN {} {}", name, col_def
                )).execute(pool).await;
            }
            "postgres" => {
                let _ = sqlx::query(&format!(
                    "ALTER TABLE security_config ADD COLUMN IF NOT EXISTS {} {}",
                    name, col_def
                )).execute(pool).await;
            }
            _ => {
                // MySQL: auto_ban 用 TINYINT(1)，其他 INT
                let mysql_def = if name == "auto_ban" {
                    "TINYINT(1) NOT NULL DEFAULT 1".to_string()
                } else {
                    format!("INT NOT NULL DEFAULT {}", 
                        match name {
                            "authfail_threshold" => "5",
                            "authfail_window_s"  => "60",
                            "ban_duration_s"     => "3600",
                            "ratelimit_regs"     => "20",
                            "ratelimit_window_s" => "60",
                            _                    => "0",
                        }
                    )
                };
                let _ = sqlx::query(&format!(
                    "ALTER TABLE security_config ADD COLUMN IF NOT EXISTS {} {}",
                    name, mysql_def
                )).execute(pool).await;
            }
        }
    }

    Ok(())
}

/// Web 管理用户（从 web_users 表查询返回）。
#[derive(Clone, Debug, sqlx::FromRow)]
pub struct WebUser {
    pub id: i64,
    pub username: String,
    pub role: String,
    pub password_phc: String,
    pub enabled: i64,
}

/// CDR 记录（SELECT 输出结构）。
#[derive(Clone, Debug, sqlx::FromRow, serde::Serialize)]
pub struct CdrRow {
    pub call_id: String,
    pub a_call_id: Option<String>,
    pub b_call_id: Option<String>,
    pub caller: Option<String>,
    pub callee: Option<String>,
    pub start_time: String,
    pub answer_time: Option<String>,
    pub end_time: Option<String>,
    pub duration_sec: Option<i64>,
    pub billsec: Option<i64>,
    pub cause_code: Option<i32>,
    pub cause_text: Option<String>,
    pub leg_a_final: Option<i32>,
    pub leg_b_final: Option<i32>,
    pub metadata: Option<String>,
}

/// 分页参数。
#[derive(Clone, Debug, Default)]
pub struct CdrQuery {
    pub caller: Option<String>,
    pub callee: Option<String>,
    pub from: Option<String>,
    pub to: Option<String>,
    pub limit: u32,
    pub offset: u32,
}

impl CdrQuery {
    pub fn normalized(self) -> Self {
        Self {
            limit: self.limit.clamp(1, 500),
            offset: self.offset.max(0),
            ..self
        }
    }
}

/// 执行 CDR 查询（动态 WHERE 子句，参数化避免注入）。
pub async fn query_cdrs(
    pool: &Pool<Any>,
    q: &CdrQuery,
    _backend: &str,
) -> Result<(Vec<CdrRow>, i64), sqlx::Error> {
    let q = q.clone().normalized();

    // 动态拼接 WHERE 子句；? 占位符数量与 bind 顺序对应
    let mut where_clauses: Vec<String> = Vec::new();
    let mut bound_idx: u32 = 0;
    let mut binds: Vec<String> = Vec::new();

    if let Some(c) = &q.caller {
        bound_idx += 1;
        where_clauses.push(format!("caller LIKE ${bound_idx}"));
        binds.push(format!("%{}%", c));
    }
    if let Some(c) = &q.callee {
        bound_idx += 1;
        where_clauses.push(format!("callee LIKE ${bound_idx}"));
        binds.push(format!("%{}%", c));
    }
    if let Some(f) = &q.from {
        bound_idx += 1;
        where_clauses.push(format!("start_time >= ${bound_idx}"));
        binds.push(f.clone());
    }
    if let Some(t) = &q.to {
        bound_idx += 1;
        where_clauses.push(format!("start_time <= ${bound_idx}"));
        binds.push(t.clone());
    }

    let where_sql = if where_clauses.is_empty() {
        String::new()
    } else {
        "WHERE ".to_string() + &where_clauses.join(" AND ")
    };

    // count
    let count_sql = format!("SELECT COUNT(*) FROM cdr {where_sql}");
    let mut count_q = sqlx::query_scalar::<_, i64>(&count_sql);
    for b in &binds { count_q = count_q.bind(b.as_str()); }
    let total: i64 = count_q.fetch_one(pool).await.unwrap_or(0);

    // rows
    let rows_sql = format!(
        "SELECT call_id, a_call_id, b_call_id, caller, callee, \
                start_time, answer_time, end_time, duration_sec, billsec, \
                cause_code, cause_text, leg_a_final, leg_b_final, metadata \
         FROM cdr {where_sql} ORDER BY start_time DESC LIMIT ? OFFSET ?"
    );
    // LIMIT/OFFSET 作为最后两个参数绑定
    let final_limit = binds.len() + 1;
    let final_offset = binds.len() + 2;
    let rows_sql = rows_sql
        .replace("LIMIT ?", &format!("LIMIT ${final_limit}"))
        .replace("OFFSET ?", &format!("OFFSET ${final_offset}"));

    let mut rows_q = sqlx::query_as::<_, CdrRow>(&rows_sql);
    for b in &binds { rows_q = rows_q.bind(b.as_str()); }
    rows_q = rows_q.bind(q.limit as i64).bind(q.offset as i64);
    let rows: Vec<CdrRow> = rows_q.fetch_all(pool).await?;

    Ok((rows, total))
}

// ---------- recordings 查询（REQ-REC-004 Web REST） ----------

/// 单行录音记录：从 cdr 表中挑出 metadata 含 rec_path 的记录展开。
#[derive(Debug, Clone, serde::Serialize)]
pub struct RecordingRow {
    pub call_id:      String,
    pub tenant:       Option<String>,
    pub caller:       String,
    pub callee:       String,
    pub start_time:   Option<String>,
    pub end_time:     Option<String>,
    pub duration_sec: Option<i64>,
    pub rec_path:     String,
    pub cause_code:   Option<i64>,
    pub cause_text:   Option<String>,
}

#[derive(Clone)]
pub struct RecordingQuery {
    pub caller: Option<String>,
    pub callee: Option<String>,
    pub from:   Option<String>,
    pub to:     Option<String>,
    pub limit:  Option<u32>,
    pub offset: Option<u32>,
}

impl Default for RecordingQuery {
    fn default() -> Self {
        Self { caller: None, callee: None, from: None, to: None,
               limit: Some(50), offset: Some(0) }
    }
}

pub async fn query_recordings(
    pool: &Pool<Any>,
    q: &RecordingQuery,
    backend: &str,
) -> Result<(Vec<RecordingRow>, i64), sqlx::Error> {
    let limit  = q.limit.unwrap_or(50).clamp(1, 500) as i64;
    let offset = q.offset.unwrap_or(0).max(0) as i64;

    // 跨后端 metadata->rec_path 提取：
    //   PG      metadata ->> 'rec_path'  (jsonb 文本操作符)
    //   MySQL   JSON_UNQUOTE(JSON_EXTRACT(metadata, '$.rec_path'))
    //   SQLite  json_extract(metadata, '$.rec_path')
    let (rec_path_sql, has_rec_sql): (&str, &str) = match backend {
        "postgres" => (
            "metadata ->> 'rec_path'",
            "metadata ->> 'rec_path' IS NOT NULL AND metadata ->> 'rec_path' != ''",
        ),
        "mysql" => (
            "JSON_UNQUOTE(JSON_EXTRACT(metadata, '$.rec_path'))",
            "JSON_EXTRACT(metadata, '$.rec_path') IS NOT NULL",
        ),
        _ => (
            "json_extract(metadata, '$.rec_path')",
            "json_extract(metadata, '$.rec_path') IS NOT NULL",
        ),
    };

    // 动态 WHERE 子句（has_rec_sql 是固定条件，caller/callee/from/to 是可选条件）
    let mut where_clauses: Vec<String> = vec![has_rec_sql.to_string()];
    let mut binds: Vec<String> = Vec::new();
    let mut idx: u32 = 0;

    let add = |cond: &str, val: String, where_clauses: &mut Vec<String>,
                   binds: &mut Vec<String>, idx: &mut u32| {
        *idx += 1;
        where_clauses.push(format!("{cond} ${idx}"));
        binds.push(val);
    };

    if let Some(c) = &q.caller {
        add("caller LIKE", format!("%{c}%"), &mut where_clauses, &mut binds, &mut idx);
    }
    if let Some(c) = &q.callee {
        add("callee LIKE", format!("%{c}%"), &mut where_clauses, &mut binds, &mut idx);
    }
    if let Some(f) = &q.from {
        add("start_time >=", f.clone(), &mut where_clauses, &mut binds, &mut idx);
    }
    if let Some(t) = &q.to {
        add("start_time <=", t.clone(), &mut where_clauses, &mut binds, &mut idx);
    }

    let where_sql = format!("WHERE {}", where_clauses.join(" AND "));

    // COUNT — 复用同一个 WHERE + binds（不增占位）
    let count_sql = format!("SELECT COUNT(*) FROM cdr {where_sql}");
    let mut count_q = sqlx::query_scalar::<_, i64>(&count_sql);
    for b in &binds { count_q = count_q.bind(b.as_str()); }
    let total: i64 = count_q.fetch_one(pool).await.unwrap_or(0);

    // ROWS
    let last_l = idx + 1;  // LIMIT
    let last_o = idx + 2;  // OFFSET
    let rows_sql = format!(
        "SELECT call_id, caller, callee, start_time, end_time, duration_sec, \
                cause_code, cause_text, metadata, \
                {rec_path_sql} AS rec_path \
         FROM cdr {where_sql} \
         ORDER BY start_time DESC \
         LIMIT ${last_l} OFFSET ${last_o}"
    );
    let mut rows_q = sqlx::query::<sqlx::Any>(&rows_sql);
    for b in &binds { rows_q = rows_q.bind(b.as_str()); }
    rows_q = rows_q.bind(limit).bind(offset);

    let mut out: Vec<RecordingRow> = Vec::new();
    let mut rows = rows_q.fetch_all(pool).await?;
    for row in rows.drain(..) {
        let call_id: String           = row.try_get("call_id")?;
        let caller: String            = row.try_get("caller")?;
        let callee: String            = row.try_get("callee")?;
        let start_time: Option<String>   = row.try_get("start_time").ok();
        let end_time: Option<String>     = row.try_get("end_time").ok();
        let duration_sec: Option<i64>    = row.try_get("duration_sec").ok();
        let cause_code: Option<i64>      = row.try_get("cause_code").ok();
        let cause_text: Option<String>   = row.try_get("cause_text").ok();
        let metadata: Option<String>     = row.try_get("metadata").ok();
        let rec_path: String             = row.try_get("rec_path").unwrap_or_default();

        // tenant 从 metadata JSON 解析（三后端列名一致，不再开一列）
        let tenant = metadata
            .as_ref()
            .and_then(|m| serde_json::from_str::<serde_json::Value>(m).ok())
            .and_then(|v: serde_json::Value| v.get("tenant").cloned())
            .and_then(|v| v.as_str().map(|s: &str| s.to_string()));

        out.push(RecordingRow {
            call_id, tenant, caller, callee,
            start_time, end_time, duration_sec, rec_path,
            cause_code, cause_text,
        });
    }

    Ok((out, total))
}

// ---------- extensions CRUD（REQ-WEB-016）----------

/// 分机记录（SELECT 输出 + INSERT/UPDATE 输入统一用）。
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct Extension {
    pub id:           i64,
    pub tenant_id:    i64,
    pub extension:    String,
    pub realm:         String,
    pub password:      String,    // 明文密码（内网管理端可见，API 暴露）
    pub ha1:           String,
    pub ha1b:          String,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enabled:       i64,
    pub clip:          String,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub clir:          i64,
    pub max_contacts:  i64,
    pub dialplan_match: String,
    pub description:   String,
    // --- 外部分机支持（REQ-PBX-EXT-EXT-001） ---
    pub ext_type:        String,   // 'internal' | 'external'
    pub external_number: String,   // 外呼目标号码（E.164）
    pub trunk_name:      String,   // 出局中继（走哪个 trunk 外呼）
    pub display_number:  String,   // 外呼时的主叫显示（caller_id）
    pub created_at:    String,
    pub updated_at:    String,
}

/// 创建分机请求体（不含 id/created_at/updated_at，由 DB 生成）。
/// password 可选：给了就服务端算 ha1/ha1b；不给则 ha1 必须手动传。
#[derive(Debug, Clone, serde::Deserialize)]
pub struct ExtensionCreate {
    pub tenant_id:      i64,
    pub extension:    String,
    #[serde(default)]
    pub realm:          String,
    /// 可选明文密码：传了则服务端自动算 ha1 = md5(ext:realm:password)。
    /// API response **永远不回传此列**。
    #[serde(default)]
    pub password:       String,
    #[serde(default)]
    pub ha1:            String,
    #[serde(default)]
    pub ha1b:           String,
    #[serde(default = "default_enabled")]
    pub enabled:        bool,
    #[serde(default)]
    pub clip:           String,
    #[serde(default)]
    pub clir:           bool,
    #[serde(default = "default_max_contacts")]
    pub max_contacts:   i64,
    #[serde(default)]
    pub dialplan_match: String,
    #[serde(default)]
    pub description:    String,
    // --- 外部分机支持 ---
    #[serde(default = "default_internal")]
    pub ext_type:        String,
    #[serde(default)]
    pub external_number: String,
    #[serde(default)]
    pub trunk_name:      String,
    #[serde(default)]
    pub display_number:  String,
}

fn default_enabled() -> bool { true }
fn default_max_contacts() -> i64 { 10 }
fn default_internal() -> String { "internal".to_string() }

/// 更新分机请求体（所有字段可选）。password 传了就同时更新 ha1/ha1b。
#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct ExtensionUpdate {
    pub tenant_id:      Option<i64>,
    pub extension:      Option<String>,
    pub password:       Option<String>,
    pub ha1:            Option<String>,
    pub ha1b:           Option<String>,
    pub enabled:        Option<bool>,
    pub clip:           Option<String>,
    pub clir:           Option<bool>,
    pub max_contacts:   Option<i64>,
    pub dialplan_match: Option<String>,
    pub description:    Option<String>,
    // --- 外部分机支持 ---
    pub ext_type:        Option<String>,
    pub external_number: Option<String>,
    pub trunk_name:      Option<String>,
    pub display_number:  Option<String>,
}

impl Db {
    /// 列表查询：按 extension/realm/enabled 过滤，分页。
    pub async fn list_extensions(
        &self,
        tenant_id: Option<i64>,
        extension: Option<&str>,
        realm: Option<&str>,
        enabled: Option<bool>,
        limit: i64,
        offset: i64,
    ) -> Result<(Vec<Extension>, i64), sqlx::Error> {
        let Some(pool) = self.pool() else {
            return Ok((Vec::new(), 0));
        };
        let limit = limit.clamp(1, 500);
        let offset = offset.max(0);

        let mut clauses: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();
        let mut idx: u32 = 0;

        if let Some(t) = tenant_id {
            if t > 0 { idx += 1; clauses.push(format!("tenant_id = ${idx}")); binds.push(t.to_string()); }
        }
        if let Some(e) = extension {
            idx += 1;
            clauses.push(format!("extension LIKE ${idx}"));
            binds.push(format!("%{e}%"));
        }
        if let Some(r) = realm {
            idx += 1;
            clauses.push(format!("realm LIKE ${idx}"));
            binds.push(format!("%{r}%"));
        }
        if let Some(en) = enabled {
            idx += 1;
            clauses.push(format!("enabled = ${idx}"));
            binds.push(if en { "1".into() } else { "0".into() });
        }

        let where_sql = if clauses.is_empty() {
            String::new()
        } else {
            "WHERE ".to_string() + &clauses.join(" AND ")
        };

        let count_sql = format!("SELECT COUNT(*) FROM extensions {where_sql}");
        let mut cq = sqlx::query_scalar::<_, i64>(&count_sql);
        for b in &binds { cq = cq.bind(b.as_str()); }
        let total = cq.fetch_one(pool).await.unwrap_or(0);

        let l = idx + 1;
        let o = idx + 2;
        let rows_sql = format!(
            "SELECT id, tenant_id, extension, realm, password, ha1, ha1b, enabled, clip, clir, max_contacts, dialplan_match, description, ext_type, external_number, trunk_name, display_number, created_at, updated_at FROM extensions {where_sql} ORDER BY extension LIMIT ${l} OFFSET ${o}"
        );
        let mut q = sqlx::query_as::<_, Extension>(&rows_sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(limit).bind(offset);
        let rows = q.fetch_all(pool).await?;
        Ok((rows, total))
    }

    /// 按 id 查单条。
    pub async fn get_extension(&self, id: i64) -> Result<Option<Extension>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(None); };
        let sql = fix_ph("SELECT id, tenant_id, extension, realm, password, ha1, ha1b, enabled, clip, clir, max_contacts, dialplan_match, description, ext_type, external_number, trunk_name, display_number, created_at, updated_at FROM extensions WHERE id = $1", self.backend());
        let row = sqlx::query_as::<_, Extension>(&sql)
            .bind(id).fetch_optional(pool).await?;
        Ok(row)
    }

    /// 按 extension + realm 联合唯一键查。
    pub async fn get_extension_by_eaor(
        &self, extension: &str, realm: &str,
    ) -> Result<Option<Extension>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(None); };
        let sql = fix_ph(
            "SELECT id, tenant_id, extension, realm, password, ha1, ha1b, enabled, clip, clir, max_contacts, dialplan_match, description, ext_type, external_number, trunk_name, display_number, created_at, updated_at FROM extensions WHERE extension = $1 AND realm = $2",
            self.backend()
        );
        let row = sqlx::query_as::<_, Extension>(&sql)
            .bind(extension).bind(realm).fetch_optional(pool).await?;
        Ok(row)
    }

    /// 创建分机。UNIQUE 冲突返回 sqlx::Error（调用方判断 409）。
    pub async fn create_extension(
        &self, c: ExtensionCreate,
    ) -> Result<Extension, sqlx::Error> {
        let Some(pool) = self.pool() else {
            return Err(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()));
        };
        let backend = self.backend();
        let dp = if c.dialplan_match.is_empty() { "{}".to_string() } else { c.dialplan_match };
        /* Phase 2 多租户安全加固：realm 强制 = tenant_name（从 tenants 表按 tenant_id 查）
         * 以前硬编码 "nsbc.local" 或信任前端传值 → 不同租户分机可能 realm 相同 →
         * ep_ha1_lookup 认证时被错误匹配/不匹配。
         * tenant_id 找不到（租户被删）→ fallback "nsbc.local"。 */
        let realm_sub_sql = fix_ph("SELECT tenant_name FROM tenants WHERE id = $1", backend);
        let realm: String = sqlx::query_scalar(&realm_sub_sql)
            .bind(c.tenant_id).fetch_optional(pool).await?
            .unwrap_or_else(|| "nsbc.local".to_string());

        // 密码处理：给了明文就算 ha1/ha1b；没给但 ha1 传了就直接用
        let (password, ha1, ha1b) = if !c.password.is_empty() {
            let ha1_hex = ha1_of(&c.extension, &realm, &c.password);
            let ha1b_hex = ha1_of(&c.extension, &realm, &c.password);
            (c.password.clone(), ha1_hex, ha1b_hex)
        } else {
            ("".to_string(), c.ha1.clone(), c.ha1b.clone())
        };

        let raw_sql = "INSERT INTO extensions
                (tenant_id, extension, realm, password, ha1, ha1b, enabled, clip, clir,
                 max_contacts, dialplan_match, description,
                 ext_type, external_number, trunk_name, display_number)
             VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16)
             RETURNING id, tenant_id, extension, realm, password, ha1, ha1b, enabled, clip, clir,
                       max_contacts, dialplan_match, description,
                       ext_type, external_number, trunk_name, display_number,
                       created_at, updated_at";

        if backend == "mysql" {
            let exec_sql = fix_ph(&strip_returning(raw_sql), backend);
            let mut q = sqlx::query(&exec_sql);
            q = q.bind(c.tenant_id).bind(&c.extension).bind(&realm);
            q = q.bind(&password).bind(&ha1).bind(&ha1b);
            q = q.bind(c.enabled as i64).bind(&c.clip).bind(c.clir as i64);
            q = q.bind(c.max_contacts).bind(&dp).bind(&c.description);
            q = q.bind(&c.ext_type).bind(&c.external_number).bind(&c.trunk_name).bind(&c.display_number);
            q.execute(pool).await?;
            let row = sqlx::query_as::<_, Extension>(
                "SELECT id, tenant_id, extension, realm, password, ha1, ha1b, enabled, clip, clir, max_contacts, dialplan_match, description, ext_type, external_number, trunk_name, display_number, created_at, updated_at FROM extensions WHERE id = LAST_INSERT_ID()"
            ).fetch_one(pool).await?;
            Ok(row)
        } else {
            let exec_sql = fix_ph(raw_sql, backend);
            let mut q = sqlx::query_as::<_, Extension>(&exec_sql);
            q = q.bind(c.tenant_id).bind(&c.extension).bind(&realm);
            q = q.bind(&password).bind(&ha1).bind(&ha1b);
            q = q.bind(c.enabled as i64).bind(&c.clip).bind(c.clir as i64);
            q = q.bind(c.max_contacts).bind(&dp).bind(&c.description);
            q = q.bind(&c.ext_type).bind(&c.external_number).bind(&c.trunk_name).bind(&c.display_number);
            Ok(q.fetch_one(pool).await?)
        }
    }

    /// 部分更新。返回更新后的记录；不存在返回 sqlx::Error（调用方判断 404）。
    pub async fn update_extension(
        &self, id: i64, u: ExtensionUpdate,
    ) -> Result<Extension, sqlx::Error> {
        let Some(pool) = self.pool() else {
            return Err(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()));
        };
        let backend = self.backend();
        let mut sets: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();
        let mut idx: u32 = 0;

        macro_rules! set_if_some {
            ($field:expr, $val:expr) => {{
                if let Some(v) = $val {
                    idx += 1;
                    sets.push(format!("{} = ${}", $field, idx));
                    binds.push(v);
                }
            }};
        }

        /* Phase 2 多租户：先查现有 extension 的 tenant_id/realm/extension，
         * 因为 realm 强制 = tenant_name，tenant_id 变更时 realm 必须同步变更。 */
        let old_row_sql = fix_ph("SELECT tenant_id, realm, extension FROM extensions WHERE id = $1", backend);
        let (old_tid, old_realm, ext): (i64, String, String) = sqlx::query_as(&old_row_sql)
            .bind(id).fetch_one(pool).await?;
        let new_tid = u.tenant_id.unwrap_or(old_tid);
        let realm_changed = new_tid != old_tid;

        /* 计算更新后的 realm：tenant_id 变更 → 从 tenants 表查新 tenant_name；
         * 不变 → 保持旧值。tenant 被删 → fallback "nsbc.local"。 */
        let new_realm: String = if realm_changed {
            let realm_sub_sql = fix_ph("SELECT tenant_name FROM tenants WHERE id = $1", backend);
            sqlx::query_scalar(&realm_sub_sql)
                .bind(new_tid).fetch_optional(pool).await?
                .unwrap_or_else(|| "nsbc.local".to_string())
        } else { old_realm.clone() };

        // password + realm 组合处理：算正确的 ha1
        let ha1_from_password: Option<(String, String)> = if let Some(ref pw) = u.password {
            let ha1_hex = ha1_of(&ext, &new_realm, pw);
            let ha1b_hex = ha1_of(&ext, &new_realm, pw);
            Some((ha1_hex, ha1b_hex))
        } else if realm_changed {
            /* tenant_id 变了但没改密码 → 用新 realm 重算 ha1（旧 ha1 是老 realm 算的，会导致认证失败） */
            let pw_sql = fix_ph("SELECT password FROM extensions WHERE id = $1", backend);
            let cur_pw: String = sqlx::query_scalar(&pw_sql).bind(id).fetch_one(pool).await?;
            if !cur_pw.is_empty() {
                let ha1_hex = ha1_of(&ext, &new_realm, &cur_pw);
                let ha1b_hex = ha1_of(&ext, &new_realm, &cur_pw);
                Some((ha1_hex, ha1b_hex))
            } else { None }
        } else { None };

        // tenant_id + realm 同步写入
        set_if_some!("tenant_id", Some(new_tid.to_string()));
        if realm_changed {
            idx += 1; sets.push(format!("realm = ${idx}")); binds.push(new_realm.clone());
        }

        if let Some(ref pw) = u.password {
            idx += 1; sets.push(format!("password = ${idx}")); binds.push(pw.clone());
        }
        if let Some((h1, h1b)) = &ha1_from_password {
            idx += 1; sets.push(format!("ha1 = ${idx}"));  binds.push(h1.clone());
            idx += 1; sets.push(format!("ha1b = ${idx}")); binds.push(h1b.clone());
        } else {
            set_if_some!("ha1",  u.ha1.as_ref().map(|s| s.clone()));
            set_if_some!("ha1b", u.ha1b.as_ref().map(|s| s.clone()));
        }
        if let Some(en) = u.enabled   { idx += 1; sets.push(format!("enabled = CAST(${idx} AS INTEGER)")); binds.push(if en { "1".into() } else { "0".into() }); }
        set_if_some!("clip",          u.clip.as_ref().map(|s| s.clone()));
        if let Some(cl) = u.clir      { idx += 1; sets.push(format!("clir = CAST(${idx} AS INTEGER)")); binds.push(if cl { "1".into() } else { "0".into() }); }
        if let Some(mc) = u.max_contacts {
            idx += 1; sets.push(format!("max_contacts = CAST(${idx} AS INTEGER)")); binds.push(mc.to_string());
        }
        set_if_some!("dialplan_match", u.dialplan_match.as_ref().map(|s| s.clone()));
        set_if_some!("description",    u.description.as_ref().map(|s| s.clone()));
        // --- 外部分机支持 ---
        set_if_some!("ext_type",        u.ext_type.as_ref().map(|s| s.clone()));
        set_if_some!("external_number", u.external_number.as_ref().map(|s| s.clone()));
        set_if_some!("trunk_name",      u.trunk_name.as_ref().map(|s| s.clone()));
        set_if_some!("display_number",  u.display_number.as_ref().map(|s| s.clone()));

        if sets.is_empty() {
            // 没传任何字段——直接返回现有记录
            return self.get_extension(id).await.and_then(|r| {
                r.ok_or_else(|| sqlx::Error::RowNotFound)
            });
        }

        // updated_at 自动更新（三后端表达式略有不同）
        let ts_expr = match backend {
            "sqlite" => "datetime('now')",
            "postgres" => "CAST(NOW() AS TEXT)",
            _ => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)",
        };
        sets.push(format!("updated_at = {ts_expr}"));

        let where_idx = idx + 1;
        let raw_sql = format!(
            "UPDATE extensions SET {} WHERE id = ${where_idx} \
             RETURNING id, extension, realm, password, ha1, ha1b, enabled, clip, clir, \
                       max_contacts, dialplan_match, description, \
                       ext_type, external_number, trunk_name, display_number, \
                       created_at, updated_at",
            sets.join(", ")
        );

        if backend == "mysql" {
            let exec_sql = fix_ph(&strip_returning(&raw_sql), backend);
            let mut q = sqlx::query(&exec_sql);
            for b in &binds { q = q.bind(b.as_str()); }
            q = q.bind(id);
            q.execute(pool).await?;
            let row_sql = fix_ph(
                "SELECT id, tenant_id, extension, realm, password, ha1, ha1b, enabled, clip, clir, max_contacts, dialplan_match, description, ext_type, external_number, trunk_name, display_number, created_at, updated_at FROM extensions WHERE id = $1",
                backend
            );
            let row = sqlx::query_as::<_, Extension>(&row_sql).bind(id).fetch_one(pool).await?;
            Ok(row)
        } else {
            let exec_sql = fix_ph(&raw_sql, backend);
            let mut q = sqlx::query_as::<_, Extension>(&exec_sql);
            for b in &binds { q = q.bind(b.as_str()); }
            q = q.bind(id);
            Ok(q.fetch_one(pool).await?)
        }
    }

    /// 删除分机。返回 true=删除；false=不存在。
    pub async fn delete_extension(&self, id: i64) -> Result<bool, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(false); };
        let sql = fix_ph("DELETE FROM extensions WHERE id = $1", self.backend());
        let res = sqlx::query(&sql).bind(id).execute(pool).await?;
        Ok(res.rows_affected() > 0)
    }

    /// 导出所有启用分机（供 extensions.reload UDS 命令批量推入核心）。
    pub async fn list_enabled_extensions(&self) -> Result<Vec<Extension>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(Vec::new()); };
        let rows = sqlx::query_as::<_, Extension>(
            "SELECT id, tenant_id, extension, realm, password, ha1, ha1b, enabled, clip, clir, max_contacts, dialplan_match, description, ext_type, external_number, trunk_name, display_number, created_at, updated_at FROM extensions WHERE enabled = 1 ORDER BY extension"
        ).fetch_all(pool).await?;
        Ok(rows)
    }
}

// ---------- gateways + trunks CRUD（REQ-WEB-016）----------

/// 外线网关（逻辑分组，对应 route_group_t）。
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct Gateway {
    pub id:            i64,
    pub name:          String,
    pub description:   String,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enabled:       i64,
    pub created_at:    String,
    pub updated_at:    String,
}

#[derive(Debug, Clone, serde::Deserialize)]
pub struct GatewayCreate {
    pub name:          String,
    #[serde(default)]
    pub description:   String,
    #[serde(default = "default_true")]
    pub enabled:       bool,
}

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct GatewayUpdate {
    pub name:          Option<String>,
    pub description:   Option<String>,
    pub enabled:       Option<bool>,
}

/// 中继（对应 trunk_t）。
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct Trunk {
    pub id:             i64,
    pub tenant_id:      i64,
    pub name:           String,
    pub gateway_name:   String,
    pub profile_name:   String,
    pub kind:           String,
    pub proto:          i64,
    pub host:           String,
    pub port:           i64,
    pub auth_user:      String,
    pub realm:          String,
    pub codec_set:      String,
    pub weight:         i64,
    pub priority:       i64,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enabled:        i64,
    pub max_channels:   i64,
    pub failover_code:  i64,
    pub description:    String,
    // --- FreeSWITCH 兼容 + 用户新增字段 ---
    pub password:                String,
    pub from_user:               String,
    pub from_domain:             String,
    pub proxy:                   String,
    pub register_proxy:          String,
    pub expire_seconds:          i64,
    pub retry_seconds:           i64,
    pub caller_id_in_from:       i64,
    pub contact_params:          String,
    pub extension_in_contact:    i64,
    pub ping_seconds:            i64,
    pub inbound_extension:       String,
    pub cid_type:                String,
    pub rfc5626:                 i64,
    pub reg_id:                  String,
    pub peer_ip:                 String,
    pub caller_id_pool:          String,
    pub per_number_max_channels: i64,
    pub custom_headers:          String,
    // --- 3GPP IMS 自动头注入（REQ-TRUNK-IMS-001） ---
    pub ims_enabled:             i64,
    pub ims_domain:              String,
    pub ims_auto_privacy:        i64,
    pub ims_auto_pani:           i64,
    pub ims_pani_value:          String,
    pub created_at:     String,
    pub updated_at:     String,
}

#[derive(Debug, Clone, serde::Deserialize)]
pub struct TrunkCreate {
    pub tenant_id:      i64,
    pub name:           String,
    #[serde(default)]
    pub gateway_name:   String,
    #[serde(default)]
    pub profile_name:   String,
    #[serde(default = "default_static_peer")]
    pub kind:           String,
    #[serde(default)]
    pub proto:          i64,
    #[serde(default)]
    pub host:           String,
    #[serde(default = "default_5060")]
    pub port:           i64,
    #[serde(default)]
    pub auth_user:      String,
    #[serde(default)]
    pub realm:          String,
    #[serde(default)]
    pub codec_set:      String,
    #[serde(default = "default_1")]
    pub weight:         i64,
    #[serde(default = "default_1")]
    pub priority:       i64,
    #[serde(default = "default_true")]
    pub enabled:        bool,
    #[serde(default)]
    pub max_channels:   i64,
    #[serde(default)]
    pub failover_code:  i64,
    #[serde(default)]
    pub description:    String,
    // --- FreeSWITCH 兼容 + 用户新增字段 ---
    #[serde(default)] pub password:                String,
    #[serde(default)] pub from_user:               String,
    #[serde(default)] pub from_domain:             String,
    #[serde(default)] pub proxy:                   String,
    #[serde(default)] pub register_proxy:          String,
    #[serde(default = "default_3600")] pub expire_seconds: i64,
    #[serde(default = "default_5")]    pub retry_seconds:  i64,
    #[serde(default, deserialize_with = "deserialize_i64_or_bool")]
    pub caller_id_in_from:       i64,
    #[serde(default)] pub contact_params:          String,
    #[serde(default, deserialize_with = "deserialize_i64_or_bool")]
    pub extension_in_contact:    i64,
    #[serde(default)] pub ping_seconds:            i64,
    #[serde(default)] pub inbound_extension:       String,
    #[serde(default)] pub cid_type:                String,
    #[serde(default, alias = "rfc5626_enabled", deserialize_with = "deserialize_i64_or_bool")]
    pub rfc5626:                 i64,
    #[serde(default)] pub reg_id:                  String,
    #[serde(default)] pub peer_ip:                 String,
    #[serde(default)] pub caller_id_pool:          String,
    #[serde(default)] pub per_number_max_channels: i64,
    #[serde(default)] pub custom_headers:          String,
    // --- 3GPP IMS 自动头注入 ---
    #[serde(default, deserialize_with = "deserialize_i64_or_bool")] pub ims_enabled: i64,
    #[serde(default)] pub ims_domain:              String,
    #[serde(default, deserialize_with = "deserialize_i64_or_bool")] pub ims_auto_privacy: i64,
    #[serde(default, deserialize_with = "deserialize_i64_or_bool")] pub ims_auto_pani:    i64,
    #[serde(default)] pub ims_pani_value:          String,
}

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct TrunkUpdate {
    pub tenant_id:      Option<i64>,
    pub name:           Option<String>,
    pub gateway_name:   Option<String>,
    pub profile_name:   Option<String>,
    pub kind:           Option<String>,
    pub proto:          Option<i64>,
    pub host:           Option<String>,
    pub port:           Option<i64>,
    pub auth_user:      Option<String>,
    pub realm:          Option<String>,
    pub codec_set:      Option<String>,
    pub weight:         Option<i64>,
    pub priority:       Option<i64>,
    pub enabled:        Option<bool>,
    pub max_channels:   Option<i64>,
    pub failover_code:  Option<i64>,
    pub description:    Option<String>,
    // --- FreeSWITCH 兼容 + 用户新增字段 ---
    pub password:                Option<String>,
    pub from_user:               Option<String>,
    pub from_domain:             Option<String>,
    pub proxy:                   Option<String>,
    pub register_proxy:          Option<String>,
    pub expire_seconds:          Option<i64>,
    pub retry_seconds:           Option<i64>,
    #[serde(deserialize_with = "deserialize_opt_i64_or_bool", default)]
    pub caller_id_in_from:       Option<i64>,
    pub contact_params:          Option<String>,
    #[serde(deserialize_with = "deserialize_opt_i64_or_bool", default)]
    pub extension_in_contact:    Option<i64>,
    pub ping_seconds:            Option<i64>,
    pub inbound_extension:       Option<String>,
    pub cid_type:                Option<String>,
    #[serde(alias = "rfc5626_enabled", deserialize_with = "deserialize_opt_i64_or_bool", default)]
    pub rfc5626:                 Option<i64>,
    pub reg_id:                  Option<String>,
    pub peer_ip:                 Option<String>,
    pub caller_id_pool:          Option<String>,
    pub per_number_max_channels: Option<i64>,
    pub custom_headers:          Option<String>,
    // --- 3GPP IMS 自动头注入 ---
    pub ims_enabled:             Option<bool>,
    pub ims_domain:              Option<String>,
    pub ims_auto_privacy:        Option<bool>,
    pub ims_auto_pani:           Option<bool>,
    pub ims_pani_value:          Option<String>,
}

fn default_true() -> bool { true }
fn default_1() -> i64 { 1 }
fn default_5() -> i64 { 5 }
fn default_3600() -> i64 { 3600 }
fn default_5060() -> i64 { 5060 }
fn default_static_peer() -> String { "static_peer".to_string() }

/// 前端 bool 字段（true/false）→ Rust i64(0/1) 兼容。serde 默认不会自动 bool↔i64 转换，
/// 前端 formSchema 里 type:'bool' 的字段 POST 时发 JSON true/false，Rust i64 直接炸 422。
/// 用法：#[serde(default, deserialize_with = "deserialize_i64_or_bool")]
fn deserialize_i64_or_bool<'de, D>(deserializer: D) -> Result<i64, D::Error>
where D: serde::Deserializer<'de>
{
    use serde::Deserialize;
    #[derive(serde::Deserialize)]
    #[serde(untagged)]
    enum MaybeBoolOrI64 { Bool(bool), Int(i64) }
    match MaybeBoolOrI64::deserialize(deserializer)? {
        MaybeBoolOrI64::Bool(b) => Ok(if b { 1 } else { 0 }),
        MaybeBoolOrI64::Int(i) => Ok(i),
    }
}
/// Option<i64> 版本：前端可以传 bool / i64 / null / 不传 → Option<i64>
fn deserialize_opt_i64_or_bool<'de, D>(deserializer: D) -> Result<Option<i64>, D::Error>
where D: serde::Deserializer<'de>
{
    use serde::Deserialize;
    #[derive(serde::Deserialize)]
    #[serde(untagged)]
    enum MaybeBoolOrI64 { Bool(bool), Int(i64) }
    let v: Option<MaybeBoolOrI64> = Option::deserialize(deserializer)?;
    Ok(v.map(|x| match x {
        MaybeBoolOrI64::Bool(b) => if b { 1 } else { 0 },
        MaybeBoolOrI64::Int(i) => i,
    }))
}

impl Db {
    // ===== gateways =====

    pub async fn list_gateways(
        &self, limit: i64, offset: i64,
    ) -> Result<(Vec<Gateway>, i64), sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok((Vec::new(), 0)); };
        let backend = self.backend();
        let limit = limit.clamp(1, 500);
        let offset = offset.max(0);
        let total: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM gateways")
            .fetch_one(pool).await.unwrap_or(0);
        let rows_sql = fix_ph("SELECT * FROM gateways ORDER BY name LIMIT $1 OFFSET $2", backend);
        let rows = sqlx::query_as::<_, Gateway>(&rows_sql)
            .bind(limit).bind(offset).fetch_all(pool).await?;
        Ok((rows, total))
    }

    pub async fn list_enabled_gateways(&self) -> Result<Vec<Gateway>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(Vec::new()); };
        sqlx::query_as::<_, Gateway>(
            "SELECT * FROM gateways WHERE enabled = 1 ORDER BY name"
        ).fetch_all(pool).await
    }

    pub async fn get_gateway(&self, id: i64) -> Result<Option<Gateway>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(None); };
        let sql = fix_ph("SELECT * FROM gateways WHERE id = $1", self.backend());
        sqlx::query_as::<_, Gateway>(&sql)
            .bind(id).fetch_optional(pool).await
    }

    pub async fn create_gateway(&self, c: GatewayCreate) -> Result<Gateway, sqlx::Error> {
        let pool = self.pool().ok_or(
            sqlx::Error::Configuration("NSBC_DB_DSN not set".into())
        )?;
        let backend = self.backend();
        let raw_sql = "INSERT INTO gateways (name, description, enabled) VALUES ($1, $2, $3) RETURNING *";

        if backend == "mysql" {
            let exec_sql = fix_ph(&strip_returning(raw_sql), backend);
            sqlx::query(&exec_sql)
                .bind(&c.name).bind(&c.description).bind(c.enabled as i64)
                .execute(pool).await?;
            let row = sqlx::query_as::<_, Gateway>(
                "SELECT * FROM gateways WHERE id = LAST_INSERT_ID()"
            ).fetch_one(pool).await?;
            Ok(row)
        } else {
            let exec_sql = fix_ph(raw_sql, backend);
            Ok(sqlx::query_as::<_, Gateway>(&exec_sql)
                .bind(&c.name).bind(&c.description).bind(c.enabled as i64)
                .fetch_one(pool).await?)
        }
    }

    pub async fn update_gateway(
        &self, id: i64, u: GatewayUpdate,
    ) -> Result<Gateway, sqlx::Error> {
        let pool = self.pool().ok_or(
            sqlx::Error::Configuration("NSBC_DB_DSN not set".into())
        )?;
        let backend = self.backend();
        let mut sets: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();
        let mut idx: u32 = 0;

        if let Some(n) = u.name { idx += 1; sets.push(format!("name = ${idx}")); binds.push(n); }
        if let Some(d) = u.description { idx += 1; sets.push(format!("description = ${idx}")); binds.push(d); }
        if let Some(en) = u.enabled { idx += 1; sets.push(format!("enabled = ${idx}")); binds.push(if en { "1".into() } else { "0".into() }); }

        if sets.is_empty() {
            return self.get_gateway(id).await.and_then(|r| {
                r.ok_or_else(|| sqlx::Error::RowNotFound)
            });
        }

        let ts = match backend {
            "sqlite" => "datetime('now')",
            "postgres" => "CAST(NOW() AS TEXT)",
            _ => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)",
        };
        sets.push(format!("updated_at = {ts}"));

        let where_idx = idx + 1;
        let raw_sql = format!("UPDATE gateways SET {} WHERE id = ${where_idx} RETURNING *", sets.join(", "));

        if backend == "mysql" {
            let exec_sql = fix_ph(&strip_returning(&raw_sql), backend);
            let mut q = sqlx::query(&exec_sql);
            for b in &binds { q = q.bind(b.as_str()); }
            q = q.bind(id);
            q.execute(pool).await?;
            let row_sql = fix_ph("SELECT * FROM gateways WHERE id = $1", backend);
            let row = sqlx::query_as::<_, Gateway>(&row_sql).bind(id).fetch_one(pool).await?;
            Ok(row)
        } else {
            let exec_sql = fix_ph(&raw_sql, backend);
            let mut q = sqlx::query_as::<_, Gateway>(&exec_sql);
            for b in &binds { q = q.bind(b.as_str()); }
            q = q.bind(id);
            Ok(q.fetch_one(pool).await?)
        }
    }

    pub async fn delete_gateway(&self, id: i64) -> Result<bool, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(false); };
        let sql = fix_ph("DELETE FROM gateways WHERE id = $1", self.backend());
        let res = sqlx::query(&sql).bind(id).execute(pool).await?;
        Ok(res.rows_affected() > 0)
    }

    // ===== trunks =====

    pub async fn list_trunks(
        &self,
        tenant_id: Option<i64>,
        gateway_name: Option<&str>,
        enabled: Option<bool>,
        limit: i64,
        offset: i64,
    ) -> Result<(Vec<Trunk>, i64), sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok((Vec::new(), 0)); };
        let backend = self.backend();
        let limit = limit.clamp(1, 500);
        let offset = offset.max(0);

        let mut clauses: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();
        let mut idx: u32 = 0;

        if let Some(t) = tenant_id {
            if t > 0 { idx += 1; clauses.push(format!("tenant_id = ${idx}")); binds.push(t.to_string()); }
        }
        if let Some(g) = gateway_name {
            idx += 1; clauses.push(format!("gateway_name = ${idx}")); binds.push(g.to_string());
        }
        if let Some(en) = enabled {
            idx += 1; clauses.push(format!("enabled = ${idx}"));
            binds.push(if en { "1".into() } else { "0".into() });
        }

        let where_sql = if clauses.is_empty() { String::new() } else { "WHERE ".to_string() + &clauses.join(" AND ") };

        let count_sql = format!("SELECT COUNT(*) FROM trunks {where_sql}");
        let mut cq = sqlx::query_scalar::<_, i64>(&count_sql);
        for b in &binds { cq = cq.bind(b.as_str()); }
        let total = cq.fetch_one(pool).await.unwrap_or(0);

        let l = idx + 1;
        let o = idx + 2;
        let rows_sql = fix_ph(&format!("SELECT * FROM trunks {where_sql} ORDER BY priority, name LIMIT ${l} OFFSET ${o}"), backend);
        let mut q = sqlx::query_as::<_, Trunk>(&rows_sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(limit).bind(offset);
        let rows = q.fetch_all(pool).await?;
        Ok((rows, total))
    }

    pub async fn get_trunk(&self, id: i64) -> Result<Option<Trunk>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(None); };
        let sql = fix_ph("SELECT * FROM trunks WHERE id = $1", self.backend());
        sqlx::query_as::<_, Trunk>(&sql)
            .bind(id).fetch_optional(pool).await
    }

    pub async fn create_trunk(&self, c: TrunkCreate) -> Result<Trunk, sqlx::Error> {
        let pool = self.pool().ok_or(
            sqlx::Error::Configuration("NSBC_DB_DSN not set".into())
        )?;
        let backend = self.backend();
        let raw_sql = "INSERT INTO trunks
                (tenant_id, name, gateway_name, kind, proto, host, port, auth_user, realm,
                 codec_set, weight, priority, enabled, max_channels, failover_code, description,
                 password, from_user, from_domain, proxy, register_proxy,
                 expire_seconds, retry_seconds, caller_id_in_from, contact_params,
                 extension_in_contact, ping_seconds, inbound_extension, cid_type,
                 rfc5626, reg_id, peer_ip, caller_id_pool, per_number_max_channels, custom_headers,
                 ims_enabled, ims_domain, ims_auto_privacy, ims_auto_pani, ims_pani_value)
             VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15,
                     $16, $17, $18, $19, $20, $21, $22, $23, $24, $25, $26, $27, $28,
                     $29, $30, $31, $32, $33, $34,
                     $35, $36, $37, $38, $39, $40)
             RETURNING *";

        if backend == "mysql" {
            let exec_sql = fix_ph(&strip_returning(raw_sql), backend);
            sqlx::query(&exec_sql)
                .bind(c.tenant_id).bind(&c.name).bind(&c.gateway_name).bind(&c.kind)
                .bind(c.proto).bind(&c.host).bind(c.port).bind(&c.auth_user).bind(&c.realm)
                .bind(&c.codec_set).bind(c.weight).bind(c.priority).bind(c.enabled as i64)
                .bind(c.max_channels).bind(c.failover_code).bind(&c.description)
                .bind(&c.password).bind(&c.from_user).bind(&c.from_domain).bind(&c.proxy)
                .bind(&c.register_proxy).bind(c.expire_seconds).bind(c.retry_seconds)
                .bind(c.caller_id_in_from).bind(&c.contact_params).bind(c.extension_in_contact)
                .bind(c.ping_seconds).bind(&c.inbound_extension).bind(&c.cid_type)
                .bind(c.rfc5626).bind(&c.reg_id).bind(&c.peer_ip).bind(&c.caller_id_pool)
                .bind(c.per_number_max_channels).bind(&c.custom_headers)
                .bind(c.ims_enabled).bind(&c.ims_domain).bind(c.ims_auto_privacy)
                .bind(c.ims_auto_pani).bind(&c.ims_pani_value)
                .execute(pool).await?;
            let row = sqlx::query_as::<_, Trunk>(
                "SELECT * FROM trunks WHERE id = LAST_INSERT_ID()"
            ).fetch_one(pool).await?;
            Ok(row)
        } else {
            let exec_sql = fix_ph(raw_sql, backend);
            Ok(sqlx::query_as::<_, Trunk>(&exec_sql)
                .bind(c.tenant_id).bind(&c.name).bind(&c.gateway_name).bind(&c.kind)
                .bind(c.proto).bind(&c.host).bind(c.port).bind(&c.auth_user).bind(&c.realm)
                .bind(&c.codec_set).bind(c.weight).bind(c.priority).bind(c.enabled as i64)
                .bind(c.max_channels).bind(c.failover_code).bind(&c.description)
                .bind(&c.password).bind(&c.from_user).bind(&c.from_domain).bind(&c.proxy)
                .bind(&c.register_proxy).bind(c.expire_seconds).bind(c.retry_seconds)
                .bind(c.caller_id_in_from).bind(&c.contact_params).bind(c.extension_in_contact)
                .bind(c.ping_seconds).bind(&c.inbound_extension).bind(&c.cid_type)
                .bind(c.rfc5626).bind(&c.reg_id).bind(&c.peer_ip).bind(&c.caller_id_pool)
                .bind(c.per_number_max_channels).bind(&c.custom_headers)
                .bind(c.ims_enabled).bind(&c.ims_domain).bind(c.ims_auto_privacy)
                .bind(c.ims_auto_pani).bind(&c.ims_pani_value)
                .fetch_one(pool).await?)
        }
    }

    pub async fn update_trunk(
        &self, id: i64, u: TrunkUpdate,
    ) -> Result<Trunk, sqlx::Error> {
        let pool = self.pool().ok_or(
            sqlx::Error::Configuration("NSBC_DB_DSN not set".into())
        )?;
        let backend = self.backend();
        let mut sets: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();
        let mut idx: u32 = 0;

        macro_rules! set_s {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = ${idx}", $col)); binds.push(v); }
            }};
        }
        macro_rules! set_i {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = CAST(${idx} AS INTEGER)", $col)); binds.push(v.to_string()); }
            }};
        }
        macro_rules! set_b {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = CAST(${idx} AS INTEGER)", $col)); binds.push(if v { "1".into() } else { "0".into() }); }
            }};
        }

        set_s!("gateway_name", u.gateway_name.clone());
        set_s!("kind", u.kind.clone());
        set_i!("proto", u.proto);
        set_s!("host", u.host.clone());
        set_i!("port", u.port);
        set_s!("auth_user", u.auth_user.clone());
        set_s!("realm", u.realm.clone());
        set_s!("codec_set", u.codec_set.clone());
        set_i!("weight", u.weight);
        set_i!("priority", u.priority);
        set_b!("enabled", u.enabled);
        set_i!("max_channels", u.max_channels);
        set_i!("failover_code", u.failover_code);
        set_s!("description", u.description.clone());
        // --- FreeSWITCH 兼容 + 用户新增字段 ---
        set_s!("password", u.password.clone());
        set_s!("from_user", u.from_user.clone());
        set_s!("from_domain", u.from_domain.clone());
        set_s!("proxy", u.proxy.clone());
        set_s!("register_proxy", u.register_proxy.clone());
        set_i!("expire_seconds", u.expire_seconds);
        set_i!("retry_seconds", u.retry_seconds);
        set_i!("caller_id_in_from", u.caller_id_in_from);
        set_s!("contact_params", u.contact_params.clone());
        set_i!("extension_in_contact", u.extension_in_contact);
        set_i!("ping_seconds", u.ping_seconds);
        set_s!("inbound_extension", u.inbound_extension.clone());
        set_s!("cid_type", u.cid_type.clone());
        set_i!("rfc5626", u.rfc5626);
        set_s!("reg_id", u.reg_id.clone());
        set_s!("peer_ip", u.peer_ip.clone());
        set_s!("caller_id_pool", u.caller_id_pool.clone());
        set_i!("per_number_max_channels", u.per_number_max_channels);
        set_s!("custom_headers", u.custom_headers.clone());
        // --- 3GPP IMS 自动头注入 ---
        set_b!("ims_enabled",      u.ims_enabled);
        set_s!("ims_domain",       u.ims_domain.clone());
        set_b!("ims_auto_privacy", u.ims_auto_privacy);
        set_b!("ims_auto_pani",    u.ims_auto_pani);
        set_s!("ims_pani_value",   u.ims_pani_value.clone());

        if sets.is_empty() {
            return self.get_trunk(id).await.and_then(|r| {
                r.ok_or_else(|| sqlx::Error::RowNotFound)
            });
        }

        let ts = match backend {
            "sqlite" => "datetime('now')",
            "postgres" => "CAST(NOW() AS TEXT)",
            _ => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)",
        };
        sets.push(format!("updated_at = {ts}"));

        let where_idx = idx + 1;
        let raw_sql = format!("UPDATE trunks SET {} WHERE id = ${where_idx} RETURNING *", sets.join(", "));

        if backend == "mysql" {
            let exec_sql = fix_ph(&strip_returning(&raw_sql), backend);
            let mut q = sqlx::query(&exec_sql);
            for b in &binds { q = q.bind(b.as_str()); }
            q = q.bind(id);
            q.execute(pool).await?;
            let row_sql = fix_ph("SELECT * FROM trunks WHERE id = $1", backend);
            let row = sqlx::query_as::<_, Trunk>(&row_sql).bind(id).fetch_one(pool).await?;
            Ok(row)
        } else {
            let exec_sql = fix_ph(&raw_sql, backend);
            let mut q = sqlx::query_as::<_, Trunk>(&exec_sql);
            for b in &binds { q = q.bind(b.as_str()); }
            q = q.bind(id);
            Ok(q.fetch_one(pool).await?)
        }
    }

    pub async fn delete_trunk(&self, id: i64) -> Result<bool, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(false); };
        let sql = fix_ph("DELETE FROM trunks WHERE id = $1", self.backend());
        let res = sqlx::query(&sql).bind(id).execute(pool).await?;
        Ok(res.rows_affected() > 0)
    }

    pub async fn list_enabled_trunks(&self) -> Result<Vec<Trunk>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(Vec::new()); };
        sqlx::query_as::<_, Trunk>(
            "SELECT * FROM trunks WHERE enabled = 1 ORDER BY priority, name"
        ).fetch_all(pool).await
    }
}

// ---------- dialplan_rules + routes CRUD（REQ-WEB-016 / REQ-PBX-DP-001）----------

/// 路由策略组（对应 C 侧 route_group_t——中继组选路策略容器）。
/// 一个 route 可以关联多个 trunk（通过 trunk.gateway_name 或独立桥表——
/// 先用 gateway_name 字符串关联，假期回来编译时决定是否加 route_trunks 桥表）。
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct Route {
    pub id:            i64,
    pub name:          String,
    pub strategy:       String,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enabled:       i64,
    pub description:   String,
    pub created_at:    String,
    pub updated_at:    String,
}

#[derive(Debug, Clone, serde::Deserialize)]
pub struct RouteCreate {
    pub name:          String,
    #[serde(default = "default_priority")]
    pub strategy:       String,
    #[serde(default = "default_true")]
    pub enabled:       bool,
    #[serde(default)]
    pub description:   String,
}

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct RouteUpdate {
    pub name:          Option<String>,
    pub strategy:       Option<String>,
    pub enabled:       Option<bool>,
    pub description:   Option<String>,
}

/// 分机/DID/出局规则（对应 C 侧 dp_rule）。
/// - layer: "ext" 分机内呼 / "biz" 企业业务号 / "out" 出局路由
/// - match_expr: 被叫/主叫模式（"2000" / "100[0-9]" / "_44XXX" 等，pm_pattern_t 匹配）
/// - target: out 层=route/trunk 名；ext/biz 层=extension 号/IVR/queue
/// - action: JSON {"clip":"...", "clir":true, "record":true, "emergency":false, ...}
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct DialplanRule {
    pub id:         i64,
    pub tenant_id:  i64,
    pub name:       String,
    pub layer:          String,
    /* §15 新增：强制分区键 + 规则级 IP 准入 */
    pub profile_name:   String,  /* DEFAULT 'internal' */
    pub src_ip_cidr:    String,  /* DEFAULT '10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,127.0.0.0/8' */
    pub match_expr:     String,
    pub caller_match:   String,
    pub prio:           i64,
    pub strip:          i64,
    pub target:         String,
    pub rec_action:     i64,  /* 0=none 1=start 2=stop — C 侧 dp_execute 每条规则独立携带 */
    pub rec_path_template: String,  /* 可选路径模板，空=默认 yyyymmdd/callid_mix_seq.wav */
    pub action_json:    String,
    /* REQ-DP-APP-001: Application 链 JSON 数组 — reload 时推给 C 侧 dp_json_parse_one */
    pub app_chain:      String,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enabled:        i64,
    pub description:    String,
    pub created_at:     String,
    pub updated_at:     String,
}

#[derive(Debug, Clone, serde::Deserialize)]
pub struct DialplanRuleCreate {
    pub tenant_id:  i64,
    pub name:       String,
    /* §15: layer 保留但废弃，由 target 前缀自动推断——不再要求前端传 */
    #[serde(default = "default_ext")]
    pub layer:          String,
    /* §15 新增：默认 'internal' 和 RFC1918 内网网段，向后兼容旧 API 调用 */
    #[serde(default = "default_internal")]
    pub profile_name:   String,
    #[serde(default = "default_rfc1918")]
    pub src_ip_cidr:    String,
    pub match_expr:     String,
    #[serde(default)]
    pub caller_match:   String,
    #[serde(default = "default_1")]
    pub prio:           i64,
    #[serde(default)]
    pub strip:          i64,
    pub target:         String,
    #[serde(default)]
    pub rec_action:     i64,  /* 0=none 1=start 2=stop */
    #[serde(default)]
    pub rec_path_template: String,  /* 可选路径模板，空=系统默认 */
    #[serde(default = "default_empty_json")]
    pub action_json:    String,
    /* REQ-DP-APP-001: 默认空数组（无 app_chain 时跳过） */
    #[serde(default = "default_empty_array")]
    pub app_chain:      String,
    #[serde(default = "default_true")]
    pub enabled:        bool,
    #[serde(default)]
    pub description:    String,
}

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct DialplanRuleUpdate {
    pub tenant_id:  Option<i64>,
    pub name:       Option<String>,
    pub layer:      Option<String>,
    /* §15 新增：允许更新 */
    pub profile_name:   Option<String>,
    pub src_ip_cidr:    Option<String>,
    pub match_expr:     Option<String>,
    pub caller_match:   Option<String>,
    pub prio:           Option<i64>,
    pub strip:          Option<i64>,
    pub target:         Option<String>,
    pub rec_action:     Option<i64>,  /* 0=none 1=start 2=stop */
    pub rec_path_template: Option<String>,  /* 可选路径模板 */
    pub action_json:    Option<String>,
    /* REQ-DP-APP-001 */
    pub app_chain:      Option<String>,
    pub enabled:        Option<bool>,
    pub description:    Option<String>,
}

fn default_priority() -> String { "priority".to_string() }
fn default_empty_json() -> String { "{}".to_string() }
fn default_empty_array() -> String { "[]".to_string() }
/* §15 RFC1918 内网网段（默认 IP 准入，防盗打） */
const RFC1918_CIDR_DEFAULT: &str = "10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,127.0.0.0/8";
fn default_ext() -> String { "ext".to_string() }
/* default_internal() 已在 L2633 定义 */
/* §15 RFC1918 内网网段默认 IP 准入（防盗打） */
fn default_rfc1918() -> String { RFC1918_CIDR_DEFAULT.to_string() }

// ---------- ha_groups CRUD ----------

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct HaGroup {
    pub id:              i64,
    pub name:            String,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enabled:         i64,
    pub node_list_json:  String,
    pub quorum:          i64,
    pub election_timeout: i64,
    pub state:           String,
    pub description:     String,
    pub created_at:      String,
    pub updated_at:      String,
}

#[derive(Debug, Clone, serde::Deserialize)]
pub struct HaGroupCreate {
    pub name:             String,
    #[serde(default = "default_true")]
    pub enabled:          bool,
    #[serde(default = "default_empty_json")]
    pub node_list_json:   String,
    #[serde(default = "default_2")]
    pub quorum:           i64,
    #[serde(default = "default_5000")]
    pub election_timeout: i64,
    #[serde(default = "default_initializing")]
    pub state:            String,
    #[serde(default)]
    pub description:      String,
}

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct HaGroupUpdate {
    pub enabled:          Option<bool>,
    pub node_list_json:   Option<String>,
    pub quorum:           Option<i64>,
    pub election_timeout: Option<i64>,
    pub state:            Option<String>,
    pub description:      Option<String>,
}

fn default_2() -> i64 { 2 }
fn default_5000() -> i64 { 5000 }
fn default_initializing() -> String { "initializing".to_string() }

// ---------- time_groups CRUD ----------

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct TimeGroup {
    pub id:          i64,
    pub name:        String,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enabled:     i64,
    pub time_ranges: String,
    pub fallback_to: String,
    pub description: String,
    pub created_at:  String,
    pub updated_at:  String,
}

#[derive(Debug, Clone, serde::Deserialize)]
pub struct TimeGroupCreate {
    pub name:         String,
    #[serde(default = "default_true")]
    pub enabled:      bool,
    #[serde(default = "default_empty_json")]
    pub time_ranges:  String,
    #[serde(default)]
    pub fallback_to:  String,
    #[serde(default)]
    pub description:  String,
}

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct TimeGroupUpdate {
    pub enabled:      Option<bool>,
    pub time_ranges:  Option<String>,
    pub fallback_to:  Option<String>,
    pub description:  Option<String>,
}

// ---------- queues CRUD ----------

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct Queue {
    pub id:            i64,
    pub tenant_id:     i64,
    pub name:          String,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enabled:       i64,
    pub ring_strategy: String,
    pub max_wait_sec:  i64,
    pub moh_sound:     String,
    pub members_json:  String,
    pub description:   String,
    pub created_at:    String,
    pub updated_at:    String,
}

#[derive(Debug, Clone, serde::Deserialize)]
pub struct QueueCreate {
    pub tenant_id:     i64,
    pub name:            String,
    #[serde(default = "default_true")]
    pub enabled:         bool,
    #[serde(default = "default_ringall")]
    pub ring_strategy:   String,
    #[serde(default = "default_600")]
    pub max_wait_sec:    i64,
    #[serde(default = "default_moh")]
    pub moh_sound:       String,
    #[serde(default = "default_empty_json")]
    pub members_json:    String,
    #[serde(default)]
    pub description:     String,
}

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct QueueUpdate {
    pub tenant_id:       Option<i64>,
    pub name:            Option<String>,
    pub enabled:         Option<bool>,
    pub ring_strategy:   Option<String>,
    pub max_wait_sec:    Option<i64>,
    pub moh_sound:       Option<String>,
    pub members_json:    Option<String>,
    pub description:     Option<String>,
}

fn default_ringall() -> String { "ringall".to_string() }
fn default_600() -> i64 { 600 }
fn default_moh() -> String { "default".to_string() }

// ---------- ivr_menus CRUD ----------

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct IvrMenu {
    pub id:             i64,
    pub name:           String,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enabled:        i64,
    pub prompt_audio:   String,
    pub invalid_audio:  String,
    pub timeout_sec:    i64,
    pub max_attempts:   i64,
    pub options_json:   String,
    pub timeout_target: String,
    pub invalid_target: String,
    pub description:    String,
    pub created_at:     String,
    pub updated_at:     String,
}

#[derive(Debug, Clone, serde::Deserialize)]
pub struct IvrMenuCreate {
    pub name:            String,
    #[serde(default = "default_true")]
    pub enabled:         bool,
    #[serde(default)]
    pub prompt_audio:    String,
    #[serde(default)]
    pub invalid_audio:   String,
    #[serde(default = "default_10")]
    pub timeout_sec:     i64,
    #[serde(default = "default_3")]
    pub max_attempts:    i64,
    #[serde(default = "default_empty_json")]
    pub options_json:    String,
    #[serde(default)]
    pub timeout_target:  String,
    #[serde(default)]
    pub invalid_target:  String,
    #[serde(default)]
    pub description:     String,
}

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct IvrMenuUpdate {
    pub enabled:         Option<bool>,
    pub prompt_audio:    Option<String>,
    pub invalid_audio:   Option<String>,
    pub timeout_sec:     Option<i64>,
    pub max_attempts:    Option<i64>,
    pub options_json:    Option<String>,
    pub timeout_target:  Option<String>,
    pub invalid_target:  Option<String>,
    pub description:     Option<String>,
}

/// SIP 传输 Profile（多实例：每个 profile 独立 socket/RTP/协议特性）。
/// 字段与 C 侧 src/include/sip_profile.h sip_profile_t 1:1 对齐。
#[derive(Clone, Debug, sqlx::FromRow, serde::Serialize, serde::Deserialize)]
pub struct SipProfile {
    pub id:              i64,
    pub name:            String,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enabled:         i64,
    pub sip_ip:          String,
    pub external_sip_ip: String,
    pub rtp_ip:          String,
    pub external_rtp_ip: String,
    pub sip_udp_port:    i64,
    pub sip_tcp_port:    i64,
    pub sip_tls_port:    i64,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enable_udp:      i64,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enable_tcp:      i64,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enable_tls:      i64,
    pub rtp_port_start:  i64,
    pub rtp_port_end:    i64,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enable_prack:    i64,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enable_update:   i64,
    pub dtmf_mode:       String,
    pub realm:           String,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub auth_calls:      i64,
    pub ip_whitelist:    String,
    pub ip_blacklist:    String,
    pub description:     String,
    pub created_at:      String,
    pub updated_at:      String,
}

#[derive(Clone, Debug, serde::Deserialize)]
pub struct SipProfileCreate {
    pub name:            String,
    pub enabled:         Option<bool>,
    pub sip_ip:          Option<String>,
    pub external_sip_ip: Option<String>,
    pub rtp_ip:          Option<String>,
    pub external_rtp_ip: Option<String>,
    pub sip_udp_port:    Option<i64>,
    pub sip_tcp_port:    Option<i64>,
    pub sip_tls_port:    Option<i64>,
    pub enable_udp:      Option<bool>,
    pub enable_tcp:      Option<bool>,
    pub enable_tls:      Option<bool>,
    pub rtp_port_start:  Option<i64>,
    pub rtp_port_end:    Option<i64>,
    pub enable_prack:    Option<bool>,
    pub enable_update:   Option<bool>,
    pub dtmf_mode:       Option<String>,
    pub realm:           Option<String>,
    pub auth_calls:      Option<bool>,
    pub ip_whitelist:    Option<String>,
    pub ip_blacklist:    Option<String>,
    pub description:     Option<String>,
}

#[derive(Clone, Debug, serde::Deserialize)]
pub struct SipProfileUpdate {
    pub enabled:         Option<bool>,
    pub sip_ip:          Option<String>,
    pub external_sip_ip: Option<String>,
    pub rtp_ip:          Option<String>,
    pub external_rtp_ip: Option<String>,
    pub sip_udp_port:    Option<i64>,
    pub sip_tcp_port:    Option<i64>,
    pub sip_tls_port:    Option<i64>,
    pub enable_udp:      Option<bool>,
    pub enable_tcp:      Option<bool>,
    pub enable_tls:      Option<bool>,
    pub rtp_port_start:  Option<i64>,
    pub rtp_port_end:    Option<i64>,
    pub enable_prack:    Option<bool>,
    pub enable_update:   Option<bool>,
    pub dtmf_mode:       Option<String>,
    pub realm:           Option<String>,
    pub auth_calls:      Option<bool>,
    pub ip_whitelist:    Option<String>,
    pub ip_blacklist:    Option<String>,
    pub description:     Option<String>,
}


// ================ rate_tables 结构体 ================

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct RateTable {
    pub id:               i64,
    pub prefix:           String,
    pub name:             String,
    pub price_per_min_uc: i64,
    pub connect_fee_uc:   i64,
    pub bill_inc_s:       i64,
    pub min_bill_s:       i64,
    pub initial_s:        i64,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enabled:          i64,
    pub description:      String,
    pub created_at:       String,
    pub updated_at:       String,
}

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct RateTableCreate {
    pub prefix:           String,
    #[serde(default)]
    pub name:             String,
    #[serde(default)]
    pub price_per_min_uc: i64,
    #[serde(default)]
    pub connect_fee_uc:   i64,
    #[serde(default = "default_1i64")]
    pub bill_inc_s:       i64,
    #[serde(default)]
    pub min_bill_s:       i64,
    #[serde(default)]
    pub initial_s:        i64,
    #[serde(default = "default_true")]
    pub enabled:          bool,
    #[serde(default)]
    pub description:      String,
}

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct RateTableUpdate {
    pub prefix:           Option<String>,
    pub name:             Option<String>,
    pub price_per_min_uc: Option<i64>,
    pub connect_fee_uc:   Option<i64>,
    pub bill_inc_s:       Option<i64>,
    pub min_bill_s:       Option<i64>,
    pub initial_s:        Option<i64>,
    pub enabled:          Option<bool>,
    pub description:      Option<String>,
}

fn default_1i64() -> i64 { 1 }

// ================ security_blacklist 结构体（NIP 三层围栏 + deny/allow 双模式） ================

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct SecurityBlacklist {
    pub id:         i64,
    pub kind:       String,   // 'deny' | 'allow'
    pub rule_type:  String,   // 'ip_cidr' | 'ua' | 'called_prefix' | 'caller_prefix'
    pub value:      String,
    pub comment:    String,
    #[serde(serialize_with = "bool_i64::serialize")]
    pub enabled:    i64,
    pub created_at: String,
    pub updated_at: String,
}

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct SecurityBlacklistCreate {
    #[serde(default = "default_deny")]
    pub kind:       String,   // 缺省 deny
    /// 前端提交 "type"，DB 列名 rule_type，双名兼容
    #[serde(rename = "type", alias = "rule_type")]
    pub rule_type:  String,
    pub value:      String,
    #[serde(default)]
    pub comment:    String,
    #[serde(default = "default_true")]
    pub enabled:    bool,
}

fn default_deny() -> String { "deny".into() }

/// 仅允许改备注/启用（改 value = 删了重加，绕开 unique 冲突语义）
#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct SecurityBlacklistUpdate {
    pub comment: Option<String>,
    pub enabled: Option<bool>,
}

// ================ security_config 结构体（全局围栏配置） ================

#[derive(Debug, Clone, Default, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct SecurityConfig {
    pub fence_mode:       i64,  // 1=security, 0=performance
    pub method_blocked:   i64,  // 位图 (1<<(method-1))
    // fail2ban 五维配置（与 C 侧 security.c 的 g_cfg 对齐）
    pub auto_ban:             i64, // 1=开（默认）, 0=关
    pub authfail_threshold:   i64, // 窗口内认证失败次数阈值，默认 5
    pub authfail_window_s:    i64, // 时间窗秒，默认 60
    pub ban_duration_s:       i64, // 基础封禁秒（递进封禁基准），默认 3600
    pub ratelimit_regs:       i64, // REGISTER 纯频率阈值，默认 20/60s
    pub ratelimit_window_s:   i64, // ratelimit 窗口秒，默认 60
}

// ================ accounts 结构体 ================

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct Account {
    pub id:              i64,
    pub account_key:     String,
    pub balance_uc:      i64,
    pub credit_limit_uc: i64,
    pub description:     String,
    pub created_at:       String,
    pub updated_at:       String,
}

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct AccountCreate {
    pub account_key:     String,
    #[serde(default)]
    pub balance_uc:      i64,
    #[serde(default)]
    pub credit_limit_uc: i64,
    #[serde(default)]
    pub description:     String,
}
fn default_10() -> i64 { 10 }
fn default_3() -> i64 { 3 }

impl Db {
    // ===== routes =====

    pub async fn list_routes(
        &self, enabled: Option<bool>, limit: i64, offset: i64,
    ) -> Result<(Vec<Route>, i64), sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok((Vec::new(), 0)); };
        let backend = self.backend();
        let limit = limit.clamp(1, 500);
        let offset = offset.max(0);
        let (where_sql, binds) = match enabled {
            Some(v) => ("WHERE enabled = $1".to_string(),
                        vec![if v { "1".to_string() } else { "0".to_string() }]),
            None => (String::new(), Vec::new()),
        };
        let count_sql = fix_ph(&format!("SELECT COUNT(*) FROM routes {where_sql}"), backend);
        let mut cq = sqlx::query_scalar::<_, i64>(&count_sql);
        for b in &binds { cq = cq.bind(b.as_str()); }
        let total = cq.fetch_one(pool).await.unwrap_or(0);
        let rows_sql = fix_ph(&format!("SELECT * FROM routes {where_sql} ORDER BY name LIMIT ${} OFFSET ${}",
            binds.len() + 1, binds.len() + 2), backend);
        let mut q = sqlx::query_as::<_, Route>(&rows_sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(limit).bind(offset);
        let rows = q.fetch_all(pool).await?;
        Ok((rows, total))
    }

    pub async fn get_route(&self, id: i64) -> Result<Option<Route>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(None); };
        let backend = self.backend();
        sqlx::query_as::<_, Route>(&fix_ph("SELECT * FROM routes WHERE id = $1", backend))
            .bind(id).fetch_optional(pool).await
    }

    pub async fn create_route(&self, c: RouteCreate) -> Result<Route, sqlx::Error> {
        let pool = self.pool().ok_or(
            sqlx::Error::Configuration("NSBC_DB_DSN not set".into())
        )?;
        let backend = self.backend();
        let is_mysql = backend == "mysql";
        let raw_sql = "INSERT INTO routes (name, strategy, enabled, description)
             VALUES ($1, $2, $3, $4) RETURNING *";
        if is_mysql {
            let plain_sql = strip_returning(raw_sql);
            let sql = fix_ph(&plain_sql, backend);
            let res = sqlx::query(&sql)
                .bind(&c.name).bind(&c.strategy).bind(c.enabled as i64).bind(&c.description)
                .execute(pool).await?;
            let id = res.last_insert_id();
            let pick_sql = fix_ph("SELECT * FROM routes WHERE id = ?", backend);
            return sqlx::query_as::<_, Route>(&pick_sql).bind(id).fetch_one(pool).await;
        }
        let sql = fix_ph(raw_sql, backend);
        sqlx::query_as::<_, Route>(&sql)
        .bind(&c.name).bind(&c.strategy).bind(c.enabled as i64).bind(&c.description)
        .fetch_one(pool).await
    }

    pub async fn update_route(
        &self, id: i64, u: RouteUpdate,
    ) -> Result<Route, sqlx::Error> {
        let pool = self.pool().ok_or(
            sqlx::Error::Configuration("NSBC_DB_DSN not set".into())
        )?;
        let mut sets: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();
        let mut idx: u32 = 0;

        if let Some(n) = u.name { idx += 1; sets.push(format!("name = ${idx}")); binds.push(n); }
        if let Some(s) = u.strategy { idx += 1; sets.push(format!("strategy = ${idx}")); binds.push(s); }
        if let Some(en) = u.enabled { idx += 1; sets.push(format!("enabled = ${idx}")); binds.push(if en { "1".into() } else { "0".into() }); }
        if let Some(d) = u.description { idx += 1; sets.push(format!("description = ${idx}")); binds.push(d); }

        if sets.is_empty() {
            return self.get_route(id).await.and_then(|r| r.ok_or_else(|| sqlx::Error::RowNotFound));
        }
        let ts = match self.backend() { "sqlite" => "datetime('now')", "postgres" => "CAST(NOW() AS TEXT)", _ => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)" };
        sets.push(format!("updated_at = {ts}"));
        let where_idx = idx + 1;
        let sql = format!("UPDATE routes SET {} WHERE id = ${where_idx} RETURNING *", sets.join(", "));
        let mut q = sqlx::query_as::<_, Route>(&sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(id);
        q.fetch_one(pool).await
    }

    pub async fn delete_route(&self, id: i64) -> Result<bool, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(false); };
        let backend = self.backend();
        let res = sqlx::query(&fix_ph("DELETE FROM routes WHERE id = $1", backend)).bind(id).execute(pool).await?;
        Ok(res.rows_affected() > 0)
    }

    pub async fn list_enabled_routes(&self) -> Result<Vec<Route>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(Vec::new()); };
        sqlx::query_as::<_, Route>("SELECT * FROM routes WHERE enabled = 1 ORDER BY name")
            .fetch_all(pool).await
    }

    // ===== dialplan_rules =====

    pub async fn list_dialplan_rules(
        &self,
        tenant_id: Option<i64>,
        layer: Option<&str>,
        enabled: Option<bool>,
        limit: i64,
        offset: i64,
    ) -> Result<(Vec<DialplanRule>, i64), sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok((Vec::new(), 0)); };
        let backend = self.backend();
        let limit = limit.clamp(1, 500);
        let offset = offset.max(0);

        let mut clauses: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();
        let mut idx: u32 = 0;

        if let Some(t) = tenant_id {
            if t > 0 { idx += 1; clauses.push(format!("tenant_id = ${idx}")); binds.push(t.to_string()); }
        }
        if let Some(l) = layer { idx += 1; clauses.push(format!("layer = ${idx}")); binds.push(l.to_string()); }
        if let Some(en) = enabled {
            idx += 1; clauses.push(format!("enabled = ${idx}"));
            binds.push(if en { "1".into() } else { "0".into() });
        }
        let where_sql = if clauses.is_empty() { String::new() } else { "WHERE ".to_string() + &clauses.join(" AND ") };

        let count_sql = fix_ph(&format!("SELECT COUNT(*) FROM dialplan_rules {where_sql}"), backend);
        let mut cq = sqlx::query_scalar::<_, i64>(&count_sql);
        for b in &binds { cq = cq.bind(b.as_str()); }
        let total = cq.fetch_one(pool).await.unwrap_or(0);

        let l = idx + 1;
        let o = idx + 2;
        let rows_sql = fix_ph(&format!("SELECT * FROM dialplan_rules {where_sql} ORDER BY layer, prio, name LIMIT ${l} OFFSET ${o}"), backend);
        let mut q = sqlx::query_as::<_, DialplanRule>(&rows_sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(limit).bind(offset);
        let rows = q.fetch_all(pool).await?;
        Ok((rows, total))
    }

    pub async fn get_dialplan_rule(&self, id: i64) -> Result<Option<DialplanRule>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(None); };
        let backend = self.backend();
        sqlx::query_as::<_, DialplanRule>(&fix_ph("SELECT * FROM dialplan_rules WHERE id = $1", backend))
            .bind(id).fetch_optional(pool).await
    }

    pub async fn create_dialplan_rule(
        &self, c: DialplanRuleCreate,
    ) -> Result<DialplanRule, sqlx::Error> {
        let pool = self.pool().ok_or(
            sqlx::Error::Configuration("NSBC_DB_DSN not set".into())
        )?;
        let act = if c.action_json.is_empty() { "{}".to_string() } else { c.action_json };
        let ac = if c.app_chain.is_empty() { "[]".to_string() } else { c.app_chain };
        sqlx::query_as::<_, DialplanRule>(
            "INSERT INTO dialplan_rules
                (tenant_id, name, layer, profile_name, src_ip_cidr, match_expr, caller_match,
                 prio, strip, target, rec_action, rec_path_template, action_json, app_chain, enabled, description)
             VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16)
             RETURNING *"
        )
        .bind(c.tenant_id).bind(&c.name).bind(&c.layer).bind(&c.profile_name).bind(&c.src_ip_cidr)
        .bind(&c.match_expr).bind(&c.caller_match)
        .bind(c.prio).bind(c.strip).bind(&c.target).bind(c.rec_action).bind(&c.rec_path_template).bind(&act).bind(&ac)
        .bind(c.enabled as i64).bind(&c.description)
        .fetch_one(pool).await
    }

    pub async fn update_dialplan_rule(
        &self, id: i64, u: DialplanRuleUpdate,
    ) -> Result<DialplanRule, sqlx::Error> {
        let pool = self.pool().ok_or(
            sqlx::Error::Configuration("NSBC_DB_DSN not set".into())
        )?;
        let backend = self.backend();
        let is_mysql = backend == "mysql";
        let mut sets: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();
        let mut idx: u32 = 0;

        macro_rules! set_sr {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = ${idx}", $col)); binds.push(v); }
            }};
        }
        macro_rules! set_ir {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = CAST(${idx} AS INTEGER)", $col)); binds.push(v.to_string()); }
            }};
        }
        macro_rules! set_br {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = CAST(${idx} AS INTEGER)", $col)); binds.push(if v { "1".into() } else { "0".into() }); }
            }};
        }

        set_sr!("layer", u.layer.clone());
        set_sr!("profile_name", u.profile_name.clone());
        set_sr!("src_ip_cidr", u.src_ip_cidr.clone());
        set_sr!("match_expr", u.match_expr.clone());
        set_sr!("caller_match", u.caller_match.clone());
        set_ir!("prio", u.prio);
        set_ir!("strip", u.strip);
        set_sr!("target", u.target.clone());
        set_ir!("rec_action", u.rec_action);  /* §16 新增：录音控制每条规则独立 */
        set_sr!("rec_path_template", u.rec_path_template.clone());  /* §18 新增：可选路径模板 */
        set_sr!("action_json", u.action_json.clone());
        set_sr!("app_chain", u.app_chain.clone());  /* REQ-DP-APP-001 */
        set_br!("enabled", u.enabled);
        set_sr!("description", u.description.clone());

        if sets.is_empty() {
            return self.get_dialplan_rule(id).await.and_then(|r| r.ok_or_else(|| sqlx::Error::RowNotFound));
        }
        let ts = match backend { "sqlite" => "datetime('now')", "postgres" => "CAST(NOW() AS TEXT)", _ => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)" };
        sets.push(format!("updated_at = {ts}"));
        let where_idx = idx + 1;
        let raw = format!("UPDATE dialplan_rules SET {} WHERE id = ${where_idx} RETURNING *", sets.join(", "));
        if is_mysql {
            let plain_sql = strip_returning(&raw);
            let sql = fix_ph(&plain_sql, backend);
            let mut q = sqlx::query(&sql);
            for b in &binds { q = q.bind(b.as_str()); }
            q = q.bind(id);
            q.execute(pool).await?;
            let pick_sql = fix_ph("SELECT * FROM dialplan_rules WHERE id = ?", backend);
            return sqlx::query_as::<_, DialplanRule>(&pick_sql).bind(id).fetch_one(pool).await;
        }
        let sql = fix_ph(&raw, backend);
        let mut q = sqlx::query_as::<_, DialplanRule>(&sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(id);
        q.fetch_one(pool).await
    }

    pub async fn delete_dialplan_rule(&self, id: i64) -> Result<bool, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(false); };
        let backend = self.backend();
        let res = sqlx::query(&fix_ph("DELETE FROM dialplan_rules WHERE id = $1", backend)).bind(id).execute(pool).await?;
        Ok(res.rows_affected() > 0)
    }

    pub async fn list_enabled_dialplan_rules(
        &self, layer: Option<&str>,
    ) -> Result<Vec<DialplanRule>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(Vec::new()); };
        let backend = self.backend();
        let rows = if let Some(l) = layer {
            sqlx::query_as::<_, DialplanRule>(&fix_ph(
                "SELECT * FROM dialplan_rules WHERE enabled = 1 AND layer = $1 \
                 ORDER BY profile_name, prio, id", backend
            )).bind(l).fetch_all(pool).await?
        } else {
            sqlx::query_as::<_, DialplanRule>(&fix_ph(
                "SELECT * FROM dialplan_rules WHERE enabled = 1 \
                 ORDER BY profile_name, prio, id", backend
            )).fetch_all(pool).await?
        };
        Ok(rows)
    }

    // ===== ha_groups =====

    pub async fn list_ha_groups(
        &self, enabled: Option<bool>, limit: i64, offset: i64,
    ) -> Result<(Vec<HaGroup>, i64), sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok((Vec::new(), 0)); };
        let backend = self.backend();
        let limit = limit.clamp(1, 500);
        let offset = offset.max(0);
        let (where_sql, binds) = match enabled {
            Some(v) => ("WHERE enabled = $1".to_string(),
                        vec![if v { "1".to_string() } else { "0".to_string() }]),
            None => (String::new(), Vec::new()),
        };
        let count_sql = fix_ph(&format!("SELECT COUNT(*) FROM ha_groups {where_sql}"), backend);
        let mut cq = sqlx::query_scalar::<_, i64>(&count_sql);
        for b in &binds { cq = cq.bind(b.as_str()); }
        let total = cq.fetch_one(pool).await.unwrap_or(0);
        let rows_sql = fix_ph(&format!("SELECT * FROM ha_groups {where_sql} ORDER BY name LIMIT ${} OFFSET ${}",
            binds.len() + 1, binds.len() + 2), backend);
        let mut q = sqlx::query_as::<_, HaGroup>(&rows_sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(limit).bind(offset);
        let rows = q.fetch_all(pool).await?;
        Ok((rows, total))
    }

    pub async fn get_ha_group(&self, id: i64) -> Result<Option<HaGroup>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(None); };
        let backend = self.backend();
        sqlx::query_as::<_, HaGroup>(&fix_ph("SELECT * FROM ha_groups WHERE id = $1", backend))
            .bind(id).fetch_optional(pool).await
    }

    pub async fn create_ha_group(&self, c: HaGroupCreate) -> Result<HaGroup, sqlx::Error> {
        let pool = self.pool().ok_or(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()))?;
        let backend = self.backend();
        let is_mysql = backend == "mysql";
        let nodes = if c.node_list_json.is_empty() { "[]".to_string() } else { c.node_list_json };
        let raw_sql = "INSERT INTO ha_groups
                (name, enabled, node_list_json, quorum, election_timeout, state, description)
             VALUES ($1, $2, $3, $4, $5, $6, $7)
             RETURNING *";
        if is_mysql {
            let plain_sql = strip_returning(raw_sql);
            let sql = fix_ph(&plain_sql, backend);
            let res = sqlx::query(&sql)
                .bind(&c.name).bind(c.enabled as i64).bind(&nodes)
                .bind(c.quorum).bind(c.election_timeout)
                .bind(&c.state).bind(&c.description)
                .execute(pool).await?;
            let id = res.last_insert_id();
            let pick_sql = fix_ph("SELECT * FROM ha_groups WHERE id = ?", backend);
            return sqlx::query_as::<_, HaGroup>(&pick_sql).bind(id).fetch_one(pool).await;
        }
        let sql = fix_ph(raw_sql, backend);
        sqlx::query_as::<_, HaGroup>(&sql)
        .bind(&c.name).bind(c.enabled as i64).bind(&nodes)
        .bind(c.quorum).bind(c.election_timeout)
        .bind(&c.state).bind(&c.description)
        .fetch_one(pool).await
    }

    pub async fn update_ha_group(&self, id: i64, u: HaGroupUpdate) -> Result<HaGroup, sqlx::Error> {
        let pool = self.pool().ok_or(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()))?;
        let backend = self.backend();
        let is_mysql = backend == "mysql";
        let mut sets: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();
        let mut idx: u32 = 0;

        macro_rules! set_sr {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = ${idx}", $col)); binds.push(v); }
            }};
        }
        macro_rules! set_ir {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = CAST(${idx} AS INTEGER)", $col)); binds.push(v.to_string()); }
            }};
        }
        macro_rules! set_br {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = CAST(${idx} AS INTEGER)", $col)); binds.push(if v { "1".into() } else { "0".into() }); }
            }};
        }

        set_br!("enabled", u.enabled);
        set_sr!("node_list_json", u.node_list_json.clone());
        set_ir!("quorum", u.quorum);
        set_ir!("election_timeout", u.election_timeout);
        set_sr!("state", u.state.clone());
        set_sr!("description", u.description.clone());

        if sets.is_empty() {
            return self.get_ha_group(id).await.and_then(|r| r.ok_or_else(|| sqlx::Error::RowNotFound));
        }
        let ts = match backend { "sqlite" => "datetime('now')", "postgres" => "CAST(NOW() AS TEXT)", _ => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)" };
        sets.push(format!("updated_at = {ts}"));
        let where_idx = idx + 1;
        let raw = format!("UPDATE ha_groups SET {} WHERE id = ${where_idx} RETURNING *", sets.join(", "));
        if is_mysql {
            let plain_sql = strip_returning(&raw);
            let sql = fix_ph(&plain_sql, backend);
            let mut q = sqlx::query(&sql);
            for b in &binds { q = q.bind(b.as_str()); }
            q = q.bind(id);
            q.execute(pool).await?;
            let pick_sql = fix_ph("SELECT * FROM ha_groups WHERE id = ?", backend);
            return sqlx::query_as::<_, HaGroup>(&pick_sql).bind(id).fetch_one(pool).await;
        }
        let sql = fix_ph(&raw, backend);
        let mut q = sqlx::query_as::<_, HaGroup>(&sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(id);
        q.fetch_one(pool).await
    }

    pub async fn delete_ha_group(&self, id: i64) -> Result<bool, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(false); };
        let backend = self.backend();
        let res = sqlx::query(&fix_ph("DELETE FROM ha_groups WHERE id = $1", backend)).bind(id).execute(pool).await?;
        Ok(res.rows_affected() > 0)
    }

    pub async fn list_enabled_ha_groups(&self) -> Result<Vec<HaGroup>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(Vec::new()); };
        sqlx::query_as::<_, HaGroup>("SELECT * FROM ha_groups WHERE enabled = 1 ORDER BY name")
            .fetch_all(pool).await
    }

    // ===== time_groups =====

    pub async fn list_time_groups(
        &self, enabled: Option<bool>, limit: i64, offset: i64,
    ) -> Result<(Vec<TimeGroup>, i64), sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok((Vec::new(), 0)); };
        let backend = self.backend();
        let limit = limit.clamp(1, 500);
        let offset = offset.max(0);
        let (where_sql, binds) = match enabled {
            Some(v) => ("WHERE enabled = $1".to_string(),
                        vec![if v { "1".to_string() } else { "0".to_string() }]),
            None => (String::new(), Vec::new()),
        };
        let count_sql = fix_ph(&format!("SELECT COUNT(*) FROM time_groups {where_sql}"), backend);
        let mut cq = sqlx::query_scalar::<_, i64>(&count_sql);
        for b in &binds { cq = cq.bind(b.as_str()); }
        let total = cq.fetch_one(pool).await.unwrap_or(0);
        let rows_sql = fix_ph(&format!("SELECT * FROM time_groups {where_sql} ORDER BY name LIMIT ${} OFFSET ${}",
            binds.len() + 1, binds.len() + 2), backend);
        let mut q = sqlx::query_as::<_, TimeGroup>(&rows_sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(limit).bind(offset);
        let rows = q.fetch_all(pool).await?;
        Ok((rows, total))
    }

    pub async fn get_time_group(&self, id: i64) -> Result<Option<TimeGroup>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(None); };
        let backend = self.backend();
        sqlx::query_as::<_, TimeGroup>(&fix_ph("SELECT * FROM time_groups WHERE id = $1", backend))
            .bind(id).fetch_optional(pool).await
    }

    pub async fn create_time_group(&self, c: TimeGroupCreate) -> Result<TimeGroup, sqlx::Error> {
        let pool = self.pool().ok_or(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()))?;
        let backend = self.backend();
        let is_mysql = backend == "mysql";
        let tr = if c.time_ranges.is_empty() { "[]".to_string() } else { c.time_ranges };
        let raw_sql = "INSERT INTO time_groups (name, enabled, time_ranges, fallback_to, description)
             VALUES ($1, $2, $3, $4, $5) RETURNING *";
        if is_mysql {
            let plain_sql = strip_returning(raw_sql);
            let sql = fix_ph(&plain_sql, backend);
            let res = sqlx::query(&sql)
                .bind(&c.name).bind(c.enabled as i64).bind(&tr).bind(&c.fallback_to).bind(&c.description)
                .execute(pool).await?;
            let id = res.last_insert_id();
            let pick_sql = fix_ph("SELECT * FROM time_groups WHERE id = ?", backend);
            return sqlx::query_as::<_, TimeGroup>(&pick_sql).bind(id).fetch_one(pool).await;
        }
        let sql = fix_ph(raw_sql, backend);
        sqlx::query_as::<_, TimeGroup>(&sql)
        .bind(&c.name).bind(c.enabled as i64).bind(&tr).bind(&c.fallback_to).bind(&c.description)
        .fetch_one(pool).await
    }

    pub async fn update_time_group(&self, id: i64, u: TimeGroupUpdate) -> Result<TimeGroup, sqlx::Error> {
        let pool = self.pool().ok_or(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()))?;
        let backend = self.backend();
        let is_mysql = backend == "mysql";
        let mut sets: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();
        let mut idx: u32 = 0;

        macro_rules! set_sr_ {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = ${idx}", $col)); binds.push(v); }
            }};
        }
        macro_rules! set_br_ {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = CAST(${idx} AS INTEGER)", $col)); binds.push(if v { "1".into() } else { "0".into() }); }
            }};
        }

        set_br_!("enabled", u.enabled);
        set_sr_!("time_ranges", u.time_ranges.clone());
        set_sr_!("fallback_to", u.fallback_to.clone());
        set_sr_!("description", u.description.clone());

        if sets.is_empty() {
            return self.get_time_group(id).await.and_then(|r| r.ok_or_else(|| sqlx::Error::RowNotFound));
        }
        let ts = match backend { "sqlite" => "datetime('now')", "postgres" => "CAST(NOW() AS TEXT)", _ => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)" };
        sets.push(format!("updated_at = {ts}"));
        let where_idx = idx + 1;
        let raw = format!("UPDATE time_groups SET {} WHERE id = ${where_idx} RETURNING *", sets.join(", "));
        if is_mysql {
            let plain_sql = strip_returning(&raw);
            let sql = fix_ph(&plain_sql, backend);
            let mut q = sqlx::query(&sql);
            for b in &binds { q = q.bind(b.as_str()); }
            q = q.bind(id);
            q.execute(pool).await?;
            let pick_sql = fix_ph("SELECT * FROM time_groups WHERE id = ?", backend);
            return sqlx::query_as::<_, TimeGroup>(&pick_sql).bind(id).fetch_one(pool).await;
        }
        let sql = fix_ph(&raw, backend);
        let mut q = sqlx::query_as::<_, TimeGroup>(&sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(id);
        q.fetch_one(pool).await
    }

    pub async fn delete_time_group(&self, id: i64) -> Result<bool, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(false); };
        let backend = self.backend();
        let res = sqlx::query(&fix_ph("DELETE FROM time_groups WHERE id = $1", backend)).bind(id).execute(pool).await?;
        Ok(res.rows_affected() > 0)
    }

    pub async fn list_enabled_time_groups(&self) -> Result<Vec<TimeGroup>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(Vec::new()); };
        sqlx::query_as::<_, TimeGroup>("SELECT * FROM time_groups WHERE enabled = 1 ORDER BY name")
            .fetch_all(pool).await
    }

    // ===== queues =====

    pub async fn list_queues(
        &self, tenant_id: Option<i64>, enabled: Option<bool>, limit: i64, offset: i64,
    ) -> Result<(Vec<Queue>, i64), sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok((Vec::new(), 0)); };
        let backend = self.backend();
        let limit = limit.clamp(1, 500);
        let offset = offset.max(0);

        let mut clauses: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();
        let mut idx: u32 = 0;

        if let Some(t) = tenant_id {
            if t > 0 { idx += 1; clauses.push(format!("tenant_id = ${idx}")); binds.push(t.to_string()); }
        }
        if let Some(en) = enabled {
            idx += 1; clauses.push(format!("enabled = ${idx}"));
            binds.push(if en { "1".to_string() } else { "0".to_string() });
        }
        let where_sql = if clauses.is_empty() { String::new() } else { "WHERE ".to_string() + &clauses.join(" AND ") };

        let count_sql = fix_ph(&format!("SELECT COUNT(*) FROM queues {where_sql}"), backend);
        let mut cq = sqlx::query_scalar::<_, i64>(&count_sql);
        for b in &binds { cq = cq.bind(b.as_str()); }
        let total = cq.fetch_one(pool).await.unwrap_or(0);
        let rows_sql = fix_ph(&format!("SELECT * FROM queues {where_sql} ORDER BY name LIMIT ${} OFFSET ${}",
            binds.len() + 1, binds.len() + 2), backend);
        let mut q = sqlx::query_as::<_, Queue>(&rows_sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(limit).bind(offset);
        let rows = q.fetch_all(pool).await?;
        Ok((rows, total))
    }

    pub async fn get_queue(&self, id: i64) -> Result<Option<Queue>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(None); };
        let backend = self.backend();
        sqlx::query_as::<_, Queue>(&fix_ph("SELECT * FROM queues WHERE id = $1", backend))
            .bind(id).fetch_optional(pool).await
    }

    pub async fn create_queue(&self, c: QueueCreate) -> Result<Queue, sqlx::Error> {
        let pool = self.pool().ok_or(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()))?;
        let backend = self.backend();
        let is_mysql = backend == "mysql";
        let members = if c.members_json.is_empty() { "[]".to_string() } else { c.members_json };
        let raw_sql = "INSERT INTO queues (tenant_id, name, enabled, ring_strategy, max_wait_sec, moh_sound, members_json, description)
             VALUES ($1, $2, $3, $4, $5, $6, $7, $8) RETURNING *";
        if is_mysql {
            let plain_sql = strip_returning(raw_sql);
            let sql = fix_ph(&plain_sql, backend);
            let res = sqlx::query(&sql)
                .bind(c.tenant_id).bind(&c.name).bind(c.enabled as i64).bind(&c.ring_strategy)
                .bind(c.max_wait_sec).bind(&c.moh_sound).bind(&members).bind(&c.description)
                .execute(pool).await?;
            let id = res.last_insert_id();
            let pick_sql = fix_ph("SELECT * FROM queues WHERE id = ?", backend);
            return sqlx::query_as::<_, Queue>(&pick_sql).bind(id).fetch_one(pool).await;
        }
        let sql = fix_ph(raw_sql, backend);
        sqlx::query_as::<_, Queue>(&sql)
        .bind(c.tenant_id).bind(&c.name).bind(c.enabled as i64).bind(&c.ring_strategy)
        .bind(c.max_wait_sec).bind(&c.moh_sound).bind(&members).bind(&c.description)
        .fetch_one(pool).await
    }

    pub async fn update_queue(&self, id: i64, u: QueueUpdate) -> Result<Queue, sqlx::Error> {
        let pool = self.pool().ok_or(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()))?;
        let backend = self.backend();
        let is_mysql = backend == "mysql";
        let mut sets: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();
        let mut idx: u32 = 0;

        macro_rules! set_srq {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = ${idx}", $col)); binds.push(v); }
            }};
        }
        macro_rules! set_iq {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = CAST(${idx} AS INTEGER)", $col)); binds.push(v.to_string()); }
            }};
        }
        macro_rules! set_bq {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = CAST(${idx} AS INTEGER)", $col)); binds.push(if v { "1".into() } else { "0".into() }); }
            }};
        }

        set_bq!("enabled", u.enabled);
        set_srq!("ring_strategy", u.ring_strategy.clone());
        set_iq!("max_wait_sec", u.max_wait_sec);
        set_srq!("moh_sound", u.moh_sound.clone());
        set_srq!("members_json", u.members_json.clone());
        set_srq!("description", u.description.clone());

        if sets.is_empty() {
            return self.get_queue(id).await.and_then(|r| r.ok_or_else(|| sqlx::Error::RowNotFound));
        }
        let ts = match backend { "sqlite" => "datetime('now')", "postgres" => "CAST(NOW() AS TEXT)", _ => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)" };
        sets.push(format!("updated_at = {ts}"));
        let where_idx = idx + 1;
        let raw = format!("UPDATE queues SET {} WHERE id = ${where_idx} RETURNING *", sets.join(", "));
        if is_mysql {
            let plain_sql = strip_returning(&raw);
            let sql = fix_ph(&plain_sql, backend);
            let mut q = sqlx::query(&sql);
            for b in &binds { q = q.bind(b.as_str()); }
            q = q.bind(id);
            q.execute(pool).await?;
            let pick_sql = fix_ph("SELECT * FROM queues WHERE id = ?", backend);
            return sqlx::query_as::<_, Queue>(&pick_sql).bind(id).fetch_one(pool).await;
        }
        let sql = fix_ph(&raw, backend);
        let mut q = sqlx::query_as::<_, Queue>(&sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(id);
        q.fetch_one(pool).await
    }

    pub async fn delete_queue(&self, id: i64) -> Result<bool, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(false); };
        let backend = self.backend();
        let res = sqlx::query(&fix_ph("DELETE FROM queues WHERE id = $1", backend)).bind(id).execute(pool).await?;
        Ok(res.rows_affected() > 0)
    }

    /// 列出所有启用的队列（供 C 侧 reload 用）。同时 JOIN tenants 表取 tenant_name，
    /// 因为 C 侧 queue_table_reload_json 用 tenant_name 字符串做匹配（SIP realm），
    /// 不能用 tenant_id 数字串。
    pub async fn list_enabled_queues(&self) -> Result<Vec<(Queue, String)>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(Vec::new()); };
        // 用 LEFT JOIN 兼容租户被删的情况（fallback 用 tenant_id 数字串）
        // sqlx::query_as 不支持 tuple 里放 Option<T>，这里用手动 Fetch 再组装
        let rows: Vec<Queue> = sqlx::query_as::<_, Queue>(
            "SELECT q.* FROM queues q WHERE q.enabled = 1 ORDER BY q.name"
        ).fetch_all(pool).await?;
        // 批量拿 tenant_id → tenant_name 映射
        let tenant_ids: Vec<i64> = rows.iter().map(|q| q.tenant_id).collect();
        let mut map: std::collections::HashMap<i64, String> = std::collections::HashMap::new();
        if !tenant_ids.is_empty() {
            // 用 IN 查询拿 tenant_name（多后端参数占位符统一用 $1 绑定 slice——需逐值 IN）
            // 简单方案：每条单独查（队列数通常 <100，O(n) 可接受）
            for tid in &tenant_ids {
                if map.contains_key(tid) { continue; }
                let name: Option<String> = sqlx::query_scalar("SELECT tenant_name FROM tenants WHERE id = $1")
                    .bind(tid).fetch_optional(pool).await.unwrap_or(None);
                let tn = name.unwrap_or_else(|| format!("{}", tid));
                map.insert(*tid, tn);
            }
        }
        Ok(rows.into_iter().map(|q| {
            let tn = map.get(&q.tenant_id).cloned().unwrap_or_else(|| format!("{}", q.tenant_id));
            (q, tn)
        }).collect())
    }

    // ===== ivr_menus =====

    pub async fn list_ivr_menus(
        &self, enabled: Option<bool>, limit: i64, offset: i64,
    ) -> Result<(Vec<IvrMenu>, i64), sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok((Vec::new(), 0)); };
        let backend = self.backend();
        let limit = limit.clamp(1, 500);
        let offset = offset.max(0);
        let (where_sql, binds) = match enabled {
            Some(v) => ("WHERE enabled = $1".to_string(),
                        vec![if v { "1".to_string() } else { "0".to_string() }]),
            None => (String::new(), Vec::new()),
        };
        let count_sql = fix_ph(&format!("SELECT COUNT(*) FROM ivr_menus {where_sql}"), backend);
        let mut cq = sqlx::query_scalar::<_, i64>(&count_sql);
        for b in &binds { cq = cq.bind(b.as_str()); }
        let total = cq.fetch_one(pool).await.unwrap_or(0);
        let rows_sql = fix_ph(&format!("SELECT * FROM ivr_menus {where_sql} ORDER BY name LIMIT ${} OFFSET ${}",
            binds.len() + 1, binds.len() + 2), backend);
        let mut q = sqlx::query_as::<_, IvrMenu>(&rows_sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(limit).bind(offset);
        let rows = q.fetch_all(pool).await?;
        Ok((rows, total))
    }

    pub async fn get_ivr_menu(&self, id: i64) -> Result<Option<IvrMenu>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(None); };
        let backend = self.backend();
        sqlx::query_as::<_, IvrMenu>(&fix_ph("SELECT * FROM ivr_menus WHERE id = $1", backend))
            .bind(id).fetch_optional(pool).await
    }

    pub async fn create_ivr_menu(&self, c: IvrMenuCreate) -> Result<IvrMenu, sqlx::Error> {
        let pool = self.pool().ok_or(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()))?;
        let backend = self.backend();
        let is_mysql = backend == "mysql";
        let opts = if c.options_json.is_empty() { "[]".to_string() } else { c.options_json };
        let raw_sql = "INSERT INTO ivr_menus
                (name, enabled, prompt_audio, invalid_audio, timeout_sec, max_attempts,
                 options_json, timeout_target, invalid_target, description)
             VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)
             RETURNING *";
        if is_mysql {
            let plain_sql = strip_returning(raw_sql);
            let sql = fix_ph(&plain_sql, backend);
            let res = sqlx::query(&sql)
                .bind(&c.name).bind(c.enabled as i64)
                .bind(&c.prompt_audio).bind(&c.invalid_audio)
                .bind(c.timeout_sec).bind(c.max_attempts)
                .bind(&opts).bind(&c.timeout_target).bind(&c.invalid_target)
                .bind(&c.description)
                .execute(pool).await?;
            let id = res.last_insert_id();
            let pick_sql = fix_ph("SELECT * FROM ivr_menus WHERE id = ?", backend);
            return sqlx::query_as::<_, IvrMenu>(&pick_sql).bind(id).fetch_one(pool).await;
        }
        let sql = fix_ph(raw_sql, backend);
        sqlx::query_as::<_, IvrMenu>(&sql)
        .bind(&c.name).bind(c.enabled as i64)
        .bind(&c.prompt_audio).bind(&c.invalid_audio)
        .bind(c.timeout_sec).bind(c.max_attempts)
        .bind(&opts).bind(&c.timeout_target).bind(&c.invalid_target)
        .bind(&c.description)
        .fetch_one(pool).await
    }

    pub async fn update_ivr_menu(&self, id: i64, u: IvrMenuUpdate) -> Result<IvrMenu, sqlx::Error> {
        let pool = self.pool().ok_or(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()))?;
        let backend = self.backend();
        let is_mysql = backend == "mysql";
        let mut sets: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();
        let mut idx: u32 = 0;

        macro_rules! set_siv {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = ${idx}", $col)); binds.push(v); }
            }};
        }
        macro_rules! set_iiv {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = CAST(${idx} AS INTEGER)", $col)); binds.push(v.to_string()); }
            }};
        }
        macro_rules! set_biv {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { idx += 1; sets.push(format!("{} = CAST(${idx} AS INTEGER)", $col)); binds.push(if v { "1".into() } else { "0".into() }); }
            }};
        }

        set_biv!("enabled", u.enabled);
        set_siv!("prompt_audio", u.prompt_audio.clone());
        set_siv!("invalid_audio", u.invalid_audio.clone());
        set_iiv!("timeout_sec", u.timeout_sec);
        set_iiv!("max_attempts", u.max_attempts);
        set_siv!("options_json", u.options_json.clone());
        set_siv!("timeout_target", u.timeout_target.clone());
        set_siv!("invalid_target", u.invalid_target.clone());
        set_siv!("description", u.description.clone());

        if sets.is_empty() {
            return self.get_ivr_menu(id).await.and_then(|r| r.ok_or_else(|| sqlx::Error::RowNotFound));
        }
        let ts = match backend { "sqlite" => "datetime('now')", "postgres" => "CAST(NOW() AS TEXT)", _ => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)" };
        sets.push(format!("updated_at = {ts}"));
        let where_idx = idx + 1;
        let raw = format!("UPDATE ivr_menus SET {} WHERE id = ${where_idx} RETURNING *", sets.join(", "));
        if is_mysql {
            let plain_sql = strip_returning(&raw);
            let sql = fix_ph(&plain_sql, backend);
            let mut q = sqlx::query(&sql);
            for b in &binds { q = q.bind(b.as_str()); }
            q = q.bind(id);
            q.execute(pool).await?;
            let pick_sql = fix_ph("SELECT * FROM ivr_menus WHERE id = ?", backend);
            return sqlx::query_as::<_, IvrMenu>(&pick_sql).bind(id).fetch_one(pool).await;
        }
        let sql = fix_ph(&raw, backend);
        let mut q = sqlx::query_as::<_, IvrMenu>(&sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(id);
        q.fetch_one(pool).await
    }

    pub async fn delete_ivr_menu(&self, id: i64) -> Result<bool, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(false); };
        let backend = self.backend();
        let res = sqlx::query(&fix_ph("DELETE FROM ivr_menus WHERE id = $1", backend)).bind(id).execute(pool).await?;
        Ok(res.rows_affected() > 0)
    }

    pub async fn list_enabled_ivr_menus(&self) -> Result<Vec<IvrMenu>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(Vec::new()); };
        sqlx::query_as::<_, IvrMenu>("SELECT * FROM ivr_menus WHERE enabled = 1 ORDER BY name")
            .fetch_all(pool).await
    }

    // ===== sip_profiles =====

    pub async fn list_sip_profiles(
        &self, enabled: Option<bool>, limit: i64, offset: i64,
    ) -> Result<(Vec<SipProfile>, i64), sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok((Vec::new(), 0)); };
        let backend = self.backend();
        let (rows, total) = if let Some(on) = enabled {
            let on_i = if on { 1i64 } else { 0 };
            let rows = sqlx::query_as::<_, SipProfile>(&fix_ph(
                "SELECT * FROM sip_profiles WHERE enabled = ? ORDER BY name LIMIT ? OFFSET ?", backend
            )).bind(on_i).bind(limit).bind(offset).fetch_all(pool).await?;
            let total: (i64,) = sqlx::query_as(&fix_ph(
                "SELECT COUNT(*) FROM sip_profiles WHERE enabled = ?", backend
            )).bind(on_i).fetch_one(pool).await?;
            (rows, total.0)
        } else {
            let rows = sqlx::query_as::<_, SipProfile>(&fix_ph(
                "SELECT * FROM sip_profiles ORDER BY name LIMIT ? OFFSET ?", backend
            )).bind(limit).bind(offset).fetch_all(pool).await?;
            let total: (i64,) = sqlx::query_as("SELECT COUNT(*) FROM sip_profiles").fetch_one(pool).await?;
            (rows, total.0)
        };
        Ok((rows, total))
    }

    pub async fn get_sip_profile(&self, id: i64) -> Result<Option<SipProfile>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(None); };
        let backend = self.backend();
        sqlx::query_as::<_, SipProfile>(&fix_ph(
            "SELECT * FROM sip_profiles WHERE id = ?", backend
        )).bind(id).fetch_optional(pool).await
    }

    pub async fn create_sip_profile(
        &self, c: SipProfileCreate,
    ) -> Result<SipProfile, sqlx::Error> {
        let Some(pool) = self.pool() else {
            return Err(sqlx::Error::AnyDriverError("DB not configured".into()));
        };
        let backend = self.backend();
        let default = SipProfileCreate {
            name: c.name,
            enabled: c.enabled.or(Some(true)),
            sip_ip: c.sip_ip.or_else(|| Some("0.0.0.0".into())),
            external_sip_ip: c.external_sip_ip.or_else(|| Some("".into())),
            rtp_ip: c.rtp_ip.or_else(|| Some("0.0.0.0".into())),
            external_rtp_ip: c.external_rtp_ip.or_else(|| Some("".into())),
            sip_udp_port: c.sip_udp_port.or(Some(5060)),
            sip_tcp_port: c.sip_tcp_port.or(Some(5060)),
            sip_tls_port: c.sip_tls_port.or(Some(5061)),
            enable_udp: c.enable_udp.or(Some(true)),
            enable_tcp: c.enable_tcp.or(Some(true)),
            enable_tls: c.enable_tls.or(Some(false)),
            rtp_port_start: c.rtp_port_start.or(Some(10000)),
            rtp_port_end: c.rtp_port_end.or(Some(59999)),
            enable_prack: c.enable_prack.or(Some(false)),
            enable_update: c.enable_update.or(Some(true)),
            dtmf_mode: c.dtmf_mode.or_else(|| Some("rfc4733".into())),
            realm: c.realm.or_else(|| Some("nsbc.local".into())),
            auth_calls: c.auth_calls.or(Some(true)),
            ip_whitelist: c.ip_whitelist.or_else(|| Some("".into())),
            ip_blacklist: c.ip_blacklist.or_else(|| Some("".into())),
            description: c.description.or_else(|| Some("".into())),
        };
        let ts = match backend { "postgres" => "CAST(NOW() AS TEXT)", "mysql" => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)", _ => "datetime('now')" };
        let sql = fix_ph(&format!(
            "INSERT INTO sip_profiles \
             (name, enabled, sip_ip, external_sip_ip, rtp_ip, external_rtp_ip, \
              sip_udp_port, sip_tcp_port, sip_tls_port, enable_udp, enable_tcp, enable_tls, \
              rtp_port_start, rtp_port_end, enable_prack, enable_update, dtmf_mode, realm, \
              auth_calls, ip_whitelist, ip_blacklist, description, created_at, updated_at) \
             VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, {ts}, {ts})"
        ), backend);
        let res = sqlx::query(&sql)
            .bind(&default.name)
            .bind(i64::from(default.enabled.unwrap_or(true)))
            .bind(&default.sip_ip.unwrap())
            .bind(&default.external_sip_ip.unwrap())
            .bind(&default.rtp_ip.unwrap())
            .bind(&default.external_rtp_ip.unwrap())
            .bind(default.sip_udp_port.unwrap_or(5060))
            .bind(default.sip_tcp_port.unwrap_or(5060))
            .bind(default.sip_tls_port.unwrap_or(5061))
            .bind(i64::from(default.enable_udp.unwrap_or(true)))
            .bind(i64::from(default.enable_tcp.unwrap_or(true)))
            .bind(i64::from(default.enable_tls.unwrap_or(false)))
            .bind(default.rtp_port_start.unwrap_or(10000))
            .bind(default.rtp_port_end.unwrap_or(59999))
            .bind(i64::from(default.enable_prack.unwrap_or(false)))
            .bind(i64::from(default.enable_update.unwrap_or(true)))
            .bind(&default.dtmf_mode.unwrap())
            .bind(&default.realm.unwrap())
            .bind(i64::from(default.auth_calls.unwrap_or(true)))
            .bind(&default.ip_whitelist.unwrap())
            .bind(&default.ip_blacklist.unwrap())
            .bind(&default.description.unwrap())
            .execute(pool).await?;
        let id = res.last_insert_id();
        sqlx::query_as::<_, SipProfile>(&fix_ph(
            "SELECT * FROM sip_profiles WHERE id = ?", backend
        )).bind(id).fetch_one(pool).await
    }

    pub async fn update_sip_profile(
        &self, id: i64, u: SipProfileUpdate,
    ) -> Result<SipProfile, sqlx::Error> {
        let Some(pool) = self.pool() else {
            return Err(sqlx::Error::Configuration("DB not configured".into()));
        };
        let backend = self.backend();
        let ts = match backend { "postgres" => "CAST(NOW() AS TEXT)", "mysql" => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)", _ => "datetime('now')" };
        let mut sets: Vec<String> = Vec::new();
        let mut binds: Vec<String> = Vec::new();

        macro_rules! set_s {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { sets.push(format!("{col} = ?", col=$col)); binds.push(v); }
            }};
        }
        macro_rules! set_i {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { sets.push(format!("{col} = CAST(? AS INTEGER)", col=$col)); binds.push(v.to_string()); }
            }};
        }
        macro_rules! set_b {
            ($col:expr, $val:expr) => {{
                if let Some(v) = $val { sets.push(format!("{col} = CAST(? AS INTEGER)", col=$col)); binds.push(if v { "1".into() } else { "0".into() }); }
            }};
        }

        set_b!("enabled", u.enabled);
        set_s!("sip_ip", u.sip_ip);
        set_s!("external_sip_ip", u.external_sip_ip);
        set_s!("rtp_ip", u.rtp_ip);
        set_s!("external_rtp_ip", u.external_rtp_ip);
        set_i!("sip_udp_port", u.sip_udp_port);
        set_i!("sip_tcp_port", u.sip_tcp_port);
        set_i!("sip_tls_port", u.sip_tls_port);
        set_b!("enable_udp", u.enable_udp);
        set_b!("enable_tcp", u.enable_tcp);
        set_b!("enable_tls", u.enable_tls);
        set_i!("rtp_port_start", u.rtp_port_start);
        set_i!("rtp_port_end", u.rtp_port_end);
        set_b!("enable_prack", u.enable_prack);
        set_b!("enable_update", u.enable_update);
        set_s!("dtmf_mode", u.dtmf_mode);
        set_s!("realm", u.realm);
        set_b!("auth_calls", u.auth_calls);
        set_s!("ip_whitelist", u.ip_whitelist);
        set_s!("ip_blacklist", u.ip_blacklist);
        set_s!("description", u.description);

        if sets.is_empty() {
            return self.get_sip_profile(id).await.and_then(|r| r.ok_or(sqlx::Error::RowNotFound));
        }
        sets.push(format!("updated_at = {ts}"));
        let raw = format!("UPDATE sip_profiles SET {} WHERE id = ? RETURNING *", sets.join(", "));
        let is_mysql = backend == "mysql";
        if is_mysql {
            let plain_sql = strip_returning(&raw);
            let sql = fix_ph(&plain_sql, backend);
            let mut q = sqlx::query(&sql);
            for b in &binds { q = q.bind(b.as_str()); }
            q = q.bind(id);
            q.execute(pool).await?;
            let pick_sql = fix_ph("SELECT * FROM sip_profiles WHERE id = ?", backend);
            return sqlx::query_as::<_, SipProfile>(&pick_sql).bind(id).fetch_one(pool).await;
        }
        let sql = fix_ph(&raw, backend);
        let mut q = sqlx::query_as::<_, SipProfile>(&sql);
        for b in &binds { q = q.bind(b.as_str()); }
        q = q.bind(id);
        q.fetch_one(pool).await
    }

    pub async fn delete_sip_profile(&self, id: i64) -> Result<bool, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(false); };
        let backend = self.backend();
        let res = sqlx::query(&fix_ph(
            "DELETE FROM sip_profiles WHERE id = ?", backend
        )).bind(id).execute(pool).await?;
        Ok(res.rows_affected() > 0)
    }

    pub async fn list_enabled_sip_profiles(&self) -> Result<Vec<SipProfile>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(Vec::new()); };
        sqlx::query_as::<_, SipProfile>("SELECT * FROM sip_profiles WHERE enabled = 1 ORDER BY name")
            .fetch_all(pool).await
    }

// ===== 费率 CRUD =====

pub async fn list_rates(
    &self, prefix: Option<&str>, enabled: Option<bool>,
    limit: i64, offset: i64,
) -> Result<(Vec<RateTable>, i64), sqlx::Error> {
    let Some(pool) = self.pool() else { return Ok((Vec::new(), 0)); };
    let backend = self.backend();
    let mut binds: Vec<String> = Vec::new();
    let mut where_parts: Vec<String> = Vec::new();
    if let Some(p) = prefix {
        where_parts.push(format!("prefix LIKE ${}", binds.len() + 1));
        binds.push(format!("%{}%", p));
    }
    if let Some(e) = enabled {
        where_parts.push(format!("enabled = ${}", binds.len() + 1));
        binds.push((if e { "1" } else { "0" }).to_string());
    }
    let where_sql = if where_parts.is_empty() { String::new() } else { format!("WHERE {}", where_parts.join(" AND ")) };
    let count_sql = fix_ph(&format!("SELECT COUNT(*) FROM rate_tables {}", where_sql), backend);
    let rows_sql = fix_ph(&format!("SELECT * FROM rate_tables {} ORDER BY prefix LIMIT ${} OFFSET ${}",
        where_sql, binds.len() + 1, binds.len() + 2), backend);
    let mut cq = sqlx::query_scalar::<_, i64>(&count_sql);
    for b in &binds { cq = cq.bind(b.as_str()); }
    let total = cq.fetch_one(pool).await.unwrap_or(0);
    let mut q = sqlx::query_as::<_, RateTable>(&rows_sql);
    for b in &binds { q = q.bind(b.as_str()); }
    q = q.bind(limit).bind(offset);
    let rows = q.fetch_all(pool).await?;
    Ok((rows, total))
}

pub async fn get_rate(&self, id: i64) -> Result<Option<RateTable>, sqlx::Error> {
    let Some(pool) = self.pool() else { return Ok(None); };
    let backend = self.backend();
    sqlx::query_as::<_, RateTable>(&fix_ph("SELECT * FROM rate_tables WHERE id = $1", backend))
        .bind(id).fetch_optional(pool).await
}

pub async fn create_rate(&self, c: RateTableCreate) -> Result<RateTable, sqlx::Error> {
    let Some(pool) = self.pool() else {
        return Err(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()));
    };
    let backend = self.backend();
    let sql_with_ret = "INSERT INTO rate_tables (prefix, name, price_per_min_uc, connect_fee_uc, bill_inc_s, min_bill_s, initial_s, enabled, description) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9) RETURNING *";
    let is_mysql = backend == "mysql";
    if is_mysql {
        let plain_sql = strip_returning(sql_with_ret);
        let sql = fix_ph(&plain_sql, backend);
        sqlx::query(&sql)
            .bind(&c.prefix)
            .bind(&c.name)
            .bind(c.price_per_min_uc)
            .bind(c.connect_fee_uc)
            .bind(c.bill_inc_s)
            .bind(c.min_bill_s)
            .bind(c.initial_s)
            .bind(c.enabled as i64)
            .bind(&c.description)
            .execute(pool).await?;
        let pick_sql = fix_ph("SELECT * FROM rate_tables WHERE id = LAST_INSERT_ID()", backend);
        return sqlx::query_as::<_, RateTable>(&pick_sql).fetch_one(pool).await;
    }
    sqlx::query_as::<_, RateTable>(&fix_ph(sql_with_ret, backend))
    .bind(&c.prefix)
    .bind(&c.name)
    .bind(c.price_per_min_uc)
    .bind(c.connect_fee_uc)
    .bind(c.bill_inc_s)
    .bind(c.min_bill_s)
    .bind(c.initial_s)
    .bind(c.enabled as i64)
    .bind(&c.description)
    .fetch_one(pool).await
}

pub async fn update_rate(&self, id: i64, u: RateTableUpdate) -> Result<RateTable, sqlx::Error> {
    let Some(pool) = self.pool() else { return Err(sqlx::Error::RowNotFound); };
    let backend = self.backend();
    let mut binds: Vec<(&str, String)> = Vec::new();
    if let Some(ref v) = u.prefix { binds.push(("prefix", v.clone())); }
    if let Some(ref v) = u.name { binds.push(("name", v.clone())); }
    if let Some(v) = u.price_per_min_uc { binds.push(("price_per_min_uc", v.to_string())); }
    if let Some(v) = u.connect_fee_uc { binds.push(("connect_fee_uc", v.to_string())); }
    if let Some(v) = u.bill_inc_s { binds.push(("bill_inc_s", v.to_string())); }
    if let Some(v) = u.min_bill_s { binds.push(("min_bill_s", v.to_string())); }
    if let Some(v) = u.initial_s { binds.push(("initial_s", v.to_string())); }
    if let Some(v) = u.enabled { binds.push(("enabled", (v as i64).to_string())); }
    if let Some(ref v) = u.description { binds.push(("description", v.clone())); }
    if binds.is_empty() { return Err(sqlx::Error::RowNotFound); }
    // 按列类型区分：整数列要 CAST（pg 严格类型不接受 text 参数赋整数列），文本列直接 $n
    let sets: Vec<String> = binds.iter().enumerate().map(|(i, (k, _))| {
        if matches!(*k, "enabled" | "price_per_min_uc" | "connect_fee_uc" | "bill_inc_s" | "min_bill_s" | "initial_s") {
            format!("{} = CAST(${} AS INTEGER)", k, i + 1)
        } else {
            format!("{} = ${}", k, i + 1)
        }
    }).collect();
    let ts = match backend { "postgres" => "CAST(NOW() AS TEXT)", "mysql" => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)", _ => "datetime('now')" };
    let raw_sql = format!("UPDATE rate_tables SET {}, updated_at = {ts} WHERE id = ${} RETURNING *",
        sets.join(", "), binds.len() + 1);
    let is_mysql = backend == "mysql";
    if is_mysql {
        let plain_sql = fix_ph(&strip_returning(&raw_sql), backend);
        let mut q = sqlx::query(&plain_sql);
        for (_, v) in &binds { q = q.bind(v.as_str()); }
        q = q.bind(id);
        q.execute(pool).await?;
        let pick_sql = fix_ph("SELECT * FROM rate_tables WHERE id = $1", backend);
        return sqlx::query_as::<_, RateTable>(&pick_sql).bind(id).fetch_one(pool).await;
    }
    let sql = fix_ph(&raw_sql, backend);
    let mut q = sqlx::query_as::<_, RateTable>(&sql);
    for (_, v) in &binds { q = q.bind(v.as_str()); }
    q = q.bind(id);
    q.fetch_one(pool).await
}

pub async fn delete_rate(&self, id: i64) -> Result<bool, sqlx::Error> {
    let Some(pool) = self.pool() else { return Ok(false); };
    let backend = self.backend();
    let res = sqlx::query(&fix_ph("DELETE FROM rate_tables WHERE id = $1", backend)).bind(id).execute(pool).await?;
    Ok(res.rows_affected() > 0)
}

pub async fn list_enabled_rates(&self) -> Result<Vec<RateTable>, sqlx::Error> {
    let Some(pool) = self.pool() else { return Ok(Vec::new()); };
    let backend = self.backend();
    sqlx::query_as::<_, RateTable>(&fix_ph("SELECT * FROM rate_tables WHERE enabled = 1 ORDER BY prefix", backend))
        .fetch_all(pool).await
}

// ===== 安全围栏 CRUD（NIP 三层 + deny/allow 双模式） =====

pub async fn list_security_blacklist(
    &self, kind: Option<&str>, rule_type: Option<&str>, enabled: Option<bool>,
    limit: i64, offset: i64,
) -> Result<(Vec<SecurityBlacklist>, i64), sqlx::Error> {
    let Some(pool) = self.pool() else { return Ok((Vec::new(), 0)); };
    let backend = self.backend();
    let mut binds: Vec<String> = Vec::new();
    let mut where_parts: Vec<String> = Vec::new();
    if let Some(k) = kind {
        where_parts.push(format!("kind = ${}", binds.len() + 1));
        binds.push(k.to_string());
    }
    if let Some(t) = rule_type {
        where_parts.push(format!("rule_type = ${}", binds.len() + 1));
        binds.push(t.to_string());
    }
    if let Some(e) = enabled {
        where_parts.push(format!("enabled = ${}", binds.len() + 1));
        binds.push((if e { "1" } else { "0" }).to_string());
    }
    let where_sql = if where_parts.is_empty() { String::new() } else { format!("WHERE {}", where_parts.join(" AND ")) };
    let count_sql = fix_ph(&format!("SELECT COUNT(*) FROM security_blacklist {}", where_sql), backend);
    let rows_sql = fix_ph(&format!(
        "SELECT * FROM security_blacklist {} ORDER BY kind, rule_type, id LIMIT ${} OFFSET ${}",
        where_sql, binds.len() + 1, binds.len() + 2), backend);
    let mut cq = sqlx::query_scalar::<_, i64>(&count_sql);
    for b in &binds { cq = cq.bind(b.as_str()); }
    let total = cq.fetch_one(pool).await.unwrap_or(0);
    let mut q = sqlx::query_as::<_, SecurityBlacklist>(&rows_sql);
    for b in &binds { q = q.bind(b.as_str()); }
    q = q.bind(limit).bind(offset);
    let rows = q.fetch_all(pool).await?;
    Ok((rows, total))
}

pub async fn get_security_blacklist(&self, id: i64) -> Result<Option<SecurityBlacklist>, sqlx::Error> {
    let Some(pool) = self.pool() else { return Ok(None); };
    let backend = self.backend();
    sqlx::query_as::<_, SecurityBlacklist>(&fix_ph("SELECT * FROM security_blacklist WHERE id = $1", backend))
        .bind(id).fetch_optional(pool).await
}

pub async fn create_security_blacklist(&self, c: SecurityBlacklistCreate) -> Result<SecurityBlacklist, sqlx::Error> {
    let Some(pool) = self.pool() else {
        return Err(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()));
    };
    let backend = self.backend();
    let sql_with_ret = "INSERT INTO security_blacklist (kind, rule_type, value, comment, enabled) VALUES ($1, $2, $3, $4, $5) RETURNING *";
    let is_mysql = backend == "mysql";
    if is_mysql {
        let plain_sql = strip_returning(sql_with_ret);
        let sql = fix_ph(&plain_sql, backend);
        sqlx::query(&sql)
            .bind(&c.kind)
            .bind(&c.rule_type)
            .bind(&c.value)
            .bind(&c.comment)
            .bind(c.enabled as i64)
            .execute(pool).await?;
        let pick_sql = fix_ph("SELECT * FROM security_blacklist WHERE id = LAST_INSERT_ID()", backend);
        return sqlx::query_as::<_, SecurityBlacklist>(&pick_sql).fetch_one(pool).await;
    }
    sqlx::query_as::<_, SecurityBlacklist>(&fix_ph(sql_with_ret, backend))
        .bind(&c.kind)
        .bind(&c.rule_type)
        .bind(&c.value)
        .bind(&c.comment)
        .bind(c.enabled as i64)
        .fetch_one(pool).await
}

pub async fn update_security_blacklist(&self, id: i64, u: SecurityBlacklistUpdate) -> Result<SecurityBlacklist, sqlx::Error> {
    let Some(pool) = self.pool() else { return Err(sqlx::Error::RowNotFound); };
    let backend = self.backend();
    let mut binds: Vec<(&str, String)> = Vec::new();
    if let Some(ref v) = u.comment { binds.push(("comment", v.clone())); }
    if let Some(v) = u.enabled { binds.push(("enabled", (v as i64).to_string())); }
    if binds.is_empty() { return Err(sqlx::Error::RowNotFound); }
    let sets: Vec<String> = binds.iter().enumerate().map(|(i, (k, _))| {
        if *k == "enabled" {
            format!("{} = CAST(${} AS INTEGER)", k, i + 1)  // pg 严格类型
        } else {
            format!("{} = ${}", k, i + 1)
        }
    }).collect();
    let ts = match backend { "postgres" => "CAST(NOW() AS TEXT)", "mysql" => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)", _ => "datetime('now')" };
    let raw_sql = format!("UPDATE security_blacklist SET {}, updated_at = {ts} WHERE id = ${} RETURNING *",
        sets.join(", "), binds.len() + 1);
    let is_mysql = backend == "mysql";
    if is_mysql {
        let plain_sql = fix_ph(&strip_returning(&raw_sql), backend);
        let mut q = sqlx::query(&plain_sql);
        for (_, v) in &binds { q = q.bind(v.as_str()); }
        q = q.bind(id);
        q.execute(pool).await?;
        let pick_sql = fix_ph("SELECT * FROM security_blacklist WHERE id = $1", backend);
        return sqlx::query_as::<_, SecurityBlacklist>(&pick_sql).bind(id).fetch_one(pool).await;
    }
    let sql = fix_ph(&raw_sql, backend);
    let mut q = sqlx::query_as::<_, SecurityBlacklist>(&sql);
    for (_, v) in &binds { q = q.bind(v.as_str()); }
    q = q.bind(id);
    q.fetch_one(pool).await
}

pub async fn delete_security_blacklist(&self, id: i64) -> Result<bool, sqlx::Error> {
    let Some(pool) = self.pool() else { return Ok(false); };
    let backend = self.backend();
    let res = sqlx::query(&fix_ph("DELETE FROM security_blacklist WHERE id = $1", backend)).bind(id).execute(pool).await?;
    Ok(res.rows_affected() > 0)
}

/// sync 推送用：全量启用规则（C 侧 security.blacklist.reload 整表替换）
pub async fn list_enabled_security_blacklist(&self) -> Result<Vec<SecurityBlacklist>, sqlx::Error> {
    let Some(pool) = self.pool() else { return Ok(Vec::new()); };
    let backend = self.backend();
    sqlx::query_as::<_, SecurityBlacklist>(&fix_ph(
        "SELECT * FROM security_blacklist WHERE enabled = 1 ORDER BY kind, rule_type, id", backend))
        .fetch_all(pool).await
}

// ===== security_config CRUD =====

/// 读取全局安全配置；无行则返回默认。
pub async fn get_security_config(&self) -> Result<SecurityConfig, sqlx::Error> {
    let Some(pool) = self.pool() else { return Ok(SecurityConfig::default()); };
    let backend = self.backend();
    let row = sqlx::query_as::<_, SecurityConfig>(&fix_ph(
        "SELECT fence_mode, method_blocked, auto_ban, authfail_threshold, authfail_window_s, ban_duration_s, ratelimit_regs, ratelimit_window_s FROM security_config WHERE id = 1", backend))
        .fetch_optional(pool).await?;
    Ok(row.unwrap_or_default())
}

/// upsert：更新 id=1 行；不存在则 INSERT。
pub async fn upsert_security_config(&self, cfg: &SecurityConfig) -> Result<SecurityConfig, sqlx::Error> {
    let Some(pool) = self.pool() else {
        return Err(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()));
    };
    let backend = self.backend();
    let is_mysql = backend == "mysql";
    let upsert_sql = if is_mysql {
        "INSERT INTO security_config (id, fence_mode, method_blocked, auto_ban, authfail_threshold, authfail_window_s, ban_duration_s, ratelimit_regs, ratelimit_window_s) VALUES (1, $1, $2, $3, $4, $5, $6, $7, $8) \
         ON DUPLICATE KEY UPDATE fence_mode=VALUES(fence_mode), method_blocked=VALUES(method_blocked), auto_ban=VALUES(auto_ban), authfail_threshold=VALUES(authfail_threshold), authfail_window_s=VALUES(authfail_window_s), ban_duration_s=VALUES(ban_duration_s), ratelimit_regs=VALUES(ratelimit_regs), ratelimit_window_s=VALUES(ratelimit_window_s)"
    } else if backend == "postgres" {
        "INSERT INTO security_config (id, fence_mode, method_blocked, auto_ban, authfail_threshold, authfail_window_s, ban_duration_s, ratelimit_regs, ratelimit_window_s) VALUES (1, $1, $2, $3, $4, $5, $6, $7, $8) \
         ON CONFLICT (id) DO UPDATE SET fence_mode=EXCLUDED.fence_mode, method_blocked=EXCLUDED.method_blocked, auto_ban=EXCLUDED.auto_ban, authfail_threshold=EXCLUDED.authfail_threshold, authfail_window_s=EXCLUDED.authfail_window_s, ban_duration_s=EXCLUDED.ban_duration_s, ratelimit_regs=EXCLUDED.ratelimit_regs, ratelimit_window_s=EXCLUDED.ratelimit_window_s \
         RETURNING fence_mode, method_blocked, auto_ban, authfail_threshold, authfail_window_s, ban_duration_s, ratelimit_regs, ratelimit_window_s"
    } else {
        "INSERT OR REPLACE INTO security_config (id, fence_mode, method_blocked, auto_ban, authfail_threshold, authfail_window_s, ban_duration_s, ratelimit_regs, ratelimit_window_s) VALUES (1, $1, $2, $3, $4, $5, $6, $7, $8)"
    };
    let ph = fix_ph(upsert_sql, backend);
    let q = sqlx::query(&ph)
        .bind(cfg.fence_mode).bind(cfg.method_blocked)
        .bind(cfg.auto_ban).bind(cfg.authfail_threshold).bind(cfg.authfail_window_s)
        .bind(cfg.ban_duration_s).bind(cfg.ratelimit_regs).bind(cfg.ratelimit_window_s);
    if is_mysql {
        q.execute(pool).await?;
        return self.get_security_config().await;
    }
    if backend == "postgres" {
        return sqlx::query_as::<_, SecurityConfig>(&ph)
            .bind(cfg.fence_mode).bind(cfg.method_blocked)
            .bind(cfg.auto_ban).bind(cfg.authfail_threshold).bind(cfg.authfail_window_s)
            .bind(cfg.ban_duration_s).bind(cfg.ratelimit_regs).bind(cfg.ratelimit_window_s)
            .fetch_one(pool).await;
    }
    sqlx::query(&ph)
        .bind(cfg.fence_mode).bind(cfg.method_blocked)
        .bind(cfg.auto_ban).bind(cfg.authfail_threshold).bind(cfg.authfail_window_s)
        .bind(cfg.ban_duration_s).bind(cfg.ratelimit_regs).bind(cfg.ratelimit_window_s)
        .execute(pool).await?;
    self.get_security_config().await
}

// ===== 账户 CRUD =====

pub async fn list_accounts(
    &self, keyword: Option<&str>, limit: i64, offset: i64,
) -> Result<(Vec<Account>, i64), sqlx::Error> {
    let Some(pool) = self.pool() else { return Ok((Vec::new(), 0)); };
    let backend = self.backend();
    let mut binds: Vec<String> = Vec::new();
    let mut where_sql = String::new();
    if let Some(k) = keyword {
        where_sql = String::from("WHERE account_key LIKE $1");
        binds.push(format!("%{}%", k));
    }
    let count_sql = fix_ph(&format!("SELECT COUNT(*) FROM accounts {}", where_sql), backend);
    let rows_sql = fix_ph(&format!("SELECT * FROM accounts {} ORDER BY account_key LIMIT ${} OFFSET ${}",
        where_sql, binds.len() + 1, binds.len() + 2), backend);
    let mut cq = sqlx::query_scalar::<_, i64>(&count_sql);
    for b in &binds { cq = cq.bind(b.as_str()); }
    let total = cq.fetch_one(pool).await.unwrap_or(0);
    let mut q = sqlx::query_as::<_, Account>(&rows_sql);
    for b in &binds { q = q.bind(b.as_str()); }
    q = q.bind(limit).bind(offset);
    let rows = q.fetch_all(pool).await?;
    Ok((rows, total))
}

pub async fn get_account(&self, id: i64) -> Result<Option<Account>, sqlx::Error> {
    let Some(pool) = self.pool() else { return Ok(None); };
    let backend = self.backend();
    sqlx::query_as::<_, Account>(&fix_ph("SELECT * FROM accounts WHERE id = $1", backend))
        .bind(id).fetch_optional(pool).await
}

pub async fn open_account(&self, c: AccountCreate) -> Result<Account, sqlx::Error> {
    let Some(pool) = self.pool() else {
        return Err(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()));
    };
    let backend = self.backend();
    let sql_with_ret = "INSERT INTO accounts (account_key, balance_uc, credit_limit_uc, description) VALUES ($1, $2, $3, $4) RETURNING *";
    let is_mysql = backend == "mysql";
    if is_mysql {
        let sql = fix_ph(&strip_returning(sql_with_ret), backend);
        sqlx::query(&sql)
            .bind(&c.account_key)
            .bind(c.balance_uc)
            .bind(c.credit_limit_uc)
            .bind(&c.description)
            .execute(pool).await?;
        let pick_sql = fix_ph("SELECT * FROM accounts WHERE id = LAST_INSERT_ID()", backend);
        return sqlx::query_as::<_, Account>(&pick_sql).fetch_one(pool).await;
    }
    sqlx::query_as::<_, Account>(&fix_ph(sql_with_ret, backend))
    .bind(&c.account_key)
    .bind(c.balance_uc)
    .bind(c.credit_limit_uc)
    .bind(&c.description)
    .fetch_one(pool).await
}

pub async fn topup_account(&self, id: i64, delta_uc: i64) -> Result<Account, sqlx::Error> {
    let Some(pool) = self.pool() else { return Err(sqlx::Error::RowNotFound); };
    let backend = self.backend();
    let ts = match backend { "postgres" => "CAST(NOW() AS TEXT)", "mysql" => "CAST(CURRENT_TIMESTAMP(3) AS CHAR)", _ => "datetime('now')" };
    let raw_sql = format!("UPDATE accounts SET balance_uc = balance_uc + $1, updated_at = {ts} WHERE id = $2 RETURNING *");
    let is_mysql = backend == "mysql";
    if is_mysql {
        let plain_sql = fix_ph(&strip_returning(&raw_sql), backend);
        sqlx::query(&plain_sql)
            .bind(delta_uc)
            .bind(id)
            .execute(pool).await?;
        let pick_sql = fix_ph("SELECT * FROM accounts WHERE id = $1", backend);
        return sqlx::query_as::<_, Account>(&pick_sql).bind(id).fetch_one(pool).await;
    }
    let sql = fix_ph(&raw_sql, backend);
    sqlx::query_as::<_, Account>(&sql)
    .bind(delta_uc)
    .bind(id)
    .fetch_one(pool).await
}

pub async fn delete_account(&self, id: i64) -> Result<bool, sqlx::Error> {
    let Some(pool) = self.pool() else { return Ok(false); };
    let backend = self.backend();
    let res = sqlx::query(&fix_ph("DELETE FROM accounts WHERE id = $1", backend)).bind(id).execute(pool).await?;
    Ok(res.rows_affected() > 0)
}

} // ← 关闭 impl Db ④（routes + accounts 模块）

// ==================== 租户（REQ-TENANT-001） ====================

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize, sqlx::FromRow)]
pub struct Tenant {
    pub id:             i64,
    pub tenant_name:    String,
    pub description:    String,
    pub enabled:        i64,
    pub match_domain:   String,
    pub match_did_prefix: String,
    pub match_trunk:    String,
    pub created_at:     String,
    pub updated_at:     String,
}

#[derive(Debug, Clone, serde::Deserialize)]
pub struct TenantCreate {
    pub tenant_name:    String,
    #[serde(default)] pub description: String,
    #[serde(default = "default_true_i64")] pub enabled: i64,
    #[serde(default)] pub match_domain: String,
    #[serde(default)] pub match_did_prefix: String,
    #[serde(default)] pub match_trunk: String,
}
fn default_true_i64() -> i64 { 1 }

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct TenantUpdate {
    pub tenant_name:      Option<String>,
    pub description:      Option<String>,
    pub enabled:          Option<bool>,
    pub match_domain:     Option<String>,
    pub match_did_prefix: Option<String>,
    pub match_trunk:      Option<String>,
}

impl Db {
    pub async fn list_tenants(&self) -> Result<Vec<Tenant>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(Vec::new()); };
        let backend = self.backend();
        sqlx::query_as::<_, Tenant>(&fix_ph("SELECT * FROM tenants ORDER BY id", backend))
            .fetch_all(pool).await
    }

    pub async fn get_tenant(&self, id: i64) -> Result<Option<Tenant>, sqlx::Error> {
        let Some(pool) = self.pool() else { return Ok(None); };
        let backend = self.backend();
        sqlx::query_as::<_, Tenant>(&fix_ph("SELECT * FROM tenants WHERE id = $1", backend))
            .bind(id).fetch_optional(pool).await
    }

    pub async fn create_tenant(&self, c: TenantCreate) -> Result<Tenant, sqlx::Error> {
        let Some(pool) = self.pool() else {
            return Err(sqlx::Error::Configuration("NSBC_DB_DSN not set".into()));
        };
        let backend = self.backend();
        let sql_with_ret = "INSERT INTO tenants (tenant_name, description, enabled, match_domain, match_did_prefix, match_trunk) VALUES ($1, $2, $3, $4, $5, $6) RETURNING *";
        let is_mysql = backend == "mysql";
        if is_mysql {
            let sql = fix_ph(&strip_returning(sql_with_ret), backend);
            sqlx::query(&sql)
                .bind(&c.tenant_name)
                .bind(&c.description)
                .bind(c.enabled)
                .bind(&c.match_domain)
                .bind(&c.match_did_prefix)
                .bind(&c.match_trunk)
                .execute(pool).await?;
            let pick_sql = fix_ph("SELECT * FROM tenants WHERE id = LAST_INSERT_ID()", backend);
            return sqlx::query_as::<_, Tenant>(&pick_sql).fetch_one(pool).await;
        }
        sqlx::query_as::<_, Tenant>(&fix_ph(sql_with_ret, backend))
        .bind(&c.tenant_name)
        .bind(&c.description)
        .bind(c.enabled)
        .bind(&c.match_domain)
        .bind(&c.match_did_prefix)
        .bind(&c.match_trunk)
        .fetch_one(pool).await
    }

    pub async fn update_tenant(&self, id: i64, u: TenantUpdate) -> Result<Tenant, sqlx::Error> {
        let Some(pool) = self.pool() else { return Err(sqlx::Error::RowNotFound); };
        let backend = self.backend();
        let mut sets: Vec<String> = Vec::new();
        let mut binds: Vec<(&str, String)> = Vec::new();
        let mut idx = 1usize;
        if let Some(v) = u.tenant_name      { sets.push(format!("tenant_name = ${idx}"));       binds.push(("t", v)); idx += 1; }
        if let Some(v) = u.description      { sets.push(format!("description = ${idx}"));       binds.push(("t", v)); idx += 1; }
        if let Some(v) = u.enabled          { sets.push(format!("enabled = ${idx}"));           binds.push(("t", (v as i64).to_string())); idx += 1; }
        if let Some(v) = u.match_domain     { sets.push(format!("match_domain = ${idx}"));      binds.push(("t", v)); idx += 1; }
        if let Some(v) = u.match_did_prefix { sets.push(format!("match_did_prefix = ${idx}"));  binds.push(("t", v)); idx += 1; }
        if let Some(v) = u.match_trunk      { sets.push(format!("match_trunk = ${idx}"));       binds.push(("t", v)); idx += 1; }
        if sets.is_empty() {
            let sql = fix_ph("SELECT * FROM tenants WHERE id = $1", backend);
            return sqlx::query_as::<_, Tenant>(&sql).bind(id).fetch_one(pool).await;
        }
        let ts = match backend {
            "postgres" => "updated_at = NOW()",
            "mysql"    => "updated_at = CURRENT_TIMESTAMP(3)",
            _          => "updated_at = datetime('now')",
        };
        sets.push(ts.into());
        let raw_sql = format!("UPDATE tenants SET {} WHERE id = ${} RETURNING *", sets.join(", "), idx);
        let is_mysql = backend == "mysql";
        if is_mysql {
            let plain_sql = fix_ph(&strip_returning(&raw_sql), backend);
            let mut q = sqlx::query(&plain_sql);
            for (_, v) in &binds { q = q.bind(v.as_str()); }
            q = q.bind(id);
            q.execute(pool).await?;
            let pick_sql = fix_ph("SELECT * FROM tenants WHERE id = $1", backend);
            return sqlx::query_as::<_, Tenant>(&pick_sql).bind(id).fetch_one(pool).await;
        }
        let sql = fix_ph(&raw_sql, backend);
        let mut q = sqlx::query_as::<_, Tenant>(&sql);
        for (_, v) in &binds { q = q.bind(v); }
        q = q.bind(id);
        q.fetch_one(pool).await
    }

    pub async fn delete_tenant(&self, id: i64) -> Result<bool, sqlx::Error> {
        if id == 1 { return Ok(false); } // default tenant 不可删
        let Some(pool) = self.pool() else { return Ok(false); };
        let backend = self.backend();
        let res = sqlx::query(&fix_ph("DELETE FROM tenants WHERE id = $1", backend)).bind(id).execute(pool).await?;
        Ok(res.rows_affected() > 0)
    }
}
