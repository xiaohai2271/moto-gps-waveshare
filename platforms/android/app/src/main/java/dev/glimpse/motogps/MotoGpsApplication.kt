package dev.glimpse.motogps

import android.app.Application
import dev.glimpse.motogps.ble.Esp32BleCentral
import dev.glimpse.motogps.nav.NavigationSession

/**
 * Process-scoped singletons: the BLE central and the navigation session must be
 * shared by the Compose screens so switching tabs cannot create a second GATT
 * connection or a second navigation core.
 */
class MotoGpsApplication : Application() {
    lateinit var bleCentral: Esp32BleCentral
        private set
    lateinit var navigationSession: NavigationSession
        private set

    override fun onCreate() {
        super.onCreate()
        CrashLogger.install(this)
        bleCentral = Esp32BleCentral(this)
        navigationSession = NavigationSession(this, bleCentral)
    }
}
