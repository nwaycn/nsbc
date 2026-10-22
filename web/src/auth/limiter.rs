//! Login failure lockout + three-endpoint per-IP rate limiting (REQ-WEB-004/008 failure lockout, REQ-WEB-009).
//!
//! Design tradeoffs (P0 minimal implementation, management-plane low frequency):
//!   - **Lockout keyed by username**: prevents credential spraying/brute force against single accounts, independent of source IP;
//!     attempts against non-existent accounts are also counted (no user-existence distinction, REQ-WEB-018).
//!   - **Rate-limit keyed by source IP x bucket**: fixed window (60s), login/refresh/command execution
//!         three independent thresholds per bucket; exceeding returns 429 + Retry-After.
//!   - All thresholds configurable via env, 0=disables the respective mechanism (see [`Limits`]).
//!   - std::Mutex short critical sections, no lock held across await; counter bounded (lazy-expire when over 1024 keys).
//!   - Per-IP concurrent connection cap (transport-layer counting) and auto-ban are future items, see state §6#5.
//!   - In-process state: cleared on restart (consistent with sessions).

use std::collections::HashMap;
use std::sync::Mutex;

/// Rate-limit buckets: REQ-WEB-009 requires login/refresh/command-exec endpoints to be limited separately.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Bucket {
    Login,
    Refresh,
    Api,
}

impl Bucket {
    fn key(self) -> u8 {
        match self {
            Bucket::Login => 0,
            Bucket::Refresh => 1,
            Bucket::Api => 2,
        }
    }

    /// Select bucket by request path (already stripped of /api/v1 prefix).
    pub fn for_path(path: &str) -> Bucket {
        match path {
            "/auth/login" => Bucket::Login,
            "/auth/refresh" => Bucket::Refresh,
            _ => Bucket::Api,
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Bucket::Login => "login",
            Bucket::Refresh => "refresh",
            Bucket::Api => "api",
        }
    }
}

/// All anti-abuse thresholds (fail-closed config, refuse startup on invalid values; 0=disables the respective mechanism).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Limits {
    /// Number of consecutive failures before locking the account (default 5)
    pub lock_threshold: u32,
    /// Lockout duration in seconds (default 900)
    pub lock_secs: u64,
    /// login requests per IP per minute (default 10)
    pub rl_login: u32,
    /// refresh requests per IP per minute (default 30)
    pub rl_refresh: u32,
    /// remaining /api requests per IP per minute (default 120)
    pub rl_api: u32,
    /// Rate-limit window seconds (default 60; Retry-After given per window remainder)
    pub window_secs: u64,
}

impl Default for Limits {
    fn default() -> Self {
        Limits {
            lock_threshold: 5,
            lock_secs: 900,
            rl_login: 10,
            rl_refresh: 30,
            rl_api: 120,
            window_secs: 60,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Window {
    start: u64,
    count: u32,
}

/// Fixed-window per-IP counter.
pub struct RateLimiter {
    cfg: Limits,
    wins: Mutex<HashMap<(u8, String), Window>>,
}

impl RateLimiter {
    pub fn new(cfg: Limits) -> RateLimiter {
        RateLimiter {
            cfg,
            wins: Mutex::new(HashMap::new()),
        }
    }

    fn limit_for(&self, b: Bucket) -> u32 {
        match b {
            Bucket::Login => self.cfg.rl_login,
            Bucket::Refresh => self.cfg.rl_refresh,
            Bucket::Api => self.cfg.rl_api,
        }
    }

    /// Record a request; if exceeded, returns suggested Retry-After seconds (>=1).
    pub fn check(&self, b: Bucket, ip: &str, now: u64) -> Result<(), u64> {
        let limit = self.limit_for(b);
        if limit == 0 || self.cfg.window_secs == 0 {
            return Ok(());
        }
        let mut wins = self.wins.lock().unwrap();
        // Prevent unbounded key growth (attack/spoofed XFF scenario): when table too large, purge keys that have ended in this window.
        if wins.len() > 1024 {
            wins.retain(|_, w| now.saturating_sub(w.start) < self.cfg.window_secs);
        }
        let w = wins.entry((b.key(), ip.to_string())).or_insert(Window {
            start: now,
            count: 0,
        });
        if now.saturating_sub(w.start) >= self.cfg.window_secs {
            w.start = now;
            w.count = 0;
        }
        if w.count >= limit {
            let retry = w
                .start
                .saturating_add(self.cfg.window_secs)
                .saturating_sub(now)
                .max(1);
            return Err(retry);
        }
        w.count += 1;
        Ok(())
    }

    /// Bucket name (used in audit/logging).
    pub fn bucket_name(b: Bucket) -> &'static str {
        b.as_str()
    }
}

/// Failure result: lock-crossing attempt still returns bad credentials (routing layer decides 401); subsequent attempts get 423.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FailOutcome {
    /// Not locked; payload = remaining failure count before lock
    Remaining(u32),
    /// This failure triggered the lock
    JustLocked { until: u64 },
}

/// Per-username consecutive failure count + lock-expiration table.
pub struct Lockout {
    threshold: u32,
    secs: u64,
    /// username -> (failure count, lock-expire UNIX seconds; 0=not locked)
    m: Mutex<HashMap<String, (u32, u64)>>,
}

impl Lockout {
    pub fn new(threshold: u32, secs: u64) -> Lockout {
        Lockout {
            threshold,
            secs,
            m: Mutex::new(HashMap::new()),
        }
    }

