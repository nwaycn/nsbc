//! API contract and version discovery (REQ-WEB-020).
//!
//! All /api calls must explicitly carry `X-NSBC-API-Version: <major>`;
//! missing or mismatched major version -> fail-closed (400), no "guess latest".
//! Version exposure is unified with core REST (/api/v1/) and real-time control interfaces.

use axum::extract::Request;
use axum::http::header::HeaderName;
use axum::middleware::Next;
use axum::response::Response;

use crate::error::ApiError;
use crate::API_MAJOR;

pub static HEADER_API_VERSION: HeaderName =
    HeaderName::from_static("x-nsbc-api-version");

pub async fn enforce(req: Request, next: Next) -> Result<Response, ApiError> {
    let got = req
        .headers()
        .get(&HEADER_API_VERSION)
        .and_then(|v| v.to_str().ok())
        .map(|s| s.to_string());

    let major = match got.as_deref() {
        Some(v) => parse_major(v)
            .ok_or_else(|| ApiError::VersionMismatch {
                got: Some(v.to_string()),
                supported: API_MAJOR,
            })?,
        None => {
            return Err(ApiError::VersionMismatch {
                got: None,
                supported: API_MAJOR,
            })
        }
    };
    if major != API_MAJOR {
        return Err(ApiError::VersionMismatch {
            got,
            supported: API_MAJOR,
        });
    }
    Ok(next.run(req).await)
}

/// "1" / "1.2" / "v1" all yield the major version; invalid ("latest", empty, non-numeric) -> None.
fn parse_major(v: &str) -> Option<u32> {
    let v = v.strip_prefix('v').unwrap_or(v);
    v.split(['.', '-']).next()?.parse::<u32>().ok()
}
