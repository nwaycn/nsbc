import Foundation
import WebRTC

/// 一通呼叫的句柄。
///
/// 生命周期由 SDK 内部管理；业务层只读 `state`、调用 `answer()` / `hangup()` 等动作。
public final class NsbcCall {

    /// 是否呼入
    public let isIncoming: Bool
    /// 对端 SIP URI，例如 `sip:1001@nsbc.local`
    public let remoteUri: String
    /// 内部唯一 ID（等于 SIP Call-ID）
    let callId: String

    /// 呼叫状态，由 SDK 更新
    public internal(set) var state: CallState = .idle
    /// 对端显示名（若 SIP 头带 display name）
    public internal(set) var remoteDisplayName: String?
    /// 接通时间戳（秒），未接通为 nil
    public internal(set) var establishedAtMs: TimeInterval?
    /// 结束原因（如 `normal` / `busy` / `canceled` / `timeout`）
    public internal(set) var endReason: String?
    /// 结束时间戳（秒），未结束为 0
    var lastEndedAtMs: TimeInterval = 0

    weak var actions: NsbcCallActions?

    init(isIncoming: Bool, remoteUri: String, callId: String, actions: NsbcCallActions?) {
        self.isIncoming = isIncoming
        self.remoteUri = remoteUri
        self.callId = callId
        self.actions = actions
    }

    /// 通话时长（秒），未接通返回 0
    public var durationSeconds: Int64 {
        guard let start = establishedAtMs else { return 0 }
        let end: TimeInterval
        if state == .established {
            end = Date().timeIntervalSince1970
        } else {
            end = lastEndedAtMs
        }
        return Int64(max(end - start, 0))
    }

    public var isActive: Bool {
        return state == .calling || state == .ringing || state == .established
    }

    /// 接听（仅呼入有效）。
    public func answer() { actions?.answer(self) }

    /// 拒接（仅呼入有效），默认 486 Busy Here。
    public func reject(code: Int = 486) { actions?.reject(self, code: code) }

    /// 挂断 / 取消。
    public func hangup() { actions?.hangup(self) }

    /// 麦克风静音。
    public func setMuted(_ muted: Bool) { actions?.setMuted(self, muted: muted) }

    /// 开关摄像头。
    public func setVideoEnabled(_ enabled: Bool) { actions?.setVideoEnabled(self, enabled: enabled) }

    /// 前后摄像头切换。
    public func switchCamera() { actions?.switchCamera(self) }
}

/// 呼叫动作回调（由 SipEngine 实现）。
protocol NsbcCallActions: AnyObject {
    func answer(_ call: NsbcCall)
    func reject(_ call: NsbcCall, code: Int)
    func hangup(_ call: NsbcCall)
    func setMuted(_ call: NsbcCall, muted: Bool)
    func setVideoEnabled(_ call: NsbcCall, enabled: Bool)
    func switchCamera(_ call: NsbcCall)
}

/// SDK 事件回调。所有方法在主线程触发。
public protocol NsbcClientDelegate: AnyObject {
    /// 注册状态变化
    func nsbcClient(_ client: NsbcClient, didChangeRegistration state: RegistrationState, detail: String?)
    /// 收到呼入
    func nsbcClient(_ client: NsbcClient, didReceiveIncoming call: NsbcCall)
    /// 呼叫状态变化
    func nsbcClient(_ client: NsbcClient, didChangeCall call: NsbcCall)
    /// ICE / 媒体状态变化
    func nsbcClient(_ client: NsbcClient, didChangeMediaState state: String)
    /// 错误
    func nsbcClient(_ client: NsbcClient, didFail error: NsbcError)
}

public extension NsbcClientDelegate {
    func nsbcClient(_ client: NsbcClient, didChangeRegistration state: RegistrationState, detail: String?) {}
    func nsbcClient(_ client: NsbcClient, didReceiveIncoming call: NsbcCall) {}
    func nsbcClient(_ client: NsbcClient, didChangeCall call: NsbcCall) {}
    func nsbcClient(_ client: NsbcClient, didChangeMediaState state: String) {}
    func nsbcClient(_ client: NsbcClient, didFail error: NsbcError) {}
}

/// NSBC iOS SDK 入口。
///
/// 典型用法：
/// ```swift
/// let client = try NsbcClient(config: cfg, delegate: self)
/// client.connect()
/// try client.call("1001")
/// ```
///
/// 线程约定：所有公开方法建议在主线程调用；回调在主线程触发。
public final class NsbcClient {

    public let config: NsbcConfig

    public weak var delegate: NsbcClientDelegate?

    private let mediaEngine: MediaEngine
    private let engine: SipEngine

    private var initialized = false
    private var reconnectAttempts = 0
    private var manuallyDisconnected = false
    private var reconnectWorkItem: DispatchWorkItem?

