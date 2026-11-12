package com.nway.nsbc.sdk.sip

import com.nway.nsbc.sdk.NsbcConfig
import com.nway.nsbc.sdk.NsbcError
import com.nway.nsbc.sdk.NsbcErrorCode
import com.nway.nsbc.sdk.NsbcLogger
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

/**
 * SIP over WebSocket 传输层（RFC 7118）。
 *
 * 只负责「建链 / 收发文本帧 / 断链通知」，不做任何 SIP 语义处理。
 * 上层 [SipEngine] 负责事务与状态机。
 */
class SipTransport(
    private val config: NsbcConfig,
    private val listener: Listener
) {

    interface Listener {
        fun onOpen()
        fun onSipMessage(raw: String)
        fun onClosed(code: Int, reason: String)
        fun onFailure(error: NsbcError)
    }

    private val tag = "SipTransport"
    private val opened = AtomicBoolean(false)
    private var webSocket: WebSocket? = null
    private var client: OkHttpClient? = null

    val isOpen: Boolean get() = opened.get()

    // ---------------------------------------------------------------- 建链

    fun connect() {
        if (opened.get()) return
        val http = buildClient()
        client = http
        val req = Request.Builder()
            .url(config.webSocketUrl)
            .header("Sec-WebSocket-Protocol", "sip")
            .build()

        NsbcLogger.i(tag, "连接 ${config.webSocketUrl}")
        webSocket = http.newWebSocket(req, object : WebSocketListener() {
            override fun onOpen(ws: WebSocket, response: Response) {
                opened.set(true)
                NsbcLogger.i(tag, "WSS 已连接")
                listener.onOpen()
            }

            override fun onMessage(ws: WebSocket, text: String) {
                NsbcLogger.d(tag, "<<< ${text.lineSequence().firstOrNull() ?: ""}")
                listener.onSipMessage(text)
            }

            override fun onClosing(ws: WebSocket, code: Int, reason: String) {
                ws.close(1000, null)
            }

            override fun onClosed(ws: WebSocket, code: Int, reason: String) {
                if (opened.compareAndSet(true, false)) {
                    NsbcLogger.w(tag, "WSS 已关闭 code=$code reason=$reason")
                    listener.onClosed(code, reason)
                }
                cleanup()
            }

            override fun onFailure(ws: WebSocket, t: Throwable, response: Response?) {
                val wasOpen = opened.getAndSet(false)
                val err = NsbcError(
                    if (wasOpen) NsbcErrorCode.WS_CLOSED else NsbcErrorCode.NETWORK_UNREACHABLE,
                    "WebSocket 失败: ${t.message}", t
                )
                NsbcLogger.e(tag, "WSS 失败", t)
                listener.onFailure(err)
                cleanup()
            }
        })
    }

    /** 发送 SIP 消息（TEXT 帧）。返回是否成功入队。 */
    fun send(raw: String): Boolean {
        val ws = webSocket
        if (ws == null || !opened.get()) {
            NsbcLogger.w(tag, "未连接，丢弃发送: ${raw.lineSequence().firstOrNull()}")
            return false
        }
        NsbcLogger.d(tag, ">>> ${raw.lineSequence().firstOrNull() ?: ""}")
        return ws.send(raw)
    }

    fun disconnect(code: Int = 1000, reason: String = "bye") {
        // 只用 close()：先发送关闭帧，已入队的注销 REGISTER 仍会被写出去。
        // 不用 cancel()，否则会立即中止连接导致注销/挂断消息丢失。
        runCatching { webSocket?.close(code, reason) }
        opened.set(false)
        cleanup()
    }

    /** 释放当前连接对应的 HTTP 客户端资源（可重复调用）。 */
    private fun cleanup() {
        webSocket = null
        client?.let {
            it.dispatcher.executorService.shutdown()
            it.connectionPool.evictAll()
        }
        client = null
    }

    // ---------------------------------------------------------------- HTTP 客户端

    private fun buildClient(): OkHttpClient {
        val b = OkHttpClient.Builder()
            .connectTimeout(15, TimeUnit.SECONDS)
            .readTimeout(0, TimeUnit.MILLISECONDS)   // WebSocket 长连接不设读超时
            .writeTimeout(15, TimeUnit.SECONDS)
            .pingInterval(30, TimeUnit.SECONDS)      // 保活

        if (config.allowSelfSignedCert) {
            // 内网 NSBC 首次启动生成自签证书；仅当显式允许时启用宽松校验
            val trustAll = arrayOf<TrustManager>(object : X509TrustManager {
                override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
                override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
                override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
            })
            val ctx = SSLContext.getInstance("TLS").apply {
                init(null, trustAll, SecureRandom())
            }
            b.sslSocketFactory(ctx.socketFactory, trustAll[0] as X509TrustManager)
            b.hostnameVerifier { _, _ -> true }
        }
        return b.build()
    }
}
