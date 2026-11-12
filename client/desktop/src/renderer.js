/**
 * 渲染进程：四屏状态机 + 设置持久化 + 最近通话。
 * 通过 preload 暴露的 window.nsbc 读写 userData/config.json。
 */

import { SipClient } from './sip.js';
import { WebRtcMedia } from './media.js';

const RECENTS_KEY = 'nsbc.recents.v1';
const MAX_RECENTS = 30;

const BASE_CONFIG = {
  server: '',
  wssPort: 5062,
  wsPath: '/ws',
  useTls: true,
  realm: '',
  username: '',
  password: '',
  displayName: '',
  stunServer: '',
  turnServer: '',
  turnUsername: '',
  turnCredential: '',
  videoEnabled: true,
  videoWidth: 1280,
  videoHeight: 720,
  videoFps: 30,
  mirrorLocalPreview: true,
  registerExpiresSec: 300,
  autoReconnect: true,
  reconnectDelayMs: 2000,
  allowSelfSignedCert: true,
  icePolicy: 'ALL',
  logLevel: 'INFO'
};

const ids = [
  'app',
  'screenDialer', 'screenSettings', 'screenCall', 'incomingOverlay',
  'statusChip', 'settingsBtn', 'accountText', 'dialedText', 'recentList', 'recentEmpty',
  'key1', 'key2', 'key3', 'key4', 'key5', 'key6', 'key7', 'key8', 'key9',
  'keyStar', 'key0', 'keyHash',
  'backspaceBtn', 'callBtn',
  'serverInput', 'portInput', 'realmInput', 'userInput', 'passInput', 'displayInput',
  'tlsSwitch', 'pathInput', 'stunInput', 'turnInput', 'turnUserInput', 'turnCredInput',
  'iceSelect', 'videoSwitch', 'widthInput', 'heightInput', 'fpsInput', 'mirrorSwitch',
  'reconnectSwitch', 'selfSignedSwitch', 'logSelect', 'problemsText',
  'saveConnectBtn', 'disconnectBtn', 'resetBtn', 'settingsBackBtn',
  'remoteVideo', 'callAvatar', 'callName', 'callSubtitle', 'callTimer',
  'localContainer', 'localVideo', 'muteBtn', 'speakerBtn', 'videoBtn', 'switchBtn', 'hangupBtn',
  'incomingAvatar', 'incomingName', 'incomingUri', 'rejectBtn', 'answerBtn',
  'toast'
];

const el = {};
ids.forEach((id) => {
  el[id] = document.getElementById(id);
});

const bridge = window.nsbc || null;

let appConfig = normalizeConfig(BASE_CONFIG);
let defaults = normalizeConfig(BASE_CONFIG);
let sip = null;
let media = null;
let sessionToken = 0;
let currentCall = null;
let recents = [];
let dialed = '';
let muted = false;
let speakerOn = false;
let videoOn = true;
let remoteHasVideo = false;
let timerHandle = null;
let toastHandle = null;

// ============================================================== 配置工具

function intOf(value, fallback) {
  const n = Number.parseInt(value, 10);
  return Number.isFinite(n) ? n : fallback;
}

function normalizeConfig(raw) {
  const src = raw && typeof raw === 'object' ? raw : {};
  const merged = { ...BASE_CONFIG, ...src };
  return {
    ...merged,
    server: String(merged.server || '').trim(),
    wssPort: intOf(merged.wssPort, 5062),
    wsPath: String(merged.wsPath || '/ws').trim() || '/ws',
    useTls: Boolean(merged.useTls),
    realm: String(merged.realm || '').trim(),
    username: String(merged.username || '').trim(),
    password: String(merged.password || ''),
    displayName: String(merged.displayName || '').trim(),
    stunServer: String(merged.stunServer || '').trim(),
    turnServer: String(merged.turnServer || '').trim(),
    turnUsername: String(merged.turnUsername || '').trim(),
    turnCredential: String(merged.turnCredential || ''),
    videoEnabled: Boolean(merged.videoEnabled),
    videoWidth: intOf(merged.videoWidth, 1280),
    videoHeight: intOf(merged.videoHeight, 720),
    videoFps: intOf(merged.videoFps, 30),
    mirrorLocalPreview: Boolean(merged.mirrorLocalPreview),
    registerExpiresSec: intOf(merged.registerExpiresSec, 300),
    autoReconnect: Boolean(merged.autoReconnect),
    reconnectDelayMs: intOf(merged.reconnectDelayMs, 2000),
    allowSelfSignedCert: Boolean(merged.allowSelfSignedCert),
    icePolicy: String(merged.icePolicy || 'ALL').toUpperCase() === 'RELAY' ? 'RELAY' : 'ALL',
    logLevel: String(merged.logLevel || 'INFO').toUpperCase()
  };
}

