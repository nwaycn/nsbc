//! NSBC Web configuration loading (REQ-WEB-006).
//!
//! Precedence: **environment variables > config file > built-in defaults**.
//! The config file is shared with nsbcd (same YAML — default `./conf/nsbc.yaml` or
//! `/etc/nsbc/nsbc.yaml`); web reads `web:`, `auth:`, `db:`, `core:`, `audit:` sections;
//! nsbcd ignores these.
//!
//! Why YAML instead of TOML: nsbcd already uses YAML config (key/value style like
//! `log.path`); a unified format means operators only need to maintain one file.

use serde::Deserialize;
use std::net::SocketAddr;
use std::path::PathBuf;

/// Full Web configuration (deserialized from YAML + environment variable overrides).
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
    /// TLS toggle. Recommend true for dev/test (WebRTC requires HTTPS); may be
    /// temporarily set false for LAN-only debugging.
    pub enabled: bool,
    /// Certificate PEM path (required when enabled=true, unless auto_generate=true)
    pub cert_path: PathBuf,
    /// Private key PEM path
    pub key_path:  PathBuf,
    /// Auto-generate self-signed certs when cert/key files are missing (convenient for dev;
    /// configure manually for production)
    pub auto_generate: bool,
    /// Self-signed cert validity (days). Default 2 (very short + auto background renewal,
    /// security first).
    pub validity_days: u32,
    /// Self-signed cert SAN list (comma-separated); empty = auto-fill with bind host
    pub san: String,

    // ===== Public ACME mode (experimental — needs certmagic dependency; currently
    // falls back to self-signed) =====

    /// true = enable public ACME certs (Let's Encrypt/ZeroSSL). Requires public_ip + domain
    /// reachable on 80/443.
    /// false = LAN self-signed cert (default).
    pub public: bool,
    /// Real domain name for public mode (e.g. "nsbc.example.com"). Required when public=true.
    pub domain: String,
    /// ACME account notification email (Let's Encrypt expiry mail, revocation notices, etc.).
    /// May be empty but recommended.
    pub acme_email: String,
    /// ACME account + certificate cache directory (prevents re-requesting on every restart,
    /// which would trigger rate limits).
    pub acme_cache_dir: String,
    /// true = use Let's Encrypt staging (for testing, avoids hitting the production
    /// 5/week/domain limit)
    pub acme_staging: bool,

    // ===== Internal self-signed renewal parameters =====

    /// Background renewal check interval (seconds). Default 3600 (checks once per hour).
    pub check_interval_secs: u64,
    /// Trigger re-signing when hours remaining before expiry ≤ this value. Default 24
    /// (2-day validity → renew when 1 day remains).
    pub renew_threshold_hours: u64,
}

