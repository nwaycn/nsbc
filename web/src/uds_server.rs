//! NSBC UDS Data Service — C 核心→Web 方向 UDS Server（REQ-DATA-SVC-002 P0，统一帧协议）。
//!
//! 帧格式（与 src/include/uds_client.h 完全对齐）：
//!   偏移  长度  字段
//!     0     1    magic         固定 0x01
//!     1     1    frame_type    0x1=HELLO  0x2=HELLO_ACK  0x3=CMD_REQ  0x4=CMD_RSP
//!                              0x5=PUSH    0x6=ACK         0x7=HB       0xF=ERR
//!     2     4    total_len     大端 NBO，整帧字节数（含 12B 固定头）
//!     6     4    ticket        大端 NBO，REQ→RSP 配对；PUSH/HELLO/HB 用 0
//!    10     1    flags         bit0=more_fragment  bit1=compressed  bit2-7=0
//!    11     1    rsvd          置 0
//!    12     N    payload       NBO JSON
//!
//! 固定头 = 12 字节。payload_len = total_len - 12。
//! DATA_PUSH payload: {"kind":"cdr","payload":{...}}
//!                    {"kind":"register","payload":{...}}
//!                    {"kind":"runtime","payload":{...}}
//!                    {"kind":"audit","payload":{...}}
//! CMD_REQ  payload: {"cmd":"ext.ha1","user":"1000","realm":"nsbc.local"}
//!           → CMD_RSP {"ok":true,"ha1":..,"clip":..,"clir":..,"max_contacts":..}
//!                    {"ok":false,"reason":"not_found|disabled|db_error|unknown_cmd"}
//! HELLO（核心启动/重连）→ HELLO_ACK + 触发 [`crate::sync`] 全量配置推送。

use std::sync::Arc;

use crate::AppState;

pub const FRAME_MAGIC: u8 = 0x01;
pub const FRAME_HDR_LEN: usize = 12;

pub const FT_HELLO:       u8 = 0x01;
pub const FT_HELLO_ACK:   u8 = 0x02;
pub const FT_CMD_REQ:     u8 = 0x03;
pub const FT_CMD_RSP:     u8 = 0x04;
pub const FT_DATA_PUSH:   u8 = 0x05;
pub const FT_ACK:         u8 = 0x06;
pub const FT_HEARTBEAT:   u8 = 0x07;
pub const FT_ERROR:       u8 = 0x0F;

const MAX_PAYLOAD: usize = 1_000_000;
const MAX_FRAME:  usize = FRAME_HDR_LEN + MAX_PAYLOAD;

pub fn default_sock_path() -> std::path::PathBuf {
    std::path::PathBuf::from(std::env::var_os("NSBC_DATA_SOCK")
        .unwrap_or_else(|| "/var/run/nsbc/data.sock".into()))
}

#[cfg(unix)]
pub fn start<C: crate::coreclient::CoreApi + Clone + Send + Sync + 'static>(
    state: Arc<AppState<C>>,
    sock_path: std::path::PathBuf,
) -> std::io::Result<tokio::task::JoinHandle<()>> {
    if let Some(parent) = sock_path.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent).ok();
        }
    }
    if sock_path.exists() { let _ = std::fs::remove_file(&sock_path); }
    let listener = tokio::net::UnixListener::bind(&sock_path)?;
    tracing::info!(path = %sock_path.display(), "UDS Data Service bound (unified frame protocol)");

    Ok(tokio::spawn(async move {
        loop {
            let (mut stream, _) = match listener.accept().await {
                Ok(v) => v,
                Err(e) => { tracing::warn!(error = %e, "UDS accept failed"); continue; }
            };
            let state = state.clone();
            tokio::spawn(async move {
                if let Err(e) = handle_conn(&mut stream, &state).await {
                    tracing::warn!(error = %e, "UDS connection error");
                }
            });
        }
    }))
}

#[cfg(not(unix))]
pub fn start<C: crate::coreclient::CoreApi + Clone + Send + Sync + 'static>(
    _state: Arc<AppState<C>>,
    _sock_path: std::path::PathBuf,
) -> std::io::Result<tokio::task::JoinHandle<()>> {
    Ok(tokio::spawn(async {
        tracing::info!("UDS Data Service: not available on this platform (cfg(not(unix)))");
    }))
}

// --- Unix-only ---

