package dev.glimpse.motogps.gateway

import org.json.JSONArray
import org.json.JSONObject

data class Place(
    val id: String,
    val name: String,
    val address: String,
    val city: String,
    val district: String,
    val displayArea: String,
    val longitude: Double,
    val latitude: Double,
    val coordinateSystem: String,
    val distanceM: Double?,
) {
    val isWgs84: Boolean get() = coordinateSystem == "WGS84"
}

data class RoutePoint(val longitude: Double, val latitude: Double)

data class RouteManeuver(
    val id: Long,
    val type: Int,
    val routeOffsetM: Double,
    val roadName: String,
    val instruction: String,
    val roundaboutExit: Int,
)

data class RouteTrafficSegment(
    val startOffsetM: Double,
    val endOffsetM: Double,
    val level: Int,
)

data class GatewayRoute(
    val routeId: String,
    val provider: String,
    val coordinateSystem: String,
    val generatedAtMs: Long,
    val totalDistanceM: Double,
    val totalDurationS: Double,
    val speedLimitKph: Int,
    val polyline: List<RoutePoint>,
    val maneuvers: List<RouteManeuver>,
    val traffic: List<RouteTrafficSegment>,
)

data class GatewayError(
    val code: String,
    val message: String,
    val retryable: Boolean,
    val httpStatus: Int,
)

/** Downloadable city/ district bounds in WGS84: [west, south, east, north]. */
data class CityArea(
    val id: String,
    val name: String,
    val detail: String,
    val west: Double,
    val south: Double,
    val east: Double,
    val north: Double,
) {
    fun bounds(): DoubleArray = doubleArrayOf(west, south, east, north)
}

class GatewayException(val error: GatewayError) :
    Exception("${error.httpStatus} ${error.code}: ${error.message}")

/** Parses the gateway's JSON payloads. Pure functions, unit-testable on the JVM. */
object GatewayJson {
    /**
     * Some endpoints (notably `GET /v1/map/cities`) omit `protocol_version`
     * entirely, which the iOS client tolerates. Only reject an explicitly
     * unsupported version so a missing field never breaks a working endpoint.
     */
    private fun checkProtocol(root: JSONObject) {
        if (root.has("protocol_version") && root.optInt("protocol_version", 1) != 1) {
            throw IllegalArgumentException("protocol version")
        }
    }

    fun parsePlaces(body: String): List<Place> {
        val root = JSONObject(body)
        checkProtocol(root)
        val array = root.getJSONArray("places")
        return List(array.length()) { i ->
            val o = array.getJSONObject(i)
            val location = o.getJSONObject("location")
            Place(
                id = o.getString("id"),
                name = o.getString("name"),
                address = o.optString("address", ""),
                city = o.optString("city", ""),
                district = o.optString("district", ""),
                displayArea = o.optString("display_area", ""),
                longitude = location.getDouble("longitude_deg"),
                latitude = location.getDouble("latitude_deg"),
                coordinateSystem = location.getString("coordinate_system"),
                distanceM = if (o.has("distance_m")) o.getDouble("distance_m") else null,
            )
        }
    }

    fun parseCities(body: String): List<CityArea> {
        val root = JSONObject(body)
        checkProtocol(root)
        val array = root.getJSONArray("cities")
        return List(array.length()) { i ->
            val o = array.getJSONObject(i)
            val bounds = o.getJSONArray("bounds_wgs84")
            require(bounds.length() == 4) { "bounds_wgs84 must have 4 values" }
            CityArea(
                id = o.getString("id"),
                name = o.getString("name"),
                detail = o.optString("detail", ""),
                west = bounds.getDouble(0),
                south = bounds.getDouble(1),
                east = bounds.getDouble(2),
                north = bounds.getDouble(3),
            )
        }
    }

    /** Basic tile-document validation; geometry stays GCJ-02 and is not re-offset. */
    fun validateTile(body: String, z: Int, x: Int, y: Int): Int {
        val root = JSONObject(body)
        require(root.getInt("schema_version") == 1) { "unexpected tile schema" }
        require(root.getString("coordinate_system") == "GCJ-02") { "tile must be GCJ-02" }
        val tile = root.getJSONObject("tile")
        require(tile.getInt("z") == z && tile.getInt("x") == x && tile.getInt("y") == y) {
            "tile identity mismatch"
        }
        val roads = root.optJSONArray("roads")?.length() ?: 0
        val buildings = root.optJSONArray("buildings")?.length() ?: 0
        return roads + buildings
    }

