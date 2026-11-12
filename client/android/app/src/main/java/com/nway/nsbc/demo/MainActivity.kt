package com.nway.nsbc.demo

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.media.AudioManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.Spinner
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import com.nway.nsbc.sdk.CallState
import com.nway.nsbc.sdk.IceTransportPolicy
import com.nway.nsbc.sdk.LogLevel
import com.nway.nsbc.sdk.NsbcCall
import com.nway.nsbc.sdk.NsbcClient
import com.nway.nsbc.sdk.NsbcConfig
import com.nway.nsbc.sdk.NsbcError
import com.nway.nsbc.sdk.RegistrationState
import org.json.JSONArray
import org.json.JSONObject
import org.webrtc.SurfaceViewRenderer
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * NSBC 软电话 Demo。
 *
 * 界面结构参考主流软电话（Sipnetic / Zoiper / Linphone）：
 *  - 拨号盘：注册状态 + 号码输入 + 3x4 键盘 + 最近通话
 *  - 设置：账号 / 网络 / 媒体 / 高级 四组，一屏完成配置
 *  - 通话中：远端视频全屏 + 可拖动本地小窗 + 计时 + 静音/免提/视频/切换/挂断
 *  - 来电：全屏浮层，接听 / 拒接
 */
class MainActivity : Activity(), NsbcClient.Listener {

    // ---------------------------------------------------------------- 常量

    private val permissions = arrayOf(
        Manifest.permission.CAMERA,
        Manifest.permission.RECORD_AUDIO,
        Manifest.permission.MODIFY_AUDIO_SETTINGS
    )

    private val iceLabels = arrayOf("全部（直连 + 中继）", "仅中继（TURN）")
    private val logLevels = arrayOf(
        LogLevel.NONE, LogLevel.ERROR, LogLevel.WARN,
        LogLevel.INFO, LogLevel.DEBUG, LogLevel.VERBOSE
    )

    // ---------------------------------------------------------------- 状态

    private lateinit var prefs: SharedPreferences
    private var client: NsbcClient? = null
    private var currentConfig: NsbcConfig? = null
    private var currentCall: NsbcCall? = null
    private var pendingConnect = false

    private var regState = RegistrationState.UNREGISTERED
    private var dialed = ""
    private var muted = false
    private var speakerOn = false
    private var videoOn = true

    private val handler = Handler(Looper.getMainLooper())

    // ---------------------------------------------------------------- 视图

    private lateinit var screenDialer: View
    private lateinit var screenSettings: View
    private lateinit var screenCall: View
    private lateinit var incomingOverlay: View

    private lateinit var statusChip: TextView
    private lateinit var accountText: TextView
    private lateinit var dialedText: TextView
    private lateinit var recentList: LinearLayout
    private lateinit var recentEmpty: TextView

    private lateinit var serverInput: EditText
    private lateinit var portInput: EditText
    private lateinit var realmInput: EditText
    private lateinit var userInput: EditText
    private lateinit var passInput: EditText
    private lateinit var displayInput: EditText
    private lateinit var tlsSwitch: Switch
    private lateinit var pathInput: EditText
    private lateinit var stunInput: EditText
    private lateinit var turnInput: EditText
    private lateinit var turnUserInput: EditText
    private lateinit var turnCredInput: EditText
    private lateinit var iceSpinner: Spinner
    private lateinit var videoSwitch: Switch
    private lateinit var widthInput: EditText
    private lateinit var heightInput: EditText
    private lateinit var fpsInput: EditText
    private lateinit var mirrorSwitch: Switch
    private lateinit var reconnectSwitch: Switch
    private lateinit var selfSignedSwitch: Switch
    private lateinit var logSpinner: Spinner
    private lateinit var problemsText: TextView

    private lateinit var remoteView: SurfaceViewRenderer
    private lateinit var localView: SurfaceViewRenderer
    private lateinit var localContainer: FrameLayout
    private lateinit var callAvatar: TextView
    private lateinit var callName: TextView
    private lateinit var callSubtitle: TextView
    private lateinit var callTimer: TextView
    private lateinit var muteBtn: Button
    private lateinit var speakerBtn: Button
    private lateinit var videoBtn: Button
    private lateinit var switchBtn: Button
    private lateinit var hangupBtn: Button

