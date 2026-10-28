/* global JsSIP */
// =============================================================================
// nsbc-phone.js — NSBC WebRTC Total享电话层
// -----------------------------------------------------------------------------
// 包含完整的 JsSIP 3.10.x 控制逻辑 + Hold/DTMF/拨号盘增强功能。
// 多个 HTML 面可以通过 <script src="js/nsbc-phone.js"></script> Total享。
// 面需先加载 jssip-3.10.0.min.js，并提供这些 DOM ID:
//   wss, stun, user, pass, btnReg, btnUnreg, regStatus,
//   localVideo, remoteVideo, localDot, remoteDot, btnMute, btnStopVid, btnHold,
//   target, btnCall, btnHang, callStatus,
//   incoming, inFrom, btnHold, dtmfDisplay,
//   dcMsg, btnDc, dcLog,
//   sipLog
// =============================================================================

const $ = (id) => document.getElementById(id);
let ua = null;
let currentSession = null;
let pendingSession = null;
let localStream = null;
let remoteStream = null;
let dc = null, dcOpen = false;
let isHangingUp = false;

// ============ Ringing音（Web Audio 合成，无需外部文件） ============
// 经典 440+480Hz 双音Ringing（Bell 风格），2s on / 4s off 循环
let _ringAC = null, _ringTimer = null, _ringGain = null;
function playRingTone() {
  if (_ringTimer) return;           // 已在响
  try {
    if (!_ringAC) _ringAC = new (window.AudioContext || window.webkitAudioContext)();
    if (_ringAC.state === 'suspended') _ringAC.resume();
    const osc1 = _ringAC.createOscillator(); osc1.frequency.value = 440;
    const osc2 = _ringAC.createOscillator(); osc2.frequency.value = 480;
    const gain = _ringAC.createGain();
    gain.gain.value = 0;            // 初始Mute，让 gain 包络控制 on/off
    osc1.connect(gain); osc2.connect(gain); gain.connect(_ringAC.destination);
    osc1.start(); osc2.start();
    _ringGain = gain;

    // 2s响 + 4s停 循环（经典Ringing节奏）
    const tick = () => {
      if (!_ringGain) return;
      if (_ringGain.gain.value === 0) {
        // on
        _ringGain.gain.linearRampToValueAtTime(0.25, _ringAC.currentTime + 0.05);
        _ringTimer = setTimeout(() => {
          if (_ringGain) _ringGain.gain.linearRampToValueAtTime(0, _ringAC.currentTime + 0.1);
          _ringTimer = setTimeout(tick, 4000);  // off 4s
        }, 2000);                                // on  2s
      }
    };
    tick();
    log('info', '🔔 180 Ringing → 播放本地Ringing');
  } catch (e) {
    console.warn('[NSBC] playRingTone fail:', e.message);
  }
}
function stopRingTone() {
  if (_ringTimer) { clearTimeout(_ringTimer); _ringTimer = null; }
  if (_ringGain && _ringAC) {
    try { _ringGain.gain.cancelScheduledValues(_ringAC.currentTime); } catch(_) {}
    try { _ringGain.gain.linearRampToValueAtTime(0, _ringAC.currentTime + 0.02); } catch(_) {}
  }
  // 不销毁 AC，留着下次用；彻底清理交给 _destroyRingTone
}
function _destroyRingTone() {
  stopRingTone();
  if (_ringAC) { try { _ringAC.close(); } catch(_) {} _ringAC = null; _ringGain = null; }
}
// SIP realm/domain — 优先读面 Realm 输入框，fallback 到 localStorage，最后 fallback nsbc.local
// 多Tenant场景：realm = Tenant名（tenant_name），每个Tenant可以有独立的 SIP 域
function getSipDomain() {
  const dom = document.getElementById('realm');
  if (dom && dom.value && dom.value.trim()) {
    const v = dom.value.trim();
    try { localStorage.setItem('nsbc_sip_domain', v); } catch(_) {}
    return v;
  }
  return localStorage.getItem('nsbc_sip_domain') || 'nsbc.local';
}
const PC_CONFIG = {
  iceServers: [],
  iceTransportPolicy: 'all',
  bundlePolicy: 'max-bundle',
  rtcpMuxPolicy: 'require',
};
const AUDIO_KEEP_MAIN = new Set(['pcma']);
const VIDEO_KEEP_MAIN = new Set(['h264', 'vp9']);

// ============================================================================
// 【工具函数】fixVideoPortForLocal — 必须在所有 patch 之前定义
// ============================================================================
function fixVideoPortForLocal(sdp) {
  const lines = sdp.split(/\r\n|\n/);
  let validPort = null;
  for (const line of lines) {
    const m = line.match(/^m=\w+ (\d+)/);
    if (m && m[1] !== '0' && m[1] !== '9') {
      validPort = m[1];
      break;
    }
  }
  if (!validPort) {
    console.log('[NSBC] 🔧 fixVideoPort: no valid port found, skip');
    return sdp;
  }
  let fixed = 0;
  for (let i = 0; i < lines.length; i++) {
    if (/^m=(audio|video) 9 /.test(lines[i])) {
      lines[i] = lines[i].replace(/^m=(audio|video) 9 /, 'm=$1 ' + validPort + ' ');
      fixed++;
    }
  }
  if (fixed > 0) {
    console.log('[NSBC] 🔧 fixVideoPort: ' + fixed + ' m-lines 9→' + validPort);
  }
  return lines.join('\r\n');
}

