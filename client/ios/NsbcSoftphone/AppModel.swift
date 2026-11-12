import Foundation
import SwiftUI
import AVFoundation
import WebRTC
import NsbcSDK

/// 一次性提示。
struct AlertMessage: Identifiable {
    let id = UUID()
    let text: String

    init(_ text: String) {
        self.text = text
    }
}

/// 最近通话记录（持久化）。
struct RecentCall: Codable, Identifiable {
    let uri: String
    let outgoing: Bool
    let ts: TimeInterval
    let duration: Int64

    var id: String { "\(ts)-\(uri)" }

    /// 从 `sip:1001@nsbc.local` 提取 `1001`
    var number: String {
        var s = uri
        if let range = s.range(of: "sip:") {
            s = String(s[range.upperBound...])
        }
        if let at = s.firstIndex(of: "@") {
            s = String(s[..<at])
        }
        return s
    }
}

/// App 全局状态：注册 / 呼叫 / 配置持久化 / 最近通话。
final class AppModel: ObservableObject {

    // MARK: - 已发布状态

    @Published var config: NsbcConfig
    @Published var registration: RegistrationState = .unregistered
    @Published var registrationDetail: String?
    @Published var currentCall: NsbcCall?
    @Published var callState: CallState = .idle
    @Published var callMediaState: String?
    @Published var showSettings = false
    @Published var showCall = false
    @Published var showIncoming = false
    @Published var dialed = ""
    @Published var muted = false
    @Published var speakerOn = false
    @Published var videoOn = true
    @Published var callSeconds: Int64 = 0
    @Published var recents: [RecentCall] = []
    @Published var problemsText: String?
    @Published var alert: AlertMessage?

    // MARK: - 私有状态

    private var client: NsbcClient?
    private var timer: Timer?
    private weak var localRenderer: RTCMTLVideoView?
    private weak var remoteRenderer: RTCMTLVideoView?

    private static let configKey = "nsbc.config"
    private static let recentsKey = "nsbc.recents"

    init() {
        let cfg = AppModel.loadConfig()
        config = cfg
        videoOn = cfg.videoEnabled
        recents = AppModel.loadRecents()
    }

    // MARK: - 展示用计算属性

    var statusText: String {
        switch registration {
        case .unregistered: return "未注册"
        case .registering: return "注册中…"
        case .registered: return "已注册"
        case .failed: return "注册失败"
        }
    }

    var statusColor: Color {
        switch registration {
        case .unregistered: return NsbcTheme.chipGray
        case .registering: return NsbcTheme.chipAmber
        case .registered: return NsbcTheme.chipGreen
        case .failed: return NsbcTheme.chipRed
        }
    }

    var accountSummary: String {
        if config.server.isEmpty {
            return "未配置服务器"
        }
        return "\(config.sipUri)  ·  \(config.webSocketUrl)"
    }

    var remoteNumber: String {
        guard let uri = currentCall?.remoteUri else { return "" }
        var s = uri
        if let range = s.range(of: "sip:") {
            s = String(s[range.upperBound...])
        }
        if let at = s.firstIndex(of: "@") {
            s = String(s[..<at])
        }
        return s
    }

    var callDisplayName: String {
        if let name = currentCall?.remoteDisplayName, !name.isEmpty {
            return name
        }
        let number = remoteNumber
        return number.isEmpty ? "未知号码" : number
    }

    var callSubtitle: String {
        switch callState {
        case .calling: return "正在呼叫…"
        case .ringing: return "对方振铃…"
        case .established: return "通话中"
        case .failed: return "呼叫失败"
        case .terminated: return "已结束"
        case .idle: return ""
        }
    }

    var callTimeText: String {
        String(format: "%02lld:%02lld", callSeconds / 60, callSeconds % 60)
    }

    // MARK: - 连接

    func saveAndConnect() {
        let cfg = config.normalized()
        let problems = cfg.validate()
        if !problems.isEmpty {
            problemsText = problems.joined(separator: "\n")
            return
        }
        problemsText = nil
        config = cfg
        persistConfig()
        showSettings = false

        requestPermissions { [weak self] granted in
            guard let self = self else { return }
            if !granted {
                self.alert = AlertMessage("缺少摄像头/麦克风权限，通话可能无法建立")
            }
            self.startClient(cfg)
        }
    }

