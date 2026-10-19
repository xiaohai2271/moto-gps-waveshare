package dev.glimpse.motogps.permissions

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 权限状态判定与申请清单的纯 JVM 测试（plan §9.3 “权限状态映射”）。
 * Android 侧读取（PermissionCenter）依赖 framework，不在单测范围。
 */
class PermissionLogicTest {

    // MARK: - 定位

    @Test
    fun location_grantedWhenFineAndCoarseAndSystemEnabled() {
        assertEquals(
            PermissionStatus.GRANTED,
            PermissionLogic.evaluateLocation(fineGranted = true, coarseGranted = true, locationEnabled = true),
        )
    }

    @Test
    fun location_limitedWhenOnlyCoarse() {
        assertEquals(
            PermissionStatus.LIMITED,
            PermissionLogic.evaluateLocation(fineGranted = false, coarseGranted = true, locationEnabled = true),
        )
    }

    @Test
    fun location_grantedWhenFineEvenWithoutCoarse() {
        // 精确权限在语义上覆盖模糊定位；系统实际也不会出现这种半授权组合。
        assertEquals(
            PermissionStatus.GRANTED,
            PermissionLogic.evaluateLocation(fineGranted = true, coarseGranted = false, locationEnabled = true),
        )
    }

    @Test
    fun location_deniedWhenNoLocationPermission() {
        assertEquals(
            PermissionStatus.DENIED,
            PermissionLogic.evaluateLocation(fineGranted = false, coarseGranted = false, locationEnabled = true),
        )
    }

    @Test
    fun location_disabledTakesPrecedenceOverGrantedButSystemOff() {
        // 已授权但系统定位开关关闭 → 未开启（导航不可用，需跳系统设置）。
        assertEquals(
            PermissionStatus.DISABLED,
            PermissionLogic.evaluateLocation(fineGranted = true, coarseGranted = true, locationEnabled = false),
        )
    }

    @Test
    fun location_deniedWhenNothingGrantedEvenIfSystemOff() {
        // 完全没授权时优先报“未授权”，引导页内申请，而不是先跳设置。
        assertEquals(
            PermissionStatus.DENIED,
            PermissionLogic.evaluateLocation(fineGranted = false, coarseGranted = false, locationEnabled = false),
        )
    }

    // MARK: - 蓝牙

    @Test
    fun bluetooth_api31_grantedWhenScanConnectGrantedAndAdapterOn() {
        assertEquals(
            PermissionStatus.GRANTED,
            PermissionLogic.evaluateBluetooth(
                sdkInt = 33, scanGranted = true, connectGranted = true, adapterOn = true,
            ),
        )
    }

    @Test
    fun bluetooth_api31_deniedWhenScanMissing() {
        assertEquals(
            PermissionStatus.DENIED,
            PermissionLogic.evaluateBluetooth(
                sdkInt = 31, scanGranted = false, connectGranted = true, adapterOn = true,
            ),
        )
    }

    @Test
    fun bluetooth_api31_deniedWhenConnectMissing() {
        assertEquals(
            PermissionStatus.DENIED,
            PermissionLogic.evaluateBluetooth(
                sdkInt = 31, scanGranted = true, connectGranted = false, adapterOn = true,
            ),
        )
    }

    @Test
    fun bluetooth_api31_disabledWhenAdapterOff() {
        assertEquals(
            PermissionStatus.DISABLED,
            PermissionLogic.evaluateBluetooth(
                sdkInt = 33, scanGranted = true, connectGranted = true, adapterOn = false,
            ),
        )
    }

    @Test
    fun bluetooth_api30_installTimePermissionAlwaysGranted() {
        // API ≤30 的 BLUETOOTH 是安装时权限，永远视为已授权；只看适配器开关。
        assertEquals(
            PermissionStatus.GRANTED,
            PermissionLogic.evaluateBluetooth(sdkInt = 30, scanGranted = false, connectGranted = false, adapterOn = true),
        )
        assertEquals(
            PermissionStatus.DISABLED,
            PermissionLogic.evaluateBluetooth(sdkInt = 29, scanGranted = false, connectGranted = false, adapterOn = false),
        )
    }

