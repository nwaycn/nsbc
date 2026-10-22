//! 配置接口（REQ-WEB-004 / REQ-CFG-011/013）：
//! - GET /config       全量脱敏生效视图（C2：转发核心 `config.export`）
//! - GET /config/:key  生效值只读查询（C1：转发核心 `config.get`）
//! - POST /config/set         热设置（L1，转发核心 `config.set`；审计到 config_history）
//! - POST /config/validate    dry-run 校验（L0，转发核心 `config.validate`，不改状态）
//!
//! 注意：config.set 的操作人（L1 用户身份）已由 rbac middleware 的独立审计器
//! 记录（含 sub/username/method/path/status），config_history 只做配置级变更快照。
use std::sync::Arc;

use axum::extract::{Path, State};
use axum::routing::{get, post};
use axum::{Json, Router};
use serde::Deserialize;
use serde_json::json;

use crate::coreclient::{ConfigSetResult, CoreApi, CoreError};
use crate::error::{ApiError, ApiResult};
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/config", get(get_config::<C>))
        .route("/config/:key", get(get_config_key::<C>))
        .route("/config/set", post(post_config_set::<C>))
        .route("/config/validate", post(post_config_validate::<C>))
        .route("/listens", get(listens))
}

/// 配置键路由层校验：1..127 字节、字符集 [A-Za-z0-9._-]，不以点起/止、
/// 不含连续点（与核心 obs_control valid_cfg_key 双端对齐）。
fn valid_config_key(key: &str) -> bool {
    let n = key.len();
    n >= 1
        && n < 128
        && !key.starts_with('.')
        && !key.ends_with('.')
        && !key.contains("..")
        && key.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-')
}

/// 请求体（value 允许空串删除语义；路由层不截断，核心侧 valid_cfg_value 校验）。
#[derive(Debug, Deserialize)]
pub struct ConfigWriteReq {
    pub key: String,
    #[serde(default)]
    pub value: String,
}

/// GET /api/v1/config/{key}：核心生效值（L0 只读，REQ-CFG-011）。
/// 键不存在 → 404；键格式非法 → 400（不发控制命令）；核心不可达 → 502/504。
async fn get_config_key<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(key): Path<String>,
) -> ApiResult<Json<crate::coreclient::ConfigValue>> {
    if !valid_config_key(&key) {
        return Err(ApiError::BadRequest("invalid config key".into()));
    }
    match st.core.config_get(&key).await {
        Ok(v) => Ok(Json(v)),
        Err(CoreError::NotFound(_)) => Err(ApiError::ConfigKeyNotFound),
        Err(e) => Err(e.into()),
    }
}

/// GET /api/v1/config：核心全量脱敏生效视图（L0 只读，REQ-CFG-013）。
/// 核心不可达 → 502/504；敏感项 value=null/secret=true，**** 不出边车。
async fn get_config<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<crate::coreclient::ConfigExport>> {
    Ok(Json(st.core.config_export().await?))
}

/// POST /api/v1/config/validate：dry-run 值校验（L0，REQ-CFG-011）。
/// 校验通过 → 200 `{"valid":true,"key":...}`；非法 → 400 + 核心侧原因。
async fn post_config_validate<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(req): Json<ConfigWriteReq>,
) -> ApiResult<Json<serde_json::Value>> {
    if !valid_config_key(&req.key) {
        return Err(ApiError::BadRequest("invalid config key".into()));
    }
    match st.core.config_validate(&req.key, &req.value).await {
        Ok(()) => Ok(Json(json!({ "valid": true, "key": req.key }))),
        Err(CoreError::CoreRejected(reason)) => Err(ApiError::BadRequest(reason)),
        Err(e) => Err(e.into()),
    }
}

/// POST /api/v1/config/set：热设置（L1，REQ-CFG-011）。
/// 成功 → 200 + ConfigSetResult；核心拒绝 → 400 + 原因；后端 502/504 不变。
/// 成功后写 config_history（DB 未配置则静默跳过）。
async fn post_config_set<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<ConfigWriteReq>,
) -> ApiResult<Json<ConfigSetResult>> {
    if !valid_config_key(&body.key) {
        return Err(ApiError::BadRequest("invalid config key".into()));
    }

    let result = match st.core.config_set(&body.key, &body.value).await {
        Ok(r) => r,
        Err(CoreError::CoreRejected(reason)) => return Err(ApiError::BadRequest(reason)),
        Err(e) => return Err(e.into()),
    };

    // DB 写入 config_history：actor 为 Web 侧 L1 操作占位符（操作人身份已由
    // rbac middleware 独立审计记录）。DB 未配置静默跳过；写入失败降级（不
    // 阻断 API 成功响应，防止审计反压业务）。
    if let Err(e) = st
        .db
        .insert_config_history(
            "api",
            if result.changed { "set" } else { "set-noop" },
            &result.key,
            None, // 原子 set 无法 pre-read old_value，留空
            &body.value,
        )
        .await
    {
        tracing::warn!(error = %e, key = %result.key,
            "config_history write failed — set committed but history missed");
    }

    Ok(Json(result))
}

async fn listens() -> ApiResult<Json<serde_json::Value>> {
    Ok(Json(json!({
        "status": "stub",
        "listens": [],
    })))
}
