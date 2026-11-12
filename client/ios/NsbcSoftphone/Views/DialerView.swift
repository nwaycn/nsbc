import SwiftUI
import NsbcSDK

/// 拨号盘：注册状态 + 账号 + 号码 + 3x4 键盘 + 最近通话。
struct DialerView: View {
    @EnvironmentObject private var model: AppModel

    private static let keys: [[String]] = [
        ["1", "2", "3"],
        ["4", "5", "6"],
        ["7", "8", "9"],
        ["*", "0", "#"]
    ]

    private static let dateFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "MM-dd HH:mm"
        return f
    }()

    var body: some View {
        VStack(spacing: 0) {
            header
            accountLine
            numberDisplay
            keypad
            actionRow
            recentsSection
        }
        .padding(.horizontal, 20)
        .padding(.top, 8)
        .padding(.bottom, 8)
        .background(NsbcTheme.background.ignoresSafeArea())
    }

    // MARK: - 顶部

    private var header: some View {
        HStack(spacing: 8) {
            Text("NSBC 软电话")
                .font(.system(size: 20, weight: .bold))
                .foregroundColor(NsbcTheme.textPrimary)

            Spacer(minLength: 4)

            Text(model.statusText)
                .font(.system(size: 12))
                .foregroundColor(.white)
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background(model.statusColor)
                .cornerRadius(12)

            Button(action: { model.showSettings = true }) {
                Text("设置")
                    .font(.system(size: 13))
                    .foregroundColor(Color(red: 0xE2 / 255.0, green: 0xE8 / 255.0, blue: 0xF0 / 255.0))
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .background(NsbcTheme.card)
                    .cornerRadius(10)
            }
        }
    }

    private var accountLine: some View {
        Text(model.accountSummary)
            .font(.system(size: 12))
            .foregroundColor(NsbcTheme.textDim)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.top, 6)
    }

    private var numberDisplay: some View {
        Text(model.dialed.isEmpty ? "输入号码或分机" : model.dialed)
            .font(.system(size: 30, weight: .bold, design: .rounded))
            .foregroundColor(model.dialed.isEmpty ? NsbcTheme.textDim : NsbcTheme.textPrimary)
            .lineLimit(1)
            .minimumScaleFactor(0.5)
            .frame(maxWidth: .infinity)
            .frame(height: 56)
    }

    // MARK: - 键盘

    private var keypad: some View {
        VStack(spacing: 10) {
            ForEach(0..<DialerView.keys.count, id: \.self) { row in
                HStack(spacing: 12) {
                    ForEach(DialerView.keys[row], id: \.self) { key in
                        Button(action: { model.appendDigit(key) }) {
                            Text(key)
                                .font(.system(size: 24, weight: .medium))
                                .foregroundColor(NsbcTheme.textPrimary)
                                .frame(maxWidth: .infinity)
                                .frame(height: 58)
                                .background(NsbcTheme.keypad)
                                .cornerRadius(14)
                        }
                        .buttonStyle(PlainButtonStyle())
                    }
                }
            }
        }
    }

    private var actionRow: some View {
        HStack(spacing: 16) {
            Button(action: { model.backspace() }) {
                Text("⌫")
                    .font(.system(size: 20))
                    .foregroundColor(NsbcTheme.textSecondary)
                    .frame(width: 56, height: 56)
                    .background(NsbcTheme.keypad)
                    .cornerRadius(28)
            }
            .buttonStyle(PlainButtonStyle())

            Button(action: { model.placeCall() }) {
                Text("拨号")
                    .font(.system(size: 16, weight: .bold))
                    .foregroundColor(.white)
                    .frame(maxWidth: .infinity)
                    .frame(height: 56)
                    .background(NsbcTheme.green)
                    .cornerRadius(28)
            }
            .buttonStyle(PlainButtonStyle())
        }
        .padding(.top, 14)
    }

    // MARK: - 最近通话

    private var recentsSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("最近通话")
                .font(.system(size: 12))
                .foregroundColor(NsbcTheme.textSecondary)
                .padding(.top, 18)

            if model.recents.isEmpty {
                Text("暂无通话记录")
                    .font(.system(size: 12))
                    .foregroundColor(Color(red: 0x47 / 255.0, green: 0x55 / 255.0, blue: 0x69 / 255.0))
                    .padding(.top, 8)
                Spacer(minLength: 0)
            } else {
                ScrollView(showsIndicators: false) {
                    VStack(alignment: .leading, spacing: 0) {
                        ForEach(model.recents.prefix(12)) { item in
                            Button(action: { model.dialed = item.number }) {
                                HStack(spacing: 8) {
                                    Text(item.outgoing ? "↗" : "↙")
                                        .font(.system(size: 13))
                                        .foregroundColor(NsbcTheme.textSecondary)
                                    Text(item.number)
                                        .font(.system(size: 13))
                                        .foregroundColor(NsbcTheme.textSecondary)
                                    Spacer()
                                    Text(DialerView.dateFormatter.string(
                                        from: Date(timeIntervalSince1970: item.ts)))
                                        .font(.system(size: 12))
                                        .foregroundColor(NsbcTheme.textDim)
                                    Text(DialerView.formatDuration(item.duration))
                                        .font(.system(size: 12))
                                        .foregroundColor(NsbcTheme.textDim)
                                }
                                .padding(.vertical, 10)
                            }
                            .buttonStyle(PlainButtonStyle())
                        }
                    }
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }

    private static func formatDuration(_ seconds: Int64) -> String {
        return String(format: "%02lld:%02lld", seconds / 60, seconds % 60)
    }
}
