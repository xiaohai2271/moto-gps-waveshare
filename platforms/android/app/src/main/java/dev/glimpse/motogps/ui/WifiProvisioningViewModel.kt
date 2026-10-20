package dev.glimpse.motogps.ui

import dev.glimpse.motogps.ble.BleProtocol
import dev.glimpse.motogps.ble.DecodedWifiScan
import dev.glimpse.motogps.ble.DecodedWifiStatus
import dev.glimpse.motogps.ble.Esp32BleCentral
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * State holder for the WiFi provisioning screen (docs §7.6). Wraps the BLE
 * central's WiFi message surface: WifiConfig / WifiScanRequest send, and
 * WifiStatus / WifiScanResult receive. Not a ViewModel — it is created by the
 * screen and holds a reference to the app-scoped BLE central.
 */
class WifiProvisioningViewModel(private val central: Esp32BleCentral) {

    /** True when the connected device offers capability bit9 (WiFi provisioning). */
    val deviceSupportsWifi: Boolean
        get() = (central.state.value as? Esp32BleCentral.State.Connected)
            ?.let { it.capabilities and BleProtocol.Capability.WIFI_PROVISIONING != 0L } ?: false

    val bleState: StateFlow<Esp32BleCentral.State> = central.state

    val wifiStatus: StateFlow<DecodedWifiStatus?> = central.deviceWifiStatus
    val wifiScan: StateFlow<DecodedWifiScan?> = central.deviceWifiScan

    /** Latest scan request id sent (for correlation). */
    private var nextScanRequestId = 1
    private val _scanInFlight = MutableStateFlow(false)
    val scanInFlight: StateFlow<Boolean> = _scanInFlight.asStateFlow()

    private val _lastScanRequestId = MutableStateFlow(0)
    val lastScanRequestId: StateFlow<Int> = _lastScanRequestId.asStateFlow()

    fun connectWifi(ssid: String, password: String, hidden: Boolean, autoReconnect: Boolean): Boolean {
        var flags = 0
        if (hidden) flags = flags or BleProtocol.MessageType.WIFI_FLAG_HIDDEN
        if (autoReconnect) flags = flags or BleProtocol.MessageType.WIFI_FLAG_AUTO_RECONNECT
        return central.sendWifiConfig(
            BleProtocol.MessageType.WIFI_ACTION_SAVE_AND_CONNECT, flags, ssid, password,
        )
    }

    fun connectTemporary(ssid: String, password: String): Boolean =
        central.sendWifiConfig(
            BleProtocol.MessageType.WIFI_ACTION_CONNECT_TEMPORARY,
            0,
            ssid,
            password,
        )

    fun disconnectWifi(): Boolean =
        central.sendWifiConfig(BleProtocol.MessageType.WIFI_ACTION_DISCONNECT, 0, "", "")

    fun forgetWifi(): Boolean =
        central.sendWifiConfig(BleProtocol.MessageType.WIFI_ACTION_FORGET, 0, "", "")

    fun startScan(): Boolean {
        val requestId = nextScanRequestId
        val ok = central.sendWifiScanRequest(requestId)
        if (ok) {
            nextScanRequestId = requestId + 1
            _lastScanRequestId.value = requestId
            _scanInFlight.value = true
        }
        return ok
    }

    fun onScanResult(scan: DecodedWifiScan) {
        if (scan.requestId == _lastScanRequestId.value) {
            _scanInFlight.value = false
        }
    }
}
