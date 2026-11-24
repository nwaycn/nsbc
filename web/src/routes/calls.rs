//! Real-time traffic (REQ-WEB-004 P0 / REQ-OAM-007):
//! - GET  /calls         Active call counters (read-only, `show calls`)
//! - GET  /channels      In-flight call A/B Leg listing (read-only, `show channels`)
//! - GET  /calls/{id}    Single-call two-sided Leg diagnosis (read-only, `uuid_dump`)
//! - POST /calls/{id}/hangup Force hangup (L1 audit, `hangup`)
//! - POST /calls/{id}/snapshot Start video snapshot job(s) (L1 audit, `snapshot`,
//!   V2 §3.4.4: returns job ids immediately; result via SNAPSHOT event / OAM)
//!
//! REQ-CCMD-001 command enhancements (L2, core cmd_registry new routes):
//! - POST /calls/{id}/kill          uuid_kill
//! - POST /calls/{id}/answer        uuid_answer
//! - POST /calls/{id}/hold          uuid_hold (body on/off)
//! - POST /calls/{id}/transfer      uuid_transfer (body dest)
//! - POST /calls/{id}/record        uuid_record (body action+path)
//! - POST /calls/{id}/send-dtmf     uuid_send_dtmf (body digits)
//! - POST /calls/{id}/send-msg      send_msg (body text, MESSAGE forward)
//! - POST /originate                originate (body call_url)
//!
//! Media plane fields (mode / gate / SSRC / VQ) always surface as null when the
//! core has no collection source — UNACCURATE when REQ-OBS-010 is UNAVAILABLE;
//! never guess values.
use std::convert::Infallible;
use std::sync::Arc;

use axum::extract::{Path, State};
use axum::http::header;
use axum::response::sse::{Event, KeepAlive, Sse};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use axum::{Json, Router};
use serde::{Deserialize, Serialize};
use serde_json::json;
use tokio_stream::wrappers::BroadcastStream;
use tokio_stream::{Stream, StreamExt};

use crate::coreclient::{CallCounters, CallDetail, CallList, CoreApi};
use crate::error::{ApiError, ApiResult};
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + 'static,
{
    Router::new()
        .route("/calls", get(list_calls::<C>))
        .route("/channels", get(list_channels::<C>))
        // nsbc-esl-web §8#1: SESSION real-time event SSE stream (static segment
        // takes priority over /calls/:id)
        .route("/calls/stream", get(stream_calls::<C>))
        // REQ-AI-BARGEIN-001: VAD_EVENT real-time SSE stream (external AI engine subscribes)
        .route("/vad/stream", get(stream_vad::<C>))
        .route("/calls/:id", get(get_call::<C>))
        .route("/calls/:id/hangup", post(hangup::<C>))
        .route("/calls/:id/snapshot", post(snapshot::<C>))
        // Snapshot job follow-up: status (JSON) + captured JPEG. Static "snapshot"
        // segment wins over "/calls/:id", so these never shadow GET /calls/{id}.
        .route("/calls/snapshot/:job", get(snapshot_status::<C>))
        .route("/calls/snapshot/:job/image", get(snapshot_image::<C>))
        // REQ-CCMD-001 new L2 command endpoints
        .route("/calls/:id/kill", post(kill::<C>))
        .route("/calls/:id/answer", post(answer::<C>))
        .route("/calls/:id/hold", post(hold::<C>))
        .route("/calls/:id/mute", post(mute::<C>))
        .route("/calls/:id/broadcast", post(broadcast::<C>))
        .route("/calls/:id/vad-start", post(vad_start::<C>))
        .route("/calls/:id/vad-stop", post(vad_stop::<C>))
        .route("/calls/:id/playback", post(playback::<C>))
        .route("/calls/:id/playback-stop", post(playback_stop::<C>))
        .route("/calls/:id/playback-status", get(playback_status::<C>))
        .route("/calls/:id/transfer", post(transfer::<C>))
        .route("/calls/:id/record", post(record::<C>))
        .route("/calls/:id/send-dtmf", post(send_dtmf::<C>))
        .route("/calls/:id/send-msg", post(send_message::<C>))
        .route("/originate", post(originate::<C>))
}

