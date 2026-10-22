//! NSBC Web 配置加载（REQ-WEB-006）。
//!
//! 优先级：**环境变量 > 配置文件 > 内置默认**。
//! 配置文件与 nsbcd 共享同一 YAML（默认 `./conf/nsbc.yaml` 或 `/etc/nsbc/nsbc.yaml`），
//! 从中读取 `web:`、`auth:`、`db:`、`core:`、`audit:` 段；nsbcd 忽略这些段。
//!
//! 为什么 YAML 而不是 TOML：nsbcd 已用 YAML 配置（键值对风格如 `log.path`），
//! 统一格式用户只需维护一份文件。

use serde::Deserialize;
use std::net::SocketAddr;
use std::path::PathBuf;

/// 完整 Web 配置（从 YAML 反序列化 + 环境变量覆盖）。
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct WebConfig {
    pub web:     WebSection,
    pub auth:    AuthSection,
    pub db:      DbSection,
    pub core:    CoreSection,
    pub audit:   AuditSection,
    pub recording: RecordingSection,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct WebSection {
    pub enabled: bool,
    pub bind:    String,
    pub tls:     TlsSection,
}

impl Default for WebSection {
    fn default() -> Self {
        Self {
            enabled: true,
            bind:    "127.0.0.1:8443".into(),
            tls:     TlsSection::default(),
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct TlsSection {
    /// TLS 开关。开发测试建议 true（WebRTC 需 HTTPS），纯内网调试可临时 false。
    pub enabled: bool,
    /// 证书 PEM 路径（enable=true 时必填，除非 auto_generate=true）
    pub cert_path: PathBuf,
    /// 私钥 PEM 路径
    pub key_path:  PathBuf,
    /// 证书/私钥文件不存在时自动生成自签证书（开发便利，生产请手动配）
    pub auto_generate: bool,
    /// 自签证书有效期（天）。默认 2（极短 + 后台自动续期，安全优先）
    pub validity_days: u32,
    /// 自签证书 SAN 列表（逗号分隔）；空=自动填 bind 的 host
    pub san: String,

    // ===== 公网 ACME 模式（experimental — 需 certmagic 依赖；当前 fallback 自签） =====

    /// true = 启用公网 ACME 证书（Let's Encrypt/ZeroSSL）。需 public_ip + domain 可达 80/443。
    /// false = 内网自签证书（默认）。
    pub public: bool,
    /// 公网模式下的真实域名（如 "nsbc.example.com"）。public=true 时必填。
    pub domain: String,
    /// ACME 账户通知邮箱（Let's Encrypt 到期邮件、吊销通知等）。可空但建议填。
    pub acme_email: String,
    /// ACME 账户 + 证书缓存目录（避免每次重启都重新申请触发 rate limit）。
    pub acme_cache_dir: String,
    /// true = 用 Let's Encrypt staging（测试时用，避免触发生产 5/week/domain 限制）
    pub acme_staging: bool,

    // ===== 内部自签续期参数 =====

    /// 后台续期检查间隔（秒）。默认 3600（1 小时查一次）。
    pub check_interval_secs: u64,
    /// 证书距到期剩余小时数 ≤ 此值时触发重签。默认 24（2 天有效期 → 1 天就续）。
    pub renew_threshold_hours: u64,
}

impl Default for TlsSection {
    fn default() -> Self {
        Self {
            enabled: true,
            cert_path:  "/etc/nsbc/web-cert.pem".into(),
            key_path:   "/etc/nsbc/web-key.pem".into(),
            auto_generate: true,
            validity_days: 2,                      // 默认 2 天
            san: String::new(),

            public: false,                          // 默认内网
            domain: String::new(),
            acme_email: String::new(),
            acme_cache_dir: "/var/lib/nsbc/acme-cache".into(),
            acme_staging: false,

            check_interval_secs: 3600,              // 1h
            renew_threshold_hours: 24,              // 24h 前续
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct AuthSection {
    /// 显式账号串（ROLE:user:PHC 分号分隔）；空=走默认账号注入
    pub accounts:          String,
    pub default_admin_pw:  String,
    pub default_dev_pw:    String,
    pub jwt_secret:        String,
    pub access_ttl_secs:   Option<u64>,
    pub refresh_ttl_secs: Option<u64>,
    pub audit_file:        String,
}

impl Default for AuthSection {
    fn default() -> Self {
        Self {
            accounts:          String::new(),
            default_admin_pw:  String::new(),
            default_dev_pw:    String::new(),
            jwt_secret:        String::new(),
            access_ttl_secs:   None,
            refresh_ttl_secs: None,
            audit_file:        String::new(),
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct DbSection {
    pub dsn: String,
}

impl Default for DbSection {
    fn default() -> Self { Self { dsn: String::new() } }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct CoreSection {
    pub control_sock: String,
    pub data_sock:    String,
}

impl Default for CoreSection {
    fn default() -> Self {
        Self {
            control_sock: "/var/run/nsbc/control.sock".into(),
            data_sock:    "/var/run/nsbc/data.sock".into(),
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct AuditSection {
    pub file: String,
}

impl Default for AuditSection {
    fn default() -> Self { Self { file: String::new() } }
}

/// 录音根目录（nsbcd 写文件，nsbc-web 读文件给浏览器下载）。
/// 从共享 YAML 的 `recording.path` 读取；默认 `/var/lib/nsbc/recordings`（与 C 侧 cfg_schema.c 对齐）。
/// 支持相对路径（相对于 YAML 所在目录 resolve）。
#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct RecordingSection {
    pub path: String,
}

impl Default for RecordingSection {
    fn default() -> Self {
        Self { path: "/var/lib/nsbc/recordings".to_string() }
    }
}

impl WebConfig {
    /// 加载配置：先找配置文件 → 相对路径 resolve 为绝对路径 → 环境变量覆盖。
    ///
    /// **相对路径解析规则**：所有路径类字段如果是相对路径，都相对于 YAML 文件
    /// 所在目录解析（不是进程 cwd）。这样整个部署目录可自由搬迁，YAML 不变。
    pub fn load(config_path: Option<&str>) -> Result<Self, String> {
        // 1. 找配置文件
        let found_path = find_config(config_path);
        let (mut cfg, cfg_dir) = match found_path {
            Some(p) => {
                let content = std::fs::read_to_string(&p)
                    .map_err(|e| format!("failed to read {}: {e}", p.display()))?;
                let c: WebConfig = serde_yaml::from_str(&content)
                    .map_err(|e| format!("failed to parse {}: {e}", p.display()))?;
                // YAML 所在目录（用于后续相对路径 resolve）
                // 必须 canonicalize 掉 find_config 里拼出来的 ".."！
                let dir = p.parent()
                    .and_then(|d| std::fs::canonicalize(d).ok())
                    .unwrap_or_else(|| p.parent().map(|d| d.to_path_buf()).unwrap_or_default());
                tracing::info!("config loaded from {}", p.display());
                (c, Some(dir))
            }
            None => {
                tracing::info!("no config file found, using defaults + env");
                (WebConfig::default(), None)
            }
        };

        // 2. 相对路径 → 绝对路径
        tracing::info!("🔧 [resolve] cfg_dir = {:?}", cfg_dir.as_ref().map(|d| d.display().to_string()));

        // 拿 exe_dir（二进制所在目录）——recording.path 相对它解析，
        // 其他路径相对 cfg_dir（YAML 所在目录）解析
        let exe_dir = std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().and_then(|d| std::fs::canonicalize(d).ok()));
        tracing::info!("🔧 [resolve] exe_dir = {:?}", exe_dir.as_ref().map(|d| d.display().to_string()));

        if let Some(ref dir) = cfg_dir {
            cfg.resolve_relative_paths(dir, exe_dir.as_deref());
        } else if let Some(ref edir) = exe_dir {
            cfg.resolve_recording_path_only(edir);
        }

        // 3. 环境变量覆盖（env > file，env 通常是绝对路径，不做 resolve）
        let cfg = apply_env_overrides(cfg);
        tracing::info!("🔧 [resolve] after env override db.dsn = {:?}", cfg.db.dsn);

        Ok(cfg)
    }

    /// 把路径类字段从相对路径 resolve 为绝对路径。
    /// - tls / uds / audit / db.dsn  → 相对 cfg_dir（YAML 所在目录）
    /// - recording.path              → 相对 exe_dir（nsbc-web 二进制所在目录）
    ///                                  绝对路径原样返回
    fn resolve_relative_paths(
        &mut self,
        cfg_dir: &std::path::Path,
        exe_dir: Option<&std::path::Path>,
    ) {
        // TLS cert/key
        self.web.tls.cert_path = resolve_path(&self.web.tls.cert_path, cfg_dir);
        self.web.tls.key_path  = resolve_path(&self.web.tls.key_path,  cfg_dir);

        // Core UDS sockets
        self.core.control_sock = resolve_uds_path(&self.core.control_sock, cfg_dir);
        self.core.data_sock    = resolve_uds_path(&self.core.data_sock,    cfg_dir);

        // Audit files
        if !self.auth.audit_file.is_empty() {
            self.auth.audit_file = resolve_string_path(&self.auth.audit_file, cfg_dir);
        }
        if !self.audit.file.is_empty() {
            self.audit.file = resolve_string_path(&self.audit.file, cfg_dir);
        }

        // DB dsn：sqlite:///xxx 里的路径部分
        if !self.db.dsn.is_empty() {
            self.db.dsn = resolve_dsn_path(&self.db.dsn, cfg_dir);
        }

        // 录音根目录 —— 相对 exe_dir（nsbc-web 二进制所在目录）
        // 绝对路径原样返回（resolve_path 内部判断）
        if !self.recording.path.is_empty() {
            let base = exe_dir.unwrap_or(cfg_dir);
            self.recording.path = resolve_string_path(&self.recording.path, base);
        }
    }

    /// 无 YAML fallback（全默认值）时，只 resolve recording.path
    fn resolve_recording_path_only(&mut self, exe_dir: &std::path::Path) {
        if !self.recording.path.is_empty() {
            self.recording.path = resolve_string_path(&self.recording.path, exe_dir);
        }
    }

    pub fn bind_addr(&self) -> Result<SocketAddr, String> {
        self.web.bind
            .parse()
            .map_err(|e| format!("invalid NSBC_WEB_BIND {}: {e}", self.web.bind))
    }
}

/// 把相对路径 resolve 到绝对路径（相对 cfg_dir），**同时 normalize `..` 和 `.`**。
/// cfg_dir 必须是已经 canonicalize 过的绝对路径。
/// 绝对路径原样返回。空 Path 返回 cfg_dir 本身。
fn resolve_path(p: &std::path::Path, cfg_dir: &std::path::Path) -> std::path::PathBuf {
    if p.as_os_str().is_empty() {
        return cfg_dir.to_path_buf();
    }
    if p.is_absolute() {
        return p.to_path_buf();
    }

    // 手动 normalize：数开头有多少个 ../，cfg_dir 向上走同样多的级
    let mut up = 0usize;
    let mut remaining = p.as_os_str().to_string_lossy().into_owned();
    while let Some(new) = remaining.strip_prefix("../") {
        up += 1;
        remaining = new.to_string();
    }
    while let Some(new) = remaining.strip_prefix("./") {
        remaining = new.to_string();
    }
    if remaining == "." { remaining = String::new(); }

    let mut base = cfg_dir.to_path_buf();
    for _ in 0..up {
        match base.parent() {
            Some(parent) => base = parent.to_path_buf(),
            None => break,  // 已经到根了
        }
    }
    if remaining.is_empty() { base } else { base.join(&remaining) }
}

/// String 版路径解析（用于 audit.file 这类 String 字段）。
fn resolve_string_path(s: &str, cfg_dir: &std::path::Path) -> String {
    let p = std::path::Path::new(s);
    resolve_path(p, cfg_dir).to_string_lossy().into_owned()
}

/// UDS socket 路径：支持 "unix:<path>" 和纯路径两种写法。
fn resolve_uds_path(s: &str, cfg_dir: &std::path::Path) -> String {
    if let Some(rest) = s.strip_prefix("unix:") {
        format!("unix:{}", resolve_string_path(rest, cfg_dir))
    } else {
        resolve_string_path(s, cfg_dir)
    }
}

/// SQLite DSN 路径解析：只 resolve 相对路径，绝对路径原样保留。
///
/// 判断规则（**path_part 内容优先**，斜杠数仅作参考）：
///
///   需要 resolve（相对）:
///     - path_part 以 "../" 或 "./" 开头，或等于 "."
///     - path_part 包含 "/../" 或 "/./" 等中间 .. 组件
///     - leading_slashes < 3（sqlite://rel/path 这种 2 斜杠写法）
///
///   不需要 resolve（纯绝对路径）:
///     - leading_slashes ≥ 3 **且** path_part 不含 ".." 也不含 "."
///       例：sqlite:///home/NSBC/web.db  → 原样保留
///
///   输出统一：绝对路径 3 斜杠格式 sqlite:///abs/path
fn resolve_dsn_path(dsn: &str, cfg_dir: &std::path::Path) -> String {
    tracing::info!("🔧 [resolve_dsn_path] input dsn = {:?}, cfg_dir = {:?}", dsn, cfg_dir.display());
    if dsn.contains(":memory:") {
        tracing::info!("🔧 [resolve_dsn_path] → :memory: passthrough");
        return dsn.to_string();
    }
    if let Some(rest) = dsn.strip_prefix("sqlite:") {
        let leading_slashes = rest.chars().take_while(|c| *c == '/').count();
        let path_part = rest.trim_start_matches('/');
        tracing::info!("🔧 [resolve_dsn_path] sqlite rest={:?} leading_slashes={} path_part={:?}",
                       rest, leading_slashes, path_part);

        if path_part.is_empty() {
            tracing::info!("🔧 [resolve_dsn_path] → path_part empty, passthrough");
            return dsn.to_string();
        }

        // path_part 包含 .. 或 . → 必须 resolve（不管斜杠数）
        // path_part 以 / 开头 → 纯绝对路径，直接补斜杠输出
        let has_dotdot = path_part.contains("..");
        let has_dot    = path_part.contains('/') && path_part.contains("./");
        let is_rel     = has_dotdot || has_dot || leading_slashes < 3;

        tracing::info!("🔧 [resolve_dsn_path] has_dotdot={} has_dot={} leading_slashes={} → is_rel={}",
                       has_dotdot, has_dot, leading_slashes, is_rel);

        if is_rel {
            // 相对路径 resolve 成绝对路径（resolve_string_path 返回带前导 / 的绝对路径）
            let resolved = resolve_string_path(path_part, cfg_dir);
            let out = format!("sqlite://{}", resolved);
            tracing::info!("🔧 [resolve_dsn_path] → RELATIVE resolved to {:?}, output = {:?}", resolved, out);
            out
        } else {
            // 纯绝对路径 → 统一输出 3 斜杠格式
            let out = format!("sqlite:///{}", path_part);
            tracing::info!("🔧 [resolve_dsn_path] → ABSOLUTE passthrough, output = {:?}", out);
            out
        }
    } else {
        tracing::info!("🔧 [resolve_dsn_path] → non-sqlite scheme, passthrough");
        dsn.to_string()
    }
}

/// 配置文件搜索顺序（与 nsbcd (C) 侧对齐，确保同级部署共用 conf/nsbc.yaml）：
///   1. 显式 -c 参数
///   2. <exe_dir>/conf/nsbc.yaml      ← 同级部署：nsbcd + nsbc-web + conf/ 都在同一目录
///   3. <exe_dir>/../conf/nsbc.yaml   ← 兼容旧 web/ 子目录部署
///   4. ./conf/nsbc.yaml              ← 当前工作目录
///   5. /etc/nsbc/nsbc.yaml           ← 系统级 fallback
///   6. None → 全部用内置默认值
fn find_config(explicit: Option<&str>) -> Option<PathBuf> {
    if let Some(p) = explicit {
        let pb = PathBuf::from(p);
        if pb.exists() { return Some(pb); }
        tracing::warn!("explicit config {} not found", p);
        return None;
    }

    // 2. exe_dir 相关路径（Rust std::env::current_exe 等价 C 的 /proc/self/exe）
    let exe_candidates: Vec<PathBuf> = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .map(|exe_dir| {
            vec![
                // 同级部署：exe_dir = /home/NSBC → /home/NSBC/conf/nsbc.yaml
                exe_dir.join("conf").join("nsbc.yaml"),
                // 旧 web/ 子目录部署：exe_dir = /home/NSBC/web → /home/NSBC/web/../conf/nsbc.yaml
                exe_dir.join("..").join("conf").join("nsbc.yaml"),
            ]
        })
        .unwrap_or_default();

    // 3+4+5. 相对路径 + 系统级 fallback
    let mut candidates: Vec<PathBuf> = exe_candidates;
    candidates.push(PathBuf::from("./conf/nsbc.yaml"));
    candidates.push(PathBuf::from("/etc/nsbc/nsbc.yaml"));

    for pb in candidates {
        if pb.exists() { return Some(pb); }
    }
    None
}

/// 环境变量覆盖配置文件值（env > file）。
/// 未设的 env 保持配置文件值。
fn apply_env_overrides(mut cfg: WebConfig) -> WebConfig {
    // web.bind
    if let Ok(v) = std::env::var("NSBC_WEB_BIND") {
        cfg.web.bind = v;
    }
    // auth.jwt_secret
    if let Ok(v) = std::env::var("NSBC_JWT_SECRET") {
        cfg.auth.jwt_secret = v;
    }
    // auth.accounts
    if let Ok(v) = std::env::var("NSBC_AUTH_ACCOUNTS") {
        cfg.auth.accounts = v;
    }
    // auth.default_*_pw
    if let Ok(v) = std::env::var("NSBC_DEFAULT_ADMIN_PW") {
        cfg.auth.default_admin_pw = v;
    }
    if let Ok(v) = std::env::var("NSBC_DEFAULT_DEV_PW") {
        cfg.auth.default_dev_pw = v;
    }
    // db.dsn
    if let Ok(v) = std::env::var("NSBC_DB_DSN") {
        cfg.db.dsn = v;
    }
    // core.*_sock
    if let Ok(v) = std::env::var("NSBC_CONTROL_SOCK") {
        cfg.core.control_sock = v;
    }
    if let Ok(v) = std::env::var("NSBC_DATA_SOCK") {
        cfg.core.data_sock = v;
    }
    // audit.file
    if let Ok(v) = std::env::var("NSBC_AUDIT_FILE") {
        cfg.audit.file = v;
    }
    // recording.path
    if let Ok(v) = std::env::var("NSBC_RECORDING_PATH") {
        cfg.recording.path = v;
    }
    cfg
}
