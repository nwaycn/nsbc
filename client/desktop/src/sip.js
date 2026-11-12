/**
 * SIP over WebSocket 客户端（RFC 3261 / RFC 7118）。
 *
 * 协议行为逐条对齐 Android 端 SipEngine / SipMessage / DigestAuth：
 *  - REGISTER（401/407 Digest 重发、按有效期一半刷新、断开 Expires:0 注销）
 *  - 呼出 INVITE（先收集完整 ICE 再发 SDP）、ACK/BYE/CANCEL
 *  - 呼入 100/180/200/486/487、ACK 后 ESTABLISHED
 *  - 响应构造统一走 SipMessage.buildResponse（复制 Via/From/To/Call-ID/CSeq）
 */

import { parseChallenge, buildAuthorization } from './digest.js';

const USER_AGENT = 'NsbcDesktopSDK/1.0';
const ALLOW = 'INVITE, ACK, CANCEL, BYE, OPTIONS, MESSAGE';
const SUPPORTED = 'replaces, outbound';
const ALLOW_KEEPALIVE_INTERVAL_MS = 30000;
const KEEPALIVE_MAX_MISSES = 3;
const MAX_AUTH_RETRIES = 3;
const BRANCH_PREFIX = 'z9hG4bK-';

const COMPACT = {
  v: 'via', f: 'from', t: 'to', i: 'call-id', m: 'contact',
  l: 'content-length', c: 'content-type', k: 'supported', s: 'subject',
  e: 'content-encoding', o: 'event', u: 'allow-events', x: 'session-expires'
};

let branchCounter = 0;

function canonical(name) {
  const key = String(name || '').trim().toLowerCase();
  return COMPACT[key] || key;
}

function newBranch() {
  branchCounter += 1;
  return `${BRANCH_PREFIX}${Date.now()}-${branchCounter}`;
}

function randomHex(length) {
  const bytes = new Uint8Array(Math.ceil(length / 2));
  crypto.getRandomValues(bytes);
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('').slice(0, length);
}

function newTag() {
  return randomHex(12);
}

function newCallId(host) {
  const uuid = typeof crypto.randomUUID === 'function' ? crypto.randomUUID() : randomHex(32);
  return `${uuid}.${Date.now()}@${host}`;
}

function extractUri(header) {
  if (!header) return null;
  const lt = header.indexOf('<');
  const gt = header.indexOf('>');
  if (lt >= 0 && gt > lt) return header.slice(lt + 1, gt).trim();
  return header.split(';')[0].trim();
}

function extractTag(header) {
  if (!header) return null;
  const match = /;tag=([^;>\s]+)/.exec(header);
  return match ? match[1] : null;
}

function contactUri(header) {
  if (!header) return null;
  const lt = header.indexOf('<');
  const gt = header.indexOf('>');
  if (lt >= 0 && gt > lt) return header.slice(lt + 1, gt).trim();
  return header.split(';')[0].trim();
}

function normalizeTarget(target, realm) {
  const value = String(target || '').trim();
  if (value.startsWith('sip:') || value.startsWith('sips:')) return value;
  if (value.includes('@')) return `sip:${value}`;
  return `sip:${value}@${realm}`;
}

/** 极简 SIP 消息模型：头部保序、大小写不敏感，Content-Length 自动计算。 */
export class SipMessage {
  constructor() {
    this.method = null;
    this.requestUri = null;
    this.statusCode = null;
    this.reasonPhrase = null;
    this.headers = [];
    this.body = '';
  }

  get isRequest() {
    return this.method != null;
  }

  get isResponse() {
    return this.statusCode != null;
  }

  addHeader(name, value) {
    this.headers.push([name, value]);
    return this;
  }

  header(name) {
    const key = canonical(name);
    const found = this.headers.find(([n]) => canonical(n) === key);
    return found ? found[1] : undefined;
  }

  headersOf(name) {
    const key = canonical(name);
    return this.headers.filter(([n]) => canonical(n) === key).map(([, v]) => v);
  }

  callId() {
    return this.header('Call-ID');
  }

  cseqMethod() {
    const cseq = this.header('CSeq');
    return cseq ? cseq.trim().split(/\s+/)[1] : undefined;
  }

  fromUri() {
    return extractUri(this.header('From'));
  }

  fromTag() {
    return extractTag(this.header('From'));
  }

  toTag() {
    return extractTag(this.header('To'));
  }

