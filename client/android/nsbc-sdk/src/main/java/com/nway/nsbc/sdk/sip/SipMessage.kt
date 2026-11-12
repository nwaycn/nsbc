package com.nway.nsbc.sdk.sip

/**
 * 极简 SIP 消息模型（RFC 3261）。
 *
 * 只覆盖 NSBC 客户端所需子集：请求行 / 状态行、头部（保序、大小写不敏感）、消息体。
 * 解析时兼容紧凑头（`v`=Via、`f`=From、`t`=To、`i`=Call-ID、`m`=Contact、`l`=Content-Length…）。
 */
class SipMessage {

    /** 请求方法（请求消息），如 `REGISTER`；状态消息为 null */
    var method: String? = null
    /** 请求 URI（请求消息） */
    var requestUri: String? = null
    /** 状态码（响应消息） */
    var statusCode: Int? = null
    /** 原因短语 */
    var reasonPhrase: String? = null

    /** 头部（保序） */
    private val headers = mutableListOf<Pair<String, String>>()

    /** 消息体 */
    var body: String = ""

    val isRequest: Boolean get() = method != null
    val isResponse: Boolean get() = statusCode != null

    // ---------------------------------------------------------------- 头部操作

    fun addHeader(name: String, value: String): SipMessage {
        headers.add(name to value)
        return this
    }

    fun setHeader(name: String, value: String): SipMessage {
        val key = canonical(name)
        val idx = headers.indexOfFirst { canonical(it.first) == key }
        if (idx >= 0) {
            headers[idx] = name to value
            // 移除同名重复项（除 Via/Record-Route 等允许多值的头部）
            if (key !in MULTI_VALUE) {
                var i = headers.size - 1
                while (i > idx) {
                    if (canonical(headers[i].first) == key) headers.removeAt(i)
                    i--
                }
            }
        } else {
            headers.add(name to value)
        }
        return this
    }

    fun header(name: String): String? =
        headers.firstOrNull { canonical(it.first) == canonical(name) }?.second

    fun headers(name: String): List<String> =
        headers.filter { canonical(it.first) == canonical(name) }.map { it.second }

    fun removeHeader(name: String): SipMessage {
        headers.removeAll { canonical(it.first) == canonical(name) }
        return this
    }

    fun allHeaders(): List<Pair<String, String>> = headers.toList()

    /** CSeq 的数字部分 */
    fun cseqNumber(): Long =
        header("CSeq")?.trim()?.split(Regex("\\s+"))?.firstOrNull()?.toLongOrNull() ?: 0L

    /** CSeq 的方法部分 */
    fun cseqMethod(): String? =
        header("CSeq")?.trim()?.split(Regex("\\s+"))?.getOrNull(1)

    /** Call-ID */
    fun callId(): String? = header("Call-ID")

    /** 从 From/To 头部提取 SIP URI（尖括号内） */
    fun fromUri(): String? = extractUri(header("From"))
    fun toUri(): String? = extractUri(header("To"))

    /** 从 From/To 头部提取 tag */
    fun fromTag(): String? = extractTag(header("From"))
    fun toTag(): String? = extractTag(header("To"))

    // ---------------------------------------------------------------- 序列化

    fun toWire(): String {
        val sb = StringBuilder()
        when {
            isRequest -> sb.append(method).append(' ').append(requestUri).append(" SIP/2.0\r\n")
            isResponse -> sb.append("SIP/2.0 ").append(statusCode).append(' ')
                .append(reasonPhrase ?: "").append("\r\n")
            else -> error("SipMessage 既不是请求也不是响应")
        }
        for ((n, v) in headers) {
            // 跳过自动计算的 Content-Length
            if (canonical(n) == "content-length") continue
            sb.append(n).append(": ").append(v).append("\r\n")
        }
        sb.append("Content-Length: ").append(body.toByteArray(Charsets.UTF_8).size).append("\r\n")
        sb.append("\r\n")
        if (body.isNotEmpty()) sb.append(body)
        return sb.toString()
    }

    override fun toString(): String =
        if (isRequest) "$method $requestUri" else "$statusCode $reasonPhrase"

