//! 静态资源运行时磁盘读取（替代编译期 rust-embed 嵌入）。
//!
//! 【临时方案】用户要求暂时去掉编译期 embed，改成运行时从磁盘 `static/`
//! 目录读取。开发时改 HTML/JS 不用重新编译 Rust，直接刷新浏览器即生效。
//! 以后需要恢复 embed 时，把本文件还原回 rust-embed 版本即可（保留 git 历史）。
//!
//! 部署时需在 nsbc-web 二进制所在目录下放一个 `static/` 子目录。

use axum::body::Body;
use axum::http::{header, StatusCode, Uri};
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use axum::Router;
use std::path::{Path, PathBuf};

/// 静态文件根目录（相对 nsbc-web 进程工作目录）。
const STATIC_DIR: &str = "static";

/// 静态文件路由（fallback handler）。
/// 必须放在嵌套路由之后，否则会吞掉 /api/v1。
///
/// 注意：不做 SPA fallback（Vue Router history 模式需要额外处理）。
/// 目前只服务 webrtc.html 等独立页面，不影响 REST API。
pub fn router() -> Router {
    Router::new().fallback(get(static_handler))
}

async fn static_handler(uri: Uri) -> Response {
    let path = uri.path().trim_start_matches('/');
    if path.is_empty() {
        return serve("index.html").await;
    }
    // 防目录穿越：去掉所有 ../
    let clean = path.replace("..", "");
    serve(&clean).await
}

async fn serve(rel: &str) -> Response {
    let dir = Path::new(STATIC_DIR);
    let full: PathBuf = dir.join(rel);

    // 只允许在 static/ 目录内
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
            (
                StatusCode::OK,
                [(header::CONTENT_TYPE, ctype)],
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
