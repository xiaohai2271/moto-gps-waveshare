package dev.glimpse.motogps.nav

import dev.glimpse.motogps.NativeBridge
import dev.glimpse.motogps.gateway.GatewayRoute
import org.json.JSONObject

/** Command emitted by the shared NavCore and executed by a platform adapter. */
data class EngineCommand(
    val type: Int,
    val requestId: Int,
    val originLon: Double,
    val originLat: Double,
    val destinationLon: Double,
    val destinationLat: Double,
    val reroute: Boolean,
    val routeId: String,
) {
    val isRequestRoute: Boolean get() = type == TYPE_REQUEST_ROUTE

    companion object {
        const val TYPE_REQUEST_ROUTE = 0
        const val TYPE_REQUEST_TRAFFIC = 1
    }
}

/** Flat view of moto::nav::NavSnapshot for UI and BLE encoding. */
data class EngineSnapshot(
    val state: Int,
    val network: Int,
    val displayPage: Int,
    val hasDestination: Boolean,
    val hasUsableFix: Boolean,
    val gnssStale: Boolean,
    val offRoute: Boolean,
    val routeRequestInFlight: Boolean,
    val trafficRequestInFlight: Boolean,
    val positionLon: Double,
    val positionLat: Double,
    val destinationLon: Double,
    val destinationLat: Double,
    val speedMps: Double,
    val headingDeg: Double,
    val accuracyM: Double,
    val crossTrackM: Double,
    val speedLimitKph: Int,
    val hasRouteView: Boolean,
    val routeViewOriginLon: Double,
    val routeViewOriginLat: Double,
    val routeViewPoints: List<Pair<Double, Double>>,
    val routeId: String,
    val routeProgressM: Double,
    val totalDistanceM: Double,
    val remainingDistanceM: Double,
    val remainingDurationS: Int,
    val hasNextManeuver: Boolean,
    val maneuverType: Int,
    val maneuverId: Int,
    val distanceToManeuverM: Double,
    val roadName: String,
    val instruction: String,
    val roundaboutExit: Int,
    val trafficAhead: Int,
    val nowMs: Long,
    val routeGeneration: Int,
    val mapSceneRevision: Int,
)

data class EngineResult(val commands: List<EngineCommand>, val snapshot: EngineSnapshot)

/**
 * Kotlin facade over moto::nav::NavApp. The C++ core owns navigation state;
 * this class only converts JSON and forwards events.
 */
class NavEngine : AutoCloseable {
    private var handle: Long = NativeBridge.navCreate()

    fun beginNavigation(destinationLon: Double, destinationLat: Double): EngineResult =
        parse(NativeBridge.navBeginNavigation(handle, destinationLon, destinationLat))

    fun cancel(): EngineResult = parse(NativeBridge.navCancelNavigation(handle))

    fun reset(): EngineResult = parse(NativeBridge.navReset(handle))

    fun setNetwork(network: Int): EngineResult = parse(NativeBridge.navSetNetwork(handle, network))

    fun pushFix(
        lon: Double,
        lat: Double,
        accuracyM: Double,
        speedMps: Double,
        headingDeg: Double,
        timestampMs: Long,
    ): EngineResult = parse(
        NativeBridge.navPushFix(handle, lon, lat, accuracyM, speedMps, headingDeg, timestampMs),
    )

    fun tick(nowMs: Long): EngineResult = parse(NativeBridge.navTick(handle, nowMs))

    fun snapshot(): EngineSnapshot = snapshotFrom(JSONObject(NativeBridge.navSnapshot(handle)))

    fun acceptRoute(
        requestId: Int,
        route: GatewayRoute,
        receivedAtMs: Long,
    ): EngineResult {
        val polyline = DoubleArray(route.polyline.size * 2)
        route.polyline.forEachIndexed { i, (lon, lat) ->
            polyline[i * 2] = lon
            polyline[i * 2 + 1] = lat
        }
        val numeric = IntArray(route.maneuvers.size * 5)
        val roads = arrayOfNulls<String>(route.maneuvers.size)
        val instructions = arrayOfNulls<String>(route.maneuvers.size)
        route.maneuvers.forEachIndexed { i, maneuver ->
            numeric[i * 5] = maneuver.id.toInt()
            numeric[i * 5 + 1] = maneuver.type
            numeric[i * 5 + 2] = maneuver.routeOffsetM.toInt()
            numeric[i * 5 + 3] = maneuver.roundaboutExit
            numeric[i * 5 + 4] = 0
            roads[i] = maneuver.roadName
            instructions[i] = maneuver.instruction
        }
        val traffic = DoubleArray(route.traffic.size * 3)
        route.traffic.forEachIndexed { i, segment ->
            traffic[i * 3] = segment.startOffsetM
            traffic[i * 3 + 1] = segment.endOffsetM
            traffic[i * 3 + 2] = segment.level.toDouble()
        }
        return parse(
            NativeBridge.navAcceptRoute(
                handle, requestId, route.routeId, route.totalDistanceM,
                route.totalDurationS, route.speedLimitKph, route.generatedAtMs,
                receivedAtMs, polyline, numeric,
                roads.requireNoNulls(), instructions.requireNoNulls(), traffic,
            ),
        )
    }

