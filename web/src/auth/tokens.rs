//! 紧凑 JWT（HS256）签发/校验（REQ-WEB-008 P0，W3a 自实现，无第三方 JWT 库）。
//!
//! 线格式：`b64url(header).b64url(claims).b64url(HMAC-SHA256("header.claims"))`。
//! header 固定为 `{"alg":"HS256","typ":"JWT"}`：
//!   - 不接受 alg 协商，拒绝 alg=none / 算法混淆；
//!   - HMAC 经 ring 常量时间比较；
//!   - 时钟以参数注入（now=UNIX 秒），过期判定可单测；
//!   - jti 为 128bit 随机数（getrandom），用于吊销表/刷新轮转/防重放。
//!
//! 仅承载会话必需声明：sub（账号名）、role（0/1/2）、iat/exp、jti、typ。
//! 不承载任何敏感信息（令牌可能出现在网关日志，REQ-WEB-018）。

use std::time::Duration;

use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use ring::hmac;
use serde::{Deserialize, Serialize};

/// 固定且唯一的 JWT header（序列化字节稳定，校验时逐字节比对）。
const HEADER_JSON: &[u8] = br#"{"alg":"HS256","typ":"JWT"}"#;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TokenKind {
    /// 访问令牌（短时效，随每次业务请求出示）
    Access,
    /// 刷新令牌（长时效，仅 /auth/refresh 出示，服务端有状态登记）
    Refresh,
}

impl TokenKind {
    fn tag(self) -> &'static str {
        match self {
            TokenKind::Access => "a",
            TokenKind::Refresh => "r",
        }
    }
}

