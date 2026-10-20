package dev.glimpse.motogps.layout

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * §9.3：钳制/吸附/圆弧角度纯函数单元测试。
 */
class LayoutGeometryTest {

    // --- 安全区钳制 --------------------------------------------------------

    @Test
    fun centerComponentNeverNeedsClamping() {
        val (x, y) = LayoutGeometry.clampToSafeArea(185, 195, 96, 72)
        assertEquals(185, x)
        assertEquals(195, y)
    }

    @Test
    fun cornerComponentOutsideSafeCircleIsPulledBack() {
        // 1×1 组件中心最远 220；把中心放到 (500, 500) 必然越界。
        val (x, y) = LayoutGeometry.clampToSafeArea(500, 500, 1, 1)
        val centerX = x + 0.5f
        val centerY = y + 0.5f
        val dist = kotlin.math.sqrt(
            (centerX - LayoutCanvas.CENTER) * (centerX - LayoutCanvas.CENTER) +
                (centerY - LayoutCanvas.CENTER) * (centerY - LayoutCanvas.CENTER),
        )
        assertTrue("clamped center must be within radius 220, was $dist", dist <= 220.5f)
    }

    @Test
    fun oversizedComponentFallsBackToCenter() {
        // 600×600 的组件对角线超过安全区直径：居中兜底。
        val (x, y) = LayoutGeometry.clampToSafeArea(10, 10, 600, 600)
        assertEquals((LayoutCanvas.CENTER - 300f).toInt(), x)
        assertEquals((LayoutCanvas.CENTER - 300f).toInt(), y)
    }

    @Test
    fun insideSafeAreaMatchesCornerRule() {
        // (233,233) 10×10：四角均在圆内。
        assertTrue(LayoutGeometry.insideSafeArea(228, 228, 10, 10))
        // (0,0) 10×10：左上角在圆外。
        assertFalse(LayoutGeometry.insideSafeArea(0, 0, 10, 10))
    }

    // --- 吸附 ---------------------------------------------------------------

    @Test
    fun gridSnapRoundsTo8px() {
        // 选靠近安全区中心的位置，避免被钳制逻辑改写（该路径由
        // clampToSafeArea 专属用例覆盖）。
        val (x1, y1) = LayoutGeometry.settleFreeComponent(185, 300, 96, 72)
        assertEquals(184, x1) // (185+4)/8*8 = 184
        assertEquals(304, y1) // (300+4)/8*8 = 304
        val (x2, y2) = LayoutGeometry.settleFreeComponent(181, 300, 96, 72)
        assertEquals(184, x2) // (181+4)/8*8 = 184
        assertEquals(304, y2)
        // 180 与 181 同格中点两侧，均落到 184。
        val (x3, y3) = LayoutGeometry.settleFreeComponent(180, 300, 96, 72)
        assertEquals(184, x3)
        assertEquals(304, y3)
    }

    @Test
    fun centerLineSnapBeatsGrid() {
        // 235 距中心线 233 只有 2px（≤6），优先吸附中心线而不是 8px 网格 240。
        val (x, _) = LayoutGeometry.settleFreeComponent(235, 40, 96, 72)
        assertEquals(233, x)
    }

    @Test
    fun centerLineOutsideThresholdKeepsGridSnap() {
        val (x, _) = LayoutGeometry.settleFreeComponent(245, 40, 96, 72)
        assertEquals(240, x)
    }

    @Test
    fun snapResultOutOfSafeAreaFallsBackToClampedPosition() {
        // 极端位置：吸附结果越界时回退钳制值，保证结果总在安全区内。
        val (x, y) = LayoutGeometry.settleFreeComponent(460, 460, 96, 72)
        assertTrue(
            "settle must stay inside safe area",
            LayoutGeometry.insideSafeArea(x, y, 96, 72),
        )
    }

    // --- 圆弧 ---------------------------------------------------------------

    @Test
    fun arcAnglesMatchLvglConvention() {
        // 上锚点中心角 270°，span 120° → start 210°。
        val (start, sweep) = LayoutGeometry.arcAngles(LayoutArcAnchor.TOP, 120)
        assertEquals(210f, start)
        assertEquals(120f, sweep)
        // 右锚点 0°，span 90° → start -45°（等价 315°）。
        val (rStart, rSweep) = LayoutGeometry.arcAngles(LayoutArcAnchor.RIGHT, 90)
        assertEquals(-45f, rStart)
        assertEquals(90f, rSweep)
    }

