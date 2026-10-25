//! REQ-EXT-API-001: API Key / Webhook 管理接口（Web UI 用）
//!
//! merge 到 protected router → nest 到 /api/v1 → 被 rbac::require_auth 保护。
//! 路径实际是 /api/v1/ext-keys 和 /api/v1/webhooks。

use axum::{
    extract::{Extension, Path, State},
    http::StatusCode,
    response::IntoResponse,
    routing::{delete, get, post},
    Json, Router,
};
use ring::digest::{Context, SHA256};
use serde::{Deserialize, Serialize};

use std::sync::Arc;

use crate::auth::Identity;
use crate::auth::Role;
use crate::error::ApiError;
use crate::AppState;

type ApiResult<T> = Result<T, ApiError>;

fn hex_encode(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0x0f) as usize] as char);
    }
    out
}

fn hash_key(raw: &str) -> String {
    let mut h = Context::new(&SHA256);
    h.update(raw.as_bytes());
    hex_encode(h.finish().as_ref())
}

fn require_l2(Identity(claims): &Identity) -> Result<(), ApiError> {
    if claims.role < Role::L2 as u8 {
        return Err(ApiError::Forbidden { required: "L2" });
    }
    Ok(())
}

// ======================================================================
// API Key CRUD
// ======================================================================

#[derive(Debug, Serialize)]
pub struct ExtKeyRow {
    pub id: i64,
    pub label: String,
    pub scope: String,
    pub created_at: String,
    pub last_used_at: Option<String>,
    pub revoked_at: Option<String>,
    pub active: bool,
}

#[derive(Debug, Deserialize)]
pub struct ExtKeyCreate {
    pub label: String,
    #[serde(default = "default_scope")]
    pub scope: String,
}
fn default_scope() -> String { "readwrite".into() }

#[derive(Debug, Serialize)]
pub struct ExtKeyCreated {
    pub id: i64,
    pub raw_key: String,
    pub label: String,
    pub scope: String,
}

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: crate::coreclient::CoreApi + Clone + 'static,
{
    Router::new()
        // === REQ-EXT-API-003: Introspection（JWT 侧，给 Web UI guide 页用）===
        .route("/ext-docs", get(ext_docs::<C>))
        // === REQ-EXT-API-001: API Key / Webhook CRUD ===
        .route("/ext-keys", get(list_ext_keys::<C>))
        .route("/ext-keys", post(create_ext_key::<C>))
        .route("/ext-keys/:id/revoke", post(revoke_ext_key::<C>))
        .route("/ext-keys/:id", delete(delete_ext_key::<C>))
        .route("/webhooks", get(list_webhooks::<C>))
        .route("/webhooks", post(create_webhook::<C>))
        .route("/webhooks/:id", delete(delete_webhook::<C>))
}

// ======================================================================
// REQ-EXT-API-003: GET /api/v1/ext-docs — Web UI JWT 侧的 introspection
// 返回 /ext/v1/* 所有端点清单（从 ext.rs 导入的 ENDPOINTS/SCOPES）
// + nsbcd help 命令解析成 JSON（动态从 core 拿）
// L1 只读即可（文档不应要求 L2）
// ======================================================================

#[derive(Debug, Serialize)]
pub struct ExtDocsResp {
    pub api_major: u32,
    pub web_version: &'static str,
    pub base_path: &'static str,
    pub scopes: &'static [crate::routes::ext::ScopeDesc],
    pub endpoints: &'static [crate::routes::ext::EndpointDesc],
    pub cmds: Vec<crate::routes::ext::CmdDescJson>,
    pub cmd_count: usize,
}

async fn ext_docs<C>(
    State(st): State<Arc<AppState<C>>>,
    Extension(_identity): Extension<Identity>,
) -> ApiResult<Json<ExtDocsResp>>
where
    C: crate::coreclient::CoreApi + Clone + 'static,
{
    // 从 ext.rs 导入元信息（compile-time const，和 router 注册一一对应）
    let scopes = crate::routes::ext::SCOPES;
    let endpoints = crate::routes::ext::ENDPOINTS;

    // 动态调 nsbcd 拿 help 列表
    let raw = st.core.exec_cmd("help").await?;
    let mut cmds = Vec::new();
    for line in raw.lines() {
        let line = line.trim();
        if line.is_empty() || !line.contains('[') { continue; }
        if let Some(bracket_pos) = line.find(" [") {
            let name = line[..bracket_pos].trim().to_string();
            let rest = &line[bracket_pos + 2..];
            if let Some(level_end) = rest.find(']') {
                let level_char = rest[..level_end].chars().next().unwrap_or('1');
                let help = rest[level_end + 1..].trim().to_string();
                let level: u8 = if level_char == '2' { 2 } else { 1 };
                if !name.is_empty() {
                    cmds.push(crate::routes::ext::CmdDescJson { name, level, help });
                }
            }
        }
    }
    cmds.sort_by(|a, b| a.name.cmp(&b.name));

    Ok(Json(ExtDocsResp {
        api_major: st.api_major,
        web_version: env!("CARGO_PKG_VERSION"),
        base_path: "/ext/v1",
        scopes,
        endpoints,
        cmd_count: cmds.len(),
        cmds,
    }))
}