// ============================================================================
// 【修复 0】patch addTrack — 同一 track 重复加，直接返回已有 sender
// ============================================================================
(function() {
  if (!window.RTCPeerConnection) return;
  const orig = RTCPeerConnection.prototype.addTrack;
  RTCPeerConnection.prototype.addTrack = function(track, ...streams) {
    const existing = this.getSenders().find(s => s.track && s.track.id === track.id);
    if (existing) {
      console.log('[NSBC] 🔧 addTrack skip (already exists):', track.kind, track.id);
      return existing;
    }
    console.log('[NSBC] 🔧 addTrack:', track.kind, track.id);
    return orig.call(this, track, ...streams);
  };
  console.log('[NSBC] ✅ patched addTrack');
})();

// ============================================================================
// 【修复】patch setLocalDescription — 诊断用（仅打 m-lines Log）
// ============================================================================
(function() {
  if (!window.RTCPeerConnection) return;
  const orig = RTCPeerConnection.prototype.setLocalDescription;
  RTCPeerConnection.prototype.setLocalDescription = function(description) {
    if (description?.sdp) {
      const mlines = description.sdp.split(/\r\n|\n/).filter(l => l.startsWith('m='));
      console.log('[NSBC] 📋 setLocalDescription m-lines:', mlines.join(' | '));
    }
    return orig.call(this, description);
  };
  console.log('[NSBC] ✅ patched setLocalDescription (diagnostic only)');
})();

// ============================================================================
// SDP codec 白名单
// ============================================================================
function sdpCodecWhitelist(description) {
  if (!description?.sdp) return description;
  const lines = description.sdp.split(/\r\n|\n/);

  const ptToCodec = new Map();
  const aptToRedPts = new Map();
  for (const line of lines) {
    const rm = line.match(/^a=rtpmap:(\d+)\s+([^/\s]+)/i);
    if (rm) ptToCodec.set(rm[1], rm[2].toLowerCase());
    const fm = line.match(/^a=fmtp:(\d+)\s+apt=(\d+)/i);
    if (fm) {
      const redPt = fm[1], mainPt = fm[2];
      if (!aptToRedPts.has(mainPt)) aptToRedPts.set(mainPt, []);
      aptToRedPts.get(mainPt).push(redPt);
    }
  }

  const mKeepPts = [];
  let curMediaIdx = -1;
  let curKind = null;
  for (const line of lines) {
    if (line.startsWith('m=')) {
      curMediaIdx++;
      curKind = line.startsWith('m=audio ') ? 'audio'
               : line.startsWith('m=video ') ? 'video' : null;
      if (curKind) {
        const parts = line.split(' ');
        const pts = parts.slice(3);
        const mainKeep = curKind === 'audio' ? AUDIO_KEEP_MAIN : VIDEO_KEEP_MAIN;
        const keep = new Set();
        for (const pt of pts) {
          const codec = ptToCodec.get(pt);
          if (!codec) continue;
          if (mainKeep.has(codec)) {
            keep.add(pt);
            const reds = aptToRedPts.get(pt);
            if (reds) reds.forEach(r => keep.add(r));
          }
        }
        if (curKind === 'audio') {
          for (const pt of pts) {
            if (ptToCodec.get(pt) === 'telephone-event') keep.add(pt);
          }
        }
        mKeepPts.push(keep);
      } else {
        mKeepPts.push(null);
      }
    }
  }

  curMediaIdx = -1;
  curKind = null;
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];

    if (line.startsWith('m=')) {
      curMediaIdx++;
      curKind = line.startsWith('m=audio ') ? 'audio'
               : line.startsWith('m=video ') ? 'video' : null;
      const keep = mKeepPts[curMediaIdx];
      if (keep) {
        const parts = line.split(' ');
        const allPts = parts.slice(3);
        const filtered = allPts.filter(pt => keep.has(pt));
        if (filtered.length > 0) {
          lines[i] = [...parts.slice(0, 3), ...filtered].join(' ');
        }
      }
      continue;
    }

    if (line.startsWith('a=extmap-allow-mixed') || line.startsWith('a=rtcp-xr:')) {
      lines.splice(i, 1); i--; continue;
    }

    const attrMatch = line.match(/^a=(?:rtpmap|fmtp|rtcp-fb):(\d+)/);
    if (attrMatch && curKind) {
      const pt = attrMatch[1];
      const keep = mKeepPts[curMediaIdx];
      if (keep && !keep.has(pt)) {
        lines.splice(i, 1); i--;
      }
    }
  }

  const mlines = lines.filter(l => l.startsWith('m='));
  console.log(`[NSBC] 📋 SDP after whitelist type=${description.type} (${mlines.length} media):`);
  mlines.forEach(m => console.log(`  ${m}`));

  return { sdp: lines.join('\r\n'), type: description.type };
}

// ============ Log ============
function log(cls, msg) {
  const ts = new Date().toLocaleTimeString();
  const div = document.createElement('div');
  // 安全：msg 来自 SIP 远端 reason_phrase/cause 等，必须 textContent 防 XSS
  const tsSpan = document.createElement('span');
  tsSpan.className = 'ts'; tsSpan.textContent = `[${ts}]`;
  const msgSpan = document.createElement('span');
  msgSpan.className = cls; msgSpan.textContent = msg;
  div.appendChild(tsSpan); div.appendChild(msgSpan);
  const box = $('sipLog');
  if (box) { box.appendChild(div); box.scrollTop = box.scrollHeight;
    while (box.children.length > 500) box.removeChild(box.firstChild); }
  const cmap = { info: 'log', ok: 'log', warn: 'warn', err: 'error' };
  const fn = cmap[cls] || 'log';
  console[fn](`[NSBC] ${msg}`);
}
function dcLog(text, incoming) {
  const box = $('dcLog');
  if (!box) return;
  const div = document.createElement('div');
  div.className = incoming ? 'in' : 'out';
  div.textContent = (incoming ? '◀ ' : '▶ ') + text;
  box.appendChild(div); box.scrollTop = box.scrollHeight;
}

