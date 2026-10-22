//! 系统接口：版本发现、健康检查、综合状态（REQ-WEB-004/015，+ REQ-CCMD-001 status）。
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

/// GET /api/v1/version：边车版本 + 核心实时版本（控制口 `version`）、
/// 支持的 API 主版本。核心不可达按 REQ-WEB-014 映射 502/504。
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

/// GET /api/v1/status —— REQ-CCMD-001 L1 综合状态快照：
/// 活动/累计/失败呼叫、注册数、SIP Profile 数、启动后运行时长 ms。
async fn status<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<StatusInfo>> {
    Ok(Json(st.core.status().await?))
}

/// GET /health：不带鉴权，供本机/容器探活（不暴露业务信息）。
pub async fn health() -> &'static str {
    "OK\n"
}
