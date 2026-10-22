//! RBAC 角色映射（REQ-WEB-008 / REQ-OAM-008）。
//!
//! 三级特权，与核心命令注册表逐命令标注的级别同口径：
//!   L0 只读：查看配置/通话/指标/告警
//!   L1 操作：reload、挂断、录音触发（参数快照+审计）
//!   L2 高危：改监听/证书/ACL、重启 → 必须二次授权（P1：短时效 Token
//!           或双人确认，REQ-OAM-008）
//!
//! W3a 起身份为登录签发的 HS256 JWT（access 令牌，见 crate::auth）：
//! 校验签名/类型/过期/吊销表后，把 Identity 注入 request extensions。
//! 开发令牌表（NSBC_DEV_TOKENS）已删除；mTLS 证书 CN/O 映射为后续联调项。

use std::sync::Arc;

use axum::extract::{Request, State};
use axum::http::{header, Method};
use axum::middleware::Next;
use axum::response::Response;

use serde_json::json;

use crate::auth::tokens::{now_secs, TokenKind};
use crate::auth::{Identity, PeerCtx, Role};
use crate::coreclient::CoreApi;
use crate::error::ApiError;
use crate::AppState;

/// 按请求方法+路径声明所需级别（P0 最小集合；完整表由核心命令注册表下发）。
fn required_level(method: &Method, path: &str) -> Role {
    let item = path.trim_start_matches('/').trim_end_matches('/');
    let action = item.rsplit('/').next().unwrap_or(item);
    // GET 末段是资源字面量而非固定动作的只读嵌套路由（白名单式枚举，
    // 禁止裸前缀放开）：/calls/{id}（uuid_dump 诊断）、/config/{key}
    // （config.get 生效值）。仅 GET + 单段命中；POST .../hangup 方法不同
    // 天然不受影响，多段路径落默认并由路由层 405。
    let get_nested_readonly = method == Method::GET
        && ["calls/", "config/"].into_iter().any(|p| {
            item.strip_prefix(p)
                .is_some_and(|rest| !rest.is_empty() && !rest.contains('/'))
        });
    match (method, action) {
        // 只读 L0：嵌套只读资源（优先于 action 白名单，末段为字面量）
        _ if get_nested_readonly => Role::L0,
        // 只读 L0
        (
            &Method::GET,
            "version" | "config" | "listens" | "calls" | "channels"
            | "registrations" | "metrics" | "cdrs" | "recordings" | "bans"
            | "blacklist",
        ) => Role::L0,
        // 写操作 dry-run 仍属只读语义（不改变核心状态）
        (&Method::POST, "validate") => Role::L0,
        // 高危 L2（真正的整资源级危险操作）
        (&Method::PUT | &Method::PATCH | &Method::DELETE, "config")
        | (&Method::POST, "restart") => Role::L2,
        // 操作类 L1；未显式标注的写操作默认 L1（不默认 L0）
        _ => Role::L1,
    }
}

pub async fn require_auth<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    mut req: Request,
    next: Next,
) -> Result<Response, ApiError> {
    let token = req
        .headers()
        .get(header::AUTHORIZATION)
        .and_then(|v| v.to_str().ok())
        .and_then(|s| s.strip_prefix("Bearer "))
        .ok_or(ApiError::Unauthenticated)?;
    if token.len() > 4096 {
        return Err(ApiError::TokenInvalid);
    }
    let now = now_secs();
    let claims = st
        .auth
        .tokens
        .verify(token, TokenKind::Access, now)?;
    let role = Role::from_u8(claims.role).ok_or(ApiError::TokenInvalid)?;
    if st.auth.sessions.access_revoked(&claims.jti, now) {
        return Err(ApiError::TokenInvalid);
    }

    let need = required_level(req.method(), req.uri().path());
    if role < need {
        return Err(ApiError::Forbidden {
            required: match need {
                Role::L0 => "L0",
                Role::L1 => "L1",
                Role::L2 => "L2",
            },
        });
    }
    // REQ-WEB-010：写操作（L1/L2，即非安全方法）在 Web 侧记独立审计，
    // 含操作人/来源/方法/路径/响应状态；参数快照在各 handler 细化（后续）。
    // 核心侧对到达控制面的命令另有 log_audit（双层独立）。
    let auditable = !matches!(req.method(), &Method::GET | &Method::HEAD | &Method::OPTIONS);
    let peer = req
        .extensions()
        .get::<PeerCtx>()
        .cloned()
        .unwrap_or_default();
    let sub = claims.sub.clone();
    let method = req.method().to_string();
    let path = req.uri().path().to_string();
    req.extensions_mut().insert(Identity(claims));
    let resp = next.run(req).await;
    if auditable {
        st.auth.audit.record(
            "api_write",
            Some(&sub),
            &peer,
            json!({"method": method, "path": path, "status": resp.status().as_u16(),
                  "required_level": need.as_str()}),
        );
    }
    Ok(resp)
}