    fun rejectRoute(requestId: Int, retryable: Boolean, receivedAtMs: Long): EngineResult =
        parse(NativeBridge.navRejectRoute(handle, requestId, retryable, receivedAtMs))

    override fun close() {
        if (handle != 0L) {
            NativeBridge.navDestroy(handle)
            handle = 0L
        }
    }

    private fun parse(json: String): EngineResult {
        val root = JSONObject(json)
        val commands = root.getJSONArray("commands")
        val list = ArrayList<EngineCommand>(commands.length())
        for (i in 0 until commands.length()) {
            val c = commands.getJSONObject(i)
            list += EngineCommand(
                type = c.getInt("type"),
                requestId = c.getInt("requestId"),
                originLon = c.getDouble("originLon"),
                originLat = c.getDouble("originLat"),
                destinationLon = c.getDouble("destinationLon"),
                destinationLat = c.getDouble("destinationLat"),
                reroute = c.getBoolean("reroute"),
                routeId = c.optString("routeId", ""),
            )
        }
        return EngineResult(list, snapshotFrom(root.getJSONObject("snapshot")))
    }

    private fun snapshotFrom(o: JSONObject): EngineSnapshot {
        val pointsArray = o.optJSONArray("routeViewPoints")
        val points = ArrayList<Pair<Double, Double>>()
        if (pointsArray != null) {
            for (i in 0 until pointsArray.length()) {
                val pair = pointsArray.getJSONArray(i)
                points += pair.getDouble(0) to pair.getDouble(1)
            }
        }
        return EngineSnapshot(
            state = o.getInt("state"),
            network = o.getInt("network"),
            displayPage = o.getInt("displayPage"),
            hasDestination = o.getBoolean("hasDestination"),
            hasUsableFix = o.getBoolean("hasUsableFix"),
            gnssStale = o.getBoolean("gnssStale"),
            offRoute = o.getBoolean("offRoute"),
            routeRequestInFlight = o.getBoolean("routeRequestInFlight"),
            trafficRequestInFlight = o.getBoolean("trafficRequestInFlight"),
            positionLon = o.getDouble("positionLon"),
            positionLat = o.getDouble("positionLat"),
            destinationLon = o.getDouble("destinationLon"),
            destinationLat = o.getDouble("destinationLat"),
            speedMps = o.getDouble("speedMps"),
            headingDeg = o.getDouble("headingDeg"),
            accuracyM = o.getDouble("accuracyM"),
            crossTrackM = o.getDouble("crossTrackM"),
            speedLimitKph = o.getInt("speedLimitKph"),
            hasRouteView = o.getBoolean("hasRouteView"),
            routeViewOriginLon = o.getDouble("routeViewOriginLon"),
            routeViewOriginLat = o.getDouble("routeViewOriginLat"),
            routeViewPoints = points,
            routeId = o.optString("routeId", ""),
            routeProgressM = o.getDouble("routeProgressM"),
            totalDistanceM = o.getDouble("totalDistanceM"),
            remainingDistanceM = o.getDouble("remainingDistanceM"),
            remainingDurationS = o.getInt("remainingDurationS"),
            hasNextManeuver = o.getBoolean("hasNextManeuver"),
            maneuverType = o.getInt("maneuverType"),
            maneuverId = o.getInt("maneuverId"),
            distanceToManeuverM = o.getDouble("distanceToManeuverM"),
            roadName = o.optString("roadName", ""),
            instruction = o.optString("instruction", ""),
            roundaboutExit = o.getInt("roundaboutExit"),
            trafficAhead = o.getInt("trafficAhead"),
            nowMs = o.getLong("nowMs"),
            routeGeneration = o.getInt("routeGeneration"),
            mapSceneRevision = o.getInt("mapSceneRevision"),
        )
    }
}
