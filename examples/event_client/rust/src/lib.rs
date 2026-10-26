//! NSBC Unified Client Library (Rust).
//!
//! Feature-parity with Python `nsbc_client` and Go `nsbc_sdk`:
//!   - Unix domain socket transport (Unix-only — std::os::unix::net::UnixStream)
//!   - HTTP originate path over reqwest blocking + rustls-tls
//!   - originate dual-path: UDS preferred → HTTP fallback
//!   - frame parsing / classification
//!   - CallCorrelator (HashMap + callback, end-of-CDR timeline)
//!   - WebhookSink (daemon thread POST every frame)
//!   - TLS verify switch (for self-signed nsbc-web certs)
//!
//! Build: `cargo build` (Cargo.lock updated from Cargo.toml)
//! Run demo: `cargo run --release -- --orig-to 7gja30rf`

use std::collections::HashMap;
use std::io::BufRead;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

pub const DEFAULT_UDS:       &str = "/var/run/nsbc/control.sock";
pub const DEFAULT_API_BASE:  &str = "https://127.0.0.1:443";
pub const DEFAULT_SSE_URL:   &str = "https://127.0.0.1:443/api/v1/calls/stream";
pub const DEFAULT_TOPICS:    &str = "events,cdr";

const PROMPT: &str = "nsbc> ";

// ---------------------------------------------------------------------------
// TLS verify switch (reqwest rustls)
// ---------------------------------------------------------------------------

static SSL_VERIFY: AtomicBool = AtomicBool::new(true);

pub fn set_ssl_verify(verify: bool) { SSL_VERIFY.store(verify, Ordering::Relaxed); }

fn build_reqwest_client(timeout_secs: u64) -> reqwest::Result<reqwest::blocking::Client> {
    let mut builder = reqwest::blocking::Client::builder()
        .timeout(Duration::from_secs(timeout_secs))
        .user_agent("nsbc-event-client/0.1");
    if !SSL_VERIFY.load(Ordering::Relaxed) {
        builder = builder.danger_accept_invalid_certs(true);
    }
    builder.build()
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

#[derive(Debug)]
pub enum NsbcError {
    Io(std::io::Error),
    UdsNotFound(String),
    Http(reqwest::Error),
    Other(String),
}

impl std::fmt::Display for NsbcError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            NsbcError::Io(e)       => write!(f, "IO: {e}"),
            NsbcError::UdsNotFound(p) => write!(f, "UDS not found: {p}"),
            NsbcError::Http(e)     => write!(f, "HTTP: {e}"),
            NsbcError::Other(s)    => write!(f, "{s}"),
        }
    }
}

impl From<std::io::Error> for NsbcError  { fn from(e: std::io::Error) -> Self { NsbcError::Io(e) } }
impl From<reqwest::Error>    for NsbcError { fn from(e: reqwest::Error)    -> Self { NsbcError::Http(e) } }

// ---------------------------------------------------------------------------
// Frame type + parser
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct Frame {
    pub ts:       String,
    pub domain:   String,
    pub kind:     String,
    pub fields:   HashMap<String, String>,
    pub cdr_json: Option<serde_json::Value>,
    pub raw:      String,
    pub source:   String, // "uds" | "http"
}

pub fn parse_frame(line: &str) -> Option<Frame> {
    let line = line.trim_end_matches(&['\n', '\r'][..]);
    if line.len() < 28 { return None; }
    let ts = &line[..23];
    let rest = &line[23..];
    if !rest.starts_with(' ') { return None; }
    let rest = &rest[1..];
    let domain_end = rest.find(' ').unwrap_or(rest.len());
    let domain = &rest[..domain_end];
    let body = if domain_end < rest.len() { &rest[domain_end + 1..] } else { "" };

    let mut fields = HashMap::new();
    let mut cdr_json = None;
    if domain == "CDR" {
        cdr_json = serde_json::from_str(body).ok();
    } else {
        for part in body.split_whitespace() {
            if let Some(eq) = part.find('=') {
                fields.insert(part[..eq].to_string(), part[eq + 1..].to_string());
            }
        }
    }
    Some(Frame {
        ts: ts.to_string(), domain: domain.to_string(),
        kind: classify_domain(domain),
        fields, cdr_json, raw: line.to_string(), source: "uds".to_string(),
    })
}