    func disconnect() {
        client?.disconnect()
        stopTimer()
        showIncoming = false
        showCall = false
        currentCall = nil
        callState = .idle
        registration = .unregistered
        registrationDetail = nil
        resetCallAudio()
    }

    func resetDefaults() {
        config = NsbcConfig.sample
        videoOn = config.videoEnabled
        problemsText = nil
        persistConfig()
    }

    private func startClient(_ cfg: NsbcConfig) {
        closeClient()
        do {
            let client = try NsbcClient(config: cfg, delegate: self)
            self.client = client
            client.attachRenderers(local: localRenderer, remote: remoteRenderer)
            client.connect()
        } catch let error as NsbcError {
            alert = AlertMessage(error.message)
        } catch {
            alert = AlertMessage(error.localizedDescription)
        }
    }

    private func closeClient() {
        client?.close()
        client = nil
    }

    private func requestPermissions(completion: @escaping (Bool) -> Void) {
        AVCaptureDevice.requestAccess(for: .video) { videoOK in
            AVCaptureDevice.requestAccess(for: .audio) { audioOK in
                DispatchQueue.main.async {
                    completion(videoOK && audioOK)
                }
            }
        }
    }

    // MARK: - 拨号 / 呼叫

    func appendDigit(_ digit: String) {
        guard dialed.count < 32 else { return }
        dialed += digit
    }

    func backspace() {
        if !dialed.isEmpty {
            dialed.removeLast()
        }
    }

    func placeCall() {
        guard let client = client, registration == .registered else {
            alert = AlertMessage("尚未注册到 NSBC，请先到设置里连接")
            showSettings = true
            return
        }
        guard !dialed.isEmpty else {
            alert = AlertMessage("请输入要呼叫的号码")
            return
        }
        do {
            // 先配置音频会话，再创建 PeerConnection / 启动采集
            prepareCallAudio()
            let call = try client.call(dialed)
            currentCall = call
            callState = .calling
            callSeconds = 0
            dialed = ""
            muted = false
            speakerOn = false
            videoOn = config.videoEnabled
            client.setMuted(false)
            client.setVideoEnabled(config.videoEnabled)
            showCall = true
        } catch let error as NsbcError {
            alert = AlertMessage(error.message)
        } catch {
            alert = AlertMessage(error.localizedDescription)
        }
    }

    func answerCall() {
        guard let call = currentCall else { return }
        muted = false
        speakerOn = false
        videoOn = config.videoEnabled
        client?.setMuted(false)
        client?.setVideoEnabled(config.videoEnabled)
        prepareCallAudio()
        call.answer()
        showIncoming = false
        showCall = true
        callState = .calling
    }

    func rejectCall() {
        currentCall?.reject()
        showIncoming = false
    }

    func hangup() {
        currentCall?.hangup()
    }

    func toggleMute() {
        muted.toggle()
        if let call = currentCall {
            call.setMuted(muted)
        } else {
            client?.setMuted(muted)
        }
    }

    func toggleSpeaker() {
        speakerOn.toggle()
        applyAudioRoute()
    }

    func toggleVideo() {
        videoOn.toggle()
        if let call = currentCall {
            call.setVideoEnabled(videoOn)
        } else {
            client?.setVideoEnabled(videoOn)
        }
    }

    func switchCamera() {
        if let call = currentCall {
            call.switchCamera()
        } else {
            client?.switchCamera()
        }
    }

    // MARK: - 渲染器绑定

    func attachRenderers(local: RTCMTLVideoView, remote: RTCMTLVideoView) {
        localRenderer = local
        remoteRenderer = remote
        client?.attachRenderers(local: local, remote: remote)
    }

    // MARK: - 音频会话

    private func prepareCallAudio() {
        let session = AVAudioSession.sharedInstance()
        do {
            try applyAudioCategory()
            try session.setActive(true)
            try session.overrideOutputAudioPort(speakerOn ? .speaker : .none)
        } catch {
            print("AVAudioSession 配置失败: \(error.localizedDescription)")
        }
    }

