import Foundation

/// SipSocket 的事件回调，全部由 SipEngine 实现。
protocol SipSocketListener: AnyObject {
    func onSocketOpen()
    func onSocketMessage(_ raw: String)
    func onSocketClosed(code: Int, reason: String)
    func onSocketFailure(_ error: NsbcError)
}

/// SIP over WebSocket 传输层（RFC 7118）。
///
/// 只负责「建链 / 收发文本帧 / 断链通知」，不做任何 SIP 语义处理。
/// 使用 `URLSessionWebSocketTask`，子协议固定为 `sip`。
final class SipSocket: NSObject {

    weak var listener: SipSocketListener?

    private let config: NsbcConfig
    private var session: URLSession?
    private var task: URLSessionWebSocketTask?
    private var pingTimer: Timer?
    /// 主动断开时置位，避免把关闭回调当失败上报
    private var closing = false

    private(set) var isOpen = false

    init(config: NsbcConfig) {
        self.config = config
        super.init()
    }

    // MARK: - 建链 / 收发

    func connect() {
        guard task == nil else { return }
        guard let url = URL(string: config.webSocketUrl) else {
            listener?.onSocketFailure(NsbcError(.configInvalid, "WebSocket URL 非法: \(config.webSocketUrl)"))
            return
        }

        closing = false
        var request = URLRequest(url: url)
        request.setValue("sip", forHTTPHeaderField: "Sec-WebSocket-Protocol")
        request.timeoutInterval = 15

        let configuration = URLSessionConfiguration.default
        configuration.waitsForConnectivity = false

        let session = URLSession(configuration: configuration, delegate: self, delegateQueue: nil)
        self.session = session

        let task = session.webSocketTask(with: request)
        self.task = task
        task.resume()
        receive()
    }

    /// 发送 SIP 消息（TEXT 帧）。返回是否成功入队。
    @discardableResult
    func send(_ raw: String) -> Bool {
        guard let task = task, isOpen else { return false }
        task.send(.string(raw)) { _ in }
        return true
    }

    func disconnect(code: URLSessionWebSocketTask.CloseCode = .normalClosure, reason: String = "bye") {
        closing = true
        isOpen = false
        stopPing()
        // 与 Android 一致：先让已入队的注销/挂断帧写出去，再关闭连接
        let oldTask = task
        let oldSession = session
        task = nil
        session = nil
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(150)) {
            oldTask?.cancel(with: code, reason: Data(reason.utf8))
            oldSession?.invalidateAndCancel()
        }
    }

    // MARK: - 内部

    private func receive() {
        guard let task = task, !closing else { return }
        task.receive { [weak self] result in
            guard let self = self else { return }
            switch result {
            case .success(let message):
                switch message {
                case .string(let text):
                    self.listener?.onSocketMessage(text)
                case .data(let data):
                    if let text = String(data: data, encoding: .utf8) {
                        self.listener?.onSocketMessage(text)
                    }
                @unknown default:
                    break
                }
                self.receive()
            case .failure(let error):
                self.reportFailure(error)
            }
        }
    }

    private func reportFailure(_ error: Error) {
        guard !closing else { return }
        let wasOpen = isOpen
        isOpen = false
        stopPing()
        task = nil
        session?.invalidateAndCancel()
        session = nil
        listener?.onSocketFailure(
            NsbcError(wasOpen ? .wsClosed : .networkUnreachable,
                      "WebSocket 失败: \(error.localizedDescription)")
        )
    }

    private func startPing() {
        // Timer 必须在其 RunLoop 线程上创建，delegate 回调线程没有 RunLoop
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.stopPing()
            self.pingTimer = Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
                self?.task?.sendPing { _ in }
            }
        }
    }

    private func stopPing() {
        if Thread.isMainThread {
            pingTimer?.invalidate()
            pingTimer = nil
        } else {
            DispatchQueue.main.async { [weak self] in
                self?.pingTimer?.invalidate()
                self?.pingTimer = nil
            }
        }
    }
}

// MARK: - URLSessionDelegate

extension SipSocket: URLSessionWebSocketDelegate {

    /// 自签证书接受：仅当 allowSelfSignedCert 打开时信任 server trust。
    func urlSession(
        _ session: URLSession,
        didReceive challenge: URLAuthenticationChallenge,
        completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void
    ) {
        if config.allowSelfSignedCert,
           challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
           let trust = challenge.protectionSpace.serverTrust {
            completionHandler(.useCredential, URLCredential(trust: trust))
            return
        }
        completionHandler(.performDefaultHandling, nil)
    }

    func urlSession(
        _ session: URLSession,
        webSocketTask: URLSessionWebSocketTask,
        didOpenWithProtocol protocol: String?
    ) {
        isOpen = true
        startPing()
        listener?.onSocketOpen()
    }

    func urlSession(
        _ session: URLSession,
        webSocketTask: URLSessionWebSocketTask,
        didCloseWith closeCode: URLSessionWebSocketTask.CloseCode,
        reason: Data?
    ) {
        isOpen = false
        stopPing()
        // 释放底层资源，允许后续 connect() 重新建链
        task = nil
        session?.invalidateAndCancel()
        session = nil
        guard !closing else { return }
        let reasonText = reason.flatMap { String(data: $0, encoding: .utf8) } ?? ""
        listener?.onSocketClosed(code: closeCode.rawValue, reason: reasonText)
    }
}