/** 与 Android NsbcConfig.validate 对齐。 */
function validateConfig(cfg) {
  const problems = [];
  if (!cfg.server) problems.push('服务器地址不能为空');
  if (!cfg.username) problems.push('用户名 / 分机不能为空');
  if (!cfg.realm) problems.push('域 / Realm 不能为空');
  if (!cfg.password) problems.push('密码不能为空');
  if (!Number.isInteger(cfg.wssPort) || cfg.wssPort < 1 || cfg.wssPort > 65535) {
    problems.push(`WSS 端口超出范围: ${cfg.wssPort}`);
  }
  if (!Number.isInteger(cfg.registerExpiresSec) || cfg.registerExpiresSec < 30 || cfg.registerExpiresSec > 3600) {
    problems.push(`注册有效期建议在 30..3600 秒: ${cfg.registerExpiresSec}`);
  }
  if (cfg.videoEnabled) {
    if (!Number.isInteger(cfg.videoWidth) || cfg.videoWidth < 160 || cfg.videoWidth > 1920) {
      problems.push(`视频宽超出范围（160..1920）: ${cfg.videoWidth}`);
    }
    if (!Number.isInteger(cfg.videoHeight) || cfg.videoHeight < 120 || cfg.videoHeight > 1080) {
      problems.push(`视频高超出范围（120..1080）: ${cfg.videoHeight}`);
    }
    if (!Number.isInteger(cfg.videoFps) || cfg.videoFps < 1 || cfg.videoFps > 60) {
      problems.push(`视频帧率超出范围（1..60）: ${cfg.videoFps}`);
    }
  }
  if (cfg.turnServer && !cfg.turnUsername) problems.push('配置了 TURN 服务器但缺少 TURN 用户名');
  if (cfg.icePolicy === 'RELAY' && !cfg.turnServer) problems.push('仅中继模式必须配置 TURN 服务器');
  return problems;
}

function wsUrl(cfg) {
  const scheme = cfg.useTls ? 'wss' : 'ws';
  const path = cfg.wsPath.startsWith('/') ? cfg.wsPath : `/${cfg.wsPath}`;
  return `${scheme}://${cfg.server}:${cfg.wssPort}${path}`;
}

// ============================================================== 设置读写

function fillSettings(cfg) {
  el.serverInput.value = cfg.server;
  el.portInput.value = String(cfg.wssPort);
  el.realmInput.value = cfg.realm;
  el.userInput.value = cfg.username;
  el.passInput.value = cfg.password;
  el.displayInput.value = cfg.displayName;
  el.tlsSwitch.checked = cfg.useTls;
  el.pathInput.value = cfg.wsPath;
  el.stunInput.value = cfg.stunServer;
  el.turnInput.value = cfg.turnServer;
  el.turnUserInput.value = cfg.turnUsername;
  el.turnCredInput.value = cfg.turnCredential;
  el.iceSelect.value = cfg.icePolicy === 'RELAY' ? 'RELAY' : 'ALL';
  el.videoSwitch.checked = cfg.videoEnabled;
  el.widthInput.value = String(cfg.videoWidth);
  el.heightInput.value = String(cfg.videoHeight);
  el.fpsInput.value = String(cfg.videoFps);
  el.mirrorSwitch.checked = cfg.mirrorLocalPreview;
  el.reconnectSwitch.checked = cfg.autoReconnect;
  el.selfSignedSwitch.checked = cfg.allowSelfSignedCert;
  el.logSelect.value = cfg.logLevel;
}

