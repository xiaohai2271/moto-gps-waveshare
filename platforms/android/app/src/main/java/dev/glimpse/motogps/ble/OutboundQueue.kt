package dev.glimpse.motogps.ble

/**
 * Outbound frame queue with whole-message admission and the upstream overflow
 * rule: when admitting a message would exceed the cap, drop everything and
 * require a full route-geometry resend instead of interleaving fragments.
 */
class OutboundQueue(private val capacity: Int = 128) {
    data class Entry(val bytes: ByteArray, val label: String)

    private val entries = ArrayDeque<Entry>()

    var overflowed: Boolean = false
        private set
    var resendGeometryRequired: Boolean = false
        private set

    val size: Int get() = entries.size

    fun enqueueMessage(frames: List<ByteArray>, label: String): Boolean {
        if (frames.isEmpty()) return true
        if (entries.size + frames.size > capacity) {
            entries.clear()
            overflowed = true
            resendGeometryRequired = true
            return false
        }
        frames.forEach { entries.addLast(Entry(it, label)) }
        return true
    }

    fun poll(): Entry? = entries.removeFirstOrNull()

    fun clear() {
        entries.clear()
    }

    fun acknowledgeGeometry() {
        resendGeometryRequired = false
    }

    fun resetOverflow() {
        overflowed = false
    }
}

/** Paces write-without-response callbacks: one frame per [pacingMs]. */
class WritePumpPolicy(private val pacingMs: Long = 15) {
    fun mayWrite(lastWriteAtMs: Long, nowMs: Long): Boolean =
        lastWriteAtMs == 0L || nowMs - lastWriteAtMs >= pacingMs

    fun delayUntilNextWrite(lastWriteAtMs: Long, nowMs: Long): Long {
        if (lastWriteAtMs == 0L) return 0
        val elapsed = nowMs - lastWriteAtMs
        return if (elapsed >= pacingMs) 0 else pacingMs - elapsed
    }
}
