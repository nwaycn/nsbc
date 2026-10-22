//! CDR history query (REQ-WEB-004 extension / REQ-WEB-013 persistence layer).
//!
//! Depends on the database pool configured via the NSBC_DB_DSN env var. When not
//! configured or initialization fails, all endpoints return 503 Service Unavailable,
//! and the front-end shows a "persistence not configured" prompt.
//!
//! GET /cdrs — paginated query with caller/callee/time window filters (L0 read-only)

use std::sync::Arc;

use axum::extract::{Query, State};
use axum::routing::get;
use axum::{Json, Router};
use serde::Deserialize;
use serde_json::{json, Value};

use crate::db::{self, CdrQuery};
use crate::error::{ApiError, ApiResult};
use crate::{AppState, CoreApi};

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/cdrs", get(list_cdrs::<C>))
}

#[derive(Deserialize)]
pub struct CdrQueryParams {
    pub caller: Option<String>,
    pub callee: Option<String>,
    pub from: Option<String>,
    pub to: Option<String>,
    #[serde(default = "default_limit")]
    pub limit: Option<u32>,
    #[serde(default)]
    pub offset: Option<u32>,
}
fn default_limit() -> Option<u32> { Some(20) }

/// GET /api/v1/cdrs — paginated CDR history (L0 read-only).
///
/// Response format: `{ items: [...], total: N }`; returns 503 when DB is not configured.
async fn list_cdrs<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Query(p): Query<CdrQueryParams>,
) -> ApiResult<Json<Value>> {
    let pool = st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let backend = st.db.backend();
    let q = CdrQuery {
        caller: p.caller,
        callee: p.callee,
        from: p.from,
        to: p.to,
        limit: p.limit.unwrap_or(20),
        offset: p.offset.unwrap_or(0),
    };
    let (rows, total) = db::query_cdrs(pool, &q, backend)
        .await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    Ok(Json(json!({
        "items": rows,
        "total": total,
    })))
}
