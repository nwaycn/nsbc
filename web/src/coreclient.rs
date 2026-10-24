//! Web→core realtime control channel client (REQ-WEB-004 primary, REQ-WEB-013/014 auxiliary).
//!
//! The sole real channel: the core's obs_control UNIX domain stream socket
//! (cfg `oam.control_sock: unix:<path>`, file mode 0600, privileged local account only).
//! Line-text protocol, each frame terminated by prompt `nsbc> `:
//!   Connect banner: `NSBC <v> control interface (try 'help')\nnsbc> `
//!   `version`             → `nsbcd <v>\n`
//!   `show calls`          → `active=N total=N failed=N\n`
//!   `show channels`       → one line per call `call a_callid=.. a_state=.. …`
//!                           + trailing summary line `channels=N truncated=0|1`
//!   `uuid_dump <call-id>` → per-Leg `leg N state/proto/cseq/tags/media/…`
//!                           SDP segment length-framed (`leg N sdp_offer <len>` followed
//!                           by raw bytes)
//!   `config.get <key>`    → header line `config key=.. layer=.. secret=0|1`
//!                           + value on its own line (sensitive keys: secret=1, value is ****)
//!   `config.export`       → header line `config-export keys=N redact=1`
//!                           + one key per line `<dotted.key>: <value|****>`
//!   `show registrations`  → `registrations=N\n`
//!   `stats`               → Prometheus exposition (`# HELP/# TYPE` + `name value`)
//! Core-side rejections start with an `ERR` line. Read-only queries go through request();
//! write operations (hangup) and read-only commands that need their own notfound branching
//! (uuid_dump/config.get) go through send_cmd; Web-side write entry points like config.set
//! are not yet exposed (P1 unified command registry + Idempotency-Key).
//!
//! Fault tolerance (REQ-WEB-014): connect / I/O / timeout / protocol errors are explicitly
//! classified; routes map them to 502/504. The sidecar does not cache or fabricate core data.
//! All parsers are pure functions with cross-platform unit tests; UDS transport only compiles
//! under `cfg(unix)`, Windows development builds return [`CoreError::Unsupported`].
use std::collections::BTreeMap;
use std::fmt;
use std::future::Future;
use std::path::{Path, PathBuf};
use std::time::Duration;

use serde::Serialize;

/// Core control socket default path (matches configs/nsbc.yaml oam.control_sock).
pub const DEFAULT_SOCK_PATH: &str = "/var/run/nsbc/control.sock";

const PROMPT: &[u8] = b"nsbc> ";
/// stats exposition capped at 256KB on the core side; frame limit leaves ample headroom.
/// Only consumed by the cfg(unix) transport read path; Windows dev builds have no consumer.
#[cfg_attr(not(unix), allow(dead_code))]
const MAX_FRAME: usize = 512 * 1024;
const DEFAULT_TIMEOUT_MS: u64 = 2000;

/// Error classification for core control-plane interaction (REQ-WEB-014).
/// Connect/Io/Timeout are only constructed by the cfg(unix) real transport; under
/// Windows builds the classification is still covered by mock unit tests and the
/// route 502/504 mapping, so non-unix targets allow dead_code.
#[derive(Clone, Debug, PartialEq, Eq)]
#[cfg_attr(not(unix), allow(dead_code))]
pub enum CoreError {
    /// Failed to connect to the control socket (core not running / permission denied / path absent)
    Connect(String),
    /// Stream I/O failure after connect (peer reset, etc.)
    Io(String),
    /// Single request timeout
    Timeout,
    /// Target object not found (e.g. Call-ID for hangup already ended/absent) → HTTP 404
    NotFound(String),
    /// Protocol violation: truncated frame, invalid UTF-8, output did not match contract
    Protocol(String),
    /// Core returned an ERR line
    CoreRejected(String),
    /// Current build target does not support this transport (Windows has no UDS control socket)
    Unsupported(&'static str),
}

impl fmt::Display for CoreError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CoreError::Connect(e) => write!(f, "core connect failed: {e}"),
            CoreError::Io(e) => write!(f, "core socket io error: {e}"),
            CoreError::Timeout => f.write_str("core control request timed out"),
            CoreError::NotFound(w) => write!(f, "core object not found: {w}"),
            CoreError::Protocol(e) => write!(f, "core protocol error: {e}"),
            CoreError::CoreRejected(e) => write!(f, "core rejected: {e}"),
            CoreError::Unsupported(e) => write!(f, "unsupported core transport: {e}"),
        }
    }
}

impl std::error::Error for CoreError {}

/// `version` response.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct VersionInfo {
    /// Core version string (e.g. "2.5.5")
    pub core: String,
}

/// `show calls` counters (REQ-WEB-004 realtime traffic read-only subset).
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct CallCounters {
    pub active: u64,
    pub total: u64,
    pub failed: u64,
}

/// Single dialplan Application (show apps per line "name  help").
/// REQ-DP-APP-001 / REQ-AI-001: used by the front-end app_chain editor dropdown.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct AppInfo {
    pub name: String,
    pub help: String,
}

/// Single Codec description (show codecs per line: name,pt,sample_rate,impl,fmtp).
/// Source: core-side codec_registry self-registered entries; mod_codecs (G711/G729),
/// mod_av (G722/Opus/H264/VP8/VP9), or future iLBC/AMR modules fill them at init.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct CodecInfo {
    /// Codec name, e.g. "PCMA" / "G729" / "G722" / "Opus" / "H264"
    pub name: String,
    /// RTP Payload Type (static 0-95 or dynamic 96+)
    pub pt: u8,
    /// Sample rate (Hz): 8000 / 16000 / 48000 / 90000 (video)
    pub sample_rate: u32,
    /// Registering module name, e.g. "mod_codecs" / "mod_av" — helps ops judge which .so
    /// provided this codec
    pub impl_module: String,
    /// Optional default fmtp, e.g. "annexb=yes" / "maxplaybackrate=16000" / "packetization-mode=0"
    pub fmtp_default: String,
    /// Media type: "audio" / "video" — front-end extension editor groups codec dropdown by type
    pub media_type: String,
}

/// Parse show codecs output:
///   codecs=N
///   g711a PT=8 rate=8000 ch=1 ptime=20ms
///   g729  PT=18 rate=8000 ch=1 ptime=20ms
///   g722  PT=9 rate=16000 ch=1 ptime=20ms
///   opus  PT=111 rate=48000 ch=2 ptime=20ms
/// (obs_control.c cmd_show_codecs emits k=v tokens per codec line;
///  legacy 6-column "NAME PT SRATE IMPL FMTP MEDIA" lines are also accepted)
fn parse_show_codecs(buf: &str) -> Vec<CodecInfo> {
    let mut out = Vec::new();
    for line in buf.lines() {
        let trimmed = line.trim();
        if trimmed.is_empty() { continue; }
        if let Some(rest) = trimmed.strip_prefix("codecs=") {
            // Header line codecs=N
            if rest.trim().is_empty() { continue; }
            continue;
        }
        // Skip header/prompt lines
        let low = trimmed.to_ascii_lowercase();
        if low.starts_with("name") || low.starts_with('(') { continue; }

        let parts: Vec<&str> = trimmed.split_whitespace().collect();
        if parts.len() < 2 { continue; }
        let name = parts[0].to_string();

        // Format 1 (current C output): NAME PT=8 rate=8000 ch=1 ptime=20ms
        if let Some(info) = parse_codec_kv_line(&name, &parts[1..]) {
            out.push(info);
            continue;
        }

        // Format 2 (legacy columns): NAME PT SRATE IMPL FMTP MEDIA_TYPE
        // (FMTP can contain spaces like "annexb=yes")
        if parts.len() < 5 { continue; }
        let pt: u8 = match parts[1].parse() { Ok(v) => v, Err(_) => continue };
        let sample_rate: u32 = match parts[2].parse() { Ok(v) => v, Err(_) => continue };
        let impl_module = parts[3].to_string();
        // MEDIA_TYPE fixed at last column; FMTP is the concatenation of the middle columns
        let media_type = parts.last().unwrap_or(&"audio").to_string();
        let fmtp_default = if parts.len() >= 6 {
            parts[4..parts.len()-1].join(" ")
        } else {
            "-".to_string()
        };

        out.push(CodecInfo { name, pt, sample_rate, impl_module, fmtp_default, media_type });
    }
    out
}

/// Parse k=v tokens of the C-side `show codecs` line, e.g. `PT=8 rate=8000 ch=1 ptime=20ms`.
/// PT is required; rate defaults to 8000; ch/ptime are informational (not in CodecInfo).
fn parse_codec_kv_line(name: &str, tokens: &[&str]) -> Option<CodecInfo> {
    let mut pt: Option<u8> = None;
    let mut sample_rate: u32 = 8000;
    let mut has_kv = false;
    for t in tokens {
        if let Some(v) = t.strip_prefix("PT=") {
            pt = v.parse().ok();
            has_kv = true;
        } else if let Some(v) = t.strip_prefix("rate=") {
            if let Ok(r) = v.parse() { sample_rate = r; }
            has_kv = true;
        }
    }
    let pt = pt?;
    if !has_kv { return None; }
    Some(CodecInfo {
        name: name.to_string(),
        pt,
        sample_rate,
        impl_module: "core".to_string(),
        fmtp_default: "-".to_string(),
        media_type: "audio".to_string(),
    })
}

/// Parse show apps output: header line apps=N, then each subsequent line "name  help"
/// (help can be empty).
fn parse_show_apps(buf: &str) -> Vec<AppInfo> {
    let mut out = Vec::new();
    for line in buf.lines() {
        if let Some(rest) = line.strip_prefix("apps=") {
            if rest.trim().is_empty() { continue; }
            continue;
        }
        let trimmed = line.trim();
        if trimmed.is_empty() { continue; }
        // Skip bracket prompt "(no app registry...)"
        if trimmed.starts_with('(') && trimmed.ends_with(')') { continue; }
        // "name  help" — split on the first double space; help may contain single spaces
        let mut parts = trimmed.splitn(2, "  ");
        let name = parts.next().unwrap_or("").trim().to_string();
        let help = parts.next().unwrap_or("").trim().to_string();
        if !name.is_empty() {
            out.push(AppInfo { name, help });
        }
    }
    out
}

/// Single registration binding (show registrations per line).
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct BindingEntry {
    pub aor: String,
    pub user: String,
    pub contact: String,
    pub expires_sec: Option<i64>,
    pub alive_sec: Option<i64>,
    pub q: Option<i64>,
    /* === NAT metadata (REQ-NAT-001) ===
     * C-side obs_control.c cmd_reg_line_cb output: nat=N transport=X received_ip=X received_port=N pub_contact=X */
    pub nat_detected: Option<i64>,
    pub transport: String,
    pub received_ip: String,
    pub received_port: Option<i64>,
    pub public_contact: String,
}

/// `show registrations` full output: one line per binding + trailing summary line.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct RegistrationSummary {
    pub bindings: Vec<BindingEntry>,
    pub registrations: u64,
    pub truncated: bool,
}

/// Effective-value view of `config.get <key>` (REQ-CFG-011).
/// When secret=true, value is always None (core side masks with ****; Web does not echo placeholders);
/// layer indicates the matched config layer (default/system/env/profile/runtime/session).
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ConfigValue {
    pub key: String,
    pub value: Option<String>,
    pub layer: String,
    pub secret: bool,
}

/// Single key item in the full `config.export` snapshot (REQ-CFG-013).
/// When secret=true under desensitized export, value is always None (core side masks with ****;
/// placeholders do not leave the sidecar).
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ConfigExportItem {
    pub key: String,
    pub value: Option<String>,
    pub secret: bool,
}

/// Desensitized full effective view of `config.export` (GET /config).
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct ConfigExport {
    pub redacted: bool,
    pub items: Vec<ConfigExportItem>,
}

/// `show channels` single row: A/B Leg summary of one in-progress call (REQ-OAM-007).
/// Missing legs (e.g. originate with only B leg in the round) or fields not collected by core are None.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct CallSummary {
    pub a_call_id: Option<String>,
    pub a_state: Option<String>,
    pub b_call_id: Option<String>,
    pub b_state: Option<String>,
    pub caller: Option<String>,
    pub callee: Option<String>,
    pub start_ms: Option<u64>,
    pub answer_ms: Option<u64>,
    pub duration_ms: Option<u64>,
    pub codec_a: Option<String>,
    pub codec_b: Option<String>,
    pub rtp_loss_pct: Option<u32>,
    pub rtp_jitter_ms: Option<u32>,
    pub mos_x1000: Option<i32>,
}

