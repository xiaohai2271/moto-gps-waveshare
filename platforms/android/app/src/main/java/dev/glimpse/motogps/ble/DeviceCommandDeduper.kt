package dev.glimpse.motogps.ble

/**
 * Remembers recent device command ids for one BLE session.
 *
 * The protocol requires the phone to deduplicate by (session_id, command_id):
 * the round display retransmits a command until it is acknowledged, so a lost
 * ACK must not replay the gesture. Replaying a page change snaps the display
 * back to the previous page; replaying play/pause toggles twice.
 */
class DeviceCommandDeduper(private val capacity: Int = 128) {
    private val seen = ArrayDeque<Long>()

    /** @return true the first time [commandId] is seen, false for a retransmission. */
    fun accept(commandId: Long): Boolean {
        if (seen.contains(commandId)) return false
        seen.addLast(commandId)
        while (seen.size > capacity) seen.removeFirst()
        return true
    }

    fun reset() = seen.clear()

    val size: Int get() = seen.size
}