// ============ SIP 消息Log ============
function onSipMessage(e) {
  try {
    if (e.direction === 'incoming') {
      if (e.response) log('rx', `← ${e.response.status_code} ${e.response.reason_phrase}`);
      else if (e.request) log('rx', `← ${e.request.method}`);
    } else if (e.direction === 'outgoing') {
      if (e.request) log('tx', `→ ${e.request.method}`);
      else if (e.response) log('tx', `→ ${e.response.status_code} ${e.response.reason_phrase}`);
    }
  } catch(_) {}
}

// ============ SDP 过滤事件 ============
function handleSdpEvent(e) {
  if (!e || !e.sdp) return;
  const mlines = e.sdp.split(/\r\n/).filter(l => l.startsWith('m='));
  const mports = mlines.map(l => {
    const m = l.match(/^m=(\w+) (\d+)/);
    return m ? `${m[1]}:${m[2]}` : '?';
  }).join(' ');
  log('info', `📨 SDP ${e.originator}/${e.type} ports: [${mports}] (${mlines.length} media)`);

  if (e.originator !== 'local') return;
  try {
    e.sdp = fixVideoPortForLocal(e.sdp);
    const filtered = sdpCodecWhitelist({ sdp: e.sdp, type: e.type });
    e.sdp = filtered.sdp;
    const after = e.sdp.split(/\r\n/).filter(l => l.startsWith('m='));
    const mports2 = after.map(l => {
      const m = l.match(/^m=(\w+) (\d+)/);
      return m ? `${m[1]}:${m[2]}` : '?';
    }).join(' ');
    log('info', `🔧 sdp(${e.originator}/${e.type}): 修复后 ports: [${mports2}]`);
  } catch (err) {
    console.warn('[NSBC] SDP filter failed:', err);
  }
}

// ============ Chrome autoplay 策略 + play() 竞态修复 ============
const _playingEls = new WeakMap();
function tryPlayWithGesture(el) {
  if (!el) return;
  if (!el.paused && el.readyState >= 2) return;  // 已经在播了
  if (_playingEls.has(el)) return;                // 已有 pending play()
  
  const p = el.play();
  if (p && typeof p.then === 'function') {
    _playingEls.set(el, p);
    p.then(() => {
      _playingEls.delete(el);
      console.log('[NSBC] ✅ 播放Success:', el.id);
    }).catch(err => {
      _playingEls.delete(el);
      console.warn('[NSBC] ⚠️ play() 失败:', el.id, err.name, err.message);
      // ★ AbortError 自动重试 500ms 后
      if (err.name === 'AbortError') {
        console.log('[NSBC] 🔁 AbortError 自动重试 500ms 后...');
        setTimeout(() => tryPlayWithGesture(el), 500);
      }
    });
  }
}

// ============ PC 装配 ============
function attachPC(pc, session) {
  if (!pc) return;
  window.__pc = pc;

  if (!remoteStream) remoteStream = new MediaStream();
  pc.ontrack = (e) => {
    log('ok', `🎬 ONTRACK: ${e.track.kind} label=${e.track.label} enabled=${e.track.enabled}`);
    if (!remoteStream.getTrackById(e.track.id)) {
      remoteStream.addTrack(e.track);
    }
    $('remoteDot').className = 'dot on';
    // ★ 立即绑 srcObject — 等 connectionState connected 可能因 autoplay 拒掉 play()
    const rv = $('remoteVideo');
    if (rv && rv.srcObject !== remoteStream) {
      rv.srcObject = remoteStream;
      window.__nsbcRvBound = true;
      try { rv.muted = false; rv.unmute && rv.unmute(); } catch(_) {}
      tryPlayWithGesture(rv);
    }
  };

  pc.ondatachannel = (e) => {
    log('info', `📥 DC inbound: ${e.channel.label}`);
    dc = e.channel;
    bindDC();
  };

  pc.onconnectionstatechange = () => {
    const st = pc.connectionState;
    log(st === 'connected' || st === 'completed' ? 'ok' : 'info', `📶 PC conn: ${st}`);
    if (st === 'connected' || st === 'completed') {
      dumpMediaStats(pc, 'connected');
      // ★ Media管道完全就绪后，再统一绑 srcObject + play()
      // 此时 DTLS/SRTP 已建立，Chrome 不会再因内部 load() 打断 play()
      const rv = $('remoteVideo');
      if (rv && remoteStream && remoteStream.getTracks().length > 0) {
        if (!window.__nsbcRvBound) {
          rv.srcObject = remoteStream;
          window.__nsbcRvBound = true;
          log('info', `📺 remoteVideo.srcObject 绑定 (${remoteStream.getTracks().length} tracks)`);
        }
        tryPlayWithGesture(rv);
      }
    }
    if (st === 'failed' || st === 'disconnected') {
      log('err', `❌ PC conn ${st}`);
      dumpMediaStats(pc, `fail-${st}`);
    }
  };
  pc.oniceconnectionstatechange = () => log('info', `🧊 ICE: ${pc.iceConnectionState}`);
  pc.onicegatheringstatechange = () => log('info', `🧊 ICE gather: ${pc.iceGatheringState}`);
  pc.onsignalingstatechange = () => log('info', `📡 signaling: ${pc.signalingState}`);

  // ★★★ Incoming路径关Key兜底 ★★★
  // JsSIP answer() 内部先 pc.setRemoteDescription(invite.sdp) → 浏览器触发 ontrack
  // 然后才 emit('peerconnection') → 我们 attachPC Register ontrack — 时序反了！
  // 解决：attachPC Register ontrack 后，立即扫一遍已有的 receivers，把漏掉的 track 补回来
  setTimeout(() => {
    const existing = pc.getReceivers().filter(r => r.track).length;
    const ours = remoteStream.getTracks().length;
    log('info', `🔍 receiver 兜底: pc.getReceivers=${existing} remoteStream.tracks=${ours} sig=${pc.signalingState}`);
    if (existing > ours) {
      pc.getReceivers().forEach(r => {
        if (r.track && !remoteStream.getTrackById(r.track.id)) {
          log('ok', `🎬 RECOVER: 从 receiver 找回 track kind=${r.track.kind} id=${r.track.id}`);
          remoteStream.addTrack(r.track);
        }
      });
      $('remoteDot').className = 'dot on';
      const rv = $('remoteVideo');
      if (rv && rv.srcObject !== remoteStream) {
        rv.srcObject = remoteStream;
        window.__nsbcRvBound = true;
        try { rv.muted = false; rv.unmute && rv.unmute(); } catch(_) {}
        tryPlayWithGesture(rv);
      }
    }
  }, 10);
}

