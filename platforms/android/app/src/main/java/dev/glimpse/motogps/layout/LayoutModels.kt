package dev.glimpse.motogps.layout

/**
 * 圆屏布局数据模型，对应 docs/NAV_LAYOUT_EDITOR_PLAN.md §4。
 *
 * 与 §4.9 的 JSON 一致（修正：orientation 默认 north_up；basemap 无 source
 * 字段——栅格影像本期不做，也不出现任何"瓦片来源/影像"入口）。
 *
 * 画布固定 466×466，原点左上，中心 (233, 233)；组件坐标是外接矩形左上角。
 * 布局数据是纯 Kotlin 值对象，不依赖 Android 框架，便于 JVM 单测。
 */

/** 466×466 画布常量（App 与固件各保留一份，文档 §4 锁定）。 */
object LayoutCanvas {
    const val SIZE = 466
    const val CENTER = SIZE / 2f
    /** 圆形安全区半径（§4.1，留 13px 边距）。 */
    const val SAFE_RADIUS = 220f
}

/** 组件 ID（协议 §4.2，1..8）。 */
enum class LayoutComponentId(val id: Int, val label: String, val isArc: Boolean) {
    SPEED(1, "实时速度", false),
    SPEED_LIMIT(2, "限速牌", false),
    HEADING(3, "当前方向", false),
    MANEUVER(4, "转向指示", false),
    REMAINING_DISTANCE(5, "剩余距离", false),
    REMAINING_TIME(6, "剩余时间", false),
    PROGRESS_ARC(7, "导航进度弧", true),
    SPEED_ARC(8, "速度/加速度弧", true);

    companion object {
        fun fromId(id: Int): LayoutComponentId? = entries.firstOrNull { it.id == id }
    }
}

/** 尺寸档位（协议 size：0 S / 1 M / 2 L）。 */
enum class LayoutSize(val code: Int, val label: String) {
    S(0, "小"), M(1, "中"), L(2, "大");

    companion object {
        fun fromCode(code: Int): LayoutSize = entries.firstOrNull { it.code == code } ?: M
    }
}

/** 圆弧锚点（协议 anchor：0 上 / 1 右 / 2 下 / 3 左；LVGL 角度 270/0/90/180）。 */
enum class LayoutArcAnchor(val code: Int, val label: String, val centerAngleDeg: Float) {
    TOP(0, "上", 270f),
    RIGHT(1, "右", 0f),
    BOTTOM(2, "下", 90f),
    LEFT(3, "左", 180f);

    companion object {
        fun fromCode(code: Int): LayoutArcAnchor =
            entries.firstOrNull { it.code == code } ?: TOP
    }
}

/** 弧长档位。 */
enum class LayoutArcSpan(val degrees: Int) {
    SPAN_90(90), SPAN_120(120), SPAN_180(180);

    companion object {
        fun fromDegrees(deg: Int): LayoutArcSpan =
            entries.firstOrNull { it.degrees == deg } ?: SPAN_120
    }
}

/** 方向模式（§4.7）：默认北向上；航向只来自手机，App 不涉及设备 IMU。 */
enum class LayoutOrientation(val json: String) {
    NORTH_UP("north_up"),
    HEADING_UP("heading_up");

    companion object {
        fun fromJson(value: String): LayoutOrientation =
            entries.firstOrNull { it.json == value } ?: NORTH_UP
    }
}

/** 底图主题预设（协议 theme_id：0 dark / 1 light / 2 high_contrast / 3 custom）。 */
enum class BasemapTheme(val code: Int, val label: String) {
    DARK(0, "深色"),
    LIGHT(1, "浅色"),
    HIGH_CONTRAST(2, "高对比");

    companion object {
        fun fromCode(code: Int): BasemapTheme =
            entries.firstOrNull { it.code == code } ?: DARK
    }
}

/** 0xRRGGBB 颜色的最小封装，避免引入 Compose 依赖。 */
data class LayoutColor(val rgb: Int) {
    val hex: String get() = "#%06X".format(rgb and 0xFFFFFF)

    companion object {
        fun fromHexOrNull(hex: String): LayoutColor? {
            val cleaned = hex.trim().removePrefix("#")
            if (cleaned.length != 6) return null
            val value = cleaned.toIntOrNull(16) ?: return null
            return LayoutColor(value)
        }

        fun fromHexOrDefault(hex: String, fallback: LayoutColor): LayoutColor =
            fromHexOrNull(hex) ?: fallback

        val WHITE = LayoutColor(0xFFFFFF)
        val STREET_GREY = LayoutColor(0x8E8E93)
    }
}

/**
 * 单个组件配置。非圆弧组件有 x/y（外接矩形左上角）；圆弧组件只有
 * anchor/spanDeg（x/y 固定为 0，由固件贴边绘制）。
 */
data class LayoutComponent(
    val id: LayoutComponentId,
    val enabled: Boolean,
    val size: LayoutSize = LayoutSize.M,
    /** 透明度 10..100（协议 0..100；低于 10 不允许，§4.6）。 */
    val opacity: Int = 100,
    val x: Int = 0,
    val y: Int = 0,
    val anchor: LayoutArcAnchor = LayoutArcAnchor.TOP,
    val spanDeg: Int = 120,
) {
    val isArc: Boolean get() = id.isArc
}

/** 地图区（§4.8：贴顶固定，只调高度与透明度）。 */
data class MapConfig(
    /** 160..466，偶数。 */
    val heightPx: Int = 232,
    /** 10..100。 */
    val opacity: Int = 100,
    /** 50..200。 */
    val zoomPercent: Int = 100,
)

