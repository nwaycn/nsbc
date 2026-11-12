# NSBC Android 客户端与独立 SDK

面向 Nway SBC（NSBC）的 Android 视频通话客户端，以及可独立复用的 `nsbc-sdk`。

技术路线：**SIP over WebSocket（RFC 7118）+ WebRTC（DTLS-SRTP / ICE）**，
与 NSBC 服务端默认配置（`sip.ws.port=5062`、`sip.ws.tls=true`、STUN `3478`）直接对接。

---

## 目录结构

```
client/android/
├── settings.gradle.kts
├── build.gradle.kts
├── gradle.properties
├── nsbc-sdk/                    ← 独立 SDK（可单独发布/复用到任何 App）
│   └── src/main/java/com/nway/nsbc/sdk/
│       ├── NsbcClient.kt        门面：connect / call / 事件回调
│       ├── NsbcConfig.kt        配置：数据类 + Builder + JSON
│       ├── NsbcCall.kt          呼叫句柄
│       ├── NsbcTypes.kt         CallState / RegistrationState / NsbcError
│       ├── NsbcLogger.kt        日志出口（可接管到 UI）
│       ├── sip/
│       │   ├── SipMessage.kt    SIP 消息解析/构造
│       │   ├── SipUtils.kt      branch / tag / MD5
│       │   ├── DigestAuth.kt    RFC 7616 Digest 认证
│       │   ├── SipTransport.kt  OkHttp WebSocket 传输
│       │   └── SipEngine.kt     REGISTER + 呼叫状态机
│       ├── media/
│       │   ├── MediaEngine.kt   媒体引擎抽象（可替换）
│       │   └── WebRtcMediaEngine.kt  Google WebRTC 实现
│       └── net/NetworkUtils.kt  本机地址探测
└── app/                         ← Demo 应用（仅依赖 :nsbc-sdk）
    ├── src/main/java/com/nway/nsbc/demo/MainActivity.kt
    ├── src/main/assets/nsbc.json          ← 开箱即用的配置模板
    └── src/main/res/...
```

---

## 一、编译运行

**方式 A：Android Studio（推荐）**
直接用 Android Studio 打开 `client/android` 目录，等待 Gradle Sync 完成后运行 `app`。

**方式 B：命令行**

```bash
cd client/android

# 首次需要生成 wrapper（仓库未提交 gradlew 二进制）
gradle wrapper --gradle-version 8.4

# 编译（Windows 用 gradlew.bat）
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

环境要求：JDK 17、Android SDK Platform 34、Build Tools 34.0.0。

首次运行前后端准备（NSBC 侧）：

```yaml
# nsbc.yaml 服务端需开启（默认已开启）
sip:
  ws:
    enabled: true
    port: 5062
    tls: true
```

并在 Web UI 中创建分机 `1000` / `1001`，或使用已有分机。

---

## 二、三步接入 SDK（业务 App）

### 1) 引入依赖

`settings.gradle.kts`：

```kotlin
include(":nsbc-sdk")
```

业务模块：

```kotlin
dependencies { implementation(project(":nsbc-sdk")) }
```

### 2) 准备配置

放在 `app/src/main/assets/nsbc.json`：

```json
{
  "server": "10.0.0.80",
  "wssPort": 5062,
  "realm": "nsbc.local",
  "username": "1000",
  "password": "secret123",
  "stunServer": "stun:10.0.0.80:3478"
}
```

也可以纯代码配置：

```kotlin
val cfg = NsbcConfig(
    server = "10.0.0.80",
    realm = "nsbc.local",
    username = "1000",
    password = "secret123",
    stunServer = "stun:10.0.0.80:3478"
)
```

### 3) 连接 + 呼叫

```kotlin
val client = NsbcClient.builder(context)
    .configJson(assets.open("nsbc.json").bufferedReader().use { it.readText() })
    .listener(object : NsbcClient.Listener {
        override fun onRegistrationChanged(state: RegistrationState, detail: String?) {
            // REGISTERED 后才能呼叫
        }
        override fun onIncomingCall(call: NsbcCall) {
            call.answer()          // 或 call.reject()
        }
        override fun onCallStateChanged(call: NsbcCall) {
            when (call.state) {
                CallState.ESTABLISHED -> { /* 通话已建立 */ }
                CallState.TERMINATED, CallState.FAILED -> { /* 结束 */ }
                else -> Unit
            }
        }
        override fun onMediaStateChanged(state: String) { /* ice: connected… */ }
        override fun onError(error: NsbcError) { /* 统一错误出口 */ }
    })
    .build()

