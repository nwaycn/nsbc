package com.nway.nsbc.sdk

import org.json.JSONObject

/**
 * 日志级别。
 */
enum class LogLevel { NONE, ERROR, WARN, INFO, DEBUG, VERBOSE }

/**
 * ICE 传输策略。
 *
 * - [ALL]：允许直连 + 中继（默认）
 * - [RELAY]：只走 TURN 中继（公网严格 NAT 场景）
 */
enum class IceTransportPolicy { ALL, RELAY }

/**
 * NSBC SDK 的全部可配置项。
 *
 * 设计目标是「易配置」：所有字段都有合理默认值，最小只需 4 个字段
 * （[server] / [realm] / [username] / [password]）即可接通 NSBC。
 *
 * 三种配置方式（任选其一）：
 * 1. 代码直接构造：[NsbcConfig] 数据类构造器
 * 2. Builder（Java 友好）：[NsbcConfig.Builder]
 * 3. 从 JSON 加载：[NsbcConfig.fromJson]，可放在 `assets/nsbc.json`
 *
 * ```json
 * {
 *   "server": "10.0.0.80",
 *   "wssPort": 5062,
 *   "realm": "nsbc.local",
 *   "username": "1000",
 *   "password": "secret123",
 *   "stunServer": "stun:10.0.0.80:3478"
 * }
 * ```
 */
