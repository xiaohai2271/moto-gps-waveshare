package dev.glimpse.motogps.permissions

/**
 * 权限中心纯逻辑层（无 Android 依赖，可做纯 JVM 单测）。
 *
 * 状态语义（对应 docs/NAV_LAYOUT_EDITOR_PLAN.md §7.5）：
 * - [PermissionStatus.GRANTED]  已授权（就绪）
 * - [PermissionStatus.DENIED]   未授权（运行时权限被拒）
 * - [PermissionStatus.LIMITED]  受限（仅模糊定位等部分能力可用）
 * - [PermissionStatus.DISABLED] 未开启（系统定位开关 / 蓝牙适配器关闭）
 */

enum class PermissionKind { LOCATION, BLUETOOTH, NOTIFICATION, NOTIFICATION_LISTENER }

enum class PermissionStatus { GRANTED, DENIED, LIMITED, DISABLED }

data class PermissionCheckResult(
    val kind: PermissionKind,
    val status: PermissionStatus,
) {
    val isReady: Boolean get() = status == PermissionStatus.GRANTED
}

/** 运行时权限字符串（与 android.Manifest 常量一致，用字面量保持本文件纯 JVM）。 */
object PermissionIds {
    const val FINE_LOCATION = "android.permission.ACCESS_FINE_LOCATION"
    const val COARSE_LOCATION = "android.permission.ACCESS_COARSE_LOCATION"
    const val BLUETOOTH_SCAN = "android.permission.BLUETOOTH_SCAN"
    const val BLUETOOTH_CONNECT = "android.permission.BLUETOOTH_CONNECT"
    const val BLUETOOTH = "android.permission.BLUETOOTH"
    const val POST_NOTIFICATIONS = "android.permission.POST_NOTIFICATIONS"
}

object PermissionLogic {

    const val SDK_S = 31            // Build.VERSION_CODES.S：BLUETOOTH_SCAN/CONNECT
    const val SDK_P = 28            // LocationManager.isLocationEnabled
    const val SDK_TIRAMISU = 33     // POST_NOTIFICATIONS

    const val TOTAL_ITEMS = 4

    /**
     * 定位：无任何定位权限 → 未授权；系统定位开关关闭 → 未开启；
     * 仅模糊定位 → 受限（导航精度受限）；精确 + 模糊 → 已授权。
     */
    fun evaluateLocation(
        fineGranted: Boolean,
        coarseGranted: Boolean,
        locationEnabled: Boolean,
    ): PermissionStatus {
        if (!fineGranted && !coarseGranted) return PermissionStatus.DENIED
        if (!locationEnabled) return PermissionStatus.DISABLED
        return if (fineGranted) PermissionStatus.GRANTED else PermissionStatus.LIMITED
    }

    /**
     * 蓝牙：API 31+ 需要 SCAN+CONNECT；API ≤30 的 BLUETOOTH 是安装时权限。
     * 权限缺失 → 未授权；适配器关闭 → 未开启；否则已授权。
     * [adapterOn] 为 null 表示无法查询（无权限读适配器或无适配器），按已授权处理，
     * 由调用方保证只在权限已授予时传入适配器状态。
     */
    fun evaluateBluetooth(
        sdkInt: Int,
        scanGranted: Boolean,
        connectGranted: Boolean,
        adapterOn: Boolean?,
    ): PermissionStatus {
        val granted = if (sdkInt >= SDK_S) scanGranted && connectGranted else true
        if (!granted) return PermissionStatus.DENIED
        return when (adapterOn) {
            false -> PermissionStatus.DISABLED
            else -> PermissionStatus.GRANTED
        }
    }

    /** 通知：API 33 以下系统不要求运行时授权；API 33+ 看 POST_NOTIFICATIONS。 */
    fun evaluateNotification(sdkInt: Int, postNotificationsGranted: Boolean): PermissionStatus =
        if (sdkInt < SDK_TIRAMISU || postNotificationsGranted) {
            PermissionStatus.GRANTED
        } else {
            PermissionStatus.DENIED
        }

    /** 媒体通知使用权：只能由用户在系统设置开启，无“受限/未开启系统开关”细分。 */
    fun evaluateNotificationListener(listenerEnabled: Boolean): PermissionStatus =
        if (listenerEnabled) PermissionStatus.GRANTED else PermissionStatus.DENIED

    /** 首次启动要申请的运行时权限集合（原 MainActivity.requestMissingPermissions 的清单）。 */
    fun startupRuntimePermissions(sdkInt: Int): List<String> = buildList {
        add(PermissionIds.FINE_LOCATION)
        add(PermissionIds.COARSE_LOCATION)
        if (sdkInt >= SDK_S) {
            add(PermissionIds.BLUETOOTH_SCAN)
            add(PermissionIds.BLUETOOTH_CONNECT)
        }
        if (sdkInt >= SDK_TIRAMISU) add(PermissionIds.POST_NOTIFICATIONS)
    }

    /** 某检查项对应的、可在页内申请的运行时权限；空列表表示只能跳系统设置。 */
    fun runtimePermissionsFor(kind: PermissionKind, sdkInt: Int): List<String> = when (kind) {
        PermissionKind.LOCATION -> listOf(PermissionIds.FINE_LOCATION, PermissionIds.COARSE_LOCATION)
        PermissionKind.BLUETOOTH ->
            if (sdkInt >= SDK_S) listOf(PermissionIds.BLUETOOTH_SCAN, PermissionIds.BLUETOOTH_CONNECT) else emptyList()
        PermissionKind.NOTIFICATION ->
            if (sdkInt >= SDK_TIRAMISU) listOf(PermissionIds.POST_NOTIFICATIONS) else emptyList()
        PermissionKind.NOTIFICATION_LISTENER -> emptyList()
    }

    fun readyCount(vararg results: PermissionCheckResult): Int = results.count { it.isReady }
}
