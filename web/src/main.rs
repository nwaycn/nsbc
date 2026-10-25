//! NSBC Web management sidecar (REQ-WEB-001~021, P0 skeleton)
//!
//! Copyright 2024-2026 李浩 (Li Hao), 上海宁卫信息技术有限公司
//! Contact: 1354608370@qq.com
//! Licensed under the Apache License, Version 2.0. See LICENSE in project root.
//!
//! Isolation principle (REQ-WEB-001/013): separate process / separate account /
//! separate dependency tree; access to the core is only through the REST API
//! (REQ-OAM-001) and the real-time control protocol (REQ-OAM-004). Faults,
//! jitter and releases do not affect the call plane. The core applies unified
//! rate-limiting / auth / audit for all clients; the sidecar never bypasses
//! permissions (REQ-WEB-020).
//!
//! Security baseline: #![deny(unsafe_code)] (REQ-WEB-011); API version discovery
//! cross-boundary fail-closed (REQ-WEB-020); RBAC L0/L1/L2 is aligned with the
//! core command registry (REQ-WEB-008 / REQ-OAM-008).
#![deny(unsafe_code)]

mod auth;
mod config;
mod coreclient;
mod tls;
mod db;
mod error;
mod middleware;
mod poller;
mod ai_agent;
mod routes;
mod static_files;
mod sync;
mod uds_server;
use std::io::IsTerminal;
use std::io::Write as _;
use std::net::SocketAddr;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use axum::{middleware as axmw, Router};
use axum::http::{HeaderName, HeaderValue};
use tokio::net::TcpListener;
use tokio_rustls::TlsAcceptor;
use rustls::ServerConfig;
use tower_http::limit::RequestBodyLimitLayer;
use tower_http::set_header::SetResponseHeaderLayer;
use tracing_subscriber::EnvFilter;
use tracing_subscriber::fmt::time::FormatTime;

use auth::AuthSvc;
use middleware::{apiversion, ratelimit, rbac};
use coreclient::{CoreApi, RealCore};

/// Local short-format timestamp ("HH:MM:SS.mmm") for JSON logs.
struct LocalShortTimer;

impl FormatTime for LocalShortTimer {
    fn format_time(&self, w: &mut tracing_subscriber::fmt::format::Writer<'_>) -> std::fmt::Result {
        // Only use now_utc() — avoids the local-offset feature pulling in a missing vendor dep
        let now = time::OffsetDateTime::now_utc();
        // Short format: MMDD HH:MM:SS.mmm UTC (17 chars, balances date with readability;
        // UTC makes timezone unambiguous)
        let mo = now.month() as u8;
        let d  = now.day();
        let h  = now.hour();
        let m  = now.minute();
        let s  = now.second();
        let ms = now.nanosecond() / 1_000_000;
        write!(w, "{:02}{:02} {:02}:{:02}:{:02}.{:03}Z", mo, d, h, m, s, ms)
    }
}

/// Writer wrapper: rename tracing-subscriber's JSON layer hard-coded "timestamp" field to "tm".
struct TsRenamingWriter(std::io::Stderr);

impl std::io::Write for TsRenamingWriter {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        static OLD: &[u8] = b"\"timestamp\":\"";
        static NEW: &[u8] = b"\"tm\":\"";
        let mut pos = 0usize;
        let out = &mut self.0;
        while let Some(idx) = buf[pos..].windows(OLD.len()).position(|w| w == OLD) {
            let abs = pos + idx;
            out.write_all(&buf[pos..abs])?;
            out.write_all(NEW)?;
            pos = abs + OLD.len();
        }
        if pos < buf.len() {
            out.write_all(&buf[pos..])?;
        }
        Ok(buf.len())
    }
    fn flush(&mut self) -> std::io::Result<()> { self.0.flush() }
}

/// File version: timestamp → tm, writes to a file.
struct TsRenamingFile(Arc<std::sync::Mutex<std::fs::File>>);

impl std::io::Write for TsRenamingFile {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        static OLD: &[u8] = b"\"timestamp\":\"";
        static NEW: &[u8] = b"\"tm\":\"";
        let mut pos = 0usize;
        let mut out = self.0.lock().unwrap();
        while let Some(idx) = buf[pos..].windows(OLD.len()).position(|w| w == OLD) {
            let abs = pos + idx;
            out.write_all(&buf[pos..abs])?;
            out.write_all(NEW)?;
            pos = abs + OLD.len();
        }
        if pos < buf.len() {
            out.write_all(&buf[pos..])?;
        }
        Ok(buf.len())
    }
    fn flush(&mut self) -> std::io::Result<()> { self.0.lock().unwrap().flush() }
}


/// Client (REQ-WEB-013, the sole channel); tests use a mock, no dyn dispatch or global
/// singletons. Auth domain (accounts/JWT/session state) is in [`AuthSvc`] (REQ-WEB-008).
/// Persistence domain (CDR/audit/config_history) is in [`Db`] (optional, REQ-WEB-013).
#[derive(Clone)]
pub struct AppState<C: CoreApi + Clone> {
    pub core:          C,
    pub auth:          Arc<AuthSvc>,
    pub db:            db::Db,
    pub api_major:     u32,
    pub bind:          SocketAddr,
    pub recording_root: std::path::PathBuf,
    /// REQ-EXT-API-004: true → 即使 127.0.0.1 / ::1 也必须 Bearer Token。
    /// 默认 false（本机脚本零摩擦）；反代部署必须开（否则反代进来的远程请求 peer 就是 loopback）。
    pub require_loopback_auth: bool,
    /// C→Web data.sock channel established (set true on UDS connect to let poller skip
    /// polling). Two tasks (uds_server accept loop + poller interval loop) live across
    /// tokio tasks; AtomicBool provides lock-free coordination.
    pub data_pushed:   Arc<AtomicBool>,
    /// SESSION real-time event broadcast (nsbc-esl-web §8#1): SESSION events pushed via
    /// C→Web data.sock are fan-out'd through this channel to all `/calls/stream` (SSE)
    /// subscribers — front-end uses this to replace 5-second polling with real-time
    /// refresh. Slow consumers are handled by the broadcast ring buffer (cap 256);
    /// lagging is recorded as lagged, the UDS dispatch hot path never blocks (no back-
    /// pressure, matching project constraints).
    pub calls_tx:      tokio::sync::broadcast::Sender<String>,
    /// VAD_EVENT real-time broadcast (REQ-AI-BARGEIN-001): {"kind":"vad_event",
    /// "payload":{event:"VAD_EVENT",...}} pushed by the C core's vad_engine is fan-out'd
    /// through this channel to the ai_agent module for barge-in decisions. Larger
    /// buffer (cap 1024) because VAD frames are high-frequency.
    pub vad_tx:        tokio::sync::broadcast::Sender<String>,
}

