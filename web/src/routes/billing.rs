//! Billing management REST (REQ-OAM-009): rate tables + billing accounts.
//!
//! Rate tables:
//!   GET    /rates              List (DB, pagination + prefix/enabled filter)
//!   GET    /rates/:id          Detail (DB)
//!   POST   /rates              Create (DB)
//!   PUT    /rates/:id          Update (DB)
//!   DELETE /rates/:id          Delete (DB)
//!   POST   /rates/reload       Push all enabled → core UDS (rate.reload)
//!   GET    /rates/core         Core-side real-time rate snapshot (show rates, L0 read-only)
//!
//! Accounts:
//!   GET    /balances           List (DB, pagination + keyword filter)
//!   GET    /balances/:id       Detail (DB)
//!   POST   /balances           Open account (DB + account.open UDS)
//!   POST   /balances/:id/topup Top up (DB + account.topup UDS)
//!   DELETE /balances/:id       Close account (DB only; core side keeps reserve state)
//!   GET    /balances/core      Core-side real-time balance snapshot (show balances, L0 read-only)
//!
//! Design principles:
//!   - DB CRUD separated from reload (same pattern as extensions.rs); reload is explicit
//!   - Account topup does sync DB write + UDS push simultaneously (avoid DB/core divergence)
//!   - prefix/account_key UNIQUE conflict → 409 Conflict

use std::sync::Arc;

use axum::{
    extract::{Path, Query, State},
    http::StatusCode,
    response::IntoResponse,
    routing::{get, post},
    Json, Router,
};
use serde::Deserialize;
use serde_json::json;
use tower::ServiceBuilder;
use tower_http::trace::TraceLayer;

use crate::coreclient::{AccountList, CoreApi, RateList};
use crate::db::{AccountCreate, RatePlanCreate, RatePlanEntryCreate, RatePlanUpdate, RateTableCreate, RateTableUpdate};
use crate::error::ApiError;
use crate::sync;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        // reload and core snapshot registered before /:id — axum matches by registration order
        // ===== Rate tables =====
        .route("/rates", get(list_rates::<C>).post(create_rate::<C>))
        .route("/rates/reload", post(reload_rates::<C>))
        .route("/rates/core", get(core_rates::<C>))
        .route(
            "/rates/:id",
            get(get_rate::<C>)
                .put(update_rate::<C>)
                .delete(delete_rate::<C>),
        )
        // ===== Accounts =====
        .route("/balances", get(list_accounts::<C>).post(open_account::<C>))
        .route("/balances/core", get(core_balances::<C>))
        .route("/balances/:id", get(get_account::<C>).put(update_account::<C>).delete(close_account::<C>))
        .route("/balances/:id/topup", post(topup_account::<C>))
        // ===== Rate Plans =====
        .route("/rate-plans", get(list_rate_plans::<C>).post(create_rate_plan::<C>))
        .route("/rate-plans/reload", post(reload_rate_plans::<C>))
        .route(
            "/rate-plans/:id",
            get(get_rate_plan::<C>)
                .put(update_rate_plan::<C>)
                .delete(delete_rate_plan::<C>),
        )
        // ===== Rate Plan Entries =====
        .route("/rate-plan-entries", get(list_rate_plan_entries::<C>).post(create_rate_plan_entry::<C>))
        .route("/rate-plan-entries/reload", post(reload_rate_plan_entries::<C>))
        .route(
            "/rate-plan-entries/:id",
            get(get_rate_plan_entry::<C>)
                .delete(delete_rate_plan_entry::<C>),
        )
        // ===== Billing Transactions =====
        .route("/transactions", get(list_transactions::<C>))
        .layer(ServiceBuilder::new().layer(TraceLayer::new_for_http()))
}

// ================================================================
// Rate table CRUD
// ================================================================

#[derive(Debug, Deserialize, Default)]
pub struct RateListQuery {
    pub prefix:  Option<String>,
    pub enabled: Option<String>,
    pub limit:   Option<i64>,
    pub offset:  Option<i64>,
}

