package dev.glimpse.motogps.ui

import android.content.Intent
import android.provider.Settings
import androidx.compose.animation.animateColorAsState
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
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
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
import dev.glimpse.motogps.nav.SavedPlace

/**
 * High-fidelity port of the iOS single-flow UI (ContentView.swift):
 * 出发 (destination search) → 路线 (route preview) → 导航中 (ride).
 * Device details, map downloads, gateway settings and data use are sheets,
 * exactly like the iOS app.
 */

// iOS system colors used by the SwiftUI design.
private val IosBlue = Color(0xFF007AFF)
private val IosGreen = Color(0xFF34C759)
private val IosRed = Color(0xFFFF3B30)
private val IosOrange = Color(0xFFFF9500)
private val IosSecondary = Color(0xFF8E8E93)
private val IosSeparator = Color(0x1F000000)
private val IosGroupedBackground = Color(0xFFF2F2F7)
private val IosCard = Color.White

@Composable
fun MotoAppScreen(vm: NavViewModel = viewModel()) {
    val state by vm.state.collectAsState()
    val bleState by vm.bleState.collectAsState()

    val screen = when {
        state.isNavigating -> MotoScreen.ActiveNavigation
        state.selected != null -> MotoScreen.RoutePreview
        else -> MotoScreen.Home
    }

    var showDevice by remember { mutableStateOf(false) }
    var showGateway by remember { mutableStateOf(false) }
    var showPrivacy by remember { mutableStateOf(false) }
    var showMapDownloads by remember { mutableStateOf(false) }

    Surface(color = IosGroupedBackground) {
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
            MotoScreen.ActiveNavigation -> ActiveNavigationScreen(
                vm,
                onOpenDevice = { showDevice = true },
                onOpenMapDownloads = { showMapDownloads = true },
            )
        }
    }

    if (showDevice) DeviceDetailsSheet(vm, bleState, onDismiss = { showDevice = false })
    if (showGateway) GatewaySettingsSheet(vm, onDismiss = { showGateway = false })
    if (showPrivacy) DataUseSheet(onDismiss = { showPrivacy = false })
    if (showMapDownloads) MapDownloadsSheet(onDismiss = { showMapDownloads = false })
}

enum class MotoScreen { Home, RoutePreview, ActiveNavigation }

// MARK: - Grouped-list building blocks (List + insetGrouped equivalents)

@Composable
private fun GroupedList(
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    Column(modifier.fillMaxSize().verticalScroll(rememberScrollState())) { content() }
}

@Composable
private fun GroupSection(
    header: String? = null,
    footer: String? = null,
    content: @Composable () -> Unit,
) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
        if (!header.isNullOrEmpty()) {
            Text(
                header.uppercase(),
                fontSize = 13.sp,
                color = IosSecondary,
                modifier = Modifier.padding(start = 16.dp, top = 18.dp, bottom = 7.dp),
            )
        }
        Surface(
            shape = RoundedCornerShape(10.dp),
            color = IosCard,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Column { content() }
        }
        if (!footer.isNullOrEmpty()) {
            Text(
                footer,
                fontSize = 13.sp,
                color = IosSecondary,
                modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 7.dp, bottom = 4.dp),
            )
        } else {
            Spacer(Modifier.height(18.dp))
        }
    }
}

@Composable
private fun RowDivider() = Box(
    Modifier
        .padding(start = 16.dp)
        .fillMaxWidth()
        .height(0.5.dp)
        .background(IosSeparator),
)

/** LabeledContent(statusRow) equivalent. */
@Composable
private fun StatusRow(title: String, icon: String, value: String, valueColor: Color) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 11.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
            Text(icon, fontSize = 16.sp)
            Spacer(Modifier.width(8.dp))
            Text(title, fontSize = 16.sp, color = Color(0xFF000000))
        }
        Text(value, fontSize = 16.sp, color = valueColor, textAlign = TextAlign.End)
    }
}

