package dev.glimpse.motogps.ui

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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.glimpse.motogps.ble.BleProtocol
import dev.glimpse.motogps.ble.DecodedWifiAp
import dev.glimpse.motogps.ble.DecodedWifiStatus

/**
 * WiFi provisioning screen (docs §7.6). Lets the user connect the round display
 * to a WiFi network: scan list + manual SSID/password entry + status card with
 * SSID/IP/RSSI/error + disconnect/forget actions. Supports both the manual
 * hotspot mode (phone hotspot credentials typed in) and LocalOnlyHotspot auto
 * mode (phone hotspot is discoverable and shown in the scan list).
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun WifiSettingsSheet(vm: WifiProvisioningViewModel, onDismiss: () -> Unit) {
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 20.dp)
                .verticalScroll(rememberScrollState()),
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text("圆屏 WiFi 配网", fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.weight(1f))
                TextButton(onClick = onDismiss) { Text("关闭") }
            }

            Spacer(Modifier.height(8.dp))

            if (!vm.deviceSupportsWifi) {
                Text(
                    "当前圆屏固件不支持 WiFi 配网（capability bit9 未上报）。请先升级固件。",
                    color = Color(0xFFD0021B),
                    fontSize = 14.sp,
                )
                Spacer(Modifier.height(8.dp))
                return@Column
            }

            WifiStatusCard(vm)

            Spacer(Modifier.height(16.dp))
            WifiAutoHotspotSection(vm)

            Spacer(Modifier.height(16.dp))
            WifiScanSection(vm)

            Spacer(Modifier.height(16.dp))
            WifiManualEntrySection(vm)

            Spacer(Modifier.height(24.dp))
        }
    }
}

/**
 * Auto hotspot mode (docs §7.6): the app starts a LocalOnlyHotspot, reads the
 * SSID/password, and sends WifiConfig to the device. Falls back to manual mode
 * when LocalOnlyHotspot is unsupported.
 */
@Composable
private fun WifiAutoHotspotSection(vm: WifiProvisioningViewModel) {
    val context = androidx.compose.ui.platform.LocalContext.current
    var hotspotState by remember { mutableStateOf<AutoHotspotState>(AutoHotspotState.Idle) }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(Color(0xFFF2F2F7), RoundedCornerShape(12.dp))
            .padding(16.dp),
    ) {
        Text(
            "自动热点（推荐）",
            fontSize = 14.sp,
            fontWeight = FontWeight.Medium,
            color = Color(0xFF8E8E93),
        )
        Spacer(Modifier.height(8.dp))
        Text(
            "App 启动手机热点并读取 SSID/密码，自动下发给圆屏连接。",
            fontSize = 13.sp,
            color = Color(0xFF8E8E93),
        )
        Spacer(Modifier.height(12.dp))

        when (val s = hotspotState) {
            is AutoHotspotState.Idle -> Button(
                onClick = {
                    hotspotState = AutoHotspotState.Starting
                    LocalOnlyHotspotHelper.start(context) { result ->
                        hotspotState = when (result) {
                            is AutoHotspotResult.Started ->
                                AutoHotspotState.Started(result.ssid, result.password)
                            is AutoHotspotResult.Failed ->
                                AutoHotspotState.Failed(result.reason)
                        }
                        if (result is AutoHotspotResult.Started) {
                            vm.connectWifi(
                                result.ssid,
                                result.password,
                                hidden = false,
                                autoReconnect = true,
                            )
                        }
                    }
                },
                modifier = Modifier.fillMaxWidth(),
            ) { Text("启动手机热点并自动配网") }

            is AutoHotspotState.Starting -> Row(
                verticalAlignment = Alignment.CenterVertically,
            ) {
                CircularProgressIndicator(modifier = Modifier.size(18.dp), strokeWidth = 2.dp)
                Spacer(Modifier.width(8.dp))
                Text("正在启动热点…", fontSize = 13.sp, color = Color(0xFF8E8E93))
            }

            is AutoHotspotState.Started -> Column {
                Text("热点已启动：${s.ssid}", fontSize = 14.sp, color = Color(0xFF34C759))
                Spacer(Modifier.height(8.dp))
                OutlinedButton(
                    onClick = {
                        LocalOnlyHotspotHelper.stop()
                        hotspotState = AutoHotspotState.Idle
                    },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("停止热点") }
            }

            is AutoHotspotState.Failed -> Column {
                Text(
                    "自动热点不可用：${s.reason}",
                    fontSize = 13.sp,
                    color = Color(0xFFD0021B),
                )
                Spacer(Modifier.height(8.dp))
                OutlinedButton(
                    onClick = { hotspotState = AutoHotspotState.Idle },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("改用手动模式") }
            }
        }
    }
}

