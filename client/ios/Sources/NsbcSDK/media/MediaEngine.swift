import Foundation
import AVFoundation
import CoreMedia
import WebRTC
import UIKit

/// 媒体引擎事件。
protocol MediaEngineDelegate: AnyObject {
    func mediaEngine(_ engine: MediaEngine, didChangeIceState state: String)
    func mediaEngine(_ engine: MediaEngine, didFailWith error: NsbcError)
}

/// 媒体引擎抽象。SDK 默认使用 `WebRtcMediaEngine`。
protocol MediaEngine: AnyObject {
    var delegate: MediaEngineDelegate? { get set }

    /// 初始化引擎（创建 PeerConnectionFactory）。
    func setup(config: NsbcConfig)

    /// 创建本地 offer（含已收集的 ICE candidate），呼出场景使用。
    func createOffer(completion: @escaping (Result<String, NsbcError>) -> Void)

    /// 创建本地 answer（含已收集的 ICE candidate），呼入场景使用。
    func createAnswer(completion: @escaping (Result<String, NsbcError>) -> Void)

    /// 应用远端 SDP。isOffer=true 表示对端 offer（呼入），false 表示 answer（呼出）。
    func applyRemoteSdp(_ sdp: String, isOffer: Bool, completion: @escaping (Result<Void, NsbcError>) -> Void)

    /// 绑定本地预览渲染器。
    func attachLocalRenderer(_ view: RTCMTLVideoView)

    /// 绑定远端渲染器。
    func attachRemoteRenderer(_ view: RTCMTLVideoView)

    func setMicrophoneEnabled(_ enabled: Bool)
    func setCameraEnabled(_ enabled: Bool)
    func switchCamera()

    /// 通话结束：停止摄像头采集（省电 + 隐私）。
    func onCallStopped()

    func close()
}

/// 基于 Google WebRTC 的媒体引擎实现。
///
/// - 音频：AEC / NS / AGC 由 WebRTC 内置处理
/// - 视频：摄像头采集 + 硬件编解码
/// - 传输：ICE + DTLS-SRTP（与 NSBC 的 `media.force_srtp` 对齐）
final class WebRtcMediaEngine: NSObject, MediaEngine {

    weak var delegate: MediaEngineDelegate?

    private static var sslInitialized = false
    private static let mediaStreamId = "nsbc"

    private var config: NsbcConfig?
    private var factory: RTCPeerConnectionFactory?
    private var peerConnection: RTCPeerConnection?

    /// 单路呼叫下 restartIce 只尝试一次，避免无限循环。
    private var iceRestartAttempted = false

    private var audioSource: RTCAudioSource?
    private var audioTrack: RTCAudioTrack?

    private var videoSource: RTCVideoSource?
    private var videoTrack: RTCVideoTrack?
    private var capturer: RTCCameraVideoCapturer?
    private var captureDevice: AVCaptureDevice?
    private var captureFormat: AVCaptureDevice.Format?
    private var isCapturing = false

    private weak var localRenderer: RTCMTLVideoView?
    private weak var remoteRenderer: RTCMTLVideoView?
    private var remoteTrack: RTCVideoTrack?

    /// 呼入 offer 是否包含 m=video —— 决定 answer 是否添加视频轨
    private var remoteOfferHasVideo = false

    private var micEnabled = true
    private var cameraEnabled = true

    // MARK: - 初始化

    func setup(config: NsbcConfig) {
        self.config = config
        releaseInternal()

        if !WebRtcMediaEngine.sslInitialized {
            _ = RTCInitializeSSL()
            WebRtcMediaEngine.sslInitialized = true
        }

        let encoder = RTCDefaultVideoEncoderFactory()
        let decoder = RTCDefaultVideoDecoderFactory()
        factory = RTCPeerConnectionFactory(encoderFactory: encoder, decoderFactory: decoder)
    }

    // MARK: - offer / answer

