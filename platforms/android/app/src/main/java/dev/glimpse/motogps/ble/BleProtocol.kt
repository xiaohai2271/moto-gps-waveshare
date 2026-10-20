package dev.glimpse.motogps.ble

/**
 * BLE v1 constants mirrored from shared/ble_protocol/include/moto/ble_protocol/
 * ble_protocol.hpp. The authoritative codec lives in C++ (see MotoBleCodec);
 * these values are used for filtering scans and for UI/state decisions.
 */
object BleProtocol {
    const val SERVICE_UUID = "7e57a000-b50c-4b6a-9c57-40a54e8e1000"
    const val PHONE_TO_DEVICE_UUID = "7e57a001-b50c-4b6a-9c57-40a54e8e1000"
    const val DEVICE_TO_PHONE_UUID = "7e57a002-b50c-4b6a-9c57-40a54e8e1000"
    const val CCCD_UUID = "00002902-0000-1000-8000-00805f9b34fb"

    const val FRAME_MAGIC = 0xB7
    const val PROTOCOL_VERSION = 1
    const val PAYLOAD_REVISION = 1

    const val FRAME_HEADER_SIZE = 10
    const val FRAME_CRC_SIZE = 2
    const val MAX_ATTRIBUTE_VALUE = 512
    const val DEFAULT_MAX_MESSAGE_SIZE = 4096
    const val MIN_FRAME_SIZE = 13

    object MessageType {
        const val CONNECTION_STATUS = 0x01
        const val HEARTBEAT = 0x02
        const val ACK = 0x03
        const val NAVIGATION_SNAPSHOT = 0x10
        const val ROUTE_GEOMETRY = 0x11
        const val TRAFFIC_DEVIATION = 0x12
        const val MEDIA_STATE = 0x13
        const val MAP_SCENE = 0x14
        const val UI_LAYOUT = 0x15
        const val UI_LAYOUT_STATE = 0x16
        const val WIFI_CONFIG = 0x17
        const val WIFI_STATUS = 0x18
        const val WIFI_SCAN_REQUEST = 0x19
        const val WIFI_SCAN_RESULT = 0x1A

        /** WifiStatus.state (docs §5.5). */
        object WifiState {
            const val OFF = 0
            const val CONNECTING = 1
            const val CONNECTED = 2
            const val FAILED = 3
            const val NO_CREDENTIALS = 4
        }

        /** WifiStatus.error. */
        object WifiError {
            const val NONE = 0
            const val AUTH = 1
            const val SSID_NOT_FOUND = 2
            const val TIMEOUT = 3
            const val INTERNAL = 4
        }

        /** WifiScanResult AP auth mode. */
        object WifiAuth {
            const val OPEN = 0
            const val WPA2 = 1
            const val WPA3 = 2
            const val WPA2_WPA3 = 3
        }

        /** WifiConfig.action (docs §5.5). */
        const val WIFI_ACTION_SAVE_AND_CONNECT = 0
        const val WIFI_ACTION_CONNECT_TEMPORARY = 1
        const val WIFI_ACTION_DISCONNECT = 2
        const val WIFI_ACTION_FORGET = 3

        /** WifiConfig.flags bits. */
        const val WIFI_FLAG_HIDDEN = 1 shl 0
        const val WIFI_FLAG_AUTO_RECONNECT = 1 shl 1
        const val DEVICE_COMMAND = 0x20
    }

    object Flags {
        const val START = 0x01
        const val END = 0x02
        const val ACK_REQUESTED = 0x04
        const val URGENT = 0x08
    }

    object Role {
        const val PHONE = 1
        const val DEVICE = 2
    }

    object State {
        const val STARTING = 0
        const val READY = 1
        const val DEGRADED = 2
        const val CLOSING = 3
    }

    /** Capability bits shared by both endpoints. */
    object Capability {
        const val NAVIGATION = 1L shl 0
        const val ROUTE_GEOMETRY = 1L shl 1
        const val TRAFFIC = 1L shl 2
        const val MEDIA_STATE = 1L shl 3
        const val TOUCH_COMMANDS = 1L shl 4
        const val MUSIC_COMMANDS = 1L shl 5
        const val COMMAND_ACK = 1L shl 6
        const val MAP_SCENE = 1L shl 7
        const val UI_LAYOUT = 1L shl 8
        const val WIFI_PROVISIONING = 1L shl 9
    }

    /** What the phone offers; intersection with the device is negotiated. */
    const val PHONE_CAPABILITIES: Long =
        Capability.NAVIGATION or Capability.ROUTE_GEOMETRY or Capability.TRAFFIC or
            Capability.MEDIA_STATE or Capability.TOUCH_COMMANDS or
            Capability.MUSIC_COMMANDS or Capability.COMMAND_ACK or Capability.MAP_SCENE or
            Capability.UI_LAYOUT or Capability.WIFI_PROVISIONING

    const val DEFAULT_HEARTBEAT_INTERVAL_MS = 1000
    const val DEVICE_LIVENESS_TIMEOUT_MS = 5000L
    const val FRAGMENT_TIMEOUT_MS = 1000L

    fun mtuToFrameSize(mtu: Int): Int = (mtu - 3).coerceIn(MIN_FRAME_SIZE, MAX_ATTRIBUTE_VALUE)
}