@Composable
private fun LabelFooter(text: String, modifier: Modifier = Modifier) {
    Text(
        text,
        fontSize = 13.sp,
        color = IosSecondary,
        modifier = modifier.padding(start = 16.dp, end = 16.dp, top = 7.dp, bottom = 4.dp),
    )
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

    Column(Modifier.fillMaxSize()) {
        // Navigation bar
        Box(Modifier.fillMaxWidth().height(52.dp)) {
            Text(
                "出发",
                fontSize = 17.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.align(Alignment.Center),
            )
        }

        GroupedList(Modifier.weight(1f)) {
            if (!vm.gatewayCustomised()) {
                GroupSection(
                    footer = "填写你部署的网关地址后，就可以搜索地点和规划路线。",
                ) {
                    Row(
                        Modifier.fillMaxWidth()
                            .clickable { onOpenGateway() }
                            .padding(horizontal = 16.dp, vertical = 11.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text("🌐", fontSize = 16.sp)
                        Spacer(Modifier.width(8.dp))
                        Text("设置导航网关", fontSize = 16.sp, color = IosBlue)
                    }
                }
            }
            GroupSection {
                SearchField(
                    query = state.query,
                    onQuery = { vm.queryChanged(it) },
                    onSubmit = { vm.submitSearch() },
                    onClear = { vm.clearQuery() },
                )
            }
            if (state.query.isNotEmpty()) {
                GroupSection(header = "搜索结果") {
                    when {
                        state.isSearching -> Row(
                            Modifier.fillMaxWidth().heightIn(min = 56.dp).padding(horizontal = 16.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(12.dp),
                        ) {
                            CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                            Text("正在搜索…", fontSize = 16.sp, color = IosSecondary)
                        }

                        state.places.isNotEmpty() -> Column {
                            state.places.forEachIndexed { index, place ->
                                if (index > 0) RowDivider()
                                PlaceRow(place, symbol = "📍", tint = IosBlue) { vm.select(place) }
                            }
                        }

                        else -> EmptyState(
                            title = if (state.searchFailure == null) "输入地点名称" else "暂无结果",
                            icon = "🔍",
                            description = state.searchFailure ?: "至少输入两个字，例如“奥体中心”。",
                            action = if (state.searchFailure != null) {
                                { vm.submitSearch() }
                            } else null,
                            actionLabel = "重新搜索",
                        )
                    }
                }
                LabelFooter(
                    when {
                        state.hasFix -> "已按当前位置优先排序"
                        state.searchFailure != null -> "未获定位权限；暂按全国搜索"
                        else -> "正在获取当前位置；暂按全国搜索"
                    },
                    modifier = Modifier.padding(horizontal = 16.dp),
                )
            } else {
                if (recentPlaces.isEmpty()) {
                    EmptyState(
                        title = "选择目的地",
                        icon = "🗺",
                        description = "搜索地点或地址，选好路线就出发。",
                    )
                    Spacer(Modifier.height(12.dp))
                } else {
                    GroupSection(header = "最近搜索") {
                        recentPlaces.forEachIndexed { index, place ->
                            if (index > 0) RowDivider()
                            RecentRow(place) { vm.selectRecent(place) }
                        }
                    }
                }
                GroupSection(footer = "连接圆屏后，导航指引会自动同步。") {
                    DeviceSummaryRow(vm) { onOpenDevice() }
                    RowDivider()
                    MapDownloadsRow { onOpenMapDownloads() }
                    RowDivider()
                    Row(
                        Modifier.fillMaxWidth().clickable { vm.startDemo() }.padding(horizontal = 16.dp, vertical = 11.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text("▶️", fontSize = 20.sp)
                        Spacer(Modifier.width(14.dp))
                        Column(Modifier.weight(1f)) {
                            Text("演示导航", fontSize = 16.sp, color = Color(0xFF000000))
                            Text("先体验一次导航流程", fontSize = 13.sp, color = IosSecondary)
                        }
                    }
                }
                GroupSection {
                    Row(
                        Modifier.fillMaxWidth().clickable { onOpenGateway() }
                            .padding(horizontal = 16.dp, vertical = 11.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text("🌐", fontSize = 16.sp)
                        Spacer(Modifier.width(8.dp))
                        Text("网关设置", fontSize = 16.sp, color = Color(0xFF000000))
                    }
                    RowDivider()
                    Row(
                        Modifier.fillMaxWidth().clickable { onOpenPrivacy() }
                            .padding(horizontal = 16.dp, vertical = 11.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text("✋", fontSize = 16.sp)
                        Spacer(Modifier.width(8.dp))
                        Text("隐私与数据", fontSize = 16.sp, color = Color(0xFF000000))
                    }
                }
            }
        }
    }
}

@Composable
private fun SearchField(
    query: String,
    onQuery: (String) -> Unit,
    onSubmit: () -> Unit,
    onClear: () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth().heightIn(min = 44.dp).padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("🔍", fontSize = 16.sp, color = IosSecondary)
        androidx.compose.foundation.text.BasicTextField(
            value = query,
            onValueChange = onQuery,
            modifier = Modifier.weight(1f),
            singleLine = true,
            textStyle = androidx.compose.ui.text.TextStyle(fontSize = 16.sp, color = Color(0xFF000000)),
            keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(
                imeAction = androidx.compose.ui.text.input.ImeAction.Search,
            ),
            keyboardActions = androidx.compose.foundation.text.KeyboardActions(onSearch = { onSubmit() }),
            decorationBox = { inner ->
                Box {
                    if (query.isEmpty()) {
                        Text("搜索地点或地址", fontSize = 16.sp, color = IosSecondary)
                    }
                    inner()
                }
            },
        )
        if (query.isNotEmpty()) {
            Text(
                "✕",
                fontSize = 15.sp,
                color = IosSecondary,
                modifier = Modifier
                    .clickable { onClear() }
                    .padding(8.dp),
            )
        }
    }
}

@Composable
private fun EmptyState(
    title: String,
    icon: String,
    description: String,
    action: (() -> Unit)? = null,
    actionLabel: String = "",
) {
    Column(
        Modifier.fillMaxWidth().padding(horizontal = 32.dp, vertical = 24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text(icon, fontSize = 34.sp)
        Text(title, fontSize = 17.sp, fontWeight = FontWeight.SemiBold, textAlign = TextAlign.Center)
        Text(
            description,
            fontSize = 13.sp,
            color = IosSecondary,
            textAlign = TextAlign.Center,
        )
        if (action != null) {
            TextButton(onClick = action) { Text(actionLabel) }
        }
    }
}

@Composable
private fun PlaceRow(place: Place, symbol: String, tint: Color, onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 16.dp, vertical = 9.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(symbol, fontSize = 19.sp, color = tint, modifier = Modifier.width(28.dp))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(place.name, fontSize = 16.sp, color = Color(0xFF000000))
            Text(
                placeSubtitle(place),
                fontSize = 13.sp,
                color = IosSecondary,
                maxLines = 2,
            )
        }
        Text("›", fontSize = 14.sp, color = IosSecondary.copy(alpha = 0.6f))
    }
}

@Composable
private fun RecentRow(place: SavedPlace, onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 16.dp, vertical = 9.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("🕘", fontSize = 19.sp, color = IosSecondary, modifier = Modifier.width(28.dp))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(place.name, fontSize = 16.sp, color = Color(0xFF000000))
            Text(place.subtitle(), fontSize = 13.sp, color = IosSecondary, maxLines = 2)
        }
        Text("›", fontSize = 14.sp, color = IosSecondary.copy(alpha = 0.6f))
    }
}

private fun placeSubtitle(place: Place): String {
    val area = place.displayArea.ifEmpty {
        listOf(place.city, place.district).filter { it.isNotEmpty() }.joinToString(" · ")
    }
    val parts = mutableListOf<String>()
    if (area.isNotEmpty()) parts += area
    if (place.address.isNotEmpty()) parts += place.address
    place.distanceM?.takeIf { it >= 0 }?.let { distance ->
        parts += if (distance >= 1_000) {
            String.format(java.util.Locale.US, "距你 %.1f km", distance / 1_000)
        } else {
            "距你 ${distance.toInt()} m"
        }
    }
    return parts.joinToString(" · ").ifEmpty { "查看路线" }
}

@Composable
private fun DeviceSummaryRow(vm: NavViewModel, onClick: () -> Unit) {
    val bleState by vm.bleState.collectAsState()
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("◎", fontSize = 19.sp, color = IosBlue, modifier = Modifier.width(24.dp))
        Spacer(Modifier.width(14.dp))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text("我的圆屏", fontSize = 16.sp, color = Color(0xFF000000))
            Text(deviceStatus(bleState), fontSize = 13.sp, color = IosSecondary)
        }
        if (bleState is Esp32BleCentral.State.Connected) {
            Text("✓", fontSize = 17.sp, color = IosGreen)
            Spacer(Modifier.width(6.dp))
        }
        Text("›", fontSize = 14.sp, color = IosSecondary.copy(alpha = 0.6f))
    }
}

