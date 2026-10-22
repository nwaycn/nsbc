//! 安全/操作审计（REQ-WEB-010）。
//!
//! 与业务 tracing 日志**物理隔离**：审计事件以 JSON Lines 追加到独立文件
//! （O_APPEND，部署侧独立轮转/保留策略，默认保留 1 年），不随业务日志轮转
//! 被覆盖。每条事件落盘即 flush（管理面低频， durability 优先于吞吐）。
//!
//! 失败策略：磁盘满/权限错等写入失败只记 tracing::error，不阻断业务请求
//! （fail-open for availability；审计中断对运维可见，不能让管理面整体不可用，
//! 该取舍已在 state §6 登记）。
//!
//! 未配置 NSBC_AUDIT_FILE 时 sink 关闭（启动 warn）——开发/测试形态；
//! 生产部署必须配置。

use std::fs::{File, OpenOptions};
use std::io::{BufWriter, Write};
use std::path::Path;
use std::sync::Mutex;

use serde_json::{json, Value};

use crate::auth::tokens::now_secs;

#[derive(Default)]
pub struct AuditLog {
    sink: Option<Mutex<BufWriter<File>>>,
}

impl AuditLog {
    /// 未启用（无审计文件）。
    pub fn disabled() -> AuditLog {
        AuditLog { sink: None }
    }

    /// 打开（不存在则创建，仅追加）；路径非法/不可写返回 Err（启动 fail-closed）。
    pub fn open(path: &str) -> Result<AuditLog, String> {
        let f = OpenOptions::new()
            .create(true)
            .append(true)
            .open(Path::new(path))
            .map_err(|e| format!("open audit file {path}: {e}"))?;
        Ok(AuditLog {
            sink: Some(Mutex::new(BufWriter::new(f))),
        })
    }

    pub fn is_enabled(&self) -> bool {
        self.sink.is_some()
    }

    /// 记一条事件。`detail` 的字段平铺进事件 JSON；与保留字段重名被忽略。
    pub fn record(&self, event: &str, sub: Option<&str>, peer: &PeerCtx, detail: Value) {
        let Some(sink) = &self.sink else {
            return;
        };
        let mut o = match detail {
            Value::Object(m) => m,
            _ => serde_json::Map::new(),
        };
        // 固定字段优先，不允许 detail 覆盖。
        o.insert("ts".into(), json!(now_secs()));
        o.insert("event".into(), json!(event));
        o.insert("sub".into(), json!(sub));
        o.insert("ip".into(), json!(peer.ip));
        o.insert("ua".into(), json!(peer.ua));
        let line = match serde_json::to_vec(&Value::Object(o)) {
            Ok(mut v) => {
                v.push(b'\n');
                v
            }
            Err(e) => {
                tracing::error!(error = %e, "audit event serialize failed");
                return;
            }
        };
        if let Ok(mut w) = sink.lock() {
            if let Err(e) = w.write_all(&line).and_then(|_| w.flush()) {
                tracing::error!(error = %e, event, "audit write failed");
            }
        } else {
            tracing::error!("audit mutex poisoned; event lost: {event}");
        }
    }
}

/// 请求来源上下文（限流中间件构造后注入 extensions，全链路复用）。
#[derive(Clone, Debug, Default)]
pub struct PeerCtx {
    /// 对端 IP；无 ConnectInfo（测试/非 TCP 接入）时为 "-"
    pub ip: String,
    /// User-Agent，截断 200 字符
    pub ua: String,
}

impl PeerCtx {
    pub fn new(ip: Option<String>, ua: Option<&str>) -> PeerCtx {
        PeerCtx {
            ip: ip.unwrap_or_else(|| "-".to_string()),
            ua: ua.unwrap_or("").chars().take(200).collect(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn unique_path() -> std::path::PathBuf {
        let n = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!("nsbc_audit_{}_{}.jsonl", std::process::id(), n))
    }

    #[test]
    fn writes_json_lines_and_fixed_fields_win() {
        let path = unique_path();
        let p = path.to_string_lossy().to_string();
        let log = AuditLog::open(&p).unwrap();
        assert!(log.is_enabled());
        let peer = PeerCtx::new(Some("10.0.0.9".into()), Some("curl/8"));
        log.record(
            "login_success",
            Some("u0"),
            &peer,
            json!({"method":"password", "event":"forged"}),
        );
        log.record(
            "rate_limited",
            None,
            &peer,
            json!({"bucket":"login","retry_after":42}),
        );
        drop(log);
        let text = std::fs::read_to_string(&path).unwrap();
        let lines: Vec<Value> = text.lines().map(|l| serde_json::from_str(l).unwrap()).collect();
        assert_eq!(lines.len(), 2);
        assert_eq!(lines[0]["event"], "login_success");
        assert_eq!(lines[0]["sub"], "u0");
        assert_eq!(lines[0]["ip"], "10.0.0.9");
        assert_eq!(lines[0]["ua"], "curl/8");
        assert_eq!(lines[0]["method"], "password");
        assert!(lines[0]["ts"].as_u64().unwrap() > 0);
        assert_eq!(lines[1]["retry_after"], 42);
        assert!(lines[1]["sub"].is_null());
        std::fs::remove_file(&path).unwrap();
    }

    #[test]
    fn disabled_sink_is_silent() {
        let log = AuditLog::disabled();
        assert!(!log.is_enabled());
        log.record("x", None, &PeerCtx::default(), json!({})); // 不应恐慌
    }
}
