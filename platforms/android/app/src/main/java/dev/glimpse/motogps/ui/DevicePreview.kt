package dev.glimpse.motogps.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.glimpse.motogps.layout.ComponentSizes
import dev.glimpse.motogps.layout.LayoutComponent
import dev.glimpse.motogps.layout.LayoutComponentId
import dev.glimpse.motogps.layout.LayoutArcAnchor
import dev.glimpse.motogps.layout.LayoutCanvas
import dev.glimpse.motogps.layout.LayoutGeometry
import dev.glimpse.motogps.layout.UiLayout
import kotlin.math.abs

/**
 * 466×466 圆屏近似复刻预览（§7.3）：Compose Canvas，按手机宽度缩放保持
 * 1:1。几何/透明度与固件一致；字体渲染允许差异。坐标回调一律使用画布
 * 坐标（0..466）；拖拽中实时回报原始中心点，松手后由父级做吸附并提交。
 *
 * 演示数据按 §7.3：速度 72、限速 70、航向 359°、右转 300 m、剩余 8.2 km /
 * 21 min、进度 24%、加速度 +1.2。
 */
object PreviewDemoData {
    const val SPEED_KPH = 72
    const val SPEED_LIMIT_KPH = 70
    const val HEADING_DEG = 359
    const val REMAINING_DISTANCE_KM = "8.2 km"
    const val REMAINING_TIME = "21 分钟"
    const val MANEUVER_DISTANCE = "300 m"
    const val MANEUVER_SYMBOL = "↱"
    const val PROGRESS_PERCENT = 24
    const val ACCEL_MPS2 = "+1.2"

    fun headingText(): String = "359° 北"
}

private val PreviewBackground = Color(0xFF101216)
private val PreviewMapBase = Color(0xFF1C2026)
private val PreviewStreet = Color(0xFF8E8E93)
private val PreviewRoute = Color(0xFFFFFFFF)
private val PreviewText = Color(0xFFF2F2F7)
private val PreviewTextDim = Color(0xFF8E8E93)
private val PreviewTrafficSmooth = Color(0xFF30B0C8)
private val PreviewAccel = Color(0xFF34C759)
private val PreviewSpeedLimit = Color(0xFFFF3B30)
private val PreviewSelection = Color(0xFF007AFF)
private val PreviewSlot = Color(0x55FFFFFF)

/**
 * @param onDrag 拖拽中回调，参数为组件 id 与拖拽点画布坐标（中心点）。
 * @param onDragEnd 松手回调（父级负责吸附与提交）。
 */
@Composable
fun DevicePreview(
    layout: UiLayout,
    selected: LayoutComponentId?,
    modifier: Modifier = Modifier,
    onComponentTap: (LayoutComponentId?) -> Unit = {},
    onDragStart: (LayoutComponentId) -> Unit = {},
    onDrag: (LayoutComponentId, Float, Float) -> Unit = { _, _, _ -> },
    onDragEnd: (LayoutComponentId) -> Unit = {},
) {
    val textMeasurer = rememberTextMeasurer()
    var canvasPx by remember { mutableFloatStateOf(1f) }

    fun toCanvas(offset: Offset): Pair<Float, Float> =
        (offset.x / canvasPx * LayoutCanvas.SIZE) to
            (offset.y / canvasPx * LayoutCanvas.SIZE)

    Box(
        modifier
            .fillMaxWidth()
            .aspectRatio(1f)
            .padding(8.dp)
            .clip(CircleShape)
            .pointerInput(layout, selected) {
                detectTapGestures { offset ->
                    val (x, y) = toCanvas(offset)
                    onComponentTap(hitTest(layout, x, y))
                }
            }
            .pointerInput(layout, selected) {
                detectDragGestures(
                    onDragStart = { offset ->
                        val (x, y) = toCanvas(offset)
                        val hit = hitTest(layout, x, y)
                        if (hit != null) onDragStart(hit)
                    },
                    onDrag = { change, _ ->
                        val target = selected ?: return@detectDragGestures
                        change.consume()
                        val (x, y) = toCanvas(change.position)
                        onDrag(target, x, y)
                    },
                    onDragEnd = { selected?.let(onDragEnd) },
                )
            },
    ) {
        Canvas(Modifier.fillMaxSize()) {
            canvasPx = size.width
            if (canvasPx <= 0f) return@Canvas
            val k = size.width / LayoutCanvas.SIZE

            drawRect(PreviewBackground)

            // 地图区贴顶：高度/透明度可调（§4.8），方向模式不影响预览绘制。
            if (layout.map.opacity > 0) {
                drawRect(
                    PreviewMapBase,
                    size = Size(size.width, layout.map.heightPx * k),
                    alpha = layout.map.opacity / 100f,
                )
                drawDemoStreets(layout, k)
            }

            drawArcs(layout, k)
            drawFreeComponents(layout, k, textMeasurer)
            if (selected != null) drawSelectionOverlay(layout, k, selected)

            // 安全区参考线（编辑提示，设备上不存在）。
            drawCircle(
                Color(0x22FFFFFF),
                radius = LayoutCanvas.SAFE_RADIUS * k,
                center = center,
                style = Stroke(1.dp.toPx()),
            )
        }
    }
}

