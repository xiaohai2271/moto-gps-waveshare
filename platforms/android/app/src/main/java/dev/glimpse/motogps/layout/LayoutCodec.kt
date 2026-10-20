package dev.glimpse.motogps.layout

import dev.glimpse.motogps.NativeBridge

/**
 * 模型 → BLE payload 映射（§7.4：编码走共享 C++ JNI，不在 Kotlin 重复
 * 实现字节格式）。C++ 侧拥有 wire 布局与校验，这里只做 int 数组映射。
 */
object LayoutCodec {

    /** flags bit0 north_up, bit1 music_page_enabled, bit2 map_visible。 */
    const val FLAG_NORTH_UP = 0x01
    const val FLAG_MUSIC_PAGE_ENABLED = 0x02
    const val FLAG_MAP_VISIBLE = 0x04

    /** 组件 cflags bit0 enabled。 */
    const val COMPONENT_FLAG_ENABLED = 0x01

    /**
     * 编码为 UiLayout payload（payload_revision=1）。
     * @throws IllegalStateException 由 C++ 校验器抛出（非法值/字段不匹配）。
     */
    fun encode(layout: UiLayout): ByteArray {
        val components = layout.components.values
            .sortedBy { it.id.id }
            .map(::componentToInts)
            .flatten()
            .toIntArray()

        val fixed = intArrayOf(
            layout.revision,
            globalFlags(layout),
            layout.basemap.theme.code,
            layout.map.heightPx,
            layout.map.opacity,
            layout.basemap.route.color.rgb,
            layout.basemap.route.widthPx,
            layout.basemap.route.opacity,
            layout.basemap.street.color.rgb,
            layout.basemap.street.widthPx,
            layout.basemap.street.opacity,
            layout.basemap.buildingOpacity,
            layout.basemap.layers.mask,
            layout.map.zoomPercent,
            layout.components.size,
        )
        return NativeBridge.encodeUiLayout(fixed, components)
    }

    fun globalFlags(layout: UiLayout): Int {
        var flags = 0
        if (layout.orientation == LayoutOrientation.NORTH_UP) flags = flags or FLAG_NORTH_UP
        if (layout.musicPageEnabled) flags = flags or FLAG_MUSIC_PAGE_ENABLED
        if (layout.map.opacity > 0) flags = flags or FLAG_MAP_VISIBLE
        return flags
    }

    private fun componentToInts(c: LayoutComponent): List<Int> = listOf(
        c.id.id,
        (if (c.enabled) COMPONENT_FLAG_ENABLED else 0) or
            ((c.textOpacity.coerceIn(0, 100) and 0x7F) shl 1),
        c.size.code,
        c.opacity.coerceIn(0, 100),
        if (c.isArc) 0 else c.x,
        if (c.isArc) 0 else c.y,
        if (c.isArc) c.anchor.code else 0,
        if (c.isArc) c.spanDeg else 0,
    )

    /**
     * 解码 UiLayoutState payload。
     * @return [revision, layoutRevision, source, supportedMask, flags, reserved]
     *         的数据类；解码失败返回 null。
     */
    fun decodeState(payload: ByteArray): DeviceLayoutState? = runCatching {
        val v = NativeBridge.decodeUiLayoutState(payload)
        DeviceLayoutState(
            revision = v[0],
            layoutRevision = v[1],
            source = v[2],
            supportedMask = v[3],
            flags = v[4],
            reserved = v[5],
        )
    }.getOrNull()

    /** source：0 default / 1 nvs / 2 phone。 */
    const val SOURCE_DEFAULT = 0
    const val SOURCE_NVS = 1
    const val SOURCE_PHONE = 2

    /** UiLayoutState flags bit0 nvs_saved。 */
    const val STATE_FLAG_NVS_SAVED = 0x01

    /** supported_mask bit0..bit7 对应组件 ID 1..8。 */
    fun isComponentSupported(supportedMask: Int, id: LayoutComponentId): Boolean =
        supportedMask and (1 shl (id.id - 1)) != 0
}

/** 解码后的 UiLayoutState（协议 §5.4）。 */
data class DeviceLayoutState(
    val revision: Int,
    val layoutRevision: Int,
    val source: Int,
    val supportedMask: Int,
    val flags: Int,
    val reserved: Int,
) {
    val nvsSaved: Boolean get() = flags and LayoutCodec.STATE_FLAG_NVS_SAVED != 0
}