/// Full `show channels` snapshot (REQ-OAM-007: L0 read-only). `truncated=true` means
/// the core-side buffer was insufficient and the tail was clipped; the caller must
/// surface this to the front-end so operators know the list is partial.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct CallList {
    pub calls: Vec<CallSummary>,
    pub truncated: bool,
}

/// Single Leg media plane view (REQ-OBS-010). None = core has no collection source
/// (UNAVAILABLE, e.g. P0 media did not yet attach to b2b or a bypass leg does not transit
/// through this device); strictly forbidden to fill with estimated values.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct LegMedia {
    pub mode: Option<String>,
    pub gate: Option<String>,
    pub ssrc_in: Option<String>,
    pub ssrc_out: Option<String>,
    /// Packet loss/jitter/RTT/R-Factor/MOS and other VQ metrics (P0 always None).
    pub vq: Option<String>,
}

/// Single Leg detail from `uuid_dump`: FSM/transport/CSeq/tag + negotiated SDP raw + media view.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct LegDump {
    pub leg: u8,
    pub state: String,
    pub proto: String,
    pub reliable: bool,
    pub cseq: i64,
    pub routes: u32,
    pub local_tag: Option<String>,
    pub remote_tag: Option<String>,
    pub offer: Option<String>,
    pub answer: Option<String>,
    pub media: LegMedia,
}

/// `uuid_dump <call-id>` dual-leg diagnosis (REQ-OBS-010: does not create a separate ID,
/// anchored on A/B Leg Call-ID; bypass/unconnected fields are expressed truthfully via LegMedia=None).
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct CallDetail {
    pub a_call_id: Option<String>,
    pub a_state: Option<String>,
    pub b_call_id: Option<String>,
    pub b_state: Option<String>,
    pub legs: Vec<LegDump>,
}

/// `stats` Prometheus exposition snapshot: `metric_name -> value`.
/// Current core metrics are all gauge with no labels; label-bearing `{...}` samples are
/// aggregated by metric name (last occurrence wins), for read-only dashboard display.
/// Authoritative counts still come from dedicated commands.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct MetricSnapshot(pub BTreeMap<String, f64>);

/// `config.set` response. `changed=true` means core actually modified the value;
/// `changed=false` means the key already had the new value (idempotent success, not counted as a write).
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ConfigSetResult {
    pub key: String,
    pub changed: bool,
}

/// Ban/watch entry (parsed from the `ent` lines of `security.ban.list`).
/// state: "ban" = banned (remaining_s>0); "watch" = count in window has not reached threshold.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct BanEntry {
    pub ip: String,
    pub state: String,
    pub fails: u32,
    pub remaining_s: u64,
    pub act_ago_s: u64,
}

/// Parse `security.ban.list [filter]` response frame:
/// each line is `ent ip=X state=ban|watch fails=N remaining_s=N act_ago_s=N`,
/// trailing line `bans=N shown=M total=K` (skipped); ERR prefix -> CoreRejected.
pub fn parse_ban_list(body: &str) -> Result<Vec<BanEntry>, CoreError> {
    let mut out = Vec::new();
    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() { continue; }
        if let Some(msg) = line.strip_prefix("ERR") {
            return Err(CoreError::CoreRejected(msg.trim().to_string()));
        }
        let Some(rest) = line.strip_prefix("ent ") else { continue };
        let toks: Vec<&str> = rest.split_whitespace().collect();
        let kv = |k: &str| {
            toks.iter()
                .find(|t| t.starts_with(k) && t.len() > k.len() && t.as_bytes()[k.len()] == b'=')
                .map(|t| &t[k.len() + 1..])
        };
        let (Some(ip), Some(state)) = (kv("ip"), kv("state")) else {
            return Err(CoreError::Protocol("ban ent missing ip/state".into()));
        };
        out.push(BanEntry {
            ip: ip.to_string(),
            state: state.to_string(),
            fails: kv("fails").and_then(|v| v.parse().ok()).unwrap_or(0),
            remaining_s: kv("remaining_s").and_then(|v| v.parse().ok()).unwrap_or(0),
            act_ago_s: kv("act_ago_s").and_then(|v| v.parse().ok()).unwrap_or(0),
        });
    }
    Ok(out)
}

/// Recent CDR from core (parsed single-line output of `cdr.recent` command).
/// Fields align 1:1 with cdr_event_t; `end_ms/billsec/duration_s` etc. are 0 when unanswered.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct CdrEvent {
    pub call_id: String,
    pub tenant: Option<String>,
    pub from_num: String,
    pub to_num: String,
    pub trunk: Option<String>,
    pub rec_path: Option<String>,
    pub dir: String,
    pub phase: String,
    pub disposition: String,
    pub start_ms: u64,
    pub answer_ms: u64,
    pub end_ms: u64,
    pub billsec: u32,
    pub duration_s: u32,
    pub cause: i32,
    pub emergency: bool,
    /* ---- network endpoint + codec (aligns 1:1 with cdr_event_t) ---- */
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub from_ip: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub a_peer_ip: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub b_peer_ip: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub codec_a: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub codec_b: Option<String>,
    /* ---- REQ-SEC-005 threat marking (poller fallback path, aligns with UDS write_cdr) ---- */
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub is_malicious: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub threat_reason: Option<String>,
}

/// `status` overall status snapshot (L1, REQ-CCMD-001, new command added by core cmd_registry).
/// Core output format: `status active_calls=N total=N failed=N registrations=N
/// profiles=N started_ago_ms=U\nalive=1` (first line key-value pairs; `alive=1` is trailing anchor line).
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct StatusInfo {
    pub active_calls: u64,
    pub total_calls: u64,
    pub failed_calls: u64,
    pub registrations: u64,
    pub profiles: u64,
    pub started_ago_ms: u64,
}

/// Single rate entry from `show rates` (aligns with C side rate_entry_t, REQ-OAM-009).
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct RateEntry {
    pub prefix: String,
    pub name: String,
    pub price_per_min_uc: i64,
    pub connect_fee_uc: i64,
    pub bill_inc_s: i32,
    pub min_bill_s: i32,
    pub initial_s: i32,
    pub enabled: bool,
}

/// `show rates` list + summary.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct RateList {
    pub rates: Vec<RateEntry>,
    pub total: i64,
}

/// Single account from `show balances`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct AccountBalance {
    pub key: String,
    pub balance_uc: i64,
    pub reserved_uc: i64,
    pub credit_limit_uc: i64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct AccountList {
    pub accounts: Vec<AccountBalance>,
    pub total: i64,
}

