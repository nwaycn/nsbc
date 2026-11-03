//! NSBC UDS Data Service — C core→Web direction UDS Server (REQ-DATA-SVC-002 P0, unified
//! frame protocol).
//!
//! Transport: 2 × AF_UNIX SOCK_DGRAM (Linux reliable datagram, no fragmentation, one
//! datagram = one frame — natural message boundary, no half-frame reassembly needed).
//!
//!   data_push.sock — C→Rust only, carries FT_DATA_PUSH (CDR/register/runtime/audit/
//!                    session/vad/billing_settle). Fire-and-forget — NO ACK back.
//!                    C-side ring buffer self-heals on overflow.
//!   data_cmd.sock  — bidirectional, carries FT_HELLO / FT_HELLO_ACK / FT_CMD_REQ /
//!                    FT_CMD_RSP. C-side blocks on cond_wait for matching ticket.
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
//! One recv_from = one complete datagram = one complete frame.
//!
//! Peer cred: Rust server binds, C core connect()s. AF_UNIX datagram is already
//! same-isolated-machine only; the legacy peer_cred() check from UnixStream mode is
//! skipped here — the HELLO frame carries pid/hostname as a soft handshake.

use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;
use std::collections::HashMap;
use tokio::sync::oneshot;

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

/* 256KB datagram-safe payload upper bound (matches C-side NSBC_MAX_PAYLOAD exactly). */
const MAX_PAYLOAD: usize = 256 * 1024;
const MAX_FRAME:  usize = FRAME_HDR_LEN + MAX_PAYLOAD;

/* ---- flags 位定义 (与 C 端 uDS_client.h 对齐) ---- */
const UDS_FLG_MORE_FRAG:   u8 = 0x01;  /* bit0: 还有后续分片 */
const UDS_FLG_COMPRESSED:  u8 = 0x02;  /* bit1: payload 已压缩（预留） */
const UDS_FRAG_TIMEOUT_MS: u64 = 3000;  /* 分片重组超时阈值 */
const UDS_FRAG_MAX_TOTAL:  usize = 64 * 1024 * 1024;  /* 重组 buffer 上限 64MB */

/* ---- Rust→C CMD_REQ 状态 (Web 发命令给 C core, 等 RSP) ----
 * ticket 生成用高位 0x80000000 区分方向（C→Rust ticket 高位为 0）。 */
static CORE_CMD_TICKET: AtomicU32 = AtomicU32::new(0x80000000);
static CORE_CMD_WAITERS: Mutex<Option<HashMap<u32, oneshot::Sender<Vec<u8>>>>> = Mutex::new(None);
/* 分片重组状态（cmd_recv_loop 内部持有，每次 loop 重置） */
struct FragState {
    ticket: u32,
    buf: Vec<u8>,
    last_ms: u64,
}
impl FragState {
    fn new() -> Self { Self { ticket: 0, buf: Vec::new(), last_ms: 0 } }
    fn reset(&mut self) { self.ticket = 0; self.buf.clear(); self.last_ms = 0; }
}

pub fn default_sock_base_path() -> std::path::PathBuf {
    std::path::PathBuf::from(std::env::var_os("NSBC_DATA_SOCK")
        .unwrap_or_else(|| "/var/run/nsbc/data.sock".into()))
}

/* Derive push / cmd socket paths from base path (mirrors C-side uds_derive_paths). */
fn derive_paths(base: &std::path::Path) -> (std::path::PathBuf, std::path::PathBuf) {
    let base_str = base.to_string_lossy();
    if let Some(dot) = base_str.rfind('.') {
        let prefix = &base_str[..dot];
        (
            std::path::PathBuf::from(format!("{prefix}_push.sock")),
            std::path::PathBuf::from(format!("{prefix}_cmd.sock")),
        )
    } else {
        (
            std::path::PathBuf::from(format!("{base_str}_push.sock")),
            std::path::PathBuf::from(format!("{base_str}_cmd.sock")),
        )
    }
}

