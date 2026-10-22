//! 媒体 bridge 活跃会话（REQ-OAM-010）：透传核心 `show transcode` 为只读 JSON。
//! 展示当前所有 bypass/relay/transcode 模式的 bridge 实例及其端口/计数。
use std::sync::Arc;

use axum::extract::State;
use axum::routing::get;
use axum::{Json, Router};

use crate::coreclient::{BridgeList, CoreApi};
use crate::error::ApiResult;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new().route("/transcode", get(list_transcode::<C>))
}

/// GET /api/v1/transcode：活跃 bridge 会话清单（L0 只读）。
async fn list_transcode<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<BridgeList>> {
    Ok(Json(st.core.show_transcode().await?))
}
