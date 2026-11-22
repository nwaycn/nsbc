//! 首次启动 Web 初始化向导（数据库选择 + 安全材料自动生成）。
//!
//! 触发条件：启动时既没有 `NSBC_DB_DSN`，也没有 `db.dsn`。
//! 该模块只挂载静态页面 + 两个公开端点，不构建 AuthSvc / AppState：
//!   GET  /setup/status  → 前端据此切换到初始化页面
//!   POST /setup         → 建库 + 建表 + 生成 JWT 密钥/管理员密码 + 写回主配置
//! 成功后把 `db.dsn` / `auth.jwt_secret` 写回 `conf/nsbc.yaml`（0600），
//! 并 re-exec 自身进程：新进程读到配置即进入正常业务流程，不产生第二份配置文件。
//!
//! 安全说明：
//! - 只在“完全未配置数据库”的首次启动窗口内可用，配置成功后该路由不再挂载；
//! - 仅允许一次成功写入（成功/失败都记录日志，失败可重试）；
//! - 日志里的 DSN 一律脱敏，管理员密码只在 HTTP 响应中返回一次
//!   （同时写入数据库的是 argon2id PHC，不落明文）。

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use axum::extract::State;
use axum::routing::{get, post};
use axum::{Json, Router};
use serde::Deserialize;
use serde_json::{json, Value};
use tower_http::limit::RequestBodyLimitLayer;

use crate::auth::accounts::hash_password_argon2id;
use crate::db;
use crate::error::{ApiError, ApiResult};

/// 支持的数据库后端。
const BACKENDS: [&str; 3] = ["sqlite", "postgres", "mysql"];

/// 初始化上下文：主配置 YAML 路径 + 默认 SQLite 文件路径。
#[derive(Clone)]
pub struct SetupCtx {
    pub config_file: PathBuf,
    pub sqlite_path: PathBuf,
}

/// 单飞：避免并发请求重复建库/重复写文件。
static BUSY: AtomicBool = AtomicBool::new(false);

pub fn router(ctx: Arc<SetupCtx>) -> Router {
    let app = Router::new()
        .route("/setup/status", get(status))
        .route("/setup", post(create))
        .layer(RequestBodyLimitLayer::new(64 * 1024))
        .with_state(ctx)
        .merge(crate::static_files::router());
    crate::harden(app)
}

