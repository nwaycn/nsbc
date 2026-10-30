//! NSBC UDS Data Service — C core→Web direction UDS Server (REQ-DATA-SVC-002 P0, unified
//! frame protocol).
//!
//! Frame format (fully aligned with src/include/uds_client.h):
//!   Offset  Length  Field
//!     0       1    magic         fixed 0x01
//!     1       1    frame_type    0x1=HELLO  0x2=HELLO_ACK  0x3=CMD_REQ  0x4=CMD_RSP
//!                                0x5=PUSH    0x6=ACK         0x7=HB        0xF=ERR
//!     2       4    total_len     big-endian NBO, whole-frame byte count (incl. 12B fixed hdr)
//!     6       4    ticket        big-endian NBO, REQ→RSP pairing; 0 for PUSH/HELLO/HB
//!    10       1    flags         bit0=more_fragment  bit1=compressed  bit2-7=0
//!    11       1    rsvd          set to 0
//!    12       N    payload       NBO JSON
//!
//! Fixed header = 12 bytes. payload_len = total_len - 12.
//! DATA_PUSH payload: {"kind":"cdr","payload":{...}}
//!                    {"kind":"register","payload":{...}}
//!                    {"kind":"runtime","payload":{...}}
//!                    {"kind":"audit","payload":{...}}
//! CMD_REQ  payload: {"cmd":"ext.ha1","user":"1000","realm":"nsbc.local"}
//!           → CMD_RSP {"ok":true,"ha1":..,"clip":..,"clir":..,"max_contacts":..}
//!                    {"ok":false,"reason":"not_found|disabled|db_error|unknown_cmd"}
//! HELLO (core start/reconnect) → HELLO_ACK + trigger full config push via
//! [`crate::sync`].

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
            if let Err(e) = std::fs::create_dir_all(parent) {
                eprintln!("warning: create_dir_all {}: {e}", parent.display());
            }
        }
    }
    if sock_path.exists() { let _ = std::fs::remove_file(&sock_path); }
    let listener = tokio::net::UnixListener::bind(&sock_path)?;
    tracing::info!(path = %sock_path.display(), "UDS Data Service bound (unified frame protocol)");

    Ok(tokio::spawn(async move {
        // audit RW-C1 hardening: resolve our own effective UID once (startup 时取)
        // 纯 Rust 读 /proc/self/status 的 Uid 行拿 euid（避免引入 libc crate）
        #[cfg(unix)]
        let our_euid: u32 = std::fs::read_to_string("/proc/self/status")
            .ok()
            .and_then(|s| {
                s.lines()
                    .find(|l| l.starts_with("Uid:"))
                    .and_then(|l| l.split_whitespace().nth(1))
                    .and_then(|v| v.parse::<u32>().ok())
            })
            .unwrap_or(0); // fallback 0 (假设 root)
        #[cfg(not(unix))]
        let our_euid: u32 = 0;
        loop {
            let (mut stream, _peer_addr) = match listener.accept().await {
                Ok(v) => v,
                Err(e) => { tracing::warn!(error = %e, "UDS accept failed"); continue; }
            };

            // --- RW-C1: peer_cred 鉴权 ---
            // tokio: peer_cred() -> Result<UCred, io::Error>
            match stream.peer_cred() {
                Ok(cred) => {
                    let caller_uid = cred.uid() as u32;
                    tracing::debug!(caller_uid, "UDS accepted");
                    // 加固：uid != 0 且 != 自身 euid → 拒绝
                    if caller_uid != 0 && caller_uid != our_euid {
                        tracing::warn!(
                            caller_uid, our_euid,
                            "UDS rejected: caller uid {} not 0/self (RW-C1)",
                            caller_uid
                        );
                        drop(stream);
                        continue;
                    }
                }
                Err(e) => {
                    tracing::warn!(error = %e, "UDS rejected: peer_cred() failed (RW-C1)");
                    drop(stream);
                    continue;
                }
            }

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

/// Per-connection loop: decode frame → dispatch → send ACK; keep reading until EOF/error.
/// Any early exit (EOF / error / format anomaly) goes through a finally-style block that
/// sets data_pushed back to false, so the poller resumes polling as a fallback
/// (REQ-DATA-002).
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

    // REQ-DATA-002: UDS link drops → data_pushed back to false, poller resumes.
    // Multi-connection scenario: any handle_conn exit sets false — as long as one is
    // alive the next accept will set it back true; a brief false only causes a few
    // extra poller rounds, no correctness impact.
    state.data_pushed.store(false, Ordering::Relaxed);
    tracing::debug!(result_ok = result.is_ok(), "UDS client disconnected, poller re-enabled");

    result
}

/// Frame builder: 12B fixed header (type + ticket) + payload.
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

/// DATA_PUSH / CMD_REQ payload dispatch. payload = {"kind":"...", "payload":...}
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
        "cdr" => submit_cdr(&state.db, &body),
        "register" => submit_register(&state.db, &body),
        "runtime" => submit_runtime(&state.db, &body),
        "audit" => insert_audit(&state.db, &body).await,
        "session" => {
            let r = submit_session(&state.db, &body);
            if r.is_ok() {
                // nsbc-esl-web §8#1: SESSION event fan-out to /calls/stream (SSE)
                let evt = serde_json::json!({"kind": "session", "payload": body}).to_string();
                let _ = state.calls_tx.send(evt);
            }
            r
        }
        "vad_event" => {
            let evt = serde_json::json!({"kind": "vad_event", "payload": body}).to_string();
            let _ = state.vad_tx.send(evt);
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

/// CMD_REQ payload dispatch: C core real-time query (REQ-DATA-SVC-003 hybrid fallback).
///
/// payload = {"cmd":"ext.ha1","user":"1000","realm":"nsbc.local"},
/// used when REGISTER auth misses in C-side in-memory table → query Web DB via UDS.
/// Reply is always Ok(body) (errors encoded in reason); Err only for JSON parse failure.
/// Reason semantics (agreed with C-side negative-cache TTL):
///   not_found / disabled → negative cache 30s; db_error → negative cache 3s
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

// --- DB helpers (Phase 1: 异步批量写入 submit_*，不再 .await DB 操作) ---

/// submit_cdr — 解析 JSON 后调用 db.submit_cdr（mpsc 入队后立即返回）。
/// 返回 Ok 表示入队成功或 fallback 同步写成功；Err 表示 JSON 缺关键字段。
fn submit_cdr(db: &crate::db::Db, v: &serde_json::Value) -> Result<(), String> {
    let call_id = v.get("call_id").and_then(|x| x.as_str()).unwrap_or("");
    if call_id.is_empty() {
        return Err("missing call_id".into());
    }
    let req = |k: &str| v.get(k).and_then(|x| x.as_str()).unwrap_or("").to_string();
    let ms = |k: &str| v.get(k).and_then(|x| x.as_u64()).unwrap_or(0);
    let num = |k: &str| v.get(k).and_then(|x| x.as_i64()).unwrap_or(0) as u32;
    let from_ip   = opt_str(v, "from_ip");
    let codec_a   = opt_str(v, "codec_a");
    let codec_b   = opt_str(v, "codec_b");
    let a_peer_ip = opt_str(v, "a_peer_ip");
    let b_peer_ip = opt_str(v, "b_peer_ip");
    let is_malicious = v.get("is_malicious").and_then(|x| x.as_u64()).unwrap_or(0) as u8;
    let threat_reason = opt_str(v, "threat_reason");
    db.submit_cdr(
        call_id.to_string(),
        opt_str(v, "tenant"),
        req("from"),
        req("to"),
        opt_str(v, "trunk"),
        opt_str(v, "rec_path"),
        req("dir"),
        req("phase"),
        req("disposition"),
        ms("start_ms"),
        ms("answer_ms"),
        ms("end_ms"),
        num("billsec"),
        num("duration_s"),
        v.get("cause").and_then(|x| x.as_i64()).unwrap_or(0) as i32,
        v.get("emergency").and_then(|x| x.as_bool()).unwrap_or(false),
        from_ip,
        codec_a,
        codec_b,
        a_peer_ip,
        b_peer_ip,
        is_malicious,
        threat_reason,
    );
    Ok(())
}

fn submit_register(db: &crate::db::Db, v: &serde_json::Value) -> Result<(), String> {
    let user = v.get("user").and_then(|x| x.as_str()).unwrap_or("");
    if user.is_empty() {
        return Err("missing user".into());
    }
    let contact = opt_str(v, "contact");
    let instance_id = v.get("instance_id").and_then(|x| x.as_str()).unwrap_or("").to_string();
    let agent = opt_str(v, "agent");
    let expires_sec = v.get("expires_sec").and_then(|x| x.as_u64()).unwrap_or(0) as u32;
    let received_ip    = opt_str(v, "received_ip");
    let received_port  = v.get("received_port").and_then(|x| x.as_i64());
    let nat_detected   = v.get("nat_detected").and_then(|x| x.as_i64());
    let public_contact = opt_str(v, "public_contact");
    let transport      = opt_str(v, "transport");
    db.submit_register(
        user.to_string(),
        contact,
        instance_id,
        agent,
        expires_sec,
        received_ip,
        received_port,
        nat_detected,
        public_contact,
        transport,
    );
    Ok(())
}

fn submit_runtime(db: &crate::db::Db, v: &serde_json::Value) -> Result<(), String> {
    let call_id = v.get("call_id").and_then(|x| x.as_str()).unwrap_or("");
    let state_name = v.get("state").and_then(|x| x.as_str()).unwrap_or("");
    if call_id.is_empty() || state_name.is_empty() {
        return Err("missing call_id/state".into());
    }
    let detail = opt_str(v, "detail");
    db.submit_runtime(call_id.to_string(), state_name.to_string(), detail);
    Ok(())
}

fn submit_session(db: &crate::db::Db, v: &serde_json::Value) -> Result<(), String> {
    let call_id = v.get("call_id").and_then(|x| x.as_str()).unwrap_or("");
    if call_id.is_empty() {
        return Err("missing call_id".into());
    }
    let event = opt_str(v, "event").unwrap_or_default();
    let state = opt_str(v, "state").unwrap_or_default();
    let from  = opt_str(v, "from").unwrap_or_default();
    let to    = opt_str(v, "to").unwrap_or_default();
    let cause = v.get("cause").and_then(|x| x.as_i64()).unwrap_or(0) as i32;
    let dir   = opt_str(v, "dir").unwrap_or_else(|| "inbound".to_string());
    let from_ip   = opt_str(v, "from_ip").unwrap_or_default();
    let codec_a   = opt_str(v, "codec_a").unwrap_or_default();
    let codec_b   = opt_str(v, "codec_b").unwrap_or_default();
    let a_peer_ip = opt_str(v, "a_peer_ip").unwrap_or_default();
    let b_peer_ip = opt_str(v, "b_peer_ip").unwrap_or_default();
    db.submit_session(
        call_id.to_string(), from, to, dir, state, event, cause,
        from_ip, codec_a, codec_b, a_peer_ip, b_peer_ip,
    );
    Ok(())
}

/// insert_audit — 保持同步 await（低频，不走 batch）。
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

fn opt_str(v: &serde_json::Value, key: &str) -> Option<String> {
    v.get(key)
        .and_then(|x| x.as_str())
        .filter(|s| !s.is_empty())
        .map(|s| s.to_string())
}
