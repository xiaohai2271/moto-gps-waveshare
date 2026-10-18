package dev.glimpse.motogps.ui

import android.content.Intent
import android.provider.Settings
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import dev.glimpse.motogps.ble.Esp32BleCentral
import dev.glimpse.motogps.gateway.Place
import dev.glimpse.motogps.nav.NavigationSession
import dev.glimpse.motogps.nav.RideFormatting
import dev.glimpse.motogps.nav.RouteCandidate

/**
 * Single primary flow ported from the iOS app (ContentView.swift):
 * 出发 (destination search) → 路线 (route preview) → 导航中 (ride).
 * Device, map downloads, gateway and privacy are secondary sheets.
 */
enum class MotoScreen { Home, RoutePreview, ActiveNavigation }

@Composable
fun MotoAppScreen(vm: NavViewModel = viewModel()) {
    val state by vm.state.collectAsState()
    val bleState by vm.bleState.collectAsState()

    val screen = when {
        vm.isNavigationActive -> MotoScreen.ActiveNavigation
        state.selected != null -> MotoScreen.RoutePreview
        else -> MotoScreen.Home
    }

    var showDevice by remember { mutableStateOf(false) }
    var showGateway by remember { mutableStateOf(false) }
    var showPrivacy by remember { mutableStateOf(false) }
    var showMapDownloads by remember { mutableStateOf(false) }

    when (screen) {
        MotoScreen.Home -> HomeScreen(
            vm,
            onOpenDevice = { showDevice = true },
            onOpenGateway = { showGateway = true },
            onOpenPrivacy = { showPrivacy = true },
            onOpenMapDownloads = { showMapDownloads = true },
        )
        MotoScreen.RoutePreview -> RoutePreviewScreen(
            vm,
            onOpenDevice = { showDevice = true },
            onOpenMapDownloads = { showMapDownloads = true },
        )
        MotoScreen.ActiveNavigation -> ActiveNavigationScreen(vm, onOpenDevice = { showDevice = true })
    }

    if (showDevice) DeviceSheet(vm, bleState, onDismiss = { showDevice = false })
    if (showGateway) GatewaySheet(vm, onDismiss = { showGateway = false })
    if (showPrivacy) PrivacySheet(onDismiss = { showPrivacy = false })
    if (showMapDownloads) MapDownloadsSheet(vm, onDismiss = { showMapDownloads = false })
}

// MARK: - 出发 (home)

