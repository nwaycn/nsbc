//! AI backend configuration REST — 2026-11-19 NEW
//! Six endpoints: list / get / create / update / delete / reload.

use std::sync::Arc;

use axum::{
    extract::{Path, Query, State},
    http::StatusCode,
    response::IntoResponse,
    routing::get,
    Json, Router,
};
use serde::Deserialize;
use serde_json::json;

use crate::coreclient::CoreApi;
use crate::db::{AiBackendCreate, AiBackendUpdate};
use crate::error::ApiError;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/ai-backends", get(list::<C>).post(create::<C>))
        .route(
            "/ai-backends/:id",
            get(get_one::<C>).put(update::<C>).delete(delete_one::<C>),
        )
}

#[derive(Debug, Deserialize, Default)]
pub struct ListQuery {
    pub enabled: Option<String>,
    pub keyword: Option<String>,
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
    let items = st.db
        .list_ai_backends(enabled, q.keyword.as_deref())
        .await.map_err(|e| ApiError::Db(e.to_string()))?;
    Ok(Json(json!({ "items": items, "total": items.len() })))
}

pub async fn get_one<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.get_ai_backend(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("ai backend not found".into()))?;
    Ok(Json(row))
}

pub async fn create<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<AiBackendCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    // 唯一性校验：name 不能重复
    if st.db.get_ai_backend_by_name(&body.name).await
        .map_err(|e| ApiError::Db(e.to_string()))?.is_some()
    {
        return Err(ApiError::BadRequest(format!(
            "ai backend '{}' already exists", body.name
        )));
    }

    let row = st.db.create_ai_backend(body).await.map_err(|e| {
        if let sqlx::Error::Database(ref db_err) = e {
            let msg = db_err.message().to_ascii_lowercase();
            if msg.contains("unique") || msg.contains("duplicate") {
                ApiError::BadRequest("ai backend name already exists".into())
            } else {
                ApiError::Db(e.to_string())
            }
        } else {
            ApiError::Db(e.to_string())
        }
    })?;
    Ok((StatusCode::CREATED, Json(row)))
}

pub async fn update<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
    Json(body): Json<AiBackendUpdate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    // 存在性校验
    let _ = st.db.get_ai_backend(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("ai backend not found".into()))?;

    let row = st.db.update_ai_backend(id, body).await.map_err(|e| {
        if let sqlx::Error::Database(ref db_err) = e {
            let msg = db_err.message().to_ascii_lowercase();
            if msg.contains("unique") || msg.contains("duplicate") {
                ApiError::BadRequest("ai backend name conflict".into())
            } else {
                ApiError::Db(e.to_string())
            }
        } else {
            ApiError::Db(e.to_string())
        }
    })?;
    Ok(Json(row))
}

pub async fn delete_one<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let deleted = st.db.delete_ai_backend(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    if !deleted {
        return Err(ApiError::NotFound("ai backend not found".into()));
    }
    Ok(StatusCode::NO_CONTENT)
}
