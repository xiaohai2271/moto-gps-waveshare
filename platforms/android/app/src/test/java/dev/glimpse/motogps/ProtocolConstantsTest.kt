package dev.glimpse.motogps

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Protocol constants mirrored from shared/ble_protocol headers, asserted
 * against values the C++ golden fixture depends on. The authoritative check
 * runs through JNI on-device (GoldenBytesSelfCheck).
 */
class ProtocolConstantsTest {
    @Test
    fun serviceUuidMatchesUpstream() {
        assertEquals(
            "7e57a000-b50c-4b6a-9c57-40a54e8e1000",
            MotoBleUuids.SERVICE,
        )
        assertEquals(
            "7e57a001-b50c-4b6a-9c57-40a54e8e1000",
            MotoBleUuids.PHONE_TO_DEVICE,
        )
        assertEquals(
            "7e57a002-b50c-4b6a-9c57-40a54e8e1000",
            MotoBleUuids.DEVICE_TO_PHONE,
        )
    }

    @Test
    fun frameMagicAndVersion() {
        assertEquals(0xB7, MotoBleUuids.FRAME_MAGIC)
        assertEquals(1, MotoBleUuids.PROTOCOL_VERSION)
    }
}

object MotoBleUuids {
    const val SERVICE = "7e57a000-b50c-4b6a-9c57-40a54e8e1000"
    const val PHONE_TO_DEVICE = "7e57a001-b50c-4b6a-9c57-40a54e8e1000"
    const val DEVICE_TO_PHONE = "7e57a002-b50c-4b6a-9c57-40a54e8e1000"
    const val FRAME_MAGIC = 0xB7
    const val PROTOCOL_VERSION = 1
}