/// Single active bridge session from `show transcode` (REQ-OAM-010).
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct BridgeSession {
    pub idx: i64,
    #[serde(rename = "call_key_ptr")]
    pub call_key_ptr: String,
    pub mode: String,
    pub a_rtp: u16,
    pub b_rtp: u16,
    pub pkts: u64,
    pub drops: u64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct BridgeList {
    pub sessions: Vec<BridgeSession>,
    pub total: i64,
}

/// Core control-plane read-only capabilities. Generic injection (not `dyn`);
/// production uses [`RealCore`], routing tests use mocks; no extra deps like async-trait needed.
///
/// Return type explicitly declared as `impl Future + Send` (not bare `async fn`): axum handler's
/// Future must be Send; bare RPITIT lacks Send constraint, causing generic handlers to not satisfy Handler.
pub trait CoreApi: Send + Sync {
    fn version(&self) -> impl Future<Output = Result<VersionInfo, CoreError>> + Send;
    fn call_counters(
        &self,
    ) -> impl Future<Output = Result<CallCounters, CoreError>> + Send;
    fn registrations(
        &self,
    ) -> impl Future<Output = Result<RegistrationSummary, CoreError>> + Send;
    fn metrics(&self) -> impl Future<Output = Result<MetricSnapshot, CoreError>> + Send;
    /// In-progress call list (`show channels`, L0 read-only, REQ-OAM-007).
    fn list_calls(
        &self,
    ) -> impl Future<Output = Result<CallList, CoreError>> + Send;
    /// Single call dual-leg diagnosis (`uuid_dump <call-id>`, L0 read-only,
    /// REQ-OAM-007/010): Call-ID ended/not found -> [`CoreError::NotFound`].
    fn call_detail(
        &self,
        call_id: &str,
    ) -> impl Future<Output = Result<CallDetail, CoreError>> + Send;
    /// Effective value of a single config key (`config.get <key>`, L0 read-only, REQ-CFG-011):
    /// key not found -> [`CoreError::NotFound`]; sensitive keys value=None/secret=true.
    fn config_get(
        &self,
        key: &str,
    ) -> impl Future<Output = Result<ConfigValue, CoreError>> + Send;
    /// Full desensitized effective view (`config.export`, L0 read-only, REQ-CFG-013):
    /// sensitive keys value=None/secret=true; core rejection -> [`CoreError::CoreRejected`].
    fn config_export(&self)
        -> impl Future<Output = Result<ConfigExport, CoreError>> + Send;
    /// Force hangup (REQ-OAM-004, write op / L1 audit): success Ok; target not found
    /// [`CoreError::NotFound`]; core rejection -> [`CoreError::CoreRejected`].
    fn hangup(
        &self,
        call_id: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// Dialplan Application registry (`show apps`, L0 read-only, REQ-DP-APP-001/REQ-AI-001).
    /// Returns the list of all registered dialplan apps (answer/ai/bridge/...),
    /// empty array when the dialplan module is not loaded. Core side uses `app_help` to output
    /// each line in `name  help` format; Web parses into JSON. */
    fn dialplan_apps(&self)
        -> impl Future<Output = Result<Vec<AppInfo>, CoreError>> + Send;

    /// Pull codec registry from core side (`show codecs`).
    /// W-06 dynamic codec list routing; once C side codec_registry self-registers,
    /// all mod_codecs / mod_av / future iLBC/AMR init entries show up automatically.
    /// Empty array when no codec module loaded (not CoreRejected).
    fn show_codecs(&self)
        -> impl Future<Output = Result<Vec<CodecInfo>, CoreError>> + Send;

    /// Hot-set a config key (`config.set <k> <v>`, L1, REQ-CFG-011, core side audit).
    /// Returns `ConfigSetResult` with applied/changed flags; core rejection ->
    /// [`CoreError::CoreRejected`]; key not found / illegal value etc. -> core ERR.
    fn config_set(
        &self,
        key: &str,
        value: &str,
    ) -> impl Future<Output = Result<ConfigSetResult, CoreError>> + Send;

    /// Dry-run validate a config key (`config.validate <k> <v>`, L0, REQ-CFG-011, not applied).
    /// Ok means key and value passed core-side validation; core rejection -> [`CoreError::CoreRejected`].
    fn config_validate(
        &self,
        key: &str,
        value: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// Pull recent CDR from core side (`cdr.recent [N]`, REQ-OBS-006 P1).
    /// Multi-frame dynamic output, trailing `cdrs=N` anchor line; not implemented on core ->
    /// [`CoreError::CoreRejected`] (ERR unknown command).
    fn cdr_recent(
        &self,
        limit: u32,
    ) -> impl Future<Output = Result<Vec<CdrEvent>, CoreError>> + Send;

    // ===== Security ban management (security.ban.* commands) =====

    /// `security.ban.list [filter]` — ban/watch entries (filter=IP substring search).
    fn security_ban_list(
        &self,
        filter: &str,
    ) -> impl Future<Output = Result<Vec<BanEntry>, CoreError>> + Send;

    /// `security.ban.unban <ip>` — unban (clear all counts and ban state for that IP).
    fn security_ban_unban(
        &self,
        ip: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    // ===== reload commands (Web DB -> C memory, C side implements cmd_register) =====

    /// `extensions.reload <json>` — full extension table replace.
    fn reload_extensions(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `gateways.reload <json>` — full gateway/route group table replace.
    fn reload_gateways(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `trunks.reload <json>` — full trunk table replace.
    fn reload_trunks(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `dialplan-rules.reload <json>` — full dialplan rules table replace.
    fn reload_dialplan_rules(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `queues.reload <json>` — full ACD queue table replace (REQ-PBX-CC-002).
    fn reload_queues(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `profiles.reload <json>` — SIP Profile table RCU atomic replace.
    fn reload_sip_profiles(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `security.blacklist.reload <json>` — full static security blacklist (IP CIDR/UA) table replace.
    fn reload_security_blacklist(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `ha.reload <json>` — reload HA groups (Core no-op, Web DB authoritative).
    fn reload_ha_groups(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `ivr.reload <json>` — reload IVR menus (Core no-op, Web DB authoritative).
    fn reload_ivr_menus(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `route_groups.reload <json>` — reload route groups (Core no-op, Web DB authoritative).
    fn reload_route_groups(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    // ===== REQ-CCMD-001 command enhancements: call-level L2 write ops + originate + status =====

    /// Generic call-level write op (uuid_kill/hold/transfer/record/answer/send_dtmf/
    /// broadcast/break/bridge — routed via cmd_registry).
    /// `action` maps to core command name (e.g. `"kill"` -> `uuid_kill`),
    /// `args` concatenated into subsequent space-separated params. Returns Ok(()) if core accepted;
    /// Call-ID not found -> [`CoreError::NotFound`]; core rejection -> [`CoreError::CoreRejected`].
    fn call_action(
        &self,
        call_id: &str,
        action: &str,
        args: &[&str],
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `send_msg <call-id> <body>` — send MESSAGE within a dialog (REQ-SIP-MSG-001).
    /// Independent from call_action because core command name has no prefix, and body may contain spaces.
    fn send_message(
        &self,
        call_id: &str,
        body: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `originate <call-url>` — initiate a new outbound call (L2). call-url format is defined by Dialplan
    /// originate command (see command enhancement doc REQ-CCMD-001). Core accepts -> Ok(());
    /// illegal params / core rejection -> [`CoreError::CoreRejected`].
    fn originate(
        &self,
        call_url: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `status` — overall status snapshot (L1, REQ-CCMD-001).
    fn status(
        &self,
    ) -> impl Future<Output = Result<StatusInfo, CoreError>> + Send;

    // ===== REQ-OAM-009: billing management read-only =====

    /// `show rates` — list all rate entries (L0).
    fn show_rates(
        &self,
    ) -> impl Future<Output = Result<RateList, CoreError>> + Send;

    /// `show balances` — list all account balances (L0).
    fn show_balances(
        &self,
    ) -> impl Future<Output = Result<AccountList, CoreError>> + Send;

    // ===== REQ-OAM-010: media bridge active sessions =====

    /// `show transcode` — active bridge/relay session list (L0).
    fn show_transcode(
        &self,
    ) -> impl Future<Output = Result<BridgeList, CoreError>> + Send;

    // ===== REQ-OAM-009: billing write commands (rate reload + account open/topup) =====

    /// `rate.reload <json>` — full rate table replace (RCU atomic pointer swap).
    fn reload_rates(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `rate.upsert <json>` — single rate entry upsert (JSON object).
    fn rate_upsert_json(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `rate.delete <prefix>` — delete rate entries matching a prefix.
    fn rate_delete(
        &self,
        prefix: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `account.open <key> <balance_uc> <credit_limit_uc>` — open account.
    fn account_open(
        &self,
        key: &str,
        balance_uc: i64,
        credit_limit_uc: i64,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `account.topup <key> <delta_uc>` — account topup (can be negative = deduction).
    fn account_topup(
        &self,
        key: &str,
        delta_uc: i64,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;
}

/// Real core client: short connection (each command is an independent frame on core side), cheap Clone for sharing config.
#[derive(Clone, Debug)]
pub struct RealCore {
    sock_path: PathBuf,
    timeout: Duration,
}

impl RealCore {
    pub fn new(sock_path: PathBuf, timeout: Duration) -> Self {
        Self { sock_path, timeout }
    }

    /// Strip `unix:` prefix — config.rs resolve_uds_path keeps this prefix,
    /// but tokio::net::UnixStream::connect needs the plain file path.
    fn to_path_buf(s: String) -> PathBuf {
        if let Some(rest) = s.strip_prefix("unix:") {
            PathBuf::from(rest)
        } else {
            PathBuf::from(s)
        }
    }

    /// Env config: `NSBC_CONTROL_SOCK` (default /var/run/nsbc/control.sock),
    /// `NSBC_CORE_TIMEOUT_MS` (default 2000ms, REQ-WEB-014).
    #[allow(dead_code)]
    pub fn from_env() -> Self {
        let sock_path = std::env::var("NSBC_CONTROL_SOCK")
            .map(Self::to_path_buf)
            .unwrap_or_else(|_| PathBuf::from(DEFAULT_SOCK_PATH));
        let timeout = std::env::var("NSBC_CORE_TIMEOUT_MS")
            .ok()
            .and_then(|v| v.parse::<u64>().ok())
            .map(Duration::from_millis)
            .unwrap_or_else(|| Duration::from_millis(DEFAULT_TIMEOUT_MS));
        Self::new(sock_path, timeout)
    }

    /// Construct from WebConfig::CoreSection (env var may still override sock path).
    /// cfg.control_sock may carry `unix:` prefix — strip uniformly.
    pub fn from_config(cfg: &super::config::CoreSection) -> Self {
        let sock_path = std::env::var("NSBC_CONTROL_SOCK")
            .map(Self::to_path_buf)
            .unwrap_or_else(|_| {
                let raw = if cfg.control_sock.is_empty() {
                    DEFAULT_SOCK_PATH.to_string()
                } else {
                    cfg.control_sock.clone()
                };
                Self::to_path_buf(raw)
            });
        let timeout = std::env::var("NSBC_CORE_TIMEOUT_MS")
            .ok()
            .and_then(|v| v.parse::<u64>().ok())
            .map(Duration::from_millis)
            .unwrap_or_else(|| Duration::from_millis(DEFAULT_TIMEOUT_MS));
        Self::new(sock_path, timeout)
    }
}

impl CoreApi for RealCore {
    async fn version(&self) -> Result<VersionInfo, CoreError> {
        parse_version(&request(&self.sock_path, self.timeout, "version").await?)
    }
    async fn call_counters(&self) -> Result<CallCounters, CoreError> {
        parse_call_counters(&request(&self.sock_path, self.timeout, "show calls").await?)
    }
    async fn registrations(&self) -> Result<RegistrationSummary, CoreError> {
        parse_registrations(
            &request(&self.sock_path, self.timeout, "show registrations").await?,
        )
    }
    async fn metrics(&self) -> Result<MetricSnapshot, CoreError> {
        Ok(parse_metrics(
            &request(&self.sock_path, self.timeout, "stats").await?,
        ))
    }
    async fn list_calls(&self) -> Result<CallList, CoreError> {
        parse_call_list(
            &request(&self.sock_path, self.timeout, "show channels").await?,
        )
    }
    async fn call_detail(&self, call_id: &str) -> Result<CallDetail, CoreError> {
        // Same as hangup: notfound must map to 404; use send_cmd, skip the unified ERR pre-check.
        let cmd = format!("uuid_dump {call_id}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_call_detail(&body)
    }
    async fn config_get(&self, key: &str) -> Result<ConfigValue, CoreError> {
        // Read-only but must self-dispatch notfound -> 404 (invalid key is blocked earlier by routing layer).
        let cmd = format!("config.get {key}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_config_value(&body)
    }
    async fn config_export(&self) -> Result<ConfigExport, CoreError> {
        // Pure read-only with no notfound semantics: ERR is pre-checked as CoreRejected by request().
        parse_config_export(
            &request(&self.sock_path, self.timeout, "config.export").await?,
        )
    }
    async fn hangup(&self, call_id: &str) -> Result<(), CoreError> {
        // Build command string as owned first so we do not borrow input params across await; call_id validity
        // (length/charset) is validated twice by routing layer and core; transport layer only sends faithfully.
        let cmd = format!("hangup {call_id}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_hangup(&body)
    }
    async fn dialplan_apps(&self) -> Result<Vec<AppInfo>, CoreError> {
        // show apps -> header apps=N + each line "name  help"; when dialplan module is not loaded
        // output is "(no app registry ...)", parse_show_apps skips it and returns empty array.
        let body = send_cmd(&self.sock_path, self.timeout, "show apps").await?;
        Ok(parse_show_apps(&body))
    }
    async fn show_codecs(&self) -> Result<Vec<CodecInfo>, CoreError> {
        // show codecs -> header codecs=N + each line "NAME PT SRATE IMPL FMTP MEDIA";
        // when codec module not loaded output is "(no codec registry ...)", parse_show_codecs
        // skips it and returns empty array. C side codec_registry self-registering entries auto-display.
        let body = send_cmd(&self.sock_path, self.timeout, "show codecs").await?;
        Ok(parse_show_codecs(&body))
    }
    async fn config_set(&self, key: &str, value: &str) -> Result<ConfigSetResult, CoreError> {
        // split_kv splits key from value at first space; value may contain spaces;
        // Empty value (deletion) is expressed by a trailing space in `config.set <key> `.
        let cmd = format!("config.set {key} {value}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_config_set(&body, key)
    }
    async fn config_validate(&self, key: &str, value: &str) -> Result<(), CoreError> {
        let cmd = format!("config.validate {key} {value}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_config_validate(&body)
    }
    async fn cdr_recent(&self, limit: u32) -> Result<Vec<CdrEvent>, CoreError> {
        let cmd = if limit > 0 { format!("cdr.recent {limit}") } else { "cdr.recent".into() };
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_cdr_recent(&body)
    }
    async fn security_ban_list(&self, filter: &str) -> Result<Vec<BanEntry>, CoreError> {
        let cmd = if filter.is_empty() {
            "security.ban.list".to_string()
        } else {
            format!("security.ban.list {filter}")
        };
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_ban_list(&body)
    }
    async fn security_ban_unban(&self, ip: &str) -> Result<(), CoreError> {
        let cmd = format!("security.ban.unban {ip}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        if body.trim_start().starts_with("OK") {
            Ok(())
        } else {
            // "ERR security.ban.unban ip=X not found" etc.
            let msg = body.trim().strip_prefix("ERR").unwrap_or(body.trim());
            Err(CoreError::CoreRejected(msg.trim().to_string()))
        }
    }

    async fn reload_extensions(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("extensions.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_gateways(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("gateways.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_trunks(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("trunks.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_dialplan_rules(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("dialplan-rules.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_queues(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("queues.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_sip_profiles(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("profiles.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_security_blacklist(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("security.blacklist.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }

    async fn reload_ha_groups(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("ha.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_ivr_menus(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("ivr.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_route_groups(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("route_groups.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }

    async fn call_action(
        &self,
        call_id: &str,
        action: &str,
        args: &[&str],
    ) -> Result<(), CoreError> {
        // Build `uuid_<action> <call_id> <args...>`
        let args_str = if args.is_empty() {
            String::new()
        } else {
            args.iter().cloned().collect::<Vec<_>>().join(" ")
        };
        let cmd = if args_str.is_empty() {
            format!("uuid_{action} {call_id}")
        } else {
            format!("uuid_{action} {call_id} {args_str}")
        };
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_call_action_ok_err(&body, call_id)
    }

    async fn send_message(&self, call_id: &str, body: &str) -> Result<(), CoreError> {
        // send_msg command format: send_msg <call-id> <body>, body may contain spaces,
        // so just concatenate directly (call_id charset already validated by routing layer).
        let cmd = format!("send_msg {call_id} {body}");
        let resp = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_call_action_ok_err(&resp, call_id)
    }

    async fn originate(&self, call_url: &str) -> Result<(), CoreError> {
        let cmd = format!("originate {call_url}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_call_action_ok_err(&body, call_url)
    }

    async fn status(&self) -> Result<StatusInfo, CoreError> {
        parse_status(&request(&self.sock_path, self.timeout, "status").await?)
    }

    async fn show_rates(&self) -> Result<RateList, CoreError> {
        let body = request(&self.sock_path, self.timeout, "show rates").await?;
        parse_show_rates(&body)
    }

    async fn show_balances(&self) -> Result<AccountList, CoreError> {
        let body = request(&self.sock_path, self.timeout, "show balances").await?;
        parse_show_balances(&body)
    }

    async fn show_transcode(&self) -> Result<BridgeList, CoreError> {
        let body = request(&self.sock_path, self.timeout, "show transcode").await?;
        parse_show_transcode(&body)
    }

    async fn reload_rates(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("rate.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn rate_upsert_json(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("rate.upsert {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn rate_delete(&self, prefix: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("rate.delete {prefix}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn account_open(&self, key: &str, balance_uc: i64, credit_limit_uc: i64) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("account.open {key} {balance_uc} {credit_limit_uc}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn account_topup(&self, key: &str, delta_uc: i64) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("account.topup {key} {delta_uc}"))
            .await.and_then(|b| check_rejected(&b))
    }
}

/// Send a single command and return response text before the prompt (already trimmed, ERR lines already classified).
async fn request(
    sock: &Path,
    timeout: Duration,
    cmd: &str,
) -> Result<String, CoreError> {
    let body = send_cmd(sock, timeout, cmd).await?;
    check_rejected(&body)?;
    Ok(body)
}

/// Only fetch response text, do not pre-check ERR (write commands need to distinguish ERR semantics, e.g. notfound->404).
async fn send_cmd(
    sock: &Path,
    timeout: Duration,
    cmd: &str,
) -> Result<String, CoreError> {
    let buf = request_raw(sock, timeout, cmd).await?;
    let text = String::from_utf8(buf).map_err(|e| CoreError::Protocol(e.to_string()))?;
    Ok(text
        .split(str::from_utf8(PROMPT).unwrap_or("nsbc> "))
        .next()
        .unwrap_or("")
        .trim()
        .to_string())
}

/// UDS transport (unix targets only): connect -> drain connection banner -> send command -> read until next prompt.
#[cfg(unix)]
async fn request_raw(
    sock: &Path,
    timeout: Duration,
    cmd: &str,
) -> Result<Vec<u8>, CoreError> {
    use tokio::io::AsyncWriteExt;
    use tokio::net::UnixStream;

    let mut stream = match tokio::time::timeout(timeout, UnixStream::connect(sock)).await {
        Ok(Ok(s)) => s,
        Ok(Err(e)) => return Err(CoreError::Connect(e.to_string())),
        Err(_) => return Err(CoreError::Timeout),
    };

    // Banner ends at first prompt; drain but do not parse (version mismatch does not affect subsequent commands).
    let mut banner = Vec::new();
    read_until_prompt(&mut stream, timeout, &mut banner).await?;

    stream
        .write_all(cmd.as_bytes())
        .await
        .map_err(|e| CoreError::Io(e.to_string()))?;
    stream
        .write_all(b"\n")
        .await
        .map_err(|e| CoreError::Io(e.to_string()))?;

    let mut body = Vec::new();
    read_until_prompt(&mut stream, timeout, &mut body).await?;
    Ok(body)
}

#[cfg(unix)]
async fn read_until_prompt<R: tokio::io::AsyncRead + Unpin>(
    r: &mut R,
    timeout: Duration,
    buf: &mut Vec<u8>,
) -> Result<(), CoreError> {
    use tokio::io::AsyncReadExt;

    let mut chunk = [0u8; 1024];
    loop {
        let n = match tokio::time::timeout(timeout, r.read(&mut chunk)).await {
            Ok(Ok(0)) => return Err(CoreError::Protocol(
                "connection closed before prompt".into(),
            )),
            Ok(Ok(n)) => n,
            Ok(Err(e)) => return Err(CoreError::Io(e.to_string())),
            Err(_) => return Err(CoreError::Timeout),
        };
        buf.extend_from_slice(&chunk[..n]);
        if buf.windows(PROMPT.len()).any(|w| w == PROMPT) {
            return Ok(());
        }
        if buf.len() > MAX_FRAME {
            return Err(CoreError::Protocol("oversized control frame".into()));
        }
    }
}

/// Non-unix target (Windows dev machine): transport does not compile, explicitly Unsupported,
/// parsing layer and routing logic can still build and test on this machine.
#[cfg(not(unix))]
async fn request_raw(
    _sock: &Path,
    _timeout: Duration,
    _cmd: &str,
) -> Result<Vec<u8>, CoreError> {
    Err(CoreError::Unsupported(
        "core UDS control transport is only built on unix targets",
    ))
}

/// Core `ERR ...` line -> [`CoreError::CoreRejected`].
fn check_rejected(body: &str) -> Result<(), CoreError> {
    for line in body.lines() {
        let t = line.trim();
        if let Some(msg) = t.strip_prefix("ERR") {
            return Err(CoreError::CoreRejected(msg.trim().to_string()));
        }
    }
    Ok(())
}

/// Parse `nsbcd <version>`.
pub fn parse_version(body: &str) -> Result<VersionInfo, CoreError> {
    let line = body.lines().next().unwrap_or("").trim();
    let mut it = line.split_whitespace();
    match (it.next(), it.next()) {
        (Some("nsbcd"), Some(v)) if !v.is_empty() => {
            Ok(VersionInfo { core: v.to_string() })
        }
        _ => Err(CoreError::Protocol(format!("bad version response: {body:?}"))),
    }
}

/// Extract a u64 key from a single `k=v k=v ...` line.
fn kv_u64(line: &str, key: &str) -> Option<u64> {
    line.split_whitespace()
        .filter_map(|tok| tok.split_once('='))
        .find(|(k, _)| *k == key)
        .and_then(|(_, v)| v.parse::<u64>().ok())
}

/// Parse `active=N total=N failed=N` (key order insensitive; missing key is protocol error).
pub fn parse_call_counters(body: &str) -> Result<CallCounters, CoreError> {
    let line = body.lines().next().unwrap_or("");
    match (
        kv_u64(line, "active"),
        kv_u64(line, "total"),
        kv_u64(line, "failed"),
    ) {
        (Some(active), Some(total), Some(failed)) => {
            Ok(CallCounters { active, total, failed })
        }
        _ => Err(CoreError::Protocol(format!(
            "bad call counters: {body:?}"
        ))),
    }
}

/// Parse show registrations: one `reg k=v k=v ...` line per valid binding,
/// trailing line `registrations=N truncated=T` (channels=N same pattern).
pub fn parse_registrations(body: &str) -> Result<RegistrationSummary, CoreError> {
    let mut bindings = Vec::new();
    let mut total: u64 = 0;
    let mut truncated = false;
    for line in body.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("reg ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            bindings.push(BindingEntry {
                aor: kv_opt(toks.iter().copied(), "aor").unwrap_or_default(),
                user: kv_opt(toks.iter().copied(), "user").unwrap_or_default(),
                contact: kv_opt(toks.iter().copied(), "contact").unwrap_or_default(),
                expires_sec: kv_opt(toks.iter().copied(), "expires_sec")
                    .and_then(|v| v.parse::<i64>().ok()),
                alive_sec: kv_opt(toks.iter().copied(), "alive_sec")
                    .and_then(|v| v.parse::<i64>().ok()),
                q: kv_opt(toks.iter().copied(), "q")
                    .and_then(|v| v.parse::<i64>().ok())
                    .filter(|&v| v > 0),
                /* REQ-NAT-001: NAT metadata parsing. Older C core may not yet emit these fields,
                 * kv_opt returns None -> each String field unwrap_or_default() yields empty string,
                 * integer fields become None. Frontend display handles them safely via Optional. */
                nat_detected: kv_opt(toks.iter().copied(), "nat")
                    .and_then(|v| v.parse::<i64>().ok()),
                transport: kv_opt(toks.iter().copied(), "transport").unwrap_or_default(),
                received_ip: kv_opt(toks.iter().copied(), "received_ip")
                    .map(|s| if s == "-" { String::new() } else { s.to_string() })
                    .unwrap_or_default(),
                received_port: kv_opt(toks.iter().copied(), "received_port")
                    .and_then(|v| v.parse::<i64>().ok())
                    .filter(|&v| v > 0),
                public_contact: kv_opt(toks.iter().copied(), "pub_contact")
                    .unwrap_or_default(),
            });
        } else if let Some(rest) = line.strip_prefix("registrations=") {
            let mut rest = rest.split_whitespace();
            if let Some(n) = rest.next().and_then(|v| v.parse::<u64>().ok()) {
                total = n;
            }
            truncated = rest.find_map(|t| t.strip_prefix("truncated="))
                .map(|v| v == "1").unwrap_or(false);
        } else if line.is_empty() {
            continue;
        }
        // Backward-compatible with old version that only outputs registrations=N (no reg lines)
        else if let Some(n) = kv_u64(line, "registrations") {
            total = n;
        }
    }
    Ok(RegistrationSummary { bindings, registrations: total, truncated })
}

/// Parse `hangup <call-id>` response frame (C side obs_control cmd_hangup):
/// `OK hungup [call-id]` → Ok(())；`ERR notfound` → [`CoreError::NotFound`]；
/// Any other `ERR ...` -> [`CoreError::CoreRejected`]; contract mismatch -> Protocol.
pub fn parse_hangup(body: &str) -> Result<(), CoreError> {
    let line = body.lines().next().unwrap_or("").trim();
    if line == "OK hungup" || line.starts_with("OK hungup ") {
        Ok(())
    } else if line == "ERR notfound" {
        Err(CoreError::NotFound("call-id not found".into()))
    } else if let Some(msg) = line.strip_prefix("ERR") {
        Err(CoreError::CoreRejected(msg.trim().to_string()))
    } else {
        Err(CoreError::Protocol(format!("bad hangup response: {body:?}")))
    }
}

/// Parse `config.set <k> <v>` / `config.validate <k> <v>` response (C side obs_control
/// cmd_config_propose; see src/oam/obs_control.c):
///   `OK set <key>` / `OK unchanged <key>` → Ok(ConfigSetResult{changed})
///   `ERR ...` → CoreRejected
pub fn parse_config_set(body: &str, key: &str) -> Result<ConfigSetResult, CoreError> {
    let line = body.lines().next().unwrap_or("").trim();
    check_err_line(line)?;
    if let Some(rest) = line.strip_prefix("OK set ") {
        Ok(ConfigSetResult { key: rest.trim().to_string(), changed: true })
    } else if let Some(rest) = line.strip_prefix("OK unchanged ") {
        Ok(ConfigSetResult { key: rest.trim().to_string(), changed: false })
    } else {
        // Some core branches may return `OK set <key> <reason>`; try permissive match first
        if line.starts_with("OK set ") {
            let rest = line.strip_prefix("OK set ").unwrap_or("").trim();
            let reported_key = rest.split_whitespace().next().unwrap_or(key);
            Ok(ConfigSetResult { key: reported_key.to_string(), changed: true })
        } else if line.starts_with("OK unchanged ") {
            let rest = line.strip_prefix("OK unchanged ").unwrap_or("").trim();
            let reported_key = rest.split_whitespace().next().unwrap_or(key);
            Ok(ConfigSetResult { key: reported_key.to_string(), changed: false })
        } else {
            Err(CoreError::Protocol(format!("bad config.set response: {body:?}")))
        }
    }
}

/// Parse `config.validate <k> <v>` response:
///   `OK` / `OK valid` → Ok(())
///   `ERR ...` → CoreRejected
pub fn parse_config_validate(body: &str) -> Result<(), CoreError> {
    let line = body.lines().next().unwrap_or("").trim();
    if line == "OK" || line.starts_with("OK ") { Ok(()) }
    else if let Some(msg) = line.strip_prefix("ERR") {
        Err(CoreError::CoreRejected(msg.trim().to_string()))
    } else {
        Err(CoreError::Protocol(format!("bad config.validate response: {body:?}")))
    }
}

/// Parse `cdr.recent [N]` response frame (C side obs_control cmd_cdr_recent, P1):
/// Header may be `cdrs=N` (similar to `channels=N truncated=0` from show channels);
/// One CDR per line, key-value pairs:
///   `cdr call_id=X tenant=Y from=A to=B [trunk=T] [rec=R] dir=<in|out|int|unk>
///    phase=<start|answer|end> disp=<answered|noanswer|busy|failed|abandoned>
///    start_ms=U answer_ms=U end_ms=U billsec=U duration_s=U cause=N emergency=0|1`
/// Trailing `cdrs=N` summary anchor line possible; when no CDRs, only `cdrs=0`.
pub fn parse_cdr_recent(body: &str) -> Result<Vec<CdrEvent>, CoreError> {
    let mut cdrs = Vec::new();
    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() { continue; }
        if let Some(msg) = line.strip_prefix("ERR") {
            return Err(CoreError::CoreRejected(msg.trim().to_string()));
        }
        if let Some(rest) = line.strip_prefix("cdr ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            cdrs.push(parse_cdr_line(&toks)?);
        }
        // Skip `cdrs=N` summary line (already parsed line by line)
    }
    Ok(cdrs)
}

/// Single-line parsing (`cdr` prefix already stripped, remaining `k=v k=v ...` token sequence).
fn parse_cdr_line(toks: &[&str]) -> Result<CdrEvent, CoreError> {
    let kv = |k: &str| kv_opt(toks.iter().copied(), k);
    let kvi64 = |k: &str| kv_opt(toks.iter().copied(), k).and_then(|v| v.parse::<i32>().ok());
    let kvu64 = |k: &str| kv_opt(toks.iter().copied(), k).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
    let kvu32 = |k: &str| kv_opt(toks.iter().copied(), k).and_then(|v| v.parse::<u32>().ok()).unwrap_or(0);

    let call_id = kv("call_id").ok_or_else(|| CoreError::Protocol("cdr missing call_id".into()))?;
    let from_num = kv("from").ok_or_else(|| CoreError::Protocol("cdr missing from".into()))?;
    let to_num = kv("to").ok_or_else(|| CoreError::Protocol("cdr missing to".into()))?;
    let dir = kv("dir").unwrap_or_else(|| "unknown".into());
    let phase = kv("phase").unwrap_or_else(|| "end".into());
    let disp = kv("disp").unwrap_or_else(|| "failed".into());
    let emergency = kv_opt(toks.iter().copied(), "emergency")
        .map(|v| v == "1")
        .unwrap_or(false);

    Ok(CdrEvent {
        call_id,
        tenant: kv("tenant"),
        from_num,
        to_num,
        trunk: kv("trunk"),
        rec_path: kv("rec"),
        dir, phase, disposition: disp,
        start_ms: kvu64("start_ms"),
        answer_ms: kvu64("answer_ms"),
        end_ms: kvu64("end_ms"),
        billsec: kvu32("billsec"),
        duration_s: kvu32("duration_s"),
        cause: kvi64("cause").unwrap_or(0),
        emergency,
        /* codec/IP/threat extension fields: fmt_one (cdr.c) emits them since
         * REQ-CDR-002 completion; "-" placeholder filtered out for absent values.
         * Cores without these tokens simply yield None (backward compatible). */
        from_ip: kv_opt(toks.iter().copied(), "from_ip").filter(|v| v != "-"),
        a_peer_ip: kv_opt(toks.iter().copied(), "a_peer").filter(|v| v != "-"),
        b_peer_ip: kv_opt(toks.iter().copied(), "b_peer").filter(|v| v != "-"),
        codec_a: kv_opt(toks.iter().copied(), "codec_a").filter(|v| v != "-"),
        codec_b: kv_opt(toks.iter().copied(), "codec_b").filter(|v| v != "-"),
        is_malicious: kv_opt(toks.iter().copied(), "mal")
            .map(|v| v == "1")
            .unwrap_or(false),
        threat_reason: kv_opt(toks.iter().copied(), "threat").filter(|v| v != "-"),
    })
}

fn check_err_line(line: &str) -> Result<(), CoreError> {
    if let Some(msg) = line.strip_prefix("ERR") {
        Err(CoreError::CoreRejected(msg.trim().to_string()))
    } else { Ok(()) }
}

/// Generic uuid_* call-level write op / originate command response parsing (core cmd_registry
/// dispatch output):
///   `OK <action> [<detail>]` → Ok(())
///   `ERR notfound` / `ERR call-id not found` → [`CoreError::NotFound`]
///   `ERR ...` → [`CoreError::CoreRejected`]
///   empty response / other -> [`CoreError::Protocol`]
pub fn parse_call_action_ok_err(body: &str, target: &str) -> Result<(), CoreError> {
    let line = body.lines().next().unwrap_or("").trim();
    if line.starts_with("OK") {
        Ok(())
    } else if line == "ERR notfound" || line == "ERR call-id not found" {
        Err(CoreError::NotFound(target.to_string()))
    } else if let Some(msg) = line.strip_prefix("ERR") {
        Err(CoreError::CoreRejected(msg.trim().to_string()))
    } else if line.is_empty() {
        Err(CoreError::Protocol("empty response".into()))
    } else {
        Err(CoreError::Protocol(format!("bad response: {body:?}")))
    }
}

/// Parse `status` command output (REQ-CCMD-001 L1 read-only):
/// First line key-value pairs `status active_calls=N total=N failed=N registrations=N
/// profiles=N started_ago_ms=U` (`status` is the command line itself; core output does not include the command name,
/// directly starting from `active_calls=`); `alive=1` is the trailing anchor line.
/// Missing fields default to 0 (non-required); protocol violation (empty response / no alive anchor) returns Protocol.
pub fn parse_status(body: &str) -> Result<StatusInfo, CoreError> {
    let mut active_calls = 0u64;
    let mut total_calls = 0u64;
    let mut failed_calls = 0u64;
    let mut registrations = 0u64;
    let mut profiles = 0u64;
    let mut started_ago_ms = 0u64;
    let mut saw_alive = false;

    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() { continue; }
        if line.starts_with("ERR") {
            return Err(CoreError::CoreRejected(line.strip_prefix("ERR").unwrap().trim().to_string()));
        }
        if let Some(rest) = line.strip_prefix("alive=") {
            saw_alive = rest.starts_with('1');
            continue;
        }
        // Key-value pair line: may start with "status " (command echo-back); strip then process uniformly
        let stripped = line.strip_prefix("status ").unwrap_or(line);
        for tok in stripped.split_whitespace() {
            if let Some((k, v)) = tok.split_once('=') {
                let parse_u = |s: &str| s.parse::<u64>().unwrap_or(0);
                match k {
                    "active_calls" => active_calls = parse_u(v),
                    "total_calls" => total_calls = parse_u(v),
                    "failed_calls" => failed_calls = parse_u(v),
                    "registrations" => registrations = parse_u(v),
                    "profiles" => profiles = parse_u(v),
                    "started_ago_ms" => started_ago_ms = parse_u(v),
                    _ => {} // unknown keys ignored, permissive protocol
                }
            }
        }
    }
    if !saw_alive {
        return Err(CoreError::Protocol(format!("status frame missing alive=1: {body:?}")));
    }
    Ok(StatusInfo {
        active_calls, total_calls, failed_calls, registrations,
        profiles, started_ago_ms,
    })
}

/// Parse `show rates` frame (REQ-OAM-009):
/// Header `rates=N` + one rate per line `rate prefix=X name=Y price_per_min_uc=N
/// connect_fee_uc=F bill_inc_s=I min_bill_s=M initial_s=S enabled=B`。
pub fn parse_show_rates(body: &str) -> Result<RateList, CoreError> {
    let mut rates = Vec::new();
    let mut total = 0i64;
    let mut saw_head = false;
    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() { continue; }
        if let Some(msg) = line.strip_prefix("ERR") {
            return Err(CoreError::CoreRejected(msg.trim().to_string()));
        }
        if let Some(rest) = line.strip_prefix("rates=") {
            total = rest.trim().parse::<i64>().unwrap_or(0);
            saw_head = true;
        } else if let Some(rest) = line.strip_prefix("rate ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            let prefix = kv_opt(toks.iter().copied(), "prefix")
                .ok_or_else(|| CoreError::Protocol("rate missing prefix".into()))?;
            rates.push(RateEntry {
                prefix,
                name: kv_opt(toks.iter().copied(), "name").unwrap_or_default(),
                price_per_min_uc: kv_opt(toks.iter().copied(), "price_per_min_uc")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                connect_fee_uc: kv_opt(toks.iter().copied(), "connect_fee_uc")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                bill_inc_s: kv_opt(toks.iter().copied(), "bill_inc_s")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                min_bill_s: kv_opt(toks.iter().copied(), "min_bill_s")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                initial_s: kv_opt(toks.iter().copied(), "initial_s")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                enabled: kv_opt(toks.iter().copied(), "enabled")
                    .map(|v| v == "1").unwrap_or(false),
            });
        }
    }
    if !saw_head {
        return Err(CoreError::Protocol(format!("missing rates header: {body:?}")));
    }
    Ok(RateList { rates, total })
}

/// Parse `show balances` frame: header `accounts=N` + per-line `account key=K
/// balance_uc=B reserved_uc=R credit_limit_uc=C`。
pub fn parse_show_balances(body: &str) -> Result<AccountList, CoreError> {
    let mut accounts = Vec::new();
    let mut total = 0i64;
    let mut saw_head = false;
    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() { continue; }
        if let Some(msg) = line.strip_prefix("ERR") {
            return Err(CoreError::CoreRejected(msg.trim().to_string()));
        }
        if let Some(rest) = line.strip_prefix("accounts=") {
            total = rest.trim().parse::<i64>().unwrap_or(0);
            saw_head = true;
        } else if let Some(rest) = line.strip_prefix("account ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            let key = kv_opt(toks.iter().copied(), "key")
                .ok_or_else(|| CoreError::Protocol("account missing key".into()))?;
            accounts.push(AccountBalance {
                key,
                balance_uc: kv_opt(toks.iter().copied(), "balance_uc")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                reserved_uc: kv_opt(toks.iter().copied(), "reserved_uc")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                credit_limit_uc: kv_opt(toks.iter().copied(), "credit_limit_uc")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
            });
        }
    }
    if !saw_head {
        return Err(CoreError::Protocol(format!("missing accounts header: {body:?}")));
    }
    Ok(AccountList { accounts, total })
}

/// Parse `show transcode` frame: header `sessions=N` + per-line `bridge idx=I
/// call_key=P mode=M a_rtp=A b_rtp=B pkts=N drops=D`。
pub fn parse_show_transcode(body: &str) -> Result<BridgeList, CoreError> {
    let mut sessions = Vec::new();
    let mut total = 0i64;
    let mut saw_head = false;
    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() { continue; }
        if let Some(msg) = line.strip_prefix("ERR") {
            return Err(CoreError::CoreRejected(msg.trim().to_string()));
        }
        if let Some(rest) = line.strip_prefix("sessions=") {
            total = rest.trim().parse::<i64>().unwrap_or(0);
            saw_head = true;
        } else if let Some(rest) = line.strip_prefix("bridge ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            sessions.push(BridgeSession {
                idx: kv_opt(toks.iter().copied(), "idx")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                call_key_ptr: kv_opt(toks.iter().copied(), "call_key")
                    .unwrap_or_default(),
                mode: kv_opt(toks.iter().copied(), "mode").unwrap_or_default(),
                a_rtp: kv_opt(toks.iter().copied(), "a_rtp")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                b_rtp: kv_opt(toks.iter().copied(), "b_rtp")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                pkts: kv_opt(toks.iter().copied(), "pkts")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                drops: kv_opt(toks.iter().copied(), "drops")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
            });
        }
    }
    if !saw_head {
        return Err(CoreError::Protocol(format!("missing sessions header: {body:?}")));
    }
    Ok(BridgeList { sessions, total })
}

/// Extract a string key from `k=v k=v ...` token sequence; "-" and missing both treated as None.
fn kv_opt<'a, I, S>(toks: I, key: &str) -> Option<String>
where
    I: IntoIterator<Item = &'a S>,
    S: AsRef<str> + ?Sized + 'a,
{
    toks.into_iter()
        .filter_map(|t| t.as_ref().split_once('='))
        .find(|(k, _)| *k == key)
        .map(|(_, v)| v)
        .filter(|v| !v.is_empty() && *v != "-")
        .map(str::to_string)
}

/// Parse `show channels` frame:
/// `call a_callid=X a_state=S b_callid=Y b_state=S` (one line per call,
/// "-" -> None) + trailing line `channels=N truncated=0|1` (protocol anchor; missing one is an error).
pub fn parse_call_list(body: &str) -> Result<CallList, CoreError> {
    let mut calls = Vec::new();
    let mut summary = false;
    let mut truncated = false;
    for line in body.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("call ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            let start_ms = kv_opt(toks.iter().copied(), "start_ms")
                .and_then(|v| v.parse::<u64>().ok());
            let answer_ms = kv_opt(toks.iter().copied(), "answer_ms")
                .and_then(|v| if v == "-" || v == "0" { None } else { v.parse::<u64>().ok() });
            let duration_ms = kv_opt(toks.iter().copied(), "duration_ms")
                .and_then(|v| v.parse::<u64>().ok());
            calls.push(CallSummary {
                a_call_id: kv_opt(toks.iter().copied(), "a_callid").filter(|v| v != "-"),
                a_state: kv_opt(toks.iter().copied(), "a_state").filter(|v| v != "-"),
                b_call_id: kv_opt(toks.iter().copied(), "b_callid").filter(|v| v != "-"),
                b_state: kv_opt(toks.iter().copied(), "b_state").filter(|v| v != "-"),
                caller: kv_opt(toks.iter().copied(), "caller").filter(|v| v != "-"),
                callee: kv_opt(toks.iter().copied(), "callee").filter(|v| v != "-"),
                start_ms,
                answer_ms,
                duration_ms,
                codec_a: kv_opt(toks.iter().copied(), "codec_a").filter(|v| v != "-"),
                codec_b: kv_opt(toks.iter().copied(), "codec_b").filter(|v| v != "-"),
                rtp_loss_pct: kv_opt(toks.iter().copied(), "rtp_loss_pct")
                    .and_then(|v| if v == "-" || v == "0" { None } else { v.parse::<u32>().ok() }),
                rtp_jitter_ms: kv_opt(toks.iter().copied(), "rtp_jitter_ms")
                    .and_then(|v| if v == "-" || v == "0" { None } else { v.parse::<u32>().ok() }),
                mos_x1000: kv_opt(toks.iter().copied(), "mos_x1000")
                    .and_then(|v| if v == "-" || v == "0" { None } else { v.parse::<i32>().ok() })
                    .filter(|&v| v >= 0),
            });
        } else if let Some(rest) = line.strip_prefix("channels=") {
            // channels=N truncated=T (two k=v on the same trailing line)
            let mut toks = rest.split_whitespace();
            if toks.next().and_then(|v| v.parse::<u64>().ok()).is_none() {
                return Err(CoreError::Protocol(format!("bad channels: {body:?}")));
            }
            truncated = toks
                .find_map(|t| t.strip_prefix("truncated="))
                .map(|v| v == "1")
                .unwrap_or(false);
            summary = true;
        }
    }
    if !summary {
        return Err(CoreError::Protocol(format!("missing channels summary: {body:?}")));
    }
    Ok(CallList { calls, truncated })
}

/// Parse `uuid_dump <call-id>` frame (length-framed, SDP may contain newlines):
/// Header `call a_callid=.. a_state=.. b_callid=.. b_state=..`;
/// Per leg: three groups of state/tags/media key-value lines + optional `leg N sdp_offer|sdp_answer <len>`
/// followed immediately by len bytes of raw text. Media UNAVAILABLE -> None; notfound/ERR explicitly classified.
pub fn parse_call_detail(body: &str) -> Result<CallDetail, CoreError> {
    let line0 = body.lines().next().unwrap_or("").trim();
    if line0 == "ERR notfound" {
        return Err(CoreError::NotFound("call-id not found".into()));
    }
    if let Some(msg) = line0.strip_prefix("ERR") {
        return Err(CoreError::CoreRejected(msg.trim().to_string()));
    }
    let bytes = body.as_bytes();
    let mut pos = 0usize;
    let mut head: Option<(Option<String>, Option<String>, Option<String>,
                          Option<String>)> = None;
    let mut legs = std::collections::BTreeMap::<u8, LegDump>::new();
    let mut saw_leg = false;

    while pos < bytes.len() {
        let nl = bytes[pos..]
            .iter()
            .position(|&x| x == b'\n')
            .map(|rel| pos + rel)
            .unwrap_or(bytes.len());
        let line = std::str::from_utf8(&bytes[pos..nl])
            .map_err(|_| CoreError::Protocol("non-utf8 dump header".into()))?
            .trim_end_matches('\r');
        let after_nl = if nl < bytes.len() { nl + 1 } else { bytes.len() };

        if let Some(rest) = line.strip_prefix("call ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            head = Some((
                kv_opt(toks.iter().copied(), "a_callid"),
                kv_opt(toks.iter().copied(), "a_state"),
                kv_opt(toks.iter().copied(), "b_callid"),
                kv_opt(toks.iter().copied(), "b_state"),
            ));
            pos = after_nl;
        } else if let Some(rest) = line.strip_prefix("leg ") {
            // Line format:
            //   N state=.. proto=.. reliable=B cseq=K routes=R (no command keyword)
            //   N tags local=.. remote=..
            //   N media mode=.. gate=.. ssrc_in=.. ssrc_out=.. vq=..
            //   N sdp_offer|sdp_answer <len>
            let mut it = rest.split_whitespace();
            let n: u8 = it
                .next()
                .and_then(|s| s.parse().ok())
                .ok_or_else(|| CoreError::Protocol(format!("bad leg line: {line:?}")))?;
            let key = it.next().unwrap_or("");
            let rest_toks: Vec<&str> = it.collect();
            // First token of state line is state=X; first token of remaining lines is a command keyword.
            if key.starts_with("state=") {
                saw_leg = true;
                let mut toks = vec![key];
                toks.extend(rest_toks);
                {
                    let get = |k: &str| kv_opt(toks.iter().copied(), k);
                    let state = get("state")
                        .ok_or_else(|| CoreError::Protocol(format!("no state: {line:?}")))?;
                    let reliable = toks
                        .iter()
                        .find_map(|t| t.strip_prefix("reliable="))
                        .is_some_and(|v| v == "1");
                    let cseq = toks
                        .iter()
                        .find_map(|t| t.strip_prefix("cseq="))
                        .and_then(|v| v.parse::<i64>().ok())
                        .ok_or_else(|| CoreError::Protocol(format!("bad cseq: {line:?}")))?;
                    let routes = toks
                        .iter()
                        .find_map(|t| t.strip_prefix("routes="))
                        .and_then(|v| v.parse::<u32>().ok())
                        .unwrap_or(0);
                    legs.entry(n).or_insert_with(|| LegDump {
                        leg: n, state: String::new(), proto: String::new(),
                        reliable: false, cseq: 0, routes: 0,
                        local_tag: None, remote_tag: None,
                        offer: None, answer: None, media: LegMedia::default(),
                    });
                    let leg = legs.get_mut(&n).unwrap();
                    leg.state = state;
                    leg.proto = get("proto").unwrap_or_else(|| "?".into());
                    leg.reliable = reliable;
                    leg.cseq = cseq;
                    leg.routes = routes;
                }
            } else if key == "tags" {
                let leg = legs.entry(n).or_insert_with(|| LegDump {
                    leg: n, state: "?".into(), proto: "?".into(),
                    reliable: false, cseq: 0, routes: 0,
                    local_tag: None, remote_tag: None,
                    offer: None, answer: None, media: LegMedia::default(),
                });
                leg.local_tag = kv_opt(rest_toks.iter().copied(), "local");
                leg.remote_tag = kv_opt(rest_toks.iter().copied(), "remote");
            } else if key == "media" {
                // UNAVAILABLE = core has no source (P0 media plane / bypass leg) -> null,
                // must not map to any estimated value.
                let val = |k: &str| {
                    kv_opt(rest_toks.iter().copied(), k)
                        .filter(|v| v != "UNAVAILABLE")
                };
                let leg = legs.entry(n).or_insert_with(|| LegDump {
                    leg: n, state: "?".into(), proto: "?".into(),
                    reliable: false, cseq: 0, routes: 0,
                    local_tag: None, remote_tag: None,
                    offer: None, answer: None, media: LegMedia::default(),
                });
                leg.media = LegMedia {
                    mode: val("mode"),
                    gate: val("gate"),
                    ssrc_in: val("ssrc_in"),
                    ssrc_out: val("ssrc_out"),
                    vq: val("vq"),
                };
            } else if key == "sdp_offer" || key == "sdp_answer" {
                let len: usize = rest_toks
                    .first()
                    .and_then(|v| v.parse().ok())
                    .ok_or_else(|| CoreError::Protocol(format!("bad sdp len: {line:?}")))?;
                let end = after_nl.checked_add(len).ok_or_else(|| {
                    CoreError::Protocol("sdp length overflow".into())
                })?;
                if end > bytes.len() {
                    return Err(CoreError::Protocol(format!(
                        "truncated sdp section: want {len} bytes")));
                }
                let raw = std::str::from_utf8(&bytes[after_nl..end])
                    .map_err(|_| CoreError::Protocol("non-utf8 sdp".into()))?
                    .to_string();
                let leg = legs.get_mut(&n).ok_or_else(|| {
                    CoreError::Protocol("sdp before leg state line".into())
                })?;
                if key == "sdp_offer" {
                    leg.offer = Some(raw);
                } else {
                    leg.answer = Some(raw);
                }
                // Core always appends a '\n' separator after raw SDP (SDP itself
                // trailing CRLF counts toward len, not part of separator).
                pos = if end < bytes.len() && bytes[end] == b'\n' {
                    end + 1
                } else {
                    end
                };
                continue;
            } else {
                return Err(CoreError::Protocol(format!("bad leg key: {line:?}")));
            }
            pos = after_nl;
        } else {
            pos = after_nl; // blank/unknown lines skipped
        }
    }

    let (a_call_id, a_state, b_call_id, b_state) = head.ok_or_else(|| {
        CoreError::Protocol(format!("missing call header: {body:?}"))
    })?;
    if !saw_leg {
        return Err(CoreError::Protocol(format!("no leg sections: {body:?}")));
    }
    Ok(CallDetail {
        a_call_id,
        a_state,
        b_call_id,
        b_state,
        legs: legs.into_values().collect(),
    })
}

/// Parse `config.get <key>` frame (two lines):
/// `config key=<k> layer=<name> secret=<0|1>` + one raw value line.
/// Sensitive key secret=1: value=None (core **** redacted, placeholder does not leak upward).
/// Note: transport trims the whole frame; scalar empty values cannot be framed (declared keys all have defaults, not expected).
pub fn parse_config_value(body: &str) -> Result<ConfigValue, CoreError> {
    let line0 = body.lines().next().unwrap_or("").trim_end();
    if line0 == "ERR notfound" {
        return Err(CoreError::NotFound("config key not found".into()));
    }
    if let Some(msg) = line0.strip_prefix("ERR") {
        return Err(CoreError::CoreRejected(msg.trim().to_string()));
    }
    let rest = line0
        .strip_prefix("config ")
        .ok_or_else(|| CoreError::Protocol(format!("bad config frame: {body:?}")))?;
    let toks: Vec<&str> = rest.split_whitespace().collect();
    let key = kv_opt(toks.iter().copied(), "key")
        .ok_or_else(|| CoreError::Protocol(format!("missing key: {body:?}")))?;
    let layer = kv_opt(toks.iter().copied(), "layer")
        .ok_or_else(|| CoreError::Protocol(format!("missing layer: {body:?}")))?;
    let secret = toks
        .iter()
        .find_map(|t| t.strip_prefix("secret="))
        .is_some_and(|v| v == "1");
    let raw = body
        .lines()
        .nth(1)
        .ok_or_else(|| CoreError::Protocol(format!("missing value line: {body:?}")))?;
    Ok(ConfigValue {
        key,
        value: if secret { None } else { Some(raw.to_string()) },
        layer,
        secret,
    })
}

/// Parse `config.export` frame: header `config-export keys=N redact=1`,
/// followed by one key per line `<dotted.key>: <value|****>` (split at first ':'; value may contain
/// spaces/':'; empty-value line `key:` is valid, value is empty string). Skip `#` comments and blank lines;
/// **** maps to None/secret=true; declared key count vs actual line count mismatch treated as protocol violation.
pub fn parse_config_export(body: &str) -> Result<ConfigExport, CoreError> {
    let mut lines = body.lines();
    let head = lines.next().unwrap_or("").trim_end();
    let rest = head
        .strip_prefix("config-export ")
        .ok_or_else(|| CoreError::Protocol(format!("bad export frame: {body:?}")))?;
    let toks: Vec<&str> = rest.split_whitespace().collect();
    let declared = toks
        .iter()
        .find_map(|t| t.strip_prefix("keys="))
        .and_then(|v| v.parse::<usize>().ok())
        .ok_or_else(|| CoreError::Protocol(format!("missing keys: {body:?}")))?;
    let redacted = toks
        .iter()
        .find_map(|t| t.strip_prefix("redact="))
        .is_some_and(|v| v == "1");
    let mut items = Vec::with_capacity(declared);
    for line in lines {
        let line = line.trim_end();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        /* Key charset is [A-Za-z0-9._-], does not contain ':', so first ':' is the delimiter.
         * Do not require ": ": empty-value lines (e.g. ha.peer:) have no trailing space after trim_end. */
        let ci = line
            .find(':')
            .ok_or_else(|| CoreError::Protocol(format!("bad export line: {line:?}")))?;
        let key = line[..ci].trim_end();
        let val = line[ci + 1..].trim_start();
        if key.is_empty() {
            return Err(CoreError::Protocol(format!("empty export key: {body:?}")));
        }
        let secret = val == "****";
        items.push(ConfigExportItem {
            key: key.to_string(),
            value: if secret { None } else { Some(val.to_string()) },
            secret,
        });
    }
    if items.len() != declared {
        return Err(CoreError::Protocol(format!(
            "export keys mismatch: header={declared} parsed={}",
            items.len()
        )));
    }
    Ok(ConfigExport { redacted, items })
}

/// Parse Prometheus exposition into `name -> value`; skip comments/blank lines/illegal samples.
pub fn parse_metrics(body: &str) -> MetricSnapshot {
    let mut out = BTreeMap::new();
    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut it = line.split_whitespace();
        let (Some(name), Some(value), None) = (it.next(), it.next(), it.next()) else {
            continue;
        };
        let name = name.split('{').next().unwrap_or(name);
        if let Ok(v) = value.parse::<f64>() {
            out.insert(name.to_string(), v);
        }
    }
    MetricSnapshot(out)
}

#[cfg(test)]
pub(crate) use tests::MockCore;

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};

    // ---- Pure parsing layer: covers success / missing key / illegal / ERR / comment / label ----

    #[test]
    fn version_ok() {
        assert_eq!(
            parse_version("nsbcd 2.5.5\n").unwrap(),
            VersionInfo { core: "2.5.5".into() }
        );
    }

    #[test]
    fn version_bad() {
        assert!(parse_version("").is_err());
        assert!(parse_version("weird 2.5.5\n").is_err());
        assert!(parse_version("nsbcd\n").is_err());
    }

    #[test]
    fn counters_ok_and_order_insensitive() {
        assert_eq!(
            parse_call_counters("active=3 total=10 failed=2\n").unwrap(),
            CallCounters { active: 3, total: 10, failed: 2 }
        );
        assert_eq!(
            parse_call_counters("failed=1 total=4 active=0\r\n").unwrap(),
            CallCounters { active: 0, total: 4, failed: 1 }
        );
    }

    #[test]
    fn counters_missing_or_bad() {
        assert!(parse_call_counters("active=1 total=2\n").is_err());
        assert!(parse_call_counters("active=x total=2 failed=1\n").is_err());
        assert!(parse_call_counters("").is_err());
    }

    #[test]
    fn registrations_ok_bad() {
        assert_eq!(
            parse_registrations("registrations=42\n").unwrap(),
            RegistrationSummary { bindings: Vec::new(), registrations: 42, truncated: false }
        );
        assert!(parse_registrations("registrations=\n").is_err());
        assert!(parse_registrations("\n").is_err());
    }

    #[test]
    fn metrics_exposition() {
        let text = "\
# HELP nsbc_up 1 if the NSBC core process is up
# TYPE nsbc_up gauge
nsbc_up 1
# HELP nsbc_calls_active active calls
# TYPE nsbc_calls_active gauge
nsbc_calls_active 7
nsbc_rtp_ports_in_use 12
junk line without value
nsbc_bad notanumber
";
        let m = parse_metrics(text);
        assert_eq!(m.0.get("nsbc_up"), Some(&1.0));
        assert_eq!(m.0.get("nsbc_calls_active"), Some(&7.0));
        assert_eq!(m.0.get("nsbc_rtp_ports_in_use"), Some(&12.0));
        assert_eq!(m.0.len(), 3);
    }

    #[test]
    fn metrics_labeled_sample_strips_braces() {
        let m = parse_metrics("nsbc_x{dir=\"in\"} 1.5\nnsbc_x{dir=\"out\"} 2.5\n");
        // Same name last-wins
        assert_eq!(m.0.get("nsbc_x"), Some(&2.5));
    }

    #[test]
    fn err_lines_classified() {
        assert!(check_rejected("ERR unknown command: foo (try 'help')").is_err());
        assert!(check_rejected("ERR oom").is_err());
        assert!(check_rejected("OK reload dispatched\n").is_ok());
        assert!(check_rejected("active=1 total=2 failed=0").is_ok());
    }

    #[test]
    fn hangup_responses() {
        assert!(parse_hangup("OK hungup\n").is_ok());
        assert!(parse_hangup("OK hungup call-1@host\n").is_ok());
        assert!(matches!(
            parse_hangup("ERR notfound\n"),
            Err(CoreError::NotFound(_))
        ));
        assert!(matches!(
            parse_hangup("ERR invalid call-id\n"),
            Err(CoreError::CoreRejected(_))
        ));
        assert!(matches!(
            parse_hangup("weird\n"),
            Err(CoreError::Protocol(_))
        ));
    }

    #[test]
    fn config_set_responses() {
        let r = parse_config_set("OK set sip.bind_addr\n", "sip.bind_addr").unwrap();
        assert_eq!(r.key, "sip.bind_addr");
        assert!(r.changed);
        let r = parse_config_set("OK unchanged sip.port\n", "sip.port").unwrap();
        assert!(!r.changed);
        assert!(matches!(
            parse_config_set("ERR invalid key\n", "bad key"),
            Err(CoreError::CoreRejected(_))
        ));
        assert!(matches!(
            parse_config_set("weird\n", "k"),
            Err(CoreError::Protocol(_))
        ));
    }

    #[test]
    fn config_validate_responses() {
        assert!(parse_config_validate("OK\n").is_ok());
        assert!(parse_config_validate("OK valid\n").is_ok());
        assert!(matches!(
            parse_config_validate("ERR value out of range\n"),
            Err(CoreError::CoreRejected(_))
        ));
    }

    #[test]
    fn cdr_recent_parses_full_frame() {
        // Single-line CDR (avoid visual clutter from \n inside Rust string literals).
        let body = "\
cdrs=1\ncdr call_id=call-1@host tenant=acme from=alice@A to=bob@B trunk=out rec=/var/rec/call1.wav dir=out phase=end disp=answered start_ms=1700000000000 answer_ms=1700000005000 end_ms=1700000070000 billsec=65 duration_s=70 cause=16 emergency=0\ncdrs=1\n";
        let cdrs = parse_cdr_recent(body).unwrap();
        assert_eq!(cdrs.len(), 1);
        let c = &cdrs[0];
        assert_eq!(c.call_id, "call-1@host");
        assert_eq!(c.dir, "out");
        assert_eq!(c.phase, "end");
        assert_eq!(c.disposition, "answered");
        assert_eq!(c.billsec, 65);
        assert_eq!(c.cause, 16);
        assert_eq!(c.trunk.as_deref(), Some("out"));
        assert_eq!(c.emergency, false);
    }

    #[test]
    fn cdr_recent_empty_set_and_unknown_command() {
        assert_eq!(parse_cdr_recent("cdrs=0\n").unwrap().len(), 0);
        assert_eq!(parse_cdr_recent("cdrs=5\n").unwrap().len(), 0);
        assert!(matches!(
            parse_cdr_recent("ERR unknown command: cdr.recent\n"),
            Err(CoreError::CoreRejected(_))
        ));
    }

    #[test]
    fn cdr_recent_minimal_line_sets_defaults() {
        let body = "cdr call_id=call-x from=a to=b\ncdrs=1\n";
        let cdrs = parse_cdr_recent(body).unwrap();
        let c = &cdrs[0];
        assert_eq!(c.dir, "unknown");
        assert_eq!(c.phase, "end");
        assert_eq!(c.disposition, "failed");
        assert_eq!(c.tenant, None);
        assert_eq!(c.start_ms, 0);
        assert_eq!(c.emergency, false);
    }

    #[test]
    fn call_list_ok_dash_and_truncated() {
        let body = "\
call a_callid=a@h a_state=CONFIRMED b_callid=b@h b_state=CONFIRMED
call a_callid=c@h a_state=EARLY b_callid=- b_state=-
channels=2 truncated=0
";
        let l = parse_call_list(body).unwrap();
        assert!(!l.truncated);
        assert_eq!(l.calls.len(), 2);
        assert_eq!(l.calls[0].a_call_id.as_deref(), Some("a@h"));
        assert_eq!(l.calls[0].b_state.as_deref(), Some("CONFIRMED"));
        assert_eq!(l.calls[1].b_call_id, None);
        assert_eq!(l.calls[1].b_state, None);
        let l2 = parse_call_list("channels=0 truncated=1\n").unwrap();
        assert!(l2.truncated);
        assert!(l2.calls.is_empty());
    }

    #[test]
    fn call_list_missing_summary_is_protocol() {
        assert!(parse_call_list("call a_callid=x a_state=IDLE b_callid=- b_state=-\n")
            .is_err());
        assert!(parse_call_list("channels=x truncated=0\n").is_err());
    }

    #[test]
    fn call_detail_full_frame_with_framed_sdp() {
        // offer may contain multi-line content (including forged "leg 1" lines); length framing must be preserved verbatim
        let offer = "v=0\r\no=- 1 1 IN IP4 1.2.3.4\r\nleg 1 state=FORGED\r\n";
        let body = format!(
            "call a_callid=a@h a_state=CONFIRMED b_callid=b@h b_state=EARLY\n\
leg 0 state=CONFIRMED proto=udp reliable=0 cseq=21 routes=0\n\
leg 0 tags local=lt0 remote=rt0\n\
leg 0 media mode=UNAVAILABLE gate=UNAVAILABLE ssrc_in=UNAVAILABLE ssrc_out=UNAVAILABLE vq=UNAVAILABLE\n\
leg 0 sdp_offer {ol}\n{offer}\n\
leg 1 state=EARLY proto=tcp reliable=1 cseq=3 routes=2\n\
leg 1 tags local=- remote=rt1\n\
leg 1 media mode=UNAVAILABLE gate=UNAVAILABLE ssrc_in=UNAVAILABLE ssrc_out=UNAVAILABLE vq=UNAVAILABLE\n",
            ol = offer.len(),
            offer = offer
        );
        let d = parse_call_detail(&body).unwrap();
        assert_eq!(d.a_call_id.as_deref(), Some("a@h"));
        assert_eq!(d.b_state.as_deref(), Some("EARLY"));
        assert_eq!(d.legs.len(), 2);
        let l0 = &d.legs[0];
        assert_eq!(l0.leg, 0);
        assert_eq!(l0.state, "CONFIRMED");
        assert_eq!(l0.proto, "udp");
        assert!(!l0.reliable);
        assert_eq!(l0.cseq, 21);
        assert_eq!(l0.local_tag.as_deref(), Some("lt0"));
        assert!(l0.offer.as_deref().unwrap().contains("state=FORGED"));
        assert_eq!(l0.offer.as_deref(), Some(offer));
        assert_eq!(l0.answer, None);
        assert_eq!(l0.media.mode, None); // UNAVAILABLE → None
        let l1 = &d.legs[1];
        assert!(l1.reliable);
        assert_eq!(l1.routes, 2);
        assert_eq!(l1.local_tag, None);
        assert_eq!(l1.remote_tag.as_deref(), Some("rt1"));
    }

    #[test]
    fn call_detail_errors() {
        assert!(matches!(
            parse_call_detail("ERR notfound\n"),
            Err(CoreError::NotFound(_))
        ));
        assert!(matches!(
            parse_call_detail("ERR invalid call-id\n"),
            Err(CoreError::CoreRejected(_))
        ));
        assert!(parse_call_detail("weird\n").is_err()); // no call header/leg
        // SDP segment truncated (declared length exceeds frame)
        let bad = "call a_callid=a a_state=IDLE b_callid=- b_state=-\n\
leg 0 state=IDLE proto=udp reliable=0 cseq=1 routes=0\n\
leg 0 sdp_offer 50\nabc\n";
        assert!(parse_call_detail(bad).is_err());
    }

    #[test]
    fn config_value_plain_and_secret() {
        let v = parse_config_value(
            "config key=log.level layer=system secret=0\n6\n",
        )
        .unwrap();
        assert_eq!(v.key, "log.level");
        assert_eq!(v.value.as_deref(), Some("6"));
        assert_eq!(v.layer, "system");
        assert!(!v.secret);
        // Values with whitespace kept as raw second line (no whitespace tokenization)
        let v2 = parse_config_value(
            "config key=log.path layer=default secret=0\n/var/log/nsbc/x.log\n",
        )
        .unwrap();
        assert_eq!(v2.value.as_deref(), Some("/var/log/nsbc/x.log"));
        // Sensitive key: secret=1 -> value=None (**** placeholder, do not leak)
        let s = parse_config_value(
            "config key=db.password layer=system secret=1\n****\n",
        )
        .unwrap();
        assert!(s.secret);
        assert_eq!(s.value, None);
        assert_eq!(s.layer, "system");
    }

    #[test]
    fn config_value_errors() {
        assert!(matches!(
            parse_config_value("ERR notfound\n"),
            Err(CoreError::NotFound(_))
        ));
        assert!(matches!(
            parse_config_value("ERR invalid key\n"),
            Err(CoreError::CoreRejected(_))
        ));
        assert!(parse_config_value("weird\n").is_err()); // no config header
        assert!(parse_config_value("config key=k layer=system secret=0\n").is_err());
        // Missing value line
        assert!(parse_config_value(
            "config layer=system secret=0\n6\n"
        )
        .is_err()); // missing key
    }

    #[test]
    fn config_export_plain_secret_and_spaces() {
        let body = "config-export keys=3 redact=1\n\
                    log.level: 6\n\
                    sip.user_agent: NSBC/2.5 extra\n\
                    db.dsn: ****\n";
        let e = parse_config_export(body).unwrap();
        assert!(e.redacted);
        assert_eq!(e.items.len(), 3);
        assert_eq!(e.items[0].key, "log.level");
        assert_eq!(e.items[0].value.as_deref(), Some("6"));
        assert!(!e.items[0].secret);
        // Spaces / ": " inside value: take the raw text after the first delimiter
        assert_eq!(e.items[1].value.as_deref(), Some("NSBC/2.5 extra"));
        // Sensitive key redacted: null + secret=true, **** does not leave the sidecar
        assert_eq!(e.items[2].key, "db.dsn");
        assert_eq!(e.items[2].value, None);
        assert!(e.items[2].secret);
        // Defensive skip comments/blank lines (core does not currently emit, parser tolerates)
        let with_comment = "config-export keys=1 redact=1\n# note\n\nlog.level: 6\n";
        let e2 = parse_config_export(with_comment).unwrap();
        assert_eq!(e2.items.len(), 1);
        // Empty-value line (real core ha.peer: no trailing space) must parse as empty string, not protocol error
        let empty_val = "config-export keys=2 redact=1\nha.enabled: false\nha.peer:\n";
        let e3 = parse_config_export(empty_val).unwrap();
        assert_eq!(e3.items.len(), 2);
        assert_eq!(e3.items[1].key, "ha.peer");
        assert_eq!(e3.items[1].value.as_deref(), Some(""));
        assert!(!e3.items[1].secret);
    }

    #[test]
    fn config_export_errors() {
        // Key count mismatch (declared 2, actual 1)
        assert!(parse_config_export("config-export keys=2 redact=1\nlog.level: 6\n").is_err());
        // Bad frame header / missing keys / non-numeric
        assert!(parse_config_export("weird\n").is_err());
        assert!(parse_config_export("config-export redact=1\nlog.level: 6\n").is_err());
        assert!(parse_config_export("config-export keys=x redact=1\n").is_err());
        // Value line has no ": " separator
        assert!(parse_config_export("config-export keys=1 redact=1\nlog.level=6\n").is_err());
        // ERR frame (real path pre-checked as CoreRejected via request(); parser also rejects)
        assert!(parse_config_export("ERR export failed\n").is_err());
    }

    // ---- MockCore: used by routing-layer tests; records commands and replays canned responses ----

    #[derive(Clone, Default)]
    pub(crate) struct MockCore {
        pub(crate) calls: Arc<Mutex<Vec<String>>>,
        pub(crate) canned: Arc<BTreeMap<String, Result<String, CoreError>>>,
    }

    impl MockCore {
        fn record(&self, cmd: &str) -> Result<String, CoreError> {
            self.calls.lock().unwrap().push(cmd.to_string());
            self.canned
                .get(cmd)
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned command".into())))
        }
    }

    impl CoreApi for MockCore {
        async fn version(&self) -> Result<VersionInfo, CoreError> {
            parse_version(&self.record("version")?)
        }
        async fn call_counters(&self) -> Result<CallCounters, CoreError> {
            parse_call_counters(&self.record("show calls")?)
        }
        async fn registrations(&self) -> Result<RegistrationSummary, CoreError> {
            parse_registrations(&self.record("show registrations")?)
        }
        async fn metrics(&self) -> Result<MetricSnapshot, CoreError> {
            Ok(parse_metrics(&self.record("stats")?))
        }
        async fn list_calls(&self) -> Result<CallList, CoreError> {
            parse_call_list(&self.record("show channels")?)
        }
        async fn call_detail(
            &self,
            call_id: &str,
        ) -> Result<CallDetail, CoreError> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("uuid_dump {call_id}"));
            // Canned key "uuid_dump:<id>"; missing -> Protocol (tests must explicitly can).
            let key = format!("uuid_dump:{call_id}");
            let canned = self
                .canned
                .get(&key)
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned uuid_dump".into())));
            parse_call_detail(&canned?)
        }
        async fn config_get(&self, key: &str) -> Result<ConfigValue, CoreError> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("config.get {key}"));
            let ckey = format!("config.get:{key}");
            let canned = self
                .canned
                .get(&ckey)
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned config.get".into())));
            parse_config_value(&canned?)
        }
        async fn config_export(&self) -> Result<ConfigExport, CoreError> {
            parse_config_export(&self.record("config.export")?)
        }
        async fn hangup(&self, call_id: &str) -> Result<(), CoreError> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("hangup {call_id}"));
            // Canned keys: prefer "hangup:<id>" (distinguishes success/404), fallback to generic "hangup".
            let key = format!("hangup:{call_id}");
            let canned = self
                .canned
                .get(&key)
                .or_else(|| self.canned.get("hangup"))
                .cloned()
                .unwrap_or_else(|| {
                    Err(CoreError::Protocol("uncanned hangup".into()))
                });
            parse_hangup(&canned?)
        }
        async fn dialplan_apps(&self) -> Result<Vec<AppInfo>, CoreError> {
            // Can "show apps" response; default empty array (dialplan module not loaded)
            let body = self
                .record("show apps")
                .unwrap_or_else(|_| "apps=0\n".to_string());
            Ok(parse_show_apps(&body))
        }
        async fn show_codecs(&self) -> Result<Vec<CodecInfo>, CoreError> {
            // Can "show codecs" response; default empty array (codec module not loaded)
            let body = self
                .record("show codecs")
                .unwrap_or_else(|_| "codecs=0\n".to_string());
            Ok(parse_show_codecs(&body))
        }
        async fn config_set(&self, key: &str, value: &str) -> Result<ConfigSetResult, CoreError> {
            self.calls.lock().unwrap().push(format!("config.set {key} {value}"));
            let ckey = format!("config.set:{key}");
            let canned = self
                .canned
                .get(&ckey)
                .or_else(|| self.canned.get("config.set"))
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned config.set".into())));
            parse_config_set(&canned?, key)
        }
        async fn config_validate(&self, key: &str, value: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("config.validate {key} {value}"));
            let ckey = format!("config.validate:{key}");
            let canned = self
                .canned
                .get(&ckey)
                .or_else(|| self.canned.get("config.validate"))
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned config.validate".into())));
            parse_config_validate(&canned?)
        }
        async fn cdr_recent(&self, limit: u32) -> Result<Vec<CdrEvent>, CoreError> {
            self.calls.lock().unwrap().push(format!("cdr.recent {limit}"));
            let canned = self
                .canned
                .get("cdr.recent")
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned cdr.recent".into())));
            parse_cdr_recent(&canned?)
        }
        async fn security_ban_list(&self, filter: &str) -> Result<Vec<BanEntry>, CoreError> {
            self.calls.lock().unwrap().push(format!("security.ban.list {filter}"));
            let canned = self
                .canned
                .get("security.ban.list")
                .cloned()
                .unwrap_or_else(|| "bans=0 shown=0 total=0\n".to_string());
            parse_ban_list(&canned)
        }
        async fn security_ban_unban(&self, ip: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("security.ban.unban {ip}"));
            let canned = self
                .canned
                .get("security.ban.unban")
                .cloned()
                .unwrap_or_else(|| format!("OK security.ban.unban ip={ip} removed=1\n"));
            if canned.trim_start().starts_with("OK") {
                Ok(())
            } else {
                let msg = canned.trim().strip_prefix("ERR").unwrap_or(canned.trim());
                Err(CoreError::CoreRejected(msg.trim().to_string()))
            }
        }

        async fn reload_extensions(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("extensions.reload {json}"));
            self.canned
                .get("extensions.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_gateways(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("gateways.reload {json}"));
            self.canned
                .get("gateways.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_trunks(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("trunks.reload {json}"));
            self.canned
                .get("trunks.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_dialplan_rules(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("dialplan-rules.reload {json}"));
            self.canned
                .get("dialplan-rules.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_queues(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("queues.reload {json}"));
            self.canned
                .get("queues.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_sip_profiles(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("profiles.reload {json}"));
            self.canned
                .get("profiles.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_security_blacklist(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("security.blacklist.reload {json}"));
            self.canned
                .get("security.blacklist.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }

        async fn reload_ha_groups(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("ha.reload {json}"));
            self.canned
                .get("ha.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_ivr_menus(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("ivr.reload {json}"));
            self.canned
                .get("ivr.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_route_groups(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("route_groups.reload {json}"));
            self.canned
                .get("route_groups.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }

        async fn call_action(
            &self,
            call_id: &str,
            action: &str,
            args: &[&str],
        ) -> Result<(), CoreError> {
            let args_str = args.iter().cloned().collect::<Vec<_>>().join(" ");
            let cmd = if args_str.is_empty() {
                format!("uuid_{action} {call_id}")
            } else {
                format!("uuid_{action} {call_id} {args_str}")
            };
            self.calls.lock().unwrap().push(cmd.clone());
            // Canned key prefers "uuid_<action>:<id>" -> fallback to "uuid_<action>"
            let key_specific = format!("uuid_{action}:{call_id}");
            let canned = self
                .canned
                .get(&key_specific)
                .or_else(|| self.canned.get(&format!("uuid_{action}")))
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol(format!("uncanned {cmd}"))));
            parse_call_action_ok_err(&canned?, call_id)
        }

        async fn send_message(&self, call_id: &str, body: &str) -> Result<(), CoreError> {
            let cmd = format!("send_msg {call_id} {body}");
            self.calls.lock().unwrap().push(cmd.clone());
            let key = format!("send_msg:{call_id}");
            let canned = self
                .canned
                .get(&key)
                .or_else(|| self.canned.get("send_msg"))
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol(format!("uncanned send_msg {call_id}"))));
            parse_call_action_ok_err(&canned?, call_id)
        }

        async fn originate(&self, call_url: &str) -> Result<(), CoreError> {
            let cmd = format!("originate {call_url}");
            self.calls.lock().unwrap().push(cmd.clone());
            let canned = self
                .canned
                .get("originate")
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned originate".into())));
            parse_call_action_ok_err(&canned?, call_url)
        }

        async fn status(&self) -> Result<StatusInfo, CoreError> {
            parse_status(&self.record("status")?)
        }

        async fn show_rates(&self) -> Result<RateList, CoreError> {
            parse_show_rates(&self.record("show rates")?)
        }

        async fn show_balances(&self) -> Result<AccountList, CoreError> {
            parse_show_balances(&self.record("show balances")?)
        }

        async fn show_transcode(&self) -> Result<BridgeList, CoreError> {
            parse_show_transcode(&self.record("show transcode")?)
        }

        async fn reload_rates(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("rate.reload {json}"));
            self.canned
                .get("rate.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn rate_upsert_json(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("rate.upsert {json}"));
            self.canned
                .get("rate.upsert")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn rate_delete(&self, prefix: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("rate.delete {prefix}"));
            let key = format!("rate.delete:{prefix}");
            self.canned
                .get(&key)
                .or_else(|| self.canned.get("rate.delete"))
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn account_open(&self, key: &str, balance_uc: i64, credit_limit_uc: i64) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("account.open {key} {balance_uc} {credit_limit_uc}"));
            let ckey = format!("account.open:{key}");
            self.canned
                .get(&ckey)
                .or_else(|| self.canned.get("account.open"))
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn account_topup(&self, key: &str, delta_uc: i64) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("account.topup {key} {delta_uc}"));
            let ckey = format!("account.topup:{key}");
            self.canned
                .get(&ckey)
                .or_else(|| self.canned.get("account.topup"))
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
    }

    /// Non-unix target: RealCore must explicitly return Unsupported instead of panicking (testable on Windows).
    #[cfg(not(unix))]
    #[tokio::test]
    async fn real_core_unsupported_on_windows() {
        let c = RealCore::new(PathBuf::from(DEFAULT_SOCK_PATH), Duration::from_secs(1));
        assert!(matches!(
            c.call_counters().await,
            Err(CoreError::Unsupported(_))
        ));
    }
}