impl Default for TlsSection {
    fn default() -> Self {
        Self {
            enabled: true,
            cert_path:  "/etc/nsbc/web-cert.pem".into(),
            key_path:   "/etc/nsbc/web-key.pem".into(),
            auto_generate: true,
            validity_days: 2,                      // Default 2 days
            san: String::new(),

            public: false,                          // Default: LAN
            domain: String::new(),
            acme_email: String::new(),
            acme_cache_dir: "/var/lib/nsbc/acme-cache".into(),
            acme_staging: false,

            check_interval_secs: 3600,              // 1h
            renew_threshold_hours: 24,              // Renew 24h before expiry
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct AuthSection {
    /// Explicit account string (ROLE:user:PHC, semicolon-separated); empty = inject defaults
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

/// Recording root directory (nsbcd writes files; nsbc-web reads them to serve for
/// browser download). Read from `recording.path` in the shared YAML; default
/// `/var/lib/nsbc/recordings` (aligned with C-side cfg_schema.c).
/// Supports relative paths (resolved relative to the YAML file directory).
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
    /// Load configuration: find config file first → resolve relative paths to absolute →
    /// then apply environment variable overrides.
    ///
    /// **Relative path resolution rules**: every path field that is a relative path
    /// is resolved relative to the YAML file directory (not the process cwd). This way the
    /// entire deployment directory can be freely moved without changing the YAML.
    pub fn load(config_path: Option<&str>) -> Result<Self, String> {
        // 1. Find config file
        let found_path = find_config(config_path);
        let (mut cfg, cfg_dir) = match found_path {
            Some(p) => {
                let content = std::fs::read_to_string(&p)
                    .map_err(|e| format!("failed to read {}: {e}", p.display()))?;
                let c: WebConfig = serde_yaml::from_str(&content)
                    .map_err(|e| format!("failed to parse {}: {e}", p.display()))?;
                // YAML directory (used for subsequent relative path resolution)
                // Must canonicalize the ".." concatenated paths from find_config!
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

        // 2. Relative paths → absolute paths
        tracing::info!("🔧 [resolve] cfg_dir = {:?}", cfg_dir.as_ref().map(|d| d.display().to_string()));

        // Get exe_dir (binary directory) — recording.path resolves relative to it,
        // other paths resolve relative to cfg_dir (YAML directory)
        let exe_dir = std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().and_then(|d| std::fs::canonicalize(d).ok()));
        tracing::info!("🔧 [resolve] exe_dir = {:?}", exe_dir.as_ref().map(|d| d.display().to_string()));

        if let Some(ref dir) = cfg_dir {
            cfg.resolve_relative_paths(dir, exe_dir.as_deref());
        } else if let Some(ref edir) = exe_dir {
            cfg.resolve_recording_path_only(edir);
        }

        // 3. Environment variable overrides (env > file; env values are typically absolute,
        // so no resolve needed)
        let cfg = apply_env_overrides(cfg);
        tracing::info!("🔧 [resolve] after env override db.dsn = {:?}", cfg.db.dsn);

        Ok(cfg)
    }

    /// Resolve path-like fields from relative paths to absolute paths.
    /// - tls / uds / audit / db.dsn  → relative to cfg_dir (YAML directory)
    /// - recording.path              → relative to exe_dir (nsbc-web binary directory)
    ///                                  absolute paths are returned unchanged
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

        // DB dsn: the path portion inside sqlite:///xxx
        if !self.db.dsn.is_empty() {
            self.db.dsn = resolve_dsn_path(&self.db.dsn, cfg_dir);
        }

        // Recording root directory — relative to exe_dir (nsbc-web binary directory)
        // Absolute paths returned unchanged (resolve_path handles that internally)
        if !self.recording.path.is_empty() {
            let base = exe_dir.unwrap_or(cfg_dir);
            self.recording.path = resolve_string_path(&self.recording.path, base);
        }
    }

    /// When there is no YAML fallback (all defaults), only resolve recording.path
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

/// Resolve a relative path to absolute (relative to cfg_dir), **also normalize `..` and `.`**.
/// cfg_dir must be an already-canonicalized absolute path.
/// Absolute paths are returned unchanged. An empty Path returns cfg_dir itself.
fn resolve_path(p: &std::path::Path, cfg_dir: &std::path::Path) -> std::path::PathBuf {
    if p.as_os_str().is_empty() {
        return cfg_dir.to_path_buf();
    }
    if p.is_absolute() {
        return p.to_path_buf();
    }

    // Manual normalize: count leading "../", walk up cfg_dir by that many levels
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
            None => break,  // Already at root
        }
    }
    if remaining.is_empty() { base } else { base.join(&remaining) }
}

/// String version of path resolution (for String fields like audit.file).
fn resolve_string_path(s: &str, cfg_dir: &std::path::Path) -> String {
    let p = std::path::Path::new(s);
    resolve_path(p, cfg_dir).to_string_lossy().into_owned()
}

/// UDS socket path: supports both "unix:<path>" and plain path forms.
fn resolve_uds_path(s: &str, cfg_dir: &std::path::Path) -> String {
    if let Some(rest) = s.strip_prefix("unix:") {
        format!("unix:{}", resolve_string_path(rest, cfg_dir))
    } else {
        resolve_string_path(s, cfg_dir)
    }
}

/// SQLite DSN path resolution: only resolve relative paths; absolute paths kept as-is.
///
/// Judgment rules (**path_part content takes priority**, number of slashes is only a hint):
///
///   Needs resolution (relative):
///     - path_part starts with "../" or "./", or equals "."
///     - path_part contains "/../" or "/./" or similar intermediate ".." components
///     - leading_slashes < 3 (like sqlite://rel/path with 2 slashes)
///
///   Does NOT need resolution (pure absolute):
///     - leading_slashes ≥ 3 **and** path_part contains neither ".." nor "."
///       Example: sqlite:///home/NSBC/web.db  → kept unchanged
///
///   Unified output: absolute paths get 3-slash format sqlite:///abs/path
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

        // path_part contains .. or . → must resolve (regardless of slash count)
        // path_part starts with / → pure absolute path, just add leading slashes
        let has_dotdot = path_part.contains("..");
        let has_dot    = path_part.contains('/') && path_part.contains("./");
        let is_rel     = has_dotdot || has_dot || leading_slashes < 3;

        tracing::info!("🔧 [resolve_dsn_path] has_dotdot={} has_dot={} leading_slashes={} → is_rel={}",
                       has_dotdot, has_dot, leading_slashes, is_rel);

        if is_rel {
            // Resolve relative path to absolute (resolve_string_path returns absolute with
            // leading /)
            let resolved = resolve_string_path(path_part, cfg_dir);
            let out = format!("sqlite://{}", resolved);
            tracing::info!("🔧 [resolve_dsn_path] → RELATIVE resolved to {:?}, output = {:?}", resolved, out);
            out
        } else {
            // Pure absolute → unified 3-slash format
            let out = format!("sqlite:///{}", path_part);
            tracing::info!("🔧 [resolve_dsn_path] → ABSOLUTE passthrough, output = {:?}", out);
            out
        }
    } else {
        tracing::info!("🔧 [resolve_dsn_path] → non-sqlite scheme, passthrough");
        dsn.to_string()
    }
}

