package com.nway.nsbc.sdk.media

import android.content.Context
import android.os.Handler
import android.os.Looper
import com.nway.nsbc.sdk.IceTransportPolicy
import com.nway.nsbc.sdk.NsbcConfig
import com.nway.nsbc.sdk.NsbcError
import com.nway.nsbc.sdk.NsbcErrorCode
import com.nway.nsbc.sdk.NsbcLogger
import org.webrtc.Camera2Enumerator
import org.webrtc.CameraVideoCapturer
import org.webrtc.DefaultVideoDecoderFactory
import org.webrtc.DefaultVideoEncoderFactory
import org.webrtc.EglBase
import org.webrtc.IceCandidate
import org.webrtc.IceServer
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RtpReceiver
import org.webrtc.RtpSender
import org.webrtc.RtpTransceiver
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.SurfaceTextureHelper
import org.webrtc.SurfaceViewRenderer
import org.webrtc.VideoCapturer
import org.webrtc.VideoSource
import org.webrtc.VideoTrack
import org.webrtc.AudioSource
import org.webrtc.AudioTrack

/**
 * 基于 Google WebRTC 的媒体引擎实现。
 *
 * - 音频：AEC / NS / AGC 由 WebRTC 内置处理
 * - 视频：Camera2 采集 + 硬件编解码
 * - 传输：ICE + DTLS-SRTP（与 NSBC 的 `media.force_srtp` 对齐）
 *
 * 资源生命周期严格与 [init] / [close] 配对；重复 init 会先释放旧资源。
 */
class WebRtcMediaEngine(private val appContext: Context) : MediaEngine {

    private val tag = "WebRtcMedia"
    private val mainHandler = Handler(Looper.getMainLooper())

    private var config: NsbcConfig? = null
    private var listener: MediaEngine.Listener? = null

    private var eglBase: EglBase? = null
    private var factory: PeerConnectionFactory? = null
    private var peerConnection: PeerConnection? = null

    private var videoCapturer: VideoCapturer? = null
    private var videoSource: VideoSource? = null
    private var videoTrack: VideoTrack? = null
    private var surfaceTextureHelper: SurfaceTextureHelper? = null
    private var audioSource: AudioSource? = null
    private var audioTrack: AudioTrack? = null

    private var localRenderer: SurfaceViewRenderer? = null
    private var remoteRenderer: SurfaceViewRenderer? = null

    /** 摄像头是否正在采集（startCapture / stopCapture 配对） */
    private var isCapturing = false

    /** 呼入 offer 是否包含 m=video —— 决定 answer 是否添加视频轨 */
    private var remoteOfferHasVideo = false

    @Volatile private var micEnabled = true
    @Volatile private var cameraEnabled = true

    // ---------------------------------------------------------------- init

    override fun init(config: NsbcConfig, listener: MediaEngine.Listener) {
        this.config = config
        this.listener = listener

        // 若已初始化过，先释放
        releaseInternal()

        PeerConnectionFactory.initialize(
            PeerConnectionFactory.InitializationOptions.builder(appContext)
                .setEnableInternalTracer(false)
                .createInitializationOptions()
        )

        val egl = EglBase.create()
        eglBase = egl
        factory = PeerConnectionFactory.builder()
            .setVideoEncoderFactory(DefaultVideoEncoderFactory(egl.eglBaseContext, true, true))
            .setVideoDecoderFactory(DefaultVideoDecoderFactory(egl.eglBaseContext))
            .createPeerConnectionFactory()

        NsbcLogger.i(tag, "WebRTC 引擎初始化完成")
    }

    // ---------------------------------------------------------------- offer / answer

