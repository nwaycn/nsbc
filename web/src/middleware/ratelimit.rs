//! per-IP 接口限流中间件（REQ-WEB-009）。
//!
//! 挂在版本治理之后、rbac/业务之前（版本头非法的请求 400，不消耗配额）。
//! 三桶：/auth/login、/auth/refresh、其余 /api（命令执行面），阈值见
//! [`crate::auth::limiter::Limits`]；超限 429 + Retry-After，审计+告警。
//!
//! 来源 IP 取 ConnectInfo（生产 serve 以 into_make_service_with_connect_info
//! 装配）；UA 经 PeerCtx 一并注入，供下游审计复用。无 ConnectInfo
//! （oneshot 测试/非 TCP 接入）时 ip="-"，同样计入独立桶（不免限）。

use std::sync::Arc;

use axum::extract::{ConnectInfo, Request, State};
use axum::middleware::Next;
use axum::response::Response;
use serde_json::json;

use crate::auth::limiter::{Bucket, RateLimiter};
use crate::auth::tokens::now_secs;
use crate::auth::PeerCtx;
use crate::coreclient::CoreApi;
use crate::error::ApiError;
use crate::AppState;

pub async fn enforce<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    mut req: Request,
    next: Next,
) -> Result<Response, ApiError> {
    let ip = req
        .extensions()
        .get::<ConnectInfo<std::net::SocketAddr>>()
        .map(|c| c.0.ip().to_string());
    let ua = req
        .headers()
        .get(axum::http::header::USER_AGENT)
        .and_then(|v| v.to_str().ok())
        .map(str::to_string);
    let peer = PeerCtx::new(ip, ua.as_deref());
    let path = req
        .uri()
        .path()
        .trim_start_matches("/api/v1")
        .to_string();
    let bucket = Bucket::for_path(&path);
    let now = now_secs();
    if let Err(retry_after) = st.auth.guard.rate.check(bucket, &peer.ip, now) {
        tracing::warn!(
            ip = %peer.ip,
            bucket = RateLimiter::bucket_name(bucket),
            retry_after,
            "rate limit exceeded"
        );
        st.auth.audit.record(
            "rate_limited",
            None,
            &peer,
            json!({"bucket": RateLimiter::bucket_name(bucket), "path": path, "retry_after": retry_after}),
        );
        return Err(ApiError::RateLimited { retry_after });
    }
    // 通过后 PeerCtx 供 rbac/handler 审计复用，客户端无法伪造。
    req.extensions_mut().insert(peer);
    Ok(next.run(req).await)
}