function buildConfigFromUi() {
  return normalizeConfig({
    ...appConfig,
    server: el.serverInput.value.trim(),
    wssPort: intOf(el.portInput.value, 5062),
    wsPath: el.pathInput.value.trim() || '/ws',
    useTls: el.tlsSwitch.checked,
    realm: el.realmInput.value.trim(),
    username: el.userInput.value.trim(),
    password: el.passInput.value.trim(),
    displayName: el.displayInput.value.trim(),
    stunServer: el.stunInput.value.trim(),
    turnServer: el.turnInput.value.trim(),
    turnUsername: el.turnUserInput.value.trim(),
    turnCredential: el.turnCredInput.value,
    videoEnabled: el.videoSwitch.checked,
    videoWidth: intOf(el.widthInput.value, 1280),
    videoHeight: intOf(el.heightInput.value, 720),
    videoFps: intOf(el.fpsInput.value, 30),
    mirrorLocalPreview: el.mirrorSwitch.checked,
    autoReconnect: el.reconnectSwitch.checked,
    allowSelfSignedCert: el.selfSignedSwitch.checked,
    icePolicy: el.iceSelect.value === 'RELAY' ? 'RELAY' : 'ALL',
    logLevel: el.logSelect.value
  });
}

function updateAccountText() {
  const cfg = buildConfigFromUi();
  el.accountText.textContent = cfg.server
    ? `sip:${cfg.username}@${cfg.realm}  ·  ${wsUrl(cfg)}`
    : '未配置服务器';
}

function showProblems(problems) {
  if (problems.length) {
    el.problemsText.textContent = problems.join('\n');
    el.problemsText.hidden = false;
  } else {
    el.problemsText.hidden = true;
    el.problemsText.textContent = '';
  }
}

async function saveConfigSafe(cfg) {
  if (!bridge) return cfg;
  try {
    return normalizeConfig(await bridge.saveConfig(cfg));
  } catch (err) {
    toast(`配置保存失败：${err.message}`);
    return cfg;
  }
}

async function applySelfSigned(allow) {
  if (!bridge) return;
  try {
    await bridge.setAllowSelfSigned(Boolean(allow));
  } catch (err) {
    toast(`证书开关设置失败：${err.message}`);
  }
}

// ============================================================== 屏幕与注册状态

function showScreen(name) {
  el.screenDialer.hidden = name !== 'dialer';
  el.screenSettings.hidden = name !== 'settings';
  el.screenCall.hidden = name !== 'call';
}

function setRegistration(state, detail) {
  const meta = {
    UNREGISTERED: { label: '未注册', cls: 'chip-gray' },
    REGISTERING: { label: '注册中…', cls: 'chip-orange' },
    REGISTERED: { label: '已注册', cls: 'chip-green' },
    FAILED: { label: '注册失败', cls: 'chip-red' }
  }[state] || { label: '未注册', cls: 'chip-gray' };
  el.statusChip.textContent = meta.label;
  el.statusChip.className = `chip ${meta.cls}`;
  if (state === 'FAILED' && detail) toast(`注册失败：${detail}`);
}

// ============================================================== 连接管理

function connectWithConfig(cfg) {
  const token = ++sessionToken;
  const handlers = {
    onRegistration: (state, detail) => {
      if (token !== sessionToken) return;
      setRegistration(state, detail);
    },
    onIncomingCall: (call) => {
      if (token !== sessionToken) return;
      handleIncomingCall(call);
    },
    onCallState: (call) => {
      if (token !== sessionToken) return;
      handleCallState(call);
    },
    onError: (message) => {
      if (token !== sessionToken) return;
      toast(message);
    }
  };

  const old = sip;
  sip = new SipClient(cfg, handlers);
  if (old) {
    try {
      old.disconnect();
    } catch {
      // 旧连接已释放
    }
  }
  sip.connect();
}

function disconnectCurrent() {
  sessionToken += 1;
  const old = sip;
  sip = null;
  if (old) {
    try {
      old.disconnect();
    } catch {
      // 忽略
    }
  }
  stopTimer();
  currentCall = null;
  media = null;
  remoteHasVideo = false;
  el.remoteVideo.srcObject = null;
  el.localVideo.srcObject = null;
  el.incomingOverlay.hidden = true;
  showScreen('dialer');
  setRegistration('UNREGISTERED', null);
}

