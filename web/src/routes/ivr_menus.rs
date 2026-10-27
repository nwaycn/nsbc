//! IVR menu REST. Six endpoints.

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
use crate::sync;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/ivr-menus", get(list::<C>).post(create::<C>))
        .route("/ivr-menus/reload", post(reload::<C>))
        .route("/ivr-menus/:id",
            get(get_one::<C>).put(update::<C>).delete(delete_one::<C>))
        .layer(ServiceBuilder::new().layer(TraceLayer::new_for_http()))
}

#[derive(Debug, Deserialize, Default)]
pub struct ListQuery {
    pub enabled:   Option<String>,
    pub tenant_id: Option<i64>,
    pub limit:     Option<i64>,
    pub offset:    Option<i64>,
}

pub async fn list<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>, Query(q): Query<ListQuery>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let enabled = match q.enabled.as_deref() { Some("true") => Some(true), Some("false") => Some(false), _ => None };
    let (items, total) = st.db
        .list_ivr_menus(enabled, q.tenant_id, q.limit.unwrap_or(50).clamp(1, 500), q.offset.unwrap_or(0).max(0))
        .await.map_err(|e| ApiError::Db(e.to_string()))?;
    Ok(Json(json!({ "items": items, "total": total })))
}

pub async fn get_one<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>, Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.get_ivr_menu(id).await.map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("ivr menu not found".into()))?;
    Ok(Json(row))
}

pub async fn create<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>, Json(body): Json<crate::db::IvrMenuCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.create_ivr_menu(body).await.map_err(|e| {
        if let sqlx::Error::Database(ref de) = e {
            let m = de.message().to_ascii_lowercase();
            if m.contains("unique") || m.contains("duplicate") { return ApiError::Conflict("ivr menu name already exists".into()); }
        }
        ApiError::Db(e.to_string())
    })?;
    let _ = st.db.insert_config_history("api","create",&format!("ivr-menus/{}",row.id),None,&format!("ivr={}",row.name)).await;
    sync::spawn_push(st.clone(), sync::Kind::IvrMenus);
    Ok((StatusCode::CREATED, Json(row)))
}

pub async fn update<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>, Path(id): Path<i64>,
    Json(body): Json<crate::db::IvrMenuUpdate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.update_ivr_menu(id, body).await.map_err(|e| match e {
        sqlx::Error::RowNotFound => ApiError::NotFound("ivr menu not found".into()),
        other => ApiError::Db(other.to_string()),
    })?;
    let _ = st.db.insert_config_history("api","update",&format!("ivr-menus/{}",row.id),None,&format!("ivr={}",row.name)).await;
    sync::spawn_push(st.clone(), sync::Kind::IvrMenus);
    Ok(Json(row))
}

pub async fn delete_one<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>, Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    if !st.db.delete_ivr_menu(id).await.map_err(|e| ApiError::Db(e.to_string()))? {
        return Err(ApiError::NotFound("ivr menu not found".into()));
    }
    let _ = st.db.insert_config_history("api","delete",&format!("ivr-menus/{id}"),None,"deleted").await;
    sync::spawn_push(st.clone(), sync::Kind::IvrMenus);
    Ok(StatusCode::NO_CONTENT)
}

pub async fn reload<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let rows = st.db.list_enabled_ivr_menus().await.map_err(|e| ApiError::Db(e.to_string()))?;
    let json = serde_json::to_string(&rows)
        .map_err(|e| ApiError::Db(e.to_string()))?;
    st.core.reload_ivr_menus(&json).await?;

    let _ = st.db.insert_config_history(
        "api", "reload", "ivr-menus", None,
        &format!("loaded {} enabled IVR menus (full replace)", rows.len()),
    ).await;

    Ok(Json(json!({ "reloaded": rows.len(), "uds_wired": true })))
}
