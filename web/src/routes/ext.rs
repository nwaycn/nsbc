//! External API surface (REQ-EXT-API-001 + REQ-EXT-API-002): `/ext/v1/*`
//!
//! 和 Web UI 的 `/api/v1/*`（JWT + rbac）完全隔离 —— 外部系统只需要一个
//! API Key 就能：
//!   1) 透明代理所有控制命令（fs_cli / nsbc_cli 等价能力）
//!   2) 接收 SSE 实时事件流
//!   3) 读系统状态
//!
//! 架构：nsbc-web 是 **n sbcd 控制 socket 的 HTTP 透明代理**。外部系统发
//! POST /ext/v1/cmd 任意命令 → nsbc-web 通过 control.sock 直送 nsbcd →
//! 返回纯文本 stdout。这等价于 FreeSWITCH 的 mod_event_socket + fs_cli
//! 的完整能力——只是传输层从 TCP 变成了 HTTPS。

use axum::{
    body::Body,
    extract::{Extension, Path, State},
    http::{header, StatusCode},
    response::{IntoResponse, Response},
    routing::{get, post},
    Json, Router,
};
use axum::response::sse::{Event, KeepAlive, Sse};
use serde::{Deserialize, Serialize};
use std::convert::Infallible;
use tokio_stream::{StreamExt, wrappers::BroadcastStream};
use std::sync::Arc;
use crate::coreclient::CoreApi;
use crate::error::ApiError;
use crate::AppState;
use crate::middleware::ext_key::{ApiKeyScope, ExtKeyClaims};

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        // === REQ-EXT-API-002: 透明代理（核心能力）===
        .route("/cmd", post(exec_cmd::<C>))
        // === REQ-EXT-API-003: Introspection（动态自描述）===
        .route("/docs", get(api_docs))
        .route("/cmd/help", get(cmd_help::<C>))
        // === REQ-EXT-API-001: 结构化端点（方便外部系统的 JSON API 消费）===
        .route("/originate", post(originate::<C>))
        .route("/calls/{id}/hangup", post(hangup::<C>))
        .route("/calls", get(list_calls::<C>))
        .route("/system/status", get(system_status::<C>))
        .route("/system/version", get(system_version::<C>))
        // === 事件流 ===
        .route("/events/stream", get(events_stream::<C>))
}

fn require_scope(claims: &ExtKeyClaims, need: ApiKeyScope) -> Result<(), ApiError> {
    if !claims.scope.allows(need) {
        return Err(ApiError::Forbidden { required: "insufficient scope" });
    }
    Ok(())
}

// ======================================================================
// POST /ext/v1/cmd  —— 透明代理：任意控制命令直送 nsbcd
// ======================================================================

/// 请求体：纯命令字符串（不含换行），最大 4096 字节。
#[derive(Debug, Deserialize)]
pub struct CmdBody {
    pub cmd: String,
}

/// 成功响应：nsbcd 返回的纯文本 stdout（命令执行后、下一个 nsbc> 之前的内容）。
/// 调用方自己判断输出里有没有 "ERR" / "OK"。
#[derive(Debug, Serialize)]
pub struct CmdResp {
    pub cmd: String,
    pub output: String,
}

async fn exec_cmd<C>(
    State(st): State<Arc<AppState<C>>>,
    Extension(claims): Extension<ExtKeyClaims>,
    Json(body): Json<CmdBody>,
) -> Result<Response, ApiError>
where
    C: CoreApi + Clone + 'static,
{
    // 所有命令都需要 readwrite（连 help/show 也走 L2 admin 等价 UDS 信任边界）
    require_scope(&claims, ApiKeyScope::Readwrite)?;

    // 防御：去换行 + 限长
    let cleaned: String = body
        .cmd
        .chars()
        .filter(|c| *c != '\n' && *c != '\r')
        .collect();
    if cleaned.is_empty() {
        return Err(ApiError::BadRequest("cmd must be non-empty".into()));
    }
    if cleaned.len() > 4096 {
        return Err(ApiError::BadRequest("cmd too long (max 4096 bytes)".into()));
    }

    let output = st.core.exec_cmd(&cleaned).await?;

    // 返回纯文本（text/plain）而不是 JSON —— 这样外部系统可以直接当 CLI stdout 用
    // 同时也支持 JSON 格式包装：Accept: application/json 时返回 {cmd, output}
    let accept = ""; // 简化：默认 text/plain，所有客户端用文本就好
    let _ = accept;

    Ok(Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "text/plain; charset=utf-8")
        .body(Body::from(output))
        .unwrap())
}

