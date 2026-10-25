//! RBAC role mapping (REQ-WEB-008 / REQ-OAM-008).
//!
//! Three privilege tiers, aligned with the core command registry's per-command
//! level annotations:
//!   L0 read-only:   view config / calls / metrics / alerts
//!   L1 operations:  reload, hangup, record trigger (snapshot + audit)
//!   L2 high-risk:   change listen addr / certs / ACLs, restart → MUST require
//!                   secondary authorization (P1: short-lived token or dual-confirm,
//!                   REQ-OAM-008)
//!
//! Since W3a identity is issued on login as an HS256 JWT (access token, see crate::auth):
//! after verifying signature/type/expiry/revocation, inject Identity into request
//! extensions. The dev token table (NSBC_DEV_TOKENS) has been removed; mTLS cert
//! CN/O mapping is a follow-up integration item.

use std::sync::Arc;

use axum::extract::{Request, State};
use axum::http::{header, Method};
use axum::middleware::Next;
use axum::response::Response;

use serde_json::json;

use crate::auth::tokens::{now_secs, TokenKind};
use crate::auth::{Identity, PeerCtx, Role};
use crate::coreclient::CoreApi;
use crate::error::ApiError;
use crate::AppState;

/// Declare required level by request method + path (P0 minimal set; complete table is
/// pushed by the core command registry).
fn required_level(method: &Method, path: &str) -> Role {
    let item = path.trim_start_matches('/').trim_end_matches('/');
    let action = item.rsplit('/').next().unwrap_or(item);
    // GET last-segment is a literal resource rather than a fixed-action read-only nested
    // route (whitelist enumeration only, no blanket prefix open): /calls/{id}
    // (uuid_dump diagnostic), /config/{key} (config.get effective value). Only GET + one
    // segment hit; POST .../hangup naturally unaffected by method difference, multi-segment
    // paths fall back to default and the route layer returns 405.
    let get_nested_readonly = method == Method::GET
        && ["calls/", "config/"].into_iter().any(|p| {
            item.strip_prefix(p)
                .is_some_and(|rest| !rest.is_empty() && !rest.contains('/'))
        });
    match (method, action) {
        // Read-only L0: nested read-only resources (priority over action whitelist,
        // last-segment is literal)
        _ if get_nested_readonly => Role::L0,
        // Read-only L0
        (
            &Method::GET,
            "version" | "config" | "listens" | "calls" | "channels"
            | "registrations" | "metrics" | "cdrs" | "recordings" | "bans"
            | "blacklist",
        ) => Role::L0,
        // Write-operation dry-run is still read-only (does not change core state)
        (&Method::POST, "validate") => Role::L0,
        // High-risk L2 (resource-level truly dangerous operations)
        (&Method::PUT | &Method::PATCH | &Method::DELETE, "config")
        | (&Method::POST, "restart")
        // W-02 安全修复：呼叫控制命令端点（REQ-CCMD-001 新增的 L2 命令）
        // POST /calls/:id/{kill,answer,hold,mute,broadcast,vad-start,vad-stop,
        //                  transfer,record,send-dtmf,send-msg,originate}
        // 这些端点之前因为 action 名不在显式 L2 表中，会 fall-through 到默认 L1，
        // 导致 L1 JWT 可以执行杀通话、转移、录音等高风险操作。
        | (&Method::POST, "kill" | "answer" | "hold" | "mute" | "broadcast"
           | "vad-start" | "vad-stop" | "transfer" | "record"
           | "send-dtmf" | "send-msg" | "originate") => Role::L2,
        // Operational L1; write operations not explicitly annotated default to L1
        // (do NOT default to L0)
        _ => Role::L1,
    }
}

pub async fn require_auth<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    mut req: Request,
    next: Next,
) -> Result<Response, ApiError> {
    let token = req
        .headers()
        .get(header::AUTHORIZATION)
        .and_then(|v| v.to_str().ok())
        .and_then(|s| s.strip_prefix("Bearer "))
        .ok_or(ApiError::Unauthenticated)?;
    if token.len() > 4096 {
        return Err(ApiError::TokenInvalid);
    }
    let now = now_secs();
    let claims = st
        .auth
        .tokens
        .verify(token, TokenKind::Access, now)?;
    let role = Role::from_u8(claims.role).ok_or(ApiError::TokenInvalid)?;
    if st.auth.sessions.access_revoked(&claims.jti, now) {
        return Err(ApiError::TokenInvalid);
    }

    let need = required_level(req.method(), req.uri().path());
    if role < need {
        return Err(ApiError::Forbidden {
            required: match need {
                Role::L0 => "L0",
                Role::L1 => "L1",
                Role::L2 => "L2",
            },
        });
    }
    // REQ-WEB-010: write operations (L1/L2, i.e. non-safe methods) have Web-side
    // independent audit records, including actor / source / method / path / response
    // status; handler-level parameter snapshots are handled per-handler (follow-up).
    // The core side has its own log_audit for commands reaching the control plane (two
    // independent layers).
    let auditable = !matches!(req.method(), &Method::GET | &Method::HEAD | &Method::OPTIONS);
    let peer = req
        .extensions()
        .get::<PeerCtx>()
        .cloned()
        .unwrap_or_default();
    let sub = claims.sub.clone();
    let method = req.method().to_string();
    let path = req.uri().path().to_string();
    req.extensions_mut().insert(Identity(claims));
    let resp = next.run(req).await;
    if auditable {
        st.auth.audit.record(
            "api_write",
            Some(&sub),
            &peer,
            json!({"method": method, "path": path, "status": resp.status().as_u16(),
                  "required_level": need.as_str()}),
        );
    }
    Ok(resp)
}
