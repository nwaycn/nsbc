/**
 * SIP Digest 认证（RFC 7616 / RFC 2617），与 Android DigestAuth.kt 行为一致。
 * 浏览器 WebCrypto 不提供 MD5，这里内置一份纯 JS MD5。
 */

const SHIFT = [
  7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
];

// K[i] = floor(abs(sin(i + 1)) * 2^32)
const K = new Uint32Array(64);
for (let i = 0; i < 64; i++) {
  K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296) >>> 0;
}

function rotl(value, bits) {
  return ((value << bits) | (value >>> (32 - bits))) >>> 0;
}

/** MD5 十六进制小写（输入按 UTF-8 编码，与 Kotlin `toByteArray` 一致）。 */
export function md5(input) {
  const bytes = new TextEncoder().encode(String(input));
  const length = bytes.length;
  const total = (((length + 8) >>> 6) + 1) << 6; // 含 0x80 与 8 字节长度
  const buffer = new Uint8Array(total);
  buffer.set(bytes);
  buffer[length] = 0x80;

  const view = new DataView(buffer.buffer);
  const bitLength = length * 8;
  view.setUint32(total - 8, bitLength >>> 0, true);
  view.setUint32(total - 4, Math.floor(bitLength / 4294967296), true);

  let a0 = 0x67452301;
  let b0 = 0xefcdab89;
  let c0 = 0x98badcfe;
  let d0 = 0x10325476;
  const words = new Uint32Array(16);

  for (let offset = 0; offset < total; offset += 64) {
    for (let i = 0; i < 16; i++) {
      words[i] = view.getUint32(offset + i * 4, true);
    }
    let a = a0;
    let b = b0;
    let c = c0;
    let d = d0;

    for (let i = 0; i < 64; i++) {
      let f;
      let g;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
      }
      const sum = (a + f + K[i] + words[g]) >>> 0;
      const next = (b + rotl(sum, SHIFT[i])) >>> 0;
      a = d;
      d = c;
      c = b;
      b = next;
    }

    a0 = (a0 + a) >>> 0;
    b0 = (b0 + b) >>> 0;
    c0 = (c0 + c) >>> 0;
    d0 = (d0 + d) >>> 0;
  }

  const out = new DataView(new ArrayBuffer(16));
  out.setUint32(0, a0, true);
  out.setUint32(4, b0, true);
  out.setUint32(8, c0, true);
  out.setUint32(12, d0, true);
  let hex = '';
  for (let i = 0; i < 16; i++) {
    hex += out.getUint8(i).toString(16).padStart(2, '0');
  }
  return hex;
}

/** 解析 WWW-Authenticate / Proxy-Authenticate（忽略引号内的逗号）。 */
export function parseChallenge(headerValue) {
  const result = {};
  const body = String(headerValue || '').replace(/^\s*Digest\s+/i, '').trim();
  let inQuote = false;
  let start = 0;
  for (let i = 0; i <= body.length; i++) {
    const ch = body[i];
    if (ch === '"') inQuote = !inQuote;
    if ((ch === ',' && !inQuote) || i === body.length) {
      const pair = parsePair(body.slice(start, i));
      if (pair) result[pair[0]] = pair[1];
      start = i + 1;
    }
  }
  return result;
}

function parsePair(raw) {
  const idx = raw.indexOf('=');
  if (idx <= 0) return null;
  const key = raw.slice(0, idx).trim().toLowerCase();
  const value = raw.slice(idx + 1).trim().replace(/^"|"$/g, '');
  return key ? [key, value] : null;
}

/**
 * 生成 Authorization / Proxy-Authorization 头的值。
 * qop 优先取挑战里的 `auth`（NSBC 默认 qop=auth）；无 qop 时退化为 RFC 2069 形式。
 */
export function buildAuthorization({
  username,
  password,
  realm,
  nonce,
  uri,
  method,
  qop,
  nc,
  cnonce,
  algorithm = 'MD5',
  opaque = null
}) {
  const qopValue = qop
    ? (qop.split(',').map((v) => v.trim()).find((v) => v === 'auth') || qop)
    : null;

  const ha1Base = md5(`${username}:${realm}:${password}`);
  const ha1 = algorithm && algorithm.toLowerCase() === 'md5-sess'
    ? md5(`${ha1Base}:${nonce}:${cnonce}`)
    : ha1Base;
  const ha2 = md5(`${method}:${uri}`);
  const response = qopValue
    ? md5(`${ha1}:${nonce}:${nc}:${cnonce}:${qopValue}:${ha2}`)
    : md5(`${ha1}:${nonce}:${ha2}`);

  let header = `Digest username="${username}"`;
  header += `, realm="${realm}"`;
  header += `, nonce="${nonce}"`;
  header += `, uri="${uri}"`;
  header += `, response="${response}"`;
  if (algorithm) header += `, algorithm=${algorithm}`;
  if (qopValue) {
    header += `, qop=${qopValue}`;
    header += `, nc=${nc}`;
    header += `, cnonce="${cnonce}"`;
  }
  if (opaque != null) header += `, opaque="${opaque}"`;
  return header;
}
