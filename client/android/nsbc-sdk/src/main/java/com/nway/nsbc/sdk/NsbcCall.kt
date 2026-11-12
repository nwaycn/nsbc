package com.nway.nsbc.sdk

/**
 * 一通呼叫的句柄。
 *
 * 生命周期由 SDK 内部管理；业务层只读 [state]、调用 [answer] / [hangup] 等动作。
 * 所有回调都会在 SDK 内部调度线程触发，UI 更新请自行切主线程。
 */
class NsbcCall internal constructor(
    /** 是否呼入 */
    val isIncoming: Boolean,
    /** 对端 SIP URI，例如 `sip:1001@nsbc.local` */
    val remoteUri: String,
    /** 内部唯一 ID（等于 SIP Call-ID） */
    internal val id: String,
    private val actions: Actions
) {

    internal interface Actions {
        fun answer(call: NsbcCall)
        fun reject(call: NsbcCall, code: Int)
        fun hangup(call: NsbcCall)
        fun setMuted(call: NsbcCall, muted: Boolean)
        fun setVideoEnabled(call: NsbcCall, enabled: Boolean)
        fun switchCamera(call: NsbcCall)
    }

    /** 呼叫状态，由 SDK 更新 */
    @Volatile
    var state: CallState = CallState.IDLE
        internal set

    /** 对端显示名（若 SIP 头带 display name） */
    @Volatile
    var remoteDisplayName: String? = null
        internal set

    /** 接通时间戳（毫秒），未接通为 null */
    @Volatile
    var establishedAtMs: Long? = null
        internal set

    /** 结束原因（如 `normal` / `busy` / `canceled` / `timeout`） */
    @Volatile
    var endReason: String? = null
        internal set

    /** 通话时长（秒），未接通返回 0 */
    val durationSeconds: Long
        get() {
            val start = establishedAtMs ?: return 0
            val end = if (state == CallState.ESTABLISHED) System.currentTimeMillis()
            else lastEndedAtMs
            return ((end - start) / 1000).coerceAtLeast(0)
        }

    @Volatile
    internal var lastEndedAtMs: Long = 0

    val isActive: Boolean
        get() = state == CallState.CALLING || state == CallState.RINGING ||
                state == CallState.ESTABLISHED

    /** 接听（仅呼入有效）。 */
    fun answer() = actions.answer(this)

    /** 拒接（仅呼入有效），默认 486 Busy Here。 */
    fun reject(code: Int = 486) = actions.reject(this, code)

    /** 挂断 / 取消。 */
    fun hangup() = actions.hangup(this)

    /** 麦克风静音。 */
    fun setMuted(muted: Boolean) = actions.setMuted(this, muted)

    /** 开关摄像头。 */
    fun setVideoEnabled(enabled: Boolean) = actions.setVideoEnabled(this, enabled)

    /** 前后摄像头切换。 */
    fun switchCamera() = actions.switchCamera(this)

    override fun toString(): String = "NsbcCall($remoteUri, $state)"
}
