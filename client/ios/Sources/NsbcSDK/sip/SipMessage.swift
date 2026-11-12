import Foundation

/// 极简 SIP 消息模型（RFC 3261）。
///
/// 只覆盖 NSBC 客户端所需子集：请求行 / 状态行、头部（保序、大小写不敏感）、消息体。
/// 解析时兼容紧凑头（`v`=Via、`f`=From、`t`=To、`i`=Call-ID、`m`=Contact、`l`=Content-Length…）。
public final class SipMessage {

    /// 请求方法（请求消息），如 `REGISTER`；状态消息为 nil
    public var method: String?
    /// 请求 URI（请求消息）
    public var requestUri: String?
    /// 状态码（响应消息）
    public var statusCode: Int?
    /// 原因短语
    public var reasonPhrase: String?
    /// 消息体
    public var body: String = ""

    private var headerList: [(name: String, value: String)] = []

    public var isRequest: Bool { method != nil }
    public var isResponse: Bool { statusCode != nil }

    public init() {}

    // MARK: - 头部操作

    @discardableResult
    public func addHeader(_ name: String, _ value: String) -> SipMessage {
        headerList.append((name: name, value: value))
        return self
    }

    public func header(_ name: String) -> String? {
        let key = SipMessage.canonical(name)
        return headerList.first { SipMessage.canonical($0.name) == key }?.value
    }

    public func headers(_ name: String) -> [String] {
        let key = SipMessage.canonical(name)
        return headerList.filter { SipMessage.canonical($0.name) == key }.map { $0.value }
    }

    /// CSeq 的数字部分
    public func cseqNumber() -> Int64 {
        guard let cseq = header("CSeq") else { return 0 }
        let parts = cseq.trimmingCharacters(in: .whitespaces)
            .split(whereSeparator: { $0 == " " || $0 == "\t" })
        return parts.first.flatMap { Int64($0) } ?? 0
    }

    /// CSeq 的方法部分
    public func cseqMethod() -> String? {
        guard let cseq = header("CSeq") else { return nil }
        let parts = cseq.trimmingCharacters(in: .whitespaces)
            .split(whereSeparator: { $0 == " " || $0 == "\t" })
        return parts.count > 1 ? String(parts[1]) : nil
    }

    /// Call-ID
    public func callId() -> String? { header("Call-ID") }

    /// 从 From/To 头部提取 SIP URI（尖括号内）
    public func fromUri() -> String? { SipMessage.extractUri(header("From")) }
    public func toUri() -> String? { SipMessage.extractUri(header("To")) }

    /// 从 From/To 头部提取 tag
    public func fromTag() -> String? { SipMessage.extractTag(header("From")) }
    public func toTag() -> String? { SipMessage.extractTag(header("To")) }

    // MARK: - 序列化

    public func toWire() -> String {
        var sb = ""
        if let method = method {
            sb += "\(method) \(requestUri ?? "") SIP/2.0\r\n"
        } else if let statusCode = statusCode {
            sb += "SIP/2.0 \(statusCode) \(reasonPhrase ?? "")\r\n"
        }
        for item in headerList {
            // 跳过自动计算的 Content-Length
            if SipMessage.canonical(item.name) == "content-length" { continue }
            sb += "\(item.name): \(item.value)\r\n"
        }
        let byteCount = body.data(using: .utf8)?.count ?? 0
        sb += "Content-Length: \(byteCount)\r\n"
        sb += "\r\n"
        if !body.isEmpty { sb += body }
        return sb
    }

    // MARK: - 解析 / 构造

    private static let compactNames: [String: String] = [
        "v": "Via", "f": "From", "t": "To", "i": "Call-ID",
        "m": "Contact", "l": "Content-Length", "c": "Content-Type",
        "k": "Supported", "s": "Subject", "e": "Content-Encoding",
        "o": "Event", "u": "Allow-Events", "x": "Session-Expires"
    ]

    private static func canonical(_ name: String) -> String {
        let n = name.trimmingCharacters(in: .whitespaces)
        if let full = compactNames[n.lowercased()] {
            return full.lowercased()
        }
        return n.lowercased()
    }