    override fun createOffer(onReady: (Result<String>) -> Unit) {
        val f = factory
        val cfg = config
        if (f == null || cfg == null) {
            onReady(Result.failure(NsbcError(NsbcErrorCode.MEDIA_FAILED, "引擎未初始化")))
            return
        }
        try {
            // 每次呼出都用全新的 PeerConnection，避免上一通呼叫的 transceiver 残留
            resetPeerConnection()
            val pc = ensurePeerConnection()
            attachLocalTracks(pc, allowVideo = cfg.videoEnabled)

            // Unified Plan 下 MediaConstraints 的 OfferToReceiveAudio/Video 已废弃，
            // 改用显式 addTransceiver 保证方向可控；已在 attachLocalTracks 内完成。
            pc.createOffer(object : SdpObserver {
                override fun onCreateSuccess(desc: SessionDescription) {
                    val sdp = filterOfferSdp(desc.description)
                    pc.setLocalDescription(object : SdpObserver {
                        override fun onSetSuccess() {
                            // 等待 ICE 收集完成后再返回完整 SDP（最多 3 秒）
                            waitIceComplete(pc, 0, onReady)
                        }
                        override fun onSetFailure(err: String?) =
                            onReady(Result.failure(NsbcError(NsbcErrorCode.MEDIA_FAILED, "setLocalDescription 失败: $err")))
                        override fun onCreateSuccess(p0: SessionDescription?) {}
                        override fun onCreateFailure(p0: String?) {}
                    }, SessionDescription(desc.type, sdp))
                }

                override fun onCreateFailure(err: String?) =
                    onReady(Result.failure(NsbcError(NsbcErrorCode.MEDIA_FAILED, "createOffer 失败: $err")))

                override fun onSetSuccess() {}
                override fun onSetFailure(p0: String?) {}
            })
        } catch (t: Throwable) {
            onReady(Result.failure(NsbcError(NsbcErrorCode.MEDIA_FAILED, "创建 offer 异常: ${t.message}", t)))
        }
    }

    private fun waitIceComplete(
        pc: PeerConnection,
        elapsedMs: Int,
        onReady: (Result<String>) -> Unit
    ) {
        val complete = pc.iceGatheringState() == PeerConnection.IceGatheringState.COMPLETE
        if (complete || elapsedMs >= 3000) {
            val sdp = pc.localDescription()?.description
            if (sdp != null) onReady(Result.success(sdp))
            else onReady(Result.failure(NsbcError(NsbcErrorCode.MEDIA_FAILED, "本地 SDP 为空")))
            return
        }
        mainHandler.postDelayed({ waitIceComplete(pc, elapsedMs + 200, onReady) }, 200)
    }

    override fun createAnswer(onReady: (Result<String>) -> Unit) {
        val cfg = config
        if (cfg == null) {
            onReady(Result.failure(NsbcError(NsbcErrorCode.MEDIA_FAILED, "引擎未初始化")))
            return
        }
        try {
            val pc = ensurePeerConnection()
            // 对端 offer 没有 m=video 时不能凭空加视频轨，否则 answer 段数多于 offer
            attachLocalTracks(pc, allowVideo = cfg.videoEnabled && remoteOfferHasVideo)
            // Unified Plan 下 MediaConstraints 对 answer 无效，方向由远端 offer 决定
            pc.createAnswer(object : SdpObserver {
                override fun onCreateSuccess(desc: SessionDescription) {
                    val sdp = filterAnswerSdp(desc.description)
                    pc.setLocalDescription(object : SdpObserver {
                        override fun onSetSuccess() = waitIceComplete(pc, 0, onReady)
                        override fun onSetFailure(err: String?) =
                            onReady(Result.failure(NsbcError(NsbcErrorCode.MEDIA_FAILED, "setLocalDescription 失败: $err")))
                        override fun onCreateSuccess(p0: SessionDescription?) {}
                        override fun onCreateFailure(p0: String?) {}
                    }, SessionDescription(desc.type, sdp))
                }
                override fun onCreateFailure(err: String?) =
                    onReady(Result.failure(NsbcError(NsbcErrorCode.MEDIA_FAILED, "createAnswer 失败: $err")))
                override fun onSetSuccess() {}
                override fun onSetFailure(p0: String?) {}
            })
        } catch (t: Throwable) {
            onReady(Result.failure(NsbcError(NsbcErrorCode.MEDIA_FAILED, "创建 answer 异常: ${t.message}", t)))
        }
    }

    override fun applyRemoteSdp(sdp: String, isOffer: Boolean, onReady: (Result<Unit>) -> Unit) {
        if (isOffer) {
            remoteOfferHasVideo = sdp.contains("m=video ")
            NsbcLogger.i(tag, "远端 offer ${if (remoteOfferHasVideo) "含" else "不含"} m=video")
            // 呼入是新一轮协商，先清掉上一通呼叫遗留的 PeerConnection
            resetPeerConnection()
        }
        // 呼入场景 peerConnection 尚未创建，这里按需创建
        val pc = runCatching { ensurePeerConnection() }.getOrElse { e ->
            onReady(
                Result.failure(
                    NsbcError(NsbcErrorCode.MEDIA_FAILED, "创建 PeerConnection 失败: ${e.message}", e)
                )
            )
            return
        }
        val type = if (isOffer) SessionDescription.Type.OFFER else SessionDescription.Type.ANSWER
        pc.setRemoteDescription(object : SdpObserver {
            override fun onSetSuccess() {
                NsbcLogger.i(tag, "远端 SDP 已应用 (isOffer=$isOffer)")
                onReady(Result.success(Unit))
            }

            override fun onSetFailure(err: String?) =
                onReady(Result.failure(NsbcError(NsbcErrorCode.MEDIA_FAILED, "setRemoteDescription 失败: $err")))

            override fun onCreateSuccess(p0: SessionDescription?) {}
            override fun onCreateFailure(p0: String?) {}
        }, SessionDescription(type, sdp))
    }

