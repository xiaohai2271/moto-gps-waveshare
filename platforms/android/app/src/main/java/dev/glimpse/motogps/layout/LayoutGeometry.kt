package dev.glimpse.motogps.layout

import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sqrt

/**
 * 布局几何纯函数：钳制、吸附、圆弧角度（§4.4/§4.5）。与固件各保留一份
 * 常量；JVM 单测锁定行为。
 */
object LayoutGeometry {

    /** 吸附阈值（设计像素）。 */
    const val GRID = 8
    const val GRID_SNAP = 8
    const val CENTER_LINE_SNAP = 6
    const val ARC_ANCHOR_SNAP = 24

    /**
     * 圆形安全区钳制：外接矩形四角必须在半径 220 的圆内。越界时沿最近
     * 方向回弹——这里用简单的轴向投影：把矩形中心限制在
     * (SAFE_RADIUS - halfDiagonal) 的圆内，等价于四角约束的最保守实现，
     * 且拖拽体验稳定（无跳变）。
     */
    fun clampToSafeArea(x: Int, y: Int, width: Int, height: Int): Pair<Int, Int> {
        if (width <= 0 || height <= 0) return x to y
        val halfDiag = sqrt(
            (width / 2f) * (width / 2f) + (height / 2f) * (height / 2f),
        )
        val maxCenterRadius = LayoutCanvas.SAFE_RADIUS - halfDiag
        if (maxCenterRadius <= 0f) {
            // 组件对角线超过安全区直径：居中兜底。
            val cx = (LayoutCanvas.CENTER - width / 2f).toInt()
            val cy = (LayoutCanvas.CENTER - height / 2f).toInt()
            return cx to cy
        }
        val centerX = x + width / 2f
        val centerY = y + height / 2f
        val dx = centerX - LayoutCanvas.CENTER
        val dy = centerY - LayoutCanvas.CENTER
        val dist = sqrt(dx * dx + dy * dy)
        return if (dist <= maxCenterRadius) {
            x to y
        } else {
            val scale = maxCenterRadius / dist
            val newCx = LayoutCanvas.CENTER + dx * scale
            val newCy = LayoutCanvas.CENTER + dy * scale
            (newCx - width / 2f).toInt() to (newCy - height / 2f).toInt()
        }
    }

    /** 外接矩形四角是否全部落在安全圆内（校验用）。 */
    fun insideSafeArea(x: Int, y: Int, width: Int, height: Int): Boolean {
        if (width <= 0 || height <= 0) return true
        val r2 = LayoutCanvas.SAFE_RADIUS * LayoutCanvas.SAFE_RADIUS
        val cx = LayoutCanvas.CENTER
        val cy = LayoutCanvas.CENTER
        for (cornerX in intArrayOf(x, x + width)) {
            for (cornerY in intArrayOf(y, y + height)) {
                val dx = cornerX - cx
                val dy = cornerY - cy
                if (dx * dx + dy * dy > r2) return false
            }
        }
        return true
    }

    /**
     * 非圆弧组件松手位置：先钳制到安全区，再吸附（中线 6px 优先，其次
     * 8px 网格），吸附结果若越界则回退钳制值。
     */
    fun settleFreeComponent(x: Int, y: Int, width: Int, height: Int): Pair<Int, Int> {
        val (cx, cy) = clampToSafeArea(x, y, width, height)
        var sx = snapToGrid(cx)
        var sy = snapToGrid(cy)
        sx = snapToCenterLine(cx, sx)
        sy = snapToCenterLine(cy, sy)
        if (!insideSafeArea(sx, sy, width, height)) {
            sx = snapToGrid(cx)
            if (!insideSafeArea(sx, sy, width, height)) sx = cx
            if (!insideSafeArea(sx, sy, width, height)) sy = snapToGrid(cy)
            if (!insideSafeArea(sx, sy, width, height)) sy = cy
        }
        return sx to sy
    }

    private fun snapToGrid(v: Int): Int = ((v + GRID / 2) / GRID) * GRID

    /** 值在中心线 233 的 6px 邻域内时吸附到中心线。 */
    fun snapToCenterLine(value: Int, snapped: Int): Int =
        if (abs(value - LayoutCanvas.CENTER) <= CENTER_LINE_SNAP) {
            LayoutCanvas.CENTER.toInt()
        } else {
            snapped
        }