@Composable
private fun MapDownloadsRow(onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("🗺", fontSize = 20.sp, color = IosBlue)
        Spacer(Modifier.width(14.dp))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text("地图与离线下载", fontSize = 16.sp, color = Color(0xFF000000))
            Text("自动加载周边，也能提前保存城市和沿途地图", fontSize = 13.sp, color = IosSecondary)
        }
    }
}

private fun deviceStatus(state: Esp32BleCentral.State): String = when (state) {
    is Esp32BleCentral.State.Connected -> "已连接"
    is Esp32BleCentral.State.Scanning -> "正在寻找圆屏"
    is Esp32BleCentral.State.Connecting -> "正在连接"
    is Esp32BleCentral.State.Handshaking -> "正在连接"
    is Esp32BleCentral.State.Failed -> "连接未成功"
    is Esp32BleCentral.State.Unavailable -> "蓝牙不可用"
    is Esp32BleCentral.State.Reconnecting -> "正在连接"
    else -> "未连接"
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

    Column(Modifier.fillMaxSize()) {
        // Inline navigation bar with a trailing 更换 button.
        Box(Modifier.fillMaxWidth().height(52.dp)) {
            Text(
                "路线",
                fontSize = 17.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.align(Alignment.Center),
            )
            TextButton(
                onClick = { vm.clearDestination() },
                modifier = Modifier.align(Alignment.CenterEnd),
            ) { Text("更换", color = IosBlue) }
        }

        GroupedList(Modifier.weight(1f)) {
            place?.let { p ->
                GroupSection(header = "从我的位置出发") {
                    Row(
                        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text("📍", fontSize = 22.sp, color = IosRed)
                        Spacer(Modifier.width(12.dp))
                        Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                            Text(p.name, fontSize = 17.sp, fontWeight = FontWeight.SemiBold)
                            Text(placeSubtitle(p), fontSize = 13.sp, color = IosSecondary)
                        }
                    }
                }

                val candidates = RouteCandidate.fromRoutes(state.routeOptions)
                val selectedId = candidates.getOrNull(state.chosenOptionIndex)?.id
                when {
                    state.optionsLoading -> GroupSection {
                        Row(
                            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 14.dp),
                            horizontalArrangement = Arrangement.spacedBy(14.dp),
                        ) {
                            CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                            Column {
                                Text("正在规划路线", fontSize = 16.sp)
                                Text("获取当前位置与路况…", fontSize = 13.sp, color = IosSecondary)
                            }
                        }
                    }

                    candidates.isNotEmpty() -> {
                        GroupSection {
                            RouteOverviewMap(
                                candidates = candidates,
                                selectedId = selectedId,
                                origin = vm.routePreviewOrigin,
                                destination = p.longitude to p.latitude,
                                modifier = Modifier.fillMaxWidth().height(270.dp),
                            )
                        }
                        GroupSection(
                            header = "选择路线",
                            footer = "高德驾车路线 · 预计时间会随路况变化",
                        ) {
                            candidates.forEachIndexed { index, candidate ->
                                if (index > 0) RowDivider()
                                RouteOptionRow(
                                    candidate = candidate,
                                    selected = index == state.chosenOptionIndex,
                                    onClick = { vm.selectOption(index) },
                                )
                            }
                        }
                        GroupSection { MapDownloadsRow(onOpenMapDownloads) }
                    }

                    state.routePreviewFailure != null -> GroupSection {
                        val failureText = state.routePreviewFailure ?: ""
                        EmptyState(
                            title = "无法规划路线",
                            icon = "⚠️",
                            description = failureText,
                        )
                    }
                }
                state.navigationFailure?.let { failure ->
                    GroupSection {
                        Text(
                            "⚠️ $failure",
                            fontSize = 13.sp,
                            color = IosSecondary,
                            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                        )
                    }
                }
            }
        }

        // No filler Spacer: the scrollable list owns the remaining height
        // (weight 1f) and the action bar stays pinned below it.
        NavigationActionBar(vm, state)
    }
}

