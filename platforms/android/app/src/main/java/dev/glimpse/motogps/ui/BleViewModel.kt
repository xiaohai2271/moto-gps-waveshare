package dev.glimpse.motogps.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import dev.glimpse.motogps.MotoGpsApplication
import dev.glimpse.motogps.ble.BleProtocol
import dev.glimpse.motogps.ble.DiscoveredDevice
import dev.glimpse.motogps.ble.Esp32BleCentral
import dev.glimpse.motogps.nav.NavigationSession
import kotlinx.coroutines.flow.StateFlow

class BleViewModel(application: Application) : AndroidViewModel(application) {
    private val central: Esp32BleCentral =
        (application as MotoGpsApplication).bleCentral
    private val nav: NavigationSession =
        (application as MotoGpsApplication).navigationSession

    val state: StateFlow<Esp32BleCentral.State> = central.state
    val devices: StateFlow<List<DiscoveredDevice>> = central.devices
    val log: StateFlow<List<String>> = central.log
    val navState: StateFlow<NavigationSession.UiState> = nav.state

    fun scan() = central.startScan()
    fun stopScan() = central.stopScan()
    fun connect(address: String) = central.connect(address)
    fun disconnect() = central.disconnect()

    fun openMediaAccessSettings() = nav.openMediaAccessSettings()

    fun frameSizeFor(mtu: Int): Int = BleProtocol.mtuToFrameSize(mtu)
}