    func createOffer(completion: @escaping (Result<String, NsbcError>) -> Void) {
        guard factory != nil, let cfg = config else {
            finish(completion, .failure(NsbcError(.mediaFailed, "引擎未初始化")))
            return
        }

        // 每次呼出都用全新的 PeerConnection，避免上一通呼叫的 transceiver 残留
        resetPeerConnection()
        let pc = ensurePeerConnection()
        attachLocalTracks(pc, allowVideo: cfg.videoEnabled)

        // Unified Plan 下 MediaConstraints 的 OfferToReceiveAudio/Video 已废弃，
        // 方向由显式 addTrack/addTransceiver 决定；传 nil 使用 libwebrtc 默认行为。
        pc.offer(for: nil) { [weak self] desc, error in
            guard let self = self else { return }
            guard let desc = desc else {
                self.finish(completion, .failure(NsbcError(.mediaFailed, "createOffer 失败: \(error?.localizedDescription ?? "unknown")")))
                return
            }
            // 商业级 SDP 过滤：H.264 baseline + opus 带宽
            let filteredSdp = self.filterOfferSdp(desc.sdp)
            let filteredDesc = RTCSessionDescription(type: desc.type, sdp: filteredSdp)
            pc.setLocalDescription(filteredDesc) { [weak self] error in
                guard let self = self else { return }
                if let error = error {
                    self.finish(completion, .failure(NsbcError(.mediaFailed, "setLocalDescription 失败: \(error.localizedDescription)")))
                    return
                }
                // 等待 ICE 收集完成后再返回完整 SDP（最多 3 秒）
                self.waitIceComplete(pc, elapsedMs: 0, completion: completion)
            }
        }
    }

    func createAnswer(completion: @escaping (Result<String, NsbcError>) -> Void) {
        guard let cfg = config else {
            finish(completion, .failure(NsbcError(.mediaFailed, "引擎未初始化")))
            return
        }
        let pc = ensurePeerConnection()
        // 对端 offer 没有 m=video 时不能凭空加视频轨，否则 answer 段数多于 offer
        attachLocalTracks(pc, allowVideo: cfg.videoEnabled && remoteOfferHasVideo)

        // Unified Plan 下 MediaConstraints 对 answer 无效，方向由远端 offer 决定
        pc.answer(for: nil) { [weak self] desc, error in
            guard let self = self else { return }
            guard let desc = desc else {
                self.finish(completion, .failure(NsbcError(.mediaFailed, "createAnswer 失败: \(error?.localizedDescription ?? "unknown")")))
                return
            }
            let filteredSdp = self.filterAnswerSdp(desc.sdp)
            let filteredDesc = RTCSessionDescription(type: desc.type, sdp: filteredSdp)
            pc.setLocalDescription(filteredDesc) { [weak self] error in
                guard let self = self else { return }
                if let error = error {
                    self.finish(completion, .failure(NsbcError(.mediaFailed, "setLocalDescription 失败: \(error.localizedDescription)")))
                    return
                }
                self.waitIceComplete(pc, elapsedMs: 0, completion: completion)
            }
        }
    }

    func applyRemoteSdp(_ sdp: String, isOffer: Bool, completion: @escaping (Result<Void, NsbcError>) -> Void) {
        if isOffer {
            remoteOfferHasVideo = sdp.contains("m=video ")
            // 呼入是新一轮协商，先清掉上一通呼叫遗留的 PeerConnection
            resetPeerConnection()
        }
        let pc = ensurePeerConnection()
        let desc = RTCSessionDescription(type: isOffer ? .offer : .answer, sdp: sdp)
        pc.setRemoteDescription(desc) { [weak self] error in
            guard let self = self else { return }
            if let error = error {
                self.finish(completion, .failure(NsbcError(.mediaFailed, "setRemoteDescription 失败: \(error.localizedDescription)")))
            } else {
                self.finish(completion, .success(()))
            }
        }
    }

    private func waitIceComplete(
        _ pc: RTCPeerConnection,
        elapsedMs: Int,
        completion: @escaping (Result<String, NsbcError>) -> Void
    ) {
        let complete = pc.iceGatheringState == .complete
        if complete || elapsedMs >= 3000 {
            if let sdp = pc.localDescription?.sdp {
                finish(completion, .success(sdp))
            } else {
                finish(completion, .failure(NsbcError(.mediaFailed, "本地 SDP 为空")))
            }
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(200)) { [weak self] in
            self?.waitIceComplete(pc, elapsedMs: elapsedMs + 200, completion: completion)
        }
    }

    /// 统一回到主线程回调，避免上层 UI 状态竞争。
    private func finish<T>(_ completion: @escaping (Result<T, NsbcError>) -> Void, _ result: Result<T, NsbcError>) {
        DispatchQueue.main.async {
            completion(result)
        }
    }

    // MARK: - 渲染

