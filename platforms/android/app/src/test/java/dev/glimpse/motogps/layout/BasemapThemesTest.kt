package dev.glimpse.motogps.layout

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * §P2 底图主题预设：切换主题必须真的改写样式字段（固件只认具体样式，
 * 不解释 theme_id），且改写后仍落在协议允许的范围内。
 */
class BasemapThemesTest {

    @Test
    fun darkPresetReproducesTheShippedDefaultStyle() {
        // 深色是既有默认视觉；切回深色应与默认 BasemapConfig 完全一致，
        // 否则"恢复默认"和"选深色"会得到两种画面。
        val defaults = BasemapConfig()
        val dark = BasemapThemes.styleFor(BasemapTheme.DARK)
        assertEquals(defaults.route, dark.route)
        assertEquals(defaults.street, dark.street)
        assertEquals(defaults.buildingOpacity, dark.buildingOpacity)
    }

    @Test
    fun eachThemeProducesADistinctVisibleStyle() {
        val styles = BasemapTheme.entries.map { BasemapThemes.styleFor(it) }
        for (i in styles.indices) {
            for (j in i + 1 until styles.size) {
                assertNotEquals(
                    "themes $i and $j must not look identical",
                    styles[i],
                    styles[j],
                )
            }
        }
    }

    @Test
    fun switchingThemesRewritesStyleFieldsButKeepsLayerToggles() {
        val start = BasemapConfig(
            layers = LayerToggles(roads = true, buildings = false, route = false),
        )
        val switched = BasemapThemes.apply(start, BasemapTheme.HIGH_CONTRAST)
        assertEquals(BasemapTheme.HIGH_CONTRAST, switched.theme)
        assertEquals(BasemapThemes.HIGH_CONTRAST.route, switched.route)
        assertEquals(BasemapThemes.HIGH_CONTRAST.street, switched.street)
        assertEquals(BasemapThemes.HIGH_CONTRAST.buildingOpacity, switched.buildingOpacity)
        // 图层显隐是用户选择，不能被换主题顺手改掉。
        assertEquals(start.layers, switched.layers)
    }

    @Test
    fun everyPresetStaysInsideTheProtocolBounds() {
        // 编码器会拒绝越界值（§5.3），预设必须天生合法。
        for (theme in BasemapTheme.entries) {
            val style = BasemapThemes.styleFor(theme)
            assertTrue(
                "route width out of 1..16 for $theme",
                style.route.widthPx in 1..16,
            )
            assertTrue(
                "route opacity out of 0..100 for $theme",
                style.route.opacity in 0..100,
            )
            assertTrue(
                "street width out of 1..12 for $theme",
                style.street.widthPx in 1..12,
            )
            assertTrue(
                "street opacity out of 0..100 for $theme",
                style.street.opacity in 0..100,
            )
            assertTrue(
                "building opacity out of 0..100 for $theme",
                style.buildingOpacity in 0..100,
            )
            assertTrue(
                "route colour must fit 0xRRGGBB",
                style.route.color.rgb in 0x000000..0xFFFFFF,
            )
            assertTrue(
                "street colour must fit 0xRRGGBB",
                style.street.color.rgb in 0x000000..0xFFFFFF,
            )
        }
    }

    @Test
    fun switchingThemesIsIdempotent() {
        // 同一主题连点两次不能继续改动布局（避免 revision 空转推送）。
        val once = BasemapThemes.apply(BasemapConfig(), BasemapTheme.LIGHT)
        val twice = BasemapThemes.apply(once, BasemapTheme.LIGHT)
        assertEquals(once, twice)
    }
}
