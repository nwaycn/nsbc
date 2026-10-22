//! Route strategy group REST (REQ-WEB-016). Six endpoints: list/get/create/update/delete/reload.
//! Module name route_groups (avoid name collision with axum::routing submodule);
//! URL paths remain at `/routes`.

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
        .route("/routes", get(list::<C>).post(create::<C>))
        .route("/routes/reload", post(reload::<C>))
        .route(
            "/routes/:id",
            get(get_one::<C>).put(update::<C>).delete(delete_one::<C>),
        )
        .layer(ServiceBuilder::new().layer(TraceLayer::new_for_http()))
}

#[derive(Debug, Deserialize, Default)]
pub struct ListQuery {
    pub enabled: Option<String>,
    pub limit:  Option<i64>,
    pub offset: Option<i64>,
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
        .list_routes(enabled, q.limit.unwrap_or(50).clamp(1, 500), q.offset.unwrap_or(0).max(0))
        .await.map_err(|e| ApiError::Db(e.to_string()))?;
    Ok(Json(json!({ "items": items, "total": total })))
}

pub async fn get_one<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.get_route(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("route not found".into()))?;
    Ok(Json(row))
}

pub async fn create<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<crate::db::RouteCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.create_route(body).await.map_err(|e| {
        if let sqlx::Error::Database(ref db_err) = e {
            let msg = db_err.message().to_ascii_lowercase();
            if msg.contains("unique") || msg.contains("duplicate") {
                return ApiError::Conflict("route name already exists".into());
            }
        }
        ApiError::Db(e.to_string())
    })?;
    let _ = st.db.insert_config_history(
        "api", "create", &format!("routes/{}", row.id),
        None, &format!("route={} strategy={}", row.name, row.strategy),
    ).await;
    Ok((StatusCode::CREATED, Json(row)))
}

pub async fn update<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
    Json(body): Json<crate::db::RouteUpdate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.update_route(id, body).await.map_err(|e| match e {
        sqlx::Error::RowNotFound => ApiError::NotFound("route not found".into()),
        other => ApiError::Db(other.to_string()),
    })?;
    let _ = st.db.insert_config_history(
        "api", "update", &format!("routes/{}", row.id),
        None, &format!("route={}", row.name),
    ).await;
    Ok(Json(row))
}

pub async fn delete_one<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let deleted = st.db.delete_route(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    if !deleted {
        return Err(ApiError::NotFound("route not found".into()));
    }
    let _ = st.db.insert_config_history(
        "api", "delete", &format!("routes/{id}"), None, "deleted",
    ).await;
    Ok(StatusCode::NO_CONTENT)
}

pub async fn reload<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let rows = st.db.list_enabled_routes().await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    tracing::info!(count = rows.len(), "routes.reload requested — UDS command not yet wired");
    Ok(Json(json!({
        "loaded": rows.len(),
        "uds_wired": false,
        "message": "routes.reload UDS command will be wired after Linux compile"
    })))
}
