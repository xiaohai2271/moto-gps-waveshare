package dev.glimpse.motogps.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.glimpse.motogps.layout.BasemapTheme
import dev.glimpse.motogps.layout.BasemapThemes
import dev.glimpse.motogps.layout.LayoutArcAnchor
import dev.glimpse.motogps.layout.LayoutArcSpan
import dev.glimpse.motogps.layout.LayoutColor
import dev.glimpse.motogps.layout.LayoutComponentId
import dev.glimpse.motogps.layout.LayoutSize
import dev.glimpse.motogps.layout.LayoutOrientation
import dev.glimpse.motogps.layout.LayoutEditorState
/**
 * 圆屏布局编辑器（§7.1–7.2）：全屏页面，顶部 466 圆形预览 + 底部
 * 组件 / 底图 / 全局 三个页签。工具栏：撤销/重做、恢复默认（二次确认）。
 * 中文文案、iOS-like 分组列表风格。
 */
@Composable
fun LayoutEditorScreen(
    editor: LayoutEditorState,
    onPushLayout: () -> Unit,
    onBack: () -> Unit,
) {
    val state by editor.state.collectAsState()
    var selectedTab by remember { mutableIntStateOf(0) }
    var selectedComponent by remember { mutableStateOf<LayoutComponentId?>(null) }
    var showResetDialog by remember { mutableStateOf(false) }

    // 待推送变更自动触发推送（300ms 防抖由 NavViewModel 协程负责）。
    LaunchedEffect(state.layout.revision, state.syncState) {
        if (state.syncState == LayoutEditorState.SyncState.PENDING) onPushLayout()
    }

    Column(Modifier.fillMaxSize()) {
        // 顶部导航栏
        Box(Modifier.fillMaxWidth().height(52.dp)) {
            TextButton(
                onClick = onBack,
                modifier = Modifier.align(Alignment.CenterStart),
            ) { Text("‹ 返回", color = IosBlue, fontSize = 16.sp) }
            Text(
                "圆屏布局",
                fontSize = 17.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.align(Alignment.Center),
            )
            Text(
                syncLabel(state.syncState),
                fontSize = 12.sp,
                color = syncColor(state.syncState),
                modifier = Modifier.align(Alignment.CenterEnd).padding(end = 16.dp),
            )
        }

        // 工具栏
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            ToolButton("撤销", enabled = editor.canUndo) { editor.undo() }
            ToolButton("重做", enabled = editor.canRedo) { editor.redo() }
            Spacer(Modifier.weight(1f))
            ToolButton("恢复默认", enabled = true, destructive = true) { showResetDialog = true }
        }

        // 466 圆形预览（按屏宽缩放）
        DevicePreview(
            layout = state.layout,
            selected = selectedComponent,
            onComponentTap = { selectedComponent = it },
            onDragStart = { selectedComponent = it },
            onDragEnd = { id ->
                editor.settleDraggedComponent(id, lastDragCenter.first, lastDragCenter.second)
            },
            onDrag = { _, x, y -> lastDragCenter = x to y },
        )

        Spacer(Modifier.height(4.dp))

        // 页签
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            TabButton("组件", selectedTab == 0) { selectedTab = 0 }
            TabButton("底图", selectedTab == 1) { selectedTab = 1 }
            TabButton("全局", selectedTab == 2) { selectedTab = 2 }
        }

        Column(
            Modifier
                .fillMaxWidth()
                .weight(1f)
                .verticalScroll(rememberScrollState()),
        ) {
            when (selectedTab) {
                0 -> ComponentsTab(editor, selectedComponent, onSelected = { selectedComponent = it })
                1 -> BasemapTab(editor)
                else -> GlobalTab(editor)
            }
            Spacer(Modifier.height(24.dp))
        }
    }

    if (showResetDialog) {
        AlertDialog(
            onDismissRequest = { showResetDialog = false },
            title = { Text("恢复默认布局？") },
            text = { Text("当前所有布局调整将被替换为默认布局，并推送到圆屏。") },
            confirmButton = {
                TextButton(onClick = {
                    editor.resetToDefault()
                    showResetDialog = false
                }) { Text("恢复默认", color = IosRed) }
            },
            dismissButton = {
                TextButton(onClick = { showResetDialog = false }) { Text("取消") }
            },
        )
    }
}

/** 最近一次拖拽中心点（画布坐标），松手时用于吸附计算。 */
private var lastDragCenter = 0f to 0f

