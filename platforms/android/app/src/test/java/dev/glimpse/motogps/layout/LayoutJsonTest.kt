package dev.glimpse.motogps.layout

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * §9.3：布局 JSON 序列化与默认值测试（§4.9 结构 + 需求修正：
 * orientation 默认 north_up、basemap 无 source 字段）。
 */
class LayoutJsonTest {

    @Test
    fun defaultLayoutIsNorthUpWithEightComponents() {
        val layout = LayoutDefaults.defaultLayout()
        assertEquals(LayoutOrientation.NORTH_UP, layout.orientation)
        assertEquals(8, layout.components.size)
        assertEquals(true, layout.musicPageEnabled)
        assertEquals(232, layout.map.heightPx)
    }

    @Test
    fun defaultsMatchSection410() {
        val layout = LayoutDefaults.defaultLayout()
        // heading 默认关闭，其余开启。
        assertFalse(layout[LayoutComponentId.HEADING].enabled)
        listOf(
            LayoutComponentId.SPEED, LayoutComponentId.SPEED_LIMIT, LayoutComponentId.MANEUVER,
            LayoutComponentId.REMAINING_DISTANCE, LayoutComponentId.REMAINING_TIME,
            LayoutComponentId.PROGRESS_ARC, LayoutComponentId.SPEED_ARC,
        ).forEach { assertTrue("$it should default to enabled", layout[it].enabled) }
        // 进度弧上边 120°，速度弧下边 120°。
        assertEquals(LayoutArcAnchor.TOP, layout[LayoutComponentId.PROGRESS_ARC].anchor)
        assertEquals(120, layout[LayoutComponentId.PROGRESS_ARC].spanDeg)
        assertEquals(LayoutArcAnchor.BOTTOM, layout[LayoutComponentId.SPEED_ARC].anchor)
        assertEquals(120, layout[LayoutComponentId.SPEED_ARC].spanDeg)
    }

    @Test
    fun roundTripPreservesEveryField() {
        val original = LayoutDefaults.defaultLayout(revision = 12).let {
            it.copy(
                orientation = LayoutOrientation.HEADING_UP,
                musicPageEnabled = false,
                map = MapConfig(heightPx = 300, opacity = 80, zoomPercent = 150),
                basemap = it.basemap.copy(
                    theme = BasemapTheme.LIGHT,
                    route = RouteStyle(LayoutColor(0x30B0C8), 8, 90),
                    street = StreetStyle(LayoutColor(0xFF9500), 5, 60),
                    buildingOpacity = 40,
                    layers = LayerToggles(roads = false, buildings = true, route = true),
                ),
                components = it.components + mapOf(
                    LayoutComponentId.SPEED to it[LayoutComponentId.SPEED].copy(
                        enabled = false, x = 100, y = 120, size = LayoutSize.L, opacity = 55,
                    ),
                    LayoutComponentId.PROGRESS_ARC to it[LayoutComponentId.PROGRESS_ARC].copy(
                        anchor = LayoutArcAnchor.LEFT, spanDeg = 180, opacity = 40,
                    ),
                ),
            )
        }
        val decoded = LayoutJson.decode(LayoutJson.encode(original))
        assertNotNull(decoded)
        assertEquals(original, decoded)
    }

    @Test
    fun encodedJsonHasNoBasemapSourceField() {
        val json = JSONObject(LayoutJson.encode(LayoutDefaults.defaultLayout()))
        val basemap = json.getJSONObject("basemap")
        // 需求修正：不出现瓦片来源/影像入口。
        assertNull(basemap.opt("source"))
        assertEquals(
            setOf("theme", "route", "street", "building", "layers"),
            basemap.keySet(),
        )
    }

    @Test
    fun encodeUsesLowercaseKeysLikeSection49() {
        val json = JSONObject(LayoutJson.encode(LayoutDefaults.defaultLayout()))
        assertEquals("north_up", json.getString("orientation"))
        val components = json.getJSONObject("components")
        assertEquals(
            setOf(
                "speed", "speed_limit", "heading", "maneuver",
                "remaining_distance", "remaining_time", "progress_arc", "speed_arc",
            ),
            components.keySet(),
        )
        val progress = components.getJSONObject("progress_arc")
        assertEquals("top", progress.getString("anchor"))
        assertEquals(120, progress.getInt("span_deg"))
        val speed = components.getJSONObject("speed")
        assertEquals(185, speed.getInt("x"))
        assertEquals(300, speed.getInt("y"))
        assertEquals("M", speed.getString("size"))
    }

    @Test
    fun decodeRejectsUnknownSchema() {
        assertNull(LayoutJson.decode("""{"schema": 99}"""))
    }

    @Test
    fun decodeFallsBackToDefaultsForMissingFields() {
        val minimal = """{"schema":1,"revision":3}"""
        val decoded = LayoutJson.decode(minimal)
        assertNotNull(decoded)
        assertEquals(3, decoded!!.revision)
        assertEquals(LayoutOrientation.NORTH_UP, decoded.orientation)
        assertEquals(232, decoded.map.heightPx)
        assertTrue(decoded[LayoutComponentId.SPEED].enabled)
    }

    @Test
    fun decodeGarbageReturnsNull() {
        assertNull(LayoutJson.decode("not json at all"))
    }

    @Test
    fun decodeUnknownEnumValuesFallBackToDefaults() {
        val raw = """
            {"schema":1,"orientation":"sideways",
             "components":{"speed":{"enabled":true,"x":1,"y":2,"size":"XL","opacity":70}}}
        """.trimIndent()
        val decoded = LayoutJson.decode(raw)
        assertNotNull(decoded)
        assertEquals(LayoutOrientation.NORTH_UP, decoded!!.orientation)
        assertEquals(LayoutSize.M, decoded[LayoutComponentId.SPEED].size)
        assertEquals(70, decoded[LayoutComponentId.SPEED].opacity)
        assertEquals(1, decoded[LayoutComponentId.SPEED].x)
    }
}
