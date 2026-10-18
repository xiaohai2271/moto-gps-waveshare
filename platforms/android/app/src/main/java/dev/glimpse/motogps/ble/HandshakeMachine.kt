package dev.glimpse.motogps.ble

/**
 * Pure handshake state machine for the v1 protocol:
 *
 *   phone Starting -> device Ready -> phone Ready -> device Ready
 *
 * Nothing here touches Android BLE; Esp32BleCentral owns the transport and
 * feeds decoded DeviceStatus values in. Keeping it pure lets JVM tests cover
 * session mismatch, version mismatch and capability intersection.
 */
class HandshakeMachine(
    private val localCapabilities: Long = BleProtocol.PHONE_CAPABILITIES,
    private val localMaxFrameSize: Int = BleProtocol.MAX_ATTRIBUTE_VALUE,
    private val heartbeatIntervalMs: Int = BleProtocol.DEFAULT_HEARTBEAT_INTERVAL_MS,
) {
    enum class Phase { Idle, AwaitingDeviceReady, AwaitingFinalReady, Ready }

    data class Negotiated(
        val sessionId: Long,
        val frameSize: Int,
        val capabilities: Long,
        val heartbeatIntervalMs: Int,
        val degraded: Boolean,
    )

    sealed interface Intent {
        data object None : Intent
        data class SendStatus(val role: Int, val state: Int) : Intent
        data class SendStatusAndReady(val role: Int, val state: Int) : Intent
        data class Complete(val negotiated: Negotiated) : Intent
        data class Restart(val reason: String) : Intent
        data class Failed(val reason: String) : Intent
    }

    var phase: Phase = Phase.Idle
        private set

    var sessionId: Long = 0
        private set

    var sessionElapsedMs: Long = 0
        private set

    fun start(sessionId: Long, nowMs: Long): Intent {
        require(sessionId != 0L) { "session id must be non-zero" }
        this.sessionId = sessionId
        sessionElapsedMs = 0
        startedAtMs = nowMs
        phase = Phase.AwaitingDeviceReady
        return Intent.SendStatus(BleProtocol.Role.PHONE, BleProtocol.State.STARTING)
    }

    private var startedAtMs = 0L

    /** Advances the session clock; used by the heartbeat payload. */
    fun onTick(nowMs: Long) {
        if (phase != Phase.Idle) {
            sessionElapsedMs = (nowMs - startedAtMs).coerceAtLeast(0)
        }
    }

    fun onDeviceStatus(status: DeviceStatus): Intent {
        if (phase == Phase.Idle) return Intent.None
        if (status.sessionId != sessionId) {
            return Intent.Restart("session mismatch")
        }
        if (!status.isDevice) return Intent.None
        if (status.state == BleProtocol.State.CLOSING) {
            return Intent.Failed("device closing")
        }
        if (status.maximumVersion < BleProtocol.PROTOCOL_VERSION ||
            status.minimumVersion > BleProtocol.PROTOCOL_VERSION
        ) {
            return Intent.Failed("no common protocol version")
        }
        return when (phase) {
            Phase.AwaitingDeviceReady -> {
                if (status.state != BleProtocol.State.READY &&
                    status.state != BleProtocol.State.DEGRADED
                ) {
                    return Intent.None
                }
                negotiatedFrameSize = minOf(
                    localMaxFrameSize,
                    status.maxFrameSize,
                    BleProtocol.MAX_ATTRIBUTE_VALUE,
                ).coerceAtLeast(BleProtocol.MIN_FRAME_SIZE)
                negotiatedCapabilities = localCapabilities and status.capabilities
                negotiatedHeartbeatMs = status.heartbeatIntervalMs
                    .coerceAtLeast(250)
                    .coerceAtMost(BleProtocol.DEFAULT_HEARTBEAT_INTERVAL_MS)
                deviceDegraded = status.state == BleProtocol.State.DEGRADED
                phase = Phase.AwaitingFinalReady
                Intent.SendStatusAndReady(BleProtocol.Role.PHONE, BleProtocol.State.READY)
            }

            Phase.AwaitingFinalReady -> {
                if (status.state != BleProtocol.State.READY) return Intent.None
                phase = Phase.Ready
                Intent.Complete(
                    Negotiated(
                        sessionId = sessionId,
                        frameSize = negotiatedFrameSize,
                        capabilities = negotiatedCapabilities,
                        heartbeatIntervalMs = negotiatedHeartbeatMs,
                        degraded = deviceDegraded,
                    ),
                )
            }

            else -> Intent.None
        }
    }

    fun onTransportLost() {
        phase = Phase.Idle
        sessionElapsedMs = 0
    }

    var negotiatedFrameSize = BleProtocol.MIN_FRAME_SIZE
        private set
    var negotiatedCapabilities = 0L
        private set
    var negotiatedHeartbeatMs = BleProtocol.DEFAULT_HEARTBEAT_INTERVAL_MS
        private set
    private var deviceDegraded = false
}
