//! Auth foundation (REQ-WEB-008 P0, W3a).
//!
//! Composition:
//!   - [`accounts`]: local accounts (argon2id PHC), env/file config, fail-closed
//!   - [`tokens`]: HS256 compact JWT sign/verify (access 15min / refresh 8h by default)
//!   - [`sessions`]: refresh server-side state (rotation/replay detection) + access revocation table
//!
//! W3b additions: login failure lockout + login/refresh/API three-bucket per-IP rate limiting (limiter),
//! independent JSONL security audit (audit), password policy and hash-password ops subcommand.
//! Still deferred: password history / mandatory first-login password change (needs account state storage), per-IP connection limit and
//! auto-ban (transport layer), OIDC/LDAP/TOTP (SRS marked "optional"),
//! mTLS certificate identity mapping (REQ-WEB-007).

pub mod accounts;
pub mod audit;
pub mod captcha;
pub mod limiter;
pub mod sessions;
pub mod tokens;

use std::time::Duration;

// Internal private use within module; only re-exports whitelist validation directly used by routing layer.
use accounts::AccountStore;
use audit::AuditLog;
use limiter::{AbuseGuard, Limits};
use sessions::SessionStore;
use tokens::{Claims, TokenSvc};
pub use accounts::valid_username;
pub use audit::PeerCtx;

/// Same scale as core L0/L1/L2 (REQ-OAM-008); value is the JWT role claim.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Role {
    L0 = 0,
    L1 = 1,
    L2 = 2,
}

impl Role {
    pub fn parse(s: &str) -> Option<Role> {
        match s {
            "L0" => Some(Role::L0),
            "L1" => Some(Role::L1),
            "L2" => Some(Role::L2),
            _ => None,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Role::L0 => "L0",
            Role::L1 => "L1",
            Role::L2 => "L2",
        }
    }

    pub fn as_u8(self) -> u8 {
        self as u8
    }

    pub fn from_u8(v: u8) -> Option<Role> {
        match v {
            0 => Some(Role::L0),
            1 => Some(Role::L1),
            2 => Some(Role::L2),
            _ => None,
        }
    }
}

/// Authenticated request identity (injected into request extensions after rbac check passes).
#[derive(Clone, Debug)]
pub struct Identity(pub Claims);

/// All runtime components of the auth domain, injected via a single AppState field.
pub struct AuthSvc {
    pub accounts: AccountStore,
    pub tokens: TokenSvc,
    pub sessions: SessionStore,
    pub guard: AbuseGuard,
    pub audit: AuditLog,
    pub password: accounts::PasswordPolicy,
    pub captcha: captcha::CaptchaStore,
}

impl AuthSvc {
    /// Default TTL values. Admin console sessions must stay alive long enough
    /// for real operations — 15min is way too short for an ops engineer editing
    /// dialplans, reviewing CDRs, then switching to profiles.
    const DEFAULT_ACCESS_TTL_SECS: u64 = 14_400;   // 4 hours
    const DEFAULT_REFRESH_TTL_SECS: u64 = 604_800; // 7 days

    /// Resolve TTL: env var > YAML cfg > built-in default.
    /// Clamped to sensible minima (60s access, 300s refresh) regardless of source.
    fn resolve_ttl_secs(
        env_key: &str,
        cfg_val: Option<u64>,
        default: u64,
        min: u64,
    ) -> Result<u64, String> {
        if let Ok(v) = std::env::var(env_key) {
            let n = v.parse::<u64>()
                .map_err(|_| format!("{env_key} must be a non-negative integer"))?;
            return Ok(n.max(min));
        }
        if let Some(v) = cfg_val {
            return Ok(v.max(min));
        }
        Ok(default)
    }