fn classify_domain(domain: &str) -> String {
    if domain.starts_with("CALL_") || domain == "CALL_OUTGOING" { "call" }
    else if domain == "CDR" { "cdr" }
    else if domain == "REGISTER" { "register" }
    else if domain == "OK" || domain == "ERR" || domain.starts_with("subscribed:") { "cmd" }
    else { "other" }.into()
}

// ---------------------------------------------------------------------------
// ClientUds — Unix Domain Socket (Unix only, std::os::unix::net)
// ---------------------------------------------------------------------------

#[cfg(unix)]
pub struct ClientUds {
    path:   String,
    sock:   std::os::unix::net::UnixStream,
    banner: String,
}

#[cfg(unix)]
impl ClientUds {
    pub fn new(uds_path: &str) -> Result<Self, NsbcError> {
        if !Path::new(uds_path).exists() {
            return Err(NsbcError::UdsNotFound(uds_path.to_string()));
        }
        Ok(ClientUds {
            path: uds_path.to_string(),
            sock: std::os::unix::net::UnixStream::connect(uds_path)?,
            banner: String::new(),
        })
    }

    pub fn connect(mut self) -> Result<Self, NsbcError> {
        use std::io::{BufRead, BufReader};
        let mut reader = BufReader::new(self.sock.try_clone()?);
        let deadline = std::time::SystemTime::now() + Duration::from_secs(2);
        let mut line = String::new();
        while std::time::SystemTime::now() < deadline {
            line.clear();
            let n = match reader.read_line(&mut line) { Ok(n) => n, Err(_) => break };
            if n == 0 { break; }
            if line.starts_with("NSBC ") { self.banner.push_str(line.trim_end()); }
            if line.ends_with(PROMPT) { break; }
        }
        Ok(self)
    }

    pub fn banner(&self) -> &str { &self.banner }
    pub fn close(mut self) { let _ = self.sock.shutdown(std::net::Shutdown::Both); }

    pub fn send_command(&mut self, cmd: &str, timeout_secs: u64) -> Result<String, NsbcError> {
        use std::io::{BufRead, BufReader, Write};
        self.sock.write_all(format!("{}\n", cmd.trim_end_matches('\n')).as_bytes())?;
        self.sock.set_read_timeout(Some(Duration::from_secs(timeout_secs))).ok();

        let mut reader = BufReader::new(self.sock.try_clone()?);
        let mut sb = String::new();
        loop {
            let mut line = String::new();
            match reader.read_line(&mut line) {
                Ok(0) | Err(_) => break,
                Ok(_) => { sb.push_str(&line); if sb.contains(PROMPT) { break; } }
            }
        }
        self.sock.set_read_timeout(None).ok();
        if let Some(idx) = sb.rfind(PROMPT) { sb.truncate(idx); }
        Ok(sb.trim_end().to_string())
    }

    pub fn subscribe(&mut self, topics: &str) -> Result<String, NsbcError> {
        self.send_command(&format!("sub {topics}"), 2)
    }

    pub fn iter_frames(self) -> FrameIterator {
        use std::io::BufRead;
        let sock = self.sock.try_clone().unwrap_or_else(|_| {
            std::os::unix::net::UnixStream::connect(&self.path).expect("UDS reconnect failed")
        });
        FrameIterator { reader: BufReader::new(sock) }
    }
}

#[cfg(unix)]
pub struct FrameIterator { reader: std::io::BufReader<std::os::unix::net::UnixStream> }
#[cfg(unix)]
impl Iterator for FrameIterator {
    type Item = Frame;
    fn next(&mut self) -> Option<Frame> {
        use std::io::BufRead;
        let mut line = String::new();
        match self.reader.read_line(&mut line) {
            Ok(0) | Err(_) => None,
            Ok(_) => parse_frame(&line).map(|mut f| { f.source = "uds".into(); f }),
        }
    }
}

