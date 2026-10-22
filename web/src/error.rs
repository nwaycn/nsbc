//! Unified error response (REQ-WEB-009/020): all boundary/auth failures return structured JSON,
//! no internal details leaked (REQ-WEB-018 minimalization).
use axum::http::header::RETRY_AFTER;
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use axum::Json;
use serde_json::json;

#[derive(Debug)]
pub enum ApiError {
    /// API major version missing or unsupported (fail-closed, REQ-WEB-020)
    VersionMismatch { got: Option<String>, supported: u32 },
    /// Unauthenticated (no identity / missing token)
    Unauthenticated,
    /// Bad credentials (username/password; do not distinguish whether user exists, REQ-WEB-018)
    BadCredentials,
    /// Invalid token (wrong signature/structure/type, or revoked)
    TokenInvalid,
    /// Token expired
    TokenExpired,
    /// Refresh token replay (rotated/logged-out jti seen again, REQ-WEB-008)
    TokenReplay,
    /// Account locked due to consecutive login failures (REQ-WEB-004/008 failure lockout)
    AccountLocked,
    /// Per-IP interface rate limit triggered (REQ-WEB-009); payload=remaining seconds in window (Retry-After)
    RateLimited { retry_after: u64 },
    /// Authenticated but insufficient role (L0<L1<L2, REQ-OAM-008)
    Forbidden { required: &'static str },

    /// Core unreachable / I/O / protocol violation (REQ-WEB-014 fault tolerance: explicit 502)
    CoreUnavailable,
    /// Core control-plane timeout (REQ-WEB-014: explicit 504, caller may backoff and retry)
    CoreTimeout,
    /// Invalid request body/parameters
    BadRequest(String),
    /// Target call does not exist (e.g. hangup on already-ended / wrong Call-ID, REQ-OAM-004);
    /// original Call-ID not echoed back (REQ-WEB-018 minimalization).
    CallNotFound,
    /// Config key not found (GET /config/{key}, REQ-CFG-011); do not echo original key value.
    ConfigKeyNotFound,

    /// DB persistence layer not configured (NSBC_DB_DSN missing or connection pool init failed).
    /// All DB-dependent endpoints (CDR/history) return unified 503, explicitly distinguished from core unreachable.
    DbUnavailable,
    /// DB query execution error (SQL syntax/constraint/network); detailed in internal message,
    /// not echoed back to client (REQ-WEB-018 minimalization).
    Db(String),

    /// Resource not found (CRUD GET/PUT/DELETE target id not found).
    NotFound(String),
    /// Resource conflict (UNIQUE constraint violation, e.g. duplicate extension+realm).
    Conflict(String),
    /// Service temporarily unavailable (e.g. captcha store full, backend resources exhausted).
    ServiceUnavailable(String),
}

impl From<crate::auth::tokens::TokenError> for ApiError {
    fn from(e: crate::auth::tokens::TokenError) -> Self {
        match e {
            crate::auth::tokens::TokenError::Expired => ApiError::TokenExpired,
            // Malformed signature/structure/type all map to 401 TOKEN_INVALID; do not distinguish details (REQ-WEB-018)
            _ => ApiError::TokenInvalid,
        }
    }
}

impl From<crate::coreclient::CoreError> for ApiError {
    fn from(e: crate::coreclient::CoreError) -> Self {
        match e {
            crate::coreclient::CoreError::Timeout => ApiError::CoreTimeout,
            crate::coreclient::CoreError::NotFound(_) => ApiError::CallNotFound,
            // Connection failure / stream error / protocol mismatch / core rejection / platform unsupported -> 502,
            // do not echo internal details to client (REQ-WEB-018 minimalization).
            _ => ApiError::CoreUnavailable,
        }
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        // Tuple 4th element = Retry-After seconds (only for rate-limit non-zero).
        let (status, code, msg, retry_after) = match self {
            ApiError::VersionMismatch { got, supported } => (
                StatusCode::BAD_REQUEST,
                "API_VERSION_MISMATCH",
                format!(
                    "X-NSBC-API-Version missing/unsupported: {:?}; \
                     supported major version is v{}",
                    got, supported
                ),
                0,
            ),
            ApiError::Unauthenticated => (
                StatusCode::UNAUTHORIZED,
                "UNAUTHENTICATED",
                "authentication required".to_string(),
                0,
            ),
            ApiError::BadCredentials => (
                StatusCode::UNAUTHORIZED,
                "INVALID_CREDENTIALS",
                "invalid username or password".to_string(),
                0,
            ),
            ApiError::TokenInvalid => (
                StatusCode::UNAUTHORIZED,
                "TOKEN_INVALID",
                "invalid or revoked token".to_string(),
                0,
            ),
            ApiError::TokenExpired => (
                StatusCode::UNAUTHORIZED,
                "TOKEN_EXPIRED",
                "token expired; refresh or re-authenticate".to_string(),
                0,
            ),
            ApiError::TokenReplay => (
                StatusCode::UNAUTHORIZED,
                "TOKEN_REPLAY",
                "refresh token reuse detected; session invalidated".to_string(),
                0,
            ),
            ApiError::AccountLocked => (
                StatusCode::LOCKED,
                "ACCOUNT_LOCKED",
                "account temporarily locked after repeated failed logins"
                    .to_string(),
                0,
            ),
            ApiError::RateLimited { retry_after } => (
                StatusCode::TOO_MANY_REQUESTS,
                "RATE_LIMITED",
                "rate limit exceeded; retry after the indicated interval"
                    .to_string(),
                retry_after.max(1),
            ),
            ApiError::Forbidden { required } => (
                StatusCode::FORBIDDEN,
                "FORBIDDEN",
                format!("requires {} privilege", required),
                0,
            ),
            ApiError::CoreUnavailable => (
                StatusCode::BAD_GATEWAY,
                "CORE_UNAVAILABLE",
                "core control plane unavailable; retry with backoff"
                    .to_string(),
                0,
            ),
            ApiError::CoreTimeout => (
                StatusCode::GATEWAY_TIMEOUT,
                "CORE_TIMEOUT",
                "core control plane timed out; retry with backoff".to_string(),
                0,
            ),
            ApiError::BadRequest(d) => {
                (StatusCode::BAD_REQUEST, "BAD_REQUEST", d, 0)
            }
            // Do not echo original Call-ID (REQ-WEB-018 minimalization)
            ApiError::CallNotFound => (
                StatusCode::NOT_FOUND,
                "CALL_NOT_FOUND",
                "active call not found or already terminated".to_string(),
                0,
            ),
            // Do not echo original config key (REQ-WEB-018 minimalization)
            ApiError::ConfigKeyNotFound => (
                StatusCode::NOT_FOUND,
                "CONFIG_KEY_NOT_FOUND",
                "config key not found in the effective view".to_string(),
                0,
            ),
            ApiError::DbUnavailable => (
                StatusCode::SERVICE_UNAVAILABLE,
                "DB_UNAVAILABLE",
                "persistence layer not configured; set NSBC_DB_DSN and restart".to_string(),
                0,
            ),
            ApiError::Db(msg) => {
                // Internal error details only logged, not echoed to client (REQ-WEB-018 minimalization)
                tracing::warn!(%msg, "db query failed");
                (
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "DB_ERROR",
                    "persistence layer query failed".to_string(),
                    0,
                )
            }
            ApiError::NotFound(d) => (
                StatusCode::NOT_FOUND,
                "NOT_FOUND",
                d,
                0,
            ),
            ApiError::Conflict(d) => (
                StatusCode::CONFLICT,
                "CONFLICT",
                d,
                0,
            ),
            ApiError::ServiceUnavailable(d) => (
                StatusCode::SERVICE_UNAVAILABLE,
                "SERVICE_UNAVAILABLE",
                d,
                0,
            ),
        };
        let body = Json(json!({
            "error": { "code": code, "message": msg }
        }));
        let mut resp = (status, body).into_response();
        if retry_after > 0 {
            if let Ok(v) = retry_after.to_string().parse() {
                resp.headers_mut().insert(RETRY_AFTER, v);
            }
        }
        resp
    }
}

pub type ApiResult<T> = Result<T, ApiError>;