/** 命中测试：先圆弧槽（40px 方形热区），再非圆弧矩形。 */
private fun hitTest(layout: UiLayout, x: Float, y: Float): LayoutComponentId? {
    layout.components.values.filter { it.isArc && it.enabled }.forEach { c ->
        val (ax, ay) = LayoutGeometry.arcAnchorPoint(c.anchor)
        if (abs(x - ax) <= 40f && abs(y - ay) <= 40f) return c.id
    }
    layout.components.values.filter { it.enabled && !it.isArc }.forEach { c ->
        val (w, h) = ComponentSizes.sizeOf(c.id, c.size)
        if (x >= c.x && x <= c.x + w && y >= c.y && y <= c.y + h) return c.id
    }
    return null
}

private fun DrawScope.drawDemoStreets(layout: UiLayout, k: Float) {
    val w = size.width
    val h = layout.map.heightPx * k
    val streets = listOf(
        0.15f to 0.35f, 0.85f to 0.30f,
        0.30f to 0.05f, 0.38f to 0.95f,
        0.55f to 0.10f, 0.52f to 0.90f,
        0.10f to 0.70f, 0.90f to 0.66f,
    )
    streets.chunked(2).forEach { chunk ->
        val (a, b) = chunk
        drawLine(
            PreviewStreet.copy(alpha = 0.35f * layout.basemap.street.opacity / 100f),
            start = Offset(a.first * w, a.second * h),
            end = Offset(b.first * w, b.second * h),
            strokeWidth = layout.basemap.street.widthPx * k,
        )
    }
    // 演示路线：北向上时地图不旋转。
    val route = Path().apply {
        moveTo(0.50f * w, 0f)
        cubicTo(0.50f * w, 0.30f * h, 0.42f * w, 0.55f * h, 0.55f * w, 0.62f * h)
        cubicTo(0.68f * w, 0.70f * h, 0.62f * w, 0.90f * h, 0.50f * w, h)
    }
    drawPath(
        route,
        PreviewRoute.copy(alpha = layout.basemap.route.opacity / 100f),
        style = Stroke(layout.basemap.route.widthPx * k, cap = StrokeCap.Round),
    )
    // 车标：始终显示在路线窗口内；北向上时它按手机航向旋转，这里固定朝上示意。
    drawCircle(Color(0xFF007AFF), radius = 5f * k, center = Offset(0.50f * w, 0.62f * h))
}

private fun DrawScope.drawArcs(layout: UiLayout, k: Float) {
    val progress = layout[LayoutComponentId.PROGRESS_ARC]
    val speed = layout[LayoutComponentId.SPEED_ARC]
    val (progressDiameter, speedDiameter) = LayoutGeometry.arcDiameters(progress, speed)

    if (progress.enabled) drawOneArc(
        progress, progressDiameter,
        PreviewTextDim, PreviewTrafficSmooth,
        PreviewDemoData.PROGRESS_PERCENT / 100f, k,
    )
    if (speed.enabled) drawOneArc(
        speed, speedDiameter,
        PreviewTextDim, PreviewAccel,
        PreviewDemoData.SPEED_KPH / 160f, k,
    )
}

private fun DrawScope.drawOneArc(
    component: LayoutComponent,
    diameter: Int,
    baseColor: Color,
    fillColor: Color,
    sweepFraction: Float,
    k: Float,
) {
    val (startDeg, sweepDeg) = LayoutGeometry.arcAngles(component.anchor, component.spanDeg)
    val alpha = component.opacity / 100f
    val stroke = ComponentSizes.arcStroke(component.size)
    val diameterPx = diameter * k
    val topLeft = Offset(center.x - diameterPx / 2f, center.y - diameterPx / 2f)
    val arcSize = Size(diameterPx, diameterPx)
    drawArc(
        baseColor, startDeg, sweepDeg, false, topLeft, arcSize,
        style = Stroke(stroke * k, cap = StrokeCap.Butt), alpha = alpha,
    )
    if (sweepFraction > 0f) {
        drawArc(
            fillColor, startDeg, sweepDeg * sweepFraction, false, topLeft, arcSize,
            style = Stroke(stroke * k, cap = StrokeCap.Round), alpha = alpha,
        )
    }
}

