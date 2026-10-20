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
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
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
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.dp
import dev.glimpse.motogps.layout.ComponentSizes
import dev.glimpse.motogps.layout.LayoutArcAnchor
import dev.glimpse.motogps.layout.LayoutCanvas
import dev.glimpse.motogps.layout.LayoutComponent
import dev.glimpse.motogps.layout.LayoutComponentId
import dev.glimpse.motogps.layout.LayoutGeometry
import dev.glimpse.motogps.layout.LayoutOrientation
import dev.glimpse.motogps.layout.LayoutSize
import dev.glimpse.motogps.layout.UiLayout
import kotlin.math.abs

/**
 * 466×466 圆屏近似复刻预览（§7.3）：Compose Canvas，按手机宽度缩放保持
 * 1:1。几何/透明度与固件一致；字号按固件的 466 画布像素换算成 sp，避免
 * 之前把 sp 当像素用导致文字比真机大 2~3 倍、和相邻组件重叠。
 *
 * 坐标回调一律使用画布坐标（0..466）；拖拽过程中组件在预览里实时跟随手
 * 指，松手后由父级做吸附并提交。手势检测不再以 selected 作为 key，否则
 * 按下选中组件会让 pointerInput 重启，拖动第一帧就中断。
 *
 * 演示数据按 §7.3：速度 72、限速 70、航向 359°、右转 300 m、剩余 8.2 km /
 * 21 min、进度 24%、加速度 +1.2。
 */
object PreviewDemoData {
    const val SPEED_KPH = 72
    const val SPEED_LIMIT_KPH = 70
    const val HEADING_DEG = 359
    const val REMAINING_DISTANCE = "8.2"
    const val REMAINING_DISTANCE_CAPTION = "剩余 km"
    const val REMAINING_TIME = "21"
    const val REMAINING_TIME_CAPTION = "剩余分钟"
    const val MANEUVER_DISTANCE = "300 m"
    const val MANEUVER_SYMBOL = "↱"
    const val PROGRESS_PERCENT = 24
    const val ACCEL_MPS2 = "+1.2"

