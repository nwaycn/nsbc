//! Login failure lockout + three-endpoint per-IP rate limiting (REQ-WEB-004/008 failure lockout, REQ-WEB-009).
//!
//! Design tradeoffs (P0 minimal implementation, management-plane low frequency):
//!   - **Lockout keyed by username**: prevents credential spraying/brute force against single accounts, independent of source IP;
//!     attempts against non-existent accounts are also counted (no user-existence distinction, REQ-WEB-018).
//!   - **Rate-limit keyed by source IP x bucket**: fixed window (60s), login/refresh/command execution
//!         three independent thresholds per bucket; exceeding returns 429 + Retry-After.
//!   - All thresholds configurable via env, 0=disables the respective mechanism (see [`Limits`]).
//!   - std::Mutex short critical sections, no lock held across await; rate-limit entries are capped at 65,536,
//!     with expired entries reclaimed before new keys are rejected.
//!   - Per-IP concurrent connection cap (transport-layer counting) and auto-ban are future items, see state §6#5.
//!   - In-process state: cleared on restart (consistent with sessions).

use std::collections::HashMap;
use std::sync::Mutex;

const MAX_RATE_LIMIT_KEYS: usize = 65_536;
const MAX_LOCKOUT_USERS: usize = 65_536;

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
    /// Idle window for accumulating failures in seconds (default 900)
    pub lock_window_secs: u64,
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
            lock_window_secs: 900,
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

/// Fixed-window per-IP counter with a hard memory bound.
pub struct RateLimiter {
    cfg: Limits,
    wins: Mutex<HashMap<(u8, String), Window>>,
    max_keys: usize,
}

impl RateLimiter {
    pub fn new(cfg: Limits) -> RateLimiter {
        Self::with_key_limit(cfg, MAX_RATE_LIMIT_KEYS)
    }

    fn with_key_limit(cfg: Limits, max_keys: usize) -> RateLimiter {
        RateLimiter {
            cfg,
            wins: Mutex::new(HashMap::new()),
            max_keys,
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
        let key = (b.key(), ip.to_string());
        if let Some(w) = wins.get_mut(&key) {
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
            return Ok(());
        }

        if wins.len() >= self.max_keys {
            wins.retain(|_, w| now.saturating_sub(w.start) < self.cfg.window_secs);
        }
        if wins.len() >= self.max_keys {
            let retry = wins
                .values()
                .map(|w| {
                    self.cfg
                        .window_secs
                        .saturating_sub(now.saturating_sub(w.start))
                        .max(1)
                })
                .min()
                .unwrap_or(1);
            return Err(retry);
        }

        wins.insert(
            key,
            Window {
                start: now,
                count: 1,
            },
        );
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
    /// The bounded lockout table is full after expired entries were purged.
    Capacity,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct LockoutEntry {
    failures: u32,
    locked_until: u64,
    last_failure: u64,
}

/// Per-username failure counter with idle expiry and a hard memory bound.
pub struct Lockout {
    threshold: u32,
    secs: u64,
    window_secs: u64,
    max_users: usize,
    m: Mutex<HashMap<String, LockoutEntry>>,
}

impl Lockout {
    pub fn new(threshold: u32, secs: u64) -> Lockout {
        Self::with_limits(threshold, secs, secs, MAX_LOCKOUT_USERS)
    }

    fn with_window(threshold: u32, secs: u64, window_secs: u64) -> Lockout {
        Self::with_limits(threshold, secs, window_secs, MAX_LOCKOUT_USERS)
    }

    fn with_limits(threshold: u32, secs: u64, window_secs: u64, max_users: usize) -> Lockout {
        Lockout {
            threshold,
            secs,
            window_secs: window_secs.max(1),
            max_users,
            m: Mutex::new(HashMap::new()),
        }
    }

    /// Whether currently in lockout period; expired locks and idle counters are removed.
    pub fn is_locked(&self, user: &str, now: u64) -> bool {
        let mut m = self.m.lock().unwrap();
        match m.get(user).copied() {
            Some(entry) if entry.locked_until > now => true,
            Some(entry)
                if entry.locked_until != 0
                    || entry.failures == 0
                    || now.saturating_sub(entry.last_failure) >= self.window_secs =>
            {
                m.remove(user);
                false
            }
            _ => false,
        }
    }

    /// Record a failure; threshold=0 means lockout disabled (always Remaining).
    pub fn record_fail(&self, user: &str, now: u64) -> FailOutcome {
        if self.threshold == 0 {
            return FailOutcome::Remaining(u32::MAX);
        }
        let mut m = self.m.lock().unwrap();
        let existing = m.get(user).copied();
        if let Some(entry) = existing {
            if entry.locked_until > now {
                return FailOutcome::JustLocked {
                    until: entry.locked_until,
                };
            }
        }

        let mut entry = existing.unwrap_or_default();
        if entry.locked_until != 0
            || (entry.failures > 0 && now.saturating_sub(entry.last_failure) >= self.window_secs)
        {
            entry = LockoutEntry::default();
        }

        if existing.is_none() {
            if m.len() >= self.max_users {
                m.retain(|_, value| {
                    if value.locked_until != 0 {
                        value.locked_until > now
                    } else {
                        value.failures > 0
                            && now.saturating_sub(value.last_failure) < self.window_secs
                    }
                });
            }
            if m.len() >= self.max_users {
                return FailOutcome::Capacity;
            }
        }

        entry.failures = entry.failures.saturating_add(1);
        entry.last_failure = now;
        if entry.failures >= self.threshold {
            let until = now.saturating_add(self.secs);
            entry.failures = 0;
            entry.locked_until = until;
            m.insert(user.to_string(), entry);
            return FailOutcome::JustLocked { until };
        }
        let remaining = self.threshold - entry.failures;
        m.insert(user.to_string(), entry);
        FailOutcome::Remaining(remaining)
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
            lock: Lockout::with_window(cfg.lock_threshold, cfg.lock_secs, cfg.lock_window_secs),
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
    fn rate_limiter_capacity_rejects_new_keys_until_window_expires() {
        let g = RateLimiter::with_key_limit(
            Limits {
                rl_login: 10,
                window_secs: 60,
                ..Limits::default()
            },
            1,
        );
        assert!(g.check(Bucket::Login, "1.2.3.4", 100).is_ok());
        assert_eq!(g.check(Bucket::Login, "9.9.9.9", 100), Err(60));
        assert!(g.check(Bucket::Login, "9.9.9.9", 160).is_ok());
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
    fn lockout_failure_window_expires_but_active_lock_is_preserved() {
        let l = Lockout::with_limits(3, 90, 10, 4);
        assert_eq!(l.record_fail("u", 100), FailOutcome::Remaining(2));
        assert_eq!(l.record_fail("u", 109), FailOutcome::Remaining(1));
        assert_eq!(l.record_fail("u", 119), FailOutcome::Remaining(2));
        assert_eq!(l.record_fail("u", 120), FailOutcome::Remaining(1));
        assert_eq!(
            l.record_fail("u", 121),
            FailOutcome::JustLocked { until: 211 }
        );
        assert!(l.is_locked("u", 210));
        assert!(!l.is_locked("u", 211));
    }

    #[test]
    fn lockout_capacity_reclaims_idle_counters_and_rejects_when_full() {
        let l = Lockout::with_limits(3, 90, 10, 1);
        assert_eq!(l.record_fail("first", 100), FailOutcome::Remaining(2));
        assert_eq!(l.record_fail("second", 101), FailOutcome::Capacity);
        assert_eq!(l.record_fail("second", 110), FailOutcome::Remaining(2));
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
