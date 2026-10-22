//! 录音相关路由：
//!   GET /api/v1/recordings        — 分页列表（受 RBAC 保护，L0 只读）
//!   GET /recordings/*path         — 公开文件下载（HTML5 <audio> 无法带 JWT）
//!
//! 文件下载路由安全：
//!   1) 根目录由配置 `recording.path` 决定（默认 `/var/lib/nsbc/recordings`）
//!   2) URL 中的 `*path` 会去掉所有 `..` 组件，防止目录穿越
//!   3) canonicalize 后必须仍在根目录内，否则 403
//!   4) 非 .wav / .g711a / .pcm 后缀直接 403（只允许录音文件）
//!   5) CSP 需允许 audio 加载（img-src 已覆盖 data:；audio-src 默认继承 default-src）
//!
//! 录音文件命名：nsbcd recording_stop 时输出到 cfg recording.path 下，
//! 路径格式由 `rec_path_template` 决定（默认 {caller}-{callee}-{callid}.wav
//! 或 yyyymmdd/callid_mix_seq.wav），CDR metadata.rec_path 存的就是这个相对路径。
use tracing;

use std::path::{Path, PathBuf};
use std::sync::Arc;

use axum::body::Body;
use axum::extract::{Query, State};
use axum::http::{header, HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use axum::{Json, Router};
use serde::Deserialize;
use serde_json::{json, Value};

use crate::db::{self, RecordingQuery};
use crate::error::{ApiError, ApiResult};
use crate::{AppState, CoreApi};

// ============================================================
// 受 RBAC 保护的录音列表
// ============================================================

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/recordings", get(list_recordings::<C>))
}

#[derive(Deserialize)]
pub struct RecordingQueryParams {
    pub caller: Option<String>,
    pub callee: Option<String>,
    pub from: Option<String>,
    pub to: Option<String>,
    pub limit: Option<u32>,
    pub offset: Option<u32>,
}

/// GET /api/v1/recordings — 分页录音列表（L0 只读）。
///
/// 查询参数（全可选）：
///   `caller` / `callee`  — 模糊匹配（LIKE %X%）
///   `from` / `to`        — start_time ISO 字符串（>= 和 <=）
///   `limit`              — 1-500，默认 50
///   `offset`             — 默认 0
///
/// 响应：`{ items: RecordingRow[], total: N }`；DB 未配置 → 503。
async fn list_recordings<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Query(p): Query<RecordingQueryParams>,
) -> ApiResult<Json<Value>> {
    let pool = st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let backend = st.db.backend();

    let q = RecordingQuery {
        caller: p.caller,
        callee: p.callee,
        from:   p.from,
        to:     p.to,
        limit:  p.limit,
        offset: p.offset,
    };

    let (rows, total) = db::query_recordings(pool, &q, backend)
        .await
        .map_err(|e| ApiError::Db(e.to_string()))?;

    Ok(Json(json!({
        "items": rows,
        "total": total,
    })))
}

// ============================================================
// 公开文件下载（不走 RBAC）
// ============================================================