    /// Environment configuration (fail-closed; refuse to start if any security config is missing/invalid):
    /// - `NSBC_JWT_SECRET` or `NSBC_JWT_SECRET_FILE` (>=32 bytes, one of them required)
    /// - `NSBC_AUTH_ACCOUNTS` or `NSBC_AUTH_FILE` (account text, empty=all logins rejected)
    /// - `NSBC_ACCESS_TTL_SECS` (default 14400 = 4h), `NSBC_REFRESH_TTL_SECS` (default 604800 = 7d)
    /// - Lockout/rate-limit: `NSBC_LOCKOUT_THRESHOLD`(5)/`NSBC_LOCKOUT_SECS`(900)/
    ///   `NSBC_LOCKOUT_WINDOW_SECS`(900)/
    ///   `NSBC_RL_LOGIN_PER_MIN`(10)/`NSBC_RL_REFRESH_PER_MIN`(30)/
    ///   `NSBC_RL_API_PER_MIN`(120); threshold 0 disables the respective mechanism
    /// - `NSBC_AUDIT_FILE` (audit JSONL; empty=disabled with warn, must be configured for production)
    /// - `NSBC_PASSWORD_MIN_LEN` (default 12, floored to 8)
    pub fn from_env() -> Result<AuthSvc, String> {
        let secret = env_bytes_or_file("NSBC_JWT_SECRET", "NSBC_JWT_SECRET_FILE")?
            .ok_or_else(|| "set NSBC_JWT_SECRET or NSBC_JWT_SECRET_FILE (>=32 bytes)".to_string())?;
        let accounts_text =
            env_text_or_file("NSBC_AUTH_ACCOUNTS", "NSBC_AUTH_FILE")?.unwrap_or_default();
        let accounts = if accounts_text.trim().is_empty() {
            AccountStore::with_defaults()?
        } else {
            AccountStore::parse(&accounts_text)?
        };
        Self::build_from_parts(secret, accounts, None, None)
    }

    /// Construct from WebConfig::AuthSection (env > file > cfg > fail-closed).
    pub fn from_config(cfg: &super::config::AuthSection) -> Result<AuthSvc, String> {
        // JWT secret: env/file > cfg.jwt_secret (YAML) > fail-closed
        // Production: prefer env NSBC_JWT_SECRET; YAML value acceptable but don't commit it.
        let secret = env_bytes_or_file("NSBC_JWT_SECRET", "NSBC_JWT_SECRET_FILE")?
            .or_else(|| {
                if !cfg.jwt_secret.trim().is_empty() {
                    Some(cfg.jwt_secret.as_bytes().to_vec())
                } else {
                    None
                }
            })
            .ok_or_else(|| {
                "NSBC_JWT_SECRET / NSBC_JWT_SECRET_FILE or auth.jwt_secret in YAML \
                 must be set (>=32 bytes). Generate: openssl rand -hex 32".to_string()
            })?;

        // Accounts: env > cfg > defaults
        let accounts_text = env_text_or_file("NSBC_AUTH_ACCOUNTS", "NSBC_AUTH_FILE")
            ?.unwrap_or_default();
        let accounts_text = if accounts_text.trim().is_empty() && !cfg.accounts.trim().is_empty() {
            cfg.accounts.clone()
        } else {
            accounts_text
        };

        let accounts = if accounts_text.trim().is_empty() {
            // Default account injection — cfg default_*_pw overrides built-in defaults
            if let Some(pw) = std::env::var("NSBC_DEFAULT_ADMIN_PW").ok() {
                std::env::set_var("NSBC_DEFAULT_ADMIN_PW", &pw);
            } else if !cfg.default_admin_pw.is_empty() {
                std::env::set_var("NSBC_DEFAULT_ADMIN_PW", &cfg.default_admin_pw);
            }
            if let Some(pw) = std::env::var("NSBC_DEFAULT_DEV_PW").ok() {
                std::env::set_var("NSBC_DEFAULT_DEV_PW", &pw);
            } else if !cfg.default_dev_pw.is_empty() {
                std::env::set_var("NSBC_DEFAULT_DEV_PW", &cfg.default_dev_pw);
            }
            AccountStore::with_defaults()?
        } else {
            AccountStore::parse(&accounts_text)?
        };

        Self::build_from_parts(
            secret, accounts,
            cfg.access_ttl_secs, cfg.refresh_ttl_secs,
        )
    }

