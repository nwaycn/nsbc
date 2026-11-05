//! Registration live list (REQ-WEB-004 dashboard).
//!
//! Data source: registrations table (DB), written by C-side reg_push() on
//! every REGISTER 200 OK / un-REGISTER. Core UDS CMD path removed —
//! eliminates the 256KB / 512KB hard buffer cap that caused bad gateway
//! once bindings grew past ~2000.
//!
//! Query: GET /registrations?keyword=1000&limit=50&offset=0
//! Returns { bindings: BindingEntry[], registrations: total, truncated: false }

use std::sync::Arc;

use axum::{
    extract::{Query, State},
    routing::get,
    Json, Router,
};
use serde::Deserialize;

use crate::coreclient::{CoreApi, BindingEntry};
use crate::db::RegistrationRow;
use crate::error::ApiResult;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new().route("/registrations", get(get_registrations::<C>))
}

#[derive(Debug, Deserialize, Default)]
struct RegQuery {
    keyword: Option<String>,
    limit:   Option<i64>,
    offset:  Option<i64>,
}

/// Convert DB row → BindingEntry (frontend contract unchanged).
/// `aor` is derived as `user_name@<domain>` — domain pulled from public_contact.
/// `expires_sec` = remaining seconds until expires_at;
/// `alive_sec` = seconds since registered_at.
fn row_to_binding(row: &RegistrationRow) -> BindingEntry {
    let now_sec = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);

    let expires_sec = row.expires_at.as_ref().and_then(|iso| iso_to_sec(iso))
        .map(|exp| (exp - now_sec).max(0));

    let alive_sec = iso_to_sec(&row.registered_at)
        .map(|reg| (now_sec - reg).max(0));

    let aor_domain = extract_domain(Some(row.public_contact.as_str()))
        .unwrap_or_else(|| "nsbc.local".to_string());

    BindingEntry {
        aor: format!("{}@{}", row.user_name, aor_domain),
        user: row.user_name.clone(),
        contact: row.contact.clone().unwrap_or_default(),
        expires_sec,
        alive_sec,
        q: None,
        nat_detected: if row.nat_detected > 0 { Some(row.nat_detected) } else { None },
        transport: row.transport.clone(),
        received_ip: row.received_ip.clone(),
        received_port: Some(row.received_port).filter(|&p| p > 0),
        public_contact: row.public_contact.clone(),
    }
}

/// Parse ISO 8601 timestamp → unix seconds. Supports both
/// "2025-11-04T12:34:56Z" and "2025-11-04 12:34:56".
fn iso_to_sec(iso: &str) -> Option<i64> {
    let bytes = iso.as_bytes();
    if bytes.len() < 19 { return None; }
    let y: i64 = iso[0..4].parse().ok()?;
    let m: i64 = iso[5..7].parse().ok()?;
    let d: i64 = iso[8..10].parse().ok()?;
    let hh: i64 = iso[11..13].parse().ok()?;
    let mm: i64 = iso[14..16].parse().ok()?;
    let ss: i64 = iso[17..19].parse().ok()?;
    Some(ymdhms_to_unix(y, m, d, hh, mm, ss))
}

/// Extract SIP domain from contact URI like "sip:21505@10.0.0.25:5060"
fn extract_domain(contact: Option<&str>) -> Option<String> {
    let c = contact?;
    let after_at = c.split('@').nth(1)?;
    let host_part = after_at.split(':').next()?;
    if host_part.is_empty() { None } else { Some(host_part.to_string()) }
}

/// YMDHMS → unix epoch seconds (valid 1970..2100).
fn ymdhms_to_unix(y: i64, m: i64, d: i64, h: i64, mi: i64, s: i64) -> i64 {
    let days_in_month = [31,28,31,30,31,30,31,31,30,31,30,31];
    let mut epoch = 0i64;
    for yr in 1970..y {
        let leap = (yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0;
        epoch += if leap { 366 } else { 365 };
    }
    let leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    for mo in 0..m-1 {
        epoch += days_in_month[mo as usize];
        if mo == 1 && leap { epoch += 1; }
    }
    epoch += d - 1;
    epoch *= 86400;
    epoch += h * 3600 + mi * 60 + s;
    epoch
}

async fn get_registrations<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Query(q): Query<RegQuery>,
) -> ApiResult<Json<serde_json::Value>> {
    st.db.pool().ok_or_else(|| crate::error::ApiError::DbUnavailable)?;

    let limit  = q.limit.unwrap_or(200).clamp(1, 500);
    let offset = q.offset.unwrap_or(0).max(0);

    let (rows, total) = st.db
        .list_registrations(q.keyword.as_deref(), limit, offset)
        .await
        .map_err(|e| crate::error::ApiError::Db(e.to_string()))?;

    let items: Vec<BindingEntry> = rows.iter()
        .map(|r| row_to_binding(r))
        .collect();

    Ok(Json(serde_json::json!({
        "bindings":      items,
        "registrations": total,
        "truncated":     false,   // DB handles pagination natively
    })))
}
