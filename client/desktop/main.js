'use strict';

const { app, BrowserWindow, ipcMain, session, systemPreferences } = require('electron');
const fs = require('fs');
const path = require('path');

// file:// 下加载 ES Module 需要放开本地文件访问（渲染进程只加载本地文件，不加载远程页面）
app.commandLine.appendSwitch('allow-file-access-from-files');

/**
 * 默认配置与 Android assets/nsbc.json 对齐。
 * registerExpiresSec / reconnectDelayMs 在桌面端沿用 SDK 默认值，未放到设置页。
 */
const DEFAULT_CONFIG = {
  server: '10.0.0.80',
  wssPort: 5062,
  wsPath: '/ws',
  useTls: true,
  realm: 'nsbc.local',
  username: '1000',
  password: 'secret123',
  displayName: '',
  stunServer: 'stun:10.0.0.80:3478',
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

const LOG_LEVELS = ['NONE', 'ERROR', 'WARN', 'INFO', 'DEBUG', 'VERBOSE'];

let allowSelfSigned = true;
let mainWindow = null;

function configPath() {
  return path.join(app.getPath('userData'), 'config.json');
}

function toInt(value, fallback) {
  const n = Number.parseInt(value, 10);
  return Number.isFinite(n) ? n : fallback;
}

function toBool(value, fallback) {
  return typeof value === 'boolean' ? value : fallback;
}

function toText(value, fallback) {
  return typeof value === 'string' ? value : fallback;
}

/** 只接受已知字段并做类型收敛，避免配置文件被污染。 */
function sanitize(raw) {
  const o = raw && typeof raw === 'object' ? raw : {};
  const icePolicy = toText(o.icePolicy, DEFAULT_CONFIG.icePolicy).toUpperCase();
  const logLevel = toText(o.logLevel, DEFAULT_CONFIG.logLevel).toUpperCase();
  return {
    server: toText(o.server, DEFAULT_CONFIG.server).trim(),
    wssPort: toInt(o.wssPort, DEFAULT_CONFIG.wssPort),
    wsPath: toText(o.wsPath, DEFAULT_CONFIG.wsPath) || DEFAULT_CONFIG.wsPath,
    useTls: toBool(o.useTls, DEFAULT_CONFIG.useTls),
    realm: toText(o.realm, DEFAULT_CONFIG.realm).trim(),
    username: toText(o.username, DEFAULT_CONFIG.username).trim(),
    password: toText(o.password, DEFAULT_CONFIG.password),
    displayName: toText(o.displayName, DEFAULT_CONFIG.displayName).trim(),
    stunServer: toText(o.stunServer, DEFAULT_CONFIG.stunServer).trim(),
    turnServer: toText(o.turnServer, DEFAULT_CONFIG.turnServer).trim(),
    turnUsername: toText(o.turnUsername, DEFAULT_CONFIG.turnUsername).trim(),
    turnCredential: toText(o.turnCredential, DEFAULT_CONFIG.turnCredential),
    videoEnabled: toBool(o.videoEnabled, DEFAULT_CONFIG.videoEnabled),
    videoWidth: toInt(o.videoWidth, DEFAULT_CONFIG.videoWidth),
    videoHeight: toInt(o.videoHeight, DEFAULT_CONFIG.videoHeight),
    videoFps: toInt(o.videoFps, DEFAULT_CONFIG.videoFps),
    mirrorLocalPreview: toBool(o.mirrorLocalPreview, DEFAULT_CONFIG.mirrorLocalPreview),
    registerExpiresSec: toInt(o.registerExpiresSec, DEFAULT_CONFIG.registerExpiresSec),
    autoReconnect: toBool(o.autoReconnect, DEFAULT_CONFIG.autoReconnect),
    reconnectDelayMs: toInt(o.reconnectDelayMs, DEFAULT_CONFIG.reconnectDelayMs),
    allowSelfSignedCert: toBool(o.allowSelfSignedCert, DEFAULT_CONFIG.allowSelfSignedCert),
    icePolicy: icePolicy === 'RELAY' ? 'RELAY' : 'ALL',
    logLevel: LOG_LEVELS.includes(logLevel) ? logLevel : DEFAULT_CONFIG.logLevel
  };
}

function readConfig() {
  try {
    const raw = fs.readFileSync(configPath(), 'utf8');
    return sanitize(JSON.parse(raw));
  } catch {
    return sanitize(DEFAULT_CONFIG);
  }
}

function writeConfig(config) {
  const normalized = sanitize(config);
  const file = configPath();
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, JSON.stringify(normalized, null, 2), 'utf8');
  return normalized;
}