/// GET /calls/stream — SESSION real-time event SSE stream (nsbc-esl-web §8#1).
///
/// SESSION events pushed by C→Web data.sock are broadcast via [`AppState::calls_tx`]
/// and converted here into `text/event-stream`. Auth still goes through the protected
/// router's Bearer rbac (the path `/calls/stream` resolves to L0 read-only). The
/// front-end consumes via fetch stream (EventSource cannot carry the Authorization
/// header); event name is unified as `session`, data is `{"kind","payload"}`. When
/// a slow consumer lags and drops frames, emit a `lag` event so the front-end can do
/// a full-table re-pull as a fallback.
async fn stream_calls<C>(
    State(state): State<Arc<AppState<C>>>,
) -> Sse<impl Stream<Item = Result<Event, Infallible>>>
where
    C: CoreApi + Clone + 'static,
{
    let rx = state.calls_tx.subscribe();
    let stream = BroadcastStream::new(rx).map(|item| {
        let data = match item {
            Ok(msg) => msg,
            Err(_lagged) => "{\"kind\":\"lag\"}".to_string(),
        };
        Ok::<Event, Infallible>(Event::default().event("session").data(data))
    });
    Sse::new(stream).keep_alive(KeepAlive::default())
}

/// GET /vad/stream — VAD_EVENT real-time SSE stream (REQ-AI-BARGEIN-001).
/// External AI engines (Go / Python / Node) subscribe to vad_event JSON through this
/// endpoint to implement barge-in decisions (user speaking → mute AI side → ASR → LLM →
/// TTS → broadcast → unmute).
async fn stream_vad<C>(
    State(state): State<Arc<AppState<C>>>,
) -> Sse<impl Stream<Item = Result<Event, Infallible>>>
where
    C: CoreApi + Clone + 'static,
{
    let rx = state.vad_tx.subscribe();
    let stream = BroadcastStream::new(rx).map(|item| {
        let data = match item {
            Ok(msg) => msg,
            Err(_lagged) => "{\"kind\":\"lag\"}".to_string(),
        };
        Ok::<Event, Infallible>(Event::default().event("vad").data(data))
    });
    Sse::new(stream).keep_alive(KeepAlive::default())
}

/// Call-ID routing-layer validation: 1..127 bytes, printable ASCII (0x21-0x7e),
/// aligned with core obs_control valid_callid()/DLG_CALLID_MAX. Both ends validate
/// once each.
pub(crate) fn valid_call_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() < 128
        && id.bytes().all(|b| (0x21..=0x7e).contains(&b))
}

/// Command-argument whitelist: ASCII alphanumeric plus path/URI punctuation.
/// Rejects whitespace (including CR/LF), NUL and any other control character before
/// the value is concatenated into the line-text UDS command. `extra` allows
/// operation-specific characters (e.g. `|` for playback chains).
pub(crate) fn valid_cmd_arg(s: &str, max_len: usize, extra: &[char]) -> bool {
    !s.is_empty()
        && s.len() <= max_len
        && s.chars().all(|c| {
            c.is_ascii_alphanumeric()
                || matches!(c, '@' | '.' | ':' | '-' | '_' | '+' | '/' | '\\')
                || extra.contains(&c)
        })
}

/// Generic L2 action success response (shared by all uuid_* write operations).
#[derive(Debug, Serialize)]
struct ActionResponse {
    status: &'static str,
    call_id: String,
}

fn action_response(call_id: &str) -> Json<ActionResponse> {
    Json(ActionResponse { status: "ok", call_id: call_id.to_string() })
}

