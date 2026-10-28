//! Compact JWT (HS256) sign/verify (REQ-WEB-008 P0, self-implemented W3a, no third-party JWT lib).
//!
//! On-wire format: `b64url(header).b64url(claims).b64url(HMAC-SHA256("header.claims"))`.
//! header is fixed to `{"alg":"HS256","typ":"JWT"}`:
//!   - alg negotiation not accepted, reject alg=none / algorithm confusion;
//!   - HMAC compared in constant time via ring;
//!   - clock injected as parameter (now=UNIX seconds), expiring behavior is unit-testable;
//!   - jti is 128bit random (getrandom), used for revocation table / refresh rotation / anti-replay.
//!
//! Only carries required session claims: sub (account name), role (0/1/2), iat/exp, jti, typ.
//! Does NOT carry any sensitive information (tokens may appear in gateway logs, REQ-WEB-018).

use std::time::Duration;

use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use ring::hmac;
use serde::{Deserialize, Serialize};

/// Fixed and unique JWT header (serialized bytes stable, compared byte-by-byte during verification).
const HEADER_JSON: &[u8] = br#"{"alg":"HS256","typ":"JWT"}"#;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TokenKind {
    /// Access token (short-lived, presented with each business request)
    Access,
    /// Refresh token (long-lived, presented only at /auth/refresh, stateful on server side)
    Refresh,
    /// Step-up token (very short-lived, only for L2 high-risk ops; RW-C3 hardening)
    StepUp,
}

impl TokenKind {
    fn tag(self) -> &'static str {
        match self {
            TokenKind::Access => "a",
            TokenKind::Refresh => "r",
            TokenKind::StepUp => "s",
        }
    }
}

/// JWT payload (access/refresh share the same structure, distinguished by typ).
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
    /// Malformed structure/encoding/JSON
    Malformed,
    /// Signature mismatch (including alg confusion, wrong key)
    BadSignature,
    /// Expired
    Expired,
    /// Token type does not match endpoint (e.g. using refresh to access business endpoints)
    WrongKind,
}

/// Current UNIX seconds (monotonic clock not suitable for wall-clock; JWT exp must be wall-clock time).
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
    /// Key must be >=32 bytes (REQ-WEB-008 session key strength; fail-closed).
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

    /// Sign a pair of tokens; returns (access, refresh) each as (string, payload),
    /// caller is responsible for registering refresh jti into the server-side session table.
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

    /// RW-C3 hardening: issue a short-lived step-up token (60s) for L2 ops.
    pub fn issue_step_up(&self, sub: &str, role: u8, now: u64) -> (String, Claims) {
        self.issue(sub, role, TokenKind::StepUp, now)
    }

    fn issue(&self, sub: &str, role: u8, kind: TokenKind, now: u64) -> (String, Claims) {
        let ttl = match kind {
            TokenKind::Access => self.access_ttl,
            TokenKind::Refresh => self.refresh_ttl,
            TokenKind::StepUp => std::time::Duration::from_secs(60), // RW-C3: step-up 只活 60 秒
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

    /// Verify: encoding structure -> header byte-by-byte -> HMAC (constant time) -> claims -> type -> expiry.
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
            // alg confusion / alg=none / header not issued by this service
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

/// 128-bit random jti -> lowercase hex (32 chars).
fn new_jti() -> String {
    let mut raw = [0u8; 16];
    // When the process cannot continue (OS RNG failure), fail sign rather than degrade: expect mapped to 5xx by caller.
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
        // access TTL=900: 1000+900=1900 (refresh is 1000+28800)
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
        // exp boundary: now < exp means valid; now == exp means expired
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
        // Flip first char of signature field: unpadded base64 has ignore bits in last block, changing last char may
        // cause direct decode failure (Malformed); first char 6 bits all participate, so always valid decode but signature fails.
        let flipped = if parts[2].starts_with('A') { "B" } else { "A" };
        parts[2].replace_range(0..1, flipped);
        assert_eq!(
            t.verify(&parts.join("."), TokenKind::Access, 1001),
            Err(TokenError::BadSignature)
        );
        // payload modified but signature unchanged
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