    private val preparedRenderers = mutableSetOf<SurfaceViewRenderer>()

    override fun prepareRenderer(view: SurfaceViewRenderer) {
        // 未初始化（eglBase 为空）时直接返回，等 connect() 后 SDK 会重新调用
        val egl = eglBase ?: return
        // SurfaceViewRenderer.init 只允许调用一次，重复调用会抛异常
        if (!preparedRenderers.add(view)) return
        view.init(egl.eglBaseContext, null)
    }

    // ---------------------------------------------------------------- 渲染

    override fun attachLocalRenderer(view: SurfaceViewRenderer) {
        localRenderer = view
        // 前置摄像头镜像符合自拍习惯
        runCatching { view.setMirror(config?.mirrorLocalPreview ?: true) }
        videoTrack?.removeSink(view)
        videoTrack?.addSink(view)
    }

    override fun attachRemoteRenderer(view: SurfaceViewRenderer) {
        remoteRenderer = view
        remoteTrack?.let { track ->
            track.removeSink(view)
            track.addSink(view)
        }
    }

    // ---------------------------------------------------------------- 设备控制

    override fun setMicrophoneEnabled(enabled: Boolean) {
        micEnabled = enabled
        audioTrack?.setEnabled(enabled)
    }

    override fun setCameraEnabled(enabled: Boolean) {
        cameraEnabled = enabled
        videoTrack?.setEnabled(enabled)
        // 关闭时真正停采集（省电 + 隐私），重新打开时恢复
        if (enabled) startCaptureIfNeeded() else stopCapture()
    }

    override fun switchCamera() {
        (videoCapturer as? CameraVideoCapturer)?.switchCamera(null)
    }

    override fun onCallStopped() {
        // 通话结束停止采集；下一次呼叫在 attachLocalTracks 里自动恢复
        stopCapture()
    }

    // ---------------------------------------------------------------- 内部实现

    private var remoteTrack: VideoTrack? = null

    private fun ensurePeerConnection(): PeerConnection {
        peerConnection?.let { return it }
        val f = factory ?: error("PeerConnectionFactory 未初始化")
        val cfg = config ?: error("config 未初始化")

        val iceServers = mutableListOf<IceServer>()
        cfg.stunServer?.takeIf { it.isNotBlank() }?.let {
            iceServers.add(IceServer.builder(it).createIceServer())
            NsbcLogger.i(tag, "STUN: $it")
        }
        cfg.turnServer?.takeIf { it.isNotBlank() }?.let { url ->
            val builder = IceServer.builder(url)
            cfg.turnUsername?.let { builder.setUsername(it) }
            cfg.turnCredential?.let { builder.setPassword(it) }
            iceServers.add(builder.createIceServer())
            NsbcLogger.i(tag, "TURN: $url")
        }

        val rtc = PeerConnection.RTCConfiguration(iceServers).apply {
            // Unified Plan 为 libwebrtc 默认，无需显式设置
            if (cfg.icePolicy == IceTransportPolicy.RELAY) {
                iceTransportPolicy = PeerConnection.IceTransportPolicy.RELAY
            }
            bundlePolicy = PeerConnection.BundlePolicy.MAXBUNDLE
            rtcpMuxPolicy = PeerConnection.RtcpMuxPolicy.REQUIRE
            // 预生成候选池加速首次 ICE 收集（商业软电话标配）
            iceCandidatePoolSize = 10
        }

        val pc = f.createPeerConnection(rtc, object : PeerConnection.Observer {
            private var iceRestartAttempted = false

            override fun onIceCandidate(candidate: IceCandidate) {
                // Trickle ICE：本 SDK 采用「收集完成后一次性发送 offer」，
                // 因此丢弃 trickle candidate，最终 candidate 已包含在 SDP 中。
            }

            override fun onIceCandidatesRemoved(candidates: Array<out IceCandidate>?) {}

            override fun onIceConnectionChange(newState: PeerConnection.IceConnectionState) {
                NsbcLogger.i(tag, "ICE 状态: $newState")
                mainHandler.post { listener?.onIceStateChanged(newState.name.lowercase()) }
                // ICE failed：商业软电话标配，尝试 iceRestart 触发重新收集候选
                if (newState == PeerConnection.IceConnectionState.FAILED && !iceRestartAttempted) {
                    iceRestartAttempted = true
                    NsbcLogger.w(tag, "ICE failed，尝试 iceRestart")
                    runCatching { pc.restartIce() }
                }
            }

            override fun onIceConnectionReceivingChange(receiving: Boolean) {}
            override fun onIceGatheringChange(newState: PeerConnection.IceGatheringState) {
                NsbcLogger.d(tag, "ICE 收集: $newState")
            }

            override fun onAddStream(stream: MediaStream?) {
                stream?.videoTracks?.firstOrNull()?.let { bindRemoteTrack(it) }
            }

            override fun onRemoveStream(stream: MediaStream?) {}

            override fun onDataChannel(dc: org.webrtc.DataChannel?) {}

            override fun onRenegotiationNeeded() {}

            override fun onAddTrack(receiver: RtpReceiver?, mediaStreams: Array<out MediaStream>?) {
                (receiver?.track() as? VideoTrack)?.let { bindRemoteTrack(it) }
            }
        }) ?: error("createPeerConnection 返回 null")

        peerConnection = pc
        return pc
    }