private fun syncLabel(state: LayoutEditorState.SyncState): String = when (state) {
    LayoutEditorState.SyncState.NOT_CONNECTED -> "未连接 · 待同步"
    LayoutEditorState.SyncState.PENDING -> "待同步"
    LayoutEditorState.SyncState.PUSHING -> "正在同步"
    LayoutEditorState.SyncState.SYNCED -> "已同步"
    LayoutEditorState.SyncState.RETRY_EXHAUSTED -> "未同步 · 连接后重试"
}

private fun syncColor(state: LayoutEditorState.SyncState): Color = when (state) {
    LayoutEditorState.SyncState.SYNCED -> IosGreen
    LayoutEditorState.SyncState.PUSHING -> IosSecondary
    else -> IosOrange
}

@Composable
private fun ToolButton(label: String, enabled: Boolean, destructive: Boolean = false, onClick: () -> Unit) {
    Surface(
        shape = RoundedCornerShape(8.dp),
        color = Color.White,
        modifier = Modifier.clickable(enabled = enabled, onClick = onClick),
    ) {
        Text(
            label,
            fontSize = 14.sp,
            color = when {
                !enabled -> IosSecondary.copy(alpha = 0.4f)
                destructive -> IosRed
                else -> IosBlue
            },
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 7.dp),
        )
    }
}

@Composable
private fun TabButton(label: String, selected: Boolean, onClick: () -> Unit) {
    Surface(
        shape = RoundedCornerShape(8.dp),
        color = if (selected) IosBlue else Color.White,
        modifier = Modifier.clickable(onClick = onClick),
    ) {
        Text(
            label,
            fontSize = 14.sp,
            fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
            color = if (selected) Color.White else Color(0xFF000000),
            modifier = Modifier.padding(horizontal = 14.dp, vertical = 7.dp),
        )
    }
}

// MARK: - 组件页签 -----------------------------------------------------------

@Composable
private fun ComponentsTab(
    editor: LayoutEditorState,
    selected: LayoutComponentId?,
    onSelected: (LayoutComponentId) -> Unit,
) {
    val state by editor.state.collectAsState()
    EditorSection(header = "全部组件", footer = "点选组件后在预览中拖动；圆弧组件只能停靠四边。") {
        LayoutComponentId.entries.forEachIndexed { index, id ->
            if (index > 0) EditorDivider()
            val component = state.layout[id]
            val supported = editor.isComponentSupportedOnDevice(id)
            Row(
                Modifier.fillMaxWidth()
                    .clickable(enabled = supported) { onSelected(id) }
                    .padding(horizontal = 16.dp, vertical = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text(
                        id.label,
                        fontSize = 16.sp,
                        color = if (supported) Color(0xFF000000) else IosSecondary,
                    )
                    if (!supported) {
                        Text("固件不支持该组件", fontSize = 12.sp, color = IosOrange)
                    }
                }
                Text(
                    if (component.enabled) "开" else "关",
                    fontSize = 13.sp,
                    color = if (component.enabled) IosGreen else IosSecondary,
                )
                if (selected == id) {
                    Spacer(Modifier.width(8.dp))
                    Text("✓", fontSize = 15.sp, color = IosBlue)
                }
            }
        }
    }

    selected?.let { id ->
        val component = state.layout[id]
        EditorSection(header = "属性 · ${id.label}") {
            ToggleRow("显示组件", component.enabled) { enabled ->
                editor.updateComponent(id) { it.copy(enabled = enabled) }
            }
            if (component.isArc) {
                EditorDivider()
                SegmentedRow("锚点", LayoutArcAnchor.entries.map { it.label }) { index ->
                    editor.updateComponent(id) { it.copy(anchor = LayoutArcAnchor.entries[index]) }
                }
                EditorDivider()
                SegmentedRow(
                    "弧长",
                    LayoutArcSpan.entries.map { "${it.degrees}°" },
                ) { index ->
                    editor.updateComponent(id) { it.copy(spanDeg = LayoutArcSpan.entries[index].degrees) }
                }
            } else {
                EditorDivider()
                SegmentedRow("尺寸", LayoutSize.entries.map { it.label }) { index ->
                    editor.updateComponent(id) { it.copy(size = LayoutSize.entries[index]) }
                }
            }
            EditorDivider()
            SliderRow(
                "透明度",
                component.opacity.toFloat(),
                10f..100f,
                valueLabel = "${component.opacity}%",
            ) { value ->
                editor.updateComponent(id) { it.copy(opacity = value.toInt()) }
            }
        }
    }
}

