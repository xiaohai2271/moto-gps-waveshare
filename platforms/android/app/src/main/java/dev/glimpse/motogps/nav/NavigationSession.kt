package dev.glimpse.motogps.nav

import android.content.Context
import android.os.SystemClock
import android.util.Log
import dev.glimpse.motogps.BuildConfig
import dev.glimpse.motogps.NativeBridge
import dev.glimpse.motogps.ble.Esp32BleCentral
import dev.glimpse.motogps.gateway.GatewayClient
import dev.glimpse.motogps.gateway.GatewayException
import dev.glimpse.motogps.gateway.GatewayRoute
import dev.glimpse.motogps.gateway.Place
import dev.glimpse.motogps.location.GpsLocationSource
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlin.math.roundToInt
import kotlin.math.roundToLong

/**
 * Ties the shared NavCore, the gateway and BLE together.
 *
 * Coordinates: GNSS fixes are WGS84 and are passed unchanged into NavCore and
 * the gateway request. Route geometry returned by the gateway is GCJ-02 and is
 * sent to the display untouched; no double conversion anywhere.
 */
class NavigationSession(
    context: Context,
    private val central: Esp32BleCentral,
) {
    private val gatewayStore = GatewaySettingsStore(context)
    private var gateway: GatewayClient = GatewayClient(gatewayStore.current())
    private val recentStore = RecentPlacesStore(context)
    data class UiState(
        val engineState: Int = 0,
        val status: String = "未开始",
        val query: String = "",
        val isSearching: Boolean = false,
        val searchFailure: String? = null,
        val hasFix: Boolean = false,
        val accuracyM: Double = 0.0,
        val places: List<Place> = emptyList(),
        val selected: Place? = null,
        val routeOptions: List<GatewayRoute> = emptyList(),
        val optionsLoading: Boolean = false,
        val chosenOptionIndex: Int = -1,
        val routePreviewFailure: String? = null,
        val route: GatewayRoute? = null,
        val remainingM: Double = 0.0,
        val remainingS: Int = 0,
        val instruction: String = "",
        val roadName: String = "",
        val distanceToManeuverM: Double = 0.0,
        val speedMps: Double = 0.0,
        val bleReady: Boolean = false,
        val mediaAccess: Boolean = false,
        val mediaConnected: Boolean = false,
        val mediaTitle: String = "",
        val navigationFailure: String? = null,
        val previewOrigin: Pair<Double, Double>? = null,
        val isNavigating: Boolean = false,
        val log: List<String> = emptyList(),
    )

    private val engine = NavEngine()
    private val location = GpsLocationSource(context)
    private val tileStore = dev.glimpse.motogps.map.MapTileStore(context, gateway)
    private val media = dev.glimpse.motogps.media.MediaControllerSource(context)
    private val compass = dev.glimpse.motogps.location.CompassSource(context)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    private val _recent = MutableStateFlow(recentStore.load())
    val recent: StateFlow<List<SavedPlace>> = _recent.asStateFlow()

    val isNavigationActive: Boolean get() = navigating

    private var lastFix: GpsLocationSource.Fix? = null
    private var navigating = false
    private var ticker: Job? = null
    private var lastSnapshotAt = 0L
    private var lastGeometryKey: String? = null
    private var bleWasReady = false
    private var requestCounter = 0
    private var hasLoggedFix = false
    private var fixCount = 0
    private var chosenPreviewRoute: GatewayRoute? = null
    private var suppliedPreviewRoute = false
    private var previewOriginLon: Double? = null
    private var previewOriginLat: Double? = null

    // Debug-only harness (BuildConfig.DEBUG): lets adb start a live navigation
    // without touch input. Never enabled in release builds.
    private var pendingDebugQuery: String? = null
    private var autoStartAfterSearch = false
    private var destinationLon: Double? = null
    private var destinationLat: Double? = null
    private var mapSceneRevision = 0L
    private var lastSceneAtMs = 0L
    private var lastSceneLon = 0.0
    private var lastSceneLat = 0.0
    private var lastSnapshotDiagAt = 0L
    private var lastMediaKey: String? = null
    private var compassHeading: Double? = null
    private var lastCompassPushAt = 0L

    init {
        central.commandListener = { command -> onDeviceCommand(command) }
        scope.launch {
            while (true) {
                delay(1_000)
                pushMediaState()
            }
        }
        location.start(
            onFix = { fix -> deliverFix(fix) },
            onFailure = { reason -> log("定位不可用：$reason") },
        )
        val compassStarted = compass.start { degrees -> onCompassHeading(degrees) }
        if (!compassStarted) {
            log("罗盘传感器不可用：静止时航向来自 GPS（通常为 0）")
        }
        scope.launch {
            central.state.collect { bleState ->
                val ready = bleState is Esp32BleCentral.State.Connected
                _state.value = _state.value.copy(bleReady = ready)
                if (ready && !bleWasReady) log("圆屏已就绪，可以发送导航数据")
                if (!ready && bleWasReady) log("圆屏断开，导航继续但数据停发")
                bleWasReady = ready
            }
        }
    }

    /** Pushes MediaState at 1 Hz; harmless when no session or no capability. */
    private fun pushMediaState() {
        val snapshot = runCatching { media.snapshot() }.getOrNull() ?: return
        val access = media.hasAccess()
        _state.value = _state.value.copy(
            mediaAccess = access,
            mediaConnected = snapshot.connected,
            mediaTitle = snapshot.trackTitle,
        )
        val flags = (if (snapshot.connected) 1 else 0) or (if (snapshot.playing) 2 else 0)
        val key = "${snapshot.connected}/${snapshot.playing}/${snapshot.trackTitle}"
        if (key != lastMediaKey) {
            lastMediaKey = key
            log(
                when {
                    !access -> "媒体：未授予通知使用权"
                    !snapshot.connected -> "媒体：已授权，当前没有播放器"
                    else -> "媒体：${snapshot.trackTitle} — ${snapshot.artistName}" +
                        if (snapshot.playing) "（播放中）" else "（暂停）"
                },
            )
        }
        central.sendMediaState(
            flags, snapshot.trackToken, snapshot.positionS, snapshot.durationS,
            snapshot.sourceName, snapshot.trackTitle, snapshot.artistName,
        )
    }

    fun hasMediaAccess(): Boolean = media.hasAccess()

    fun openMediaAccessSettings() = media.openAccessSettings()

    /** Debug-only: exercise the media transport from adb. */
    fun debugMediaAction(action: String) {
        if (!BuildConfig.DEBUG) return
        when (action) {
            "next" -> log("调试媒体：下一首 ${media.next()}")
            "prev" -> log("调试媒体：上一首 ${media.previous()}")
            "toggle" -> log("调试媒体：播放/暂停 ${media.toggle()}")
        }
        pushMediaState()
    }

    private fun deliverFix(fix: GpsLocationSource.Fix) {
        fixCount += 1
        // While stationary the GNSS course is meaningless; use the phone
        // compass so the round display's heading/compass page is meaningful.
        val effective = if (fix.speedMps < 1.0 && compassHeading != null) {
            fix.copy(headingDeg = compassHeading!!)
        } else {
            fix
        }
        lastFix = fix
        _state.value = _state.value.copy(
            hasFix = true,
            accuracyM = fix.accuracyM,
            speedMps = fix.speedMps,
        )
        if (!hasLoggedFix) {
            hasLoggedFix = true
            log("定位可用（${fix.provider}，${fix.accuracyM.roundToInt()} m）")
        }
        if (pendingDebugQuery != null && !navigating) {
            val query = pendingDebugQuery
            pendingDebugQuery = null
            search(query!!)
            autoStartAfterSearch = true
        }
        if (navigating) {
            feedEngine(
                engine.pushFix(
                    effective.longitude, effective.latitude, effective.accuracyM,
                    effective.speedMps, effective.headingDeg, effective.timestampMs,
                ),
            )
        }
    }

    /**
     * Compass updates push a heading-only fix at <=4 Hz while stationary so the
     * display rotates smoothly without waiting for the next GNSS callback.
     */
    private fun onCompassHeading(degrees: Double) {
        compassHeading = degrees
        if (!navigating) return
        val fix = lastFix ?: return
        if (fix.speedMps >= 1.0) return
        val now = SystemClock.elapsedRealtime()
        if (now - lastCompassPushAt < 250) return
        lastCompassPushAt = now
        feedEngine(
            engine.pushFix(
                fix.longitude, fix.latitude, fix.accuracyM,
                0.0, degrees, System.currentTimeMillis(),
            ),
        )
    }

    /**
     * Debug-only: injects a clearly labelled simulated position so the full
     * hardware path can be exercised without a GNSS fix (indoors). Never used
     * by the release UI, and the log states it is simulated.
     */
    fun debugSimulatedFix(lon: Double, lat: Double) {
        if (!BuildConfig.DEBUG) return
        log("调试：注入模拟定位（非真实 GPS）$lat,$lon")
        deliverFix(
            GpsLocationSource.Fix(
                longitude = lon,
                latitude = lat,
                accuracyM = 5.0,
                speedMps = 0.0,
                headingDeg = 0.0,
                timestampMs = System.currentTimeMillis(),
                provider = "simulated",
            ),
        )
    }

    /** Debug-only: pre-cache tiles around a GCJ-02 position for MapScene tests. */
    fun debugPrefetch(lon: Double, lat: Double) {
        if (!BuildConfig.DEBUG) return
        scope.launch {
            val gcj = NativeBridge.wgs84ToGcj02(lon, lat)
            if (gcj[2] == 0.0) return@launch
            val tiles = runCatching {
                dev.glimpse.motogps.map.MapTilePlanner.aroundGcj02(gcj[1], gcj[0], 500.0)
            }.getOrElse { return@launch }
            log("调试：预取 ${tiles.size} 个瓦片")
            runCatching {
                tileStore.download("调试预取", "debug-prefetch", tiles)
            }.onSuccess { pack ->
                log("调试：预取完成 ${pack.tiles.size} 瓦片")
            }.onFailure { log("调试：预取失败 ${it.message}") }
        }
    }

    /**
     * Device gestures are authoritative for page selection: swipe left/right
     * cycles the pages the phone supports and the next snapshot carries the new
     * display_page. Music is excluded until MediaState is implemented, so the
     * device never lands on an empty music page.
     */
    private fun onDeviceCommand(command: dev.glimpse.motogps.ble.RemoteCommand) {
        when (command.kind) {
            COMMAND_MUSIC_PREVIOUS -> {
                val ok = media.previous()
                log(if (ok) "圆屏按键：上一首" else "上一首失败（无媒体会话或未授权）")
                pushMediaState()
                return
            }
            COMMAND_MUSIC_TOGGLE -> {
                val ok = media.toggle()
                log(if (ok) "圆屏按键：播放/暂停" else "播放控制失败（无媒体会话或未授权）")
                pushMediaState()
                return
            }
            COMMAND_MUSIC_NEXT -> {
                val ok = media.next()
                log(if (ok) "圆屏按键：下一首" else "下一首失败（无媒体会话或未授权）")
                pushMediaState()
                return
            }
            COMMAND_MUSIC_LIKE -> {
                log("圆屏按键：喜欢（协议支持，但 Android 无通用点赞接口，已忽略）")
                return
            }
        }
        val pages = SUPPORTED_PAGES
        val target = when (command.kind) {
            COMMAND_PAGE_SELECTED -> command.page
            COMMAND_SWIPE_LEFT -> {
                val index = pages.indexOf(engine.snapshot().displayPage).let { if (it < 0) 0 else it }
                pages[(index + 1) % pages.size]
            }
            COMMAND_SWIPE_RIGHT -> {
                val index = pages.indexOf(engine.snapshot().displayPage).let { if (it < 0) 0 else it }
                pages[(index - 1 + pages.size) % pages.size]
            }
            else -> return
        }
        if (target !in pages) {
            log("设备请求的页面 ${target} 暂不支持（音乐需先接入 MediaState）")
            return
        }
        val current = engine.snapshot().displayPage
        if (target == current) return
        // No time-based debounce: the BLE layer already drops retransmissions by
        // command_id. Dropping a legitimate second swipe here made the display
        // fall back to the previous page on the next snapshot (a visible jump).
        runCatching { feedEngine(engine.selectPage(target)) }
            .onSuccess {
                log("切换圆屏页面 → $target")
                // The firmware does not switch its own page on a gesture: it
                // asks the phone and only shows the new page once a snapshot
                // carrying that page arrives. Echo it immediately (twice, to
                // survive a dropped frame) so a quick second swipe computes its
                // next page from the new one instead of the stale one.
                echoPageNow()
                scope.launch {
                    delay(PAGE_ECHO_MS)
                    echoPageNow()
                }
            }
            .onFailure { log("切换页面失败：${it.message}") }
    }

    /** Bypasses the 200 ms snapshot throttle to acknowledge a page change. */
    private fun echoPageNow() {
        if (!_state.value.bleReady) return
        lastSnapshotAt = SystemClock.elapsedRealtime()
        runCatching { sendSnapshot(engine.snapshot()) }
    }

    /** Debug-only: inject a device gesture (e.g. swipe) for hardware checks. */
    fun debugRemoteCommand(kind: Int, page: Int = 0) {
        if (!BuildConfig.DEBUG) return
        onDeviceCommand(dev.glimpse.motogps.ble.RemoteCommand(kind, 0, page, 0, 0, 0))
    }

    fun search(keywords: String) {        scope.launch {
            runCatching {
                val fix = lastFix
                gateway.searchPlaces(keywords, fix?.let { it.longitude to it.latitude })
            }.onSuccess { places ->
                _state.value = _state.value.copy(places = places)
                log("搜索到 ${places.size} 个地点")
                if (autoStartAfterSearch) {
                    places.firstOrNull { it.isWgs84 }?.let { place ->
                        select(place)
                    }
                }
            }.onFailure { log("搜索失败：${it.message}") }
        }
    }

    fun select(place: Place) {
        if (!place.isWgs84) {
            log("地点坐标系不是 WGS84，已拒绝")
            return
        }
        _state.value = _state.value.copy(
            selected = place,
            routeOptions = emptyList(),
            chosenOptionIndex = -1,
            navigationFailure = null,
        )
        log("已选择：${place.name}")
        recordRecent(place)
        planRoutePreview()
    }

    /**
     * Mirrors iOS planRoutePreview(): always take a fresh one-shot fix (never
     * the stale point that biased the search), then ask the gateway for up to
     * three candidate routes for the user to pick from.
     */
    fun planRoutePreview() {
        if (navigating) return
        val place = _state.value.selected ?: return
        _state.value = _state.value.copy(
            optionsLoading = true,
            routeOptions = emptyList(),
            chosenOptionIndex = -1,
            navigationFailure = null,
        )
        log("正在规划路线")
        scope.launch {
            val origin = awaitFreshFix()
            if (origin == null) {
                _state.value = _state.value.copy(
                    optionsLoading = false,
                    navigationFailure = "需要当前位置才能规划路线，请检查定位",
                )
                return@launch
            }
            previewOriginLon = origin.first
            previewOriginLat = origin.second
            _state.value = _state.value.copy(previewOrigin = origin.first to origin.second)
            val requestId = ++requestCounter
            runCatching {
                gateway.routeOptions(
                    requestId, origin.first, origin.second,
                    place.longitude, place.latitude,
                    isReroute = false, previousRouteId = null,
                    destinationPoiId = place.id.ifEmpty { null },
                )
            }.onSuccess { candidates ->
                if (candidates.isEmpty()) {
                    _state.value = _state.value.copy(
                        optionsLoading = false,
                        navigationFailure = "高德没有返回可用路线，请稍后重试",
                    )
                    return@onSuccess
                }
                _state.value = _state.value.copy(
                    optionsLoading = false,
                    routeOptions = candidates,
                    chosenOptionIndex = 0,
                )
                log("已获取 ${candidates.size} 条候选路线")
                if (autoStartAfterSearch) {
                    autoStartAfterSearch = false
                    start()
                }
            }.onFailure { error ->
                _state.value = _state.value.copy(
                    optionsLoading = false,
                    navigationFailure = "路线规划失败，请检查网络后重试",
                )
                log("候选路线请求失败：${error.message}")
            }
        }
    }

    /** One-shot fresh fix for route planning (iOS refreshForRoutePlanning). */
    private suspend fun awaitFreshFix(): Pair<Double, Double>? {
        val recent = lastFix
        if (recent != null && System.currentTimeMillis() - recent.timestampMs <= 5_000) {
            return recent.longitude to recent.latitude
        }
        log("正在获取当前位置")
        val deadline = SystemClock.elapsedRealtime() + 12_000
        val startCount = fixCount
        while (SystemClock.elapsedRealtime() < deadline) {
            delay(250)
            if (fixCount > startCount) {
                lastFix?.let { return it.longitude to it.latitude }
            }
        }
        return lastFix?.let { it.longitude to it.latitude }
    }

    fun selectOption(index: Int) {
        val options = _state.value.routeOptions
        if (index !in options.indices) return
        _state.value = _state.value.copy(chosenOptionIndex = index, navigationFailure = null)
        log("已选择：${optionTitle(_state.value.chosenOptionIndex)}")
    }

    /** iOS clearDestination(): back to the destination search state. */
    fun clearDestination() {
        if (navigating) {
            log("请先结束导航，再更换目的地")
            return
        }
        _state.value = _state.value.copy(
            selected = null,
            routeOptions = emptyList(),
            chosenOptionIndex = -1,
            navigationFailure = null,
            route = null,
        )
    }

    // --- iOS-parity search surface (debounced query, recent places) ---------

    private var searchDebounce: Job? = null
    private var lastQuery = ""

    fun queryChanged(query: String) {
        if (navigating) return
        _state.value = _state.value.copy(query = query)
        val trimmed = query.trim()
        if (trimmed == lastQuery) return
        lastQuery = trimmed
        if (trimmed.length < 2) {
            searchDebounce?.cancel()
            _state.value = _state.value.copy(places = emptyList())
            return
        }
        searchDebounce?.cancel()
        searchDebounce = scope.launch {
            delay(480)
            searchInternal(trimmed)
        }
    }

    fun submitSearch() {
        if (navigating) return
        searchDebounce?.cancel()
        if (lastQuery.length >= 2) searchInternal(lastQuery)
    }

    fun clearQuery() {
        searchDebounce?.cancel()
        lastQuery = ""
        _state.value = _state.value.copy(places = emptyList(), query = "")
    }

    fun clearRecent() {
        _recent.value = recentStore.clear()
        log("已清空最近搜索")
    }

    fun selectRecent(place: SavedPlace) {
        select(
            Place(
                id = place.id, name = place.name, address = place.address,
                city = place.city, district = place.district,
                displayArea = place.displayArea,
                longitude = place.longitude, latitude = place.latitude,
                coordinateSystem = "WGS84", distanceM = null,
            ),
        )
    }

    private fun recordRecent(place: Place) {
        _recent.value = recentStore.record(SavedPlace.fromPlace(place))
    }

    private fun searchInternal(keywords: String) {
        scope.launch {
            _state.value = _state.value.copy(isSearching = true, searchFailure = null)
            runCatching {
                val fix = lastFix
                gateway.searchPlaces(keywords, fix?.let { it.longitude to it.latitude })
            }.onSuccess { places ->
                _state.value = _state.value.copy(places = places, isSearching = false)
                log("搜索到 ${places.size} 个地点")
            }.onFailure { error ->
                _state.value = _state.value.copy(
                    isSearching = false,
                    searchFailure = "地点搜索失败，请检查网络后重试",
                )
                log("搜索失败：${error.message}")
            }
        }
    }

    // --- gateway settings（iOS GatewaySettingsView 副作用） -----------------

    fun gatewayCurrent(): String = gatewayStore.current()

    /** Shared download store, so map packs follow the configured gateway. */
    fun mapTiles(): dev.glimpse.motogps.map.MapTileStore = tileStore

    fun gatewayClient(): GatewayClient = gateway

    /**
     * Geometry used by the route-corridor download: the active route while
     * navigating, otherwise the selected preview candidate (iOS
     * mapDownloadRoute). Points are GCJ-02, exactly as the gateway returned them.
     */
    fun routePolylineForDownload(): List<Pair<Double, Double>> {
        val state = _state.value
        return state.route?.polyline?.map { it.longitude to it.latitude }
            ?: state.routeOptions.getOrNull(state.chosenOptionIndex)
                ?.polyline?.map { it.longitude to it.latitude }
            ?: emptyList()
    }

    fun gatewayCustomised(): Boolean = gatewayStore.isCustomised()

    /** @return the saved URL; throws IllegalArgumentException on bad input. */
    fun gatewaySave(raw: String): String {
        require(!navigating) { "请先结束导航，再更换网关地址。" }
        val url = gatewayStore.save(raw)
        gateway = GatewayClient(url)
        clearDestination()
        log("网关已更新：$url")
        return url
    }

    // --- demo（iOS 演示模式：模拟定位 + 真实路网，明确标注） ----------------

    fun startDemo() {
        if (navigating) return
        log("演示导航开始：位置与行驶过程为模拟数据")
        // Jinan demo fixture origin (shared/demo_fixture), labelled simulated.
        debugSimulatedFix(117.1210, 36.6705)
        pendingDebugQuery = "泉城广场"
        autoStartAfterSearch = true
        if (lastFix != null) {
            val query = pendingDebugQuery
            pendingDebugQuery = null
            search(query!!)
        }
    }

    fun start() {
        val place = _state.value.selected ?: run {
            log("请先选择终点")
            return
        }
        val fix = lastFix ?: run {
            log("等待定位后再开始导航")
            return
        }
        val options = _state.value.routeOptions
        if (options.isEmpty()) {
            log("请先完成路线规划并选择一条路线")
            planRoutePreview()
            return
        }
        navigating = true
        _state.value = _state.value.copy(isNavigating = true)
        destinationLon = place.longitude
        destinationLat = place.latitude
        // PreviewSelectedRouteProvider semantics: the chosen candidate is fed to
        // NavCore's first (non-reroute) request; reroutes use the live gateway.
        chosenPreviewRoute = _state.value.chosenOptionIndex
            .takeIf { it in options.indices }?.let { options[it] }
        suppliedPreviewRoute = false
        _state.value = _state.value.copy(status = "规划中", places = emptyList())
        log("开始导航：${place.name}")
        // A JNI/protocol failure must surface as a status line, never a crash.
        val begun = runCatching { engine.beginNavigation(place.longitude, place.latitude) }
            .onFailure {
                navigating = false
                _state.value = _state.value.copy(
                    isNavigating = false,
                    navigationFailure = "导航核心启动失败：${it.message}",
                )
                log("导航核心启动失败：${it.message}")
            }
            .getOrNull()
        if (begun == null) return
        feedEngine(begun)
        scope.launch {
            runCatching {
                engine.setNetwork(2) // online
                engine.pushFix(fix.longitude, fix.latitude, fix.accuracyM, fix.speedMps, fix.headingDeg, fix.timestampMs)
            }.onSuccess { feedEngine(it) }
                .onFailure { log("定位注入失败：${it.message}") }
        }
        ticker?.cancel()
        ticker = scope.launch {
            while (true) {
                delay(200)
                feedEngine(engine.tick(SystemClock.elapsedRealtime()))
            }
        }
    }

    fun stop() {
        navigating = false
        ticker?.cancel()
        ticker = null
        engine.cancel()
        chosenPreviewRoute = null
        suppliedPreviewRoute = false
        // iOS stopNavigation(): return to the destination search state.
        _state.value = _state.value.copy(
            status = "已结束",
            route = null,
            instruction = "",
            selected = null,
            routeOptions = emptyList(),
            chosenOptionIndex = -1,
            navigationFailure = null,
            isNavigating = false,
            engineState = 0,
            remainingM = 0.0,
            remainingS = 0,
            previewOrigin = null,
        )
        log("导航已结束")
    }

    fun close() {
        stop()
        location.stop()
        engine.close()
    }

    /**
     * Debug harness only: search a destination and start navigation as soon as a
     * usable fix exists. Used by `adb shell am start ... --es motoSearch 西湖`.
     */
    fun debugSearchAndStart(query: String) {
        if (!BuildConfig.DEBUG) return
        log("调试入口：搜索并导航「$query」")
        if (lastFix != null) {
            autoStartAfterSearch = true
            search(query)
        } else {
            pendingDebugQuery = query
            log("调试入口等待定位")
        }
    }

    private fun feedEngine(result: EngineResult) {
        try {
            result.commands.forEach { command ->
                when {
                    command.isRequestRoute -> requestRoute(command)
                    command.type == EngineCommand.TYPE_REQUEST_TRAFFIC -> requestTraffic(command)
                    else -> log("暂不支持的命令 type=${command.type}")
                }
            }
            publish(result.snapshot)
        } catch (error: Throwable) {
            // A protocol/JNI rejection must degrade to a status line, not a crash.
            log("导航核心处理失败：${error.message}")
        }
    }

    /**
     * Traffic refresh reuses /v1/routes (the gateway has no traffic-only
     * endpoint) and feeds the bundle's segments back into NavCore.
     */
    private fun requestTraffic(command: EngineCommand) {
        val fix = lastFix ?: return
        val lon = destinationLon ?: return
        val lat = destinationLat ?: return
        val route = _state.value.route ?: return
        scope.launch {
            runCatching {
                gateway.planRoute(
                    command.requestId,
                    fix.longitude, fix.latitude, lon, lat,
                    false, route.routeId,
                )
            }.onSuccess { refreshed ->
                val segments = refreshed.traffic.map {
                    Triple(it.startOffsetM, it.endOffsetM, it.level)
                }
                runCatching {
                    feedEngine(
                        engine.acceptTraffic(
                            command.requestId, refreshed.routeId,
                            refreshed.totalDurationS.toInt(),
                            System.currentTimeMillis(), segments,
                        ),
                    )
                    _state.value = _state.value.copy(route = refreshed)
                }.onFailure { log("路况应用失败：${it.message}") }
            }.onFailure { log("路况刷新失败：${it.message}") }
        }
    }

    private fun requestRoute(command: EngineCommand) {
        val requestId = if (command.requestId != 0) command.requestId else ++requestCounter
        // First non-reroute request consumes the preview-selected candidate
        // (only when the planning origin has not drifted), matching iOS.
        if (!command.reroute && !suppliedPreviewRoute) {
            val chosen = chosenPreviewRoute
            val fix = lastFix
            val plannedLon = previewOriginLon
            val plannedLat = previewOriginLat
            if (chosen != null && fix != null && plannedLon != null && plannedLat != null) {
                val drift = haversineM(plannedLon, plannedLat, fix.longitude, fix.latitude)
                if (drift <= PREVIEW_ORIGIN_DRIFT_M) {
                    suppliedPreviewRoute = true
                    applyRoute(requestId, chosen, command)
                    return
                }
                log("起点已漂移 ${drift.roundToInt()} m，放弃预选路线并实时重算")
            }
            suppliedPreviewRoute = true
        }
        scope.launch {
            runCatching {
                val previous = _state.value.route?.routeId
                gateway.planRoute(
                    requestId,
                    command.originLon, command.originLat,
                    command.destinationLon, command.destinationLat,
                    command.reroute, previous,
                )
            }.onSuccess { route ->
                applyRoute(requestId, route, command)
            }.onFailure { error ->
                val retryable = (error as? GatewayException)?.error?.retryable ?: true
                runCatching {
                    feedEngine(engine.rejectRoute(requestId, retryable, System.currentTimeMillis()))
                }
                log("路线请求失败：${error.message}")
            }
        }
    }

    private fun applyRoute(requestId: Int, route: GatewayRoute, command: EngineCommand) {
        runCatching {
            val accepted = engine.acceptRoute(requestId, route, System.currentTimeMillis())
            _state.value = _state.value.copy(
                route = route,
                status = if (command.reroute) "偏航，正在重新规划" else "导航中",
                remainingM = route.totalDistanceM,
            )
            log(
                "路线已就绪：${(route.totalDistanceM / 1000).roundToInt()} km，" +
                    "${(route.totalDurationS / 60).roundToInt()} 分钟，${route.polyline.size} 点",
            )
            feedEngine(accepted)
            lastGeometryKey = null
        }.onFailure { error ->
            log("路线应用失败：${error.message}")
        }
    }

    private fun publish(snapshot: EngineSnapshot) {
        _state.value = _state.value.copy(
            engineState = snapshot.state,
            remainingM = snapshot.remainingDistanceM,
            remainingS = snapshot.remainingDurationS,
            instruction = snapshot.instruction,
            roadName = snapshot.roadName,
            distanceToManeuverM = snapshot.distanceToManeuverM,
            speedMps = snapshot.speedMps,
        )
        if (!navigating) return
        val now = SystemClock.elapsedRealtime()
        if (now - lastSnapshotAt < SNAPSHOT_INTERVAL_MS) return
        lastSnapshotAt = now
        sendSnapshot(snapshot)
        sendGeometryIfChanged(snapshot)
        maybeSendMapScene()
    }

    /**
     * Sends a bounded MapScene from cached tiles when the rider has moved
     * >100 m. Origin conversion WGS84 -> GCJ-02 happens exactly once, here,
     * because tile geometry is already GCJ-02.
     */
    private fun maybeSendMapScene() {
        if (!_state.value.bleReady || _state.value.route == null) return
        val fix = lastFix ?: return
        val now = SystemClock.elapsedRealtime()
        val moved = haversineM(lastSceneLon, lastSceneLat, fix.longitude, fix.latitude)
        if (lastSceneAtMs != 0L && moved < SCENE_MIN_MOVE_M && now - lastSceneAtMs < 30_000) return

        val gcj = NativeBridge.wgs84ToGcj02(fix.longitude, fix.latitude)
        if (gcj[2] == 0.0) return
        val tiles = runCatching {
            dev.glimpse.motogps.map.MapTilePlanner.aroundGcj02(gcj[1], gcj[0], SCENE_RADIUS_M.toDouble())
        }.getOrElse { return }
        val cached = tiles.map { tileStore.tileFile(it) }.filter { it.exists() }
        if (cached.isEmpty()) {
            logOnce("附近没有已缓存的离线瓦片，圆屏小地图暂不发送")
            return
        }
        mapSceneRevision += 1
        val scene = dev.glimpse.motogps.map.MapSceneBuilder.build(
            mapSceneRevision, gcj[0], gcj[1], SCENE_RADIUS_M, cached,
        ) ?: return
        if (central.sendMapScene(scene.revision, scene.encoded)) {
            lastSceneAtMs = now
            lastSceneLon = fix.longitude
            lastSceneLat = fix.latitude
            log(
                "已发送地图场景 rev=${scene.revision}（${scene.roads.size} 路 / " +
                    "${scene.buildings.size} 建筑，${cached.size} 瓦片）",
            )
        }
    }

    private fun haversineM(lon1: Double, lat1: Double, lon2: Double, lat2: Double): Double {
        if (lon1 == 0.0 && lat1 == 0.0) return Double.MAX_VALUE
        val r = 6_371_000.0
        val dLat = Math.toRadians(lat2 - lat1)
        val dLon = Math.toRadians(lon2 - lon1)
        val h = kotlin.math.sin(dLat / 2) * kotlin.math.sin(dLat / 2) +
            kotlin.math.cos(Math.toRadians(lat1)) * kotlin.math.cos(Math.toRadians(lat2)) *
            kotlin.math.sin(dLon / 2) * kotlin.math.sin(dLon / 2)
        return 2 * r * kotlin.math.asin(kotlin.math.sqrt(h))
    }

    private fun sendSnapshot(snapshot: EngineSnapshot, priority: Boolean = false) {
        val now = SystemClock.elapsedRealtime()
        val flags = (if (snapshot.hasDestination) 1L else 0L) or
            (if (snapshot.hasUsableFix) 2L else 0L) or
            (if (snapshot.gnssStale) 4L else 0L) or
            (if (snapshot.offRoute) 8L else 0L) or
            (if (snapshot.hasNextManeuver) 16L else 0L) or
            (if (snapshot.routeRequestInFlight) 32L else 0L) or
            (if (snapshot.trafficRequestInFlight) 64L else 0L) or
            (if (snapshot.hasRouteView) 128L else 0L)
        val numeric = longArrayOf(
            snapshot.state.toLong(),
            snapshot.network.toLong(),
            snapshot.displayPage.toLong(),
            snapshot.maneuverType.toLong(),
            snapshot.trafficAhead.toLong(),
            flags,
            if (snapshot.routeId.isEmpty()) 0L else NativeBridge.bleRouteToken(snapshot.routeId),
            snapshot.routeGeneration.toLong(),
            snapshot.maneuverId.toLong(),
            snapshot.distanceToManeuverM.roundToLong(),
            snapshot.remainingDistanceM.roundToLong(),
            snapshot.remainingDurationS.toLong(),
            snapshot.routeProgressM.roundToLong(),
            snapshot.totalDistanceM.roundToLong(),
            (snapshot.speedMps * 36.0).roundToLong().coerceIn(0, 65_535),
            snapshot.speedLimitKph.toLong().coerceIn(0, 65_535),
            (snapshot.headingDeg * 100.0).roundToLong().coerceIn(0, 65_535).let {
                ((it % 36_000) + 36_000) % 36_000
            },
            (snapshot.accuracyM * 10.0).roundToLong().coerceIn(0, 65_535),
            (snapshot.crossTrackM * 10.0).roundToLong().coerceIn(0, 65_535),
            snapshot.roundaboutExit.toLong(),
        )
        if (!central.sendNavigationSnapshot(
                numeric, snapshot.roadName, snapshot.instruction, priority = priority,
            )
        ) {
            if (!_state.value.bleReady) logOnce("圆屏未连接，快照未发送")
        } else if (now - lastSnapshotDiagAt > 5_000) {
            lastSnapshotDiagAt = now
            log(
                "快照诊断 state=${snapshot.state} page=${snapshot.displayPage} flags=$flags " +
                    "heading=${(snapshot.headingDeg * 100).roundToLong()} " +
                    "token=${numeric[6]} gen=${numeric[7]} " +
                    "dist=${numeric[9]} remain=${numeric[10]}",
            )
        }
    }

    private var lastBleWarning = 0L
    private fun logOnce(message: String) {
        val now = SystemClock.elapsedRealtime()
        if (now - lastBleWarning > 5_000) {
            lastBleWarning = now
            log(message)
        }
    }

    private fun sendGeometryIfChanged(snapshot: EngineSnapshot) {
        if (!snapshot.hasRouteView || snapshot.routeViewPoints.size < 2) return
        val key = "${snapshot.routeId}/${snapshot.routeGeneration}/${snapshot.routeViewPoints.size}"
        if (key == lastGeometryKey) return
        lastGeometryKey = key
        val token = NativeBridge.bleRouteToken(snapshot.routeId)
        val points = snapshot.routeViewPoints
        val chunkCount = ((points.size + MAX_POINTS_PER_CHUNK - 1) / MAX_POINTS_PER_CHUNK)
        var index = 0
        var chunk = 0
        while (index < points.size) {
            val end = minOf(index + MAX_POINTS_PER_CHUNK, points.size)
            val slice = points.subList(index, end)
            val origin = slice.first()
            val flat = DoubleArray(slice.size * 2)
            slice.forEachIndexed { i, (lon, lat) ->
                flat[i * 2] = lon
                flat[i * 2 + 1] = lat
            }
            val meta = longArrayOf(
                2L, // GCJ-02
                token,
                snapshot.routeGeneration.toLong(),
                chunk.toLong(),
                chunkCount.toLong(),
                index.toLong(),
                points.size.toLong(),
                (origin.second * 1e6).roundToLong(),
                (origin.first * 1e6).roundToLong(),
            )
            central.sendRouteGeometry(meta, flat)
            index = end
            chunk += 1
        }
        log("已发送路线几何 ${points.size} 点 / $chunkCount 片")
    }

    private fun log(line: String) {
        Log.i(TAG, line)
        val stamped = "${java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US).format(java.util.Date())}  $line"
        _state.value = _state.value.copy(log = (_state.value.log + stamped).takeLast(60))
    }

    companion object {
        const val TAG = "MotoGpsNav"
        const val SNAPSHOT_INTERVAL_MS = 200L
        const val MAX_POINTS_PER_CHUNK = 24
        const val SCENE_RADIUS_M = 500
        const val SCENE_MIN_MOVE_M = 100.0
        const val COMMAND_PAGE_SELECTED = 0
        const val COMMAND_SWIPE_LEFT = 3
        const val COMMAND_SWIPE_RIGHT = 4
        const val COMMAND_MUSIC_PREVIOUS = 16
        const val COMMAND_MUSIC_TOGGLE = 17
        const val COMMAND_MUSIC_NEXT = 18
        const val COMMAND_MUSIC_LIKE = 19
        /** Delay before the second page echo, in case the first frame is lost. */
        const val PAGE_ECHO_MS = 90L
        const val PREVIEW_ORIGIN_DRIFT_M = 120.0
        // 0 navigation, 1 speed, 2 compass, 3 music (remote control only).
        val SUPPORTED_PAGES = listOf(0, 1, 2, 3)

        /** iOS RoutePreviewCandidate.title */
        fun optionTitle(index: Int): String = when (index) {
            0 -> "推荐路线"
            1 -> "备选路线一"
            else -> "备选路线二"
        }

        fun optionDistanceText(totalDistanceM: Double): String =
            if (totalDistanceM >= 1_000) {
                String.format(java.util.Locale.US, "%.1f km", totalDistanceM / 1_000)
            } else {
                "${totalDistanceM.roundToInt()} m"
            }

        fun optionDurationText(totalDurationS: Double): String {
            val minutes = maxOf(1, Math.round(totalDurationS / 60.0).toInt())
            return if (minutes >= 60) "${minutes / 60} 小时 ${minutes % 60} 分" else "$minutes 分钟"
        }

        /** iOS trafficSummary(): weighted slow/congested share over the route. */
        fun optionTrafficSummary(route: GatewayRoute): String {
            if (route.traffic.isEmpty() || route.totalDistanceM <= 0) return "暂无分段路况"
            var slow = 0.0
            var congested = 0.0
            route.traffic.forEach { segment ->
                val length = maxOf(0.0, segment.endOffsetM - segment.startOffsetM)
                when (segment.level) {
                    2 -> slow += length
                    3, 4 -> congested += length
                }
            }
            val congestedShare = congested / route.totalDistanceM
            val affectedShare = (slow + congested) / route.totalDistanceM
            return when {
                congestedShare >= 0.12 -> "拥堵较多"
                affectedShare >= 0.18 -> "部分路段缓行"
                else -> "路况顺畅"
            }
        }

        fun formatDistance(meters: Double): String =
            if (meters >= 1_000) {
                String.format(java.util.Locale.US, "%.1f km", meters / 1_000)
            } else {
                "${meters.roundToInt()} m"
            }

        fun formatDuration(seconds: Int): String = when {
            seconds <= 0 -> "--"
            seconds < 3_600 -> "${maxOf(1, seconds / 60)} 分钟"
            else -> "${seconds / 3_600} 时 ${(seconds % 3_600) / 60} 分"
        }
    }
}
