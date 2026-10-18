package dev.glimpse.motogps.ble

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class OutboundQueueTest {
    private fun frames(n: Int): List<ByteArray> = List(n) { byteArrayOf(it.toByte()) }

    @Test
    fun admitsWholeMessageAtomically() {
        val queue = OutboundQueue(capacity = 10)
        assertTrue(queue.enqueueMessage(frames(4), "nav"))
        assertTrue(queue.enqueueMessage(frames(6), "nav"))
        assertEquals(10, queue.size)
        assertFalse(queue.overflowed)
    }

    @Test
    fun overflowClearsQueueAndRequestsGeometryResend() {
        val queue = OutboundQueue(capacity = 10)
        queue.enqueueMessage(frames(8), "nav")
        assertFalse(queue.enqueueMessage(frames(4), "geometry"))
        assertEquals(0, queue.size)
        assertTrue(queue.overflowed)
        assertTrue(queue.resendGeometryRequired)
        queue.acknowledgeGeometry()
        assertFalse(queue.resendGeometryRequired)
    }

    @Test
    fun pollIsFifo() {
        val queue = OutboundQueue()
        queue.enqueueMessage(listOf(byteArrayOf(1), byteArrayOf(2)), "a")
        assertEquals(1.toByte(), queue.poll()!!.bytes[0])
        assertEquals(2.toByte(), queue.poll()!!.bytes[0])
        assertEquals(null, queue.poll())
    }

    @Test
    fun writePumpPacesFifteenMilliseconds() {
        val policy = WritePumpPolicy(pacingMs = 15)
        assertTrue(policy.mayWrite(lastWriteAtMs = 0, nowMs = 100))
        assertFalse(policy.mayWrite(lastWriteAtMs = 100, nowMs = 110))
        assertEquals(5, policy.delayUntilNextWrite(lastWriteAtMs = 100, nowMs = 110))
        assertTrue(policy.mayWrite(lastWriteAtMs = 100, nowMs = 115))
    }

    @Test
    fun reconnectBackoffMatchesUpstream() {
        val policy = ReconnectPolicy()
        val expected = listOf(1_000L, 2_000L, 4_000L, 8_000L, 15_000L, 30_000L, 30_000L)
        expected.forEach { assertEquals(it, policy.nextDelayMs()) }
        policy.reset()
        assertEquals(1_000L, policy.nextDelayMs())
    }

    @Test
    fun mtuToFrameSizeClamps() {
        assertEquals(20, BleProtocol.mtuToFrameSize(23))
        assertEquals(13, BleProtocol.mtuToFrameSize(5))
        assertEquals(247, BleProtocol.mtuToFrameSize(250))
        assertEquals(512, BleProtocol.mtuToFrameSize(1000))
    }
}