/// 公开路由（挂在 app_router 外层，不带 /api/v1 前缀，不带 RBAC 层）。
/// 注意：走 `.with_state(state)` 得到 `Router<()>`，与 api_router 同模式。
pub fn public_file_router<C>(state: Arc<AppState<C>>) -> Router<()>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/recordings/*path", get(download_recording::<C>))
        .with_state(state)
}

/// 只允许的录音文件后缀（白名单，防任意文件下载）。
const ALLOWED_EXTS: &[&str] = &["wav", "g711a", "g711u", "pcm", "raw"];

async fn download_recording<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    headers: HeaderMap,
    axum::extract::Path(rel): axum::extract::Path<String>,
) -> Response {
    let root = st.recording_root.clone();

    // 0) 先检查 root 目录是否存在（提前报清楚错误，不要到后面 canonicalize 文件才 404）
    if !root.exists() || !root.is_dir() {
        return (
            StatusCode::NOT_FOUND,
            format!("recording root not found: {}", root.display()),
        )
            .into_response();
    }
    let root_canon = match root.canonicalize() {
        Ok(p) => p,
        Err(_) => return (StatusCode::INTERNAL_SERVER_ERROR, "recording root resolve failed").into_response(),
    };

    // 1) 白名单后缀检查
    let p = Path::new(&rel);
    let ext_ok = p.extension()
        .and_then(|e| e.to_str())
        .map(|e| ALLOWED_EXTS.contains(&e.to_ascii_lowercase().as_str()))
        .unwrap_or(false);
    if !ext_ok {
        return (StatusCode::FORBIDDEN, "forbidden extension").into_response();
    }

    // 2) 防目录穿越：去掉所有 ".." 和 "."
    let safe_rel: String = rel
        .split('/')
        .filter(|seg| !seg.is_empty() && *seg != ".." && *seg != ".")
        .collect::<Vec<_>>()
        .join("/");
    if safe_rel.is_empty() {
        return (StatusCode::BAD_REQUEST, "empty path").into_response();
    }

    // 保险层：老版本 nsbcd 写 CDR rec_path 时会带 base_dir 前缀
    // （如 "../recordings/20261011/xxx.wav" → 浏览器解析成 recordings/20261011/xxx.wav）
    // 如果 safe_rel 第一段碰巧等于 root 的目录名，剥掉它
    let safe_rel = {
        let bn = root_canon.file_name().and_then(|n| n.to_str().map(|s| s.to_string()));
        if let Some(bn) = bn {
            let mut parts = safe_rel.splitn(2, '/');
            if parts.next() == Some(bn.as_str()) {
                if let Some(rest) = parts.next() {
                    if !rest.is_empty() {
                        rest.to_string()
                    } else {
                        safe_rel
                    }
                } else {
                    safe_rel
                }
            } else {
                safe_rel
            }
        } else {
            safe_rel
        }
    };

    let full: PathBuf = root.join(&safe_rel);

    // 3) canonicalize 后必须仍在 root 内
    tracing::info!(
        "recordings req: safe_rel={}, full={}, root={}",
        safe_rel, full.display(), root.display()
    );
    let canon = match full.canonicalize() {
        Ok(p) => p,
        Err(_) => return (StatusCode::NOT_FOUND, format!("file not found: {}", full.display())).into_response(),
    };
    if !canon.starts_with(&root_canon) {
        return (StatusCode::FORBIDDEN, "path traversal blocked").into_response();
    }

    // 4) 读文件 + Content-Type
    let data = match tokio::fs::read(&canon).await {
        Ok(d) => d,
        Err(_) => return (StatusCode::NOT_FOUND, "not found").into_response(),
    };

    let ctype = match canon.extension().and_then(|e| e.to_str()) {
        Some("wav")   => "audio/wav",
        Some("g711a") | Some("g711u") | Some("pcm") | Some("raw") => "application/octet-stream",
        _             => "application/octet-stream",
    };

    // 5) Range 请求支持（简单实现：只处理 bytes=start- 格式）
    if let Some(range) = headers.get(header::RANGE) {
        if let Ok(s) = range.to_str() {
            if let Some(start_str) = s.strip_prefix("bytes=").and_then(|v| v.split('-').next()) {
                if let Ok(start) = start_str.parse::<usize>() {
                    if start < data.len() {
                        let sliced = &data[start..];
                        let end = data.len() - 1;
                        let resp = (
                            StatusCode::PARTIAL_CONTENT,
                            [
                                (header::CONTENT_TYPE, ctype.to_string()),
                                (header::CONTENT_LENGTH, sliced.len().to_string()),
                                (header::ACCEPT_RANGES, "bytes".to_string()),
                                (header::CONTENT_RANGE,
                                 format!("bytes {start}-{end}/{}", data.len())),
                            ],
                            Body::from(sliced.to_vec()),
                        ).into_response();
                        return resp;
                    }
                }
            }
        }
    }

    (
        StatusCode::OK,
        [
            (header::CONTENT_TYPE, ctype.to_string()),
            (header::CONTENT_LENGTH, data.len().to_string()),
            (header::ACCEPT_RANGES, "bytes".to_string()),
            (header::CACHE_CONTROL, "no-cache".to_string()),
        ],
        Body::from(data),
    ).into_response()
}
