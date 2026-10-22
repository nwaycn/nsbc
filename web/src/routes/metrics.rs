//! 仪表盘指标（REQ-WEB-004/015）：透传核心 `stats` 的 Prometheus 指标
//! 为只读 JSON（L0）。权威计数仍以 /calls、/registrations 专用命令为准。
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
