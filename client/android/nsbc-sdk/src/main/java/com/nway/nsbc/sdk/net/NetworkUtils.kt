package com.nway.nsbc.sdk.net

import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.Collections

/**
 * 本机网络地址探测（用于 SIP Contact / Via 的 host 部分）。
 *
 * 在 NAT 场景下这些地址对服务端不可达，但 NSBC 的 WebSocket 注册
 * 以连接句柄为准（`loc_binding_t.ws_conn`），host 部分仅作展示，不影响路由。
 */
object NetworkUtils {

    /** 返回第一个非回环 IPv4 地址；找不到返回 `0.0.0.0`。 */
    fun localIpv4(): String {
        return runCatching {
            Collections.list(NetworkInterface.getNetworkInterfaces())
                .asSequence()
                .filter { it.isUp && !it.isLoopback }
                .flatMap { Collections.list(it.inetAddresses).asSequence() }
                .filterIsInstance<Inet4Address>()
                .firstOrNull()
                ?.hostAddress
        }.getOrNull() ?: "0.0.0.0"
    }
}