@Composable
private fun RouteOptionRow(candidate: RouteCandidate, selected: Boolean, onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 16.dp, vertical = 9.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(5.dp)) {
            Text(
                candidate.title,
                fontSize = 15.sp,
                color = if (selected) IosBlue else IosSecondary,
            )
            Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(
                    candidate.durationText,
                    fontSize = 22.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = Color(0xFF000000),
                )
                Text(candidate.distanceText, fontSize = 13.sp, color = IosSecondary)
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("🚗", fontSize = 13.sp)
                Text(
                    candidate.trafficSummary,
                    fontSize = 13.sp,
                    color = when (candidate.trafficSummary) {
                        "拥堵较多" -> IosRed
                        "部分路段缓行" -> IosOrange
                        "路况顺畅" -> IosGreen
                        else -> IosSecondary
                    },
                )
            }
        }
        Text(
            if (selected) "✓" else "○",
            fontSize = 22.sp,
            color = if (selected) IosBlue else IosSecondary.copy(alpha = 0.45f),
        )
    }
}

// MARK: - 导航中 (ride)

@Composable
private fun ActiveNavigationScreen(
    vm: NavViewModel,
    onOpenDevice: () -> Unit,
    onOpenMapDownloads: () -> Unit,
) {
    val state by vm.state.collectAsState()
    val hasEstimate = state.remainingM > 0 || state.remainingS > 0

    Column(Modifier.fillMaxSize()) {
        Box(Modifier.fillMaxWidth().height(52.dp)) {
            Text(
                "导航中",
                fontSize = 17.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.align(Alignment.Center),
            )
        }

        GroupedList(Modifier.weight(1f)) {
            GroupSection {
                Column(
                    Modifier.fillMaxWidth().padding(vertical = 18.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Box(
                        Modifier.size(76.dp).background(IosBlue.copy(alpha = 0.08f), androidx.compose.foundation.shape.CircleShape),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(navigationSymbol(state), fontSize = 34.sp, color = if (state.navigationFailure == null) IosBlue else IosOrange)
                    }
                    Text(
                        navigationTitle(state),
                        fontSize = 22.sp,
                        fontWeight = FontWeight.SemiBold,
                        textAlign = TextAlign.Center,
                    )
                    Text(
                        navigationDetail(state),
                        fontSize = 13.sp,
                        color = IosSecondary,
                        textAlign = TextAlign.Center,
                    )
                }
            }
            GroupSection {
                Row(
                    Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("🏁", fontSize = 20.sp, color = IosBlue)
                    Spacer(Modifier.width(12.dp))
                    Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                        Text("目的地", fontSize = 13.sp, color = IosSecondary)
                        Text(state.selected?.name ?: "目的地", fontSize = 17.sp, fontWeight = FontWeight.SemiBold)
                    }
                }
                RowDivider()
                Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp)) {
                    RideMetric("剩余时间", if (hasEstimate) RideFormatting.duration(state.remainingS) else "—", Modifier.weight(1f))
                    Spacer(Modifier.width(24.dp))
                    RideMetric("剩余路程", if (hasEstimate) RideFormatting.distance(state.remainingM) else "—", Modifier.weight(1f))
                }
            }
            GroupSection(header = "导航状态") {
                DeviceSummaryRow(vm) { onOpenDevice() }
                RowDivider()
                StatusRow(
                    "手机定位", "📍",
                    if (state.hasFix) "已获取" else "获取中",
                    if (state.hasFix) IosGreen else IosSecondary,
                )
                RowDivider()
                StatusRow(
                    "路况", "🚗",
                    if (state.remainingS > 0) "在线" else "离线",
                    IosSecondary,
                )
                RowDivider()
                Row(
                    Modifier.fillMaxWidth().clickable { onOpenMapDownloads() }
                        .padding(horizontal = 16.dp, vertical = 11.dp),
                ) {
                    Text("管理离线地图", fontSize = 16.sp, color = IosBlue)
                }
            }
            if (state.instruction.isNotBlank()) {
                GroupSection {
                    Column(Modifier.padding(horizontal = 16.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                        Text("当前指引", fontSize = 13.sp, color = IosSecondary)
                        Text(state.instruction, fontSize = 15.sp, fontWeight = FontWeight.Medium)
                        if (state.roadName.isNotBlank()) {
                            Text(
                                "${state.roadName} · ${state.distanceToManeuverM.toInt()} m",
                                fontSize = 13.sp,
                                color = IosSecondary,
                            )
                        }
                    }
                }
            }
        }

        // No filler Spacer: the scrollable list owns the remaining height
        // (weight 1f) and the action bar stays pinned below it.
        NavigationActionBar(vm, state)
    }
}

