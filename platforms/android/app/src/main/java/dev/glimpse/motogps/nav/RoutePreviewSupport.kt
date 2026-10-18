package dev.glimpse.motogps.nav

import dev.glimpse.motogps.gateway.GatewayRoute
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Route-preview candidate formatting, ported from
 * platforms/ios/App/Adapters/Navigation/RoutePreviewSupport.swift so both
 * platforms show identical titles, estimates and traffic summaries.
 */
data class RouteCandidate(val ordinal: Int, val route: GatewayRoute) {
    val id: String get() = "${route.routeId}#$ordinal"

    val title: String
        get() = when (ordinal) {
            0 -> "推荐路线"
            1 -> "备选路线一"
            else -> "备选路线二"
        }

    val distanceText: String
        get() = if (route.totalDistanceM >= 1_000) {
            String.format(java.util.Locale.US, "%.1f km", route.totalDistanceM / 1_000)
        } else {
            "${route.totalDistanceM.roundToInt()} m"
        }

    val durationText: String
        get() {
            val minutes = max(1, (route.totalDurationS / 60.0).roundToInt())
            return if (minutes >= 60) "${minutes / 60} 小时 ${minutes % 60} 分" else "$minutes 分钟"
        }

    val trafficSummary: String
        get() {
            if (route.traffic.isEmpty() || route.totalDistanceM <= 0) return "暂无分段路况"
            var slow = 0.0
            var congested = 0.0
            route.traffic.forEach { segment ->
                val length = max(0.0, segment.endOffsetM - segment.startOffsetM)
                when (segment.level) {
                    2 -> slow += length          // slow
                    3, 4 -> congested += length  // congested, severe
                }
            }
            val congestedShare = congested / route.totalDistanceM
            val affectedShare = (slow + congested) / route.totalDistanceM
            return when {
                congestedShare >= 0.12 -> "拥堵较多"
                affectedShare >= 0.18 -> "部分路段缓行"
                else -> "路况顺畅"
            }
        }

    companion object {
        fun fromRoutes(routes: List<GatewayRoute>): List<RouteCandidate> =
            routes.mapIndexed { index, route -> RouteCandidate(index, route) }
    }
}

/** Distance/duration formatting shared with the ride screen. */
object RideFormatting {
    fun distance(meters: Double): String =
        if (meters >= 1_000) {
            String.format(java.util.Locale.US, "%.1f km", meters / 1_000)
        } else {
            "${meters.roundToInt()} m"
        }

    fun duration(seconds: Int): String {
        val minutes = max(1, (seconds / 60.0).roundToInt())
        return if (minutes >= 60) "${minutes / 60} 小时 ${minutes % 60} 分" else "$minutes 分钟"
    }

    fun trafficTint(summary: String): String = when (summary) {
        "拥堵较多" -> "red"
        "部分路段缓行" -> "orange"
        "路况顺畅" -> "green"
        else -> "secondary"
    }
}