// --- List ---

async fn list_ext_keys<C: crate::coreclient::CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Extension(identity): Extension<Identity>,
) -> ApiResult<Json<Vec<ExtKeyRow>>> {
    require_l2(&identity)?;
    let pool = st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let rows: Vec<(i64, String, String, String, Option<String>, Option<String>)> = sqlx::query_as(
        "SELECT id, label, scope, created_at, last_used_at, revoked_at \
         FROM ext_api_keys ORDER BY id DESC LIMIT 500",
    )
    .fetch_all(pool)
    .await
    .map_err(|_| ApiError::Db("query failed".into()))?;

    Ok(Json(
        rows.into_iter()
            .map(|(id, label, scope, created_at, last_used_at, revoked_at)| ExtKeyRow {
                id, label, scope, created_at, last_used_at: last_used_at.clone(),
                revoked_at: revoked_at.clone(), active: revoked_at.is_none(),
            })
            .collect(),
    ))
}

// --- Create（生成随机 key，返回一次）---

async fn create_ext_key<C: crate::coreclient::CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Extension(identity): Extension<Identity>,
    Json(body): Json<ExtKeyCreate>,
) -> ApiResult<Json<ExtKeyCreated>> {
    require_l2(&identity)?;
    if body.label.is_empty() || body.label.len() > 128 {
        return Err(ApiError::BadRequest("label 1..128 bytes".into()));
    }
    if !matches!(body.scope.as_str(), "readonly" | "readwrite" | "admin") {
        return Err(ApiError::BadRequest("scope must be readonly/readwrite/admin".into()));
    }

    let pool = st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let backend = st.db.backend();

    let mut random = [0u8; 32];
    getrandom::getrandom(&mut random).map_err(|_| {
        ApiError::ServiceUnavailable("getrandom failed".into())
    })?;
    let raw_key = format!("nsbc_live_{}", hex_encode(&random));
    let key_hash = hash_key(&raw_key);

    // INSERT — 分 backend：postgres RETURNING，sqlite/mysql 无 RETURNING，之后单独查 id
    let sql = match backend {
        "postgres" =>
            "INSERT INTO ext_api_keys (key_hash, label, scope) VALUES ($1, $2, $3) RETURNING id",
        _ =>
            "INSERT INTO ext_api_keys (key_hash, label, scope) VALUES (?, ?, ?)",
    };

    let id: i64 = match backend {
        "postgres" => sqlx::query_as::<_, (i64,)>(sql)
            .bind(&key_hash).bind(&body.label).bind(&body.scope)
            .fetch_one(pool).await
            .map_err(|e| { tracing::error!("create_ext_key insert error: {e}");
                ApiError::Db("insert failed".into()) })?
            .0,
        _ => {
            sqlx::query(sql)
                .bind(&key_hash).bind(&body.label).bind(&body.scope)
                .execute(pool).await
                .map_err(|e| { tracing::error!("create_ext_key insert error: {e}");
                    ApiError::Db("insert failed".into()) })?;
            // SQLite: last_insert_rowid(), MySQL: LAST_INSERT_ID()
            let (v,): (i64,) = if backend == "mysql" {
                sqlx::query_as("SELECT LAST_INSERT_ID()").fetch_one(pool).await
                    .map_err(|_| ApiError::Db("id lookup failed".into()))?
            } else {
                sqlx::query_as("SELECT last_insert_rowid()").fetch_one(pool).await
                    .map_err(|_| ApiError::Db("id lookup failed".into()))?
            };
            v
        }
    };

    tracing::warn!(
        "🔑 new ext-api-key id={id} scope={} label='{}' raw_key=*** (shown once to caller)",
        body.scope, body.label,
    );

    Ok(Json(ExtKeyCreated { id, raw_key, label: body.label, scope: body.scope }))
}

// --- Revoke（soft-delete）---

async fn revoke_ext_key<C: crate::coreclient::CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Extension(identity): Extension<Identity>,
    Path(id): Path<i64>,
) -> ApiResult<StatusCode> {
    require_l2(&identity)?;
    let pool = st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let backend = st.db.backend();

    let now_expr = match backend {
        "postgres" | "mysql" => "CURRENT_TIMESTAMP",
        _ => "datetime('now')",
    };
    let sql = format!(
        "UPDATE ext_api_keys SET revoked_at = {now_expr} WHERE id = $1 AND revoked_at IS NULL"
    );

    let rc = sqlx::query(&sql).bind(id).execute(pool).await
        .map_err(|_| ApiError::Db("revoke failed".into()))?;

    if rc.rows_affected() == 0 { return Err(ApiError::NotFound("ext_api_key".into())); }
    Ok(StatusCode::OK)
}