@Composable
private fun androidx.compose.foundation.layout.RowScope.RideMetric(title: String, value: String, modifier: Modifier = Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(value, fontSize = 20.sp, fontWeight = FontWeight.SemiBold, color = Color(0xFF000000))
        Text(title, fontSize = 13.sp, color = IosSecondary)
    }
}

// MARK: - Primary action bar (safeAreaInset bottom)

@Composable
private fun NavigationActionBar(vm: NavViewModel, state: NavigationSession.UiState) {
    val active = state.isNavigating
    val title = when {
        active -> "结束导航"
        state.optionsLoading -> "正在规划路线"
        state.selected == null -> "请先选择终点"
        state.routeOptions.isEmpty() -> "重新规划路线"
        else -> "开始导航"
    }
    val loading = !active && state.optionsLoading
    val enabled = active || (state.selected != null && !state.optionsLoading && state.routeOptions.isNotEmpty())

    Surface(color = Color(0xF7F2F2F7), shadowElevation = 8.dp) {
        Column(Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 12.dp)) {
            Button(
                onClick = { if (active) vm.stop() else vm.start() },
                enabled = enabled,
                modifier = Modifier.fillMaxWidth().height(50.dp),
                shape = RoundedCornerShape(16.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = if (active) IosRed else IosBlue,
                    disabledContainerColor = IosBlue.copy(alpha = 0.3f),
                ),
            ) {
                if (loading) {
                    CircularProgressIndicator(Modifier.size(18.dp), color = Color.White, strokeWidth = 2.dp)
                    Spacer(Modifier.width(9.dp))
                }
                Text(
                    if (active) "■ " else "➤ ",
                    fontSize = 16.sp,
                )
                Text(title, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
            }
        }
    }
}

private fun navigationTitle(state: NavigationSession.UiState): String {
    if (state.navigationFailure != null) return "导航需要处理"
    return when (state.engineState) {
        1 -> "正在获取位置"
        2 -> "正在规划路线"
        3 -> "正在导航"
        4 -> "正在重新规划"
        5 -> "已到达目的地"
        else -> "准备出发"
    }
}

private fun navigationDetail(state: NavigationSession.UiState): String {
    state.navigationFailure?.let { return it }
    return when (state.engineState) {
        3 -> if (state.bleReady) "圆屏已连接，手机可锁屏收好。" else "圆屏连接后，导航指引会自动同步。"
        4 -> "新路线生成后会自动同步到圆屏"
        5 -> "本次导航已经完成"
        2 -> "正在读取高德实时路线与路况"
        1 -> "请保持精确定位开启"
        else -> "选择终点后，路线会通过蓝牙发送到圆屏"
    }
}

private fun navigationSymbol(state: NavigationSession.UiState): String = when {
    state.navigationFailure != null -> "⚠️"
    state.engineState == 5 -> "🏁"
    state.engineState == 4 -> "🔄"
    state.engineState == 3 -> "🧭"
    else -> "📍"
}

