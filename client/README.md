# NSBC 软电话客户端

NSBC（Nway SBC）官方客户端套件，三套独立工程，共享一套跨端对齐的 SIP over WebSocket + WebRTC 协议栈。

| 目录 | 平台 | 语言 | UI 框架 | 说明 |
|---|---|---|---|---|
| `android/` | Android (手机 / 平板) | Kotlin | 原生 Activity | 含可复用的 **`nsbc-sdk`** 模块，主工程 `app` 是一套完整 Demo |
| `desktop/` | Windows + macOS | JavaScript (ES Modules) | Electron | 一套代码跨两个桌面平台 |
| `ios/` | iPhone / iPad | Swift 5.9 | SwiftUI | 含可复用的 **`NsbcSDK`** SwiftPM 包，App 工程 `NsbcSoftphone` |

> 三套客户端的协议行为**严格对齐**：Via 全方法带 `;rport`、`Supported: replaces, outbound`、`Session-ID`（RFC 7989）、REGISTER 200 后 OPTIONS keepalive、ICE failed → `restartIce()` 一次、SDP 过滤（H.264 → baseline `profile-level-id=42e01f;packetization-mode=1`、Opus `maxplaybackrate=8000`）。

---

## 一、通用前置条件

### 1. NSBC 服务端

所有客户端默认连接 `wss://<nsbc-host>:5062/ws`（SIP over WebSocket，NSBC 默认端口 `sip.ws.port=5062`）：

- 首次启动 NSBC 会自动生成**自签 TLS 证书**，因此客户端侧需打开「允许自签证书」开关（UI 设置里叫 `allowSelfSignedCert`）。
- 如果你的 NSBC 已接入 CA 签发证书，建议在所有客户端里关闭这个开关。
- STUN 服务端建议同时启用（NSBC 默认内建 `3478/udp`），公网场景下否则直连媒体难以打通。

### 2. 三方账号

客户端只需知道四个值即可注册：

| 字段 | 示例 |
|---|---|
| 服务器 | `10.0.0.80` 或 `sip.example.com` |
| Realm | `nsbc.local` |
| 用户名 | `1000` |
| 密码 | `secret123` |

### 3. 操作系统与工具总览

| 客户端 | 最低操作系统 | 构建工具 | 版本 |
|---|---|---|---|
| Android | Android 7.0 (API 24) | Android Studio + Gradle | AS Flamingo / Hedgehog，AGP 8.2.2，Kotlin 1.9.22，JDK 17 |
| desktop | Windows 10 (64-bit) / macOS 11 | Node.js + npm + Electron | Node.js ≥ 18，Electron 31.x，electron-builder 24.x |
| iOS | iOS 14.0 | Xcode + XcodeGen + SwiftPM | Xcode 15+，Swift 5.9，CocoaPods 可选 |

> desktop **不需要 Mac 也能跑起来**（开发态 `npm start` 在 Windows 即可调试），但打 DMG 安装包必须在 macOS 上执行（Apple 签名机制）。

---

## 二、Android

### 要求

| 项 | 值 |
|---|---|
| compileSdk | 34 |
| minSdk | 24 |
| AGP | 8.2.2 |
| Kotlin | 1.9.22 |
| JDK | 17（Android Studio 自带） |
| WebRTC | `io.github.webrtc-sdk:android:125.6422.07`（Maven Central） |

### 工程结构

```
client/android/
├── build.gradle.kts          # 根工程（AGP/Kotlin 版本）
├── settings.gradle.kts       # 包含 app + nsbc-sdk，mavenCentral() 已配置
├── gradle.properties
├── gradle/wrapper/            # 如缺失可用 Android Studio 自动生成
├── nsbc-sdk/                  # 可复用 SDK（发布 AAR 给别的 App）
│   └── src/main/java/com/nway/nsbc/sdk/
│       ├── NsbcConfig.kt      # 配置模型
│       ├── NsbcClient.kt      # 对外门面
│       ├── sip/               # SIP over WSS
│       └── media/             # WebRTC 媒体层
└── app/                       # Demo App
    └── src/main/...
        ├── java/.../MainActivity.kt
        ├── res/               # 四屏 UI（拨号/设置/通话/来电）
        └── assets/nsbc.json   # 默认配置（首次启动读取）
```