async fn list_rates<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Query(q): Query<RateListQuery>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let enabled = match q.enabled.as_deref() {
        Some("true")  => Some(true),
        Some("false") => Some(false),
        _ => None,
    };
    let limit  = q.limit.unwrap_or(50).clamp(1, 500);
    let offset = q.offset.unwrap_or(0).max(0);

    let (items, total) = st.db
        .list_rates(q.prefix.as_deref(), enabled, limit, offset)
        .await.map_err(|e| ApiError::Db(e.to_string()))?;

    Ok(Json(json!({ "items": items, "total": total })))
}

async fn get_rate<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.get_rate(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("rate not found".into()))?;
    Ok(Json(row))
}

async fn create_rate<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<RateTableCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let row = st.db.create_rate(body).await.map_err(|e| {
        if let sqlx::Error::Database(ref db_err) = e {
            let msg = db_err.message().to_ascii_lowercase();
            if msg.contains("unique") || msg.contains("duplicate") {
                return ApiError::Conflict("prefix already exists".into());
            }
        }
        ApiError::Db(e.to_string())
    })?;

    let _ = st.db.insert_config_history(
        "api", "create",
        &format!("rates/{}", row.id),
        None, &format!("prefix={}", row.prefix),
    ).await;

    sync::spawn_push(st.clone(), sync::Kind::Rates);
    Ok((StatusCode::CREATED, Json(row)))
}

async fn update_rate<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
    Json(body): Json<RateTableUpdate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let row = st.db.update_rate(id, body).await.map_err(|e| {
        match e {
            sqlx::Error::RowNotFound => ApiError::NotFound("rate not found".into()),
            sqlx::Error::Database(ref db_err) => {
                let msg = db_err.message().to_ascii_lowercase();
                if msg.contains("unique") || msg.contains("duplicate") {
                    return ApiError::Conflict("prefix already exists".into());
                }
                ApiError::Db(e.to_string())
            }
            other => ApiError::Db(other.to_string()),
        }
    })?;

    let _ = st.db.insert_config_history(
        "api", "update",
        &format!("rates/{}", row.id),
        None, &format!("prefix={}", row.prefix),
    ).await;

    sync::spawn_push(st.clone(), sync::Kind::Rates);
    Ok(Json(row))
}

async fn delete_rate<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let row = st.db.get_rate(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("rate not found".into()))?;

    let deleted = st.db.delete_rate(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    if !deleted {
        return Err(ApiError::NotFound("rate not found".into()));
    }

    let _ = st.db.insert_config_history(
        "api", "delete",
        &format!("rates/{id}"),
        Some(&format!("prefix={}", row.prefix)),
        "deleted",
    ).await;

    sync::spawn_push(st.clone(), sync::Kind::Rates);
    Ok(StatusCode::NO_CONTENT)
}

/// POST /rates/reload — DB enabled rate tables → core UDS rate.reload
async fn reload_rates<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let rows = st.db.list_enabled_rates().await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    let json = serde_json::to_string(&rows).map_err(|e| ApiError::Db(e.to_string()))?;
    st.core.reload_rates(&json).await
        .map_err(|_| ApiError::CoreUnavailable)?;
    tracing::info!(count = rows.len(), "rate.reload pushed to core");

    let _ = st.db.insert_config_history(
        "api", "reload", "rates", None,
        &format!("loaded {} enabled rates", rows.len()),
    ).await;

    Ok(Json(json!({ "loaded": rows.len() })))
}

/// GET /rates/core — core-side real-time rate snapshot (show rates)
async fn core_rates<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<Json<RateList>, ApiError> {
    Ok(Json(st.core.show_rates().await?))
}

// ================================================================
// Account CRUD
// ================================================================

#[derive(Debug, Deserialize, Default)]
pub struct AccountListQuery {
    pub keyword: Option<String>,
    pub limit:   Option<i64>,
    pub offset:  Option<i64>,
}

