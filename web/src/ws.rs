//! REQ-AI-WS-001: Self-implemented WebSocket（RFC 6455 子集，零新 crate）
//!
//! 为什么 self-implemented：vendor/ 没有 tungstenite/tokio-tungstenite；
//! nsbc-web 离线构建（vendor/ + replace-with），加新 crate 需联网 vendoring。
//! 本实现只依赖：ring（SHA1）+ base64 0.22 + tokio + axum + hyper 1.0 + hyper_util 0.1（全已有）。
//!
//! 协议子集（AI 媒体流最小集）：
//!   - Binary frame (opcode 0x2)：PCMU 8kHz 原始字节，零 base64 开销
//!   - Ping (0x9) / Pong (0xA)：心跳，30s 间隔
//!   - Close (0x8)：优雅断开
//!   - Server→Client 帧 MASK=0；Client→Server 帧 MASK=1（RFC 6455 要求）
//!
//! 端点：GET /ws/ai-media/:call_id?token=<ext_key>
//!   - Admin scope 鉴权（token query 验证 ext_key）
//!   - Binary frame 双向桥接 WS 客户端 ↔ nsbc-web ↔ nsbcd UDS ↔ ring buffer/A腿

use crate::AppState;
use crate::coreclient::CoreApi;
use crate::error::ApiError;
use crate::middleware::ext_key::{ApiKeyScope, hash_key};
use axum::{
    body::Body,
    extract::{Path, Query, State},
    http::{header, Request, StatusCode},
    response::Response,
    routing::get,
    Router,
};
use base64::{Engine as _, engine::general_purpose::STANDARD as B64};
// hyper::upgrade 由 upgrade::on 内部使用，OnUpgrade 不需要显式 import
use ring::digest::{Context, SHA1_FOR_LEGACY_USE_ONLY as SHA1};
use serde::Deserialize;
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::time::Duration;

// =========================================================================
// WebSocket 帧编解码（RFC 6455 §5.2）
// =========================================================================

#[repr(u8)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum Opcode {
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
}

/// SimpleIo：hyper_util::rt::TokioIo<Upgraded> — hyper upgrade 拿到的 Upgraded 实现 hyper rt Read/Write，
/// TokioIo 包装后就有了 tokio AsyncRead/AsyncWrite
type SimpleIo = hyper_util::rt::TokioIo<hyper::upgrade::Upgraded>;

struct WsFrame {
    opcode: Opcode,
    payload: Vec<u8>,
}

/// Server→Client 帧（MASK=0）
async fn ws_write(io: &mut SimpleIo, opcode: Opcode, payload: &[u8]) -> std::io::Result<()> {
    let mut buf: Vec<u8> = Vec::with_capacity(14 + payload.len());
    buf.extend_from_slice(&[0x80u8 | (opcode as u8)]);  // FIN + opcode

    let len = payload.len();
    if len < 126 {
        buf.extend_from_slice(&[len as u8]);
    } else if len < 65536 {
        buf.extend_from_slice(&[126]);
        buf.extend_from_slice(&(len as u16).to_be_bytes());
    } else {
        buf.extend_from_slice(&[127]);
        buf.extend_from_slice(&(len as u64).to_be_bytes());
    }

    io.write_all(&buf).await?;
    if !payload.is_empty() {
        io.write_all(payload).await?;
    }
    io.flush().await?;
    Ok(())
}

/// Client→Server 帧（必须 MASK=1，RFC 6455 §5.1）
async fn ws_read(io: &mut SimpleIo) -> std::io::Result<WsFrame> {
    let mut hdr = [0u8; 2];
    io.read_exact(&mut hdr).await?;

    let opcode = match hdr[0] & 0x0F {
        0x2 => Opcode::Binary,
        0x8 => Opcode::Close,
        0x9 => Opcode::Ping,
        0xA => Opcode::Pong,
        _ => return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "bad opcode")),
    };
    let masked = (hdr[1] & 0x80) != 0;
    let mut len = (hdr[1] & 0x7F) as usize;

    if len == 126 {
        let mut lb = [0u8; 2];
        io.read_exact(&mut lb).await?;
        len = u16::from_be_bytes(lb) as usize;
    } else if len == 127 {
        let mut lb = [0u8; 8];
        io.read_exact(&mut lb).await?;
        len = u64::from_be_bytes(lb) as usize;
    }
    if len > 256 * 1024 {
        return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "frame too large"));
    }

    let mut mask = [0u8; 4];
    if masked {
        io.read_exact(&mut mask).await?;
    }

    let mut payload = vec![0u8; len];
    if len > 0 {
        io.read_exact(&mut payload).await?;
        if masked {
            for i in 0..len { payload[i] ^= mask[i % 4]; }
        }
    }
    Ok(WsFrame { opcode, payload })
}

// =========================================================================
// HTTP Upgrade → WebSocket 握手
// =========================================================================

