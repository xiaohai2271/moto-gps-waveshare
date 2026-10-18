package dev.glimpse.motogps.map

import dev.glimpse.motogps.NativeBridge
import kotlin.math.abs

/**
 * GCJ-02 -> WGS84 inverse.
 *
 * The shared C++ core only exposes WGS84 -> GCJ-02 (the provider-facing
 * direction). Tile numbering must use WGS84, so the inverse is computed with a
 * bounded bisection against that authoritative forward transform, mirroring
 * backend/src/coordinates.js (32 rounds, ±0.02°). No offset polynomial is
 * duplicated here.
 */
object GcjTransform {
    private const val MAX_OFFSET_DEG = 0.02
    private const val ROUNDS = 32
    private const val EPSILON = 1e-9

    data class Wgs84(val longitude: Double, val latitude: Double)

    fun gcj02ToWgs84(lon: Double, lat: Double): Wgs84? {
        if (!lon.isFinite() || !lat.isFinite()) return null
        var wgsLon = lon
        var wgsLat = lat
        var minLon = lon - MAX_OFFSET_DEG
        var maxLon = lon + MAX_OFFSET_DEG
        var minLat = lat - MAX_OFFSET_DEG
        var maxLat = lat + MAX_OFFSET_DEG

        repeat(ROUNDS) {
            wgsLon = (minLon + maxLon) / 2
            wgsLat = (minLat + maxLat) / 2
            val forward = NativeBridge.wgs84ToGcj02(wgsLon, wgsLat)
            if (forward[2] == 0.0) return null
            val dLon = forward[0] - lon
            val dLat = forward[1] - lat
            if (abs(dLon) < EPSILON && abs(dLat) < EPSILON) return@repeat
            if (dLon > 0) maxLon = wgsLon else minLon = wgsLon
            if (dLat > 0) maxLat = wgsLat else minLat = wgsLat
        }
        return Wgs84(wgsLon, wgsLat)
    }
}