// ======================================================================
// POST /ext/v1/originate  —— 结构化快捷端点（内部调 exec_cmd originate）
// ======================================================================

#[derive(Debug, Deserialize)]
pub struct OriginateBody {
    pub call_url: String,
}

#[derive(Debug, Serialize)]
pub struct OriginateResp {
    pub status: &'static str,
    pub call_url: String,
    pub core_output: String,
}

async fn originate<C>(
    State(st): State<Arc<AppState<C>>>,
    Extension(claims): Extension<ExtKeyClaims>,
    Json(body): Json<OriginateBody>,
) -> Result<impl IntoResponse, ApiError>
where
    C: CoreApi + Clone + 'static,
{
    require_scope(&claims, ApiKeyScope::Readwrite)?;
    if body.call_url.is_empty() || body.call_url.len() > 512 {
        return Err(ApiError::BadRequest("call_url must be 1..512 bytes".into()));
    }
    let cmd = format!("originate {}", body.call_url);
    let output = st.core.exec_cmd(&cmd).await?;
    Ok((StatusCode::OK, Json(OriginateResp {
        status: "originate-dispatched",
        call_url: body.call_url,
        core_output: output,
    })))
}

// ======================================================================
// POST /ext/v1/calls/{id}/hangup  —— 结构化快捷端点（内部调 exec_cmd hangup）
// ======================================================================

async fn hangup<C>(
    State(st): State<Arc<AppState<C>>>,
    Extension(claims): Extension<ExtKeyClaims>,
    Path(call_id): Path<String>,
) -> Result<impl IntoResponse, ApiError>
where
    C: CoreApi + Clone + 'static,
{
    require_scope(&claims, ApiKeyScope::Readwrite)?;
    let cmd = format!("hangup {}", call_id);
    let _output = st.core.exec_cmd(&cmd).await?;
    Ok(StatusCode::OK)
}

// ======================================================================
// GET /ext/v1/calls  —— 结构化快捷端点（内部调 list_calls）
// ======================================================================

async fn list_calls<C>(
    State(st): State<Arc<AppState<C>>>,
    Extension(claims): Extension<ExtKeyClaims>,
) -> Result<impl IntoResponse, ApiError>
where
    C: CoreApi + Clone + 'static,
{
    require_scope(&claims, ApiKeyScope::Readonly)?;
    Ok((StatusCode::OK, Json(st.core.list_calls().await?)))
}

// ======================================================================
// GET /ext/v1/system/status
// ======================================================================

async fn system_status<C>(
    State(st): State<Arc<AppState<C>>>,
    Extension(claims): Extension<ExtKeyClaims>,
) -> Result<impl IntoResponse, ApiError>
where
    C: CoreApi + Clone + 'static,
{
    require_scope(&claims, ApiKeyScope::Readonly)?;
    Ok((StatusCode::OK, Json(st.core.status().await?)))
}

// ======================================================================
// GET /ext/v1/system/version
// ======================================================================

async fn system_version<C>(
    State(st): State<Arc<AppState<C>>>,
    Extension(claims): Extension<ExtKeyClaims>,
) -> Result<impl IntoResponse, ApiError>
where
    C: CoreApi + Clone + 'static,
{
    require_scope(&claims, ApiKeyScope::Readonly)?;
    let core = st.core.version().await?;
    Ok((StatusCode::OK, Json(serde_json::json!({
        "web": env!("CARGO_PKG_VERSION"),
        "core": core.core,
        "api_major": st.api_major,
    }))))
}

