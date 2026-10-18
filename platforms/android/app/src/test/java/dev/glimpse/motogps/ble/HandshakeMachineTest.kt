package dev.glimpse.motogps.ble

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class HandshakeMachineTest {
    private fun deviceReady(
        session: Long,
        state: Int = BleProtocol.State.READY,
        maxFrame: Int = 185,
        capabilities: Long = BleProtocol.PHONE_CAPABILITIES,
        minVersion: Int = 1,
        maxVersion: Int = 1,
    ) = DeviceStatus(
        role = BleProtocol.Role.DEVICE,
        state = state,
        minimumVersion = minVersion,
        maximumVersion = maxVersion,
        capabilities = capabilities,
        sessionId = session,
        maxFrameSize = maxFrame,
        heartbeatIntervalMs = 1000,
    )

    @Test
    fun completesThreeStepHandshake() {
        val machine = HandshakeMachine()
        val start = machine.start(0x12345678L, nowMs = 0)
        assertTrue(start is HandshakeMachine.Intent.SendStatus)
        assertEquals(BleProtocol.State.STARTING, (start as HandshakeMachine.Intent.SendStatus).state)
        assertEquals(HandshakeMachine.Phase.AwaitingDeviceReady, machine.phase)

        val firstReady = machine.onDeviceStatus(deviceReady(0x12345678L))
        assertTrue(firstReady is HandshakeMachine.Intent.SendStatusAndReady)
        assertEquals(HandshakeMachine.Phase.AwaitingFinalReady, machine.phase)

        val finalReady = machine.onDeviceStatus(deviceReady(0x12345678L))
        assertTrue(finalReady is HandshakeMachine.Intent.Complete)
        val negotiated = (finalReady as HandshakeMachine.Intent.Complete).negotiated
        assertEquals(185, negotiated.frameSize)
        assertEquals(0x12345678L, negotiated.sessionId)
        assertEquals(HandshakeMachine.Phase.Ready, machine.phase)
    }

    @Test
    fun sessionMismatchRestarts() {
        val machine = HandshakeMachine()
        machine.start(111L, 0)
        val intent = machine.onDeviceStatus(deviceReady(222L))
        assertTrue(intent is HandshakeMachine.Intent.Restart)
    }

    @Test
    fun versionMismatchFails() {
        val machine = HandshakeMachine()
        machine.start(7L, 0)
        val intent = machine.onDeviceStatus(deviceReady(7L, minVersion = 2, maxVersion = 3))
        assertTrue(intent is HandshakeMachine.Intent.Failed)
    }

    @Test
    fun deviceClosingFails() {
        val machine = HandshakeMachine()
        machine.start(7L, 0)
        val intent = machine.onDeviceStatus(deviceReady(7L, state = BleProtocol.State.CLOSING))
        assertTrue(intent is HandshakeMachine.Intent.Failed)
    }

    @Test
    fun frameSizeIsIntersectionAndClamped() {
        val machine = HandshakeMachine(localMaxFrameSize = 512)
        machine.start(9L, 0)
        machine.onDeviceStatus(deviceReady(9L, maxFrame = 40))
        assertEquals(40, machine.negotiatedFrameSize)

        val tiny = HandshakeMachine()
        tiny.start(9L, 0)
        tiny.onDeviceStatus(deviceReady(9L, maxFrame = 10))
        assertEquals(BleProtocol.MIN_FRAME_SIZE, tiny.negotiatedFrameSize)
    }

    @Test
    fun capabilitiesAreIntersected() {
        val machine = HandshakeMachine()
        machine.start(5L, 0)
        machine.onDeviceStatus(
            deviceReady(5L, capabilities = BleProtocol.Capability.NAVIGATION or BleProtocol.Capability.MAP_SCENE),
        )
        assertEquals(
            BleProtocol.Capability.NAVIGATION or BleProtocol.Capability.MAP_SCENE,
            machine.negotiatedCapabilities,
        )
    }

    @Test
    fun heartbeatClockUsesSessionElapsedTime() {
        val machine = HandshakeMachine()
        machine.start(3L, nowMs = 1_000)
        machine.onTick(6_500)
        assertEquals(5_500, machine.sessionElapsedMs)
        machine.onTransportLost()
        assertEquals(0, machine.sessionElapsedMs)
    }

    @Test
    fun ignoresTrafficInIdlePhase() {
        val machine = HandshakeMachine()
        assertEquals(HandshakeMachine.Intent.None, machine.onDeviceStatus(deviceReady(1L)))
    }
}
