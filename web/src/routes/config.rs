//! Config interface (REQ-WEB-004 / REQ-CFG-011/013):
//! - GET /config       Full redacted effective snapshot (C2: forwards core `config.export`)
//! - GET /config/:key  Effective value read-only query (C1: forwards core `config.get`)
//! - POST /config/set         Hot-set (L1, forwards core `config.set`; audited to config_history)
//! - POST /config/validate    Dry-run validation (L0, forwards core `config.validate`, no state change)
//!
//! Note: the operator identity for config.set (L1 user identity) is already recorded
//! by the rbac middleware's independent auditor (includes sub/username/method/path/status),
//! so config_history only keeps the config-level change snapshot.
use std::sync::Arc;

use axum::extract::{Path, State};
use axum::routing::{get, post};
use axum::{Json, Router};
use serde::Deserialize;
use serde_json::json;

use crate::coreclient::{ConfigSetResult, CoreApi, CoreError};
use crate::error::{ApiError, ApiResult};
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/config", get(get_config::<C>))
        .route("/config/:key", get(get_config_key::<C>))
        .route("/config/set", post(post_config_set::<C>))
        .route("/config/validate", post(post_config_validate::<C>))
        .route("/listens", get(listens))
}

/// Config key route-layer validation: 1..127 bytes, charset [A-Za-z0-9._-], no leading/trailing
/// dot, no consecutive dots (dual-end aligned with core obs_control valid_cfg_key).
fn valid_config_key(key: &str) -> bool {
    let n = key.len();
    n >= 1
        && n < 128
        && !key.starts_with('.')
        && !key.ends_with('.')
        && !key.contains("..")
        && key.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-')
}

/// Request body (value allows empty-string delete semantics; route layer does not truncate,
/// core-side valid_cfg_value does the validation).
#[derive(Debug, Deserialize)]
pub struct ConfigWriteReq {
    pub key: String,
    #[serde(default)]
    pub value: String,
}

/// GET /api/v1/config/{key}: core effective value (L0 read-only, REQ-CFG-011).
/// Key not found → 404; key format illegal → 400 (no control command sent); core unreachable → 502/504.
async fn get_config_key<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(key): Path<String>,
) -> ApiResult<Json<crate::coreclient::ConfigValue>> {
    if !valid_config_key(&key) {
        return Err(ApiError::BadRequest("invalid config key".into()));
    }
    match st.core.config_get(&key).await {
        Ok(v) => Ok(Json(v)),
        Err(CoreError::NotFound(_)) => Err(ApiError::ConfigKeyNotFound),
        Err(e) => Err(e.into()),
    }
}

/// GET /api/v1/config: core full redacted effective snapshot (L0 read-only, REQ-CFG-013).
/// Core unreachable → 502/504; sensitive items value=null/secret=true, **** does not leave the sidecar.
async fn get_config<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<crate::coreclient::ConfigExport>> {
    Ok(Json(st.core.config_export().await?))
}

/// POST /api/v1/config/validate: dry-run value validation (L0, REQ-CFG-011).
/// Valid → 200 `{"valid":true,"key":...}`; invalid → 400 + core-side reason.
async fn post_config_validate<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(req): Json<ConfigWriteReq>,
) -> ApiResult<Json<serde_json::Value>> {
    if !valid_config_key(&req.key) {
        return Err(ApiError::BadRequest("invalid config key".into()));
    }
    match st.core.config_validate(&req.key, &req.value).await {
        Ok(()) => Ok(Json(json!({ "valid": true, "key": req.key }))),
        Err(CoreError::CoreRejected(reason)) => Err(ApiError::BadRequest(reason)),
        Err(e) => Err(e.into()),
    }
}

/// POST /api/v1/config/set: hot-set (L1, REQ-CFG-011).
/// Success → 200 + ConfigSetResult; core rejected → 400 + reason; backend stays 502/504 unchanged.
/// After success, writes config_history (silently skipped if DB not configured).
async fn post_config_set<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<ConfigWriteReq>,
) -> ApiResult<Json<ConfigSetResult>> {
    if !valid_config_key(&body.key) {
        return Err(ApiError::BadRequest("invalid config key".into()));
    }

    let result = match st.core.config_set(&body.key, &body.value).await {
        Ok(r) => r,
        Err(CoreError::CoreRejected(reason)) => return Err(ApiError::BadRequest(reason)),
        Err(e) => return Err(e.into()),
    };

    // DB config_history write: actor is a Web-side L1 operation placeholder (operator
    // identity already recorded independently by the rbac middleware auditor).
    // Silently skipped when DB is not configured; degraded on write failure (do NOT
    // block API success response, avoid audit back-pressure on business flow).
    if let Err(e) = st
        .db
        .insert_config_history(
            "api",
            if result.changed { "set" } else { "set-noop" },
            &result.key,
            None, // Atomic set cannot pre-read old_value — leave empty
            &body.value,
        )
        .await
    {
        tracing::warn!(error = %e, key = %result.key,
            "config_history write failed — set committed but history missed");
    }

    Ok(Json(result))
}

async fn listens() -> ApiResult<Json<serde_json::Value>> {
    Ok(Json(json!({
        "status": "stub",
        "listens": [],
    })))
}
