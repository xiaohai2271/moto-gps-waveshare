package dev.glimpse.motogps.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import dev.glimpse.motogps.permissions.PermissionCenter
import dev.glimpse.motogps.permissions.PermissionKind
import dev.glimpse.motogps.permissions.PermissionStatus

/**
 * 权限中心（P3，plan §7.5）：四项权限的状态检查、运行时申请与系统设置跳转。
 * 进入页面自动检查；onResume（从系统设置/权限弹窗返回）自动刷新。
 *
 * 入口（由 Lead 合并进 MotoAppScreen 的 DeviceDetailsSheet）：
 * ```
 * var showPermissions by remember { mutableStateOf(false) }
 * // 设备详情 sheet 内加一行：
 * RowDivider()
 * Row(
 *     Modifier.fillMaxWidth().clickable { showPermissions = true }
 *         .padding(horizontal = 16.dp, vertical = 11.dp),
 * ) {
 *     Text("🛡", fontSize = 16.sp)
 *     Spacer(Modifier.width(8.dp))
 *     Text("权限检查", fontSize = 16.sp, color = Color(0xFF000000))
 *     Spacer(Modifier.weight(1f))
 *     Text("›", fontSize = 14.sp, color = IosSecondary.copy(alpha = 0.6f))
 * }
 * // MotoAppScreen() 顶层：
 * if (showPermissions) PermissionCenterSheet(onDismiss = { showPermissions = false })
 * ```
 */
@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
fun PermissionCenterSheet(onDismiss: () -> Unit) {
    val context = LocalContext.current
    val center = remember(context) { PermissionCenter(context) }
    var snapshot by remember { mutableStateOf<PermissionCenter.Snapshot?>(null) }
    var refreshTick by remember { mutableStateOf(0) }

    fun refresh() {
        refreshTick++ // triggers the LaunchedEffect below
    }

    // Initial check when entering the page.
    LaunchedEffect(Unit) { refresh() }
    // Re-check on every refreshTick bump.
    LaunchedEffect(refreshTick) {
        // runCatching: a vendor-specific failure while reading system state
        // must never crash the page (P3 acceptance criterion).
        snapshot = runCatching { center.snapshot() }.getOrNull()
    }
    // Re-check on every lifecycle resume (back from settings / permission dialog).
    // ComponentActivity (MainActivity) implements LifecycleOwner; the app's
    // context here is the activity itself.
    val lifecycleOwner = remember(context) {
        var c: android.content.Context? = context
        while (c != null) {
            if (c is androidx.lifecycle.LifecycleOwner) return@remember c
            c = if (c is android.content.ContextWrapper) c.baseContext else null
        }
        null
    }
    androidx.compose.runtime.DisposableEffect(lifecycleOwner) {
        val owner = lifecycleOwner ?: return@DisposableEffect onDispose { }
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) refresh()
        }
        owner.lifecycle.addObserver(observer)
        onDispose { owner.lifecycle.removeObserver(observer) }
    }

    // In-page runtime permission requests; refresh right after the dialog closes.
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { refresh() }

    androidx.compose.material3.ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            Modifier
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(bottom = 24.dp),
        ) {
            Box(Modifier.fillMaxWidth().height(48.dp)) {
                Text(
                    "权限检查",
                    fontSize = 17.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.align(Alignment.Center),
                )
                TextButton(
                    onClick = onDismiss,
                    modifier = Modifier.align(Alignment.CenterEnd),
                ) { Text("完成", color = IosPermissionBlue) }
            }

            val snap = snapshot
            if (snap == null) {
                Box(
                    Modifier.fillMaxWidth().padding(vertical = 48.dp),
                    contentAlignment = Alignment.Center,
                ) { CircularProgressIndicator() }
            } else {
                PermissionSummaryHeader(snap)
                PermissionGroupSection {
                    PermissionRow(
                        title = "定位",
                        icon = "📍",
                        result = snap.location,
                        actionLabel = locationActionLabel(snap.location.status),
                        onClick = {
                            when (snap.location.status) {
                                PermissionStatus.DISABLED -> launchSettings(context) {
                                    center.locationSettingsIntent()
                                }
                                PermissionStatus.LIMITED, PermissionStatus.DENIED -> {
                                    // Ask for precise location; the system dialog
                                    // covers both fine+coarse in one prompt.
                                    launchPermissions(permissionLauncher, PermissionKind.LOCATION)
                                }
                                PermissionStatus.GRANTED -> Unit
                            }
                        },
                    )
                    PermissionRowDivider()
                    PermissionRow(
                        title = "蓝牙",
                        icon = "📶",
                        result = snap.bluetooth,
                        actionLabel = bluetoothActionLabel(snap.bluetooth.status),
                        onClick = {
                            when (snap.bluetooth.status) {
                                PermissionStatus.DISABLED -> launchSettings(context) {
                                    center.bluetoothSettingsIntent()
                                }
                                PermissionStatus.DENIED -> launchPermissions(
                                    permissionLauncher,
                                    PermissionKind.BLUETOOTH,
                                )
                                else -> Unit
                            }
                        },
                    )
                    PermissionRowDivider()
                    PermissionRow(
                        title = "通知",
                        icon = "🔔",
                        result = snap.notification,
                        actionLabel = notificationActionLabel(snap.notification.status),
                        onClick = {
                            when (snap.notification.status) {
                                PermissionStatus.DENIED -> launchPermissions(
                                    permissionLauncher,
                                    PermissionKind.NOTIFICATION,
                                )
                                else -> Unit
                            }
                        },
                    )
                    PermissionRowDivider()
                    PermissionRow(
                        title = "媒体通知使用权",
                        icon = "🎵",
                        result = snap.notificationListener,
                        actionLabel = listenerActionLabel(snap.notificationListener.status),
                        onClick = {
                            if (snap.notificationListener.status != PermissionStatus.GRANTED) {
                                launchSettings(context) { center.notificationListenerSettingsIntent() }
                            }
                        },
                    )
                }
                PermissionFooter(
                    "被拒绝不会影响其他功能；需要时随时回到这里重新授权。\n" +
                        "媒体通知使用权无法在应用内直接开启，需要你到系统设置里手动允许。",
                )
            }
        }
    }
}