    fun parseRouteBundle(o: JSONObject): GatewayRoute {
        val route = o
        val polyline = route.getJSONArray("polyline")
        val points = List(polyline.length()) { i ->
            val p = polyline.getJSONObject(i)
            RoutePoint(p.getDouble("longitude_deg"), p.getDouble("latitude_deg"))
        }
        val maneuversArray = route.optJSONArray("maneuvers") ?: JSONArray()
        val maneuvers = List(maneuversArray.length()) { i ->
            val m = maneuversArray.getJSONObject(i)
            RouteManeuver(
                id = m.getLong("id"),
                type = maneuverType(m.getString("type")),
                routeOffsetM = m.getDouble("route_offset_m"),
                roadName = m.optString("road_name", ""),
                instruction = m.optString("instruction", ""),
                roundaboutExit = m.optInt("roundabout_exit", 0),
            )
        }
        val trafficArray = route.optJSONArray("traffic") ?: JSONArray()
        val traffic = List(trafficArray.length()) { i ->
            val t = trafficArray.getJSONObject(i)
            RouteTrafficSegment(
                startOffsetM = t.getDouble("start_offset_m"),
                endOffsetM = t.getDouble("end_offset_m"),
                level = trafficLevel(t.getString("level")),
            )
        }
        return GatewayRoute(
            routeId = route.getString("route_id"),
            provider = route.optString("provider", ""),
            coordinateSystem = route.getString("coordinate_system"),
            generatedAtMs = route.getLong("generated_at_ms"),
            totalDistanceM = route.getDouble("total_distance_m"),
            totalDurationS = route.getDouble("total_duration_s"),
            speedLimitKph = route.optInt("speed_limit_kph", 0),
            polyline = points,
            maneuvers = maneuvers,
            traffic = traffic,
        )
    }

    fun parseRouteOptions(body: String, expectedRequestId: Int): List<GatewayRoute> {
        val root = JSONObject(body)
        checkProtocol(root)
        if (root.getInt("request_id") != expectedRequestId) {
            throw IllegalArgumentException("request_id echo mismatch")
        }
        val routes = root.getJSONArray("routes")
        return List(routes.length()) { parseRouteBundle(routes.getJSONObject(it)) }
            .filter { it.polyline.size >= 2 && it.totalDistanceM > 0 && it.totalDurationS > 0 }
            .distinctBy { it.routeId }
            .take(3)
    }

    fun parseSingleRoute(body: String, expectedRequestId: Int): GatewayRoute {
        val root = JSONObject(body)
        checkProtocol(root)
        if (root.getInt("request_id") != expectedRequestId) {
            throw IllegalArgumentException("request_id echo mismatch")
        }
        return parseRouteBundle(root.getJSONObject("route"))
    }

    fun parseError(status: Int, body: String): GatewayError {
        val fallback = GatewayError("HTTP_$status", "unexpected response", status >= 500, status)
        return runCatching {
            val json = JSONObject(body).getJSONObject("error")
            GatewayError(
                code = json.getString("code"),
                message = json.optString("message", ""),
                retryable = json.optBoolean("retryable", false),
                httpStatus = status,
            )
        }.getOrDefault(fallback)
    }

    fun maneuverType(name: String): Int = when (name) {
        "continue" -> 1
        "slight_left" -> 2
        "left" -> 3
        "sharp_left" -> 4
        "u_turn_left" -> 5
        "slight_right" -> 6
        "right" -> 7
        "sharp_right" -> 8
        "u_turn_right" -> 9
        "roundabout" -> 10
        "exit" -> 11
        "arrive" -> 12
        else -> 0
    }

    fun trafficLevel(name: String): Int = when (name) {
        "free_flow" -> 1
        "slow" -> 2
        "congested" -> 3
        "severe" -> 4
        else -> 0
    }

    /** Request body for POST /v1/routes and /v1/route-options. */
    fun routeRequestJson(
        requestId: Int,
        originLon: Double,
        originLat: Double,
        destinationLon: Double,
        destinationLat: Double,
        isReroute: Boolean,
        previousRouteId: String?,
        destinationPoiId: String?,
    ): String {
        val root = JSONObject()
        root.put("protocol_version", 1)
        root.put("request_id", requestId)
        root.put("route_mode", "driving")
        root.put("origin", coordinate(originLon, originLat))
        root.put("destination", coordinate(destinationLon, destinationLat))
        root.put("is_reroute", isReroute)
        previousRouteId?.let { root.put("previous_route_id", it) }
        destinationPoiId?.let { root.put("destination_poi_id", it) }
        return root.toString()
    }

    private fun coordinate(lon: Double, lat: Double): JSONObject = JSONObject()
        .put("coordinate_system", "WGS84")
        .put("longitude_deg", lon)
        .put("latitude_deg", lat)
}

