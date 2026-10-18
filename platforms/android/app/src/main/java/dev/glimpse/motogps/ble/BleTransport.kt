package dev.glimpse.motogps.ble

/** Scanned device shown to the user for explicit selection. */
data class DiscoveredDevice(val address: String, val name: String?, val rssi: Int)

/**
 * BLE transport abstraction. AndroidBleTransport is the production
 * implementation; tests can substitute a fake to drive the handshake machine.
 */
interface BleTransport {
    interface Listener {
        fun onBluetoothUnavailable(reason: String)
        fun onScanResult(device: DiscoveredDevice)
        fun onScanStopped()
        fun onConnected(address: String)
        fun onMtuChanged(mtu: Int)
        /** Services discovered and TX notifications subscribed. */
        fun onReady()
        fun onNotification(value: ByteArray)
        fun onWriteComplete(success: Boolean)
        fun onDisconnected(reason: String?)
        fun onError(message: String)
        /** Non-fatal transport anomaly (kept for logging only). */
        fun onWarning(message: String) {}
    }

    val connectedAddress: String?
    val negotiatedMtu: Int

    fun startScan()
    fun stopScan()
    fun connect(address: String)
    fun disconnect()
    /** Queues one complete GATT frame; [withResponse] for control frames. */
    fun write(frame: ByteArray, withResponse: Boolean)
    fun close()
}