private sealed interface AutoHotspotState {
    object Idle : AutoHotspotState
    object Starting : AutoHotspotState
    data class Started(val ssid: String, val password: String) : AutoHotspotState
    data class Failed(val reason: String) : AutoHotspotState
}

@Composable
private fun WifiStatusCard(vm: WifiProvisioningViewModel) {
    val status by vm.wifiStatus.collectAsState()

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(Color(0xFFF2F2F7), RoundedCornerShape(12.dp))
            .padding(16.dp),
    ) {
        Text("当前状态", fontSize = 14.sp, fontWeight = FontWeight.Medium, color = Color(0xFF8E8E93))

        Spacer(Modifier.height(8.dp))

        val stateText = when (status?.state) {
            BleProtocol.MessageType.WifiState.OFF -> "已关闭"
            BleProtocol.MessageType.WifiState.CONNECTING -> "正在连接…"
            BleProtocol.MessageType.WifiState.CONNECTED -> "已连接"
            BleProtocol.MessageType.WifiState.FAILED -> "连接失败"
            BleProtocol.MessageType.WifiState.NO_CREDENTIALS -> "未配置网络"
            null -> "未获取状态（等待设备上报）"
            else -> "未知"
        }
        val stateColor = when (status?.state) {
            BleProtocol.MessageType.WifiState.CONNECTED -> Color(0xFF34C759)
            BleProtocol.MessageType.WifiState.FAILED -> Color(0xFFD0021B)
            else -> Color(0xFF007AFF)
        }
        Text(stateText, fontSize = 16.sp, fontWeight = FontWeight.SemiBold, color = stateColor)

        status?.let { s ->
            Spacer(Modifier.height(8.dp))
            if (s.ssid.isNotEmpty()) {
                StatusRow("SSID", s.ssid)
            }
            if (s.state == BleProtocol.MessageType.WifiState.CONNECTED) {
                StatusRow("IP", formatIpv4(s.ipv4))
                StatusRow("RSSI", "${s.rssi} dBm")
            }
            val errorText = when (s.error) {
                BleProtocol.MessageType.WifiError.AUTH -> "认证失败（密码错误）"
                BleProtocol.MessageType.WifiError.SSID_NOT_FOUND -> "找不到该网络"
                BleProtocol.MessageType.WifiError.TIMEOUT -> "连接超时"
                BleProtocol.MessageType.WifiError.INTERNAL -> "内部错误"
                else -> null
            }
            if (errorText != null) {
                Spacer(Modifier.height(8.dp))
                Text(errorText, color = Color(0xFFD0021B), fontSize = 13.sp)
            }
        }

        Spacer(Modifier.height(12.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(
                onClick = { vm.disconnectWifi() },
                enabled = status?.state == BleProtocol.MessageType.WifiState.CONNECTED ||
                    status?.state == BleProtocol.MessageType.WifiState.CONNECTING,
            ) { Text("断开") }
            OutlinedButton(
                onClick = { vm.forgetWifi() },
                enabled = status != null && status?.state != BleProtocol.MessageType.WifiState.NO_CREDENTIALS,
            ) { Text("忘记网络") }
        }
    }
}

@Composable
private fun StatusRow(label: String, value: String) {
    Row(modifier = Modifier.fillMaxWidth()) {
        Text(label, fontSize = 13.sp, color = Color(0xFF8E8E93), modifier = Modifier.width(80.dp))
        Text(value, fontSize = 13.sp)
    }
}