### 构建步骤

**方式 A：Android Studio（推荐）**

```
Open → 选择 client/android → 等待 Gradle Sync 完成 → Run ▶
```

首次 Sync 会从 Maven Central 下载 `io.github.webrtc-sdk:android:125.6422.07`（约 200 MB），请确保网络通畅。

**方式 B：命令行（需要先让 AS 生成 gradle wrapper）**

```powershell
cd client/android
# 首次在 Android Studio 打开过之后，会自动生成 gradlew/gradlew.bat
.\gradlew.bat :app:assembleDebug     # 输出 app\build\outputs\apk\debug\app-debug.apk
.\gradlew.bat :nsbc-sdk:assembleRelease   # 输出 AAR
```

### SDK 接入最小代码

```kotlin
val client = NsbcClient.builder(context)
    .configJson(assets.open("nsbc.json").bufferedReader().readText())
    .listener(object : NsbcClient.Listener {
        override fun onRegistrationChanged(state: RegistrationState, detail: String?) {}
        override fun onIncomingCall(call: NsbcCall) {}
        override fun onCallStateChanged(call: NsbcCall) {}
    })
    .build()

client.bindRenderers(localSurface, remoteSurface)  // SurfaceViewRenderer
client.connect()
client.call("1001")
```

### 常见问题

| 现象 | 原因 / 解决 |
|---|---|
| Gradle Sync 卡在 `io.github.webrtc-sdk` | 国内镜像可在 `settings.gradle.kts` 的 pluginManagement / dependencyResolutionManagement 里加 Aliyun 源，或让 Gradle 使用代理 |
| 运行时报 `MediaCodecVideoEncoder not found` | 部分模拟器镜像缺少硬编支持，换真机或系统映像里带 Google Play 的模拟器 |
| 自签证书 WSS 连不上 | 设置里打开「允许自签证书」或在 `NsbcConfig` 里设 `allowSelfSignedCert = true` |
| SIP 注册失败 401 | 账号/密码/Realm 任一错误；Realm 通常等于 NSBC 服务器地址 |
| 通话无视频 | 对方 offer 不含 `m=video`（已在 SDK 里自动降级为音频），或 AndroidManifest 里 CAMERA/RECORD_AUDIO 权限未授予 |

---

## 三、desktop（Windows + macOS）

### 要求

| 项 | 值 |
|---|---|
| Node.js | ≥ 18.17（LTS） |
| npm | ≥ 9（Electron 31 的最低要求） |
| Electron | `^31.0.0` |
| electron-builder | `^24.13.3` |

> 开发态在 Windows / macOS 上都可 `npm start` 运行，**打 DMG 安装包必须在 macOS 上执行**（Apple notarization 机制）。

### 工程结构

```
client/desktop/
├── package.json            # name=nsbc-softphone-desktop, main=main.js
├── main.js                 # BrowserWindow + 自签证书 IPC + 媒体权限
├── preload.js              # contextBridge 仅暴露 config 读写与自签开关
└── src/
    ├── index.html          # 四屏单页
    ├── styles.css           # 暗色主题
    ├── sip.js              # 自研 SIP/WSS 栈
    ├── digest.js           # 纯 JS MD5 + qop=auth
    ├── media.js            # WebRtcMedia 封装
    └── renderer.js         # 状态机 + UI 绑定 + 持久化
```

### 构建步骤

#### 1. 安装依赖（Windows / macOS 相同）

```bash
cd client/desktop
npm install
```

首次 `npm install` 会下载 Electron 预编译二进制（约 150 MB）。如国内网络不通，设置 Electron 镜像：

```bash
# Windows PowerShell
$env:ELECTRON_MIRROR="https://npmmirror.com/mirrors/electron/"
npm install
```

#### 2. 开发运行

```bash
npm start            # 启动 Electron 窗口，窗口比例约 420×860
```

#### 3. 打包

