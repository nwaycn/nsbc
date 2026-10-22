//! AI Agent core module (REQ-AI-BARGEIN-001 P0).
//!
//! Subscribes to VAD_EVENT pushed by data.sock (via state.vad_tx broadcast channel),
//! implements user interrupting AI speech (barge-in):
//!   SILENCE→SPEECH (user starts speaking) → uuid_mute call-id on 1 (mute AI side)
//!   SPEECH→SILENCE (user pauses)        → uuid_mute call-id off 1 (resume)
//!
//! P0: only mute switching + tracing logs (prove the path works).
//! P1: wire up AI ASR (hep_sink) + LLM + TTS (uuid_broadcast) for the full dialogue loop.
//!
//! Architecture:
//! ```
//! nsbcd vad_engine ──uds_send_vad──► uds_server.rs dispatch_push
//!                                            │
//!                                   "vad_event" kind
//!                                            │
//!                                   state.vad_tx.send()  (broadcast)
//!                                            │
//!                                   ai_agent.rs subscribe ──► barge-in decision
//! ```

use std::sync::Arc;

use tokio::sync::broadcast;
use tracing::{debug, info, warn};

use crate::coreclient::CoreApi;
use crate::AppState;

/// Spawn the AI Agent barge-in background task. Subscribes to the vad_tx channel;
/// on receiving VAD_EVENT it issues uuid_mute commands via control.sock to toggle muting.
pub fn spawn<C>(state: Arc<AppState<C>>) -> tokio::task::JoinHandle<()>
where
    C: CoreApi + Clone + 'static,
{
    // Skip history when subscribing (only care about future events) to avoid burst
    // processing at startup.
    let mut rx = state.vad_tx.subscribe();
    // Drop the Lagged history (only care about real-time VAD state changes)
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
                    // Drop on lagged — VAD is an instantaneous state, catching up on history
                    // has no value
                }
            }
        }

        info!("ai_agent: barge-in listener stopped");
    })
}

/// Parse a single VAD_EVENT + execute barge-in decision.
///
/// JSON structure pushed by the C-side vad_engine:
///   {"kind":"vad_event","payload":{"event":"VAD_EVENT","call_key":"0x7f...","leg":0,
///    "state":"SPEECH|SILENCE","rms":1234,"ts_ns":...}}
///
/// leg=0 = A leg (caller side / user side), leg=1 = B leg (callee side / AI side).
/// When the user-side leg=0 detects speech → we should mute the AI-side leg=1.
///
/// P0 simplified: only emit tracing logs (link verification).
/// P1 upgrade: call_key→call-id mapping (extracted from SESSION events) → call_action mute on/off.
///   The C-side call_key is a b2b_call_t* pointer address ("0x..."), while control.sock's
///   uuid_mute command expects a b2b_leg.callid string — they are different. We need to maintain
///   a mapping table on the Rust side.
async fn handle_vad_event<C>(
    state: &Arc<AppState<C>>,
    raw: &str,
) -> Result<(), String>
where
    C: CoreApi + Clone + 'static,
{
    let root: serde_json::Value = serde_json::from_str(raw)
        .map_err(|e| format!("invalid VAD_EVENT JSON: {e}"))?;

    // Compatibility: sometimes raw is the payload itself (no "kind" wrapper)
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

    // Only process VAD state changes on the A leg (user side)
    if leg != 0 {
        return Ok(());
    }

    info!(call_key, state = state_str, rms, "ai_agent: VAD_EVENT (A leg, user side)");

    match state_str {
        "SPEECH" => {
            // User starts speaking → barge-in triggered
            // P0: only log (call_key is a C pointer, don't call control.sock yet)
            // P1: call_action(call_key, "mute", &["on", "1"]) — needs call-id mapping
            info!(call_key, rms, "ai_agent: barge-in trigger → should mute AI side (P0: log only)");
        }
        "SILENCE" => {
            // User pauses → resume
            info!(call_key, rms, "ai_agent: silence → should unmute AI side (P0: log only)");
        }
        _ => { /* Unknown state, ignore */ }
    }

    Ok(())
}
