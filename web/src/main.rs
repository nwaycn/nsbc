//! NSBC Web 管理系统边车（REQ-WEB-001~021，P0 骨架）
//!
//! 隔离原则（REQ-WEB-001/013）：独立进程/独立账户/独立依赖树，
//! 仅经 REST API（REQ-OAM-001）与实时控制协议（REQ-OAM-004）访问核心；
//! 故障、抖动、发布不影响呼叫面。核心侧对所有客户端统一限流/鉴权/审计，
//! 边车不做权限旁路（REQ-WEB-020）。
//!
//! 安全基线：#![deny(unsafe_code)]（REQ-WEB-011）；
//! API 版本发现越界 fail-closed（REQ-WEB-020）；RBAC L0/L1/L2
//! 与核心命令注册表同口径（REQ-WEB-008 / REQ-OAM-008）。
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

/// 本地短格式时间戳（"HH:MM:SS.mmm"），用于 JSON 日志。
struct LocalShortTimer;

impl FormatTime for LocalShortTimer {
    fn format_time(&self, w: &mut tracing_subscriber::fmt::format::Writer<'_>) -> std::fmt::Result {
        // 只用 now_utc()——避免 local-offset feature 引入的 vendor 缺失依赖
        let now = time::OffsetDateTime::now_utc();
        // 短格式：MMDD HH:MM:SS.mmm UTC（17 字符，兼顾日期与可读性；UTC 明确时区）
        let mo = now.month() as u8;
        let d  = now.day();
        let h  = now.hour();
        let m  = now.minute();
        let s  = now.second();
        let ms = now.nanosecond() / 1_000_000;
        write!(w, "{:02}{:02} {:02}:{:02}:{:02}.{:03}Z", mo, d, h, m, s, ms)
    }
}

/// Writer wrapper：把 tracing-subscriber json 层硬编码的 "timestamp" 字段重命名为 "tm"。
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

/// File version：timestamp → tm，写文件。
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


/// 客户端（REQ-WEB-013 唯一通道），测试为 mock，不做 dyn 分发与全局单例。
/// 认证域（账号/JWT/会话状态）见 [`AuthSvc`]（REQ-WEB-008）。
/// 持久化域（CDR/audit/config_history）见 [`Db`]（REQ-WEB-013 可选）。
#[derive(Clone)]
pub struct AppState<C: CoreApi + Clone> {
    pub core:          C,
    pub auth:          Arc<AuthSvc>,
    pub db:            db::Db,
    pub api_major:     u32,
    pub bind:          SocketAddr,
    pub recording_root: std::path::PathBuf,
    /// C→Web data.sock 通道已建立（UDS 连上后置 true，用来让 poller 跳过轮询）。
    /// 两个任务（uds_server accept loop + poller interval loop）跨 tokio task，
    /// 用 AtomicBool 做无锁协调。
    pub data_pushed:   Arc<AtomicBool>,
    /// SESSION 实时事件广播（nsbc-esl-web §8#1）：C→Web data.sock 推送的会话
    /// 事件经此通道 fan-out 给所有 `/calls/stream`（SSE）订阅者，前端由此替代
    /// 5s 轮询实现实时刷新。慢消费者由 broadcast 环形缓冲（cap 256）处理，
    /// 落后即记 lagged，不阻塞 UDS dispatch 热路径（无反压，符合项目约束）。
    pub calls_tx:      tokio::sync::broadcast::Sender<String>,
    /// VAD_EVENT 实时广播（REQ-AI-BARGEIN-001）：C 核心 vad_engine 推的
    /// {"kind":"vad_event","payload":{event:"VAD_EVENT",...}} 经此通道 fan-out
    /// 给 ai_agent 模块做 barge-in 决策。缓冲更大（cap 1024）因为 VAD 帧频率高。
    pub vad_tx:        tokio::sync::broadcast::Sender<String>,
}

pub const API_MAJOR: u32 = 1;

