//! Security center API: dynamic bans (security.ban.* command surface) + static fence CRUD + global config.
//!
//! Dynamic ban (built-in fail2ban) view/search/unban entry point when real customers get falsely banned:
//!   GET    /security/bans?query=IP fragment → list (L0 read-only)
//!   DELETE /security/bans/{ip}              → unban (L1 write by default)
//!
//! Static fence (NIP three layers: IP CIDR / UA / number prefix, deny+allow dual mode):
//!   GET    /security/blacklist?kind=deny|allow&type=... → list (L0)
//!   POST   /security/blacklist                          → add (L1)
//!   PUT    /security/blacklist/{id}                     → change remark/enable
//!   DELETE /security/blacklist/{id}                     → delete
//!   After write: spawn_push(Kind::SecurityBlacklist) → C reload
//!
//! Global fence config (single-row security_config table):
//!   GET    /security/config → fence_mode + method_blocked
//!   PUT    /security/config → upsert then spawn_push
//!
//! Command injection defense: query/filter and ip are directly concatenated into UDS command lines
//! (`security.ban.list <filter>` / `security.ban.unban <ip>`), so the charset MUST
//! be whitelist-validated (IPv4 dotted / IPv6 colon + hex), forbidding space/newline
//! and other token separators from entering the command line.

use std::sync::Arc;

use axum::{
    extract::{Path, Query, State},
    http::StatusCode,
    routing::{delete, get, post, put},
    Json, Router,
};
use serde::Deserialize;
use serde_json::{json, Value};

use crate::coreclient::CoreApi;
use crate::db::{SecurityBlacklistCreate, SecurityBlacklistUpdate, SecurityConfig};
use crate::error::{ApiError, ApiResult};
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/security/bans", get(list_bans::<C>))
        .route("/security/bans/:ip", delete(unban::<C>))
        .route(
            "/security/blacklist",
            get(list_blacklist::<C>).post(create_blacklist::<C>),
        )
        .route(
            "/security/blacklist/:id",
            put(update_blacklist::<C>).delete(delete_blacklist::<C>),
        )
        .route(
            "/security/config",
            get(get_config::<C>).put(put_config::<C>),
        )
}

// ====================== Dynamic bans ======================

/// IP substring character whitelist: alphanumeric, '.', ':' (subset of IPv4/IPv6 literals).
fn is_safe_ip_fragment(s: &str) -> bool {
    s.len() <= 45
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b':')
}

#[derive(Deserialize)]
pub struct BanQueryParams {
    pub query: Option<String>,
}

async fn list_bans<C: CoreApi + Clone>(
    State(st): State<Arc<AppState<C>>>,
    Query(q): Query<BanQueryParams>,
) -> ApiResult<axum::Json<Value>> {
    let filter = q.query.as_deref().unwrap_or("").trim();
    if !is_safe_ip_fragment(filter) {
        return Err(ApiError::BadRequest(
            "query must be an IP fragment (alnum . : only)".into(),
        ));
    }
    let bans = st.core.security_ban_list(filter).await?;
    Ok(axum::Json(json!({ "total": bans.len(), "data": bans })))
}

async fn unban<C: CoreApi + Clone>(
    State(st): State<Arc<AppState<C>>>,
    Path(ip): Path<String>,
) -> ApiResult<axum::Json<Value>> {
    if !is_safe_ip_fragment(&ip) || ip.is_empty() {
        return Err(ApiError::BadRequest("invalid ip".into()));
    }
    st.core.security_ban_unban(&ip).await?;
    Ok(axum::Json(json!({ "ok": true, "ip": ip })))
}

// ====================== Validation helpers ======================

const VALID_KINDS: [&str; 2] = ["deny", "allow"];
const VALID_TYPES: [&str; 4] = ["ip_cidr", "ua", "called_prefix", "caller_prefix"];