function bindRemoteTracks(pc) {
  if (!pc) return;
  if (!remoteStream) remoteStream = new MediaStream();
  let added = 0;
  pc.getReceivers().forEach(r => {
    if (r.track && !remoteStream.getTrackById(r.track.id)) {
      remoteStream.addTrack(r.track);
      added++;
    }
  });
  if (added > 0) {
    log('info', `📥 bindRemoteTracks: 兜底 addTrack ${added} 个`);
  }
  if (remoteStream.getTracks().length > 0) {
    $('remoteDot').className = 'dot on';
  }
}

function setupLocalPreview(pc) {
  if (!pc) return;
  const senders = pc.getSenders();
  if (!localStream) localStream = new MediaStream();
  senders.forEach(s => {
    if (s.track && !localStream.getTrackById(s.track.id)) {
      localStream.addTrack(s.track);
    }
  });
  if (localStream.getTracks().length > 0) {
    $('localVideo').srcObject = localStream;
    $('localDot').className = 'dot on';
    log('ok', `📺 本地流 OK (${localStream.getTracks().length} tracks)`);
  }
  const sndDescr = senders.map(s => s.track ? `${s.track.kind}[${s.track.label||'?'}]` : 'null').join(',');
  log('info', `📤 SENDERS(${senders.length}): ${sndDescr}`);
}

async function ensureVideoReady(stream, timeoutMs = 2500) {
  const v = stream.getVideoTracks()[0];
  if (!v) { log('warn', '⚠️ 流里没有 video track'); return; }
  if (v.readyState === 'live' && !v.muted) {
    log('ok', `🎥 video track Ready (readyState=live, muted=false)`);
    return;
  }
  log('warn', `⏳ 等待 video unmute (readyState=${v.readyState}, muted=${v.muted})`);
  await new Promise(resolve => {
    const done = (why) => {
      log('ok', `🎥 video track ${why} (readyState=${v.readyState}, muted=${v.muted})`);
      resolve();
    };
    const timer = setTimeout(() => done('超时'), timeoutMs);
    v.addEventListener('unmute', () => { clearTimeout(timer); done('unmute'); }, { once: true });
  });
}

function makeSessionHandlers(isCallee) {
  return {
    peerconnection: (data) => {
      const pc = data.peerconnection;
      log('info', `🔗 peerconnection event (callee=${isCallee})`);

      if (localStream) {
        const existingIds = new Set(
          pc.getSenders().map(s => s.track?.id).filter(Boolean)
        );
        let added = 0;
        localStream.getTracks().forEach(track => {
          if (existingIds.has(track.id)) return;
          try {
            pc.addTrack(track, localStream);
            added++;
            log('ok', `➕ pre-offer addTrack ${track.kind} (${track.label || '?'})`);
          } catch(e) {
            log('warn', `pre-offer addTrack ${track.kind} 失败: ${e.message}`);
          }
        });
        log('info', `📤 senders after addTrack: ${pc.getSenders().length} (+${added})`);
      }

      pc.addEventListener('negotiationneeded', () =>
        log('warn', '⚠️ negotiationneeded fired'));

      attachPC(pc, null);

      const senders = pc.getSenders();
      log('info', `📤 PC senders: [${senders.map(s=>s.track?.kind||'null').join(',')}] total=${senders.length}`);
    },
  };
}

function onSessionConfirmed(session, isCallee) {
  currentSession = session;
  pendingSession = null;
  const cs = $('callStatus'); if (cs) cs.textContent = 'Active ✅';
  const bh = $('btnHang'); if (bh) bh.disabled = false;
  const inc = $('incoming'); if (inc) inc.classList.remove('show');

  const pc = session.connection;
  if (pc) {
    setupLocalPreview(pc);
    bindRemoteTracks(pc);
    if (!isCallee && !dc) {
      try {
        dc = pc.createDataChannel('nsbc-chat');
        log('info', '📤 DC created');
        bindDC();
      } catch (err) {
        log('warn', 'DC create: ' + err.message);
      }
    }
  }
}

