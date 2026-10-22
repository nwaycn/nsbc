//! Security / ops audit (REQ-WEB-010).
//!
//! **Physically isolated** from business tracing logs: audit events are appended as JSON Lines to a dedicated file
//! (O_APPEND, independent rotation/retention policy on the deploy side, default retention 1 year); not rotated together with business logs
//! and not overwritten. Each event is flushed immediately on disk (management-plane low frequency, durability prioritized over throughput).
//!
//! Failure policy: write failures such as disk-full / permission-error only log tracing::error, do NOT block business requests
//! (fail-open for availability; audit outages are visible to ops, management plane must not become entirely unavailable,
//! this tradeoff is recorded in state §6).
//!
//! When NSBC_AUDIT_FILE is not configured, sink is disabled (warning at startup) — development/testing posture;
//! production deployments must configure it.

use std::fs::{File, OpenOptions};
use std::io::{BufWriter, Write};
use std::path::Path;
use std::sync::Mutex;

use serde_json::{json, Value};

use crate::auth::tokens::now_secs;

#[derive(Default)]
pub struct AuditLog {
    sink: Option<Mutex<BufWriter<File>>>,
}

impl AuditLog {
    /// Disabled (no audit file).
    pub fn disabled() -> AuditLog {
        AuditLog { sink: None }
    }

    /// Open (create if not exists, append-only); return Err for invalid/unwritable path (fail-closed at startup).
    pub fn open(path: &str) -> Result<AuditLog, String> {
        let f = OpenOptions::new()
            .create(true)
            .append(true)
            .open(Path::new(path))
            .map_err(|e| format!("open audit file {path}: {e}"))?;
        Ok(AuditLog {
            sink: Some(Mutex::new(BufWriter::new(f))),
        })
    }

    pub fn is_enabled(&self) -> bool {
        self.sink.is_some()
    }

    /// Record an event. `detail` fields are flattened into event JSON; names colliding with reserved fields are ignored.
    pub fn record(&self, event: &str, sub: Option<&str>, peer: &PeerCtx, detail: Value) {
        let Some(sink) = &self.sink else {
            return;
        };
        let mut o = match detail {
            Value::Object(m) => m,
            _ => serde_json::Map::new(),
        };
        // Fixed fields take precedence; do not allow detail to overwrite.
        o.insert("ts".into(), json!(now_secs()));
        o.insert("event".into(), json!(event));
        o.insert("sub".into(), json!(sub));
        o.insert("ip".into(), json!(peer.ip));
        o.insert("ua".into(), json!(peer.ua));
        let line = match serde_json::to_vec(&Value::Object(o)) {
            Ok(mut v) => {
                v.push(b'\n');
                v
            }
            Err(e) => {
                tracing::error!(error = %e, "audit event serialize failed");
                return;
            }
        };
        if let Ok(mut w) = sink.lock() {
            if let Err(e) = w.write_all(&line).and_then(|_| w.flush()) {
                tracing::error!(error = %e, event, "audit write failed");
            }
        } else {
            tracing::error!("audit mutex poisoned; event lost: {event}");
        }
    }
}

/// Request-source context (constructed by rate-limit middleware then injected into extensions, reused across the full chain).
#[derive(Clone, Debug, Default)]
pub struct PeerCtx {
    /// Peer IP; "-" when ConnectInfo is absent (tests / non-TCP access)
    pub ip: String,
    /// User-Agent, truncated to 200 chars
    pub ua: String,
}

impl PeerCtx {
    pub fn new(ip: Option<String>, ua: Option<&str>) -> PeerCtx {
        PeerCtx {
            ip: ip.unwrap_or_else(|| "-".to_string()),
            ua: ua.unwrap_or("").chars().take(200).collect(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn unique_path() -> std::path::PathBuf {
        let n = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!("nsbc_audit_{}_{}.jsonl", std::process::id(), n))
    }

    #[test]
    fn writes_json_lines_and_fixed_fields_win() {
        let path = unique_path();
        let p = path.to_string_lossy().to_string();
        let log = AuditLog::open(&p).unwrap();
        assert!(log.is_enabled());
        let peer = PeerCtx::new(Some("10.0.0.9".into()), Some("curl/8"));
        log.record(
            "login_success",
            Some("u0"),
            &peer,
            json!({"method":"password", "event":"forged"}),
        );
        log.record(
            "rate_limited",
            None,
            &peer,
            json!({"bucket":"login","retry_after":42}),
        );
        drop(log);
        let text = std::fs::read_to_string(&path).unwrap();
        let lines: Vec<Value> = text.lines().map(|l| serde_json::from_str(l).unwrap()).collect();
        assert_eq!(lines.len(), 2);
        assert_eq!(lines[0]["event"], "login_success");
        assert_eq!(lines[0]["sub"], "u0");
        assert_eq!(lines[0]["ip"], "10.0.0.9");
        assert_eq!(lines[0]["ua"], "curl/8");
        assert_eq!(lines[0]["method"], "password");
        assert!(lines[0]["ts"].as_u64().unwrap() > 0);
        assert_eq!(lines[1]["retry_after"], 42);
        assert!(lines[1]["sub"].is_null());
        std::fs::remove_file(&path).unwrap();
    }

    #[test]
    fn disabled_sink_is_silent() {
        let log = AuditLog::disabled();
        assert!(!log.is_enabled());
        log.record("x", None, &PeerCtx::default(), json!({})); // must not panic
    }
}
