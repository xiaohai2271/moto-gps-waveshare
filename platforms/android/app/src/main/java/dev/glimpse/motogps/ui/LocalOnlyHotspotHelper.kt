package dev.glimpse.motogps.ui

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper

/**
 * Wraps Android's LocalOnlyHotspot API (docs §7.6 auto hotspot mode). The app
 * starts a local-only hotspot on the phone, reads the SSID/password from the
 * reservation, and sends them to the device via BLE WifiConfig. The reservation
 * is released when the screen closes to avoid draining the battery.
 *
 * On Android 8+ this uses [WifiManager.startLocalOnlyHotspot]. The SSID/password
 * are delivered to the callback as [WifiManager.LocalOnlyHotspotReservation] is
 * an opaque handle — Android does not expose the passphrase to third-party apps,
 * so this helper surfaces that limitation and the caller falls back to manual mode.
 */
object LocalOnlyHotspotHelper {
    private var reservation: WifiManager.LocalOnlyHotspotReservation? = null
    private val mainHandler = Handler(Looper.getMainLooper())

    /**
     * Starts a LocalOnlyHotspot. Invokes [onResult] with either
     * Started(ssid, password) or Failed(reason). On most Android versions the
     * SSID/password are not exposed to third-party apps, so this typically
     * reports Failed with an explanatory reason and the caller falls back to
     * manual mode (docs §7.6).
     */
    fun start(
        context: Context,
        onResult: (AutoHotspotResult) -> Unit,
    ) {
        val wifiManager =
            context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            mainHandler.post {
                onResult(AutoHotspotResult.Failed("Android 8.0 以下不支持 LocalOnlyHotspot"))
            }
            return
        }
        try {
            @Suppress("DEPRECATION")
            wifiManager.startLocalOnlyHotspot(
                object : WifiManager.LocalOnlyHotspotCallback() {
                    override fun onStarted(res: WifiManager.LocalOnlyHotspotReservation) {
                        reservation = res
                        // Android 8+ exposes the generated credentials through
                        // wifiConfiguration; the previous "not exposed" fallback
                        // made the auto-hotspot button always fail even though
                        // the reservation had started.
                        @Suppress("DEPRECATION")
                        val config = res.wifiConfiguration
                        val ssid = config?.SSID?.trim('"').orEmpty()
                        val password = config?.preSharedKey?.trim('"').orEmpty()
                        mainHandler.post {
                            if (ssid.isNotEmpty() && password.isNotEmpty()) {
                                onResult(AutoHotspotResult.Started(ssid, password))
                            } else {
                                onResult(
                                    AutoHotspotResult.Failed(
                                        "系统未提供热点 SSID/密码，请改用手动模式",
                                    ),
                                )
                            }
                        }
                    }

                    override fun onStopped() {
                        reservation = null
                    }

                    override fun onFailed(reason: Int) {
                        reservation = null
                        val reasonText = when (reason) {
                            WifiManager.LocalOnlyHotspotCallback.ERROR_NO_CHANNEL ->
                                "无可用信道（请关闭其他热点后重试）"
                            WifiManager.LocalOnlyHotspotCallback.ERROR_GENERIC ->
                                "系统错误"
                            WifiManager.LocalOnlyHotspotCallback.ERROR_INCOMPATIBLE_MODE ->
                                "当前 WiFi 模式不兼容"
                            else -> "错误码 $reason"
                        }
                        mainHandler.post {
                            onResult(AutoHotspotResult.Failed(reasonText))
                        }
                    }
                },
                mainHandler,
            )
        } catch (e: SecurityException) {
            mainHandler.post {
                onResult(
                    AutoHotspotResult.Failed(
                        "缺少 NEARBY_WIFI_DEVICES 权限或系统限制：${e.message}",
                    ),
                )
            }
        } catch (e: IllegalStateException) {
            mainHandler.post {
                onResult(AutoHotspotResult.Failed("热点启动失败：${e.message}"))
            }
        }
    }

    fun stop() {
        reservation?.close()
        reservation = null
    }
}

sealed interface AutoHotspotResult {
    data class Started(val ssid: String, val password: String) : AutoHotspotResult
    data class Failed(val reason: String) : AutoHotspotResult
}
