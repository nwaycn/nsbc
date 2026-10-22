//! Recording-related routes:
//!   GET /api/v1/recordings        — paginated list (protected by RBAC, L0 read-only)
//!   GET /recordings/*path         — public file download (HTML5 <audio> cannot carry JWT)
//!
//! File download route security:
//!   1) Root directory decided by config `recording.path` (default `/var/lib/nsbc/recordings`)
//!   2) `..` components are stripped from the URL `*path` to prevent directory traversal
//!   3) After canonicalization, the path must still be within the root directory or 403
//!   4) Non .wav / .g711a / .pcm extensions directly return 403 (only allow recording files)
//!   5) CSP must allow audio loading (img-src already covers data:; audio-src inherits
//!      default-src by default)
//!
//! Recording file naming: nsbcd recording_stop writes to cfg recording.path;
//! path format decided by `rec_path_template` (default {caller}-{callee}-{callid}.wav
//! or yyyymmdd/callid_mix_seq.wav); CDR metadata.rec_path stores exactly this relative path.
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
// RBAC-protected recording list
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

/// GET /api/v1/recordings — paginated recording list (L0 read-only).
///
/// Query params (all optional):
///   `caller` / `callee`  — fuzzy match (LIKE %X%)
///   `from` / `to`        — start_time ISO string (>= and <=)
///   `limit`              — 1-500, default 50
///   `offset`             — default 0
///
/// Response: `{ items: RecordingRow[], total: N }`; DB not configured → 503.
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
// Public file download (no RBAC)
// ============================================================

/// Public route (mounted outside app_router, no /api/v1 prefix, no RBAC layer).
/// Note: `.with_state(state)` yields `Router<()>`, same pattern as api_router.
pub fn public_file_router<C>(state: Arc<AppState<C>>) -> Router<()>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/recordings/*path", get(download_recording::<C>))
        .with_state(state)
}

/// Allowed recording file extensions only (whitelist, prevent arbitrary file download).
const ALLOWED_EXTS: &[&str] = &["wav", "g711a", "g711u", "pcm", "raw"];

async fn download_recording<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    headers: HeaderMap,
    axum::extract::Path(rel): axum::extract::Path<String>,
) -> Response {
    let root = st.recording_root.clone();

    // 0) Pre-check the root directory exists (fail early with a clear error, don't wait
    //    for a later canonicalize on a file that's not there)
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

    // 1) Whitelist extension check
    let p = Path::new(&rel);
    let ext_ok = p.extension()
        .and_then(|e| e.to_str())
        .map(|e| ALLOWED_EXTS.contains(&e.to_ascii_lowercase().as_str()))
        .unwrap_or(false);
    if !ext_ok {
        return (StatusCode::FORBIDDEN, "forbidden extension").into_response();
    }

    // 2) Directory-traversal defense: strip all ".." and "."
    let safe_rel: String = rel
        .split('/')
        .filter(|seg| !seg.is_empty() && *seg != ".." && *seg != ".")
        .collect::<Vec<_>>()
        .join("/");
    if safe_rel.is_empty() {
        return (StatusCode::BAD_REQUEST, "empty path").into_response();
    }

    // Insurance layer: older nsbcd versions wrote CDR rec_path with a base_dir prefix
    // (e.g. "../recordings/20261011/xxx.wav" → browser parses it as
    // recordings/20261011/xxx.wav). If safe_rel's first segment happens to match the
    // root's directory name, strip it.
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

    // 3) After canonicalization the path must still be within root
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

    // 4) Read file + Content-Type
    let data = match tokio::fs::read(&canon).await {
        Ok(d) => d,
        Err(_) => return (StatusCode::NOT_FOUND, "not found").into_response(),
    };

    let ctype = match canon.extension().and_then(|e| e.to_str()) {
        Some("wav")   => "audio/wav",
        Some("g711a") | Some("g711u") | Some("pcm") | Some("raw") => "application/octet-stream",
        _             => "application/octet-stream",
    };

    // 5) Range request support (simple implementation: only handles bytes=start- format)
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