private fun DrawScope.drawFreeComponents(layout: UiLayout, k: Float, textMeasurer: TextMeasurer) {
    layout.components.values
        .filter { it.enabled && !it.isArc }
        .sortedBy { it.id.id }
        .forEach { c -> drawOneFreeComponent(c, k, textMeasurer) }

    // speed_arc 内侧的加速度读数（§4.5）。
    val speedArc = layout[LayoutComponentId.SPEED_ARC]
    if (speedArc.enabled) {
        val anchorOffset = when (speedArc.anchor) {
            LayoutArcAnchor.TOP -> Offset(LayoutCanvas.CENTER, 60f)
            LayoutArcAnchor.BOTTOM -> Offset(LayoutCanvas.CENTER, LayoutCanvas.SIZE - 60f)
            LayoutArcAnchor.LEFT -> Offset(60f, LayoutCanvas.CENTER)
            LayoutArcAnchor.RIGHT -> Offset(LayoutCanvas.SIZE - 60f, LayoutCanvas.CENTER)
        }
        drawCanvasText(
            textMeasurer, PreviewDemoData.ACCEL_MPS2, 14.sp, PreviewAccel,
            anchorOffset, speedArc.opacity / 100f, k, center = true,
        )
    }
}

private fun DrawScope.drawOneFreeComponent(c: LayoutComponent, k: Float, textMeasurer: TextMeasurer) {
    val alpha = c.opacity / 100f
    val x = c.x * k
    val y = c.y * k
    val (w, h) = ComponentSizes.sizeOf(c.id, c.size)
    val wPx = w * k
    val hPx = h * k
    val midX = x + wPx / 2f
    val midY = y + hPx / 2f
    when (c.id) {
        LayoutComponentId.SPEED -> {
            drawCanvasText(textMeasurer, "${PreviewDemoData.SPEED_KPH}", 24.sp, PreviewText, Offset(midX, midY - 7f * k), alpha, k, center = true)
            drawCanvasText(textMeasurer, "km/h", 10.sp, PreviewTextDim, Offset(midX, midY + 13f * k), alpha, k, center = true)
        }
        LayoutComponentId.SPEED_LIMIT -> {
            drawCircle(PreviewText.copy(alpha = alpha), 27f * k, Offset(midX, midY))
            drawCircle(PreviewSpeedLimit.copy(alpha = alpha), 27f * k, Offset(midX, midY), style = Stroke(4f * k))
            drawCanvasText(textMeasurer, "${PreviewDemoData.SPEED_LIMIT_KPH}", 15.sp, PreviewText, Offset(midX, midY), alpha, k, center = true)
        }
        LayoutComponentId.HEADING -> {
            drawCanvasText(textMeasurer, PreviewDemoData.headingText(), 16.sp, PreviewText, Offset(midX, midY), alpha, k, center = true)
        }
        LayoutComponentId.MANEUVER -> {
            drawCanvasText(textMeasurer, PreviewDemoData.MANEUVER_SYMBOL, 22.sp, PreviewTrafficSmooth, Offset(x + 18f * k, midY), alpha, k, center = true)
            drawCanvasText(textMeasurer, PreviewDemoData.MANEUVER_DISTANCE, 12.sp, PreviewText, Offset(x + 40f * k, midY), alpha, k)
        }
        LayoutComponentId.REMAINING_DISTANCE -> {
            drawCanvasText(textMeasurer, PreviewDemoData.REMAINING_DISTANCE_KM, 13.sp, PreviewText, Offset(midX, midY), alpha, k, center = true)
        }
        LayoutComponentId.REMAINING_TIME -> {
            drawCanvasText(textMeasurer, PreviewDemoData.REMAINING_TIME, 13.sp, PreviewText, Offset(midX, midY), alpha, k, center = true)
        }
        else -> Unit
    }
}

private fun DrawScope.drawCanvasText(
    textMeasurer: TextMeasurer,
    text: String,
    fontSize: TextUnit,
    color: Color,
    at: Offset,
    alpha: Float,
    k: Float,
    center: Boolean = false,
) {
    val measured = textMeasurer.measure(
        AnnotatedString(text),
        style = TextStyle(fontSize = (fontSize.value * k).sp, color = color.copy(alpha = alpha)),
    )
    val topLeft = if (center) {
        Offset(at.x - measured.size.width / 2f, at.y - measured.size.height / 2f)
    } else {
        at
    }
    drawText(textLayoutResult = measured, topLeft = topLeft)
}

private fun DrawScope.drawSelectionOverlay(layout: UiLayout, k: Float, selected: LayoutComponentId) {
    val component = layout[selected]
    if (component.isArc) {
        LayoutArcAnchor.entries.forEach { anchor ->
            val (ax, ay) = LayoutGeometry.arcAnchorPoint(anchor)
            drawCircle(
                PreviewSlot,
                radius = 14f * k,
                center = Offset(ax * k, ay * k),
                style = Stroke(1.5f.dp.toPx()),
            )
        }
        val (ax, ay) = LayoutGeometry.arcAnchorPoint(component.anchor)
        drawCircle(PreviewSelection.copy(alpha = 0.45f), radius = 10f * k, center = Offset(ax * k, ay * k))
    } else {
        val (w, h) = ComponentSizes.sizeOf(component.id, component.size)
        drawRoundRect(
            PreviewSelection,
            topLeft = Offset(component.x * k, component.y * k),
            size = Size(w * k, h * k),
            cornerRadius = CornerRadius(4f * k),
            style = Stroke(1.5f.dp.toPx()),
        )
    }
}