@Composable
private fun HomeScreen(
    vm: NavViewModel,
    onOpenDevice: () -> Unit,
    onOpenGateway: () -> Unit,
    onOpenPrivacy: () -> Unit,
    onOpenMapDownloads: () -> Unit,
) {
    val state by vm.state.collectAsState()
    val recentPlaces by vm.recentPlaces.collectAsState()
    val context = LocalContext.current

    Column(
        Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text("出发", fontSize = 28.sp, fontWeight = FontWeight.Bold)
            Row {
                TextButton(onClick = { vm.startDemo() }) { Text("演示") }
                TextButton(onClick = onOpenDevice) { Text("圆屏") }
            }
        }

        if (!vm.gatewayCustomised()) {
            Card(
                Modifier.fillMaxWidth().clickable { onOpenGateway() },
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer),
            ) {
                Column(Modifier.padding(14.dp)) {
                    Text("设置导航网关", fontWeight = FontWeight.SemiBold)
                    Text(
                        "填写你部署的网关地址后，就可以搜索地点和规划路线。",
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
        }

        OutlinedTextField(
            value = state.query,
            onValueChange = { vm.queryChanged(it) },
            modifier = Modifier.fillMaxWidth(),
            placeholder = { Text("搜索地点或地址") },
            singleLine = true,
            trailingIcon = {
                if (state.query.isNotEmpty()) {
                    TextButton(onClick = { vm.clearQuery() }) { Text("清除") }
                }
            },
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = { vm.submitSearch() }, enabled = state.query.trim().length >= 2) {
                Text("搜索")
            }
        }

        when {
            state.isSearching -> Row(
                Modifier.fillMaxWidth().padding(8.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                CircularProgressIndicator(Modifier.size(20.dp))
                Text("正在搜索…", color = MaterialTheme.colorScheme.secondary)
            }

            state.places.isNotEmpty() -> LazyColumn(Modifier.weight(1f, fill = false)) {
                items(state.places) { place ->
                    PlaceRow(place, symbol = "📍") { vm.select(place) }
                }
            }

            else -> Column(Modifier.weight(1f).fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                if (recentPlaces.isEmpty()) {
                    Card(Modifier.fillMaxWidth()) {
                        Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text("选择目的地", fontWeight = FontWeight.SemiBold, fontSize = 17.sp)
                            Text(
                                "搜索地点或地址，选好路线就出发。",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.secondary,
                            )
                        }
                    }
                } else {
                    Text("最近搜索", style = MaterialTheme.typography.titleSmall)
                    recentPlaces.forEach { place ->
                        PlaceRowSaved(place) { vm.selectRecent(place) }
                    }
                    TextButton(onClick = { vm.clearRecent() }) { Text("清空最近搜索") }
                }
            }
        }

        Spacer(Modifier.weight(0.01f))
        HorizontalDivider()
        SecondaryLink("地图与离线下载", "自动加载周边，也能提前保存城市和沿途地图") {
            onOpenMapDownloads()
        }
        HorizontalDivider()
        SecondaryLink("网关设置", if (vm.gatewayCustomised()) vm.gatewayCurrent() else "使用默认网关") {
            onOpenGateway()
        }
        SecondaryLink("隐私与数据", "位置、蓝牙与存储的说明") {
            onOpenPrivacy()
        }
        Text(
            "连接圆屏后，导航指引会自动同步。",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.secondary,
        )
    }
}

@Composable
private fun PlaceRow(place: Place, symbol: String, onClick: () -> Unit) {
    Card(Modifier.fillMaxWidth().padding(vertical = 3.dp).clickable(onClick = onClick)) {
        Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(symbol, fontSize = 20.sp)
            Column(Modifier.weight(1f).padding(start = 10.dp)) {
                Text(place.name, fontWeight = FontWeight.Medium)
                Text(
                    placeSubtitle(place.displayArea, place.city, place.district, place.address, place.distanceM),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.secondary,
                )
            }
            Text("›", color = MaterialTheme.colorScheme.secondary)
        }
    }
}

@Composable
private fun PlaceRowSaved(place: dev.glimpse.motogps.nav.SavedPlace, onClick: () -> Unit) {
    Card(Modifier.fillMaxWidth().padding(vertical = 3.dp).clickable(onClick = onClick)) {
        Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("🕘", fontSize = 18.sp)
            Column(Modifier.weight(1f).padding(start = 10.dp)) {
                Text(place.name, fontWeight = FontWeight.Medium)
                Text(
                    place.subtitle(),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.secondary,
                )
            }
            Text("›", color = MaterialTheme.colorScheme.secondary)
        }
    }
}

private fun placeSubtitle(
    displayArea: String,
    city: String,
    district: String,
    address: String,
    distanceM: Double?,
): String {
    val area = displayArea.ifEmpty {
        listOf(city, district).filter { it.isNotEmpty() }.joinToString(" · ")
    }
    val parts = mutableListOf<String>()
    if (area.isNotEmpty()) parts += area
    if (address.isNotEmpty()) parts += address
    if (distanceM != null && distanceM >= 0) {
        parts += if (distanceM >= 1_000) {
            "距你 %.1f km".format(java.util.Locale.US, distanceM / 1_000)
        } else {
            "距你 ${distanceM.toInt()} m"
        }
    }
    return parts.joinToString(" · ").ifEmpty { "查看路线" }
}

// MARK: - 路线 (route preview)