const WS_GUID: &[u8] = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

fn compute_accept(key: &str) -> String {
    let mut ctx = Context::new(&SHA1);
    ctx.update(key.as_bytes());
    ctx.update(WS_GUID);
    B64.encode(ctx.finish().as_ref())
}

// =========================================================================
// 路由
// =========================================================================

pub fn router<C: CoreApi + Clone + 'static>(st: Arc<AppState<C>>) -> Router {
    Router::new()
        .route("/ai-media/:call_id", get(ws_ai_media::<C>))
        .with_state(st)
}

#[derive(Debug, Deserialize)]
struct WsQuery { token: String }

// =========================================================================
// GET /ws/ai-media/:call_id?token=<ext_key>
// =========================================================================

async fn ws_ai_media<C>(
    State(st): State<Arc<AppState<C>>>,
    Path(call_id): Path<String>,
    Query(q): Query<WsQuery>,
    req: Request<Body>,
) -> Result<Response, ApiError>
where C: CoreApi + Clone + 'static
{
    // 1. Admin scope 鉴权：复用 ext_key middleware 逻辑
    let db = st.db.pool().ok_or(ApiError::ServiceUnavailable("API key store".into()))?;
    let key_hash = hash_key(&q.token);
    let row: Option<(String, String)> = sqlx::query_as(
        "SELECT label, scope FROM ext_api_keys WHERE key_hash = ? AND revoked_at IS NULL LIMIT 1",
    ).bind(&key_hash).fetch_optional(&*db).await
        .map_err(|e| ApiError::Db(format!("key lookup: {}", e)))?;
    let (label, scope_str) = row.ok_or(ApiError::Unauthenticated)?;
    let scope = match scope_str.as_str() {
        "admin" => ApiKeyScope::Admin,
        _ => return Err(ApiError::Forbidden { required: "Admin scope" }),
    };
    let _ = label;  // DB 里的 key label — 审计用
    let _ = scope;  // 显式 admin scope 已在 match 里确认过

    // 1b. call_id 校验
    if call_id.is_empty() || call_id.len() > 128 {
        return Err(ApiError::BadRequest("bad call_id".to_string()));
    }

    // 2. HTTP Upgrade 握手
    let hyper_req: hyper::Request<Body> = req.into();
    // hyper::upgrade::on() 需要 consume Request（move，不是 &）
    // 所以先把 WS 握手所需的 header 取出来
    let key = hyper_req.headers().get("sec-websocket-key")
        .ok_or_else(|| ApiError::BadRequest("missing Sec-WebSocket-Key".to_string()))?
        .to_str().map_err(|_| ApiError::BadRequest("bad key".to_string()))?
        .to_string();
    let accept = compute_accept(&key);
    let on_upgrade = hyper::upgrade::on(hyper_req);  // move 进去

    // 返回 101 —— hyper 看到 101 + Upgrade header 会自动处理 upgrade
    let resp = Response::builder()
        .status(StatusCode::SWITCHING_PROTOCOLS)
        .header(header::UPGRADE, "websocket")
        .header(header::CONNECTION, "Upgrade")
        .header("Sec-WebSocket-Accept", accept)
        .body(Body::empty())
        .map_err(|e| ApiError::Db(format!("build ws resp: {}", e)))?;

    // 3. spawn bridge task — await upgrade → TokioIo<Upgraded> → 桥接 WS ↔ UDS
    let state_clone = st.clone();
    let cid = call_id.clone();
    tokio::spawn(async move {
        match tokio::time::timeout(Duration::from_secs(30), on_upgrade).await {
            Ok(Ok(upgraded)) => {
                // Upgraded 实现 hyper::rt::Read + Write；用 TokioIo 包装成 tokio AsyncRead/Write
                let mut io = hyper_util::rt::TokioIo::new(upgraded);
                tracing::info!(ws_call_id = %cid, "WS ai-media connected");
                ws_bridge_async(&mut io, &state_clone, &cid).await;
                tracing::info!(ws_call_id = %cid, "WS ai-media disconnected");
            }
            Ok(Err(e)) => tracing::warn!("WS upgrade err: {}", e),
            Err(_) => tracing::warn!("WS upgrade timeout"),
        }
    });

    Ok(resp)
}

// =========================================================================
// ws_bridge_async —— 完整 WS ↔ UDS 双向桥接（串行单 task，避免 tokio::net 并发 mut）
//
// Heartbeat 策略（REQ-AI-WS-001 补强）：
//   服务端→客户端 ping 间隔 30s；发出 ping 后 10s 内没收到任何客户端帧（pong/binary/close）→ 主动断开
//   僵尸检测：如果 120s 没收到来自客户端的任何数据（包括 pong）→ 连接已死 → 断开
//   客户端→服务端数据帧（binary / pong）都会刷新 last_client_activity，天然解决 pong 帧偶尔丢的场景
// =========================================================================