/// 连接级循环：decode 帧 → dispatch → 回 ACK，持续读到 EOF/错误。
/// 任何早退（EOF / 错误 / 格式异常）都会走 finally 块把 data_pushed 置 false，
/// 让 poller 恢复轮询兜底（REQ-DATA-002）。
#[cfg(unix)]
async fn handle_conn<C: crate::coreclient::CoreApi + Clone + Send + Sync + 'static>(
    stream: &mut tokio::net::UnixStream,
    state: &Arc<AppState<C>>,
) -> Result<(), Box<dyn std::error::Error>> {
    use std::sync::atomic::Ordering;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    state.data_pushed.store(true, Ordering::Relaxed);
    tracing::debug!("UDS client connected");

    let result: Result<(), Box<dyn std::error::Error>> = async {
        loop {
            let mut hdr = [0u8; FRAME_HDR_LEN];
            match stream.read_exact(&mut hdr).await {
                Ok(_) => {}
                Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(()),
                Err(e) => return Err(e.into()),
            }

            if hdr[0] != FRAME_MAGIC {
                let _ = stream.write_all(&bad_frame_ack(0, "invalid magic".as_bytes())).await;
                return Ok(());
            }

            let frame_type = hdr[1];
            let total_len = u32::from_be_bytes([hdr[2], hdr[3], hdr[4], hdr[5]]) as usize;
            let ticket    = u32::from_be_bytes([hdr[6], hdr[7], hdr[8], hdr[9]]);

            if total_len < FRAME_HDR_LEN || total_len > MAX_FRAME {
                let _ = stream.write_all(&bad_frame_ack(ticket, "frame too large or malformed".as_bytes())).await;
                return Ok(());
            }

            let payload_len = total_len - FRAME_HDR_LEN;
            let mut payload_buf = vec![0u8; payload_len];
            if payload_len > 0 {
                stream.read_exact(&mut payload_buf).await?;
            }
            let payload_str = String::from_utf8_lossy(&payload_buf).to_string();

            let resp: Option<(u8, String)> = match frame_type {
                FT_DATA_PUSH => match dispatch_push(&payload_str, state).await {
                    Ok(body) => Some((FT_ACK, body)),
                    Err(e) => Some((FT_ACK, serde_json::json!({"ok": false, "error": e}).to_string())),
                },
                FT_CMD_REQ => match dispatch_cmd_req(&payload_str, state).await {
                    Ok(body) => Some((FT_CMD_RSP, body)),
                    Err(e) => Some((FT_CMD_RSP, serde_json::json!({"ok": false, "reason": "bad_request", "error": e}).to_string())),
                },
                FT_HELLO => {
                    crate::sync::on_hello(state.clone());
                    Some((FT_HELLO_ACK, serde_json::json!({"ok": true}).to_string()))
                }
                FT_HEARTBEAT => Some((FT_ACK, serde_json::json!({"ok": true}).to_string())),
                FT_ERROR => {
                    tracing::warn!(payload = %payload_str, "C side UDS error frame");
                    None
                }
                FT_HELLO_ACK => None,
                other => {
                    tracing::warn!(frame_type = other, "unknown UDS frame type");
                    Some((FT_ACK, serde_json::json!({"ok": false, "error": format!("unknown frame_type=0x{other:02x}")}).to_string()))
                }
            };

            if let Some((ft, body)) = resp {
                if !body.is_empty() {
                    let frame = build_frame(ft, ticket, &body);
                    let _ = stream.write_all(&frame).await;
                    let _ = stream.flush().await;
                }
            }
        }
    }.await;

    // REQ-DATA-002：UDS 断链 → data_pushed 置回 false，poller 恢复轮询兜底。
    // 多连接场景：任一 handle_conn 退出都置 false——只要有一个活着，下一个 accept
    // 就会重新置 true；短暂 false 只会让 poller 多跑几轮，不影响正确性。
    state.data_pushed.store(false, Ordering::Relaxed);
    tracing::debug!(result_ok = result.is_ok(), "UDS client disconnected, poller re-enabled");

    result
}

/// 组帧：12B 固定头（type + ticket）+ payload。
fn build_frame(frame_type: u8, ticket: u32, payload: &str) -> Vec<u8> {
    let total_len = (FRAME_HDR_LEN + payload.len()) as u32;
    let mut out = Vec::with_capacity(total_len as usize);
    out.push(FRAME_MAGIC);
    out.push(frame_type);
    out.extend_from_slice(&total_len.to_be_bytes());
    out.extend_from_slice(&ticket.to_be_bytes());
    out.push(0); // flags
    out.push(0); // rsvd
    out.extend_from_slice(payload.as_bytes());
    out
}