@Composable
private fun RoutePreviewScreen(
    vm: NavViewModel,
    onOpenDevice: () -> Unit,
    onOpenMapDownloads: () -> Unit,
) {
    val state by vm.state.collectAsState()
    val place = state.selected

    Column(
        Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text("路线", fontSize = 22.sp, fontWeight = FontWeight.Bold)
            TextButton(onClick = { vm.clearQuery() }) { Text("更换") }
        }

        place?.let { p ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("📍", fontSize = 18.sp)
                Column(Modifier.padding(start = 8.dp)) {
                    Text(p.name, fontWeight = FontWeight.SemiBold)
                    Text(
                        placeSubtitle(p.displayArea, p.city, p.district, p.address, p.distanceM),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.secondary,
                    )
                }
            }
            Text(
                "从我的位置出发",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.secondary,
            )
        }

        val previewFailure = state.routePreviewFailure
        when {
            state.optionsLoading -> Row(
                Modifier.fillMaxWidth().padding(12.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                CircularProgressIndicator(Modifier.size(20.dp))
                Column {
                    Text("正在规划路线")
                    Text(
                        "获取当前位置与路况…",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.secondary,
                    )
                }
            }

            state.routeOptions.isNotEmpty() -> {
                Text("选择路线", style = MaterialTheme.typography.titleSmall)
                state.routeOptions.forEachIndexed { index, route ->
                    RouteOptionRow(
                        index = index,
                        title = RouteCandidate.fromRoutes(state.routeOptions)[index].title,
                        distanceText = RideFormatting.distance(route.totalDistanceM),
                        durationText = RideFormatting.duration(route.totalDurationS.toInt()),
                        traffic = RouteCandidate.fromRoutes(state.routeOptions)[index].trafficSummary,
                        selected = index == state.chosenOptionIndex,
                        onClick = { vm.selectOption(index) },
                    )
                }
                Text(
                    "高德驾车路线 · 预计时间会随路况变化",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.secondary,
                )
                SecondaryLink("地图与离线下载", "保存城市和沿途地图") { onOpenMapDownloads() }
            }

            previewFailure != null -> Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text("无法规划路线", fontWeight = FontWeight.SemiBold)
                    Text(
                        previewFailure,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.secondary,
                    )
                    Button(onClick = { vm.planRoutePreview() }) { Text("重试") }
                }
            }
        }

        Spacer(Modifier.weight(1f))
        PrimaryActionBar(
            title = when {
                state.optionsLoading -> "正在规划路线"
                state.routeOptions.isEmpty() -> "重新规划路线"
                else -> "开始导航"
            },
            enabled = !state.optionsLoading && state.routeOptions.isNotEmpty(),
            danger = false,
            loading = state.optionsLoading,
            onClick = { vm.start() },
        )
    }
}

@Composable
private fun RouteOptionRow(
    index: Int,
    title: String,
    distanceText: String,
    durationText: String,
    traffic: String,
    selected: Boolean,
    onClick: () -> Unit,
) {
    Card(
        Modifier.fillMaxWidth().clickable(onClick = onClick),
        colors = CardDefaults.cardColors(
            containerColor = if (selected) MaterialTheme.colorScheme.primaryContainer
            else MaterialTheme.colorScheme.surfaceVariant,
        ),
    ) {
        Row(Modifier.padding(14.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(5.dp)) {
                Text(
                    title,
                    color = if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary,
                )
                Text(durationText, fontSize = 22.sp, fontWeight = FontWeight.SemiBold)
                Text(distanceText, style = MaterialTheme.typography.bodySmall)
                Text(
                    "🚗 $traffic",
                    style = MaterialTheme.typography.bodySmall,
                    color = when (traffic) {
                        "拥堵较多" -> Color(0xFFC62828)
                        "部分路段缓行" -> Color(0xFFEF6C00)
                        "路况顺畅" -> Color(0xFF2E7D32)
                        else -> MaterialTheme.colorScheme.secondary
                    },
                )
            }
            Text(
                if (selected) "✓" else "○",
                fontSize = 22.sp,
                color = if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary,
            )
        }
    }
}

// MARK: - 导航中 (ride)