    companion object {

        private val MULTI_VALUE = setOf("via", "record-route", "route", "contact")

        private val COMPACT = mapOf(
            "v" to "Via", "f" to "From", "t" to "To", "i" to "Call-ID",
            "m" to "Contact", "l" to "Content-Length", "c" to "Content-Type",
            "k" to "Supported", "s" to "Subject", "e" to "Content-Encoding",
            "o" to "Event", "u" to "Allow-Events", "x" to "Session-Expires"
        )

        private fun canonical(name: String): String {
            val n = name.trim()
            COMPACT[n.lowercase()]?.let { return it.lowercase() }
            return n.lowercase()
        }

        private fun extractUri(h: String?): String? {
            if (h == null) return null
            val lt = h.indexOf('<')
            val gt = h.indexOf('>')
            return if (lt >= 0 && gt > lt) h.substring(lt + 1, gt).trim()
            else h.substringBefore(';').trim()
        }

        private fun extractTag(h: String?): String? {
            if (h == null) return null
            val m = Regex(";tag=([^;>\\s]+)").find(h) ?: return null
            return m.groupValues[1]
        }

        /**
         * 解析线上消息。失败返回 null。
         */
        fun parse(raw: String): SipMessage? {
            if (raw.isBlank()) return null
            val normalized = raw.replace("\r\n", "\n")
            val sep = normalized.indexOf("\n\n")
            val headPart = if (sep >= 0) normalized.substring(0, sep) else normalized
            val bodyPart = if (sep >= 0) normalized.substring(sep + 2) else ""

            val lines = headPart.split('\n')
            if (lines.isEmpty()) return null

            val msg = SipMessage()
            val first = lines[0].trim()
            if (first.startsWith("SIP/2.0")) {
                // 状态行
                val parts = first.split(' ', limit = 3)
                msg.statusCode = parts.getOrNull(1)?.toIntOrNull() ?: return null
                msg.reasonPhrase = parts.getOrNull(2) ?: ""
            } else {
                // 请求行
                val parts = first.split(' ', limit = 3)
                if (parts.size < 2 || parts[2].uppercase() != "SIP/2.0") return null
                msg.method = parts[0].uppercase()
                msg.requestUri = parts[1]
            }

            // 头部（支持折行：以空格/制表符开头的行是上一行的续行）
            var current: Pair<String, String>? = null
            for (i in 1 until lines.size) {
                val line = lines[i]
                if (line.isEmpty()) break
                if (line[0] == ' ' || line[0] == '\t') {
                    current?.let { (n, v) -> msg.headers[msg.headers.size - 1] = n to (v + " " + line.trim()) }
                    continue
                }
                val idx = line.indexOf(':')
                if (idx <= 0) continue
                val name = line.substring(0, idx).trim()
                val value = line.substring(idx + 1).trim()
                msg.addHeader(name, value)
                current = name to value
            }

            msg.body = bodyPart
            // 用头部声明的 Content-Length 截断（避免分帧残留）
            val cl = msg.header("Content-Length")?.trim()?.toIntOrNull()
            if (cl != null && cl in 0..msg.body.length) {
                msg.body = msg.body.substring(0, cl)
            }
            return msg
        }

        // ------------------------------------------------------------ 构造器

        fun buildResponse(
            request: SipMessage,
            code: Int,
            reason: String,
            toTag: String? = null
        ): SipMessage {
            val r = SipMessage()
            r.statusCode = code
            r.reasonPhrase = reason
            // 逐条复制关键头部（Via 保序）
            request.headers("Via").forEach { r.addHeader("Via", it) }
            request.header("From")?.let { r.addHeader("From", it) }
            request.header("To")?.let { to ->
                // To tag 必须在同一请求的所有响应中保持一致；
                // 请求 To 已带 tag 时原样回传，否则补一个稳定 tag。
                if (extractTag(to) != null) {
                    r.addHeader("To", to)
                } else {
                    r.addHeader("To", "$to;tag=${toTag ?: SipUtils.newTag()}")
                }
            }
            request.header("Call-ID")?.let { r.addHeader("Call-ID", it) }
            request.header("CSeq")?.let { r.addHeader("CSeq", it) }
            return r
        }
    }
}