  fromDisplayName() {
    const header = this.header('From');
    if (!header) return '';
    const quoted = /^\s*"([^"]*)"/.exec(header);
    if (quoted) return quoted[1];
    const lt = header.indexOf('<');
    return lt > 0 ? header.slice(0, lt).trim() : '';
  }

  toWire() {
    let head;
    if (this.isRequest) {
      head = `${this.method} ${this.requestUri} SIP/2.0\r\n`;
    } else if (this.isResponse) {
      head = `SIP/2.0 ${this.statusCode} ${this.reasonPhrase || ''}\r\n`;
    } else {
      throw new Error('SipMessage 既不是请求也不是响应');
    }
    for (const [name, value] of this.headers) {
      if (canonical(name) === 'content-length') continue;
      head += `${name}: ${value}\r\n`;
    }
    const bodyBytes = new TextEncoder().encode(this.body).length;
    head += `Content-Length: ${bodyBytes}\r\n\r\n`;
    return this.body ? head + this.body : head;
  }

  /** 解析失败返回 null（与 Android SipMessage.parse 一致）。 */
  static parse(raw) {
    if (!raw || !String(raw).trim()) return null;
    const normalized = String(raw).replace(/\r\n/g, '\n');
    const sep = normalized.indexOf('\n\n');
    const headPart = sep >= 0 ? normalized.slice(0, sep) : normalized;
    const bodyPart = sep >= 0 ? normalized.slice(sep + 2) : '';
    const lines = headPart.split('\n');
    if (!lines.length) return null;

    const msg = new SipMessage();
    const first = lines[0].trim();
    if (first.startsWith('SIP/2.0')) {
      const parts = first.split(' ');
      const code = Number.parseInt(parts[1], 10);
      if (!Number.isFinite(code)) return null;
      msg.statusCode = code;
      msg.reasonPhrase = parts.slice(2).join(' ');
    } else {
      const parts = first.split(' ');
      if (parts.length < 3 || parts[2].toUpperCase() !== 'SIP/2.0') return null;
      msg.method = parts[0].toUpperCase();
      msg.requestUri = parts[1];
    }

    let lastIndex = -1;
    for (let i = 1; i < lines.length; i++) {
      const line = lines[i];
      if (!line) break;
      if (line[0] === ' ' || line[0] === '\t') {
        // 折行续接
        if (lastIndex >= 0) {
          const [name, value] = msg.headers[lastIndex];
          msg.headers[lastIndex] = [name, `${value} ${line.trim()}`];
        }
        continue;
      }
      const idx = line.indexOf(':');
      if (idx <= 0) continue;
      msg.headers.push([line.slice(0, idx).trim(), line.slice(idx + 1).trim()]);
      lastIndex = msg.headers.length - 1;
    }

    msg.body = bodyPart;
    const declared = Number.parseInt(msg.header('Content-Length'), 10);
    if (Number.isFinite(declared) && declared >= 0) {
      const bytes = new TextEncoder().encode(bodyPart);
      if (declared <= bytes.length) {
        msg.body = new TextDecoder().decode(bytes.slice(0, declared));
      }
    }
    return msg;
  }

  /**
   * 与 Android SipMessage.buildResponse 相同：复制 Via（保序）/From/To/Call-ID/CSeq。
   * 请求 To 无 tag 时补 toTag，保证同一 INVITE 的多个响应 tag 稳定。
   */
  static buildResponse(request, code, reason, toTag = null) {
    const response = new SipMessage();
    response.statusCode = code;
    response.reasonPhrase = reason;
    request.headersOf('Via').forEach((via) => response.addHeader('Via', via));
    const from = request.header('From');
    if (from) response.addHeader('From', from);
    const to = request.header('To');
    if (to) {
      response.addHeader('To', extractTag(to) != null ? to : `${to};tag=${toTag || newTag()}`);
    }
    const callId = request.header('Call-ID');
    if (callId) response.addHeader('Call-ID', callId);
    const cseq = request.header('CSeq');
    if (cseq) response.addHeader('CSeq', cseq);
    return response;
  }
}

export class SipClient {
  /**
   * @param {object} config 与 Android NsbcConfig 同名字段
   * @param {object} handlers onRegistration/onIncomingCall/onCallState/onError/onLog
   */
  constructor(config, handlers = {}) {
    this.config = {
      ...config,
      displayName: (config.displayName || '').trim() || config.username,
      wsPath: config.wsPath && config.wsPath.startsWith('/') ? config.wsPath : `/${config.wsPath || 'ws'}`
    };
    this.handlers = handlers;
    this.viaProtocol = this.config.useTls ? 'WSS' : 'WS';
    // 浏览器拿不到本机 IP；Via sent-by 仅作占位，响应仍沿同一条 WS 连接返回
    this.localHost = 'nsbc-desktop';

    this.ws = null;
    this.manualClose = false;
    this.hadError = false;
    this.reconnectTimer = null;
    this.reconnectAttempt = 0;
    this.refreshTimer = null;

    this.state = 'UNREGISTERED';
    this.regCallId = '';
    this.regTag = '';
    this.regCseq = 0;
    this.regAuth = null;
    this.regAuthAttempts = 0;

    this.keepaliveTimer = null;
    this.keepaliveCallId = '';
    this.keepaliveMisses = 0;
    this.sessionId = `${newTag()}@${this.config.server}`;

    this.ctx = null;
    this.media = null;
    this.lastRequest = null;
  }