/// GET /api/v1/calls: core current call counters (L0 read-only).
async fn list_calls<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<CallCounters>> {
    Ok(Json(st.core.call_counters().await?))
}

/// GET /api/v1/channels: core in-flight call A/B Leg summary (L0 read-only, REQ-OAM-007).
async fn list_channels<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<CallList>> {
    Ok(Json(st.core.list_calls().await?))
}

/// GET /api/v1/calls/{id}: single-call two-sided Leg session diagnosis (L0 read-only,
/// REQ-OAM-007/010). Call-ID missing/ended → 404; core unreachable → 502/504.
async fn get_call<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
) -> ApiResult<Json<CallDetail>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    Ok(Json(st.core.call_detail(&id).await?))
}

/// POST /api/v1/calls/{id}/hangup — L1 operation, forwards to core
/// `hangup <call-id>` (REQ-OAM-004); core-side forced audit.
/// Success 200; Call-ID missing/ended → 404; core unreachable → 502/504.
/// Length upper bound aligns with core DLG_CALLID_MAX-1 (1..127 bytes, charset
/// re-verified by core).
async fn hangup<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
) -> ApiResult<Json<serde_json::Value>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    st.core.hangup(&id).await?;
    Ok(Json(json!({
        "status": "hungup",
        "call_id": id,
    })))
}

/// Video snapshot request body (optional). `leg`: 0=A, 1=B, -1=both (default);
/// `frames`: 1..=3 decoded candidates, the sharpest is saved (default 1).
#[derive(Debug, Default, Deserialize)]
pub(crate) struct SnapshotBody {
    pub(crate) leg: Option<i32>,
    pub(crate) frames: Option<u32>,
}

/// POST /calls/{id}/snapshot — start video snapshot job(s) (V2 §3.4.4, L1 audit).
/// Returns the started job ids immediately; decoding/JPEG encoding happens on the
/// core snapshot worker. Completion is announced via the `SNAPSHOT` event
/// (metadata only, never image content). Core errors surface as-is:
/// call not found -> 404, no video / rate-limited / busy -> 4xx with reason.
async fn snapshot<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
    body: Option<Json<SnapshotBody>>,
) -> ApiResult<Json<serde_json::Value>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    let body = body.map(|Json(b)| b).unwrap_or_default();
    if let Some(l) = body.leg {
        if !(-1..=1).contains(&l) {
            return Err(ApiError::BadRequest("leg must be -1, 0 or 1".into()));
        }
    }
    if let Some(f) = body.frames {
        if !(1..=3).contains(&f) {
            return Err(ApiError::BadRequest("frames must be 1..3".into()));
        }
    }
    let started = st.core.snapshot(&id, body.leg, body.frames).await?;
    Ok(Json(json!({
        "status": "started",
        "call_id": id,
        "jobs": started.jobs,
    })))
}

/// Snapshot job state derived from the core `snapshot_status <job-id>` line.
#[derive(Debug, Serialize)]
pub(crate) struct SnapshotJobStatus {
    pub(crate) job_id: u64,
    pub(crate) running: bool,
    pub(crate) result: Option<String>,
    pub(crate) leg: Option<i32>,
    pub(crate) width: Option<i32>,
    pub(crate) height: Option<i32>,
    pub(crate) frames: Option<i32>,
    pub(crate) elapsed_ms: Option<u64>,
    pub(crate) bytes: Option<u64>,
    /// Host-side JPEG path; internal only (not echoed to the client).
    #[serde(skip)]
    pub(crate) path: Option<String>,
    /// Ready-to-use JPEG endpoint (only when `result=OK` and the file exists).
    pub(crate) image_url: Option<String>,
}

