import Foundation

/// 呼叫状态（对齐 SIP 语义，与 Android SDK 一致）。
public enum CallState: String, Codable {
    /// 空闲
    case idle = "IDLE"
    /// 正在拨出，已发 INVITE，等待响应
    case calling = "CALLING"
    /// 对端振铃（180/183）
    case ringing = "RINGING"
    /// 呼叫已接通
    case established = "ESTABLISHED"
    /// 已挂断
    case terminated = "TERMINATED"
    /// 呼叫失败
    case failed = "FAILED"
}

/// 注册状态。
public enum RegistrationState: String, Codable {
    case unregistered = "UNREGISTERED"
    case registering = "REGISTERING"
    case registered = "REGISTERED"
    case failed = "FAILED"
}

/// SDK 错误分类。
public enum NsbcErrorCode: String, Codable {
    case configInvalid = "CONFIG_INVALID"
    case networkUnreachable = "NETWORK_UNREACHABLE"
    case tlsHandshakeFailed = "TLS_HANDSHAKE_FAILED"
    case wsClosed = "WS_CLOSED"
    case authFailed = "AUTH_FAILED"
    case registerTimeout = "REGISTER_TIMEOUT"
    case registerRejected = "REGISTER_REJECTED"
    case callFailed = "CALL_FAILED"
    case callRejected = "CALL_REJECTED"
    case mediaFailed = "MEDIA_FAILED"
    case notRegistered = "NOT_REGISTERED"
    case `internal` = "INTERNAL"
}

/// SDK 统一错误类型。
public struct NsbcError: Error, LocalizedError {
    public let code: NsbcErrorCode
    public let message: String

    public var errorDescription: String? { message }

    public init(_ code: NsbcErrorCode, _ message: String) {
        self.code = code
        self.message = message
    }
}
