package dev.glimpse.motogps.permissions

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.location.LocationManager
import android.os.Build
import android.provider.Settings
import android.text.TextUtils
import androidx.core.app.NotificationManagerCompat
import dev.glimpse.motogps.media.MediaNotificationListener

/**
 * 权限中心 Android 侧：实际读取系统状态，产出 [PermissionCheckResult]，
 * 并提供每项对应的系统设置跳转 Intent。读取失败一律安全降级，
 * 永不抛异常（拒绝/异常不崩溃是验收项）。
 */
class PermissionCenter(private val context: Context) {

    /** 一次快照：四项检查结果 + 就绪数量。 */
    data class Snapshot(
        val location: PermissionCheckResult,
        val bluetooth: PermissionCheckResult,
        val notification: PermissionCheckResult,
        val notificationListener: PermissionCheckResult,
    ) {
        val readyCount: Int =
            listOf(location, bluetooth, notification, notificationListener).count { it.isReady }
        val totalCount: Int = PermissionLogic.TOTAL_ITEMS
    }

    fun snapshot(): Snapshot = Snapshot(
        location = PermissionCheckResult(PermissionKind.LOCATION, checkLocation()),
        bluetooth = PermissionCheckResult(PermissionKind.BLUETOOTH, checkBluetooth()),
        notification = PermissionCheckResult(PermissionKind.NOTIFICATION, checkNotification()),
        notificationListener = PermissionCheckResult(
            PermissionKind.NOTIFICATION_LISTENER,
            checkNotificationListener(),
        ),
    )

    private fun granted(permission: String): Boolean =
        androidx.core.content.ContextCompat.checkSelfPermission(context, permission) ==
            android.content.pm.PackageManager.PERMISSION_GRANTED

    private fun checkLocation(): PermissionStatus = PermissionLogic.evaluateLocation(
        fineGranted = granted(Manifest.permission.ACCESS_FINE_LOCATION),
        coarseGranted = granted(Manifest.permission.ACCESS_COARSE_LOCATION),
        locationEnabled = isLocationEnabled(),
    )

    private fun checkBluetooth(): PermissionStatus {
        val adapterOn = runCatching {
            val manager =
                context.getSystemService(Context.BLUETOOTH_SERVICE) as? android.bluetooth.BluetoothManager
            val adapter = manager?.adapter
            if (adapter == null) null else adapter.isEnabled
        }.getOrNull()
        return PermissionLogic.evaluateBluetooth(
            sdkInt = Build.VERSION.SDK_INT,
            scanGranted = granted(Manifest.permission.BLUETOOTH_SCAN),
            connectGranted = granted(Manifest.permission.BLUETOOTH_CONNECT),
            adapterOn = adapterOn,
        )
    }

    private fun checkNotification(): PermissionStatus = PermissionLogic.evaluateNotification(
        sdkInt = Build.VERSION.SDK_INT,
        postNotificationsGranted = granted(Manifest.permission.POST_NOTIFICATIONS),
    )

    private fun checkNotificationListener(): PermissionStatus =
        PermissionLogic.evaluateNotificationListener(isNotificationListenerEnabled())

    /**
     * 媒体通知使用权：优先用 NotificationManagerCompat（内部读
     * Settings.Secure.ENABLED_NOTIFICATION_LISTENERS），并校验是否是本 App 的
     * listener 组件，而不是任何 App 都算开启。
     */
    private fun isNotificationListenerEnabled(): Boolean {
        val componentName = ComponentName(context, MediaNotificationListener::class.java)
        // 1) Official API path (API 24+).
        val enabledListeners = NotificationManagerCompat.getEnabledListenerPackages(context)
        if (enabledListeners.contains(context.packageName)) return true
        // 2) Fallback: parse the raw secure setting and match our ComponentName.
        val flat = runCatching {
            Settings.Secure.getString(context.contentResolver, "enabled_notification_listeners")
        }.getOrNull() ?: return false
        return runCatching {
            TextUtils.SimpleStringSplitter(':').apply { setString(flat) }
                .any { it.equals(componentName.flattenToString(), ignoreCase = true) }
        }.getOrDefault(false)
    }

    /**
     * 系统定位开关。minSdk 26：API 28+ 用 LocationManager.isLocationEnabled，
     * 更早版本用 isProviderEnabled(GPS/NETWORK) 兜底（这版 targetSdk 36 实际都会走 28+）。
     */
    private fun isLocationEnabled(): Boolean = runCatching {
        val lm = context.getSystemService(Context.LOCATION_SERVICE) as LocationManager
        if (Build.VERSION.SDK_INT >= PermissionLogic.SDK_P) {
            lm.isLocationEnabled
        } else {
            @Suppress("DEPRECATION")
            lm.isProviderEnabled(LocationManager.GPS_PROVIDER) ||
                @Suppress("DEPRECATION")
                lm.isProviderEnabled(LocationManager.NETWORK_PROVIDER)
        }
    }.getOrDefault(true)

    // MARK: - 系统设置跳转（返回后由 UI 触发刷新）

    /** 通知使用权唯一入口：通知使用权设置页。 */
    fun notificationListenerSettingsIntent(): Intent =
        Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)

    /** 蓝牙系统设置（适配器开关）。 */
    fun bluetoothSettingsIntent(): Intent =
        Intent(Settings.ACTION_BLUETOOTH_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)

    /** 系统定位开关设置页。 */
    fun locationSettingsIntent(): Intent =
        Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)

    /** 应用通知设置（POST_NOTIFICATIONS 被永久拒绝时的兜底）。 */
    fun appNotificationSettingsIntent(): Intent = Intent(
        Settings.ACTION_APP_NOTIFICATION_SETTINGS,
        android.net.Uri.parse("package:${context.packageName}"),
    ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)

    companion object {
        /** 启动时应补齐的运行时权限（供 MainActivity 复用）。 */
        fun startupMissingPermissions(context: Context): Array<String> =
            PermissionLogic.startupRuntimePermissions(Build.VERSION.SDK_INT)
                .filter {
                    androidx.core.content.ContextCompat.checkSelfPermission(context, it) !=
                        android.content.pm.PackageManager.PERMISSION_GRANTED
                }
                .toTypedArray()
    }
}