@Composable
private fun ActiveNavigationScreen(vm: NavViewModel, onOpenDevice: () -> Unit) {
    val state by vm.state.collectAsState()
    val hasEstimate = state.remainingM > 0 || state.remainingS > 0

    Column(
        Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(
                if (false) "演示导航" else "导航中",
                fontSize = 22.sp,
                fontWeight = FontWeight.Bold,
            )
            TextButton(onClick = onOpenDevice) { Text("圆屏") }
        }

        Card(Modifier.fillMaxWidth()) {
            Column(
                Modifier.fillMaxWidth().padding(18.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Box(
                    Modifier.size(72.dp).background(
                        MaterialTheme.colorScheme.primary.copy(alpha = 0.08f),
                        CircleShape,
                    ),
                    contentAlignment = Alignment.Center,
                ) { Text("➤", fontSize = 30.sp, color = MaterialTheme.colorScheme.primary) }
                Text(
                    navigationTitle(state.engineState, false),
                    fontSize = 20.sp,
                    fontWeight = FontWeight.SemiBold,
                    textAlign = TextAlign.Center,
                )
                Text(
                    navigationDetail(state, vm),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.secondary,
                    textAlign = TextAlign.Center,
                )
            }
        }

        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("🏁", fontSize = 18.sp)
            Column(Modifier.padding(start = 8.dp)) {
                Text(
                    if (false) "演示目的地" else "目的地",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.secondary,
                )
                Text(
                    state.selected?.name ?: "目的地",
                    fontWeight = FontWeight.SemiBold,
                )
            }
        }

        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
            RideMetric("剩余时间", if (hasEstimate) RideFormatting.duration(state.remainingS) else "—")
            RideMetric("剩余路程", if (hasEstimate) RideFormatting.distance(state.remainingM) else "—")
        }

        if (state.instruction.isNotBlank()) {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                    Text("当前指引", style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.secondary)
                    Text(state.instruction, fontWeight = FontWeight.Medium)
                    if (state.roadName.isNotBlank()) {
                        Text(
                            "${state.roadName} · ${state.distanceToManeuverM.toInt()} m",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.secondary,
                        )
                    }
                }
            }
        }

        Text("导航状态", style = MaterialTheme.typography.titleSmall)
        StatusRow(
            "我的圆屏",
            if (state.bleReady) "已连接" else "未连接",
            if (state.bleReady) Color(0xFF2E7D32) else MaterialTheme.colorScheme.secondary,
        )
        StatusRow(
            "手机定位",
            if (false) "演示位置" else if (state.hasFix) "已获取" else "获取中",
            if (state.hasFix || false) Color(0xFF2E7D32) else MaterialTheme.colorScheme.secondary,
        )

        if (false) {
            Text(
                "演示中的位置与行驶过程为模拟数据。道路数据 © OpenStreetMap contributors",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.secondary,
            )
        }

        Spacer(Modifier.weight(1f))
        PrimaryActionBar(
            title = "结束导航",
            enabled = true,
            danger = true,
            loading = false,
            onClick = { vm.stop() },
        )
    }
}

@Composable
private fun androidx.compose.foundation.layout.RowScope.RideMetric(title: String, value: String) {
    Column(Modifier.weight(1f)) {
        Text(value, fontSize = 24.sp, fontWeight = FontWeight.SemiBold)
        Text(title, style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.secondary)
    }
}

private fun navigationTitle(engineState: Int, demo: Boolean): String = when (engineState) {
    1 -> "正在获取位置"
    2 -> "正在规划路线"
    3 -> if (demo) "正在演示" else "正在导航"
    4 -> "正在重新规划"
    5 -> "已到达目的地"
    else -> "准备出发"
}

private fun navigationDetail(state: NavigationSession.UiState, vm: NavViewModel): String = when {
    false && state.engineState == 3 ->
        "预览行驶过程与圆屏上的导航指引。"
    state.engineState == 3 ->
        if (state.bleReady) "圆屏已连接，手机可锁屏收好。"
        else "圆屏连接后，导航指引会自动同步。"
    state.engineState == 4 -> "新路线生成后会自动同步到圆屏"
    state.engineState == 5 -> "本次导航已经完成"
    state.engineState == 2 -> "正在读取高德实时路线与路况"
    state.engineState == 1 -> "请保持精确定位开启"
    else -> "选择终点后，路线会通过蓝牙发送到圆屏"
}

