package com.nway.nsbc.sdk.media

import com.nway.nsbc.sdk.NsbcConfig
import com.nway.nsbc.sdk.NsbcError
import org.webrtc.SurfaceViewRenderer

/**
 * 媒体引擎抽象。
 *
 * SDK 默认使用 [WebRtcMediaEngine]（DTLS-SRTP + ICE，兼容 NSBC WebRTC 路径）。
 * 业务方也可以替换为自有实现（例如纯音频 RTP 引擎），只需实现本接口并在
 * `NsbcClient.Builder.mediaEngine(...)` 注入。
 */
interface MediaEngine {

    interface Listener {
        /** ICE 连接状态变化：`new` / `checking` / `connected` / `completed` / `failed` / `disconnected` */
        fun onIceStateChanged(state: String)
        fun onError(error: NsbcError)
    }

    /** 初始化引擎。必须在主线程调用。 */
    fun init(config: NsbcConfig, listener: Listener)

    /**
     * 创建本地 SDP offer（含已收集的 ICE candidate）。呼出场景使用。
     * 回调在任意线程，UI 操作需自行切主线程。
     */
    fun createOffer(onReady: (Result<String>) -> Unit)

    /**
     * 创建本地 SDP answer（含已收集的 ICE candidate）。呼入场景使用。
     * 调用前必须先 [applyRemoteSdp] 应用对端 offer。
     */
    fun createAnswer(onReady: (Result<String>) -> Unit)

    /**
     * 应用远端 SDP。
     *
     * @param isOffer true 表示这是对端 offer（呼入），false 表示 answer（呼出）
     */
    fun applyRemoteSdp(sdp: String, isOffer: Boolean, onReady: (Result<Unit>) -> Unit)

    /** 初始化渲染器（绑定 EGL 上下文）。必须在 attach 前调用一次。 */
    fun prepareRenderer(view: SurfaceViewRenderer)

    /** 绑定本地预览渲染器。 */
    fun attachLocalRenderer(view: SurfaceViewRenderer)

    /** 绑定远端渲染器。 */
    fun attachRemoteRenderer(view: SurfaceViewRenderer)

    /** 麦克风开关。 */
    fun setMicrophoneEnabled(enabled: Boolean)

    /** 摄像头开关。 */
    fun setCameraEnabled(enabled: Boolean)

    /** 前后摄像头切换。 */
    fun switchCamera()

    /**
     * 通话结束时回调：默认停掉摄像头采集（省电 + 隐私）。
     * 自定义实现可忽略；下一次呼叫会自动重新开始采集。
     */
    fun onCallStopped() {}

    /** 释放全部资源。 */
    fun close()
}
