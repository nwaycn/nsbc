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
    /// Call-ID LIKE 搜索
    pub call_id: Option<String>,
    /// 源 IP LIKE 搜索（从 metadata JSON 解析，Rust 侧 filter）
    pub from_ip: Option<String>,
    pub from: Option<String>,
    pub to: Option<String>,
    /// 通话时长范围（秒）
    pub duration_min: Option<i64>,
    pub duration_max: Option<i64>,
    #[serde(default = "default_limit")]
    pub limit: Option<u32>,
    #[serde(default)]
    pub offset: Option<u32>,
    /* REQ-SEC-005: disposition + is_malicious filters（前端 Tab 分流） */
    pub disposition: Option<String>,
    /// `malicious=1` → 只看恶意；`malicious=0` → 只看正常；不提供 → 全部
    pub malicious: Option<u8>,
    /// 排序：sort_by=duration|answer|caller|callee|start_time（默认 start_time），sort_dir=asc|desc（默认 desc）
    pub sort_by: Option<String>,
    pub sort_dir: Option<String>,
}
fn default_limit() -> Option<u32> { Some(20) }

/// GET /api/v1/cdrs — paginated CDR history (L0 read-only).
///
/// Response format: `{ items: [...], total: N }`; returns 503 when DB is not configured.
///
/// All filter params:
///   caller / callee / call_id / from_ip  → SQL LIKE / Rust metadata LIKE
///   from / to                            → start_time range
///   duration_min / duration_max          → duration_sec range
///   disposition=answered|failed|busy     → cause_text = ?
///   malicious=1|0                        → is_malicious filter（metadata JSON）
///   sort_by=duration|answer|caller|callee|start_time  sort_dir=asc|desc
async fn list_cdrs<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Query(p): Query<CdrQueryParams>,
) -> ApiResult<Json<Value>> {
    let pool = st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let backend = st.db.backend();
    let q = CdrQuery {
        caller: p.caller,
        callee: p.callee,
        call_id: p.call_id,
        from_ip: p.from_ip,
        from: p.from,
        to: p.to,
        duration_min: p.duration_min,
        duration_max: p.duration_max,
        limit: p.limit.unwrap_or(20),
        offset: p.offset.unwrap_or(0),
        disposition: p.disposition,
        is_malicious: p.malicious,
        sort_by: p.sort_by,
        sort_dir: p.sort_dir,
    };
    let (rows, total) = db::query_cdrs(pool, &q, backend)
        .await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    Ok(Json(json!({
        "items": rows,
        "total": total,
    })))
}
