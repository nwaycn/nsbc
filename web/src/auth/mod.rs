//! 认证基座（REQ-WEB-008 P0，W3a）。
//!
//! 组成：
//!   - [`accounts`]：本地账号（argon2id PHC），环境/文件装配，fail-closed
//!   - [`tokens`]：HS256 紧凑 JWT 签发/校验（access 15min / refresh 8h 默认）
//!   - [`sessions`]：refresh 服务端状态（轮转/重放检测）+ access 吊销表
//!
//! W3b 增补：登录失败锁定 + login/refresh/API 三桶 per-IP 限流（limiter）、
//! 独立 JSONL 安全审计（audit）、口令策略与 hash-password 运维子命令。
//! 仍后置：历史口令/强制首登改密（需账号状态存储）、单 IP 连接数上限与
//! 自动封禁（传输层）、OIDC/LDAP/TOTP（SRS 明示"可选"）、
//! mTLS 证书身份映射（REQ-WEB-007）。

pub mod accounts;
pub mod audit;
pub mod captcha;
pub mod limiter;
pub mod sessions;
pub mod tokens;

use std::time::Duration;

// 模块内自用走私有 use；对外仅重导出路由层直接使用的白名单校验。
use accounts::AccountStore;
use audit::AuditLog;
use limiter::{AbuseGuard, Limits};
use sessions::SessionStore;
use tokens::{Claims, TokenSvc};
pub use accounts::valid_username;
pub use audit::PeerCtx;

/// 与核心 L0/L1/L2 同口径（REQ-OAM-008）；数值即 JWT role 声明。
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

/// 鉴权后的请求身份（rbac 校验通过后注入 request extensions）。
#[derive(Clone, Debug)]
pub struct Identity(pub Claims);

/// 认证域全部运行态组件，经 AppState 单字段注入。
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
    /// 环境装配（fail-closed；任一安全配置缺失/非法即拒绝启动）：
    /// - `NSBC_JWT_SECRET` 或 `NSBC_JWT_SECRET_FILE`（≥32 字节，二选一必填）
    /// - `NSBC_AUTH_ACCOUNTS` 或 `NSBC_AUTH_FILE`（账号文本，可空=登录全拒）
    /// - `NSBC_ACCESS_TTL_SECS`（默认 900）、`NSBC_REFRESH_TTL_SECS`（默认 28800）
    /// - 锁定/限流：`NSBC_LOCKOUT_THRESHOLD`(5)/`NSBC_LOCKOUT_SECS`(900)/
    ///   `NSBC_RL_LOGIN_PER_MIN`(10)/`NSBC_RL_REFRESH_PER_MIN`(30)/
    ///   `NSBC_RL_API_PER_MIN`(120)，阈值 0=关闭对应机制
    /// - `NSBC_AUDIT_FILE`（审计 JSONL；空=关闭并 warn，生产必须配置）
    /// - `NSBC_PASSWORD_MIN_LEN`（默认 12，下限钳 8）
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
        Self::build_from_parts(secret, accounts)
    }

    /// 从 WebConfig::AuthSection 构造（env > file > defaults）。
    pub fn from_config(cfg: &super::config::AuthSection) -> Result<AuthSvc, String> {
        // JWT secret: env > cfg > fail
        let secret = env_bytes_or_file("NSBC_JWT_SECRET", "NSBC_JWT_SECRET_FILE")?;
        let secret = if secret.is_none() && !cfg.jwt_secret.is_empty() {
            Some(cfg.jwt_secret.as_bytes().to_vec())
        } else {
            secret
        }.ok_or_else(|| {
            "set NSBC_JWT_SECRET / NSBC_JWT_SECRET_FILE, or web.auth.jwt_secret in config (>=32 bytes)"
                .to_string()
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
            // 默认账号注入——cfg 里的 default_*_pw 覆盖内置默认
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

        Self::build_from_parts(secret, accounts)
    }

    fn build_from_parts(secret: Vec<u8>, accounts: AccountStore) -> Result<AuthSvc, String> {
        if accounts.is_empty() {
            tracing::warn!(
                "no local accounts configured; all logins will fail closed"
            );
        }
        let access_ttl = Duration::from_secs(
            env_u64_or("NSBC_ACCESS_TTL_SECS", 900)?.max(60),
        );
        let refresh_ttl = Duration::from_secs(
            env_u64_or("NSBC_REFRESH_TTL_SECS", 28800)?.max(300),
        );
        let tokens = TokenSvc::new(&secret, access_ttl, refresh_ttl)?;
        let limits = Limits {
            lock_threshold: env_u64_or("NSBC_LOCKOUT_THRESHOLD", 5)? as u32,
            lock_secs: env_u64_or("NSBC_LOCKOUT_SECS", 900)?,
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

    /// 测试装配：给定账号文本与固定密钥、生产默认阈值（仅 cfg(test) 可见）。
    #[cfg(test)]
    pub(crate) fn for_test(accounts_text: &str) -> AuthSvc {
        AuthSvc::build(accounts_text, Limits::default(), AuditLog::disabled())
    }

    #[cfg(test)]
    pub(crate) fn for_test_empty() -> AuthSvc {
        AuthSvc::for_test("")
    }
}

/// 取 `$env` 原始字节或 `$env_file` 指向文件的字节（仅去尾部换行，不动其他字符）。
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
