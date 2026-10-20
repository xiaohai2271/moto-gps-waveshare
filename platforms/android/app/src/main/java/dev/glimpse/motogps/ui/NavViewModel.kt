package dev.glimpse.motogps.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import dev.glimpse.motogps.MotoGpsApplication
import dev.glimpse.motogps.ble.BleProtocol
import dev.glimpse.motogps.ble.DiscoveredDevice
import dev.glimpse.motogps.ble.Esp32BleCentral
import dev.glimpse.motogps.gateway.Place
import dev.glimpse.motogps.layout.DeviceLayoutState
import dev.glimpse.motogps.layout.LayoutCodec
import dev.glimpse.motogps.layout.LayoutEditorState
import dev.glimpse.motogps.layout.LayoutStore
import dev.glimpse.motogps.nav.NavigationForegroundService
import dev.glimpse.motogps.nav.NavigationSession
import dev.glimpse.motogps.nav.SavedPlace
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/**
 * View model for the iOS-parity single-flow UI (出发 → 路线 → 导航中) plus the
 * device/map/gateway sheets. The BLE central and navigation session are
 * application-scoped so screens never own connection state.
 */
class NavViewModel(application: Application) : AndroidViewModel(application) {
    private val central: Esp32BleCentral =
        (application as MotoGpsApplication).bleCentral

    /** The BLE central, for WiFi provisioning screens. */
    fun bleCentral(): Esp32BleCentral = central
    private val session: NavigationSession =
        (application as MotoGpsApplication).navigationSession

    val state: StateFlow<NavigationSession.UiState> = session.state
    val recentPlaces: StateFlow<List<SavedPlace>> = session.recent
    val bleState: StateFlow<Esp32BleCentral.State> = central.state
    val devices: StateFlow<List<DiscoveredDevice>> = central.devices

    // --- 圆屏布局编辑（§7.4） ----------------------------------------------

    // Shared with NavigationSession so the zoom chosen here reaches the
    // MapScene window radius (§4.8).
    private val layoutStore = LayoutStore.shared(application)
    val layoutEditor = LayoutEditorState(layoutStore)

    /** 是否支持布局编辑（连接后能力位 bit8；未连接按本地可编辑处理）。 */
    val deviceSupportsLayout: Boolean
        get() = (central.state.value as? Esp32BleCentral.State.Connected)
            ?.let { it.capabilities and BleProtocol.Capability.UI_LAYOUT != 0L } ?: false

    val deviceSupportsWifi: Boolean
        get() = (central.state.value as? Esp32BleCentral.State.Connected)
            ?.let { it.capabilities and BleProtocol.Capability.WIFI_PROVISIONING != 0L } ?: false

    private var lastPushedRevision = -1
    private var pushDebounce: Job? = null

    init {
        // 连接状态驱动同步状态；UiLayoutState 决定是否需要推送。
        viewModelScope.launch {
            central.state.collect { ble ->
                when (ble) {
                    is Esp32BleCentral.State.Connected ->
                        if (ble.capabilities and BleProtocol.Capability.UI_LAYOUT != 0L) {
                            layoutEditor.onConnected()
                        }
                    else -> layoutEditor.onDisconnected()
                }
            }
        }
        central.layoutStateListener = { deviceState ->
            val needsPush = layoutEditor.onDeviceLayoutState(
                DeviceLayoutState(
                    revision = deviceState.revision,
                    layoutRevision = deviceState.layoutRevision,
                    source = deviceState.source,
                    supportedMask = deviceState.supportedMask,
                    flags = deviceState.flags,
                    reserved = deviceState.reserved,
                ),
            )
            if (needsPush) scheduleLayoutPush()
        }
        central.layoutAckListener = { ok ->
            if (ok) layoutEditor.onPushAcked() else layoutEditor.onPushFailed()
        }
    }

    /**
     * 推送布局：§5.6 300ms 防抖合并；同一 revision 不重复发；编码失败
     * （未连接/固件不支持）时保持待同步状态。
     */
    fun scheduleLayoutPush() {
        pushDebounce?.cancel()
        pushDebounce = viewModelScope.launch {
            delay(300)
            pushLayoutNow()
        }
    }

    fun pushLayoutNow() {
        val layout = layoutEditor.layout
        if (layout.revision == lastPushedRevision) return
        if (!deviceSupportsLayout) return
        val encoded = runCatching { LayoutCodec.encode(layout) }.getOrElse { return }
        if (central.sendUiLayout(layout.revision.toLong(), encoded)) {
            lastPushedRevision = layout.revision
            layoutEditor.onPushStarted()
        }
    }

    /** ACK 成功后由外层观测（central 日志/状态）；此处保留 API 给编辑器。 */
    fun onLayoutPushAcked() = layoutEditor.onPushAcked()

    fun onLayoutPushFailed() = layoutEditor.onPushFailed()

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

    val isNavigationActive: Boolean get() = session.state.value.isNavigating

    /** Planning origin of the current preview (lon, lat), like iOS routePreviewOrigin. */
    val routePreviewOrigin: Pair<Double, Double>?
        get() = session.state.value.previewOrigin

    // --- demo -----------------------------------------------------------------

    fun startDemo(query: String = "泉城广场") = session.startDemo(query)

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
