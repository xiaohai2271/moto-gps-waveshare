package dev.glimpse.motogps.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import dev.glimpse.motogps.MotoGpsApplication
import dev.glimpse.motogps.ble.BleProtocol
import dev.glimpse.motogps.ble.DiscoveredDevice
import dev.glimpse.motogps.ble.Esp32BleCentral
import dev.glimpse.motogps.gateway.Place
import dev.glimpse.motogps.nav.NavigationForegroundService
import dev.glimpse.motogps.nav.NavigationSession
import dev.glimpse.motogps.nav.SavedPlace
import kotlinx.coroutines.flow.StateFlow

/**
 * View model for the iOS-parity single-flow UI (出发 → 路线 → 导航中) plus the
 * device/map/gateway sheets. The BLE central and navigation session are
 * application-scoped so screens never own connection state.
 */
class NavViewModel(application: Application) : AndroidViewModel(application) {
    private val central: Esp32BleCentral =
        (application as MotoGpsApplication).bleCentral
    private val session: NavigationSession =
        (application as MotoGpsApplication).navigationSession

    val state: StateFlow<NavigationSession.UiState> = session.state
    val recentPlaces: StateFlow<List<SavedPlace>> = session.recent
    val bleState: StateFlow<Esp32BleCentral.State> = central.state
    val devices: StateFlow<List<DiscoveredDevice>> = central.devices

    // --- destination search -------------------------------------------------

    fun queryChanged(query: String) = session.queryChanged(query)
    fun submitSearch() = session.submitSearch()
    fun clearQuery() = session.clearQuery()
    fun clearRecent() = session.clearRecent()
    fun search(keywords: String) = session.search(keywords)
    fun clearDestination() = session.clearDestination()
    fun select(place: Place) = session.select(place)
    fun selectRecent(place: SavedPlace) = session.selectRecent(place)

    // --- route preview / ride ------------------------------------------------

    fun planRoutePreview() = session.planRoutePreview()
    fun selectOption(index: Int) = session.selectOption(index)

    fun start() {
        // Foreground service first: navigation must not be killed when the
        // screen turns off. It is started from a visible activity.
        NavigationForegroundService.start(getApplication())
        session.start()
    }

    fun stop() {
        session.stop()
        NavigationForegroundService.stop(getApplication())
    }

    val isNavigationActive: Boolean get() = session.isNavigationActive

    // --- demo -----------------------------------------------------------------

    fun startDemo() = session.startDemo()

    // --- device / gateway / media ---------------------------------------------

    fun startScan() = central.startScan()
    fun stopScan() = central.stopScan()
    fun connect(address: String) = central.connect(address)
    fun disconnect() = central.disconnect()
    fun openMediaAccessSettings() = session.openMediaAccessSettings()
    fun hasMediaAccess(): Boolean = session.hasMediaAccess()
    fun frameSizeFor(mtu: Int): Int = BleProtocol.mtuToFrameSize(mtu)

    // --- gateway ---------------------------------------------------------------

    fun gatewayCurrent(): String = session.gatewayCurrent()
    fun gatewayCustomised(): Boolean = session.gatewayCustomised()
    fun gatewaySave(raw: String): String = session.gatewaySave(raw)
}
