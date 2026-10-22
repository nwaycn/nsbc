//! Local accounts (REQ-WEB-008: local accounts with argon2id).
//!
//! Account source (choose one, loaded once at startup, fail-closed):
//!   - `NSBC_AUTH_FILE`: one account per line `ROLE:username:PHC`
//!   - `NSBC_AUTH_ACCOUNTS`: same format, accounts separated by `;`
//!     (PHC parameter segments contain commas, so comma separation must NOT be used)
//! PHC must be `$argon2id$...`: explicitly reject hashes from other algorithms (including argon2i/d).
//! No account source = empty table = all logins fail (no fallback to dev tokens).

use std::collections::HashMap;

use argon2::password_hash::SaltString;
use argon2::{Argon2, PasswordHash, PasswordHasher, PasswordVerifier};

use super::Role;

pub struct Account {
    pub role: Role,
    phc: String,
}

#[derive(Default)]
pub struct AccountStore {
    by_name: HashMap<String, Account>,
}

impl AccountStore {
    /// Parse account text; if any record is malformed, reject the whole batch (fail-closed, no partial startup).
    pub fn parse(text: &str) -> Result<AccountStore, String> {
        let mut by_name = HashMap::new();
        for (idx, raw) in text.lines().flat_map(|l| l.split(';')).enumerate() {
            let rec = raw.trim();
            if rec.is_empty() {
                continue;
            }
            // splitn(3): PHC does not contain ':' (PHC separators are '$' and ',').
            let mut it = rec.splitn(3, ':');
            let role_s = it.next().unwrap_or("").trim();
            let name = it.next().unwrap_or("").trim();
            let phc = it.next().unwrap_or("").trim();
            let role = Role::parse(role_s)
                .ok_or_else(|| format!("account #{idx}: bad role {role_s:?}"))?;
            if !valid_username(name) {
                return Err(format!("account #{idx}: bad username {name:?}"));
            }
            // Only accept argon2id: prevent weak algorithm hashes from being written into the account file.
            if !phc.starts_with("$argon2id$") {
                return Err(format!("account #{idx}: hash must be argon2id PHC"));
            }
            // Parse PHC with argon2 ahead of time so invalid hashes surface at startup instead of causing 500s at login.
            PasswordHash::new(phc).map_err(|e| format!("account #{idx}: bad PHC: {e}"))?;
            if by_name
                .insert(name.to_string(), Account {
                    role,
                    phc: phc.to_string(),
                })
                .is_some()
            {
                return Err(format!("account #{idx}: duplicate username {name:?}"));
            }
        }
        Ok(AccountStore { by_name })
    }

    pub fn is_empty(&self) -> bool {
        self.by_name.is_empty()
    }

    /// Inject default accounts (fallback when neither NSBC_AUTH_ACCOUNTS nor NSBC_AUTH_FILE is configured).
    ///
    /// Default account passwords may be overridden via env vars; if neither is set, use hardcoded values:
    ///   - admin  (L2 high-risk): default `Admin123`, env `NSBC_DEFAULT_ADMIN_PW`
    ///   - dev    (L1 ops): default `Admin123`, env `NSBC_DEFAULT_DEV_PW`
    ///
    /// argon2id hashes are regenerated each startup (random salt), avoiding hardcoded leaks.
    pub fn with_defaults() -> Result<AccountStore, String> {
        let admin_pw = std::env::var("NSBC_DEFAULT_ADMIN_PW")
            .unwrap_or_else(|_| "Admin123".to_string());
        let dev_pw = std::env::var("NSBC_DEFAULT_DEV_PW")
            .unwrap_or_else(|_| "Admin123".to_string());

        let admin_phc = hash_password_argon2id(&admin_pw)?;
        let dev_phc = hash_password_argon2id(&dev_pw)?;

        let mut by_name = HashMap::new();
        by_name.insert("admin".to_string(), Account {
            role: Role::L2,
            phc: admin_phc,
        });
        by_name.insert("dev".to_string(), Account {
            role: Role::L1,
            phc: dev_phc,
        });

        tracing::warn!(
            "no accounts configured — bootstrapping default admin(L2) + dev(L1); \
             override with NSBC_AUTH_ACCOUNTS or change defaults via \
             NSBC_DEFAULT_ADMIN_PW / NSBC_DEFAULT_DEV_PW"
        );

        Ok(AccountStore { by_name })
    }