    fun headingValue(): String = "359°"
    fun headingCardinal(): String = "北"
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
private val PreviewLimitInk = Color(0xFF050607)
private val PreviewSelection = Color(0xFF007AFF)
private val PreviewSlot = Color(0x55FFFFFF)

/** 固件 §4.3 字号：数值按档位，说明文字固定 16/20 画布像素。 */
private fun valueFontPx(size: LayoutSize): Float = when (size) {
    LayoutSize.S -> 16f
    LayoutSize.M -> 28f
    LayoutSize.L -> 48f
}

private fun captionFontPx(size: LayoutSize): Float =
    if (size == LayoutSize.S) 16f else 20f

/** Montserrat 行高约为字号的 1.17 倍，和 LVGL 的 line_height 对齐。 */
private fun lineHeightPx(fontPx: Float): Float = fontPx * 1.17f

/**
 * @param onDrag 拖拽中回调，参数为组件 id 与拖拽点画布坐标（中心点）。
 * @param onDragEnd 松手回调，参数为组件 id 与最终中心点画布坐标。
 * @param showDemoData 是否叠加演示底图（示例路网/路线/车标）。关闭后只留组件与
 *   地图区几何，便于在空底图上检查布局与透明度（§7.2 工具栏开关）。
 */
@Composable
fun DevicePreview(
    layout: UiLayout,
    selected: LayoutComponentId?,
    modifier: Modifier = Modifier,
    showDemoData: Boolean = true,
    onComponentTap: (LayoutComponentId?) -> Unit = {},
    onDragStart: (LayoutComponentId) -> Unit = {},
    onDrag: (LayoutComponentId, Float, Float) -> Unit = { _, _, _ -> },
    onDragEnd: (LayoutComponentId, Float, Float) -> Unit = { _, _, _ -> },
) {
    val textMeasurer = rememberTextMeasurer()
    var canvasPx by remember { mutableFloatStateOf(1f) }
    var dragTarget by remember { mutableStateOf<LayoutComponentId?>(null) }
    var dragCenter by remember { mutableStateOf<Offset?>(null) }
    val currentSelected by rememberUpdatedState(selected)
    val currentTap by rememberUpdatedState(onComponentTap)
    val currentDragStart by rememberUpdatedState(onDragStart)
    val currentDrag by rememberUpdatedState(onDrag)
    val currentDragEnd by rememberUpdatedState(onDragEnd)

    fun toCanvas(offset: Offset): Pair<Float, Float> =
        (offset.x / canvasPx * LayoutCanvas.SIZE) to
            (offset.y / canvasPx * LayoutCanvas.SIZE)

    Box(
        modifier
            .fillMaxWidth()
            .aspectRatio(1f)
            .padding(8.dp)
            .clip(CircleShape)
            .pointerInput(layout) {
                detectTapGestures { offset ->
                    val (x, y) = toCanvas(offset)
                    currentTap(hitTest(layout, x, y))
                }
            }
            .pointerInput(layout) {
                detectDragGestures(
                    onDragStart = { offset ->
                        val (x, y) = toCanvas(offset)
                        val hit = hitTest(layout, x, y)
                        if (hit != null) {
                            dragTarget = hit
                            dragCenter = Offset(x, y)
                            currentDragStart(hit)
                        }
                    },
                    onDrag = { change, _ ->
                        val target = dragTarget ?: return@detectDragGestures
                        change.consume()
                        val (x, y) = toCanvas(change.position)
                        dragCenter = Offset(x, y)
                        currentDrag(target, x, y)
                    },
                    onDragEnd = {
                        val target = dragTarget
                        val center = dragCenter
                        dragTarget = null
                        dragCenter = null
                        if (target != null && center != null) {
                            currentDragEnd(target, center.x, center.y)
                        }
                    },
                    onDragCancel = {
                        dragTarget = null
                        dragCenter = null
                    },
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
                // The firmware rotates the whole map under a fixed rider when
                // the layout is heading-up. Mirror that here, otherwise the
                // synthetic preview route stays north-up and the device looks
                // "rotated" after an unrelated map-height change.
                val mapCenter = Offset(size.width / 2f, layout.map.heightPx * k / 2f)
                if (layout.orientation == LayoutOrientation.HEADING_UP) {
                    rotate(degrees = -PreviewDemoData.HEADING_DEG.toFloat(), pivot = mapCenter) {
                        drawDemoStreets(layout, k, showDemoData)
                    }
                } else {
                    drawDemoStreets(layout, k, showDemoData)
                }
                drawCanvasText(
                    textMeasurer,
                    if (layout.orientation == LayoutOrientation.HEADING_UP) {
                        "设备向上"
                    } else {
                        "北向上"
                    },
                    14f,
                    PreviewTextDim,
                    Offset(28f * k, 20f * k),
                    0.8f,
                    k,
                )
            }

            drawArcs(layout, k)
            drawFreeComponents(layout, k, textMeasurer, dragTarget, dragCenter)
            if (currentSelected != null) {
                drawSelectionOverlay(layout, k, currentSelected!!, dragCenter)
            }

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

private fun DrawScope.drawDemoStreets(layout: UiLayout, k: Float, showDemoData: Boolean) {
    if (!showDemoData) return
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

private fun DrawScope.drawFreeComponents(
    layout: UiLayout,
    k: Float,
    textMeasurer: TextMeasurer,
    dragTarget: LayoutComponentId?,
    dragCenter: Offset?,
) {
    layout.components.values
        .filter { it.enabled && !it.isArc }
        .sortedBy { it.id.id }
        .forEach { c ->
            val override = if (c.id == dragTarget) dragCenter else null
            drawOneFreeComponent(c, k, textMeasurer, override)
        }

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
            textMeasurer, PreviewDemoData.ACCEL_MPS2, 14f, PreviewAccel,
            anchorOffset, speedArc.opacity / 100f, k, center = true,
        )
    }
}

private fun DrawScope.drawOneFreeComponent(
    c: LayoutComponent,
    k: Float,
    textMeasurer: TextMeasurer,
    dragCenter: Offset?,
) {
    val alpha = c.opacity / 100f
    // 拖拽中的组件以手指所在中心点实时绘制，松手后才吸附并提交。
    val x = if (dragCenter != null) (dragCenter.x - ComponentSizes.sizeOf(c.id, c.size).first / 2f) else c.x.toFloat()
    val y = if (dragCenter != null) (dragCenter.y - ComponentSizes.sizeOf(c.id, c.size).second / 2f) else c.y.toFloat()
    val (w, h) = ComponentSizes.sizeOf(c.id, c.size)
    val wPx = w * k
    val hPx = h * k
    val midX = (x + wPx / 2f / k) * k
    val midY = (y + hPx / 2f / k) * k

    when (c.id) {
        LayoutComponentId.SPEED -> {
            val valueFont = valueFontPx(c.size)
            val captionFont = captionFontPx(c.size)
            val block = lineHeightPx(valueFont) + lineHeightPx(captionFont) + 2f
            val top = y * k + (h * k - block * k) / 2f
            drawCanvasText(
                textMeasurer, "${PreviewDemoData.SPEED_KPH}", valueFont, PreviewText,
                Offset(midX, top), alpha, k, anchor = TextAnchor.TOP_CENTER,
            )
            drawCanvasText(
                textMeasurer, "km/h", captionFont, PreviewTextDim,
                Offset(midX, top + lineHeightPx(valueFont + 2f) * k), alpha, k,
                anchor = TextAnchor.TOP_CENTER,
            )
        }
        LayoutComponentId.SPEED_LIMIT -> {
            drawCircle(PreviewText.copy(alpha = alpha), 30.5f * k, Offset(midX, midY))
            drawCircle(
                PreviewSpeedLimit.copy(alpha = alpha), 30.5f * k, Offset(midX, midY),
                style = Stroke(6f * k),
            )
            // 白底黑字：之前用白字直接画在白底上，真机有数字、预览却是空圆。
            drawCanvasText(
                textMeasurer, "${PreviewDemoData.SPEED_LIMIT_KPH}", 28f, PreviewLimitInk,
                Offset(midX, midY), alpha, k, anchor = TextAnchor.CENTER,
            )
        }
        LayoutComponentId.HEADING -> {
            val valueFont = valueFontPx(c.size)
            val captionFont = captionFontPx(c.size)
            val block = lineHeightPx(valueFont) + lineHeightPx(captionFont) + 2f
            val top = y * k + (h * k - block * k) / 2f
            drawCanvasText(
                textMeasurer, PreviewDemoData.headingValue(), valueFont, PreviewText,
                Offset(midX, top), alpha, k, anchor = TextAnchor.TOP_CENTER,
            )
            drawCanvasText(
                textMeasurer, PreviewDemoData.headingCardinal(), captionFont, PreviewTextDim,
                Offset(midX, top + lineHeightPx(valueFont + 2f) * k), alpha, k,
                anchor = TextAnchor.TOP_CENTER,
            )
        }
        LayoutComponentId.MANEUVER -> {
            drawCanvasText(
                textMeasurer, PreviewDemoData.MANEUVER_SYMBOL, 40f, PreviewTrafficSmooth,
                Offset(x * k + 20f * k, midY), alpha, k, anchor = TextAnchor.CENTER,
            )
            drawCanvasText(
                textMeasurer, PreviewDemoData.MANEUVER_DISTANCE, 20f, PreviewText,
                Offset(x * k + 44f * k, midY), alpha, k,
            )
        }
        LayoutComponentId.REMAINING_DISTANCE -> {
            drawTwoLineBlock(
                textMeasurer, PreviewDemoData.REMAINING_DISTANCE,
                PreviewDemoData.REMAINING_DISTANCE_CAPTION, c.size, x, y, w, h, alpha, k,
            )
        }
        LayoutComponentId.REMAINING_TIME -> {
            drawTwoLineBlock(
                textMeasurer, PreviewDemoData.REMAINING_TIME,
                PreviewDemoData.REMAINING_TIME_CAPTION, c.size, x, y, w, h, alpha, k,
            )
        }
        else -> Unit
    }
}

private fun DrawScope.drawTwoLineBlock(
    textMeasurer: TextMeasurer,
    value: String,
    caption: String,
    size: LayoutSize,
    x: Float,
    y: Float,
    w: Int,
    h: Int,
    alpha: Float,
    k: Float,
) {
    val valueFont = valueFontPx(size)
    val captionFont = captionFontPx(size)
    val block = lineHeightPx(valueFont) + lineHeightPx(captionFont) + 2f
    val top = y * k + (h * k - block * k) / 2f
    val midX = (x + w / 2f) * k
    drawCanvasText(
        textMeasurer, value, valueFont, PreviewText, Offset(midX, top), alpha, k,
        anchor = TextAnchor.TOP_CENTER,
    )
    drawCanvasText(
        textMeasurer, caption, captionFont, PreviewTextDim,
        Offset(midX, top + lineHeightPx(valueFont + 2f) * k), alpha, k,
        anchor = TextAnchor.TOP_CENTER,
    )
}

private enum class TextAnchor { TOP_LEFT, TOP_CENTER, CENTER }

private fun DrawScope.drawCanvasText(
    textMeasurer: TextMeasurer,
    text: String,
    fontPx: Float,
    color: Color,
    at: Offset,
    alpha: Float,
    k: Float,
    anchor: TextAnchor = TextAnchor.TOP_LEFT,
    center: Boolean = false,
) {
    val measured = textMeasurer.measure(
        AnnotatedString(text),
        // sp is scaled by the display density; converting the canvas pixels
        // through toSp() keeps the preview text the same size as the firmware
        // instead of 2-3x larger.
        style = TextStyle(
            fontSize = (fontPx * k).toSp(),
            color = color.copy(alpha = alpha),
        ),
    )
    val resolvedAnchor = if (center) TextAnchor.CENTER else anchor
    val topLeft = when (resolvedAnchor) {
        TextAnchor.TOP_LEFT -> at
        TextAnchor.TOP_CENTER ->
            Offset(at.x - measured.size.width / 2f, at.y)
        TextAnchor.CENTER ->
            Offset(at.x - measured.size.width / 2f, at.y - measured.size.height / 2f)
    }
    drawText(textLayoutResult = measured, topLeft = topLeft)
}

private fun DrawScope.drawSelectionOverlay(
    layout: UiLayout,
    k: Float,
    selected: LayoutComponentId,
    dragCenter: Offset?,
) {
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
        val x = if (dragCenter != null) dragCenter.x - w / 2f else component.x.toFloat()
        val y = if (dragCenter != null) dragCenter.y - h / 2f else component.y.toFloat()
        drawRoundRect(
            PreviewSelection,
            topLeft = Offset(x * k, y * k),
            size = Size(w * k, h * k),
            cornerRadius = CornerRadius(4f * k),
            style = Stroke(1.5f.dp.toPx()),
        )
    }
}