    private lateinit var incomingAvatar: TextView
    private lateinit var incomingName: TextView
    private lateinit var incomingUri: TextView

    // ============================================================== 生命周期

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        prefs = getSharedPreferences("nsbc_demo", MODE_PRIVATE)

        bindViews()
        wireActions()
        bindKeypad()
        restoreConfig()
        applyVideoUi()
        setRegistrationUi(RegistrationState.UNREGISTERED, null)
        updateAccountText()
        renderRecents()
    }

    override fun onDestroy() {
        stopTimer()
        runCatching { client?.close() }
        client = null
        runCatching { localView.release() }
        runCatching { remoteView.release() }
        resetAudioRoute()
        super.onDestroy()
    }

    @Suppress("DEPRECATION")
    override fun onBackPressed() {
        if (screenSettings.visibility == View.VISIBLE) {
            showDialer()
        } else if (screenCall.visibility == View.VISIBLE) {
            // 通话中不允许物理返回直接退出界面
            return
        } else {
            super.onBackPressed()
        }
    }

    private fun bindViews() {
        screenDialer = findViewById(R.id.screenDialer)
        screenSettings = findViewById(R.id.screenSettings)
        screenCall = findViewById(R.id.screenCall)
        incomingOverlay = findViewById(R.id.incomingOverlay)

        statusChip = findViewById(R.id.statusChip)
        accountText = findViewById(R.id.accountText)
        dialedText = findViewById(R.id.dialedText)
        recentList = findViewById(R.id.recentList)
        recentEmpty = findViewById(R.id.recentEmpty)

        serverInput = findViewById(R.id.serverInput)
        portInput = findViewById(R.id.portInput)
        realmInput = findViewById(R.id.realmInput)
        userInput = findViewById(R.id.userInput)
        passInput = findViewById(R.id.passInput)
        displayInput = findViewById(R.id.displayInput)
        tlsSwitch = findViewById(R.id.tlsSwitch)
        pathInput = findViewById(R.id.pathInput)
        stunInput = findViewById(R.id.stunInput)
        turnInput = findViewById(R.id.turnInput)
        turnUserInput = findViewById(R.id.turnUserInput)
        turnCredInput = findViewById(R.id.turnCredInput)
        iceSpinner = findViewById(R.id.iceSpinner)
        videoSwitch = findViewById(R.id.videoSwitch)
        widthInput = findViewById(R.id.widthInput)
        heightInput = findViewById(R.id.heightInput)
        fpsInput = findViewById(R.id.fpsInput)
        mirrorSwitch = findViewById(R.id.mirrorSwitch)
        reconnectSwitch = findViewById(R.id.reconnectSwitch)
        selfSignedSwitch = findViewById(R.id.selfSignedSwitch)
        logSpinner = findViewById(R.id.logSpinner)
        problemsText = findViewById(R.id.problemsText)

        remoteView = findViewById(R.id.remoteView)
        localView = findViewById(R.id.localView)
        localContainer = findViewById(R.id.localContainer)
        callAvatar = findViewById(R.id.callAvatar)
        callName = findViewById(R.id.callName)
        callSubtitle = findViewById(R.id.callSubtitle)
        callTimer = findViewById(R.id.callTimer)
        muteBtn = findViewById(R.id.muteBtn)
        speakerBtn = findViewById(R.id.speakerBtn)
        videoBtn = findViewById(R.id.videoBtn)
        switchBtn = findViewById(R.id.switchBtn)
        hangupBtn = findViewById(R.id.hangupBtn)

        incomingAvatar = findViewById(R.id.incomingAvatar)
        incomingName = findViewById(R.id.incomingName)
        incomingUri = findViewById(R.id.incomingUri)

        // Spinner 适配暗色
        iceSpinner.adapter = spinnerAdapter(iceLabels.toList())
        logSpinner.adapter = spinnerAdapter(logLevels.map { it.name })

        // 本地小窗可拖动
        localContainer.isClickable = true
        enablePipDrag()
    }

    private fun spinnerAdapter(items: List<String>) =
        ArrayAdapter(this, android.R.layout.simple_spinner_item, items).apply {
            setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        }

    private fun wireActions() {
        findViewById<Button>(R.id.settingsBtn).setOnClickListener { showSettings() }
        findViewById<Button>(R.id.settingsBackBtn).setOnClickListener { showDialer() }
        findViewById<Button>(R.id.backspaceBtn).setOnClickListener { backspace() }
        findViewById<Button>(R.id.callBtn).setOnClickListener { placeCall() }
        findViewById<Button>(R.id.saveConnectBtn).setOnClickListener { saveAndConnect() }
        findViewById<Button>(R.id.disconnectBtn).setOnClickListener { disconnect() }
        findViewById<Button>(R.id.resetBtn).setOnClickListener { resetToDefaults() }

        muteBtn.setOnClickListener {
            muted = !muted
            val call = currentCall
            if (call != null) call.setMuted(muted) else client?.setMuted(muted)
            muteBtn.text = getString(if (muted) R.string.btn_unmute else R.string.btn_mute)
        }
        speakerBtn.setOnClickListener { setSpeaker(!speakerOn) }
        videoBtn.setOnClickListener {
            videoOn = !videoOn
            val call = currentCall
            if (call != null) call.setVideoEnabled(videoOn) else client?.setVideoEnabled(videoOn)
            applyVideoUi()
        }
        switchBtn.setOnClickListener {
            val call = currentCall
            if (call != null) call.switchCamera() else client?.switchCamera()
        }
        hangupBtn.setOnClickListener { currentCall?.hangup() }

        findViewById<Button>(R.id.answerBtn).setOnClickListener {
            val call = currentCall ?: return@setOnClickListener
            call.answer()
            incomingOverlay.visibility = View.GONE
            enterCallUi(call)
        }
        findViewById<Button>(R.id.rejectBtn).setOnClickListener {
            currentCall?.reject()
            currentCall = null
            incomingOverlay.visibility = View.GONE
            showDialer()
        }
    }

    private fun bindKeypad() {
        val keys = listOf(
            R.id.key1 to "1", R.id.key2 to "2", R.id.key3 to "3",
            R.id.key4 to "4", R.id.key5 to "5", R.id.key6 to "6",
            R.id.key7 to "7", R.id.key8 to "8", R.id.key9 to "9",
            R.id.keyStar to "*", R.id.key0 to "0", R.id.keyHash to "#"
        )
        keys.forEach { (id, label) ->
            findViewById<Button>(id).apply {
                text = label
                setOnClickListener { appendDigit(label) }
            }
        }
    }

    // ============================================================== 屏幕切换

    private fun showDialer() {
        screenDialer.visibility = View.VISIBLE
        screenSettings.visibility = View.GONE
        screenCall.visibility = View.GONE
        incomingOverlay.visibility = View.GONE
        updateDialedText()
    }

    private fun showSettings() {
        screenDialer.visibility = View.GONE
        screenSettings.visibility = View.VISIBLE
        screenCall.visibility = View.GONE
        incomingOverlay.visibility = View.GONE
    }

    // ============================================================== 拨号

    private fun appendDigit(d: String) {
        if (dialed.length >= 32) return
        dialed += d
        updateDialedText()
    }

    private fun backspace() {
        if (dialed.isNotEmpty()) dialed = dialed.dropLast(1)
        updateDialedText()
    }

    private fun updateDialedText() {
        dialedText.text = dialed
    }

    private fun placeCall() {
        val c = client ?: run {
            toast(getString(R.string.toast_not_registered))
            showSettings()
            return
        }
        if (c.registrationState != RegistrationState.REGISTERED) {
            toast(getString(R.string.toast_not_registered))
            showSettings()
            return
        }
        if (dialed.isBlank()) {
            toast(getString(R.string.toast_no_number))
            return
        }
        val call = runCatching { c.call(dialed) }.getOrElse {
            toast(it.message ?: "呼叫失败")
            return
        }
        currentCall = call
        dialed = ""
        updateDialedText()
        enterCallUi(call)
    }

    // ============================================================== 连接

    private fun saveAndConnect() {
        if (!ensurePermissions()) {
            pendingConnect = true
            return
        }
        connectNow()
    }

    private fun connectNow() {
        val cfg = buildConfigFromUi()
        val problems = cfg.validate()
        if (problems.isNotEmpty()) {
            problemsText.text = problems.joinToString("\n")
            problemsText.visibility = View.VISIBLE
            return
        }
        problemsText.visibility = View.GONE
        persistConfig(cfg)

        closeClient()
        val created = runCatching {
            NsbcClient.builder(this).config(cfg).listener(this).build()
        }.getOrElse {
            problemsText.text = it.message ?: "创建客户端失败"
            problemsText.visibility = View.VISIBLE
            return
        }
        client = created
        currentConfig = cfg
        created.bindRenderers(localView, remoteView)
        created.connect()

        updateAccountText()
        showDialer()
        toast("正在连接 ${cfg.webSocketUrl}")
    }

    private fun disconnect() {
        pendingConnect = false
        closeClient()
        currentConfig = buildConfigFromUi()
        setRegistrationUi(RegistrationState.UNREGISTERED, null)
        toast("已断开")
    }

    private fun closeClient() {
        runCatching { client?.close() }
        client = null
    }

    private fun ensurePermissions(): Boolean {
        val missing = permissions.filter {
            checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) return true
        requestPermissions(missing.toTypedArray(), REQ_PERMISSIONS)
        return false
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_PERMISSIONS && pendingConnect) {
            pendingConnect = false
            if (grantResults.isNotEmpty() && grantResults.all { it == PackageManager.PERMISSION_GRANTED }) {
                connectNow()
            } else {
                toast("缺少摄像头/麦克风权限，无法建立通话")
            }
        }
    }

    // ============================================================== 配置读写

    private fun buildConfigFromUi(): NsbcConfig = NsbcConfig(
        server = textOf(serverInput),
        wssPort = intOf(portInput, 5062),
        wsPath = textOf(pathInput).ifBlank { "/ws" },
        useTls = tlsSwitch.isChecked,
        realm = textOf(realmInput),
        username = textOf(userInput),
        password = textOf(passInput),
        displayName = textOf(displayInput),
        stunServer = textOf(stunInput).ifBlank { null },
        turnServer = textOf(turnInput).ifBlank { null },
        turnUsername = textOf(turnUserInput).ifBlank { null },
        turnCredential = textOf(turnCredInput).ifBlank { null },
        videoEnabled = videoSwitch.isChecked,
        videoWidth = intOf(widthInput, 1280),
        videoHeight = intOf(heightInput, 720),
        videoFps = intOf(fpsInput, 30),
        mirrorLocalPreview = mirrorSwitch.isChecked,
        autoReconnect = reconnectSwitch.isChecked,
        allowSelfSignedCert = selfSignedSwitch.isChecked,
        icePolicy = if (iceSpinner.selectedItemPosition == 1) {
            IceTransportPolicy.RELAY
        } else {
            IceTransportPolicy.ALL
        },
        logLevel = logLevels.getOrElse(logSpinner.selectedItemPosition) { LogLevel.INFO }
    )

    private fun fillUi(cfg: NsbcConfig) {
        serverInput.setText(cfg.server)
        portInput.setText(cfg.wssPort.toString())
        realmInput.setText(cfg.realm)
        userInput.setText(cfg.username)
        passInput.setText(cfg.password)
        displayInput.setText(cfg.displayName)
        tlsSwitch.isChecked = cfg.useTls
        pathInput.setText(cfg.wsPath)
        stunInput.setText(cfg.stunServer.orEmpty())
        turnInput.setText(cfg.turnServer.orEmpty())
        turnUserInput.setText(cfg.turnUsername.orEmpty())
        turnCredInput.setText(cfg.turnCredential.orEmpty())
        iceSpinner.setSelection(if (cfg.icePolicy == IceTransportPolicy.RELAY) 1 else 0)
        videoSwitch.isChecked = cfg.videoEnabled
        widthInput.setText(cfg.videoWidth.toString())
        heightInput.setText(cfg.videoHeight.toString())
        fpsInput.setText(cfg.videoFps.toString())
        mirrorSwitch.isChecked = cfg.mirrorLocalPreview
        reconnectSwitch.isChecked = cfg.autoReconnect
        selfSignedSwitch.isChecked = cfg.allowSelfSignedCert
        logSpinner.setSelection(logLevels.indexOf(cfg.logLevel).coerceAtLeast(0))
    }

    private fun restoreConfig() {
        val json = prefs.getString(KEY_CONFIG, null) ?: runCatching {
            assets.open("nsbc.json").bufferedReader().use { it.readText() }
        }.getOrNull()
        val cfg = runCatching { json?.let { NsbcConfig.fromJson(it) } }.getOrNull()
            ?: NsbcConfig()
        currentConfig = cfg
        fillUi(cfg)
    }

    private fun resetToDefaults() {
        val cfg = runCatching {
            assets.open("nsbc.json").bufferedReader().use { NsbcConfig.fromJson(it.readText()) }
        }.getOrElse { NsbcConfig() }
        fillUi(cfg)
        persistConfig(cfg)
        toast("已恢复默认配置")
    }

    private fun persistConfig(cfg: NsbcConfig) {
        val o = JSONObject().apply {
            put("server", cfg.server)
            put("wssPort", cfg.wssPort)
            put("wsPath", cfg.wsPath)
            put("useTls", cfg.useTls)
            put("realm", cfg.realm)
            put("username", cfg.username)
            put("password", cfg.password)
            put("displayName", cfg.displayName)
            put("stunServer", cfg.stunServer ?: "")
            put("turnServer", cfg.turnServer ?: "")
            put("turnUsername", cfg.turnUsername ?: "")
            put("turnCredential", cfg.turnCredential ?: "")
            put("videoEnabled", cfg.videoEnabled)
            put("videoWidth", cfg.videoWidth)
            put("videoHeight", cfg.videoHeight)
            put("videoFps", cfg.videoFps)
            put("mirrorLocalPreview", cfg.mirrorLocalPreview)
            put("autoReconnect", cfg.autoReconnect)
            put("allowSelfSignedCert", cfg.allowSelfSignedCert)
            put("icePolicy", cfg.icePolicy.name)
            put("logLevel", cfg.logLevel.name)
        }
        prefs.edit().putString(KEY_CONFIG, o.toString()).apply()
    }

    private fun updateAccountText() {
        val cfg = buildConfigFromUi()
        accountText.text = if (cfg.server.isBlank()) {
            "未配置服务器"
        } else {
            "sip:${cfg.username}@${cfg.realm}  ·  ${cfg.webSocketUrl}"
        }
    }

    private fun textOf(e: EditText): String = e.text.toString().trim()
    private fun intOf(e: EditText, def: Int): Int = textOf(e).toIntOrNull() ?: def

    // ============================================================== 注册状态

    private fun setRegistrationUi(state: RegistrationState, detail: String?) {
        regState = state
        val (label, color) = when (state) {
            RegistrationState.UNREGISTERED -> R.string.status_unregistered to "#334155"
            RegistrationState.REGISTERING -> R.string.status_registering to "#B45309"
            RegistrationState.REGISTERED -> R.string.status_registered to "#15803D"
            RegistrationState.FAILED -> R.string.status_failed to "#B91C1C"
        }
        statusChip.setText(label)
        statusChip.backgroundTintList = android.content.res.ColorStateList.valueOf(
            android.graphics.Color.parseColor(color)
        )
        statusChip.setTextColor(android.graphics.Color.WHITE)
        if (state == RegistrationState.FAILED && !detail.isNullOrBlank()) {
            toast("注册失败：$detail")
        }
    }

    // ============================================================== SDK 回调

    override fun onRegistrationChanged(state: RegistrationState, detail: String?) {
        runOnUiThread { setRegistrationUi(state, detail) }
    }

    override fun onIncomingCall(call: NsbcCall) {
        runOnUiThread {
            currentCall = call
            val name = call.remoteDisplayName?.takeIf { it.isNotBlank() }
                ?: call.remoteUri.substringAfter("sip:").substringBefore("@")
            incomingName.text = name
            incomingUri.text = call.remoteUri
            incomingAvatar.text = name.take(1).uppercase()
            incomingOverlay.visibility = View.VISIBLE
        }
    }

    override fun onCallStateChanged(call: NsbcCall) {
        runOnUiThread {
            when (call.state) {
                CallState.CALLING -> {
                    callSubtitle.text = getString(R.string.call_connecting)
                    callTimer.text = "00:00"
                }
                CallState.RINGING -> callSubtitle.text = getString(R.string.call_ringing)
                CallState.ESTABLISHED -> {
                    callSubtitle.text = getString(R.string.call_established)
                    startTimer()
                }
                CallState.TERMINATED, CallState.FAILED -> {
                    stopTimer()
                    addRecent(call)
                    exitCallUi()
                }
                else -> Unit
            }
        }
    }

    override fun onMediaStateChanged(state: String) {
        runOnUiThread { callSubtitle.text = "媒体：$state" }
    }

    override fun onError(error: NsbcError) {
        runOnUiThread { toast(error.message ?: error.code.name) }
    }

    // ============================================================== 通话 UI

    private fun enterCallUi(call: NsbcCall) {
        val name = call.remoteDisplayName?.takeIf { it.isNotBlank() }
            ?: call.remoteUri.trim().let {
                if (it.startsWith("sip:")) it.substringAfter("sip:").substringBefore("@") else it
            }
        callName.text = name
        callSubtitle.text = if (call.isIncoming) "来电" else getString(R.string.call_connecting)
        callAvatar.text = name.take(1).uppercase()

        // 每次进入通话把本地小窗复位到右上角
        localContainer.translationX = 0f
        localContainer.translationY = 0f

        muted = false
        speakerOn = false
        videoOn = currentConfig?.videoEnabled ?: true
        muteBtn.text = getString(R.string.btn_mute)
        speakerBtn.text = getString(R.string.btn_speaker)
        callTimer.text = "00:00"

        screenDialer.visibility = View.GONE
        screenSettings.visibility = View.GONE
        incomingOverlay.visibility = View.GONE
        screenCall.visibility = View.VISIBLE
        applyVideoUi()

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val am = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        am.mode = AudioManager.MODE_IN_COMMUNICATION
        am.isSpeakerphoneOn = false
    }

    private fun exitCallUi() {
        stopTimer()
        resetAudioRoute()
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        currentCall = null
        screenCall.visibility = View.GONE
        showDialer()
    }

    private fun applyVideoUi() {
        videoBtn.text = getString(if (videoOn) R.string.btn_video_off else R.string.btn_video_on)
        localContainer.visibility = if (videoOn) View.VISIBLE else View.INVISIBLE
        switchBtn.isEnabled = videoOn
        remoteView.visibility = if (videoOn) View.VISIBLE else View.INVISIBLE
        callAvatar.visibility = if (videoOn) View.GONE else View.VISIBLE
    }

    private fun setSpeaker(on: Boolean) {
        speakerOn = on
        val am = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        am.mode = AudioManager.MODE_IN_COMMUNICATION
        am.isSpeakerphoneOn = on
        speakerBtn.text = getString(if (on) R.string.btn_speaker_on else R.string.btn_speaker)
    }

    private fun resetAudioRoute() {
        runCatching {
            val am = getSystemService(Context.AUDIO_SERVICE) as AudioManager
            am.isSpeakerphoneOn = false
            am.mode = AudioManager.MODE_NORMAL
        }
        speakerOn = false
    }

    private fun startTimer() {
        handler.removeCallbacks(timerRunnable)
        handler.post(timerRunnable)
    }

    private fun stopTimer() {
        handler.removeCallbacks(timerRunnable)
    }

    private val timerRunnable = object : Runnable {
        override fun run() {
            val call = currentCall ?: return
            val sec = call.durationSeconds
            callTimer.text = String.format(Locale.US, "%02d:%02d", sec / 60, sec % 60)
            handler.postDelayed(this, 500)
        }
    }

    // 本地小窗拖动
    private fun enablePipDrag() {
        var startRawX = 0f
        var startRawY = 0f
        var baseX = 0f
        var baseY = 0f
        localContainer.setOnTouchListener { v, e ->
            when (e.action) {
                MotionEvent.ACTION_DOWN -> {
                    startRawX = e.rawX
                    startRawY = e.rawY
                    baseX = v.x
                    baseY = v.y
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    val parent = v.parent as? View ?: return@setOnTouchListener true
                    v.x = (baseX + e.rawX - startRawX)
                        .coerceIn(0f, (parent.width - v.width).toFloat().coerceAtLeast(0f))
                    v.y = (baseY + e.rawY - startRawY)
                        .coerceIn(0f, (parent.height - v.height).toFloat().coerceAtLeast(0f))
                    true
                }
                else -> true
            }
        }
    }

    // ============================================================== 最近通话

    private data class Recent(
        val uri: String,
        val outgoing: Boolean,
        val ts: Long,
        val durationSec: Long
    )

    private fun addRecent(call: NsbcCall) {
        val list = loadRecents()
        list.add(
            0,
            Recent(
                uri = call.remoteUri,
                outgoing = !call.isIncoming,
                ts = System.currentTimeMillis(),
                durationSec = call.durationSeconds
            )
        )
        saveRecents(list.take(30))
        renderRecents()
    }

    private fun loadRecents(): MutableList<Recent> {
        val raw = prefs.getString(KEY_RECENTS, null) ?: return mutableListOf()
        return runCatching {
            val arr = JSONArray(raw)
            MutableList(arr.length()) { i ->
                val o = arr.getJSONObject(i)
                Recent(
                    uri = o.optString("uri"),
                    outgoing = o.optBoolean("outgoing", true),
                    ts = o.optLong("ts"),
                    durationSec = o.optLong("duration", 0)
                )
            }
        }.getOrElse { mutableListOf() }
    }

    private fun saveRecents(list: List<Recent>) {
        val arr = JSONArray()
        list.forEach {
            arr.put(
                JSONObject().apply {
                    put("uri", it.uri)
                    put("outgoing", it.outgoing)
                    put("ts", it.ts)
                    put("duration", it.durationSec)
                }
            )
        }
        prefs.edit().putString(KEY_RECENTS, arr.toString()).apply()
    }

    private fun renderRecents() {
        recentList.removeAllViews()
        val list = loadRecents()
        recentEmpty.visibility = if (list.isEmpty()) View.VISIBLE else View.GONE

        val fmt = SimpleDateFormat("MM-dd HH:mm", Locale.getDefault())
        list.take(12).forEach { r ->
            val number = r.uri.substringAfter("sip:").substringBefore("@")
            val arrow = if (r.outgoing) "↗" else "↙"
            val dur = String.format(Locale.US, "%02d:%02d", r.durationSec / 60, r.durationSec % 60)
            val row = TextView(this).apply {
                text = "$arrow  $number    ${fmt.format(Date(r.ts))}    $dur"
                setTextColor(0xFF94A3B8.toInt())
                textSize = 13f
                setPadding(0, dp(10), 0, dp(10))
                setOnClickListener {
                    dialed = number
                    updateDialedText()
                }
            }
            recentList.addView(row)
        }
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }

    companion object {
        private const val REQ_PERMISSIONS = 1001
        private const val KEY_CONFIG = "config_json"
        private const val KEY_RECENTS = "recents_json"
    }
}