    func attachLocalRenderer(_ view: RTCMTLVideoView) {
        localRenderer = view
        setupRenderer(view)
        videoTrack?.remove(view)
        videoTrack?.add(view)
    }

    func attachRemoteRenderer(_ view: RTCMTLVideoView) {
        remoteRenderer = view
        setupRenderer(view)
        if let track = remoteTrack {
            track.remove(view)
            track.add(view)
        }
    }

    private func setupRenderer(_ view: RTCMTLVideoView) {
        view.videoContentMode = .scaleAspectFill
    }

    private func bindRemoteTrack(_ track: RTCVideoTrack) {
        remoteTrack = track
        track.isEnabled = true
        if let renderer = remoteRenderer {
            track.remove(renderer)
            track.add(renderer)
        }
    }

    // MARK: - 设备控制

    func setMicrophoneEnabled(_ enabled: Bool) {
        micEnabled = enabled
        audioTrack?.isEnabled = enabled
    }

    func setCameraEnabled(_ enabled: Bool) {
        cameraEnabled = enabled
        videoTrack?.isEnabled = enabled
        // 关闭时真正停采集（省电 + 隐私），重新打开时恢复
        if enabled {
            startCaptureIfNeeded()
        } else {
            stopCapture()
        }
    }

    func switchCamera() {
        guard isCapturing else { return }
        capturer?.switchCamera()
    }

    func onCallStopped() {
        // 通话结束停止采集；下一次呼叫在 attachLocalTracks 里自动恢复
        stopCapture()
    }

    // MARK: - PeerConnection

    private func ensurePeerConnection() -> RTCPeerConnection {
        if let pc = peerConnection { return pc }
        guard let factory = factory, let cfg = config else {
            fatalError("PeerConnectionFactory 未初始化")
        }

        var iceServers: [RTCIceServer] = []
        if let stun = cfg.stunServer, !stun.isEmpty {
            iceServers.append(RTCIceServer(urlStrings: [stun]))
        }
        if let turn = cfg.turnServer, !turn.isEmpty {
            iceServers.append(RTCIceServer(
                urlStrings: [turn],
                username: cfg.turnUsername,
                credential: cfg.turnCredential
            ))
        }

        let rtc = RTCConfiguration()
        rtc.iceServers = iceServers
        rtc.sdpSemantics = .unifiedPlan
        rtc.bundlePolicy = .maxBundle
        rtc.rtcpMuxPolicy = .require
        if cfg.icePolicy == .relay {
            rtc.iceTransportPolicy = .relay
        }
        // 预生成候选池加速首次 ICE 收集（商业软电话标配）
        rtc.iceCandidatePoolSize = 10

        let constraints = RTCMediaConstraints(mandatoryConstraints: nil, optionalConstraints: nil)
        guard let pc = factory.peerConnection(with: rtc, constraints: constraints, delegate: self) else {
            fatalError("createPeerConnection 返回 nil")
        }
        peerConnection = pc
        iceRestartAttempted = false
        return pc
    }

    private func resetPeerConnection() {
        remoteTrack = nil
        peerConnection?.close()
        peerConnection = nil
        iceRestartAttempted = false
    }

    private func attachLocalTracks(_ pc: RTCPeerConnection, allowVideo: Bool) {
        guard let factory = factory else { return }

        // 音频：显式 addTransceiver 保证 Unified Plan 方向可控（sendRecv），跨端与 Android/desktop 对齐
        let audio: RTCAudioTrack
        if let existing = audioTrack {
            audio = existing
        } else {
            let source = factory.audioSource(with: RTCMediaConstraints(mandatoryConstraints: nil, optionalConstraints: nil))
            audio = factory.audioTrack(with: source, trackId: "audio0")
            audioSource = source
            audioTrack = audio
        }
        let audioInit = RTCRtpTransceiverInit()
        audioInit.direction = .sendRecv
        _ = pc.addTransceiver(with: .audio, init: audioInit)
        runCatching { pc.transceivers.last?.sender.setTrack(audio) }
        audio.isEnabled = micEnabled

        guard allowVideo else { return }

        // 视频：显式 addTransceiver + sender.setTrack 绑定本地视频轨
        guard let track = ensureVideoTrack() else { return }
        let videoInit = RTCRtpTransceiverInit()
        videoInit.direction = .sendRecv
        _ = pc.addTransceiver(with: .video, init: videoInit)
        runCatching { pc.transceivers.last?.sender.setTrack(track) }
        track.isEnabled = cameraEnabled
        if cameraEnabled {
            startCaptureIfNeeded()
        }
        if let renderer = localRenderer {
            // 第二次呼叫复用同一 track，先移除再添加，避免重复 sink
            track.remove(renderer)
            track.add(renderer)
        }
    }

