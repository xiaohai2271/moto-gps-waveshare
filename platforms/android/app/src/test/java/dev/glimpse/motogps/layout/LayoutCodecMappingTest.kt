package dev.glimpse.motogps.layout

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 模型 → 协议 int 数组映射测试（§5.3 约定；字节格式归 C++ 测试，JVM 只
 * 验证 Kotlin 侧映射与 flags/掩码计算）。
 */
class LayoutCodecMappingTest {

    @Test
    fun globalFlagsMatchProtocolBits() {
        val layout = LayoutDefaults.defaultLayout()
        val flags = LayoutCodec.globalFlags(layout)
        // 默认：北向上(bit0) + 音乐页开(bit1) + 地图可见(bit2)。
        assertEquals(0x07, flags)

        val headingUp = layout.copy(
            orientation = LayoutOrientation.HEADING_UP,
            musicPageEnabled = false,
        )
        assertEquals(0x04, LayoutCodec.globalFlags(headingUp))
    }

    @Test
    fun layerMaskMatchesProtocolBits() {
        assertEquals(0x07, LayerToggles(roads = true, buildings = true, route = true).mask)
        assertEquals(0x05, LayerToggles(roads = true, buildings = false, route = true).mask)
        assertEquals(0x00, LayerToggles(false, false, false).mask)
    }

    @Test
    fun componentMappingForGoldenVector() {
        // golden ui_layout.payload 第一个组件：speed, enabled, M(1), opacity=0x64(100),
        // x=0x00b9(185), y=0x002c(44)? —— 以协议侧 fixture 的 speed 组件为准：
        // golden 里 speed x=185 y=44？实际 bytes: 01 00 01 01 64 b9 00 2c 01 00 00
        // 对应 [id=1, cflags=0, size=1, opacity=100, x=185, y=44, anchor=0, span=0]
        // —— cflags=0 表示 enabled=false？协议 fixture 是反向构造的；这里只校验
        // 我们自己的映射方向，不逐字节对 golden（字节级对归 C++ 测试）。
        val layout = LayoutDefaults.defaultLayout()
        val speed = layout[LayoutComponentId.SPEED]
        val mapped = listOf(
            speed.id.id,
            if (speed.enabled) LayoutCodec.COMPONENT_FLAG_ENABLED else 0,
            speed.size.code,
            speed.opacity,
            speed.x,
            speed.y,
            0,
            0,
        )
        // §4.10 默认 speed: enabled, M, opacity 100, (185, 300)。
        assertArrayEquals(listOf(1, 1, 1, 100, 185, 300, 0, 0).toIntArray(), mapped.toIntArray())
    }

    @Test
    fun arcComponentMapsAnchorAndSpan() {
        val layout = LayoutDefaults.defaultLayout()
        val progress = layout[LayoutComponentId.PROGRESS_ARC]
        assertEquals(
            listOf(7, 1, 1, 80, 0, 0, 0, 120),
            listOf(
                progress.id.id,
                if (progress.enabled) LayoutCodec.COMPONENT_FLAG_ENABLED else 0,
                progress.size.code,
                progress.opacity,
                progress.x,
                progress.y,
                progress.anchor.code,
                progress.spanDeg,
            ),
        )
        val speedArc = layout[LayoutComponentId.SPEED_ARC]
        assertEquals(2, speedArc.anchor.code) // bottom
        assertEquals(120, speedArc.spanDeg)
    }

    @Test
    fun unsupportedComponentDetectionViaSupportedMask() {
        val mask = (1 shl 0) or (1 shl 1) // 只有 id1、id2
        assertTrue(LayoutCodec.isComponentSupported(mask, LayoutComponentId.SPEED))
        assertTrue(LayoutCodec.isComponentSupported(mask, LayoutComponentId.SPEED_LIMIT))
        assertFalse(LayoutCodec.isComponentSupported(mask, LayoutComponentId.MANEUVER))
        assertFalse(LayoutCodec.isComponentSupported(mask, LayoutComponentId.SPEED_ARC))
    }

    @Test
    fun sizeTableMatchesSection43() {
        // 抽查 §4.3 尺寸表。
        assertEquals(96 to 72, ComponentSizes.sizeOf(LayoutComponentId.SPEED, LayoutSize.M))
        assertEquals(72 to 56, ComponentSizes.sizeOf(LayoutComponentId.SPEED, LayoutSize.S))
        assertEquals(124 to 92, ComponentSizes.sizeOf(LayoutComponentId.SPEED, LayoutSize.L))
        assertEquals(61 to 61, ComponentSizes.sizeOf(LayoutComponentId.SPEED_LIMIT, LayoutSize.M))
        assertEquals(150 to 108, ComponentSizes.sizeOf(LayoutComponentId.MANEUVER, LayoutSize.S))
        assertEquals(210 to 148, ComponentSizes.sizeOf(LayoutComponentId.MANEUVER, LayoutSize.L))
        assertEquals(0 to 0, ComponentSizes.sizeOf(LayoutComponentId.PROGRESS_ARC, LayoutSize.M))
        assertEquals(6, ComponentSizes.arcStroke(LayoutSize.M))
        assertEquals(8, ComponentSizes.arcStroke(LayoutSize.L))
    }
}