function bindSessionNonPC(session, isCallee, skipPeerConn) {
  if (!skipPeerConn) {
    session.on('peerconnection', makeSessionHandlers(isCallee).peerconnection);
  }
  session.on('sdp', handleSdpEvent);
  session.on('progress', (e) => {
    const r = e.response;
    const code = r?.status_code || 0;
    const phrase = r?.reason_phrase || '';
    log('info', `📞 progress: ${code} ${phrase}`);

    // ★ 区分 180 vs 183（RFC 3261 §21.1.2 / §21.1.5）
    if (code === 180) {
      // 180 Ringing — 对端开始Ringing，无Early media；播本地自定义Ringing音
      if (!isCallee) playRingTone();
    } else if (code === 183) {
      // 183 Session Progress — 对端可能带Early media（SDP 已在 183 里）
      // 停本地Ringing，让远端 early media（如 MOH / 提前语音）透传
      stopRingTone();
      log('info', `📡 183 → Remote early media takeover`);
    }
  });
  session.on('accepted', () => {
    log('ok', '✅ accepted (180/183)');
    stopRingTone();   // 对方接了，不管之前是 180 还是 183，Ringing都停
  });
  session.on('confirmed', () => {
    log('ok', '✅ confirmed (Established)');
    stopRingTone();
    onSessionConfirmed(session, isCallee);
  });
  session.on('ended', (e) => {
    stopRingTone();
    const org = e && e.originator;
    log('info', `📴 ended (originator=${org})`);
    onCallEnd(org === 'remote' ? '对端Hang up' : 'Local hangup');
  });
  session.on('failed', (e) => {
    stopRingTone();
    log('err', `❌ failed: ${e.cause || ''}`);
    onCallEnd('失败: ' + (e.cause || ''));
  });
  session.on('getusermediafailed', (e) => log('err', '❌ getUserMedia FAIL: ' + (e.error?.message || e.error)));
  session.on('peerconnection:createofferfailed', (e) => log('err', '❌ createOffer FAIL: ' + (e.error?.message || e.error)));
  session.on('peerconnection:createanswerfailed', (e) => log('err', '❌ createAnswer FAIL: ' + (e.error?.message || e.error)));
  session.on('peerconnection:setlocaldescriptionfailed', (e) => log('err', '❌ setLocalDesc FAIL: ' + (e.error?.message || e.error)));
  session.on('peerconnection:setremotedescriptionfailed', (e) => log('err', '❌ setRemoteDesc FAIL: ' + (e.error?.message || e.error)));
}

// ============ 自动推导 WSS / STUN ============
(function autoDeriveServers() {
  const host = window.location.hostname;
  const wssEl = document.getElementById('wss');
  const stunEl = document.getElementById('stun');
  if (!wssEl || !stunEl) return;
  if (host) {
    wssEl.value = 'wss://' + host + ':5062';
    stunEl.value = 'stun:' + host + ':3478';
    console.log('[NSBC] 自动探测 host=' + host + ' → WSS=' + wssEl.value);
  } else {
    wssEl.placeholder = '手动填写 (如 wss://10.0.0.80:5062)';
    stunEl.placeholder = '手动填写 (如 stun:10.0.0.80:3478)';
  }
})();