    /// 创建（或复用）摄像头采集链路与视频轨；失败返回 nil 并降级为音频。
    private func ensureVideoTrack() -> RTCVideoTrack? {
        if let track = videoTrack { return track }
        guard let factory = factory, let cfg = config else { return nil }

        let devices = RTCCameraVideoCapturer.captureDevices()
            .filter { $0.deviceType == .builtInWideAngleCamera }
        guard let device = devices.first(where: { $0.position == .front }) ?? devices.first else {
            delegate?.mediaEngine(self, didFailWith: NsbcError(.mediaFailed, "摄像头不可用"))
            return nil
        }
        guard let format = selectFormat(for: device, width: cfg.videoWidth, height: cfg.videoHeight) else {
            delegate?.mediaEngine(self, didFailWith: NsbcError(.mediaFailed, "摄像头没有可用格式"))
            return nil
        }

        let source = factory.videoSource()
        let capturer = RTCCameraVideoCapturer(delegate: source)
        let track = factory.videoTrack(with: source, trackId: "video0")

        self.videoSource = source
        self.videoTrack = track
        self.capturer = capturer
        self.captureDevice = device
        self.captureFormat = format

        if let renderer = localRenderer {
            track.add(renderer)
        }
        return track
    }

    /// 选择与目标分辨率最接近的采集格式。
    private func selectFormat(for device: AVCaptureDevice, width: Int, height: Int) -> AVCaptureDevice.Format? {
        let formats = RTCCameraVideoCapturer.supportedFormats(for: device)
        var best: AVCaptureDevice.Format?
        var bestDiff = Int.max
        for format in formats {
            let dims = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
            let w = Int(dims.width)
            let h = Int(dims.height)
            guard w >= 160, h >= 120 else { continue }
            let diff = abs(w - width) + abs(h - height)
            if diff < bestDiff {
                bestDiff = diff
                best = format
            }
        }
        return best ?? formats.first
    }

    private func startCaptureIfNeeded() {
        guard let capturer = capturer,
              let device = captureDevice,
              let format = captureFormat,
              !isCapturing else { return }

        let maxFps = format.videoSupportedFrameRateRanges.first?.maxFrameRate ?? 30
        let desired = Double(config?.videoFps ?? 30)
        let fps = max(1, Int(min(desired, maxFps)))

        capturer.startCapture(with: device, format: format, fps: fps)
        isCapturing = true
    }

    private func stopCapture() {
        guard isCapturing else { return }
        capturer?.stopCapture()
        isCapturing = false
    }

    // MARK: - 商业级 SDP 过滤

    private static let h264FmtpRegexPattern = "a=fmtp:\\d+ profile-level-id=[0-9a-fA-F]{6}($|[^\\r\\n]*)"
    private static let opusFmtpRegexPattern = "a=fmtp:111[^\\r\\n]*"

    /// Offer SDP 过滤（商业软电话兼容策略）：
    ///  - H.264 profile-level-id 非 baseline (42e01f) → 降级为 42e01f 并强制 packetization-mode=1
    ///  - opus maxplaybackrate=8000（移动网络节省带宽）
    private func filterOfferSdp(_ sdp: String) -> String {
        var out = sdp
        out = replaceRegex(Self.h264FmtpRegexPattern, in: out) { line in
            var replaced = line.replacingOccurrences(
                of: "profile-level-id=[0-9a-fA-F]{6}",
                with: "profile-level-id=42e01f",
                options: .regularExpression
            )
            if !replaced.contains("packetization-mode") {
                replaced += ";packetization-mode=1"
            }
            return replaced
        }
        out = replaceRegex(Self.opusFmtpRegexPattern, in: out) { line in
            if line.contains("maxplaybackrate") {
                return line
            }
            return line + ";maxplaybackrate=8000"
        }
        return out
    }