// --- Delete（hard delete）---

async fn delete_ext_key<C: crate::coreclient::CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Extension(identity): Extension<Identity>,
    Path(id): Path<i64>,
) -> ApiResult<StatusCode> {
    require_l2(&identity)?;
    let pool = st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let rc = sqlx::query("DELETE FROM ext_api_keys WHERE id = $1").bind(id).execute(pool).await
        .map_err(|_| ApiError::Db("delete failed".into()))?;

    if rc.rows_affected() == 0 { return Err(ApiError::NotFound("ext_api_key".into())); }
    Ok(StatusCode::OK)
}

// ======================================================================
// Webhook 管理（P1 dispatcher 后续加，先建表 + CRUD）
// ======================================================================

#[derive(Debug, Serialize)]
pub struct WebhookRow {
    pub id: i64,
    pub url: String,
    pub events_filter: String,
    pub active: bool,
    pub last_ok_at: Option<String>,
    pub fail_count: i64,
}

#[derive(Debug, Deserialize)]
pub struct WebhookCreate {
    pub url: String,
    #[serde(default = "default_events")]
    pub events_filter: String,
}
fn default_events() -> String { "CALL_INCOMING,CALL_ANSWERED,CALL_END".into() }

async fn list_webhooks<C: crate::coreclient::CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Extension(identity): Extension<Identity>,
) -> ApiResult<Json<Vec<WebhookRow>>> {
    require_l2(&identity)?;
    let pool = st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let rows: Vec<(i64, String, String, bool, Option<String>, i64)> = sqlx::query_as(
        "SELECT id, url, events_filter, active, last_ok_at, fail_count \
         FROM ext_webhooks ORDER BY id DESC LIMIT 500",
    )
    .fetch_all(pool)
    .await
    .map_err(|_| ApiError::Db("query failed".into()))?;

    Ok(Json(rows.into_iter().map(|(id, url, events_filter, active, last_ok_at, fail_count)| {
        WebhookRow { id, url, events_filter, active, last_ok_at, fail_count }
    }).collect()))
}

async fn create_webhook<C: crate::coreclient::CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Extension(identity): Extension<Identity>,
    Json(body): Json<WebhookCreate>,
) -> ApiResult<Json<WebhookRow>> {
    require_l2(&identity)?;
    if !body.url.starts_with("https://") && !body.url.starts_with("http://") {
        return Err(ApiError::BadRequest("url must start with http(s)://".into()));
    }
    if body.url.len() > 512 {
        return Err(ApiError::BadRequest("url too long".into()));
    }

    let pool = st.db.pool().ok_or(ApiError::DbUnavailable)?;
    let backend = st.db.backend();

    let id: i64 = match backend {
        "postgres" => {
            sqlx::query_as::<_, (i64,)>(
                "INSERT INTO ext_webhooks (url, events_filter) VALUES ($1, $2) RETURNING id",
            )
            .bind(&body.url).bind(&body.events_filter)
            .fetch_one(pool).await
            .map_err(|e| { tracing::error!("create_webhook insert error: {e}");
                ApiError::Db("insert failed".into()) })?
            .0
        }
        _ => {
            sqlx::query("INSERT INTO ext_webhooks (url, events_filter) VALUES (?, ?)")
                .bind(&body.url).bind(&body.events_filter)
                .execute(pool).await
                .map_err(|e| { tracing::error!("create_webhook insert error: {e}");
                    ApiError::Db("insert failed".into()) })?;
            let (v,): (i64,) = if backend == "mysql" {
                sqlx::query_as("SELECT LAST_INSERT_ID()").fetch_one(pool).await
                    .map_err(|_| ApiError::Db("id lookup failed".into()))?
            } else {
                sqlx::query_as("SELECT last_insert_rowid()").fetch_one(pool).await
                    .map_err(|_| ApiError::Db("id lookup failed".into()))?
            };
            v
        }
    };

    Ok(Json(WebhookRow {
        id, url: body.url, events_filter: body.events_filter,
        active: true, last_ok_at: None, fail_count: 0,
    }))
}

async fn delete_webhook<C: crate::coreclient::CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Extension(identity): Extension<Identity>,
    Path(id): Path<i64>,
) -> ApiResult<StatusCode> {
    require_l2(&identity)?;
    let pool = st.db.pool().ok_or(ApiError::DbUnavailable)?;

    let rc = sqlx::query("DELETE FROM ext_webhooks WHERE id = $1").bind(id).execute(pool).await
        .map_err(|_| ApiError::Db("delete failed".into()))?;

    if rc.rows_affected() == 0 { return Err(ApiError::NotFound("webhook".into())); }
    Ok(StatusCode::OK)
}