const WS_PING_INTERVAL: Duration = Duration::from_secs(30);
const WS_PONG_TIMEOUT:   Duration = Duration::from_secs(10);   // ping 发出后等 pong
const WS_ZOMBIE_TIMEOUT: Duration = Duration::from_secs(120);  // 客户端最长静默时间

async fn ws_bridge_async<C: CoreApi + Clone + 'static>(
    io: &mut SimpleIo,
    state: &Arc<AppState<C>>,
    call_id: &str,
) {
    // 1. producer=1（让 nsbcd 停内建 ASR/LLM/TTS + 自动启 VAD）
    let _ = state.core.exec_cmd(&format!("ai-set-producer {} 1", call_id)).await;

    let now = tokio::time::Instant::now();
    let mut last_ping_sent   = now - WS_PING_INTERVAL;  // 立刻允许发第一个 ping
    let mut last_client_activity = now;                  // 最近收到客户端任何帧的时间
    let mut pending_ping = false;                         // 是否有一个 ping 还没等到 pong
    let mut disconnected = false;

    // 首个空 binary frame 测试链路
    if ws_write(io, Opcode::Binary, b"").await.is_err() {
        tracing::warn!(ws_call_id = %call_id, "WS initial probe failed");
        disconnected = true;
    }

    while !disconnected {
        let now = tokio::time::Instant::now();

        // === A: 读 WS frame（带 timeout，避免阻塞 UDS 50ms 轮询）===
        match tokio::time::timeout(Duration::from_millis(50), ws_read(io)).await {
            Ok(Ok(frame)) => {
                // 任何来自客户端的合法帧都刷新僵尸计时器
                last_client_activity = now;
                if pending_ping {
                    pending_ping = false;   // 收到客户端帧就认为 ping 已被响应（可能客户端回了 binary 而不是 pong）
                }

                match frame.opcode {
                    Opcode::Binary => {
                        // WS→UDS：外部 AI 推的 PCMU → base64 → ai-send-tx
                        let b64 = B64.encode(&frame.payload);
                        let _ = state.core.exec_cmd(
                            &format!("ai-send-tx {} 0 {}", call_id, b64),
                        ).await;
                    }
                    Opcode::Ping => { let _ = ws_write(io, Opcode::Pong, &frame.payload).await; }
                    Opcode::Close => {
                        let _ = ws_write(io, Opcode::Close, &[]).await;
                        disconnected = true;
                    }
                    Opcode::Pong => { /* 我方 ping 的响应 */ }
                }
            }
            Ok(Err(e)) => { tracing::warn!("WS read err: {}", e); break; }
            Err(_) => { /* 50ms timeout — 继续下面的检查 */ }
        }

        if disconnected { break; }

        // === B: heartbeat 三重检查 ===

        // B1. 僵尸检测：120s 没收到任何客户端帧 → 断开
        if now.duration_since(last_client_activity) > WS_ZOMBIE_TIMEOUT {
            tracing::warn!(ws_call_id = %call_id, "WS zombie: {}s no client data → disconnect",
                           WS_ZOMBIE_TIMEOUT.as_secs());
            break;
        }

        // B2. 已发出 ping 但 10s 内没收到任何帧 → pong 超时 → 断开
        if pending_ping && now.duration_since(last_ping_sent) > WS_PONG_TIMEOUT {
            tracing::warn!(ws_call_id = %call_id, "WS pong timeout: {}s → disconnect",
                           WS_PONG_TIMEOUT.as_secs());
            break;
        }

        // B3. 30s 发一次 ping（只有没有 pending ping 时才发，避免堆积）
        if !pending_ping && now.duration_since(last_ping_sent) > WS_PING_INTERVAL {
            if ws_write(io, Opcode::Ping, b"ping").await.is_err() { break; }
            last_ping_sent = now;
            pending_ping = true;
        }

        // === C: UDS→WS：ai-get-rx 50ms 轮询 → binary frame ===
        match state.core.exec_cmd(&format!("ai-get-rx {} 4096", call_id)).await {
            Ok(output) => {
                for line in output.lines() {
                    if let Some(b64) = line.strip_prefix("b64:") {
                        let b64 = b64.trim();
                        if !b64.is_empty() {
                            if let Ok(pcmu) = B64.decode(b64.as_bytes()) {
                                if !pcmu.is_empty() {
                                    if ws_write(io, Opcode::Binary, &pcmu).await.is_err() {
                                        disconnected = true;
                                        break;
                                    }
                                }
                            }
                        }
                        break;
                    }
                }
            }
            Err(_) => { /* nsbcd 暂时不可用 — 忽略一次 */ }
        }
    }

    // 清理：producer=0 切回 demo 模式（幂等，Close 帧处理里也会走到这里）
    let _ = state.core.exec_cmd(&format!("ai-set-producer {} 0", call_id)).await;
}
