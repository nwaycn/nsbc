//! Codec 注册表 REST（W-06 / Phase 3 mod_av 对齐）。
//!
//! 单端点：`GET /codecs`
//!   → JSON 数组 [{ "name": "PCMA", "pt": 8, "sample_rate": 8000,
//!                  "impl_module": "mod_codecs", "fmtp_default": "-",
//!                  "media_type": "audio"}, ...]
//!
//! 数据来源：核心侧 UDS `show codecs` 命令（codec_registry 由
//! mod_codecs（G711/G729）、mod_av（G722/Opus/H264/VP8/VP9）、
//! 或未来 iLBC/AMR 模块在 init 时自注册条目）。
//!
//! 典型用途：
//!   - 前端分机编辑页 codec 下拉（按 media_type 分组 audio/video）
//!   - 前端 transcode 页面 codec 选择
//!   - 运维面板显示当前可用编解码清单

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

/// 路由层 Codec 条目（从 coreclient::CodecInfo 直接透传，无额外加工）。
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