/// CIDR normalization: single IP auto-padded with /32 (v4) or /128 (v6);
/// when a prefix is present, validate the format and range.
fn normalize_cidr(v: &str) -> Result<String, String> {
    let v = v.trim();
    if v.is_empty() || v.len() > 64 {
        return Err("cidr must be 1-64 chars".into());
    }
    if v.contains(|c: char| c.is_whitespace()) {
        return Err("whitespace not allowed".into());
    }
    match v.split_once('/') {
        Some((ip_s, mask_s)) => {
            let ip: std::net::IpAddr = ip_s
                .parse()
                .map_err(|_| format!("invalid ip: {ip_s}"))?;
            if !mask_s.bytes().all(|b| b.is_ascii_digit()) || mask_s.is_empty() {
                return Err(format!("invalid prefix: {mask_s}"));
            }
            let mask: u8 = mask_s
                .parse()
                .map_err(|_| format!("invalid prefix: {mask_s}"))?;
            let max = if ip.is_ipv4() { 32 } else { 128 };
            if mask > max {
                return Err(format!("prefix /{mask} out of range 0-{max}"));
            }
            Ok(format!("{ip}/{mask}"))
        }
        None => {
            let ip: std::net::IpAddr = v
                .parse()
                .map_err(|_| format!("invalid ip or cidr: {v}"))?;
            Ok(format!("{ip}/{}", if ip.is_ipv4() { 32 } else { 128 }))
        }
    }
}

/// UA rule validation: 1-128 bytes, control characters forbidden (case-insensitive substring)
fn validate_ua(v: &str) -> Result<String, String> {
    let v = v.trim();
    if v.is_empty() || v.len() > 128 {
        return Err("ua must be 1-128 bytes".into());
    }
    if v.bytes().any(|b| b < 0x20 || b == 0x7f) {
        return Err("control characters not allowed".into());
    }
    Ok(v.to_string())
}

/// Number prefix validation (called/caller): 1-128 bytes;
/// allows trailing '*' prefix wildcard (e.g. "100*"); otherwise exact match (e.g. "100").
/// Forbids control chars / whitespace / letters (pure digits + optional trailing '*').
fn validate_prefix(v: &str) -> Result<String, String> {
    let v = v.trim();
    if v.is_empty() || v.len() > 128 {
        return Err("prefix must be 1-128 bytes".into());
    }
    if v.bytes().any(|b| b < 0x20 || b == 0x7f || b == b' ') {
        return Err("control characters and whitespace not allowed".into());
    }
    let is_wild = v.ends_with('*');
    let core = if is_wild { &v[..v.len() - 1] } else { v };
    if core.is_empty() {
        return Err("prefix cannot be bare '*'".into());
    }
    if !core.bytes().all(|b| b.is_ascii_digit()) {
        return Err("prefix body must be digits only (optional trailing '*' wildcard)".into());
    }
    Ok(v.to_string())
}

// ====================== Static fence CRUD ======================

#[derive(Deserialize, Default)]
pub struct BlacklistQueryParams {
    pub kind:       Option<String>,   // deny | allow (default = all)
    #[serde(rename = "type", alias = "rule_type")]
    pub rule_type:  Option<String>,
    pub enabled:    Option<String>,
    pub limit:      Option<i64>,
    pub offset:     Option<i64>,
}

async fn list_blacklist<C: CoreApi + Clone>(
    State(st): State<Arc<AppState<C>>>,
    Query(q): Query<BlacklistQueryParams>,
) -> ApiResult<axum::Json<Value>> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let kind = match q.kind.as_deref() {
        Some("") | None => None,
        Some(k) if VALID_KINDS.contains(&k) => Some(k),
        Some(other) => {
            return Err(ApiError::BadRequest(format!(
                "kind must be deny or allow, got: {other}"
            )))
        }
    };
    let rule_type = match q.rule_type.as_deref() {
        Some("") | None => None,
        Some(t) if VALID_TYPES.contains(&t) => Some(t),
        Some(other) => {
            return Err(ApiError::BadRequest(format!(
                "type must be one of [ip_cidr, ua, called_prefix, caller_prefix], got: {other}"
            )))
        }
    };
    let enabled = match q.enabled.as_deref() {
        Some("true") => Some(true),
        Some("false") => Some(false),
        _ => None,
    };

    let (items, total) = st
        .db
        .list_security_blacklist(
            kind,
            rule_type,
            enabled,
            q.limit.unwrap_or(200).clamp(1, 500),
            q.offset.unwrap_or(0).max(0),
        )
        .await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    Ok(axum::Json(json!({ "items": items, "total": total })))
}