  get registrationState() {
    return this.state;
  }

  get isOpen() {
    return !!this.ws && this.ws.readyState === WebSocket.OPEN;
  }

  // ------------------------------------------------------------ 连接

  connect() {
    if (this.ws && (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)) {
      return;
    }
    this.clearReconnect();
    this.manualClose = false;
    this.hadError = false;

    const url = this.webSocketUrl();
    this.log('info', `连接 ${url}`);
    let ws;
    try {
      ws = new WebSocket(url, 'sip');
    } catch (err) {
      this.setRegistration('FAILED', `创建 WebSocket 失败: ${err.message}`);
      this.scheduleReconnect();
      return;
    }
    this.ws = ws;

    ws.onopen = () => {
      if (ws !== this.ws) return;
      this.reconnectAttempt = 0;
      this.hadError = false;
      this.log('info', 'WebSocket 已连接');
      this.sendRegister(null);
    };

    ws.onmessage = (event) => {
      if (ws !== this.ws) return;
      this.handleFrame(event.data);
    };

    ws.onerror = () => {
      if (ws !== this.ws) return;
      this.hadError = true;
      this.stopKeepalive();
      this.log('error', 'WebSocket 连接错误');
    };

    ws.onclose = (event) => {
      if (ws !== this.ws) return;
      this.ws = null;
      this.clearRefresh();
      this.stopKeepalive();
      const detail = event.reason ? `连接关闭: ${event.reason}` : `连接关闭 (${event.code})`;
      this.setRegistration(this.hadError ? 'FAILED' : 'UNREGISTERED', detail);
      if (this.hadError) this.emit('onError', detail);
      this.scheduleReconnect();
    };
  }

  disconnect() {
    this.manualClose = true;
    this.clearReconnect();
    this.clearRefresh();
    this.stopKeepalive();
    this.reconnectAttempt = 0;

    const ctx = this.ctx;
    if (ctx) {
      if (ctx.call.state === 'ESTABLISHED') {
        this.sendBye(ctx);
      } else if (ctx.isIncoming) {
        this.rejectIncoming(ctx, 486);
      } else {
        this.sendCancel(ctx);
      }
    }
    if (this.state === 'REGISTERED') this.sendUnregister();

    const ws = this.ws;
    this.ws = null;
    if (ws) {
      try {
        ws.close(1000, 'bye');
      } catch {
        // 忽略关闭异常
      }
    }
    this.setRegistration('UNREGISTERED', '已主动断开');
  }

  webSocketUrl() {
    const scheme = this.config.useTls ? 'wss' : 'ws';
    return `${scheme}://${this.config.server}:${this.config.wssPort}${this.config.wsPath}`;
  }

