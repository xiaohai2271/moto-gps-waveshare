package dev.glimpse.motogps

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import dev.glimpse.motogps.gateway.GatewayRoute
import dev.glimpse.motogps.gateway.RouteManeuver
import dev.glimpse.motogps.gateway.RoutePoint
import dev.glimpse.motogps.nav.NavEngine
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Device-side check of the moto::nav::NavApp JNI bridge. Proves the shared core
 * issues a route request and transitions to Navigating after a RouteReady.
 */
@RunWith(AndroidJUnit4::class)
class NavEngineBridgeTest {

    private fun route(): GatewayRoute = GatewayRoute(
        routeId = "test-route-1",
        provider = "amap",
        coordinateSystem = "GCJ-02",
        generatedAtMs = 1_700_000_000_000,
        totalDistanceM = 1_200.0,
        totalDurationS = 300.0,
        speedLimitKph = 40,
        polyline = listOf(
            RoutePoint(117.0200, 36.6670),
            RoutePoint(117.0210, 36.6680),
            RoutePoint(117.0220, 36.6690),
        ),
        maneuvers = listOf(
            RouteManeuver(1, 1, 0.0, "起点", "直行", 0),
            RouteManeuver(2, 12, 1_200.0, "终点", "到达目的地", 0),
        ),
        traffic = emptyList(),
    )

    @Test
    fun beginsNavigationAndAcceptsRoute() {
        NavEngine().use { engine ->
            // NavCore only issues RequestRoute once a usable fix exists and the
            // network state is Online.
            engine.pushFix(117.0200, 36.6670, 5.0, 0.0, 0.0, System.currentTimeMillis())
            val start = engine.beginNavigation(117.0220, 36.6690)
            val online = engine.setNetwork(2)
            // A fix after BeginNavigation moves Acquiring -> Planning.
            val afterFix = engine.pushFix(
                117.0200, 36.6670, 5.0, 0.0, 0.0, System.currentTimeMillis(),
            )
            val commands = start.commands + online.commands + afterFix.commands
            assertTrue("expected a route request", commands.any { it.isRequestRoute })
            val request = commands.first { it.isRequestRoute }

            engine.pushFix(117.0200, 36.6670, 5.0, 0.0, 0.0, System.currentTimeMillis())
            val accepted = engine.acceptRoute(request.requestId, route(), System.currentTimeMillis())
            assertEquals(3, accepted.snapshot.state) // Navigating
            assertTrue(accepted.snapshot.hasDestination)
            assertEquals(1_200.0, accepted.snapshot.totalDistanceM, 0.5)
        }
    }

    @Test
    fun tokenMatchesUpstreamFnv1a() {
        // ble_protocol golden uses FNV-1a of "web-beijing-fixture" = 0xbf6210a6.
        assertEquals(0xbf6210a6L, NativeBridge.bleRouteToken("web-beijing-fixture"))
    }
}
