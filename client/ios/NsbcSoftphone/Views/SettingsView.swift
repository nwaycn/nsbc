import SwiftUI
import UIKit
import NsbcSDK

/// 设置：账号 / 网络 / 媒体 / 高级 四组。
struct SettingsView: View {
    @EnvironmentObject private var model: AppModel

    @State private var draft = NsbcConfig()
    @State private var portText = "5062"
    @State private var widthText = "1280"
    @State private var heightText = "720"
    @State private var fpsText = "30"

    init() {
        // 让 Form 背景透出统一深色
        UITableView.appearance().backgroundColor = .clear
        UITableViewCell.appearance().backgroundColor = .clear
    }

    var body: some View {
        VStack(spacing: 0) {
            header

            Form {
                accountSection
                networkSection
                mediaSection
                advancedSection

                if let problems = model.problemsText, !problems.isEmpty {
                    Section {
                        Text(problems)
                            .font(.system(size: 12))
                            .foregroundColor(Color(red: 0xF8 / 255.0, green: 0x71 / 255.0, blue: 0x71 / 255.0))
                    }
                    .listRowBackground(NsbcTheme.card)
                }

                actionsSection
            }
            .accentColor(NsbcTheme.green)
        }
        .background(NsbcTheme.background.ignoresSafeArea())
        .onAppear { syncDraft() }
    }

    // MARK: - 顶部

    private var header: some View {
        HStack(spacing: 12) {
            Button(action: { model.showSettings = false }) {
                Text("返回")
                    .font(.system(size: 13))
                    .foregroundColor(Color(red: 0xE2 / 255.0, green: 0xE8 / 255.0, blue: 0xF0 / 255.0))
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .background(NsbcTheme.card)
                    .cornerRadius(10)
            }

            Text("设置")
                .font(.system(size: 20, weight: .bold))
                .foregroundColor(NsbcTheme.textPrimary)

            Spacer()

            Text(model.statusText)
                .font(.system(size: 12))
                .foregroundColor(.white)
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background(model.statusColor)
                .cornerRadius(12)
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 8)
    }

    // MARK: - 分组

    private var accountSection: some View {
        Section(header: sectionHeader("账号")) {
            field("服务器地址", text: $draft.server)
            field("WSS 端口", text: $portText, keyboard: .numberPad)
            field("域 / Realm", text: $draft.realm)
            field("用户名 / 分机", text: $draft.username)
            secureField("密码", text: $draft.password)
            field("显示名（可选）", text: $draft.displayName)
        }
        .listRowBackground(NsbcTheme.card)
    }

    private var networkSection: some View {
        Section(header: sectionHeader("网络")) {
            Toggle("使用 TLS（WSS）", isOn: $draft.useTls)
            field("WebSocket 路径", text: $draft.wsPath)
            field("STUN 服务器", text: stringBinding(\.stunServer), keyboard: .URL)
            field("TURN 服务器", text: stringBinding(\.turnServer), keyboard: .URL)
            field("TURN 用户名", text: stringBinding(\.turnUsername))
            secureField("TURN 密码", text: stringBinding(\.turnCredential))

            Picker("ICE 策略", selection: $draft.icePolicy) {
                Text("全部（直连 + 中继）").tag(IceTransportPolicy.all)
                Text("仅中继（TURN）").tag(IceTransportPolicy.relay)
            }
        }
        .listRowBackground(NsbcTheme.card)
    }

    private var mediaSection: some View {
        Section(header: sectionHeader("媒体")) {
            Toggle("启用视频", isOn: $draft.videoEnabled)
            field("视频宽", text: $widthText, keyboard: .numberPad)
            field("视频高", text: $heightText, keyboard: .numberPad)
            field("帧率", text: $fpsText, keyboard: .numberPad)
            Toggle("本地预览镜像", isOn: $draft.mirrorLocalPreview)
        }
        .listRowBackground(NsbcTheme.card)
    }

