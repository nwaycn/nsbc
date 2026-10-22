//! 中继管理 REST（REQ-WEB-016）。6 端点：list/get/create/update/delete/reload。

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
        .route("/trunks", get(list::<C>).post(create::<C>))
        .route("/trunks/reload", post(reload::<C>))
        .route(
            "/trunks/:id",
            get(get_one::<C>)
                .put(update::<C>)
                .delete(delete_one::<C>),
        )
        .layer(ServiceBuilder::new().layer(TraceLayer::new_for_http()))
}

#[derive(Debug, Deserialize, Default)]
pub struct ListQuery {
    pub tenant_id:     Option<i64>,
    pub gateway_name: Option<String>,
    pub enabled:       Option<String>,
    pub limit:        Option<i64>,
    pub offset:       Option<i64>,
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
        .list_trunks(
            q.tenant_id,
            q.gateway_name.as_deref(),
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
    let row = st.db.get_trunk(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("trunk not found".into()))?;
    Ok(Json(row))
}

pub async fn create<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<crate::db::TrunkCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.create_trunk(body).await.map_err(|e| {
        if let sqlx::Error::Database(ref db_err) = e {
            let msg = db_err.message().to_ascii_lowercase();
            if msg.contains("unique") || msg.contains("duplicate") {
                return ApiError::Conflict("trunk name already exists".into());
            }
        }
        ApiError::Db(e.to_string())
    })?;
    let _ = st.db.insert_config_history(
        "api", "create", &format!("trunks/{}", row.id),
        None, &format!("trunk={} gateway={}", row.name, row.gateway_name),
    ).await;
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::Trunks);
    Ok((StatusCode::CREATED, Json(row)))
}

pub async fn update<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
    Json(body): Json<crate::db::TrunkUpdate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.update_trunk(id, body).await.map_err(|e| match e {
        sqlx::Error::RowNotFound => ApiError::NotFound("trunk not found".into()),
        other => ApiError::Db(other.to_string()),
    })?;
    let _ = st.db.insert_config_history(
        "api", "update", &format!("trunks/{}", row.id),
        None, &format!("trunk={}", row.name),
    ).await;
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::Trunks);
    Ok(Json(row))
}

pub async fn delete_one<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let deleted = st.db.delete_trunk(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    if !deleted {
        return Err(ApiError::NotFound("trunk not found".into()));
    }
    let _ = st.db.insert_config_history(
        "api", "delete", &format!("trunks/{id}"), None, "deleted",
    ).await;
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::Trunks);
    Ok(StatusCode::NO_CONTENT)
}

pub async fn reload<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let rows = st.db.list_enabled_trunks().await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    let json = serde_json::to_string(&rows).map_err(|e| ApiError::Db(e.to_string()))?;
    st.core.reload_trunks(&json).await
        .map_err(|_| ApiError::CoreUnavailable)?;
    tracing::info!(count = rows.len(), "trunks.reload pushed to core");
    Ok(Json(json!({ "loaded": rows.len() })))
}
