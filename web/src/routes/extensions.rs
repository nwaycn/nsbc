//! Extension management REST (REQ-WEB-016).
//!
//! Six endpoints: list/detail/create/update/delete + a reload that pushes to the core.
//! DB not configured (Db::new() / pool=None) → all endpoints return 503 DbUnavailable;
//! does not block main business startup (REQ-WEB-013 optional persistence design).
//!
//! Reload endpoint (POST /extensions/reload):
//!   Query DB for all enabled extensions → ExtSyncDto (strips password, only sends HA1) →
//!   UDS command `extensions.reload <json>` batch push to core.
//!   CRUD save also auto-pushes via sync::spawn_push (300ms debounced merge);
//!   reload endpoint is kept as a manual full-total fallback.

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
        // reload registered before /:id — axum matches by registration order, prevents
        // /reload from being consumed as :id
        .route("/extensions", get(list::<C>).post(create::<C>))
        .route("/extensions/reload", post(reload::<C>))
        .route(
            "/extensions/:id",
            get(get_one::<C>)
                .put(update::<C>)
                .delete(delete_one::<C>),
        )
        .layer(ServiceBuilder::new().layer(TraceLayer::new_for_http()))
}

// --- list ---

#[derive(Debug, Deserialize, Default)]
pub struct ListQuery {
    pub tenant_id: Option<i64>,
    pub extension: Option<String>,
    pub realm:     Option<String>,
    pub enabled:    Option<String>,   // "true"/"false" or empty
    pub limit:     Option<i64>,
    pub offset:    Option<i64>,
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
    let limit  = q.limit.unwrap_or(50).clamp(1, 500);
    let offset = q.offset.unwrap_or(0).max(0);

    let (items, total) = st.db
        .list_extensions(
            q.tenant_id,
            q.extension.as_deref(),
            q.realm.as_deref(),
            enabled,
            limit, offset,
        )
        .await.map_err(|e| ApiError::Db(e.to_string()))?;

    Ok(Json(json!({ "items": items, "total": total })))
}

// --- get one ---

pub async fn get_one<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let row = st.db.get_extension(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("extension not found".into()))?;
    Ok(Json(row))
}

// --- create ---

pub async fn create<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<crate::db::ExtensionCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let row = st.db.create_extension(body).await.map_err(|e| {
        // UNIQUE conflict → 409 Conflict
        if let sqlx::Error::Database(ref db_err) = e {
            let msg = db_err.message().to_ascii_lowercase();
            if msg.contains("unique") || msg.contains("duplicate") {
                return ApiError::Conflict(
                    "extension+realm already exists".into()
                );
            }
        }
        ApiError::Db(e.to_string())
    })?;

    // Audit (reuses config_history)
    let _ = st.db.insert_config_history(
        "api", "create",
        &format!("extensions/{}", row.id),
        None, &format!("extension={} realm={}", row.extension, row.realm),
    ).await;

    // After save succeeds → push to core (300ms debounced merge)
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::Extensions);

    Ok((StatusCode::CREATED, Json(row)))
}

// --- update ---

pub async fn update<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
    Json(body): Json<crate::db::ExtensionUpdate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let row = st.db.update_extension(id, body).await.map_err(|e| {
        match e {
            sqlx::Error::RowNotFound => ApiError::NotFound("extension not found".into()),
            other => ApiError::Db(other.to_string()),
        }
    })?;

    let _ = st.db.insert_config_history(
        "api", "update",
        &format!("extensions/{}", row.id),
        None, &format!("extension={}", row.extension),
    ).await;

    crate::sync::spawn_push(st.clone(), crate::sync::Kind::Extensions);

    Ok(Json(row))
}

// --- delete ---

pub async fn delete_one<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let deleted = st.db.delete_extension(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    if !deleted {
        return Err(ApiError::NotFound("extension not found".into()));
    }

    let _ = st.db.insert_config_history(
        "api", "delete",
        &format!("extensions/{id}"), None, "deleted",
    ).await;

    crate::sync::spawn_push(st.clone(), crate::sync::Kind::Extensions);

    Ok(StatusCode::NO_CONTENT)
}

// --- reload (batch push to core UDS) ---

pub async fn reload<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let rows = st.db.list_enabled_extensions().await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    // ExtSyncDto: password never leaves the Web side (C side only needs HA1 for Digest verification)
    let dtos: Vec<crate::sync::ExtSyncDto> =
        rows.iter().map(crate::sync::ExtSyncDto::from).collect();
    let json = serde_json::to_string(&dtos).map_err(|e| ApiError::Db(e.to_string()))?;
    st.core.reload_extensions(&json).await
        .map_err(|_| ApiError::CoreUnavailable)?;
    tracing::info!(count = dtos.len(), "extensions.reload pushed to core");
    Ok(Json(json!({ "loaded": dtos.len() })))
}
