// NSBC Event/CDR subscription & originate client (Rust CLI demo).
//
// Thin shell — all real API lives in nsbc_sdk (src/lib.rs).
// This file is ONLY flag parsing + mode dispatch + pretty printing.
//
// Rust std-only version: UDS + originate + parse + correlate.
// HTTP originate / webhook forward → use Python or Go client.

use nsbc_sdk::*;
use std::io::Write;
use std::path::Path;
use std::time::Duration;

// ---------------------------------------------------------------------------
// Flag parsing (minimal — zero extra deps)
// ---------------------------------------------------------------------------

struct Args {
    mode:        String,
    uds:         String,
    api_base:    String,
    sse_url:     String,
    transport:   String,
    topics:      String,
    orig_via:    String,
    call_url:    Option<String>,
    orig_to:     String,
    app:         String,
    caller_id:   Option<String>,
    caller_name: Option<String>,
    timeout:     Option<i64>,
    video:       bool,
    audio:       bool,
    api_token:   Option<String>,
    webhook_url: Option<String>,
    exit_on_cdr: bool,
    insecure:    bool,
    help:        bool,
}

fn parse_args() -> Args {
    let mut a = Args {
        mode:        "auto".into(),
        uds:         DEFAULT_UDS.into(),
        api_base:    DEFAULT_API_BASE.into(),
        sse_url:     DEFAULT_SSE_URL.into(),
        transport:   "uds".into(),
        topics:      DEFAULT_TOPICS.into(),
        orig_via:    "auto".into(),
        call_url:    None,
        orig_to:     "1000".into(),
        app:         "echo".into(),
        caller_id:   None,
        caller_name: None,
        timeout:     None,
        video:       true,
        audio:       true,
        api_token:   None,
        webhook_url: None,
        exit_on_cdr: false,
        insecure:    false,
        help:        false,
    };
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--mode"        => { if let Some(v) = it.next() { a.mode = v; } }
            "--uds"         => { if let Some(v) = it.next() { a.uds = v; } }
            "--api-base"    => { if let Some(v) = it.next() { a.api_base = v; } }
            "--sse-url"     => { if let Some(v) = it.next() { a.sse_url = v; } }
            "--transport"   => { if let Some(v) = it.next() { a.transport = v; } }
            "--topics"      => { if let Some(v) = it.next() { a.topics = v; } }
            "--orig-via"    => { if let Some(v) = it.next() { a.orig_via = v; } }
            "--call-url"    => { a.call_url = it.next(); }
            "--orig-to"     => { if let Some(v) = it.next() { a.orig_to = v; } }
            "--app"         => { if let Some(v) = it.next() { a.app = v; } }
            "--caller-id"   => { a.caller_id = it.next(); }
            "--caller-name" => { a.caller_name = it.next(); }
            "--timeout"     => { a.timeout = it.next().and_then(|s| s.parse().ok()); }
            "--video"       => { a.video = true; }
            "--no-video"    => { a.video = false; }
            "--audio"       => { a.audio = true; }
            "--no-audio"    => { a.audio = false; }
            "--api-token"   => { a.api_token = it.next(); }
            "--webhook-url" => { a.webhook_url = it.next(); }
            "--exit-on-cdr" => { a.exit_on_cdr = true; }
            "--insecure"    => { a.insecure = true; }
            "-h" | "--help" => { a.help = true; }
            _ => {}
        }
    }
    a
}

fn print_help() {
    println!("nsbc-event-client — Rust CLI demo for nsbc_sdk");
    println!();
    println!("Usage:");
    println!("  cargo run --release -- --orig-to 7gja30rf --video    # WebRTC video (default on)");
    println!("  cargo run --release -- --mode subscribe              # pure UDS observer");
    println!("  cargo run --release -- --mode subscribe --transport sse --insecure");
    println!("  cargo run --release -- --mode originate --orig-to 1001 --no-video");
}

// ---------------------------------------------------------------------------
// Colors (ANSI on Unix only)
// ---------------------------------------------------------------------------

fn color_for(domain: &str) -> &'static str {
    #[cfg(unix)]
    {
        match domain {
            "CALL_INCOMING" => "\x1b[36m", "CALL_ANSWERED" => "\x1b[32m",
            "CALL_END"      => "\x1b[31m", "REGISTER"      => "\x1b[33m",
            "CDR"           => "\x1b[35m", _               => "\x1b[37m",
        }
    }
    #[cfg(not(unix))] { "" }
}

fn banner(msg: &str) { writeln!(std::io::stdout(), "[nsbc] {msg}").ok(); }

fn print_frame(f: &Frame) {
    let short_ts = if f.ts.len() >= 23 { &f.ts[11..23] } else { &f.ts };
    let cc = color_for(&f.domain);
    if f.domain == "CDR" {
        let from = f.fields.get("from").map(|s| s.as_str()).unwrap_or("?");
        let to = f.fields.get("to").map(|s| s.as_str()).unwrap_or("?");
        let disp = f.fields.get("disposition").map(|s| s.as_str()).unwrap_or("?");
        let dur = f.fields.get("duration_s").map(|s| s.as_str()).unwrap_or("?");
        let bill = f.fields.get("billsec").map(|s| s.as_str()).unwrap_or("?");
        println!("{short_ts} {cc}{:>12}\x1b[0m {}→{} {} dur={dur}s bill={bill}s",
            "CDR", from, to, disp);
    } else {
        let mut parts: Vec<String> = Vec::new();
        for k in ["call_id","orig","to","src_ip","trunk","profile","hangup_by"] {
            if let Some(v) = f.fields.get(k) { parts.push(format!("{k}={v}")); }
        }
        for (k, v) in &f.fields {
            if !["call_id","orig","to","src_ip","trunk","profile","hangup_by"].contains(&k.as_str()) {
                parts.push(format!("{k}={v}"));
            }
        }
        let body = if parts.is_empty() { f.raw.clone() } else { parts.join(" ") };
        println!("{short_ts} {cc}{:>15}\x1b[0m {body}", f.domain);
    }
}