// MARK: - Shared pieces

@Composable
private fun PrimaryActionBar(
    title: String,
    enabled: Boolean,
    danger: Boolean,
    loading: Boolean,
    onClick: () -> Unit,
) {
    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = Modifier.fillMaxWidth().height(52.dp),
        shape = RoundedCornerShape(16.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = if (danger) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.primary,
        ),
    ) {
        if (loading) {
            CircularProgressIndicator(Modifier.size(18.dp), color = Color.White, strokeWidth = 2.dp)
            Spacer(Modifier.size(8.dp))
        }
        Text(title, fontWeight = FontWeight.SemiBold)
    }
}

@Composable
private fun SecondaryLink(title: String, subtitle: String, onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onClick).padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, fontWeight = FontWeight.Medium)
            if (subtitle.isNotEmpty()) {
                Text(
                    subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.secondary,
                )
            }
        }
        Text("›", color = MaterialTheme.colorScheme.secondary)
    }
}

@Composable
private fun StatusRow(title: String, value: String, valueColor: Color) {
    Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(title)
        Text(value, color = valueColor)
    }
}

// MARK: - Sheets

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DeviceSheet(vm: NavViewModel, bleState: Esp32BleCentral.State, onDismiss: () -> Unit) {
    val devices by vm.devices.collectAsState()
    val context = LocalContext.current
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text("我的圆屏", fontSize = 20.sp, fontWeight = FontWeight.Bold)
            Text(
                when (bleState) {
                    is Esp32BleCentral.State.Connected -> "已连接"
                    is Esp32BleCentral.State.Scanning -> "正在寻找圆屏"
                    is Esp32BleCentral.State.Connecting -> "正在连接"
                    is Esp32BleCentral.State.Failed -> "连接未成功"
                    is Esp32BleCentral.State.Unavailable -> "蓝牙不可用"
                    is Esp32BleCentral.State.Reconnecting -> "正在重连"
                    else -> "未连接"
                },
                color = MaterialTheme.colorScheme.secondary,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = { vm.startScan() }) { Text("扫描") }
                OutlinedButton(onClick = { vm.disconnect() }) { Text("断开") }
                OutlinedButton(onClick = {
                    runCatching {
                        context.startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
                    }
                }) { Text("蓝牙设置") }
            }
            LazyColumn(Modifier.weight(1f, fill = false)) {
                items(devices) { device ->
                    Card(Modifier.fillMaxWidth().clickable { vm.connect(device.address) }) {
                        Row(Modifier.padding(12.dp), horizontalArrangement = Arrangement.SpaceBetween) {
                            Column {
                                Text(device.name ?: "(未命名)")
                                Text(device.address, style = MaterialTheme.typography.bodySmall)
                            }
                            Text("${device.rssi} dBm", style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            }
            TextButton(onClick = onDismiss) { Text("完成") }
            Spacer(Modifier.height(12.dp))
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun GatewaySheet(vm: NavViewModel, onDismiss: () -> Unit) {
    val current = remember { vm.gatewayCurrent() }
    var address by remember { mutableStateOf(current) }
    var message by remember { mutableStateOf<String?>(null) }
    var checking by remember { mutableStateOf(false) }
    val context = LocalContext.current

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text("网关设置", fontSize = 20.sp, fontWeight = FontWeight.Bold)
            OutlinedTextField(
                value = address,
                onValueChange = { address = it; message = null },
                modifier = Modifier.fillMaxWidth(),
                placeholder = { Text("https://你的网关地址/") },
                singleLine = true,
            )
            Text(
                "填写完整的 HTTPS 根地址，不要填高德 Key。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.secondary,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    enabled = address.isNotBlank() && !checking,
                    onClick = {
                        checking = true
                        message = null
                        // healthz probe with the candidate URL.
                        Thread {
                            val result = runCatching {
                                val normalized = vm.gatewaySave(address)
                                val url = normalized.trimEnd('/') + "/healthz"
                                val connection = java.net.URI(url).toURL().openConnection()
                                    as java.net.HttpURLConnection
                                connection.connectTimeout = 10_000
                                connection.readTimeout = 10_000
                                val code = connection.responseCode
                                val body = connection.inputStream?.use {
                                    it.readBytes().toString(Charsets.UTF_8)
                                }.orEmpty()
                                if (code != 200) throw IllegalStateException("HTTP $code")
                                when {
                                    body.contains("\"ready_for_live_navigation\":true") ->
                                        "网关已连接，已启用实时导航配置。"
                                    body.contains("\"status\":\"ok\"") ->
                                        "网关已连接，但尚未启用实时导航。请检查服务端配置。"
                                    else -> throw IllegalStateException("unexpected")
                                }
                            }.getOrElse { "连接失败，请检查地址、HTTPS 证书和网关是否已启动。" }
                            message = result
                            checking = false
                        }.start()
                    },
                ) { Text("测试连接") }
                Button(
                    enabled = address.isNotBlank(),
                    onClick = {
                        runCatching { vm.gatewaySave(address) }
                            .onSuccess {
                                android.widget.Toast.makeText(
                                    context, "已保存，立即生效", android.widget.Toast.LENGTH_SHORT,
                                ).show()
                                onDismiss()
                            }
                            .onFailure { message = it.message }
                    },
                ) { Text("保存") }
            }
            message?.let { Text(it, color = MaterialTheme.colorScheme.secondary) }
            Text(
                "保存后立即生效，无需重新安装。地点、路线和定位请求将发送到这个地址，请使用你信任的网关。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.secondary,
            )
            TextButton(onClick = onDismiss) { Text("完成") }
            Spacer(Modifier.height(12.dp))
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PrivacySheet(onDismiss: () -> Unit) {
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            Modifier.padding(16.dp)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text("隐私与数据", fontSize = 20.sp, fontWeight = FontWeight.Bold)
            PrivacySection("位置与路线", listOf(
                "地点搜索、规划和重新规划路线时，搜索内容、起终点及所需位置会发送给配置的导航服务。",
                "导航期间，定位用于更新路线和圆屏指引；锁屏或切换应用后仍可继续。结束导航会停止导航定位。",
            ))
            PrivacySection("蓝牙与音乐", listOf(
                "连接圆屏后，导航、速度及周边地图通过蓝牙同步。",
                "授权通知使用权后，系统媒体会话的曲名、歌手和播放状态会同步到圆屏，圆屏可控制上一首、播放或暂停、下一首。音乐信息不发送到导航服务器。",
            ))
            PrivacySection("存储与清理", listOf(
                "最近地点、已连接设备标识和下载地图保存在手机上。地图页可删除下载包。",
                "服务器访问日志可能保留 IP、请求时间、搜索词、位置参数及地图区域，由部署方的隐私政策说明。",
            ))
            Text(
                "圆屏周边道路与建筑使用 OpenStreetMap / Protomaps 数据（ODbL-1.0）。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.secondary,
            )
            TextButton(onClick = onDismiss) { Text("完成") }
            Spacer(Modifier.height(12.dp))
        }
    }
}

@Composable
private fun PrivacySection(title: String, paragraphs: List<String>) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(title, fontWeight = FontWeight.SemiBold)
        paragraphs.forEach { Text(it, style = MaterialTheme.typography.bodySmall) }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun MapDownloadsSheet(vm: NavViewModel, onDismiss: () -> Unit) {
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text("地图与离线下载", fontSize = 20.sp, fontWeight = FontWeight.Bold)
            Text(
                "骑行时自动加载圆屏周边的道路和建筑；城市和沿途下载在「地图」标签页管理。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.secondary,
            )
            TextButton(onClick = onDismiss) { Text("完成") }
            Spacer(Modifier.height(12.dp))
        }
    }
}