/// Parse core output (src/oam/call_commands.c cmd_snapshot_status):
///   running: `OK job=12 running leg=0`
///   done:    `OK job=12 result=OK leg=0 640x360 frames=1 123ms 45678 bytes path=/tmp/x.jpg`
///   unknown: `ERR job 12 not found`
fn parse_snapshot_status(job_id: u64, out: &str) -> ApiResult<SnapshotJobStatus> {
    let mut st = SnapshotJobStatus {
        job_id,
        running: false,
        result: None,
        leg: None,
        width: None,
        height: None,
        frames: None,
        elapsed_ms: None,
        bytes: None,
        path: None,
        image_url: None,
    };
    let mut ok_seen = false;
    for tok in out.split_whitespace() {
        if tok == "OK" {
            ok_seen = true;
        } else if tok == "running" {
            st.running = true;
        } else if let Some(v) = tok.strip_prefix("result=") {
            st.result = Some(v.to_string());
        } else if let Some(v) = tok.strip_prefix("leg=") {
            st.leg = v.parse().ok();
        } else if let Some(v) = tok.strip_prefix("frames=") {
            st.frames = v.parse().ok();
        } else if let Some(v) = tok.strip_prefix("bytes=") {
            st.bytes = v.parse().ok();
        } else if let Some(v) = tok.strip_prefix("path=") {
            // 失败作业打印 path=-，不能当成文件路径
            if v != "-" {
                st.path = Some(v.to_string());
            }
        } else if let Some(ms) = tok.strip_suffix("ms") {
            st.elapsed_ms = ms.parse().ok();
        } else if let Some((w, h)) = tok.split_once('x') {
            // 分辨率 token（640x360）；解析失败忽略
            if let (Ok(w), Ok(h)) = (w.parse::<i32>(), h.parse::<i32>()) {
                st.width = Some(w);
                st.height = Some(h);
            }
        }
    }
    if !ok_seen {
        return Err(ApiError::NotFound("snapshot job not found".into()));
    }
    if st.result.as_deref() == Some("OK") && st.path.is_some() {
        st.image_url = Some(format!("/api/v1/calls/snapshot/{job_id}/image"));
    }
    Ok(st)
}

/// GET /calls/snapshot/{job} — poll one snapshot job (L1 read-only).
async fn snapshot_status<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(job): Path<String>,
) -> ApiResult<Json<SnapshotJobStatus>> {
    let job_id = parse_job_id(&job)?;
    let raw = st
        .core
        .exec_cmd(&format!("snapshot_status {job_id}"))
        .await?;
    Ok(Json(parse_snapshot_status(job_id, &raw)?))
}

/// GET /calls/snapshot/{job}/image — stream the captured JPEG (L1 read-only).
///
/// The core writes frames under `NSBC_SNAPSHOT_DIR` (default `/tmp/nsbc_snapshots`,
/// same env the core reads). The `path=` reported by `snapshot_status` is never
/// trusted directly: it must canonicalize inside that root.
async fn snapshot_image<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(job): Path<String>,
) -> ApiResult<Response> {
    let job_id = parse_job_id(&job)?;
    let raw = st
        .core
        .exec_cmd(&format!("snapshot_status {job_id}"))
        .await?;
    let stt = parse_snapshot_status(job_id, &raw)?;
    if stt.running {
        return Err(ApiError::ServiceUnavailable(
            "snapshot job still running".into(),
        ));
    }
    if stt.result.as_deref() != Some("OK") {
        return Err(ApiError::NotFound(format!(
            "snapshot failed: {}",
            stt.result.unwrap_or_else(|| "UNKNOWN".into())
        )));
    }
    let reported = stt
        .path
        .ok_or_else(|| ApiError::NotFound("snapshot image path missing".into()))?;

    let root = std::env::var("NSBC_SNAPSHOT_DIR")
        .unwrap_or_else(|_| "/tmp/nsbc_snapshots".to_string());
    let root_canon = std::fs::canonicalize(&root)
        .map_err(|_| ApiError::NotFound("snapshot directory missing".into()))?;
    let file = std::fs::canonicalize(&reported)
        .map_err(|_| ApiError::NotFound("snapshot image missing".into()))?;
    if !file.starts_with(&root_canon) {
        tracing::warn!(path = %file.display(), "snapshot image outside root blocked");
        return Err(ApiError::NotFound("snapshot image not found".into()));
    }
    let bytes = tokio::fs::read(&file).await.map_err(|e| {
        tracing::warn!(err = %e, path = %file.display(), "read snapshot failed");
        ApiError::ServiceUnavailable("snapshot image read failed".into())
    })?;
    Ok((
        [
            (header::CONTENT_TYPE, "image/jpeg"),
            (header::CACHE_CONTROL, "no-store"),
        ],
        bytes,
    )
        .into_response())
}

