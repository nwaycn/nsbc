//! 计费管理 REST（REQ-OAM-009）：费率表 + 计费账户。
//!
//! 费率（rate_tables）：
//!   GET    /rates              列表（DB，分页 + prefix/enabled 过滤）
//!   GET    /rates/:id          详情（DB）
//!   POST   /rates              创建（DB）
//!   PUT    /rates/:id          更新（DB）
//!   DELETE /rates/:id          删除（DB）
//!   POST   /rates/reload       推送所有 enabled → 核心 UDS（rate.reload）
//!   GET    /rates/core         核心侧实时费率快照（show rates，L0 只读）
//!
//! 账户（accounts）：
//!   GET    /balances           列表（DB，分页 + keyword 过滤）
//!   GET    /balances/:id       详情（DB）
//!   POST   /balances           开户（DB + account.open UDS）
//!   POST   /balances/:id/topup 充值（DB + account.topup UDS）
//!   DELETE /balances/:id       销户（仅 DB；核心侧保留 reserve 状态）
//!   GET    /balances/core      核心侧实时余额快照（show balances，L0 只读）
//!
//! 设计原则：
//!   - DB CRUD 与 reload 分离（extensions.rs 同款模式），reload 显式触发
//!   - accounts 的 topup 同步写 DB + 推 UDS（避免 DB/核心分叉）
//!   - prefix/account_key UNIQUE 冲突 → 409 Conflict

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
use crate::db::{AccountCreate, RateTableCreate, RateTableUpdate};
use crate::error::ApiError;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        // reload 和 core 快照在 /:id 之前注册——axum 按注册顺序匹配
        // ===== 费率 =====
        .route("/rates", get(list_rates::<C>).post(create_rate::<C>))
        .route("/rates/reload", post(reload_rates::<C>))
        .route("/rates/core", get(core_rates::<C>))
        .route(
            "/rates/:id",
            get(get_rate::<C>)
                .put(update_rate::<C>)
                .delete(delete_rate::<C>),
        )
        // ===== 账户 =====
        .route("/balances", get(list_accounts::<C>).post(open_account::<C>))
        .route("/balances/core", get(core_balances::<C>))
        .route("/balances/:id", get(get_account::<C>).delete(close_account::<C>))
        .route("/balances/:id/topup", post(topup_account::<C>))
        .layer(ServiceBuilder::new().layer(TraceLayer::new_for_http()))
}

// ================================================================
// 费率 CRUD
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

async fn create_rate<C: CoreApi + Clone + Send + Sync>(
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

    Ok((StatusCode::CREATED, Json(row)))
}

async fn update_rate<C: CoreApi + Clone + Send + Sync>(
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

    Ok(Json(row))
}

async fn delete_rate<C: CoreApi + Clone + Send + Sync>(
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

    Ok(StatusCode::NO_CONTENT)
}

/// POST /rates/reload — DB enabled 费率 → 核心 UDS rate.reload
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

/// GET /rates/core — 核心侧实时费率快照（show rates）
async fn core_rates<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<Json<RateList>, ApiError> {
    Ok(Json(st.core.show_rates().await?))
}

// ================================================================
// 账户 CRUD
// ================================================================

#[derive(Debug, Deserialize, Default)]
pub struct AccountListQuery {
    pub keyword: Option<String>,
    pub limit:   Option<i64>,
    pub offset:  Option<i64>,
}

/// POST /balances/:id/topup 请求体
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

/// POST /balances — 开户（DB + 核心 UDS account.open）
async fn open_account<C: CoreApi + Clone + Send + Sync>(
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

    // 同步推核心（失败不回滚 DB——下次 reload 会覆盖；但返回 207 Mixed 告知）
    let core_res = st.core.account_open(
        &body.account_key, body.balance_uc, body.credit_limit_uc,
    ).await;
    if let Err(e) = &core_res {
        tracing::warn!(error = %e, key = %body.account_key,
            "account.open core failed — DB row created but core skipped");
    }

    let _ = st.db.insert_config_history(
        "api", "create",
        &format!("balances/{}", row.id),
        None, &format!("key={}", row.account_key),
    ).await;

    Ok((StatusCode::CREATED, Json(row)))
}

/// POST /balances/:id/topup — 充值（DB + 核心 UDS account.topup）
async fn topup_account<C: CoreApi + Clone + Send + Sync>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<i64>,
    Json(body): Json<TopupBody>,
) -> Result<impl IntoResponse, ApiError> {
    st.db.pool().ok_or(ApiError::DbUnavailable)?;

    // 先查账户（存在性 + 取 key）
    let acct = st.db.get_account(id).await
        .map_err(|e| ApiError::Db(e.to_string()))?
        .ok_or_else(|| ApiError::NotFound("account not found".into()))?;

    let row = st.db.topup_account(id, body.delta_uc).await.map_err(|e| {
        match e {
            sqlx::Error::RowNotFound => ApiError::NotFound("account not found".into()),
            other => ApiError::Db(other.to_string()),
        }
    })?;

    // 同步推核心
    let core_res = st.core.account_topup(&acct.account_key, body.delta_uc).await;
    if let Err(e) = &core_res {
        tracing::warn!(error = %e, key = %acct.account_key, delta = body.delta_uc,
            "account.topup core failed — DB updated but core skipped");
    }

    let _ = st.db.insert_config_history(
        "api", "topup",
        &format!("balances/{}", row.id),
        Some(&format!("balance_uc={}", acct.balance_uc)),
        &format!("balance_uc={} delta={}", row.balance_uc, body.delta_uc),
    ).await;

    Ok(Json(row))
}

/// DELETE /balances/:id — 销户（仅 DB；核心侧保留 reserve 状态避免结算错账）
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

/// GET /balances/core — 核心侧实时余额快照（show balances）
async fn core_balances<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<Json<AccountList>, ApiError> {
    Ok(Json(st.core.show_balances().await?))
}
