//! Web→核心实时控制通道客户端（REQ-WEB-004 主，辅 REQ-WEB-013/014）。
//!
//! 唯一真实通道：核心 obs_control UNIX domain stream socket
//! （cfg `oam.control_sock: unix:<path>`，文件权限 0600，仅本机特权账户）。
//! 行文本协议，每帧以提示符 `nsbc> ` 结束：
//!   连接横幅 : `NSBC <v> control interface (try 'help')\nnsbc> `
//!   `version`             → `nsbcd <v>\n`
//!   `show calls`          → `active=N total=N failed=N\n`
//!   `show channels`       → 每呼叫一行 `call a_callid=.. a_state=.. …`
//!                           + 尾行 `channels=N truncated=0|1`
//!   `uuid_dump <call-id>` → 逐 Leg `leg N state/proto/cseq/tags/media/…`
//!                           SDP 段长度分帧（`leg N sdp_offer <len>` 后紧跟原字节）
//!   `config.get <key>`    → 头行 `config key=.. layer=.. secret=0|1`
//!                           + 值原文一行（敏感键 secret=1，值为 ****）
//!   `config.export`       → 头行 `config-export keys=N redact=1`
//!                           + 每键一行 `<dotted.key>: <value|****>`
//!   `show registrations`  → `registrations=N\n`
//!   `stats`               → Prometheus exposition（`# HELP/# TYPE` + `name value`）
//! 核心拒绝时响应行以 `ERR` 起始。只读查询经 request()；写操作（挂断）
//! 与需自分流 notfound 的只读命令（uuid_dump/config.get）用 send_cmd；
//! config.set 等 Web 写入口暂不开放（P1 统一命令注册表+Idempotency-Key）。
//!
//! 容错（REQ-WEB-014）：连接 / I/O / 超时 / 协议错误显式分类，由路由映射为
//! 502/504，边车不缓存、不伪造核心数据。所有解析均为纯函数，跨平台单测；
//! UDS 传输仅 `cfg(unix)` 编译，Windows 开发机构建返回 [`CoreError::Unsupported`]。
use std::collections::BTreeMap;
use std::fmt;
use std::future::Future;
use std::path::{Path, PathBuf};
use std::time::Duration;

use serde::Serialize;

/// 核心控制口默认路径（与 configs/nsbc.yaml 的 oam.control_sock 一致）。
pub const DEFAULT_SOCK_PATH: &str = "/var/run/nsbc/control.sock";

const PROMPT: &[u8] = b"nsbc> ";
/// stats  exposition 核心侧上限 256KB，帧上限留足余量。
/// 仅 cfg(unix) 传输读取路径使用，Windows 开发机构建无消费方。
#[cfg_attr(not(unix), allow(dead_code))]
const MAX_FRAME: usize = 512 * 1024;
const DEFAULT_TIMEOUT_MS: u64 = 2000;

/// 与核心控制面交互的错误分类（REQ-WEB-014）。
/// Connect/Io/Timeout 仅由 cfg(unix) 真实传输构造；Windows 构建下分类仍由
/// mock 单测与路由 502/504 映射覆盖，故非 unix 目标允许 dead_code。
#[derive(Clone, Debug, PartialEq, Eq)]
#[cfg_attr(not(unix), allow(dead_code))]
pub enum CoreError {
    /// 连接控制口失败（核心未起 / 权限不足 / 路径不存在）
    Connect(String),
    /// 已连接后的流 I/O 失败（对端重置等）
    Io(String),
    /// 单次请求超时
    Timeout,
    /// 目标对象不存在（如 hangup 的 Call-ID 已结束/不存在）→ HTTP 404
    NotFound(String),
    /// 协议违例：帧不完整、UTF-8 非法、输出不符合契约
    Protocol(String),
    /// 核心返回 ERR 行
    CoreRejected(String),
    /// 当前构建目标不支持该传输（Windows 无 UDS 控制口）
    Unsupported(&'static str),
}

impl fmt::Display for CoreError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CoreError::Connect(e) => write!(f, "core connect failed: {e}"),
            CoreError::Io(e) => write!(f, "core socket io error: {e}"),
            CoreError::Timeout => f.write_str("core control request timed out"),
            CoreError::NotFound(w) => write!(f, "core object not found: {w}"),
            CoreError::Protocol(e) => write!(f, "core protocol error: {e}"),
            CoreError::CoreRejected(e) => write!(f, "core rejected: {e}"),
            CoreError::Unsupported(e) => write!(f, "unsupported core transport: {e}"),
        }
    }
}

impl std::error::Error for CoreError {}

/// `version` 响应。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct VersionInfo {
    /// 核心版本字符串（如 "2.5.5"）
    pub core: String,
}

/// `show calls` 计数器（REQ-WEB-004 实时话务只读子集）。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct CallCounters {
    pub active: u64,
    pub total: u64,
    pub failed: u64,
}

/// 单条 dialplan Application（show apps 每行 "name  help"）。
/// REQ-DP-APP-001 / REQ-AI-001：供前端 app_chain 编辑器下拉选择。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct AppInfo {
    pub name: String,
    pub help: String,
}

/// 单条 Codec 描述（show codecs 每行格式：name,pt,sample_rate,impl,fmtp）。
/// 来源：核心侧 codec_registry 自注册条目；mod_codecs（G711/G729）、
/// mod_av（G722/Opus/H264/VP8/VP9）、或未来 iLBC/AMR 模块 init 时填入。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct CodecInfo {
    /// 编解码名称，例 "PCMA" / "G729" / "G722" / "Opus" / "H264"
    pub name: String,
    /// RTP Payload Type（静态 0-95 或动态 96+）
    pub pt: u8,
    /// 采样率（Hz）：8000 / 16000 / 48000 / 90000（视频）
    pub sample_rate: u32,
    /// 注册模块名称，例 "mod_codecs" / "mod_av" — 帮助运维判断 codec 来自哪个 .so
    pub impl_module: String,
    /// 可选 fmtp 默认值，例 "annexb=yes" / "maxplaybackrate=16000" / "packetization-mode=0"
    pub fmtp_default: String,
    /// 媒体类型："audio" / "video" — 前端分机编辑页按类型分组 codec 下拉
    pub media_type: String,
}

/// 解析 show codecs 输出：
///   codecs=N
///   NAME        PT  SRATE  IMPL        FMTP                       MEDIA
///   PCMA        8   8000   mod_codecs  -                          audio
///   G729        18  8000   mod_codecs  annexb=yes                 audio
///   G722        9   16000  mod_av      -                          audio
///   Opus        111 48000  mod_av      maxplaybackrate=16000      audio
///   H264        96  90000  mod_av      packetization-mode=0        video
fn parse_show_codecs(buf: &str) -> Vec<CodecInfo> {
    let mut out = Vec::new();
    for line in buf.lines() {
        let trimmed = line.trim();
        if trimmed.is_empty() { continue; }
        if let Some(rest) = trimmed.strip_prefix("codecs=") {
            // 头行 codecs=N
            if rest.trim().is_empty() { continue; }
            continue;
        }
        // 跳过表头/提示行
        let low = trimmed.to_ascii_lowercase();
        if low.starts_with("name") || low.starts_with('(') { continue; }

        // 5+ 空格分隔列：NAME PT SRATE IMPL FMTP MEDIA_TYPE
        // 用 split_whitespace 再组合（FMTP 可含空格如 "annexb=yes"）
        let parts: Vec<&str> = trimmed.split_whitespace().collect();
        if parts.len() < 5 { continue; }

        let name = parts[0].to_string();
        let pt: u8 = match parts[1].parse() { Ok(v) => v, Err(_) => continue };
        let sample_rate: u32 = match parts[2].parse() { Ok(v) => v, Err(_) => continue };
        let impl_module = parts[3].to_string();
        // MEDIA_TYPE 固定在最后一列；FMTP 为中间剩余列的拼接
        let media_type = parts.last().unwrap_or(&"audio").to_string();
        let fmtp_default = if parts.len() >= 6 {
            parts[4..parts.len()-1].join(" ")
        } else {
            "-".to_string()
        };

        out.push(CodecInfo { name, pt, sample_rate, impl_module, fmtp_default, media_type });
    }
    out
}

/// 解析 show apps 输出：
/// 头行 apps=N，后续每行 "name  help"（help 允许空）。
fn parse_show_apps(buf: &str) -> Vec<AppInfo> {
    let mut out = Vec::new();
    for line in buf.lines() {
        if let Some(rest) = line.strip_prefix("apps=") {
            if rest.trim().is_empty() { continue; }
            continue;
        }
        let trimmed = line.trim();
        if trimmed.is_empty() { continue; }
        // 跳过括号提示 "(no app registry...)"
        if trimmed.starts_with('(') && trimmed.ends_with(')') { continue; }
        // "name  help" — 按第一个双空格切；help 里可能含单空格
        let mut parts = trimmed.splitn(2, "  ");
        let name = parts.next().unwrap_or("").trim().to_string();
        let help = parts.next().unwrap_or("").trim().to_string();
        if !name.is_empty() {
            out.push(AppInfo { name, help });
        }
    }
    out
}

/// 单条注册 binding（show registrations 每行）。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct BindingEntry {
    pub aor: String,
    pub user: String,
    pub contact: String,
    pub expires_sec: Option<i64>,
    pub alive_sec: Option<i64>,
    pub q: Option<i64>,
    /* === NAT 元数据（REQ-NAT-001）===
     * C 侧 obs_control.c cmd_reg_line_cb 输出：nat=N transport=X received_ip=X received_port=N pub_contact=X */
    pub nat_detected: Option<i64>,
    pub transport: String,
    pub received_ip: String,
    pub received_port: Option<i64>,
    pub public_contact: String,
}

/// `show registrations` 完整输出：每绑定一行 + 尾行汇总。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct RegistrationSummary {
    pub bindings: Vec<BindingEntry>,
    pub registrations: u64,
    pub truncated: bool,
}

/// `config.get <key>` 生效值视图（REQ-CFG-011）。
/// secret=true 时 value 恒为 None（核心 **** 脱敏，Web 不回显占位串）；
/// layer 为命中配置层（default/system/env/profile/runtime/session）。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ConfigValue {
    pub key: String,
    pub value: Option<String>,
    pub layer: String,
    pub secret: bool,
}

/// `config.export` 全量快照中的单键项（REQ-CFG-013）。
/// 脱敏导出下 secret=true 时 value 恒为 None（核心 ****，占位串不出边车）。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ConfigExportItem {
    pub key: String,
    pub value: Option<String>,
    pub secret: bool,
}

/// `config.export` 脱敏全量生效视图（GET /config）。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ConfigExport {
    pub redacted: bool,
    pub items: Vec<ConfigExportItem>,
}

/// `show channels` 单行：一个在途呼叫的 A/B Leg 摘要（REQ-OAM-007）。
/// 缺腿（如 originate 仅 B 腿在轮）或核心未采集字段为 None。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct CallSummary {
    pub a_call_id: Option<String>,
    pub a_state: Option<String>,
    pub b_call_id: Option<String>,
    pub b_state: Option<String>,
    pub caller: Option<String>,
    pub callee: Option<String>,
    pub start_ms: Option<u64>,
    pub answer_ms: Option<u64>,
    pub duration_ms: Option<u64>,
    pub codec_a: Option<String>,
    pub codec_b: Option<String>,
    pub rtp_loss_pct: Option<u32>,
    pub rtp_jitter_ms: Option<u32>,
    pub mos_x1000: Option<i32>,
}

/// `show channels` 全量快照；truncated=true 表示核心侧缓冲不足、尾部被截。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct CallList {
    pub calls: Vec<CallSummary>,
    pub truncated: bool,
}

/// 单 Leg 媒体面视图（REQ-OBS-010）。None = 核心无采集来源（UNAVAILABLE，
/// 如 P0 媒体未回挂 b2b 或 bypass 腿不经本设备），严禁以估算值填充。
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct LegMedia {
    pub mode: Option<String>,
    pub gate: Option<String>,
    pub ssrc_in: Option<String>,
    pub ssrc_out: Option<String>,
    /// 丢包/抖动/RTT/R-Factor/MOS 等 VQ 集合（P0 一律 None）。
    pub vq: Option<String>,
}

