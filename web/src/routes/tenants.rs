//! 租户管理 REST（REQ-TENANT-001）。
//!
//! 五个端点：列表/详情/创建/更新/删除。
//! DB 未配置 → 所有端点返回 503。
//! 默认租户 (id=1) 不可删除。

use std::sync::Arc;

use axum::{
    extract::{Path, State},
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
use crate::db::{Db, TenantCreate, TenantUpdate};
use crate::error::ApiError;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/tenants", get(list::<C>).post(create::<C>))
        .route(
            "/tenants/:id",
            get(get_one::<C>).put(update::<C>).delete(delete_one::<C>),
        )
        .layer(ServiceBuilder::new().layer(TraceLayer::new_for_http()))
}

// --- list ---

#[derive(Debug, Deserialize, Default)]
pub struct ListQuery {
    pub keyword: Option<String>,
}

pub async fn list<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let items = st.db.list_tenants().await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    Ok(Json(json!({ "items": items, "total": items.len() })))
}

// --- get ---

pub async fn get_one<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let t = st.db.get_tenant(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("tenant not found".into()))?;
    Ok(Json(t))
}

// --- create ---

pub async fn create<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Json(c): Json<TenantCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    if c.tenant_name.trim().is_empty() {
        return Err(ApiError::BadRequest("tenant_name required".into()));
    }
    let t = st.db.create_tenant(c).await
        .map_err(|e| match e {
            sqlx::Error::Database(ref de) => {
                let m = de.message().to_ascii_lowercase();
                if m.contains("unique") || m.contains("duplicate") {
                    ApiError::Conflict("tenant_name already exists".into())
                } else {
                    ApiError::Db(e.to_string())
                }
            }
            other => ApiError::Db(other.to_string()),
        })?;
    Ok((StatusCode::CREATED, Json(t)))
}

// --- update ---

pub async fn update<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
    Json(u): Json<TenantUpdate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let t = st.db.update_tenant(id, u).await
        .map_err(|e| match e {
            sqlx::Error::RowNotFound => ApiError::NotFound("tenant not found".into()),
            other => ApiError::Db(other.to_string()),
        })?;
    Ok(Json(t))
}

// --- delete ---

pub async fn delete_one<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    if id == 1 {
        return Err(ApiError::BadRequest("default tenant (id=1) cannot be deleted".into()));
    }
    let ok = st.db.delete_tenant(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    if ok {
        Ok(Json(json!({ "ok": true })))
    } else {
        Err(ApiError::NotFound("tenant not found".into()))
    }
}
