package dev.glimpse.motogps

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * JVM-side structural checks against the upstream golden fixture. The real
 * end-to-end check (C++ encoder -> bytes -> fixture) runs on-device in
 * GoldenBytesSelfCheckTest; JVM tests cannot load the NDK library.
 */
class GoldenBytesJvmTest {
    private val golden by lazy {
        javaClass.getResourceAsStream("/ble-navigation-v1.golden.txt")!!
            .bufferedReader(Charsets.UTF_8).readLines()
            .filter { it.contains("=") && !it.trimStart().startsWith("#") }
            .associate {
                val (k, v) = it.split("=", limit = 2)
                k.trim() to v.trim()
            }
    }

    private fun hex(s: String): ByteArray =
        s.chunked(2).map { it.toInt(16).toByte() }.toByteArray()

    @Test
    fun frameHeaderMagicVersionAndTypeMatchGolden() {
        val frame = hex(golden.getValue("connection.frame"))
        assertEquals(0xB7, frame[0].toInt() and 0xFF) // magic
        assertEquals(1, frame[1].toInt())              // protocol version
        assertEquals(0x01, frame[2].toInt())           // ConnectionStatus
        assertEquals(0x03, frame[3].toInt())           // START|END
        assertEquals(1, (frame[4].toInt() and 0xFF) or (frame[5].toInt() shl 8))
        // message_length at offset 8..9, little-endian
        val length = (frame[8].toInt() and 0xFF) or ((frame[9].toInt() and 0xFF) shl 8)
        assertEquals(17, length)
        assertEquals(17 + 12, frame.size)
    }

    @Test
    fun connectionPayloadIs17Bytes() {
        assertEquals(17, hex(golden.getValue("connection.payload")).size)
    }

    @Test
    fun crc16CcittFalseMatchesGoldenTrailer() {
        // Independent JVM implementation used only as a test oracle.
        val frame = hex(golden.getValue("connection.frame"))
        val crc = crc16(frame.copyOfRange(0, frame.size - 2))
        val trailer = (frame[frame.size - 2].toInt() and 0xFF) or
            ((frame[frame.size - 1].toInt() and 0xFF) shl 8)
        assertEquals("golden CRC trailer must be 0x6954", 0x6954, trailer)
        assertEquals(0x6954, crc)
    }

    @Test
    fun commandFramesUseEightByteFragmentsAtMtu20() {
        val frames = golden.getValue("command.frames20").split(";")
        assertEquals(2, frames.size)
        val first = hex(frames[0])
        val second = hex(frames[1])
        // A 20-byte GATT value carries 8 payload bytes; the last frame is shorter
        // (10-byte header + 5-byte remainder + 2-byte CRC = 17).
        assertEquals(20, first.size)
        assertEquals(17, second.size)
        // fragment_offset of second frame = 8
        assertEquals(8, (second[6].toInt() and 0xFF) or ((second[7].toInt() and 0xFF) shl 8))
        // first frame carries START and ACK_REQUESTED and URGENT
        assertEquals(0x0D, first[3].toInt())
        assertEquals(0x0E, second[3].toInt())
    }

    @Test
    fun allFixtureKeysPresent() {
        listOf(
            "connection.payload", "connection.frame", "navigation.payload",
            "geometry.payload", "command.payload", "command.frames20",
        ).forEach { assertTrue("missing $it", golden.containsKey(it)) }
    }

    private fun crc16(bytes: ByteArray): Int {
        var crc = 0xFFFF
        for (byte in bytes) {
            crc = crc xor ((byte.toInt() and 0xFF) shl 8)
            repeat(8) {
                crc = if ((crc and 0x8000) != 0) {
                    ((crc shl 1) xor 0x1021) and 0xFFFF
                } else {
                    (crc shl 1) and 0xFFFF
                }
            }
        }
        return crc
    }
}