/// `uuid_dump` 的单 Leg 明细：FSM/传输/CSeq/tag + 协商 SDP 原文 + 媒体视图。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct LegDump {
    pub leg: u8,
    pub state: String,
    pub proto: String,
    pub reliable: bool,
    pub cseq: i64,
    pub routes: u32,
    pub local_tag: Option<String>,
    pub remote_tag: Option<String>,
    pub offer: Option<String>,
    pub answer: Option<String>,
    pub media: LegMedia,
}

/// `uuid_dump <call-id>` 双侧 Leg 诊断（REQ-OBS-010：不另造 ID，以
/// A/B Leg Call-ID 为锚点；bypass/未接字段经 LegMedia=None 如实表达）。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct CallDetail {
    pub a_call_id: Option<String>,
    pub a_state: Option<String>,
    pub b_call_id: Option<String>,
    pub b_state: Option<String>,
    pub legs: Vec<LegDump>,
}

/// `stats` 的 Prometheus exposition 快照：`指标名 → 值`。
/// 当前核心指标均为 gauge 且无 label；带 `{...}` label 的采样按指标名聚合
/// （同名最后一条生效），供仪表盘只读展示。权威计数仍以专用命令为准。
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct MetricSnapshot(pub BTreeMap<String, f64>);

/// `config.set` 响应。`changed=true` 表示核心侧实际修改了值；
/// `changed=false` 表示键已存在新值（幂等成功，不算写入）。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ConfigSetResult {
    pub key: String,
    pub changed: bool,
}

/// 封禁/观察条目（`security.ban.list` 的 `ent` 行解析结果）。
/// state: "ban"=封禁中（remaining_s>0）；"watch"=窗口内计数未达阈值。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct BanEntry {
    pub ip: String,
    pub state: String,
    pub fails: u32,
    pub remaining_s: u64,
    pub act_ago_s: u64,
}

/// 解析 `security.ban.list [filter]` 响应帧：
/// 每条一行 `ent ip=X state=ban|watch fails=N remaining_s=N act_ago_s=N`，
/// 尾行 `bans=N shown=M total=K`（跳过）；ERR 前缀 → CoreRejected。
pub fn parse_ban_list(body: &str) -> Result<Vec<BanEntry>, CoreError> {
    let mut out = Vec::new();
    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() { continue; }
        if let Some(msg) = line.strip_prefix("ERR") {
            return Err(CoreError::CoreRejected(msg.trim().to_string()));
        }
        let Some(rest) = line.strip_prefix("ent ") else { continue };
        let toks: Vec<&str> = rest.split_whitespace().collect();
        let kv = |k: &str| {
            toks.iter()
                .find(|t| t.starts_with(k) && t.len() > k.len() && t.as_bytes()[k.len()] == b'=')
                .map(|t| &t[k.len() + 1..])
        };
        let (Some(ip), Some(state)) = (kv("ip"), kv("state")) else {
            return Err(CoreError::Protocol("ban ent missing ip/state".into()));
        };
        out.push(BanEntry {
            ip: ip.to_string(),
            state: state.to_string(),
            fails: kv("fails").and_then(|v| v.parse().ok()).unwrap_or(0),
            remaining_s: kv("remaining_s").and_then(|v| v.parse().ok()).unwrap_or(0),
            act_ago_s: kv("act_ago_s").and_then(|v| v.parse().ok()).unwrap_or(0),
        });
    }
    Ok(out)
}

/// 核心侧最近 CDR（`cdr.recent` 命令输出单行的解析结果）。
/// 字段与 cdr_event_t 一一对齐；`end_ms/billsec/duration_s` 等未应答时为 0。
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct CdrEvent {
    pub call_id: String,
    pub tenant: Option<String>,
    pub from_num: String,
    pub to_num: String,
    pub trunk: Option<String>,
    pub rec_path: Option<String>,
    pub dir: String,
    pub phase: String,
    pub disposition: String,
    pub start_ms: u64,
    pub answer_ms: u64,
    pub end_ms: u64,
    pub billsec: u32,
    pub duration_s: u32,
    pub cause: i32,
    pub emergency: bool,
    /* ---- 网络端点 + codec（与 cdr_event_t 一一对齐）---- */
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub from_ip: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub a_peer_ip: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub b_peer_ip: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub codec_a: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub codec_b: Option<String>,
}

/// `status` 综合状态快照（L1，REQ-CCMD-001，核心 cmd_registry 新增命令）。
/// 核心输出格式：`status active_calls=N total=N failed=N registrations=N
/// profiles=N started_ago_ms=U\nalive=1`（首行键值对；`alive=1` 为末尾锚行）。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct StatusInfo {
    pub active_calls: u64,
    pub total_calls: u64,
    pub failed_calls: u64,
    pub registrations: u64,
    pub profiles: u64,
    pub started_ago_ms: u64,
}

/// `show rates` 的单条费率（与 C 侧 rate_entry_t 对齐，REQ-OAM-009）。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct RateEntry {
    pub prefix: String,
    pub name: String,
    pub price_per_min_uc: i64,
    pub connect_fee_uc: i64,
    pub bill_inc_s: i32,
    pub min_bill_s: i32,
    pub initial_s: i32,
    pub enabled: bool,
}

/// `show rates` 清单 + 汇总。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct RateList {
    pub rates: Vec<RateEntry>,
    pub total: i64,
}

/// `show balances` 的单条账户。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct AccountBalance {
    pub key: String,
    pub balance_uc: i64,
    pub reserved_uc: i64,
    pub credit_limit_uc: i64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct AccountList {
    pub accounts: Vec<AccountBalance>,
    pub total: i64,
}