// ============ Register ============
async function doRegister() {
  if (!window.JsSIP) { alert('JsSIP 未加载！请检查 jssip-3.10.0.min.js'); return; }
  const rs = $('regStatus'); if (rs) { rs.textContent = 'Connect中...'; rs.className = 'status'; }
  const br = $('btnReg'); if (br) br.disabled = true;

  // ★ 预热 AudioContext——Register 点击是用户手势，能解锁 autoplay 策略，
  // 之后 INVITE 到达（非手势）时 playRingTone 才真正出声
  try {
    if (!_ringAC) _ringAC = new (window.AudioContext || window.webkitAudioContext)();
    if (_ringAC.state === 'suspended') _ringAC.resume();
  } catch (_) {}

  try {
    const user = $('user').value.trim();
    const pass = $('pass').value;
    const wssUrl = $('wss').value.trim();

    const socket = new JsSIP.WebSocketInterface(wssUrl);

    ua = new JsSIP.UA({
      sockets: [socket],
      uri: `sip:${user}@${getSipDomain()}`,
      password: pass,
      register: false,
      session_timers: false,
      pcConfig: PC_CONFIG,
      connection_recovery_min_interval: 2,
      connection_recovery_max_interval: 30,
    });

    ua.on('connecting',   () => log('info', '🔌 JsSIP connecting...'));
    ua.on('connected',    () => { log('ok', '🔌 JsSIP connected'); ua.register(); });
    ua.on('disconnected', () => log('warn', '🔌 JsSIP disconnected'));

    ua.on('registered', () => {
      const rs2 = $('regStatus'); if (rs2) { rs2.textContent = '✅ Registered ' + user; rs2.className = 'status ok'; }
      const bu = $('btnUnreg'); if (bu) bu.disabled = false;
      const bc = $('btnCall'); if (bc) bc.disabled = false;
      log('ok', `REGISTERED as ${user}`);
    });
    ua.on('unregistered', () => {
      const rs3 = $('regStatus'); if (rs3) { rs3.textContent = 'Not registered'; rs3.className = 'status'; }
      const bu2 = $('btnUnreg'); if (bu2) bu2.disabled = true;
      const bc2 = $('btnCall'); if (bc2) bc2.disabled = true;
      log('info', 'UNREGISTERED');
    });
    ua.on('registrationFailed', (e) => {
      const rs4 = $('regStatus'); if (rs4) { rs4.textContent = '❌ Register失败'; rs4.className = 'status err'; }
      const br2 = $('btnReg'); if (br2) br2.disabled = false;
      log('err', `REGISTRATION FAILED: ${e.cause}`);
    });

    ua.on('sip', onSipMessage);

    ua.on('newRTCSession', (e) => {
      const session = e.session;
      const from = session.remote_identity?.uri?.toString() || '?';

      if (e.originator === 'remote') {
        log('warn', `📞 INCOMING from ${from}`);
        pendingSession = session;
        const inf = $('inFrom'); if (inf) inf.textContent = from;
        const inc = $('incoming'); if (inc) inc.classList.add('show');
        bindSessionNonPC(session, true, false);
        // ★ Incoming铃声：INVITE 到达即播放（accept/reject/failed/ended 都会 stopRingTone）
        playRingTone();
        // ★ 兜底：JsSIP 在触发 newRTCSession 事件之前，就已经Create pc
        // 并 emit('peerconnection', pc) + setRemoteDescription 了，
        // 此时我们的 session.on('peerconnection') 还没Register → attachPC 永远不调 → ontrack 丢失！
        // 检查 session.connection 已存在 → 立即手动 attachPC 一次
        const incomingPC = session.connection;
        if (incomingPC) {
          log('info', `📶 callee: pc 已存在 (sig=${incomingPC.signalingState}) — 手动 attachPC 兜底`);
          attachPC(incomingPC, session);
        }
      } else {
        log('info', `📤 outgoing to ${from}`);
        // ★ Outgoing call也要存 session + 开 Hangup 按钮，让Ringing阶段能Hang up
        pendingSession = session;
        const bh2 = $('btnHang'); if (bh2) bh2.disabled = false;
        bindSessionNonPC(session, false, true);
      }
    });

    ua.start();
    log('info', 'JsSIP UA 已启动，等待 WebSocket Connect...');
  } catch(e) {
    const rs5 = $('regStatus'); if (rs5) { rs5.textContent = '❌ 异常'; rs5.className = 'status err'; }
    const br3 = $('btnReg'); if (br3) br3.disabled = false;
    log('err', 'EXCEPTION: ' + (e.message || e));
  }
}

async function doUnregister() {
  try { if (ua) ua.unregister(); } catch(e) { log('warn', 'unreg err: ' + e.message); }
  setTimeout(() => {
    try { if (ua) ua.stop(); } catch(e) { log('warn', 'stop err: ' + e.message); }
    ua = null; currentSession = null; pendingSession = null;
    stopMedia(); closePC();
  }, 500);
}

// ============ Outgoing call ============
async function doCall() {
  if (!ua) return;
  let target = $('target').value.trim();
  if (!target) { log('err', 'Callee为空'); return; }
  if (!target.startsWith('sip:')) target = 'sip:' + target;
  if (!target.includes('@')) target = target + '@' + getSipDomain();

  log('tx', `📤 INVITE → ${target}`);
  const cs = $('callStatus'); if (cs) cs.textContent = 'Calling...';
  const bc = $('btnCall'); if (bc) bc.disabled = true;

  try {
    if (!localStream) {
      localStream = await navigator.mediaDevices.getUserMedia(
        { audio: true, video: true });
      await ensureVideoReady(localStream);
      $('localVideo').srcObject = localStream;
      $('localDot').className = 'dot on';
      log('ok', `📺 本地流就绪 (${localStream.getTracks().length} tracks)`);
    }
  } catch(e) {
    log('err', '❌ getUserMedia FAIL: ' + e.message);
    if (cs) cs.textContent = '❌ 无Media';
    if (bc) bc.disabled = false;
    return;
  }
  await new Promise(r => setTimeout(r, 0));
  try {
    ua.call(target, {
        mediaStream: localStream,
        pcConfig: PC_CONFIG,
        eventHandlers: makeSessionHandlers(false),
    });
  } catch(e) {
    if (cs) cs.textContent = '❌ Call失败';
    if (bc) bc.disabled = false;
    log('err', 'CALL FAIL: ' + (e.message || e));
  }
}

// ============ Accept ============
async function doAccept() {
  if (!pendingSession) return;
  const inc = $('incoming'); if (inc) inc.classList.remove('show');
  log('ok', '✅ ACCEPT');
  const cs = $('callStatus'); if (cs) cs.textContent = 'Accept中...';

  try {
    if (!localStream) {
      localStream = await navigator.mediaDevices.getUserMedia(
        { audio: true, video: true });
      await ensureVideoReady(localStream);
      $('localVideo').srcObject = localStream;
      $('localDot').className = 'dot on';
      log('ok', `📺 本地流就绪 (${localStream.getTracks().length} tracks)`);
    }
  } catch(e) {
    log('err', '❌ getUserMedia FAIL: ' + e.message);
    if (cs) cs.textContent = '❌ 无Media';
    return;
  }

  try {
    pendingSession.answer({
        mediaStream: localStream,
        pcConfig: PC_CONFIG,
        });
  } catch(e) {
    log('err', 'ACCEPT FAIL: ' + (e.message || e));
  }
}

