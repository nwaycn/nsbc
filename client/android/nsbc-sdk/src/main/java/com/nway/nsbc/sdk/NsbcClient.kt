package com.nway.nsbc.sdk

import android.content.Context
import android.os.Handler
import android.os.Looper
import com.nway.nsbc.sdk.media.MediaEngine
import com.nway.nsbc.sdk.media.WebRtcMediaEngine
import com.nway.nsbc.sdk.sip.SipEngine
import org.webrtc.SurfaceViewRenderer

/**
 * NSBC Android SDK 入口。
 *
 * 典型用法（3 步）：
 * ```kotlin
 * // 1. 配置 + 创建
 * val client = NsbcClient.Builder(context)
 *     .configJson(assets.open("nsbc.json").bufferedReader().readText())
 *     .listener(myListener)
 *     .build()
 *
 * // 2. 连接 + 注册
 * client.connect()
 *
 * // 3. 呼叫
 * client.call("1001")
 * ```
 *
 * 线程约定：所有公开方法建议在主线程调用；回调默认在主线程触发。
 */
class NsbcClient private constructor(
    private val appContext: Context,
    val config: NsbcConfig,
    private val mediaEngine: MediaEngine,
    private val listener: Listener?
) {

    /** 事件回调。所有方法在主线程触发。 */
    interface Listener {
        /** 注册状态变化 */
        fun onRegistrationChanged(state: RegistrationState, detail: String?) {}
        /** 收到呼入 */
        fun onIncomingCall(call: NsbcCall) {}
        /** 呼叫状态变化 */
        fun onCallStateChanged(call: NsbcCall) {}
        /** ICE / 媒体状态变化 */
        fun onMediaStateChanged(state: String) {}
        /** 错误 */
        fun onError(error: NsbcError) {}
    }

    private val mainHandler = Handler(Looper.getMainLooper())

    // 桥接 SipEngine 事件（SipEngine 为 internal，故用匿名对象避免 public 暴露）
    private val engineEvents = object : SipEngine.Events {
        override fun onTransportOpen() {
            NsbcLogger.i("NsbcClient", "传输已连接")
        }

        override fun onTransportClosed(reason: String) {
            NsbcLogger.w("NsbcClient", "传输关闭: $reason")
            if (config.autoReconnect) scheduleReconnect()
        }

        override fun onTransportFailure(error: NsbcError) {
            listener?.onError(error)
            if (config.autoReconnect) scheduleReconnect()
        }

        override fun onRegistrationChanged(state: RegistrationState, detail: String?) {
            NsbcLogger.i("NsbcClient", "注册状态: $state ${detail ?: ""}")
            listener?.onRegistrationChanged(state, detail)
        }

        override fun onIncomingCall(call: NsbcCall) {
            listener?.onIncomingCall(call)
        }

        override fun onCallStateChanged(call: NsbcCall) {
            listener?.onCallStateChanged(call)
        }
    }

    private val engine = SipEngine(config, mediaEngine, engineEvents)
    private var initialized = false
    private var reconnectAttempts = 0
    private var manuallyDisconnected = false

    /** 渲染器引用保留，便于在 connect() 初始化 EGL 后补做绑定 */
    private var localRenderer: SurfaceViewRenderer? = null
    private var remoteRenderer: SurfaceViewRenderer? = null

    // ============================================================== 生命周期

    /**
     * 建立 WSS 连接并自动注册。
     */
    fun connect() {
        if (!initialized) {
            mediaEngine.init(config, object : MediaEngine.Listener {
                override fun onIceStateChanged(state: String) {
                    listener?.onMediaStateChanged(state)
                }

                override fun onError(error: NsbcError) {
                    listener?.onError(error)
                }
            })
            initialized = true
        }
        NsbcLogger.level = config.logLevel
        reconnectAttempts = 0
        manuallyDisconnected = false
        // EGL 已就绪：补做渲染器初始化（支持先 bindRenderers 再 connect）
        bindRenderers(localRenderer, remoteRenderer)
        engine.connect()
    }

    /** 断开连接（会自动注销并挂断当前呼叫）。 */
    fun disconnect() {
        manuallyDisconnected = true
        engine.disconnect()
    }

    /** 完全释放资源，之后不可复用。 */
    fun close() {
        manuallyDisconnected = true
        runCatching { engine.disconnect() }
        runCatching { mediaEngine.close() }
        initialized = false
    }

    // ============================================================== 呼叫

    /** 发起呼叫。target 可为分机号（`1001`）或完整 SIP URI。 */
    fun call(target: String): NsbcCall = engine.startCall(target)

    /** 当前活跃呼叫，无则返回 null。 */
    fun currentCall(): NsbcCall? = engine.currentCall

    /** 当前注册状态。 */
    val registrationState: RegistrationState
        get() = engine.registrationState

    // ============================================================== 媒体

    /**
     * 绑定本地/远端渲染器。两个都可为 null。
     * 可在 connect() 前后任意时机调用；connect() 时会自动补做 EGL 初始化。
     */
    fun bindRenderers(local: SurfaceViewRenderer?, remote: SurfaceViewRenderer?) {
        localRenderer = local
        remoteRenderer = remote
        local?.let {
            mediaEngine.prepareRenderer(it)
            mediaEngine.attachLocalRenderer(it)
        }
        remote?.let {
            mediaEngine.prepareRenderer(it)
            mediaEngine.attachRemoteRenderer(it)
        }
    }

    fun setMuted(muted: Boolean) {
        mediaEngine.setMicrophoneEnabled(!muted)
    }

    fun setVideoEnabled(enabled: Boolean) {
        mediaEngine.setCameraEnabled(enabled)
    }

    fun switchCamera() {
        mediaEngine.switchCamera()
    }

    // ============================================================== 重连

    private fun scheduleReconnect() {
        if (manuallyDisconnected) return
        if (reconnectAttempts > 5) return
        val delay = config.reconnectDelayMs * (1L shl reconnectAttempts.coerceAtMost(4))
        reconnectAttempts++
        NsbcLogger.i("NsbcClient", "将在 ${delay}ms 后第 $reconnectAttempts 次重连")
        mainHandler.postDelayed({
            if (engine.registrationState != RegistrationState.REGISTERED) {
                engine.connect()
            }
        }, delay)
    }

    // ============================================================== Builder

    /**
     * 客户端构造器。最小用法只需 [config]。
     */
    class Builder(private val context: Context) {

        private var config: NsbcConfig? = null
        private var listener: Listener? = null
        private var mediaEngine: MediaEngine? = null

        fun config(c: NsbcConfig) = apply { config = c }

        /** 从 JSON 字符串配置（可放在 `assets/nsbc.json`）。 */
        fun configJson(json: String) = apply { config = NsbcConfig.fromJson(json) }

        fun listener(l: Listener?) = apply { listener = l }

        /** 自定义媒体引擎；不传则使用 [WebRtcMediaEngine]。 */
        fun mediaEngine(e: MediaEngine?) = apply { mediaEngine = e }

        fun build(): NsbcClient {
            val cfg = config ?: throw IllegalStateException(
                "未配置：请调用 config(...) 或 configJson(...)"
            )
            val problems = cfg.validate()
            if (problems.isNotEmpty()) {
                throw IllegalArgumentException("配置不合法: ${problems.joinToString("; ")}")
            }
            val app = context.applicationContext
            return NsbcClient(
                appContext = app,
                config = cfg,
                mediaEngine = mediaEngine ?: WebRtcMediaEngine(app),
                listener = listener
            )
        }
    }

    companion object {
        fun builder(context: Context) = Builder(context)
    }
}