// MARK: - 底图页签（无瓦片来源/影像入口，§D9） --------------------------------

@Composable
private fun BasemapTab(editor: LayoutEditorState) {
    val state by editor.state.collectAsState()
    val basemap = state.layout.basemap

    EditorSection(header = "主题预设", footer = "切换主题会改写下方路线/街道样式；底图为矢量主题，本期不提供影像来源选项。") {
        SegmentedRow("主题", BasemapTheme.entries.map { it.label }) { index ->
            val theme = BasemapTheme.entries[index]
            editor.updateBasemap { BasemapThemes.apply(it, theme) }
        }
    }

    EditorSection(header = "路线样式") {
        ColorRow("路线颜色", basemap.route.color) { color ->
            editor.updateBasemap { it.copy(route = it.route.copy(color = color)) }
        }
        EditorDivider()
        StepperRow("路线宽度", basemap.route.widthPx, 1..16) { w ->
            editor.updateBasemap { it.copy(route = it.route.copy(widthPx = w)) }
        }
        EditorDivider()
        SliderRow("路线透明度", basemap.route.opacity.toFloat(), 0f..100f, valueLabel = "${basemap.route.opacity}%") { v ->
            editor.updateBasemap { it.copy(route = it.route.copy(opacity = v.toInt())) }
        }
    }

    EditorSection(header = "街道样式") {
        ColorRow("街道颜色", basemap.street.color) { color ->
            editor.updateBasemap { it.copy(street = it.street.copy(color = color)) }
        }
        EditorDivider()
        StepperRow("街道宽度", basemap.street.widthPx, 1..12) { w ->
            editor.updateBasemap { it.copy(street = it.street.copy(widthPx = w)) }
        }
        EditorDivider()
        SliderRow("街道透明度", basemap.street.opacity.toFloat(), 0f..100f, valueLabel = "${basemap.street.opacity}%") { v ->
            editor.updateBasemap { it.copy(street = it.street.copy(opacity = v.toInt())) }
        }
    }

    EditorSection(header = "建筑与图层") {
        SliderRow("建筑透明度", basemap.buildingOpacity.toFloat(), 0f..100f, valueLabel = "${basemap.buildingOpacity}%") { v ->
            editor.updateBasemap { it.copy(buildingOpacity = v.toInt()) }
        }
        EditorDivider()
        ToggleRow("道路", basemap.layers.roads) { on ->
            editor.updateBasemap { it.copy(layers = it.layers.copy(roads = on)) }
        }
        EditorDivider()
        ToggleRow("建筑", basemap.layers.buildings) { on ->
            editor.updateBasemap { it.copy(layers = it.layers.copy(buildings = on)) }
        }
        EditorDivider()
        ToggleRow("路线", basemap.layers.route) { on ->
            editor.updateBasemap { it.copy(layers = it.layers.copy(route = on)) }
        }
    }
}

// MARK: - 全局页签 -----------------------------------------------------------

@Composable
private fun GlobalTab(editor: LayoutEditorState) {
    val state by editor.state.collectAsState()
    val layout = state.layout

    EditorSection(
        header = "方向模式",
        footer = "北向上：地图固定北向，车标随航向旋转。航向由手机提供，圆屏不做罗盘解算。",
    ) {
        SegmentedRow(
            "方向",
            listOf("北向上", "设备向上"),
        ) { index ->
            editor.updateOrientation(
                if (index == 0) LayoutOrientation.NORTH_UP else LayoutOrientation.HEADING_UP,
            )
        }
    }

    EditorSection(header = "音乐页") {
        ToggleRow("独立音乐页", layout.musicPageEnabled) { on ->
            editor.updateMusicPage(on)
        }
    }

    EditorSection(header = "地图区", footer = "地图贴顶显示，只调整高度与透明度。") {
        // map_height_px u16：160..466 偶数，默认 232。
        SliderRow(
            "地图高度",
            layout.map.heightPx.toFloat(),
            160f..466f,
            valueLabel = "${layout.map.heightPx} px",
        ) { v ->
            val even = (v.toInt() / 2) * 2
            editor.updateMap { it.copy(heightPx = even.coerceIn(160, 466)) }
        }
        EditorDivider()
        SliderRow("地图透明度", layout.map.opacity.toFloat(), 10f..100f, valueLabel = "${layout.map.opacity}%") { v ->
            editor.updateMap { it.copy(opacity = v.toInt()) }
        }
        EditorDivider()
        SliderRow("缩放", layout.map.zoomPercent.toFloat(), 50f..200f, valueLabel = "${layout.map.zoomPercent}%") { v ->
            editor.updateMap { it.copy(zoomPercent = v.toInt()) }
        }
    }
}

