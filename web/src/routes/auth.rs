//! Login / refresh / logout (REQ-WEB-004 login and session, REQ-WEB-008 session).
//!
//! Two public endpoints (login/refresh) sit outside the rbac layer (but still go
//! through version governance and body size limits); logout is a protected endpoint
//! (present access token → revoke self + optional refresh).
//!
//! Error semantics (REQ-WEB-018): non-existent username and wrong password share
//! **the same** 401; never leak whether the account exists. Token errors do not echo
//! the concrete JWT content. Login-failure lockout / interface rate limiting live in
//! W3b (REQ-WEB-009); this module only writes security logs.
use std::sync::Arc;

use axum::extract::{Extension, State};
use axum::routing::{get, post};
use axum::{Json, Router};
use serde::Deserialize;
use serde_json::{json, Value};

use crate::auth::limiter::FailOutcome;
use crate::auth::sessions::RefreshError;
use crate::auth::tokens::{now_secs, TokenKind};
use crate::auth::{valid_username, Identity, PeerCtx};
use crate::coreclient::CoreApi;
use crate::error::{ApiError, ApiResult};
use crate::AppState;

pub fn public_router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/auth/captcha", get(get_captcha::<C>))
        .route("/auth/login", post(login::<C>))
        .route("/auth/refresh", post(refresh_token::<C>))
}

pub fn protected_router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/auth/logout", post(logout::<C>))
        .route("/auth/change-password", post(change_password::<C>))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)] // REQ-WEB-009: reject unknown fields
