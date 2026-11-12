package com.nway.nsbc.sdk

/** 呼叫状态（对齐 SIP 语义）。 */
enum class CallState {
    /** 空闲 */
    IDLE,
    /** 正在拨出，已发 INVITE，等待响应 */
    CALLING,
    /** 对端振铃（180/183） */
    RINGING,
    /** 呼叫已接通 */
    ESTABLISHED,
    /** 已挂断 */
    TERMINATED,
    /** 呼叫失败 */
    FAILED
}

/** 注册状态。 */
enum class RegistrationState {
    UNREGISTERED,
    REGISTERING,
    REGISTERED,
    FAILED
}

/** SDK 错误分类。 */
enum class NsbcErrorCode {
    CONFIG_INVALID,
    NETWORK_UNREACHABLE,
    TLS_HANDSHAKE_FAILED,
    WS_CLOSED,
    AUTH_FAILED,
    REGISTER_TIMEOUT,
    REGISTER_REJECTED,
    CALL_FAILED,
    CALL_REJECTED,
    MEDIA_FAILED,
    NOT_REGISTERED,
    INTERNAL
}

class NsbcError(
    val code: NsbcErrorCode,
    override val message: String,
    override val cause: Throwable? = null
) : Exception(message, cause) {
    override fun toString(): String = "NsbcError($code): $message"
}
