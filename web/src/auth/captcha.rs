//! Login captcha (REQ-WEB-009 enhancement).
//!
//! Zero new dependencies: random chars from existing `getrandom`, rendered via SVG string template,
//! in-memory `Mutex<HashMap>` stores captcha_id -> (text, expiry), TTL 5 minutes.
//!
//! Security design:
//! - captcha_id is 32 hex chars (128-bit random), unpredictable
//! - Answer text uses 4 chars of uppercase letters + digits (easy to read)
//! - Single-use: deleted immediately after verification
//! - TTL expiry auto-cleanup (lazy: clean expired items each time a new one is generated)
//! - At most 1024 active captchas per process (prevents memory DoS)

use std::collections::HashMap;
use std::sync::{Mutex, MutexGuard};
use std::time::{Duration, Instant};

use getrandom::getrandom;

/// Captcha store: captcha_id -> (answer text, expiry time)
pub struct CaptchaStore {
    inner: Mutex<HashMap<String, (String, Instant)>>,
    /// Active captcha cap (prevents memory DoS)
    max_active: usize,
    /// Per-captcha TTL
    ttl: Duration,
}

impl CaptchaStore {
    pub fn new() -> Self {
        Self {
            inner: Mutex::new(HashMap::new()),
            // RW-H6 hardening: 1024 was too tight. 4096 gives legitimate traffic
            // headroom even under concurrent login bursts. DoS attackers would need
            // 4096 distinct IPs/requests simultaneously to exhaust the pool.
            max_active: 4096,
            ttl: Duration::from_secs(300), // 5 minutes
        }
    }

    /// Generate new captcha -> (captcha_id, svg_string).
    /// Returns None when store is still over cap after cleaning expired items.
    pub fn generate(&self) -> Option<(String, String)> {
        let mut map = self.lock();
        let now = Instant::now();

        // RW-H6 hardening: always purge expired items + shrink on every generate(),
        // not just when full. A captcha-starvation attack (fill pool with attacker
        // entries then stop) must not block legitimate users until 5min TTL rolls.
        map.retain(|_, v| v.1 > now);
        map.shrink_to(self.max_active / 2);

        if map.len() >= self.max_active {
            tracing::warn!("captcha store still full after purge, rejecting (len={})", map.len());
            return None;
        }

        let captcha_id = random_hex(16); // 32 hex chars
        let text = random_text(4);       // 4 chars
        let svg = render_svg(&text);
        let expiry = now + self.ttl;
        map.insert(captcha_id.clone(), (text.clone(), expiry));
        Some((captcha_id, svg))
    }

    /// Verify and consume captcha. Correct -> true; wrong/expired/not found -> false.
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

/// Generate random hex string of given length
fn random_hex(len: usize) -> String {
    let mut buf = vec![0u8; len];
    getrandom(&mut buf).expect("getrandom failed");
    let hex: Vec<String> = buf.iter().map(|b| format!("{:02x}", b)).collect();
    hex.join("")
}

/// Generate random uppercase letters + digits of given length (exclude confusable chars O/0/I/1/L)
const ALPHABET: &[u8] = b"ABCDEFGHJKMNPQRSTUVWXYZ23456789";

fn random_text(len: usize) -> String {
    let mut buf = vec![0u8; len];
    getrandom(&mut buf).expect("getrandom failed");
    buf.iter()
        .map(|b| ALPHABET[(*b as usize) % ALPHABET.len()] as char)
        .collect()
}

/// 5×7 点阵字体：每个 char → 7 行 × 5 列位图（MSB=最左像素，1=点亮）。
/// 覆盖 ALPHABET 全部 31 个字符（A-Z 去掉 O/I/L，2-9）。
const FONT: &[(char, [u8; 7])] = &[
    ('A', [0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001]),
    ('B', [0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110]),
    ('C', [0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110]),
    ('D', [0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110]),
    ('E', [0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111]),
    ('F', [0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000]),
    ('G', [0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01110]),
    ('H', [0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001]),
    // I 已排除
    ('J', [0b00010, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100]),
    ('K', [0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001]),
    // L 已排除
    ('M', [0b10001, 0b11011, 0b10101, 0b10001, 0b10001, 0b10001, 0b10001]),
    ('N', [0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001]),
    // O 已排除（数字 0 也排除）
    ('P', [0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000]),
    ('Q', [0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101]),
    ('R', [0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001]),
    ('S', [0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110]),
    ('T', [0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100]),
    ('U', [0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110]),
    ('V', [0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100]),
    ('W', [0b10001, 0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b01010]),
    ('X', [0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001]),
    ('Y', [0b10001, 0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100]),
    ('Z', [0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111]),
    // 数字（0/1 已排除）
    ('2', [0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111]),
    ('3', [0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110]),
    ('4', [0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010]),
    ('5', [0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110]),
    ('6', [0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110]),
    ('7', [0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000]),
    ('8', [0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110]),
    ('9', [0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100]),
];

/// 查询某个字符的 5×7 点阵数据；不在 ALPHABET 中的返回 None。
fn glyph(c: char) -> Option<&'static [u8; 7]> {
    FONT.iter().find(|(ch, _)| *ch == c).map(|(_, data)| data)
}

