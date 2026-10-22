//! 注册摘要（REQ-WEB-004 仪表盘）：当前在线注册数（L0 只读）。
//! 数据来自核心控制口 `show registrations`（组合根只读计数槽，
//! 不横向包含 registrar 内部头，A3 分层约束）。
use std::sync::Arc;

use axum::extract::State;
use axum::routing::get;
use axum::{Json, Router};

use crate::coreclient::{CoreApi, RegistrationSummary};
use crate::error::ApiResult;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new().route("/registrations", get(get_registrations::<C>))
}

async fn get_registrations<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<RegistrationSummary>> {
    Ok(Json(st.core.registrations().await?))
}