    private fun bindRemoteTrack(track: VideoTrack) {
        remoteTrack = track
        track.setEnabled(true)
        remoteRenderer?.let {
            track.removeSink(it)
            track.addSink(it)
        }
    }

    /** 关闭并丢弃当前 PeerConnection（音视频轨、采集链路都保留复用）。 */
    private fun resetPeerConnection() {
        remoteTrack = null
        runCatching { peerConnection?.close() }
        runCatching { peerConnection?.dispose() }
        peerConnection = null
    }

    private fun attachLocalTracks(pc: PeerConnection, allowVideo: Boolean) {
        val f = factory ?: return
        val cfg = config ?: return

        // 音频：每通呼叫显式 addTransceiver 保证 Unified Plan 方向可控
        if (audioTrack == null) {
            val src = f.createAudioSource(MediaConstraints())
            audioSource = src
            audioTrack = f.createAudioTrack("audio0", src)
        }
        pc.addTransceiver("audio",
            RtpTransceiver.RtpTransceiverInit(RtpTransceiver.RtpTransceiverDirection.SEND_RECV)
        )
        // 绑定本地音轨到新建 transceiver 的 sender
        runCatching {
            pc.transceivers.lastOrNull()?.sender?.setTrack(audioTrack, false)
        }
        audioTrack?.setEnabled(micEnabled)

        if (!allowVideo) {
            NsbcLogger.i(tag, "本次呼叫不协商视频")
            return
        }

        // 视频：轨道创建一次复用，采集随通话/开关动态启停
        val track = ensureVideoTrack() ?: return
        pc.addTransceiver("video",
            RtpTransceiver.RtpTransceiverInit(RtpTransceiver.RtpTransceiverDirection.SEND_RECV)
        )
        runCatching {
            pc.transceivers.lastOrNull()?.sender?.setTrack(track, false)
        }
        track.setEnabled(cameraEnabled)
        if (cameraEnabled) startCaptureIfNeeded()
        localRenderer?.let {
            // 第二次呼叫复用同一 track，先移除再添加，避免重复 sink
            track.removeSink(it)
            track.addSink(it)
        }
    }

    /** 创建（或复用）摄像头采集链路与视频轨；失败返回 null 并降级为音频。 */
    private fun ensureVideoTrack(): VideoTrack? {
        videoTrack?.let { return it }
        val f = factory ?: return null
        val cfg = config ?: return null

        return runCatching {
            val enumerator = Camera2Enumerator(appContext)
            val name = enumerator.deviceNames.firstOrNull { enumerator.isFrontFacing(it) }
                ?: enumerator.deviceNames.firstOrNull()
                ?: return@runCatching null
            val capturer = enumerator.createCapturer(name, null)
                ?: return@runCatching null
            val source = f.createVideoSource(false)
            val helper = SurfaceTextureHelper.create("NsbcCapture", eglBase?.eglBaseContext)
                ?: error("SurfaceTextureHelper 创建失败")
            capturer.initialize(helper, appContext, source.capturerObserver)

            videoCapturer = capturer
            videoSource = source
            surfaceTextureHelper = helper
            f.createVideoTrack("video0", source).also { track ->
                videoTrack = track
                NsbcLogger.i(
                    tag,
                    "视频轨已创建 ${cfg.videoWidth}x${cfg.videoHeight}@${cfg.videoFps}"
                )
            }
        }.getOrElse {
            NsbcLogger.e(tag, "摄像头初始化失败，本次呼叫降级为音频", it)
            listener?.onError(
                NsbcError(NsbcErrorCode.MEDIA_FAILED, "摄像头不可用: ${it.message}", it)
            )
            null
        }
    }

