package com.nway.nsbc.sdk

import android.util.Log

/**
 * 统一日志出口。可通过 [NsbcLogger.sink] 接管（例如写入 UI 日志面板）。
 */
object NsbcLogger {

    private const val TAG = "NsbcSdk"

    /** 日志级别，由 [NsbcClient] 根据配置更新 */
    @Volatile
    var level: LogLevel = LogLevel.INFO

    /** 自定义日志接收器；SDK 同时输出到 logcat */
    @Volatile
    var sink: ((LogLevel, String, String) -> Unit)? = null

    fun d(tag: String, msg: String) = log(LogLevel.DEBUG, tag, msg)
    fun i(tag: String, msg: String) = log(LogLevel.INFO, tag, msg)
    fun w(tag: String, msg: String) = log(LogLevel.WARN, tag, msg)
    fun e(tag: String, msg: String, t: Throwable? = null) {
        log(LogLevel.ERROR, tag, if (t != null) "$msg :: ${t.message}" else msg)
    }

    private fun log(l: LogLevel, tag: String, msg: String) {
        if (l.ordinal > level.ordinal) return
        val full = "[$tag] $msg"
        runCatching {
            when (l) {
                LogLevel.ERROR -> Log.e(TAG, full)
                LogLevel.WARN -> Log.w(TAG, full)
                LogLevel.INFO -> Log.i(TAG, full)
                LogLevel.DEBUG -> Log.d(TAG, full)
                LogLevel.VERBOSE -> Log.v(TAG, full)
                LogLevel.NONE -> Unit
            }
        }
        sink?.invoke(l, tag, msg)
    }
}
