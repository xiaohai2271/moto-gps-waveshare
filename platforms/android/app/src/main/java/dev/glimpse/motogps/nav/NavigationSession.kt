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
    private val gateway: GatewayClient = GatewayClient(),
) {
    data class UiState(
        val engineState: Int = 0,
        val status: String = "未开始",
        val hasFix: Boolean = false,
        val accuracyM: Double = 0.0,
        val places: List<Place> = emptyList(),
        val selected: Place? = null,
        val route: GatewayRoute? = null,
        val remainingM: Double = 0.0,
        val remainingS: Int = 0,
        val instruction: String = "",
        val roadName: String = "",
        val distanceToManeuverM: Double = 0.0,
        val speedMps: Double = 0.0,
        val bleReady: Boolean = false,
        val log: List<String> = emptyList(),
    )

    private val engine = NavEngine()
    private val location = GpsLocationSource(context)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    private var lastFix: GpsLocationSource.Fix? = null
    private var navigating = false
    private var ticker: Job? = null
    private var lastSnapshotAt = 0L
    private var lastGeometryKey: String? = null
    private var bleWasReady = false
    private var requestCounter = 0
    private var hasLoggedFix = false

    // Debug-only harness (BuildConfig.DEBUG): lets adb start a live navigation
    // without touch input. Never enabled in release builds.
    private var pendingDebugQuery: String? = null
    private var autoStartAfterSearch = false

    init {
        location.start(
            onFix = { fix ->
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
                            fix.longitude, fix.latitude, fix.accuracyM,
                            fix.speedMps, fix.headingDeg, fix.timestampMs,
                        ),
                    )
                }
            },
            onFailure = { reason -> log("定位不可用：$reason") },
        )
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

    fun search(keywords: String) {
        scope.launch {
            runCatching {
                val fix = lastFix
                gateway.searchPlaces(keywords, fix?.let { it.longitude to it.latitude })
            }.onSuccess { places ->
                _state.value = _state.value.copy(places = places)
                log("搜索到 ${places.size} 个地点")
                if (autoStartAfterSearch) {
                    autoStartAfterSearch = false
                    places.firstOrNull { it.isWgs84 }?.let { place ->
                        select(place)
                        start()
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
        _state.value = _state.value.copy(selected = place)
        log("已选择：${place.name}")
    }

    fun start() {
        val place = _state.value.selected ?: run {
            log("请先选择目的地")
            return
        }
        val fix = lastFix ?: run {
            log("等待定位后再开始导航")
            return
        }
        navigating = true
        _state.value = _state.value.copy(status = "规划中", places = emptyList())
        log("开始导航：${place.name}")
        val result = engine.beginNavigation(place.longitude, place.latitude)
        feedEngine(result)
        scope.launch {
            engine.setNetwork(2) // online
            feedEngine(engine.pushFix(fix.longitude, fix.latitude, fix.accuracyM, fix.speedMps, fix.headingDeg, fix.timestampMs))
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
        _state.value = _state.value.copy(status = "已结束", route = null, instruction = "")
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
                if (command.isRequestRoute) requestRoute(command) else {
                    log("暂不支持的命令 type=${command.type}")
                }
            }
            publish(result.snapshot)
        } catch (error: Throwable) {
            // A protocol/JNI rejection must degrade to a status line, not a crash.
            log("导航核心处理失败：${error.message}")
        }
    }

    private fun requestRoute(command: EngineCommand) {
        val requestId = if (command.requestId != 0) command.requestId else ++requestCounter
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
                runCatching {
                    val accepted = engine.acceptRoute(requestId, route, System.currentTimeMillis())
                    _state.value = _state.value.copy(
                        route = route,
                        status = if (command.reroute) "已重新规划" else "导航中",
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
            }.onFailure { error ->
                val retryable = (error as? GatewayException)?.error?.retryable ?: true
                runCatching {
                    feedEngine(engine.rejectRoute(requestId, retryable, System.currentTimeMillis()))
                }
                log("路线请求失败：${error.message}")
            }
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
    }

    private fun sendSnapshot(snapshot: EngineSnapshot) {
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
        if (!central.sendNavigationSnapshot(numeric, snapshot.roadName, snapshot.instruction)) {
            if (!_state.value.bleReady) logOnce("圆屏未连接，快照未发送")
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

    private companion object {
        const val TAG = "MotoGpsNav"
        const val SNAPSHOT_INTERVAL_MS = 200L
        const val MAX_POINTS_PER_CHUNK = 24
    }
}