    /**
     * 圆弧松手：拖拽中心点吸附到最近的锚点槽（阈值 24px），不允许停在
     * 非锚点位置。返回 null 表示没有任何锚点在阈值内（保持原锚点）。
     */
    fun nearestArcAnchor(
        pointX: Float,
        pointY: Float,
        current: LayoutArcAnchor,
    ): LayoutArcAnchor {
        var best = current
        var bestDist = Float.MAX_VALUE
        for (anchor in LayoutArcAnchor.entries) {
            val (ax, ay) = arcAnchorPoint(anchor)
            val d = sqrt((pointX - ax) * (pointX - ax) + (pointY - ay) * (pointY - ay))
            if (d < bestDist) {
                bestDist = d
                best = anchor
            }
        }
        return if (bestDist <= ARC_ANCHOR_SNAP) best else current
    }

    /**
     * 锚点槽位置：安全圆边界上锚点中心角对应的点（编辑器拖拽把手）。
     */
    fun arcAnchorPoint(anchor: LayoutArcAnchor): Pair<Float, Float> {
        val angleRad = Math.toRadians(anchor.centerAngleDeg.toDouble())
        val r = LayoutCanvas.SAFE_RADIUS
        val x = LayoutCanvas.CENTER + (r * kotlin.math.cos(angleRad)).toFloat()
        val y = LayoutCanvas.CENTER + (r * kotlin.math.sin(angleRad)).toFloat()
        return x to y
    }

    /**
     * 圆弧弧段角度（§4.5，与固件一致）：LVGL 0° = 右（3 点钟）顺时针。
     * @return (startDeg, sweepDeg) 供 Compose drawArc 使用。
     */
    fun arcAngles(anchor: LayoutArcAnchor, spanDeg: Int): Pair<Float, Float> {
        val span = spanDeg.toFloat()
        val start = anchor.centerAngleDeg - span / 2f
        // Compose drawArc 的 startAngle 0° = 右、sweep 正方向为顺时针，
        // 与 LVGL 约定一致，无需换算。
        return start to span
    }

    /**
     * 同侧双弧的内外分层（§4.3）：两个圆弧停在同一锚点时，speed_arc
     * 用内径 408，progress_arc 用外径 440。
     */
    fun arcDiameters(
        progress: LayoutComponent,
        speed: LayoutComponent,
    ): Pair<Int, Int> {
        val sameSide = progress.anchor == speed.anchor &&
            progress.enabled && speed.enabled
        return if (sameSide) {
            ComponentSizes.ARC_DIAMETER to ComponentSizes.ARC_DIAMETER_INNER
        } else {
            ComponentSizes.ARC_DIAMETER to ComponentSizes.ARC_DIAMETER
        }
    }

    /** 拖拽过程中组件中心点 → 左上角坐标（画布坐标，未钳制）。 */
    fun topLeftFromCenter(
        centerX: Float,
        centerY: Float,
        width: Int,
        height: Int,
    ): Pair<Int, Int> =
        (centerX - width / 2f).toInt() to (centerY - height / 2f).toInt()

    /** 数值范围钳制。 */
    fun clamp(v: Int, min: Int, max: Int): Int = min(max(v, min), max)

    /** 100% 缩放对应的 MapScene 窗口半径（米）。 */
    const val BASE_SCENE_RADIUS_M = 500
    const val MIN_SCENE_RADIUS_M = 100
    const val MAX_SCENE_RADIUS_M = 1500

    /**
     * 布局缩放 → MapScene 窗口半径（§4.8）：
     * `radius_m = clamp(500 × 100 / zoom_percent, 100, 1500)`。
     *
     * 100% 保持既有 500 m；50% 约 1 km；200% 约 250 m。设备侧按同一个
     * zoom_percent 缩放投影（`0.44 px/m × zoom`），两侧以 100% 为共同基线，
     * 因此 50%/200% 时路线与道路仍然对齐。
     */
    fun sceneRadiusM(zoomPercent: Int): Int {
        val zoom = clamp(zoomPercent, 50, 200)
        val radius = (BASE_SCENE_RADIUS_M * 100.0 / zoom).roundToInt()
        return clamp(radius, MIN_SCENE_RADIUS_M, MAX_SCENE_RADIUS_M)
    }
}
