import SwiftUI
import NsbcSDK

/// 来电浮层：接听 / 拒接。
struct IncomingCallView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        ZStack {
            NsbcTheme.background.opacity(0.97).ignoresSafeArea()

            VStack(spacing: 10) {
                Text("来电")
                    .font(.system(size: 13, weight: .medium))
                    .tracking(4)
                    .foregroundColor(Color(red: 0x93 / 255.0, green: 0xC5 / 255.0, blue: 0xFD / 255.0))

                ZStack {
                    Circle()
                        .fill(NsbcTheme.keypad)
                        .frame(width: 88, height: 88)
                    Text(initialOf(model.callDisplayName))
                        .font(.system(size: 30, weight: .bold))
                        .foregroundColor(Color(red: 0xE2 / 255.0, green: 0xE8 / 255.0, blue: 0xF0 / 255.0))
                }
                .padding(.top, 18)

                Text(model.callDisplayName)
                    .font(.system(size: 24, weight: .bold))
                    .foregroundColor(NsbcTheme.textPrimary)
                    .padding(.top, 6)

                Text(model.remoteNumber)
                    .font(.system(size: 13))
                    .foregroundColor(NsbcTheme.textSecondary)

                Text("等待接听…")
                    .font(.system(size: 12))
                    .foregroundColor(NsbcTheme.textDim)
                    .padding(.top, 2)

                HStack(spacing: 40) {
                    Button(action: { model.rejectCall() }) {
                        VStack(spacing: 6) {
                            Image(systemName: "phone.down.fill")
                                .font(.system(size: 24))
                                .foregroundColor(.white)
                            Text("拒接")
                                .font(.system(size: 12))
                                .foregroundColor(.white)
                        }
                        .frame(width: 68, height: 68)
                        .background(NsbcTheme.red)
                        .cornerRadius(34)
                    }
                    .buttonStyle(PlainButtonStyle())

                    Button(action: { model.answerCall() }) {
                        VStack(spacing: 6) {
                            Image(systemName: "phone.fill")
                                .font(.system(size: 24))
                                .foregroundColor(.white)
                            Text("接听")
                                .font(.system(size: 12))
                                .foregroundColor(.white)
                        }
                        .frame(width: 68, height: 68)
                        .background(NsbcTheme.green)
                        .cornerRadius(34)
                    }
                    .buttonStyle(PlainButtonStyle())
                }
                .padding(.top, 40)
            }
            .padding(.horizontal, 32)
        }
    }

    private func initialOf(_ text: String) -> String {
        guard let first = text.first else { return "?" }
        return String(first).uppercased()
    }
}
