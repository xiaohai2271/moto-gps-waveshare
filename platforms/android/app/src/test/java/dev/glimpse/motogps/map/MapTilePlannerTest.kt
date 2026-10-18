package dev.glimpse.motogps.map

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class MapTilePlannerTest {
    @Test
    fun webMercatorMatchesUpstreamFixtures() {
        // WGS84 positions from platforms/ios/AppTests/MapTilePlannerTests.swift.
        assertEquals(
            MapTilePlanner.TileId(15, 27044, 12791),
            MapTilePlanner.webMercatorTile(117.12, 36.67),
        )
        assertEquals(
            MapTilePlanner.TileId(15, 27440, 13389),
            MapTilePlanner.webMercatorTile(121.4737, 31.2304),
        )
    }

    @Test
    fun clampsAtMercatorLimits() {
        val north = MapTilePlanner.webMercatorTile(0.0, 89.0)
        val south = MapTilePlanner.webMercatorTile(0.0, -89.0)
        assertEquals(0, north.y)
        assertEquals(32767, south.y)
    }

    @Test
    fun boundsRejectInvertedAndOutOfRangeInput() {
        val invalid = listOf(
            doubleArrayOf(118.0, 36.0, 117.0, 37.0),
            doubleArrayOf(117.0, 37.0, 118.0, 36.0),
            doubleArrayOf(117.0, 36.0, 117.0, 37.0),
            doubleArrayOf(-181.0, 36.0, 118.0, 37.0),
            doubleArrayOf(117.0, -86.0, 118.0, 37.0),
        )
        invalid.forEach { b ->
            var threw = false
            try {
                MapTilePlanner.boundsWgs84(b[0], b[1], b[2], b[3])
            } catch (e: MapTilePlanner.PlanningError.InvalidCoordinates) {
                threw = true
            }
            assertTrue("expected invalid for ${b.toList()}", threw)
        }
    }

    @Test
    fun boundsRejectHugeAreasBeforeAllocating() {
        var threw = false
        try {
            MapTilePlanner.boundsWgs84(-180.0, -80.0, 180.0, 80.0)
        } catch (e: MapTilePlanner.PlanningError.AreaTooLarge) {
            threw = true
        }
        assertTrue(threw)
    }

    @Test
    fun smallBoundsProduceContiguousTiles() {
        val tiles = MapTilePlanner.boundsWgs84(117.119, 36.669, 117.121, 36.671)
        assertTrue(tiles.isNotEmpty())
        assertTrue(tiles.size <= 4)
        tiles.forEach { assertEquals(15, it.z) }
    }

    @Test
    fun radiusAtZeroIsSingleTile() {
        val tiles = MapTilePlanner.aroundWgs84(36.67, 117.12, 0.0)
        assertEquals(listOf(MapTilePlanner.TileId(15, 27044, 12791)), tiles)
    }

    @Test
    fun radiusGrowsTileCountMonotonically() {
        val small = MapTilePlanner.aroundWgs84(36.67, 117.12, 200.0).size
        val large = MapTilePlanner.aroundWgs84(36.67, 117.12, 2_000.0).size
        assertTrue("$small !< $large", small < large)
    }
}