    /// Answer SDP 过滤：只处理 opus maxplaybackrate 与 H.264 packetization-mode，
    /// 不降级 profile-level-id（那是 offer 才做的事）。
    private func filterAnswerSdp(_ sdp: String) -> String {
        var out = sdp
        out = replaceRegex(Self.h264FmtpRegexPattern, in: out) { line in
            if line.contains("packetization-mode") {
                return line
            }
            return line + ";packetization-mode=1"
        }
        out = replaceRegex(Self.opusFmtpRegexPattern, in: out) { line in
            if line.contains("maxplaybackrate") {
                return line
            }
            return line + ";maxplaybackrate=8000"
        }
        return out
    }

    /// 正则替换：遍历所有匹配项，倒序替换避免 range 偏移。
    private func replaceRegex(_ pattern: String, in text: String, _ handler: (String) -> String) -> String {
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return text }
        let ns = text as NSString
        let matches = regex.matches(in: text, range: NSRange(location: 0, length: ns.length))
        if matches.isEmpty { return text }
        var out = text
        for match in matches.reversed() {
            let matched = ns.substring(with: match.range)
            let replacement = handler(matched)
            if matched != replacement {
                out = (out as NSString).replacingCharacters(in: match.range, with: replacement)
            }
        }
        return out
    }

    // MARK: - 释放

    func close() {
        releaseInternal()
    }

    private func releaseInternal() {
        stopCapture()
        resetPeerConnection()
        capturer = nil
        captureDevice = nil
        captureFormat = nil
        videoTrack = nil
        videoSource = nil
        audioTrack = nil
        audioSource = nil
        remoteTrack = nil
        localRenderer = nil
        remoteRenderer = nil
        factory = nil
    }
}

// MARK: - RTCPeerConnectionDelegate

extension WebRtcMediaEngine: RTCPeerConnectionDelegate {

    func peerConnection(_ peerConnection: RTCPeerConnection, didChange stateChanged: RTCSignalingState) {
    }

    func peerConnection(_ peerConnection: RTCPeerConnection, didAdd stream: RTCMediaStream) {
        guard let track = stream.videoTracks.first else { return }
        DispatchQueue.main.async { [weak self] in
            self?.bindRemoteTrack(track)
        }
    }

    func peerConnection(_ peerConnection: RTCPeerConnection, didRemove stream: RTCMediaStream) {
    }

    func peerConnectionShouldNegotiate(_ peerConnection: RTCPeerConnection) {
    }

    func peerConnection(_ peerConnection: RTCPeerConnection, didChange newState: RTCIceConnectionState) {
        let name = WebRtcMediaEngine.iceStateName(newState)
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.delegate?.mediaEngine(self, didChangeIceState: name)
        }
        // 商业软电话标配：ICE failed 时尝试 restartIce 触发重新收集候选；
        // 只尝试一次，避免在网络完全不通时无限循环。
        if newState == .failed, !iceRestartAttempted {
            iceRestartAttempted = true
            peerConnection.restartIce()
        }
    }

    func peerConnection(_ peerConnection: RTCPeerConnection, didChange newState: RTCIceGatheringState) {
    }

    func peerConnection(_ peerConnection: RTCPeerConnection, didGenerate candidate: RTCIceCandidate) {
        // Trickle ICE：本 SDK 采用「收集完成后一次性发送 SDP」，
        // 因此丢弃 trickle candidate，最终 candidate 已包含在 SDP 中。
    }

    func peerConnection(_ peerConnection: RTCPeerConnection, didRemove candidates: [RTCIceCandidate]) {
    }

    func peerConnection(_ peerConnection: RTCPeerConnection, didOpen dataChannel: RTCDataChannel) {
    }

    func peerConnection(
        _ peerConnection: RTCPeerConnection,
        didAdd rtpReceiver: RTCRtpReceiver,
        streams mediaStreams: [RTCMediaStream]
    ) {
        guard let track = rtpReceiver.track as? RTCVideoTrack else { return }
        DispatchQueue.main.async { [weak self] in
            self?.bindRemoteTrack(track)
        }
    }

    func peerConnection(_ peerConnection: RTCPeerConnection, didRemove rtpReceiver: RTCRtpReceiver) {
    }

    private static func iceStateName(_ state: RTCIceConnectionState) -> String {
        switch state {
        case .new: return "new"
        case .checking: return "checking"
        case .connected: return "connected"
        case .completed: return "completed"
        case .failed: return "failed"
        case .disconnected: return "disconnected"
        case .closed: return "closed"
        case .count: return "count"
        @unknown default: return "unknown"
        }
    }
}
