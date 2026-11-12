# NSBC iOS 软电话

NSBC（Nway SBC）iOS 客户端，与 Android 版协议行为一致：
SIP over WebSocket（RFC 7118）+ WebRTC，纯 SwiftUI，无 Storyboard。

## 目录结构

```
client/ios/
  Package.swift                  # NsbcSDK（SwiftPM library）
  Sources/NsbcSDK/
    NsbcConfig.swift             # 配置模型 + 校验
    NsbcTypes.swift              # CallState / RegistrationState / NsbcError
    sip/SipMessage.swift         # SIP 解析 / 序列化 / 响应构造
    sip/DigestAuth.swift         # Digest MD5(qop=auth)
    sip/SipUtils.swift           # branch / tag / Call-ID / MD5 / 本机 IP
    sip/SipSocket.swift          # URLSessionWebSocketTask 传输
    sip/SipEngine.swift          # REGISTER / INVITE / ACK / BYE / CANCEL
    media/MediaEngine.swift      # WebRTC 音视频引擎
    NsbcClient.swift             # 对外门面 + delegate + 重连
  NsbcSoftphone/                 # SwiftUI App
  project.yml                    # XcodeGen 工程描述
```

## 环境要求

- macOS + Xcode 15 或更高（Swift 5.9）
- [XcodeGen](https://github.com/yonaskolb/XcodeGen)：`brew install xcodegen`
- 真机（模拟器没有摄像头，音视频功能不完整）

## 构建步骤

```bash
cd client/ios
xcodegen generate
open NsbcSoftphone.xcodeproj
```

在 Xcode 中：

1. 选择 `NsbcSoftphone` target → Signing & Capabilities → 选择你的 Team；
2. 连接 iPhone，选择设备后 Run；
3. 首次启动系统会请求摄像头/麦克风权限，以及本地网络权限。

> 首次构建需要从 `https://github.com/stasel/WebRTC` 解析 SwiftPM 二进制包，
> 请保持网络可访问 GitHub。包体积较大（约 100MB+），下载耗时正常。

### 固定 WebRTC 版本

`Package.swift` 与 `project.yml` 默认跟随 `stasel/WebRTC` 的 `master` 分支。
正式发布前建议改成确切 tag/版本，例如：

```swift
// Package.swift
.package(url: "https://github.com/stasel/WebRTC", exact: "120.0.0")
```

```yaml
# project.yml
packages:
  WebRTC:
    url: https://github.com/stasel/WebRTC
    exactVersion: 120.0.0
```

（以仓库实际发布的 tag 为准。）

## 功能

- 账号注册 / 注销（REGISTER，Digest 401/407，自动续期，断开发 Expires:0）
- 呼出（INVITE + offer）、呼入（接听 / 拒接）、挂断、取消（CANCEL 复用 INVITE branch）
- 视频：默认前摄 1280x720@30，前后摄像头切换，视频开关（停/启采集）
- 本地预览小窗可拖动，限制在屏幕内
- 静音（audio track 禁用）、免提（AVAudioSession 路由切换）
- 通话计时、最近通话与配置持久化（UserDefaults）
- 断线自动重连（指数退避，最多 5 次）
- 自签证书：`allowSelfSignedCert` 打开时接受 server trust

## 协议要点

- WebSocket 地址：`ws|wss://server:port/wsPath`，子协议 `sip`
- Via 传输：`SIP/2.0/WSS`（TLS）或 `SIP/2.0/WS`
- User-Agent：`NsbcIOSSDK/1.0`
- Contact：`<sip:user@realm;transport=ws>`
- 呼出：先 createOffer 并等待 ICE 收集完成（最多 3 秒），再发 INVITE；
  200 后先 ACK（To 带 tag）再 setRemoteDescription(answer)
- 呼入：100/180（To tag 稳定）→ 来电 UI；接听 setRemoteDescription(offer)
  → createAnswer 等 ICE → 200；拒接 486；收到 CANCEL 先 200 再对原 INVITE 回 487
- 媒体：unifiedPlan / maxBundle / rtcpMux；`icePolicy=RELAY` 时只走 TURN

## 真机注意事项

- 必须在真机上验证摄像头（模拟器会退回音频）；`RTCCameraVideoCapturer`
  获取不到设备时 SDK 会以音频继续呼叫。
- 若服务器使用自签证书，保持「允许自签名证书」开启；正式环境建议换受信证书。
- ATS 已在 `Info.plist` 中打开 `NSAllowsArbitraryLoads`，用于连接明文 `ws://` 测试服务器。
- iOS 14+ 访问局域网 IP 会触发本地网络权限弹窗；拒绝后无法连接 NSBC。
- 后台通话未申请 `UIBackgroundModes=audio`，App 退到后台会结束媒体；如需后台通话，
  在 `Info.plist` 增加该后台模式并补充相应说明。
- 若固定 WebRTC 版本后出现 delegate 方法签名不匹配，请按该版本
  `RTCPeerConnectionDelegate` 头文件调整 `MediaEngine.swift` 末尾的扩展。
