//! CDR 历史查询（REQ-WEB-004 扩展 / REQ-WEB-013 持久化层）。
//!
//! 依赖 NSBC_DB_DSN 环境变量配置的数据库连接池。未配置或初始化失败时
//! 所有端点返回 503 Service Unavailable，前端显示"未配置持久化"提示。
//!
//! GET /cdrs — 分页查询，支持 caller/callee/时间窗过滤（L0 只读）

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

/// GET /api/v1/cdrs — 分页 CDR 历史（L0 只读）。
///
/// 响应格式：`{ items: [...], total: N }`；DB 未配置时返回 503。
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
