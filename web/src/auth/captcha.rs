//! 登录验证码（REQ-WEB-009 增强）。
//!
//! 零新依赖：用现有 `getrandom` 生成随机字符，SVG 字符串模板渲染，
//! 内存 `Mutex<HashMap>` 存 captcha_id → (text, expiry)，TTL 5 分钟。
//!
//! 安全设计：
//! - captcha_id 为 32 hex（128 bit 随机），不可预测
//! - 答案 text 用 4 字符大写字母+数字（易读）
//! - 一次消费：校验后立即删除
//! - TTL 过期自动清理（懒执行，每次生成时顺便清过期项）
//! - 每进程最多 1024 个活跃验证码（防内存 DoS）

use std::collections::HashMap;
use std::sync::{Mutex, MutexGuard};
use std::time::{Duration, Instant};

use getrandom::getrandom;

/// 验证码存储：captcha_id → (答案文本, 过期时间)
pub struct CaptchaStore {
    inner: Mutex<HashMap<String, (String, Instant)>>,
    /// 活跃验证码上限（防内存 DoS）
    max_active: usize,
    /// 单个验证码 TTL
    ttl: Duration,
}

impl CaptchaStore {
    pub fn new() -> Self {
        Self {
            inner: Mutex::new(HashMap::new()),
            max_active: 1024,
            ttl: Duration::from_secs(300), // 5 分钟
        }
    }

    /// 生成新验证码 → (captcha_id, svg_string)。
    /// 内存满时清理过期项后仍超上限则返回 None。
    pub fn generate(&self) -> Option<(String, String)> {
        let mut map = self.lock();
        // 先清理过期项
        let now = Instant::now();
        map.retain(|_, v| v.1 > now);

        // 活跃数超限 → 清理过期后仍满 → 拒绝
        if map.len() >= self.max_active {
            map.retain(|_, v| v.1 > now);
            if map.len() >= self.max_active {
                tracing::warn!("captcha store full, rejecting");
                return None;
            }
        }

        let captcha_id = random_hex(16); // 32 hex chars
        let text = random_text(4);       // 4 chars
        let svg = render_svg(&text);
        let expiry = now + self.ttl;
        map.insert(captcha_id.clone(), (text.clone(), expiry));
        Some((captcha_id, svg))
    }

    /// 校验并消费验证码。正确 → true，错误/过期/不存在 → false。
    pub fn verify(&self, captcha_id: &str, captcha_code: &str) -> bool {
        let mut map = self.lock();
        let now = Instant::now();
        map.retain(|_, v| v.1 > now);

        let code_upper = captcha_code.trim().to_ascii_uppercase();
        match map.remove(captcha_id) {
            Some((stored, _)) => stored == code_upper,
            None => false,
        }
    }

    fn lock(&self) -> MutexGuard<'_, HashMap<String, (String, Instant)>> {
        self.inner.lock().expect("captcha store poisoned")
    }
}

impl Default for CaptchaStore {
    fn default() -> Self {
        Self::new()
    }
}

/// 生成指定长度的随机 hex 字符串
fn random_hex(len: usize) -> String {
    let mut buf = vec![0u8; len];
    getrandom(&mut buf).expect("getrandom failed");
    let hex: Vec<String> = buf.iter().map(|b| format!("{:02x}", b)).collect();
    hex.join("")
}

/// 生成指定长度的随机大写字母+数字（去掉易混淆字符 O/0/I/1/L）
const ALPHABET: &[u8] = b"ABCDEFGHJKMNPQRSTUVWXYZ23456789";

fn random_text(len: usize) -> String {
    let mut buf = vec![0u8; len];
    getrandom(&mut buf).expect("getrandom failed");
    buf.iter()
        .map(|b| ALPHABET[(*b as usize) % ALPHABET.len()] as char)
        .collect()
}

/// 渲染简单 SVG 验证码（4 字符，扭曲+干扰线+噪点）
/// viewBox 96×36；零外部图像依赖。
fn render_svg(text: &str) -> String {
    let width = 96usize;
    let height = 36usize;
    let chars: Vec<char> = text.chars().collect();
    let n = chars.len();
    let step = width.saturating_sub(16) / n.max(1);

    // 每字符随机偏移+颜色
    let mut char_exprs = String::new();
    let color_palette = ["#2c5f8a", "#3d7a9e", "#1f4d6e", "#4a6b8c"];
    for (i, &c) in chars.iter().enumerate() {
        let x = 11 + i * step;
        let y_off: i32 = random_range(-3, 3);
        let rot: i32 = random_range(-12, 12);
        let color = color_palette[(i * 7 + 3) % color_palette.len()];
        char_exprs.push_str(&format!(
            "<text x='{x}' y='{y}' font-family='monospace' font-size='22' font-weight='bold' fill='{color}' transform='rotate({rot},{cx},{cy})'>{c}</text>",
            x = x,
            y = 25i32 + y_off,
            rot = rot,
            cx = x + 6,
            cy = 18,
            c = c,
            color = color,
        ));
    }

    // 2 条随机干扰线
    let mut lines = String::new();
    for _ in 0..2 {
        let x1 = random_range(4, width as i32 - 16);
        let y1 = random_range(4, height as i32 - 4);
        let x2 = random_range(x1 + 12, width as i32 - 4);
        let y2 = random_range(4, height as i32 - 4);
        lines.push_str(&format!(
            "<line x1='{x1}' y1='{y1}' x2='{x2}' y2='{y2}' stroke='#9aaabb' stroke-width='1'/>"
        ));
    }

    // 6 个噪点
    let mut dots = String::new();
    for _ in 0..6 {
        let cx = random_range(0, width as i32);
        let cy = random_range(0, height as i32);
        dots.push_str(&format!(
            "<circle cx='{cx}' cy='{cy}' r='1' fill='#cccccc'/>"
        ));
    }

    format!(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {width} {height}' width='{width}' height='{height}'>\
<rect width='100%' height='100%' fill='#f6f8fa'/>\
{lines}{dots}{char_exprs}</svg>",
        width = width,
        height = height,
    )
}

fn random_range(min: i32, max: i32) -> i32 {
    let mut buf = [0u8; 4];
    getrandom(&mut buf).expect("getrandom failed");
    let v = u32::from_le_bytes(buf);
    let range = (max - min).max(1) as u32;
    min as i32 + (v % range) as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_generate_and_verify() {
        let store = CaptchaStore::new();
        let (id, svg) = store.generate().expect("generate");
        assert!(!id.is_empty());
        assert!(svg.contains("<svg"));
        assert!(svg.contains("<text"));

        // 不存在的 captcha_id → false
        assert!(!store.verify("unknown", "ABCD"));
    }

    #[test]
    fn test_random_text_length() {
        let t = random_text(4);
        assert_eq!(t.len(), 4);
        for c in t.chars() {
            assert!(c.is_ascii_uppercase() || c.is_ascii_digit());
        }
    }

    #[test]
    fn test_random_hex_length() {
        let h = random_hex(16);
        assert_eq!(h.len(), 32);
        assert!(h.chars().all(|c| c.is_ascii_hexdigit()));
    }

    #[test]
    fn test_svg_renders() {
        let svg = render_svg("ABCD");
        assert!(svg.contains("<svg"));
        assert!(svg.contains("xmlns"));
        assert!(svg.contains("<text"));
        assert!(svg.contains("<line"));
        assert!(svg.contains("<circle"));
        // 字符应该出现在 <text> 标签之间
        assert!(svg.contains(">A<") || svg.contains("'A'>") || svg.contains(">A</text"));
    }
}
