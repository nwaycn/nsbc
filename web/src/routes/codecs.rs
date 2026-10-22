//! Codec registry REST (W-06 / Phase 3 mod_av aligned).
//!
//! Single endpoint: `GET /codecs`
//!   → JSON array [{ "name": "PCMA", "pt": 8, "sample_rate": 8000,
//!                  "impl_module": "mod_codecs", "fmtp_default": "-",
//!                  "media_type": "audio"}, ...]
//!
//! Data source: core-side UDS `show codecs` command (codec_registry is
//! self-registered at init time by mod_codecs (G711/G729), mod_av
//! (G722/Opus/H264/VP8/VP9), or future iLBC/AMR modules).
//!
//! Typical use:
//!   - Front-end extension editor codec dropdown (grouped by media_type audio/video)
//!   - Front-end transcode page codec selection
//!   - Ops dashboard showing the current codec list

use std::sync::Arc;

use axum::{
    extract::State,
    http::StatusCode,
    response::IntoResponse,
    routing::get,
    Json, Router,
};
use serde::Serialize;
use serde_json::json;
use tower::ServiceBuilder;
use tower_http::trace::TraceLayer;

use crate::coreclient::{CodecInfo, CoreApi};
use crate::error::ApiError;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/codecs", get(list::<C>))
        .layer(ServiceBuilder::new().layer(TraceLayer::new_for_http()))
}

/// Route-layer Codec entry (pass-through from coreclient::CodecInfo, no extra processing).
#[derive(Debug, Serialize)]
pub struct CodecEntry {
    pub name: String,
    pub pt: u8,
    pub sample_rate: u32,
    pub impl_module: String,
    pub fmtp_default: String,
    pub media_type: String,
}

pub async fn list<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    let codecs = st.core.show_codecs().await
        .map_err(|_| ApiError::CoreUnavailable)?;
    let entries: Vec<CodecEntry> = codecs
        .into_iter()
        .map(|c: CodecInfo| CodecEntry {
            name: c.name,
            pt: c.pt,
            sample_rate: c.sample_rate,
            impl_module: c.impl_module,
            fmtp_default: c.fmtp_default,
            media_type: c.media_type,
        })
        .collect();
    Ok((StatusCode::OK, Json(json!({ "items": entries }))))
}