    /// Whether currently in lockout period (also resets expired locks).
    pub fn is_locked(&self, user: &str, now: u64) -> bool {
        let mut m = self.m.lock().unwrap();
        match m.get_mut(user) {
            Some((_, until)) if *until > now => true,
            // Lock expired: reset failure count; if not locked (until==0), keep the count.
            Some((fails, until)) => {
                if *until != 0 {
                    *fails = 0;
                    *until = 0;
                }
                false
            }
            None => false,
        }
    }

    /// Record a failure; threshold=0 means lockout disabled (always Remaining).
    pub fn record_fail(&self, user: &str, now: u64) -> FailOutcome {
        if self.threshold == 0 {
            return FailOutcome::Remaining(u32::MAX);
        }
        let mut m = self.m.lock().unwrap();
        let e = m.entry(user.to_string()).or_insert((0, 0));
        if e.1 > now {
            return FailOutcome::JustLocked { until: e.1 };
        }
        if e.1 != 0 {
*e = (0, 0); // previous lock expired, recount
        }
        e.0 += 1;
        if e.0 >= self.threshold {
            let until = now.saturating_add(self.secs);
            *e = (0, until);
            return FailOutcome::JustLocked { until };
        }
        FailOutcome::Remaining(self.threshold - e.0)
    }

    /// Login success: clear count and lock.
    pub fn clear(&self, user: &str) {
        self.m.lock().unwrap().remove(user);
    }
}

/// Anti-abuse composite (held in a single AuthSvc field).
pub struct AbuseGuard {
    pub rate: RateLimiter,
    pub lock: Lockout,
}

impl AbuseGuard {
    pub fn new(cfg: Limits) -> AbuseGuard {
        AbuseGuard {
            rate: RateLimiter::new(cfg),
            lock: Lockout::new(cfg.lock_threshold, cfg.lock_secs),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rate_window_allows_then_blocks_and_resets() {
        let g = RateLimiter::new(Limits {
            rl_login: 3,
            window_secs: 60,
            ..Limits::default()
        });
        for _ in 0..3 {
            assert!(g.check(Bucket::Login, "1.2.3.4", 100).is_ok());
        }
        let retry = g.check(Bucket::Login, "1.2.3.4", 100).unwrap_err();
        assert!((1..=60).contains(&retry));
        // Buckets independent from each other
        assert!(g.check(Bucket::Refresh, "1.2.3.4", 100).is_ok());
        // IPs independent from each other
        assert!(g.check(Bucket::Login, "9.9.9.9", 100).is_ok());
        // Recovers after window rolls
        assert!(g.check(Bucket::Login, "1.2.3.4", 160).is_ok());
    }

    #[test]
    fn bucket_path_selection() {
        assert_eq!(Bucket::for_path("/auth/login"), Bucket::Login);
        assert_eq!(Bucket::for_path("/auth/refresh"), Bucket::Refresh);
        assert_eq!(Bucket::for_path("/calls/c1/hangup"), Bucket::Api);
        assert_eq!(Bucket::for_path("/metrics"), Bucket::Api);
    }

    #[test]
    fn lockout_threshold_unlock_and_clear() {
        let l = Lockout::new(3, 900);
        assert_eq!(l.record_fail("u", 0), FailOutcome::Remaining(2));
        assert_eq!(l.record_fail("u", 1), FailOutcome::Remaining(1));
        assert!(!l.is_locked("u", 2));
        assert_eq!(
            l.record_fail("u", 2),
            FailOutcome::JustLocked { until: 902 }
        );
        // During lockout: is_locked true; further failures remain in locked state
        assert!(l.is_locked("u", 3));
        assert_eq!(
            l.record_fail("u", 3),
            FailOutcome::JustLocked { until: 902 }
        );
        // Auto-unlocks on expiry and recounts
        assert!(!l.is_locked("u", 903));
        assert_eq!(l.record_fail("u", 903), FailOutcome::Remaining(2));
        // Successful login clears
        l.clear("u");
        assert_eq!(l.record_fail("u", 904), FailOutcome::Remaining(2));
    }

    #[test]
    fn zero_threshold_means_disabled() {
        let l = Lockout::new(0, 900);
        for _ in 0..100 {
            assert_eq!(l.record_fail("u", 0), FailOutcome::Remaining(u32::MAX));
        }
        assert!(!l.is_locked("u", 0));
        let g = RateLimiter::new(Limits {
            rl_login: 0,
            window_secs: 60,
            ..Limits::default()
        });
        for _ in 0..1000 {
            assert!(g.check(Bucket::Login, "ip", 0).is_ok());
        }
    }
}