async function saveAndConnect() {
  const cfg = buildConfigFromUi();
  const problems = validateConfig(cfg);
  if (problems.length) {
    showProblems(problems);
    toast('配置有误，请检查红字提示');
    return;
  }
  showProblems([]);
  appConfig = cfg;
  await saveConfigSafe(cfg);
  await applySelfSigned(cfg.allowSelfSignedCert);
  updateAccountText();
  showScreen('dialer');
  connectWithConfig(cfg);
  toast(`正在连接 ${wsUrl(cfg)}`);
}

async function resetDefaults() {
  appConfig = normalizeConfig(defaults);
  fillSettings(appConfig);
  showProblems([]);
  await saveConfigSafe(appConfig);
  await applySelfSigned(appConfig.allowSelfSignedCert);
  updateAccountText();
  toast('已恢复默认配置');
}

// ============================================================== 拨号

function appendDigit(digit) {
  if (dialed.length >= 32) return;
  dialed += digit;
  el.dialedText.textContent = dialed;
}

function backspace() {
  if (dialed) dialed = dialed.slice(0, -1);
  el.dialedText.textContent = dialed;
}

function placeCall() {
  if (!sip || sip.registrationState !== 'REGISTERED') {
    toast('尚未注册到 NSBC，请先到设置里连接');
    showScreen('settings');
    return;
  }
  if (!dialed.trim()) {
    toast('请输入要呼叫的号码');
    return;
  }
  const target = dialed.trim();
  dialed = '';
  el.dialedText.textContent = '';

  const callMedia = new WebRtcMedia(appConfig, mediaHandlers());
  media = callMedia;
  let call;
  try {
    call = sip.startCall(target, callMedia);
  } catch (err) {
    media = null;
    try {
      callMedia.stop();
    } catch {
      // 忽略
    }
    toast(err.message);
    return;
  }
  currentCall = call;
  enterCallUi(call);
  el.callSubtitle.textContent = '正在呼叫…';
}

// ============================================================== 呼叫状态

function mediaHandlers() {
  return {
    onLocalStream: (stream) => {
      el.localVideo.srcObject = stream;
      syncVideoUi();
    },
    onRemoteStream: (stream) => {
      el.remoteVideo.srcObject = stream;
      remoteHasVideo = stream.getVideoTracks().length > 0;
      syncVideoUi();
    },
    onError: (message) => toast(message)
  };
}

function handleIncomingCall(call) {
  currentCall = call;
  call._answered = false;
  call._finished = false;
  const name = displayNameOf(call);
  el.incomingName.textContent = name;
  el.incomingUri.textContent = call.remoteUri;
  el.incomingAvatar.textContent = initialOf(name);
  el.incomingOverlay.hidden = false;
}

function handleCallState(call) {
  if (!currentCall || call !== currentCall) return;
  switch (call.state) {
    case 'CALLING':
      setCallSubtitle('正在呼叫…');
      break;
    case 'RINGING':
      setCallSubtitle(call._answered ? '正在接通…' : '对方振铃…');
      break;
    case 'ESTABLISHED':
      setCallSubtitle('通话中');
      startTimer(call);
      break;
    case 'TERMINATED':
    case 'FAILED':
      finishCall(call);
      break;
    default:
      break;
  }
}

function finishCall(call) {
  if (call._finished) return;
  call._finished = true;
  stopTimer();
  addRecent(call);
  const reason = call.endReason || '';
  if (call.state === 'FAILED') {
    toast(`通话失败：${reasonText(reason)}`);
  } else if (!['local_hangup', 'local_cancel', 'remote_hangup', 'remote_cancel'].includes(reason)) {
    toast(`通话结束：${reasonText(reason)}`);
  }
  exitCallUi();
}

