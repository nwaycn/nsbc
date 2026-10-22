//! Media bridge active sessions (REQ-OAM-010): pass-through of the core's
//! `show transcode` as read-only JSON. Shows all currently active bridge instances
//! in bypass/relay/transcode mode, plus their ports and counters.
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

/// GET /api/v1/transcode: active bridge session listing (L0 read-only).
async fn list_transcode<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<BridgeList>> {
    Ok(Json(st.core.show_transcode().await?))
}
