//! Server-side session state (REQ-WEB-008: server can force invalidation + JWT anti-replay).
//!
//! Three in-memory tables (management-plane low frequency, std::Mutex sufficient; no lock held across await):
//!   - active: valid refresh jti -> subject/role/expiry (must be in this table to refresh)
//!   - used:   expired tombstones for rotated/logged-out refresh jti (replay detection, lazy cleanup)
//!   - denied: access jti that have been logged out/banned -> expiry (access is short-lived, table is bounded)
//!
//! Semantics:
//!   - Refresh tokens are **single-use**: consume success removes from active, lands in used;
//!     presenting the same jti again -> Replay (REQ-WEB-008 anti-replay), caller alerts;
//!   - Access tokens themselves are stateless; logout/ban revokes instantly via denied table, no need to wait 15min.
//!
//! All tables invalidated on restart (ops semantics: restart = force logout all); persistent ban list is a future item.

use std::collections::HashMap;
use std::sync::Mutex;

const MAX_ACTIVE_REFRESH: usize = 100_000;
const MAX_USED_REFRESH: usize = 200_000;
const MAX_DENIED_ACCESS: usize = 200_000;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RefreshEntry {
    pub sub: String,
    pub role: u8,
    pub exp: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RefreshError {
    /// Never registered (forged / old token after restart / expired and cleaned up)
    Unknown,
    /// Previously rotated or logged-out jti used again -> replay
    Replay,
    /// Bounded replay-protection table is full; token remains active.
    Capacity,
}

pub struct SessionStore {
    active: Mutex<HashMap<String, RefreshEntry>>,
    used: Mutex<HashMap<String, u64>>,
    denied: Mutex<HashMap<String, u64>>,
    active_limit: usize,
    used_limit: usize,
    denied_limit: usize,
}

impl SessionStore {
    pub fn new() -> SessionStore {
        Self::with_limits(MAX_ACTIVE_REFRESH, MAX_USED_REFRESH, MAX_DENIED_ACCESS)
    }

    /// Register a new refresh jti after successful login/refresh.
    /// Returns false when the bounded store is full after expired entries are purged.
    pub fn register_refresh(&self, jti: &str, sub: String, role: u8, exp: u64, now: u64) -> bool {
        let mut active = self.active.lock().unwrap();
        if active.len() >= self.active_limit {
            active.retain(|_, entry| entry.exp > now);
        }
        if active.len() >= self.active_limit {
            return false;
        }
        active.insert(jti.to_string(), RefreshEntry { sub, role, exp });
        true
    }

    /// Consume refresh (rotation): remove immediately and create tombstone; if exp passed, treat as Unknown.
    pub fn consume_refresh(&self, jti: &str, now: u64) -> Result<RefreshEntry, RefreshError> {
        let mut active = self.active.lock().unwrap();
        let Some(entry) = active.remove(jti) else {
            drop(active);
            let mut used = self.used.lock().unwrap();
            return if used.get(jti).is_some_and(|exp| *exp > now) {
                Err(RefreshError::Replay)
            } else {
                used.remove(jti);
                Err(RefreshError::Unknown)
            };
        };
        if entry.exp <= now {
            return Err(RefreshError::Unknown);
        }
        let mut used = self.used.lock().unwrap();
        if used.len() >= self.used_limit {
            used.retain(|_, exp| *exp > now);
        }
        if used.len() >= self.used_limit {
            active.insert(jti.to_string(), entry);
            return Err(RefreshError::Capacity);
        }
        used.insert(jti.to_string(), entry.exp);
        Ok(entry)
    }

    /// Deregister refresh on logout (idempotent: no error when not found / expired).
    /// Returns false only when an active token could not be recorded as a replay tombstone.
    pub fn revoke_refresh(&self, jti: &str, exp: u64, now: u64) -> bool {
        let mut active = self.active.lock().unwrap();
        let Some(entry) = active.remove(jti) else {
            return true;
        };
        let expiry = exp.min(entry.exp);
        if expiry <= now {
            return true;
        }
        let mut used = self.used.lock().unwrap();
        if used.len() >= self.used_limit {
            used.retain(|_, value| *value > now);
        }
        if used.len() >= self.used_limit {
            active.insert(jti.to_string(), entry);
            return false;
        }
        used.insert(jti.to_string(), expiry);
        true
    }

    /// W-07 修复：撤销某 subject 的所有活跃 refresh session。
    /// 密码修改、账号禁用、角色降级时调用，确保旧 refresh token 不能续期。
    /// 返回被撤销的数量。
    pub fn revoke_all_for_subject(&self, sub: &str, now: u64) -> usize {
        let mut active = self.active.lock().unwrap();
        let mut used = self.used.lock().unwrap();
        let mut revoked = 0usize;
        let mut to_move: Vec<(String, u64)> = Vec::new();
        for (jti, entry) in active.iter() {
            if entry.sub == sub && entry.exp > now {
                to_move.push((jti.clone(), entry.exp));
            }
        }
        for (jti, exp) in to_move {
            active.remove(&jti);
            if used.len() < self.used_limit {
                used.insert(jti, exp);
            }
            revoked += 1;
        }
        revoked
    }

    /// Revoke access (logout/ban); if exp already passed, no need to add to table.
    /// Returns false when the bounded store is full after expired entries are purged.
    pub fn deny_access(&self, jti: &str, exp: u64, now: u64) -> bool {
        if exp > now {
            let mut denied = self.denied.lock().unwrap();
            if denied.get(jti).is_some_and(|value| *value > now) {
                return true;
            }
            if denied.len() >= self.denied_limit {
                denied.retain(|_, value| *value > now);
            }
            if denied.len() >= self.denied_limit {
                return false;
            }
            denied.insert(jti.to_string(), exp);
        }
        true
    }

    /// Whether access is in the revocation table (also lazily cleans expired entries).
    pub fn access_revoked(&self, jti: &str, now: u64) -> bool {
        let mut denied = self.denied.lock().unwrap();
        match denied.get(jti) {
            Some(exp) if *exp > now => true,
            Some(_) => {
                denied.remove(jti);
                false
            }
            None => false,
        }
    }

    fn with_limits(active: usize, used: usize, denied: usize) -> SessionStore {
        SessionStore {
            active: Mutex::new(HashMap::with_capacity(active.min(1024))),
            used: Mutex::new(HashMap::with_capacity(used.min(1024))),
            denied: Mutex::new(HashMap::with_capacity(denied.min(1024))),
            active_limit: active,
            used_limit: used,
            denied_limit: denied,
        }
    }

    /// RW-H5 hardening: background purge. Drop all entries whose exp <= now
    /// from all three tables. Called by a 60s tokio interval task in AuthSvc.
    /// Returns (active_remaining, used_remaining, denied_remaining) snapshot.
    pub fn purge_expired(&self, now: u64) -> (usize, usize, usize) {
        let mut active = self.active.lock().unwrap();
        active.retain(|_, e| e.exp > now);
        let mut used = self.used.lock().unwrap();
        used.retain(|_, exp| *exp > now);
        let mut denied = self.denied.lock().unwrap();
        denied.retain(|_, exp| *exp > now);
        // shrink_to 避免长时间运行后 bucket 过度膨胀
        active.shrink_to(self.active_limit / 2);
        used.shrink_to(self.used_limit / 2);
        denied.shrink_to(self.denied_limit / 2);
        let a = active.len();
        let u = used.len();
        let d = denied.len();
        (a, u, d)
    }
}

impl Default for SessionStore {
    fn default() -> Self {
        SessionStore::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn refresh_rotate_is_single_use() {
        let s = SessionStore::new();
        assert!(s.register_refresh("j1", "u0".into(), 0, 1000, 100));
        let e = s.consume_refresh("j1", 100).unwrap();
        assert_eq!(e.sub, "u0");
        // Same jti reused -> replay
        assert_eq!(s.consume_refresh("j1", 200), Err(RefreshError::Replay));
        // Never seen before
        assert_eq!(s.consume_refresh("nope", 200), Err(RefreshError::Unknown));
    }

    #[test]
    fn expired_refresh_unknown_after_purge() {
        let s = SessionStore::new();
        assert!(s.register_refresh("old", "u0".into(), 0, 1000, 100));
        // Consumed after expiry: treat as invalid (no tombstone created)
        assert_eq!(s.consume_refresh("old", 1000), Err(RefreshError::Unknown));
        // Table cleaned; second attempt is still Unknown, not Replay
        assert_eq!(s.consume_refresh("old", 1001), Err(RefreshError::Unknown));
    }

    #[test]
    fn access_denylist_lifecycle() {
        let s = SessionStore::new();
        assert!(!s.access_revoked("a1", 100));
        assert!(s.deny_access("a1", 200, 100));
        assert!(s.access_revoked("a1", 100));
        // Expired entries disappear lazily
        assert!(!s.access_revoked("a1", 200));
        // Expired entries do not enter the table
        assert!(s.deny_access("a2", 100, 100));
        assert!(!s.access_revoked("a2", 100));
    }

    #[test]
    fn logout_revokes_refresh_idempotently() {
        let s = SessionStore::new();
        assert!(s.register_refresh("j2", "u1".into(), 1, 1000, 100));
        assert!(s.revoke_refresh("j2", 1000, 100));
        assert_eq!(s.consume_refresh("j2", 200), Err(RefreshError::Replay));
        assert!(s.revoke_refresh("never", 1000, 100));
    }

    #[test]
    fn session_tables_are_bounded_and_fail_closed() {
        let s = SessionStore::with_limits(1, 1, 1);
        assert!(s.register_refresh("j1", "u1".into(), 1, 1000, 100));
        assert!(!s.register_refresh("j2", "u2".into(), 1, 1000, 100));
        assert_eq!(s.consume_refresh("j1", 200).unwrap().sub, "u1");
        assert!(s.register_refresh("j2", "u2".into(), 1, 1000, 200));
        assert_eq!(s.consume_refresh("j2", 300), Err(RefreshError::Capacity));
        assert_eq!(
            s.consume_refresh("j2", 300).unwrap_err(),
            RefreshError::Capacity
        );

        assert!(s.deny_access("a1", 400, 100));
        assert!(s.deny_access("a1", 400, 100));
        assert!(!s.deny_access("a2", 500, 100));
        assert!(!s.access_revoked("a1", 400));
        assert!(s.deny_access("a2", 500, 400));
    }

    #[test]
    fn access_revocation_lookup_only_removes_the_requested_expired_entry() {
        let s = SessionStore::with_limits(1, 1, 2);
        assert!(s.deny_access("expired", 100, 10));
        assert!(s.deny_access("active", 200, 10));
        assert!(!s.access_revoked("expired", 100));
        assert!(s.access_revoked("active", 100));
    }
}