/* -------------------------------------------------------------------------- */
/* Platform selection                                                         */
/* -------------------------------------------------------------------------- */

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

    let (push_path, cmd_path) = derive_paths(&sock_path);
    /* Clean stale socket paths from previous runs */
    for p in [&push_path, &cmd_path] {
        if p.exists() { let _ = std::fs::remove_file(p); }
    }

    let push_sock = tokio::net::UnixDatagram::bind(&push_path)?;
    let cmd_sock  = tokio::net::UnixDatagram::bind(&cmd_path)?;
    tracing::info!(
        push = %push_path.display(),
        cmd  = %cmd_path.display(),
        "UDS Data Service bound (dual AF_UNIX DGRAM)"
    );

    Ok(tokio::spawn(async move {
        let push_state = state.clone();
        let cmd_state  = state.clone();

        let push_join = tokio::spawn(async move {
            if let Err(e) = push_recv_loop(push_sock, &push_state).await {
                tracing::warn!(error = %e, "UDS push loop exited");
            }
        });

        let cmd_join = tokio::spawn(async move {
            if let Err(e) = cmd_recv_loop(cmd_sock, &cmd_state).await {
                tracing::warn!(error = %e, "UDS cmd loop exited");
            }
        });

        let _ = tokio::join!(push_join, cmd_join);
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

/* -------------------------------------------------------------------------- */
/* Frame decode helpers — one datagram = one frame, no reassembly              */
/* -------------------------------------------------------------------------- */

/// Decode one frame from a raw datagram buffer. Returns parsed fields on success,
/// or None on malformed magic/len. payload bytes reference `buf`.
fn decode_frame(buf: &[u8]) -> Option<(u8, u32, u32, u8, &[u8])> {
    if buf.len() < FRAME_HDR_LEN { return None; }
    if buf[0] != FRAME_MAGIC { return None; }
    let total_len = u32::from_be_bytes([buf[2], buf[3], buf[4], buf[5]]) as usize;
    if total_len < FRAME_HDR_LEN || total_len > MAX_FRAME { return None; }
    let ticket    = u32::from_be_bytes([buf[6], buf[7], buf[8], buf[9]]);
    let frame_type = buf[1];
    let flags = buf[10];
    let payload = &buf[FRAME_HDR_LEN..total_len.min(buf.len())];
    Some((frame_type, total_len as u32, ticket, flags, payload))
}

fn build_frame(frame_type: u8, ticket: u32, flags: u8, payload: &[u8]) -> Vec<u8> {
    let total_len = (FRAME_HDR_LEN + payload.len()) as u32;
    let mut out = Vec::with_capacity(total_len as usize);
    out.push(FRAME_MAGIC);
    out.push(frame_type);
    out.extend_from_slice(&total_len.to_be_bytes());
    out.extend_from_slice(&ticket.to_be_bytes());
    out.push(flags);   /* flags: bit0=more_fragment, bit1=compressed */
    out.push(0);       /* rsvd */
    out.extend_from_slice(payload);
    out
}

/// Build one or more frames: if payload fits in MAX_PAYLOAD → single frame.
/// Otherwise split into ceil(len/MAX_PAYLOAD) fragments, all sharing the same
/// ticket; first N-1 frames have flags|=UDS_FLG_MORE_FRAG.
fn build_fragmented_frames(frame_type: u8, ticket: u32, payload: &[u8]) -> Vec<Vec<u8>> {
    let chunk = MAX_PAYLOAD;
    if payload.len() <= chunk {
        return vec![build_frame(frame_type, ticket, 0, payload)];
    }
    let mut out = Vec::new();
    let mut sent = 0;
    while sent < payload.len() {
        let remaining = payload.len() - sent;
        let this_chunk = remaining.min(chunk);
        let is_last = sent + this_chunk >= payload.len();
        let flags = if is_last { 0 } else { UDS_FLG_MORE_FRAG };
        out.push(build_frame(frame_type, ticket, flags, &payload[sent..sent+this_chunk]));
        sent += this_chunk;
    }
    out
}

fn build_bad_frame_ack(ticket: u32, msg: &[u8]) -> Vec<u8> {
    let total_len = (FRAME_HDR_LEN + msg.len()) as u32;
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

/* -------------------------------------------------------------------------- */
/* Push recv loop — fire-and-forget, NO ACK back                              */
/* -------------------------------------------------------------------------- */

#[cfg(unix)]
async fn push_recv_loop<C: crate::coreclient::CoreApi + Clone + Send + Sync + 'static>(
    sock: tokio::net::UnixDatagram,
    state: &Arc<AppState<C>>,
) -> std::io::Result<()> {
    let mut buf = vec![0u8; MAX_FRAME];
    loop {
        let (n, _peer) = sock.recv_from(&mut buf).await?;
        let frame = match decode_frame(&buf[..n]) {
            Some(f) => f,
            None => {
                tracing::debug!("push: malformed frame, dropped");
                continue;
            }
        };
        let (frame_type, _total, _ticket, _flags, payload) = frame;
        let payload_str = String::from_utf8_lossy(payload).to_string();

        match frame_type {
            FT_DATA_PUSH => {
                /* Fire-and-forget — no ACK back to C side. */
                if let Err(e) = dispatch_push(&payload_str, state).await {
                    tracing::warn!(error = %e, "push dispatch failed");
                }
            }
            FT_HELLO => {
                /* Hello sometimes arrives on wrong channel if C-side misconfig.
                 * Forward to sync module (no need to reply here). */
                crate::sync::on_hello(state.clone());
            }
            other => {
                tracing::debug!(frame_type = other, "push channel: unexpected frame type");
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Cmd recv loop — bidirectional REQ/RSP + HELLO + fragment reassembly        */
/* -------------------------------------------------------------------------- */

#[cfg(unix)]
async fn cmd_recv_loop<C: crate::coreclient::CoreApi + Clone + Send + Sync + 'static>(
    sock: tokio::net::UnixDatagram,
    state: &Arc<AppState<C>>,
) -> std::io::Result<()> {
    /* 初始化 CORE_CMD_WAITERS（send_core_cmd_via_uds 暴露给 coreclient.rs 用） */
    {
        let mut w = CORE_CMD_WAITERS.lock().unwrap();
        if w.is_none() {
            *w = Some(HashMap::new());
        }
    }

    let mut buf = vec![0u8; MAX_FRAME];
    let mut frag = FragState::new();

    loop {
        let (n, peer) = sock.recv_from(&mut buf).await?;
        let frame = match decode_frame(&buf[..n]) {
            Some(f) => f,
            None => {
                if let Some(p) = peer.as_pathname() {
                    let _ = sock.send_to(&build_bad_frame_ack(0, b"invalid frame"), p).await;
                }
                continue;
            }
        };
        let (frame_type, _total, ticket, flags, payload) = frame;

        /* ---- 分片重组 ---- */
        let is_last = (flags & UDS_FLG_MORE_FRAG) == 0;
        if !is_last {
            /* 中间分片：累积 */
            if frag.ticket != ticket { frag.reset(); }
            let new_len = frag.buf.len() + payload.len();
            if new_len > UDS_FRAG_MAX_TOTAL {
                tracing::warn!(ticket, "fragment too large, dropping");
                frag.reset();
                continue;
            }
            frag.buf.extend_from_slice(payload);
            frag.ticket = ticket;
            frag.last_ms = now_ms();
            continue;
        }

        /* 最后一片：拼完整 payload */
        let full_payload: Vec<u8> = if frag.ticket == ticket && !frag.buf.is_empty() {
            let mut complete = std::mem::take(&mut frag.buf);
            complete.extend_from_slice(payload);
            frag.reset();
            complete
        } else {
            payload.to_vec()
        };

        let is_rust_originated = (ticket & 0x80000000) != 0;

        /* ---- Rust→C CMD_RSP 匹配（Web 发的命令回来了） ---- */
        if frame_type == FT_CMD_RSP && is_rust_originated {
            let mut w = CORE_CMD_WAITERS.lock().unwrap();
            if let Some(map) = w.as_mut() {
                if let Some(tx) = map.remove(&ticket) {
                    let _ = tx.send(full_payload);
                    continue;  /* 成功匹配，跳过下面的 CMD_REQ 分发 */
                }
            }
        }

        /* ---- C→Rust CMD_REQ（cmd_recv_loop 原生处理的请求） ---- */
        if frame_type == FT_CMD_REQ && !is_rust_originated {
            let payload_str = String::from_utf8_lossy(&full_payload).to_string();
            let resp: Option<(u8, Vec<u8>)> = match dispatch_cmd_req(&payload_str, state).await {
                Ok(body)  => Some((FT_CMD_RSP, body.into_bytes())),
                Err(e)    => Some((FT_CMD_RSP, serde_json::json!({"ok": false, "reason": "bad_request", "error": e}).to_string().into_bytes())),
            };
            if let Some((ft, body)) = resp {
                let frames = build_fragmented_frames(ft, ticket, &body);
                if let Some(p) = peer.as_pathname() {
                    for f in frames {
                        let _ = sock.send_to(&f, p).await;
                    }
                }
            }
            continue;
        }

        /* ---- 其他帧类型 ---- */
        let payload_str = String::from_utf8_lossy(&full_payload).to_string();
        let resp: Option<(u8, Vec<u8>)> = match frame_type {
            FT_HELLO => {
                crate::sync::on_hello(state.clone());
                Some((FT_HELLO_ACK, serde_json::json!({"ok": true}).to_string().into_bytes()))
            }
            FT_HEARTBEAT => Some((FT_ACK, serde_json::json!({"ok": true}).to_string().into_bytes())),
            FT_DATA_PUSH => {
                if let Err(e) = dispatch_push(&payload_str, state).await {
                    tracing::warn!(error = %e, "cmd channel push fallback failed");
                }
                None
            }
            FT_ERROR => {
                tracing::warn!(payload = %payload_str, "C side UDS error frame");
                None
            }
            FT_HELLO_ACK | FT_CMD_RSP | FT_ACK => None, /* 静默 */
            other => {
                tracing::debug!(frame_type = other, "unknown UDS frame type");
                None
            }
        };

        if let Some((ft, body)) = resp {
            let frames = build_fragmented_frames(ft, ticket, &body);
            if let Some(p) = peer.as_pathname() {
                for f in frames {
                    let _ = sock.send_to(&f, p).await;
                }
            }
        }
    }
}

/// 当前时间戳（ms，供分片超时用）
fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/* -------------------------------------------------------------------------- */
/* send_core_cmd_via_uds —— Web→C 命令发送 + 分片 + 等待 RSP                  */
/* -------------------------------------------------------------------------- */

/// Send a command to C core via data_cmd.sock UDS frame protocol.
/// Handles fragmentation automatically if payload exceeds MAX_PAYLOAD.
/// Returns the raw RSP bytes from C core (may also be fragmented on C→Rust path,
/// reassembled by cmd_recv_loop before being delivered here).
///
/// Uses a temporary client datagram bind path (mirrors C-side uds_open_dgram).
/// The actual RSP delivery is routed through cmd_recv_loop + CORE_CMD_WAITERS map,
/// so this function blocks on oneshot::Receiver until timeout.
#[cfg(unix)]
pub async fn send_core_cmd_via_uds(
    sock_path: &std::path::Path,
    cmd_text: &str,
    timeout_ms: u64,
) -> Result<Vec<u8>, String> {
    use tokio::io::AsyncWriteExt;

    let cmd_payload = cmd_text.as_bytes();
    let ticket = CORE_CMD_TICKET.fetch_add(1, Ordering::Relaxed);

    /* 注册等待者 */
    let (tx, rx) = oneshot::channel();
    {
        let mut w = CORE_CMD_WAITERS.lock().map_err(|e| e.to_string())?;
        if let Some(map) = w.as_mut() {
            map.insert(ticket, tx);
        } else {
            return Err("CORE_CMD_WAITERS not initialized".into());
        }
    }

    /* 构造分片帧 */
    let frames = build_fragmented_frames(FT_CMD_REQ, ticket, cmd_payload);

    /* 用临时 client datagram bind → connect → 发送所有分片
     * （C 端 connect 到 server path，send() 就自动 route 回 server 的 recv_from） */
    let client_path = format!("/tmp/nsbc_web_cmd_{}.sock", std::process::id());
    let client = tokio::net::UnixDatagram::bind(&client_path)
        .map_err(|e| format!("bind client datagram {client_path}: {e}"))?;
    let _ = tokio::fs::remove_file(&client_path).await; /* 清理残留 */

    let cmd_path = crate::uds_server::default_sock_base_path();
    let (_, cmd_path) = derive_paths(&cmd_path);
    client.connect(&cmd_path)
        .map_err(|e| format!("connect to cmd_path: {e}"))?;

    for frame in &frames {
        client.send(frame).await
            .map_err(|e| format!("send CMD_REQ fragment: {e}"))?;
    }

    /* 等 RSP —— 来自 cmd_recv_loop 通过 oneshot 唤醒 */
    let timeout = std::time::Duration::from_millis(timeout_ms);
    match tokio::time::timeout(timeout, rx).await {
        Ok(Ok(data)) => Ok(data),
        Ok(Err(_)) => Err(format!("CMD_REQ ticket={ticket} dropped (cmd_recv_loop exited)")),
        Err(_) => {
            /* 超时 —— 清理等待者 */
            let mut w = CORE_CMD_WAITERS.lock().map_err(|e| e.to_string())?;
            if let Some(map) = w.as_mut() { map.remove(&ticket); }
            Err(format!("CMD_REQ ticket={ticket} timed out after {timeout_ms}ms"))
        }
    }
}

/* ========================================================================
 *  PUSH / CMD dispatch handlers (kept verbatim from stream era — logic
 *  is identical, only transport framing changed above).
 * ======================================================================*/

/// DATA_PUSH payload dispatch. payload = {"kind":"...", "payload":...}
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
        // G1: C 侧 billing.c END settle 成功后推 billing_settle
        // payload: {"account_key":"tenant:1:extension:1000","call_id":"xxx","charge_uc":-150000,"billsec":185}
        "billing_settle" => {
            let ak = body.get("account_key").and_then(|x| x.as_str()).unwrap_or("");
            let call_id = body.get("call_id").and_then(|x| x.as_str()).unwrap_or("");
            let charge_uc = body.get("charge_uc").and_then(|x| x.as_i64()).unwrap_or(0);
            if ak.is_empty() || call_id.is_empty() {
                return Err("billing_settle missing account_key or call_id".into());
            }
            // account_key 查 account_id — 账户不存在可能是中继付费场景，跳过不写流水
            let acct = state.db.get_account_by_key(ak).await
                .map_err(|e| format!("billing_settle db err: {e}"))?;
            if let Some(acct) = acct {
                state.db.settle_billing(acct.id, call_id, -charge_uc, None).await
                    .map_err(|e| format!("billing_settle settle_billing err: {e}"))?;
                tracing::info!(account_key = ak, call_id, charge_uc, acct.id,
                    "billing_settle: call_charge written to billing_transactions");
            } else {
                tracing::debug!(account_key = ak, call_id, "billing_settle: account not found in DB — skipping");
            }
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
        /* ---- cfg.pull_all：C core 启动/reload 时拉全量 enabled 配置表 ----
         * C 侧发 {"cmd":"cfg.pull_all"}，返回小表的 enabled 行 JSON。
         *
         * 大表（extensions）不分发在此通道：
         *   90000 extensions × ~140B = 12.6MB → 超过 NSBC_MAX_FRAME → 无限
         *   frame-len-invalid loop。extensions 走独立的 extensions.reload 推送。
         *
         * C 侧 cfg_sync.c accept_missing=1 已容忍 extensions key 缺失。
         * 其他 7 张小表（gateways/trunks/dprules/profiles/queues/ivr_menus/
         * security_blacklist）加起来 < 100KB，UDS 单帧完全装得下。 */
        "cfg.pull_all" => {
            if state.db.pool().is_none() {
                return Ok(serde_json::json!({"ok": false, "reason": "db_not_ready"}).to_string());
            }
            // 大表 extensions 不分发（走独立 extensions.reload）
            let ext_count = state.db.count_enabled_extensions().await.unwrap_or(0);
            let large_ext = ext_count > 5000;

            // --- gateways ---
            let gateways = state.db.list_enabled_gateways().await.unwrap_or_default();

            // --- trunks ---
            let trunks = state.db.list_enabled_trunks().await.unwrap_or_default();

            // --- dialplan_rules ---
            let dprules = state.db.list_enabled_dialplan_rules(None).await.unwrap_or_default();

            // --- sip_profiles ---
            let profiles = state.db.list_enabled_sip_profiles().await.unwrap_or_default();

            // --- queues: list_enabled_queues 返回 (Queue, tenant_name) tuple，展开 members_json ---
            let q_rows = state.db.list_enabled_queues().await.unwrap_or_default();
            let queues: Vec<crate::sync::QueueSyncDto> = q_rows.into_iter().map(|(q, tenant_name)| {
                let members: Vec<String> = serde_json::from_str(&q.members_json).unwrap_or_default();
                crate::sync::QueueSyncDto {
                    tenant: tenant_name,
                    name: q.name.clone(),
                    enabled: q.enabled != 0,
                    strategy: q.ring_strategy.clone(),
                    max_wait_s: q.max_wait_sec,
                    wrap_time_s: 30,
                    require_skill: String::new(),
                    members,
                }
            }).collect();

            // --- ivr_menus ---
            let ivr_menus = state.db.list_enabled_ivr_menus().await.unwrap_or_default();

            // --- security_blacklist: 含 fence_mode + method_blocked + auto_ban 等 ---
            let bl_rows = state.db.list_enabled_security_blacklist().await.unwrap_or_default();
            let bl_dtos: Vec<_> = bl_rows.iter().map(crate::sync::BlacklistSyncDto::from).collect();
            let sec_cfg = state.db.get_security_config().await.unwrap_or_default();
            let security_blacklist = serde_json::json!({
                "fence_mode":               sec_cfg.fence_mode,
                "method_blocked":           sec_cfg.method_blocked,
                "auto_ban":                 sec_cfg.auto_ban,
                "authfail_threshold":       sec_cfg.authfail_threshold,
                "authfail_window_s":        sec_cfg.authfail_window_s,
                "ban_duration_s":           sec_cfg.ban_duration_s,
                "ratelimit_regs":           sec_cfg.ratelimit_regs,
                "ratelimit_window_s":       sec_cfg.ratelimit_window_s,
                "ratelimit_invites":        sec_cfg.ratelimit_invites,
                "ratelimit_invites_active": sec_cfg.ratelimit_invites_active,
                "rules":                    bl_dtos,
            });

            tracing::info!(
                ext_total = ext_count,
                ext_in_response = if large_ext { "skipped(large)" } else { "included" },
                gateways = gateways.len(),
                trunks = trunks.len(),
                dprules = dprules.len(),
                profiles = profiles.len(),
                queues = queues.len(),
                ivr_menus = ivr_menus.len(),
                security_rules = bl_dtos.len(),
                "cfg.pull_all requested by C core"
            );

            // 大表 extensions 从拉取通道剥离，走独立 extensions.reload
            let extensions_payload: Option<Vec<_>> = if large_ext {
                None
            } else {
                let ext_rows = state.db.list_enabled_extensions().await.unwrap_or_default();
                Some(ext_rows.iter().map(crate::sync::ExtSyncDto::from).collect())
            };

            let mut resp = serde_json::json!({
                "ok": true,
                "gateways": gateways,
                "trunks": trunks,
                "dialplan_rules": dprules,
                "sip_profiles": profiles,
                "queues": queues,
                "ivr_menus": ivr_menus,
                "security_blacklist": security_blacklist,
            });
            if let Some(ext) = extensions_payload {
                resp["extensions"] = serde_json::json!(ext);
            } else {
                resp["extensions_large_skipped"] = serde_json::json!(true);
            }
            Ok(resp.to_string())
        }
        "" => Ok(serde_json::json!({"ok": false, "reason": "bad_request"}).to_string()),
        _ => {
            tracing::warn!(cmd, "unknown CMD_REQ");
            Ok(serde_json::json!({"ok": false, "reason": "unknown_cmd"}).to_string())
        }
    }
}

/* -------------------------------------------------------------------------- */
/* DB helpers (kept verbatim from stream era)                                  */
/* -------------------------------------------------------------------------- */

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