    private fun startCaptureIfNeeded() {
        val c = videoCapturer ?: return
        if (isCapturing) return
        val cfg = config ?: return
        runCatching { c.startCapture(cfg.videoWidth, cfg.videoHeight, cfg.videoFps) }
            .onSuccess {
                isCapturing = true
                NsbcLogger.i(tag, "摄像头采集已启动")
            }
            .onFailure { NsbcLogger.e(tag, "启动摄像头采集失败", it) }
    }

    private fun stopCapture() {
        val c = videoCapturer ?: return
        if (!isCapturing) return
        runCatching { c.stopCapture() }
            .onFailure { NsbcLogger.w(tag, "停止摄像头采集失败: ${it.message}") }
        isCapturing = false
        NsbcLogger.i(tag, "摄像头采集已停止")
    }

    override fun close() {
        releaseInternal()
    }

    private fun releaseInternal() {
        stopCapture()
        runCatching { videoCapturer?.dispose() }
        videoCapturer = null

        runCatching { videoTrack?.dispose() }
        runCatching { videoSource?.dispose() }
        runCatching { surfaceTextureHelper?.dispose() }
        runCatching { audioTrack?.dispose() }
        runCatching { audioSource?.dispose() }
        videoTrack = null
        videoSource = null
        surfaceTextureHelper = null
        audioTrack = null
        audioSource = null
        remoteTrack = null

        runCatching { peerConnection?.close() }
        runCatching { peerConnection?.dispose() }
        peerConnection = null

        runCatching { factory?.dispose() }
        factory = null

        // 释放 SDK 绑定过的渲染器，避免下次 init 时重复初始化崩溃
        preparedRenderers.forEach { runCatching { it.release() } }
        preparedRenderers.clear()
        runCatching { eglBase?.release() }
        eglBase = null

        NsbcLogger.i(tag, "媒体引擎已释放")
    }

    // ---------------------------------------------------------------- 商业级 SDP 过滤

    /**
     * Offer SDP 过滤（商业软电话兼容策略）：
     *  - H.264 profile-level-id 非 baseline (42e01f) → 降级为 42e01f 并强制 packetization-mode=1
     *  - opus maxplaybackrate=8000（移动网络节省带宽）
     */
    private fun filterOfferSdp(sdp: String): String {
        var out = sdp
        // 定位 m=video section，改 H.264 fmtp
        out = out.replace(H264_FMTP_REGEX) { match ->
            val line = match.value
            val replaced = line.replace(
                Regex("profile-level-id=[0-9a-fA-F]{6}"),
                "profile-level-id=42e01f"
            ).let { if ("packetization-mode" !in it) "$it;packetization-mode=1" else it }
            if (replaced != line) NsbcLogger.d(tag, "H.264 fmtp 过滤: $line → $replaced")
            replaced
        }
        // opus maxplaybackrate
        out = out.replace(OPUS_FMTP_REGEX) { match ->
            val line = match.value
            if ("maxplaybackrate" in line) line else "$line;maxplaybackrate=8000"
        }
        return out
    }

    /**
     * Answer SDP 过滤：只处理 opus maxplaybackrate 与 H.264 packetization-mode，
     * 不降级 profile-level-id（那是 offer 才做的事）。
     */
    private fun filterAnswerSdp(sdp: String): String {
        var out = sdp
        out = out.replace(H264_FMTP_REGEX) { match ->
            val line = match.value
            if ("packetization-mode" in line) line else "$line;packetization-mode=1"
        }
        out = out.replace(OPUS_FMTP_REGEX) { match ->
            val line = match.value
            if ("maxplaybackrate" in line) line else "$line;maxplaybackrate=8000"
        }
        return out
    }

    companion object {
        private const val MEDIA_STREAM_ID = "nsbc"

        // 匹配 m=video section 内的 H.264 fmtp 行（profile-level-id 非 baseline 需降级）
        private val H264_FMTP_REGEX =
            Regex("""a=fmtp:\d+ profile-level-id=[0-9a-fA-F]{6}($|[^\r\n]*)""")
        // 匹配 opus fmtp 行（通常 pt=111，但不固定 pt 号）
        private val OPUS_FMTP_REGEX =
            Regex("""a=fmtp:111[^\r\n]*""")
    }
}
