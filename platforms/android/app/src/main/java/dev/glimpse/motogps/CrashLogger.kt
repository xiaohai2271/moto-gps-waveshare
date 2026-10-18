package dev.glimpse.motogps

import android.content.Context
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Writes the next uncaught exception to a private file before the process dies,
 * so a field crash can be retrieved with
 *   adb shell run-as dev.glimpse.motogps cat files/last-crash.txt
 * The file stays inside the app sandbox and is never uploaded.
 */
object CrashLogger {
    fun install(context: Context) {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            runCatching {
                val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())
                val text = buildString {
                    appendLine("time=$stamp")
                    appendLine("thread=${thread.name}")
                    appendLine(error.stackTraceToString())
                }
                File(context.filesDir, "last-crash.txt").writeText(text)
            }
            previous?.uncaughtException(thread, error)
        }
    }
}