    @Test
    fun anchorPointsSitOnSafeCircle() {
        LayoutArcAnchor.entries.forEach { anchor ->
            val (x, y) = LayoutGeometry.arcAnchorPoint(anchor)
            val dist = kotlin.math.sqrt(
                (x - LayoutCanvas.CENTER) * (x - LayoutCanvas.CENTER) +
                    (y - LayoutCanvas.CENTER) * (y - LayoutCanvas.CENTER),
            )
            assertEquals("anchor $anchor must be at radius 220", 220f, dist, 0.01f)
        }
    }

    @Test
    fun nearestArcAnchorSnapsWithinThreshold() {
        // 上锚点槽 (233, 13)：距离 20px 内吸附。
        val anchor = LayoutGeometry.nearestArcAnchor(233f, 20f, LayoutArcAnchor.BOTTOM)
        assertEquals(LayoutArcAnchor.TOP, anchor)
    }

    @Test
    fun nearestArcAnchorKeepsCurrentWhenNothingWithinThreshold() {
        // 画布中心离所有锚点都远大于 24px。
        val anchor = LayoutGeometry.nearestArcAnchor(233f, 233f, LayoutArcAnchor.BOTTOM)
        assertEquals(LayoutArcAnchor.BOTTOM, anchor)
    }

    @Test
    fun sameSideArcsSplitOuterAndInnerDiameter() {
        val progress = LayoutDefaults.component(LayoutComponentId.PROGRESS_ARC)
            .copy(anchor = LayoutArcAnchor.TOP)
        val speed = LayoutDefaults.component(LayoutComponentId.SPEED_ARC)
            .copy(anchor = LayoutArcAnchor.TOP)
        val (outer, inner) = LayoutGeometry.arcDiameters(progress, speed)
        assertEquals(ComponentSizes.ARC_DIAMETER, outer)
        assertEquals(ComponentSizes.ARC_DIAMETER_INNER, inner)
    }

    @Test
    fun oppositeSideArcsBothUseOuterDiameter() {
        val progress = LayoutDefaults.component(LayoutComponentId.PROGRESS_ARC)
            .copy(anchor = LayoutArcAnchor.TOP)
        val speed = LayoutDefaults.component(LayoutComponentId.SPEED_ARC)
            .copy(anchor = LayoutArcAnchor.BOTTOM)
        val (a, b) = LayoutGeometry.arcDiameters(progress, speed)
        assertEquals(ComponentSizes.ARC_DIAMETER, a)
        assertEquals(ComponentSizes.ARC_DIAMETER, b)
    }

    // --- 缩放 → MapScene 窗口半径（§4.8） ----------------------------------

    @Test
    fun zoom100KeepsTheExisting500MetreWindow() {
        // 100% 必须与改造前的固定窗口一致，否则现有离线路网会突然变稀。
        assertEquals(500, LayoutGeometry.sceneRadiusM(100))
    }

    @Test
    fun zoomHalfDoublesTheWindowAndZoom200HalvesIt() {
        assertEquals(1000, LayoutGeometry.sceneRadiusM(50))
        assertEquals(250, LayoutGeometry.sceneRadiusM(200))
    }

    @Test
    fun zoomRadiusIsMonotonicAndClampedToProtocolRange() {
        var previous = LayoutGeometry.sceneRadiusM(50)
        for (zoom in 50..200) {
            val radius = LayoutGeometry.sceneRadiusM(zoom)
            assertTrue("radius must shrink as zoom grows (zoom=$zoom)", radius <= previous)
            assertTrue("radius below the minimum (zoom=$zoom)", radius >= LayoutGeometry.MIN_SCENE_RADIUS_M)
            assertTrue("radius above the maximum (zoom=$zoom)", radius <= LayoutGeometry.MAX_SCENE_RADIUS_M)
            previous = radius
        }
    }

    @Test
    fun outOfRangeZoomIsClampedNotExtrapolated() {
        // 协议是 50..200；越界值按边界处理，避免窗口被放大到不可用的范围。
        assertEquals(LayoutGeometry.sceneRadiusM(50), LayoutGeometry.sceneRadiusM(0))
        assertEquals(LayoutGeometry.sceneRadiusM(200), LayoutGeometry.sceneRadiusM(1000))
    }
}