/// /api/v1 业务路由（鉴权/版本治理分层在 main 装配，REQ-WEB-009/012/013）。
/// login/refresh 为公开端点（无 rbac）；logout 与其余端点受 rbac 保护。
///
/// rbac 中间件需要读取 [`AppState`]（JWT/会话表），而 axum 的
/// `from_fn` 不注入 State，必须在装配期用 `from_fn_with_state`
/// 给出状态，故本函数直接消费 state 并返回已定型的 `Router<()>`。
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
        .merge(routes::billing::router())
        .merge(routes::transcode::router())
        .merge(routes::tenants::router())
        .merge(routes::auth::protected_router())
        .layer(axmw::from_fn_with_state(st.clone(), rbac::require_auth::<C>));
    Router::new()
        .merge(routes::auth::public_router())
        .merge(protected)
        .layer(axmw::from_fn_with_state(st.clone(), ratelimit::enforce::<C>))
        .layer(axmw::from_fn(apiversion::enforce))
        .layer(RequestBodyLimitLayer::new(1024 * 1024))
        .with_state(st)
}

/// 完整应用：/api/v1 业务面 + 不带鉴权的 /health 探活 + 安全响应头。
fn app_router<C: CoreApi + Clone + 'static>(state: Arc<AppState<C>>) -> Router {
    // 安全响应头层（OWASP 基础基线，兼容 Vue 3 编译期模板 eval）。
    // Vue 3 global build 用 new Function() 做模板编译 → script-src 必须 'unsafe-eval'。
    // Cross-Origin-Opener-Policy 仅 HTTPS 生效，HTTP 上 Chrome 直接忽略并报警告 → 移除。
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
        .route("/health", axum::routing::get(routes::system::health))
        .merge(routes::recordings::public_file_router(state.clone()))
        .merge(static_files::router());

    // 逐层添加安全头（SetResponseHeaderLayer 覆盖重复设置）
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

/// Phase 4 DB DSN 引导（2026-10-15 增量）：
/// 优先级链：NSBC_DB_DSN env > cfg.db.dsn > 交互式引导 > SQLite 默认
fn resolve_db_dsn(cfg: &config::DbSection) -> String {
    // 1. env override 最高优先
    if let Ok(v) = std::env::var("NSBC_DB_DSN") {
        tracing::info!("🔧 db: NSBC_DB_DSN env override → {:?}", v);
        return v;
    }
    // 2. cfg 文件显式配置
    if !cfg.dsn.is_empty() {
        tracing::info!("🔧 db: cfg.dsn → {:?}", cfg.dsn);
        return cfg.dsn.clone();
    }

    // 3. 交互式引导（TTY 可用时）
    let is_tty = std::io::stdin().is_terminal();

    if is_tty {
        tracing::warn!("db.dsn 未配置，进入交互式引导…");
        println!();
        println!("╔══════════════════════════════════════╗");
        println!("║  NSBC Web DB 初始化引导（Phase 4）   ║");
        println!("╠══════════════════════════════════════╣");
        println!("║  1) SQLite  (轻量单机，推荐 PoC)      ║");
        println!("║  2) MySQL   (生产级)                 ║");
        println!("║  3) PostgreSQL (生产级)              ║");
        println!("║  4) 跳过（仅内存）                   ║");
        println!("╚══════════════════════════════════════╝");
        print!("请选择 [1/2/3/4] 默认(1): ");
        std::io::stdout().flush().ok();

        let mut choice = String::new();
        std::io::stdin().read_line(&mut choice).ok();
        let choice = choice.trim();

        match choice {
            "2" => {
                print!("MySQL 主机 [127.0.0.1]: "); std::io::stdout().flush().ok();
                let mut host = String::new();
                std::io::stdin().read_line(&mut host).ok();
                let host = host.trim_or("127.0.0.1");

                print!("MySQL 端口 [3306]: "); std::io::stdout().flush().ok();
                let mut port = String::new();
                std::io::stdin().read_line(&mut port).ok();
                let port = port.trim_or("3306");

                print!("MySQL 用户 [nsbc]: "); std::io::stdout().flush().ok();
                let mut user = String::new();
                std::io::stdin().read_line(&mut user).ok();
                let user = user.trim_or("nsbc");

                print!("MySQL 密码: "); std::io::stdout().flush().ok();
                let mut pass = String::new();
                std::io::stdin().read_line(&mut pass).ok();
                let pass = pass.trim();

                print!("MySQL 数据库 [nsbc]: "); std::io::stdout().flush().ok();
                let mut dbname = String::new();
                std::io::stdin().read_line(&mut dbname).ok();
                let dbname = dbname.trim_or("nsbc");

                let dsn = format!("mysql://{}:{}@{}:{}/{}",
                    user, pass, host, port, dbname);
                tracing::info!("🔧 db: MySQL DSN → {}:{}/{}", host, port, dbname);
                return dsn;
            }
            "3" => {
                print!("PostgreSQL 主机 [127.0.0.1]: "); std::io::stdout().flush().ok();
                let mut host = String::new();
                std::io::stdin().read_line(&mut host).ok();
                let host = host.trim_or("127.0.0.1");

                print!("PostgreSQL 端口 [5432]: "); std::io::stdout().flush().ok();
                let mut port = String::new();
                std::io::stdin().read_line(&mut port).ok();
                let port = port.trim_or("5432");

                print!("PostgreSQL 用户 [nsbc]: "); std::io::stdout().flush().ok();
                let mut user = String::new();
                std::io::stdin().read_line(&mut user).ok();
                let user = user.trim_or("nsbc");

                print!("PostgreSQL 密码: "); std::io::stdout().flush().ok();
                let mut pass = String::new();
                std::io::stdin().read_line(&mut pass).ok();
                let pass = pass.trim();

                print!("PostgreSQL 数据库 [nsbc]: "); std::io::stdout().flush().ok();
                let mut dbname = String::new();
                std::io::stdin().read_line(&mut dbname).ok();
                let dbname = dbname.trim_or("nsbc");

                let dsn = format!("postgres://{}:{}@{}:{}/{}",
                    user, pass, host, port, dbname);
                tracing::info!("🔧 db: PostgreSQL DSN → {}:{}/{}", host, port, dbname);
                return dsn;
            }
            "4" => {
                tracing::info!("🔧 db: 引导跳过 — 持久化禁用");
                return String::new();
            }
            _ => { /* 默认 SQLite，走下面 */ }
        }
    }

    // 4. SQLite 默认（TTY 不可选 或 用户选 1 或 直接回车）
    let db_path = std::path::PathBuf::from("data/web.db");
    if let Some(parent) = db_path.parent() {
        std::fs::create_dir_all(parent).ok();
    }
    let dsn = format!("sqlite:{}", db_path.display());
    tracing::info!("🔧 db: SQLite 默认 → {:?}", dsn);
    dsn
}

