package com.nway.nsbc.sdk.sip

import android.os.Handler
import android.os.Looper
import com.nway.nsbc.sdk.CallState
import com.nway.nsbc.sdk.NsbcCall
import com.nway.nsbc.sdk.NsbcConfig
import com.nway.nsbc.sdk.NsbcError
import com.nway.nsbc.sdk.NsbcLogger
import com.nway.nsbc.sdk.RegistrationState
import com.nway.nsbc.sdk.media.MediaEngine
import com.nway.nsbc.sdk.net.NetworkUtils

/**
 * SIP 信令引擎：负责 REGISTER 与呼叫状态机（RFC 3261 / RFC 7118）。
 *
 * 只处理「一路呼叫」，足以覆盖手机端场景；多路并发可在上层扩展成 Map。
 */
internal class SipEngine(
    private val config: NsbcConfig,
    private val mediaEngine: MediaEngine,
    private val events: Events
) : SipTransport.Listener {

    interface Events {
        fun onTransportOpen()
        fun onTransportClosed(reason: String)
        fun onTransportFailure(error: NsbcError)
        fun onRegistrationChanged(state: RegistrationState, detail: String?)
        fun onIncomingCall(call: NsbcCall)
        fun onCallStateChanged(call: NsbcCall)
    }

    private val tag = "SipEngine"
    private val handler = Handler(Looper.getMainLooper())
    private val transport = SipTransport(config, this)
    private val localHost: String = NetworkUtils.localIpv4()
    private val displayName: String = config.displayName.ifBlank { config.username }
    /** Via 的传输协议：TLS 连接用 WSS，明文连接用 WS */
    private val viaProtocol: String = if (config.useTls) "WSS" else "WS"

    // ---- 注册状态 ----
    @Volatile var registrationState: RegistrationState = RegistrationState.UNREGISTERED
        private set

    private var regCseq = 0L
    private var regCallId = ""
    private var regTag = ""
    private var regBranch = ""
    private var regRefreshRunnable: Runnable? = null

    // ---- 鉴权缓存（按 realm）----
    private class AuthChallenge(
        val realm: String,
        val nonce: String,
        val qop: String?,
        val opaque: String?,
        val algorithm: String,
        val proxy: Boolean
    ) {
        var nc: Int = 0
    }

    private var regAuth: AuthChallenge? = null

    // ---- 当前呼叫 ----
    private class CallCtx(
        val call: NsbcCall,
        val callId: String,
        val localTag: String,
        val isIncoming: Boolean,
        var remoteUri: String
    ) {
        var remoteTag: String? = null
        var branch: String = SipUtils.newBranch()
        var cseq: Long = 0
        var localSdp: String? = null
        var remoteSdp: String? = null
        var remoteTarget: String? = null
        var auth: AuthChallenge? = null
        var acked: Boolean = false
        /** 呼入场景缓存 INVITE 原始请求，用于构造 200 / 487 响应 */
        var inviteRequest: SipMessage? = null
    }

    private var ctx: CallCtx? = null

    /** 当前活跃呼叫；无则 null。 */
    val currentCall: NsbcCall? get() = ctx?.call

    // ============================================================== 生命周期

    fun connect() {
        transport.connect()
    }

    fun disconnect() {
        cancelRefresh()
        ctx?.let { c ->
            if (c.call.state == CallState.ESTABLISHED) sendBye(c) else sendCancel(c)
        }
        ctx = null
        if (registrationState == RegistrationState.REGISTERED) sendUnregister()
        transport.disconnect()
        registrationState = RegistrationState.UNREGISTERED
        events.onRegistrationChanged(registrationState, "已主动断开")
    }

    val transportOpen: Boolean get() = transport.isOpen

    // ============================================================== Transport 回调

    override fun onOpen() {
        NsbcLogger.i(tag, "传输已就绪，开始注册")
        events.onTransportOpen()
        sendRegister(null)
    }

    override fun onClosed(code: Int, reason: String) {
        cancelRefresh()
        registrationState = RegistrationState.UNREGISTERED
        events.onRegistrationChanged(registrationState, "连接关闭: $reason")
        events.onTransportClosed(reason)
    }

    override fun onFailure(error: NsbcError) {
        cancelRefresh()
        registrationState = RegistrationState.FAILED
        events.onRegistrationChanged(registrationState, error.message)
        events.onTransportFailure(error)
    }

    override fun onSipMessage(raw: String) {
        val msg = SipMessage.parse(raw)
        if (msg == null) {
            NsbcLogger.w(tag, "无法解析 SIP 消息")
            return
        }
        when {
            msg.isResponse -> handleResponse(msg)
            msg.method == "INVITE" -> handleIncomingInvite(msg)
            msg.method == "ACK" -> handleIncomingAck(msg)
            msg.method == "BYE" -> handleIncomingBye(msg)
            msg.method == "CANCEL" -> handleIncomingCancel(msg)
            msg.method == "OPTIONS" -> reply(msg, 200, "OK")
            msg.method == "MESSAGE" -> handleIncomingMessage(msg)
            else -> reply(msg, 405, "Method Not Allowed")
        }
    }

    // ============================================================== REGISTER

    private fun sendRegister(auth: AuthChallenge?) {
        registrationState = RegistrationState.REGISTERING
        events.onRegistrationChanged(registrationState, null)

        if (regCallId.isEmpty()) {
            regCallId = SipUtils.newCallId(config.server)
            regTag = SipUtils.newTag()
        }
        regBranch = SipUtils.newBranch()
        regCseq++

        val req = SipMessage().apply {
            method = "REGISTER"
            requestUri = "sip:${config.realm}"
            addHeader("Via", "SIP/2.0/$viaProtocol $localHost;branch=$regBranch")
            addHeader("Max-Forwards", "70")
            addHeader("From", "\"${displayName}\" <${config.sipUri}>;tag=$regTag")
            addHeader("To", "<${config.sipUri}>")
            addHeader("Call-ID", regCallId)
            addHeader("CSeq", "$regCseq REGISTER")
            addHeader(
                "Contact",
                "<${config.sipUri};transport=${config.contactTransport}>"
            )
            addHeader("Expires", config.registerExpiresSec.toString())
            addHeader("Allow", "INVITE, ACK, CANCEL, BYE, OPTIONS, MESSAGE")
            addHeader("User-Agent", USER_AGENT)
        }
        auth?.let { a ->
            val uri = "sip:${config.realm}"
            req.addHeader(
                if (a.proxy) "Proxy-Authorization" else "Authorization",
                buildAuthValue(a, "REGISTER", uri)
            )
        }
        transport.send(req.toWire())
    }

    private fun sendUnregister() {
        // Expires: 0 注销
        regBranch = SipUtils.newBranch()
        regCseq++
        val req = SipMessage().apply {
            method = "REGISTER"
            requestUri = "sip:${config.realm}"
            addHeader("Via", "SIP/2.0/$viaProtocol $localHost;branch=$regBranch")
            addHeader("Max-Forwards", "70")
            addHeader("From", "\"${displayName}\" <${config.sipUri}>;tag=$regTag")
            addHeader("To", "<${config.sipUri}>")
            addHeader("Call-ID", regCallId)
            addHeader("CSeq", "$regCseq REGISTER")
            addHeader("Contact", "<${config.sipUri};transport=${config.contactTransport}>")
            addHeader("Expires", "0")
            addHeader("User-Agent", USER_AGENT)
        }
        regAuth?.let { a ->
            req.addHeader(
                if (a.proxy) "Proxy-Authorization" else "Authorization",
                buildAuthValue(a, "REGISTER", "sip:${config.realm}")
            )
        }
        transport.send(req.toWire())
    }

    private fun scheduleRefresh(expiresSec: Int) {
        cancelRefresh()
        val delayMs = (expiresSec.coerceAtLeast(30) * 1000L) / 2
        val r = Runnable {
            if (registrationState == RegistrationState.REGISTERED && transport.isOpen) {
                NsbcLogger.i(tag, "刷新注册")
                sendRegister(regAuth)
            }
        }
        regRefreshRunnable = r
        handler.postDelayed(r, delayMs)
    }

    private fun cancelRefresh() {
        regRefreshRunnable?.let { handler.removeCallbacks(it) }
        regRefreshRunnable = null
    }

    // ============================================================== 响应处理

    private fun handleResponse(msg: SipMessage) {
        val cseqMethod = msg.cseqMethod()
        val callId = msg.callId()

        when {
            // ---------- REGISTER 响应 ----------
            cseqMethod == "REGISTER" && callId == regCallId -> handleRegisterResponse(msg)

            // ---------- INVITE 响应 ----------
            cseqMethod == "INVITE" && ctx?.callId == callId -> handleInviteResponse(msg)

            // ---------- BYE 响应 ----------
            cseqMethod == "BYE" -> NsbcLogger.d(tag, "BYE 响应 ${msg.statusCode}")

            // ---------- CANCEL 响应 ----------
            cseqMethod == "CANCEL" -> NsbcLogger.d(tag, "CANCEL 响应 ${msg.statusCode}")

            else -> NsbcLogger.d(tag, "未匹配响应 $msg cseq=$cseqMethod")
        }
    }

    private fun handleRegisterResponse(msg: SipMessage) {
        when (val code = msg.statusCode ?: return) {
            200 -> {
                val expires = msg.header("Expires")?.toIntOrNull()
                    ?: config.registerExpiresSec
                registrationState = RegistrationState.REGISTERED
                NsbcLogger.i(tag, "注册成功，有效期 ${expires}s")
                events.onRegistrationChanged(registrationState, null)
                scheduleRefresh(expires)
            }
            401, 407 -> {
                val challengeHeader = if (code == 401)
                    msg.header("WWW-Authenticate") else msg.header("Proxy-Authenticate")
                if (challengeHeader == null) {
                    failRegistration("缺少鉴权挑战头")
                    return
                }
                val params = DigestAuth.parseChallenge(challengeHeader)
                regAuth = AuthChallenge(
                    realm = params["realm"] ?: config.realm,
                    nonce = params["nonce"] ?: "",
                    qop = params["qop"],
                    opaque = params["opaque"],
                    algorithm = params["algorithm"] ?: "MD5",
                    proxy = code == 407
                )
                NsbcLogger.i(tag, "收到 $code 挑战，带认证重发")
                sendRegister(regAuth)
            }
            in 400..699 -> failRegistration("注册被拒绝: $code ${msg.reasonPhrase}")
            else -> NsbcLogger.w(tag, "REGISTER 中间响应 $code")
        }
    }

    private fun failRegistration(detail: String) {
        registrationState = RegistrationState.FAILED
        NsbcLogger.e(tag, detail)
        events.onRegistrationChanged(registrationState, detail)
    }

    private fun handleInviteResponse(msg: SipMessage) {
        val c = ctx ?: return
        when (val code = msg.statusCode ?: return) {
            in 100..199 -> {
                if (code == 180 || code == 183) {
                    updateCall(c, CallState.RINGING)
                }
                // 183 带 early media SDP：可选，这里不处理
            }
            200 -> {
                c.remoteTag = msg.toTag()
                c.remoteTarget = msg.header("Contact")?.substringAfter("<")?.substringBefore(">")
                    ?: c.remoteUri
                sendAck(c)

                val sdp = msg.body
                if (sdp.isNotBlank()) {
                    c.remoteSdp = sdp
                    mediaEngine.applyRemoteSdp(sdp, isOffer = false) { r ->
                        if (r.isSuccess) {
                            establish(c)
                        } else {
                            NsbcLogger.e(tag, "应用 answer SDP 失败", r.exceptionOrNull())
                            failCall(c, "媒体协商失败")
                        }
                    }
                } else {
                    establish(c)
                }
            }
            401, 407 -> {
                val headerName = if (code == 401) "WWW-Authenticate" else "Proxy-Authenticate"
                val h = msg.header(headerName)
                if (h == null) {
                    failCall(c, "缺少鉴权挑战头")
                    return
                }
                val params = DigestAuth.parseChallenge(h)
                c.auth = AuthChallenge(
                    realm = params["realm"] ?: config.realm,
                    nonce = params["nonce"] ?: "",
                    qop = params["qop"],
                    opaque = params["opaque"],
                    algorithm = params["algorithm"] ?: "MD5",
                    proxy = code == 407
                )
                NsbcLogger.i(tag, "INVITE 收到 $code，带认证重发")
                sendInvite(c)
            }
            in 300..699 -> {
                val reason = when (code) {
                    486, 600 -> "busy"
                    487 -> "canceled"
                    408 -> "timeout"
                    603 -> "declined"
                    else -> "rejected_$code"
                }
                failCall(c, reason)
            }
            else -> Unit
        }
    }

    // ============================================================== 呼出

    fun startCall(target: String): NsbcCall {
        check(registrationState == RegistrationState.REGISTERED) {
            "尚未注册到 NSBC，无法呼叫"
        }
        check(ctx?.call?.isActive != true) { "已有进行中的呼叫" }

        val remoteUri = normalizeTarget(target)
        val callId = SipUtils.newCallId(config.server)
        val call = NsbcCall(
            isIncoming = false,
            remoteUri = remoteUri,
            id = callId,
            actions = callActions
        )
        val c = CallCtx(
            call = call,
            callId = callId,
            localTag = SipUtils.newTag(),
            isIncoming = false,
            remoteUri = remoteUri
        )
        ctx = c
        updateCall(c, CallState.CALLING)

        // 先创建媒体 offer，再发 INVITE（含完整 SDP）
        mediaEngine.createOffer { result ->
            result.onSuccess { sdp ->
                c.localSdp = sdp
                sendInvite(c)
            }.onFailure { e ->
                NsbcLogger.e(tag, "创建 offer 失败", e)
                failCall(c, "媒体初始化失败")
            }
        }
        return call
    }

    private fun sendInvite(c: CallCtx) {
        c.branch = SipUtils.newBranch()
        c.cseq++
        val req = SipMessage().apply {
            method = "INVITE"
            requestUri = c.remoteUri
            addHeader("Via", "SIP/2.0/$viaProtocol $localHost;branch=${c.branch}")
            addHeader("Max-Forwards", "70")
            addHeader("From", "\"${displayName}\" <${config.sipUri}>;tag=${c.localTag}")
            addHeader("To", "<${c.remoteUri}>")
            addHeader("Call-ID", c.callId)
            addHeader("CSeq", "${c.cseq} INVITE")
            addHeader(
                "Contact",
                "<${config.sipUri};transport=${config.contactTransport}>"
            )
            addHeader("Allow", "INVITE, ACK, CANCEL, BYE, OPTIONS, MESSAGE")
            addHeader("User-Agent", USER_AGENT)
            body = c.localSdp ?: ""
            if (body.isNotEmpty()) addHeader("Content-Type", "application/sdp")
        }
        c.auth?.let { a ->
            req.addHeader(
                if (a.proxy) "Proxy-Authorization" else "Authorization",
                buildAuthValue(a, "INVITE", c.remoteUri)
            )
        }
        transport.send(req.toWire())
    }

    private fun sendAck(c: CallCtx) {
        if (c.acked) return
        c.acked = true
        val req = SipMessage().apply {
            method = "ACK"
            requestUri = c.remoteTarget ?: c.remoteUri
            addHeader("Via", "SIP/2.0/$viaProtocol $localHost;branch=${SipUtils.newBranch()}")
            addHeader("Max-Forwards", "70")
            addHeader("From", "\"${displayName}\" <${config.sipUri}>;tag=${c.localTag}")
            addHeader(
                "To",
                "<${c.remoteUri}>" + (c.remoteTag?.let { ";tag=$it" } ?: "")
            )
            addHeader("Call-ID", c.callId)
            addHeader("CSeq", "${c.cseq} ACK")
            addHeader("User-Agent", USER_AGENT)
        }
        transport.send(req.toWire())
    }

    private fun sendBye(c: CallCtx) {
        c.cseq++
        val req = SipMessage().apply {
            method = "BYE"
            requestUri = c.remoteTarget ?: c.remoteUri
            addHeader("Via", "SIP/2.0/$viaProtocol $localHost;branch=${SipUtils.newBranch()}")
            addHeader("Max-Forwards", "70")
            addHeader("From", "\"${displayName}\" <${config.sipUri}>;tag=${c.localTag}")
            addHeader("To", "<${c.remoteUri}>" + (c.remoteTag?.let { ";tag=$it" } ?: ""))
            addHeader("Call-ID", c.callId)
            addHeader("CSeq", "${c.cseq} BYE")
            addHeader("User-Agent", USER_AGENT)
        }
        transport.send(req.toWire())
        terminate(c, "local_hangup")
    }

    private fun sendCancel(c: CallCtx) {
        val req = SipMessage().apply {
            method = "CANCEL"
            requestUri = c.remoteUri
            // CANCEL 使用与被取消 INVITE 相同的 branch
            addHeader("Via", "SIP/2.0/$viaProtocol $localHost;branch=${c.branch}")
            addHeader("Max-Forwards", "70")
            addHeader("From", "\"${displayName}\" <${config.sipUri}>;tag=${c.localTag}")
            addHeader("To", "<${c.remoteUri}>")
            addHeader("Call-ID", c.callId)
            addHeader("CSeq", "${c.cseq} CANCEL")
            addHeader("User-Agent", USER_AGENT)
        }
        transport.send(req.toWire())
        terminate(c, "local_cancel")
    }

    // ============================================================== 呼入

    private fun handleIncomingInvite(msg: SipMessage) {
        if (ctx != null) {
            reply(msg, 486, "Busy Here")
            return
        }
        val callId = msg.callId() ?: run { reply(msg, 400, "Bad Request"); return }
        val fromUri = msg.fromUri() ?: "sip:unknown"
        val call = NsbcCall(
            isIncoming = true,
            remoteUri = fromUri,
            id = callId,
            actions = callActions
        )
        val c = CallCtx(
            call = call,
            callId = callId,
            localTag = SipUtils.newTag(),
            isIncoming = true,
            remoteUri = fromUri
        )
        c.remoteTag = msg.fromTag()
        c.remoteSdp = msg.body
        c.inviteRequest = msg
        ctx = c

        reply(msg, 100, "Trying", c.localTag)
        reply(msg, 180, "Ringing", c.localTag)
        updateCall(c, CallState.RINGING)

        handler.post { events.onIncomingCall(call) }
    }

    private fun answerIncoming(c: CallCtx) {
        val offer = c.remoteSdp
        if (offer.isNullOrBlank()) {
            NsbcLogger.w(tag, "呼入 INVITE 无 SDP offer，直接应答空媒体")
            sendAnswer(c, "")
            return
        }
        mediaEngine.applyRemoteSdp(offer, isOffer = true) { r ->
            if (r.isFailure) {
                NsbcLogger.e(tag, "应用 offer 失败", r.exceptionOrNull())
                rejectIncoming(c, 488)
                return@applyRemoteSdp
            }
            mediaEngine.createAnswer { ans ->
                ans.onSuccess { sdp -> sendAnswer(c, sdp) }
                    .onFailure { e ->
                        NsbcLogger.e(tag, "创建 answer 失败", e)
                        rejectIncoming(c, 488)
                    }
            }
        }
    }

    private fun sendAnswer(c: CallCtx, sdp: String) {
        c.localSdp = sdp
        val invite = c.inviteRequest ?: lastRequest
        val resp = SipMessage.buildResponse(invite, 200, "OK", c.localTag).apply {
            addHeader("Contact", "<${config.sipUri};transport=${config.contactTransport}>")
            addHeader("Allow", "INVITE, ACK, CANCEL, BYE, OPTIONS, MESSAGE")
            addHeader("User-Agent", USER_AGENT)
            if (sdp.isNotEmpty()) {
                addHeader("Content-Type", "application/sdp")
                body = sdp
            }
        }
        transport.send(resp.toWire())
        establish(c)
    }

    private fun rejectIncoming(c: CallCtx, code: Int) {
        val reason = when (code) {
            486 -> "Busy Here"
            603 -> "Decline"
            488 -> "Not Acceptable Here"
            else -> "Rejected"
        }
        val invite = c.inviteRequest ?: lastRequest
        transport.send(SipMessage.buildResponse(invite, code, reason, c.localTag).toWire())
        terminate(c, "local_reject")
    }

    /** 缓存最近的请求，用于构造 200/488 等响应。 */
    private var lastRequest: SipMessage = SipMessage()

    private fun handleIncomingAck(msg: SipMessage) {
        val c = ctx ?: return
        if (c.isIncoming && !c.acked) {
            c.acked = true
            if (c.call.state != CallState.ESTABLISHED) establish(c)
        }
    }

    private fun handleIncomingBye(msg: SipMessage) {
        reply(msg, 200, "OK")
        ctx?.let { terminate(it, "remote_hangup") }
    }

    private fun handleIncomingCancel(msg: SipMessage) {
        reply(msg, 200, "OK")
        // 同时给被取消的 INVITE 回 487（必须用 INVITE 原始请求，不能用 CANCEL）
        val c = ctx ?: return
        val invite = c.inviteRequest ?: lastRequest
        transport.send(
            SipMessage.buildResponse(invite, 487, "Request Terminated", c.localTag).toWire()
        )
        terminate(c, "remote_cancel")
    }

    private fun handleIncomingMessage(msg: SipMessage) {
        reply(msg, 200, "OK")
        NsbcLogger.i(tag, "收到 MESSAGE: ${msg.body.take(100)}")
    }

    // ============================================================== 工具

    private fun reply(
        request: SipMessage,
        code: Int,
        reason: String,
        toTag: String? = null,
        extra: ((SipMessage) -> Unit)? = null
    ) {
        lastRequest = request
        val resp = SipMessage.buildResponse(request, code, reason, toTag).apply {
            extra?.invoke(this)
        }
        transport.send(resp.toWire())
    }

    private fun normalizeTarget(target: String): String = when {
        target.startsWith("sip:") || target.startsWith("sips:") -> target
        target.contains("@") -> "sip:$target"
        else -> "sip:$target@${config.realm}"
    }

    private fun buildAuthValue(a: AuthChallenge, method: String, uri: String): String {
        a.nc++
        val ncHex = "%08x".format(a.nc)
        val cnonce = SipUtils.newTag()
        return DigestAuth.buildAuthorization(
            username = config.username,
            password = config.password,
            realm = a.realm,
            nonce = a.nonce,
            uri = uri,
            method = method,
            qop = a.qop,
            nc = ncHex,
            cnonce = cnonce,
            algorithm = a.algorithm,
            opaque = a.opaque
        )
    }

    private fun updateCall(c: CallCtx, state: CallState) {
        if (c.call.state == state) return
        c.call.state = state
        NsbcLogger.i(tag, "呼叫状态 -> $state (${c.remoteUri})")
        handler.post { events.onCallStateChanged(c.call) }
    }

    private fun establish(c: CallCtx) {
        c.call.establishedAtMs = System.currentTimeMillis()
        updateCall(c, CallState.ESTABLISHED)
    }

    private fun failCall(c: CallCtx, reason: String) {
        c.call.endReason = reason
        updateCall(c, CallState.FAILED)
        terminate(c, reason, alreadyFailed = true)
    }

    private fun terminate(c: CallCtx, reason: String, alreadyFailed: Boolean = false) {
        c.call.endReason = reason
        c.call.lastEndedAtMs = System.currentTimeMillis()
        if (!alreadyFailed && c.call.state != CallState.FAILED) {
            updateCall(c, CallState.TERMINATED)
        }
        if (ctx === c) ctx = null
        // 通话结束：停采集省电（下一次呼叫自动恢复）
        runCatching { mediaEngine.onCallStopped() }
    }

    // ============================================================== Call 动作

    private val callActions = object : NsbcCall.Actions {
        override fun answer(call: NsbcCall) {
            val c = ctx ?: return
            if (!c.isIncoming || c.call !== call) return
            answerIncoming(c)
        }

        override fun reject(call: NsbcCall, code: Int) {
            val c = ctx ?: return
            if (!c.isIncoming || c.call !== call) return
            rejectIncoming(c, code)
        }

        override fun hangup(call: NsbcCall) {
            val c = ctx ?: return
            if (c.call !== call) return
            when (call.state) {
                CallState.ESTABLISHED -> sendBye(c)
                CallState.CALLING, CallState.RINGING -> {
                    if (c.isIncoming) rejectIncoming(c, 486) else sendCancel(c)
                }
                else -> Unit
            }
        }

        override fun setMuted(call: NsbcCall, muted: Boolean) {
            mediaEngine.setMicrophoneEnabled(!muted)
        }

        override fun setVideoEnabled(call: NsbcCall, enabled: Boolean) {
            mediaEngine.setCameraEnabled(enabled)
        }

        override fun switchCamera(call: NsbcCall) {
            mediaEngine.switchCamera()
        }
    }

    companion object {
        private const val USER_AGENT = "NsbcAndroidSDK/1.0"
    }
}
