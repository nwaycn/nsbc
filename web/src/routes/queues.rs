//! 分机队列 REST。6 端点。

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
where C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/queues", get(list::<C>).post(create::<C>))
        .route("/queues/reload", post(reload::<C>))
        .route("/queues/:id",
            get(get_one::<C>).put(update::<C>).delete(delete_one::<C>))
        .layer(ServiceBuilder::new().layer(TraceLayer::new_for_http()))
}

#[derive(Debug, Deserialize, Default)]
pub struct ListQuery {
    pub tenant_id: Option<i64>,
    pub enabled: Option<String>,
    pub limit:  Option<i64>,
    pub offset: Option<i64>,
}

pub async fn list<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>, Query(q): Query<ListQuery>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let enabled = match q.enabled.as_deref() { Some("true") => Some(true), Some("false") => Some(false), _ => None };
    let (items, total) = st.db
        .list_queues(q.tenant_id, enabled, q.limit.unwrap_or(50).clamp(1, 500), q.offset.unwrap_or(0).max(0))
        .await.map_err(|e| ApiError::Db(e.to_string()))?;
    Ok(Json(json!({ "items": items, "total": total })))
}

pub async fn get_one<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>, Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.get_queue(id).await.map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("queue not found".into()))?;
    Ok(Json(row))
}

pub async fn create<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>, Json(body): Json<crate::db::QueueCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.create_queue(body).await.map_err(|e| {
        if let sqlx::Error::Database(ref de) = e {
            let m = de.message().to_ascii_lowercase();
            if m.contains("unique") || m.contains("duplicate") { return ApiError::Conflict("queue name already exists".into()); }
        }
        ApiError::Db(e.to_string())
    })?;
    let _ = st.db.insert_config_history("api","create",&format!("queues/{}",row.id),None,&format!("q={}",row.name)).await;
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::Queues);
    Ok((StatusCode::CREATED, Json(row)))
}

pub async fn update<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>, Path(id): Path<i64>,
    Json(body): Json<crate::db::QueueUpdate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.update_queue(id, body).await.map_err(|e| match e {
        sqlx::Error::RowNotFound => ApiError::NotFound("queue not found".into()),
        other => ApiError::Db(other.to_string()),
    })?;
    let _ = st.db.insert_config_history("api","update",&format!("queues/{}",row.id),None,&format!("q={}",row.name)).await;
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::Queues);
    Ok(Json(row))
}

pub async fn delete_one<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>, Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    if !st.db.delete_queue(id).await.map_err(|e| ApiError::Db(e.to_string()))? {
        return Err(ApiError::NotFound("queue not found".into()));
    }
    let _ = st.db.insert_config_history("api","delete",&format!("queues/{id}"),None,"deleted").await;
    crate::sync::spawn_push(st.clone(), crate::sync::Kind::Queues);
    Ok(StatusCode::NO_CONTENT)
}

pub async fn reload<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    // list_enabled_queues 返回 (Queue, tenant_name) tuple — 手动构造 QueueSyncDto
    let rows = st.db.list_enabled_queues().await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    let dtos: Vec<serde_json::Value> = rows.into_iter().map(|(q, tenant_name)| {
        let members: Vec<String> = serde_json::from_str(&q.members_json).unwrap_or_default();
        serde_json::json!({
            "tenant": tenant_name,
            "name": q.name,
            "enabled": q.enabled != 0,
            "strategy": q.ring_strategy,
            "max_wait_s": q.max_wait_sec,
            "wrap_time_s": 30,
            "require_skill": "",
            "members": members,
        })
    }).collect();
    let json = serde_json::to_string(&dtos).map_err(|e| ApiError::Db(e.to_string()))?;
    st.core.reload_queues(&json).await
        .map_err(|_| ApiError::CoreUnavailable)?;
    tracing::info!(count = dtos.len(), "queues.reload pushed to core");
    let _ = st.db.insert_config_history(
        "api", "reload", "queues", None,
        &format!("loaded {} enabled queues", dtos.len()),
    ).await;
    Ok(Json(json!({ "loaded": dtos.len() })))
}