    private static func extractUri(_ header: String?) -> String? {
        guard let header = header else { return nil }
        if let lt = header.firstIndex(of: "<"),
           let gt = header.firstIndex(of: ">"),
           lt < gt {
            return String(header[header.index(after: lt)..<gt]).trimmingCharacters(in: .whitespaces)
        }
        if let firstPart = header.split(separator: ";", maxSplits: 1).first {
            return String(firstPart).trimmingCharacters(in: .whitespaces)
        }
        return nil
    }

    private static func extractTag(_ header: String?) -> String? {
        guard let header = header else { return nil }
        guard let range = header.range(of: ";tag=([^;>\\s]+)", options: .regularExpression) else {
            return nil
        }
        let matched = String(header[range])
        return String(matched.dropFirst(";tag=".count))
    }

    /// 解析线上消息。失败返回 nil。
    public static func parse(_ raw: String) -> SipMessage? {
        guard !raw.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return nil }
        let normalized = raw.replacingOccurrences(of: "\r\n", with: "\n")

        var headPart = normalized
        var bodyPart = ""
        if let sep = normalized.range(of: "\n\n") {
            headPart = String(normalized[..<sep.lowerBound])
            bodyPart = String(normalized[sep.upperBound...])
        }

        let lines = headPart.components(separatedBy: "\n")
        guard let firstLine = lines.first, !firstLine.isEmpty else { return nil }

        let msg = SipMessage()
        let first = firstLine.trimmingCharacters(in: .whitespaces)
        if first.hasPrefix("SIP/2.0") {
            // 状态行
            let parts = first.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: false)
            guard parts.count >= 2, let code = Int(parts[1]) else { return nil }
            msg.statusCode = code
            msg.reasonPhrase = parts.count > 2 ? String(parts[2]) : ""
        } else {
            // 请求行
            let parts = first.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: false)
            guard parts.count >= 3,
                  String(parts[2]).uppercased() == "SIP/2.0" else { return nil }
            msg.method = String(parts[0]).uppercased()
            msg.requestUri = String(parts[1])
        }

        // 头部（支持折行：以空格/制表符开头的行是上一行的续行）
        for line in lines.dropFirst() {
            if line.isEmpty { break }
            if line.hasPrefix(" ") || line.hasPrefix("\t") {
                if let last = msg.headerList.last {
                    let merged = last.value + " " + line.trimmingCharacters(in: .whitespaces)
                    msg.headerList[msg.headerList.count - 1] = (name: last.name, value: merged)
                }
                continue
            }
            guard let colon = line.firstIndex(of: ":") else { continue }
            let name = String(line[..<colon]).trimmingCharacters(in: .whitespaces)
            guard !name.isEmpty else { continue }
            let value = String(line[line.index(after: colon)...]).trimmingCharacters(in: .whitespaces)
            msg.addHeader(name, value)
        }

        msg.body = bodyPart
        // 用头部声明的 Content-Length 截断（避免分帧残留）
        if let cl = msg.header("Content-Length").flatMap({ Int($0.trimmingCharacters(in: .whitespaces)) }),
           cl >= 0, cl <= msg.body.count {
            msg.body = String(msg.body.prefix(cl))
        }
        return msg
    }

    /// 根据请求构造响应，规则与 Android `SipMessage.buildResponse` 一致。
    public static func buildResponse(
        request: SipMessage,
        code: Int,
        reason: String,
        toTag: String? = nil
    ) -> SipMessage {
        let r = SipMessage()
        r.statusCode = code
        r.reasonPhrase = reason
        // 逐条复制关键头部（Via 保序）
        for via in request.headers("Via") {
            r.addHeader("Via", via)
        }
        if let from = request.header("From") {
            r.addHeader("From", from)
        }
        if let to = request.header("To") {
            // To tag 必须在同一请求的所有响应中保持一致；
            // 请求 To 已带 tag 时原样回传，否则补一个稳定 tag。
            if extractTag(to) != nil {
                r.addHeader("To", to)
            } else {
                r.addHeader("To", "\(to);tag=\(toTag ?? SipUtils.newTag())")
            }
        }
        if let callId = request.header("Call-ID") {
            r.addHeader("Call-ID", callId)
        }
        if let cseq = request.header("CSeq") {
            r.addHeader("CSeq", cseq)
        }
        return r
    }
}