    private func applyAudioRoute() {
        let session = AVAudioSession.sharedInstance()
        do {
            try applyAudioCategory()
            try session.overrideOutputAudioPort(speakerOn ? .speaker : .none)
        } catch {
            print("音频路由切换失败: \(error.localizedDescription)")
        }
    }

    /// 免提开启时使用 `.defaultToSpeaker`，关闭时回到听筒。
    private func applyAudioCategory() throws {
        let session = AVAudioSession.sharedInstance()
        let options: AVAudioSession.CategoryOptions = speakerOn ? [.defaultToSpeaker] : []
        try session.setCategory(.playAndRecord, mode: .voiceChat, options: options)
    }

    private func resetCallAudio() {
        let session = AVAudioSession.sharedInstance()
        try? session.setActive(false, options: .notifyOthersOnDeactivation)
        try? session.setCategory(.ambient, mode: .default, options: [])
        speakerOn = false
    }

    // MARK: - 计时

    private func startTimer() {
        stopTimer()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self = self, let call = self.currentCall else { return }
            self.callSeconds = call.durationSeconds
        }
    }

    private func stopTimer() {
        timer?.invalidate()
        timer = nil
    }

    // MARK: - 最近通话

    private func addRecent(_ call: NsbcCall) {
        let item = RecentCall(
            uri: call.remoteUri,
            outgoing: !call.isIncoming,
            ts: Date().timeIntervalSince1970,
            duration: call.durationSeconds
        )
        recents.insert(item, at: 0)
        if recents.count > 30 {
            recents = Array(recents.prefix(30))
        }
        persistRecents()
    }

    // MARK: - 持久化

    private func persistConfig() {
        if let data = try? JSONEncoder().encode(config) {
            UserDefaults.standard.set(data, forKey: AppModel.configKey)
        }
    }

    private static func loadConfig() -> NsbcConfig {
        if let data = UserDefaults.standard.data(forKey: configKey),
           let cfg = try? JSONDecoder().decode(NsbcConfig.self, from: data) {
            return cfg.normalized()
        }
        return .sample
    }

    private func persistRecents() {
        if let data = try? JSONEncoder().encode(recents) {
            UserDefaults.standard.set(data, forKey: AppModel.recentsKey)
        }
    }

    private static func loadRecents() -> [RecentCall] {
        guard let data = UserDefaults.standard.data(forKey: recentsKey) else { return [] }
        return (try? JSONDecoder().decode([RecentCall].self, from: data)) ?? []
    }
}

// MARK: - NsbcClientDelegate

extension AppModel: NsbcClientDelegate {

    func nsbcClient(_ client: NsbcClient, didChangeRegistration state: RegistrationState, detail: String?) {
        DispatchQueue.main.async {
            self.registration = state
            self.registrationDetail = detail
            if state == .failed, let detail = detail, detail.contains("注册被拒绝") {
                self.alert = AlertMessage("注册失败：\(detail)")
            }
        }
    }

    func nsbcClient(_ client: NsbcClient, didReceiveIncoming call: NsbcCall) {
        DispatchQueue.main.async {
            self.currentCall = call
            self.callState = call.state
            self.callSeconds = 0
            self.muted = false
            self.speakerOn = false
            self.videoOn = self.config.videoEnabled
            self.showIncoming = true
            self.showCall = false
            self.prepareCallAudio()
        }
    }

    func nsbcClient(_ client: NsbcClient, didChangeCall call: NsbcCall) {
        DispatchQueue.main.async {
            guard let current = self.currentCall, current === call else { return }
            self.callState = call.state
            switch call.state {
            case .established:
                self.prepareCallAudio()
                self.startTimer()
            case .terminated, .failed:
                self.stopTimer()
                self.callSeconds = call.durationSeconds
                self.addRecent(call)
                self.showIncoming = false
                self.showCall = false
                self.currentCall = nil
                self.resetCallAudio()
                self.muted = false
                self.videoOn = self.config.videoEnabled
            default:
                break
            }
        }
    }

    func nsbcClient(_ client: NsbcClient, didChangeMediaState state: String) {
        DispatchQueue.main.async {
            self.callMediaState = state
        }
    }

    func nsbcClient(_ client: NsbcClient, didFail error: NsbcError) {
        DispatchQueue.main.async {
            if self.alert == nil {
                self.alert = AlertMessage(error.message)
            }
        }
    }
}