/// JWT 载荷（access/refresh 同结构，以 typ 区分）。
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Claims {
    pub sub: String,
    pub role: u8,
    pub iat: u64,
    pub exp: u64,
    pub jti: String,
    pub typ: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TokenError {
    /// 结构/编码/JSON 非法
    Malformed,
    /// 签名不匹配（含 alg 混淆、密钥不符）
    BadSignature,
    /// 已过期
    Expired,
    /// 令牌类型与接口不符（如拿 refresh 访问业务接口）
    WrongKind,
}

/// 当前 UNIX 秒（单调时钟不适用墙上时刻；JWT exp 必须为墙上时间）。
pub fn now_secs() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

pub struct TokenSvc {
    key: hmac::Key,
    access_ttl: Duration,
    refresh_ttl: Duration,
}

impl TokenSvc {
    /// 密钥要求 ≥32 字节（REQ-WEB-008 会话密钥强度；fail-closed）。
    pub fn new(
        secret: &[u8],
        access_ttl: Duration,
        refresh_ttl: Duration,
    ) -> Result<TokenSvc, String> {
        if secret.len() < 32 {
            return Err(format!(
                "JWT secret must be at least 32 bytes, got {}",
                secret.len()
            ));
        }
        if access_ttl.as_secs() == 0 || refresh_ttl.as_secs() == 0 {
            return Err("token TTLs must be non-zero".into());
        }
        Ok(TokenSvc {
            key: hmac::Key::new(hmac::HMAC_SHA256, secret),
            access_ttl,
            refresh_ttl,
        })
    }

    pub fn access_ttl_secs(&self) -> u64 {
        self.access_ttl.as_secs()
    }

    /// 签发一对令牌；返回 (access, refresh) 各自的（字符串, 载荷），
    /// 调用方负责把 refresh 的 jti 登记进服务端会话表。
    pub fn issue_pair(
        &self,
        sub: &str,
        role: u8,
        now: u64,
    ) -> ((String, Claims), (String, Claims)) {
        (
            self.issue(sub, role, TokenKind::Access, now),
            self.issue(sub, role, TokenKind::Refresh, now),
        )
    }

    fn issue(&self, sub: &str, role: u8, kind: TokenKind, now: u64) -> (String, Claims) {
        let ttl = match kind {
            TokenKind::Access => self.access_ttl,
            TokenKind::Refresh => self.refresh_ttl,
        };
        let claims = Claims {
            sub: sub.to_string(),
            role,
            iat: now,
            exp: now.saturating_add(ttl.as_secs()),
            jti: new_jti(),
            typ: kind.tag().to_string(),
        };
        let head = URL_SAFE_NO_PAD.encode(HEADER_JSON);
        let body = URL_SAFE_NO_PAD.encode(serde_json::to_vec(&claims).expect("claims serialize"));
        let input = format!("{head}.{body}");
        let sig = URL_SAFE_NO_PAD.encode(hmac::sign(&self.key, input.as_bytes()).as_ref());
        (format!("{input}.{sig}"), claims)
    }

    /// 校验：编码结构 → header 逐字节 → HMAC（常量时间）→ 声明 → 类型 → 过期。
    pub fn verify(
        &self,
        token: &str,
        expect: TokenKind,
        now: u64,
    ) -> Result<Claims, TokenError> {
        let parts: Vec<&str> = token.split('.').collect();
        if parts.len() != 3 {
            return Err(TokenError::Malformed);
        }
        let (head, body, sig) = (parts[0], parts[1], parts[2]);
        let head_raw = URL_SAFE_NO_PAD
            .decode(head)
            .map_err(|_| TokenError::Malformed)?;
        if head_raw != HEADER_JSON {
            // alg 混淆 / alg=none / 非本服务签发的 header
            return Err(TokenError::BadSignature);
        }
        let sig_raw = URL_SAFE_NO_PAD
            .decode(sig)
            .map_err(|_| TokenError::Malformed)?;
        let input = format!("{head}.{body}");
        hmac::verify(&self.key, input.as_bytes(), &sig_raw)
            .map_err(|_| TokenError::BadSignature)?;
        let body_raw = URL_SAFE_NO_PAD
            .decode(body)
            .map_err(|_| TokenError::Malformed)?;
        let claims: Claims =
            serde_json::from_slice(&body_raw).map_err(|_| TokenError::Malformed)?;
        if claims.typ != expect.tag() {
            return Err(TokenError::WrongKind);
        }
        if now >= claims.exp {
            return Err(TokenError::Expired);
        }
        Ok(claims)
    }
}

/// 128bit 随机 jti → 小写十六进制（32 字符）。
fn new_jti() -> String {
    let mut raw = [0u8; 16];
    // 进程无法继续时（OS RNG 故障）宁可签发失败也不降级：expect 由调用层映射 5xx。
    getrandom::getrandom(&mut raw).expect("OS RNG must be available");
    let mut s = String::with_capacity(32);
    for b in raw {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    const SECRET: &[u8] = b"unit-test-secret-0123456789abcdef-32B!";

    fn svc() -> TokenSvc {
        TokenSvc::new(SECRET, Duration::from_secs(900), Duration::from_secs(28800)).unwrap()
    }

    #[test]
    fn issue_and_verify_roundtrip() {
        let t = svc();
        let ((at, ac), (rt, rc)) = t.issue_pair("u0", 0, 1000);
        assert_eq!(ac.typ, "a");
        assert_eq!(rc.typ, "r");
        assert_eq!(ac.jti.len(), 32);
        assert_ne!(ac.jti, rc.jti);
        let v = t.verify(&at, TokenKind::Access, 1001).unwrap();
        assert_eq!(v, ac);
        // access TTL=900：1000+900=1900（refresh 为 1000+28800）
        assert_eq!(v.exp, 1900);
        assert_eq!(t.verify(&rt, TokenKind::Refresh, 1001).unwrap(), rc);
    }

    #[test]
    fn rejects_wrong_kind_crossuse() {
        let t = svc();
        let ((at, _), (rt, _)) = t.issue_pair("u0", 0, 1000);
        assert_eq!(
            t.verify(&rt, TokenKind::Access, 1001),
            Err(TokenError::WrongKind)
        );
        assert_eq!(
            t.verify(&at, TokenKind::Refresh, 1001),
            Err(TokenError::WrongKind)
        );
    }

    #[test]
    fn rejects_expiry_at_boundary() {
        let t = svc();
        let ((at, ac), _) = t.issue_pair("u0", 0, 1000);
        // exp 边界：now < exp 有效；now == exp 即过期
        assert!(t.verify(&at, TokenKind::Access, ac.exp - 1).is_ok());
        assert_eq!(
            t.verify(&at, TokenKind::Access, ac.exp),
            Err(TokenError::Expired)
        );
    }

    #[test]
    fn rejects_tampered_and_malformed() {
        let t = svc();
        let ((at, _), _) = t.issue_pair("u0", 0, 1000);
        let mut parts: Vec<String> = at.split('.').map(str::to_string).collect();
        // 翻转签名字段首字符：无 padding base64 末块有忽略位，改末字符可能
        // 直接解码失败（Malformed）；首字符 6 bit 全参与，必合法解码且验签失败。
        let flipped = if parts[2].starts_with('A') { "B" } else { "A" };
        parts[2].replace_range(0..1, flipped);
        assert_eq!(
            t.verify(&parts.join("."), TokenKind::Access, 1001),
            Err(TokenError::BadSignature)
        );
        // 载荷被改但签名未变
        let mut parts: Vec<String> = at.split('.').map(str::to_string).collect();
        let swapped = if parts[1].starts_with('A') { "B" } else { "A" };
        parts[1].replace_range(0..1, swapped);
        assert_eq!(
            t.verify(&parts.join("."), TokenKind::Access, 1001),
            Err(TokenError::BadSignature)
        );
        for junk in ["", "a.b", "a.b.c.d", "...", "abc", "aaa.bbb.ccc"] {
            assert!(matches!(
                t.verify(junk, TokenKind::Access, 1001),
                Err(TokenError::Malformed | TokenError::BadSignature)
            ));
        }
    }

    #[test]
    fn rejects_other_key() {
        let t1 = svc();
        let t2 = TokenSvc::new(
            b"another-unit-secret-0123456789abcdef-32B!",
            Duration::from_secs(900),
            Duration::from_secs(28800),
        )
        .unwrap();
        let ((at, _), _) = t1.issue_pair("u0", 0, 1000);
        assert_eq!(
            t2.verify(&at, TokenKind::Access, 1001),
            Err(TokenError::BadSignature)
        );
    }

    #[test]
    fn short_secret_rejected() {
        assert!(TokenSvc::new(b"short", Duration::from_secs(1), Duration::from_secs(1)).is_err());
        assert!(TokenSvc::new(
            b"0123456789abcdef0123456789abcdef",
            Duration::from_secs(0),
            Duration::from_secs(1)
        )
        .is_err());
    }
}