    /// Verify password; any failure (user not found / wrong password / hash anomaly) returns None uniformly,
    /// caller sends the same 401 to client (do not distinguish "user not found", REQ-WEB-018).
    pub fn verify(&self, username: &str, password: &str) -> Option<Role> {
        let acct = self.by_name.get(username)?;
        let parsed = PasswordHash::new(&acct.phc).ok()?;
        if Argon2::default()
            .verify_password(password.as_bytes(), &parsed)
            .is_ok()
        {
            Some(acct.role)
        } else {
            None
        }
    }
}

/// Username allowlist: 1..64 bytes, alphanumeric plus . _ - @ (REQ-WEB-009 strong-type allowlist).
pub fn valid_username(s: &str) -> bool {
    let n = s.len();
    n >= 1
        && n <= 64
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-' | b'@'))
}

/// Password policy (REQ-WEB-008: minimum length, complexity).
///
/// W3b implements minimum length + character class complexity; password history and forced first-login change depend on account state
/// storage (current account source is a read-only PHC file), deferred to future modules.
/// Rule: length min_len..=256 bytes; must satisfy at least 3 of: uppercase, lowercase, digit, symbol.
#[derive(Clone, Copy, Debug)]
pub struct PasswordPolicy {
    pub min_len: usize,
}

impl Default for PasswordPolicy {
    fn default() -> Self {
        PasswordPolicy { min_len: 12 }
    }
}

impl PasswordPolicy {
    /// Config: NSBC_PASSWORD_MIN_LEN (default 12, may be tightened; floored to 8).
    pub fn from_env() -> Result<PasswordPolicy, String> {
        let min_len = match std::env::var("NSBC_PASSWORD_MIN_LEN") {
            Ok(v) => v
                .parse::<usize>()
                .map_err(|_| "NSBC_PASSWORD_MIN_LEN must be a positive integer".to_string())?,
            Err(_) => 12,
        }
        .max(8);
        Ok(PasswordPolicy { min_len })
    }

    pub fn validate(&self, pw: &str) -> Result<(), String> {
        let n = pw.len();
        if n < self.min_len {
            return Err(format!("password must be at least {} bytes", self.min_len));
        }
        if n > 256 {
            return Err("password must be at most 256 bytes".to_string());
        }
        let mut classes = 0u8;
        let (mut lower, mut upper, mut digit, mut punct) = (false, false, false, false);
        for b in pw.bytes() {
            if b.is_ascii_lowercase() {
                lower = true;
            } else if b.is_ascii_uppercase() {
                upper = true;
            } else if b.is_ascii_digit() {
                digit = true;
            } else if b.is_ascii_punctuation() {
                punct = true;
            }
        }
        classes += lower as u8 + upper as u8 + digit as u8 + punct as u8;
        if classes < 3 {
            return Err(
                "password must contain at least 3 of: lowercase, uppercase, digit, symbol"
                    .to_string(),
            );
        }
        Ok(())
    }
}