function enterCallUi(call) {
  muted = false;
  speakerOn = false;
  videoOn = !!appConfig.videoEnabled;
  remoteHasVideo = false;

  const name = displayNameOf(call);
  el.callName.textContent = name;
  el.callAvatar.textContent = initialOf(name);
  el.callTimer.textContent = '00:00';
  el.callSubtitle.textContent = call.isIncoming ? '来电' : '正在呼叫…';
  el.muteBtn.textContent = '静音';
  el.muteBtn.classList.remove('active');
  el.speakerBtn.textContent = '免提';
  el.speakerBtn.classList.remove('active');
  el.videoBtn.textContent = '关视频';
  el.videoBtn.classList.remove('active');
  el.incomingOverlay.hidden = true;
  resetPip();
  syncVideoUi();
  showScreen('call');
}

function exitCallUi() {
  stopTimer();
  el.remoteVideo.srcObject = null;
  el.localVideo.srcObject = null;
  el.remoteVideo.classList.remove('no-video');
  el.callAvatar.hidden = false;
  el.incomingOverlay.hidden = true;
  remoteHasVideo = false;
  currentCall = null;
  media = null;
  showScreen('dialer');
}

function setCallSubtitle(text) {
  el.callSubtitle.textContent = text;
}

function syncVideoUi() {
  const localStream = el.localVideo.srcObject;
  const hasLocalVideo = !!(localStream
    && typeof localStream.getVideoTracks === 'function'
    && localStream.getVideoTracks().length > 0);

  el.videoBtn.textContent = videoOn ? '关视频' : '开视频';
  el.videoBtn.classList.toggle('active', !videoOn);
  el.switchBtn.disabled = !videoOn || !hasLocalVideo;
  el.localContainer.hidden = !videoOn || !hasLocalVideo;
  el.localVideo.classList.toggle('mirrored', !!appConfig.mirrorLocalPreview);

  const showRemoteVideo = videoOn && remoteHasVideo;
  el.remoteVideo.classList.toggle('no-video', !showRemoteVideo);
  el.callAvatar.hidden = showRemoteVideo;
}

// ============================================================== 通话动作

function toggleMute() {
  muted = !muted;
  if (sip) sip.setMuted(muted);
  el.muteBtn.textContent = muted ? '已静音' : '静音';
  el.muteBtn.classList.toggle('active', muted);
}

async function toggleVideo() {
  videoOn = !videoOn;
  if (sip) sip.setVideoEnabled(videoOn);
  syncVideoUi();
}

async function switchCamera() {
  if (!sip) return;
  const ok = await sip.switchCamera();
  if (!ok) toast('未检测到可切换的摄像头');
}

async function toggleSpeaker() {
  const next = !speakerOn;
  const ok = await applySpeakerRoute(next);
  if (!ok) {
    toast('当前系统不支持切换音频输出，请使用系统音量设置');
    return;
  }
  speakerOn = next;
  el.speakerBtn.textContent = speakerOn ? '免提开' : '免提';
  el.speakerBtn.classList.toggle('active', speakerOn);
}

async function applySpeakerRoute(speaker) {
  if (typeof el.remoteVideo.setSinkId !== 'function') return false;
  try {
    const outputs = (await navigator.mediaDevices.enumerateDevices())
      .filter((device) => device.kind === 'audiooutput');
    if (!outputs.length) return false;
    const target = speaker
      ? (outputs.find((d) => d.deviceId !== 'default') || outputs[0])
      : (outputs.find((d) => d.deviceId === 'default') || outputs[0]);
    if (!target) return false;
    await el.remoteVideo.setSinkId(target.deviceId);
    return true;
  } catch {
    return false;
  }
}

function hangup() {
  if (sip) sip.hangup();
}

function answerCall() {
  const call = currentCall;
  if (!call || !sip) return;
  call._answered = true;
  el.incomingOverlay.hidden = true;
  el.answerBtn.disabled = true;

  const callMedia = new WebRtcMedia(appConfig, mediaHandlers());
  media = callMedia;
  enterCallUi(call);
  setCallSubtitle('正在接通…');

  sip.answer(callMedia).catch((err) => {
    toast(`接听失败：${err.message}`);
  }).finally(() => {
    el.answerBtn.disabled = false;
  });
}

function rejectCall() {
  el.incomingOverlay.hidden = true;
  if (sip) sip.reject(486);
}

// ============================================================== 计时器

