//! Static assets read from disk at runtime (replaces compile-time rust-embed embedding).
//!
//! [Temporary solution] User requested to temporarily remove compile-time embed, switch to runtime disk reading from `static/`
//! directory. During development, HTML/JS changes take effect by refreshing the browser without recompile.
//! When embed is needed again later, revert this file to the rust-embed version (git history preserved).
//!
//! At deploy time, place a `static/` subdirectory next to the nsbc-web binary.

use axum::body::Body;
use axum::http::{header, StatusCode, Uri};
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use axum::Router;
use std::path::{Path, PathBuf};

/// Static files root directory (relative to nsbc-web process working directory).
const STATIC_DIR: &str = "static";

/// Static file route (fallback handler).
/// Must be placed after nested routes, otherwise it would swallow /api/v1.
///
/// Note: no SPA fallback (Vue Router history mode needs extra handling).
/// Currently only serves standalone pages like webrtc.html, does not affect REST API.
pub fn router() -> Router {
    Router::new().fallback(get(static_handler))
}

async fn static_handler(uri: Uri) -> Response {
    let path = uri.path().trim_start_matches('/');
    if path.is_empty() {
        return serve("index.html").await;
    }
    // Prevent directory traversal: strip all ../
    let clean = path.replace("..", "");
    serve(&clean).await
}

async fn serve(rel: &str) -> Response {
    let dir = Path::new(STATIC_DIR);
    let full: PathBuf = dir.join(rel);

    // Only allow paths inside static/ directory
    let canonical = match full.canonicalize() {
        Ok(p) => p,
        Err(_) => return (StatusCode::NOT_FOUND, "not found").into_response(),
    };
    let dir_canonical = match dir.canonicalize() {
        Ok(p) => p,
        Err(_) => return (StatusCode::NOT_FOUND, "static dir missing").into_response(),
    };
    if !canonical.starts_with(&dir_canonical) {
        return (StatusCode::FORBIDDEN, "forbidden").into_response();
    }

    match tokio::fs::read(&canonical).await {
        Ok(data) => {
            let ctype = content_type(&canonical);
            // Cache-Control 策略：
            // - HTML (index.html 等): no-store，绝不缓存，每次都去服务器拿最新
            // - 带 ?v= 的 JS/CSS: public, max-age=31536000, immutable（版本号变了就新 URL）
            // - vendor/* 不带版本号: public, max-age=3600（最多缓存 1 小时）
            let is_html = canonical.extension().and_then(|e| e.to_str()) == Some("html");
            let is_vendor = rel.starts_with("vendor/");
            let cache_control = if is_html {
                "no-store"
            } else if is_vendor {
                "public, max-age=3600"
            } else {
                // 带 ?v= 的业务 JS/CSS
                "public, max-age=31536000, immutable"
            };
            (
                StatusCode::OK,
                [
                    (header::CONTENT_TYPE, ctype),
                    (header::CACHE_CONTROL, cache_control),
                ],
                Body::from(data),
            )
                .into_response()
        }
        Err(_) => (StatusCode::NOT_FOUND, "not found").into_response(),
    }
}

fn content_type(p: &Path) -> &'static str {
    match p.extension().and_then(|e| e.to_str()) {
        Some("html") => "text/html; charset=utf-8",
        Some("js")   => "application/javascript; charset=utf-8",
        Some("css")  => "text/css; charset=utf-8",
        Some("json") => "application/json; charset=utf-8",
        Some("svg")  => "image/svg+xml",
        Some("png")  => "image/png",
        Some("ico")  => "image/x-icon",
        Some("woff") => "font/woff",
        Some("woff2")=> "font/woff2",
        Some("ttf")  => "font/ttf",
        _            => "application/octet-stream",
    }
}
