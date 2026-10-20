package dev.glimpse.motogps

import android.app.Application
import dev.glimpse.motogps.ble.BleTransport
import dev.glimpse.motogps.ble.Esp32BleCentral
import dev.glimpse.motogps.nav.NavigationSession

/**
 * Process-scoped singletons: the BLE central and the navigation session must be
 * shared by the Compose screens so switching tabs cannot create a second GATT
 * connection or a second navigation core.
 *
 * [createTransportFactory] is the single debug-only injection point used by
 * `DebugMotoGpsApplication` to attach the loopback virtual device instead of
 * the Android BluetoothGatt transport. Release builds keep returning null.
 */
open class MotoGpsApplication : Application() {
    lateinit var bleCentral: Esp32BleCentral
        private set
    lateinit var navigationSession: NavigationSession
        private set

    override fun onCreate() {
        super.onCreate()
        CrashLogger.install(this)
        bleCentral = Esp32BleCentral(this, transportFactory = createTransportFactory())
        navigationSession = NavigationSession(this, bleCentral)
        // Return to the round display that was connected before the process was
        // killed. On the very first launch there is no saved address and this
        // is a no-op; permission/transport failures surface through the BLE
        // state so the UI can explain why the link did not come back.
        bleCentral.autoConnectLast()
    }

    protected open fun createTransportFactory(): ((BleTransport.Listener) -> BleTransport)? =
        null
}
