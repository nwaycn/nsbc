//! 本地账号（REQ-WEB-008：本地账号 argon2id）。
//!
//! 账号源（二选一，启动期一次性载入，fail-closed）：
//!   - `NSBC_AUTH_FILE`：每行一个账号 `ROLE:username:PHC`
//!   - `NSBC_AUTH_ACCOUNTS`：同格式，账号间以 `;` 分隔
//!     （PHC 参数段含逗号，故绝不能用逗号分隔）
//! PHC 必须是 `$argon2id$...`：显式拒绝其他算法的哈希串（含 argon2i/d）。
//! 无账号源 = 空表 = 一切登录失败（不回退开发令牌）。

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
    /// 解析账号文本；任一记录非法则整体拒绝（fail-closed，禁止半表启动）。
    pub fn parse(text: &str) -> Result<AccountStore, String> {
        let mut by_name = HashMap::new();
        for (idx, raw) in text.lines().flat_map(|l| l.split(';')).enumerate() {
            let rec = raw.trim();
            if rec.is_empty() {
                continue;
            }
            // splitn(3)：PHC 内不含 ':'（PHC 分隔符为 '$' 与 ','）。
            let mut it = rec.splitn(3, ':');
            let role_s = it.next().unwrap_or("").trim();
            let name = it.next().unwrap_or("").trim();
            let phc = it.next().unwrap_or("").trim();
            let role = Role::parse(role_s)
                .ok_or_else(|| format!("account #{idx}: bad role {role_s:?}"))?;
            if !valid_username(name) {
                return Err(format!("account #{idx}: bad username {name:?}"));
            }
            // 只接受 argon2id：防止把弱算法哈希写进账号文件。
            if !phc.starts_with("$argon2id$") {
                return Err(format!("account #{idx}: hash must be argon2id PHC"));
            }
            // 提前让 argon2 解析一遍 PHC，非法哈希在启动期暴露而非登录期 500。
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

    /// 注入默认账号（NSBC_AUTH_ACCOUNTS / NSBC_AUTH_FILE 均未配置时兜底）。
    ///
    /// 默认账号通过环境变量覆盖口令；均未设置时使用硬编码：
    ///   - admin  (L2 高危)：默认 `Admin123`，env `NSBC_DEFAULT_ADMIN_PW`
    ///   - dev    (L1 运维)：默认 `Admin123`，env `NSBC_DEFAULT_DEV_PW`
    ///
    /// argon2id 哈希每次启动动态生成（salt 随机），避免硬编码泄露。
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

    /// 校验口令；任何失败（用户不存在/口令错/哈希异常）统一返回 None，
    /// 调用方对客户端给同一条 401（不区分"用户不存在"，REQ-WEB-018）。
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

/// 账号名白名单：1..64 字节，字母数字及 . _ - @（REQ-WEB-009 强类型白名单）。
pub fn valid_username(s: &str) -> bool {
    let n = s.len();
    n >= 1
        && n <= 64
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-' | b'@'))
}

/// 口令策略（REQ-WEB-008：最小长度、复杂度）。
///
/// W3b 落地最小长度+字符类复杂度两项；历史口令、强制首登改密依赖账号状态
/// 存储（当前账号源是只读 PHC 文件），登记后续单元。
/// 规则：长度 min_len..=256 字节；大写/小写/数字/符号四类至少满足三类。
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
    /// 装配：NSBC_PASSWORD_MIN_LEN（默认 12，允许收紧；下限钳 8）。
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

/// 生成生产参数 argon2id PHC（Argon2::default：m=19456KiB/t=2/p=1，
/// 随机 128bit salt）。供 `nsbc-web hash-password` 运维子命令使用。
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

    /// 测试 PHC：低代价 argon2id。
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
        assert!(AccountStore::parse(&format!("L3:u:{h}")).is_err()); // 未知角色
        assert!(AccountStore::parse(&format!("L0:::{h}")).is_err()); // 空用户名
        assert!(AccountStore::parse(&format!("L0:bad name:{h}")).is_err()); // 字符越界
        assert!(AccountStore::parse("L0:u:not-an-argon2id-hash").is_err());
        assert!(AccountStore::parse(&format!("L0:u:$argon2i${}", &h[11..])).is_err());
        assert!(AccountStore::parse(&format!("L0:u:{h}")).is_ok());
        // 重复用户名整体拒绝
        assert!(AccountStore::parse(&format!("L0:u:{h};L1:u:{h}")).is_err());
        // 空源 = 空表（fail-closed，不报错但登录全拒）
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
        assert!(p.validate("Abcdef12!xyz").is_ok()); // 四类齐全
        assert!(p.validate("abcdef12abcdef").is_err()); // 仅小写+数字 2 类
        assert!(p.validate("Abcdef12ab").is_err()); // 10 < 12
        assert!(p.validate("ABCDEF12!XYZ").is_ok()); // 大写+数字+符号=3 类，12 字节
        assert!(p.validate("allloweronly").is_err()); // 仅 1 类
        assert!(p.validate(&format!("Abcdef12!{}", "x".repeat(248))).is_err()); // >256
        // 8 字节策略下限
        let p8 = PasswordPolicy { min_len: 8 };
        assert!(p8.validate("Ab1!xyz89").is_ok());
    }

    /// 生产参数哈希（Argon2::default，较慢，每测只做 1 次）：
    /// 输出必须能被账号表解析并成功校验。
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
