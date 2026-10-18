package dev.glimpse.motogps.map

import kotlin.math.PI
import kotlin.math.asinh
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.tan

/**
 * z15 Web Mercator tile planning, ported from
 * platforms/ios/App/Adapters/OfflineMap/MapTilePlanner.swift.
 *
 * Tile numbers are computed from WGS84 coordinates. A GCJ-02 position (the
 * coordinate system used by the gateway's route geometry) must be converted to
 * WGS84 first — that conversion happens exactly once, at this boundary.
 */
object MapTilePlanner {
    const val ZOOM = 15
    const val MAX_TILES = 40_000
    const val MAX_LATITUDE = 85.05112878
    private const val METRES_PER_DEGREE = 111_195.0
    private const val CORRIDOR_STEP_M = 400.0
    private const val CORRIDOR_RADIUS_M = 1_200.0
    private const val MAX_SAMPLES = 100_000

    data class TileId(val z: Int, val x: Int, val y: Int) : Comparable<TileId> {
        override fun compareTo(other: TileId): Int =
            compareValuesBy(this, other, TileId::y, TileId::x)

        override fun toString(): String = "$z/$x/$y"
    }

    sealed class PlanningError(message: String) : Exception(message) {
        class InvalidCoordinates(message: String) : PlanningError(message)
        class AreaTooLarge(message: String) : PlanningError(message)
        class TooManySamples(message: String) : PlanningError(message)
    }

    fun webMercatorTile(longitudeDeg: Double, latitudeDeg: Double): TileId {
        val scale = 1 shl ZOOM
        val x = floor((longitudeDeg + 180.0) / 360.0 * scale).toInt().coerceIn(0, scale - 1)
        val latRad = latitudeDeg.coerceIn(-MAX_LATITUDE, MAX_LATITUDE) * PI / 180.0
        val y = floor((1.0 - asinh(tan(latRad)) / PI) / 2.0 * scale)
            .toInt().coerceIn(0, scale - 1)
        return TileId(ZOOM, x, y)
    }

    /** Tiles covering a radius around a WGS84 position. */
    fun aroundWgs84(
        latitudeDeg: Double,
        longitudeDeg: Double,
        radiusM: Double,
    ): List<TileId> {
        validate(latitudeDeg, longitudeDeg)
        val radius = radiusM.coerceIn(0.0, 10_000.0)
        if (radius == 0.0) return listOf(webMercatorTile(longitudeDeg, latitudeDeg))
        val dy = radius / METRES_PER_DEGREE
        val cosLat = cos(latitudeDeg * PI / 180.0).coerceAtLeast(1e-6)
        val dx = dy / cosLat
        return boundsWgs84(
            longitudeDeg - dx, latitudeDeg - dy,
            longitudeDeg + dx, latitudeDeg + dy,
        )
    }

    /** Tiles covering a radius around a GCJ-02 position (converted first). */
    fun aroundGcj02(
        latitudeDeg: Double,
        longitudeDeg: Double,
        radiusM: Double,
    ): List<TileId> {
        val wgs = GcjTransform.gcj02ToWgs84(longitudeDeg, latitudeDeg)
            ?: throw PlanningError.InvalidCoordinates("invalid GCJ-02 coordinate")
        return aroundWgs84(wgs.latitude, wgs.longitude, radiusM)
    }

    /** [west, south, east, north] in WGS84. */
    fun boundsWgs84(west: Double, south: Double, east: Double, north: Double): List<TileId> {
        val values = doubleArrayOf(west, south, east, north)
        if (values.any { !it.isFinite() }) {
            throw PlanningError.InvalidCoordinates("non-finite bound")
        }
        if (west >= east || south >= north) {
            throw PlanningError.InvalidCoordinates("inverted bounds")
        }
        if (west < -180.0 || east > 180.0 || south < -MAX_LATITUDE || north > MAX_LATITUDE) {
            throw PlanningError.InvalidCoordinates("bounds outside Web Mercator range")
        }
        val topLeft = webMercatorTile(west, north)
        val bottomRight = webMercatorTile(east, south)
        val width = bottomRight.x - topLeft.x + 1
        val height = bottomRight.y - topLeft.y + 1
        val count = width.toLong() * height.toLong()
        if (count > MAX_TILES) {
            throw PlanningError.AreaTooLarge("$count tiles exceeds $MAX_TILES")
        }
        val out = ArrayList<TileId>(count.toInt())
        for (y in topLeft.y..bottomRight.y) {
            for (x in topLeft.x..bottomRight.x) {
                out += TileId(ZOOM, x, y)
            }
        }
        return out
    }

    /**
     * Route corridor: samples the GCJ-02 polyline every <=400 m and unions
     * 1200 m windows, so sparse intercity routes still have intermediate tiles.
     */
    fun corridorGcj02(points: List<Pair<Double, Double>>): List<TileId> {
        if (points.size < 2) throw PlanningError.InvalidCoordinates("route needs 2 points")
        val wgs = points.map { (lon, lat) ->
            GcjTransform.gcj02ToWgs84(lon, lat)
                ?: throw PlanningError.InvalidCoordinates("invalid route point")
        }
        val tiles = HashSet<TileId>()
        var samples = 0
        for (i in 0 until wgs.size - 1) {
            val a = wgs[i]
            val b = wgs[i + 1]
            val distance = haversineM(a, b)
            val steps = maxOf(1, (distance / CORRIDOR_STEP_M).toInt() + 1)
            if (samples + steps > MAX_SAMPLES) {
                throw PlanningError.TooManySamples("route too long to pre-cache")
            }
            for (step in 0..steps) {
                val t = step.toDouble() / steps
                val lat = a.latitude + (b.latitude - a.latitude) * t
                val lon = a.longitude + (b.longitude - a.longitude) * t
                tiles += aroundWgs84(lat, lon, CORRIDOR_RADIUS_M)
                if (tiles.size > MAX_TILES) {
                    throw PlanningError.AreaTooLarge("corridor exceeds $MAX_TILES tiles")
                }
            }
            samples += steps
        }
        return tiles.sorted()
    }

    private fun validate(latitude: Double, longitude: Double) {
        if (!latitude.isFinite() || !longitude.isFinite()) {
            throw PlanningError.InvalidCoordinates("non-finite coordinate")
        }
        if (latitude < -MAX_LATITUDE || latitude > MAX_LATITUDE ||
            longitude < -180.0 || longitude > 180.0
        ) {
            throw PlanningError.InvalidCoordinates("coordinate outside Web Mercator range")
        }
    }

    private fun haversineM(a: GcjTransform.Wgs84, b: GcjTransform.Wgs84): Double {
        val r = 6_371_000.0
        val dLat = (b.latitude - a.latitude) * PI / 180.0
        val dLon = (b.longitude - a.longitude) * PI / 180.0
        val h = kotlin.math.sin(dLat / 2) * kotlin.math.sin(dLat / 2) +
            cos(a.latitude * PI / 180.0) * cos(b.latitude * PI / 180.0) *
            kotlin.math.sin(dLon / 2) * kotlin.math.sin(dLon / 2)
        return 2 * r * kotlin.math.asin(kotlin.math.sqrt(h))
    }
}