async fn create_blacklist<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(mut body): Json<SecurityBlacklistCreate>,
) -> ApiResult<(StatusCode, axum::Json<Value>)> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    body.kind = body.kind.trim().to_string();
    if body.kind.is_empty() { body.kind = "deny".into(); }
    if !VALID_KINDS.contains(&body.kind.as_str()) {
        return Err(ApiError::BadRequest(format!(
            "kind must be deny or allow, got: {}", body.kind
        )));
    }

    body.rule_type = body.rule_type.trim().to_string();
    match body.rule_type.as_str() {
        "ip_cidr" => body.value = normalize_cidr(&body.value).map_err(ApiError::BadRequest)?,
        "ua" => body.value = validate_ua(&body.value).map_err(ApiError::BadRequest)?,
        "called_prefix" | "caller_prefix" => {
            body.value = validate_prefix(&body.value).map_err(ApiError::BadRequest)?;
        }
        other => {
            return Err(ApiError::BadRequest(format!(
                "type must be one of [ip_cidr, ua, called_prefix, caller_prefix], got: {other}"
            )))
        }
    }
    body.comment = body.comment.trim().chars().take(64).collect();

    let row = st
        .db
        .create_security_blacklist(body)
        .await
        .map_err(|e| {
            if let sqlx::Error::Database(ref db_err) = e {
                let msg = db_err.message().to_ascii_lowercase();
                if msg.contains("unique") || msg.contains("duplicate") {
                    return ApiError::Conflict("rule already exists".into());
                }
            }
            ApiError::Db(e.to_string())
        })?;
    let _ = st
        .db
        .insert_config_history(
            "api", "create", &format!("security-blacklist/{}", row.id),
            None, &format!("kind={} type={} value={}", row.kind, row.rule_type, row.value),
        )
        .await;
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::SecurityBlacklist);
    Ok((StatusCode::CREATED, axum::Json(json!({ "item": row }))))
}

async fn update_blacklist<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
    Json(mut body): Json<SecurityBlacklistUpdate>,
) -> ApiResult<axum::Json<Value>> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    if let Some(ref mut c) = body.comment {
        *c = c.trim().chars().take(64).collect();
    }
    let row = st
        .db
        .update_security_blacklist(id, body)
        .await
        .map_err(|e| match e {
            sqlx::Error::RowNotFound => ApiError::NotFound("rule not found".into()),
            other => ApiError::Db(other.to_string()),
        })?;
    let _ = st
        .db
        .insert_config_history(
            "api", "update", &format!("security-blacklist/{id}"),
            None, &format!("kind={} type={} value={} enabled={}", row.kind, row.rule_type, row.value, row.enabled != 0),
        )
        .await;
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::SecurityBlacklist);
    Ok(axum::Json(json!({ "item": row })))
}

async fn delete_blacklist<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> ApiResult<StatusCode> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.get_security_blacklist(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    let deleted = st
        .db
        .delete_security_blacklist(id)
        .await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    if !deleted {
        return Err(ApiError::NotFound("rule not found".into()));
    }
    if let Some(r) = row {
        let _ = st.db.insert_config_history(
            "api", "delete", &format!("security-blacklist/{id}"), None,
            &format!("kind={} type={} value={}", r.kind, r.rule_type, r.value),
        ).await;
    }
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::SecurityBlacklist);
    Ok(StatusCode::NO_CONTENT)
}

// ====================== Global fence config ======================

async fn get_config<C: CoreApi + Clone>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<axum::Json<SecurityConfig>> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let cfg = st.db.get_security_config().await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    Ok(axum::Json(cfg))
}

