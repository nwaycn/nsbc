import SwiftUI
import WebRTC

/// 包装 WebRTC 的 `RTCMTLVideoView`（Metal 渲染）。
///
/// 同一个 `RTCMTLVideoView` 实例需要在多次 body 计算之间复用，
/// 因此由调用方创建并通过 `view` 传入。
struct VideoView: UIViewRepresentable {
    let view: RTCMTLVideoView
    var mirror: Bool = false

    func makeUIView(context: Context) -> RTCMTLVideoView {
        view.videoContentMode = .scaleAspectFill
        view.clipsToBounds = true
        return view
    }

    func updateUIView(_ uiView: RTCMTLVideoView, context: Context) {
        uiView.videoContentMode = .scaleAspectFill
        uiView.clipsToBounds = true
        uiView.transform = mirror ? CGAffineTransform(scaleX: -1, y: 1) : .identity
    }
}
