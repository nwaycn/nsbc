//! Extension queue REST. Six endpoints.

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
        .route("/queues/agents", get(agent_list::<C>))
        .route("/queues/agents/state", get(agent_state::<C>))
        .route("/queues/agents/pause", post(agent_pause::<C>))
        .route("/queues/agents/resume", post(agent_resume::<C>))
        .route("/queues/:id",
            get(get_one::<C>).put(update::<C>).delete(delete_one::<C>))
        .layer(ServiceBuilder::new().layer(TraceLayer::new_for_http()))
}

#[derive(Debug, Deserialize, Default)]
pub struct ListQuery {
    pub tenant_id: Option<i64>,
    pub enabled: Option<String>,
    pub keyword: Option<String>,
    pub limit:  Option<i64>,
    pub offset: Option<i64>,
}

pub async fn list<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>, Query(q): Query<ListQuery>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let enabled = match q.enabled.as_deref() { Some("true") => Some(true), Some("false") => Some(false), _ => None };
    let (items, total) = st.db
        .list_queues(q.tenant_id, enabled, q.keyword.as_deref(), q.limit.unwrap_or(50).clamp(1, 500), q.offset.unwrap_or(0).max(0))
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
    // list_enabled_queues returns a (Queue, tenant_name) tuple — build QueueSyncDto manually
    let rows = st.db.list_enabled_queues().await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    let dtos: Vec<serde_json::Value> = rows.into_iter().map(|(q, tenant_name)| {
        let members = crate::sync::normalize_queue_members(&q.members_json);
        serde_json::json!({
            "tenant": tenant_name,
            "name": q.name,
            "enabled": q.enabled != 0,
            "strategy": q.ring_strategy,
            "max_wait_s": q.max_wait_sec,
            "wrap_time_s": q.wrap_time_s,
            "require_skill": q.require_skill,
            "overflow": q.overflow,
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

// ===================================================================
// 座席忙线 / 置忙控制（C 侧 cc.agent_* 命令）
//   pause/resume 立刻影响队列选路；busy 由通话自动维护。
//   tenant 传空则回退 tenant_id 查名字；都为空时传 "*"（仅查询有效）。
// ===================================================================

#[derive(Debug, Deserialize)]
pub struct AgentPauseBody {
    pub tenant_id: Option<i64>,
    pub tenant:    Option<String>,
    pub ext:       String,
    /// 1=小休/午餐 2=培训 3=其他
    pub reason:    Option<i32>,
}

#[derive(Debug, Deserialize)]
pub struct AgentQuery {
    pub tenant_id: Option<i64>,
    pub tenant:    Option<String>,
    pub ext:       Option<String>,
}

async fn tenant_name_of<C>(
    st: &AppState<C>, tenant_id: Option<i64>, tenant: Option<String>,
) -> Result<String, ApiError>
where C: CoreApi + Clone + Send + Sync,
{
    if let Some(t) = tenant {
        let t = t.trim();
        if !t.is_empty() { return Ok(t.to_string()); }
    }
    if let Some(id) = tenant_id {
        let row = st.db.get_tenant(id).await.map_err(|e| ApiError::Db(e.to_string()))?
            .ok_or_else(|| ApiError::NotFound("tenant not found".into()))?;
        return Ok(row.tenant_name);
    }
    Ok("*".to_string())
}

fn core_ok(out: &str) -> bool {
    out.trim_start().starts_with("OK")
}

pub async fn agent_pause<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>, Json(body): Json<AgentPauseBody>,
) -> Result<impl IntoResponse, ApiError> {
    let ext = body.ext.trim();
    if ext.is_empty() { return Err(ApiError::BadRequest("ext required".into())); }
    let tenant = tenant_name_of(&st, body.tenant_id, body.tenant).await?;
    let reason = body.reason.unwrap_or(1);
    let out = st.core.exec_cmd(&format!("cc.agent_pause {} {} {}", tenant, ext, reason)).await?;
    if !core_ok(&out) { return Err(ApiError::BadRequest(out.trim().to_string())); }
    tracing::info!(tenant = %tenant, ext, reason, "queue agent paused");
    Ok(Json(json!({ "ok": true, "tenant": tenant, "ext": ext, "paused": true, "reason": reason })))
}

pub async fn agent_resume<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>, Json(body): Json<AgentPauseBody>,
) -> Result<impl IntoResponse, ApiError> {
    let ext = body.ext.trim();
    if ext.is_empty() { return Err(ApiError::BadRequest("ext required".into())); }
    let tenant = tenant_name_of(&st, body.tenant_id, body.tenant).await?;
    let out = st.core.exec_cmd(&format!("cc.agent_resume {} {}", tenant, ext)).await?;
    if !core_ok(&out) { return Err(ApiError::BadRequest(out.trim().to_string())); }
    tracing::info!(tenant = %tenant, ext, "queue agent resumed");
    Ok(Json(json!({ "ok": true, "tenant": tenant, "ext": ext, "paused": false })))
}

pub async fn agent_state<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>, Query(q): Query<AgentQuery>,
) -> Result<impl IntoResponse, ApiError> {
    let ext = q.ext.as_deref().map(str::trim).unwrap_or("");
    if ext.is_empty() { return Err(ApiError::BadRequest("ext required".into())); }
    let tenant = tenant_name_of(&st, q.tenant_id, q.tenant).await?;
    let out = st.core.exec_cmd(&format!("cc.agent_state {} {}", tenant, ext)).await?;
    if !core_ok(&out) { return Err(ApiError::NotFound("extension not tracked".into())); }
    Ok(Json(json!({ "ok": true, "raw": out.trim() })))
}

pub async fn agent_list<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>, Query(q): Query<AgentQuery>,
) -> Result<impl IntoResponse, ApiError> {
    let tenant = tenant_name_of(&st, q.tenant_id, q.tenant).await?;
    let out = st.core.exec_cmd(&format!("cc.agent_list {}", tenant)).await?;
    let body = out.trim().strip_prefix("OK").map(str::trim).unwrap_or("");
    match serde_json::from_str::<serde_json::Value>(body) {
        Ok(v) => Ok(Json(json!({ "ok": true, "items": v }))),
        Err(_) => Ok(Json(json!({ "ok": true, "raw": out.trim() }))),
    }
}
