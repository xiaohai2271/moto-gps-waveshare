package dev.glimpse.motogps

import android.Manifest
import android.app.Instrumentation
import android.content.pm.PackageManager
import android.os.Build
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import dev.glimpse.motogps.ble.Esp32BleCentral
import dev.glimpse.motogps.gateway.GatewayClient
import dev.glimpse.motogps.gateway.GatewayException
import dev.glimpse.motogps.nav.NavEngine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.math.roundToLong

/**
 * Live smoke test against the real gateway and, when a MAC is supplied, the
 * real round display. The route test needs no Android runtime permission.
 *
 * Route test:
 *   adb shell am instrument -w \
 *     -e class dev.glimpse.motogps.LiveNavigationSmokeTest#planRouteThroughLiveGateway \
 *     dev.glimpse.motogps.test/androidx.test.runner.AndroidJUnitRunner
 *
 * BLE test (requires BLUETOOTH_* granted; MAC intentionally not in the repo):
 *   adb shell am instrument -w -e motoMac AA:BB:CC:DD:EE:FF \
 *     -e class dev.glimpse.motogps.LiveNavigationSmokeTest#connectRoundDisplayAndSendScene \
 *     dev.glimpse.motogps.test/androidx.test.runner.AndroidJUnitRunner
 */
@RunWith(AndroidJUnit4::class)
class LiveNavigationSmokeTest {

    private val instrumentation: Instrumentation =
        InstrumentationRegistry.getInstrumentation()

    @Test
    fun planRouteThroughLiveGateway() = runBlocking {
        val client = GatewayClient()
        val health = justTry { client.health() }
        assumeTrue("gateway unreachable from this network", health != null)
        assertTrue("gateway not ready", health!!.contains("\"ready_for_live_navigation\":true"))

        val originLon = 117.0207
        val originLat = 36.6676
        val places = client.searchPlaces("大明湖", originLon to originLat)
        assertTrue("expected search results", places.isNotEmpty())
        val place = places.first()
        assertEquals("WGS84", place.coordinateSystem)

        NavEngine().use { engine ->
            engine.pushFix(originLon, originLat, 5.0, 0.0, 0.0, System.currentTimeMillis())
            val begin = engine.beginNavigation(place.longitude, place.latitude)
            val online = engine.setNetwork(2)
            val fix = engine.pushFix(originLon, originLat, 5.0, 0.0, 0.0, System.currentTimeMillis())
            val request = (begin.commands + online.commands + fix.commands)
                .firstOrNull { it.isRequestRoute }
            assertTrue("no route request from NavCore", request != null)

            val req = request!!
            val route = justTry {
                client.planRoute(
                    req.requestId,
                    req.originLon, req.originLat,
                    req.destinationLon, req.destinationLat,
                    req.reroute,
                )
            }
            assumeTrue("route planning failed (network/provider)", route != null)
            assertEquals("GCJ-02", route!!.coordinateSystem)
            assertTrue("route too short", route.polyline.size >= 2)

            val accepted = engine.acceptRoute(req.requestId, route, System.currentTimeMillis())
            assertEquals(3, accepted.snapshot.state) // Navigating
            assertTrue(accepted.snapshot.totalDistanceM > 0.0)
        }
    }

    @Test
    fun connectRoundDisplayAndSendScene() = runBlocking {
        val mac = InstrumentationRegistry.getArguments().getString("motoMac")
        assumeTrue("pass -e motoMac <MAC> to run the BLE smoke test", !mac.isNullOrBlank())
        assumeTrue("BLUETOOTH_CONNECT not granted", hasBluetoothConnect())
        assumeTrue(
            "BLUETOOTH_SCAN not granted",
            Build.VERSION.SDK_INT < 31 || hasPermission(Manifest.permission.BLUETOOTH_SCAN),
        )

        val app = instrumentation.targetContext.applicationContext as MotoGpsApplication
        val central = app.bleCentral
        instrumentation.runOnMainSync { central.connect(mac!!) }

        val state = withTimeout(25_000) {
            central.state.first { it is Esp32BleCentral.State.Connected }
        } as Esp32BleCentral.State.Connected
        assertTrue("frame size not negotiated", state.frameSize >= 20)
        assertTrue("no capabilities", state.capabilities != 0L)

        // Idle snapshot: no destination, no fix, no route view.
        val numeric = longArrayOf(
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        )
        assertTrue("snapshot rejected", central.sendNavigationSnapshot(numeric, "", ""))

        val token = NativeBridge.bleRouteToken("live-test-route")
        val points = doubleArrayOf(
            117.0207, 36.6676, 117.0210, 36.6678, 117.0213, 36.6680,
        )
        val meta = longArrayOf(
            2, token, 1, 0, 1, 0, 3,
            (36.6676 * 1e6).roundToLong(), (117.0207 * 1e6).roundToLong(),
        )
        assertTrue("geometry rejected", central.sendRouteGeometry(meta, points))

        instrumentation.runOnMainSync { central.disconnect() }
    }

    private fun hasPermission(permission: String): Boolean =
        instrumentation.targetContext.checkSelfPermission(permission) ==
            PackageManager.PERMISSION_GRANTED

    private fun hasBluetoothConnect(): Boolean =
        Build.VERSION.SDK_INT < 31 || hasPermission(Manifest.permission.BLUETOOTH_CONNECT)

    private suspend fun <T> justTry(block: suspend () -> T): T? = try {
        block()
    } catch (e: GatewayException) {
        null
    } catch (e: Exception) {
        null
    }
}