// MARK: - Device details sheet (iOS deviceDetails)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DeviceDetailsSheet(vm: NavViewModel, bleState: Esp32BleCentral.State, onDismiss: () -> Unit) {
    val devices by vm.devices.collectAsState()
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(bottom = 24.dp)) {
            Box(Modifier.fillMaxWidth().height(48.dp)) {
                Text(
                    "我的圆屏",
                    fontSize = 17.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.align(Alignment.Center),
                )
                TextButton(
                    onClick = onDismiss,
                    modifier = Modifier.align(Alignment.CenterEnd),
                ) { Text("完成", color = IosBlue) }
            }

            GroupSection { }
            GroupSection {
                Column(
                    Modifier.fillMaxWidth().padding(vertical = 14.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Text("🧭", fontSize = 60.sp, color = IosBlue)
                    val name = when (bleState) {
                        is Esp32BleCentral.State.Connected -> "MOTO GPS"
                        is Esp32BleCentral.State.Connecting -> "MOTO GPS"
                        else -> "MOTO GPS"
                    }
                    Text(name, fontSize = 22.sp, fontWeight = FontWeight.SemiBold)
                    Text(
                        deviceDetail(bleState),
                        fontSize = 13.sp,
                        color = IosSecondary,
                        textAlign = TextAlign.Center,
                        modifier = Modifier.padding(horizontal = 32.dp),
                    )
                }
            }
            GroupSection(footer = "圆屏保持开机并靠近手机。连接成功后，当前导航会自动同步。") {
                StatusRow(
                    "连接状态", "📡",
                    deviceStatus(bleState),
                    if (bleState is Esp32BleCentral.State.Connected) IosGreen else IosSecondary,
                )
                RowDivider()
                val connected = bleState is Esp32BleCentral.State.Connected
                val scanning = bleState is Esp32BleCentral.State.Scanning
                val context = LocalContext.current
                Row(
                    Modifier.fillMaxWidth().clickable {
                        when {
                            connected -> vm.disconnect()
                            scanning -> vm.stopScan()
                            else -> vm.startScan()
                        }
                    }.padding(horizontal = 16.dp, vertical = 11.dp),
                ) {
                    Text(
                        when {
                            connected -> "断开连接"
                            scanning -> "停止搜索"
                            bleState is Esp32BleCentral.State.Connecting -> "取消连接"
                            bleState is Esp32BleCentral.State.Failed -> "重新连接"
                            else -> "连接圆屏"
                        },
                        fontSize = 16.sp,
                        color = IosBlue,
                    )
                }
                if (bleState is Esp32BleCentral.State.Unavailable) {
                    RowDivider()
                    Row(
                        Modifier.fillMaxWidth().clickable {
                            runCatching { context.startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS)) }
                        }.padding(horizontal = 16.dp, vertical = 11.dp),
                    ) { Text("打开系统设置", fontSize = 16.sp, color = IosBlue) }
                }
                // Discovered devices (Android-specific affordance, kept minimal).
                if (devices.isNotEmpty() && !connected) {
                    devices.forEach { device ->
                        RowDivider()
                        Row(
                            Modifier.fillMaxWidth().clickable { vm.connect(device.address) }
                                .padding(horizontal = 16.dp, vertical = 11.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Column(Modifier.weight(1f)) {
                                Text(device.name ?: "(未命名)", fontSize = 15.sp)
                                Text(device.address, fontSize = 12.sp, color = IosSecondary)
                            }
                            Text("${device.rssi} dBm", fontSize = 12.sp, color = IosSecondary)
                        }
                    }
                }
            }
            GroupSection(header = "定位") {
                StatusRow("搜索位置", "📍", "已获取", IosSecondary)
            }
        }
    }
}

private fun deviceDetail(state: Esp32BleCentral.State): String = when (state) {
    is Esp32BleCentral.State.Connected ->
        "圆屏已就绪，可以接收导航指引。"
    is Esp32BleCentral.State.Failed ->
        "暂时无法连接。请确认圆屏已开机并靠近手机，再试一次。"
    is Esp32BleCentral.State.Unavailable ->
        "请开启手机蓝牙，并允许 MOTO GPS 使用蓝牙。"
    is Esp32BleCentral.State.Scanning, is Esp32BleCentral.State.Connecting, is Esp32BleCentral.State.Handshaking ->
        "请将已开机的圆屏放在手机附近。"
    is Esp32BleCentral.State.Reconnecting ->
        "正在重新连接圆屏。"
    else -> "连接你的圆屏，在车把上查看导航。"
}

// MARK: - Gateway settings sheet (iOS GatewaySettingsView)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun GatewaySettingsSheet(vm: NavViewModel, onDismiss: () -> Unit) {
    var address by remember { mutableStateOf(vm.gatewayCurrent()) }
    var message by remember { mutableStateOf<String?>(null) }
    var checking by remember { mutableStateOf(false) }
    val navigationActive = vm.isNavigationActive

    ModalBottomSheet(onDismissRequest = { if (!checking) onDismiss() }) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(bottom = 24.dp)) {
            Box(Modifier.fillMaxWidth().height(48.dp)) {
                Text(
                    "网关设置",
                    fontSize = 17.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.align(Alignment.Center),
                )
                Row(Modifier.align(Alignment.CenterEnd)) {
                    TextButton(onClick = onDismiss, enabled = !checking) { Text("取消", color = IosSecondary) }
                    TextButton(
                        enabled = !navigationActive && !checking && address.isNotBlank(),
                        onClick = {
                            checking = false
                            runCatching { vm.gatewaySave(address) }
                                .onSuccess { onDismiss() }
                                .onFailure { message = it.message }
                        },
                    ) { Text("保存", color = IosBlue) }
                }
            }

            GroupSection(header = "网关地址", footer = "填写完整的 HTTPS 根地址。例如 https://nav.example.com/moto-gps/api/，不要填高德 Key。") {
                OutlinedTextField(
                    value = address,
                    onValueChange = { address = it; message = null },
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                    placeholder = { Text("https://你的网关地址/") },
                    singleLine = true,
                    enabled = !checking,
                )
            }
            GroupSection(footer = "连接测试只检查网关状态；真实搜索、导航和地图还需要相应服务可用。") {
                Row(
                    Modifier.fillMaxWidth().clickable(enabled = !checking && address.isNotBlank()) {
                        checking = true
                        message = null
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
                    }.padding(horizontal = 16.dp, vertical = 11.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("测试连接", fontSize = 16.sp, color = IosBlue)
                    if (checking) {
                        Spacer(Modifier.weight(1f))
                        CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                    }
                }
                message?.let {
                    RowDivider()
                    Text(
                        it,
                        fontSize = 15.sp,
                        color = IosSecondary,
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                    )
                }
            }
            GroupSection {
                Text(
                    "保存后立即生效，无需重新安装。更换地址会清空当前搜索并暂停地图下载，已下载的地图仍会保留。",
                    fontSize = 15.sp,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
                )
                Text(
                    "地点、路线和定位请求将发送到这个地址，请使用你信任的网关。",
                    fontSize = 15.sp,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
                )
            }
            if (navigationActive) {
                GroupSection {
                    Text(
                        "请先结束导航，再更换网关地址。",
                        fontSize = 15.sp,
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                    )
                }
            }
        }
    }
}

