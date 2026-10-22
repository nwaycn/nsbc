//! 实时话务（REQ-WEB-004 P0 / REQ-OAM-007）：
//! - GET  /calls         活动通话计数（只读，`show calls`）
//! - GET  /channels      在途呼叫 A/B Leg 清单（只读，`show channels`）
//! - GET  /calls/{id}    单呼叫双侧 Leg 诊断（只读，`uuid_dump`）
//! - POST /calls/{id}/hangup 强制挂断（L1 审计，`hangup`）
//!
//! REQ-CCMD-001 命令增强（L2，核心 cmd_registry 新路由）：
//! - POST /calls/{id}/kill          uuid_kill
//! - POST /calls/{id}/answer        uuid_answer
//! - POST /calls/{id}/hold          uuid_hold（body on/off）
//! - POST /calls/{id}/transfer      uuid_transfer（body dest）
//! - POST /calls/{id}/record        uuid_record（body action+path）
//! - POST /calls/{id}/send-dtmf     uuid_send_dtmf（body digits）
//! - POST /calls/{id}/send-msg      send_msg（body text，MESSAGE 转发）
//! - POST /originate                originate（body call_url）
//!
//! 媒体面字段（模式/门控/SSRC/VQ）在核心无采集来源时一律以 null 如实表达
//! （REQ-OBS-010：UNAVAILABLE，严禁估算）。
use std::convert::Infallible;
use std::sync::Arc;

use axum::extract::{Path, State};
use axum::response::sse::{Event, KeepAlive, Sse};
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
        // nsbc-esl-web §8#1：SESSION 实时事件 SSE 流（静态段优先于 /calls/:id）
        .route("/calls/stream", get(stream_calls::<C>))
        // REQ-AI-BARGEIN-001：VAD_EVENT 实时 SSE 流（外部 AI 引擎订阅）
        .route("/vad/stream", get(stream_vad::<C>))
        .route("/calls/:id", get(get_call::<C>))
        .route("/calls/:id/hangup", post(hangup::<C>))
        // REQ-CCMD-001 新增 L2 命令端点
        .route("/calls/:id/kill", post(kill::<C>))
        .route("/calls/:id/answer", post(answer::<C>))
        .route("/calls/:id/hold", post(hold::<C>))
        .route("/calls/:id/mute", post(mute::<C>))
        .route("/calls/:id/broadcast", post(broadcast::<C>))
        .route("/calls/:id/vad-start", post(vad_start::<C>))
        .route("/calls/:id/vad-stop", post(vad_stop::<C>))
        .route("/calls/:id/transfer", post(transfer::<C>))
        .route("/calls/:id/record", post(record::<C>))
        .route("/calls/:id/send-dtmf", post(send_dtmf::<C>))
        .route("/calls/:id/send-msg", post(send_message::<C>))
        .route("/originate", post(originate::<C>))
}

/// GET /calls/stream — SESSION 实时事件 SSE 流（nsbc-esl-web §8#1）。
///
/// C→Web data.sock 推送的 SESSION 事件经 [`AppState::calls_tx`] 广播，此处转成
/// `text/event-stream`。鉴权仍走 protected router 的 Bearer rbac（路径
/// `/calls/stream` 命中 L0 只读）。前端用 fetch 流式消费（EventSource 无法带
/// Authorization 头），event 名统一为 `session`，data 为 `{"kind","payload"}`。
/// 慢消费者落后丢帧时下发 `lag` 事件，前端可整表重拉一次兜底。
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

/// GET /vad/stream — VAD_EVENT 实时 SSE 流（REQ-AI-BARGEIN-001）。
/// 外部 AI 引擎（Go / Python / Node）通过此端点订阅 vad_event JSON，
/// 实现 barge-in 决策（用户说话 → mute AI 侧 → ASR → LLM → TTS → broadcast → unmute）。
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

/// Call-ID 路由层校验：1..127 字节、可见 ASCII（0x21-0x7e），
/// 与核心 obs_control valid_callid()/DLG_CALLID_MAX 对齐，双端各验一次。
fn valid_call_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() < 128
        && id.bytes().all(|b| (0x21..=0x7e).contains(&b))
}

/// 通用 L2 action 成功响应（所有 uuid_* 写操作共用）。
#[derive(Debug, Serialize)]
struct ActionResponse {
    status: &'static str,
    call_id: String,
}

fn action_response(call_id: &str) -> Json<ActionResponse> {
    Json(ActionResponse { status: "ok", call_id: call_id.to_string() })
}