fn parse_job_id(s: &str) -> ApiResult<u64> {
    s.parse::<u64>()
        .map_err(|_| ApiError::BadRequest("invalid snapshot job id".into()))
}

// ===== REQ-CCMD-001 L2 command endpoints =====

/// POST /calls/{id}/kill — uuid_kill [cause], cause optional 1-699.
#[derive(Debug, Deserialize)]
struct KillBody {
    cause: Option<u16>,
}

async fn kill<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
    Json(body): Json<KillBody>,
) -> ApiResult<Json<ActionResponse>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    if let Some(cause) = body.cause {
        if cause < 1 || cause > 699 {
            return Err(ApiError::BadRequest("cause must be 1..699".into()));
        }
        st.core.call_action(&id, "kill", &[&cause.to_string()]).await?;
    } else {
        st.core.call_action(&id, "kill", &[]).await?;
    }
    Ok(action_response(&id))
}

/// POST /calls/{id}/answer — uuid_answer.
async fn answer<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
) -> ApiResult<Json<ActionResponse>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    st.core.call_action(&id, "answer", &[]).await?;
    Ok(action_response(&id))
}

/// POST /calls/{id}/hold — uuid_hold on/off.
#[derive(Debug, Deserialize)]
struct HoldBody {
    on: bool,
}

async fn hold<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
    Json(body): Json<HoldBody>,
) -> ApiResult<Json<ActionResponse>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    let flag = if body.on { "on" } else { "off" };
    st.core.call_action(&id, "hold", &[flag]).await?;
    Ok(action_response(&id))
}

/// POST /calls/{id}/mute — uuid_mute on/off [leg] (REQ-AI-BARGEIN-001).
/// Barge-in core command: when the user starts speaking the AI engine calls mute
/// on leg=1 (AI side); when the user pauses it calls mute off to resume.
#[derive(Debug, Deserialize)]
struct MuteBody {
    /// true → mute on; false → mute off
    on: bool,
    /// 0=A leg (user) 1=B leg (AI) -1=both legs (default)
    leg: Option<i32>,
}

async fn mute<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
    Json(body): Json<MuteBody>,
) -> ApiResult<Json<ActionResponse>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    let flag = if body.on { "on" } else { "off" };
    let leg_str = match body.leg {
        Some(0) => "0",
        Some(1) => "1",
        _ => "-1", // -1 or None → both legs
    };
    st.core.call_action(&id, "mute", &[flag, leg_str]).await?;
    Ok(action_response(&id))
}

/// POST /calls/{id}/broadcast — uuid_broadcast <wav_path> (REQ-AI-BARGEIN-001).
/// Called after AI TTS synthesizes a WAV, to inject audio into the call side so the user
/// can hear it.
#[derive(Debug, Deserialize)]
struct BroadcastBody {
    /// WAV file path (absolute path reachable by the nsbcd process)
    wav_path: String,
}

async fn broadcast<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
    Json(body): Json<BroadcastBody>,
) -> ApiResult<Json<ActionResponse>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    if !valid_cmd_arg(&body.wav_path, 512, &['|']) {
        return Err(ApiError::BadRequest(
            "invalid wav_path (allowed: alnum @ . : - _ + / \\ |)".into(),
        ));
    }
    st.core.call_action(&id, "broadcast", &[&body.wav_path]).await?;
    Ok(action_response(&id))
}