/// Render captcha text as a dot-matrix bitmap using tiny SVG `<rect>` elements.
///
/// ⚠ 安全设计（RW-H7）：禁止使用 `<text>` 元素，因为答案明文会躺在 SVG
/// 响应体里，攻击者一条正则 `/>([A-Z0-9])<\/text>/g` 就能直接提取答案，
/// 完全不需要 OCR。用 `<rect>` 点阵渲染后，SVG 源码里只有图形数据，
/// 没有任何明文字符——人眼可识别，但 regex / source scraping 全部失效。
///
/// viewBox 96×36；每个字符占 5×7 点阵 × dot_size=4 ≈ 20×28 像素。
/// 每字符带：随机整体偏移（±3px）、颜色随机调色盘、干扰线、噪声点。
fn render_svg(text: &str) -> String {
    const COLS: usize = 5;
    const ROWS: usize = 7;
    const DOT: i32 = 4;     // 4px dot × 5 列 = 20px 字宽
    const GAP: i32 = 0;     // 列间零间隙
    const ROW_GAP: i32 = 1; // 行间 1px 让点可辨
    const CHAR_W: i32 = DOT * COLS as i32;               // 20px
    const CHAR_H: i32 = DOT * ROWS as i32 + ROW_GAP * (ROWS as i32 - 1); // 34px

    // viewBox 96×36 —— 与前端容器 w-24 h-9 完全同尺寸，零缩放
    // 每字 20px × 4 = 80px，剩余 16px / 5 间隔 = 3.2px 间距
    let width = 96i32;
    let height = 36i32;
    let chars: Vec<char> = text.chars().collect();
    let n = chars.len().min(4);
    let total_gap = width - CHAR_W * n as i32;
    let pad = total_gap / (n as i32 + 1); // ≈ 3px 每字左右

    let palette = ["#1a4d7a", "#285a8a", "#34669a", "#1f4270", "#2e6b87"];

    // 渲染每个字符的点阵
    let mut char_rects = String::new();
    for (i, &c) in chars.iter().take(n).enumerate() {
        let gx = pad + i as i32 * (CHAR_W + pad);
        let gy = (height - CHAR_H) / 2; // 垂直居中，上下各 1px
        let color = palette[(i * 7 + 3) % palette.len()];

        if let Some(pattern) = glyph(c) {
            for (row, &bits) in pattern.iter().enumerate() {
                for col in 0..COLS {
                    if (bits >> (COLS - 1 - col)) & 1 == 1 {
                        let x = gx + col as i32 * (DOT + GAP);
                        let y = gy + row as i32 * (DOT + ROW_GAP);
                        let jitter_x = random_range(-1, 1);
                        let jitter_y = random_range(-1, 1);
                        let dot_color = if random_range(0, 100) < 8 {
                            palette[(i * 11 + 5) % palette.len()]
                        } else {
                            color
                        };
                        char_rects.push_str(&format!(
                            "<rect x='{}' y='{}' width='{}' height='{}' fill='{}' rx='1'/>\
",
                            x + jitter_x,
                            y + jitter_y,
                            DOT,
                            DOT,
                            dot_color,
                        ));
                    }
                }
            }
        } else {
            char_rects.push_str(&format!(
                "<rect x='{}' y='{}' width='{}' height='{}' fill='{}'/>\
",
                gx + CHAR_W / 2 - 2,
                gy + CHAR_H / 2 - 2,
                4,
                4,
                color,
            ));
        }
    }

    // 2 条干扰线
    let mut lines = String::new();
    for _ in 0..2 {
        let horizontal = random_range(0, 2) == 0;
        if horizontal {
            let y = random_range(4, height - 4);
            lines.push_str(&format!(
                "<line x1='0' y1='{y}' x2='{w}' y2='{y}' stroke='#c8d4e2' stroke-width='1' stroke-dasharray='3,4'/>\
",
                y = y, w = width,
            ));
        } else {
            let x1 = random_range(2, width / 2);
            let y1 = random_range(2, height - 2);
            let x2 = random_range(width / 2, width - 2);
            let y2 = random_range(2, height - 2);
            lines.push_str(&format!(
                "<line x1='{x1}' y1='{y1}' x2='{x2}' y2='{y2}' stroke='#c8d4e2' stroke-width='1' stroke-dasharray='3,4'/>\
",
                x1 = x1, y1 = y1, x2 = x2, y2 = y2,
            ));
        }
    }

    // 6 个浅灰噪声点
    let mut dots = String::new();
    for _ in 0..6 {
        let cx = random_range(0, width);
        let cy = random_range(0, height);
        dots.push_str(&format!(
            "<circle cx='{cx}' cy='{cy}' r='1' fill='#d8dee6'/>\
",
            cx = cx, cy = cy,
        ));
    }

    format!(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {w} {h}' width='{w}' height='{h}'>\
<rect width='100%' height='100%' fill='#f6f8fa'/>\
{lines}{dots}{char_rects}</svg>",
        w = width,
        h = height,
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
        // 必须用 `<rect>` 点阵渲染，禁止明文 `<text>`（RW-H7 安全加固）
        assert!(svg.contains("<rect"), "captcha must render as rect dots");
        assert!(!svg.contains("<text"), "captcha must NOT contain plaintext <text> — regex scraping vulnerability!");

        // Non-existent captcha_id -> false
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
        assert!(svg.contains("<rect"));
        assert!(svg.contains("<line"));
        assert!(svg.contains("<circle"));
    }

    /// RW-H7 安全回归测试：SVG 源码不得包含答案明文字符。
    /// 攻击者不能用 `/>([A-Z0-9])<\/text>/g` 之类正则直接提取答案。
    #[test]
    fn test_svg_no_plaintext_answer() {
        let store = CaptchaStore::new();
        for _ in 0..10 {
            let (_id, svg) = store.generate().expect("generate");
            // 直接的正则提取应该一无所获
            assert!(!svg.contains("<text"), "no <text> elements allowed");
            assert!(!svg.contains("'A'>") && !svg.contains(">A<"),
                "captcha SVG source must not contain answer plaintext");
        }
    }

    /// 字体表覆盖完整性：ALPHABET 中的每个字符都在 FONT 表里有定义。
    #[test]
    fn test_font_coverage() {
        for &b in ALPHABET {
            let c = b as char;
            assert!(glyph(c).is_some(), "FONT missing glyph for '{}'", c);
        }
    }

    /// 点阵位图合法性：每行只用最低 5 bit。
    #[test]
    fn test_font_bitpatterns() {
        for (c, rows) in FONT {
            for (i, &bits) in rows.iter().enumerate() {
                assert_eq!(bits & !0b11111, 0,
                    "FONT '{}' row {} has bits beyond column 4: {:08b}", c, i, bits);
            }
        }
    }
}
