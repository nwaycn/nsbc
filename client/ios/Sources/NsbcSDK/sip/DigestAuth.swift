import Foundation

/// SIP Digest 认证（RFC 7616 / RFC 2617）。
///
/// 支持 `qop=auth` 与无 qop 两种模式；算法支持 MD5 与 MD5-sess。
/// NSBC 默认使用 MD5 + qop=auth。
public enum DigestAuth {

    /// 从 `WWW-Authenticate` / `Proxy-Authenticate` 头解析挑战参数。
    public static func parseChallenge(_ headerValue: String) -> [String: String] {
        var result: [String: String] = [:]
        // 去掉 scheme 前缀（Digest）
        var body = headerValue.trimmingCharacters(in: .whitespaces)
        if body.lowercased().hasPrefix("digest") {
            body = String(body.dropFirst("digest".count)).trimmingCharacters(in: .whitespaces)
        }

        // 按逗号分割，但要忽略引号内的逗号
        var buffer = ""
        var inQuote = false
        for ch in body {
            if ch == "\"" {
                inQuote.toggle()
                buffer.append(ch)
            } else if ch == "," && !inQuote {
                if let pair = parsePair(buffer) {
                    result[pair.0] = pair.1
                }
                buffer = ""
            } else {
                buffer.append(ch)
            }
        }
        if let pair = parsePair(buffer) {
            result[pair.0] = pair.1
        }
        return result
    }

    private static func parsePair(_ raw: String) -> (String, String)? {
        guard let idx = raw.firstIndex(of: "=") else { return nil }
        let key = String(raw[..<idx]).trimmingCharacters(in: .whitespaces).lowercased()
        var value = String(raw[raw.index(after: idx)...]).trimmingCharacters(in: .whitespaces)
        value = value.trimmingCharacters(in: CharacterSet(charactersIn: "\""))
        guard !key.isEmpty else { return nil }
        return (key, value)
    }

    /// 计算 Authorization / Proxy-Authorization 头的值。
    public static func buildAuthorization(
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
        opaque: String? = nil
    ) -> String {
        let ha1Base = SipUtils.md5("\(username):\(realm):\(password)")
        let ha1: String
        if algorithm.lowercased() == "md5-sess" {
            ha1 = SipUtils.md5("\(ha1Base):\(nonce):\(cnonce)")
        } else {
            ha1 = ha1Base
        }

        let ha2 = SipUtils.md5("\(method):\(uri)")

        let response: String
        if let qop = qop, !qop.isEmpty {
            response = SipUtils.md5("\(ha1):\(nonce):\(nc):\(cnonce):\(qop):\(ha2)")
        } else {
            response = SipUtils.md5("\(ha1):\(nonce):\(ha2)")
        }

        var sb = ""
        sb += "Digest username=\"\(username)\""
        sb += ", realm=\"\(realm)\""
        sb += ", nonce=\"\(nonce)\""
        sb += ", uri=\"\(uri)\""
        sb += ", response=\"\(response)\""
        if !algorithm.isEmpty {
            sb += ", algorithm=\(algorithm)"
        }
        if let qop = qop, !qop.isEmpty {
            sb += ", qop=\(qop)"
            sb += ", nc=\(nc)"
            sb += ", cnonce=\"\(cnonce)\""
        }
        if let opaque = opaque {
            sb += ", opaque=\"\(opaque)\""
        }
        return sb
    }
}
