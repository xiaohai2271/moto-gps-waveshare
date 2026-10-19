package dev.glimpse.motogps.nav

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import dev.glimpse.motogps.MainActivity
import dev.glimpse.motogps.MotoGpsApplication

/**
 * Foreground service that keeps GNSS and the BLE link alive while navigating.
 * Started only after the user taps "开始导航", it shows an ongoing notification
 * with a stop action.
 *
 * Declared types are location|connectedDevice, but Android requires the matching
 * runtime permission for each type on API 34+. A user who denied the Bluetooth
 * (or location) prompt must never see the app crash, so only the types that are
 * actually permitted are requested and a rejection degrades to a normal service.
 */
class NavigationForegroundService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            (application as MotoGpsApplication).navigationSession.stop()
            stopSelf()
            return START_NOT_STICKY
        }
        if (!startForegroundWithType()) {
            // Without a usable foreground type the system would kill or crash
            // the process. Stop quietly: navigation keeps running while the app
            // is visible and the UI already reports the missing permission.
            Log.w(TAG, "无法进入前台服务（缺少定位或蓝牙权限），导航继续但可能被系统回收")
            stopSelf()
            return START_NOT_STICKY
        }
        return START_STICKY
    }

    /** @return true when startForeground succeeded. */
    private fun startForegroundWithType(): Boolean {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "导航",
                NotificationManager.IMPORTANCE_LOW,
            ).apply {
                description = "导航期间保持定位与圆屏连接"
                setShowBadge(false)
            }
            manager.createNotificationChannel(channel)
        }

        val open = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val stop = PendingIntent.getService(
            this, 1,
            Intent(this, NavigationForegroundService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val notification: Notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Glimpse 导航进行中")
            .setContentText("正在使用定位并保持圆屏连接")
            .setSmallIcon(android.R.drawable.ic_menu_mylocation)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(open)
            .addAction(android.R.drawable.ic_menu_close_clear_cancel, "结束导航", stop)
            .build()

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            return runCatching { startForeground(NOTIFICATION_ID, notification) }
                .onFailure { Log.w(TAG, "startForeground 失败：${it.message}") }
                .isSuccess
        }

        val types = ArrayList<Int>(2)
        if (hasPermission(Manifest.permission.ACCESS_FINE_LOCATION) ||
            hasPermission(Manifest.permission.ACCESS_COARSE_LOCATION)
        ) {
            types += ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION
        }
        val bluetoothReady = Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            hasPermission(Manifest.permission.BLUETOOTH_CONNECT)
        if (bluetoothReady) {
            types += ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        }
        if (types.isEmpty()) return false

        val type = types.reduce { acc, value -> acc or value }
        return runCatching { startForeground(NOTIFICATION_ID, notification, type) }
            .onFailure { Log.w(TAG, "startForeground(type=$type) 失败：${it.message}") }
            .isSuccess
    }

    private fun hasPermission(permission: String): Boolean =
        ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED

    companion object {
        const val ACTION_STOP = "dev.glimpse.motogps.action.STOP_NAVIGATION"
        private const val CHANNEL_ID = "navigation"
        private const val NOTIFICATION_ID = 42
        private const val TAG = "MotoGpsNav"

        /** Never throws: a denied or revoked permission must not crash the call. */
        fun start(context: Context) {
            val intent = Intent(context, NavigationForegroundService::class.java)
            runCatching {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    context.startForegroundService(intent)
                } else {
                    context.startService(intent)
                }
            }.onFailure { Log.w(TAG, "无法启动前台服务：${it.message}") }
        }

        fun stop(context: Context) {
            runCatching {
                context.stopService(Intent(context, NavigationForegroundService::class.java))
            }.onFailure { Log.w(TAG, "无法停止前台服务：${it.message}") }
        }
    }
}
