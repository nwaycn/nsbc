//! Middleware layer: API version governance (REQ-WEB-020), per-IP rate limiting (REQ-WEB-009)
//! and RBAC (REQ-WEB-008). Plus external API Key auth (REQ-EXT-API-001).

pub mod apiversion;
pub mod ext_key;
pub mod ratelimit;
pub mod rbac;
