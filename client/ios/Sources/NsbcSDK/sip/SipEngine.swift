import Foundation

/// SipEngine 向上层（NsbcClient）抛出的回调。
protocol SipEngineEvents: AnyObject {
    func sipEngineDidOpenTransport()
    func sipEngineDidCloseTransport(reason: String)
    func sipEngineDidFailTransport(error: NsbcError)
    func sipEngineDidChangeRegistration(state: RegistrationState, detail: String?)
    func sipEngineDidReceiveIncomingCall(_ call: NsbcCall)
    func sipEngineDidChangeCall(_ call: NsbcCall)
}

/// SIP 信令引擎：负责 REGISTER 与呼叫状态机（RFC 3261 / RFC 7118）。
///
/// 只处理「一路呼叫」，足以覆盖手机端场景；所有状态变更都在主线程执行。
final class SipEngine: NSObject, SipSocketListener {

    weak var events: SipEngineEvents?

    private let config: NsbcConfig
    private let mediaEngine: MediaEngine
    private let socket: SipSocket
    private let localHost: String
    private let displayName: String
    /// Via 的传输协议：TLS 连接用 WSS，明文连接用 WS
    private let viaProtocol: String
    /// RFC 7989 Session-ID，init 时生成，贯穿全生命周期
    private let sessionId: String

    private static let userAgent = "NsbcIOSSDK/1.0"

    // MARK: - 注册状态

    private(set) var registrationState: RegistrationState = .unregistered

    private var regCseq = 0
    private var regCallId = ""
    private var regTag = ""
    private var regBranch = ""
    private var regRefreshWorkItem: DispatchWorkItem?

    // MARK: - OPTIONS keepalive（RFC 5626 outbound NAT 保活）

    private var keepaliveWorkItem: DispatchWorkItem?
    private var keepaliveCallId = ""
    private var keepaliveCseq = 0
    private var keepaliveMisses = 0

    /// 鉴权缓存（按 realm）
    private final class AuthChallenge {
        let realm: String
        let nonce: String
        let qop: String?
        let opaque: String?
        let algorithm: String
        let proxy: Bool
        var nc: Int = 0

        init(realm: String, nonce: String, qop: String?, opaque: String?, algorithm: String, proxy: Bool) {
            self.realm = realm
            self.nonce = nonce
            self.qop = qop
            self.opaque = opaque
            self.algorithm = algorithm
            self.proxy = proxy
        }
    }

    private var regAuth: AuthChallenge?

    // MARK: - 当前呼叫

    private final class CallContext {
        let call: NsbcCall
        let callId: String
        let localTag: String
        let isIncoming: Bool
        var remoteUri: String

        var remoteTag: String?
        var branch: String = SipUtils.newBranch()
        var cseq: Int = 0
        var localSdp: String?
        var remoteSdp: String?
        var remoteTarget: String?
        var auth: AuthChallenge?
        var acked = false
        /// 呼入场景缓存 INVITE 原始请求，用于构造 200 / 487 响应
        var inviteRequest: SipMessage?

        init(call: NsbcCall, callId: String, localTag: String, isIncoming: Bool, remoteUri: String) {
            self.call = call
            self.callId = callId
            self.localTag = localTag
            self.isIncoming = isIncoming
            self.remoteUri = remoteUri
        }
    }

    private var ctx: CallContext?

    /// 当前活跃呼叫；无则 nil。
    var currentCall: NsbcCall? { ctx?.call }

    // MARK: - 生命周期

    init(config: NsbcConfig, mediaEngine: MediaEngine) {
        self.config = config
        self.mediaEngine = mediaEngine
        self.displayName = config.displayName.isEmpty ? config.username : config.displayName
        self.viaProtocol = config.useTls ? "WSS" : "WS"
        self.localHost = SipUtils.localIPv4()
        self.sessionId = SipUtils.newTag() + "@" + config.server
        self.socket = SipSocket(config: config)
        super.init()
        self.socket.listener = self
    }

    func connect() {
        socket.connect()
    }

    func disconnect() {
        cancelRefresh()
        cancelKeepalive()
        if let c = ctx {
            if c.call.state == .established {
                sendBye(c)
            } else {
                sendCancel(c)
            }
        }
        ctx = nil
        if registrationState == .registered {
            sendUnregister()
        }
        socket.disconnect()
        notifyRegistration(.unregistered, detail: "已主动断开")
    }