/// GET /api/v1/calls：核心当前呼叫计数器（L0 只读）。
async fn list_calls<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<CallCounters>> {
    Ok(Json(st.core.call_counters().await?))
}

/// GET /api/v1/channels：核心在途呼叫 A/B Leg 摘要（L0 只读，REQ-OAM-007）。
async fn list_channels<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> ApiResult<Json<CallList>> {
    Ok(Json(st.core.list_calls().await?))
}

/// GET /api/v1/calls/{id}：单呼叫双侧 Leg 会话诊断（L0 只读，
/// REQ-OAM-007/010）。Call-ID 不存在/已结束 → 404；核心不可达 → 502/504。
async fn get_call<C: CoreApi + Clone + 'static>(
    State(st): State<Arc<AppState<C>>>,
    Path(id): Path<String>,
) -> ApiResult<Json<CallDetail>> {
    if !valid_call_id(&id) {
        return Err(ApiError::BadRequest("invalid call id".into()));
    }
    Ok(Json(st.core.call_detail(&id).await?))
}

/// POST /api/v1/calls/{id}/hangup —— L1 操作，转发核心
/// `hangup <call-id>`（REQ-OAM-004），核心侧强制审计。
/// 成功 200；Call-ID 不存在/已结束 → 404；核心不可达 → 502/504。
/// 长度上限与核心 DLG_CALLID_MAX-1 对齐（1..127 字节，字符集由核心复验）。
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

// ===== REQ-CCMD-001 L2 命令端点 =====

/// POST /calls/{id}/kill —— uuid_kill [cause]，cause 可选 1-699。
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

/// POST /calls/{id}/answer —— uuid_answer。
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

/// POST /calls/{id}/hold —— uuid_hold on/off。
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

/// POST /calls/{id}/mute —— uuid_mute on/off [leg]（REQ-AI-BARGEIN-001）。
/// barge-in 核心命令：用户开始说话时 AI 引擎调 mute on leg=1（AI 侧），
/// 用户停住后调 mute off 恢复。
#[derive(Debug, Deserialize)]
struct MuteBody {
    /// true → mute on；false → mute off
    on: bool,
    /// 0=A腿(用户) 1=B腿(AI) -1=双腿(默认)
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
        _ => "-1", // -1 或 None → 双腿
    };
    st.core.call_action(&id, "mute", &[flag, leg_str]).await?;
    Ok(action_response(&id))
}

/// POST /calls/{id}/broadcast —— uuid_broadcast <wav_path>（REQ-AI-BARGEIN-001）。
/// AI TTS 合成 WAV 后调用，把音频注入到通话侧播放给用户听。
#[derive(Debug, Deserialize)]
struct BroadcastBody {
    /// WAV 文件路径（nsbcd 进程能访问的绝对路径）
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
    if body.wav_path.is_empty() || body.wav_path.len() > 512 {
        return Err(ApiError::BadRequest("invalid wav_path".into()));
    }
    st.core.call_action(&id, "broadcast", &[&body.wav_path]).await?;
    Ok(action_response(&id))
}

/// POST /calls/{id}/vad-start —— uuid_vad_start <threshold>（REQ-AI-BARGEIN-001）。
/// B 腿 answer 后 AI 引擎调此命令启动 VAD 监控，之后 state.vad_tx 开始推 VAD_EVENT。
#[derive(Debug, Deserialize)]
struct VadStartBody {
    /// RMS 阈值（int16 PCM 振幅），默认 800，静音 < 300，正常说话 > 1000
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

/// POST /calls/{id}/vad-stop —— uuid_vad_stop（REQ-AI-BARGEIN-001）。
/// 通话结束前清理 VAD 监控。
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

/// POST /calls/{id}/transfer —— uuid_transfer dest。
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
    if body.dest.is_empty() || body.dest.len() > 512 {
        return Err(ApiError::BadRequest("invalid dest".into()));
    }
    st.core.call_action(&id, "transfer", &[&body.dest]).await?;
    Ok(action_response(&id))
}

/// POST /calls/{id}/record —— uuid_record start|stop [path]。
#[derive(Debug, Deserialize)]
struct RecordBody {
    /// start | stop
    action: String,
    /// start 时必填
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
                .filter(|p| !p.is_empty() && p.len() <= 512)
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

/// POST /calls/{id}/send-dtmf —— uuid_send_dtmf digits（0-9*#A-D，≤32）。
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

/// POST /calls/{id}/send-msg —— send_msg <call-id> <body>（MESSAGE 转发）。
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

/// POST /originate —— originate <call-url>。
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