// MARK: - Data use sheet (iOS DataUseView)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DataUseSheet(onDismiss: () -> Unit) {
    val context = LocalContext.current
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(bottom = 24.dp)) {
            Box(Modifier.fillMaxWidth().height(48.dp)) {
                Text(
                    "隐私与数据",
                    fontSize = 17.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.align(Alignment.Center),
                )
                TextButton(onClick = onDismiss, modifier = Modifier.align(Alignment.CenterEnd)) {
                    Text("完成", color = IosBlue)
                }
            }
            GroupSection(header = "位置与路线") {
                Text(
                    "地点搜索、规划和重新规划路线时，搜索内容、起终点及所需位置会发送给配置的导航服务。使用高德服务时，网关会再将所需信息发送给高德。在线周边地图请求也会透露所查看的区域。",
                    fontSize = 15.sp,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
                )
                Text(
                    "导航期间，定位用于更新路线和圆屏指引；锁屏或切换应用后仍可继续。结束导航会停止导航定位。可在系统设置中调整权限。",
                    fontSize = 15.sp,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
                )
            }
            GroupSection(header = "蓝牙与音乐") {
                Text(
                    "连接圆屏后，导航、速度及周边地图通过蓝牙同步。授权通知使用权后，系统媒体会话的曲名、歌手和播放状态会同步到圆屏，圆屏可控制上一首、播放或暂停、下一首。",
                    fontSize = 15.sp,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
                )
                Text(
                    "音乐信息不发送到 MOTO GPS 的导航服务器。拒绝音乐权限不影响地点搜索和导航。",
                    fontSize = 15.sp,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
                )
            }
            GroupSection(header = "存储与清理") {
                Text(
                    "最近地点、已连接设备标识和下载地图保存在手机上。首页可清空最近搜索；地图与离线下载页面可删除下载包、清理临时缓存。删除 App 可移除它的本地数据；系统备份可能包含部分本地数据。",
                    fontSize = 15.sp,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
                )
                Text(
                    "服务器访问日志可能保留 IP、请求时间、搜索词、位置参数及地图区域。记录范围、轮转和保留期限由部署方的实际配置及隐私政策说明；这不等于数据只在手机上保存。",
                    fontSize = 15.sp,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
                )
            }
            GroupSection(
                header = "第三方服务",
                footer = "手机路线底图由系统地图提供，圆屏周边道路与建筑使用 OpenStreetMap / Protomaps 数据。",
            ) {
                LinkRow("高德隐私政策", "https://lbs.amap.com/home/privacy/")
                RowDivider()
                LinkRow("OpenStreetMap 数据与许可", "https://www.openstreetmap.org/copyright")
                RowDivider()
                LinkRow("Protomaps 地图数据", "https://protomaps.com")
            }
        }
    }
}

@Composable
private fun LinkRow(label: String, url: String) {
    val context = LocalContext.current
    Row(
        Modifier.fillMaxWidth().clickable {
            runCatching {
                context.startActivity(Intent(Intent.ACTION_VIEW, android.net.Uri.parse(url)))
            }
        }.padding(horizontal = 16.dp, vertical = 11.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, fontSize = 16.sp, color = IosBlue, modifier = Modifier.weight(1f))
        Text("›", fontSize = 14.sp, color = IosSecondary.copy(alpha = 0.6f))
    }
}