struct LoginReq {
    username: String,
    password: String,
    /// Captcha ID (optional: may be empty if config disables it or on first use)
    captcha_id: Option<String>,
    /// Captcha answer (optional; must be correct when present)
    captcha_code: Option<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RefreshReq {
    refresh_token: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct LogoutReq {
    refresh_token: Option<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ChangePasswordReq {
    old_password: String,
    new_password: String,
}

/// GET /api/v1/auth/captcha → {captcha_id, svg}
async fn get_captcha<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<Value>> {
    match st.auth.captcha.generate() {
        Some((id, svg)) => Ok(Json(json!({
            "captcha_id": id,
            "svg": svg,
        }))),
        None => Err(ApiError::ServiceUnavailable("captcha store full".into())),
    }
}

/// POST /api/v1/auth/login → access(15min)+refresh(8h).
async fn login<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Extension(peer): Extension<PeerCtx>,
    Json(req): Json<LoginReq>,
) -> ApiResult<Json<Value>> {
    // Whitelist-style input validation (REQ-WEB-009); password length uses byte
    // upper bound, characters unrestricted (allow arbitrary Unicode).
    if !valid_username(&req.username) || req.password.is_empty() || req.password.len() > 256 {
        return Err(ApiError::BadRequest("invalid username or password".into()));
    }

    // Captcha mandatory verification (REQ-WEB-009: must GET /auth/captcha first to get
    // captcha_id, then bring captcha_id + captcha_code on login; only proceed when
    // verify succeeds). Missing or empty captcha_id/captcha_code → reject directly,
    // never go through the password verification path (save CPU).
    let cid = match &req.captcha_id {
        Some(s) if !s.is_empty() => s,
        _ => {
            tracing::warn!(user = %req.username, "login failed: missing captcha_id");
            return Err(ApiError::BadRequest("captcha required".into()));
        }
    };
    let ccode = match &req.captcha_code {
        Some(s) if !s.is_empty() => s,
        _ => {
            tracing::warn!(user = %req.username, "login failed: missing captcha_code");
            return Err(ApiError::BadRequest("captcha required".into()));
        }
    };
    if !st.auth.captcha.verify(cid, ccode) {
        tracing::warn!(user = %req.username, "login failed: invalid captcha");
        st.auth.audit.record(
            "login_failed",
            Some(&req.username),
            &peer,
            json!({"reason": "invalid_captcha"}),
        );
        return Err(ApiError::BadRequest("invalid captcha".into()));
    }
    let now = now_secs();
    // Lockout check before argon2 verification (save CPU, no info leak beyond account
    // enumeration difference).
    if st.auth.guard.lock.is_locked(&req.username, now) {
        tracing::warn!(user = %req.username, "login blocked: account locked");
        st.auth
            .audit
            .record("login_locked", Some(&req.username), &peer, json!({}));
        return Err(ApiError::AccountLocked);
    }

    // Account verification: only query the web_users table from the database
    // (the single authoritative account source). Completely deprecate text-based
    // account sources such as config file auth.accounts / NSBC_AUTH_FILE /
    // NSBC_AUTH_ACCOUNTS — login always goes through the DB.
    let db_user = match st.db.get_web_user(&req.username).await {
        Ok(user) => user,
        Err(error) => {
            tracing::error!(user = %req.username, error = %error, "web user lookup failed");
            st.auth.audit.record(
                "login_unavailable",
                Some(&req.username),
                &peer,
                json!({"reason": "database_query_failed"}),
            );
            return Err(ApiError::DbUnavailable);
        }
    };
    let verified_role: Option<crate::auth::Role> = {
        if let Some(u) = db_user {
            if u.enabled != 1 {
                tracing::warn!(user = %req.username,
                    "login blocked: web_user disabled in DB");
                None
            } else if u.password_phc.is_empty() {
                tracing::warn!(user = %req.username,
                    "login blocked: web_user has empty password_phc");
                None
            } else {
                use argon2::{Argon2, PasswordHash, PasswordVerifier};
                match PasswordHash::new(&u.password_phc) {
                    Ok(parsed) => {
                        if Argon2::default()
                            .verify_password(req.password.as_bytes(), &parsed)
                            .is_ok()
                        {
                            crate::auth::Role::parse(&u.role)
                        } else {
                            None
                        }
                    }
                    Err(e) => {
                        tracing::warn!(user = %req.username, error = %e,
                            "web_user: invalid password_phc in DB");
                        None
                    }
                }
            }
        } else {
            None // DB not configured / user not found → uniformly treated as bad credentials
        }
    };

    let Some(role) = verified_role else {
        let outcome = st.auth.guard.lock.record_fail(&req.username, now);
        let remaining = match outcome {
            FailOutcome::Remaining(n) => Some(n),
            FailOutcome::JustLocked { until } => {
                tracing::warn!(user = %req.username, until, "account locked after repeated failures");
                st.auth.audit.record(
                    "account_locked",
                    Some(&req.username),
                    &peer,
                    json!({"until": until}),
                );
                None
            }
            FailOutcome::Capacity => {
                tracing::error!("login lockout table reached its capacity");
                st.auth.audit.record(
                    "login_failed",
                    Some(&req.username),
                    &peer,
                    json!({"reason": "lockout_capacity"}),
                );
                return Err(ApiError::ServiceUnavailable(
                    "authentication temporarily unavailable".into(),
                ));
            }
        };
        tracing::warn!(user = %req.username, ?remaining, "login failed: invalid credentials");
        st.auth.audit.record(
            "login_failed",
            Some(&req.username),
            &peer,
            json!({"remaining": remaining}),
        );
        return Err(ApiError::BadCredentials);
    };
    st.auth.guard.lock.clear(&req.username);
    let (access, refresh) = st.auth.tokens.issue_pair(&req.username, role.as_u8(), now);
    if !st.auth.sessions.register_refresh(
        &refresh.1.jti,
        refresh.1.sub.clone(),
        refresh.1.role,
        refresh.1.exp,
        now,
    ) {
        return Err(ApiError::ServiceUnavailable(
            "session capacity reached".into(),
        ));
    }
    tracing::info!(user = %req.username, role = ?role, "login success");
    st.auth.audit.record(
        "login_success",
        Some(&req.username),
        &peer,
        json!({"role": role.as_str()}),
    );
    Ok(Json(token_pair(
        access.0,
        refresh.0,
        st.auth.tokens.access_ttl_secs(),
    )))
}

/// POST /api/v1/auth/refresh: refresh is single-use, rotated on success;
/// an old jti appearing again is treated as replay (REQ-WEB-008), security alert + 401.
async fn refresh_token<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Extension(peer): Extension<PeerCtx>,
    Json(req): Json<RefreshReq>,
) -> ApiResult<Json<Value>> {
    if req.refresh_token.is_empty() || req.refresh_token.len() > 4096 {
        return Err(ApiError::BadRequest("invalid refresh_token".into()));
    }
    let now = now_secs();
    let claims = st
        .auth
        .tokens
        .verify(&req.refresh_token, TokenKind::Refresh, now)?;
    let entry = match st.auth.sessions.consume_refresh(&claims.jti, now) {
        Ok(e) => e,
        Err(RefreshError::Replay) => {
            tracing::warn!(user = %claims.sub, jti = %claims.jti,
                "refresh token replay detected");
            st.auth.audit.record(
                "token_replay",
                Some(&claims.sub),
                &peer,
                json!({"kind": "refresh"}),
            );
            return Err(ApiError::TokenReplay);
        }
        Err(RefreshError::Unknown) => return Err(ApiError::TokenInvalid),
        Err(RefreshError::Capacity) => {
            return Err(ApiError::ServiceUnavailable(
                "session capacity reached".into(),
            ))
        }
    };
    let (access, refresh) = st.auth.tokens.issue_pair(&entry.sub, entry.role, now);
    if !st.auth.sessions.register_refresh(
        &refresh.1.jti,
        refresh.1.sub.clone(),
        refresh.1.role,
        refresh.1.exp,
        now,
    ) {
        return Err(ApiError::ServiceUnavailable(
            "session capacity reached".into(),
        ));
    }
    tracing::debug!(user = %entry.sub, "refresh token rotated");
    Ok(Json(token_pair(
        access.0,
        refresh.0,
        st.auth.tokens.access_ttl_secs(),
    )))
}

/// POST /api/v1/auth/logout: revoke the current access (until its exp) + optional refresh.
/// Idempotent: missing/expired refresh does not error.
async fn logout<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Extension(peer): Extension<PeerCtx>,
    Extension(Identity(access)): Extension<Identity>,
    payload: Option<Json<LogoutReq>>,
) -> ApiResult<Json<Value>> {
    let now = now_secs();
    let mut revoked_refresh = false;
    if !st.auth.sessions.deny_access(&access.jti, access.exp, now) {
        return Err(ApiError::ServiceUnavailable(
            "session revocation capacity reached".into(),
        ));
    }
    if let Some(Json(body)) = payload {
        if let Some(rt) = body.refresh_token.filter(|s| !s.is_empty()) {
            // Expired / forged refresh also does not affect logout success
            if let Ok(c) = st.auth.tokens.verify(&rt, TokenKind::Refresh, now) {
                if !st.auth.sessions.revoke_refresh(&c.jti, c.exp, now) {
                    return Err(ApiError::ServiceUnavailable(
                        "refresh revocation capacity reached".into(),
                    ));
                }
                revoked_refresh = true;
            }
        }
    }
    tracing::info!(user = %access.sub, "logout");
    st.auth.audit.record(
        "logout",
        Some(&access.sub),
        &peer,
        json!({"refresh_revoked": revoked_refresh}),
    );
    Ok(Json(json!({ "status": "logged_out" })))
}

