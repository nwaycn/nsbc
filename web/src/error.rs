//! 统一错误响应（REQ-WEB-009/020）：所有越界/鉴权失败返回结构化 JSON，
//! 不泄漏内部细节（REQ-WEB-018 最小化）。
use axum::http::header::RETRY_AFTER;
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use axum::Json;
use serde_json::json;

#[derive(Debug)]
pub enum ApiError {
    /// API 主版本缺失或不受支持（fail-closed，REQ-WEB-020）
    VersionMismatch { got: Option<String>, supported: u32 },
    /// 未认证（无身份/令牌缺失）
    Unauthenticated,
    /// 凭证错误（用户名/口令；不区分用户是否存在，REQ-WEB-018）
    BadCredentials,
    /// 令牌非法（签名/结构/类型错、已吊销）
    TokenInvalid,
    /// 令牌已过期
    TokenExpired,
    /// refresh 令牌重放（已轮转/登出的 jti 再次出现，REQ-WEB-008）
    TokenReplay,
    /// 账号因连续登录失败被锁定（REQ-WEB-004/008 失败锁定）
    AccountLocked,
    /// 触发 per-IP 接口限流（REQ-WEB-009）；载荷=窗口剩余秒数（Retry-After）
    RateLimited { retry_after: u64 },
    /// 已认证但角色不足（L0<L1<L2，REQ-OAM-008）
    Forbidden { required: &'static str },

    /// 核心不可达 / I/O / 协议违例（REQ-WEB-014 容错：显式 502）
    CoreUnavailable,
    /// 核心控制面超时（REQ-WEB-014：显式 504，调用方可退避重试）
    CoreTimeout,
    /// 请求体/参数非法
    BadRequest(String),
    /// 目标呼叫不存在（如 hangup 已结束/错误的 Call-ID，REQ-OAM-004）；
    /// 不携带/回显 Call-ID 原值（REQ-WEB-018 最小化）。
    CallNotFound,
    /// 配置键不存在（GET /config/{key}，REQ-CFG-011）；不回显键原值。
    ConfigKeyNotFound,

    /// DB 持久化层未配置（NSBC_DB_DSN 缺失或连接池初始化失败）。
    /// 所有依赖 DB 的端点（CDR/history）统一返回 503，与核心不可达显式区分。
    DbUnavailable,
    /// DB 查询执行期错误（SQL 语法/约束/网络）；message 内部详细，
    /// 不回显给客户端（REQ-WEB-018 最小化）。
    Db(String),

    /// 资源不存在（CRUD GET/PUT/DELETE 目标 id 未找到）。
    NotFound(String),
    /// 资源冲突（UNIQUE 约束冲突如 extension+realm 重复）。
    Conflict(String),
    /// 服务暂时不可用（如验证码存储满、后端资源耗尽）。
    ServiceUnavailable(String),
}

impl From<crate::auth::tokens::TokenError> for ApiError {
    fn from(e: crate::auth::tokens::TokenError) -> Self {
        match e {
            crate::auth::tokens::TokenError::Expired => ApiError::TokenExpired,
            // 结构错/签名错/类型混用 统一 401 TOKEN_INVALID，不区分细节（REQ-WEB-018）
            _ => ApiError::TokenInvalid,
        }
    }
}

impl From<crate::coreclient::CoreError> for ApiError {
    fn from(e: crate::coreclient::CoreError) -> Self {
        match e {
            crate::coreclient::CoreError::Timeout => ApiError::CoreTimeout,
            crate::coreclient::CoreError::NotFound(_) => ApiError::CallNotFound,
            // 连接失败 / 流错误 / 协议不符 / 核心拒绝 / 平台不支持 → 502，
            // 不把内部细节回传给客户端（REQ-WEB-018 最小化）。
            _ => ApiError::CoreUnavailable,
        }
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        // 元组第 4 项=Retry-After 秒（仅限流非零）。
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
            // 不回显 Call-ID 原值（REQ-WEB-018 最小化）
            ApiError::CallNotFound => (
                StatusCode::NOT_FOUND,
                "CALL_NOT_FOUND",
                "active call not found or already terminated".to_string(),
                0,
            ),
            // 不回显配置键原值（REQ-WEB-018 最小化）
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
                // 内部错误详情仅落日志，不回显给客户端（REQ-WEB-018 最小化）
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