```bash
# Windows — 生成 dist/nsbc-softphone-desktop-Setup-x.y.z.exe（NSIS）
npm run dist:win

# macOS — 必须在 macOS 上执行
npm run dist:mac     # 生成 dist/nsbc-softphone-desktop-x.y.z.dmg
```

### 常见问题

| 现象 | 原因 / 解决 |
|---|---|
| `npm start` 报 `... is not defined` | 旧 Node.js 版本，升级到 18+；Electron 31 不支持 Node 16 |
| 首次启动弹窗「麦克风/摄像头权限」 | macOS 首次调用 `getUserMedia()` 时系统会弹窗；Windows 上需要在「隐私与安全性」里允许 |
| WSS 自签证书报「ERR_CERT_AUTHORITY_INVALID」 | 主进程 `session.defaultSession.setCertificateVerifyProc` 会根据 preload 传来的开关放行；在 UI 设置里打开「允许自签证书」即可 |
| `file://` 下 ES Module 加载失败 | `main.js` 里已加 `webSecurity: false` + `allow-file-access-from-files` 开关；如仍异常我会改成自定义协议（`protocol.registerFileProtocol`），告知我 |
| macOS 上 `npm run dist:mac` 报 codesign 错 | 需要 Apple Developer 证书；如仅自用，可跳过 electron-builder 的 notarization，在 `package.json` 的 build.mac 里加 `hardenedRuntime: false` |

---

## 四、iOS（iPhone / iPad）

### 要求

| 项 | 值 |
|---|---|
| macOS | 13.5+（Ventura） |
| Xcode | 15.0+（App Store 免费下载） |
| Swift | 5.9 |
| iOS 部署目标 | 14.0 |
| XcodeGen | `brew install xcodegen`（2.40+） |
| 设备 | 真机（模拟器不支持 WebRTC 的 H.264 硬编） |

> 本项目使用 **XcodeGen** 生成 `.xcodeproj`（`project.yml` 单一来源），不需要手写工程文件或 CocoaPods。依赖的 WebRTC 是 SwiftPM 二进制包 `https://github.com/stasel/WebRTC`（走 `master` 分支，体积约 350 MB）。

### 工程结构

```
client/ios/
├── Package.swift                      # NsbcSDK SwiftPM 库
├── project.yml                         # XcodeGen 配置
├── README.md                           # iOS 侧说明
├── Sources/NsbcSDK/                    # 可复用 SDK
│   ├── NsbcConfig.swift
│   ├── NsbcTypes.swift
│   ├── NsbcClient.swift
│   ├── sip/                            # SipEngine + SipMessage + Digest + Socket
│   └── media/MediaEngine.swift         # WebRtcMediaEngine
└── NsbcSoftphone/                      # SwiftUI App
    ├── NsbcSoftphoneApp.swift
    ├── AppModel.swift                  # ObservableObject + 配置持久化
    ├── Info.plist
    └── Views/                          # Dialer / Settings / Call / IncomingCall / Video
```

### 构建步骤

#### 1. 安装 XcodeGen（仅首次）

```bash
brew install xcodegen
# 或 pip3 install xcodegen
```

#### 2. 生成 Xcode 工程

```bash
cd client/ios
xcodegen generate          # 读取 project.yml，生成 NsbcSoftphone.xcodeproj
```

#### 3. 打开工程、设置签名

```bash
open NsbcSoftphone.xcodeproj
```

Xcode 里：

1. 选 `NsbcSoftphone` target → Signing & Capabilities → Team（你的免费 Apple ID）
2. Bundle Identifier 改成自己的（如 `com.you.nsbcsoftphone`）
3. Connect 真机，信任开发者（`设置 → 通用 → VPN与设备管理`）
4. Run ▶

#### 4. 编译命令行（可选，CI 用）

```bash
xcodebuild \
  -scheme NsbcSoftphone \
  -configuration Release \
  -destination 'generic/platform=iOS' \
  build
```

### SDK 接入最小代码

```swift
let config = NsbcConfig(
    server: "10.0.0.80",
    realm: "nsbc.local",
    username: "1000",
    password: "secret123",
    allowSelfSignedCert: true
)
let client = NsbcClient(config: config, delegate: self)
client.connect()
client.call(target: "1001")
```

### 常见问题

