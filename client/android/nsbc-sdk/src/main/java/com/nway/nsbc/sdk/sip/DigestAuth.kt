package com.nway.nsbc.sdk.sip

/**
 * SIP Digest 认证（RFC 7616 / RFC 2617）。
 *
 * 支持 `qop=auth` 与无 qop 两种模式；算法支持 MD5 与 MD5-sess。
 * NSBC 默认使用 MD5 + qop=auth。
 */
object DigestAuth {

    /** 从 `WWW-Authenticate` / `Proxy-Authenticate` 头解析挑战参数。 */
    fun parseChallenge(headerValue: String): Map<String, String> {
        val result = mutableMapOf<String, String>()
        // 去掉 scheme 前缀（Digest）
        val body = headerValue.trim().removePrefix("Digest").trim()
        // 按逗号分割，但要忽略引号内的逗号
        var i = 0
        val sb = StringBuilder()
        var inQuote = false
        while (i < body.length) {
            val ch = body[i]
            when {
                ch == '"' -> { inQuote = !inQuote; sb.append(ch) }
                ch == ',' && !inQuote -> {
                    parsePair(sb.toString())?.let { result[it.first] = it.second }
                    sb.setLength(0)
                }
                else -> sb.append(ch)
            }
            i++
        }
        parsePair(sb.toString())?.let { result[it.first] = it.second }
        return result
    }

    private fun parsePair(raw: String): Pair<String, String>? {
        val idx = raw.indexOf('=')
        if (idx <= 0) return null
        val k = raw.substring(0, idx).trim()
        val v = raw.substring(idx + 1).trim().trim('"')
        if (k.isBlank()) return null
        return k.lowercase() to v
    }

    /**
     * 计算 Authorization / Proxy-Authorization 头的值。
     *
     * @param username 账号
     * @param password 明文密码
     * @param realm    挑战中的 realm
     * @param nonce    挑战中的 nonce
     * @param uri      请求 URI（与请求行一致）
     * @param method   请求方法
     * @param qop      挑战中的 qop（可能为空）
     * @param nc       nonce count（qop=auth 时必填）
     * @param cnonce   client nonce（qop=auth 时必填）
     * @param algorithm 算法，默认 MD5
     * @param opaque   挑战中的 opaque（原样回传）
     */
    fun buildAuthorization(
        username: String,
        password: String,
        realm: String,
        nonce: String,
        uri: String,
        method: String,
        qop: String?,
        nc: String,
        cnonce: String,
        algorithm: String = "MD5",
        opaque: String? = null
    ): String {
        val ha1Base = SipUtils.md5("$username:$realm:$password")
        val ha1 = if (algorithm.equals("MD5-sess", true)) {
            SipUtils.md5("$ha1Base:$nonce:$cnonce")
        } else ha1Base

        val ha2 = SipUtils.md5("$method:$uri")

        val response = if (qop.isNullOrBlank()) {
            SipUtils.md5("$ha1:$nonce:$ha2")
        } else {
            SipUtils.md5("$ha1:$nonce:$nc:$cnonce:$qop:$ha2")
        }

        val sb = StringBuilder()
        sb.append("Digest username=\"").append(username).append('"')
        sb.append(", realm=\"").append(realm).append('"')
        sb.append(", nonce=\"").append(nonce).append('"')
        sb.append(", uri=\"").append(uri).append('"')
        sb.append(", response=\"").append(response).append('"')
        if (algorithm.isNotEmpty()) sb.append(", algorithm=").append(algorithm)
        if (!qop.isNullOrBlank()) {
            sb.append(", qop=").append(qop)
            sb.append(", nc=").append(nc)
            sb.append(", cnonce=\"").append(cnonce).append('"')
        }
        if (opaque != null) sb.append(", opaque=\"").append(opaque).append('"')
        return sb.toString()
    }
}