fn print_timeline(tl: &CallTimeline) {
    println!();
    println!("══════ Call Lifecycle: {} ══════", tl.call_id);
    for ev in &tl.events {
        let cc = color_for(&ev.domain);
        println!("  {cc}{:>15}\x1b[0m  {}", ev.domain, ev.ts);
    }
    if let Some(cdr) = &tl.cdr {
        println!("  {:>12}  CDR record", "CDR");
        let from = cdr.get("from").map(|s| s.as_str()).unwrap_or("?");
        let to = cdr.get("to").map(|s| s.as_str()).unwrap_or("?");
        println!("    {from} → {to}");
    }
    println!("──────────────────────────────────");
    println!();
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

fn build_opts(args: &Args) -> OriginateOpts<'_> {
    let mut o = OriginateOpts::default();
    o.orig_to = Some(&args.orig_to);
    o.app = Some(&args.app);
    o.call_url = args.call_url.as_deref();
    if let Some(id) = &args.caller_id   { o.caller_id   = Some(id.as_str()); }
    if let Some(nm) = &args.caller_name { o.caller_name = Some(nm.as_str()); }
    o.timeout = args.timeout;
    if args.audio { o.audio = Some(vec!["PCMU","PCMA","G722","OPUS"]); }
    if args.video { o.video = Some(vec!["H264","VP8"]); }
    o
}

fn main() {
    let args = parse_args();
    if args.help { print_help(); return; }

    if args.insecure {
        set_ssl_verify(false);
    }

    let mode = if args.mode == "auto" { "originate+subscribe" } else { args.mode.as_str() };
    match mode {
        "originate"           => run_originate_only(&args),
        "subscribe"           => run_subscribe(&args),
        "originate+subscribe" => run_originate_and_subscribe(&args),
        _ => { eprintln!("unknown mode: {mode}"); std::process::exit(1); }
    }
}

fn run_originate_only(args: &Args) {
    let call_url = build_call_url(build_opts(args));
    banner(&format!("[originate via={}  call_url={call_url}]", args.orig_via));
    let r = originate(&call_url, &args.orig_via, &args.uds, &args.api_base, args.api_token.as_deref());
    if r.ok {
        println!("[OK] via={} → {:?}", r.via, r.payload);
    } else {
        eprintln!("[FAIL] via={} {:?}", r.via, r.payload);
        std::process::exit(3);
    }
}

fn run_subscribe(args: &Args) { run_transport_loop(args); }

fn run_originate_and_subscribe(args: &Args) {
    let call_url = build_call_url(build_opts(args));
    banner(&format!("[1/2] originate via={}  call_url={call_url}]", args.orig_via));
    let r = originate(&call_url, &args.orig_via, &args.uds, &args.api_base, args.api_token.as_deref());
    if !r.ok {
        eprintln!("[FAIL] originate {:?}", r.payload);
        std::process::exit(3);
    }
    println!("[OK] originate dispatched via {} → {:?}", r.via, r.payload);
    banner("[2/2] subscribing — waiting for events ...");
    run_transport_loop(args);
}

fn run_transport_loop(args: &Args) {
    let mut correlator = CallCorrelator::new(print_timeline);
    let webhook: Option<WebhookSink> = args.webhook_url.as_ref()
        .map(|u| WebhookSink::new(u, 5, 2));
    if let Some(ref wh) = webhook {
        banner(&format!("[webhook] every event → POST {}", args.webhook_url.as_deref().unwrap_or_default()));
        let _ = wh;
    }

    let mut handle_frame = |frame: Frame| {
        print_frame(&frame);
        correlator.feed(&frame);
        if let Some(ref wh) = webhook { wh.send(&frame); }
        if args.exit_on_cdr && frame.domain == "CDR" {
            std::thread::sleep(Duration::from_millis(100));
            std::process::exit(0);
        }
    };

    if args.transport == "sse" {
        let mut headers = Vec::new();
        if let Some(tok) = args.api_token.as_deref() {
            headers.push(("Authorization", format!("Bearer {tok}")));
        }
        let client = SseClient::new(args.sse_url.clone(), 30, headers);
        banner(&format!("[transport=SSE, url={} — Ctrl-C to quit]", args.sse_url));
        client.run(|opt_frame| {
            if let Some(f) = opt_frame { handle_frame(f); }
        });
    } else {
        // UDS path
        if !Path::new(&args.uds).exists() {
            eprintln!("[FAIL] UDS not found: {} (try --transport sse)", args.uds);
            std::process::exit(2);
        }
        let mut client = match ClientUds::new(&args.uds) {
            Ok(c) => c.connect().unwrap_or_else(|e| {
                eprintln!("[FAIL] connect: {e}");
                std::process::exit(2);
            }),
            Err(e) => { eprintln!("[FAIL] {e}"); std::process::exit(2); }
        };
        banner(client.banner());

        let resp = match client.subscribe(&args.topics) {
            Ok(r) => r, Err(e) => { eprintln!("[sub err] {e}"); String::new() }
        };
        for ln in resp.lines() {
            let s = ln.trim();
            if !s.is_empty() { banner(&format!("[sub] {s}")); }
        }
        banner(&format!("[transport=UDS, topics='{}' — Ctrl-C to quit]", args.topics));

        for frame in client.iter_frames() {
            handle_frame(frame);
        }
    }
}
