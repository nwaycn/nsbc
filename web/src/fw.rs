//! nftables firewall synchronizer for nsbc.
//!
//! Purpose: push the core's dynamic ban list (security.ban.*) down into kernel
//! nftables so that packets from banned IPs are dropped at NIC level — before
//! they reach the nsbc UDP socket (no reactor wake-up, no slab pressure).
//!
//! Scope: this module only manages the **nsbc-auto-ban** table/chain/set.
//! All other nftables rules on the host are left untouched (operator owns them).
//!
//! Architecture (zero-dependency, std::process::Command only):
//!   Table:  inet  nsbc_ban
//!   Chain:  input    — hook input priority -100 (runs BEFORE default INPUT)
//!   Sets:   banned_ips  (ipv4) + banned_ips6 (ipv6), size 10240, auto-merge
//!   Rule:   ip saddr @banned_ips  drop comment "nsbc-auto-ban"
//!           ip6 saddr @banned_ips6 drop comment "nsbc-auto-ban"
//!
//! All mutations are idempotent — the only destructive op is `flush set banned_ips`
//! inside sync_all(), which is intentional (全量替换 = 差集，幂等).

use std::collections::BTreeSet;
use std::process::{Command, Stdio};
use std::sync::Arc;
use std::time::Duration;

use tokio::time::MissedTickBehavior;
use tracing::{error, info, warn};

use crate::coreclient::{CoreApi, CoreError};

pub const NFT_TABLE: &str = "inet nsbc_ban";
pub const NFT_CHAIN: &str = "inet nsbc_ban input";
pub const NFT_SET_V4: &str = "inet nsbc_ban banned_ips";
pub const NFT_SET_V6: &str = "inet nsbc_ban banned_ips6";

#[allow(dead_code)]
#[derive(Debug)]
pub enum FwError {
    NftMissing(String),
    NftFailed { cmd: String, stderr: String },
    #[allow(dead_code)]
    Parse(String),
    Core(CoreError),
}

impl std::fmt::Display for FwError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            FwError::NftMissing(e) => write!(f, "nft not found or not executable: {e}"),
            FwError::NftFailed { cmd, stderr } => {
                write!(f, "nft command failed ({cmd}): {stderr}")
            }
            FwError::Parse(e) => write!(f, "fw parse error: {e}"),
            FwError::Core(e) => write!(f, "core: {e}"),
        }
    }
}
impl std::error::Error for FwError {}
impl From<CoreError> for FwError {
    fn from(e: CoreError) -> Self { FwError::Core(e) }
}

impl FwError {
    fn nft_failed(cmd: &str, stderr: String) -> Self {
        Self::NftFailed {
            cmd: cmd.to_string(),
            stderr: if stderr.len() > 400 {
                let mut s = stderr;
                s.truncate(400);
                s
            } else {
                stderr
            },
        }
    }
}

fn ipv4(s: &str) -> bool {
    // minimal: contains '.' and 4 dot-separated octets
    let parts: Vec<&str> = s.split('.').collect();
    if parts.len() != 4 {
        return false;
    }
    parts.iter().all(|p| {
        p.len() >= 1
            && p.len() <= 3
            && p.chars().all(|c| c.is_ascii_digit())
            && p.parse::<u16>().map(|v| v <= 255).unwrap_or(false)
    })
}

/// Ensure the nsbc_ban table/chain/sets exist.
/// Safe to call repeatedly (idempotent — `add` fails silently when object already exists).
pub fn ensure_table() -> Result<(), FwError> {
    // Quick pre-flight: is nft present and executable?
    if Command::new("nft").arg("--version").output().is_err() {
        return Err(FwError::NftMissing(
            "nft not found; install `nftables` package (apt install nftables)".into(),
        ));
    }

    // table — `add` returns error when exists; we ignore that.
    for stmt in [
        format!("add table {NFT_TABLE}"),
        format!("add chain {NFT_CHAIN} {{ type filter hook input priority -100; }}"),
        format!("add set {NFT_SET_V4} {{ type ipv4_addr; flags interval; size 10240; }}"),
        format!("add set {NFT_SET_V6} {{ type ipv6_addr; flags interval; size 10240; }}"),
        format!(
            "add rule  {NFT_CHAIN} ip saddr @banned_ips drop comment \"nsbc-auto-ban\""
        ),
        format!(
            "add rule  {NFT_CHAIN} ip6 saddr @banned_ips6 drop comment \"nsbc-auto-ban\""
        ),
    ] {
        // use `|| true` style via ignore success only — errors (e.g. "object exists") are OK
        let _ = run(&stmt);
    }
    Ok(())
}

fn run(stmt: &str) -> Result<String, FwError> {
    let out = Command::new("nft")
        .arg(stmt)
        .stdin(Stdio::null())
        .output()
        .map_err(|e| FwError::NftMissing(format!("{e}")))?;
    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr).to_string();
        // Silently tolerate "object already exists" — that's just ensure_table idempotency.
        let trimmed = stderr.trim();
        if trimmed.contains("already exists")
            || trimmed.contains("already exists.")
            || trimmed.contains("No such file or directory")
        {
            return Ok(String::new());
        }
        return Err(FwError::nft_failed(stmt, trimmed.to_string()));
    }
    Ok(String::from_utf8_lossy(&out.stdout).to_string())
}