/// POST /calls/{id}/vad-start — uuid_vad_start <threshold> (REQ-AI-BARGEIN-001).
/// After the B leg answers, the AI engine calls this command to start VAD monitoring;
/// afterwards state.vad_tx begins pushing VAD_EVENTs.
#[derive(Debug, Deserialize)]
struct VadStartBody {
    /// RMS threshold (int16 PCM amplitude), default 800, silence < 300, normal speech > 1000
    threshold: Option<u16>,
}

async fn vad_start<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
    Json(body): Json<VadStartBody>,
) -> ApiResult<Json<ActionResponse>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    let thr = body.threshold.unwrap_or(800);
    st.core.call_action(&id, "vad_start", &[&thr.to_string()]).await?;
    Ok(action_response(&id))
}

/// POST /calls/{id}/vad-stop — uuid_vad_stop (REQ-AI-BARGEIN-001).
/// Clean up VAD monitoring before the call ends.
async fn vad_stop<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
) -> ApiResult<Json<ActionResponse>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    st.core.call_action(&id, "vad_stop", &[]).await?;
    Ok(action_response(&id))
}

// ===== playback chain 路由 =====

/// POST /calls/{id}/playback — uuid_playback <wav|chain> [side=-1] [loop=0]
#[derive(Debug, Deserialize)]
struct PlaybackBody {
    /// WAV 路径或管道符拼接 chain: "9.wav|5.wav|2.wav|agent.wav"
    wav: String,
    /// -1=both(默认) 0=A 1=B
    side: Option<i32>,
    /// 0=播一次 EOF 停(默认) 1=循环 (chain 模式忽略)
    /// 字段名用 looping（loop 是 Rust 保留字），JSON key 仍叫 "loop"
    #[serde(rename = "loop")]
    looping: Option<i32>,
}

fn valid_playback_wav(w: &str) -> bool {
    valid_cmd_arg(w, 1024, &['|'])
}

async fn playback<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
    Json(body): Json<PlaybackBody>,
) -> ApiResult<Json<ActionResponse>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    if !valid_playback_wav(&body.wav) {
        return Err(ApiError::BadRequest("invalid wav (1..1024 chars)".into()));
    }
    let side = body.side.unwrap_or(-1);
    if side != -1 && side != 0 && side != 1 {
        return Err(ApiError::BadRequest("side must be -1|0|1".into()));
    }
    let lp = body.looping.unwrap_or(0);
    if lp != 0 && lp != 1 {
        return Err(ApiError::BadRequest("loop must be 0|1".into()));
    }
    st.core.call_action(
        &id, "playback",
        &[&body.wav, &side.to_string(), &lp.to_string()],
    ).await?;
    Ok(action_response(&id))
}

/// POST /calls/{id}/playback-stop — uuid_playback_stop (幂等)
async fn playback_stop<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
) -> ApiResult<Json<ActionResponse>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    st.core.call_action(&id, "playback_stop", &[]).await?;
    Ok(action_response(&id))
}

/// GET /calls/{id}/playback-status — uuid_playback_status (L1 只读)
#[derive(Debug, serde::Serialize)]
struct PlaybackStatus {
    active: bool,
    side: Option<i32>,
    #[serde(rename = "loop")]
    looping: Option<i32>,
}

/// 解析 core 输出："OK playback active=1 side=-1 loop=0 call=xxx"
fn parse_playback_status(output: &str) -> PlaybackStatus {
    let mut active = false;
    let mut side: Option<i32> = None;
    let mut looping: Option<i32> = None;

    for tok in output.split_whitespace() {
        if let Some(v) = tok.strip_prefix("active=") {
            active = v.parse::<i32>().map(|x| x == 1).unwrap_or(false);
        } else if let Some(v) = tok.strip_prefix("side=") {
            side = v.parse::<i32>().ok();
        } else if let Some(v) = tok.strip_prefix("loop=") {
            looping = v.parse::<i32>().ok();
        }
    }
    PlaybackStatus { active, side, looping }
}