pub const API_MAJOR: u32 = 1;

/// /api/v1 business routes (auth/version governance layered at main assembly,
/// REQ-WEB-009/012/013). login/refresh are public endpoints (no rbac); logout and
/// the rest are rbac-protected.
///
/// The rbac middleware needs to read [`AppState`] (JWT/session table), and axum's
/// `from_fn` does not inject State — we must give it with `from_fn_with_state` at
/// assembly time, so this function directly consumes state and returns a finished
/// `Router<()>`.
fn api_router<C: CoreApi + Clone + 'static>(st: Arc<AppState<C>>) -> Router {
    let protected = Router::new()
        .merge(routes::system::router())
        .merge(routes::extensions::router())
        .merge(routes::gateways::router())
        .merge(routes::trunks::router())
        .merge(routes::sip_profiles::router())
        .merge(routes::route_groups::router())
        .merge(routes::dialplan_rules::router())
        .merge(routes::dialplan_apps::router())
        .merge(routes::codecs::router())
        .merge(routes::ha_groups::router())
        .merge(routes::time_groups::router())
        .merge(routes::queues::router())
        .merge(routes::ivr_menus::router())
        .merge(routes::config::router())
        .merge(routes::calls::router())
        .merge(routes::registrations::router())
        .merge(routes::metrics::router())
        .merge(routes::cdrs::router())
        .merge(routes::security::router())
        .merge(routes::recordings::router())
        .merge(routes::recordings::public_file_router(st.clone()))
        .merge(routes::billing::router())
        .merge(routes::transcode::router())
        .merge(routes::tenants::router())
        .merge(routes::auth::protected_router())
        .merge(routes::ext_admin::router())
        .layer(axmw::from_fn_with_state(st.clone(), rbac::require_auth::<C>));
    Router::new()
        .merge(routes::auth::public_router())
        .merge(protected)
        .layer(axmw::from_fn_with_state(st.clone(), ratelimit::enforce::<C>))
        .layer(axmw::from_fn(apiversion::enforce))
        .layer(RequestBodyLimitLayer::new(1024 * 1024))
        .with_state(st)
}

/// REQ-EXT-API-001: External API surface `/ext/v1/*`.
///
/// 和 Web UI 的 `/api/v1/*`（JWT + rbac）**完全隔离** —— 外部系统只需要一个
/// API Key（Bearer `nsbc_live_<32bytes>`）就能完成控制（originate/hangup）、
/// 监控（calls/system）、事件接收（SSE stream）三件事。
/// 走自己的 middleware 链：ext_key（API Key 鉴权）→ ratelimit（限流），
/// **不走** rbac（Web UI JWT）和 apiversion（Web UI 版本治理）。
fn ext_router<C: CoreApi + Clone + 'static>(st: Arc<AppState<C>>) -> Router {
    // 和 api_router 完全相同的内部模式：
    // Router::new() 被 merge 的 Router<Arc<AppState<C>>> 推断成同样的类型
    // layer 注入 middleware（from_fn_with_state 捕获 Arc<AppState<C>>）
    // 最后 .with_state(st) 保持 Router<Arc<AppState<C>>>
    // 函数签名 -> Router（默认 Router<()>）靠隐式转换匹配 api_router 同模式
    Router::new()
        .merge(routes::ext::router::<C>())
        .layer(axmw::from_fn_with_state(st.clone(), middleware::ext_key::require_ext_key::<C>))
        .layer(axmw::from_fn_with_state(st.clone(), middleware::ratelimit::enforce::<C>))
        .with_state(st)
}

/// Full application: /api/v1 business surface + unauthenticated /health probe +
/// security response headers. **Plus /ext/v1 external API surface** (REQ-EXT-API-001):
/// independent from Web UI JWT — uses API Key auth, lets call-centers / CRMs / AI Agents
/// control + monitor + receive events with zero client deployment.
fn app_router<C: CoreApi + Clone + 'static>(state: Arc<AppState<C>>) -> Router {
    // Security response headers layer (OWASP baseline, compatible with Vue 3 compile-time
    // template eval). Vue 3 global build uses new Function() for template compilation →
    // script-src must be 'unsafe-eval'. Cross-Origin-Opener-Policy only takes effect on
    // HTTPS; on HTTP Chrome silently ignores and warns → removed.
    let csp = "default-src 'self'; \
        script-src 'self' 'unsafe-inline' 'unsafe-eval'; \
        style-src 'self' 'unsafe-inline'; \
        img-src 'self' data:; \
        media-src 'self'; \
        font-src 'self'; \
        connect-src 'self' ws: wss: stun: stuns:; \
        frame-ancestors 'none'; \
        base-uri 'self'; \
        form-action 'self'";

    let app = Router::new()
        .nest("/api/v1", api_router(state.clone()))
        .nest("/ext/v1", ext_router(state.clone()))
        .route("/health", axum::routing::get(routes::system::health))
        .merge(static_files::router());

    // Add security headers layer by layer (SetResponseHeaderLayer overwrites duplicates)
    app.layer(SetResponseHeaderLayer::if_not_present(
        HeaderName::from_static("content-security-policy"),
        HeaderValue::from_static(csp),
    ))
    .layer(SetResponseHeaderLayer::if_not_present(
        HeaderName::from_static("x-frame-options"),
        HeaderValue::from_static("DENY"),
    ))
    .layer(SetResponseHeaderLayer::if_not_present(
        HeaderName::from_static("x-content-type-options"),
        HeaderValue::from_static("nosniff"),
    ))
    .layer(SetResponseHeaderLayer::if_not_present(
        HeaderName::from_static("referrer-policy"),
        HeaderValue::from_static("strict-origin-when-cross-origin"),
    ))
    .layer(SetResponseHeaderLayer::if_not_present(
        HeaderName::from_static("permissions-policy"),
        HeaderValue::from_static("camera=(self), microphone=(self), geolocation=(self)"),
    ))
}

