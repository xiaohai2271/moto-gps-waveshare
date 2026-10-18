package dev.glimpse.motogps.ui

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
import androidx.compose.material3.LinearProgressIndicator
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
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel

@Composable
fun MapScreen(modifier: Modifier = Modifier, vm: MapViewModel = viewModel()) {
    val state by vm.state.collectAsState()
    var query by remember { mutableStateOf("") }

    Column(
        modifier = modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text("离线地图", style = MaterialTheme.typography.headlineSmall)
        Text(
            "网关 ${dev.glimpse.motogps.GatewayConfig.DEFAULT_BASE_URL} · z15 · " +
                "已缓存 ${state.cachedBytes / 1024 / 1024} MiB",
            style = MaterialTheme.typography.bodySmall,
        )

        OutlinedTextField(
            value = query,
            onValueChange = { query = it },
            label = { Text("城市 / 区县") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                enabled = query.trim().length >= 2 && !state.downloading,
                onClick = { vm.searchCities(query) },
            ) { Text("查询") }
        }

        Text(state.message, style = MaterialTheme.typography.bodySmall)

        state.progress?.let { progress ->
            LinearProgressIndicator(
                progress = { progress.fraction },
                modifier = Modifier.fillMaxWidth(),
            )
            Text(
                "${progress.done}/${progress.total} 瓦片 · 失败 ${progress.failed} · " +
                    "${progress.bytes / 1024} KiB",
                style = MaterialTheme.typography.bodySmall,
            )
        }

        if (state.cities.isNotEmpty()) {
            Text("下载范围", style = MaterialTheme.typography.titleSmall)
            LazyColumn(Modifier.weight(1f, fill = false)) {
                items(state.cities, key = { it.id }) { city ->
                    Card(Modifier.fillMaxWidth().padding(vertical = 3.dp)) {
                        Row(
                            Modifier.padding(12.dp).fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                        ) {
                            Column(Modifier.weight(1f)) {
                                Text(city.name, style = MaterialTheme.typography.bodyMedium)
                                Text(city.detail, style = MaterialTheme.typography.bodySmall)
                            }
                            OutlinedButton(
                                enabled = !state.downloading,
                                onClick = { vm.download(city) },
                            ) { Text("下载") }
                        }
                    }
                }
            }
        }

        if (state.packs.isNotEmpty()) {
            Text("已下载", style = MaterialTheme.typography.titleSmall)
            LazyColumn(Modifier.weight(1f)) {
                items(state.packs, key = { it.id }) { pack ->
                    Card(Modifier.fillMaxWidth().padding(vertical = 3.dp)) {
                        Row(
                            Modifier.padding(12.dp).fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                        ) {
                            Column(Modifier.weight(1f)) {
                                Text(pack.name, style = MaterialTheme.typography.bodyMedium)
                                Text(
                                    "${pack.tiles.size} 瓦片 · ${pack.bytes / 1024} KiB",
                                    style = MaterialTheme.typography.bodySmall,
                                )
                            }
                            OutlinedButton(onClick = { vm.delete(pack) }) { Text("删除") }
                        }
                    }
                }
            }
        }

        Text(
            "数据来源：gateway /v1/map/tiles（OpenStreetMap ODbL-1.0，详见后端署名）。" +
                "离线包只含背景道路与建筑，不是完整离线路线规划。",
            style = MaterialTheme.typography.bodySmall,
        )
    }
}