    private var advancedSection: some View {
        Section(header: sectionHeader("高级")) {
            Toggle("断线自动重连", isOn: $draft.autoReconnect)
            Toggle("允许自签名证书", isOn: $draft.allowSelfSignedCert)

            Picker("日志级别", selection: $draft.logLevel) {
                ForEach(LogLevel.allCases, id: \.self) { level in
                    Text(level.rawValue).tag(level)
                }
            }

            if let detail = model.registrationDetail, !detail.isEmpty {
                Text(detail)
                    .font(.system(size: 12))
                    .foregroundColor(NsbcTheme.textSecondary)
            }
        }
        .listRowBackground(NsbcTheme.card)
    }

    private var actionsSection: some View {
        Section {
            Button(action: save) {
                Text("保存并连接")
                    .font(.system(size: 15, weight: .bold))
                    .foregroundColor(.white)
                    .frame(maxWidth: .infinity)
                    .frame(height: 48)
            }
            .listRowBackground(NsbcTheme.green)

            Button(action: { model.disconnect() }) {
                Text("断开连接")
                    .font(.system(size: 14))
                    .foregroundColor(Color(red: 0xE2 / 255.0, green: 0xE8 / 255.0, blue: 0xF0 / 255.0))
                    .frame(maxWidth: .infinity)
                    .frame(height: 44)
            }
            .listRowBackground(NsbcTheme.keypad)

            Button(action: {
                model.resetDefaults()
                syncDraft()
            }) {
                Text("恢复默认")
                    .font(.system(size: 13))
                    .foregroundColor(NsbcTheme.textDim)
                    .frame(maxWidth: .infinity)
                    .frame(height: 40)
            }
            .listRowBackground(Color.clear)
        }
    }

    // MARK: - 组件

    private func sectionHeader(_ title: String) -> some View {
        Text(title)
            .font(.system(size: 13, weight: .bold))
            .foregroundColor(NsbcTheme.accent)
    }

    private func field(_ title: String, text: Binding<String>, keyboard: UIKeyboardType = .default) -> some View {
        HStack(spacing: 10) {
            Text(title)
                .font(.system(size: 14))
                .foregroundColor(NsbcTheme.textSecondary)
                .frame(width: 110, alignment: .leading)
            TextField("", text: text)
                .font(.system(size: 14))
                .foregroundColor(NsbcTheme.textPrimary)
                .keyboardType(keyboard)
                .autocapitalization(.none)
                .disableAutocorrection(true)
        }
    }

    private func secureField(_ title: String, text: Binding<String>) -> some View {
        HStack(spacing: 10) {
            Text(title)
                .font(.system(size: 14))
                .foregroundColor(NsbcTheme.textSecondary)
                .frame(width: 110, alignment: .leading)
            SecureField("", text: text)
                .font(.system(size: 14))
                .foregroundColor(NsbcTheme.textPrimary)
        }
    }

    /// 可选字符串字段：空串转 nil（对齐 Android 的 ifBlank { null }）。
    private func stringBinding(_ keyPath: WritableKeyPath<NsbcConfig, String?>) -> Binding<String> {
        Binding(
            get: { draft[keyPath: keyPath] ?? "" },
            set: { draft[keyPath: keyPath] = $0.isEmpty ? nil : $0 }
        )
    }

    // MARK: - 动作

    private func syncDraft() {
        draft = model.config
        portText = String(model.config.wssPort)
        widthText = String(model.config.videoWidth)
        heightText = String(model.config.videoHeight)
        fpsText = String(model.config.videoFps)
    }

    private func save() {
        draft.wssPort = Int(portText) ?? 5062
        draft.videoWidth = Int(widthText) ?? 1280
        draft.videoHeight = Int(heightText) ?? 720
        draft.videoFps = Int(fpsText) ?? 30
        model.config = draft
        model.saveAndConnect()
    }
}