    @Test
    fun bluetooth_adapterUnknownTreatedAsGranted() {
        // 无蓝牙硬件/无权限读适配器时不误报“未开启”。
        assertEquals(
            PermissionStatus.GRANTED,
            PermissionLogic.evaluateBluetooth(sdkInt = 33, scanGranted = true, connectGranted = true, adapterOn = null),
        )
    }

    // MARK: - 通知

    @Test
    fun notification_grantedBelowApi33() {
        assertEquals(
            PermissionStatus.GRANTED,
            PermissionLogic.evaluateNotification(sdkInt = 32, postNotificationsGranted = false),
        )
    }

    @Test
    fun notification_grantedOnApi33WhenPermissionGranted() {
        assertEquals(
            PermissionStatus.GRANTED,
            PermissionLogic.evaluateNotification(sdkInt = 33, postNotificationsGranted = true),
        )
    }

    @Test
    fun notification_deniedOnApi33WhenMissing() {
        assertEquals(
            PermissionStatus.DENIED,
            PermissionLogic.evaluateNotification(sdkInt = 34, postNotificationsGranted = false),
        )
    }

    // MARK: - 媒体通知使用权

    @Test
    fun notificationListener_binaryState() {
        assertEquals(
            PermissionStatus.GRANTED,
            PermissionLogic.evaluateNotificationListener(listenerEnabled = true),
        )
        assertEquals(
            PermissionStatus.DENIED,
            PermissionLogic.evaluateNotificationListener(listenerEnabled = false),
        )
    }

    // MARK: - 汇总

    @Test
    fun readyCount_countsOnlyGrantedItems() {
        val results = listOf(
            PermissionCheckResult(PermissionKind.LOCATION, PermissionStatus.GRANTED),
            PermissionCheckResult(PermissionKind.BLUETOOTH, PermissionStatus.GRANTED),
            PermissionCheckResult(PermissionKind.NOTIFICATION, PermissionStatus.LIMITED),
            PermissionCheckResult(PermissionKind.NOTIFICATION_LISTENER, PermissionStatus.DISABLED),
        )
        assertEquals(2, PermissionLogic.readyCount(*results.toTypedArray()))
        assertEquals(4, PermissionLogic.TOTAL_ITEMS)
    }

    // MARK: - 申请清单

    @Test
    fun startupPermissions_matchSdkGates() {
        val api30 = PermissionLogic.startupRuntimePermissions(30)
        assertTrue(api30.contains(PermissionIds.FINE_LOCATION))
        assertTrue(api30.contains(PermissionIds.COARSE_LOCATION))
        assertFalse(api30.contains(PermissionIds.BLUETOOTH_SCAN))
        assertFalse(api30.contains(PermissionIds.POST_NOTIFICATIONS))

        val api31 = PermissionLogic.startupRuntimePermissions(31)
        assertTrue(api31.contains(PermissionIds.BLUETOOTH_SCAN))
        assertTrue(api31.contains(PermissionIds.BLUETOOTH_CONNECT))
        assertFalse(api31.contains(PermissionIds.POST_NOTIFICATIONS))

        val api33 = PermissionLogic.startupRuntimePermissions(33)
        assertTrue(api33.contains(PermissionIds.BLUETOOTH_SCAN))
        assertTrue(api33.contains(PermissionIds.POST_NOTIFICATIONS))
        assertEquals(5, api33.size)
    }

    @Test
    fun runtimePermissionsFor_listenerIsAlwaysEmpty() {
        // 媒体通知使用权没有运行时弹窗，只能跳系统设置。
        assertTrue(
            PermissionLogic.runtimePermissionsFor(PermissionKind.NOTIFICATION_LISTENER, 34).isEmpty(),
        )
    }

    @Test
    fun runtimePermissionsFor_bluetoothBelowApi31IsEmpty() {
        assertTrue(PermissionLogic.runtimePermissionsFor(PermissionKind.BLUETOOTH, 30).isEmpty())
    }

    @Test
    fun runtimePermissionsFor_notificationBelowApi33IsEmpty() {
        assertTrue(PermissionLogic.runtimePermissionsFor(PermissionKind.NOTIFICATION, 32).isEmpty())
    }
}
