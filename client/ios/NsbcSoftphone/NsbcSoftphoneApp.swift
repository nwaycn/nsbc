import SwiftUI

/// 与 Android Demo 对齐的深色配色。
enum NsbcTheme {
    static let background = Color(red: 0x0B / 255.0, green: 0x12 / 255.0, blue: 0x20 / 255.0)
    static let card = Color(red: 0x11 / 255.0, green: 0x1C / 255.0, blue: 0x33 / 255.0)
    static let keypad = Color(red: 0x1E / 255.0, green: 0x29 / 255.0, blue: 0x3B / 255.0)
    static let green = Color(red: 0x16 / 255.0, green: 0xA3 / 255.0, blue: 0x4A / 255.0)
    static let red = Color(red: 0xDC / 255.0, green: 0x26 / 255.0, blue: 0x26 / 255.0)
    static let accent = Color(red: 0x38 / 255.0, green: 0xBD / 255.0, blue: 0xF8 / 255.0)
    static let textPrimary = Color(red: 0xF1 / 255.0, green: 0xF5 / 255.0, blue: 0xF9 / 255.0)
    static let textSecondary = Color(red: 0x94 / 255.0, green: 0xA3 / 255.0, blue: 0xB8 / 255.0)
    static let textDim = Color(red: 0x64 / 255.0, green: 0x74 / 255.0, blue: 0x8B / 255.0)
    static let chipGray = Color(red: 0x33 / 255.0, green: 0x41 / 255.0, blue: 0x55 / 255.0)
    static let chipAmber = Color(red: 0xB4 / 255.0, green: 0x53 / 255.0, blue: 0x09 / 255.0)
    static let chipGreen = Color(red: 0x15 / 255.0, green: 0x80 / 255.0, blue: 0x3D / 255.0)
    static let chipRed = Color(red: 0xB9 / 255.0, green: 0x1C / 255.0, blue: 0x1C / 255.0)
    static let border = Color.white.opacity(0.08)
}

@main
struct NsbcSoftphoneApp: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(model)
                .preferredColorScheme(.dark)
        }
    }
}

/// 根视图：拨号 / 设置 / 通话中 / 来电浮层。
struct RootView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        ZStack {
            NsbcTheme.background.ignoresSafeArea()

            if model.showSettings {
                SettingsView()
            } else {
                DialerView()
            }

            if model.showIncoming, model.currentCall != nil {
                IncomingCallView()
                    .transition(.opacity)
                    .zIndex(2)
            }
        }
        .fullScreenCover(isPresented: $model.showCall) {
            CallView()
                .environmentObject(model)
        }
        .alert(item: $model.alert) { message in
            Alert(title: Text(message.text), dismissButton: .default(Text("好")))
        }
    }
}