// ======================================================================
// GET /ext/v1/events/stream  —— SSE 实时事件流
// ======================================================================

async fn events_stream<C>(
    State(st): State<Arc<AppState<C>>>,
    Extension(claims): Extension<ExtKeyClaims>,
) -> Result<Sse<impl tokio_stream::Stream<Item = Result<Event, Infallible>>>, ApiError>
where
    C: CoreApi + Clone + 'static,
{
    require_scope(&claims, ApiKeyScope::Readwrite)?;

    let rx = st.calls_tx.subscribe();
    let stream = BroadcastStream::new(rx).map(|item| {
        let data = match item {
            Ok(msg) => msg,
            Err(_lagged) => "{\"kind\":\"lag\"}".to_string(),
        };
        Ok::<_, Infallible>(Event::default().data(format!("{data}\n")))
    });

    Ok(Sse::new(stream).keep_alive(KeepAlive::new()))
}

// ======================================================================
// REQ-EXT-API-003: Introspection — 动态自描述端点列表
// ======================================================================

/// 单条端点元数据（由 ext.rs router 注册时确定，docs 端点返回完整清单）
#[derive(Debug, Serialize)]
pub struct EndpointDesc {
    pub method: &'static str,
    pub path: &'static str,
    pub min_scope: &'static str,
    pub desc: &'static str,
}

/// Scope 分级说明（固定元信息，不依赖 core）
#[derive(Debug, Serialize)]
pub struct ScopeDesc {
    pub scope: &'static str,
    pub can: &'static str,
    pub example: &'static str,
}

/// 完整 introspection 响应
#[derive(Debug, Serialize)]
pub struct ApiDocs {
    pub api_major: u32,
    pub web_version: &'static str,
    pub base_path: &'static str,
    pub scopes: &'static [ScopeDesc],
    pub endpoints: &'static [EndpointDesc],
    pub note: &'static str,
}

pub const SCOPES: &[ScopeDesc] = &[
    ScopeDesc { scope: "readonly",  can: "GET /calls, /system/*, /cmd/help, /docs", example: "监控面板 / BI 报表" },
    ScopeDesc { scope: "readwrite", can: "readonly + POST /cmd (任意命令) + /originate + /calls/{id}/hangup + GET /events/stream", example: "呼叫中心 / CRM / AI Agent" },
    ScopeDesc { scope: "admin",     can: "readwrite + Web UI 侧 API Key / Webhook 管理（需 JWT + L2）", example: "系统管理员" },
];

pub const ENDPOINTS: &[EndpointDesc] = &[
    EndpointDesc { method: "POST", path: "/cmd",                 min_scope: "readwrite", desc: "★ 透明代理：任意控制命令直送 nsbcd，返回 text/plain 纯文本 stdout。等价 nsbc_cli / fs_cli 手动 REPL。" },
    EndpointDesc { method: "GET",  path: "/cmd/help",             min_scope: "readonly",  desc: "nsbcd 已注册命令完整列表（name + level + help），JSON 格式，动态从 core 取" },
    EndpointDesc { method: "GET",  path: "/docs",                 min_scope: "readonly",  desc: "本端点 — 完整 introspection 元数据（端点清单 + scope 分级 + 版本）" },
    EndpointDesc { method: "POST", path: "/originate",            min_scope: "readwrite", desc: "结构化快捷端点：发外呼 {\"call_url\": \"user/1000 &echo\"}。内部调 exec_cmd(originate ...)" },
    EndpointDesc { method: "POST", path: "/calls/{id}/hangup",    min_scope: "readwrite", desc: "结构化快捷端点：挂断指定 Call-ID。内部调 exec_cmd(hangup ...)" },
    EndpointDesc { method: "GET",  path: "/calls",                min_scope: "readonly",  desc: "活动呼叫列表（show channels 的 JSON 化）" },
    EndpointDesc { method: "GET",  path: "/system/status",        min_scope: "readonly",  desc: "系统状态快照（stats / show calls / show registrations 综合）" },
    EndpointDesc { method: "GET",  path: "/system/version",       min_scope: "readonly",  desc: "Web + Core 版本" },
    EndpointDesc { method: "GET",  path: "/events/stream",        min_scope: "readwrite", desc: "SSE 实时事件流（CALL_INCOMING / CALL_ANSWERED / CALL_END / CDR）" },
];