| 现象 | 原因 / 解决 |
|---|---|
| `xcodegen generate` 报 YAML 错 | `project.yml` 缩进必须 2 空格；用 `xcodegen spec` 校验 |
| SwiftPM 解析 WebRTC 包卡很久 | 国内建议用代理；或本地 fork `https://github.com/stasel/WebRTC` 镜像 |
| 真机 Run 报「未信任开发者」 | 设置 → 通用 → VPN 与设备管理 → 信任你的 Team |
| App 启动即退出 / `dyld` 报错 | 通常是 SwiftPM 缓存损坏：`rm -rf ~/Library/Developer/Xcode/DerivedData && rm -rf ~/Library/Caches/org.swift.swiftpm` 后重开 Xcode |
| 摄像头/麦克风权限弹一次就不再弹 | Info.plist 已包含 `NSCameraUsageDescription` / `NSMicrophoneUsageDescription`；若想强制重置，iOS 设置 → NsbcSoftphone → 开关权限 |
| 自签 WSS 报 TLS error | 打开「允许自签证书」（Info.plist 里已加 `NSAllowsArbitraryLoads` + App Transport Security 的 exception 域） |
| 无声音 / 无画面 | 检查 iOS 系统音频路由（「设置 → 辅助功能 → 音频/视觉」关闭蓝牙路由）；摄像头权限；对方 offer 是否含 `m=video` |

---

## 五、版本对齐矩阵

| 项 | Android | desktop | iOS |
|---|---|---|---|
| SIP 传输 | WSS (WebSocket over TLS) | WSS | WSS |
| SIP Via 头 | `;rport` + `z9hG4bK-` | 同左 | 同左 |
| Supported | `replaces, outbound` | 同左 | 同左 |
| Session-ID | RFC 7989（`tag@host`） | 同左 | 同左 |
| Keepalive | OPTIONS 30s × miss=3 → 重注册 | 同左 | 同左 |
| 重注册周期 | `Expires/2` | 同左 | 同左 |
| Media | WebRTC Unified Plan | 同左（浏览器原生） | 同左 |
| bundlePolicy | MAXBUNDLE | 同左 | 同左 |
| rtcpMuxPolicy | REQUIRE | 同左 | 同左 |
| iceCandidatePoolSize | 10 | 同左 | 同左 |
| ICE 失败恢复 | restartIce() 一次 | 同左 | 同左 |
| H.264 兼容 | 强制 baseline 42e01f + packetization-mode=1 | 同左 | 同左 |
| Opus 兼容 | maxplaybackrate=8000 | 同左 | 同左 |
| STUN/TURN | NsbcConfig 字段 | renderer.js config | NsbcConfig Swift |
| 自签证书 | `allowSelfSignedCert` | preload → main 证书回调 | URLSessionDelegate |
| 配置持久化 | SharedPreferences + assets 回退 | Electron userData/config.json | UserDefaults + 首次 JSON |

## 六、可选后续扩展

| 方向 | 收益 | 难度 |
|---|---|---|
| **TelecomManager 集成**（Android） | 锁屏拨号、通话记录、车载蓝牙 | 中 |
| **屏幕共享**（desktop） | getDisplayMedia + video track | 低 |
| **AudioSession routeChange 监听**（iOS） | 耳机/蓝牙切换自动恢复会话 | 低 |
| **TCP/TLS transport**（三端） | 绕过 WebSocket 某些代理 | 中（需 NSBC 支持） |
| **SRTP 加密**（三端） | 媒体层鉴权，依赖 SDES-DTLS 当前已协商 | 低 |
| **T.38 Fax** | 依赖 NSBC 侧媒体桥支持 | 高 |
| **APNs 推送唤醒**（iOS） | 后台收 INVITE，需服务端配合 | 中 |
| **Android Auto / CarPlay** | 车载场景 | 中 |
| **桌面端 Squirrel 自动更新** | 增量更新 | 中 |

---

## 七、文档索引

- Android SDK 参考：`client/android/README.md`
- desktop 开发说明：`client/desktop/README.md`
- iOS 构建说明：`client/ios/README.md`
- 本仓库根 README：`../README.md`