// ============ 拒接 ============
function doReject() {
  if (pendingSession) {
    try {
      pendingSession.terminate({ status_code: 486, reason_phrase: 'Busy Here' });
    } catch(e) {}
    pendingSession = null;
  }
  const inc = $('incoming'); if (inc) inc.classList.remove('show');
  log('info', 'REJECTED');
}

// ============ Hang up ============
async function doHangup() {
  if (isHangingUp) { log('warn', '⚠️ Hang up进行中，忽略重复点击'); return; }
  isHangingUp = true;

  log('info', `📞 HANGUP REQ current=${!!currentSession} pending=${!!pendingSession}`);
  try {
    const s = currentSession || pendingSession;
    if (s) {
      s.terminate();
    } else {
      log('warn', '⚠️ 无Session可Hang up');
    }
  } catch(e) {
    log('err', 'HANGUP FAIL: ' + (e.message || e));
  }
  onCallEnd('Local hangup');
  isHangingUp = false;
}

function onCallEnd(reason) {
  _destroyRingTone();   // ★ 彻底停Ringing + 关闭 AudioContext，防止资源残留
  log('info', 'CALL END: ' + reason);
  const cs = $('callStatus'); if (cs) cs.textContent = 'Idle';
  const bc = $('btnCall'); if (bc) bc.disabled = !ua;
  const bh = $('btnHang'); if (bh) bh.disabled = true;
  closePC(); stopMedia();
  currentSession = null; pendingSession = null;
  isHangingUp = false;
  // --- nsbc-phone enhancer: reset hold state ---
  if (typeof nsbcPhone !== 'undefined') { nsbcPhone.reset(); }
  const hb = document.getElementById('btnHold');
  if (hb) { hb.textContent = '⏸ Hold'; hb.classList.remove('btn-held'); }
}

// ============ Media控制 ============
function stopMedia() {
  window.__nsbcRvBound = false;  // Reset，让下次通话能重新绑定 srcObject
  _playingEls.delete($('remoteVideo'));
  _playingEls.delete($('localVideo'));

  if (localStream) {
    localStream.getTracks().forEach(t => { try { t.stop(); } catch(_) {} });
    localStream = null;
  }
  if ($('localVideo')) $('localVideo').srcObject = null;
  if ($('remoteVideo')) $('remoteVideo').srcObject = null;
  remoteStream = null;
  $('localDot').className = 'dot off'; $('remoteDot').className = 'dot off';
}

function closePC() {
  if (dc) { try { dc.close(); } catch(_) {} dc = null; }
  dcOpen = false;
  const dm = $('dcMsg'); if (dm) dm.disabled = true;
  const bd = $('btnDc'); if (bd) bd.disabled = true;

  const pc = currentSession?.connection;
  if (pc && pc.signalingState !== 'closed') {
    try { pc.close(); log('info', '🛑 PeerConnection closed'); } catch(_) {}
  }
}

function muteAudio() {
  if (!localStream) return;
  const t = localStream.getAudioTracks()[0];
  if (!t) return;
  t.enabled = !t.enabled;
  $('btnMute').textContent = t.enabled ? '🔊 Mute' : '🔇 Unmute';
}
function stopVideo() {
  if (!localStream) return;
  const t = localStream.getVideoTracks()[0];
  if (!t) return;
  t.enabled = !t.enabled;
  $('btnStopVid').textContent = t.enabled ? '📷 Stop Video' : '📷 Start Video';
}

// ============ DataChannel ============
function bindDC() {
  if (!dc) return;
  if (dc.readyState === 'open') {
    dcOpen = true;
    const dm = $('dcMsg'); if (dm) dm.disabled = false;
    const bd = $('btnDc'); if (bd) bd.disabled = false;
    log('ok', '📨 DC Already OPEN');
    return;
  }
  dc.onopen = () => {
    dcOpen = true;
    const dm = $('dcMsg'); if (dm) dm.disabled = false;
    const bd = $('btnDc'); if (bd) bd.disabled = false;
    log('ok', '📨 DC OPEN');
    dcLog('[system] DataChannel 已Connect', false);
    const dm2 = $('dcMsg'); if (dm2) dm2.focus();
  };
  dc.onclose = () => {
    dcOpen = false;
    const dm = $('dcMsg'); if (dm) dm.disabled = true;
    const bd = $('btnDc'); if (bd) bd.disabled = true;
    log('info', '📨 DC CLOSED');
    dcLog('[system] DataChannel 已Disconnect', false);
  };
  dc.onmessage = (e) => dcLog(e.data, true);
  dc.onerror = (e) => log('err', 'DC error: ' + e);
}
function dcSend() {
  if (!dc || dc.readyState !== 'open') return;
  const msg = $('dcMsg').value.trim();
  if (!msg) return;
  dc.send(msg);
  dcLog(msg, false);
  $('dcMsg').value = '';
}

// ============ Media Stats ============
async function dumpMediaStats(pc, tag) {
  try {
    const stats = await pc.getStats();
    let rtpSent = 0, rtpRecv = 0;
    let pktSent = 0, pktRecv = 0;
    let decErr = 0, ploss = 0;
    let dtlsState = '?';

    stats.forEach(s => {
      if (s.type === 'transport') dtlsState = s.dtlsState || '?';
      if (s.type === 'remote-inbound-rtp' || s.type === 'inbound-rtp') {
        rtpRecv += s.bytesReceived || 0;
        pktRecv += s.packetsReceived || 0;
        decErr += s.decodeErrors || 0;
        ploss += s.packetsLost || 0;
      }
      if (s.type === 'outbound-rtp') {
        rtpSent += s.bytesSent || 0;
        pktSent += s.packetsSent || 0;
      }
    });

    const mediaFlowing = (rtpSent > 0 || rtpRecv > 0) ? '✅ MEDIA' : '❌ NO MEDIA';
    log('info', `📊 STATS[${tag}] ${mediaFlowing} DTLS=${dtlsState}
      RTP sent=${rtpSent}B pkt=${pktSent} recv=${rtpRecv}B pkt=${pktRecv}
      decodeErr=${decErr} pktLost=${ploss}`);
  } catch(e) {
    log('warn', `📊 STATS[${tag}] fail: ${e.message}`);
  }
}

