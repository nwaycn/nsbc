// NSBC realtime event/CDR subscription client (Unix Domain Socket / Rust)
//
// Zero third-party deps: pure std. Single ~200KB binary after cargo build --release.
//
// Usage:
//   cargo run --release --                                       # default /tmp/nsbc-e2e/control.sock
//   cargo run --release -- /var/run/nsbc/control.sock
//   cargo run --release -- --topics events,cdr,log
//   cargo build --release && ./target/release/nsbc-event-client

#[cfg(not(unix))]
fn main() {
    eprintln!("this client requires Unix (AF_UNIX). build on Linux/macOS/WSL.");
    std::process::exit(1);
}

#[cfg(unix)]
fn main() -> std::io::Result<()> {
    use std::env;
    use std::io::{BufRead, BufReader, Write};
    use std::net::Shutdown;
    use std::os::unix::net::UnixStream;
    use std::path::Path;
    use std::time::{Duration, SystemTime, UNIX_EPOCH};

    const PROMPT: &str = "nsbc> ";
    const BANNER: &str = "NSBC ";
    const RESET: &str = "\x1b[0m";

    let args: Vec<String> = env::args().collect();
    let mut uds_path: String = "/tmp/nsbc-e2e/control.sock".to_string();
    let mut topics: String = "events,cdr".to_string();
    let mut pull_only = false;

    let mut i = 1;
    while i < args.len() {
        let a = &args[i];
        match a.as_str() {
            "--topics" => { topics = args[i + 1].clone(); i += 2; }
            "--pull-only" => { pull_only = true; i += 1; }
            "--help" | "-h" => {
                eprintln!("Usage: nsbc-event-client [uds_path] [--topics events,cdr] [--pull-only]");
                return Ok(());
            }
            s if !s.starts_with('-') => { uds_path = s.to_string(); i += 1; }
            _ => { i += 1; }
        }
    }

    // socket file check
    if !Path::new(&uds_path).exists() {
        eprintln!("ERR: UDS socket not found: {}", uds_path);
        std::process::exit(2);
    }

    // Connect
    let mut sock = UnixStream::connect(&uds_path)?;
    println!("\x1b[2m[connected to {}]{}", uds_path, RESET);

    // Receive banner + prompt (short 2s timeout)
    let mut reader = BufReader::new(sock.try_clone()?);
    let deadline = SystemTime::now() + Duration::from_secs(2);
    let mut line = String::new();
    while SystemTime::now() < deadline {
        line.clear();
        let n = match reader.read_line(&mut line) {
            Ok(n) => n,
            Err(_) => break,
        };
        if n == 0 { break; }
        if line.starts_with(BANNER) {
            println!("\x1b[2m{}{}", line.trim_end(), RESET);
        }
        if line.ends_with(PROMPT) { break; }
    }

    if !pull_only {
        write!(sock, "sub {}\n", topics)?;
        sock.flush()?;
        // Receive sub response + prompt
        let deadline = SystemTime::now() + Duration::from_secs(2);
        while SystemTime::now() < deadline {
            line.clear();
            let n = match reader.read_line(&mut line) {
                Ok(n) => n,
                Err(_) => break,
            };
            if n == 0 { break; }
            let t = line.trim();
            if !t.is_empty() && !t.starts_with(PROMPT) {
                println!("\x1b[2m[sub] {}{}", t, RESET);
            }
            if line.ends_with(PROMPT) { break; }
        }
        println!("\x1b[2m[subscribed: {} — Ctrl-C to quit]{}", topics, RESET);
    }

    // Main loop: blocking readline + frame parse
    loop {
        line.clear();
        match reader.read_line(&mut line) {
            Ok(0) => {
                eprintln!("\n\x1b[2m[server closed]{}", RESET);
                break;
            }
            Err(e) => {
                eprintln!("\n\x1b[31m[disconnected: {}]{}", e, RESET);
                break;
            }
            Ok(_) => {
                print_frame(&line.trim_end_matches(['\n', '\r']), RESET);
            }
        }
    }
    let _ = sock.shutdown(Shutdown::Both);
    Ok(())
}

#[cfg(unix)]
fn color_for(domain: &str) -> &'static str {
    match domain {
        "CALL_INCOMING" => "\x1b[36m",
        "CALL_ANSWERED" => "\x1b[32m",
        "CALL_END"      => "\x1b[31m",
        "REGISTER"      => "\x1b[33m",
        "CDR"           => "\x1b[35m",
        _               => "\x1b[37m",
    }
}

/// Hand-written frame parser to avoid a regex dependency.
/// Frame format: YYYY-MM-DDTHH:MM:SS.mmm DOMAIN body
#[cfg(unix)]
fn print_frame(line: &str, reset: &str) {
    if line.is_empty() { return; }

    // Command response / banner recognition
    if line.starts_with("NSBC ") || line.starts_with("OK ") ||
       line.starts_with("ERR ") || line.starts_with("subscribed:") {
        println!("\x1b[2m[cmd] {}{}", line, reset);
        return;
    }

    // Frame-header timestamp: 23 chars "YYYY-MM-DDTHH:MM:SS.mmm"
    // Quick check: long enough + contains 'T' and 3-digit milliseconds
    if line.len() < 28 { return; }
    let ts = &line[..23];
    let rest = &line[23..];
    if !rest.starts_with(' ') { return; }
    let rest = &rest[1..];

    // Find first space to split off DOMAIN
    let domain_end = rest.find(' ').unwrap_or(rest.len());
    let domain = &rest[..domain_end];
    let body = if domain_end < rest.len() { &rest[domain_end + 1..] } else { "" };

    // Shorten timestamp for display: 15:04:05.000
    let ts_short = if ts.len() >= 12 { &ts[11..23] } else { ts };

    let c = color_for(domain);
    if domain == "CDR" {
        println!("{} {:>12}{} {}", ts_short, "CDR", reset, body);
    } else {
        println!("{} {}{:>15}{} {}", ts_short, c, domain, reset, body);
    }
}
