import Foundation
import CommonCrypto
import Darwin

/// SIP 工具函数。
public enum SipUtils {

    private static let lock = NSLock()
    private static var branchCounter: Int64 = 0

    /// 生成 `z9hG4bK-` 前缀的 RFC 3261 branch。
    public static func newBranch() -> String {
        lock.lock()
        branchCounter += 1
        let seq = branchCounter
        lock.unlock()
        let nano = Int64(DispatchTime.now().uptimeNanoseconds)
        return "z9hG4bK-\(nano)-\(seq)"
    }

    /// 生成 tag（不含非法字符）。
    public static func newTag() -> String {
        let raw = UUID().uuidString.replacingOccurrences(of: "-", with: "")
        return String(raw.prefix(12))
    }

    /// 生成 Call-ID。
    public static func newCallId(host: String) -> String {
        let nano = DispatchTime.now().uptimeNanoseconds
        return "\(UUID().uuidString).\(nano)@\(host)"
    }

    /// 生成 SDP 用的会话 id。
    public static func newSessionId() -> Int64 {
        return Int64(Date().timeIntervalSince1970)
    }

    /// 提取 URI 中的 user 部分。
    public static func userOf(_ uri: String?) -> String? {
        guard let uri = uri else { return nil }
        var s = uri
        if let range = s.range(of: "sip:") {
            s = String(s[range.upperBound...])
        } else if let range = s.range(of: "sips:") {
            s = String(s[range.upperBound...])
        }
        let user = String(s.split(separator: "@", maxSplits: 1).first ?? "")
        return user.isEmpty ? nil : user
    }

    /// 提取 URI 中的 host 部分。
    public static func hostOf(_ uri: String?) -> String? {
        guard let uri = uri else { return nil }
        var s = uri
        if let range = s.range(of: "sip:") {
            s = String(s[range.upperBound...])
        } else if let range = s.range(of: "sips:") {
            s = String(s[range.upperBound...])
        }
        guard let at = s.firstIndex(of: "@") else { return nil }
        var host = String(s[s.index(after: at)...])
        if let colon = host.firstIndex(of: ":") { host = String(host[..<colon]) }
        if let semi = host.firstIndex(of: ";") { host = String(host[..<semi]) }
        return host.isEmpty ? nil : host
    }

    /// MD5 十六进制小写。
    public static func md5(_ input: String) -> String {
        guard let data = input.data(using: .utf8) else { return "" }
        var digest = [UInt8](repeating: 0, count: Int(CC_MD5_DIGEST_LENGTH))
        _ = data.withUnsafeBytes { bytes in
            CC_MD5(bytes.baseAddress, CC_LONG(data.count), &digest)
        }
        return digest.map { String(format: "%02x", $0) }.joined()
    }

    /// 返回第一个非回环 IPv4 地址；找不到返回 `0.0.0.0`。
    /// 仅用于 SIP Via / Contact 的 host 展示，NSBC 以 WebSocket 连接句柄为准。
    public static func localIPv4() -> String {
        var address: String?
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let first = ifaddr else { return "0.0.0.0" }
        defer { freeifaddrs(ifaddr) }
        var ptr: UnsafeMutablePointer<ifaddrs>? = first
        while let current = ptr {
            let interface = current.pointee
            let flags = Int32(interface.ifa_flags)
            let isUp = (flags & IFF_UP) != 0
            let isLoopback = (flags & IFF_LOOPBACK) != 0
            if isUp && !isLoopback,
               let addr = interface.ifa_addr,
               addr.pointee.sa_family == UInt8(AF_INET) {
                var hostname = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                let len = socklen_t(addr.pointee.sa_len)
                if getnameinfo(addr, len, &hostname, socklen_t(hostname.count), nil, 0, NI_NUMERICHOST) == 0 {
                    let ip = hostname.withUnsafeBufferPointer { buffer -> String in
                        guard let base = buffer.baseAddress else { return "" }
                        return String(cString: base)
                    }
                    if !ip.hasPrefix("127.") {
                        address = ip
                        break
                    }
                }
            }
            ptr = current.pointee.ifa_next
        }
        return address ?? "0.0.0.0"
    }

    /// 简单 XML 转义（NOTIFY 等场景可选）。
    public static func xmlEscape(_ s: String) -> String {
        return s
            .replacingOccurrences(of: "&", with: "&amp;")
            .replacingOccurrences(of: "<", with: "&lt;")
            .replacingOccurrences(of: ">", with: "&gt;")
    }
}