/// PUT /security/config — fence_mode + method_blocked + fail2ban five-dimensional config.
/// Field-range validation: fence_mode=0/1, method_blocked=uint32, auto_ban=0/1,
/// threshold=1-1000, window_s=1-86400, ban_duration_s=1-604800(7d),
/// ratelimit_regs=1-1000, ratelimit_window_s=1-3600.
/// After writing: spawn_push lets C reload take effect.
async fn put_config<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<Value>,
) -> ApiResult<axum::Json<SecurityConfig>> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let fence_mode = body.get("fence_mode")
        .and_then(|v| v.as_i64())
        .ok_or_else(|| ApiError::BadRequest("fence_mode (0 or 1) required".into()))?;
    if fence_mode != 0 && fence_mode != 1 {
        return Err(ApiError::BadRequest("fence_mode must be 0 (performance) or 1 (security)".into()));
    }

    let method_blocked = body.get("method_blocked")
        .and_then(|v| v.as_i64())
        .ok_or_else(|| ApiError::BadRequest("method_blocked (uint32 bitmap) required".into()))?;
    if method_blocked < 0 || method_blocked > 0xffffffff {
        return Err(ApiError::BadRequest("method_blocked out of uint32 range".into()));
    }

    // fail2ban five dimensions (optional, default values filled when omitted)
    let auto_ban = body.get("auto_ban").and_then(|v| v.as_i64()).unwrap_or(1);
    if auto_ban != 0 && auto_ban != 1 {
        return Err(ApiError::BadRequest("auto_ban must be 0 or 1".into()));
    }
    let authfail_threshold = body.get("authfail_threshold").and_then(|v| v.as_i64()).unwrap_or(5);
    if authfail_threshold < 1 || authfail_threshold > 1000 {
        return Err(ApiError::BadRequest("authfail_threshold must be 1-1000".into()));
    }
    let authfail_window_s = body.get("authfail_window_s").and_then(|v| v.as_i64()).unwrap_or(60);
    if authfail_window_s < 1 || authfail_window_s > 86400 {
        return Err(ApiError::BadRequest("authfail_window_s must be 1-86400".into()));
    }
    let ban_duration_s = body.get("ban_duration_s").and_then(|v| v.as_i64()).unwrap_or(3600);
    if ban_duration_s < 1 || ban_duration_s > 604800 {
        return Err(ApiError::BadRequest("ban_duration_s must be 1-604800 (7d)".into()));
    }
    let ratelimit_regs = body.get("ratelimit_regs").and_then(|v| v.as_i64()).unwrap_or(20);
    if ratelimit_regs < 1 || ratelimit_regs > 1000 {
        return Err(ApiError::BadRequest("ratelimit_regs must be 1-1000".into()));
    }
    let ratelimit_window_s = body.get("ratelimit_window_s").and_then(|v| v.as_i64()).unwrap_or(60);
    if ratelimit_window_s < 1 || ratelimit_window_s > 3600 {
        return Err(ApiError::BadRequest("ratelimit_window_s must be 1-3600".into()));
    }

    let ratelimit_invites = body.get("ratelimit_invites").and_then(|v| v.as_i64()).unwrap_or(0);
    if ratelimit_invites < 0 || ratelimit_invites > 1000 {
        return Err(ApiError::BadRequest("ratelimit_invites must be 0-1000".into()));
    }
    let ratelimit_invites_active = body.get("ratelimit_invites_active").and_then(|v| v.as_i64()).unwrap_or(0);
    if ratelimit_invites_active < 0 || ratelimit_invites_active > 1000 {
        return Err(ApiError::BadRequest("ratelimit_invites_active must be 0-1000".into()));
    }

    let cfg = SecurityConfig {
        fence_mode, method_blocked, auto_ban, authfail_threshold, authfail_window_s,
        ban_duration_s, ratelimit_regs, ratelimit_window_s,
        ratelimit_invites, ratelimit_invites_active,
    };
    let saved = st.db.upsert_security_config(&cfg).await
        .map_err(|e| ApiError::Db(e.to_string()))?;

    let _ = st.db.insert_config_history(
        "api", "update", "security-config", None,
        &format!("fence={} blocked=0x{:x} auto_ban={} thr={} win={}s ban={}s rl_regs={} rl_win={}s",
            saved.fence_mode, saved.method_blocked, saved.auto_ban,
            saved.authfail_threshold, saved.authfail_window_s, saved.ban_duration_s,
            saved.ratelimit_regs, saved.ratelimit_window_s),
    ).await;

    // Trigger C reload (re-reads cfg from YAML via cfg_get)
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::SecurityBlacklist);
    Ok(axum::Json(saved))
}
