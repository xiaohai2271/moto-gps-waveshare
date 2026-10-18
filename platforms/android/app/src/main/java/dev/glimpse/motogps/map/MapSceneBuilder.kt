package dev.glimpse.motogps.map

import dev.glimpse.motogps.NativeBridge
import org.json.JSONObject
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.roundToLong

/**
 * Builds a bounded MapScene (GCJ-02) around the rider from cached tiles.
 *
 * Tile geometry is already GCJ-02 and is never re-offset; the origin is the
 * rider's GCJ-02 position from the shared nav core. Protocol limits
 * (24 roads / 192 points, 16 buildings / 128 points) are enforced here so the
 * ESP32 never receives a scene it must truncate.
 */
object MapSceneBuilder {
    data class Scene(
        val revision: Long,
        val originLon: Double,
        val originLat: Double,
        val radiusM: Int,
        val roads: List<Road>,
        val buildings: List<Building>,
        val encoded: ByteArray,
    )

    data class Road(val roadClass: Int, val points: List<Pair<Double, Double>>)
    data class Building(val buildingClass: Int, val points: List<Pair<Double, Double>>)

    fun build(
        revision: Long,
        originLon: Double,
        originLat: Double,
        radiusM: Int,
        tileFiles: List<java.io.File>,
    ): Scene? {
        val roads = mutableListOf<Road>()
        val buildings = mutableListOf<Building>()
        val radius = radiusM.coerceIn(100, 1500)
        // Degrees per metre, inflated slightly for cos(latitude).
        val dLat = radius / 111_195.0
        val dLon = dLat / cos(originLat * Math.PI / 180.0).coerceAtLeast(0.2)

        tileFiles.forEach { file ->
            val text = runCatching { file.readText() }.getOrNull() ?: return@forEach
            val root = runCatching { JSONObject(text) }.getOrNull() ?: return@forEach
            val roadsArray = root.optJSONArray("roads") ?: return@forEach
            for (i in 0 until roadsArray.length()) {
                if (roads.size >= MAX_ROADS) break
                val o = roadsArray.getJSONObject(i)
                val points = readPoints(o) ?: continue
                val clipped = clip(points, originLon, originLat, dLon, dLat) ?: continue
                roads += Road(roadClass(o.optString("class")), simplify(clipped, MAX_ROAD_POINTS_PER_ROAD))
            }
            val buildingsArray = root.optJSONArray("buildings") ?: return@forEach
            for (i in 0 until buildingsArray.length()) {
                if (buildings.size >= MAX_BUILDINGS) break
                val o = buildingsArray.getJSONObject(i)
                val points = readPoints(o) ?: continue
                val clipped = clip(points, originLon, originLat, dLon, dLat) ?: continue
                buildings += Building(
                    buildingClass(o.optString("class")),
                    simplify(clipped, MAX_BUILDING_POINTS_PER_FOOTPRINT),
                )
            }
        }
        if (roads.isEmpty() && buildings.isEmpty()) return null

        // Global point budgets.
        var roadBudget = MAX_ROAD_TOTAL_POINTS
        val trimmedRoads = roads.take(MAX_ROADS).mapNotNull { road ->
            if (roadBudget < 2) return@mapNotNull null
            val take = minOf(road.points.size, roadBudget)
            roadBudget -= take
            if (take < 2) null else road.copy(points = road.points.take(take))
        }
        var buildingBudget = MAX_BUILDING_TOTAL_POINTS
        val trimmedBuildings = buildings.take(MAX_BUILDINGS).mapNotNull { building ->
            if (buildingBudget < 3) return@mapNotNull null
            val take = minOf(building.points.size, buildingBudget)
            buildingBudget -= take
            if (take < 3) null else building.copy(points = building.points.take(take))
        }

        val roadClasses = IntArray(trimmedRoads.size) { trimmedRoads[it].roadClass }
        val roadRanges = IntArray(trimmedRoads.size * 2)
        val roadPoints = ArrayList<Double>()
        trimmedRoads.forEachIndexed { index, road ->
            roadRanges[index * 2] = roadPoints.size / 2
            roadRanges[index * 2 + 1] = road.points.size
            road.points.forEach { (lon, lat) -> roadPoints += lon; roadPoints += lat }
        }
        val buildingClasses = IntArray(trimmedBuildings.size) { trimmedBuildings[it].buildingClass }
        val buildingRanges = IntArray(trimmedBuildings.size * 2)
        val buildingPoints = ArrayList<Double>()
        trimmedBuildings.forEachIndexed { index, building ->
            buildingRanges[index * 2] = buildingPoints.size / 2
            buildingRanges[index * 2 + 1] = building.points.size
            building.points.forEach { (lon, lat) -> buildingPoints += lon; buildingPoints += lat }
        }

        val meta = longArrayOf(
            revision,
            (originLat * 1e6).roundToLong(),
            (originLon * 1e6).roundToLong(),
            radius.toLong(),
        )
        val encoded = runCatching {
            NativeBridge.encodeMapScene(
                meta, roadClasses, roadRanges, roadPoints.toDoubleArray(),
                buildingClasses, buildingRanges, buildingPoints.toDoubleArray(),
            )
        }.getOrNull() ?: return null

        return Scene(
            revision = revision,
            originLon = originLon,
            originLat = originLat,
            radiusM = radius,
            roads = trimmedRoads,
            buildings = trimmedBuildings,
            encoded = encoded,
        )
    }

    private fun readPoints(o: JSONObject): List<Pair<Double, Double>>? {
        val array = o.optJSONArray("points_e6") ?: return null
        if (array.length() < 2) return null
        val out = ArrayList<Pair<Double, Double>>(array.length())
        for (i in 0 until array.length()) {
            val pair = array.getJSONArray(i)
            out += pair.getDouble(1) / 1e6 to pair.getDouble(0) / 1e6 // [lat,lon] -> lon,lat
        }
        return out
    }

    private fun clip(
        points: List<Pair<Double, Double>>,
        originLon: Double,
        originLat: Double,
        dLon: Double,
        dLat: Double,
    ): List<Pair<Double, Double>>? {
        val inside = points.filter {
            abs(it.first - originLon) <= dLon && abs(it.second - originLat) <= dLat
        }
        return if (inside.size >= 2) inside else null
    }

    private fun simplify(points: List<Pair<Double, Double>>, maxPoints: Int): List<Pair<Double, Double>> {
        if (points.size <= maxPoints) return points
        val step = points.size.toDouble() / maxPoints
        return List(maxPoints) { i -> points[(i * step).toInt().coerceAtMost(points.lastIndex)] }
    }

    private fun roadClass(name: String): Int = when (name) {
        "motorway" -> 0
        "primary" -> 1
        "secondary" -> 2
        "residential" -> 3
        "service" -> 4
        else -> 5
    }

    private fun buildingClass(name: String): Int = when (name) {
        "landmark" -> 1
        "parking" -> 2
        else -> 0
    }

    private const val MAX_ROADS = 24
    private const val MAX_ROAD_TOTAL_POINTS = 192
    private const val MAX_ROAD_POINTS_PER_ROAD = 64
    private const val MAX_BUILDINGS = 16
    private const val MAX_BUILDING_TOTAL_POINTS = 128
    private const val MAX_BUILDING_POINTS_PER_FOOTPRINT = 32
}
