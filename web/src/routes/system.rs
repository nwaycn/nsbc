//! System interface: version discovery, health check, aggregated status
//! (REQ-WEB-004/015, + REQ-CCMD-001 status).
use std::sync::Arc;

use axum::extract::State;
use axum::routing::get;
use axum::{Json, Router};
use serde_json::json;

use crate::coreclient::{CoreApi, StatusInfo};
use crate::error::ApiResult;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/version", get(version::<C>))
        .route("/status", get(status::<C>))
}

/// GET /api/v1/version: sidecar version + core real-time version (control socket
/// `version`) and supported API major. Core unreachable maps to 502/504 per REQ-WEB-014.
async fn version<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<serde_json::Value>> {
    let core = st.core.version().await?;
    Ok(Json(json!({
        "web": env!("CARGO_PKG_VERSION"),
        "core": core.core,
        "api_major": st.api_major,
    })))
}

/// GET /api/v1/status — REQ-CCMD-001 L1 aggregated status snapshot:
/// active/total/failed calls, registration count, SIP Profile count, uptime ms.
async fn status<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<StatusInfo>> {
    Ok(Json(st.core.status().await?))
}

/// GET /health: no auth, for host/container liveness probing (no business info exposed).
pub async fn health() -> &'static str {
    "OK\n"
}