// MARK: - helpers

private val IosPermissionBlue = Color(0xFF007AFF)
private val IosPermissionGreen = Color(0xFF34C759)
private val IosPermissionRed = Color(0xFFFF3B30)
private val IosPermissionOrange = Color(0xFFFF9500)
private val IosPermissionSecondary = Color(0xFF8E8E93)
private val IosPermissionSeparator = Color(0x1F000000)
private val IosPermissionCard = Color.White
private val IosPermissionText = Color(0xFF000000)

@Composable
private fun PermissionSummaryHeader(snap: PermissionCenter.Snapshot) {
    val allReady = snap.readyCount == snap.totalCount
    Column(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 14.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text(
            "${snap.readyCount}/${snap.totalCount} 已就绪",
            fontSize = 20.sp,
            fontWeight = FontWeight.SemiBold,
            color = if (allReady) IosPermissionGreen else IosPermissionOrange,
        )
        Text(
            when {
                allReady -> "全部权限就绪，导航与媒体功能可用"
                else -> "缺少的权限会影响对应功能，点对应项去开启"
            },
            fontSize = 13.sp,
            color = IosPermissionSecondary,
            textAlign = TextAlign.Center,
        )
    }
}

@Composable
private fun PermissionGroupSection(content: @Composable () -> Unit) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
        Surface(
            shape = RoundedCornerShape(10.dp),
            color = IosPermissionCard,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Column { content() }
        }
        Spacer(Modifier.height(18.dp))
    }
}

@Composable
private fun PermissionRowDivider() = Box(
    Modifier
        .padding(start = 16.dp)
        .fillMaxWidth()
        .height(0.5.dp)
        .background(IosPermissionSeparator),
)

@Composable
private fun PermissionRow(
    title: String,
    icon: String,
    result: dev.glimpse.motogps.permissions.PermissionCheckResult,
    actionLabel: String?,
    onClick: () -> Unit,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .clickable(enabled = actionLabel != null) { onClick() }
            .padding(horizontal = 16.dp, vertical = 11.dp),
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(icon, fontSize = 16.sp)
            Spacer(Modifier.width(8.dp))
            Text(title, fontSize = 16.sp, color = IosPermissionText, modifier = Modifier.weight(1f))
            val (text, color) = statusText(result.status)
            Text(text, fontSize = 15.sp, color = color)
            if (actionLabel != null) {
                Spacer(Modifier.width(8.dp))
                Text(actionLabel, fontSize = 15.sp, color = IosPermissionBlue)
            }
        }
        val hint = statusHint(result.kind, result.status)
        if (hint != null) {
            Text(
                hint,
                fontSize = 13.sp,
                color = IosPermissionSecondary,
                modifier = Modifier.padding(start = 24.dp, top = 3.dp),
            )
        }
    }
}