data class NsbcConfig(
    /** NSBC 主机名或 IP，例如 `10.0.0.80` / `sip.example.com` */
    val server: String = "",
    /** SIP over WebSocket 端口（NSBC 默认 `sip.ws.port=5062`） */
    val wssPort: Int = 5062,
    /** WebSocket 路径（NSBC 的 ws_listener 不校验路径，`/ws` 即可） */
    val wsPath: String = "/ws",
    /** 是否使用 TLS（WSS）。NSBC 默认 `sip.ws.tls=true` */
    val useTls: Boolean = true,
    /** SIP 域 / Realm，例如 `nsbc.local` */
    val realm: String = "",
    /** 分机号 / 账号 */
    val username: String = "",
    /** 密码（SIP Digest） */
    val password: String = "",
    /** 显示名，默认等于 [username] */
    val displayName: String = "",
    /** STUN 服务器，例如 `stun:10.0.0.80:3478`；null 表示不使用 */
    val stunServer: String? = null,
    /** TURN 服务器，例如 `turn:turn.example.com:3478`；null 表示不使用 */
    val turnServer: String? = null,
    val turnUsername: String? = null,
    val turnCredential: String? = null,
    /** 是否协商视频（关闭后只建立音频） */
    val videoEnabled: Boolean = true,
    /** 视频采集宽度（像素），默认 1280 */
    val videoWidth: Int = 1280,
    /** 视频采集高度（像素），默认 720 */
    val videoHeight: Int = 720,
    /** 视频采集帧率，默认 30 */
    val videoFps: Int = 30,
    /** 本地预览是否镜像（前置摄像头习惯镜像，后置建议 false） */
    val mirrorLocalPreview: Boolean = true,
    /** REGISTER 有效期（秒），NSBC 侧会缩小到配置上限 */
    val registerExpiresSec: Int = 300,
    /** 断线自动重连 */
    val autoReconnect: Boolean = true,
    /** 重连退避基数（毫秒），实际会指数退避 */
    val reconnectDelayMs: Long = 2000,
    /** ICE 传输策略 */
    val icePolicy: IceTransportPolicy = IceTransportPolicy.ALL,
    /**
     * 是否允许自签名证书。
     * 内网 NSBC 首次启动自动生成自签证书，必须开启才能握手；
     * 生产接入 CA 证书后建议关闭。
     */
    val allowSelfSignedCert: Boolean = true,
    /** 日志级别 */
    val logLevel: LogLevel = LogLevel.INFO
) {

    /** 实际使用的 WebSocket URL，例如 `wss://10.0.0.80:5062/ws` */
    val webSocketUrl: String
        get() {
            val scheme = if (useTls) "wss" else "ws"
            val path = if (wsPath.startsWith("/")) wsPath else "/$wsPath"
            return "$scheme://$server:$wssPort$path"
        }

    /** 本机 SIP AoR，例如 `sip:1000@nsbc.local` */
    val sipUri: String
        get() = "sip:$username@$realm"

    /** Contact 的传输参数（RFC 7118 规定 WS 连接必须声明 transport=ws） */
    val contactTransport: String get() = "ws"

    internal fun normalized(): NsbcConfig =
        copy(
            displayName = displayName.ifBlank { username },
            wsPath = if (wsPath.startsWith("/")) wsPath else "/$wsPath"
        )

    /**
     * 校验配置，返回问题列表。空列表表示可用。
     */
    fun validate(): List<String> = buildList {
        if (server.isBlank()) add("server 不能为空")
        if (username.isBlank()) add("username 不能为空")
        if (realm.isBlank()) add("realm 不能为空")
        if (password.isBlank()) add("password 不能为空")
        if (wssPort !in 1..65535) add("wssPort 超出范围: $wssPort")
        if (registerExpiresSec !in 30..3600) add("registerExpiresSec 建议在 30..3600")
        if (videoEnabled) {
            if (videoWidth !in 160..1920) add("videoWidth 超出范围: $videoWidth")
            if (videoHeight !in 120..1080) add("videoHeight 超出范围: $videoHeight")
            if (videoFps !in 1..60) add("videoFps 超出范围: $videoFps")
        }
        if (turnServer != null && turnUsername == null) add("配置了 turnServer 但缺少 turnUsername")
        if (icePolicy == IceTransportPolicy.RELAY && turnServer == null) {
            add("icePolicy=RELAY 时必须配置 turnServer")
        }
    }

    fun isValid(): Boolean = validate().isEmpty()

    /**
     * Java 友好构造器。
     */
    class Builder {
        private var c = NsbcConfig()

        fun server(v: String) = apply { c = c.copy(server = v) }
        fun wssPort(v: Int) = apply { c = c.copy(wssPort = v) }
        fun wsPath(v: String) = apply { c = c.copy(wsPath = v) }
        fun useTls(v: Boolean) = apply { c = c.copy(useTls = v) }
        fun realm(v: String) = apply { c = c.copy(realm = v) }
        fun username(v: String) = apply { c = c.copy(username = v) }
        fun password(v: String) = apply { c = c.copy(password = v) }
        fun displayName(v: String) = apply { c = c.copy(displayName = v) }
        fun stunServer(v: String?) = apply { c = c.copy(stunServer = v) }
        fun turn(v: String?, user: String?, credential: String?) = apply {
            c = c.copy(turnServer = v, turnUsername = user, turnCredential = credential)
        }
        fun videoEnabled(v: Boolean) = apply { c = c.copy(videoEnabled = v) }
        fun videoResolution(width: Int, height: Int) = apply {
            c = c.copy(videoWidth = width, videoHeight = height)
        }
        fun videoFps(v: Int) = apply { c = c.copy(videoFps = v) }
        fun mirrorLocalPreview(v: Boolean) = apply { c = c.copy(mirrorLocalPreview = v) }
        fun registerExpiresSec(v: Int) = apply { c = c.copy(registerExpiresSec = v) }
        fun autoReconnect(v: Boolean) = apply { c = c.copy(autoReconnect = v) }
        fun reconnectDelayMs(v: Long) = apply { c = c.copy(reconnectDelayMs = v) }
        fun icePolicy(v: IceTransportPolicy) = apply { c = c.copy(icePolicy = v) }
        fun allowSelfSignedCert(v: Boolean) = apply { c = c.copy(allowSelfSignedCert = v) }
        fun logLevel(v: LogLevel) = apply { c = c.copy(logLevel = v) }

        fun build(): NsbcConfig {
            val cfg = c.normalized()
            val problems = cfg.validate()
            if (problems.isNotEmpty()) {
                throw IllegalArgumentException("NsbcConfig 配置不合法: ${problems.joinToString("; ")}")
            }
            return cfg
        }
    }

    companion object {
        /**
         * 从 JSON 字符串解析。缺失字段使用默认值。
         */
        fun fromJson(json: String): NsbcConfig {
            val o = JSONObject(json)
            return NsbcConfig(
                server = o.optString("server", ""),
                wssPort = o.optInt("wssPort", 5062),
                wsPath = o.optString("wsPath", "/ws"),
                useTls = o.optBoolean("useTls", true),
                realm = o.optString("realm", ""),
                username = o.optString("username", ""),
                password = o.optString("password", ""),
                displayName = o.optString("displayName", ""),
                stunServer = o.optString("stunServer", "").ifBlank { null },
                turnServer = o.optString("turnServer", "").ifBlank { null },
                turnUsername = o.optString("turnUsername", "").ifBlank { null },
                turnCredential = o.optString("turnCredential", "").ifBlank { null },
                videoEnabled = o.optBoolean("videoEnabled", true),
                videoWidth = o.optInt("videoWidth", 1280),
                videoHeight = o.optInt("videoHeight", 720),
                videoFps = o.optInt("videoFps", 30),
                mirrorLocalPreview = o.optBoolean("mirrorLocalPreview", true),
                registerExpiresSec = o.optInt("registerExpiresSec", 300),
                autoReconnect = o.optBoolean("autoReconnect", true),
                reconnectDelayMs = o.optLong("reconnectDelayMs", 2000),
                icePolicy = if (o.optString("icePolicy", "ALL").equals("RELAY", true))
                    IceTransportPolicy.RELAY else IceTransportPolicy.ALL,
                allowSelfSignedCert = o.optBoolean("allowSelfSignedCert", true),
                logLevel = runCatching {
                    LogLevel.valueOf(o.optString("logLevel", "INFO").uppercase())
                }.getOrDefault(LogLevel.INFO)
            ).normalized()
        }

        fun builder() = Builder()
    }
}