/// `show transcode` 的单条活跃 bridge 会话（REQ-OAM-010）。
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct BridgeSession {
    pub idx: i64,
    #[serde(rename = "call_key_ptr")]
    pub call_key_ptr: String,
    pub mode: String,
    pub a_rtp: u16,
    pub b_rtp: u16,
    pub pkts: u64,
    pub drops: u64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct BridgeList {
    pub sessions: Vec<BridgeSession>,
    pub total: i64,
}

/// 核心控制面只读能力。泛型注入（而非 `dyn`），生产用 [`RealCore`]，
/// 路由测试用 mock；无需 async-trait 等额外依赖。
///
/// 返回显式声明为 `impl Future + Send`（而非裸 `async fn`）：axum handler 的
/// Future 必须 Send，裸 RPITIT 不带 Send 约束会让泛型 handler 不满足 Handler。
pub trait CoreApi: Send + Sync {
    fn version(&self) -> impl Future<Output = Result<VersionInfo, CoreError>> + Send;
    fn call_counters(
        &self,
    ) -> impl Future<Output = Result<CallCounters, CoreError>> + Send;
    fn registrations(
        &self,
    ) -> impl Future<Output = Result<RegistrationSummary, CoreError>> + Send;
    fn metrics(&self) -> impl Future<Output = Result<MetricSnapshot, CoreError>> + Send;
    /// 在途呼叫清单（`show channels`，L0 只读，REQ-OAM-007）。
    fn list_calls(
        &self,
    ) -> impl Future<Output = Result<CallList, CoreError>> + Send;
    /// 单呼叫双侧 Leg 诊断（`uuid_dump <call-id>`，L0 只读，
    /// REQ-OAM-007/010）：Call-ID 已结束/不存在 → [`CoreError::NotFound`]。
    fn call_detail(
        &self,
        call_id: &str,
    ) -> impl Future<Output = Result<CallDetail, CoreError>> + Send;
    /// 单个配置键生效值（`config.get <key>`，L0 只读，REQ-CFG-011）：
    /// 键不存在 → [`CoreError::NotFound`]；敏感键 value=None/secret=true。
    fn config_get(
        &self,
        key: &str,
    ) -> impl Future<Output = Result<ConfigValue, CoreError>> + Send;
    /// 全量脱敏生效视图（`config.export`，L0 只读，REQ-CFG-013）：
    /// 敏感键 value=None/secret=true；核心拒绝见 [`CoreError::CoreRejected`]。
    fn config_export(&self)
        -> impl Future<Output = Result<ConfigExport, CoreError>> + Send;
    /// 强制挂断（REQ-OAM-004，写操作/L1 审计）：成功 Ok；目标不存在
    /// [`CoreError::NotFound`]；核心拒绝见 [`CoreError::CoreRejected`]。
    fn hangup(
        &self,
        call_id: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// 拨号 Application 注册表（`show apps`，L0 只读，REQ-DP-APP-001/REQ-AI-001）。
    /// 返回所有已注册的 dialplan app 列表（answer/ai/bridge/...），
    /// 未加载 dialplan 模块返回空数组。核心侧用 `app_help` 逐行输出，
    /// 格式 `name  help`，Web 侧解析转 JSON。 */
    fn dialplan_apps(&self)
        -> impl Future<Output = Result<Vec<AppInfo>, CoreError>> + Send;

    /// 拉取核心侧 Codec 注册表（`show codecs`）。
    /// W-06 动态 codec 列表路由；C 侧 codec_registry 自注册后
    /// 所有 mod_codecs / mod_av / 未来 iLBC/AMR 初始化条目会自动显示。
    /// 未加载任何 codec 模块返回空数组（非 CoreRejected）。
    fn show_codecs(&self)
        -> impl Future<Output = Result<Vec<CodecInfo>, CoreError>> + Send;

    /// 配置键热设置（`config.set <k> <v>`，L1，REQ-CFG-011，核心侧审计）。
    /// 返回 `ConfigSetResult` 含 applied/changed 标志；核心拒绝见
    /// [`CoreError::CoreRejected`]；键不存在/值非法等 → 核心 ERR。
    fn config_set(
        &self,
        key: &str,
        value: &str,
    ) -> impl Future<Output = Result<ConfigSetResult, CoreError>> + Send;

    /// 配置键 dry-run 校验（`config.validate <k> <v>`，L0，REQ-CFG-011，不应用）。
    /// Ok 表示键和值通过核心侧校验；核心拒绝见 [`CoreError::CoreRejected`]。
    fn config_validate(
        &self,
        key: &str,
        value: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// 拉取核心侧最近 CDR（`cdr.recent [N]`，REQ-OBS-006 P1）。
    /// 单帧多行动态，尾部 `cdrs=N` 锚行；核心未实现 →
    /// [`CoreError::CoreRejected`]（ERR unknown command）。
    fn cdr_recent(
        &self,
        limit: u32,
    ) -> impl Future<Output = Result<Vec<CdrEvent>, CoreError>> + Send;

    // ===== 安全封禁管理（security.ban.* 命令）=====

    /// `security.ban.list [filter]` — 封禁/观察条目（filter=IP 子串搜索）。
    fn security_ban_list(
        &self,
        filter: &str,
    ) -> impl Future<Output = Result<Vec<BanEntry>, CoreError>> + Send;

    /// `security.ban.unban <ip>` — 解禁（清除该 IP 全部计数与封禁状态）。
    fn security_ban_unban(
        &self,
        ip: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    // ===== reload 命令（Web DB → C 内存，假期回来 C 侧实现 cmd_register）=====

    /// `extensions.reload <json>` — 分机表全量替换。
    fn reload_extensions(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `gateways.reload <json>` — 网关/路由组表全量替换。
    fn reload_gateways(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `trunks.reload <json>` — 中继表全量替换。
    fn reload_trunks(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `dialplan-rules.reload <json>` — Dialplan 规则表全量替换。
    fn reload_dialplan_rules(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `queues.reload <json>` — ACD 队列表全量替换（REQ-PBX-CC-002）。
    fn reload_queues(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `profiles.reload <json>` — SIP Profile 表 RCU 原子替换。
    fn reload_sip_profiles(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `security.blacklist.reload <json>` — 静态安全黑名单（IP CIDR/UA）整表替换。
    fn reload_security_blacklist(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    // ===== REQ-CCMD-001 命令增强：呼叫级 L2 写操作 + originate + status =====

    /// 通用呼叫级写操作（uuid_kill/hold/transfer/record/answer/send_dtmf/
    /// broadcast/break/bridge —— 走 cmd_registry 路由）。
    /// `action` 映射为核心命令名（如 `"kill"` → `uuid_kill`），
    /// `args` 拼接为后续空格分隔的参数。返回 Ok(()) 表示核心接受命令；
    /// Call-ID 不存在 → [`CoreError::NotFound`]；核心拒绝 → [`CoreError::CoreRejected`]。
    fn call_action(
        &self,
        call_id: &str,
        action: &str,
        args: &[&str],
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `send_msg <call-id> <body>` —— 对话内发送 MESSAGE（REQ-SIP-MSG-001）。
    /// 独立于 call_action 因为核心命令名无前缀、且 body 可含空格。
    fn send_message(
        &self,
        call_id: &str,
        body: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `originate <call-url>` — 发起新呼出（L2）。call-url 格式由 Dialplan
    /// originate 命令定义（见命令增强.md REQ-CCMD-001）。核心接受 → Ok(())；
    /// 参数非法/核心拒绝 → [`CoreError::CoreRejected`]。
    fn originate(
        &self,
        call_url: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `status` — 综合状态快照（L1，REQ-CCMD-001）。
    fn status(
        &self,
    ) -> impl Future<Output = Result<StatusInfo, CoreError>> + Send;

    // ===== REQ-OAM-009：计费管理只读 =====

    /// `show rates` — 列出所有费率条目（L0）。
    fn show_rates(
        &self,
    ) -> impl Future<Output = Result<RateList, CoreError>> + Send;

    /// `show balances` — 列出所有账户余额（L0）。
    fn show_balances(
        &self,
    ) -> impl Future<Output = Result<AccountList, CoreError>> + Send;

    // ===== REQ-OAM-010：媒体 bridge 活跃会话 =====

    /// `show transcode` — 活跃 bridge/relay 会话清单（L0）。
    fn show_transcode(
        &self,
    ) -> impl Future<Output = Result<BridgeList, CoreError>> + Send;

    // ===== REQ-OAM-009：计费写命令（rate reload + account open/topup）=====

    /// `rate.reload <json>` — 费率表全量替换（RCU 原子换指针）。
    fn reload_rates(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `rate.upsert <json>` — 单条费率条目 upsert（JSON 对象）。
    fn rate_upsert_json(
        &self,
        json: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `rate.delete <prefix>` — 删除指定前缀的费率条目。
    fn rate_delete(
        &self,
        prefix: &str,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `account.open <key> <balance_uc> <credit_limit_uc>` — 开账户。
    fn account_open(
        &self,
        key: &str,
        balance_uc: i64,
        credit_limit_uc: i64,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;

    /// `account.topup <key> <delta_uc>` — 账户充值（可为负=扣减）。
    fn account_topup(
        &self,
        key: &str,
        delta_uc: i64,
    ) -> impl Future<Output = Result<(), CoreError>> + Send;
}

/// 真实核心客户端：短连接（核心侧每命令独立帧），可廉价 Clone 共享配置。
#[derive(Clone, Debug)]
pub struct RealCore {
    sock_path: PathBuf,
    timeout: Duration,
}

impl RealCore {
    pub fn new(sock_path: PathBuf, timeout: Duration) -> Self {
        Self { sock_path, timeout }
    }

    /// 去掉 `unix:` 前缀 — config.rs 的 resolve_uds_path 会保留这个前缀，
    /// 但 tokio::net::UnixStream::connect 需要的是纯文件路径。
    fn to_path_buf(s: String) -> PathBuf {
        if let Some(rest) = s.strip_prefix("unix:") {
            PathBuf::from(rest)
        } else {
            PathBuf::from(s)
        }
    }

    /// 环境配置：`NSBC_CONTROL_SOCK`（默认 /var/run/nsbc/control.sock），
    /// `NSBC_CORE_TIMEOUT_MS`（默认 2000ms，REQ-WEB-014）。
    #[allow(dead_code)]
    pub fn from_env() -> Self {
        let sock_path = std::env::var("NSBC_CONTROL_SOCK")
            .map(Self::to_path_buf)
            .unwrap_or_else(|_| PathBuf::from(DEFAULT_SOCK_PATH));
        let timeout = std::env::var("NSBC_CORE_TIMEOUT_MS")
            .ok()
            .and_then(|v| v.parse::<u64>().ok())
            .map(Duration::from_millis)
            .unwrap_or_else(|| Duration::from_millis(DEFAULT_TIMEOUT_MS));
        Self::new(sock_path, timeout)
    }

    /// 从 WebConfig::CoreSection 构造（环境变量仍可覆盖 sock 路径）。
    /// cfg.control_sock 可能带 `unix:` 前缀 — 统一剥掉。
    pub fn from_config(cfg: &super::config::CoreSection) -> Self {
        let sock_path = std::env::var("NSBC_CONTROL_SOCK")
            .map(Self::to_path_buf)
            .unwrap_or_else(|_| {
                let raw = if cfg.control_sock.is_empty() {
                    DEFAULT_SOCK_PATH.to_string()
                } else {
                    cfg.control_sock.clone()
                };
                Self::to_path_buf(raw)
            });
        let timeout = std::env::var("NSBC_CORE_TIMEOUT_MS")
            .ok()
            .and_then(|v| v.parse::<u64>().ok())
            .map(Duration::from_millis)
            .unwrap_or_else(|| Duration::from_millis(DEFAULT_TIMEOUT_MS));
        Self::new(sock_path, timeout)
    }
}

impl CoreApi for RealCore {
    async fn version(&self) -> Result<VersionInfo, CoreError> {
        parse_version(&request(&self.sock_path, self.timeout, "version").await?)
    }
    async fn call_counters(&self) -> Result<CallCounters, CoreError> {
        parse_call_counters(&request(&self.sock_path, self.timeout, "show calls").await?)
    }
    async fn registrations(&self) -> Result<RegistrationSummary, CoreError> {
        parse_registrations(
            &request(&self.sock_path, self.timeout, "show registrations").await?,
        )
    }
    async fn metrics(&self) -> Result<MetricSnapshot, CoreError> {
        Ok(parse_metrics(
            &request(&self.sock_path, self.timeout, "stats").await?,
        ))
    }
    async fn list_calls(&self) -> Result<CallList, CoreError> {
        parse_call_list(
            &request(&self.sock_path, self.timeout, "show channels").await?,
        )
    }
    async fn call_detail(&self, call_id: &str) -> Result<CallDetail, CoreError> {
        // 同 hangup：notfound 要映射 404，用 send_cmd 不走统一 ERR 预判。
        let cmd = format!("uuid_dump {call_id}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_call_detail(&body)
    }
    async fn config_get(&self, key: &str) -> Result<ConfigValue, CoreError> {
        // 只读但需自分流 notfound→404（invalid key 由路由层先行拦截）。
        let cmd = format!("config.get {key}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_config_value(&body)
    }
    async fn config_export(&self) -> Result<ConfigExport, CoreError> {
        // 纯只读无 notfound 语义：ERR 统一交 request() 预判为 CoreRejected。
        parse_config_export(
            &request(&self.sock_path, self.timeout, "config.export").await?,
        )
    }
    async fn hangup(&self, call_id: &str) -> Result<(), CoreError> {
        // 命令字符串先构造为 owned，await 期不借用入参；call_id 合法性
        // （长度/字符集）由路由层与核心侧双重校验，传输层只负责忠实发送。
        let cmd = format!("hangup {call_id}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_hangup(&body)
    }
    async fn dialplan_apps(&self) -> Result<Vec<AppInfo>, CoreError> {
        // show apps → 头行 apps=N + 每行 "name  help"；未加载 dialplan 模块
        // 时输出 "(no app registry ...)"，parse_show_apps 跳过它返回空数组。
        let body = send_cmd(&self.sock_path, self.timeout, "show apps").await?;
        Ok(parse_show_apps(&body))
    }
    async fn show_codecs(&self) -> Result<Vec<CodecInfo>, CoreError> {
        // show codecs → 头行 codecs=N + 每行 "NAME PT SRATE IMPL FMTP MEDIA"；
        // 未加载 codec 模块时输出 "(no codec registry ...)"，parse_show_codecs
        // 跳过它返回空数组。C 侧 codec_registry 自注册后条目自动显示。
        let body = send_cmd(&self.sock_path, self.timeout, "show codecs").await?;
        Ok(parse_show_codecs(&body))
    }
    async fn config_set(&self, key: &str, value: &str) -> Result<ConfigSetResult, CoreError> {
        // split_kv 按第一个空格切分 key 与 value，值可含空格；
        // 空值（删除语义）用 `config.set <key> ` 结尾一个空格表示。
        let cmd = format!("config.set {key} {value}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_config_set(&body, key)
    }
    async fn config_validate(&self, key: &str, value: &str) -> Result<(), CoreError> {
        let cmd = format!("config.validate {key} {value}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_config_validate(&body)
    }
    async fn cdr_recent(&self, limit: u32) -> Result<Vec<CdrEvent>, CoreError> {
        let cmd = if limit > 0 { format!("cdr.recent {limit}") } else { "cdr.recent".into() };
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_cdr_recent(&body)
    }
    async fn security_ban_list(&self, filter: &str) -> Result<Vec<BanEntry>, CoreError> {
        let cmd = if filter.is_empty() {
            "security.ban.list".to_string()
        } else {
            format!("security.ban.list {filter}")
        };
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_ban_list(&body)
    }
    async fn security_ban_unban(&self, ip: &str) -> Result<(), CoreError> {
        let cmd = format!("security.ban.unban {ip}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        if body.trim_start().starts_with("OK") {
            Ok(())
        } else {
            // "ERR security.ban.unban ip=X not found" 等
            let msg = body.trim().strip_prefix("ERR").unwrap_or(body.trim());
            Err(CoreError::CoreRejected(msg.trim().to_string()))
        }
    }

    async fn reload_extensions(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("extensions.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_gateways(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("gateways.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_trunks(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("trunks.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_dialplan_rules(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("dialplan-rules.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_queues(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("queues.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_sip_profiles(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("profiles.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn reload_security_blacklist(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("security.blacklist.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }

    async fn call_action(
        &self,
        call_id: &str,
        action: &str,
        args: &[&str],
    ) -> Result<(), CoreError> {
        // 构造 `uuid_<action> <call_id> <args...>`
        let args_str = if args.is_empty() {
            String::new()
        } else {
            args.iter().cloned().collect::<Vec<_>>().join(" ")
        };
        let cmd = if args_str.is_empty() {
            format!("uuid_{action} {call_id}")
        } else {
            format!("uuid_{action} {call_id} {args_str}")
        };
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_call_action_ok_err(&body, call_id)
    }

    async fn send_message(&self, call_id: &str, body: &str) -> Result<(), CoreError> {
        // send_msg 命令格式：send_msg <call-id> <body>，body 可含空格，
        // 故直接拼接（call_id 已由路由层校验合法字符集）。
        let cmd = format!("send_msg {call_id} {body}");
        let resp = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_call_action_ok_err(&resp, call_id)
    }

    async fn originate(&self, call_url: &str) -> Result<(), CoreError> {
        let cmd = format!("originate {call_url}");
        let body = send_cmd(&self.sock_path, self.timeout, &cmd).await?;
        parse_call_action_ok_err(&body, call_url)
    }

    async fn status(&self) -> Result<StatusInfo, CoreError> {
        parse_status(&request(&self.sock_path, self.timeout, "status").await?)
    }

    async fn show_rates(&self) -> Result<RateList, CoreError> {
        let body = request(&self.sock_path, self.timeout, "show rates").await?;
        parse_show_rates(&body)
    }

    async fn show_balances(&self) -> Result<AccountList, CoreError> {
        let body = request(&self.sock_path, self.timeout, "show balances").await?;
        parse_show_balances(&body)
    }

    async fn show_transcode(&self) -> Result<BridgeList, CoreError> {
        let body = request(&self.sock_path, self.timeout, "show transcode").await?;
        parse_show_transcode(&body)
    }

    async fn reload_rates(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("rate.reload {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn rate_upsert_json(&self, json: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("rate.upsert {json}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn rate_delete(&self, prefix: &str) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("rate.delete {prefix}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn account_open(&self, key: &str, balance_uc: i64, credit_limit_uc: i64) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("account.open {key} {balance_uc} {credit_limit_uc}"))
            .await.and_then(|b| check_rejected(&b))
    }
    async fn account_topup(&self, key: &str, delta_uc: i64) -> Result<(), CoreError> {
        send_cmd(&self.sock_path, self.timeout, &format!("account.topup {key} {delta_uc}"))
            .await.and_then(|b| check_rejected(&b))
    }
}

/// 发送单条命令并返回提示符之前的响应文本（已 trim、已分类 ERR 行）。
async fn request(
    sock: &Path,
    timeout: Duration,
    cmd: &str,
) -> Result<String, CoreError> {
    let body = send_cmd(sock, timeout, cmd).await?;
    check_rejected(&body)?;
    Ok(body)
}

/// 仅取响应文本，不预判 ERR（写命令需要区分 ERR 语义，如 notfound→404）。
async fn send_cmd(
    sock: &Path,
    timeout: Duration,
    cmd: &str,
) -> Result<String, CoreError> {
    let buf = request_raw(sock, timeout, cmd).await?;
    let text = String::from_utf8(buf).map_err(|e| CoreError::Protocol(e.to_string()))?;
    Ok(text
        .split(str::from_utf8(PROMPT).unwrap_or("nsbc> "))
        .next()
        .unwrap_or("")
        .trim()
        .to_string())
}

/// UDS 传输（仅 unix 目标）：连接 → 排空连接横幅 → 发命令 → 读到下一提示符。
#[cfg(unix)]
async fn request_raw(
    sock: &Path,
    timeout: Duration,
    cmd: &str,
) -> Result<Vec<u8>, CoreError> {
    use tokio::io::AsyncWriteExt;
    use tokio::net::UnixStream;

    let mut stream = match tokio::time::timeout(timeout, UnixStream::connect(sock)).await {
        Ok(Ok(s)) => s,
        Ok(Err(e)) => return Err(CoreError::Connect(e.to_string())),
        Err(_) => return Err(CoreError::Timeout),
    };

    // 横幅以首个提示符结束，排空但不解析（版本不一致不影响后续命令）。
    let mut banner = Vec::new();
    read_until_prompt(&mut stream, timeout, &mut banner).await?;

    stream
        .write_all(cmd.as_bytes())
        .await
        .map_err(|e| CoreError::Io(e.to_string()))?;
    stream
        .write_all(b"\n")
        .await
        .map_err(|e| CoreError::Io(e.to_string()))?;

    let mut body = Vec::new();
    read_until_prompt(&mut stream, timeout, &mut body).await?;
    Ok(body)
}

#[cfg(unix)]
async fn read_until_prompt<R: tokio::io::AsyncRead + Unpin>(
    r: &mut R,
    timeout: Duration,
    buf: &mut Vec<u8>,
) -> Result<(), CoreError> {
    use tokio::io::AsyncReadExt;

    let mut chunk = [0u8; 1024];
    loop {
        let n = match tokio::time::timeout(timeout, r.read(&mut chunk)).await {
            Ok(Ok(0)) => return Err(CoreError::Protocol(
                "connection closed before prompt".into(),
            )),
            Ok(Ok(n)) => n,
            Ok(Err(e)) => return Err(CoreError::Io(e.to_string())),
            Err(_) => return Err(CoreError::Timeout),
        };
        buf.extend_from_slice(&chunk[..n]);
        if buf.windows(PROMPT.len()).any(|w| w == PROMPT) {
            return Ok(());
        }
        if buf.len() > MAX_FRAME {
            return Err(CoreError::Protocol("oversized control frame".into()));
        }
    }
}

/// 非 unix 目标（Windows 开发机）：传输不编译，显式 Unsupported，
/// 解析层与路由逻辑仍可在本机构建和测试。
#[cfg(not(unix))]
async fn request_raw(
    _sock: &Path,
    _timeout: Duration,
    _cmd: &str,
) -> Result<Vec<u8>, CoreError> {
    Err(CoreError::Unsupported(
        "core UDS control transport is only built on unix targets",
    ))
}

/// 核心 `ERR ...` 行 → [`CoreError::CoreRejected`]。
fn check_rejected(body: &str) -> Result<(), CoreError> {
    for line in body.lines() {
        let t = line.trim();
        if let Some(msg) = t.strip_prefix("ERR") {
            return Err(CoreError::CoreRejected(msg.trim().to_string()));
        }
    }
    Ok(())
}

/// 解析 `nsbcd <version>`。
pub fn parse_version(body: &str) -> Result<VersionInfo, CoreError> {
    let line = body.lines().next().unwrap_or("").trim();
    let mut it = line.split_whitespace();
    match (it.next(), it.next()) {
        (Some("nsbcd"), Some(v)) if !v.is_empty() => {
            Ok(VersionInfo { core: v.to_string() })
        }
        _ => Err(CoreError::Protocol(format!("bad version response: {body:?}"))),
    }
}

/// 从 `k=v k=v ...` 单行取一个 u64 键。
fn kv_u64(line: &str, key: &str) -> Option<u64> {
    line.split_whitespace()
        .filter_map(|tok| tok.split_once('='))
        .find(|(k, _)| *k == key)
        .and_then(|(_, v)| v.parse::<u64>().ok())
}

/// 解析 `active=N total=N failed=N`（键序不敏感，缺一即协议错误）。
pub fn parse_call_counters(body: &str) -> Result<CallCounters, CoreError> {
    let line = body.lines().next().unwrap_or("");
    match (
        kv_u64(line, "active"),
        kv_u64(line, "total"),
        kv_u64(line, "failed"),
    ) {
        (Some(active), Some(total), Some(failed)) => {
            Ok(CallCounters { active, total, failed })
        }
        _ => Err(CoreError::Protocol(format!(
            "bad call counters: {body:?}"
        ))),
    }
}

/// 解析 show registrations：每有效绑定一行 `reg k=v k=v ...`，
/// 尾行 `registrations=N truncated=T`（channels=N 同模式）。
pub fn parse_registrations(body: &str) -> Result<RegistrationSummary, CoreError> {
    let mut bindings = Vec::new();
    let mut total: u64 = 0;
    let mut truncated = false;
    for line in body.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("reg ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            bindings.push(BindingEntry {
                aor: kv_opt(toks.iter().copied(), "aor").unwrap_or_default(),
                user: kv_opt(toks.iter().copied(), "user").unwrap_or_default(),
                contact: kv_opt(toks.iter().copied(), "contact").unwrap_or_default(),
                expires_sec: kv_opt(toks.iter().copied(), "expires_sec")
                    .and_then(|v| v.parse::<i64>().ok()),
                alive_sec: kv_opt(toks.iter().copied(), "alive_sec")
                    .and_then(|v| v.parse::<i64>().ok()),
                q: kv_opt(toks.iter().copied(), "q")
                    .and_then(|v| v.parse::<i64>().ok())
                    .filter(|&v| v > 0),
                /* REQ-NAT-001：NAT 元数据解析。旧版 C 核心可能还没输出这些字段，
                 * kv_opt 返回 None → 各 String 字段 unwrap_or_default() 为空串，
                 * 整数字段 None。前端展示用 Optional 安全处理。 */
                nat_detected: kv_opt(toks.iter().copied(), "nat")
                    .and_then(|v| v.parse::<i64>().ok()),
                transport: kv_opt(toks.iter().copied(), "transport").unwrap_or_default(),
                received_ip: kv_opt(toks.iter().copied(), "received_ip")
                    .map(|s| if s == "-" { String::new() } else { s.to_string() })
                    .unwrap_or_default(),
                received_port: kv_opt(toks.iter().copied(), "received_port")
                    .and_then(|v| v.parse::<i64>().ok())
                    .filter(|&v| v > 0),
                public_contact: kv_opt(toks.iter().copied(), "pub_contact")
                    .unwrap_or_default(),
            });
        } else if let Some(rest) = line.strip_prefix("registrations=") {
            let mut rest = rest.split_whitespace();
            if let Some(n) = rest.next().and_then(|v| v.parse::<u64>().ok()) {
                total = n;
            }
            truncated = rest.find_map(|t| t.strip_prefix("truncated="))
                .map(|v| v == "1").unwrap_or(false);
        } else if line.is_empty() {
            continue;
        }
        // 兼容旧版只输出 registrations=N（无 reg 行）
        else if let Some(n) = kv_u64(line, "registrations") {
            total = n;
        }
    }
    Ok(RegistrationSummary { bindings, registrations: total, truncated })
}

/// 解析 `hangup <call-id>` 响应帧（C 侧 obs_control cmd_hangup）：
/// `OK hungup [call-id]` → Ok(())；`ERR notfound` → [`CoreError::NotFound`]；
/// 其余 `ERR ...` → [`CoreError::CoreRejected`]；不符契约 → Protocol。
pub fn parse_hangup(body: &str) -> Result<(), CoreError> {
    let line = body.lines().next().unwrap_or("").trim();
    if line == "OK hungup" || line.starts_with("OK hungup ") {
        Ok(())
    } else if line == "ERR notfound" {
        Err(CoreError::NotFound("call-id not found".into()))
    } else if let Some(msg) = line.strip_prefix("ERR") {
        Err(CoreError::CoreRejected(msg.trim().to_string()))
    } else {
        Err(CoreError::Protocol(format!("bad hangup response: {body:?}")))
    }
}

/// 解析 `config.set <k> <v>` / `config.validate <k> <v>` 响应（C 侧 obs_control
/// cmd_config_propose；见 src/oam/obs_control.c）：
///   `OK set <key>` / `OK unchanged <key>` → Ok(ConfigSetResult{changed})
///   `ERR ...` → CoreRejected
pub fn parse_config_set(body: &str, key: &str) -> Result<ConfigSetResult, CoreError> {
    let line = body.lines().next().unwrap_or("").trim();
    check_err_line(line)?;
    if let Some(rest) = line.strip_prefix("OK set ") {
        Ok(ConfigSetResult { key: rest.trim().to_string(), changed: true })
    } else if let Some(rest) = line.strip_prefix("OK unchanged ") {
        Ok(ConfigSetResult { key: rest.trim().to_string(), changed: false })
    } else {
        // 有些分支核心可能回 `OK set <key> <reason>`，先尝试宽松匹配
        if line.starts_with("OK set ") {
            let rest = line.strip_prefix("OK set ").unwrap_or("").trim();
            let reported_key = rest.split_whitespace().next().unwrap_or(key);
            Ok(ConfigSetResult { key: reported_key.to_string(), changed: true })
        } else if line.starts_with("OK unchanged ") {
            let rest = line.strip_prefix("OK unchanged ").unwrap_or("").trim();
            let reported_key = rest.split_whitespace().next().unwrap_or(key);
            Ok(ConfigSetResult { key: reported_key.to_string(), changed: false })
        } else {
            Err(CoreError::Protocol(format!("bad config.set response: {body:?}")))
        }
    }
}

/// 解析 `config.validate <k> <v>` 响应：
///   `OK` / `OK valid` → Ok(())
///   `ERR ...` → CoreRejected
pub fn parse_config_validate(body: &str) -> Result<(), CoreError> {
    let line = body.lines().next().unwrap_or("").trim();
    if line == "OK" || line.starts_with("OK ") { Ok(()) }
    else if let Some(msg) = line.strip_prefix("ERR") {
        Err(CoreError::CoreRejected(msg.trim().to_string()))
    } else {
        Err(CoreError::Protocol(format!("bad config.validate response: {body:?}")))
    }
}

/// 解析 `cdr.recent [N]` 响应帧（C 侧 obs_control cmd_cdr_recent，P1）：
/// 头行可能为 `cdrs=N`（与 show channels 的 `channels=N truncated=0` 类似）；
/// 每 CDR 一行，键值对：
///   `cdr call_id=X tenant=Y from=A to=B [trunk=T] [rec=R] dir=<in|out|int|unk>
///    phase=<start|answer|end> disp=<answered|noanswer|busy|failed|abandoned>
///    start_ms=U answer_ms=U end_ms=U billsec=U duration_s=U cause=N emergency=0|1`
/// 尾部可能有 `cdrs=N` 汇总锚行；无 CDR 时仅 `cdrs=0`。
pub fn parse_cdr_recent(body: &str) -> Result<Vec<CdrEvent>, CoreError> {
    let mut cdrs = Vec::new();
    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() { continue; }
        if let Some(msg) = line.strip_prefix("ERR") {
            return Err(CoreError::CoreRejected(msg.trim().to_string()));
        }
        if let Some(rest) = line.strip_prefix("cdr ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            cdrs.push(parse_cdr_line(&toks)?);
        }
        // `cdrs=N` 汇总行跳过（已逐行解析）
    }
    Ok(cdrs)
}

/// 单行解析（`cdr` 前缀已剥，剩余 `k=v k=v ...` token 序列）。
fn parse_cdr_line(toks: &[&str]) -> Result<CdrEvent, CoreError> {
    let kv = |k: &str| kv_opt(toks.iter().copied(), k);
    let kvi64 = |k: &str| kv_opt(toks.iter().copied(), k).and_then(|v| v.parse::<i32>().ok());
    let kvu64 = |k: &str| kv_opt(toks.iter().copied(), k).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
    let kvu32 = |k: &str| kv_opt(toks.iter().copied(), k).and_then(|v| v.parse::<u32>().ok()).unwrap_or(0);

    let call_id = kv("call_id").ok_or_else(|| CoreError::Protocol("cdr missing call_id".into()))?;
    let from_num = kv("from").ok_or_else(|| CoreError::Protocol("cdr missing from".into()))?;
    let to_num = kv("to").ok_or_else(|| CoreError::Protocol("cdr missing to".into()))?;
    let dir = kv("dir").unwrap_or_else(|| "unknown".into());
    let phase = kv("phase").unwrap_or_else(|| "end".into());
    let disp = kv("disp").unwrap_or_else(|| "failed".into());
    let emergency = kv_opt(toks.iter().copied(), "emergency")
        .map(|v| v == "1")
        .unwrap_or(false);

    Ok(CdrEvent {
        call_id,
        tenant: kv("tenant"),
        from_num,
        to_num,
        trunk: kv("trunk"),
        rec_path: kv("rec"),
        dir, phase, disposition: disp,
        start_ms: kvu64("start_ms"),
        answer_ms: kvu64("answer_ms"),
        end_ms: kvu64("end_ms"),
        billsec: kvu32("billsec"),
        duration_s: kvu32("duration_s"),
        cause: kvi64("cause").unwrap_or(0),
        emergency,
        // codec/IP 扩展字段：`cdr.recent` 单行输出不带这些，用 Default 给 None
        // UDS SESSION_END 完整 JSON 推送（write_cdr 链路）才会填
        ..Default::default()
    })
}

fn check_err_line(line: &str) -> Result<(), CoreError> {
    if let Some(msg) = line.strip_prefix("ERR") {
        Err(CoreError::CoreRejected(msg.trim().to_string()))
    } else { Ok(()) }
}

/// 通用 uuid_* 呼叫级写操作 / originate 命令响应解析（核心 cmd_registry
/// dispatch 输出）：
///   `OK <action> [<detail>]` → Ok(())
///   `ERR notfound` / `ERR call-id not found` → [`CoreError::NotFound`]
///   `ERR ...` → [`CoreError::CoreRejected`]
///   空响应 / 其他 → [`CoreError::Protocol`]
pub fn parse_call_action_ok_err(body: &str, target: &str) -> Result<(), CoreError> {
    let line = body.lines().next().unwrap_or("").trim();
    if line.starts_with("OK") {
        Ok(())
    } else if line == "ERR notfound" || line == "ERR call-id not found" {
        Err(CoreError::NotFound(target.to_string()))
    } else if let Some(msg) = line.strip_prefix("ERR") {
        Err(CoreError::CoreRejected(msg.trim().to_string()))
    } else if line.is_empty() {
        Err(CoreError::Protocol("empty response".into()))
    } else {
        Err(CoreError::Protocol(format!("bad response: {body:?}")))
    }
}

/// 解析 `status` 命令输出（REQ-CCMD-001 L1 只读）：
/// 首行键值对 `status active_calls=N total=N failed=N registrations=N
/// profiles=N started_ago_ms=U`（`status` 为命令行自身，核心输出不含命令名，
/// 直接从 `active_calls=` 开始）；`alive=1` 为末尾锚行。
/// 缺失字段默认 0（非必需），协议违例（空响应/无 alive 锚）返回 Protocol。
pub fn parse_status(body: &str) -> Result<StatusInfo, CoreError> {
    let mut active_calls = 0u64;
    let mut total_calls = 0u64;
    let mut failed_calls = 0u64;
    let mut registrations = 0u64;
    let mut profiles = 0u64;
    let mut started_ago_ms = 0u64;
    let mut saw_alive = false;

    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() { continue; }
        if line.starts_with("ERR") {
            return Err(CoreError::CoreRejected(line.strip_prefix("ERR").unwrap().trim().to_string()));
        }
        if let Some(rest) = line.strip_prefix("alive=") {
            saw_alive = rest.starts_with('1');
            continue;
        }
        // 键值对行：可能以 "status " 开头（命令 echo-back），剥离后统一处理
        let stripped = line.strip_prefix("status ").unwrap_or(line);
        for tok in stripped.split_whitespace() {
            if let Some((k, v)) = tok.split_once('=') {
                let parse_u = |s: &str| s.parse::<u64>().unwrap_or(0);
                match k {
                    "active_calls" => active_calls = parse_u(v),
                    "total_calls" => total_calls = parse_u(v),
                    "failed_calls" => failed_calls = parse_u(v),
                    "registrations" => registrations = parse_u(v),
                    "profiles" => profiles = parse_u(v),
                    "started_ago_ms" => started_ago_ms = parse_u(v),
                    _ => {} // 未知键忽略，协议宽松
                }
            }
        }
    }
    if !saw_alive {
        return Err(CoreError::Protocol(format!("status frame missing alive=1: {body:?}")));
    }
    Ok(StatusInfo {
        active_calls, total_calls, failed_calls, registrations,
        profiles, started_ago_ms,
    })
}

/// 解析 `show rates` 帧（REQ-OAM-009）：
/// 头行 `rates=N` + 每费率一行 `rate prefix=X name=Y price_per_min_uc=N
/// connect_fee_uc=F bill_inc_s=I min_bill_s=M initial_s=S enabled=B`。
pub fn parse_show_rates(body: &str) -> Result<RateList, CoreError> {
    let mut rates = Vec::new();
    let mut total = 0i64;
    let mut saw_head = false;
    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() { continue; }
        if let Some(msg) = line.strip_prefix("ERR") {
            return Err(CoreError::CoreRejected(msg.trim().to_string()));
        }
        if let Some(rest) = line.strip_prefix("rates=") {
            total = rest.trim().parse::<i64>().unwrap_or(0);
            saw_head = true;
        } else if let Some(rest) = line.strip_prefix("rate ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            let prefix = kv_opt(toks.iter().copied(), "prefix")
                .ok_or_else(|| CoreError::Protocol("rate missing prefix".into()))?;
            rates.push(RateEntry {
                prefix,
                name: kv_opt(toks.iter().copied(), "name").unwrap_or_default(),
                price_per_min_uc: kv_opt(toks.iter().copied(), "price_per_min_uc")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                connect_fee_uc: kv_opt(toks.iter().copied(), "connect_fee_uc")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                bill_inc_s: kv_opt(toks.iter().copied(), "bill_inc_s")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                min_bill_s: kv_opt(toks.iter().copied(), "min_bill_s")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                initial_s: kv_opt(toks.iter().copied(), "initial_s")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                enabled: kv_opt(toks.iter().copied(), "enabled")
                    .map(|v| v == "1").unwrap_or(false),
            });
        }
    }
    if !saw_head {
        return Err(CoreError::Protocol(format!("missing rates header: {body:?}")));
    }
    Ok(RateList { rates, total })
}

/// 解析 `show balances` 帧：头行 `accounts=N` + 每行 `account key=K
/// balance_uc=B reserved_uc=R credit_limit_uc=C`。
pub fn parse_show_balances(body: &str) -> Result<AccountList, CoreError> {
    let mut accounts = Vec::new();
    let mut total = 0i64;
    let mut saw_head = false;
    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() { continue; }
        if let Some(msg) = line.strip_prefix("ERR") {
            return Err(CoreError::CoreRejected(msg.trim().to_string()));
        }
        if let Some(rest) = line.strip_prefix("accounts=") {
            total = rest.trim().parse::<i64>().unwrap_or(0);
            saw_head = true;
        } else if let Some(rest) = line.strip_prefix("account ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            let key = kv_opt(toks.iter().copied(), "key")
                .ok_or_else(|| CoreError::Protocol("account missing key".into()))?;
            accounts.push(AccountBalance {
                key,
                balance_uc: kv_opt(toks.iter().copied(), "balance_uc")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                reserved_uc: kv_opt(toks.iter().copied(), "reserved_uc")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                credit_limit_uc: kv_opt(toks.iter().copied(), "credit_limit_uc")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
            });
        }
    }
    if !saw_head {
        return Err(CoreError::Protocol(format!("missing accounts header: {body:?}")));
    }
    Ok(AccountList { accounts, total })
}

/// 解析 `show transcode` 帧：头行 `sessions=N` + 每行 `bridge idx=I
/// call_key=P mode=M a_rtp=A b_rtp=B pkts=N drops=D`。
pub fn parse_show_transcode(body: &str) -> Result<BridgeList, CoreError> {
    let mut sessions = Vec::new();
    let mut total = 0i64;
    let mut saw_head = false;
    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() { continue; }
        if let Some(msg) = line.strip_prefix("ERR") {
            return Err(CoreError::CoreRejected(msg.trim().to_string()));
        }
        if let Some(rest) = line.strip_prefix("sessions=") {
            total = rest.trim().parse::<i64>().unwrap_or(0);
            saw_head = true;
        } else if let Some(rest) = line.strip_prefix("bridge ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            sessions.push(BridgeSession {
                idx: kv_opt(toks.iter().copied(), "idx")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                call_key_ptr: kv_opt(toks.iter().copied(), "call_key")
                    .unwrap_or_default(),
                mode: kv_opt(toks.iter().copied(), "mode").unwrap_or_default(),
                a_rtp: kv_opt(toks.iter().copied(), "a_rtp")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                b_rtp: kv_opt(toks.iter().copied(), "b_rtp")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                pkts: kv_opt(toks.iter().copied(), "pkts")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
                drops: kv_opt(toks.iter().copied(), "drops")
                    .and_then(|v| v.parse().ok()).unwrap_or(0),
            });
        }
    }
    if !saw_head {
        return Err(CoreError::Protocol(format!("missing sessions header: {body:?}")));
    }
    Ok(BridgeList { sessions, total })
}

/// 从 `k=v k=v ...` token 序列取一个字符串键；"-" 与缺失均视为 None。
fn kv_opt<'a, I, S>(toks: I, key: &str) -> Option<String>
where
    I: IntoIterator<Item = &'a S>,
    S: AsRef<str> + ?Sized + 'a,
{
    toks.into_iter()
        .filter_map(|t| t.as_ref().split_once('='))
        .find(|(k, _)| *k == key)
        .map(|(_, v)| v)
        .filter(|v| !v.is_empty() && *v != "-")
        .map(str::to_string)
}

/// 解析 `show channels` 帧：
/// `call a_callid=X a_state=S b_callid=Y b_state=S`（每呼叫一行，
/// "-" → None）+ 尾行 `channels=N truncated=0|1`（协议锚点，缺一即错）。
pub fn parse_call_list(body: &str) -> Result<CallList, CoreError> {
    let mut calls = Vec::new();
    let mut summary = false;
    let mut truncated = false;
    for line in body.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("call ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            let start_ms = kv_opt(toks.iter().copied(), "start_ms")
                .and_then(|v| v.parse::<u64>().ok());
            let answer_ms = kv_opt(toks.iter().copied(), "answer_ms")
                .and_then(|v| if v == "-" || v == "0" { None } else { v.parse::<u64>().ok() });
            let duration_ms = kv_opt(toks.iter().copied(), "duration_ms")
                .and_then(|v| v.parse::<u64>().ok());
            calls.push(CallSummary {
                a_call_id: kv_opt(toks.iter().copied(), "a_callid").filter(|v| v != "-"),
                a_state: kv_opt(toks.iter().copied(), "a_state").filter(|v| v != "-"),
                b_call_id: kv_opt(toks.iter().copied(), "b_callid").filter(|v| v != "-"),
                b_state: kv_opt(toks.iter().copied(), "b_state").filter(|v| v != "-"),
                caller: kv_opt(toks.iter().copied(), "caller").filter(|v| v != "-"),
                callee: kv_opt(toks.iter().copied(), "callee").filter(|v| v != "-"),
                start_ms,
                answer_ms,
                duration_ms,
                codec_a: kv_opt(toks.iter().copied(), "codec_a").filter(|v| v != "-"),
                codec_b: kv_opt(toks.iter().copied(), "codec_b").filter(|v| v != "-"),
                rtp_loss_pct: kv_opt(toks.iter().copied(), "rtp_loss_pct")
                    .and_then(|v| if v == "-" || v == "0" { None } else { v.parse::<u32>().ok() }),
                rtp_jitter_ms: kv_opt(toks.iter().copied(), "rtp_jitter_ms")
                    .and_then(|v| if v == "-" || v == "0" { None } else { v.parse::<u32>().ok() }),
                mos_x1000: kv_opt(toks.iter().copied(), "mos_x1000")
                    .and_then(|v| if v == "-" || v == "0" { None } else { v.parse::<i32>().ok() })
                    .filter(|&v| v >= 0),
            });
        } else if let Some(rest) = line.strip_prefix("channels=") {
            // channels=N truncated=T（同一尾行两个 k=v）
            let mut toks = rest.split_whitespace();
            if toks.next().and_then(|v| v.parse::<u64>().ok()).is_none() {
                return Err(CoreError::Protocol(format!("bad channels: {body:?}")));
            }
            truncated = toks
                .find_map(|t| t.strip_prefix("truncated="))
                .map(|v| v == "1")
                .unwrap_or(false);
            summary = true;
        }
    }
    if !summary {
        return Err(CoreError::Protocol(format!("missing channels summary: {body:?}")));
    }
    Ok(CallList { calls, truncated })
}

/// 解析 `uuid_dump <call-id>` 帧（长度分帧，SDP 可含任意换行）：
/// 头行 `call a_callid=.. a_state=.. b_callid=.. b_state=..`；
/// 每 Leg：state/tags/media 三组键值行 + 可选 `leg N sdp_offer|sdp_answer <len>`
/// 后紧跟 len 字节原文。媒体 UNAVAILABLE → None；notfound/ERR 显式分类。
pub fn parse_call_detail(body: &str) -> Result<CallDetail, CoreError> {
    let line0 = body.lines().next().unwrap_or("").trim();
    if line0 == "ERR notfound" {
        return Err(CoreError::NotFound("call-id not found".into()));
    }
    if let Some(msg) = line0.strip_prefix("ERR") {
        return Err(CoreError::CoreRejected(msg.trim().to_string()));
    }
    let bytes = body.as_bytes();
    let mut pos = 0usize;
    let mut head: Option<(Option<String>, Option<String>, Option<String>,
                          Option<String>)> = None;
    let mut legs = std::collections::BTreeMap::<u8, LegDump>::new();
    let mut saw_leg = false;

    while pos < bytes.len() {
        let nl = bytes[pos..]
            .iter()
            .position(|&x| x == b'\n')
            .map(|rel| pos + rel)
            .unwrap_or(bytes.len());
        let line = std::str::from_utf8(&bytes[pos..nl])
            .map_err(|_| CoreError::Protocol("non-utf8 dump header".into()))?
            .trim_end_matches('\r');
        let after_nl = if nl < bytes.len() { nl + 1 } else { bytes.len() };

        if let Some(rest) = line.strip_prefix("call ") {
            let toks: Vec<&str> = rest.split_whitespace().collect();
            head = Some((
                kv_opt(toks.iter().copied(), "a_callid"),
                kv_opt(toks.iter().copied(), "a_state"),
                kv_opt(toks.iter().copied(), "b_callid"),
                kv_opt(toks.iter().copied(), "b_state"),
            ));
            pos = after_nl;
        } else if let Some(rest) = line.strip_prefix("leg ") {
            // 行式：
            //   N state=.. proto=.. reliable=B cseq=K routes=R（无指令词）
            //   N tags local=.. remote=..
            //   N media mode=.. gate=.. ssrc_in=.. ssrc_out=.. vq=..
            //   N sdp_offer|sdp_answer <len>
            let mut it = rest.split_whitespace();
            let n: u8 = it
                .next()
                .and_then(|s| s.parse().ok())
                .ok_or_else(|| CoreError::Protocol(format!("bad leg line: {line:?}")))?;
            let key = it.next().unwrap_or("");
            let rest_toks: Vec<&str> = it.collect();
            // 状态行首 token 即 state=X；其余行首 token 为指令词。
            if key.starts_with("state=") {
                saw_leg = true;
                let mut toks = vec![key];
                toks.extend(rest_toks);
                {
                    let get = |k: &str| kv_opt(toks.iter().copied(), k);
                    let state = get("state")
                        .ok_or_else(|| CoreError::Protocol(format!("no state: {line:?}")))?;
                    let reliable = toks
                        .iter()
                        .find_map(|t| t.strip_prefix("reliable="))
                        .is_some_and(|v| v == "1");
                    let cseq = toks
                        .iter()
                        .find_map(|t| t.strip_prefix("cseq="))
                        .and_then(|v| v.parse::<i64>().ok())
                        .ok_or_else(|| CoreError::Protocol(format!("bad cseq: {line:?}")))?;
                    let routes = toks
                        .iter()
                        .find_map(|t| t.strip_prefix("routes="))
                        .and_then(|v| v.parse::<u32>().ok())
                        .unwrap_or(0);
                    legs.entry(n).or_insert_with(|| LegDump {
                        leg: n, state: String::new(), proto: String::new(),
                        reliable: false, cseq: 0, routes: 0,
                        local_tag: None, remote_tag: None,
                        offer: None, answer: None, media: LegMedia::default(),
                    });
                    let leg = legs.get_mut(&n).unwrap();
                    leg.state = state;
                    leg.proto = get("proto").unwrap_or_else(|| "?".into());
                    leg.reliable = reliable;
                    leg.cseq = cseq;
                    leg.routes = routes;
                }
            } else if key == "tags" {
                let leg = legs.entry(n).or_insert_with(|| LegDump {
                    leg: n, state: "?".into(), proto: "?".into(),
                    reliable: false, cseq: 0, routes: 0,
                    local_tag: None, remote_tag: None,
                    offer: None, answer: None, media: LegMedia::default(),
                });
                leg.local_tag = kv_opt(rest_toks.iter().copied(), "local");
                leg.remote_tag = kv_opt(rest_toks.iter().copied(), "remote");
            } else if key == "media" {
                // UNAVAILABLE = 核心无采集来源（P0 媒体面/ bypass Leg）→ null，
                // 严禁映射为任何估算值。
                let val = |k: &str| {
                    kv_opt(rest_toks.iter().copied(), k)
                        .filter(|v| v != "UNAVAILABLE")
                };
                let leg = legs.entry(n).or_insert_with(|| LegDump {
                    leg: n, state: "?".into(), proto: "?".into(),
                    reliable: false, cseq: 0, routes: 0,
                    local_tag: None, remote_tag: None,
                    offer: None, answer: None, media: LegMedia::default(),
                });
                leg.media = LegMedia {
                    mode: val("mode"),
                    gate: val("gate"),
                    ssrc_in: val("ssrc_in"),
                    ssrc_out: val("ssrc_out"),
                    vq: val("vq"),
                };
            } else if key == "sdp_offer" || key == "sdp_answer" {
                let len: usize = rest_toks
                    .first()
                    .and_then(|v| v.parse().ok())
                    .ok_or_else(|| CoreError::Protocol(format!("bad sdp len: {line:?}")))?;
                let end = after_nl.checked_add(len).ok_or_else(|| {
                    CoreError::Protocol("sdp length overflow".into())
                })?;
                if end > bytes.len() {
                    return Err(CoreError::Protocol(format!(
                        "truncated sdp section: want {len} bytes")));
                }
                let raw = std::str::from_utf8(&bytes[after_nl..end])
                    .map_err(|_| CoreError::Protocol("non-utf8 sdp".into()))?
                    .to_string();
                let leg = legs.get_mut(&n).ok_or_else(|| {
                    CoreError::Protocol("sdp before leg state line".into())
                })?;
                if key == "sdp_offer" {
                    leg.offer = Some(raw);
                } else {
                    leg.answer = Some(raw);
                }
                // 核心在 SDP 原文后固定补一个 '\n' 分隔符（SDP 自身
                // 末尾的 CRLF 计入 len，不属于分隔符）。
                pos = if end < bytes.len() && bytes[end] == b'\n' {
                    end + 1
                } else {
                    end
                };
                continue;
            } else {
                return Err(CoreError::Protocol(format!("bad leg key: {line:?}")));
            }
            pos = after_nl;
        } else {
            pos = after_nl; // 空行/未知行忽略
        }
    }

    let (a_call_id, a_state, b_call_id, b_state) = head.ok_or_else(|| {
        CoreError::Protocol(format!("missing call header: {body:?}"))
    })?;
    if !saw_leg {
        return Err(CoreError::Protocol(format!("no leg sections: {body:?}")));
    }
    Ok(CallDetail {
        a_call_id,
        a_state,
        b_call_id,
        b_state,
        legs: legs.into_values().collect(),
    })
}

/// 解析 `config.get <key>` 帧（两行）：
/// `config key=<k> layer=<name> secret=<0|1>` + 值原文一行。
/// 敏感键 secret=1：value=None（核心 **** 脱敏，占位串不向上泄漏）。
/// 注：传输层对帧整体 trim，标量空值无法分帧（声明键均有默认值，不预期）。
pub fn parse_config_value(body: &str) -> Result<ConfigValue, CoreError> {
    let line0 = body.lines().next().unwrap_or("").trim_end();
    if line0 == "ERR notfound" {
        return Err(CoreError::NotFound("config key not found".into()));
    }
    if let Some(msg) = line0.strip_prefix("ERR") {
        return Err(CoreError::CoreRejected(msg.trim().to_string()));
    }
    let rest = line0
        .strip_prefix("config ")
        .ok_or_else(|| CoreError::Protocol(format!("bad config frame: {body:?}")))?;
    let toks: Vec<&str> = rest.split_whitespace().collect();
    let key = kv_opt(toks.iter().copied(), "key")
        .ok_or_else(|| CoreError::Protocol(format!("missing key: {body:?}")))?;
    let layer = kv_opt(toks.iter().copied(), "layer")
        .ok_or_else(|| CoreError::Protocol(format!("missing layer: {body:?}")))?;
    let secret = toks
        .iter()
        .find_map(|t| t.strip_prefix("secret="))
        .is_some_and(|v| v == "1");
    let raw = body
        .lines()
        .nth(1)
        .ok_or_else(|| CoreError::Protocol(format!("missing value line: {body:?}")))?;
    Ok(ConfigValue {
        key,
        value: if secret { None } else { Some(raw.to_string()) },
        layer,
        secret,
    })
}

/// 解析 `config.export` 帧：首行 `config-export keys=N redact=1`，
/// 其后每键一行 `<dotted.key>: <value|****>`（以首个 ':' 切分；值可含
/// 空格/':'；空值行 `key:` 合法，值为空串）。`#` 注释行与空行跳过；
/// **** 归 None/secret=true；声明键数与实际行数不符按协议违例处理。
pub fn parse_config_export(body: &str) -> Result<ConfigExport, CoreError> {
    let mut lines = body.lines();
    let head = lines.next().unwrap_or("").trim_end();
    let rest = head
        .strip_prefix("config-export ")
        .ok_or_else(|| CoreError::Protocol(format!("bad export frame: {body:?}")))?;
    let toks: Vec<&str> = rest.split_whitespace().collect();
    let declared = toks
        .iter()
        .find_map(|t| t.strip_prefix("keys="))
        .and_then(|v| v.parse::<usize>().ok())
        .ok_or_else(|| CoreError::Protocol(format!("missing keys: {body:?}")))?;
    let redacted = toks
        .iter()
        .find_map(|t| t.strip_prefix("redact="))
        .is_some_and(|v| v == "1");
    let mut items = Vec::with_capacity(declared);
    for line in lines {
        let line = line.trim_end();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        /* 键字符集为 [A-Za-z0-9._-]，不含 ':'，故首个 ':' 即分隔符。
         * 不能要求 ": "：空值行（如 ha.peer:）经 trim_end 后无尾空格。 */
        let ci = line
            .find(':')
            .ok_or_else(|| CoreError::Protocol(format!("bad export line: {line:?}")))?;
        let key = line[..ci].trim_end();
        let val = line[ci + 1..].trim_start();
        if key.is_empty() {
            return Err(CoreError::Protocol(format!("empty export key: {body:?}")));
        }
        let secret = val == "****";
        items.push(ConfigExportItem {
            key: key.to_string(),
            value: if secret { None } else { Some(val.to_string()) },
            secret,
        });
    }
    if items.len() != declared {
        return Err(CoreError::Protocol(format!(
            "export keys mismatch: header={declared} parsed={}",
            items.len()
        )));
    }
    Ok(ConfigExport { redacted, items })
}

/// 解析 Prometheus exposition 为 `name → value`；跳过注释/空行/非法采样。
pub fn parse_metrics(body: &str) -> MetricSnapshot {
    let mut out = BTreeMap::new();
    for line in body.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut it = line.split_whitespace();
        let (Some(name), Some(value), None) = (it.next(), it.next(), it.next()) else {
            continue;
        };
        let name = name.split('{').next().unwrap_or(name);
        if let Ok(v) = value.parse::<f64>() {
            out.insert(name.to_string(), v);
        }
    }
    MetricSnapshot(out)
}

#[cfg(test)]
pub(crate) use tests::MockCore;

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};

    // ---- 纯解析层：覆盖成功 / 缺键 / 非法 / ERR / 注释 / label ----

    #[test]
    fn version_ok() {
        assert_eq!(
            parse_version("nsbcd 2.5.5\n").unwrap(),
            VersionInfo { core: "2.5.5".into() }
        );
    }

    #[test]
    fn version_bad() {
        assert!(parse_version("").is_err());
        assert!(parse_version("weird 2.5.5\n").is_err());
        assert!(parse_version("nsbcd\n").is_err());
    }

    #[test]
    fn counters_ok_and_order_insensitive() {
        assert_eq!(
            parse_call_counters("active=3 total=10 failed=2\n").unwrap(),
            CallCounters { active: 3, total: 10, failed: 2 }
        );
        assert_eq!(
            parse_call_counters("failed=1 total=4 active=0\r\n").unwrap(),
            CallCounters { active: 0, total: 4, failed: 1 }
        );
    }

    #[test]
    fn counters_missing_or_bad() {
        assert!(parse_call_counters("active=1 total=2\n").is_err());
        assert!(parse_call_counters("active=x total=2 failed=1\n").is_err());
        assert!(parse_call_counters("").is_err());
    }

    #[test]
    fn registrations_ok_bad() {
        assert_eq!(
            parse_registrations("registrations=42\n").unwrap(),
            RegistrationSummary { registrations: 42 }
        );
        assert!(parse_registrations("registrations=\n").is_err());
        assert!(parse_registrations("\n").is_err());
    }

    #[test]
    fn metrics_exposition() {
        let text = "\
# HELP nsbc_up 1 if the NSBC core process is up
# TYPE nsbc_up gauge
nsbc_up 1
# HELP nsbc_calls_active active calls
# TYPE nsbc_calls_active gauge
nsbc_calls_active 7
nsbc_rtp_ports_in_use 12
junk line without value
nsbc_bad notanumber
";
        let m = parse_metrics(text);
        assert_eq!(m.0.get("nsbc_up"), Some(&1.0));
        assert_eq!(m.0.get("nsbc_calls_active"), Some(&7.0));
        assert_eq!(m.0.get("nsbc_rtp_ports_in_use"), Some(&12.0));
        assert_eq!(m.0.len(), 3);
    }

    #[test]
    fn metrics_labeled_sample_strips_braces() {
        let m = parse_metrics("nsbc_x{dir=\"in\"} 1.5\nnsbc_x{dir=\"out\"} 2.5\n");
        // 同名 last-wins
        assert_eq!(m.0.get("nsbc_x"), Some(&2.5));
    }

    #[test]
    fn err_lines_classified() {
        assert!(check_rejected("ERR unknown command: foo (try 'help')").is_err());
        assert!(check_rejected("ERR oom").is_err());
        assert!(check_rejected("OK reload dispatched\n").is_ok());
        assert!(check_rejected("active=1 total=2 failed=0").is_ok());
    }

    #[test]
    fn hangup_responses() {
        assert!(parse_hangup("OK hungup\n").is_ok());
        assert!(parse_hangup("OK hungup call-1@host\n").is_ok());
        assert!(matches!(
            parse_hangup("ERR notfound\n"),
            Err(CoreError::NotFound(_))
        ));
        assert!(matches!(
            parse_hangup("ERR invalid call-id\n"),
            Err(CoreError::CoreRejected(_))
        ));
        assert!(matches!(
            parse_hangup("weird\n"),
            Err(CoreError::Protocol(_))
        ));
    }

    #[test]
    fn config_set_responses() {
        let r = parse_config_set("OK set sip.bind_addr\n", "sip.bind_addr").unwrap();
        assert_eq!(r.key, "sip.bind_addr");
        assert!(r.changed);
        let r = parse_config_set("OK unchanged sip.port\n", "sip.port").unwrap();
        assert!(!r.changed);
        assert!(matches!(
            parse_config_set("ERR invalid key\n", "bad key"),
            Err(CoreError::CoreRejected(_))
        ));
        assert!(matches!(
            parse_config_set("weird\n", "k"),
            Err(CoreError::Protocol(_))
        ));
    }

    #[test]
    fn config_validate_responses() {
        assert!(parse_config_validate("OK\n").is_ok());
        assert!(parse_config_validate("OK valid\n").is_ok());
        assert!(matches!(
            parse_config_validate("ERR value out of range\n"),
            Err(CoreError::CoreRejected(_))
        ));
    }

    #[test]
    fn cdr_recent_parses_full_frame() {
        // 单行 CDR（避免 Rust 字符串字面值中 \n 造成的视觉混乱）。
        let body = "\
cdrs=1\ncdr call_id=call-1@host tenant=acme from=alice@A to=bob@B trunk=out rec=/var/rec/call1.wav dir=out phase=end disp=answered start_ms=1700000000000 answer_ms=1700000005000 end_ms=1700000070000 billsec=65 duration_s=70 cause=16 emergency=0\ncdrs=1\n";
        let cdrs = parse_cdr_recent(body).unwrap();
        assert_eq!(cdrs.len(), 1);
        let c = &cdrs[0];
        assert_eq!(c.call_id, "call-1@host");
        assert_eq!(c.dir, "out");
        assert_eq!(c.phase, "end");
        assert_eq!(c.disposition, "answered");
        assert_eq!(c.billsec, 65);
        assert_eq!(c.cause, 16);
        assert_eq!(c.trunk.as_deref(), Some("out"));
        assert_eq!(c.emergency, false);
    }

    #[test]
    fn cdr_recent_empty_set_and_unknown_command() {
        assert_eq!(parse_cdr_recent("cdrs=0\n").unwrap().len(), 0);
        assert_eq!(parse_cdr_recent("cdrs=5\n").unwrap().len(), 0);
        assert!(matches!(
            parse_cdr_recent("ERR unknown command: cdr.recent\n"),
            Err(CoreError::CoreRejected(_))
        ));
    }

    #[test]
    fn cdr_recent_minimal_line_sets_defaults() {
        let body = "cdr call_id=call-x from=a to=b\ncdrs=1\n";
        let cdrs = parse_cdr_recent(body).unwrap();
        let c = &cdrs[0];
        assert_eq!(c.dir, "unknown");
        assert_eq!(c.phase, "end");
        assert_eq!(c.disposition, "failed");
        assert_eq!(c.tenant, None);
        assert_eq!(c.start_ms, 0);
        assert_eq!(c.emergency, false);
    }

    #[test]
    fn call_list_ok_dash_and_truncated() {
        let body = "\
call a_callid=a@h a_state=CONFIRMED b_callid=b@h b_state=CONFIRMED
call a_callid=c@h a_state=EARLY b_callid=- b_state=-
channels=2 truncated=0
";
        let l = parse_call_list(body).unwrap();
        assert!(!l.truncated);
        assert_eq!(l.calls.len(), 2);
        assert_eq!(l.calls[0].a_call_id.as_deref(), Some("a@h"));
        assert_eq!(l.calls[0].b_state.as_deref(), Some("CONFIRMED"));
        assert_eq!(l.calls[1].b_call_id, None);
        assert_eq!(l.calls[1].b_state, None);
        let l2 = parse_call_list("channels=0 truncated=1\n").unwrap();
        assert!(l2.truncated);
        assert!(l2.calls.is_empty());
    }

    #[test]
    fn call_list_missing_summary_is_protocol() {
        assert!(parse_call_list("call a_callid=x a_state=IDLE b_callid=- b_state=-\n")
            .is_err());
        assert!(parse_call_list("channels=x truncated=0\n").is_err());
    }

    #[test]
    fn call_detail_full_frame_with_framed_sdp() {
        // offer 内含多行（含伪造 "leg 1" 行），长度分帧必须原样保留
        let offer = "v=0\r\no=- 1 1 IN IP4 1.2.3.4\r\nleg 1 state=FORGED\r\n";
        let body = format!(
            "call a_callid=a@h a_state=CONFIRMED b_callid=b@h b_state=EARLY\n\
leg 0 state=CONFIRMED proto=udp reliable=0 cseq=21 routes=0\n\
leg 0 tags local=lt0 remote=rt0\n\
leg 0 media mode=UNAVAILABLE gate=UNAVAILABLE ssrc_in=UNAVAILABLE ssrc_out=UNAVAILABLE vq=UNAVAILABLE\n\
leg 0 sdp_offer {ol}\n{offer}\n\
leg 1 state=EARLY proto=tcp reliable=1 cseq=3 routes=2\n\
leg 1 tags local=- remote=rt1\n\
leg 1 media mode=UNAVAILABLE gate=UNAVAILABLE ssrc_in=UNAVAILABLE ssrc_out=UNAVAILABLE vq=UNAVAILABLE\n",
            ol = offer.len(),
            offer = offer
        );
        let d = parse_call_detail(&body).unwrap();
        assert_eq!(d.a_call_id.as_deref(), Some("a@h"));
        assert_eq!(d.b_state.as_deref(), Some("EARLY"));
        assert_eq!(d.legs.len(), 2);
        let l0 = &d.legs[0];
        assert_eq!(l0.leg, 0);
        assert_eq!(l0.state, "CONFIRMED");
        assert_eq!(l0.proto, "udp");
        assert!(!l0.reliable);
        assert_eq!(l0.cseq, 21);
        assert_eq!(l0.local_tag.as_deref(), Some("lt0"));
        assert!(l0.offer.as_deref().unwrap().contains("state=FORGED"));
        assert_eq!(l0.offer.as_deref(), Some(offer));
        assert_eq!(l0.answer, None);
        assert_eq!(l0.media.mode, None); // UNAVAILABLE → None
        let l1 = &d.legs[1];
        assert!(l1.reliable);
        assert_eq!(l1.routes, 2);
        assert_eq!(l1.local_tag, None);
        assert_eq!(l1.remote_tag.as_deref(), Some("rt1"));
    }

    #[test]
    fn call_detail_errors() {
        assert!(matches!(
            parse_call_detail("ERR notfound\n"),
            Err(CoreError::NotFound(_))
        ));
        assert!(matches!(
            parse_call_detail("ERR invalid call-id\n"),
            Err(CoreError::CoreRejected(_))
        ));
        assert!(parse_call_detail("weird\n").is_err()); // 无 call 头/leg
        // SDP 段截断（声明长度超出帧）
        let bad = "call a_callid=a a_state=IDLE b_callid=- b_state=-\n\
leg 0 state=IDLE proto=udp reliable=0 cseq=1 routes=0\n\
leg 0 sdp_offer 50\nabc\n";
        assert!(parse_call_detail(bad).is_err());
    }

    #[test]
    fn config_value_plain_and_secret() {
        let v = parse_config_value(
            "config key=log.level layer=system secret=0\n6\n",
        )
        .unwrap();
        assert_eq!(v.key, "log.level");
        assert_eq!(v.value.as_deref(), Some("6"));
        assert_eq!(v.layer, "system");
        assert!(!v.secret);
        // 值含空白也按第二行原文保留（不做空白分词）
        let v2 = parse_config_value(
            "config key=log.path layer=default secret=0\n/var/log/nsbc/x.log\n",
        )
        .unwrap();
        assert_eq!(v2.value.as_deref(), Some("/var/log/nsbc/x.log"));
        // 敏感键：secret=1 → value=None（**** 占位不外泄）
        let s = parse_config_value(
            "config key=db.password layer=system secret=1\n****\n",
        )
        .unwrap();
        assert!(s.secret);
        assert_eq!(s.value, None);
        assert_eq!(s.layer, "system");
    }

    #[test]
    fn config_value_errors() {
        assert!(matches!(
            parse_config_value("ERR notfound\n"),
            Err(CoreError::NotFound(_))
        ));
        assert!(matches!(
            parse_config_value("ERR invalid key\n"),
            Err(CoreError::CoreRejected(_))
        ));
        assert!(parse_config_value("weird\n").is_err()); // 无 config 头
        assert!(parse_config_value("config key=k layer=system secret=0\n").is_err());
        // 缺值行
        assert!(parse_config_value(
            "config layer=system secret=0\n6\n"
        )
        .is_err()); // 缺 key
    }

    #[test]
    fn config_export_plain_secret_and_spaces() {
        let body = "config-export keys=3 redact=1\n\
                    log.level: 6\n\
                    sip.user_agent: NSBC/2.5 extra\n\
                    db.dsn: ****\n";
        let e = parse_config_export(body).unwrap();
        assert!(e.redacted);
        assert_eq!(e.items.len(), 3);
        assert_eq!(e.items[0].key, "log.level");
        assert_eq!(e.items[0].value.as_deref(), Some("6"));
        assert!(!e.items[0].secret);
        // 值内空格/": " 取首个分隔符之后原文
        assert_eq!(e.items[1].value.as_deref(), Some("NSBC/2.5 extra"));
        // 敏感键脱敏：null + secret=true，**** 不出边车
        assert_eq!(e.items[2].key, "db.dsn");
        assert_eq!(e.items[2].value, None);
        assert!(e.items[2].secret);
        // 防御性跳过注释/空行（核心当前不发，解析器容忍）
        let with_comment = "config-export keys=1 redact=1\n# note\n\nlog.level: 6\n";
        let e2 = parse_config_export(with_comment).unwrap();
        assert_eq!(e2.items.len(), 1);
        // 空值行（真实核心 ha.peer: 无尾空格）必须解析为空串而非协议错误
        let empty_val = "config-export keys=2 redact=1\nha.enabled: false\nha.peer:\n";
        let e3 = parse_config_export(empty_val).unwrap();
        assert_eq!(e3.items.len(), 2);
        assert_eq!(e3.items[1].key, "ha.peer");
        assert_eq!(e3.items[1].value.as_deref(), Some(""));
        assert!(!e3.items[1].secret);
    }

    #[test]
    fn config_export_errors() {
        // 键数不符（声明 2 实际 1）
        assert!(parse_config_export("config-export keys=2 redact=1\nlog.level: 6\n").is_err());
        // 坏帧头 / 缺 keys / 非数字
        assert!(parse_config_export("weird\n").is_err());
        assert!(parse_config_export("config-export redact=1\nlog.level: 6\n").is_err());
        assert!(parse_config_export("config-export keys=x redact=1\n").is_err());
        // 值行无 ": " 分隔
        assert!(parse_config_export("config-export keys=1 redact=1\nlog.level=6\n").is_err());
        // ERR 帧（真实链路经 request() 已预判为 CoreRejected，解析器同样拒绝）
        assert!(parse_config_export("ERR export failed\n").is_err());
    }

    // ---- MockCore：路由层测试用，记录命令、回放罐装响应 ----

    #[derive(Clone, Default)]
    pub(crate) struct MockCore {
        pub(crate) calls: Arc<Mutex<Vec<String>>>,
        pub(crate) canned: Arc<BTreeMap<String, Result<String, CoreError>>>,
    }

    impl MockCore {
        fn record(&self, cmd: &str) -> Result<String, CoreError> {
            self.calls.lock().unwrap().push(cmd.to_string());
            self.canned
                .get(cmd)
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned command".into())))
        }
    }

    impl CoreApi for MockCore {
        async fn version(&self) -> Result<VersionInfo, CoreError> {
            parse_version(&self.record("version")?)
        }
        async fn call_counters(&self) -> Result<CallCounters, CoreError> {
            parse_call_counters(&self.record("show calls")?)
        }
        async fn registrations(&self) -> Result<RegistrationSummary, CoreError> {
            parse_registrations(&self.record("show registrations")?)
        }
        async fn metrics(&self) -> Result<MetricSnapshot, CoreError> {
            Ok(parse_metrics(&self.record("stats")?))
        }
        async fn list_calls(&self) -> Result<CallList, CoreError> {
            parse_call_list(&self.record("show channels")?)
        }
        async fn call_detail(
            &self,
            call_id: &str,
        ) -> Result<CallDetail, CoreError> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("uuid_dump {call_id}"));
            // 罐装键 "uuid_dump:<id>"，缺失即 Protocol（测试必须显式罐装）。
            let key = format!("uuid_dump:{call_id}");
            let canned = self
                .canned
                .get(&key)
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned uuid_dump".into())));
            parse_call_detail(&canned?)
        }
        async fn config_get(&self, key: &str) -> Result<ConfigValue, CoreError> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("config.get {key}"));
            let ckey = format!("config.get:{key}");
            let canned = self
                .canned
                .get(&ckey)
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned config.get".into())));
            parse_config_value(&canned?)
        }
        async fn config_export(&self) -> Result<ConfigExport, CoreError> {
            parse_config_export(&self.record("config.export")?)
        }
        async fn hangup(&self, call_id: &str) -> Result<(), CoreError> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("hangup {call_id}"));
            // 罐装键：优先 "hangup:<id>"（区分成功/404），退回通用 "hangup"。
            let key = format!("hangup:{call_id}");
            let canned = self
                .canned
                .get(&key)
                .or_else(|| self.canned.get("hangup"))
                .cloned()
                .unwrap_or_else(|| {
                    Err(CoreError::Protocol("uncanned hangup".into()))
                });
            parse_hangup(&canned?)
        }
        async fn dialplan_apps(&self) -> Result<Vec<AppInfo>, CoreError> {
            // 罐装 "show apps" 返回；默认空数组（dialplan 模块未加载）
            let body = self
                .record("show apps")
                .unwrap_or_else(|_| "apps=0\n".to_string());
            Ok(parse_show_apps(&body))
        }
        async fn show_codecs(&self) -> Result<Vec<CodecInfo>, CoreError> {
            // 罐装 "show codecs" 返回；默认空数组（codec 模块未加载）
            let body = self
                .record("show codecs")
                .unwrap_or_else(|_| "codecs=0\n".to_string());
            Ok(parse_show_codecs(&body))
        }
        async fn config_set(&self, key: &str, value: &str) -> Result<ConfigSetResult, CoreError> {
            self.calls.lock().unwrap().push(format!("config.set {key} {value}"));
            let ckey = format!("config.set:{key}");
            let canned = self
                .canned
                .get(&ckey)
                .or_else(|| self.canned.get("config.set"))
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned config.set".into())));
            parse_config_set(&canned?, key)
        }
        async fn config_validate(&self, key: &str, value: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("config.validate {key} {value}"));
            let ckey = format!("config.validate:{key}");
            let canned = self
                .canned
                .get(&ckey)
                .or_else(|| self.canned.get("config.validate"))
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned config.validate".into())));
            parse_config_validate(&canned?)
        }
        async fn cdr_recent(&self, limit: u32) -> Result<Vec<CdrEvent>, CoreError> {
            self.calls.lock().unwrap().push(format!("cdr.recent {limit}"));
            let canned = self
                .canned
                .get("cdr.recent")
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned cdr.recent".into())));
            parse_cdr_recent(&canned?)
        }
        async fn security_ban_list(&self, filter: &str) -> Result<Vec<BanEntry>, CoreError> {
            self.calls.lock().unwrap().push(format!("security.ban.list {filter}"));
            let canned = self
                .canned
                .get("security.ban.list")
                .cloned()
                .unwrap_or_else(|| "bans=0 shown=0 total=0\n".to_string());
            parse_ban_list(&canned)
        }
        async fn security_ban_unban(&self, ip: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("security.ban.unban {ip}"));
            let canned = self
                .canned
                .get("security.ban.unban")
                .cloned()
                .unwrap_or_else(|| format!("OK security.ban.unban ip={ip} removed=1\n"));
            if canned.trim_start().starts_with("OK") {
                Ok(())
            } else {
                let msg = canned.trim().strip_prefix("ERR").unwrap_or(canned.trim());
                Err(CoreError::CoreRejected(msg.trim().to_string()))
            }
        }

        async fn reload_extensions(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("extensions.reload {json}"));
            self.canned
                .get("extensions.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_gateways(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("gateways.reload {json}"));
            self.canned
                .get("gateways.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_trunks(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("trunks.reload {json}"));
            self.canned
                .get("trunks.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_dialplan_rules(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("dialplan-rules.reload {json}"));
            self.canned
                .get("dialplan-rules.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_queues(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("queues.reload {json}"));
            self.canned
                .get("queues.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_sip_profiles(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("profiles.reload {json}"));
            self.canned
                .get("profiles.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn reload_security_blacklist(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("security.blacklist.reload {json}"));
            self.canned
                .get("security.blacklist.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }

        async fn call_action(
            &self,
            call_id: &str,
            action: &str,
            args: &[&str],
        ) -> Result<(), CoreError> {
            let args_str = args.iter().cloned().collect::<Vec<_>>().join(" ");
            let cmd = if args_str.is_empty() {
                format!("uuid_{action} {call_id}")
            } else {
                format!("uuid_{action} {call_id} {args_str}")
            };
            self.calls.lock().unwrap().push(cmd.clone());
            // 罐装键优先 "uuid_<action>:<id>" → 退回 "uuid_<action>"
            let key_specific = format!("uuid_{action}:{call_id}");
            let canned = self
                .canned
                .get(&key_specific)
                .or_else(|| self.canned.get(&format!("uuid_{action}")))
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol(format!("uncanned {cmd}"))));
            parse_call_action_ok_err(&canned?, call_id)
        }

        async fn send_message(&self, call_id: &str, body: &str) -> Result<(), CoreError> {
            let cmd = format!("send_msg {call_id} {body}");
            self.calls.lock().unwrap().push(cmd.clone());
            let key = format!("send_msg:{call_id}");
            let canned = self
                .canned
                .get(&key)
                .or_else(|| self.canned.get("send_msg"))
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol(format!("uncanned send_msg {call_id}"))));
            parse_call_action_ok_err(&canned?, call_id)
        }

        async fn originate(&self, call_url: &str) -> Result<(), CoreError> {
            let cmd = format!("originate {call_url}");
            self.calls.lock().unwrap().push(cmd.clone());
            let canned = self
                .canned
                .get("originate")
                .cloned()
                .unwrap_or_else(|| Err(CoreError::Protocol("uncanned originate".into())));
            parse_call_action_ok_err(&canned?, call_url)
        }

        async fn status(&self) -> Result<StatusInfo, CoreError> {
            parse_status(&self.record("status")?)
        }

        async fn show_rates(&self) -> Result<RateList, CoreError> {
            parse_show_rates(&self.record("show rates")?)
        }

        async fn show_balances(&self) -> Result<AccountList, CoreError> {
            parse_show_balances(&self.record("show balances")?)
        }

        async fn show_transcode(&self) -> Result<BridgeList, CoreError> {
            parse_show_transcode(&self.record("show transcode")?)
        }

        async fn reload_rates(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("rate.reload {json}"));
            self.canned
                .get("rate.reload")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn rate_upsert_json(&self, json: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("rate.upsert {json}"));
            self.canned
                .get("rate.upsert")
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn rate_delete(&self, prefix: &str) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("rate.delete {prefix}"));
            let key = format!("rate.delete:{prefix}");
            self.canned
                .get(&key)
                .or_else(|| self.canned.get("rate.delete"))
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn account_open(&self, key: &str, balance_uc: i64, credit_limit_uc: i64) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("account.open {key} {balance_uc} {credit_limit_uc}"));
            let ckey = format!("account.open:{key}");
            self.canned
                .get(&ckey)
                .or_else(|| self.canned.get("account.open"))
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
        async fn account_topup(&self, key: &str, delta_uc: i64) -> Result<(), CoreError> {
            self.calls.lock().unwrap().push(format!("account.topup {key} {delta_uc}"));
            let ckey = format!("account.topup:{key}");
            self.canned
                .get(&ckey)
                .or_else(|| self.canned.get("account.topup"))
                .cloned()
                .unwrap_or(Ok("OK\n".into()))
                .and_then(|b| check_rejected(&b))
        }
    }

    /// 非 unix 目标：RealCore 必须显式 Unsupported 而非 panic（Windows 可测）。
    #[cfg(not(unix))]
    #[tokio::test]
    async fn real_core_unsupported_on_windows() {
        let c = RealCore::new(PathBuf::from(DEFAULT_SOCK_PATH), Duration::from_secs(1));
        assert!(matches!(
            c.call_counters().await,
            Err(CoreError::Unsupported(_))
        ));
    }
}
