package dev.glimpse.motogps

/**
 * Thin JNI facade over the shared C++ core (shared/ble_protocol,
 * shared/coordinates). All protocol bytes come from the upstream implementation
 * so the Android app cannot silently drift from the v1 wire format.
 */
object NativeBridge {
    init {
        System.loadLibrary("moto_core")
    }

    external fun crc16CcittFalse(bytes: ByteArray): Int

    /**
     * Fragments one encoded payload into complete GATT frames.
     * [applicationFlags] may only contain ACK_REQUESTED(4) and URGENT(8);
     * START/END are assigned by the shared implementation.
     */
    external fun fragmentMessage(
        messageType: Int,
        sequence: Int,
        applicationFlags: Int,
        maxFrameSize: Int,
        payload: ByteArray,
    ): Array<ByteArray>

    external fun decodeFrame(frameBytes: ByteArray): ByteArray

    /** Returns `key=hex` lines identical in format to the upstream golden fixture. */
    external fun goldenVectors(): String

    /** @return [longitude, latitude, ok(1.0/0.0)] */
    external fun wgs84ToGcj02(lon: Double, lat: Double): DoubleArray

    // --- BLE session codec (shared/ble_protocol) ---

    external fun encodeConnectionStatus(
        role: Int,
        state: Int,
        minimumVersion: Int,
        maximumVersion: Int,
        capabilities: Long,
        sessionId: Long,
        maxFrameSize: Int,
        heartbeatIntervalMs: Int,
    ): ByteArray

    external fun encodeHeartbeat(
        sessionId: Long,
        monotonicMs: Long,
        statusFlags: Int,
    ): ByteArray

    external fun encodeAck(
        acknowledgedSequence: Int,
        status: Int,
        commandId: Int,
    ): ByteArray

    /** @return [revision, role, state, minVersion, maxVersion, capabilities, sessionId, maxFrameSize, heartbeatIntervalMs] */
    external fun decodeConnectionStatus(payload: ByteArray): LongArray

    /** @return [revision, acknowledgedSequence, status, commandId] */
    external fun decodeAck(payload: ByteArray): IntArray

    /** @return [revision, kind, commandId, page, x, y, eventTimeMs] */
    external fun decodeDeviceCommand(payload: ByteArray): LongArray

    external fun createReassembler(timeoutMs: Long): Long

    /** @return "state=..,error=..,dropped=..,type=..,version=..,flags=..,seq=..,payload=hex" */
    external fun reassemblerPush(handle: Long, frame: ByteArray, nowMs: Long): String

    external fun reassemblerReset(handle: Long)

    external fun destroyReassembler(handle: Long)
}
