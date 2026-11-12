import SwiftUI
import WebRTC
import NsbcSDK

/// 渲染器容器：保证同一通呼叫内 RTCMTLVideoView 实例稳定。
private final class CallRenderers: ObservableObject {
    let local = RTCMTLVideoView(frame: .zero)
    let remote = RTCMTLVideoView(frame: .zero)
}

/// 通话中：远端视频全屏 + 可拖动本地小窗 + 计时 + 静音/免提/视频/切换/挂断。
struct CallView: View {
    @EnvironmentObject private var model: AppModel

    @StateObject private var renderers = CallRenderers()
    @State private var pipOffset: CGSize = .zero
    @State private var baseOffset: CGSize = .zero

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .topTrailing) {
                Color.black.ignoresSafeArea()

                // 始终保留远端渲染器视图，避免开关视频时反复挂载/卸载同一个 RTCMTLVideoView
                VideoView(view: renderers.remote)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .ignoresSafeArea()
                    .opacity(model.videoOn ? 1 : 0)

                if !model.videoOn {
                    avatar
                }

                topInfo
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)

                localPreview(geo: geo)
                    .opacity(model.videoOn ? 1 : 0)
                    .allowsHitTesting(model.videoOn)

                bottomBar
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
            }
            .onAppear {
                model.attachRenderers(local: renderers.local, remote: renderers.remote)
            }
        }
        .background(Color.black.ignoresSafeArea())
    }

    // MARK: - 顶部信息

    private var topInfo: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(model.callDisplayName)
                .font(.system(size: 20, weight: .bold))
                .foregroundColor(.white)
                .shadow(color: Color.black.opacity(0.6), radius: 4, x: 0, y: 1)

            Text(model.callSubtitle)
                .font(.system(size: 13))
                .foregroundColor(Color(red: 0xCB / 255.0, green: 0xD5 / 255.0, blue: 0xE1 / 255.0))
                .shadow(color: Color.black.opacity(0.6), radius: 4, x: 0, y: 1)

            Text(model.callTimeText)
                .font(.system(size: 14))
                .foregroundColor(Color(red: 0x93 / 255.0, green: 0xC5 / 255.0, blue: 0xFD / 255.0))
                .shadow(color: Color.black.opacity(0.6), radius: 4, x: 0, y: 1)
                .padding(.top, 4)
        }
        .padding(.leading, 20)
        .padding(.top, 28)
    }

    private var avatar: some View {
        ZStack {
            Circle()
                .fill(NsbcTheme.keypad)
                .frame(width: 96, height: 96)
            Text(initialOf(model.callDisplayName))
                .font(.system(size: 30, weight: .bold))
                .foregroundColor(Color(red: 0xE2 / 255.0, green: 0xE8 / 255.0, blue: 0xF0 / 255.0))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: - 本地小窗

    private func localPreview(geo: GeometryProxy) -> some View {
        VideoView(view: renderers.local, mirror: model.config.mirrorLocalPreview)
            .frame(width: 104, height: 148)
            .background(Color.black)
            .cornerRadius(12)
            .overlay(
                RoundedRectangle(cornerRadius: 12)
                    .stroke(Color.white.opacity(0.16), lineWidth: 1)
            )
            .offset(x: pipOffset.width, y: pipOffset.height)
            .padding(.top, 96)
            .padding(.trailing, 16)
            .gesture(dragGesture(geo: geo))
    }

    private func dragGesture(geo: GeometryProxy) -> some Gesture {
        DragGesture()
            .onChanged { value in
                let maxX = max(geo.size.width - 104 - 32, 0)
                let maxY = max(geo.size.height - 148 - 220, 0)
                let x = min(0, max(-maxX, baseOffset.width + value.translation.width))
                let y = min(maxY, max(-80, baseOffset.height + value.translation.height))
                pipOffset = CGSize(width: x, height: y)
            }
            .onEnded { _ in
                baseOffset = pipOffset
            }
    }

    // MARK: - 底部操作

    private var bottomBar: some View {
        HStack(spacing: 8) {
            callAction(
                title: model.muted ? "已静音" : "静音",
                systemImage: model.muted ? "mic.slash.fill" : "mic.fill",
                active: model.muted
            ) { model.toggleMute() }

            callAction(
                title: model.speakerOn ? "免提开" : "免提",
                systemImage: model.speakerOn ? "speaker.wave.2.fill" : "speaker.wave.1.fill",
                active: model.speakerOn
            ) { model.toggleSpeaker() }

            callAction(
                title: model.videoOn ? "关视频" : "开视频",
                systemImage: model.videoOn ? "video.fill" : "video.slash.fill",
                active: model.videoOn
            ) { model.toggleVideo() }

            callAction(
                title: "切换",
                systemImage: "camera.rotate.fill",
                active: false
            ) { model.switchCamera() }

            Button(action: { model.hangup() }) {
                VStack(spacing: 4) {
                    Image(systemName: "phone.down.fill")
                        .font(.system(size: 22))
                        .foregroundColor(.white)
                    Text("挂断")
                        .font(.system(size: 11))
                        .foregroundColor(.white)
                }
                .frame(width: 56, height: 60)
                .background(NsbcTheme.red)
                .cornerRadius(28)
            }
            .buttonStyle(PlainButtonStyle())
        }
        .padding(.horizontal, 12)
        .padding(.bottom, 24)
    }

    private func callAction(
        title: String,
        systemImage: String,
        active: Bool,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            VStack(spacing: 4) {
                Image(systemName: systemImage)
                    .font(.system(size: 20))
                    .foregroundColor(active ? NsbcTheme.green : NsbcTheme.textPrimary)
                Text(title)
                    .font(.system(size: 11))
                    .foregroundColor(active ? NsbcTheme.green : NsbcTheme.textSecondary)
            }
            .frame(width: 62, height: 60)
            .background(NsbcTheme.keypad.opacity(active ? 0.9 : 0.8))
            .cornerRadius(28)
        }
        .buttonStyle(PlainButtonStyle())
    }

    private func initialOf(_ text: String) -> String {
        guard let first = text.first else { return "?" }
        return String(first).uppercased()
    }
}