/// POST /balances/:id/topup request body
#[derive(Debug, Deserialize)]
pub struct TopupBody {
    pub delta_uc: i64,
}

async fn list_accounts<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Query(q): Query<AccountListQuery>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let limit  = q.limit.unwrap_or(50).clamp(1, 500);
    let offset = q.offset.unwrap_or(0).max(0);

    let (items, total) = st.db
        .list_accounts(q.keyword.as_deref(), limit, offset)
        .await.map_err(|e| ApiError::Db(e.to_string()))?;

    Ok(Json(json!({ "items": items, "total": total })))
}

async fn get_account<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.get_account(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("account not found".into()))?;
    Ok(Json(row))
}

/// POST /balances — open account (DB + core UDS account.open)
async fn open_account<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<AccountCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let row = st.db.open_account(body.clone()).await.map_err(|e| {
        if let sqlx::Error::Database(ref db_err) = e {
            let msg = db_err.message().to_ascii_lowercase();
            if msg.contains("unique") || msg.contains("duplicate") {
                return ApiError::Conflict("account_key already exists".into());
            }
        }
        ApiError::Db(e.to_string())
    })?;

    // Sync push to core（快速路径：立即生效）
    let core_res = st.core.account_open(
        &body.account_key, body.balance_uc, body.credit_limit_uc,
    ).await;
    if let Err(e) = &core_res {
        tracing::warn!(error = %e, key = %body.account_key,
            "account.open core failed → spawning accounts.reload safety net");
        // 安全网：300ms debounce + 5 级退避重试 + 60s periodic + HELLO 重连
        // 全量 reload 用 DB 里的绝对余额覆盖，幂等自愈 divergence
        sync::spawn_push(st.clone(), sync::Kind::Accounts);
    }

    let _ = st.db.insert_config_history(
        "api", "create",
        &format!("balances/{}", row.id),
        None, &format!("key={}", row.account_key),
    ).await;

    Ok((StatusCode::CREATED, Json(row)))
}

/// POST /balances/:id/topup — top up (DB + core UDS account.topup)
async fn topup_account<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
    Json(body): Json<TopupBody>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    // Look up account first (existence check + fetch key)
    let acct = st.db.get_account(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("account not found".into()))?;

    let row = st.db.topup_account(id, body.delta_uc).await.map_err(|e| {
        match e {
            sqlx::Error::RowNotFound => ApiError::NotFound("account not found".into()),
            other => ApiError::Db(other.to_string()),
        }
    })?;

    // Sync push to core（快速路径：立即生效）
    let core_res = st.core.account_topup(&acct.account_key, body.delta_uc).await;
    if let Err(e) = &core_res {
        tracing::warn!(error = %e, key = %acct.account_key, delta = body.delta_uc,
            "account.topup core failed → spawning accounts.reload safety net");
        // 安全网：accounts.reload 用 DB 里的绝对余额覆盖，幂等自愈 divergence
        sync::spawn_push(st.clone(), sync::Kind::Accounts);
    }

    let _ = st.db.insert_config_history(
        "api", "topup",
        &format!("balances/{}", row.id),
        Some(&format!("balance_uc={}", acct.balance_uc)),
        &format!("balance_uc={} delta={}", row.balance_uc, body.delta_uc),
    ).await;

    Ok(Json(row))
}

/// PUT /balances/:id — update account (description + credit_limit only; balance managed by billing system)
async fn update_account<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
    Json(body): Json<AccountCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let row = st.db.update_account(id, &body.description, body.credit_limit_uc).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("account not found".into()))?;

    let _ = st.db.insert_config_history(
        "api", "update",
        &format!("balances/{}", row.id),
        None,
        &format!("description={}, credit_limit_uc={}", row.description, row.credit_limit_uc),
    ).await;

    Ok(Json(row))
}

