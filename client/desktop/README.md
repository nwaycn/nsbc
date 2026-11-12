# NSBC 桌面软电话

基于 Electron + 原生 HTML/CSS/JS（ES Modules）的 Windows / macOS 软电话，协议行为与
`client/android/nsbc-sdk` 的 SIP over WebSocket + WebRTC 实现保持一致。

## 运行

```bash
cd client/desktop
npm install        # 首次运行，仅安装 devDependencies（electron / electron-builder）
npm start          # 或 npm run dev
```

要求 Node.js 18+。首次启动会在 Electron 的 `userData` 目录创建 `config.json`：

- Windows：`%APPDATA%\nsbc-softphone-desktop\config.json`
- macOS：`~/Library/Application Support/nsbc-softphone-desktop/config.json`

也可以在“设置”页修改后点击“保存并连接”写入。

## 打包

```bash
npm run dist:win   # Windows NSIS 安装包，输出到 dist/
npm run dist:mac   # macOS DMG，需要在 macOS 上执行
```

macOS 首次通话会触发系统麦克风/摄像头授权（`NSMicrophoneUsageDescription` /
`NSCameraUsageDescription` 已写入 electron-builder 配置）。正式分发仍需配置签名与公证。

## 目录

```
client/desktop/
  package.json      Electron 入口与打包配置
  main.js           窗口、自签证书校验、IPC、macOS 媒体权限、加载失败兜底
  preload.js        contextBridge：loadConfig / saveConfig / setAllowSelfSigned
  src/index.html    四屏单页：拨号 / 设置 / 通话中 / 来电浮层
  src/styles.css    深色主题 420x860 手机比例布局
  src/sip.js        SIP over WebSocket：REGISTER / INVITE / ACK / BYE / CANCEL / Digest
  src/digest.js     纯 JS MD5 + qop=auth Digest
  src/media.js      WebRTC 媒体：getUserMedia、PeerConnection、ICE、摄像头切换
  src/renderer.js   状态机、UI 绑定、配置校验/持久化、最近通话
```

## 协议要点

- WebSocket：`ws|wss://server:port/wsPath`，子协议固定为 `"sip"`，默认端口 `5062`、路径 `/ws`。
- Via：TLS 时 `SIP/2.0/WSS`，否则 `SIP/2.0/WS`；branch 前缀 `z9hG4bK-`。
- REGISTER：Request-URI `sip:realm`，Contact `<sip:user@realm;transport=ws>`；
  401/407 用 Digest MD5（qop=auth，nc 八位十六进制）重发；200 后按 Expires 一半刷新；
  主动断开时发送 `Expires: 0` 注销。
- 呼出：先 `getUserMedia` + `createOffer`，等待 `iceGatheringState === 'complete'`
  （最多 3 秒）后再发 INVITE；收到 200 先发 ACK，再 `setRemoteDescription(answer)`。
- 呼入：回 100 Trying、180 Ringing（To tag 稳定），接听后 200 OK，收到 ACK 进入
  ESTABLISHED；拒接 486；收到 CANCEL 先回 200，再对原 INVITE 回 487。
- 媒体：`bundlePolicy: max-bundle`、`rtcpMuxPolicy: require`，每通呼叫新建
  PeerConnection；远端 offer 没有 `m=video` 时不添加视频；通话结束统一
  `track.stop()` 停止采集。
- 自签证书：主进程安装 `setCertificateVerifyProc`，开关为真时回调 0，否则交给
  Chromium 默认校验（-3）；设置页切换即时生效。

## 已知限制

- 浏览器环境无法获知本机 IP，Via 的 sent-by 使用占位主机名 `nsbc-desktop`；
  SIP 响应仍沿同一条 WebSocket 返回。
- “免提”在桌面端使用 `HTMLMediaElement.setSinkId` 切换音频输出设备，系统不提供
  可用输出设备时会提示改用系统音量设置。
- 通话中暂不支持 re-INVITE / 视频重新协商；关闭视频只停发本地视频轨。