/// Config file search order (aligned with nsbcd (C) side, ensuring same-level deployment
/// shares conf/nsbc.yaml):
///   1. Explicit -c argument
///   2. <exe_dir>/conf/nsbc.yaml      ← same-level deployment: nsbcd + nsbc-web + conf/ all
///      in the same directory
///   3. <exe_dir>/../conf/nsbc.yaml   ← backward compatibility with old web/ sub-dir deployment
///   4. ./conf/nsbc.yaml              ← current working directory
///   5. /etc/nsbc/nsbc.yaml           ← system-level fallback
///   6. None → all built-in defaults
fn find_config(explicit: Option<&str>) -> Option<PathBuf> {
    if let Some(p) = explicit {
        let pb = PathBuf::from(p);
        if pb.exists() { return Some(pb); }
        tracing::warn!("explicit config {} not found", p);
        return None;
    }

    // 2. exe_dir-related paths (Rust std::env::current_exe is equivalent to C's
    // /proc/self/exe)
    let exe_candidates: Vec<PathBuf> = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .map(|exe_dir| {
            vec![
                // Same-level deployment: exe_dir = /home/NSBC → /home/NSBC/conf/nsbc.yaml
                exe_dir.join("conf").join("nsbc.yaml"),
                // Old web/ sub-dir deployment: exe_dir = /home/NSBC/web →
                // /home/NSBC/web/../conf/nsbc.yaml
                exe_dir.join("..").join("conf").join("nsbc.yaml"),
            ]
        })
        .unwrap_or_default();

    // 3+4+5. Relative paths + system-level fallback
    let mut candidates: Vec<PathBuf> = exe_candidates;
    candidates.push(PathBuf::from("./conf/nsbc.yaml"));
    candidates.push(PathBuf::from("/etc/nsbc/nsbc.yaml"));

    for pb in candidates {
        if pb.exists() { return Some(pb); }
    }
    None
}

/// Environment variable overrides for config file values (env > file).
/// Unset env vars keep the config file value.
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
