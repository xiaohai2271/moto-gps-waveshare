package dev.glimpse.motogps

import android.util.Log
import dev.glimpse.motogps.ble.BleTransport
import dev.glimpse.motogps.ble.TcpBleTransport
import java.io.File

/**
 * Debug-only application variant. When `files/ble_sim` exists it replaces the
 * Android BLE stack with the loopback link to the host virtual device, so the
 * whole app (handshake, snapshots, layout editor, WiFi provisioning) can run on
 * an emulator with no ESP32 nearby.
 *
 * The marker file is optional: an empty file means 127.0.0.1:9100. Delete it to
 * return to the real BluetoothGatt transport.
 */
class DebugMotoGpsApplication : MotoGpsApplication() {

    override fun createTransportFactory(): ((BleTransport.Listener) -> BleTransport)? {
        val spec = readSimulatorSpec() ?: return null
        val separator = spec.indexOf(':')
        val host = if (separator > 0) spec.substring(0, separator) else spec
        val port = if (separator > 0) {
            spec.substring(separator + 1).toIntOrNull() ?: 9100
        } else {
            9100
        }
        Log.i(TAG, "virtual BLE transport enabled: $host:$port")
        return { listener -> TcpBleTransport(listener, host, port) }
    }

    private fun readSimulatorSpec(): String? {
        val marker = File(filesDir, "ble_sim")
        if (!marker.isFile) return null
        val text = runCatching { marker.readText().trim() }.getOrNull() ?: return null
        return text.ifEmpty { "127.0.0.1:9100" }
    }

    private companion object {
        const val TAG = "MotoGpsSim"
    }
}
