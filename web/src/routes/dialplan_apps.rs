//! Dialplan Application registry REST (REQ-DP-APP-001 / REQ-AI-001).
//!
//! Single endpoint: `GET /dialplan-apps`
//!   → JSON array [{ "name": "answer", "help": "..."} , {"name": "ai", ...}]
//! Data source: core-side UDS `show apps` command (the app registry is filled by
//! modules such as dialplan/ai_dialog/... calling `app_register` at init time;
//! `app_help` is the uniform output).
//!
//! Typical use: front-end Dialplan Rule editor app_chain dropdown selection.

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