/// Phase 4 DB DSN bootstrap (2026-10-15 incremental):
/// Precedence chain: NSBC_DB_DSN env > cfg.db.dsn > interactive wizard > SQLite default
fn resolve_db_dsn(cfg: &config::DbSection) -> String {
    // 1. env override takes highest priority
    if let Ok(v) = std::env::var("NSBC_DB_DSN") {
        tracing::info!("database DSN environment override active");
        return v;
    }
    // 2. explicit config in cfg file
    if !cfg.dsn.is_empty() {
        tracing::info!("using database DSN from config");
        return cfg.dsn.clone();
    }

    // 3. Interactive wizard (when TTY is available)
    let is_tty = std::io::stdin().is_terminal();

    if is_tty {
        tracing::warn!("db.dsn not configured, entering interactive wizard…");
        println!();
        println!("╔══════════════════════════════════════╗");
        println!("║  NSBC Web DB Initialization Wizard (Phase 4) ║");
        println!("╠══════════════════════════════════════╣");
        println!("║  1) SQLite  (lightweight single-node, recommended for PoC) ║");
        println!("║  2) MySQL   (production-grade)                 ║");
        println!("║  3) PostgreSQL (production-grade)              ║");
        println!("║  4) Skip (in-memory only)                      ║");
        println!("╚══════════════════════════════════════╝");
        print!("Select [1/2/3/4] default(1): ");
        std::io::stdout().flush().ok();

        let mut choice = String::new();
        std::io::stdin().read_line(&mut choice).ok();
        let choice = choice.trim();

        match choice {
            "2" => {
                print!("MySQL host [127.0.0.1]: "); std::io::stdout().flush().ok();
                let mut host = String::new();
                std::io::stdin().read_line(&mut host).ok();
                let host = host.trim_or("127.0.0.1");

                print!("MySQL port [3306]: "); std::io::stdout().flush().ok();
                let mut port = String::new();
                std::io::stdin().read_line(&mut port).ok();
                let port = port.trim_or("3306");

                print!("MySQL user [nsbc]: "); std::io::stdout().flush().ok();
                let mut user = String::new();
                std::io::stdin().read_line(&mut user).ok();
                let user = user.trim_or("nsbc");

                print!("MySQL password: "); std::io::stdout().flush().ok();
                let mut pass = String::new();
                std::io::stdin().read_line(&mut pass).ok();
                let pass = pass.trim();

                print!("MySQL database [nsbc]: "); std::io::stdout().flush().ok();
                let mut dbname = String::new();
                std::io::stdin().read_line(&mut dbname).ok();
                let dbname = dbname.trim_or("nsbc");

                let dsn = format!("mysql://{}:{}@{}:{}/{}",
                    user, pass, host, port, dbname);
                tracing::info!("🔧 db: MySQL DSN → {}:{}/{}", host, port, dbname);
                return dsn;
            }
            "3" => {
                print!("PostgreSQL host [127.0.0.1]: "); std::io::stdout().flush().ok();
                let mut host = String::new();
                std::io::stdin().read_line(&mut host).ok();
                let host = host.trim_or("127.0.0.1");

                print!("PostgreSQL port [5432]: "); std::io::stdout().flush().ok();
                let mut port = String::new();
                std::io::stdin().read_line(&mut port).ok();
                let port = port.trim_or("5432");

                print!("PostgreSQL user [nsbc]: "); std::io::stdout().flush().ok();
                let mut user = String::new();
                std::io::stdin().read_line(&mut user).ok();
                let user = user.trim_or("nsbc");

                print!("PostgreSQL password: "); std::io::stdout().flush().ok();
                let mut pass = String::new();
                std::io::stdin().read_line(&mut pass).ok();
                let pass = pass.trim();

                print!("PostgreSQL database [nsbc]: "); std::io::stdout().flush().ok();
                let mut dbname = String::new();
                std::io::stdin().read_line(&mut dbname).ok();
                let dbname = dbname.trim_or("nsbc");

                let dsn = format!("postgres://{}:{}@{}:{}/{}",
                    user, pass, host, port, dbname);
                tracing::info!("🔧 db: PostgreSQL DSN → {}:{}/{}", host, port, dbname);
                return dsn;
            }
            "4" => {
                tracing::info!("🔧 db: wizard skipped — persistence disabled");
                return String::new();
            }
            _ => { /* default SQLite, handled below */ }
        }
    }

    // 4. SQLite default (TTY not available OR user picked 1 OR just hit Enter)
    let db_path = std::path::PathBuf::from("data/web.db");
    if let Some(parent) = db_path.parent() {
        if let Err(e) = std::fs::create_dir_all(parent) {
            eprintln!("warning: create_dir_all {}: {e}", parent.display());
        }
    }
    let dsn = format!("sqlite:{}", db_path.display());
    tracing::info!("using default SQLite database");
    dsn
}

/// Extension trait: trim_or default value filling on Option<String>
trait TrimOr {
    fn trim_or<'a>(&'a self, default: &'a str) -> &'a str;
}
impl TrimOr for String {
    fn trim_or<'a>(&'a self, default: &'a str) -> &'a str {
        let t = self.trim();
        if t.is_empty() { default } else { t }
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // rustls 0.23 requires explicit CryptoProvider install before any TLS operation
    // certon (may pull aws-lc-rs) + us (ring) dual provider together causes rustls to
    // fail auto-selection → panic. Use ring uniformly (matches Cargo.toml ring 0.17 /
    // rcgen(ring) / rustls(ring))
    let _ = rustls::crypto::ring::default_provider().install_default();

    // Sub-commands first (hash-password does not load any Web domain)
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).map(String::as_str) == Some("hash-password") {
        run_hash_password(&args[2..]); // returns !
    }