  scheduleReconnect() {
    if (this.manualClose || !this.config.autoReconnect) return;
    const base = Math.max(Number(this.config.reconnectDelayMs) || 2000, 500);
    const delay = Math.min(base * Math.pow(2, this.reconnectAttempt), 30000);
    this.reconnectAttempt += 1;
    this.log('info', `${delay}ms 后自动重连`);
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      if (!this.manualClose) this.connect();
    }, delay);
  }

  clearReconnect() {
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
  }

  handleFrame(data) {
    if (typeof data === 'string') {
      this.handleSip(data);
    } else if (typeof Blob !== 'undefined' && data instanceof Blob) {
      data.text().then((text) => this.handleSip(text)).catch(() => {});
    } else if (data instanceof ArrayBuffer) {
      this.handleSip(new TextDecoder().decode(data));
    } else if (ArrayBuffer.isView(data)) {
      this.handleSip(new TextDecoder().decode(data));
    }
  }

  send(message) {
    if (!this.isOpen) {
      this.log('warn', `未连接，丢弃发送: ${message.method || message.statusCode}`);
      return false;
    }
    this.log('debug', `>>> ${message.method ? `${message.method} ${message.requestUri}` : `${message.statusCode} ${message.reasonPhrase}`}`);
    try {
      this.ws.send(message.toWire());
      return true;
    } catch (err) {
      this.log('error', `发送失败: ${err.message}`);
      return false;
    }
  }

  // ------------------------------------------------------------ REGISTER

  sendRegister(auth) {
    const cfg = this.config;
    this.setRegistration('REGISTERING', null);

    if (!this.regCallId) {
      this.regCallId = newCallId(cfg.server);
      this.regTag = newTag();
    }
    const branch = newBranch();
    this.regCseq += 1;

    const req = new SipMessage();
    req.method = 'REGISTER';
    req.requestUri = `sip:${cfg.realm}`;
    req.addHeader('Via', `SIP/2.0/${this.viaProtocol} ${this.localHost};branch=${branch};rport`);
    req.addHeader('Max-Forwards', '70');
    req.addHeader('From', `"${this.config.displayName}" <${this.sipUri()}>;tag=${this.regTag}`);
    req.addHeader('To', `<${this.sipUri()}>`);
    req.addHeader('Call-ID', this.regCallId);
    req.addHeader('CSeq', `${this.regCseq} REGISTER`);
    req.addHeader('Contact', `<${this.sipUri()};transport=ws>`);
    req.addHeader('Expires', String(cfg.registerExpiresSec || 300));
    req.addHeader('Allow', ALLOW);
    req.addHeader('Supported', SUPPORTED);
    req.addHeader('Session-ID', this.sessionId);
    req.addHeader('User-Agent', USER_AGENT);

    if (auth) {
      req.addHeader(
        auth.proxy ? 'Proxy-Authorization' : 'Authorization',
        this.buildAuthValue(auth, 'REGISTER', `sip:${cfg.realm}`)
      );
    }
    this.send(req);
  }

  sendUnregister() {
    const cfg = this.config;
    this.regCseq += 1;
    const req = new SipMessage();
    req.method = 'REGISTER';
    req.requestUri = `sip:${cfg.realm}`;
    req.addHeader('Via', `SIP/2.0/${this.viaProtocol} ${this.localHost};branch=${newBranch()};rport`);
    req.addHeader('Max-Forwards', '70');
    req.addHeader('From', `"${this.config.displayName}" <${this.sipUri()}>;tag=${this.regTag}`);
    req.addHeader('To', `<${this.sipUri()}>`);
    req.addHeader('Call-ID', this.regCallId);
    req.addHeader('CSeq', `${this.regCseq} REGISTER`);
    req.addHeader('Contact', `<${this.sipUri()};transport=ws>`);
    req.addHeader('Expires', '0');
    req.addHeader('Supported', SUPPORTED);
    req.addHeader('Session-ID', this.sessionId);
    req.addHeader('User-Agent', USER_AGENT);
    if (this.regAuth) {
      req.addHeader(
        this.regAuth.proxy ? 'Proxy-Authorization' : 'Authorization',
        this.buildAuthValue(this.regAuth, 'REGISTER', `sip:${cfg.realm}`)
      );
    }
    this.send(req);
  }

  scheduleRefresh(expiresSec) {
    this.clearRefresh();
    const delay = Math.max(expiresSec, 30) * 1000 / 2;
    this.refreshTimer = setTimeout(() => {
      this.refreshTimer = null;
      if (this.state === 'REGISTERED' && this.isOpen) {
        this.log('info', '刷新注册');
        this.sendRegister(this.regAuth);
      }
    }, delay);
  }

  clearRefresh() {
    if (this.refreshTimer) clearTimeout(this.refreshTimer);
    this.refreshTimer = null;
  }

  // ------------------------------------------------------------ Keepalive OPTIONS

  startKeepalive() {
    this.stopKeepalive();
    this.keepaliveMisses = 0;
    this.keepaliveCallId = newCallId(this.config.server);
    this.sendKeepaliveOptions();
    this.keepaliveTimer = setInterval(() => {
      this.sendKeepaliveOptions();
    }, ALLOW_KEEPALIVE_INTERVAL_MS);
  }

  stopKeepalive() {
    if (this.keepaliveTimer) clearInterval(this.keepaliveTimer);
    this.keepaliveTimer = null;
    this.keepaliveCallId = '';
    this.keepaliveMisses = 0;
  }

  sendKeepaliveOptions() {
    if (!this.isOpen || this.state !== 'REGISTERED') return;
    if (!this.keepaliveCallId) this.keepaliveCallId = newCallId(this.config.server);
    const req = new SipMessage();
    req.method = 'OPTIONS';
    req.requestUri = `sip:${this.config.realm}`;
    req.addHeader('Via', `SIP/2.0/${this.viaProtocol} ${this.localHost};branch=${newBranch()};rport`);
    req.addHeader('Max-Forwards', '70');
    req.addHeader('From', `"${this.config.displayName}" <${this.sipUri()}>;tag=${this.regTag}`);
    req.addHeader('To', `<${this.sipUri()}>`);
    req.addHeader('Call-ID', this.keepaliveCallId);
    req.addHeader('CSeq', `${this.regCseq} OPTIONS`);
    req.addHeader('Supported', SUPPORTED);
    req.addHeader('Session-ID', this.sessionId);
    req.addHeader('User-Agent', USER_AGENT);
    this.send(req);
  }

  handleRegisterResponse(msg) {
    const code = msg.statusCode;
    if (code === 200) {
      this.regAuthAttempts = 0;
      const expires = Number.parseInt(msg.header('Expires'), 10);
      const effective = Number.isFinite(expires) ? expires : (this.config.registerExpiresSec || 300);
      this.setRegistration('REGISTERED', null);
      this.log('info', `注册成功，有效期 ${effective}s`);
      this.scheduleRefresh(effective);
      this.startKeepalive();
      return;
    }
    if (code === 401 || code === 407) {
      const headerName = code === 401 ? 'WWW-Authenticate' : 'Proxy-Authenticate';
      const challenge = msg.header(headerName);
      if (!challenge) {
        this.setRegistration('FAILED', '缺少鉴权挑战头');
        return;
      }
      this.regAuthAttempts += 1;
      if (this.regAuthAttempts > MAX_AUTH_RETRIES) {
        this.setRegistration('FAILED', '鉴权失败（请检查用户名或密码）');
        return;
      }
      const params = parseChallenge(challenge);
      this.regAuth = {
        realm: params.realm || this.config.realm,
        nonce: params.nonce || '',
        qop: params.qop || null,
        opaque: params.opaque != null ? params.opaque : null,
        algorithm: params.algorithm || 'MD5',
        proxy: code === 407,
        nc: 0
      };
      this.log('info', `收到 ${code} 挑战，带认证重发`);
      this.sendRegister(this.regAuth);
      return;
    }
    if (code >= 400 && code <= 699) {
      this.setRegistration('FAILED', `注册被拒绝: ${code} ${msg.reasonPhrase || ''}`.trim());
      return;
    }
    this.log('warn', `REGISTER 中间响应 ${code}`);
  }

  // ------------------------------------------------------------ 收发分发

  handleSip(raw) {
    const msg = SipMessage.parse(raw);
    if (!msg) {
      this.log('warn', '无法解析 SIP 消息');
      return;
    }
    if (msg.isResponse) {
      this.handleResponse(msg);
      return;
    }
    this.lastRequest = msg;
    switch (msg.method) {
      case 'INVITE':
        this.handleIncomingInvite(msg);
        break;
      case 'ACK':
        this.handleIncomingAck(msg);
        break;
      case 'BYE':
        this.handleIncomingBye(msg);
        break;
      case 'CANCEL':
        this.handleIncomingCancel(msg);
        break;
      case 'OPTIONS': {
        const resp = SipMessage.buildResponse(msg, 200, 'OK');
        resp.addHeader('Allow', ALLOW);
        resp.addHeader('Supported', SUPPORTED);
        this.send(resp);
        break;
      }
      case 'MESSAGE':
        this.reply(msg, 200, 'OK');
        this.log('info', `收到 MESSAGE: ${msg.body.slice(0, 100)}`);
        break;
      default:
        this.reply(msg, 405, 'Method Not Allowed');
        break;
    }
  }

  handleResponse(msg) {
    const method = msg.cseqMethod();
    const callId = msg.callId();
    if (callId && this.keepaliveCallId && callId === this.keepaliveCallId && method === 'OPTIONS') {
      if (msg.statusCode === 200) {
        this.keepaliveMisses = 0;
      } else {
        this.keepaliveMisses += 1;
        this.log('warn', `Keepalive OPTIONS 响应异常 ${msg.statusCode} (miss=${this.keepaliveMisses})`);
        if (this.keepaliveMisses >= KEEPALIVE_MAX_MISSES) {
          this.log('warn', `Keepalive 连续 ${KEEPALIVE_MAX_MISSES} 次失败，触发重注册`);
          this.keepaliveMisses = 0;
          this.sendRegister(this.regAuth);
        }
      }
      return;
    }
    if (method === 'REGISTER' && callId === this.regCallId) {
      this.handleRegisterResponse(msg);
    } else if (method === 'INVITE' && this.ctx && callId === this.ctx.callId) {
      this.handleInviteResponse(msg);
    } else {
      this.log('debug', `未匹配响应 ${msg.statusCode} cseq=${method}`);
    }
  }

  // ------------------------------------------------------------ 呼出

  startCall(target, media) {
    if (this.state !== 'REGISTERED') throw new Error('尚未注册到 NSBC，无法呼叫');
    if (this.ctx && this.ctx.call.isActive) throw new Error('已有进行中的呼叫');

    const remoteUri = normalizeTarget(target, this.config.realm);
    const callId = newCallId(this.config.server);
    const call = {
      id: callId,
      isIncoming: false,
      remoteUri,
      remoteDisplayName: '',
      state: 'CALLING',
      establishedAt: 0,
      endedAt: 0,
      endReason: '',
      get isActive() {
        return this.state === 'CALLING' || this.state === 'RINGING' || this.state === 'ESTABLISHED';
      }
    };
    const ctx = {
      call,
      callId,
      localTag: newTag(),
      isIncoming: false,
      remoteUri,
      remoteTag: null,
      branch: newBranch(),
      cseq: 0,
      localSdp: null,
      remoteSdp: null,
      remoteTarget: null,
      auth: null,
      authAttempts: 0,
      acked: false,
      inviteRequest: null
    };
    this.ctx = ctx;
    this.media = media;
    this.updateCall(ctx, 'CALLING');

    media.createOffer()
      .then((sdp) => {
        if (this.ctx !== ctx) return;
        ctx.localSdp = sdp;
        this.sendInvite(ctx);
      })
      .catch((err) => {
        this.log('error', `创建 offer 失败: ${err.message}`);
        this.failCall(ctx, 'media_failed');
      });

    return call;
  }

  sendInvite(ctx) {
    ctx.branch = newBranch();
    ctx.cseq += 1;
    const req = new SipMessage();
    req.method = 'INVITE';
    req.requestUri = ctx.remoteUri;
    req.addHeader('Via', `SIP/2.0/${this.viaProtocol} ${this.localHost};branch=${ctx.branch};rport`);
    req.addHeader('Max-Forwards', '70');
    req.addHeader('From', `"${this.config.displayName}" <${this.sipUri()}>;tag=${ctx.localTag}`);
    req.addHeader('To', `<${ctx.remoteUri}>`);
    req.addHeader('Call-ID', ctx.callId);
    req.addHeader('CSeq', `${ctx.cseq} INVITE`);
    req.addHeader('Contact', `<${this.sipUri()};transport=ws>`);
    req.addHeader('Allow', ALLOW);
    req.addHeader('Supported', SUPPORTED);
    req.addHeader('Session-ID', this.sessionId);
    req.addHeader('User-Agent', USER_AGENT);
    if (ctx.localSdp) {
      req.addHeader('Content-Type', 'application/sdp');
      req.body = ctx.localSdp;
    }
    if (ctx.auth) {
      req.addHeader(
        ctx.auth.proxy ? 'Proxy-Authorization' : 'Authorization',
        this.buildAuthValue(ctx.auth, 'INVITE', ctx.remoteUri)
      );
    }
    this.send(req);
  }

  sendAck(ctx) {
    if (ctx.acked) return;
    ctx.acked = true;
    const req = new SipMessage();
    req.method = 'ACK';
    req.requestUri = ctx.remoteTarget || ctx.remoteUri;
    req.addHeader('Via', `SIP/2.0/${this.viaProtocol} ${this.localHost};branch=${newBranch()};rport`);
    req.addHeader('Max-Forwards', '70');
    req.addHeader('From', `"${this.config.displayName}" <${this.sipUri()}>;tag=${ctx.localTag}`);
    req.addHeader('To', `<${ctx.remoteUri}>${ctx.remoteTag ? `;tag=${ctx.remoteTag}` : ''}`);
    req.addHeader('Call-ID', ctx.callId);
    req.addHeader('CSeq', `${ctx.cseq} ACK`);
    req.addHeader('Supported', SUPPORTED);
    req.addHeader('Session-ID', this.sessionId);
    req.addHeader('User-Agent', USER_AGENT);
    this.send(req);
  }

  sendBye(ctx) {
    ctx.cseq += 1;
    const req = new SipMessage();
    req.method = 'BYE';
    req.requestUri = ctx.remoteTarget || ctx.remoteUri;
    req.addHeader('Via', `SIP/2.0/${this.viaProtocol} ${this.localHost};branch=${newBranch()};rport`);
    req.addHeader('Max-Forwards', '70');
    req.addHeader('From', `"${this.config.displayName}" <${this.sipUri()}>;tag=${ctx.localTag}`);
    req.addHeader('To', `<${ctx.remoteUri}>${ctx.remoteTag ? `;tag=${ctx.remoteTag}` : ''}`);
    req.addHeader('Call-ID', ctx.callId);
    req.addHeader('CSeq', `${ctx.cseq} BYE`);
    req.addHeader('Supported', SUPPORTED);
    req.addHeader('Session-ID', this.sessionId);
    req.addHeader('User-Agent', USER_AGENT);
    this.send(req);
    this.terminate(ctx, 'local_hangup');
  }

  sendCancel(ctx) {
    const req = new SipMessage();
    req.method = 'CANCEL';
    req.requestUri = ctx.remoteUri;
    // CANCEL 必须使用与被取消 INVITE 相同的 branch
    req.addHeader('Via', `SIP/2.0/${this.viaProtocol} ${this.localHost};branch=${ctx.branch};rport`);
    req.addHeader('Max-Forwards', '70');
    req.addHeader('From', `"${this.config.displayName}" <${this.sipUri()}>;tag=${ctx.localTag}`);
    req.addHeader('To', `<${ctx.remoteUri}>`);
    req.addHeader('Call-ID', ctx.callId);
    req.addHeader('CSeq', `${ctx.cseq} CANCEL`);
    req.addHeader('Supported', SUPPORTED);
    req.addHeader('Session-ID', this.sessionId);
    req.addHeader('User-Agent', USER_AGENT);
    this.send(req);
    this.terminate(ctx, 'local_cancel');
  }

  handleInviteResponse(msg) {
    const ctx = this.ctx;
    if (!ctx) return;
    const code = msg.statusCode;

    if (code >= 100 && code <= 199) {
      if ((code === 180 || code === 183) && (ctx.call.state === 'CALLING' || ctx.call.state === 'RINGING')) {
        this.updateCall(ctx, 'RINGING');
      }
      return;
    }

    if (code === 200) {
      if (ctx.acked) return;
      ctx.remoteTag = msg.toTag();
      ctx.remoteTarget = contactUri(msg.header('Contact')) || ctx.remoteUri;
      // 先 ACK，再应用 answer SDP（与 Android 顺序一致）
      this.sendAck(ctx);
      const sdp = msg.body;
      if (sdp && sdp.trim()) {
        ctx.remoteSdp = sdp;
        const media = this.media;
        if (!media) {
          this.failCall(ctx, 'media_failed');
          return;
        }
        media.applyRemoteAnswer(sdp)
          .then(() => {
            if (this.ctx === ctx) this.establish(ctx);
          })
          .catch((err) => {
            this.log('error', `应用 answer SDP 失败: ${err.message}`);
            this.failCall(ctx, 'media_failed');
          });
      } else {
        this.establish(ctx);
      }
      return;
    }

    if (code === 401 || code === 407) {
      const headerName = code === 401 ? 'WWW-Authenticate' : 'Proxy-Authenticate';
      const challenge = msg.header(headerName);
      if (!challenge) {
        this.failCall(ctx, 'auth_missing');
        return;
      }
      ctx.authAttempts += 1;
      if (ctx.authAttempts > MAX_AUTH_RETRIES) {
        this.failCall(ctx, 'auth_failed');
        return;
      }
      const params = parseChallenge(challenge);
      ctx.auth = {
        realm: params.realm || this.config.realm,
        nonce: params.nonce || '',
        qop: params.qop || null,
        opaque: params.opaque != null ? params.opaque : null,
        algorithm: params.algorithm || 'MD5',
        proxy: code === 407,
        nc: 0
      };
      this.log('info', `INVITE 收到 ${code}，带认证重发`);
      this.sendInvite(ctx);
      return;
    }

    if (code >= 300 && code <= 699) {
      const reason = code === 486 || code === 600 ? 'busy'
        : code === 487 ? 'canceled'
          : code === 408 ? 'timeout'
            : code === 603 ? 'declined'
              : `rejected_${code}`;
      this.failCall(ctx, reason);
    }
  }

  // ------------------------------------------------------------ 呼入

  handleIncomingInvite(msg) {
    if (this.ctx) {
      this.reply(msg, 486, 'Busy Here');
      return;
    }
    const callId = msg.callId();
    if (!callId) {
      this.reply(msg, 400, 'Bad Request');
      return;
    }
    const fromUri = msg.fromUri() || 'sip:unknown';
    const call = {
      id: callId,
      isIncoming: true,
      remoteUri: fromUri,
      remoteDisplayName: msg.fromDisplayName(),
      state: 'RINGING',
      establishedAt: 0,
      endedAt: 0,
      endReason: '',
      get isActive() {
        return this.state === 'CALLING' || this.state === 'RINGING' || this.state === 'ESTABLISHED';
      }
    };
    const ctx = {
      call,
      callId,
      localTag: newTag(),
      isIncoming: true,
      remoteUri: fromUri,
      remoteTag: msg.fromTag(),
      branch: newBranch(),
      cseq: 0,
      localSdp: null,
      remoteSdp: msg.body,
      remoteTarget: null,
      auth: null,
      authAttempts: 0,
      acked: false,
      inviteRequest: msg
    };
    this.ctx = ctx;

    this.reply(msg, 100, 'Trying', ctx.localTag);
    this.reply(msg, 180, 'Ringing', ctx.localTag);
    this.updateCall(ctx, 'RINGING');
    this.emit('onIncomingCall', call);
  }

  async answer(media) {
    const ctx = this.ctx;
    if (!ctx || !ctx.isIncoming) return;
    this.media = media;
    const offer = ctx.remoteSdp;

    try {
      if (offer && offer.trim()) {
        await media.applyRemoteOffer(offer);
        const sdp = await media.createAnswer();
        this.sendAnswer(ctx, sdp);
      } else {
        this.log('warn', '呼入 INVITE 无 SDP offer，直接应答空媒体');
        this.sendAnswer(ctx, '');
      }
    } catch (err) {
      this.log('error', `应答失败: ${err.message}`);
      this.rejectIncoming(ctx, 488);
    }
  }

  sendAnswer(ctx, sdp) {
    ctx.localSdp = sdp;
    const invite = ctx.inviteRequest || this.lastRequest;
    if (!invite) return;
    const resp = SipMessage.buildResponse(invite, 200, 'OK', ctx.localTag);
    resp.addHeader('Contact', `<${this.sipUri()};transport=ws>`);
    resp.addHeader('Allow', ALLOW);
    resp.addHeader('User-Agent', USER_AGENT);
    if (sdp) {
      resp.addHeader('Content-Type', 'application/sdp');
      resp.body = sdp;
    }
    this.send(resp);
    // 200 已发出；收到 ACK 后再置为 ESTABLISHED
  }

  rejectIncoming(ctx, code) {
    const reason = code === 486 ? 'Busy Here'
      : code === 603 ? 'Decline'
        : code === 488 ? 'Not Acceptable Here'
          : 'Rejected';
    const invite = ctx.inviteRequest || this.lastRequest;
    if (invite) {
      this.send(SipMessage.buildResponse(invite, code, reason, ctx.localTag));
    }
    this.terminate(ctx, 'local_reject');
  }

  handleIncomingAck() {
    const ctx = this.ctx;
    if (!ctx || !ctx.isIncoming || ctx.acked) return;
    ctx.acked = true;
    if (ctx.call.state !== 'ESTABLISHED') this.establish(ctx);
  }

  handleIncomingBye(msg) {
    this.reply(msg, 200, 'OK');
    if (this.ctx) this.terminate(this.ctx, 'remote_hangup');
  }

  handleIncomingCancel(msg) {
    this.reply(msg, 200, 'OK');
    const ctx = this.ctx;
    if (!ctx) return;
    // 487 必须基于被取消的 INVITE 原始请求，不能用 CANCEL 构造
    const invite = ctx.inviteRequest || this.lastRequest;
    if (invite) {
      this.send(SipMessage.buildResponse(invite, 487, 'Request Terminated', ctx.localTag));
    }
    this.terminate(ctx, 'remote_cancel');
  }

  // ------------------------------------------------------------ 动作

  hangup() {
    const ctx = this.ctx;
    if (!ctx) return;
    if (ctx.call.state === 'ESTABLISHED') {
      this.sendBye(ctx);
    } else if (ctx.call.state === 'CALLING' || ctx.call.state === 'RINGING') {
      if (ctx.isIncoming) this.rejectIncoming(ctx, 486);
      else this.sendCancel(ctx);
    }
  }

  reject(code = 486) {
    const ctx = this.ctx;
    if (ctx && ctx.isIncoming) this.rejectIncoming(ctx, code);
  }

  setMuted(muted) {
    if (this.media) this.media.setMuted(muted);
  }

  setVideoEnabled(enabled) {
    if (this.media) this.media.setVideoEnabled(enabled);
  }

  switchCamera() {
    return this.media ? this.media.switchCamera() : Promise.resolve(false);
  }

  // ------------------------------------------------------------ 工具

  sipUri() {
    return `sip:${this.config.username}@${this.config.realm}`;
  }

  reply(request, code, reason, toTag = null) {
    const resp = SipMessage.buildResponse(request, code, reason, toTag);
    this.send(resp);
  }

  buildAuthValue(auth, method, uri) {
    auth.nc += 1;
    const nc = auth.nc.toString(16).padStart(8, '0');
    return buildAuthorization({
      username: this.config.username,
      password: this.config.password,
      realm: auth.realm,
      nonce: auth.nonce,
      uri,
      method,
      qop: auth.qop,
      nc,
      cnonce: newTag(),
      algorithm: auth.algorithm,
      opaque: auth.opaque
    });
  }

  establish(ctx) {
    ctx.call.establishedAt = Date.now();
    this.updateCall(ctx, 'ESTABLISHED');
  }

  updateCall(ctx, state) {
    if (ctx.call.state === state) return;
    ctx.call.state = state;
    this.log('info', `呼叫状态 -> ${state} (${ctx.remoteUri})`);
    this.emit('onCallState', ctx.call);
  }

  failCall(ctx, reason) {
    if (this.ctx !== ctx) return;
    ctx.call.endReason = reason;
    this.updateCall(ctx, 'FAILED');
    this.terminate(ctx, reason, true);
  }

  terminate(ctx, reason, alreadyFailed = false) {
    ctx.call.endReason = reason;
    ctx.call.endedAt = Date.now();
    if (!alreadyFailed && ctx.call.state !== 'FAILED') {
      this.updateCall(ctx, 'TERMINATED');
    }
    if (this.ctx === ctx) this.ctx = null;
    if (this.media) {
      try {
        this.media.stop();
      } catch {
        // 媒体已经释放
      }
      this.media = null;
    }
  }

  setRegistration(state, detail) {
    this.state = state;
    this.emit('onRegistration', state, detail);
  }

  emit(name, ...args) {
    const handler = this.handlers[name];
    if (typeof handler !== 'function') return;
    try {
      handler(...args);
    } catch (err) {
      console.error(`[SIP] 回调 ${name} 异常`, err);
    }
  }

  log(level, message) {
    const ranks = { none: 0, error: 1, warn: 2, info: 3, debug: 4, verbose: 5 };
    const configured = ranks[String(this.config.logLevel || 'INFO').toLowerCase()];
    const rank = ranks[level] != null ? ranks[level] : 3;
    if (configured === 0 || rank > (configured == null ? 3 : configured)) return;
    const line = `[SIP] ${message}`;
    if (level === 'error') console.error(line);
    else if (level === 'warn') console.warn(line);
    else console.debug(line);
    this.emit('onLog', level, message);
  }
}