// MARK: - 通用构件（iOS-like） ------------------------------------------------

@Composable
private fun EditorSection(
    header: String? = null,
    footer: String? = null,
    content: @Composable () -> Unit,
) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
        if (header != null) {
            Text(
                header.uppercase(),
                fontSize = 13.sp,
                color = IosSecondary,
                modifier = Modifier.padding(start = 16.dp, top = 18.dp, bottom = 7.dp),
            )
        }
        Surface(
            shape = RoundedCornerShape(10.dp),
            color = Color.White,
            modifier = Modifier.fillMaxWidth(),
        ) { Column { content() } }
        if (footer != null) {
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
private fun EditorDivider() = Box(
    Modifier
        .padding(start = 16.dp)
        .fillMaxWidth()
        .height(0.5.dp)
        .background(IosSeparator),
)

@Composable
private fun ToggleRow(label: String, value: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth()
            .clickable { onChange(!value) }
            .padding(horizontal = 16.dp, vertical = 11.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, fontSize = 16.sp, color = Color(0xFF000000), modifier = Modifier.weight(1f))
        Text(
            if (value) "开" else "关",
            fontSize = 15.sp,
            color = if (value) IosGreen else IosSecondary,
        )
    }
}

@Composable
private fun SegmentedRow(label: String, options: List<String>, onSelect: (Int) -> Unit) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp)) {
        Text(label, fontSize = 13.sp, color = IosSecondary)
        Spacer(Modifier.height(6.dp))
        Row(
            Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            options.forEachIndexed { index, option ->
                Surface(
                    shape = RoundedCornerShape(8.dp),
                    color = IosBlue.copy(alpha = 0.1f),
                    modifier = Modifier.clickable { onSelect(index) },
                ) {
                    Text(
                        option,
                        fontSize = 14.sp,
                        color = IosBlue,
                        modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                    )
                }
            }
        }
    }
}

@Composable
private fun SliderRow(
    label: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    steps: Int = 0,
    valueLabel: String = "",
    onChange: (Float) -> Unit,
) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp)) {
        Row {
            Text(label, fontSize = 15.sp, color = Color(0xFF000000), modifier = Modifier.weight(1f))
            Text(valueLabel, fontSize = 14.sp, color = IosSecondary)
        }
        Slider(
            value = value,
            onValueChange = onChange,
            valueRange = range,
            steps = steps,
        )
    }
}

@Composable
private fun StepperRow(label: String, value: Int, range: IntRange, onChange: (Int) -> Unit) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, fontSize = 16.sp, color = Color(0xFF000000), modifier = Modifier.weight(1f))
        TextButton(
            enabled = value > range.first,
            onClick = { onChange(value - 1) },
        ) { Text("−", fontSize = 18.sp) }
        Text("$value", fontSize = 16.sp, modifier = Modifier.width(28.dp))
        TextButton(
            enabled = value < range.last,
            onClick = { onChange(value + 1) },
        ) { Text("+", fontSize = 18.sp) }
    }
}

private val colorPalette = listOf(
    LayoutColor(0xFFFFFF), LayoutColor(0x30B0C8), LayoutColor(0x34C759),
    LayoutColor(0xFF9500), LayoutColor(0xFF3B30), LayoutColor(0x8E8E93),
    LayoutColor(0xAF52DE), LayoutColor(0x007AFF),
)

@Composable
private fun ColorRow(label: String, current: LayoutColor, onChange: (LayoutColor) -> Unit) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp)) {
        Row {
            Text(label, fontSize = 15.sp, color = Color(0xFF000000), modifier = Modifier.weight(1f))
            Text(current.hex, fontSize = 14.sp, color = IosSecondary)
        }
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            colorPalette.forEach { color ->
                val selected = color.rgb == current.rgb
                Box(
                    Modifier
                        .size(30.dp)
                        .background(
                            Color(color.rgb).copy(alpha = 1f),
                            RoundedCornerShape(15.dp),
                        )
                        .clickable { onChange(color) },
                    contentAlignment = Alignment.Center,
                ) {
                    if (selected) Text(
                        "✓",
                        fontSize = 14.sp,
                        color = if (color.rgb == 0xFFFFFF) Color.Black else Color.White,
                    )
                }
            }
        }
    }
}
