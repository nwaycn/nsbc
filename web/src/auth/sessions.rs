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
}

#[derive(Default)]
pub struct SessionStore {
    active: Mutex<HashMap<String, RefreshEntry>>,
    used: Mutex<HashMap<String, u64>>,
    denied: Mutex<HashMap<String, u64>>,
}

impl SessionStore {
    pub fn new() -> SessionStore {
        SessionStore::default()
    }

    /// Register a new refresh jti after successful login/refresh.
    pub fn register_refresh(&self, jti: &str, sub: String, role: u8, exp: u64) {
        self.active.lock().unwrap().insert(
            jti.to_string(),
            RefreshEntry { sub, role, exp },
        );
    }

    /// Consume refresh (rotation): remove immediately and create tombstone; if exp passed, treat as Unknown.
    pub fn consume_refresh(
        &self,
        jti: &str,
        now: u64,
    ) -> Result<RefreshEntry, RefreshError> {
        let mut active = self.active.lock().unwrap();
        active.retain(|_, e| e.exp > now);
        let Some(entry) = active.remove(jti) else {
            drop(active);
            let mut used = self.used.lock().unwrap();
            used.retain(|_, exp| *exp > now);
            return if used.contains_key(jti) {
                Err(RefreshError::Replay)
            } else {
                Err(RefreshError::Unknown)
            };
        };
        if entry.exp <= now {
            return Err(RefreshError::Unknown);
        }
        self.used.lock().unwrap().insert(jti.to_string(), entry.exp);
        Ok(entry)
    }

    /// Deregister refresh on logout (idempotent: no error when not found / expired).
    pub fn revoke_refresh(&self, jti: &str, exp: u64, now: u64) {
        self.active.lock().unwrap().remove(jti);
        if exp > now {
            self.used.lock().unwrap().insert(jti.to_string(), exp);
        }
    }

    /// Revoke access (logout/ban); if exp already passed, no need to add to table.
    pub fn deny_access(&self, jti: &str, exp: u64, now: u64) {
        if exp > now {
            self.denied.lock().unwrap().insert(jti.to_string(), exp);
        }
    }

    /// Whether access is in the revocation table (also lazily cleans expired entries).
    pub fn access_revoked(&self, jti: &str, now: u64) -> bool {
        let mut denied = self.denied.lock().unwrap();
        denied.retain(|_, exp| *exp > now);
        denied.contains_key(jti)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn refresh_rotate_is_single_use() {
        let s = SessionStore::new();
        s.register_refresh("j1", "u0".into(), 0, 1000);
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
        s.register_refresh("old", "u0".into(), 0, 1000);
        // Consumed after expiry: treat as invalid (no tombstone created)
        assert_eq!(s.consume_refresh("old", 1000), Err(RefreshError::Unknown));
        // Table cleaned; second attempt is still Unknown, not Replay
        assert_eq!(s.consume_refresh("old", 1001), Err(RefreshError::Unknown));
    }

    #[test]
    fn access_denylist_lifecycle() {
        let s = SessionStore::new();
        assert!(!s.access_revoked("a1", 100));
        s.deny_access("a1", 200, 100);
        assert!(s.access_revoked("a1", 100));
        // Expired entries disappear lazily
        assert!(!s.access_revoked("a1", 200));
        // Expired entries do not enter the table
        s.deny_access("a2", 100, 100);
        assert!(!s.access_revoked("a2", 100));
    }

    #[test]
    fn logout_revokes_refresh_idempotently() {
        let s = SessionStore::new();
        s.register_refresh("j2", "u1".into(), 1, 1000);
        s.revoke_refresh("j2", 1000, 100);
        assert_eq!(s.consume_refresh("j2", 200), Err(RefreshError::Replay));
        s.revoke_refresh("never", 1000, 100); // must not panic
    }
}
