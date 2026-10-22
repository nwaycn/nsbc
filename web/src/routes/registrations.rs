//! Registration summary (REQ-WEB-004 dashboard): current online registration count
//! (L0 read-only). Data comes from the core control socket `show registrations`
//! (composite root read-only counter slot, does not horizontally include registrar's
//! internal header; A3 layering constraint).
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