/** 自签证书：开关打开时回调 0（接受），否则 -3（交给 Chromium 默认校验）。 */
function installCertificateHandler() {
  session.defaultSession.setCertificateVerifyProc((request, callback) => {
    callback(allowSelfSigned ? 0 : -3);
  });
}

/**
 * 加载失败兜底：把错误渲染成一个最小页面，避免出现纯白窗口。
 * 常见原因是文件缺失或 preload 路径错误。
 */
async function loadUI(win) {
  try {
    await win.loadFile(path.join(__dirname, 'src', 'index.html'));
  } catch (err) {
    const message = String((err && err.message) || err).replace(/[&<>"]/g, (c) => ({
      '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'
    }[c]));
    const html = `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<title>NSBC 软电话</title>
<style>body{background:#0B1220;color:#E2E8F0;font-family:system-ui,"Microsoft YaHei",sans-serif;
padding:32px;line-height:1.7}h1{font-size:18px}pre{white-space:pre-wrap;color:#F87171;font-size:12px}</style>
</head><body><h1>界面加载失败</h1><p>请确认 src/index.html 存在，或重新安装应用。</p>
<pre>${message}</pre></body></html>`;
    await win.loadURL(`data:text/html;charset=utf-8,${encodeURIComponent(html)}`);
  }
}

async function requestMacPermissions() {
  if (process.platform !== 'darwin' || !systemPreferences) return;
  try {
    await systemPreferences.askForMediaAccess('microphone');
    await systemPreferences.askForMediaAccess('camera');
  } catch {
    // 用户拒绝时保留系统默认行为，通话时会再次得到 getUserMedia 错误
  }
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 420,
    height: 860,
    minWidth: 360,
    minHeight: 640,
    resizable: true,
    backgroundColor: '#0B1220',
    title: 'NSBC 软电话',
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      autoplayPolicy: 'no-user-gesture-required'
    }
  });

  mainWindow.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  mainWindow.webContents.on('will-navigate', (event) => event.preventDefault());

  loadUI(mainWindow).catch(() => {});

  mainWindow.on('closed', () => {
    mainWindow = null;
  });
}

// 单实例：重复启动时聚焦已有窗口
if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => {
    if (mainWindow) {
      if (mainWindow.isMinimized()) mainWindow.restore();
      mainWindow.focus();
    }
  });

  app.whenReady().then(async () => {
    const saved = readConfig();
    allowSelfSigned = saved.allowSelfSignedCert;

    installCertificateHandler();

    // 只放行媒体（麦克风/摄像头）权限，其余权限一律拒绝
    session.defaultSession.setPermissionRequestHandler((webContents, permission, callback) => {
      callback(permission === 'media');
    });
    session.defaultSession.setPermissionCheckHandler((webContents, permission) => permission === 'media');

    ipcMain.handle('config:load', () => ({
      config: readConfig(),
      defaults: sanitize(DEFAULT_CONFIG)
    }));

    ipcMain.handle('config:save', (event, config) => {
      const saved = writeConfig(config);
      allowSelfSigned = saved.allowSelfSignedCert;
      return saved;
    });

    ipcMain.handle('cert:set-allow-self-signed', (event, allow) => {
      allowSelfSigned = Boolean(allow);
      return allowSelfSigned;
    });

    await requestMacPermissions();
    createWindow();

    app.on('activate', () => {
      if (BrowserWindow.getAllWindows().length === 0) createWindow();
    });
  });

  app.on('window-all-closed', () => {
    if (process.platform !== 'darwin') app.quit();
  });
}