#[cfg(not(unix))]
pub struct ClientUds;
#[cfg(not(unix))]
impl ClientUds {
    pub fn new(_: &str) -> Result<Self, NsbcError> { Err(NsbcError::UdsNotFound(
        "ClientUds requires Unix (Linux/macOS/WSL)".into())) }
    pub fn connect(self) -> Result<Self, NsbcError> { Ok(self) }
    pub fn banner(&self) -> &str { "" }
    pub fn close(self) {}
    pub fn send_command(&mut self, _: &str, _: u64) -> Result<String, NsbcError> { Ok(String::new()) }
    pub fn subscribe(&mut self, _: &str) -> Result<String, NsbcError> { Ok(String::new()) }
    pub fn iter_frames(self) -> FrameIterator { FrameIterator }
}
#[cfg(not(unix))]
pub struct FrameIterator;
#[cfg(not(unix))]
impl Iterator for FrameIterator {
    type Item = Frame;
    fn next(&mut self) -> Option<Frame> { None }
}

// ---------------------------------------------------------------------------
// Originate: call_url builder + dual-path dispatch (UDS preferred → HTTP)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Default)]
pub struct OriginateOpts<'a> {
    pub orig_to:     Option<&'a str>,
    pub app:         Option<&'a str>,
    pub audio:       Option<Vec<&'a str>>,
    pub video:       Option<Vec<&'a str>>,
    pub timeout:     Option<i64>,
    pub caller_id:   Option<&'a str>,
    pub caller_name: Option<&'a str>,
    pub call_url:    Option<&'a str>,
}

pub fn build_call_url(opts: OriginateOpts) -> String {
    if let Some(cu) = opts.call_url { return cu.to_string(); }
    let ext = opts.orig_to.unwrap_or("1000");
    let app = opts.app.unwrap_or("echo");

    let mut parts: Vec<String> = Vec::new();
    if let Some(id) = opts.caller_id   { parts.push(format!("origination_caller_id_number={id}")); }
    if let Some(nm) = opts.caller_name { parts.push(format!("origination_caller_id_name={nm}")); }
    if let Some(t)  = opts.timeout     { parts.push(format!("originate_timeout={t}")); }
    if let Some(a)  = opts.audio       { parts.push(format!("audio_codecs={}", a.join(","))); }
    if let Some(v)  = opts.video       { parts.push(format!("video_codecs={}", v.join(","))); }

    let vars = if parts.is_empty() { String::new() } else { format!("{{{}}}", parts.join(",")) };
    format!("{vars}user/{ext} &{app}")
}

#[derive(Debug)]
pub struct OriginateResult {
    pub ok:      bool,
    pub payload: OriginatePayload,
    pub via:     String, // "uds" | "web" | "both_failed"
}

#[derive(Debug)]
pub enum OriginatePayload {
    Uds(String),
    Http(serde_json::Value),
    BothFailed { uds: String, web: String },
}