/// DELETE /balances/:id — close account (DB only; core side keeps reserve state to
/// avoid settling miscalculations)
async fn close_account<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let row = st.db.get_account(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("account not found".into()))?;

    let deleted = st.db.delete_account(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    if !deleted {
        return Err(ApiError::NotFound("account not found".into()));
    }

    let _ = st.db.insert_config_history(
        "api", "delete",
        &format!("balances/{id}"),
        Some(&format!("key={}", row.account_key)),
        "closed (db only, core state preserved)",
    ).await;

    Ok(StatusCode::NO_CONTENT)
}

/// GET /balances/core — core-side real-time balance snapshot (show balances)
async fn core_balances<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<Json<AccountList>, ApiError> {
    Ok(Json(st.core.show_balances().await?))
}

// ================================================================
// Rate Plan CRUD (Phase 1)
// ================================================================

#[derive(Debug, Deserialize, Default)]
pub struct RatePlanListQuery {
    pub tenant_id: Option<i64>,
    pub enabled:   Option<String>,
    pub limit:     Option<i64>,
    pub offset:    Option<i64>,
}

async fn list_rate_plans<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Query(q): Query<RatePlanListQuery>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let enabled = match q.enabled.as_deref() {
        Some("true")  => Some(true),
        Some("false") => Some(false),
        _ => None,
    };
    let limit  = q.limit.unwrap_or(50).clamp(1, 500);
    let offset = q.offset.unwrap_or(0).max(0);

    let (items, total) = st.db
        .list_rate_plans(q.tenant_id, enabled, limit, offset)
        .await.map_err(|e| ApiError::Db(e.to_string()))?;

    Ok(Json(json!({ "items": items, "total": total })))
}

async fn get_rate_plan<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.get_rate_plan(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("rate plan not found".into()))?;
    Ok(Json(row))
}

async fn create_rate_plan<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<RatePlanCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let row = st.db.create_rate_plan(body).await.map_err(|e| {
        if let sqlx::Error::Database(ref db_err) = e {
            let msg = db_err.message().to_ascii_lowercase();
            if msg.contains("unique") || msg.contains("duplicate") {
                return ApiError::Conflict("rate plan name already exists".into());
            }
        }
        ApiError::Db(e.to_string())
    })?;

    sync::spawn_push(st.clone(), sync::Kind::RatePlans);
    Ok((StatusCode::CREATED, Json(row)))
}

async fn update_rate_plan<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
    Json(body): Json<RatePlanUpdate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let row = st.db.update_rate_plan(
        id,
        body.name.as_deref(),
        body.description.as_deref(),
        body.enabled,
    ).await.map_err(|e| {
        match e {
            sqlx::Error::RowNotFound => ApiError::NotFound("rate plan not found".into()),
            sqlx::Error::Database(ref db_err) => {
                let msg = db_err.message().to_ascii_lowercase();
                if msg.contains("unique") || msg.contains("duplicate") {
                    return ApiError::Conflict("rate plan name already exists".into());
                }
                ApiError::Db(e.to_string())
            }
            other => ApiError::Db(other.to_string()),
        }
    })?.ok_or_else(|| ApiError::NotFound("rate plan not found".into()))?;

    sync::spawn_push(st.clone(), sync::Kind::RatePlans);
    Ok(Json(row))
}

async fn delete_rate_plan<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    // Pre-check: plan must not have entries
    let entry_count = st.db.count_rate_plan_entries_for_plan(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    if entry_count > 0 {
        return Err(ApiError::Conflict("plan has entries".into()));
    }

    let deleted = st.db.delete_rate_plan(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    if !deleted {
        return Err(ApiError::NotFound("rate plan not found".into()));
    }

    sync::spawn_push(st.clone(), sync::Kind::RatePlans);
    Ok(StatusCode::NO_CONTENT)
}

/// POST /rate-plans/reload — DB enabled rate plans → core UDS rate_plans.reload
async fn reload_rate_plans<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let rows = st.db.list_enabled_rate_plans().await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    let json = serde_json::to_string(&rows).map_err(|e| ApiError::Db(e.to_string()))?;
    st.core.reload_rate_plans(&json).await
        .map_err(|_| ApiError::CoreUnavailable)?;
    tracing::info!(count = rows.len(), "rate_plans.reload pushed to core");
    Ok(Json(json!({ "loaded": rows.len() })))
}

