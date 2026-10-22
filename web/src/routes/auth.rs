//! 登录/刷新/登出（REQ-WEB-004 登录与会话、REQ-WEB-008 会话）。
//!
//! 两个公开端点（login/refresh）挂在 rbac 层之外（但仍过版本治理与体限）；
//! logout 为受保护端点（出示 access 令牌吊销自身 + 可选 refresh）。
//!
//! 错误语义（REQ-WEB-018）：用户名不存在与口令错误**同一条** 401，
//! 不回显账号是否存在；令牌类错误不回传具体 JWT 内容。
//! 登录失败锁定/接口限流在 W3b（REQ-WEB-009），本单元只留安全日志。
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
    Router::new().route("/auth/logout", post(logout::<C>))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)] // REQ-WEB-009：拒绝未知字段
struct LoginReq {
    username: String,
    password: String,
    /// 验证码 ID（可选：若配置关闭或首次使用可空）
    captcha_id: Option<String>,
    /// 验证码答案（可选；存在时必须正确）
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

/// POST /api/v1/auth/login → access(15min)+refresh(8h)。
async fn login<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Extension(peer): Extension<PeerCtx>,
    Json(req): Json<LoginReq>,
) -> ApiResult<Json<Value>> {
    // 白名单式入参校验（REQ-WEB-009）；口令长度按字节上限，字符不限制（允许任意 Unicode）。
    if !valid_username(&req.username) || req.password.is_empty() || req.password.len() > 256 {
        return Err(ApiError::BadRequest("invalid username or password".into()));
    }

    // 验证码强制校验（REQ-WEB-009：必须先 GET /auth/captcha 拿到 captcha_id，
    // 然后 login 时带 captcha_id + captcha_code，verify 正确才继续）。
    // captcha_id/captcha_code 缺失或为空 → 直接拒，不走密码校验路径（省 CPU）。
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
    // 锁定检查先于 argon2 校验（省 CPU、无账号枚举差异之外的信息泄漏）。
    if st.auth.guard.lock.is_locked(&req.username, now) {
        tracing::warn!(user = %req.username, "login blocked: account locked");
        st.auth.audit.record(
            "login_locked",
            Some(&req.username),
            &peer,
            json!({}),
        );
        return Err(ApiError::AccountLocked);
    }

    // 账号验证：只从数据库 web_users 表查询（唯一权威账号源）。
    // 彻底废弃配置文件 auth.accounts、NSBC_AUTH_FILE、NSBC_AUTH_ACCOUNTS
    // 等文本账号源，登录一律走 DB。
    let verified_role: Option<crate::auth::Role> = {
        let db_user = st.db.get_web_user(&req.username).await;
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
            None // DB 未配置 / 用户不存在 → 统一当作凭据错误
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
    let (access, refresh) = st
        .auth
        .tokens
        .issue_pair(&req.username, role.as_u8(), now);
    st.auth
        .sessions
        .register_refresh(&refresh.1.jti, refresh.1.sub.clone(), refresh.1.role, refresh.1.exp);
    tracing::info!(user = %req.username, role = ?role, "login success");
    st.auth.audit.record(
        "login_success",
        Some(&req.username),
        &peer,
        json!({"role": role.as_str()}),
    );
    Ok(Json(token_pair(access.0, refresh.0, st.auth.tokens.access_ttl_secs())))
}

/// POST /api/v1/auth/refresh：refresh 单次使用，成功即轮转；
/// 旧 jti 再出现视为重放（REQ-WEB-008），安全告警并 401。
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
    };
    let (access, refresh) = st
        .auth
        .tokens
        .issue_pair(&entry.sub, entry.role, now);
    st.auth.sessions.register_refresh(
        &refresh.1.jti,
        refresh.1.sub.clone(),
        refresh.1.role,
        refresh.1.exp,
    );
    tracing::debug!(user = %entry.sub, "refresh token rotated");
    Ok(Json(token_pair(access.0, refresh.0, st.auth.tokens.access_ttl_secs())))
}

/// POST /api/v1/auth/logout：吊销当前 access（至其 exp）+ 可选 refresh。
/// 幂等：refresh 缺失/过期不报错。
async fn logout<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Extension(peer): Extension<PeerCtx>,
    Extension(Identity(access)): Extension<Identity>,
    payload: Option<Json<LogoutReq>>,
) -> ApiResult<Json<Value>> {
    let now = now_secs();
    let mut revoked_refresh = false;
    st.auth.sessions.deny_access(&access.jti, access.exp, now);
    if let Some(Json(body)) = payload {
        if let Some(rt) = body.refresh_token.filter(|s| !s.is_empty()) {
            // refresh 过期/伪造也不影响登出成功
            if let Ok(c) = st.auth.tokens.verify(&rt, TokenKind::Refresh, now) {
                st.auth.sessions.revoke_refresh(&c.jti, c.exp, now);
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

fn token_pair(access_token: String, refresh_token: String, expires_in: u64) -> Value {
    json!({
        "access_token": access_token,
        "refresh_token": refresh_token,
        "token_type": "Bearer",
        "expires_in": expires_in,
    })
}