/// Add ONE ip to the nsbc_ban set (v4 or v6 determined by the literal).
/// No-op if already present.
pub fn add_ip(ip: &str) -> Result<(), FwError> {
    if !ipv4(ip) {
        // assume v6; no prefix-range handling (we only hand single IPs)
        run(&format!("add element {NFT_SET_V6} {{ {ip} }}"))?;
    } else {
        run(&format!("add element {NFT_SET_V4} {{ {ip} }}"))?;
    }
    Ok(())
}

/// Remove ONE ip from the nsbc_ban set.
/// No-op if absent.
pub fn remove_ip(ip: &str) -> Result<(), FwError> {
    let r = if !ipv4(ip) {
        run(&format!("delete element {NFT_SET_V6} {{ {ip} }}"))
    } else {
        run(&format!("delete element {NFT_SET_V4} {{ {ip} }}"))
    };
    // "No such file" on delete means IP wasn't in set — silent.
    match r {
        Ok(_) => Ok(()),
        Err(FwError::NftFailed { stderr, .. }) if stderr.contains("No such file") => {
            Ok(())
        }
        Err(e) => Err(e),
    }
}

/// Fetch IPs currently in the nsbc_ban set (v4 ∪ v6).
/// Parses `nft list set ...` output (the `elements = { 1.2.3.4, 5.6.7.8 }` line);
/// empty = "No such file" silently → vec empty.
pub fn list_ips() -> Result<Vec<String>, FwError> {
    let mut all = BTreeSet::new();
    for (set, is_v4) in [(NFT_SET_V4, true), (NFT_SET_V6, false)] {
        let out = match run(&format!("list set {set}")) {
            Ok(s) => s,
            Err(FwError::NftFailed { stderr, .. }) if stderr.contains("No such file") => continue,
            Err(e) => return Err(e),
        };
        for line in out.lines() {
            // Match pattern: "    elements = { 1.2.3.4, 5.6.7.8 }" or multiline form
            let Some(rest) = line.strip_prefix("elements =") else { continue };
            let Some(rest) = rest.strip_prefix(' ') else { continue };
            let Some(rest) = rest.strip_prefix('{') else { continue };
            let Some(body) = rest.strip_suffix('}') else { continue };
            for tok in body.split(',') {
                let tok = tok.trim();
                if tok.is_empty() { continue; }
                if is_v4 && !ipv4(tok) { continue; }
                all.insert(tok.to_string());
            }
        }
    }
    Ok(all.into_iter().collect())
}

/// Full-replace the nsbc_ban set with `ips` (差集 = flush + add all).
/// This is the **only** write path used by the auto-sync poller — flushing the
/// set before adding guarantees old entries are removed (e.g. IPs whose ban just expired).
pub fn sync_all(ips: &[String]) -> Result<(usize, usize), FwError> {
    ensure_table()?;
    run(&format!("flush set {NFT_SET_V4}"))?;
    run(&format!("flush set {NFT_SET_V6}"))?;

    let mut v4 = 0usize;
    let mut v6 = 0usize;
    for ip in ips {
        if ipv4(ip) {
            run(&format!("add element {NFT_SET_V4} {{ {ip} }}"))?;
            v4 += 1;
        } else {
            run(&format!("add element {NFT_SET_V6} {{ {ip} }}"))?;
            v6 += 1;
        }
    }
    Ok((v4, v6))
}

// --------------------------------------------------------------------------
// Auto-sync background task (spawn from main.rs)
// --------------------------------------------------------------------------

/// Background loop: every `interval_s` seconds pull the core's active ban list
/// and push it into nftables. Silently skips when nft is missing or core is down —
/// those conditions are surfaced on first tick, not every tick.
pub fn spawn<C>(state: Arc<crate::AppState<C>>, interval_s: u64) -> tokio::task::JoinHandle<()>
where
    C: CoreApi + Clone + 'static,
{
    let mut interval = tokio::time::interval(Duration::from_secs(interval_s.max(5)));
    interval.set_missed_tick_behavior(MissedTickBehavior::Skip);

    tokio::spawn(async move {
        let mut warned_nft_missing = false;
        let mut warned_core_down = false;

        // ensure_table at start (fail-fast on boot misconfig).
        if let Err(e) = ensure_table() {
            warn!("fw: ensure_table failed: {e} — auto-sync disabled");
            return;
        }
        info!("fw: nsbc-auto-ban nftables sync task started, interval={interval_s}s");

        // tick immediately after boot so bans pushed by manual control.sock
        // show up in firewall within ~5s of restart.
        interval.tick().await;

        loop {
            interval.tick().await;

            // 1) pull ban list from core (remaining_s > 0 = still banned)
            let bans: Vec<String> = match state.core.security_ban_list("").await {
                Ok(list) => list
                    .into_iter()
                    .filter(|e| e.remaining_s > 0)
                    .map(|e| e.ip)
                    .collect::<Vec<_>>(),
                Err(e) => {
                    if !warned_core_down {
                        error!("fw: security.ban.list failed: {e}");
                        warned_core_down = true;
                    }
                    continue;
                }
            };
            warned_core_down = false;

            // 2) push to nftables
            match sync_all(&bans) {
                Ok((v4, v6)) => {
                    if !bans.is_empty() {
                        info!("fw: nftables synced {v4} v4 + {v6} v6 banned IPs");
                    }
                    warned_nft_missing = false;
                }
                Err(FwError::NftMissing(e)) => {
                    if !warned_nft_missing {
                        error!("fw: nft missing: {e} — auto-sync will keep retrying");
                        warned_nft_missing = true;
                    }
                }
                Err(e) => {
                    warn!("fw: sync_all failed: {e}");
                }
            }
        }
    })
}
