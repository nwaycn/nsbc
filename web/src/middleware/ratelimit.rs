//! Per-IP interface rate-limiting middleware (REQ-WEB-009).
//!
//! Mounted after version governance, before RBAC/business (requests with invalid version header get 400 and do not consume quota).
//! Three buckets: /auth/login, /auth/refresh, and the rest of /api (command execution surface); thresholds in
//! [`crate::auth::limiter::Limits`]; exceeding limits returns 429 + Retry-After, with audit + alerting.
//!
//! Source IP taken from ConnectInfo (production serve is mounted with into_make_service_with_connect_info
//! assembly); UA is also injected via PeerCtx for downstream audit reuse. When ConnectInfo is absent
//! (oneshot tests / non-TCP access), ip="-", still counts against its own bucket (no exemption).

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
    // --- RW-H2 hardening: X-Forwarded-For resolution ---
    // When nsbc-web sits behind a reverse proxy, ConnectInfo always reports the proxy's IP
    // (commonly 127.0.0.1). In that case, trust X-Forwarded-For header to extract the real
    // client IP. Direct connections from non-loopback keep their ConnectInfo as-is.
    let connect_ip = req
        .extensions()
        .get::<ConnectInfo<std::net::SocketAddr>>()
        .map(|c| c.0.ip());

    let ip = if let Some(addr) = connect_ip {
        if st.trust_proxy && addr.is_loopback() {
            // P2-13：仅当显式开启 web.trust_proxy 且直连对端是本机代理（loopback）时，
            // 才采信 XFF；取末位（由最近一级代理追加）地址，左侧值客户端可伪造。
            xff_client_ip(&req)
                .map(|s| s.parse().unwrap_or(addr).to_string())
                .unwrap_or_else(|| addr.to_string())
        } else {
            addr.to_string()
        }
    } else {
        "-".to_string()
    };

    let ua = req
        .headers()
        .get(axum::http::header::USER_AGENT)
        .and_then(|v| v.to_str().ok())
        .map(str::to_string);
    let peer = PeerCtx::new(Some(ip), ua.as_deref());
    // ★ 不能只用 req.uri().path()：axum 的 Router::nest 会把前缀从 URI 里剥掉，
    // /ext/v1/cmd 在 ext_router 内看到的是 "/cmd"，会被误判成 Api 桶（120/min），
    // AI agent 的媒体轮询立刻 429。MatchedPath 保留完整路由（含 nest 前缀）。
    let raw_path = req
        .extensions()
        .get::<axum::extract::MatchedPath>()
        .map(|m| m.as_str().to_string())
        .unwrap_or_else(|| req.uri().path().to_string());
    let path = raw_path.trim_start_matches("/api/v1").to_string();
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
    // PeerCtx is reused by rbac/handler for audit after passing; client cannot forge it.
    req.extensions_mut().insert(peer);
    Ok(next.run(req).await)
}

/// 取 X-Forwarded-For 末位有效 IP（由最近一级可信代理追加）。
/// P2-13：左侧值由客户端可控，不能作为限流依据，故从右向左取第一个合法 IP。
/// Returns None when no valid XFF header is present.
fn xff_client_ip(req: &Request) -> Option<String> {
    let header = req.headers().get("x-forwarded-for")?.to_str().ok()?;
    header
        .split(',')
        .rev()
        .map(str::trim)
        .find(|c| !c.is_empty() && c.parse::<std::net::IpAddr>().is_ok())
        .map(str::to_string)
}