    // Parse -c/--config argument
    let mut config_path: Option<&str> = None;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-c" | "--config" => {
                config_path = args.get(i + 1).map(String::as_str);
                i += 2;
            }
            "-h" | "--help" => {
                println!(
                    "nsbc-web - NSBC Web management sidecar\n\n\
                     USAGE:\n  nsbc-web [-c|--config <path>] [hash-password <user> <L0|L1|L2>]\n\n\
                     OPTIONS:\n  -c, --config PATH   Config file (default: ./conf/nsbc.yaml or /etc/nsbc/nsbc.yaml)\n  -h, --help          Show this help"
                );
                std::process::exit(0);
            }
            _ => i += 1,
        }
    }

    // Logging: logs/nsbc-web.log under binary directory (hard-coded, not configurable)
    let exe_dir = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .unwrap_or_else(|| std::path::PathBuf::from("."));
    let log_dir = exe_dir.join("logs");
    if let Err(e) = std::fs::create_dir_all(&log_dir) {
        eprintln!("nsbc-web: cannot create {}: {e}; falling back to stderr", log_dir.display());
    }
    let log_path = log_dir.join("nsbc-web.log");
    match std::fs::OpenOptions::new().create(true).append(true).open(&log_path) {
        Ok(f) => {
            let file = Arc::new(std::sync::Mutex::new(f));
            tracing_subscriber::fmt()
                .with_env_filter(EnvFilter::try_from_default_env()
                    .unwrap_or_else(|_| "nsbc_web=info,tower_http=warn".into()))
                .with_writer(move || TsRenamingFile(file.clone()))
                .with_timer(LocalShortTimer)
                .json()
                .init();
            eprintln!("nsbc-web: logging to {}", log_path.display());
        }
        Err(e) => {
            eprintln!("nsbc-web: cannot open {}: {e}; falling back to stderr", log_path.display());
            tracing_subscriber::fmt()
                .with_env_filter(EnvFilter::try_from_default_env()
                    .unwrap_or_else(|_| "nsbc_web=info,tower_http=warn".into()))
                .with_writer(|| TsRenamingWriter(std::io::stderr()))
                .with_timer(LocalShortTimer)
                .json()
                .init();
        }
    }

    // Load configuration (env > file > defaults)
    let cfg = config::WebConfig::load(config_path)?;
    let bind: SocketAddr = cfg.bind_addr()?;

    // TLS certificates: auto-generate self-signed / explicit PEM / public ACME (certmagic)
    let tls_source = if cfg.web.tls.enabled {
        match tls::init(&cfg.web.tls, bind).await {
            Ok(src) => {
                match &src {
                    tls::TlsSource::Internal { renew_task, .. } => {
                        tracing::info!(
                            "TLS ready ✅ (cert mode = internal, auto-renew = {})",
                            renew_task.is_some()
                        );
                    }
                    #[cfg(feature = "acme")]
                    tls::TlsSource::Public { .. } => {
                        tracing::info!("TLS ready ✅ (cert mode = public ACME via certmagic TLS-ALPN-01)");
                    }
                    tls::TlsSource::PlainHttp => unreachable!(),
                }
                Some(src)
            }
            Err(e) => {
                // W-04 安全修复：tls.enabled=true 但初始化失败 → fail-closed
                // 不再静默降级为明文 HTTP。生产配置必须正确。
                tracing::error!(
                    "tls cert setup failed: {e}; refusing to fall back to plain HTTP (tls.enabled=true)"
                );
                return Err(e.into());
            }
        }
    } else {
        tracing::warn!("tls disabled — WebRTC getUserMedia will only work on localhost");
        None
    };

    if bind.ip().is_unspecified() {
        let tls_on = tls_source.is_some();
        tracing::warn!(
            "binding on wildcard; {}",
            if tls_on { "TLS enabled" }
            else { "no TLS — production behind mTLS terminator" }
        );
    }

    // Core control channel: use cfg.core.control_sock instead of hard-coded from_env
    let core = RealCore::from_config(&cfg.core);
    tracing::info!(?core, "core control client initialized");

    // Auth domain: use cfg.auth instead of from_env (includes default account injection)
    let auth = Arc::new(AuthSvc::from_config(&cfg.auth)?);

    // ---------- Phase 4 Web DB bootstrap (2026-10-15 incremental) ----------
    // dsn precedence: NSBC_DB_DSN env > cfg.db.dsn > interactive wizard > SQLite default
    let dsn = resolve_db_dsn(&cfg.db);
    let db = db::Db::from_dsn(&dsn).await;

    // Startup seed: ensure web_users table has a default admin user (idempotent, skip if
    // already present). Password hash generated by auth::accounts::hash_password_argon2id
    // (argon2id, production parameters).
    // W-01 安全修复：不再使用硬编码默认密码 "Admin123"。
    // 生产部署必须显式设置 NSBC_DEFAULT_ADMIN_PW 环境变量或 auth.default_admin_pw 配置项。
    {
        use auth::accounts::hash_password_argon2id;
        let admin_pw = std::env::var("NSBC_DEFAULT_ADMIN_PW").ok()
            .or_else(|| {
                if cfg.auth.default_admin_pw.is_empty() { None }
                else { Some(cfg.auth.default_admin_pw.clone()) }
            });
        if let Some(admin_pw) = admin_pw {
            match hash_password_argon2id(&admin_pw) {
                Ok(phc) => {
                    db.ensure_default_web_user("admin", "L2", &phc).await;
                }
                Err(e) => {
                    tracing::warn!(error = %e,
                        "failed to hash admin password for web_users seed");
                }
            }
        } else {
            tracing::warn!(
                "no admin password configured — set NSBC_DEFAULT_ADMIN_PW or auth.default_admin_pw; skipping admin seed"
            );
        }
    }

    let state = Arc::new(AppState {
        core,
        auth,
        db,
        api_major: API_MAJOR,
        bind,
        recording_root: std::path::PathBuf::from(&cfg.recording.path),
        require_loopback_auth: cfg.web.ext.require_loopback_auth,
        data_pushed: Arc::new(AtomicBool::new(false)),
        calls_tx: tokio::sync::broadcast::channel(256).0,
        vad_tx:   tokio::sync::broadcast::channel(1024).0,
    });

    // CDR background poller: fetch core-side cdr.recent every 15s → write DB.
    poller::spawn(state.clone(), 15, 100);

    // AI Agent barge-in background task (REQ-AI-BARGEIN-001 P0):
    // Subscribe to VAD_EVENT pushed by C core's vad_engine; auto-mute AI side when user speaks.
    ai_agent::spawn(state.clone());

    // HA: every 60s full re-push config to local nsbcd (standby node does not handle CRUD,
    // relies on this to keep core in-memory tables fresh; idempotent, single-node deployments
    // are also harmless).
    sync::spawn_periodic_push(state.clone());

    // UDS Data Service: C→Web write-through (CDR/registrations/runtime state written into DB
    // in real time). Strip "unix:" prefix — UnixListener::bind needs a plain file path
    let uds_path = std::path::PathBuf::from(
        cfg.core.data_sock
            .strip_prefix("unix:")
            .unwrap_or(&cfg.core.data_sock),
    );
    match uds_server::start(state.clone(), uds_path.clone()) {
        Ok(_) => {
            tracing::info!(path = %uds_path.display(), "UDS Data Service bound (C→Web write-through)");
        }
        Err(e) => {
            tracing::error!(error = %e, path = %uds_path.display(), "UDS Data Service bind failed — write-through disabled, poller fallback active");
        }
    }

    let app = app_router(state);

    let listener = TcpListener::bind(bind).await?;

    // ---- accept loop (three TlsSource branches) ----
    match tls_source {
        Some(tls::TlsSource::Internal { holder, .. }) => {
            tracing::info!(
                "nsbc-web listening on https://{}/api/v1 (internal TLS + hot-renew)",
                bind
            );
            // Per-iteration fresh ctrl_c() future — tokio 1.54+ makes it a hard panic if the
            // SAME signal future / JoinHandle gets polled twice after completion. Creating a
            // brand-new CtrlC per loop keeps each poll isolated.
            loop {
                tokio::select! {
                    biased;
                    _ = tokio::signal::ctrl_c() => break,
                    accept_result = listener.accept() => {
                        let (stream, peer) = match accept_result {
                            Ok(v) => v,
                            Err(e) => { tracing::warn!("tcp accept: {e}"); continue; }
                        };
                        let acceptor = {
                            let guard = holder.read().expect("holder poisoned");
                            guard.clone()
                        };
                        let app = app.clone();
                        tokio::spawn(async move {
                            handle_tls_stream(acceptor, stream, peer, app).await;
                        });
                    }
                }
            }
        }

        #[cfg(feature = "acme")]
        Some(tls::TlsSource::Public { rustls_config }) => {
            tracing::info!(
                "nsbc-web listening on https://{}/api/v1 (certon TLS-ALPN-01 — dynamic ResolvesServerCert)",
                bind
            );
            // TlsAcceptor::from already wraps Arc internally; clone is O(1)
            let acceptor = Arc::new(TlsAcceptor::from(rustls_config));
            // Same per-iteration fresh ctrl_c() as the Internal branch — avoids the
            // tokio 1.54+ JoinHandle double-poll panic.
            loop {
                tokio::select! {
                    biased;
                    _ = tokio::signal::ctrl_c() => break,
                    accept_result = listener.accept() => {
                        let (stream, peer) = match accept_result {
                            Ok(v) => v,
                            Err(e) => { tracing::warn!("tcp accept: {e}"); continue; }
                        };
                        let acceptor = acceptor.clone();
                        let app = app.clone();
                        tokio::spawn(async move {
                            handle_tls_stream(acceptor, stream, peer, app).await;
                        });
                    }
                }
            }
        }

        _ => {
            tracing::info!("nsbc-web listening on http://{}/api/v1", bind);
            axum::serve(
                listener,
                app.into_make_service_with_connect_info::<SocketAddr>(),
            )
            .with_graceful_shutdown(shutdown_signal())
            .await?;
            return Ok(());
        }
    }
    Ok(())
}

