package com.nway.nsbc.sdk.sip

import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.atomic.AtomicLong

/** SIP 工具函数。 */
object SipUtils {

    private val branchCounter = AtomicLong(0)

    /** 生成 `z9hG4bK-` 前缀的 RFC 3261 branch。 */
    fun newBranch(): String =
        "z9hG4bK-${System.nanoTime()}-${branchCounter.incrementAndGet()}"

    /** 生成 tag（不含非法字符）。 */
    fun newTag(): String = UUID.randomUUID().toString().replace("-", "").take(12)

    /** 生成 Call-ID。 */
    fun newCallId(host: String): String =
        "${UUID.randomUUID()}.${System.nanoTime()}@$host"

    /** 生成 SDP 用的会话 id。 */
    fun newSessionId(): Long = System.currentTimeMillis() / 1000

    /** 提取 URI 中的 user 部分。 */
    fun userOf(uri: String?): String? {
        if (uri == null) return null
        val s = uri.substringAfter("sip:", uri)
            .substringAfter("sips:", uri)
        return s.substringBefore("@").ifBlank { null }
    }

    /** 提取 URI 中的 host 部分。 */
    fun hostOf(uri: String?): String? {
        if (uri == null) return null
        val s = uri.substringAfter("sip:", uri).substringAfter("sips:", uri)
        if (!s.contains("@")) return null
        return s.substringAfter("@").substringBefore(":").substringBefore(";")
    }

    /** MD5 十六进制小写。 */
    fun md5(input: String): String {
        val d = MessageDigest.getInstance("MD5")
        val bytes = d.digest(input.toByteArray(Charsets.UTF_8))
        return bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }

    /** 简单 XML 转义（NOTIFY 等场景可选）。 */
    fun xmlEscape(s: String): String = s
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
}
