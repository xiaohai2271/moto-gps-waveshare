package dev.glimpse.motogps.ble

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 布局相关协议常量（§5.2/§5.3）：消息类型与能力位。
 */
class LayoutProtocolConstantsTest {

    @Test
    fun uiLayoutMessageTypesUseProtocolValues() {
        assertEquals(0x15, BleProtocol.MessageType.UI_LAYOUT)
        assertEquals(0x16, BleProtocol.MessageType.UI_LAYOUT_STATE)
    }

    @Test
    fun capabilityBitsMatchProtocolSection52() {
        assertEquals(1L shl 8, BleProtocol.Capability.UI_LAYOUT)
        assertEquals(1L shl 9, BleProtocol.Capability.WIFI_PROVISIONING)
        assertEquals(0x100L, BleProtocol.Capability.UI_LAYOUT)
        assertEquals(0x200L, BleProtocol.Capability.WIFI_PROVISIONING)
    }

    @Test
    fun phoneOffersUiLayoutCapability() {
        // 手机侧能力必须包含 bit8，否则握手交集永远不含布局能力。
        assertTrue(
            BleProtocol.PHONE_CAPABILITIES and BleProtocol.Capability.UI_LAYOUT != 0L,
        )
    }

    @Test
    fun connectedStateExposesCapabilityForGating() {
        // bit8 门控语义：老固件 capability 无 bit8 → 不发送 0x15。
        val oldFirmware = 0x00000000FFL
        val newFirmware = 0x00000001FFL
        assertEquals(0L, oldFirmware and BleProtocol.Capability.UI_LAYOUT)
        assertTrue(newFirmware and BleProtocol.Capability.UI_LAYOUT != 0L)
    }
}