async fn handle_tls_stream(
    acceptor: Arc<TlsAcceptor>,
    stream: tokio::net::TcpStream,
    peer: SocketAddr,
    app: axum::Router,
) {
    let tls_stream = match acceptor.as_ref().accept(stream).await {
        Ok(s) => s,
        Err(e) => {
            let emsg = format!("{e}");
            if emsg.contains("InvalidContentType") {
                tracing::debug!(%peer, "tls accept skipped (non-TLS client): {emsg}");
            } else {
                tracing::warn!(%peer, "tls accept failed: {emsg}");
            }
            return;
        }
    };
    let io = hyper_util::rt::TokioIo::new(tls_stream);

    // ─── REQ-EXT-API-004: 在 TLS 分支也注入 peer ConnectInfo ───
    // HTTP 分支用 axum::serve + into_make_service_with_connect_info::<SocketAddr>() 自动注入；
    // TLS 分支是我们手动 accept loop → 必须自己在每个 Request 里塞 ConnectInfo(peer)，
    // 否则 ext_key middleware 的 loopback 短路永远看不到 peer IP。
    use tower::ServiceBuilder;
    let inner = ServiceBuilder::new()
        .map_request(move |mut req: axum::http::Request<hyper::body::Incoming>| {
            req.extensions_mut().insert(axum::extract::ConnectInfo(peer));
            req
        })
        .service(app.clone().into_service());
    // tower::Service → hyper::Service 桥接（MapRequest 只实现 tower trait，
    // hyper serve_connection 要 hyper::Service trait）
    let svc = hyper_util::service::TowerToHyperService::new(inner);

    let _ = hyper::server::conn::http1::Builder::new()
        .keep_alive(true)
        .serve_connection(io, svc)
        .await;
}

