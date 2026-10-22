//! Dialplan Application 注册表 REST（REQ-DP-APP-001 / REQ-AI-001）。
//!
//! 单端点：`GET /dialplan-apps`
//!   → JSON 数组 [{ "name": "answer", "help": "..."} , {"name": "ai", ...}]
//! 数据来源：核心侧 UDS `show apps` 命令（应用注册表由 dialplan/ai_dialog/...
//! 等模块在 init 时调 `app_register` 填入，`app_help` 统一输出）。
//!
//! 典型用途：前端 Dialplan Rule 编辑器的 app_chain 下拉选择。

use std::sync::Arc;

use axum::{
    extract::State,
    http::StatusCode,
    response::IntoResponse,
    routing::get,
    Json, Router,
};
use serde::Serialize;
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
        .route("/dialplan-apps", get(list::<C>))
        .layer(ServiceBuilder::new().layer(TraceLayer::new_for_http()))
}

#[derive(Debug, Serialize)]
pub struct AppEntry {
    pub name: String,
    pub help: String,
}

pub async fn list<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    let apps = st.core.dialplan_apps().await
        .map_err(|_| ApiError::CoreUnavailable)?;
    let entries: Vec<AppEntry> = apps
        .into_iter()
        .map(|a| AppEntry { name: a.name, help: a.help })
        .collect();
    Ok((StatusCode::OK, Json(json!({ "items": entries }))))
}
