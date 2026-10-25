//! External API Key auth middleware (REQ-EXT-API-001).
//!
//! 和 Web UI 的 JWT (`rbac::require_auth`) 完全独立 —— 这是给呼叫中心 / CRM / AI Agent
//! 等外部系统用的鉴权层。Bearer token 是静态 API Key（格式 `nsbc_live_<32byteshex>`），
//! token 原文只在创建时显示一次，DB 存 SHA-256 hash。
//!
//! 免鉴权短路 (REQ-EXT-API-004):
//!   源 IP 127.0.0.1 / ::1 / Unix Domain Socket peer → 跳过 Bearer 检查，
//!   自动注入 admin scope 的 localhost claims。本机脚本/CLI 直连无需配置任何 Key。
//!   **跨机器调用仍必须 Bearer Token** —— 只有 loopback 是隐式信任边界。
//!
//! Scope 分级:
//!   readonly  → GET 监控类（calls / registrations / cdrs / system）
//!   readwrite → readonly + POST 控制类（originate / hangup / transfer） + events/stream SSE
//!   admin     → readwrite + 危险操作（reload / shutdown / API Key 管理）
//!
//! 失败返回标准 JSON: { "error": "message", "code": "UNAUTHENTICATED" }

use std::net::SocketAddr;
use std::sync::Arc;

use axum::{
    extract::{ConnectInfo, Request, State},
    http::header,
    middleware::Next,
    response::Response,
};
use ring::digest::{Context, SHA256};
use serde::Serialize;

use crate::AppState;
use crate::error::ApiError;

fn hex_encode(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0x0f) as usize] as char);
    }
    out
}

/// Bearer token → SHA-256 hex (零新依赖: ring 已在 Cargo.toml)
pub fn hash_key(raw: &str) -> String {
    let mut h = Context::new(&SHA256);
    h.update(raw.as_bytes());
    hex_encode(h.finish().as_ref())
}

/// API Key scope（REQ-EXT-API-001）
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum ApiKeyScope {
    Readonly,
    Readwrite,
    Admin,
}

impl ApiKeyScope {
    pub fn allows(self, need: ApiKeyScope) -> bool {
        match (self, need) {
            (ApiKeyScope::Admin, _) => true,
            (ApiKeyScope::Readwrite, ApiKeyScope::Readonly | ApiKeyScope::Readwrite) => true,
            (ApiKeyScope::Readonly, ApiKeyScope::Readonly) => true,
            _ => false,
        }
    }
}

/// 通过 request extension 向下游 handler 传递已鉴权的 key 信息
#[derive(Debug, Clone)]
pub struct ExtKeyClaims {
    pub label: String,
    pub scope: ApiKeyScope,
}

/// Middleware: 从 Authorization: Bearer <key> 提取 API Key → 查 DB → 注入 extension
/// REQ-EXT-API-004: loopback 本机请求默认免鉴权，自动注入 localhost+admin claims。
/// 运维可通过 `web.ext.require_loopback_auth: true` 强制 loopback 也走 Bearer 检查
/// （反代场景必须开 —— 反代过来的请求 peer 就是 127.0.0.1）。
pub async fn require_ext_key<C: crate::coreclient::CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    mut req: Request,
    next: Next,
) -> Result<Response, ApiError> {
    // ─── REQ-EXT-API-004: loopback 本机请求免鉴权 ───
    // 默认：127.0.0.1 / ::1 自动注入 admin claims，本机脚本零摩擦。
    // 反代部署（nginx → 127.0.0.1:8443）：必须在 yaml 设 web.ext.require_loopback_auth: true
    // 否则反代过来的远程请求 peer 也是 127.0.0.1，全部触发免鉴权。
    let is_local = req
        .extensions()
        .get::<ConnectInfo<SocketAddr>>()
        .map(|ConnectInfo(addr)| addr.ip().is_loopback())
        .unwrap_or(false);
    if is_local && !st.require_loopback_auth {
        req.extensions_mut().insert(ExtKeyClaims {
            label: "localhost".to_string(),
            scope: ApiKeyScope::Admin,
        });
        return Ok(next.run(req).await);
    }

    // ─── Bearer Token 常规鉴权 ───
    let bearer = req
        .headers()
        .get(header::AUTHORIZATION)
        .and_then(|v| v.to_str().ok())
        .and_then(|s| s.strip_prefix("Bearer "))
        .ok_or(ApiError::Unauthenticated)?;

    if !bearer.starts_with("nsbc_live_") && !bearer.starts_with("nsbc_test_") {
        return Err(ApiError::Unauthenticated);
    }

    let key_hash = hash_key(bearer);

    // 查 DB (sqlx async)
    let pool = st.db.pool().ok_or(ApiError::Unauthenticated)?;
    let row: Option<(String, String)> = sqlx::query_as(
        "SELECT label, scope FROM ext_api_keys WHERE key_hash = $1 AND revoked_at IS NULL LIMIT 1",
    )
    .bind(&key_hash)
    .fetch_optional(pool)
    .await
    .ok()
    .flatten();

    let (label, scope_str) = row.ok_or(ApiError::Unauthenticated)?;

    let scope = match scope_str.as_str() {
        "readonly" => ApiKeyScope::Readonly,
        "readwrite" => ApiKeyScope::Readwrite,
        "admin" => ApiKeyScope::Admin,
        _ => return Err(ApiError::Unauthenticated),
    };

    req.extensions_mut().insert(ExtKeyClaims { label, scope });

    // 更新 last_used_at（fire-and-forget，不阻塞响应）
    let db = st.db.clone();
    let kh = key_hash.clone();
    tokio::spawn(async move {
        if let Some(pool) = db.pool() {
            let _ = sqlx::query(
                "UPDATE ext_api_keys SET last_used_at = now() WHERE key_hash = $1",
            )
            .bind(&kh)
            .execute(pool)
            .await;
        }
    });

    Ok(next.run(req).await)
}

/// 辅助：从 Request extensions 取 scope，不够权限则 403
pub fn check_scope<B>(req: &Request<B>, need: ApiKeyScope) -> Result<(), ApiError> {
    let claims = req
        .extensions()
        .get::<ExtKeyClaims>()
        .ok_or(ApiError::Unauthenticated)?;
    if !claims.scope.allows(need) {
        return Err(ApiError::Forbidden { required: "insufficient scope" });
    }
    Ok(())
}
