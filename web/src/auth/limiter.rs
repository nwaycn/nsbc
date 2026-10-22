//! 登录失败锁定 + 三接口 per-IP 限流（REQ-WEB-004/008 失败锁定、REQ-WEB-009）。
//!
//! 设计取舍（P0 最小实现，管理面低频）：
//!   - **锁定维度=用户名**：防针对单账号的口令喷洒/爆破，与来源 IP 无关；
//!     账号不存在的尝试也计数（不区分用户是否存在，REQ-WEB-018）。
//!   - **限流维度=源 IP × 桶**：固定窗口（60s），login/refresh/命令执行
//!         三桶独立阈值；超限 429 + Retry-After。
//!   - 阈值均可经环境配置，0=关闭该机制（见 [`Limits`]）。
//!   - std::Mutex 短临界区，不跨 await 持锁；计数器有界（超 1024 键惰性清过期）。
//!   - 单 IP 并发连接上限（传输层计数）与自动封禁为后续项，见 state §6#5。
//!   - 进程内状态：重启即清零（与 sessions 同口径）。

use std::collections::HashMap;
use std::sync::Mutex;

/// 限流桶：REQ-WEB-009 要求登录/刷新/命令执行三类接口分别限流。
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

    /// 按请求路径（已剥 /api/v1 前缀）选桶。
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

/// 全部防滥用阈值（fail-closed 装配，非法值拒绝启动；0=关闭对应机制）。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Limits {
    /// 连续失败多少次后锁定账号（默认 5）
    pub lock_threshold: u32,
    /// 锁定时长秒（默认 900）
    pub lock_secs: u64,
    /// 每 IP 每分钟 login 请求数（默认 10）
    pub rl_login: u32,
    /// 每 IP 每分钟 refresh 请求数（默认 30）
    pub rl_refresh: u32,
    /// 每 IP 每分钟其余 /api 请求数（默认 120）
    pub rl_api: u32,
    /// 限流窗口秒（默认 60；Retry-After 按窗口余量给）
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

/// 固定窗口 per-IP 计数器。
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

    /// 记一次请求；超限返回 Retry-After 建议秒数（≥1）。
    pub fn check(&self, b: Bucket, ip: &str, now: u64) -> Result<(), u64> {
        let limit = self.limit_for(b);
        if limit == 0 || self.cfg.window_secs == 0 {
            return Ok(());
        }
        let mut wins = self.wins.lock().unwrap();
        // 防御键无限增长（攻击/伪造 XFF 场景）：表过大时清掉本窗口已结束的键。
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

    /// 桶名（审计/日志用）。
    pub fn bucket_name(b: Bucket) -> &'static str {
        b.as_str()
    }
}

/// 失败结果：锁 crossing 当次仍返回凭证错误（由路由决定 401），后续尝试 423。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FailOutcome {
    /// 未锁定；载荷=距锁定剩余失败次数
    Remaining(u32),
    /// 本次失败已触发锁定
    JustLocked { until: u64 },
}

/// 按用户名的连续失败计数 + 锁定到期表。
pub struct Lockout {
    threshold: u32,
    secs: u64,
    /// username → (连续失败数, 锁定到期 UNIX 秒；0=未锁)
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

    /// 是否处于锁定期（顺带把过期锁复位）。
    pub fn is_locked(&self, user: &str, now: u64) -> bool {
        let mut m = self.m.lock().unwrap();
        match m.get_mut(user) {
            Some((_, until)) if *until > now => true,
            // 锁已过期：复位失败计数；未锁定（until==0）时保留计数。
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

    /// 记一次失败；threshold=0 表示关闭锁定（永远 Remaining）。
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
            *e = (0, 0); // 上一锁已过期，重新计数
        }
        e.0 += 1;
        if e.0 >= self.threshold {
            let until = now.saturating_add(self.secs);
            *e = (0, until);
            return FailOutcome::JustLocked { until };
        }
        FailOutcome::Remaining(self.threshold - e.0)
    }

    /// 登录成功：清零计数与锁。
    pub fn clear(&self, user: &str) {
        self.m.lock().unwrap().remove(user);
    }
}

/// 防滥用组合件（AuthSvc 单字段持有）。
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
        // 桶间互不影响
        assert!(g.check(Bucket::Refresh, "1.2.3.4", 100).is_ok());
        // IP 间互不影响
        assert!(g.check(Bucket::Login, "9.9.9.9", 100).is_ok());
        // 窗口滚动后恢复
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
        // 锁定期内：is_locked 真；再失败仍是锁定态
        assert!(l.is_locked("u", 3));
        assert_eq!(
            l.record_fail("u", 3),
            FailOutcome::JustLocked { until: 902 }
        );
        // 到期自动解锁并重新计数
        assert!(!l.is_locked("u", 903));
        assert_eq!(l.record_fail("u", 903), FailOutcome::Remaining(2));
        // 成功登录清零
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
