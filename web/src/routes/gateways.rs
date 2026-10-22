//! 外线网关管理 REST（REQ-WEB-016）。6 端点：list/get/create/update/delete/reload。

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
        .route("/gateways", get(list::<C>).post(create::<C>))
        .route("/gateways/reload", post(reload::<C>))
        .route(
            "/gateways/:id",
            get(get_one::<C>)
                .put(update::<C>)
                .delete(delete_one::<C>),
        )
        .layer(ServiceBuilder::new().layer(TraceLayer::new_for_http()))
}

#[derive(Debug, Deserialize, Default)]
pub struct ListQuery {
    pub limit:  Option<i64>,
    pub offset: Option<i64>,
}

pub async fn list<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Query(q): Query<ListQuery>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let (items, total) = st.db
        .list_gateways(q.limit.unwrap_or(50).clamp(1, 500), q.offset.unwrap_or(0).max(0))
        .await.map_err(|e| ApiError::Db(e.to_string()))?;
    Ok(Json(json!({ "items": items, "total": total })))
}

pub async fn get_one<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.get_gateway(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("gateway not found".into()))?;
    Ok(Json(row))
}

pub async fn create<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<crate::db::GatewayCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.create_gateway(body).await.map_err(|e| {
        if let sqlx::Error::Database(ref db_err) = e {
            let msg = db_err.message().to_ascii_lowercase();
            if msg.contains("unique") || msg.contains("duplicate") {
                return ApiError::Conflict("gateway name already exists".into());
            }
        }
        ApiError::Db(e.to_string())
    })?;
    let _ = st.db.insert_config_history(
        "api", "create", &format!("gateways/{}", row.id),
        None, &format!("gateway={}", row.name),
    ).await;
    Ok((StatusCode::CREATED, Json(row)))
}

pub async fn update<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
    Json(body): Json<crate::db::GatewayUpdate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.update_gateway(id, body).await.map_err(|e| match e {
        sqlx::Error::RowNotFound => ApiError::NotFound("gateway not found".into()),
        other => ApiError::Db(other.to_string()),
    })?;
    let _ = st.db.insert_config_history(
        "api", "update", &format!("gateways/{}", row.id),
        None, &format!("gateway={}", row.name),
    ).await;
    Ok(Json(row))
}

pub async fn delete_one<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let deleted = st.db.delete_gateway(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    if !deleted {
        return Err(ApiError::NotFound("gateway not found".into()));
    }
    let _ = st.db.insert_config_history(
        "api", "delete", &format!("gateways/{id}"), None, "deleted",
    ).await;
    Ok(StatusCode::NO_CONTENT)
}

pub async fn reload<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let rows = st.db.list_enabled_gateways().await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    let json = serde_json::to_string(&rows).map_err(|e| ApiError::Db(e.to_string()))?;
    st.core.reload_gateways(&json).await
        .map_err(|_| ApiError::CoreUnavailable)?;
    tracing::info!(count = rows.len(), "gateways.reload pushed to core");
    Ok(Json(json!({ "loaded": rows.len() })))
}