async fn status(State(ctx): State<Arc<SetupCtx>>) -> Json<Value> {
    Json(json!({
        "required": true,
        "sqlite_path": ctx.sqlite_path.to_string_lossy(),
        "version": env!("CARGO_PKG_VERSION"),
    }))
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SetupReq {
    pub backend: String,
    #[serde(default)]
    pub host: String,
    #[serde(default)]
    pub port: u16,
    #[serde(default)]
    pub user: String,
    #[serde(default)]
    pub password: String,
    #[serde(default)]
    pub database: String,
}

async fn create(
    State(ctx): State<Arc<SetupCtx>>,
    Json(req): Json<SetupReq>,
) -> ApiResult<Json<Value>> {
    if BUSY.swap(true, Ordering::SeqCst) {
        return Err(ApiError::Conflict("setup already in progress".into()));
    }
    let result = do_create(&ctx, req).await;
    if result.is_err() {
        // 失败允许修正参数后重试
        BUSY.store(false, Ordering::SeqCst);
    }
    result
}

async fn do_create(ctx: &SetupCtx, req: SetupReq) -> ApiResult<Json<Value>> {
    let backend = req.backend.trim().to_ascii_lowercase();
    if !BACKENDS.contains(&backend.as_str()) {
        return Err(ApiError::BadRequest(
            "backend must be sqlite / postgres / mysql".into(),
        ));
    }

    let dsn = build_dsn(&backend, &req, &ctx.sqlite_path)?;
    tracing::info!(backend = %backend, dsn = %mask_dsn(&dsn),
                   "setup: testing database connection");

    let db = db::Db::from_dsn(&dsn).await;
    if db.pool().is_none() {
        return Err(ApiError::BadRequest(format!(
            "无法连接 {backend} 数据库，请检查地址、端口、账号密码与服务状态"
        )));
    }

    // 安全材料全部自动生成，不要求用户填写：
    // JWT 签名密钥 = 32 字节随机数（64 位 hex，写回 nsbc.yaml，重启后旧令牌继续有效）
    let jwt_secret = random_hex(32)?;
    // 初始管理员密码改为随机生成并在响应中一次性返回，避免全网都知道固定 Admin123
    let admin_pw = random_hex(8)?;   // 16 位 hex

    // 已有管理员且口令非空 → 不覆盖，提示沿用原密码
    let existing = db
        .get_web_user("admin")
        .await
        .map_err(|e| ApiError::Db(format!("query web_users failed: {e}")))?;
    let generated = match existing {
        Some(u) if !u.password_phc.is_empty() => false,
        _ => {
            let phc = hash_password_argon2id(&admin_pw)
                .map_err(|e| ApiError::Db(format!("hash admin password failed: {e}")))?;
            db.ensure_default_web_user("admin", "L2", &phc).await;
            match db.get_web_user("admin").await {
                Ok(Some(u)) if !u.password_phc.is_empty() => true,
                _ => return Err(ApiError::Db("写入初始管理员账号失败".into())),
            }
        }
    };

    crate::config::write_config_file(&ctx.config_file, &dsn, &jwt_secret)
        .map_err(|e| ApiError::Db(format!("写回配置文件失败: {e}")))?;
    tracing::warn!(path = %ctx.config_file.display(), backend = %backend,
                   "setup completed; restarting nsbc-web to enter normal mode");

    #[cfg(unix)]
    {
        schedule_reexec();
    }
    #[cfg(not(unix))]
    {
        tracing::warn!("automatic restart is only supported on unix; restart nsbc-web manually");
    }

    let admin_password: Option<String> = if generated { Some(admin_pw) } else { None };
    Ok(Json(json!({
        "ok": true,
        "backend": backend,
        "admin_user": "admin",
        "admin_password": admin_password,
        "restart": cfg!(unix),
    })))
}

/// 构造 DSN：SQLite 用默认数据目录，其它后端用表单字段。
fn build_dsn(backend: &str, req: &SetupReq, sqlite_path: &Path) -> ApiResult<String> {
    match backend {
        "sqlite" => {
            if let Some(parent) = sqlite_path.parent() {
                std::fs::create_dir_all(parent).map_err(|e| {
                    ApiError::BadRequest(format!("创建数据目录失败: {e}"))
                })?;
            }
            let abs = sqlite_path.to_string_lossy();
            // Unix: sqlite:// + /abs/path → sqlite:///abs/path（三斜杠）
            // Windows: sqlite://D:\path（与 config.rs 的相对路径解析结果保持一致）
            Ok(format!("sqlite://{}", abs))
        }
        "postgres" | "mysql" => {
            let (def_port, scheme) = if backend == "postgres" {
                (5432u16, "postgres")
            } else {
                (3306u16, "mysql")
            };
            let host = if req.host.trim().is_empty() { "127.0.0.1" } else { req.host.trim() };
            if !host_ok(host) {
                return Err(ApiError::BadRequest("host 非法".into()));
            }
            let port = if req.port == 0 { def_port } else { req.port };
            let user = if req.user.trim().is_empty() { "nsbc" } else { req.user.trim() };
            let database = if req.database.trim().is_empty() { "nsbc" } else { req.database.trim() };
            if !ident_ok(database) {
                return Err(ApiError::BadRequest("database 名称非法".into()));
            }
            Ok(format!(
                "{}://{}:{}@{}:{}/{}",
                scheme,
                pct(user),
                pct(&req.password),
                host,
                port,
                database
            ))
        }
        _ => Err(ApiError::BadRequest("unsupported backend".into())),
    }
}

fn host_ok(h: &str) -> bool {
    !h.is_empty()
        && h.len() <= 253
        && h.bytes().all(|b| {
            b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-' | b':' | b'[' | b']')
        })
}

fn ident_ok(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 64
        && s.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-' | b'.'))
}

/// URL userinfo 百分号编码（只保留 RFC 3986 unreserved）。
fn pct(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for b in s.bytes() {
        if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'.' | b'_' | b'~') {
            out.push(b as char);
        } else {
            out.push('%');
            out.push_str(&format!("{b:02X}"));
        }
    }
    out
}

fn random_hex(n: usize) -> ApiResult<String> {
    let mut buf = vec![0u8; n];
    getrandom::getrandom(&mut buf).map_err(|e| ApiError::Db(format!("rng failure: {e}")))?;
    let mut out = String::with_capacity(n * 2);
    for b in buf {
        out.push_str(&format!("{b:02x}"));
    }
    Ok(out)
}

/// 日志脱敏：postgres://user:pass@host → postgres://user:****@host
fn mask_dsn(dsn: &str) -> String {
    if let Some(i) = dsn.find("://") {
        let rest = &dsn[i + 3..];
        if let Some(at) = rest.find('@') {
            let user = rest[..at].split(':').next().unwrap_or("");
            return format!("{}://{}:****@{}", &dsn[..i], user, &rest[at + 1..]);
        }
    }
    dsn.to_string()
}

/// 响应发出后 re-exec 自身：配置已写回 YAML，新进程直接进入正常模式。
/// Unix 下 exec 会保留 PID/环境/参数，CLOEXEC 让旧监听套接字自动关闭。
#[cfg(unix)]
fn schedule_reexec() {
    tokio::spawn(async {
        tokio::time::sleep(std::time::Duration::from_millis(800)).await;
        use std::os::unix::process::CommandExt;
        let exe = match std::env::current_exe() {
            Ok(p) => p,
            Err(e) => {
                tracing::error!("re-exec skipped: current_exe failed: {e}");
                return;
            }
        };
        let args: Vec<String> = std::env::args().skip(1).collect();
        tracing::warn!(exe = %exe.display(), ?args, "re-executing nsbc-web");
        let err = std::process::Command::new(exe).args(args).exec();
        tracing::error!("re-exec failed: {err}; restart nsbc-web manually");
    });
}