// ================================================================
// Rate Plan Entry CRUD (Phase 1)
// ================================================================

#[derive(Debug, Deserialize, Default)]
pub struct RatePlanEntryListQuery {
    pub plan_id: Option<i64>,
    pub enabled:  Option<String>,
    pub limit:    Option<i64>,
    pub offset:   Option<i64>,
}

async fn list_rate_plan_entries<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Query(q): Query<RatePlanEntryListQuery>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let enabled = match q.enabled.as_deref() {
        Some("true")  => Some(true),
        Some("false") => Some(false),
        _ => None,
    };
    let limit  = q.limit.unwrap_or(50).clamp(1, 500);
    let offset = q.offset.unwrap_or(0).max(0);

    let (items, total) = st.db
        .list_rate_plan_entries(q.plan_id, None, enabled, limit, offset)
        .await.map_err(|e| ApiError::Db(e.to_string()))?;

    Ok(Json(json!({ "items": items, "total": total })))
}

async fn get_rate_plan_entry<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let row = st.db.get_rate_plan_entry(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("rate plan entry not found".into()))?;
    Ok(Json(row))
}

async fn create_rate_plan_entry<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<RatePlanEntryCreate>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let row = st.db.create_rate_plan_entry(body).await.map_err(|e| {
        if let sqlx::Error::Database(ref db_err) = e {
            let msg = db_err.message().to_ascii_lowercase();
            if msg.contains("unique") || msg.contains("duplicate") {
                return ApiError::Conflict("rate plan entry already exists".into());
            }
        }
        ApiError::Db(e.to_string())
    })?;

    sync::spawn_push(st.clone(), sync::Kind::RatePlanEntries);
    Ok((StatusCode::CREATED, Json(row)))
}

async fn delete_rate_plan_entry<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let deleted = st.db.delete_rate_plan_entry(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    if !deleted {
        return Err(ApiError::NotFound("rate plan entry not found".into()));
    }

    sync::spawn_push(st.clone(), sync::Kind::RatePlanEntries);
    Ok(StatusCode::NO_CONTENT)
}

/// POST /rate-plan-entries/reload — DB enabled rate plan entries → core UDS
async fn reload_rate_plan_entries<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let rows = st.db.list_enabled_rate_plan_entries().await
        .map_err(|e| ApiError::Db(e.to_string()))?;
    let json = serde_json::to_string(&rows).map_err(|e| ApiError::Db(e.to_string()))?;
    st.core.reload_rate_plan_entries(&json).await
        .map_err(|_| ApiError::CoreUnavailable)?;
    tracing::info!(count = rows.len(), "rate_plan_entries.reload pushed to core");
    Ok(Json(json!({ "loaded": rows.len() })))
}

// ================================================================
// Billing Transactions (Phase 1)
// ================================================================

#[derive(Debug, Deserialize, Default)]
pub struct TransactionListQuery {
    pub account_id: Option<i64>,
    pub call_id:    Option<String>,
    pub tx_type:    Option<String>,
    pub limit:      Option<i64>,
    pub offset:     Option<i64>,
}

async fn list_transactions<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Query(q): Query<TransactionListQuery>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let limit  = q.limit.unwrap_or(50).clamp(1, 500);
    let offset = q.offset.unwrap_or(0).max(0);

    let (items, total) = st.db
        .list_billing_transactions(
            q.account_id,
            q.call_id.as_deref(),
            q.tx_type.as_deref(),
            limit,
            offset,
        )
        .await.map_err(|e| ApiError::Db(e.to_string()))?;

    Ok(Json(json!({ "items": items, "total": total })))
}