@Composable
private fun PermissionFooter(text: String) {
    Text(
        text,
        fontSize = 13.sp,
        color = IosPermissionSecondary,
        modifier = Modifier.padding(start = 32.dp, end = 32.dp, bottom = 12.dp),
    )
}

private fun statusText(status: PermissionStatus): Pair<String, Color> = when (status) {
    PermissionStatus.GRANTED -> "已授权" to IosPermissionGreen
    PermissionStatus.LIMITED -> "受限" to IosPermissionOrange
    PermissionStatus.DISABLED -> "未开启" to IosPermissionOrange
    PermissionStatus.DENIED -> "未授权" to IosPermissionRed
}

/** 每项状态的可操作说明（中文，拒绝后给出下一步动作）。 */
private fun statusHint(kind: PermissionKind, status: PermissionStatus): String? =
    when (kind) {
        PermissionKind.LOCATION -> when (status) {
            PermissionStatus.GRANTED -> null
            PermissionStatus.LIMITED -> "仅有模糊定位权限，导航精度受限；点右侧重新申请精确位置"
            PermissionStatus.DENIED -> "导航与路线规划需要位置权限；点右侧授权"
            PermissionStatus.DISABLED -> "系统定位服务已关闭；点右侧跳到系统设置打开"
        }
        PermissionKind.BLUETOOTH -> when (status) {
            PermissionStatus.GRANTED -> null
            PermissionStatus.DENIED -> "连接圆屏需要蓝牙（附近设备）权限；点右侧授权"
            PermissionStatus.DISABLED -> "手机蓝牙已关闭；点右侧跳到系统设置打开"
            PermissionStatus.LIMITED -> null
        }
        PermissionKind.NOTIFICATION -> when (status) {
            PermissionStatus.GRANTED -> null
            PermissionStatus.DENIED -> "导航提醒需要通知权限；点右侧授权"
            PermissionStatus.LIMITED, PermissionStatus.DISABLED -> null
        }
        PermissionKind.NOTIFICATION_LISTENER -> when (status) {
            PermissionStatus.GRANTED -> null
            PermissionStatus.DENIED -> "圆屏音乐控制需要读取媒体通知；点右侧到系统设置手动开启"
            PermissionStatus.LIMITED, PermissionStatus.DISABLED -> null
        }
    }

private fun locationActionLabel(status: PermissionStatus): String? = when (status) {
    PermissionStatus.GRANTED -> null
    PermissionStatus.DISABLED -> "去设置"
    else -> "去授权"
}

private fun bluetoothActionLabel(status: PermissionStatus): String? = when (status) {
    PermissionStatus.GRANTED -> null
    PermissionStatus.DISABLED -> "去设置"
    else -> "去授权"
}

private fun notificationActionLabel(status: PermissionStatus): String? = when (status) {
    PermissionStatus.GRANTED -> null
    else -> "去授权"
}

private fun listenerActionLabel(status: PermissionStatus): String? = when (status) {
    PermissionStatus.GRANTED -> null
    else -> "去设置"
}

/**
 * 发起运行时权限申请。申请的只是缺失项；全部已授权时不弹窗。
 * 即使用户拒绝，回调也只触发刷新，不会崩溃。
 */
private fun launchPermissions(
    launcher: androidx.activity.result.ActivityResultLauncher<Array<String>>,
    kind: PermissionKind,
) {
    val permissions = dev.glimpse.motogps.permissions.PermissionLogic
        .runtimePermissionsFor(kind, android.os.Build.VERSION.SDK_INT)
        .toTypedArray()
    if (permissions.isNotEmpty()) {
        runCatching { launcher.launch(permissions) }
    }
}

/** 跳系统设置；任何系统页面缺失（厂商定制）都吞掉异常，不崩溃。 */
private fun launchSettings(context: android.content.Context, intentFactory: () -> android.content.Intent) {
    runCatching { context.startActivity(intentFactory()) }
}