/// GET /ext/v1/docs — 公开 introspection（只需要 API Key 鉴权，不需要 readwrite）
/// 注意：docs 端点本身只需要 readonly scope，但所有 /ext/v1/* 端点都走 ext_key middleware
/// 所以必须带 API Key。这是设计上的"零配置 API 发现"—— 拿到 API Key 后先调 /docs
/// 就知道有哪些端点可用。
async fn api_docs<C>(
    State(st): State<Arc<AppState<C>>>,
    Extension(claims): Extension<ExtKeyClaims>,
) -> Result<impl IntoResponse, ApiError>
where
    C: CoreApi + Clone + 'static,
{
    require_scope(&claims, ApiKeyScope::Readonly)?;
    Ok((StatusCode::OK, Json(ApiDocs {
        api_major: st.api_major,
        web_version: env!("CARGO_PKG_VERSION"),
        base_path: "/ext/v1",
        scopes: SCOPES,
        endpoints: ENDPOINTS,
        note: "nsbc-web 是 nsbcd 控制 socket 的 HTTP 透明代理。核心能力 = POST /cmd 任意命令直送 core。",
    })))
}

// ======================================================================
// GET /ext/v1/cmd/help —— nsbcd 已注册命令完整列表
// ======================================================================

#[derive(Debug, Serialize)]
pub struct CmdDescJson {
    pub name: String,
    pub level: u8,       // 1=L1 monitor, 2=L2 admin
    pub help: String,
}

#[derive(Debug, Serialize)]
pub struct CmdHelpResp {
    pub count: usize,
    pub cmds: Vec<CmdDescJson>,
}

/// GET /ext/v1/cmd/help — 动态从 nsbcd 拿 help 输出并解析成 JSON
/// 输出格式来自 cmd_registry.c:101: "%-24s [%c] %s\r\n"
async fn cmd_help<C>(
    State(st): State<Arc<AppState<C>>>,
    Extension(claims): Extension<ExtKeyClaims>,
) -> Result<impl IntoResponse, ApiError>
where
    C: CoreApi + Clone + 'static,
{
    require_scope(&claims, ApiKeyScope::Readonly)?;

    let raw = st.core.exec_cmd("help").await?;
    let mut cmds = Vec::new();

    for line in raw.lines() {
        // 跳过空行和 "nsbcd 2.6.0 control interface" 类 banner
        let line = line.trim();
        if line.is_empty() || !line.contains('[') { continue; }

        // 解析: "%-24s [%c] %s"
        // 即: "<name padding to 24> [1 or 2] <help>"
        // 先用 ' [' 分隔 name 和 rest
        if let Some(bracket_pos) = line.find(" [") {
            let name = line[..bracket_pos].trim().to_string();
            let rest = &line[bracket_pos + 2..]; // 跳过 " ["
            if let Some(level_end) = rest.find(']') {
                let level_char = rest[..level_end].chars().next().unwrap_or('1');
                let help = rest[level_end + 1..].trim().to_string();
                let level: u8 = if level_char == '2' { 2 } else { 1 };
                if !name.is_empty() {
                    cmds.push(CmdDescJson { name, level, help });
                }
            }
        }
    }

    // 排序：先按 name 字母序
    cmds.sort_by(|a, b| a.name.cmp(&b.name));

    Ok((StatusCode::OK, Json(CmdHelpResp {
        count: cmds.len(),
        cmds,
    })))
}
