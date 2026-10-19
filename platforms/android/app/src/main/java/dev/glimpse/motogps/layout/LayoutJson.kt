package dev.glimpse.motogps.layout

import org.json.JSONObject

/**
 * 布局 JSON 序列化（§4.9 结构，App 本地存储用）。org.json 在 JVM 单测
 * 可用（testImplementation org.json:json），无需 Android 运行时。
 */
object LayoutJson {

    private const val SCHEMA = 1

    fun encode(layout: UiLayout): String {
        val root = JSONObject()
        root.put("schema", SCHEMA)
        root.put("revision", layout.revision)
        root.put("orientation", layout.orientation.json)
        root.put("music_page_enabled", layout.musicPageEnabled)

        val map = JSONObject()
        map.put("height_px", layout.map.heightPx)
        map.put("opacity", layout.map.opacity)
        map.put("zoom_percent", layout.map.zoomPercent)
        root.put("map", map)

        val basemap = JSONObject()
        basemap.put("theme", layout.basemap.theme.name.lowercase())
        val route = JSONObject()
        route.put("color", layout.basemap.route.color.hex)
        route.put("width_px", layout.basemap.route.widthPx)
        route.put("opacity", layout.basemap.route.opacity)
        basemap.put("route", route)
        val street = JSONObject()
        street.put("color", layout.basemap.street.color.hex)
        street.put("width_px", layout.basemap.street.widthPx)
        street.put("opacity", layout.basemap.street.opacity)
        basemap.put("street", street)
        val building = JSONObject()
        building.put("opacity", layout.basemap.buildingOpacity)
        basemap.put("building", building)
        val layers = JSONObject()
        layers.put("roads", layout.basemap.layers.roads)
        layers.put("buildings", layout.basemap.layers.buildings)
        layers.put("route", layout.basemap.layers.route)
        basemap.put("layers", layers)
        root.put("basemap", basemap)

        val components = JSONObject()
        layout.components.values.forEach { c ->
            val o = JSONObject()
            o.put("enabled", c.enabled)
            if (c.isArc) {
                o.put("anchor", c.anchor.name.lowercase())
                o.put("span_deg", c.spanDeg)
            } else {
                o.put("x", c.x)
                o.put("y", c.y)
                o.put("size", c.size.name)
            }
            o.put("opacity", c.opacity)
            components.put(c.id.name.lowercase(), o)
        }
        root.put("components", components)
        return root.toString()
    }

    /** 解析失败或 schema 不认识时返回 null，由调用方回退默认布局。 */
    fun decode(raw: String): UiLayout? = runCatching {
        val root = JSONObject(raw)
        if (root.optInt("schema", 0) != SCHEMA) return null
        val defaults = LayoutDefaults.defaultLayout()

        val mapObj = root.optJSONObject("map") ?: JSONObject()
        val map = MapConfig(
            heightPx = mapObj.optInt("height_px", defaults.map.heightPx),
            opacity = mapObj.optInt("opacity", defaults.map.opacity),
            zoomPercent = mapObj.optInt("zoom_percent", defaults.map.zoomPercent),
        )

        val baseObj = root.optJSONObject("basemap") ?: JSONObject()
        val routeObj = baseObj.optJSONObject("route") ?: JSONObject()
        val streetObj = baseObj.optJSONObject("street") ?: JSONObject()
        val buildingObj = baseObj.optJSONObject("building") ?: JSONObject()
        val layersObj = baseObj.optJSONObject("layers") ?: JSONObject()
        val basemap = BasemapConfig(
            theme = themeFromJson(baseObj.optString("theme", "dark")),
            route = RouteStyle(
                color = LayoutColor.fromHexOrDefault(routeObj.optString("color", "#FFFFFF"), LayoutColor.WHITE),
                widthPx = routeObj.optInt("width_px", 6),
                opacity = routeObj.optInt("opacity", 100),
            ),
            street = StreetStyle(
                color = LayoutColor.fromHexOrDefault(streetObj.optString("color", "#8E8E93"), LayoutColor.STREET_GREY),
                widthPx = streetObj.optInt("width_px", 3),
                opacity = streetObj.optInt("opacity", 35),
            ),
            buildingOpacity = buildingObj.optInt("opacity", 25),
            layers = LayerToggles(
                roads = layersObj.optBoolean("roads", true),
                buildings = layersObj.optBoolean("buildings", true),
                route = layersObj.optBoolean("route", true),
            ),
        )

        val componentsObj = root.optJSONObject("components") ?: JSONObject()
        val components = LayoutDefaults.COMPONENTS.map { default ->
            val o = componentsObj.optJSONObject(default.id.name.lowercase()) ?: JSONObject()
            if (default.isArc) {
                default.copy(
                    enabled = o.optBoolean("enabled", default.enabled),
                    anchor = anchorFromJson(o.optString("anchor", default.anchor.name.lowercase())),
                    spanDeg = o.optInt("span_deg", default.spanDeg),
                    opacity = o.optInt("opacity", default.opacity),
                )
            } else {
                default.copy(
                    enabled = o.optBoolean("enabled", default.enabled),
                    x = o.optInt("x", default.x),
                    y = o.optInt("y", default.y),
                    size = sizeFromJson(o.optString("size", default.size.name)),
                    opacity = o.optInt("opacity", default.opacity),
                )
            }
        }.associateBy { it.id }

        UiLayout(
            schema = SCHEMA,
            revision = root.optInt("revision", 0),
            orientation = orientationFromJson(root.optString("orientation", "north_up")),
            musicPageEnabled = root.optBoolean("music_page_enabled", true),
            map = map,
            basemap = basemap,
            components = components,
        )
    }.getOrNull()

    private fun themeFromJson(value: String): BasemapTheme =
        BasemapTheme.entries.firstOrNull { it.name.lowercase() == value } ?: BasemapTheme.DARK

    private fun orientationFromJson(value: String): LayoutOrientation = LayoutOrientation.fromJson(value)

    private fun anchorFromJson(value: String): LayoutArcAnchor =
        LayoutArcAnchor.entries.firstOrNull { it.name.lowercase() == value } ?: LayoutArcAnchor.TOP

    private fun sizeFromJson(value: String): LayoutSize =
        LayoutSize.entries.firstOrNull { it.name == value } ?: LayoutSize.M
}
