package dev.glimpse.motogps.ui

import android.Manifest
import android.content.pm.PackageManager
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import dev.glimpse.motogps.nav.NavigationSession
import kotlin.math.roundToInt

@Composable
fun NavScreen(modifier: Modifier = Modifier, vm: NavViewModel = viewModel()) {
    val state by vm.state.collectAsState()
    var query by remember { mutableStateOf("") }
    var showLog by remember { mutableStateOf(false) }
    val context = LocalContext.current

    val locationPermission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { }
    val notificationPermission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> if (granted) vm.start() }

    Column(
        modifier = modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text("导航", style = MaterialTheme.typography.headlineSmall)
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(
                    if (!state.hasFix) "等待 GPS 定位…"
                    else "GPS 已定位 · 精度 ${"%.0f".format(state.accuracyM)} m · " +
                        "速度 ${(state.speedMps * 3.6).roundToInt()} km/h",
                    style = MaterialTheme.typography.bodyMedium,
                )
                Text(
                    if (state.bleReady) "圆屏：已就绪" else "圆屏：未连接（导航仍会运行）",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { locationPermission.launch(Manifest.permission.ACCESS_FINE_LOCATION) }) {
                Text("授权定位")
            }
            Button(
                enabled = state.selected != null,
                onClick = {
                    if (Build.VERSION.SDK_INT >= 33 &&
                        ContextCompat.checkSelfPermission(
                            context,
                            Manifest.permission.POST_NOTIFICATIONS,
                        ) != PackageManager.PERMISSION_GRANTED
                    ) {
                        notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
                    } else {
                        vm.start()
                    }
                },
            ) { Text("开始导航") }
            OutlinedButton(onClick = { vm.stop() }) { Text("结束") }
        }

        OutlinedTextField(
            value = query,
            onValueChange = { query = it },
            label = { Text("搜索目的地") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
        )
        Button(
            enabled = query.trim().length >= 2,
            onClick = { vm.search(query) },
        ) { Text("搜索") }

        state.selected?.let { place ->
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp)) {
                    Text("已选：${place.name}", style = MaterialTheme.typography.bodyMedium)
                    Text(
                        listOf(place.displayArea, place.address).filter { it.isNotBlank() }
                            .joinToString(" · "),
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
        }

        if (state.places.isNotEmpty()) {
            LazyColumn(Modifier.weight(1f, fill = false)) {
                items(state.places, key = { it.id }) { place ->
                    Card(
                        onClick = { vm.select(place) },
                        modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp),
                    ) {
                        Column(Modifier.padding(12.dp)) {
                            Text(place.name, style = MaterialTheme.typography.bodyMedium)
                            Text(
                                place.displayArea + (place.distanceM?.let { " · ${it.roundToInt()} m" } ?: ""),
                                style = MaterialTheme.typography.bodySmall,
                            )
                        }
                    }
                }
            }
        }

        if (state.route != null) {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text("状态：${state.status}", style = MaterialTheme.typography.titleSmall)
                    Text(
                        "剩余 ${(state.remainingM / 1000).roundToInt()} km · " +
                            "${(state.remainingS / 60.0).roundToInt()} min",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    if (state.instruction.isNotBlank()) {
                        Text(state.instruction, style = MaterialTheme.typography.bodyMedium)
                    }
                    if (state.roadName.isNotBlank()) {
                        Text(
                            "${state.roadName} · ${state.distanceToManeuverM.roundToInt()} m",
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                }
            }
        }

        HorizontalDivider()
        OutlinedButton(onClick = { showLog = !showLog }) {
            Text(if (showLog) "隐藏日志" else "显示会话日志")
        }
        if (showLog) {
            LazyColumn(Modifier.weight(1f)) {
                items(state.log.asReversed()) { line ->
                    Text(line, style = MaterialTheme.typography.bodySmall)
                }
            }
        }
        Text(
            "路线来自自建网关，几何为 GCJ-02；定位输入为 WGS84。地图页可下载离线瓦片，" +
                "导航中按位置发送小地图场景。",
            style = MaterialTheme.typography.bodySmall,
        )
    }
}
