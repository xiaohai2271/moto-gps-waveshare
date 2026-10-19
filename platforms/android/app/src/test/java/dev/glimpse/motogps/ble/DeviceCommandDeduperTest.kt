package dev.glimpse.motogps.ble

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The round display retransmits unacknowledged commands, so the same
 * (session_id, command_id) can arrive several times. Replaying a page change
 * makes the display snap back; replaying play/pause toggles twice.
 */
class DeviceCommandDeduperTest {

    @Test
    fun firstSightingIsAcceptedRetransmissionsAreRejected() {
        val deduper = DeviceCommandDeduper()
        assertTrue(deduper.accept(7L))
        assertFalse(deduper.accept(7L))
        assertFalse(deduper.accept(7L))
        assertEquals(1, deduper.size)
    }

    @Test
    fun distinctCommandsAreAllAccepted() {
        val deduper = DeviceCommandDeduper()
        assertTrue(deduper.accept(1L))
        assertTrue(deduper.accept(2L))
        assertTrue(deduper.accept(3L))
        assertEquals(3, deduper.size)
    }

    @Test
    fun oldIdsAreForgottenAfterCapacity() {
        val deduper = DeviceCommandDeduper(capacity = 2)
        assertTrue(deduper.accept(1L))
        assertTrue(deduper.accept(2L))
        assertTrue(deduper.accept(3L))
        // 1 was evicted, so it looks new again — bounded memory by design.
        assertTrue(deduper.accept(1L))
    }

    @Test
    fun resetStartsAFreshSession() {
        val deduper = DeviceCommandDeduper()
        assertTrue(deduper.accept(42L))
        deduper.reset()
        assertTrue(deduper.accept(42L))
        assertEquals(1, deduper.size)
    }
}
