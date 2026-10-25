//! Dialplan rule REST (REQ-PBX-DP-001). Six endpoints: list/get/create/update/delete/reload.

use std::sync::Arc;

use axum::{
    extract::{Path, Query, State},
    http::StatusCode,
    response::IntoResponse,
    routing::{get, post},
    Json, Router,
};
use serde::Deserialize;
use serde_json::json;
use tower::ServiceBuilder;
use tower_http::trace::TraceLayer;

use crate::coreclient::CoreApi;
use crate::error::ApiError;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/dialplan-rules", get(list::<C>).post(create::<C>))
        .route("/dialplan-rules/reload", post(reload::<C>))
        .route(
            "/dialplan-rules/:id",
            get(get_one::<C>).put(update::<C>).delete(delete_one::<C>),
        )
        .layer(ServiceBuilder::new().layer(TraceLayer::new_for_http()))
}

#[derive(Debug, Deserialize, Default)]
pub struct ListQuery {
    pub tenant_id: Option<i64>,
    pub layer:  Option<String>,
    pub enabled: Option<String>,
    pub limit:  Option<i64>,
    pub offset: Option<i64>,
}

/// P2-security: validate each app name inside the app_chain JSON array against the
/// core's registered name list. Invalid app → 400 Bad Request (no dirty data in DB).
/// Skip validation when core is unavailable (dialplan module not loaded) — the executor
/// will report NOENT on its own.
fn validate_app_chain<S: AsRef<str>>(
    app_chain_json: S,
    known_apps: &[String],
) -> Result<(), String> {
    let json = app_chain_json.as_ref();
    if json.trim().is_empty() { return Ok(()); }
    let arr: serde_json::Value = serde_json::from_str(json)
        .map_err(|e| format!("app_chain JSON invalid: {e}"))?;
    let arr = arr.as_array()
        .ok_or_else(|| "app_chain must be JSON array".to_string())?;
    for (i, entry) in arr.iter().enumerate() {
        let app_name = entry
            .get("app")
            .and_then(|v| v.as_str())
            .ok_or_else(|| format!("app_chain[{i}].app missing or not a string"))?;
        if !known_apps.iter().any(|a| a == app_name) {
            return Err(format!(
                "app_chain[{i}] app '{app_name}' not registered in core (dialplan module loaded?)"
            ));
        }
    }
    Ok(())
}

pub async fn list<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Query(q): Query<ListQuery>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let enabled = match q.enabled.as_deref() {
        Some("true")  => Some(true),
        Some("false") => Some(false),
        _ => None,
    };
    let (items, total) = st.db
        .list_dialplan_rules(
            q.tenant_id,
            q.layer.as_deref(),
            enabled,
            q.limit.unwrap_or(50).clamp(1, 500),
            q.offset.unwrap_or(0).max(0),
        )
        .await.map_err(|e| ApiError::Db(e.to_string()))?;
    Ok(Json(json!({ "items": items, "total": total })))
}

pub async fn get_one<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.get_dialplan_rule(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("dialplan rule not found".into()))?;
    Ok(Json(row))
}

pub async fn create<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<crate::db::DialplanRuleCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    // P2-security: app_chain whitelist validation (skip when core dialplan module is
    // not loaded; an empty known_apps array will make a non-empty app_chain all fail —
    // which is correct: you cannot request a non-existent app when the dialplan
    // module is disabled)
    let app_chain = body.app_chain.as_str();
    if !app_chain.trim().is_empty() {
        let known: Vec<String> = st.core.dialplan_apps().await
            .map_err(|_| ApiError::CoreUnavailable)?
            .into_iter().map(|a| a.name).collect();
        validate_app_chain(app_chain, &known)
            .map_err(|msg| ApiError::BadRequest(msg))?;
    }

    let row = st.db.create_dialplan_rule(body).await.map_err(|e| {
        if let sqlx::Error::Database(ref db_err) = e {
            let msg = db_err.message().to_ascii_lowercase();
            if msg.contains("unique") || msg.contains("duplicate") {
                return ApiError::Conflict("dialplan rule name already exists".into());
            }
        }
        ApiError::Db(e.to_string())
    })?;
    let _ = st.db.insert_config_history(
        "api", "create", &format!("dialplan-rules/{}", row.id),
        None, &format!("rule={} layer={} target={}", row.name, row.layer, row.target),
    ).await;
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::DialplanRules);
    Ok((StatusCode::CREATED, Json(row)))
}

pub async fn update<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
    Json(body): Json<crate::db::DialplanRuleUpdate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    // P2-security: only validate when app_chain is being updated
    if let Some(ref app_chain) = body.app_chain {
        if !app_chain.trim().is_empty() {
            let known: Vec<String> = st.core.dialplan_apps().await
                .map_err(|_| ApiError::CoreUnavailable)?
                .into_iter().map(|a| a.name).collect();
            validate_app_chain(app_chain, &known)
                .map_err(|msg| ApiError::BadRequest(msg))?;
        }
    }

    let row = st.db.update_dialplan_rule(id, body).await.map_err(|e| match e {
        sqlx::Error::RowNotFound => ApiError::NotFound("dialplan rule not found".into()),
        other => ApiError::Db(other.to_string()),
    })?;
    let _ = st.db.insert_config_history(
        "api", "update", &format!("dialplan-rules/{}", row.id),
        None, &format!("rule={}", row.name),
    ).await;
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::DialplanRules);
    Ok(Json(row))
}

pub async fn delete_one<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let deleted = st.db.delete_dialplan_rule(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    if !deleted {
        return Err(ApiError::NotFound("dialplan rule not found".into()));
    }
    let _ = st.db.insert_config_history(
        "api", "delete", &format!("dialplan-rules/{id}"), None, "deleted",
    ).await;
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::DialplanRules);
    Ok(StatusCode::NO_CONTENT)
}

pub async fn reload<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let rows = st.db.list_enabled_dialplan_rules(None).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    let json = serde_json::to_string(&rows).map_err(|e| ApiError::Db(e.to_string()))?;
    use crate::coreclient::CoreError;
    match st.core.reload_dialplan_rules(&json).await {
        Ok(()) => {}
        Err(CoreError::CoreRejected(msg)) => {
            // Core 返回 "ERR dialplan-rules.reload failed: <具体错误>"
            // 把 msg 透传给前端 — 帮助排障（比如哪条 JSON 字段缺失/格式错误）
            tracing::warn!("dialplan-rules.reload core rejected: {msg}");
            return Err(ApiError::BadRequest(
                format!("core rejected reload: {msg}"),
            ));
        }
        Err(e) => return Err(ApiError::from(e)),
    }
    tracing::info!(count = rows.len(), "dialplan-rules.reload pushed to core");
    Ok(Json(json!({ "loaded": rows.len() })))
}