fn bad_frame_ack(ticket: u32, msg: &[u8]) -> Vec<u8> {
    let payload_len = msg.len();
    let total_len = (FRAME_HDR_LEN + payload_len) as u32;
    let mut out = Vec::with_capacity(total_len as usize);
    out.push(FRAME_MAGIC);
    out.push(FT_ACK);
    out.extend_from_slice(&total_len.to_be_bytes());
    out.extend_from_slice(&ticket.to_be_bytes());
    out.push(0);
    out.push(0);
    out.extend_from_slice(msg);
    out
}

/// DATA_PUSH / CMD_REQ payload 分发。payload = {"kind":"...", "payload":...}
async fn dispatch_push<C: crate::coreclient::CoreApi + Clone + Send + Sync + 'static>(
    payload: &str,
    state: &Arc<AppState<C>>,
) -> Result<String, String> {
    let root: serde_json::Value = serde_json::from_str(payload)
        .map_err(|e| format!("invalid frame JSON: {e}"))?;
    let kind = root.get("kind").and_then(|x| x.as_str()).unwrap_or("");
    let body = root.get("payload").cloned().unwrap_or(serde_json::Value::Null);

    if state.db.pool().is_none() {
        return Err("DB not configured".into());
    }
    tracing::debug!(kind, "UDS frame received, payload_len={}", payload.len());

    let ok = match kind {
        "cdr" => write_cdr(&state.db, &body).await,
        "register" => upsert_registration(&state.db, &body).await,
        "runtime" => insert_runtime(&state.db, &body).await,
        "audit" => insert_audit(&state.db, &body).await,
        "session" => {
            let r = dispatch_session_push(&state.db, &body).await;
            if r.is_ok() {
                // nsbc-esl-web §8#1：SESSION 事件 fan-out 给 /calls/stream（SSE）
                // 订阅者。send 仅在无订阅者时返回 Err（忽略）；环形缓冲满则丢最旧，
                // 绝不阻塞——保持 UDS dispatch 热路径无反压（项目硬约束）。
                let evt = serde_json::json!({"kind": "session", "payload": body}).to_string();
                let _ = state.calls_tx.send(evt);
            }
            r
        }
        "vad_event" => {
            // REQ-AI-BARGEIN-001：VAD 状态变化（SILENCE↔SPEECH）fan-out 给 ai_agent
            // 模块做 barge-in 决策。payload = {event:"VAD_EVENT",call_key:"0x...",
            // leg:0|1, state:"SPEECH|SILENCE", rms:N, ts_ns:N}。
            let evt = serde_json::json!({"kind": "vad_event", "payload": body}).to_string();
            let _ = state.vad_tx.send(evt);
            // VAD 事件不写 DB（高频、瞬时状态，CDR 已盖最终结果），直接返回 Ok
            Ok(())
        },
        "" => return Err("missing kind".into()),
        _ => return Err(format!("unknown kind={kind}")),
    };

    match ok {
        Ok(()) => Ok(serde_json::json!({"ok": true, "kind": kind}).to_string()),
        Err(e) => Err(e),
    }
}