// ============ 面关闭 ============
window.addEventListener('beforeunload', () => {
  try { if (currentSession) currentSession.terminate(); } catch(_) {}
  try { if (pendingSession) pendingSession.terminate(); } catch(_) {}
  try { if (ua) ua.stop(); } catch(_) {}
  stopMedia(); closePC();
});

// =============================================================================
// ★ nsbc-phone 增强层 — Hold / Unhold / DTMF / 拨号盘辅助
// =============================================================================
const nsbcPhone = {
  _held: false,

  hold() {
    const s = currentSession;
    if (!s) { console.warn('[nsbc] hold: no session'); return false; }
    if (this._held) return true;
    try {
      if (typeof s.hold === 'function') {
        s.hold();
        this._held = true;
        console.log('[nsbc] HOLD via session.hold()');
        return true;
      }
    } catch (e) {
      console.warn('[nsbc] session.hold() failed:', e.message);
    }
    if (localStream) {
      const at = localStream.getAudioTracks()[0];
      if (at) { at.enabled = false; this._held = true; console.log('[nsbc] HOLD fallback mute'); return true; }
    }
    return false;
  },

  unhold() {
    const s = currentSession;
    if (!s) return false;
    if (!this._held) return true;
    try {
      if (typeof s.unhold === 'function') {
        s.unhold();
        this._held = false;
        console.log('[nsbc] UNHOLD via session.unhold()');
        return true;
      }
    } catch (e) {
      console.warn('[nsbc] session.unhold() failed:', e.message);
    }
    if (localStream) {
      const at = localStream.getAudioTracks()[0];
      if (at) { at.enabled = true; this._held = false; console.log('[nsbc] UNHOLD fallback unmute'); return true; }
    }
    return false;
  },

  isHeld() { return this._held; },

  sendDTMF(tone) {
    const valid = '0123456789*#';
    if (!tone || valid.indexOf(tone) < 0) return false;
    const s = currentSession;
    if (!s) { console.warn('[nsbc] DTMF: no session'); return false; }

    try {
      if (typeof s.sendDTMF === 'function') {
        s.sendDTMF({ tone: tone, duration: 120, interToneGap: 50 });
        console.log('[nsbc] 📟 DTMF:', tone, '(SIP sendDTMF)');
        this._flashDTMF(tone);
        return true;
      }
    } catch (e) { console.warn('[nsbc] sendDTMF failed:', e.message); }

    try {
      const pc = s.connection;
      if (pc) {
        const sender = pc.getSenders().find(x => x.track?.kind === 'audio' && x.dtmf);
        if (sender?.dtmf) {
          sender.dtmf.insertDTMF(tone, '120', '50');
          console.log('[nsbc] 📟 DTMF:', tone, '(RTCDTMFSender)');
          this._flashDTMF(tone);
          return true;
        }
      }
    } catch (e) { console.warn('[nsbc] RTCDTMFSender failed:', e.message); }

    console.warn('[nsbc] DTMF failed: no available method');
    return false;
  },

  _flashDTMF(tone) {
    const el = document.getElementById('dtmfDisplay');
    if (!el) return;
    el.textContent += tone;
    el.classList.add('flash');
    setTimeout(() => el.classList.remove('flash'), 200);
    if (el.textContent.length > 20) el.textContent = el.textContent.slice(-20);
  },

  reset() { this._held = false; },
};

// ---------- 全局 onclick 桥接 ----------
function doHold() {
  const btn = document.getElementById('btnHold');
  if (nsbcPhone._held) {
    nsbcPhone.unhold();
    if (btn) { btn.textContent = '⏸ Hold'; btn.classList.remove('btn-held'); }
  } else {
    nsbcPhone.hold();
    if (btn) { btn.textContent = '▶️ Resume'; btn.classList.add('btn-held'); }
  }
}

function doDialKey(key) {
  const valid = '0123456789*#';
  // Active → 发 DTMF
  if (currentSession && valid.indexOf(key) >= 0) {
    nsbcPhone.sendDTMF(key);
    return;
  }
  // 未通话 → 号码输入辅助
  const tgt = document.getElementById('target');
  if (!tgt) return;
  if (key === '⌫') { tgt.value = tgt.value.slice(0, -1); }
  else if (key === 'C') { tgt.value = ''; }
  else if (valid.indexOf(key) >= 0) { tgt.value += key; }
}

// ---------- 启动Log（script 在 </body> 前执行，DOM Ready）----------
log('info', '面加载完成 · JsSIP ' + (window.JsSIP ? (window.JsSIP.version || '3.x') + ' ✅' : '❌ 未加载'));
if (window.location.protocol !== 'https:' && window.location.hostname !== 'localhost') {
  log('warn', 'getUserMedia 需要 HTTPS 或 localhost');
}
console.log('[nsbc] ✅ nsbc-phone loaded (full JsSIP + hold/DTMF/dialpad)');
