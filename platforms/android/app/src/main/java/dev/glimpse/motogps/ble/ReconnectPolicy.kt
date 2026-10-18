package dev.glimpse.motogps.ble

/**
 * Exponential reconnect backoff matching the iOS central: 1, 2, 4, 8, 15, 30 s
 * then capped at 30 s. Attempts reset only after a completed handshake.
 */
class ReconnectPolicy(
    private val delaysMs: List<Long> = listOf(1_000, 2_000, 4_000, 8_000, 15_000, 30_000),
) {
    var attempt: Int = 0
        private set

    fun nextDelayMs(): Long {
        val index = attempt.coerceAtMost(delaysMs.lastIndex)
        attempt += 1
        return delaysMs[index]
    }

    fun reset() {
        attempt = 0
    }
}