/// `hash-password` subcommand implementation (synchronous, one-shot CLI; exits 1 on failure).
fn run_hash_password(args: &[String]) -> ! {
    let usage = "usage: nsbc-web hash-password <username> <L0|L1|L2>\n\
                 password is read from stdin (one line, trimmed trailing CR/LF)";
    let (username, role_s) = match (args.first(), args.get(1)) {
        (Some(u), Some(r)) => (u.as_str(), r.as_str()),
        _ => {
            eprintln!("{usage}");
            std::process::exit(2);
        }
    };
    let role = match auth::Role::parse(role_s) {
        Some(r) => r,
        None => {
            eprintln!("error: role must be one of L0/L1/L2");
            std::process::exit(2);
        }
    };
    if !auth::valid_username(username) {
        eprintln!("error: invalid username (1..64 bytes, alnum and . _ - @)");
        std::process::exit(2);
    }
    use std::io::Read;
    let mut input = String::new();
    if std::io::stdin().read_to_string(&mut input).is_err() {
        eprintln!("error: failed to read password from stdin");
        std::process::exit(1);
    }
    let password = input.trim_end_matches(['\r', '\n']);
    let policy = auth::accounts::PasswordPolicy::from_env().unwrap_or_default();
    if let Err(e) = policy.validate(password) {
        eprintln!("error: {e}");
        std::process::exit(1);
    }
    match auth::accounts::hash_password_argon2id(password) {
        Ok(phc) => {
            println!("{}:{}:{phc}", role.as_str(), username);
            std::process::exit(0);
        }
        Err(e) => {
            eprintln!("error: {e}");
            std::process::exit(1);
        }
    }
}

async fn shutdown_signal() {
    let _ = tokio::signal::ctrl_c().await;
    tracing::info!("shutdown signal received, draining");
}

#[cfg(test)]
mod tests {
    //! Full-link (JWT login sessions + version governance + RBAC + routes + mock core)
    //! smoke tests.
    use std::collections::BTreeMap;
    use std::net::SocketAddr;
    use std::sync::Arc;
    use std::sync::atomic::AtomicBool;

    use axum::body::Body;
    use axum::http::{HeaderName, Method, Request, StatusCode};
    use serde_json::Value;
    use tower::ServiceExt;

    use argon2::password_hash::SaltString;
    use argon2::{Algorithm, Argon2, Params, PasswordHasher, Version};

    use crate::auth::AuthSvc;
    use crate::coreclient::{CoreError, MockCore};
    use crate::{app_router, AppState, API_MAJOR};

    const API_HDR: HeaderName = HeaderName::from_static("x-nsbc-api-version");

    /// Test argon2id PHC (low-cost parameters; verify algorithm path is identical to prod).
    fn phc(password: &str) -> String {
        let mut raw = [0u8; 16];
        getrandom::getrandom(&mut raw).unwrap();
        let salt = SaltString::encode_b64(&raw).unwrap();
        // m=4MiB/t=1/p=1: below production recommendation but enough to cover argon2id
        // verification path.
        let arg = Argon2::new(
            Algorithm::Argon2id,
            Version::V0x13,
            Params::new(4096, 1, 1, None).unwrap(),
        );
        arg.hash_password(password.as_bytes(), &salt).unwrap().to_string()
    }

    fn app() -> (axum::Router, Arc<AppState<MockCore>>) {
        // Two local accounts: u0=L0 read-only, u1=L1 operation.
        let accounts = format!("L0:u0:{};L1:u1:{}", phc("pw0"), phc("pw1"));
        app_with(Arc::new(AuthSvc::for_test(&accounts)))
    }

    /// Assemble a full-link app with custom AuthSvc (lock/rate-limit thresholds, audit file).
    fn app_with(auth: Arc<AuthSvc>) -> (axum::Router, Arc<AppState<MockCore>>) {
        let canned = BTreeMap::from([
            ("show calls".to_string(), Ok("active=1 total=2 failed=0\n".into())),
            (
                "show channels".to_string(),
                Ok("call a_callid=c1 a_state=CONFIRMED b_callid=c1 b_state=CONFIRMED\n\
channels=1 truncated=0\n".into()),
            ),
            (
                "uuid_dump:c1".to_string(),
                Ok("call a_callid=c1 a_state=CONFIRMED b_callid=c1 b_state=CONFIRMED\n\
leg 0 state=CONFIRMED proto=udp reliable=0 cseq=9 routes=0\n\
leg 0 tags local=la remote=ra\n\
leg 0 media mode=UNAVAILABLE gate=UNAVAILABLE ssrc_in=UNAVAILABLE \
ssrc_out=UNAVAILABLE vq=UNAVAILABLE\n".into()),
            ),
            (
                "config.get:log.level".to_string(),
                Ok("config key=log.level layer=system secret=0\n6\n".into()),
            ),
            (
                "config.export".to_string(),
                Ok("config-export keys=2 redact=1\n\
                    log.level: 6\n\
                    db.dsn: ****\n"
                    .into()),
            ),
            // hangup: c1 succeeds; other ids not present → core 404 semantics
            ("hangup".to_string(), Ok("OK hungup c1\n".into())),
            (
                "hangup:gone".to_string(),
                Err(CoreError::NotFound("x".into())),
            ),
        ]);
        let core = MockCore {
            calls: Arc::new(std::sync::Mutex::new(Vec::new())),
            canned: Arc::new(canned),
        };
        let st = Arc::new(AppState {
            core,
            auth,
            db: crate::db::Db::new(),
            api_major: API_MAJOR,
            bind: SocketAddr::from(([127, 0, 0, 1], 0)),
            recording_root: std::path::PathBuf::from("/tmp/nsbc-test-recordings"),
            require_loopback_auth: false,  /* F-10 修复 */
            data_pushed: Arc::new(AtomicBool::new(false)),
            calls_tx: tokio::sync::broadcast::channel(256).0,
        vad_tx:   tokio::sync::broadcast::channel(1024).0,
        });
        (app_router(st.clone()), st)
    }

