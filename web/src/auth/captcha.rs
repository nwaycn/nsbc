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
            max_active: 1024,
            ttl: Duration::from_secs(300), // 5 minutes
        }
    }

    /// Generate new captcha -> (captcha_id, svg_string).
    /// Returns None when store is still over cap after cleaning expired items.
    pub fn generate(&self) -> Option<(String, String)> {
        let mut map = self.lock();
        // First clean expired items
        let now = Instant::now();
        map.retain(|_, v| v.1 > now);

        // Active count over cap -> still full after cleanup -> reject
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

/// Render simple SVG captcha (4 chars, with distortion + interference lines + noise points)
/// viewBox 96x36; zero external image dependencies.
fn render_svg(text: &str) -> String {
    let width = 96usize;
    let height = 36usize;
    let chars: Vec<char> = text.chars().collect();
    let n = chars.len();
    let step = width.saturating_sub(16) / n.max(1);

    // Per-char random offset + color
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

    // 2 random interference lines
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

    // 6 noise points
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
        assert!(svg.contains("<text"));
        assert!(svg.contains("<line"));
        assert!(svg.contains("<circle"));
        // Chars should appear between <text> tags
        assert!(svg.contains(">A<") || svg.contains("'A'>") || svg.contains(">A</text"));
    }
}
