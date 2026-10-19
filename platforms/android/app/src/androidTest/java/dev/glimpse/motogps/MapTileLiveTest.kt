package dev.glimpse.motogps

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import dev.glimpse.motogps.gateway.GatewayClient
import dev.glimpse.motogps.gateway.GatewayJson
import dev.glimpse.motogps.map.GcjTransform
import dev.glimpse.motogps.map.MapTilePlanner
import dev.glimpse.motogps.map.MapTileStore
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.math.abs

/**
 * Device-side checks for the offline map path: GCJ-02 inverse matches the
 * upstream fixture pairs, and one real tile round-trips through the gateway
 * into the on-disk cache and manifest.
 */
@RunWith(AndroidJUnit4::class)
class MapTileLiveTest {

    @Test
    fun gcjInverseMatchesUpstreamPairs() {
        // Pairs from platforms/ios/AppTests/MapTilePlannerTests.swift.
        val pairs = listOf(
            Triple(117.1261301, 36.6704414, MapTilePlanner.TileId(15, 27044, 12791)),
            Triple(121.4782231, 31.2284577, MapTilePlanner.TileId(15, 27440, 13389)),
        )
        pairs.forEach { (gcjLon, gcjLat, expected) ->
            val wgs = GcjTransform.gcj02ToWgs84(gcjLon, gcjLat) ?: error("conversion failed")
            val tiles = MapTilePlanner.aroundGcj02(gcjLat, gcjLon, 0.0)
            assertEquals("tile for ($gcjLon,$gcjLat)", listOf(expected), tiles)
            // Round trip through the shared C++ forward transform.
            val forward = NativeBridge.wgs84ToGcj02(wgs.longitude, wgs.latitude)
            assertTrue("inverse drift lon", abs(forward[0] - gcjLon) < 1e-6)
            assertTrue("inverse drift lat", abs(forward[1] - gcjLat) < 1e-6)
        }
    }

    @Test
    fun citySearchFindsHangzhou() = runBlocking {
        val client = GatewayClient()
        val result = runCatching { client.searchCities("杭州") }
        assertTrue("city search failed: ${result.exceptionOrNull()}", result.isSuccess)
        val cities = result.getOrThrow()
        assertTrue("expected at least one city, got $cities", cities.isNotEmpty())
        assertTrue(
            "expected Hangzhou in ${cities.map { it.name }}",
            cities.any { it.name.contains("杭州") },
        )
        // The whole-prefecture range is far too big for a phone download, which
        // is why the UI asks for a district instead of silently running for hours.
        val tiles = MapTilePlanner.boundsWgs84(cities.first().west, cities.first().south, cities.first().east, cities.first().north)
        assertTrue("expected a large range for a prefecture, got ${tiles.size}", tiles.size > 4_000)
    }

    @Test
    fun downloadsOneTileIntoTheCache() = runBlocking {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val client = GatewayClient()
        val tile = MapTilePlanner.TileId(15, 27440, 13389)

        val body = runCatching { client.fetchTile(tile) }.getOrNull()
        assumeTrue("gateway unreachable", body != null)
        val features = GatewayJson.validateTile(body!!, tile.z, tile.x, tile.y)
        assertTrue("tile should carry features", features >= 0)

        val store = MapTileStore(context, client)
        val pack = store.download("测试·单瓦片", "instrumented", listOf(tile))
        assertEquals(1, pack.tiles.size)
        assertTrue(store.tileFile(tile).exists())
        assertTrue(store.packs().any { it.id == pack.id })
        assertTrue(store.deletePack(pack.id))
        assertTrue(!store.packs().any { it.id == pack.id })
    }
}
