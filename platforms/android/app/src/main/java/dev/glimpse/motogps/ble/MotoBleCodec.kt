package dev.glimpse.motogps.ble

import dev.glimpse.motogps.NativeBridge

/** Decoded ConnectionStatus payload (see protocol §ConnectionStatus). */
data class DeviceStatus(
    val role: Int,
    val state: Int,
    val minimumVersion: Int,
    val maximumVersion: Int,
    val capabilities: Long,
    val sessionId: Long,
    val maxFrameSize: Int,
    val heartbeatIntervalMs: Int,
) {
    val isDevice: Boolean get() = role == BleProtocol.Role.DEVICE
}

/** Decoded DeviceCommand (device -> phone). */
data class RemoteCommand(
    val kind: Int,
    val commandId: Int,
    val page: Int,
    val x: Int,
    val y: Int,
    val eventTimeMs: Long,
)

/** Decoded Ack status. */
data class DecodedAck(val acknowledgedSequence: Int, val status: Int, val commandId: Int)

/**
 * Kotlin facade over the shared C++ BLE codec. Every byte placed on the wire is
 * produced by shared/ble_protocol; Kotlin only schedules and routes frames.
 */
class MotoBleCodec : AutoCloseable {
    private var reassembler: Long = NativeBridge.createReassembler(BleProtocol.FRAGMENT_TIMEOUT_MS)

    fun encodeConnectionStatus(
        role: Int,
        state: Int,
        sessionId: Long,
        maxFrameSize: Int,
        heartbeatIntervalMs: Int,
    ): ByteArray = NativeBridge.encodeConnectionStatus(
        role, state,
        BleProtocol.PROTOCOL_VERSION, BleProtocol.PROTOCOL_VERSION,
        BleProtocol.PHONE_CAPABILITIES, sessionId, maxFrameSize, heartbeatIntervalMs,
    )

    fun encodeHeartbeat(sessionId: Long, sessionElapsedMs: Long): ByteArray =
        NativeBridge.encodeHeartbeat(sessionId, sessionElapsedMs, 0)

    fun encodeAck(acknowledgedSequence: Int, status: Int, commandId: Int): ByteArray =
        NativeBridge.encodeAck(acknowledgedSequence, status, commandId)

    fun fragment(
        messageType: Int,
        sequence: Int,
        payload: ByteArray,
        maxFrameSize: Int,
        applicationFlags: Int = 0,
    ): List<ByteArray> = NativeBridge.fragmentMessage(
        messageType, sequence, applicationFlags, maxFrameSize, payload,
    ).toList()

    fun decodeConnectionStatus(payload: ByteArray): DeviceStatus {
        val v = NativeBridge.decodeConnectionStatus(payload)
        return DeviceStatus(
            role = v[1].toInt(),
            state = v[2].toInt(),
            minimumVersion = v[3].toInt(),
            maximumVersion = v[4].toInt(),
            capabilities = v[5],
            sessionId = v[6],
            maxFrameSize = v[7].toInt(),
            heartbeatIntervalMs = v[8].toInt(),
        )
    }

    fun decodeAck(payload: ByteArray): DecodedAck {
        val v = NativeBridge.decodeAck(payload)
        return DecodedAck(v[1], v[2], v[3])
    }

    fun decodeDeviceCommand(payload: ByteArray): RemoteCommand {
        val v = NativeBridge.decodeDeviceCommand(payload)
        return RemoteCommand(v[1].toInt(), v[2].toInt(), v[3].toInt(), v[4].toInt(), v[5].toInt(), v[6])
    }

    /** Result of pushing one inbound GATT notification into the C++ reassembler. */
    sealed interface Reassembly {
        data class InProgress(val droppedIncomplete: Boolean) : Reassembly
        data class Complete(
            val type: Int,
            val flags: Int,
            val sequence: Int,
            val payload: ByteArray,
        ) : Reassembly
        data class Duplicate(val type: Int, val sequence: Int) : Reassembly
        data class Failed(val error: Int) : Reassembly
    }

    fun pushFrame(frame: ByteArray, nowMs: Long): Reassembly {
        val raw = runCatching {
            NativeBridge.reassemblerPush(reassembler, frame, nowMs)
        }.getOrNull() ?: return Reassembly.Failed(-1)
        val fields = raw.split(',').mapNotNull {
            val i = it.indexOf('=')
            if (i <= 0) null else it.substring(0, i) to it.substring(i + 1)
        }.toMap()
        val state = fields["state"]?.toIntOrNull() ?: 3
        val dropped = fields["dropped"] == "1"
        return when (state) {
            0 -> Reassembly.InProgress(dropped)
            1 -> Reassembly.Complete(
                fields["type"]?.toIntOrNull() ?: 0,
                fields["flags"]?.toIntOrNull() ?: 0,
                fields["seq"]?.toIntOrNull() ?: 0,
                fields["payload"].orEmpty().hexToBytes(),
            )
            2 -> Reassembly.Duplicate(
                fields["type"]?.toIntOrNull() ?: 0,
                fields["seq"]?.toIntOrNull() ?: 0,
            )
            else -> Reassembly.Failed(fields["error"]?.toIntOrNull() ?: 0)
        }
    }

    fun resetReassembler() {
        NativeBridge.reassemblerReset(reassembler)
    }

    override fun close() {
        if (reassembler != 0L) {
            NativeBridge.destroyReassembler(reassembler)
            reassembler = 0L
        }
    }

    private fun String.hexToBytes(): ByteArray {
        if (isEmpty()) return ByteArray(0)
        return ByteArray(length / 2) { i ->
            substring(i * 2, i * 2 + 2).toInt(16).toByte()
        }
    }
}