    // MARK: - Transport 回调

    func onSocketOpen() {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.events?.sipEngineDidOpenTransport()
            self.sendRegister(auth: nil)
        }
    }

    func onSocketMessage(_ raw: String) {
        DispatchQueue.main.async { [weak self] in
            self?.handleSipMessage(raw)
        }
    }

    func onSocketClosed(code: Int, reason: String) {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.cancelRefresh()
            self.cancelKeepalive()
            self.notifyRegistration(.unregistered, detail: "连接关闭: \(reason)")
            self.events?.sipEngineDidCloseTransport(reason: reason)
        }
    }

    func onSocketFailure(_ error: NsbcError) {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.cancelRefresh()
            self.cancelKeepalive()
            self.notifyRegistration(.failed, detail: error.message)
            self.events?.sipEngineDidFailTransport(error: error)
        }
    }

    private func handleSipMessage(_ raw: String) {
        guard let msg = SipMessage.parse(raw) else { return }
        if msg.isResponse {
            handleResponse(msg)
            return
        }
        switch msg.method {
        case "INVITE":
            handleIncomingInvite(msg)
        case "ACK":
            handleIncomingAck(msg)
        case "BYE":
            handleIncomingBye(msg)
        case "CANCEL":
            handleIncomingCancel(msg)
        case "OPTIONS":
            reply(msg, code: 200, reason: "OK") { resp in
                resp.addHeader("Allow", "INVITE, ACK, CANCEL, BYE, OPTIONS, MESSAGE")
                resp.addHeader("Supported", "replaces, outbound")
            }
        case "MESSAGE":
            handleIncomingMessage(msg)
        default:
            reply(msg, code: 405, reason: "Method Not Allowed")
        }
    }

    // MARK: - REGISTER

    private func sendRegister(auth: AuthChallenge?) {
        notifyRegistration(.registering, detail: nil)

        if regCallId.isEmpty {
            regCallId = SipUtils.newCallId(host: config.server)
            regTag = SipUtils.newTag()
        }
        regBranch = SipUtils.newBranch()
        regCseq += 1

        let req = SipMessage()
        req.method = "REGISTER"
        req.requestUri = "sip:\(config.realm)"
        req.addHeader("Via", "SIP/2.0/\(viaProtocol) \(localHost);branch=\(regBranch);rport")
        req.addHeader("Max-Forwards", "70")
        req.addHeader("From", "\"\(displayName)\" <\(config.sipUri)>;tag=\(regTag)")
        req.addHeader("To", "<\(config.sipUri)>")
        req.addHeader("Call-ID", regCallId)
        req.addHeader("CSeq", "\(regCseq) REGISTER")
        req.addHeader("Contact", "<\(config.sipUri);transport=\(config.contactTransport)>")
        req.addHeader("Expires", "\(config.registerExpiresSec)")
        req.addHeader("Allow", "INVITE, ACK, CANCEL, BYE, OPTIONS, MESSAGE")
        req.addHeader("Supported", "replaces, outbound")
        req.addHeader("Session-ID", sessionId)
        req.addHeader("User-Agent", SipEngine.userAgent)

        if let a = auth {
            let uri = "sip:\(config.realm)"
            req.addHeader(a.proxy ? "Proxy-Authorization" : "Authorization",
                          buildAuthValue(a, method: "REGISTER", uri: uri))
        }
        socket.send(req.toWire())
    }

    private func sendUnregister() {
        // Expires: 0 注销
        regBranch = SipUtils.newBranch()
        regCseq += 1

        let req = SipMessage()
        req.method = "REGISTER"
        req.requestUri = "sip:\(config.realm)"
        req.addHeader("Via", "SIP/2.0/\(viaProtocol) \(localHost);branch=\(regBranch);rport")
        req.addHeader("Max-Forwards", "70")
        req.addHeader("From", "\"\(displayName)\" <\(config.sipUri)>;tag=\(regTag)")
        req.addHeader("To", "<\(config.sipUri)>")
        req.addHeader("Call-ID", regCallId)
        req.addHeader("CSeq", "\(regCseq) REGISTER")
        req.addHeader("Contact", "<\(config.sipUri);transport=\(config.contactTransport)>")
        req.addHeader("Expires", "0")
        req.addHeader("Supported", "replaces, outbound")
        req.addHeader("Session-ID", sessionId)
        req.addHeader("User-Agent", SipEngine.userAgent)

        if let a = regAuth {
            req.addHeader(a.proxy ? "Proxy-Authorization" : "Authorization",
                          buildAuthValue(a, method: "REGISTER", uri: "sip:\(config.realm)"))
        }
        socket.send(req.toWire())
    }

    private func scheduleRefresh(expiresSec: Int) {
        cancelRefresh()
        cancelKeepalive()
        let delayMs = (max(expiresSec, 30) * 1000) / 2
        let item = DispatchWorkItem { [weak self] in
            guard let self = self else { return }
            guard self.registrationState == .registered, self.socket.isOpen else { return }
            self.sendRegister(auth: self.regAuth)
        }
        regRefreshWorkItem = item
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(delayMs), execute: item)
        // 商业软电话标配：注册后每 30s 发一次 OPTIONS 做 NAT keepalive
        scheduleKeepalive()
    }

    private func cancelRefresh() {
        regRefreshWorkItem?.cancel()
        regRefreshWorkItem = nil
    }

    // MARK: - OPTIONS keepalive

    private func scheduleKeepalive() {
        cancelKeepalive()
        let item = DispatchWorkItem { [weak self] in
            guard let self = self else { return }
            guard self.registrationState == .registered, self.socket.isOpen else { return }
            self.sendOptionsKeepalive()
            // 下一轮：重排一个 workItem（模拟 Android 的 Runnable 自续）
            let next = DispatchWorkItem { [weak self] in
                guard let self = self else { return }
                guard self.registrationState == .registered, self.socket.isOpen else { return }
                self.sendOptionsKeepalive()
            }
            self.keepaliveWorkItem = next
            DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(30_000), execute: next)
        }
        keepaliveWorkItem = item
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(30_000), execute: item)
    }

    private func cancelKeepalive() {
        keepaliveWorkItem?.cancel()
        keepaliveWorkItem = nil
    }

    /**
     * 发送 OPTIONS keepalive 到 SIP 服务器（RFC 5626 outbound 机制）。
     * 使用独立 Call-ID，响应通过 handleResponse 的 CSeq/Call-ID 分支过滤，
     * 200 静默处理；3 次无响应视作 NAT 失效，触发重注册。
     */
    private func sendOptionsKeepalive() {
        if keepaliveCallId.isEmpty {
            keepaliveCallId = SipUtils.newCallId(host: config.server)
        }
        keepaliveCseq += 1

        let req = SipMessage()
        req.method = "OPTIONS"
        req.requestUri = "sip:\(config.realm)"
        req.addHeader("Via", "SIP/2.0/\(viaProtocol) \(localHost);branch=\(SipUtils.newBranch());rport")
        req.addHeader("Max-Forwards", "70")
        req.addHeader("From", "\"\(displayName)\" <\(config.sipUri)>;tag=\(regTag)")
        req.addHeader("To", "<sip:\(config.realm)>")
        req.addHeader("Call-ID", keepaliveCallId)
        req.addHeader("CSeq", "\(keepaliveCseq) OPTIONS")
        req.addHeader("Supported", "replaces, outbound")
        req.addHeader("Session-ID", sessionId)
        req.addHeader("User-Agent", SipEngine.userAgent)
        socket.send(req.toWire())
    }

    // MARK: - 响应处理

    private func handleResponse(_ msg: SipMessage) {
        let cseqMethod = msg.cseqMethod()
        let callId = msg.callId()

        if cseqMethod == "REGISTER" && callId == regCallId {
            handleRegisterResponse(msg)
        } else if cseqMethod == "OPTIONS" && callId == keepaliveCallId {
            // OPTIONS keepalive 响应
            if let code = msg.statusCode, code == 200 {
                keepaliveMisses = 0
            } else {
                keepaliveMisses += 1
                if keepaliveMisses >= 3 {
                    keepaliveMisses = 0
                    cancelRefresh()
                    sendRegister(auth: regAuth)
                }
            }
        } else if cseqMethod == "INVITE" && ctx?.callId == callId {
            handleInviteResponse(msg)
        } else if cseqMethod == "BYE" || cseqMethod == "CANCEL" {
            // 无需额外处理，仅记录
        }
    }

    private func handleRegisterResponse(_ msg: SipMessage) {
        guard let code = msg.statusCode else { return }
        switch code {
        case 200:
            let expires = msg.header("Expires").flatMap { Int($0) } ?? config.registerExpiresSec
            notifyRegistration(.registered, detail: nil)
            scheduleRefresh(expiresSec: expires)
        case 401, 407:
            let challengeHeader = code == 401
                ? msg.header("WWW-Authenticate")
                : msg.header("Proxy-Authenticate")
            guard let challenge = challengeHeader else {
                failRegistration("缺少鉴权挑战头")
                return
            }
            let params = DigestAuth.parseChallenge(challenge)
            regAuth = AuthChallenge(
                realm: params["realm"] ?? config.realm,
                nonce: params["nonce"] ?? "",
                qop: params["qop"],
                opaque: params["opaque"],
                algorithm: params["algorithm"] ?? "MD5",
                proxy: code == 407
            )
            sendRegister(auth: regAuth)
        case 400...699:
            failRegistration("注册被拒绝: \(code) \(msg.reasonPhrase ?? "")")
        default:
            break
        }
    }

    private func failRegistration(_ detail: String) {
        notifyRegistration(.failed, detail: detail)
    }

    private func handleInviteResponse(_ msg: SipMessage) {
        guard let c = ctx, let code = msg.statusCode else { return }
        switch code {
        case 100...199:
            if code == 180 || code == 183 {
                updateCall(c, state: .ringing)
            }
            // 183 带 early media SDP：可选，这里不处理
        case 200:
            c.remoteTag = msg.toTag()
            c.remoteTarget = contactUri(msg.header("Contact")) ?? c.remoteUri
            sendAck(c)

            let sdp = msg.body
            if !sdp.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                c.remoteSdp = sdp
                mediaEngine.applyRemoteSdp(sdp, isOffer: false) { [weak self] result in
                    guard let self = self else { return }
                    switch result {
                    case .success:
                        self.establish(c)
                    case .failure:
                        self.failCall(c, reason: "媒体协商失败")
                    }
                }
            } else {
                establish(c)
            }
        case 401, 407:
            let headerName = code == 401 ? "WWW-Authenticate" : "Proxy-Authenticate"
            guard let h = msg.header(headerName) else {
                failCall(c, reason: "缺少鉴权挑战头")
                return
            }
            let params = DigestAuth.parseChallenge(h)
            c.auth = AuthChallenge(
                realm: params["realm"] ?? config.realm,
                nonce: params["nonce"] ?? "",
                qop: params["qop"],
                opaque: params["opaque"],
                algorithm: params["algorithm"] ?? "MD5",
                proxy: code == 407
            )
            sendInvite(c)
        case 300...699:
            let reason: String
            switch code {
            case 486, 600: reason = "busy"
            case 487: reason = "canceled"
            case 408: reason = "timeout"
            case 603: reason = "declined"
            default: reason = "rejected_\(code)"
            }
            failCall(c, reason: reason)
        default:
            break
        }
    }

    // MARK: - 呼出

    func startCall(target: String) throws -> NsbcCall {
        guard registrationState == .registered else {
            throw NsbcError(.notRegistered, "尚未注册到 NSBC，无法呼叫")
        }
        guard ctx?.call.isActive != true else {
            throw NsbcError(.callFailed, "已有进行中的呼叫")
        }

        let remoteUri = normalizeTarget(target)
        let callId = SipUtils.newCallId(host: config.server)
        let call = NsbcCall(
            isIncoming: false,
            remoteUri: remoteUri,
            callId: callId,
            actions: self
        )
        let c = CallContext(
            call: call,
            callId: callId,
            localTag: SipUtils.newTag(),
            isIncoming: false,
            remoteUri: remoteUri
        )
        ctx = c
        updateCall(c, state: .calling)

        // 先创建媒体 offer，再发 INVITE（含完整 SDP）
        mediaEngine.createOffer { [weak self] result in
            guard let self = self else { return }
            // 等待 offer 期间可能已被取消，避免给已结束的呼叫发 INVITE
            guard self.ctx === c else { return }
            switch result {
            case .success(let sdp):
                c.localSdp = sdp
                self.sendInvite(c)
            case .failure:
                self.failCall(c, reason: "媒体初始化失败")
            }
        }
        return call
    }

    private func sendInvite(_ c: CallContext) {
        c.branch = SipUtils.newBranch()
        c.cseq += 1

        let req = SipMessage()
        req.method = "INVITE"
        req.requestUri = c.remoteUri
        req.addHeader("Via", "SIP/2.0/\(viaProtocol) \(localHost);branch=\(c.branch);rport")
        req.addHeader("Max-Forwards", "70")
        req.addHeader("From", "\"\(displayName)\" <\(config.sipUri)>;tag=\(c.localTag)")
        req.addHeader("To", "<\(c.remoteUri)>")
        req.addHeader("Call-ID", c.callId)
        req.addHeader("CSeq", "\(c.cseq) INVITE")
        req.addHeader("Contact", "<\(config.sipUri);transport=\(config.contactTransport)>")
        req.addHeader("Allow", "INVITE, ACK, CANCEL, BYE, OPTIONS, MESSAGE")
        req.addHeader("Supported", "replaces, outbound")
        req.addHeader("Session-ID", sessionId)
        req.addHeader("User-Agent", SipEngine.userAgent)

        if let sdp = c.localSdp, !sdp.isEmpty {
            req.addHeader("Content-Type", "application/sdp")
            req.body = sdp
        }
        if let a = c.auth {
            req.addHeader(a.proxy ? "Proxy-Authorization" : "Authorization",
                          buildAuthValue(a, method: "INVITE", uri: c.remoteUri))
        }
        socket.send(req.toWire())
    }

    private func sendAck(_ c: CallContext) {
        guard !c.acked else { return }
        c.acked = true

        let req = SipMessage()
        req.method = "ACK"
        req.requestUri = c.remoteTarget ?? c.remoteUri
        req.addHeader("Via", "SIP/2.0/\(viaProtocol) \(localHost);branch=\(SipUtils.newBranch());rport")
        req.addHeader("Max-Forwards", "70")
        req.addHeader("From", "\"\(displayName)\" <\(config.sipUri)>;tag=\(c.localTag)")
        var to = "<\(c.remoteUri)>"
        if let tag = c.remoteTag { to += ";tag=\(tag)" }
        req.addHeader("To", to)
        req.addHeader("Call-ID", c.callId)
        req.addHeader("CSeq", "\(c.cseq) ACK")
        req.addHeader("Supported", "replaces, outbound")
        req.addHeader("Session-ID", sessionId)
        req.addHeader("User-Agent", SipEngine.userAgent)
        socket.send(req.toWire())
    }

    private func sendBye(_ c: CallContext) {
        c.cseq += 1

        let req = SipMessage()
        req.method = "BYE"
        req.requestUri = c.remoteTarget ?? c.remoteUri
        req.addHeader("Via", "SIP/2.0/\(viaProtocol) \(localHost);branch=\(SipUtils.newBranch());rport")
        req.addHeader("Max-Forwards", "70")
        req.addHeader("From", "\"\(displayName)\" <\(config.sipUri)>;tag=\(c.localTag)")
        var to = "<\(c.remoteUri)>"
        if let tag = c.remoteTag { to += ";tag=\(tag)" }
        req.addHeader("To", to)
        req.addHeader("Call-ID", c.callId)
        req.addHeader("CSeq", "\(c.cseq) BYE")
        req.addHeader("Supported", "replaces, outbound")
        req.addHeader("Session-ID", sessionId)
        req.addHeader("User-Agent", SipEngine.userAgent)
        socket.send(req.toWire())

        terminate(c, reason: "local_hangup")
    }

    private func sendCancel(_ c: CallContext) {
        let req = SipMessage()
        req.method = "CANCEL"
        req.requestUri = c.remoteUri
        // CANCEL 使用与被取消 INVITE 相同的 branch
        req.addHeader("Via", "SIP/2.0/\(viaProtocol) \(localHost);branch=\(c.branch);rport")
        req.addHeader("Max-Forwards", "70")
        req.addHeader("From", "\"\(displayName)\" <\(config.sipUri)>;tag=\(c.localTag)")
        req.addHeader("To", "<\(c.remoteUri)>")
        req.addHeader("Call-ID", c.callId)
        req.addHeader("CSeq", "\(c.cseq) CANCEL")
        req.addHeader("Supported", "replaces, outbound")
        req.addHeader("Session-ID", sessionId)
        req.addHeader("User-Agent", SipEngine.userAgent)
        socket.send(req.toWire())

        terminate(c, reason: "local_cancel")
    }

    // MARK: - 呼入

    private func handleIncomingInvite(_ msg: SipMessage) {
        if ctx != nil {
            reply(msg, code: 486, reason: "Busy Here")
            return
        }
        guard let callId = msg.callId() else {
            reply(msg, code: 400, reason: "Bad Request")
            return
        }
        let fromUri = msg.fromUri() ?? "sip:unknown"
        let call = NsbcCall(
            isIncoming: true,
            remoteUri: fromUri,
            callId: callId,
            actions: self
        )
        let c = CallContext(
            call: call,
            callId: callId,
            localTag: SipUtils.newTag(),
            isIncoming: true,
            remoteUri: fromUri
        )
        c.remoteTag = msg.fromTag()
        c.remoteSdp = msg.body
        c.inviteRequest = msg
        ctx = c

        reply(msg, code: 100, reason: "Trying", toTag: c.localTag)
        reply(msg, code: 180, reason: "Ringing", toTag: c.localTag)
        updateCall(c, state: .ringing)

        events?.sipEngineDidReceiveIncomingCall(call)
    }

    private func answerIncoming(_ c: CallContext) {
        let offer = c.remoteSdp
        if offer == nil || offer!.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            sendAnswer(c, sdp: "")
            return
        }
        mediaEngine.applyRemoteSdp(offer!, isOffer: true) { [weak self] result in
            guard let self = self else { return }
            switch result {
            case .failure:
                self.rejectIncoming(c, code: 488)
            case .success:
                self.mediaEngine.createAnswer { [weak self] answer in
                    guard let self = self else { return }
                    switch answer {
                    case .success(let sdp):
                        self.sendAnswer(c, sdp: sdp)
                    case .failure:
                        self.rejectIncoming(c, code: 488)
                    }
                }
            }
        }
    }

    private func sendAnswer(_ c: CallContext, sdp: String) {
        c.localSdp = sdp
        let invite = c.inviteRequest ?? lastRequest
        let resp = SipMessage.buildResponse(request: invite, code: 200, reason: "OK", toTag: c.localTag)
        resp.addHeader("Contact", "<\(config.sipUri);transport=\(config.contactTransport)>")
        resp.addHeader("Allow", "INVITE, ACK, CANCEL, BYE, OPTIONS, MESSAGE")
        resp.addHeader("User-Agent", SipEngine.userAgent)
        if !sdp.isEmpty {
            resp.addHeader("Content-Type", "application/sdp")
            resp.body = sdp
        }
        socket.send(resp.toWire())
        establish(c)
    }

    private func rejectIncoming(_ c: CallContext, code: Int) {
        let reason: String
        switch code {
        case 486: reason = "Busy Here"
        case 603: reason = "Decline"
        case 488: reason = "Not Acceptable Here"
        default: reason = "Rejected"
        }
        let invite = c.inviteRequest ?? lastRequest
        let resp = SipMessage.buildResponse(request: invite, code: code, reason: reason, toTag: c.localTag)
        socket.send(resp.toWire())
        terminate(c, reason: "local_reject")
    }

    /// 缓存最近的请求，用于构造 200/488 等响应。
    private var lastRequest = SipMessage()

    private func handleIncomingAck(_ msg: SipMessage) {
        guard let c = ctx else { return }
        if c.isIncoming && !c.acked {
            c.acked = true
            if c.call.state != .established {
                establish(c)
            }
        }
    }

    private func handleIncomingBye(_ msg: SipMessage) {
        reply(msg, code: 200, reason: "OK")
        if let c = ctx {
            terminate(c, reason: "remote_hangup")
        }
    }

    private func handleIncomingCancel(_ msg: SipMessage) {
        reply(msg, code: 200, reason: "OK")
        // 同时给被取消的 INVITE 回 487（必须用 INVITE 原始请求，不能用 CANCEL）
        guard let c = ctx else { return }
        let invite = c.inviteRequest ?? lastRequest
        let resp = SipMessage.buildResponse(request: invite, code: 487, reason: "Request Terminated", toTag: c.localTag)
        socket.send(resp.toWire())
        terminate(c, reason: "remote_cancel")
    }

    private func handleIncomingMessage(_ msg: SipMessage) {
        reply(msg, code: 200, reason: "OK")
    }

    // MARK: - 工具

    private func reply(
        _ request: SipMessage,
        code: Int,
        reason: String,
        toTag: String? = nil,
        extra: ((SipMessage) -> Void)? = nil
    ) {
        lastRequest = request
        let resp = SipMessage.buildResponse(request: request, code: code, reason: reason, toTag: toTag)
        extra?(resp)
        socket.send(resp.toWire())
    }

    private func normalizeTarget(_ target: String) -> String {
        let trimmed = target.trimmingCharacters(in: .whitespaces)
        if trimmed.hasPrefix("sip:") || trimmed.hasPrefix("sips:") {
            return trimmed
        }
        if trimmed.contains("@") {
            return "sip:\(trimmed)"
        }
        return "sip:\(trimmed)@\(config.realm)"
    }

    private func buildAuthValue(_ a: AuthChallenge, method: String, uri: String) -> String {
        a.nc += 1
        let ncHex = String(format: "%08x", UInt32(a.nc))
        let cnonce = SipUtils.newTag()
        return DigestAuth.buildAuthorization(
            username: config.username,
            password: config.password,
            realm: a.realm,
            nonce: a.nonce,
            uri: uri,
            method: method,
            qop: a.qop,
            nc: ncHex,
            cnonce: cnonce,
            algorithm: a.algorithm,
            opaque: a.opaque
        )
    }

    private func contactUri(_ header: String?) -> String? {
        guard let header = header else { return nil }
        if let lt = header.firstIndex(of: "<"),
           let gt = header.firstIndex(of: ">"),
           lt < gt {
            return String(header[header.index(after: lt)..<gt])
        }
        return header.trimmingCharacters(in: .whitespaces)
    }

    private func notifyRegistration(_ state: RegistrationState, detail: String?) {
        registrationState = state
        events?.sipEngineDidChangeRegistration(state: state, detail: detail)
    }

    private func updateCall(_ c: CallContext, state: CallState) {
        if c.call.state == state { return }
        c.call.state = state
        events?.sipEngineDidChangeCall(c.call)
    }

    private func establish(_ c: CallContext) {
        c.call.establishedAtMs = Date().timeIntervalSince1970
        updateCall(c, state: .established)
    }

    private func failCall(_ c: CallContext, reason: String) {
        c.call.endReason = reason
        updateCall(c, state: .failed)
        terminate(c, reason: reason, alreadyFailed: true)
    }

    private func terminate(_ c: CallContext, reason: String, alreadyFailed: Bool = false) {
        c.call.endReason = reason
        c.call.lastEndedAtMs = Date().timeIntervalSince1970
        if !alreadyFailed && c.call.state != .failed {
            updateCall(c, state: .terminated)
        }
        if ctx === c {
            ctx = nil
        }
        // 通话结束：停采集省电（下一次呼叫自动恢复）
        mediaEngine.onCallStopped()
    }
}

// MARK: - Call 动作

extension SipEngine: NsbcCallActions {

    func answer(_ call: NsbcCall) {
        guard let c = ctx, c.isIncoming, c.call === call else { return }
        answerIncoming(c)
    }

    func reject(_ call: NsbcCall, code: Int) {
        guard let c = ctx, c.isIncoming, c.call === call else { return }
        rejectIncoming(c, code: code)
    }

    func hangup(_ call: NsbcCall) {
        guard let c = ctx, c.call === call else { return }
        switch call.state {
        case .established:
            sendBye(c)
        case .calling, .ringing:
            if c.isIncoming {
                rejectIncoming(c, code: 486)
            } else {
                sendCancel(c)
            }
        default:
            break
        }
    }

    func setMuted(_ call: NsbcCall, muted: Bool) {
        mediaEngine.setMicrophoneEnabled(!muted)
    }

    func setVideoEnabled(_ call: NsbcCall, enabled: Bool) {
        mediaEngine.setCameraEnabled(enabled)
    }

    func switchCamera(_ call: NsbcCall) {
        mediaEngine.switchCamera()
    }
}
