// swift-tools-version:5.9
import PackageDescription

// NSBC iOS SDK：SIP over WebSocket + WebRTC。
// WebRTC 使用 stasel 提供的官方二进制包，避免本地编译 WebRTC 源码。
let package = Package(
    name: "NsbcSDK",
    platforms: [
        .iOS(.v14)
    ],
    products: [
        .library(name: "NsbcSDK", targets: ["NsbcSDK"])
    ],
    dependencies: [
        // 可在生产环境改为确切版本/tag，避免跟随 master 漂移
        .package(url: "https://github.com/stasel/WebRTC", branch: "master")
    ],
    targets: [
        .target(
            name: "NsbcSDK",
            dependencies: [
                .product(name: "WebRTC", package: "WebRTC")
            ],
            path: "Sources/NsbcSDK"
        )
    ]
)