/// CMD_REQ payload 分发：C 核心实时查询（REQ-DATA-SVC-003 混合式兜底）。
///
/// payload = {"cmd":"ext.ha1","user":"1000","realm":"nsbc.local"}，
/// 用于 REGISTER 认证时 C 侧内存表未命中 → 经 UDS 查 Web DB。
/// 回复永远 Ok(body)（错误编码进 reason），Err 仅限 JSON 解析失败。
/// reason 语义（与 C 侧负缓存 TTL 约定）：
///   not_found / disabled → 负缓存 30s；db_error → 负缓存 3s
async fn dispatch_cmd_req<C: crate::coreclient::CoreApi + Clone + Send + Sync + 'static>(
    payload: &str,
    state: &Arc<AppState<C>>,
) -> Result<String, String> {
    let root: serde_json::Value = serde_json::from_str(payload)
        .map_err(|e| format!("invalid frame JSON: {e}"))?;
    let cmd   = root.get("cmd").and_then(|x| x.as_str()).unwrap_or("");
    let user  = root.get("user").and_then(|x| x.as_str()).unwrap_or("");
    let realm = root.get("realm").and_then(|x| x.as_str()).unwrap_or("");

    match cmd {
        "ext.ha1" => {
            if user.is_empty() {
                return Ok(serde_json::json!({"ok": false, "reason": "bad_request"}).to_string());
            }
            if state.db.pool().is_none() {
                return Ok(serde_json::json!({"ok": false, "reason": "db_error"}).to_string());
            }
            let row = match state.db.get_extension_by_eaor(user, realm).await {
                Ok(v) => v,
                Err(e) => {
                    tracing::warn!(user, realm, error = %e, "ext.ha1 db query failed");
                    return Ok(serde_json::json!({"ok": false, "reason": "db_error"}).to_string());
                }
            };
            let Some(x) = row else {
                return Ok(serde_json::json!({"ok": false, "reason": "not_found"}).to_string());
            };
            if x.enabled == 0 {
                return Ok(serde_json::json!({"ok": false, "reason": "disabled"}).to_string());
            }
            Ok(serde_json::json!({
                "ok": true,
                "ha1": x.ha1,
                "clip": x.clip,
                "clir": x.clir != 0,
                "max_contacts": x.max_contacts,
            }).to_string())
        }
        "" => Ok(serde_json::json!({"ok": false, "reason": "bad_request"}).to_string()),
        _ => {
            tracing::warn!(cmd, "unknown CMD_REQ");
            Ok(serde_json::json!({"ok": false, "reason": "unknown_cmd"}).to_string())
        }
    }
}

// --- DB helpers ---

async fn write_cdr(db: &crate::db::Db, v: &serde_json::Value) -> Result<(), String> {
    let call_id = v.get("call_id").and_then(|x| x.as_str()).unwrap_or("");
    if call_id.is_empty() {
        return Err("missing call_id".into());
    }
    let req = |k: &str| v.get(k).and_then(|x| x.as_str()).unwrap_or("").to_string();
    let ms = |k: &str| v.get(k).and_then(|x| x.as_u64()).unwrap_or(0);
    let num = |k: &str| v.get(k).and_then(|x| x.as_i64()).unwrap_or(0) as u32;
    // codec / IP 扩展字段（C 侧 CDR END 完整填充；早期 START/ANSWER 事件可能空）
    let from_ip   = opt_str(v, "from_ip");
    let codec_a   = opt_str(v, "codec_a");
    let codec_b   = opt_str(v, "codec_b");
    let a_peer_ip = opt_str(v, "a_peer_ip");
    let b_peer_ip = opt_str(v, "b_peer_ip");
    db.insert_cdr(
        call_id,
        opt_str(v, "tenant").as_deref(),
        &req("from"),
        &req("to"),
        opt_str(v, "trunk").as_deref(),
        opt_str(v, "rec_path").as_deref(),
        &req("dir"),
        &req("phase"),
        &req("disposition"),
        ms("start_ms"),
        ms("answer_ms"),
        ms("end_ms"),
        num("billsec"),
        num("duration_s"),
        v.get("cause").and_then(|x| x.as_i64()).unwrap_or(0) as i32,
        v.get("emergency").and_then(|x| x.as_bool()).unwrap_or(false),
        from_ip.as_deref(),
        codec_a.as_deref(),
        codec_b.as_deref(),
        a_peer_ip.as_deref(),
        b_peer_ip.as_deref(),
    )
    .await
    .map_err(|e| e.to_string())
}

async fn upsert_registration(db: &crate::db::Db, v: &serde_json::Value) -> Result<(), String> {
    let user = v.get("user").and_then(|x| x.as_str()).unwrap_or("");
    if user.is_empty() {
        return Err("missing user".into());
    }
    let contact = opt_str(v, "contact");
    let instance_id = v.get("instance_id").and_then(|x| x.as_str()).unwrap_or("");
    let agent = opt_str(v, "agent");
    let expires_sec = v.get("expires_sec").and_then(|x| x.as_u64()).unwrap_or(0) as u32;
    // NAT 元数据（REQ-NAT-001）：C 侧 registrar.c 推送
    let received_ip    = opt_str(v, "received_ip");
    let received_port  = v.get("received_port").and_then(|x| x.as_i64());
    let nat_detected   = v.get("nat_detected").and_then(|x| x.as_i64());
    let public_contact = opt_str(v, "public_contact");
    let transport      = opt_str(v, "transport");
    db.upsert_registration(
        user,
        contact.as_deref(),
        instance_id,
        agent.as_deref(),
        expires_sec,
        received_ip.as_deref(),
        received_port,
        nat_detected,
        public_contact.as_deref(),
        transport.as_deref(),
    ).await.map_err(|e| e.to_string())
}