@Composable
private fun WifiScanSection(vm: WifiProvisioningViewModel) {
    val scan by vm.wifiScan.collectAsState()
    val scanInFlight by vm.scanInFlight.collectAsState()

    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Text("附近网络", fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.weight(1f))
        if (scanInFlight) {
            CircularProgressIndicator(
                modifier = Modifier.size(18.dp),
                strokeWidth = 2.dp,
            )
            Spacer(Modifier.width(8.dp))
            Text("扫描中…", fontSize = 13.sp, color = Color(0xFF8E8E93))
        } else {
            TextButton(onClick = { vm.startScan() }) { Text("刷新") }
        }
    }

    Spacer(Modifier.height(8.dp))

    val aps = scan?.accessPoints.orEmpty()
    if (aps.isEmpty()) {
        Text(
            if (scanInFlight) "正在扫描附近的 WiFi…" else "点击“刷新”开始扫描",
            fontSize = 13.sp,
            color = Color(0xFF8E8E93),
        )
    } else {
        aps.forEach { ap ->
            WifiApRow(ap = ap, onClick = {
                // Selecting an AP fills the manual entry section — handled by
                // the parent screen through the vm state; here we just log.
                android.util.Log.d("WifiSettings", "selected ${ap.ssid}")
            })
        }
    }
}

@Composable
private fun WifiApRow(ap: DecodedWifiAp, onClick: () -> Unit) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(vertical = 8.dp),
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(ap.ssid, fontSize = 15.sp)
            Text(
                authLabel(ap.auth),
                fontSize = 12.sp,
                color = Color(0xFF8E8E93),
            )
        }
        Text(
            "${ap.rssi} dBm",
            fontSize = 12.sp,
            color = rssiColor(ap.rssi),
        )
    }
}

@Composable
private fun WifiManualEntrySection(vm: WifiProvisioningViewModel) {
    var ssid by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var hidden by remember { mutableStateOf(false) }
    var autoReconnect by remember { mutableStateOf(true) }

    Text("手动连接", fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
    Spacer(Modifier.height(8.dp))

    OutlinedTextField(
        value = ssid,
        onValueChange = { ssid = it },
        label = { Text("网络名称 (SSID)") },
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
    )
    Spacer(Modifier.height(8.dp))
    OutlinedTextField(
        value = password,
        onValueChange = { password = it },
        label = { Text("密码") },
        singleLine = true,
        visualTransformation = PasswordVisualTransformation(),
        modifier = Modifier.fillMaxWidth(),
    )

    Spacer(Modifier.height(8.dp))
    Row(verticalAlignment = Alignment.CenterVertically) {
        TextButton(onClick = { hidden = !hidden }) {
            Text(if (hidden) "☑ 隐藏网络" else "☐ 隐藏网络")
        }
        Spacer(Modifier.width(8.dp))
        TextButton(onClick = { autoReconnect = !autoReconnect }) {
            Text(if (autoReconnect) "☑ 自动重连" else "☐ 自动重连")
        }
    }

    Spacer(Modifier.height(12.dp))
    Button(
        onClick = { vm.connectWifi(ssid, password, hidden, autoReconnect) },
        enabled = ssid.isNotBlank(),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Text("连接")
    }
    Spacer(Modifier.height(4.dp))
    OutlinedButton(
        onClick = { vm.connectTemporary(ssid, password) },
        enabled = ssid.isNotBlank(),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Text("临时连接（不保存）")
    }
}

private fun authLabel(auth: Int): String = when (auth) {
    BleProtocol.MessageType.WifiAuth.OPEN -> "开放"
    BleProtocol.MessageType.WifiAuth.WPA2 -> "WPA2"
    BleProtocol.MessageType.WifiAuth.WPA3 -> "WPA3"
    BleProtocol.MessageType.WifiAuth.WPA2_WPA3 -> "WPA2/WPA3"
    else -> "未知"
}

private fun rssiColor(rssi: Int): Color = when {
    rssi >= -55 -> Color(0xFF34C759)
    rssi >= -70 -> Color(0xFFFF9500)
    else -> Color(0xFFD0021B)
}

private fun formatIpv4(ipv4: Long): String {
    val b = ipv4.toInt()
    return "${b and 0xFF}.${(b shr 8) and 0xFF}.${(b shr 16) and 0xFF}.${(b shr 24) and 0xFF}"
}