    /// Build from resolved components.
    ///
    /// TTL resolution chain (per param, independent):
    ///   env `NSBC_ACCESS_TTL_SECS` / `NSBC_REFRESH_TTL_SECS` >
    ///   cfg Option<u64> (from YAML) >
    ///   DEFAULT_ACCESS_TTL_SECS / DEFAULT_REFRESH_TTL_SECS
    /// Minimum clamped: access ≥ 60s, refresh ≥ 300s.
    fn build_from_parts(
        secret: Vec<u8>,
        accounts: AccountStore,
        cfg_access_ttl: Option<u64>,
        cfg_refresh_ttl: Option<u64>,
    ) -> Result<AuthSvc, String> {
        if accounts.is_empty() {
            tracing::warn!(
                "no local accounts configured; all logins will fail closed"
            );
        }
        let access_secs = Self::resolve_ttl_secs(
            "NSBC_ACCESS_TTL_SECS", cfg_access_ttl,
            Self::DEFAULT_ACCESS_TTL_SECS, 60,
        )?;
        let refresh_secs = Self::resolve_ttl_secs(
            "NSBC_REFRESH_TTL_SECS", cfg_refresh_ttl,
            Self::DEFAULT_REFRESH_TTL_SECS, 300,
        )?;
        let access_ttl = Duration::from_secs(access_secs);
        let refresh_ttl = Duration::from_secs(refresh_secs);
        let tokens = TokenSvc::new(&secret, access_ttl, refresh_ttl)?;
        let lock_window_secs = env_u64_or("NSBC_LOCKOUT_WINDOW_SECS", 900)?;
        if lock_window_secs == 0 {
            return Err("NSBC_LOCKOUT_WINDOW_SECS must be greater than 0".into());
        }
        let limits = Limits {
            lock_threshold: env_u64_or("NSBC_LOCKOUT_THRESHOLD", 5)? as u32,
            lock_secs: env_u64_or("NSBC_LOCKOUT_SECS", 900)?,
            lock_window_secs,
            rl_login: env_u64_or("NSBC_RL_LOGIN_PER_MIN", 10)? as u32,
            rl_refresh: env_u64_or("NSBC_RL_REFRESH_PER_MIN", 30)? as u32,
            rl_api: env_u64_or("NSBC_RL_API_PER_MIN", 120)? as u32,
            window_secs: 60,
        };
        let audit = match std::env::var("NSBC_AUDIT_FILE") {
            Ok(p) if !p.is_empty() => AuditLog::open(&p)?,
            _ => {
                tracing::warn!(
                    "NSBC_AUDIT_FILE not set; security audit log disabled \
                     (configure an append-only file for production)"
                );
                AuditLog::disabled()
            }
        };
        let password = accounts::PasswordPolicy::from_env()?;
        Ok(AuthSvc {
            accounts,
            tokens,
            sessions: SessionStore::new(),
            guard: AbuseGuard::new(limits),
            audit,
            password,
            captcha: captcha::CaptchaStore::new(),
        })
    }

    #[cfg(test)]
    pub(crate) fn build(accounts_text: &str, limits: Limits, audit: AuditLog) -> AuthSvc {
        let accounts = AccountStore::parse(accounts_text).expect("valid test accounts");
        let tokens = TokenSvc::new(
            b"unit-test-secret-012456789abcdef-32B!!",
            Duration::from_secs(900),
            Duration::from_secs(28800),
        )
        .expect("test key valid");
        AuthSvc {
            accounts,
            tokens,
            sessions: SessionStore::new(),
            guard: AbuseGuard::new(limits),
            audit,
            password: accounts::PasswordPolicy::default(),
            captcha: captcha::CaptchaStore::new(),
        }
    }

    /// Test config: given account text and fixed keys, production default thresholds (visible only in cfg(test)).
    #[cfg(test)]
    pub(crate) fn for_test(accounts_text: &str) -> AuthSvc {
        AuthSvc::build(accounts_text, Limits::default(), AuditLog::disabled())
    }

    #[cfg(test)]
    pub(crate) fn for_test_empty() -> AuthSvc {
        AuthSvc::for_test("")
    }
}

/// Read raw bytes from `$env` or from the file pointed to by `$env_file` (only strip trailing newlines, leave other chars untouched).
fn env_bytes_or_file(env: &str, file_env: &str) -> Result<Option<Vec<u8>>, String> {
    if let Ok(v) = std::env::var(env) {
        return Ok(Some(v.into_bytes()));
    }
    if let Ok(path) = std::env::var(file_env) {
        let mut data = std::fs::read(&path).map_err(|e| format!("read {path}: {e}"))?;
        while matches!(data.last(), Some(b'\n') | Some(b'\r')) {
            data.pop();
        }
        return Ok(Some(data));
    }
    Ok(None)
}

fn env_text_or_file(env: &str, file_env: &str) -> Result<Option<String>, String> {
    Ok(match env_bytes_or_file(env, file_env)? {
        Some(b) => Some(String::from_utf8(b).map_err(|_| "non-UTF8 auth config".to_string())?),
        None => None,
    })
}

fn env_u64_or(key: &str, default: u64) -> Result<u64, String> {
    match std::env::var(key) {
        Ok(v) => v
            .parse::<u64>()
            .map_err(|_| format!("{key} must be a non-negative integer")),
        Err(_) => Ok(default),
    }
}