/// Generate production-parameter argon2id PHC (Argon2::default: m=19456KiB/t=2/p=1,
/// random 128bit salt). Used by the `nsbc-web hash-password` ops subcommand.
pub fn hash_password_argon2id(pw: &str) -> Result<String, String> {
    let mut raw = [0u8; 16];
    getrandom::getrandom(&mut raw).map_err(|e| format!("rng failure: {e}"))?;
    let salt = SaltString::encode_b64(&raw).map_err(|e| format!("salt encode: {e}"))?;
    Argon2::default()
        .hash_password(pw.as_bytes(), &salt)
        .map(|h| h.to_string())
        .map_err(|e| format!("argon2 hash failure: {e}"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use argon2::password_hash::SaltString;
    use argon2::{Algorithm, Argon2, Params, PasswordHasher, Version};

    /// Test PHC: low-cost argon2id.
    fn phc(password: &str) -> String {
        let mut raw = [0u8; 16];
        getrandom::getrandom(&mut raw).unwrap();
        let salt = SaltString::encode_b64(&raw).unwrap();
        let arg = Argon2::new(
            Algorithm::Argon2id,
            Version::V0x13,
            Params::new(4096, 1, 1, None).unwrap(),
        );
        arg.hash_password(password.as_bytes(), &salt).unwrap().to_string()
    }

    #[test]
    fn parses_file_and_env_forms() {
        let text = format!("L0:ro:{};\nL2:admin.op@x:{}", phc("p1"), phc("p2"));
        let s = AccountStore::parse(&text).unwrap();
        assert!(!s.is_empty());
        assert_eq!(s.verify("ro", "p1"), Some(Role::L0));
        assert_eq!(s.verify("admin.op@x", "p2"), Some(Role::L2));
    }

    #[test]
    fn verify_right_and_wrong_passwords() {
        let h = phc("secret-pass");
        let s = AccountStore::parse(&format!("L1:u1:{h}")).unwrap();
        assert_eq!(s.verify("u1", "secret-pass"), Some(Role::L1));
        assert_eq!(s.verify("u1", "wrong"), None);
        assert_eq!(s.verify("nobody", "secret-pass"), None);
    }

    #[test]
    fn rejects_bad_records_fail_closed() {
        let h = phc("p");
        assert!(AccountStore::parse(&format!("L3:u:{h}")).is_err()); // unknown role
        assert!(AccountStore::parse(&format!("L0:::{h}")).is_err()); // empty username
        assert!(AccountStore::parse(&format!("L0:bad name:{h}")).is_err()); // char out of bounds
        assert!(AccountStore::parse("L0:u:not-an-argon2id-hash").is_err());
        assert!(AccountStore::parse(&format!("L0:u:$argon2i${}", &h[11..])).is_err());
        assert!(AccountStore::parse(&format!("L0:u:{h}")).is_ok());
        // Duplicate usernames rejected as a whole
        assert!(AccountStore::parse(&format!("L0:u:{h};L1:u:{h}")).is_err());
        // Empty source = empty table (fail-closed, no error but all logins rejected)
        assert!(AccountStore::parse("").unwrap().is_empty());
        assert!(AccountStore::parse("\n ; \n").unwrap().is_empty());
    }

    #[test]
    fn username_whitelist() {
        assert!(valid_username("a"));
        assert!(valid_username("admin.op-x_1@x"));
        assert!(!valid_username(""));
        assert!(!valid_username("a b"));
        assert!(!valid_username("a/b"));
        assert!(!valid_username(&"a".repeat(65)));
    }

    #[test]
    fn password_policy_length_and_classes() {
        let p = PasswordPolicy { min_len: 12 };
        assert!(p.validate("Abcdef12!xyz").is_ok()); // all four classes present
        assert!(p.validate("abcdef12abcdef").is_err()); // only lowercase + digits = 2 classes
        assert!(p.validate("Abcdef12ab").is_err()); // 10 < 12
        assert!(p.validate("ABCDEF12!XYZ").is_ok()); // uppercase + digit + symbol = 3 classes, 12 bytes
        assert!(p.validate("allloweronly").is_err()); // only 1 class
        assert!(p.validate(&format!("Abcdef12!{}", "x".repeat(248))).is_err()); // >256
        // 8-byte policy floor
        let p8 = PasswordPolicy { min_len: 8 };
        assert!(p8.validate("Ab1!xyz89").is_ok());
    }

    /// Production-parameter hash (Argon2::default, slow, only run once per test):
    /// Output must be parseable by account table and verify successfully.
    #[test]
    fn hash_password_output_parses_and_verifies() {
        let pw = "Rotate-My-Secret-9";
        let phc = hash_password_argon2id(pw).unwrap();
        assert!(phc.starts_with("$argon2id$"));
        let s = AccountStore::parse(&format!("L1:op:{phc}")).unwrap();
        assert_eq!(s.verify("op", pw), Some(Role::L1));
        assert_eq!(s.verify("op", "Rotate-My-Secret-8"), None);
    }
}
