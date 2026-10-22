//! 分机管理 REST（REQ-WEB-016）。
//!
//! 六个端点：列表/详情/创建/更新/删除 + 一个 reload 推送核心。
//! DB 未配置（Db::new() / pool=None）→ 所有端点返回 503 DbUnavailable，
//! 不阻断主业务启动（REQ-WEB-013 可选持久化设计）。
//!
//! reload 端点（POST /extensions/reload）：
//!   查 DB 所有 enabled 分机 → ExtSyncDto（去 password，只下发 HA1）→
//!   UDS 命令 `extensions.reload <json>` 批量推给核心。
//!   CRUD 保存后另有 sync::spawn_push 自动推送（300ms 防抖合并），
//!   reload 端点保留为手动全量兜底。

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
        // reload 在 /:id 之前注册——axum 按注册顺序匹配防止 /reload 被当成 :id
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
    pub enabled:    Option<String>,   // "true"/"false" 或空
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
        // UNIQUE 冲突 → 409 Conflict
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

    // 审计（复用 config_history）
    let _ = st.db.insert_config_history(
        "api", "create",
        &format!("extensions/{}", row.id),
        None, &format!("extension={} realm={}", row.extension, row.realm),
    ).await;

    // 保存成功 → 推送核心（300ms 防抖合并）
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

// --- reload（批量推送到核心 UDS）---

pub async fn reload<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let rows = st.db.list_enabled_extensions().await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    // ExtSyncDto：密码永不离开 Web 端（C 侧只需 HA1 做 Digest 校验）
    let dtos: Vec<crate::sync::ExtSyncDto> =
        rows.iter().map(crate::sync::ExtSyncDto::from).collect();
    let json = serde_json::to_string(&dtos).map_err(|e| ApiError::Db(e.to_string()))?;
    st.core.reload_extensions(&json).await
        .map_err(|_| ApiError::CoreUnavailable)?;
    tracing::info!(count = dtos.len(), "extensions.reload pushed to core");
    Ok(Json(json!({ "loaded": dtos.len() })))
}
