//! 服务端会话状态（REQ-WEB-008：服务端可强制失效 + JWT 防重放）。
//!
//! 三张内存表（管理面低频，std::Mutex 足够；均不跨 await 持锁）：
//!   - active：有效 refresh jti → 主体/角色/过期（刷新时必须在此表）
//!   - used：  已轮转/已登出 refresh jti 的过期墓碑（重放检测，惰性清理）
//!   - denied：已登出/封禁的 access jti → 过期（access 短时效，表有界）
//!
//! 语义：
//!   - refresh 令牌**单次使用**：consume 成功即移出 active、落入 used；
//!     再次出示同一 jti → Replay（REQ-WEB-008 防重放），由调用方告警；
//!   - access 令牌本身无状态，登出/封禁经 denied 表即时吊销，无需等 15min。
//!
//! 重启即全表失效（运维语义：重启=强制全登出）；持久化封禁清单为后续项。

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
    /// 从未登记（伪造/重启后旧令牌/已过期被清理）
    Unknown,
    /// 已被轮转或登出的 jti 再次使用 → 重放
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

    /// 登录/刷新成功后登记新的 refresh jti。
    pub fn register_refresh(&self, jti: &str, sub: String, role: u8, exp: u64) {
        self.active.lock().unwrap().insert(
            jti.to_string(),
            RefreshEntry { sub, role, exp },
        );
    }

    /// 消费 refresh（轮转）：取出即删并立墓碑；exp 已过视为 Unknown。
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

    /// 登出时注销 refresh（幂等：不存在/过期均不报错）。
    pub fn revoke_refresh(&self, jti: &str, exp: u64, now: u64) {
        self.active.lock().unwrap().remove(jti);
        if exp > now {
            self.used.lock().unwrap().insert(jti.to_string(), exp);
        }
    }

    /// 吊销 access（登出/封禁）；exp 已过则无需落表。
    pub fn deny_access(&self, jti: &str, exp: u64, now: u64) {
        if exp > now {
            self.denied.lock().unwrap().insert(jti.to_string(), exp);
        }
    }

    /// access 是否在吊销表中（顺带惰性清理过期项）。
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
        // 同一 jti 再用 → 重放
        assert_eq!(s.consume_refresh("j1", 200), Err(RefreshError::Replay));
        // 从未见过
        assert_eq!(s.consume_refresh("nope", 200), Err(RefreshError::Unknown));
    }

    #[test]
    fn expired_refresh_unknown_after_purge() {
        let s = SessionStore::new();
        s.register_refresh("old", "u0".into(), 0, 1000);
        // 过期后消费：视为无效（不立墓碑）
        assert_eq!(s.consume_refresh("old", 1000), Err(RefreshError::Unknown));
        // 表已清理，再来仍是 Unknown 而非 Replay
        assert_eq!(s.consume_refresh("old", 1001), Err(RefreshError::Unknown));
    }

    #[test]
    fn access_denylist_lifecycle() {
        let s = SessionStore::new();
        assert!(!s.access_revoked("a1", 100));
        s.deny_access("a1", 200, 100);
        assert!(s.access_revoked("a1", 100));
        // 过期后惰性消失
        assert!(!s.access_revoked("a1", 200));
        // 已过期的不入库
        s.deny_access("a2", 100, 100);
        assert!(!s.access_revoked("a2", 100));
    }

    #[test]
    fn logout_revokes_refresh_idempotently() {
        let s = SessionStore::new();
        s.register_refresh("j2", "u1".into(), 1, 1000);
        s.revoke_refresh("j2", 1000, 100);
        assert_eq!(s.consume_refresh("j2", 200), Err(RefreshError::Replay));
        s.revoke_refresh("never", 1000, 100); // 不 panic
    }
}
