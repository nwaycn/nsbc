//! Dashboard metrics (REQ-WEB-004/015): pass-through of the core's `stats` Prometheus
//! metrics as read-only JSON (L0). For authoritative counts use the dedicated /calls and
//! /registrations commands.
use std::sync::Arc;

use axum::extract::State;
use axum::routing::get;
use axum::{Json, Router};

use crate::coreclient::{CoreApi, MetricSnapshot};
use crate::error::ApiResult;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new().route("/metrics", get(get_metrics::<C>))
}

async fn get_metrics<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<MetricSnapshot>> {
    Ok(Json(st.core.metrics().await?))
}