async fn playback_status<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
) -> ApiResult<Json<PlaybackStatus>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    let raw = st.core.exec_cmd(&format!("uuid_playback_status {}", id)).await?;
    Ok(Json(parse_playback_status(&raw)))
}

/// POST /calls/{id}/transfer — uuid_transfer dest.
#[derive(Debug, Deserialize)]
struct TransferBody {
    dest: String,
}

async fn transfer<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
    Json(body): Json<TransferBody>,
) -> ApiResult<Json<ActionResponse>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    if !valid_cmd_arg(&body.dest, 512, &[]) {
        return Err(ApiError::BadRequest(
            "invalid dest (allowed: alnum @ . : - _ + / \\ )".into(),
        ));
    }
    st.core.call_action(&id, "transfer", &[&body.dest]).await?;
    Ok(action_response(&id))
}

/// POST /calls/{id}/record — uuid_record start|stop [path].
#[derive(Debug, Deserialize)]
struct RecordBody {
    /// start | stop
    action: String,
    /// Required at start time
    path: Option<String>,
}

async fn record<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
    Json(body): Json<RecordBody>,
) -> ApiResult<Json<ActionResponse>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    let act = body.action.trim().to_ascii_lowercase();
    match act.as_str() {
        "start" => {
            let path = body.path
                .filter(|p| valid_cmd_arg(p, 512, &[]))
                .ok_or_else(|| ApiError::BadRequest("path required for start".into()))?;
            st.core.call_action(&id, "record", &["start", &path]).await?;
        }
        "stop" => {
            st.core.call_action(&id, "record", &["stop"]).await?;
        }
        _ => {
            return Err(ApiError::BadRequest("action must be 'start' or 'stop'".into()));
        }
    }
    Ok(action_response(&id))
}

/// POST /calls/{id}/send-dtmf — uuid_send_dtmf digits (0-9*#A-D, ≤32).
#[derive(Debug, Deserialize)]
struct SendDtmfBody {
    digits: String,
}

fn valid_dtmf(d: &str) -> bool {
    !d.is_empty()
        && d.len() <= 32
        && d.chars().all(|c| matches!(c, '0'..='9' | '*' | '#' | 'A'..='D' | 'a'..='d'))
}

async fn send_dtmf<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
    Json(body): Json<SendDtmfBody>,
) -> ApiResult<Json<ActionResponse>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    if !valid_dtmf(&body.digits) {
        return Err(ApiError::BadRequest(
            "digits must be 0-9*#A-D, ≤32 chars".into(),
        ));
    }
    st.core.call_action(&id, "send_dtmf", &[&body.digits]).await?;
    Ok(action_response(&id))
}

/// POST /calls/{id}/send-msg — send_msg <call-id> <body> (MESSAGE forward).
#[derive(Debug, Deserialize)]
struct SendMessageBody {
    text: String,
}

async fn send_message<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
    Json(body): Json<SendMessageBody>,
) -> ApiResult<Json<ActionResponse>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    if body.text.is_empty() || body.text.len() > 4096 {
        return Err(ApiError::BadRequest(
            "message body must be 1..4096 chars".into(),
        ));
    }
    st.core.send_message(&id, &body.text).await?;
    Ok(action_response(&id))
}

/// POST /originate — originate <call-url>.
#[derive(Debug, Deserialize)]
struct OriginateBody {
    call_url: String,
}

async fn originate<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Json(body): Json<OriginateBody>,
) -> ApiResult<Json<serde_json::Value>> {
    if body.call_url.is_empty() || body.call_url.len() > 512 {
        return Err(ApiError::BadRequest("invalid call_url".into()));
    }
    st.core.originate(&body.call_url).await?;
    Ok(Json(json!({
        "status": "originate-dispatched",
        "call_url": body.call_url,
    })))
}
