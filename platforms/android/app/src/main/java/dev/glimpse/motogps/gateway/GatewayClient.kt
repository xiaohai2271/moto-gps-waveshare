package dev.glimpse.motogps.gateway

import dev.glimpse.motogps.GatewayConfig
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URI
import java.net.URLEncoder

/**
 * HTTP client for the self-hosted Glimpse gateway.
 *
 * Contract points that matter for correctness:
 *  - requests carry WGS84 coordinates; route bundles come back in GCJ-02;
 *  - request_id must be monotonically increasing and the response must echo it;
 *  - only https (or a loopback host) is accepted, matching iOS.
 */
class GatewayClient(baseUrl: String = GatewayConfig.DEFAULT_BASE_URL) {

    private val base: String = when (val result = GatewayConfig.validate(baseUrl)) {
        is GatewayConfig.Validation.Ok -> result.baseUrl
        is GatewayConfig.Validation.Invalid -> throw IllegalArgumentException("invalid gateway: ${result.reason}")
    }

    suspend fun health(): String = request("GET", "healthz")

    suspend fun searchPlaces(keywords: String, origin: Pair<Double, Double>? = null): List<Place> {
        val trimmed = keywords.trim()
        if (trimmed.length < 2) return emptyList()
        var path = "v1/places?keywords=" + URLEncoder.encode(trimmed, "UTF-8")
        if (origin != null) {
            path += "&longitude_deg=" + String.format(java.util.Locale.US, "%.7f", origin.first)
            path += "&latitude_deg=" + String.format(java.util.Locale.US, "%.7f", origin.second)
        }
        return GatewayJson.parsePlaces(request("GET", path))
    }

    suspend fun routeOptions(
        requestId: Int,
        originLon: Double,
        originLat: Double,
        destinationLon: Double,
        destinationLat: Double,
        isReroute: Boolean,
        previousRouteId: String? = null,
        destinationPoiId: String? = null,
    ): List<GatewayRoute> {
        val body = GatewayJson.routeRequestJson(
            requestId, originLon, originLat, destinationLon, destinationLat,
            isReroute, previousRouteId, destinationPoiId,
        )
        return try {
            val response = request("POST", "v1/route-options", body)
            GatewayJson.parseRouteOptions(response, requestId)
        } catch (e: GatewayException) {
            if (e.error.httpStatus in setOf(404, 405, 501)) {
                // Older gateway without candidate routes: fall back to /v1/routes.
                listOf(planRoute(requestId, originLon, originLat, destinationLon, destinationLat, isReroute, previousRouteId))
            } else {
                throw e
            }
        }
    }

    suspend fun planRoute(
        requestId: Int,
        originLon: Double,
        originLat: Double,
        destinationLon: Double,
        destinationLat: Double,
        isReroute: Boolean,
        previousRouteId: String? = null,
        destinationPoiId: String? = null,
    ): GatewayRoute {
        val body = GatewayJson.routeRequestJson(
            requestId, originLon, originLat, destinationLon, destinationLat,
            isReroute, previousRouteId, destinationPoiId,
        )
        return GatewayJson.parseSingleRoute(request("POST", "v1/routes", body), requestId)
    }

    private suspend fun request(method: String, path: String, body: String? = null): String =
        withContext(Dispatchers.IO) {
            val endpoint = URI(GatewayConfig.resolve(base, path)).toURL()
            if (endpoint.protocol != "https" && endpoint.host !in setOf("127.0.0.1", "localhost")) {
                throw IllegalArgumentException("https or loopback required")
            }
            val connection = (endpoint.openConnection() as HttpURLConnection).apply {
                requestMethod = method
                connectTimeout = 15_000
                readTimeout = 15_000
                setRequestProperty("Accept", "application/json")
                if (body != null) {
                    doOutput = true
                    setRequestProperty("Content-Type", "application/json")
                    val bytes = body.toByteArray(Charsets.UTF_8)
                    setFixedLengthStreamingMode(bytes.size)
                    outputStream.use { it.write(bytes) }
                }
            }
            try {
                val status = connection.responseCode
                val stream = if (status in 200..299) connection.inputStream else connection.errorStream
                val text = stream?.use { input ->
                    val buffer = ByteArrayOutputStream()
                    input.copyTo(buffer)
                    buffer.toString("UTF-8")
                }.orEmpty()
                if (status !in 200..299) {
                    throw GatewayException(GatewayJson.parseError(status, text))
                }
                text
            } finally {
                connection.disconnect()
            }
        }
}
