//! CDR polling background poller (REQ-WEB-014 / REQ-OBS-006 P1).
//!
//! The core has not yet wired obs_sub TCP push into Web's UDS short-connection client
//! (would require a long-lived connection lifecycle refactor), so we fall back to polling:
//! every `interval` seconds fetch `cdr.recent N`, write into the `cdr` table
//! (ON CONFLICT DO NOTHING for idempotent dedup). If the core's `cdr.recent` is not
//! implemented → CoreRejected (ERR unknown command), the poller emits only one
//! initial warn then silently skips to avoid spamming logs.
use std::collections::HashSet;
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::Duration;

use tokio::time::MissedTickBehavior;
use tracing::{debug, warn};

use crate::coreclient::{CoreApi, CoreError};
use crate::AppState;

/// Spawn the poller background task. `interval_s` recommends 10-30s; `limit_per_poll` is
/// how many entries to fetch each time (upper bound of the core-side cdr.recent [N] command,
/// default 100). Silently skip writes when DB is not configured.
pub fn spawn<C>(
    state: Arc<AppState<C>>,
    interval_s: u64,
    limit_per_poll: u32,
) -> tokio::task::JoinHandle<()>
where
    C: CoreApi + Clone + 'static,
{
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(Duration::from_secs(interval_s.max(1)));
        // Merge missed ticks and fire immediately (machine suspend/resume or GC pause).
        interval.set_missed_tick_behavior(MissedTickBehavior::Skip);
        // Fire the first tick immediately (don't wait for the first interval).
        interval.tick().await;

        let mut not_implemented_warned = false;
        let mut last_unsupported_warn = false;
        let mut consecutive_failures: u32 = 0;

        loop {
            interval.tick().await;

            // If the C→Web data.sock channel is already established (set true on uds_server
            // accept) → skip polling (write-through real-time insertion is more timely
            // than poller; UDS disconnect does NOT automatically fall back to polling —
            // nsbc-web needs restart; avoids duplicate/out-of-order writes from two paths).
            if state.data_pushed.load(Ordering::Relaxed) {
                continue;
            }

            match state.core.cdr_recent(limit_per_poll).await {
                Ok(cdrs) => {
                    // Reset consecutive failure counter on success; next failure will
                    // be reported immediately
                    consecutive_failures = 0;
                    if cdrs.is_empty() { continue; }
                    debug!(count = cdrs.len(), "cdr poller got {} entries", cdrs.len());

                    // Dedup within a batch (same poll may contain duplicate call_ids).
                    let mut seen = HashSet::with_capacity(cdrs.len());
                    for c in cdrs {
                        if !seen.insert(c.call_id.clone()) { continue; }
                        if let Err(e) = state
                            .db
                            .insert_cdr(
                                &c.call_id,
                                c.tenant.as_deref(),
                                &c.from_num,
                                &c.to_num,
                                c.trunk.as_deref(),
                                c.rec_path.as_deref(),
                                &c.dir,
                                &c.phase,
                                &c.disposition,
                                c.start_ms,
                                c.answer_ms,
                                c.end_ms,
                                c.billsec,
                                c.duration_s,
                                c.cause,
                                c.emergency,
                                None, // from_ip: poller fallback path can't get it, only UDS full push
                                None, // codec_a
                                None, // codec_b
                                None, // a_peer_ip
                                None, // b_peer_ip
                            )
                            .await
                        {
                            // Single insert failed (unique-key conflicts already filtered;
                            // real failure is backend write error): warn once, don't interrupt
                            // the batch.
                            warn!(error = %e, call_id = %c.call_id, "cdr insert failed");
                        }
                    }
                }
                Err(CoreError::Unsupported(reason)) => {
                    // UDS is unavailable on non-Unix platforms (Windows dev machine);
                    // report once every 60s is enough.
                    if !last_unsupported_warn {
                        warn!(error = %reason, "cdr poller skipped: RealCore not supported on this platform");
                        last_unsupported_warn = true;
                    }
                }
                Err(CoreError::CoreRejected(msg)) if msg.contains("unknown") => {
                    // Core-side cdr.recent command not implemented yet; only report once.
                    if !not_implemented_warned {
                        warn!("cdr poller skipped: core does not implement cdr.recent yet ({msg})");
                        not_implemented_warned = true;
                    }
                }
                Err(e) => {
                    // Other errors (I/O, timeout, protocol, UDS missing): report immediately
                    // on first failure, then only every 16th occurrence (default interval=15s →
                    // ~4 min) to avoid log spamming when core is down. Reset immediately on
                    // success.
                    consecutive_failures = consecutive_failures.saturating_add(1);
                    if consecutive_failures == 1 || consecutive_failures % 16 == 0 {
                        warn!(error = %e, consecutive_failures,
                              "cdr poller fetch failed (core offline or UDS missing)");
                    }
                }
            }
        }
    })
}