    /// Send request: default carries version header; bearer/JSON body optional.
    /// Returns (status code, response body).
    async fn call(
        app: &axum::Router,
        method: Method,
        uri: &str,
        bearer: Option<&str>,
        json: Option<&str>,
        version_hdr: bool,
    ) -> (StatusCode, String) {
        let mut b = Request::builder().method(method).uri(uri);
        if version_hdr {
            b = b.header(&API_HDR, "1");
        }
        if let Some(t) = bearer {
            b = b.header(axum::http::header::AUTHORIZATION, format!("Bearer {t}"));
        }
        let body = match json {
            Some(j) => {
                b = b.header(axum::http::header::CONTENT_TYPE, "application/json");
                Body::from(j.to_string())
            }
            None => Body::empty(),
        };
        let resp = app.clone().oneshot(b.body(body).unwrap()).await.unwrap();
        let status = resp.status();
        let bytes = axum::body::to_bytes(resp.into_body(), 1 << 16).await.unwrap();
        (status, String::from_utf8(bytes.to_vec()).unwrap())
    }

    async fn login(app: &axum::Router, user: &str, pw: &str) -> (StatusCode, Value) {
        let (s, b) = call(
            app,
            Method::POST,
            "/api/v1/auth/login",
            None,
            Some(&format!(r#"{{"username":"{user}","password":"{pw}"}}"#)),
            true,
        )
        .await;
        (s, serde_json::from_str(&b).unwrap_or(Value::Null))
    }

    fn tokens(v: &Value) -> (String, String) {
        (
            v["access_token"].as_str().unwrap().to_string(),
            v["refresh_token"].as_str().unwrap().to_string(),
        )
    }

    #[tokio::test]
    async fn login_jwt_session_rbac_and_core_proxy() {
        let (app, st) = app();

        // /health: no auth, no version header
        assert_eq!(call(&app, Method::GET, "/health", None, None, false).await.0, StatusCode::OK);

        // Login endpoint also subject to version governance: missing version header → 400
        assert_eq!(
            call(&app, Method::POST, "/api/v1/auth/login", None, Some("{}"), false)
                .await
                .0,
            StatusCode::BAD_REQUEST
        );
        // Wrong password → unified 401 (no leak whether user exists)
        let (s, _) = login(&app, "u0", "wrong").await;
        assert_eq!(s, StatusCode::UNAUTHORIZED);
        let (s, b) = call(
            &app, Method::POST, "/api/v1/auth/login", None,
            Some(r#"{"username":"ghost","password":"pw0"}"#), true).await;
        assert_eq!(s, StatusCode::UNAUTHORIZED);
        assert!(b.contains("INVALID_CREDENTIALS"), "body={b}");
        // Username outside whitelist → 400
        let (s, _) = call(
            &app, Method::POST, "/api/v1/auth/login", None,
            Some(r#"{"username":"bad name!","password":"x"}"#), true).await;
        assert_eq!(s, StatusCode::BAD_REQUEST);

        // L0 login success, get access/refresh
        let (s, v0) = login(&app, "u0", "pw0").await;
        assert_eq!(s, StatusCode::OK);
        assert_eq!(v0["token_type"], "Bearer");
        assert_eq!(v0["expires_in"], 900);
        let (a0, _r0) = tokens(&v0);

        // No token / garbage token → 401
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", None, None, true).await.0,
            StatusCode::UNAUTHORIZED
        );
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some("garbage"), None, true).await.0,
            StatusCode::UNAUTHORIZED
        );
        // Version mismatch still fail-closed (version layer checked even with token)
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some(&a0), None, false).await.0,
            StatusCode::BAD_REQUEST
        );
        // L0 read-only → 200 real core data; L0 hangup → 403
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some(&a0), None, true).await.0,
            StatusCode::OK
        );
        // Call detail is also L0 read-only (rbac special-case for the last segment of Call-ID)
        assert_eq!(
            call(&app, Method::GET, "/api/v1/channels", Some(&a0), None, true).await.0,
            StatusCode::OK
        );
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls/c1", Some(&a0), None, true).await.0,
            StatusCode::OK
        );
        // Config effective-value query is also L0 read-only (rbac config/ single-segment special-case)
        assert_eq!(
            call(&app, Method::GET, "/api/v1/config/log.level", Some(&a0), None, true)
                .await
                .0,
            StatusCode::OK
        );
        // Full redacted export is also L0 read-only
        assert_eq!(
            call(&app, Method::GET, "/api/v1/config", Some(&a0), None, true)
                .await
                .0,
            StatusCode::OK
        );
        assert_eq!(
            call(&app, Method::POST, "/api/v1/calls/c1/hangup", Some(&a0), None, true).await.0,
            StatusCode::FORBIDDEN
        );

        // L1 login → hangup 200 / notfound 404
        let (s, v1) = login(&app, "u1", "pw1").await;
        assert_eq!(s, StatusCode::OK);
        let (a1, _r1) = tokens(&v1);
        assert_eq!(
            call(&app, Method::POST, "/api/v1/calls/c1/hangup", Some(&a1), None, true).await.0,
            StatusCode::OK
        );
        assert_eq!(
            call(&app, Method::POST, "/api/v1/calls/gone/hangup", Some(&a1), None, true).await.0,
            StatusCode::NOT_FOUND
        );

        // Expired access → 401 TOKEN_EXPIRED
        let now = crate::auth::tokens::now_secs();
        let ((old_access, _), _) = st.auth.tokens.issue_pair("u1", 1, now - 10_000);
        let (s, b) =
            call(&app, Method::GET, "/api/v1/calls", Some(&old_access), None, true).await;
        assert_eq!(s, StatusCode::UNAUTHORIZED);
        assert!(b.contains("TOKEN_EXPIRED"), "body={b}");

        // Core unreachable (uncanned command) → 502, not 500
        assert_eq!(
            call(&app, Method::GET, "/api/v1/registrations", Some(&a0), None, true).await.0,
            StatusCode::BAD_GATEWAY
        );
    }

    #[tokio::test]
    async fn refresh_rotation_replay_and_logout() {
        let (app, _st) = app();

        let (_, v) = login(&app, "u1", "pw1").await;
        let (a1, r1) = tokens(&v);
        // access token is usable directly
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some(&a1), None, true).await.0,
            StatusCode::OK
        );

        // Refresh: successful rotation, returns new token pair
        let (s, body) = call(
            &app, Method::POST, "/api/v1/auth/refresh", None,
            Some(&format!(r#"{{"refresh_token":"{r1}"}}"#)), true).await;
        assert_eq!(s, StatusCode::OK, "body={body}");
        let v2: Value = serde_json::from_str(&body).unwrap();
        let (a2, r2) = tokens(&v2);
        assert_ne!(a1, a2);
        // Old refresh is single-use: present again → 401 TOKEN_REPLAY
        let (s, b) = call(
            &app, Method::POST, "/api/v1/auth/refresh", None,
            Some(&format!(r#"{{"refresh_token":"{r1}"}}"#)), true).await;
        assert_eq!(s, StatusCode::UNAUTHORIZED);
        assert!(b.contains("TOKEN_REPLAY"), "body={b}");
        // New access/refresh work
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some(&a2), None, true).await.0,
            StatusCode::OK
        );

        // Logout: revoke current access (up to its exp) + optional refresh
        let (s, _) = call(
            &app, Method::POST, "/api/v1/auth/logout", Some(&a2),
            Some(&format!(r#"{{"refresh_token":"{r2}"}}"#)), true).await;
        assert_eq!(s, StatusCode::OK);
        // access becomes invalid immediately (don't have to wait 15min)
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some(&a2), None, true).await.0,
            StatusCode::UNAUTHORIZED
        );
        // refresh already logged out → 401
        let (s, _) = call(
            &app, Method::POST, "/api/v1/auth/refresh", None,
            Some(&format!(r#"{{"refresh_token":"{r2}"}}"#)), true).await;
        assert_eq!(s, StatusCode::UNAUTHORIZED);

        // Logout is idempotent: logout with no token → 401 (protected endpoint);
        // empty body does not affect it
        let (s, _) = call(
            &app, Method::POST, "/api/v1/auth/logout", Some(&a2),
            Some("{}"), true).await;
        assert_eq!(s, StatusCode::UNAUTHORIZED);

        // Refresh used as access → type mix-up 401
        let (_, v3) = login(&app, "u0", "pw0").await;
        let (_, r3) = tokens(&v3);
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some(&r3), None, true).await.0,
            StatusCode::UNAUTHORIZED
        );
        // r3 not consumed, still refreshes normally
        let (s, _) = call(
            &app, Method::POST, "/api/v1/auth/refresh", None,
            Some(&format!(r#"{{"refresh_token":"{r3}"}}"#)), true).await;
        assert_eq!(s, StatusCode::OK);
    }

    #[tokio::test]
    async fn lockout_rate_limit_unknown_fields_and_audit() {
        use crate::auth::audit::AuditLog;
        use crate::auth::limiter::Limits;

        // Audit goes to a temp file, verify full-link events.
        let apath = std::env::temp_dir().join(format!(
            "nsbc_w3b_audit_{}_{}.jsonl",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
        ));
        let audit = AuditLog::open(apath.to_str().unwrap()).unwrap();
        let accounts = format!("L0:u0:{};L1:u1:{}", phc("pw0"), phc("pw1"));
        // login bucket 6/min, lock threshold 3; pick values that hit both within a small sequence.
        let limits = Limits {
            lock_threshold: 3,
            rl_login: 6,
            ..Limits::default()
        };
        let (app, _st) = app_with(Arc::new(AuthSvc::build(&accounts, limits, audit)));

        // 1) REQ-WEB-009: reject unknown fields → 422 (consumes one login quota)
        let (s, _) = call(
            &app, Method::POST, "/api/v1/auth/login", None,
            Some(r#"{"username":"u0","password":"pw0","bogus":1}"#), true).await;
        assert_eq!(s, StatusCode::UNPROCESSABLE_ENTITY);

        // 2) 3 consecutive wrong passwords: all 401; 3rd triggers lockout (still 401 on that attempt)
        for _ in 0..3 {
            let (s, b) = login(&app, "u0", "wrong").await;
            assert_eq!(s, StatusCode::UNAUTHORIZED);
            assert_eq!(b["error"]["code"], "INVALID_CREDENTIALS");
        }
        // 3) Even correct password returns 423 after lockout (no argon2 verification performed)
        let (s, b) = login(&app, "u0", "pw0").await;
        assert_eq!(s, StatusCode::LOCKED);
        assert_eq!(b["error"]["code"], "ACCOUNT_LOCKED");
        // 4) Lockout is per-username: u1 is unaffected (this is the 6th login request total,
        // exactly fills the quota)
        let (s, _) = login(&app, "u1", "pw1").await;
        assert_eq!(s, StatusCode::OK);
        // 5) 7th login → 429 with Retry-After
        let req = Request::builder()
            .method(Method::POST)
            .uri("/api/v1/auth/login")
            .header(&API_HDR, "1")
            .header(axum::http::header::CONTENT_TYPE, "application/json")
            .body(Body::from(r#"{"username":"u0","password":"x"}"#))
            .unwrap();
        let resp = app.clone().oneshot(req).await.unwrap();
        assert_eq!(resp.status(), StatusCode::TOO_MANY_REQUESTS);
        assert!(resp.headers().get("retry-after").is_some());

        // Audit file: must contain at least login_failed / account_locked / login_locked /
        // login_success event types, with required fields present.
        let text = std::fs::read_to_string(&apath).unwrap();
        for ev in ["login_failed", "account_locked", "login_locked", "login_success"] {
            assert!(text.contains(&format!("\"event\":\"{ev}\"")), "missing {ev}: {text}");
        }
        assert!(text.contains("\"sub\":\"u1\""));
        std::fs::remove_file(&apath).unwrap();
    }
}