    /// 渲染器引用保留，便于在 connect() 后补做绑定
    private weak var localRenderer: RTCMTLVideoView?
    private weak var remoteRenderer: RTCMTLVideoView?

    public init(config: NsbcConfig, delegate: NsbcClientDelegate?) throws {
        let cfg = config.normalized()
        let problems = cfg.validate()
        guard problems.isEmpty else {
            throw NsbcError(.configInvalid, "配置不合法: " + problems.joined(separator: "; "))
        }

        self.config = cfg
        self.delegate = delegate

        let media = WebRtcMediaEngine()
        self.mediaEngine = media
        self.engine = SipEngine(config: cfg, mediaEngine: media)
        self.engine.events = self
        media.delegate = self
    }

    // MARK: - 生命周期

    /// 建立 WSS 连接并自动注册。
    public func connect() {
        if !initialized {
            mediaEngine.setup(config: config)
            initialized = true
        }
        // setup 会重置引擎内部引用，这里补做渲染器绑定（支持先 bind 再 connect）
        if let local = localRenderer {
            mediaEngine.attachLocalRenderer(local)
        }
        if let remote = remoteRenderer {
            mediaEngine.attachRemoteRenderer(remote)
        }
        reconnectAttempts = 0
        manuallyDisconnected = false
        engine.connect()
    }

    /// 断开连接（会自动注销并挂断当前呼叫）。
    public func disconnect() {
        manuallyDisconnected = true
        cancelReconnect()
        engine.disconnect()
    }

    /// 完全释放资源，之后不可复用。
    public func close() {
        manuallyDisconnected = true
        cancelReconnect()
        engine.disconnect()
        mediaEngine.close()
        initialized = false
    }

    // MARK: - 呼叫

    /// 发起呼叫。target 可为分机号（`1001`）或完整 SIP URI。
    public func call(_ target: String) throws -> NsbcCall {
        return try engine.startCall(target: target)
    }

    /// 当前活跃呼叫，无则返回 nil。
    public func currentCall() -> NsbcCall? { engine.currentCall }

    /// 当前注册状态。
    public var registrationState: RegistrationState { engine.registrationState }

    // MARK: - 媒体

    /// 绑定本地/远端渲染器。两个都可为 nil。
    public func attachRenderers(local: RTCMTLVideoView?, remote: RTCMTLVideoView?) {
        localRenderer = local
        remoteRenderer = remote
        if let local = local {
            mediaEngine.attachLocalRenderer(local)
        }
        if let remote = remote {
            mediaEngine.attachRemoteRenderer(remote)
        }
    }

    public func setMuted(_ muted: Bool) {
        mediaEngine.setMicrophoneEnabled(!muted)
    }

    public func setVideoEnabled(_ enabled: Bool) {
        mediaEngine.setCameraEnabled(enabled)
    }

    public func switchCamera() {
        mediaEngine.switchCamera()
    }

    // MARK: - 重连

    private func scheduleReconnect() {
        guard !manuallyDisconnected else { return }
        guard reconnectAttempts <= 5 else { return }
        let delayMs = config.reconnectDelayMs * (1 << min(reconnectAttempts, 4))
        reconnectAttempts += 1
        cancelReconnect()
        let item = DispatchWorkItem { [weak self] in
            guard let self = self else { return }
            if self.engine.registrationState != .registered {
                self.engine.connect()
            }
        }
        reconnectWorkItem = item
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(delayMs), execute: item)
    }

    private func cancelReconnect() {
        reconnectWorkItem?.cancel()
        reconnectWorkItem = nil
    }
}

// MARK: - SipEngineEvents

extension NsbcClient: SipEngineEvents {

    func sipEngineDidOpenTransport() {
    }

    func sipEngineDidCloseTransport(reason: String) {
        if config.autoReconnect {
            scheduleReconnect()
        }
    }

    func sipEngineDidFailTransport(error: NsbcError) {
        delegate?.nsbcClient(self, didFail: error)
        if config.autoReconnect {
            scheduleReconnect()
        }
    }

    func sipEngineDidChangeRegistration(state: RegistrationState, detail: String?) {
        delegate?.nsbcClient(self, didChangeRegistration: state, detail: detail)
    }

    func sipEngineDidReceiveIncomingCall(_ call: NsbcCall) {
        delegate?.nsbcClient(self, didReceiveIncoming: call)
    }

    func sipEngineDidChangeCall(_ call: NsbcCall) {
        delegate?.nsbcClient(self, didChangeCall: call)
    }
}

// MARK: - MediaEngineDelegate

extension NsbcClient: MediaEngineDelegate {

    func mediaEngine(_ engine: MediaEngine, didChangeIceState state: String) {
        delegate?.nsbcClient(self, didChangeMediaState: state)
    }

    func mediaEngine(_ engine: MediaEngine, didFailWith error: NsbcError) {
        delegate?.nsbcClient(self, didFail: error)
    }
}
