//! AI Agent 核心模块（REQ-AI-BARGEIN-001 P0）。
//!
//! 订阅 data.sock 推送的 VAD_EVENT（通过 state.vad_tx broadcast channel），
//! 实现用户打断 AI 说话（barge-in）：
//!   SILENCE→SPEECH（用户开始说话）→ uuid_mute call-id on 1（静音 AI 侧）
//!   SPEECH→SILENCE（用户停住）     → uuid_mute call-id off 1（恢复）
//!
//! P0：只做 mute 切换 + tracing 日志（证明链路通）。
//! P1：接 AI ASR（hep_sink）+ LLM + TTS（uuid_broadcast）完整对话循环。
//!
//! 架构：
//! ```
//! nsbcd vad_engine ──uds_send_vad──► uds_server.rs dispatch_push
//!                                            │
//!                                   "vad_event" kind
//!                                            │
//!                                   state.vad_tx.send()  (broadcast)
//!                                            │
//!                                   ai_agent.rs subscribe ──► barge-in 决策
//! ```

use std::sync::Arc;

use tokio::sync::broadcast;
use tracing::{debug, info, warn};

use crate::coreclient::CoreApi;
use crate::AppState;

/// 启动 AI Agent barge-in 后台任务。订阅 vad_tx channel，收到 VAD_EVENT 后
/// 调 control.sock 的 uuid_mute 命令实现静音切换。
pub fn spawn<C>(state: Arc<AppState<C>>) -> tokio::task::JoinHandle<()>
where
    C: CoreApi + Clone + 'static,
{
    // Subscribe 时跳过历史（只看未来事件），避免启动瞬间 burst 处理。
    let mut rx = state.vad_tx.subscribe();
    // 丢掉 Lagged 历史（只关心实时 VAD 状态变化）
    let _ = rx.recv();

    tokio::spawn(async move {
        info!("ai_agent: barge-in listener started (P0: VAD_EVENT → uuid_mute)");

        loop {
            match rx.recv().await {
                Ok(raw) => {
                    if let Err(e) = handle_vad_event(&state, &raw).await {
                        warn!(error = %e, "ai_agent: VAD_EVENT handling error");
                    }
                }
                Err(broadcast::error::RecvError::Closed) => {
                    info!("ai_agent: vad_tx channel closed, exiting");
                    break;
                }
                Err(broadcast::error::RecvError::Lagged(n)) => {
                    debug!(skipped = n, "ai_agent: lagged, skipped old VAD_EVENTs");
                    // Lagged 时丢掉——VAD 是瞬时状态，追历史没意义
                }
            }
        }

        info!("ai_agent: barge-in listener stopped");
    })
}

/// 解析单条 VAD_EVENT + 执行 barge-in 决策。
///
/// C 侧 vad_engine 推的 JSON 结构：
///   {"kind":"vad_event","payload":{"event":"VAD_EVENT","call_key":"0x7f...","leg":0,
///    "state":"SPEECH|SILENCE","rms":1234,"ts_ns":...}}
///
/// leg=0 = A 腿（主叫侧 / 用户侧），leg=1 = B 腿（被叫侧 / AI 侧）。
/// 用户侧 leg=0 检测到说话 → 应该静音 AI 侧 leg=1。
///
/// P0 简化：只打 tracing 日志（链路验证）。
/// P1 升级：call_key→call-id 映射（从 SESSION 事件提取）→ call_action mute on/off。
///   C 侧 call_key 是 b2b_call_t* 指针地址（"0x..."），control.sock 的 uuid_mute 命令
///   期望 b2b_leg.callid 字符串——两者不同。需要在 Rust 侧维护映射表。
async fn handle_vad_event<C>(
    state: &Arc<AppState<C>>,
    raw: &str,
) -> Result<(), String>
where
    C: CoreApi + Clone + 'static,
{
    let root: serde_json::Value = serde_json::from_str(raw)
        .map_err(|e| format!("invalid VAD_EVENT JSON: {e}"))?;

    // 兼容：有时 raw 就是 payload 本身（没 kind 包裹）
    let payload = if let Some(p) = root.get("payload") {
        p.clone()
    } else {
        root.clone()
    };

    let call_key = payload
        .get("call_key")
        .and_then(|v| v.as_str())
        .unwrap_or("");
    let leg = payload
        .get("leg")
        .and_then(|v| v.as_i64())
        .unwrap_or(-1);
    let state_str = payload
        .get("state")
        .and_then(|v| v.as_str())
        .unwrap_or("");
    let rms = payload
        .get("rms")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);

    // 只处理 A 腿（用户侧）的 VAD 状态变化
    if leg != 0 {
        return Ok(());
    }

    info!(call_key, state = state_str, rms, "ai_agent: VAD_EVENT (A leg, user side)");

    match state_str {
        "SPEECH" => {
            // 用户开始说话 → barge-in 触发
            // P0: 只记录日志（call_key 是 C 指针，暂不调 control.sock）
            // P1: call_action(call_key, "mute", &["on", "1"]) —— 需要 call-id 映射
            info!(call_key, rms, "ai_agent: barge-in trigger → should mute AI side (P0: log only)");
        }
        "SILENCE" => {
            // 用户停住 → 恢复
            info!(call_key, rms, "ai_agent: silence → should unmute AI side (P0: log only)");
        }
        _ => { /* 未知状态忽略 */ }
    }

    Ok(())
}
