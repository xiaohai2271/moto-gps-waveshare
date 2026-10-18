package dev.glimpse.motogps

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * End-to-end golden-byte check: the shared C++ encoder (via JNI) must
 * reproduce shared/protocol/fixtures/ble-navigation-v1.golden.txt byte for
 * byte. Runs only on a device or emulator (`connectedDebugAndroidTest`).
 */
@RunWith(AndroidJUnit4::class)
class GoldenBytesSelfCheckTest {

    private fun upstreamFixture(): Map<String, String> {
        val context = InstrumentationRegistry.getInstrumentation().context
        return context.assets.open("ble-navigation-v1.golden.txt")
            .bufferedReader(Charsets.UTF_8).readLines()
            .filter { it.contains("=") && !it.trimStart().startsWith("#") }
            .associate {
                val (k, v) = it.split("=", limit = 2)
                k.trim() to v.trim()
            }
    }

    @Test
    fun jniEncodingMatchesEveryGoldenVector() {
        val upstream = upstreamFixture()
        val native = NativeBridge.goldenVectors().lines()
            .filter { it.contains("=") }
            .associate {
                val (k, v) = it.split("=", limit = 2)
                k.trim() to v.trim()
            }
        for (key in upstream.keys) {
            assertEquals("mismatch for $key", upstream[key], native[key])
        }
        assertEquals(upstream.size, native.size)
    }

    @Test
    fun coordinatesMatchKnownGcj02Offset() {
        // Beijing: WGS84 -> GCJ-02 must shift by a few hundred metres.
        val result = NativeBridge.wgs84ToGcj02(116.397428, 39.90923)
        assertEquals(1.0, result[2], 0.0)
        assertTrue("longitude must increase", result[0] > 116.397428)
        assertTrue("latitude must increase", result[1] > 39.90923)
    }
}