function startTimer(call) {
  stopTimer();
  const tick = () => {
    if (!call.establishedAt) return;
    const seconds = Math.max(0, Math.floor((Date.now() - call.establishedAt) / 1000));
    el.callTimer.textContent = formatDuration(seconds);
  };
  tick();
  timerHandle = setInterval(tick, 500);
}

function stopTimer() {
  if (timerHandle) clearInterval(timerHandle);
  timerHandle = null;
}

function formatDuration(totalSeconds) {
  const sec = Math.max(0, Math.floor(totalSeconds));
  return `${String(Math.floor(sec / 60)).padStart(2, '0')}:${String(sec % 60).padStart(2, '0')}`;
}

// ============================================================== 最近通话

function loadRecents() {
  try {
    const arr = JSON.parse(localStorage.getItem(RECENTS_KEY) || '[]');
    if (!Array.isArray(arr)) return [];
    return arr.filter((item) => item && typeof item.uri === 'string').slice(0, MAX_RECENTS);
  } catch {
    return [];
  }
}

function saveRecents() {
  try {
    localStorage.setItem(RECENTS_KEY, JSON.stringify(recents.slice(0, MAX_RECENTS)));
  } catch {
    // 存储不可用时静默降级
  }
}

function addRecent(call) {
  const start = call.establishedAt || 0;
  const end = call.endedAt || Date.now();
  const duration = start ? Math.max(0, Math.floor((end - start) / 1000)) : 0;
  recents.unshift({
    uri: call.remoteUri,
    outgoing: !call.isIncoming,
    ts: end,
    duration
  });
  recents = recents.slice(0, MAX_RECENTS);
  saveRecents();
  renderRecents();
}

function renderRecents() {
  el.recentList.replaceChildren();
  el.recentEmpty.hidden = recents.length > 0;

  const formatter = new Intl.DateTimeFormat('zh-CN', {
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    hour12: false
  });

  recents.slice(0, 12).forEach((item) => {
    const row = document.createElement('button');
    row.type = 'button';
    row.className = 'recent-row';

    const arrow = document.createElement('span');
    arrow.className = `recent-arrow ${item.outgoing ? 'outgoing' : 'incoming'}`;
    arrow.textContent = item.outgoing ? '↗' : '↙';

    const number = document.createElement('span');
    number.className = 'recent-number';
    number.textContent = numberOf(item.uri);

    const time = document.createElement('span');
    time.className = 'recent-time';
    time.textContent = formatter.format(new Date(item.ts));

    const duration = document.createElement('span');
    duration.className = 'recent-duration';
    duration.textContent = formatDuration(item.duration || 0);

    row.append(arrow, number, time, duration);
    row.addEventListener('click', () => {
      dialed = numberOf(item.uri);
      el.dialedText.textContent = dialed;
      showScreen('dialer');
    });
    el.recentList.appendChild(row);
  });
}

// ============================================================== 工具

function numberOf(uri) {
  return String(uri || '')
    .replace(/^sips?:/i, '')
    .split(';')[0]
    .split('@')[0];
}

function displayNameOf(call) {
  return (call.remoteDisplayName && call.remoteDisplayName.trim())
    || numberOf(call.remoteUri)
    || call.remoteUri;
}

function initialOf(name) {
  const text = String(name || '?').trim();
  return text ? text.charAt(0).toUpperCase() : '?';
}

function reasonText(reason) {
  if (!reason) return '通话结束';
  if (reason.startsWith('rejected_')) return `对方拒绝（${reason.slice(9)}）`;
  const map = {
    busy: '对方忙',
    canceled: '已取消',
    timeout: '呼叫超时',
    declined: '对方拒接',
    local_hangup: '已挂断',
    local_cancel: '已取消',
    local_reject: '已拒接',
    remote_hangup: '对方挂断',
    remote_cancel: '对方取消',
    media_failed: '媒体协商失败',
    auth_failed: '鉴权失败',
    auth_missing: '缺少鉴权挑战'
  };
  return map[reason] || reason;
}

function toast(message) {
  if (!el.toast) return;
  el.toast.textContent = message;
  el.toast.classList.add('show');
  if (toastHandle) clearTimeout(toastHandle);
  toastHandle = setTimeout(() => {
    el.toast.classList.remove('show');
    toastHandle = null;
  }, 2800);
}