client.connect()
client.bindRenderers(localSurfaceView, remoteSurfaceView)   // 前后调用均可，connect 后自动补初始化
client.call("1001")
client.currentCall()?.hangup()
```

---

## 三、配置项（`NsbcConfig`）

| 字段 | 默认 | 说明 | 对应 NSBC 服务端 |
|---|---|---|---|
| `server` | — | 主机/IP | — |
| `wssPort` | `5062` | WSS 端口 | `sip.ws.port` |
| `wsPath` | `/ws` | WS 路径 | 服务端无路径校验项，任意路径均可（`/ws` 即可） |
| `useTls` | `true` | 是否 WSS | `sip.ws.tls` |
| `realm` | — | SIP 域 | 分机的 realm / tenant |
| `username` / `password` | — | 分机与密码 | `endpoints` 表 |
| `stunServer` | `null` | STUN URL | `nat.stun.port`（3478） |
| `turnServer` / `turnUsername` / `turnCredential` | `null` | TURN 中继 | 自建 coturn |
| `videoEnabled` | `true` | 是否协商视频（关闭后只发音频 offer） | — |
| `videoWidth` / `videoHeight` | `1280` × `720` | 摄像头采集分辨率 | 不改写，原样透传 |
| `videoFps` | `30` | 采集帧率 | — |
| `mirrorLocalPreview` | `true` | 本地小窗镜像（前置自拍习惯） | — |
| `registerExpiresSec` | `300` | 注册有效期 | 服务端可缩小 |
| `autoReconnect` | `true` | 断线重连（指数退避，最多 5 次） | — |
| `icePolicy` | `ALL` | `ALL` / `RELAY`（只走 TURN） | — |
| `allowSelfSignedCert` | `true` | 允许自签证书 | 首次启动自动生成 |
| `logLevel` | `INFO` | 日志级别 | — |

校验：`NsbcConfig.validate()` 返回问题列表；`Builder.build()` 会在配置不合法时抛 `IllegalArgumentException`。

---

## 四、SDK 能力边界

已实现（v1）：

- REGISTER / 401·407 Digest 重认证 / 定时刷新 / 注销
- 呼出 INVITE（含完整 SDP offer）→ 100/180/183 → 200 → ACK
- 呼入 INVITE → 100/180 → 200（answer）/ 486（reject）→ ACK
- BYE 挂断、CANCEL 取消、远端 BYE/CANCEL 处理
- WebRTC 媒体：摄像头采集、AEC/NS/AGC、ICE、DTLS-SRTP
- 静音、开关摄像头、前后摄像头切换
- 断线指数退避重连
- 可通过 `MediaEngine` 接口替换媒体实现

暂未实现（可扩展）：

- 多路并发呼叫（当前为单路 `ctx`）
- REFER / 呼叫转移、会议（focus）
- DataChannel / 屏幕共享
- SIP MESSAGE 收发（已回 200，未上抛业务）
- Trickle ICE（当前为「收集完成后一次性发送 SDP」）

### 视频链路说明

视频是**端到端协商**的，NSBC 不转码、只做 RTP 转发：

```
Android A ──WSS/SIP──> NSBC ──WSS/SIP──> Android B
     └── DTLS-SRTP ──> media_bridge ── DTLS-SRTP ──┘
```

1. 主叫 `createOffer()` 生成含 `m=audio` + `m=video` 的 SDP，`a=ssrc` / `a=ssrc-group:FID`
   随 offer 一起上报——NSBC 的 `admit_ssrc_from_sdp()` 依赖这两行预登记视频 SSRC，
   否则后到的视频流会被 SSRC 防注入策略丢弃。
2. NSBC 在 WS 方向把各 media section **收敛到同一个 UDP 端口**（BUNDLE + `a=rtcp-mux`），
   并在每段注入 ICE/DTLS 参数，因此客户端保持 `bundlePolicy=MAXBUNDLE`、`rtcpMuxPolicy=REQUIRE`。
3. 被叫按对端 offer 决定是否回视频：offer 里没有 `m=video` 时不会凭空添加视频轨
   （否则 answer 段数多于 offer）。
4. 编码由两端自行协商，libwebrtc 默认 offer H.264 / VP8 / VP9，与浏览器端
   `web/static/webrtc.html` 互通；若对端只支持 H.264，会自动收敛到 H.264。

服务端不需要额外开关——`media_bridge` 对 WS 客户端的 SDP 重写对 audio/video 一视同仁。
只有当 NSBC **主动向终端发起呼叫**（`originate`）时才需要配置 `video_codecs`（如 `H264`），
否则服务端自己生成的 offer 不会带 `m=video`。

---

## 五、与服务端联调排查

| 现象 | 排查 |
|---|---|
| 一直「注册中」 | 确认 `wss://host:5062/ws` 可达；`curl -k https://host:8443` 正常说明 TLS 栈可用 |
| 注册 401 循环 | 账号密码错；或 realm 与 NSBC `sip.realm` 不一致 |
| 呼叫 403 / 488 | 分机未在 `endpoints` 中、或 Dialplan 未放通该目标 |
| 接通无声音 | 检查 STUN/TURN；公网严格 NAT 时设 `icePolicy=RELAY` 并配置 TURN |
| 视频黑屏 | 检查 CAMERA 权限；模拟器无摄像头会降级为纯音频 |

日志开关：`NsbcLogger.sink = { level, tag, msg -> ... }` 可接入自有日志面板。

---

## 六、把 SDK 独立分发

`nsbc-sdk` 不依赖 Demo 的任何代码，可直接：

```bash
./gradlew :nsbc-sdk:assembleRelease
# 产物：nsbc-sdk/build/outputs/aar/nsbc-sdk-release.aar
```

或在 `nsbc-sdk/build.gradle.kts` 中启用 `maven-publish` 发布到私有仓库。
