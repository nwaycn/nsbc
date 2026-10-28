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
        if addr.is_loopback() {
            // Behind a reverse proxy (nginx / haproxy). Prefer XFF if present.
            xff_client_ip(&req)
                .map(|s| s.parse().unwrap_or(addr).to_string())
                .unwrap_or_else(|| addr.to_string())
        } else {
            addr.to_string()
        }
    } else {
        xff_client_ip(&req).unwrap_or_else(|| "-".to_string())
    };

    let ua = req
        .headers()
        .get(axum::http::header::USER_AGENT)
        .and_then(|v| v.to_str().ok())
        .map(str::to_string);
    let peer = PeerCtx::new(Some(ip), ua.as_deref());
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
    // PeerCtx is reused by rbac/handler for audit after passing; client cannot forge it.
    req.extensions_mut().insert(peer);
    Ok(next.run(req).await)
}

/// Extract the leftmost (client-side) non-loopback IP from X-Forwarded-For.
/// Returns None when no valid XFF header is present.
fn xff_client_ip(req: &Request) -> Option<String> {
    let header = req.headers().get("x-forwarded-for")?.to_str().ok()?;
    // X-Forwarded-For: client, proxy1, proxy2 ...
    // Leftmost is the original client IP.
    for candidate in header.split(',') {
        let candidate = candidate.trim();
        if candidate.is_empty() { continue; }
        if let Ok(ip) = candidate.parse::<std::net::IpAddr>() {
            if !ip.is_loopback() {
                return Some(candidate.to_string());
            }
        }
    }
    None
}