async fn insert_runtime(db: &crate::db::Db, v: &serde_json::Value) -> Result<(), String> {
    let call_id = v.get("call_id").and_then(|x| x.as_str()).unwrap_or("");
    let state_name = v.get("state").and_then(|x| x.as_str()).unwrap_or("");
    if call_id.is_empty() || state_name.is_empty() {
        return Err("missing call_id/state".into());
    }
    let detail = opt_str(v, "detail");
    db.insert_runtime_state(call_id, state_name, detail.as_deref())
        .await
        .map_err(|e| e.to_string())
}

async fn insert_audit(db: &crate::db::Db, v: &serde_json::Value) -> Result<(), String> {
    let event = v.get("event").and_then(|x| x.as_str()).unwrap_or("");
    if event.is_empty() {
        return Err("missing event".into());
    }
    let actor = opt_str(v, "actor");
    let ip = opt_str(v, "ip");
    let target = opt_str(v, "target");
    let status = opt_str(v, "status");
    let detail = opt_str(v, "detail");
    db.insert_audit_event(event, actor.as_deref(), ip.as_deref(), target.as_deref(), status.as_deref(), detail.as_deref())
        .await
        .map_err(|e| e.to_string())
}

/// session.write：C 侧 data.sock 推送的 SESSION 事件 → calls 表 UPSERT。
/// payload 字段：call_id(必填) / event / state / from / to / cause / dir
///
/// 空串保护（REQUIRE-DATA-001）：C 侧有些事件（record_start/stop、dtmf、originate
/// 路早期版本）payload 缺 state/event/dir 字段，若直接把空串传给 upsert_call 会
/// 把 calls 表已有 state/event 硬覆盖成空（from/to 已有空串保护但 state/event
/// 没有）。此处统一：空串字段 → 不更新，沿用 calls 表已有值。dir 默认 inbound
/// 仍保留（originate 路 payload 总是显式给 "outbound"，入向总是显式 "inbound"，
/// 默认值仅兜底）。
async fn dispatch_session_push(db: &crate::db::Db, v: &serde_json::Value) -> Result<(), String> {
    let call_id = v.get("call_id").and_then(|x| x.as_str()).unwrap_or("");
    if call_id.is_empty() {
        return Err("missing call_id".into());
    }
    // 空串 → 特殊哨兵 "''"（SQL CASE 里匹配空串），非空 → 原值
    let event = opt_str(v, "event").unwrap_or_default();
    let state = opt_str(v, "state").unwrap_or_default();
    let from  = opt_str(v, "from").unwrap_or_default();
    let to    = opt_str(v, "to").unwrap_or_default();
    let cause = v.get("cause").and_then(|x| x.as_i64()).unwrap_or(0) as i32;
    let dir   = opt_str(v, "dir").unwrap_or_else(|| "inbound".to_string());
    // REQ-CDR-002 扩展字段：from_ip / codec_a / codec_b / a_peer_ip / b_peer_ip
    // C 侧 CDR END 链路（signaling.c + cdr_event_to_json）完整填充；
    // START/ANSWER 早期事件可能为 empty string → SQL CASE WHEN 保留已有值。
    let from_ip   = opt_str(v, "from_ip").unwrap_or_default();
    let codec_a   = opt_str(v, "codec_a").unwrap_or_default();
    let codec_b   = opt_str(v, "codec_b").unwrap_or_default();
    let a_peer_ip = opt_str(v, "a_peer_ip").unwrap_or_default();
    let b_peer_ip = opt_str(v, "b_peer_ip").unwrap_or_default();
    db.upsert_call(call_id, &from, &to, &dir, &state, &event, cause,
                   &from_ip, &codec_a, &codec_b, &a_peer_ip, &b_peer_ip)
        .await
        .map_err(|e| e.to_string())
}

fn opt_str(v: &serde_json::Value, key: &str) -> Option<String> {
    v.get(key)
        .and_then(|x| x.as_str())
        .filter(|s| !s.is_empty())
        .map(|s| s.to_string())
}