// MARK: - Map downloads sheet

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun MapDownloadsSheet(onDismiss: () -> Unit) {
    val vm: MapDownloadsViewModel = viewModel()
    val state by vm.state.collectAsState()
    LaunchedEffect(Unit) { vm.refresh() }

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(bottom = 24.dp)) {
            Box(Modifier.fillMaxWidth().height(48.dp)) {
                Text(
                    "地图",
                    fontSize = 17.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.align(Alignment.Center),
                )
                TextButton(onClick = onDismiss, modifier = Modifier.align(Alignment.CenterEnd)) {
                    Text("完成", color = IosBlue)
                }
            }

            GroupSection(footer = "骑行时自动加载圆屏周边的道路和建筑。网络不可用时，使用已下载的地图。") {
                Row(
                    Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 11.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("🌐", fontSize = 16.sp)
                    Spacer(Modifier.width(14.dp))
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text("自动加载周边地图", fontSize = 16.sp, color = Color(0xFF000000))
                        Text(
                            "已缓存 ${state.cachedBytes / 1024 / 1024} MiB · 离线包 ${state.packs.size} 个",
                            fontSize = 13.sp,
                            color = IosSecondary,
                        )
                    }
                }
            }

            GroupSection(footer = "跨市骑行可先规划路线，只保存沿途约一公里范围的底图。离线底图不包含实时路况，规划新路线仍需要网络。") {
                if (state.routeAvailable) {
                    Row(
                        Modifier.fillMaxWidth()
                            .clickable(enabled = !state.downloading) { vm.downloadRoute() }
                            .padding(horizontal = 16.dp, vertical = 11.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text("🛣", fontSize = 16.sp, color = IosBlue)
                        Spacer(Modifier.width(12.dp))
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Text("下载这条路线周边", fontSize = 16.sp, color = IosBlue)
                            Text(
                                state.routeName ?: "当前选择的路线",
                                fontSize = 13.sp,
                                color = IosSecondary,
                            )
                        }
                    }
                    RowDivider()
                }
                // City / district search (iOS MapCitySearchView).
                Column(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
                    androidx.compose.foundation.text.BasicTextField(
                        value = state.query,
                        onValueChange = vm::queryChanged,
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                        textStyle = androidx.compose.ui.text.TextStyle(
                            fontSize = 16.sp,
                            color = Color(0xFF000000),
                        ),
                        decorationBox = { inner ->
                            Box {
                                if (state.query.isEmpty()) {
                                    Text("搜索城市或区县，例如“济南”", fontSize = 16.sp, color = IosSecondary)
                                }
                                inner()
                            }
                        },
                    )
                }
                when {
                    state.searching -> {
                        RowDivider()
                        Row(
                            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 11.dp),
                            horizontalArrangement = Arrangement.spacedBy(12.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                            Text("正在搜索城市…", fontSize = 15.sp, color = IosSecondary)
                        }
                    }

                    state.searchFailure != null -> {
                        RowDivider()
                        Text(
                            state.searchFailure ?: "",
                            fontSize = 15.sp,
                            color = IosSecondary,
                            modifier = Modifier.padding(horizontal = 16.dp, vertical = 11.dp),
                        )
                        RowDivider()
                        Row(
                            Modifier.fillMaxWidth().clickable { vm.retrySearch() }
                                .padding(horizontal = 16.dp, vertical = 11.dp),
                        ) { Text("重试", fontSize = 16.sp, color = IosBlue) }
                    }

                    else -> state.cities.forEach { city ->
                        RowDivider()
                        Row(
                            Modifier.fillMaxWidth()
                                .clickable(enabled = !state.downloading) { vm.downloadCity(city) }
                                .padding(horizontal = 16.dp, vertical = 11.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                                Text(city.name, fontSize = 16.sp, color = Color(0xFF000000))
                                Text(city.detail, fontSize = 13.sp, color = IosSecondary)
                            }
                            Text("下载", fontSize = 15.sp, color = IosBlue)
                        }
                    }
                }
            }

            if (state.downloading || state.message != null) {
                GroupSection {
                    if (state.downloading) {
                        Column(Modifier.padding(horizontal = 16.dp, vertical = 11.dp)) {
                            state.progress?.let { progress ->
                                androidx.compose.material3.LinearProgressIndicator(
                                    progress = { progress.fraction },
                                    modifier = Modifier.fillMaxWidth(),
                                )
                                Spacer(Modifier.height(6.dp))
                                Text(
                                    "${progress.done}/${progress.total} 瓦片 · 失败 ${progress.failed}",
                                    fontSize = 13.sp,
                                    color = IosSecondary,
                                )
                            } ?: Text("正在下载…", fontSize = 15.sp, color = IosSecondary)
                        }
                    } else {
                        Text(
                            state.message ?: "",
                            fontSize = 15.sp,
                            color = IosSecondary,
                            modifier = Modifier.padding(horizontal = 16.dp, vertical = 11.dp),
                        )
                    }
                }
            }

            GroupSection(header = "已保存") {
                if (state.packs.isEmpty()) {
                    Text(
                        "还没有下载城市或路线地图",
                        fontSize = 15.sp,
                        color = IosSecondary,
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 11.dp),
                    )
                } else {
                    state.packs.forEachIndexed { index, pack ->
                        if (index > 0) RowDivider()
                        Row(
                            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                                Text(pack.name, fontSize = 16.sp, color = Color(0xFF000000))
                                Text(pack.detail, fontSize = 13.sp, color = IosSecondary)
                                Text(
                                    "${pack.bytes / 1024 / 1024} MiB · ${pack.tiles.size} 瓦片",
                                    fontSize = 12.sp,
                                    color = IosSecondary,
                                )
                            }
                            TextButton(
                                enabled = !state.downloading,
                                onClick = { vm.delete(pack) },
                            ) { Text("删除", color = IosRed) }
                        }
                    }
                }
                RowDivider()
                Row(
                    Modifier.fillMaxWidth()
                        .clickable(enabled = !state.downloading) { vm.clearTemporaryCache() }
                        .padding(horizontal = 16.dp, vertical = 11.dp),
                ) { Text("清理自动缓存", fontSize = 16.sp, color = IosBlue) }
            }

            GroupSection(footer = "建筑和道路的完整程度取决于当地地图数据。") {
                LinkRow("© OpenStreetMap contributors", "https://www.openstreetmap.org/copyright")
                RowDivider()
                LinkRow("Protomaps 地图数据", "https://protomaps.com")
            }
        }
    }
}