/// 扩展 trait：Option<String> 上的 trim_or 默认值填充
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
    // rustls 0.23 必须在任何 TLS 操作前显式安装 CryptoProvider
    // certon(可能拉 aws-lc-rs) + 我们(ring) 双 provider 并存会导致 rustls 无法自动决策 → panic
    // 统一用 ring（与 Cargo.toml 的 ring 0.17 / rcgen(ring) / rustls(ring) 一致）
    let _ = rustls::crypto::ring::default_provider().install_default();

    // 子命令优先（hash-password 不加载任何 Web 域）
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).map(String::as_str) == Some("hash-password") {
        run_hash_password(&args[2..]); // 返回 !
    }

    // 解析 -c/--config 参数
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

    // 日志：二进制所在目录下 logs/nsbc-web.log（硬编码，不分发配置）
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

    // 加载配置（env > file > defaults）
    let cfg = config::WebConfig::load(config_path)?;
    let bind: SocketAddr = cfg.bind_addr()?;

    // TLS 证书：auto-generate 自签 / 显式 PEM / 公网 ACME（certmagic）
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
                tracing::error!("tls cert setup failed: {e}; falling back to plain HTTP");
                None
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

    // 核心控制通道：用 cfg.core.control_sock 替换硬编码 from_env
    let core = RealCore::from_config(&cfg.core);
    tracing::info!(?core, "core control client initialized");

    // 认证域：用 cfg.auth 替换 from_env（含默认账号注入）
    let auth = Arc::new(AuthSvc::from_config(&cfg.auth)?);

    // ---------- Phase 4 Web DB 引导（2026-10-15 增量） ----------
    // dsn 优先级：NSBC_DB_DSN env > cfg.db.dsn > 交互式引导 > SQLite 默认
    let dsn = resolve_db_dsn(&cfg.db);
    let db = db::Db::from_dsn(&dsn).await;

    // 启动期 seed：确保 web_users 表有默认 admin 用户（幂等，已存在则跳过）。
    // 密码哈希由 auth::accounts::hash_password_argon2id 生成（argon2id，生产参数）。
    // 优先级：NSBC_DEFAULT_ADMIN_PW env → cfg.auth.default_admin_pw → "Admin123"
    {
        use auth::accounts::hash_password_argon2id;
        let admin_pw = std::env::var("NSBC_DEFAULT_ADMIN_PW")
            .unwrap_or_else(|_| {
                if !cfg.auth.default_admin_pw.is_empty() {
                    cfg.auth.default_admin_pw.clone()
                } else {
                    "Admin123".to_string()
                }
            });
        match hash_password_argon2id(&admin_pw) {
            Ok(phc) => {
                db.ensure_default_web_user("admin", "L2", &phc).await;
            }
            Err(e) => {
                tracing::warn!(error = %e,
                    "failed to hash default admin password for web_users seed");
            }
        }
    }

    let state = Arc::new(AppState {
        core,
        auth,
        db,
        api_major: API_MAJOR,
        bind,
        recording_root: std::path::PathBuf::from(&cfg.recording.path),
        data_pushed: Arc::new(AtomicBool::new(false)),
        calls_tx: tokio::sync::broadcast::channel(256).0,
        vad_tx:   tokio::sync::broadcast::channel(1024).0,
    });

    // CDR 后台 poller：每 15s 拉一次核心侧 cdr.recent → 写 DB。
    poller::spawn(state.clone(), 15, 100);

    // AI Agent barge-in 后台任务（REQ-AI-BARGEIN-001 P0）：
    // 订阅 C 核心 vad_engine 推的 VAD_EVENT，用户说话时自动静音 AI 侧。
    ai_agent::spawn(state.clone());

    // HA：每 60s 向本机 nsbcd 全量重推配置（备机不经手 CRUD，
    // 靠这个保持核心内存表新鲜；幂等，单机部署同样无害）。
    sync::spawn_periodic_push(state.clone());

    // UDS Data Service：C→Web write-through（CDR/注册/运行时状态实时入库）
    // 剥 "unix:" 前缀 — UnixListener::bind 需要纯文件路径
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

    // ---- accept loop（TlsSource 三分支）----
    match tls_source {
        Some(tls::TlsSource::Internal { holder, .. }) => {
            tracing::info!(
                "nsbc-web listening on https://{}/api/v1 (internal TLS + hot-renew)",
                bind
            );
            let mut shutdown_rx = Box::pin(shutdown_signal());
            loop {
                tokio::select! {
                    biased;
                    _ = &mut shutdown_rx => break,
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
            // TlsAcceptor::from 内部已经 wrap Arc，clone 是 O(1)
            let acceptor = Arc::new(TlsAcceptor::from(rustls_config));
            let mut shutdown_rx = Box::pin(shutdown_signal());
            loop {
                tokio::select! {
                    biased;
                    _ = &mut shutdown_rx => break,
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
    let svc = hyper_util::service::TowerToHyperService::new(
        app.clone().into_service()
    );
    let _ = hyper::server::conn::http1::Builder::new()
        .serve_connection(io, svc)
        .await;
}

/// `hash-password` 子命令实现（同步，一次性 CLI；失败 exit(1)）。
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
    //! 全链路（JWT 登录会话 + 版本治理 + RBAC + 路由 + mock 核心）冒烟。
    use std::collections::BTreeMap;
    use std::net::SocketAddr;
    use std::sync::Arc;

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

    /// 测试用 argon2id PHC（低代价参数，验证算法路径与生产一致）。
    fn phc(password: &str) -> String {
        let mut raw = [0u8; 16];
        getrandom::getrandom(&mut raw).unwrap();
        let salt = SaltString::encode_b64(&raw).unwrap();
        // m=4MiB/t=1/p=1：低于生产建议但足以覆盖 argon2id 校验路径。
        let arg = Argon2::new(
            Algorithm::Argon2id,
            Version::V0x13,
            Params::new(4096, 1, 1, None).unwrap(),
        );
        arg.hash_password(password.as_bytes(), &salt).unwrap().to_string()
    }

    fn app() -> (axum::Router, Arc<AppState<MockCore>>) {
        // 两个本地账号：u0=L0 只读、u1=L1 操作。
        let accounts = format!("L0:u0:{};L1:u1:{}", phc("pw0"), phc("pw1"));
        app_with(Arc::new(AuthSvc::for_test(&accounts)))
    }

    /// 用自定义 AuthSvc（锁定/限流阈值、审计文件）装配全链路 app。
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
            // hangup：c1 成功；其余 id 不存在 → 核心 404 语义
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
            data_pushed: Arc::new(AtomicBool::new(false)),
            calls_tx: tokio::sync::broadcast::channel(256).0,
        vad_tx:   tokio::sync::broadcast::channel(1024).0,
        });
        (app_router(st.clone()), st)
    }

    /// 发请求：默认带版本头；bearer/JSON 体可选。返回（状态码，响应体）。
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

        // /health 无鉴权无版本头
        assert_eq!(call(&app, Method::GET, "/health", None, None, false).await.0, StatusCode::OK);

        // 登录面也受版本治理：缺版本头 → 400
        assert_eq!(
            call(&app, Method::POST, "/api/v1/auth/login", None, Some("{}"), false)
                .await
                .0,
            StatusCode::BAD_REQUEST
        );
        // 错误口令 → 统一 401（不区分用户是否存在）
        let (s, _) = login(&app, "u0", "wrong").await;
        assert_eq!(s, StatusCode::UNAUTHORIZED);
        let (s, b) = call(
            &app, Method::POST, "/api/v1/auth/login", None,
            Some(r#"{"username":"ghost","password":"pw0"}"#), true).await;
        assert_eq!(s, StatusCode::UNAUTHORIZED);
        assert!(b.contains("INVALID_CREDENTIALS"), "body={b}");
        // 用户名越白名单 → 400
        let (s, _) = call(
            &app, Method::POST, "/api/v1/auth/login", None,
            Some(r#"{"username":"bad name!","password":"x"}"#), true).await;
        assert_eq!(s, StatusCode::BAD_REQUEST);

        // L0 登录成功，取得 access/refresh
        let (s, v0) = login(&app, "u0", "pw0").await;
        assert_eq!(s, StatusCode::OK);
        assert_eq!(v0["token_type"], "Bearer");
        assert_eq!(v0["expires_in"], 900);
        let (a0, _r0) = tokens(&v0);

        // 无令牌 / 乱码令牌 → 401
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", None, None, true).await.0,
            StatusCode::UNAUTHORIZED
        );
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some("garbage"), None, true).await.0,
            StatusCode::UNAUTHORIZED
        );
        // 版本不匹配仍 fail-closed（带令牌也要过版本层）
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some(&a0), None, false).await.0,
            StatusCode::BAD_REQUEST
        );
        // L0 只读 → 200 真实核心数据；L0 挂断 → 403
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some(&a0), None, true).await.0,
            StatusCode::OK
        );
        // 话务明细同为 L0 只读（rbac 对 Call-ID 末段的特判）
        assert_eq!(
            call(&app, Method::GET, "/api/v1/channels", Some(&a0), None, true).await.0,
            StatusCode::OK
        );
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls/c1", Some(&a0), None, true).await.0,
            StatusCode::OK
        );
        // 配置生效值查询同为 L0 只读（rbac config/ 单段特判）
        assert_eq!(
            call(&app, Method::GET, "/api/v1/config/log.level", Some(&a0), None, true)
                .await
                .0,
            StatusCode::OK
        );
        // 全量脱敏导出同为 L0 只读
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

        // L1 登录 → hangup 200 / notfound 404
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

        // 过期 access → 401 TOKEN_EXPIRED
        let now = crate::auth::tokens::now_secs();
        let ((old_access, _), _) = st.auth.tokens.issue_pair("u1", 1, now - 10_000);
        let (s, b) =
            call(&app, Method::GET, "/api/v1/calls", Some(&old_access), None, true).await;
        assert_eq!(s, StatusCode::UNAUTHORIZED);
        assert!(b.contains("TOKEN_EXPIRED"), "body={b}");

        // 核心不可达（未罐装命令）→ 502，不 500
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
        // access 直接可用
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some(&a1), None, true).await.0,
            StatusCode::OK
        );

        // 刷新：成功轮转，返回新令牌对
        let (s, body) = call(
            &app, Method::POST, "/api/v1/auth/refresh", None,
            Some(&format!(r#"{{"refresh_token":"{r1}"}}"#)), true).await;
        assert_eq!(s, StatusCode::OK, "body={body}");
        let v2: Value = serde_json::from_str(&body).unwrap();
        let (a2, r2) = tokens(&v2);
        assert_ne!(a1, a2);
        // 旧 refresh 单次使用：再出示 → 401 TOKEN_REPLAY
        let (s, b) = call(
            &app, Method::POST, "/api/v1/auth/refresh", None,
            Some(&format!(r#"{{"refresh_token":"{r1}"}}"#)), true).await;
        assert_eq!(s, StatusCode::UNAUTHORIZED);
        assert!(b.contains("TOKEN_REPLAY"), "body={b}");
        // 新 access/refresh 可用
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some(&a2), None, true).await.0,
            StatusCode::OK
        );

        // 登出：吊销当前 access + refresh
        let (s, _) = call(
            &app, Method::POST, "/api/v1/auth/logout", Some(&a2),
            Some(&format!(r#"{{"refresh_token":"{r2}"}}"#)), true).await;
        assert_eq!(s, StatusCode::OK);
        // access 即时失效（不必等 15min）
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some(&a2), None, true).await.0,
            StatusCode::UNAUTHORIZED
        );
        // refresh 已注销 → 401
        let (s, _) = call(
            &app, Method::POST, "/api/v1/auth/refresh", None,
            Some(&format!(r#"{{"refresh_token":"{r2}"}}"#)), true).await;
        assert_eq!(s, StatusCode::UNAUTHORIZED);

        // 登出幂等：无令牌登出 → 401（受保护端点）；空体不影响
        let (s, _) = call(
            &app, Method::POST, "/api/v1/auth/logout", Some(&a2),
            Some("{}"), true).await;
        assert_eq!(s, StatusCode::UNAUTHORIZED);

        // 拿 refresh 当 access 用 → 类型混用 401
        let (_, v3) = login(&app, "u0", "pw0").await;
        let (_, r3) = tokens(&v3);
        assert_eq!(
            call(&app, Method::GET, "/api/v1/calls", Some(&r3), None, true).await.0,
            StatusCode::UNAUTHORIZED
        );
        // r3 未被消费，仍可正常刷新
        let (s, _) = call(
            &app, Method::POST, "/api/v1/auth/refresh", None,
            Some(&format!(r#"{{"refresh_token":"{r3}"}}"#)), true).await;
        assert_eq!(s, StatusCode::OK);
    }

    #[tokio::test]
    async fn lockout_rate_limit_unknown_fields_and_audit() {
        use crate::auth::audit::AuditLog;
        use crate::auth::limiter::Limits;

        // 审计落临时文件，验证全链路事件。
        let apath = std::env::temp_dir().join(format!(
            "nsbc_w3b_audit_{}_{}.jsonl",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
        ));
        let audit = AuditLog::open(apath.to_str().unwrap()).unwrap();
        let accounts = format!("L0:u0:{};L1:u1:{}", phc("pw0"), phc("pw1"));
        // login 桶 6/min、锁定阈值 3，便于在小序列内同时覆盖。
        let limits = Limits {
            lock_threshold: 3,
            rl_login: 6,
            ..Limits::default()
        };
        let (app, _st) = app_with(Arc::new(AuthSvc::build(&accounts, limits, audit)));

        // 1) REQ-WEB-009 拒绝未知字段 → 422（消耗 1 次 login 配额）
        let (s, _) = call(
            &app, Method::POST, "/api/v1/auth/login", None,
            Some(r#"{"username":"u0","password":"pw0","bogus":1}"#), true).await;
        assert_eq!(s, StatusCode::UNPROCESSABLE_ENTITY);

        // 2) 连续 3 次错口令：均 401；第 3 次触发锁定（当次仍 401）
        for _ in 0..3 {
            let (s, b) = login(&app, "u0", "wrong").await;
            assert_eq!(s, StatusCode::UNAUTHORIZED);
            assert_eq!(b["error"]["code"], "INVALID_CREDENTIALS");
        }
        // 3) 锁定后即使口令正确也 423（且不做 argon2 校验）
        let (s, b) = login(&app, "u0", "pw0").await;
        assert_eq!(s, StatusCode::LOCKED);
        assert_eq!(b["error"]["code"], "ACCOUNT_LOCKED");
        // 4) 锁定按用户名：u1 不受影响（本次为第 6 次 login 请求，恰好用满配额）
        let (s, _) = login(&app, "u1", "pw1").await;
        assert_eq!(s, StatusCode::OK);
        // 5) 第 7 次 login → 429 且带 Retry-After
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

        // 审计文件：至少包含 login_failed / account_locked / login_locked /
        // login_success 四类事件，且固定字段齐全。
        let text = std::fs::read_to_string(&apath).unwrap();
        for ev in ["login_failed", "account_locked", "login_locked", "login_success"] {
            assert!(text.contains(&format!("\"event\":\"{ev}\"")), "missing {ev}: {text}");
        }
        assert!(text.contains("\"sub\":\"u1\""));
        std::fs::remove_file(&apath).unwrap();
    }
}