pub fn originate(call_url: &str, via: &str, uds_path: &str,
                 api_base: &str, api_token: Option<&str>) -> OriginateResult {
    match via {
        "uds" => {
            let (ok, p) = originate_via_uds(uds_path, call_url);
            OriginateResult { ok, payload: OriginatePayload::Uds(p), via: "uds".into() }
        }
        "web" => {
            let (ok, p) = originate_via_web(api_base, call_url, api_token);
            OriginateResult { ok, payload: p, via: "web".into() }
        }
        _ => {
            let (ok_uds, p_uds) = originate_via_uds(uds_path, call_url);
            if ok_uds {
                OriginateResult { ok: true, payload: OriginatePayload::Uds(p_uds), via: "uds".into() }
            } else {
                let (ok_web, p_web) = originate_via_web(api_base, call_url, api_token);
                if ok_web {
                    OriginateResult { ok: true, payload: p_web, via: "web".into() }
                } else {
                    OriginateResult {
                        ok: false,
                        payload: OriginatePayload::BothFailed { uds: p_uds, web: format!("{p_web:?}") },
                        via: "both_failed".into(),
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SSEClient — Server-Sent Events over HTTP (remote deployments)
// ---------------------------------------------------------------------------

/// Raw parsed SSE event before Frame mapping.
#[derive(Debug, Clone, Default)]
pub struct SseEvent {
    pub event: String,
    pub data:  String,
    pub retry: Option<i64>,
}

/// SseClient consumes nsbc-web /calls/stream over SSE.
///
/// Uses reqwest blocking (same rustls-tls client as HTTP originate path).
/// TLS verify toggle (`set_ssl_verify(false)`) is respected.
pub struct SseClient {
    url:      String,
    timeout:  Duration,
    headers:  Vec<(String, String)>,
}

impl SseClient {
    pub fn new(url: impl Into<String>, timeout_secs: u64, headers: Vec<(impl Into<String>, impl Into<String>)>) -> Self {
        Self {
            url:      url.into(),
            timeout:  Duration::from_secs(timeout_secs),
            headers:  headers.into_iter().map(|(k, v)| (k.into(), v.into())).collect(),
        }
    }

    /// Connect once and iterate the stream forever. Auto-reconnects on disconnect.
    /// Closure receives a Frame (Some) on each valid event, or None if JSON parse failed.
    pub fn run<F>(&self, mut on_frame: F) -> !
    where F: FnMut(Option<Frame>)
    {
        loop {
            if let Err(e) = self.run_once(&mut on_frame) {
                eprintln!("[sse] stream error: {e} — reconnecting in 3s");
                std::thread::sleep(Duration::from_secs(3));
            }
        }
    }

    fn run_once<F>(&self, on_frame: &mut F) -> Result<(), NsbcError>
    where F: FnMut(Option<Frame>)
    {
        let client = build_reqwest_client(self.timeout.as_secs().max(1) * 10)?;
        let mut req_builder = client.get(&self.url)
            .header(reqwest::header::ACCEPT, "text/event-stream")
            .header(reqwest::header::CACHE_CONTROL, "no-cache");
        for (k, v) in &self.headers {
            req_builder = req_builder.header(k, v);
        }
        let resp = req_builder.send()?;
        if resp.status().as_u16() >= 400 {
            let code = resp.status().as_u16();
            return Err(NsbcError::Other(format!("SSE HTTP {code}")));
        }
        let mut reader = std::io::BufReader::new(resp);
        let mut event_name = String::from("message");
        let mut data_lines: Vec<String> = Vec::new();
        let mut retry_ms: Option<i64> = None;
        let mut line_buf = String::new();

        loop {
            line_buf.clear();
            let n = reader.read_line(&mut line_buf)?;
            if n == 0 { break; }
            let line = line_buf.trim_end_matches(&['\n', '\r'][..]);

            if line.is_empty() {
                if !data_lines.is_empty() || event_name != "message" {
                    let ev = SseEvent {
                        event: std::mem::take(&mut event_name),
                        data:  data_lines.join("\n"),
                        retry: retry_ms.take(),
                    };
                    event_name = String::from("message");
                    data_lines.clear();
                    if let Some(f) = sse_event_to_frame(&ev) {
                        on_frame(Some(f));
                    } else {
                        on_frame(None);
                    }
                }
                data_lines.clear();
                continue;
            }

            if line.starts_with(':') {
                continue;
            }
            if let Some(val) = line.strip_prefix("event:") {
                let v = val.trim();
                event_name = if v.is_empty() { String::from("message") } else { v.to_string() };
            } else if let Some(val) = line.strip_prefix("data:") {
                let v = if val.starts_with(' ') { &val[1..] } else { val };
                data_lines.push(v.to_string());
            } else if let Some(val) = line.strip_prefix("retry:") {
                if let Ok(n) = val.trim().parse::<i64>() { retry_ms = Some(n); }
            }
        }
        Ok(())
    }
}

/// Convert a raw SseEvent → the canonical Frame (source="sse").
/// Returns None if event.data is not valid JSON.
///
/// nsbc-web /calls/stream emits SSE event=session where payload.event is
/// "ring"/"answer"/"hangup"/"originate" — mapped to CALL_INCOMING / CALL_ANSWERED / CALL_END / CALL_OUTGOING.
pub fn sse_event_to_frame(ev: &SseEvent) -> Option<Frame> {
    let blob: serde_json::Value = serde_json::from_str(&ev.data).ok()?;
    let kind_outer = blob.get("kind").and_then(|v| v.as_str()).unwrap_or("");

    // Prefer blob.payload; fall back to blob itself if no payload wrapper
    let payload: &serde_json::Map<String, serde_json::Value> =
        if let Some(p) = blob.get("payload").and_then(|v| v.as_object()) { p }
        else { blob.as_object()? };

    let sse_to_domain: &[(&str, &str)] = &[
        ("ring", "CALL_INCOMING"),
        ("answer", "CALL_ANSWERED"),
        ("hangup", "CALL_END"),
        ("originate", "CALL_OUTGOING"),
    ];
    let sse_evt = payload.get("event").and_then(|v| v.as_str()).unwrap_or("").to_lowercase();
    let mut domain = sse_to_domain.iter()
        .find(|(k, _)| *k == sse_evt)
        .map(|(_, v)| (*v).to_string())
        .unwrap_or_default();
    if domain.is_empty() {
        domain = if !sse_evt.is_empty() { sse_evt.to_uppercase() }
                 else if !kind_outer.is_empty() { kind_outer.to_uppercase() }
                 else { "EVENT".to_string() };
    }
    let kind = classify_domain(&domain);

    let ts = payload.get("ts").and_then(|v| v.as_str())
        .or_else(|| payload.get("timestamp").and_then(|v| v.as_str()))
        .map(|s| s.to_string())
        .unwrap_or_else(|| format!("{}ms", std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis()).unwrap_or(0)));

    let mut fields = HashMap::new();
    let cdr_json = Some(serde_json::Value::Object(payload.clone()));
    for (k, v) in payload {
        fields.insert(k.clone(), v.to_string());
    }

    Some(Frame {
        ts, domain, kind,
        fields,
        cdr_json,
        raw: serde_json::to_string(&blob).unwrap_or_else(|_| ev.data.clone()),
        source: "sse".to_string(),
    })
}

fn originate_via_uds(uds_path: &str, call_url: &str) -> (bool, String) {
    if !Path::new(uds_path).exists() { return (false, format!("UDS not found: {uds_path}")); }
    #[cfg(unix)]
    {
        let sock = match std::os::unix::net::UnixStream::connect(uds_path) {
            Ok(s) => s, Err(e) => return (false, format!("dial: {e}")),
        };
        let mut c = ClientUds { path: uds_path.to_string(), sock, banner: String::new() };
        let Ok(resp) = c.send_command(&format!("originate {call_url}"), 10) else {
            return (false, "command failed".into());
        };
        let text = resp.trim().trim_end_matches('>').trim().to_string();
        if text.starts_with("OK ") || text.contains("\nOK ") || text == "OK" { return (true, text); }
        if text.starts_with("ERR") || text.contains("ERR ") { return (false, text); }
        if text.is_empty() { return (true, "dispatched (async)".into()); }
        (true, text)
    }
    #[cfg(not(unix))] { let _ = call_url; (false, "not on unix".into()) }
}

fn originate_via_web(api_base: &str, call_url: &str, api_token: Option<&str>) -> (bool, OriginatePayload) {
    let url = format!("{}/ext/v1/originate", api_base.trim_end_matches('/'));
    let body = serde_json::json!({ "call_url": call_url }).to_string();

    let Ok(client) = build_reqwest_client(10) else {
        return (false, OriginatePayload::Http(serde_json::json!({"network_error": "reqwest init"})));
    };
    let mut req = client.post(&url)
        .header(reqwest::header::CONTENT_TYPE, "application/json")
        .header(reqwest::header::ACCEPT, "application/json")
        .body(body);
    if let Some(tok) = api_token {
        req = req.header(reqwest::header::AUTHORIZATION, format!("Bearer {tok}"));
    }

    match req.send() {
        Ok(resp) => {
            let status = resp.status().as_u16();
            let text = resp.text().unwrap_or_default();
            if (200..400).contains(&status) {
                let v: serde_json::Value = serde_json::from_str(&text)
                    .unwrap_or(serde_json::Value::String(text.clone()));
                (true, OriginatePayload::Http(v))
            } else {
                (false, OriginatePayload::Http(serde_json::json!({
                    "http_error": status, "detail": text
                })))
            }
        }
        Err(e) => (false, OriginatePayload::Http(serde_json::json!({
            "network_error": e.to_string()
        }))),
    }
}

// ---------------------------------------------------------------------------
// CallCorrelator
// ---------------------------------------------------------------------------

pub struct CallCorrelator<F: FnMut(&CallTimeline)> {
    calls: HashMap<String, CallTimeline>,
    on_complete: F,
}

#[derive(Debug, Clone)]
pub struct CallTimeline {
    pub call_id: String,
    pub events:  Vec<TimelineEvent>,
    pub cdr:     Option<HashMap<String, String>>,
    pub start_ts: String,
    pub end_ts:  Option<String>,
}

#[derive(Debug, Clone)]
pub struct TimelineEvent {
    pub ts:     String,
    pub domain: String,
    pub fields: HashMap<String, String>,
}

impl<F: FnMut(&CallTimeline)> CallCorrelator<F> {
    pub fn new(on_complete: F) -> Self { CallCorrelator { calls: HashMap::new(), on_complete } }

    pub fn feed(&mut self, frame: &Frame) -> Option<CallTimeline> {
        let call_id = frame.fields.get("call_id")
            .or_else(|| frame.fields.get("callid"))?.clone();

        let tl = self.calls.entry(call_id.clone()).or_insert_with(|| CallTimeline {
            call_id: call_id.clone(), events: Vec::new(),
            cdr: None, start_ts: frame.ts.clone(), end_ts: None,
        });
        tl.events.push(TimelineEvent {
            ts: frame.ts.clone(), domain: frame.domain.clone(),
            fields: frame.fields.clone(),
        });
        if frame.domain == "CDR" {
            tl.cdr = Some(frame.fields.clone());
            tl.end_ts = Some(frame.ts.clone());
            (self.on_complete)(tl);
            Some(tl.clone())
        } else { None }
    }

    pub fn snapshot(&self) -> HashMap<String, CallTimeline> { self.calls.clone() }
}

// ---------------------------------------------------------------------------
// WebhookSink
// ---------------------------------------------------------------------------

pub struct WebhookSink {
    url:     String,
    timeout: u64,
    retries: u32,
}

impl WebhookSink {
    pub fn new(url: &str, timeout_secs: u64, retries: u32) -> Self {
        WebhookSink { url: url.to_string(), timeout: timeout_secs, retries }
    }

    pub fn send(&self, frame: &Frame) {
        let url = self.url.clone();
        let timeout = self.timeout;
        let retries = self.retries;
        let body = serde_json::json!({
            "ts": frame.ts, "domain": frame.domain, "kind": frame.kind,
            "fields": frame.fields, "raw": frame.raw
        }).to_string();
        let domain = frame.domain.clone();
        std::thread::spawn(move || {
            let Ok(client) = build_reqwest_client(timeout) else { return; };
            for attempt in 0..=retries {
                let resp = client.post(&url)
                    .header(reqwest::header::CONTENT_TYPE, "application/json")
                    .header("X-NSBC-Event", &domain)
                    .body(body.clone())
                    .send();
                match resp {
                    Ok(r) if r.status().as_u16() < 400 => return,
                    _ => {
                        if attempt < retries {
                            std::thread::sleep(Duration::from_millis(500 * (attempt + 1) as u64));
                        }
                    }
                }
            }
        });
    }
}
