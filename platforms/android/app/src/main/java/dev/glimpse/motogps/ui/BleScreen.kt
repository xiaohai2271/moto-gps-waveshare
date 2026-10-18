package dev.glimpse.motogps.ui

import android.Manifest
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import dev.glimpse.motogps.ble.Esp32BleCentral

private fun requiredPermissions(): Array<String> = if (Build.VERSION.SDK_INT >= 31) {
    arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
} else {
    // Pre-12 scanning requires location; navigation later needs it anyway.
    arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
}

@Composable
fun BleScreen(modifier: Modifier = Modifier, vm: BleViewModel = viewModel()) {
    val state by vm.state.collectAsState()
    val devices by vm.devices.collectAsState()
    val log by vm.log.collectAsState()
    val navState by vm.navState.collectAsState()

    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { granted ->
        if (granted.values.all { it }) vm.scan()
    }

    Column(
        modifier = modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("连接圆屏", style = MaterialTheme.typography.headlineSmall)
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(stateLabel(state), style = MaterialTheme.typography.titleMedium)
                Text(
                    "协议 v1 · 服务 7e57a000…1000 · 需设备确认 Ready 后才发送业务数据",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = { permissionLauncher.launch(requiredPermissions()) }) {
                Text("扫描设备")
            }
            OutlinedButton(onClick = { vm.stopScan() }) { Text("停止") }
            when (state) {
                is Esp32BleCentral.State.Connected, is Esp32BleCentral.State.Handshaking -> {
                    OutlinedButton(onClick = { vm.disconnect() }) { Text("断开") }
                }
                else -> Unit
            }
        }

        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    "音乐遥控：" + when {
                        !navState.mediaAccess -> "未授权通知使用权"
                        navState.mediaConnected -> "已连接 · ${navState.mediaTitle}"
                        else -> "已授权，无播放器"
                    },
                    style = MaterialTheme.typography.bodySmall,
                )
                Text(
                    "只控制系统媒体会话，不播放声音、不使用无障碍权限",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            OutlinedButton(onClick = { vm.openMediaAccessSettings() }) {
                Text("授权")
            }
        }

        if (devices.isNotEmpty()) {
            Text("选择设备（按服务 UUID 过滤）", style = MaterialTheme.typography.titleSmall)
            LazyColumn(Modifier.weight(1f, fill = false)) {
                items(devices, key = { it.address }) { device ->
                    Card(
                        onClick = { vm.connect(device.address) },
                        modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp),
                    ) {
                        Row(
                            Modifier.padding(12.dp).fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                        ) {
                            Column {
                                Text(device.name ?: "(未命名)", style = MaterialTheme.typography.bodyMedium)
                                Text(device.address, style = MaterialTheme.typography.bodySmall)
                            }
                            Text("${device.rssi} dBm", style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            }
        }

        HorizontalDivider()
        Text("会话日志", style = MaterialTheme.typography.titleSmall)
        LazyColumn(Modifier.weight(1f)) {
            items(log.asReversed()) { line ->
                Text(line, style = MaterialTheme.typography.bodySmall)
            }
        }
        Text(
            "真机已验证：扫描、GATT 连接、MTU 协商、三步握手、心跳、断开与重连。" +
                "导航数据发送尚未接入。",
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

private fun stateLabel(state: Esp32BleCentral.State): String = when (state) {
    is Esp32BleCentral.State.Idle -> "空闲"
    is Esp32BleCentral.State.Unavailable -> "蓝牙不可用：${state.reason}"
    is Esp32BleCentral.State.Scanning -> "扫描中…"
    is Esp32BleCentral.State.Connecting -> "连接中：${state.address}"
    is Esp32BleCentral.State.Handshaking -> "握手中：${state.phase}"
    is Esp32BleCentral.State.Connected ->
        "已就绪 · frameSize=${state.frameSize} · 能力=0x${state.capabilities.toString(16)}" +
            if (state.degraded) "（降级）" else ""
    is Esp32BleCentral.State.Reconnecting -> "重连中（第 ${state.attempt} 次，${state.delayMs / 1000}s）"
    is Esp32BleCentral.State.Failed -> "失败：${state.message}"
}
