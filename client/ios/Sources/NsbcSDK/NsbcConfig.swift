import Foundation

/// 日志级别。
public enum LogLevel: String, Codable, CaseIterable, Hashable {
    case none = "NONE"
    case error = "ERROR"
    case warn = "WARN"
    case info = "INFO"
    case debug = "DEBUG"
    case verbose = "VERBOSE"
}

/// ICE 传输策略。
public enum IceTransportPolicy: String, Codable, CaseIterable, Hashable {
    /// 允许直连 + 中继（默认）
    case all = "ALL"
    /// 只走 TURN 中继（公网严格 NAT 场景）
    case relay = "RELAY"
}

/// NSBC SDK 的全部可配置项，字段与 Android 版 `NsbcConfig` 完全对齐。
public struct NsbcConfig: Codable {

    public var server: String = ""
    public var wssPort: Int = 5062
    public var wsPath: String = "/ws"
    public var useTls: Bool = true
    public var realm: String = ""
    public var username: String = ""
    public var password: String = ""
    public var displayName: String = ""
    public var stunServer: String?
    public var turnServer: String?
    public var turnUsername: String?
    public var turnCredential: String?
    public var videoEnabled: Bool = true
    public var videoWidth: Int = 1280
    public var videoHeight: Int = 720
    public var videoFps: Int = 30
    public var mirrorLocalPreview: Bool = true
    public var registerExpiresSec: Int = 300
    public var autoReconnect: Bool = true
    public var reconnectDelayMs: Int = 2000
    public var icePolicy: IceTransportPolicy = .all
    public var allowSelfSignedCert: Bool = true
    public var logLevel: LogLevel = .info

    public init() {}

    /// 与 Android `assets/nsbc.json` 等价的示例默认配置。
    public static var sample: NsbcConfig {
        var c = NsbcConfig()
        c.server = "10.0.0.80"
        c.wssPort = 5062
        c.wsPath = "/ws"
        c.useTls = true
        c.realm = "nsbc.local"
        c.username = "1000"
        c.password = "secret123"
        c.stunServer = "stun:10.0.0.80:3478"
        c.videoEnabled = true
        c.videoWidth = 1280
        c.videoHeight = 720
        c.videoFps = 30
        c.mirrorLocalPreview = true
        c.autoReconnect = true
        c.allowSelfSignedCert = true
        c.logLevel = .info
        return c.normalized()
    }

    // MARK: - 计算属性

    /// 实际使用的 WebSocket URL，例如 `wss://10.0.0.80:5062/ws`
    public var webSocketUrl: String {
        let scheme = useTls ? "wss" : "ws"
        let path = wsPath.hasPrefix("/") ? wsPath : "/" + wsPath
        return "\(scheme)://\(server):\(wssPort)\(path)"
    }

    /// 本机 SIP AoR，例如 `sip:1000@nsbc.local`
    public var sipUri: String {
        return "sip:\(username)@\(realm)"
    }

    /// Contact 的传输参数（RFC 7118 规定 WS 连接必须声明 transport=ws）
    public var contactTransport: String { "ws" }

    /// 规范化：显示名缺省为账号，路径补前导斜杠。
    public func normalized() -> NsbcConfig {
        var c = self
        if c.displayName.trimmingCharacters(in: .whitespaces).isEmpty {
            c.displayName = c.username
        }
        if !c.wsPath.hasPrefix("/") {
            c.wsPath = "/" + c.wsPath
        }
        return c
    }

    /// 校验配置，返回问题列表。空列表表示可用。
    public func validate() -> [String] {
        var problems: [String] = []
        if server.trimmingCharacters(in: .whitespaces).isEmpty { problems.append("server 不能为空") }
        if username.trimmingCharacters(in: .whitespaces).isEmpty { problems.append("username 不能为空") }
        if realm.trimmingCharacters(in: .whitespaces).isEmpty { problems.append("realm 不能为空") }
        if password.isEmpty { problems.append("password 不能为空") }
        if !(1...65535).contains(wssPort) { problems.append("wssPort 超出范围: \(wssPort)") }
        if !(30...3600).contains(registerExpiresSec) {
            problems.append("registerExpiresSec 建议在 30..3600")
        }
        if videoEnabled {
            if !(160...1920).contains(videoWidth) { problems.append("videoWidth 超出范围: \(videoWidth)") }
            if !(120...1080).contains(videoHeight) { problems.append("videoHeight 超出范围: \(videoHeight)") }
            if !(1...60).contains(videoFps) { problems.append("videoFps 超出范围: \(videoFps)") }
        }
        if turnServer != nil && turnUsername == nil {
            problems.append("配置了 turnServer 但缺少 turnUsername")
        }
        if icePolicy == .relay && turnServer == nil {
            problems.append("icePolicy=RELAY 时必须配置 turnServer")
        }
        return problems
    }