/// POST /api/v1/auth/change-password — authenticated user changes own password.
/// Validates old password → argon2id-hashes new → UPDATE web_users.
/// Does NOT revoke existing sessions (token still valid); user must logout/login.
async fn change_password<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Extension(peer): Extension<PeerCtx>,
    Extension(Identity(access)): Extension<Identity>,
    Json(req): Json<ChangePasswordReq>,
) -> ApiResult<Json<Value>> {
    // Basic policy checks (delegate to PasswordPolicy for actual complexity rules)
    if req.old_password.is_empty() || req.new_password.is_empty() {
        return Err(ApiError::BadRequest("old and new password required".into()));
    }
    if req.new_password.len() > 256 {
        return Err(ApiError::BadRequest("new password too long".into()));
    }
    if req.old_password == req.new_password {
        return Err(ApiError::BadRequest(
            "new password must differ from old".into(),
        ));
    }

    // Password policy validation
    if let Err(e) = st.auth.password.validate(&req.new_password) {
        return Err(ApiError::BadRequest(format!("new password: {e}")));
    }

    // Fetch user from DB (authoritative source, same as login path)
    let u = match st.db.get_web_user(&access.sub).await {
        Ok(Some(user)) => user,
        Ok(None) => {
            tracing::warn!(user = %access.sub, "change-password: user not in DB");
            return Err(ApiError::BadCredentials);
        }
        Err(error) => {
            tracing::error!(user = %access.sub, error = %error, "change-password user lookup failed");
            st.auth.audit.record(
                "change_password_failed",
                Some(&access.sub),
                &peer,
                json!({"reason": "database_query_failed"}),
            );
            return Err(ApiError::DbUnavailable);
        }
    };
    if u.enabled != 1 {
        return Err(ApiError::BadCredentials);
    }

    // Verify old password (fail-closed: uniform "bad credentials" on any failure)
    use argon2::{Argon2, PasswordHash, PasswordVerifier};
    let Ok(parsed) = PasswordHash::new(&u.password_phc) else {
        tracing::warn!(user = %access.sub, "change-password: invalid stored PHC");
        return Err(ApiError::ServiceUnavailable(
            "account data corrupted".into(),
        ));
    };
    if Argon2::default()
        .verify_password(req.old_password.as_bytes(), &parsed)
        .is_err()
    {
        st.auth.audit.record(
            "change_password_failed",
            Some(&access.sub),
            &peer,
            json!({"reason": "bad_old_password"}),
        );
        return Err(ApiError::BadCredentials);
    }

    // Hash new password (argon2id, production params)
    let new_phc = crate::auth::accounts::hash_password_argon2id(&req.new_password)
        .map_err(|e| ApiError::ServiceUnavailable(format!("hash: {e}")))?;

    // Persist
    let ok = st.db.update_web_user_password(u.id, &new_phc).await;
    if !ok {
        return Err(ApiError::ServiceUnavailable("persist failed".into()));
    }

    // W-07 修复：密码修改后立即撤销该用户所有活跃 refresh session。
    // 之前的 refresh token 不能再续期，必须用新密码重新登录。
    let now_secs = crate::auth::tokens::now_secs();
    let revoked = st.auth.sessions.revoke_all_for_subject(&access.sub, now_secs);
    if revoked > 0 {
        tracing::info!(user = %access.sub, revoked,
            "password changed — revoked {} active refresh sessions", revoked);
    }

    tracing::info!(user = %access.sub, "password changed");
    st.auth
        .audit
        .record("change_password", Some(&access.sub), &peer, json!({}));
    Ok(Json(json!({ "status": "password_changed" })))
}

fn token_pair(access_token: String, refresh_token: String, expires_in: u64) -> Value {
    json!({
        "access_token": access_token,
        "refresh_token": refresh_token,
        "token_type": "Bearer",
        "expires_in": expires_in,
    })
}