// ============================================================== 本地小窗拖动

let pipDrag = null;

function resetPip() {
  el.localContainer.style.left = '';
  el.localContainer.style.top = '';
  el.localContainer.style.right = '';
}

function installPipDrag() {
  el.localContainer.addEventListener('pointerdown', (event) => {
    if (event.button !== 0) return;
    pipDrag = {
      pointerId: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      left: el.localContainer.offsetLeft,
      top: el.localContainer.offsetTop
    };
    el.localContainer.setPointerCapture(event.pointerId);
    event.preventDefault();
  });

  el.localContainer.addEventListener('pointermove', (event) => {
    if (!pipDrag || event.pointerId !== pipDrag.pointerId) return;
    const width = el.screenCall.clientWidth;
    const height = el.screenCall.clientHeight;
    const pipWidth = el.localContainer.offsetWidth;
    const pipHeight = el.localContainer.offsetHeight;
    const left = Math.min(
      Math.max(8, pipDrag.left + event.clientX - pipDrag.startX),
      Math.max(8, width - pipWidth - 8)
    );
    const top = Math.min(
      Math.max(8, pipDrag.top + event.clientY - pipDrag.startY),
      Math.max(8, height - pipHeight - 8)
    );
    el.localContainer.style.left = `${left}px`;
    el.localContainer.style.top = `${top}px`;
    el.localContainer.style.right = 'auto';
  });

  const endDrag = (event) => {
    if (!pipDrag || event.pointerId !== pipDrag.pointerId) return;
    el.localContainer.releasePointerCapture(pipDrag.pointerId);
    pipDrag = null;
  };
  el.localContainer.addEventListener('pointerup', endDrag);
  el.localContainer.addEventListener('pointercancel', endDrag);
}

// ============================================================== 事件绑定

function bindEvents() {
  el.settingsBtn.addEventListener('click', () => showScreen('settings'));
  el.settingsBackBtn.addEventListener('click', () => showScreen('dialer'));

  ['key1', 'key2', 'key3', 'key4', 'key5', 'key6', 'key7', 'key8', 'key9',
    'keyStar', 'key0', 'keyHash'].forEach((id) => {
    el[id].addEventListener('click', () => appendDigit(el[id].dataset.key));
  });
  el.backspaceBtn.addEventListener('click', backspace);
  el.callBtn.addEventListener('click', placeCall);

  el.saveConnectBtn.addEventListener('click', saveAndConnect);
  el.disconnectBtn.addEventListener('click', () => {
    disconnectCurrent();
    updateAccountText();
    toast('已断开');
  });
  el.resetBtn.addEventListener('click', resetDefaults);

  el.selfSignedSwitch.addEventListener('change', () => {
    applySelfSigned(el.selfSignedSwitch.checked);
  });

  ['serverInput', 'portInput', 'pathInput', 'realmInput', 'userInput'].forEach((id) => {
    el[id].addEventListener('input', updateAccountText);
  });

  el.muteBtn.addEventListener('click', toggleMute);
  el.speakerBtn.addEventListener('click', toggleSpeaker);
  el.videoBtn.addEventListener('click', toggleVideo);
  el.switchBtn.addEventListener('click', switchCamera);
  el.hangupBtn.addEventListener('click', hangup);

  el.answerBtn.addEventListener('click', answerCall);
  el.rejectBtn.addEventListener('click', rejectCall);

  installPipDrag();
}

// ============================================================== 初始化

async function init() {
  bindEvents();
  recents = loadRecents();
  renderRecents();

  let loaded = null;
  if (bridge) {
    try {
      loaded = await bridge.loadConfig();
    } catch (err) {
      toast(`读取配置失败：${err.message}`);
    }
  }
  if (loaded && loaded.defaults) defaults = normalizeConfig(loaded.defaults);
  if (loaded && loaded.config) appConfig = normalizeConfig(loaded.config);
  else appConfig = normalizeConfig(defaults);

  fillSettings(appConfig);
  updateAccountText();
  setRegistration('UNREGISTERED', null);
  await applySelfSigned(appConfig.allowSelfSignedCert);

  if (!bridge) toast('请通过 Electron 启动本客户端');
}

init();
