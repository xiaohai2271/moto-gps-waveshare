package dev.glimpse.motogps.gateway

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class GatewayJsonTest {
    private val routeBody = """
        {
          "protocol_version": 1,
          "request_id": 42,
          "route": {
            "schema_version": 1,
            "route_id": "amap-drive-1",
            "provider": "amap",
            "coordinate_system": "GCJ-02",
            "generated_at_ms": 1700000000000,
            "total_distance_m": 12345.6,
            "total_duration_s": 1020,
            "speed_limit_kph": 50,
            "polyline": [
              {"longitude_deg": 117.0207, "latitude_deg": 36.6676},
              {"longitude_deg": 117.0215, "latitude_deg": 36.6680},
              {"longitude_deg": 117.0222, "latitude_deg": 36.6684}
            ],
            "maneuvers": [
              {"id": 1, "type": "right", "route_offset_m": 300, "road_name": "大明湖路",
               "instruction": "前方路口右转", "roundabout_exit": 0},
              {"id": 2, "type": "arrive", "route_offset_m": 1200, "road_name": "目的地",
               "instruction": "到达目的地", "roundabout_exit": 0}
            ],
            "traffic": [
              {"start_offset_m": 0, "end_offset_m": 800, "level": "slow"}
            ]
          }
        }
    """.trimIndent()

    @Test
    fun parsesSingleRouteAndEnums() {
        val route = GatewayJson.parseSingleRoute(routeBody, 42)
        assertEquals("amap-drive-1", route.routeId)
        assertEquals("GCJ-02", route.coordinateSystem)
        assertEquals(3, route.polyline.size)
        assertEquals(7, route.maneuvers[0].type)  // right
        assertEquals(12, route.maneuvers[1].type) // arrive
        assertEquals(2, route.traffic[0].level)   // slow
        assertEquals(12345.6, route.totalDistanceM, 0.001)
    }

    @Test
    fun rejectsRequestIdEchoMismatch() {
        var failed = false
        try {
            GatewayJson.parseSingleRoute(routeBody, 43)
        } catch (e: IllegalArgumentException) {
            failed = true
        }
        assertTrue(failed)
    }

    @Test
    fun parsesPlacesAsWgs84() {
        val body = """
            {
              "protocol_version": 1,
              "query": "大明湖",
              "places": [
                {"id": "p1", "name": "大明湖", "address": "大明湖路", "city": "济南",
                 "district": "历下区", "display_area": "济南 · 历下区",
                 "location": {"coordinate_system": "WGS84",
                              "longitude_deg": 117.0207, "latitude_deg": 36.6676},
                 "distance_m": 120}
              ]
            }
        """.trimIndent()
        val places = GatewayJson.parsePlaces(body)
        assertEquals(1, places.size)
        assertTrue(places[0].isWgs84)
        assertEquals(117.0207, places[0].longitude, 0.0000001)
    }

    @Test
    fun routeRequestUsesWgs84AndEchoesIds() {
        val json = JSONObject(
            GatewayJson.routeRequestJson(
                requestId = 7,
                originLon = 117.0, originLat = 36.6,
                destinationLon = 117.1, destinationLat = 36.7,
                isReroute = true, previousRouteId = "prev-1", destinationPoiId = null,
            ),
        )
        assertEquals(1, json.getInt("protocol_version"))
        assertEquals("driving", json.getString("route_mode"))
        assertEquals("WGS84", json.getJSONObject("origin").getString("coordinate_system"))
        assertEquals("prev-1", json.getString("previous_route_id"))
        assertTrue(json.getBoolean("is_reroute"))
        assertTrue(!json.has("destination_poi_id"))
    }

    @Test
    fun parsesErrorBody() {
        val error = GatewayJson.parseError(429, """{"error":{"code":"RATE_LIMITED","message":"slow down","retryable":true}}""")
        assertEquals("RATE_LIMITED", error.code)
        assertTrue(error.retryable)
        assertEquals(429, error.httpStatus)
    }

    @Test
    fun unknownEnumsBecomeUnknown() {
        assertEquals(0, GatewayJson.maneuverType("teleport"))
        assertEquals(0, GatewayJson.trafficLevel("jam"))
    }
}