    public var isValid: Bool { validate().isEmpty }

    // MARK: - Codable

    private enum CodingKeys: String, CodingKey {
        case server, wssPort, wsPath, useTls, realm, username, password, displayName
        case stunServer, turnServer, turnUsername, turnCredential
        case videoEnabled, videoWidth, videoHeight, videoFps, mirrorLocalPreview
        case registerExpiresSec, autoReconnect, reconnectDelayMs, icePolicy
        case allowSelfSignedCert, logLevel
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = NsbcConfig()
        server = try c.decodeIfPresent(String.self, forKey: .server) ?? d.server
        wssPort = try c.decodeIfPresent(Int.self, forKey: .wssPort) ?? d.wssPort
        wsPath = try c.decodeIfPresent(String.self, forKey: .wsPath) ?? d.wsPath
        useTls = try c.decodeIfPresent(Bool.self, forKey: .useTls) ?? d.useTls
        realm = try c.decodeIfPresent(String.self, forKey: .realm) ?? d.realm
        username = try c.decodeIfPresent(String.self, forKey: .username) ?? d.username
        password = try c.decodeIfPresent(String.self, forKey: .password) ?? d.password
        displayName = try c.decodeIfPresent(String.self, forKey: .displayName) ?? d.displayName

        let stun = try c.decodeIfPresent(String.self, forKey: .stunServer)
        stunServer = (stun?.isEmpty == true) ? nil : stun
        let turn = try c.decodeIfPresent(String.self, forKey: .turnServer)
        turnServer = (turn?.isEmpty == true) ? nil : turn
        let turnUser = try c.decodeIfPresent(String.self, forKey: .turnUsername)
        turnUsername = (turnUser?.isEmpty == true) ? nil : turnUser
        let turnCred = try c.decodeIfPresent(String.self, forKey: .turnCredential)
        turnCredential = (turnCred?.isEmpty == true) ? nil : turnCred

        videoEnabled = try c.decodeIfPresent(Bool.self, forKey: .videoEnabled) ?? d.videoEnabled
        videoWidth = try c.decodeIfPresent(Int.self, forKey: .videoWidth) ?? d.videoWidth
        videoHeight = try c.decodeIfPresent(Int.self, forKey: .videoHeight) ?? d.videoHeight
        videoFps = try c.decodeIfPresent(Int.self, forKey: .videoFps) ?? d.videoFps
        mirrorLocalPreview = try c.decodeIfPresent(Bool.self, forKey: .mirrorLocalPreview) ?? d.mirrorLocalPreview
        registerExpiresSec = try c.decodeIfPresent(Int.self, forKey: .registerExpiresSec) ?? d.registerExpiresSec
        autoReconnect = try c.decodeIfPresent(Bool.self, forKey: .autoReconnect) ?? d.autoReconnect
        reconnectDelayMs = try c.decodeIfPresent(Int.self, forKey: .reconnectDelayMs) ?? d.reconnectDelayMs

        let iceText = try c.decodeIfPresent(String.self, forKey: .icePolicy) ?? "ALL"
        icePolicy = IceTransportPolicy(rawValue: iceText.uppercased()) ?? .all
        allowSelfSignedCert = try c.decodeIfPresent(Bool.self, forKey: .allowSelfSignedCert) ?? d.allowSelfSignedCert

        let levelText = try c.decodeIfPresent(String.self, forKey: .logLevel) ?? "INFO"
        logLevel = LogLevel(rawValue: levelText.uppercased()) ?? .info
    }
}