/** 路线/街道样式。 */
data class RouteStyle(
    val color: LayoutColor = LayoutColor.WHITE,
    /** 1..16。 */
    val widthPx: Int = 6,
    /** 0..100。 */
    val opacity: Int = 100,
)

data class StreetStyle(
    val color: LayoutColor = LayoutColor.STREET_GREY,
    /** 1..12。 */
    val widthPx: Int = 3,
    /** 0..100。 */
    val opacity: Int = 35,
)

/** 图层显隐（协议 layer_mask bit0 道路 / bit1 建筑 / bit2 路线）。 */
data class LayerToggles(
    val roads: Boolean = true,
    val buildings: Boolean = true,
    val route: Boolean = true,
) {
    val mask: Int
        get() = (if (roads) 1 else 0) or (if (buildings) 2 else 0) or (if (route) 4 else 0)
}

/** 底图样式（v1 只做矢量主题；没有 source 字段，不提供影像入口）。 */
data class BasemapConfig(
    val theme: BasemapTheme = BasemapTheme.DARK,
    val route: RouteStyle = RouteStyle(),
    val street: StreetStyle = StreetStyle(),
    /** 0..100。 */
    val buildingOpacity: Int = 25,
    val layers: LayerToggles = LayerToggles(),
)

/** 布局根模型。revision 是手机侧单调递增版本号，仅用于同步/去重。 */
data class UiLayout(
    val schema: Int = 1,
    val revision: Int = 0,
    val orientation: LayoutOrientation = LayoutOrientation.NORTH_UP,
    val musicPageEnabled: Boolean = true,
    val map: MapConfig = MapConfig(),
    val basemap: BasemapConfig = BasemapConfig(),
    val components: Map<LayoutComponentId, LayoutComponent> =
        LayoutDefaults.COMPONENTS.associateBy { it.id },
) {
    operator fun get(id: LayoutComponentId): LayoutComponent =
        components[id] ?: LayoutDefaults.component(id)
}

/** §4.10 默认布局 + §4.2/§4.3 尺寸表。 */
object LayoutDefaults {
    val COMPONENTS: List<LayoutComponent> = listOf(
        LayoutComponent(LayoutComponentId.SPEED, enabled = true, x = 185, y = 300),
        LayoutComponent(LayoutComponentId.SPEED_LIMIT, enabled = true, x = 300, y = 238),
        LayoutComponent(LayoutComponentId.HEADING, enabled = false, x = 24, y = 40),
        LayoutComponent(LayoutComponentId.MANEUVER, enabled = true, x = 29, y = 220),
        // M sizes: distance 116 wide (120–236), time 100 wide (270–370);
        // the old 150/250 pair overlapped by 16 px (device-verified).
        LayoutComponent(LayoutComponentId.REMAINING_DISTANCE, enabled = true, x = 120, y = 390, opacity = 90),
        LayoutComponent(LayoutComponentId.REMAINING_TIME, enabled = true, x = 270, y = 390, opacity = 90),
        LayoutComponent(
            LayoutComponentId.PROGRESS_ARC, enabled = true,
            anchor = LayoutArcAnchor.TOP, spanDeg = 120, opacity = 80,
        ),
        LayoutComponent(
            LayoutComponentId.SPEED_ARC, enabled = true,
            anchor = LayoutArcAnchor.BOTTOM, spanDeg = 120, opacity = 80,
        ),
    )

    fun component(id: LayoutComponentId): LayoutComponent =
        COMPONENTS.first { it.id == id }

    fun defaultLayout(revision: Int = 0): UiLayout = UiLayout(
        revision = revision,
        orientation = LayoutOrientation.NORTH_UP,
        components = COMPONENTS.associateBy { it.id },
    )
}

/** §4.3 尺寸表：非圆弧组件外接矩形（S/M/L），466 画布设计像素。 */
object ComponentSizes {
    private data class SizeTriple(val s: Pair<Int, Int>, val m: Pair<Int, Int>, val l: Pair<Int, Int>)

    private val table: Map<LayoutComponentId, SizeTriple> = mapOf(
        LayoutComponentId.SPEED to SizeTriple(72 to 56, 96 to 72, 124 to 92),
        LayoutComponentId.SPEED_LIMIT to SizeTriple(44 to 44, 61 to 61, 78 to 78),
        LayoutComponentId.HEADING to SizeTriple(68 to 60, 88 to 76, 112 to 96),
        LayoutComponentId.MANEUVER to SizeTriple(150 to 108, 180 to 128, 210 to 148),
        LayoutComponentId.REMAINING_DISTANCE to SizeTriple(92 to 44, 116 to 56, 146 to 68),
        LayoutComponentId.REMAINING_TIME to SizeTriple(78 to 44, 100 to 56, 128 to 68),
    )

    /** @return (width, height)；圆弧组件返回 0×0（几何由弧规则决定）。 */
    fun sizeOf(id: LayoutComponentId, size: LayoutSize): Pair<Int, Int> =
        when (id) {
            LayoutComponentId.PROGRESS_ARC, LayoutComponentId.SPEED_ARC -> 0 to 0
            else -> table[id]?.let {
                when (size) {
                    LayoutSize.S -> it.s
                    LayoutSize.M -> it.m
                    LayoutSize.L -> it.l
                }
            } ?: (0 to 0)
        }

    /** 圆弧直径 440；两弧同侧时内弧自动缩到 408（§4.3）。 */
    const val ARC_DIAMETER = 440
    const val ARC_DIAMETER_INNER = 408

    /** 弧宽档位 4/6/8。 */
    fun arcStroke(size: LayoutSize): Int = when (size) {
        LayoutSize.S -> 4
        LayoutSize.M -> 6
        LayoutSize.L -> 8
    }
}
