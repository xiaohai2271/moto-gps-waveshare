package dev.glimpse.motogps.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import dev.glimpse.motogps.MotoGpsApplication
import dev.glimpse.motogps.gateway.CityArea
import dev.glimpse.motogps.map.MapTilePlanner
import dev.glimpse.motogps.map.MapTileStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Offline map downloads, ported from iOS MapDownloadsView / MapDownloadsViewModel:
 * a city or district search with a 350 ms debounce, a route-corridor download for
 * the currently selected route, plus saved packs and a temporary-cache cleanup.
 *
 * All requests use the gateway configured in the app (never a second default
 * client), so switching the gateway immediately affects downloads too.
 */
class MapDownloadsViewModel(application: Application) : AndroidViewModel(application) {
    data class UiState(
        val query: String = "",
        val cities: List<CityArea> = emptyList(),
        val searching: Boolean = false,
        val searchFailure: String? = null,
        val packs: List<MapTileStore.Pack> = emptyList(),
        val cachedBytes: Long = 0L,
        val downloading: Boolean = false,
        val progress: MapTileStore.Progress? = null,
        val message: String? = null,
        val routeName: String? = null,
        val routeAvailable: Boolean = false,
    )

    private val session = (application as MotoGpsApplication).navigationSession
    private val store = session.mapTiles()
    private val _state = MutableStateFlow(
        UiState(packs = store.packs(), cachedBytes = store.cachedBytes()),
    )
    val state: StateFlow<UiState> = _state.asStateFlow()
    private var searchJob: Job? = null

    /** Refreshes the route-corridor availability when the sheet opens. */
    fun refresh() {
        val route = session.routePolylineForDownload()
        _state.value = _state.value.copy(
            packs = store.packs(),
            cachedBytes = store.cachedBytes(),
            routeAvailable = route.size >= 2,
            routeName = session.state.value.selected?.name
                ?: session.state.value.route?.let { "当前路线" },
        )
    }

    fun queryChanged(value: String) {
        _state.value = _state.value.copy(query = value, searchFailure = null)
        searchJob?.cancel()
        val keywords = value.trim()
        if (keywords.length < 2) {
            _state.value = _state.value.copy(cities = emptyList(), searching = false)
            return
        }
        searchJob = viewModelScope.launch {
            delay(350)
            searchCitiesWithRetry(keywords)
        }
    }

    /**
     * One free retry: the first request after a cold start or a network switch
     * often fails while the radio comes up, and a silent "network error" for a
     * searchable city looks like a broken feature.
     */
    private suspend fun searchCitiesWithRetry(keywords: String, attempts: Int = 2) {
        _state.value = _state.value.copy(searching = true, searchFailure = null)
        var last: Throwable? = null
        repeat(attempts) { index ->
            val result = runCatching { session.gatewayClient().searchCities(keywords) }
            result.onSuccess { cities ->
                _state.value = _state.value.copy(
                    cities = cities.take(30),
                    searching = false,
                    searchFailure = if (cities.isEmpty()) "未找到城市或区县" else null,
                )
                return
            }
            last = result.exceptionOrNull()
            android.util.Log.w("MotoGpsMap", "city search attempt ${index + 1}: ${last?.message}")
            if (index < attempts - 1) delay(900)
        }
        _state.value = _state.value.copy(
            searching = false,
            cities = emptyList(),
            searchFailure = "暂时无法搜索城市（${last?.message ?: "网络错误"}），请重试。",
        )
    }

    /** Re-runs the last city search (iOS shows a 重试 button after a failure). */
    fun retrySearch() {
        val keywords = _state.value.query.trim()
        if (keywords.length < 2) return
        _state.value = _state.value.copy(searchFailure = null)
        queryChanged(keywords)
    }

    fun downloadCity(city: CityArea) {
        if (_state.value.downloading) return
        viewModelScope.launch {
            _state.value = _state.value.copy(downloading = true, message = "正在计算 ${city.name} 的范围…")
            val tiles = runCatching {
                withContext(Dispatchers.Default) {
                    MapTilePlanner.boundsWgs84(city.west, city.south, city.east, city.north)
                }
            }.getOrElse { error ->
                _state.value = _state.value.copy(
                    downloading = false,
                    message = "范围过大或坐标非法：${error.message}",
                )
                return@launch
            }
            // A whole prefecture at z15 is tens of thousands of tiles; the
            // gateway would need hours and the UI would look frozen. Ask for a
            // district instead, matching the iOS guidance text.
            if (tiles.size > MAX_CITY_TILES) {
                _state.value = _state.value.copy(
                    downloading = false,
                    message = "范围过大（约 ${tiles.size} 个瓦片），请搜索具体区县后再下载。",
                )
                return@launch
            }
            download(city.name, city.detail, tiles)
        }
    }

    fun downloadRoute() {
        if (_state.value.downloading) return
        val points = session.routePolylineForDownload()
        if (points.size < 2) {
            _state.value = _state.value.copy(message = "请先规划并选择一条路线")
            return
        }
        viewModelScope.launch {
            _state.value = _state.value.copy(downloading = true, message = "正在计算沿途下载范围…")
            val tiles = runCatching {
                withContext(Dispatchers.Default) { MapTilePlanner.corridorGcj02(points) }
            }.getOrElse { error ->
                _state.value = _state.value.copy(
                    downloading = false,
                    message = "沿途范围计算失败：${error.message}",
                )
                return@launch
            }
            val name = "前往${_state.value.routeName ?: "目的地"}"
            download(name, "所选路线及沿途约一公里", tiles)
        }
    }

    private suspend fun download(name: String, detail: String, tiles: List<MapTilePlanner.TileId>) {
        _state.value = _state.value.copy(message = "共 ${tiles.size} 个瓦片，开始下载…")
        runCatching {
            store.download(name, detail, tiles) { progress ->
                _state.value = _state.value.copy(progress = progress)
            }
        }.onSuccess { pack ->
            _state.value = _state.value.copy(
                downloading = false,
                progress = null,
                packs = store.packs(),
                cachedBytes = store.cachedBytes(),
                message = "已下载 ${pack.name}（${pack.tiles.size} 个瓦片）",
            )
        }.onFailure { error ->
            _state.value = _state.value.copy(
                downloading = false,
                progress = null,
                packs = store.packs(),
                cachedBytes = store.cachedBytes(),
                message = "下载中断：${error.message}（已下载的瓦片保留，可重试续传）",
            )
        }
    }

    fun delete(pack: MapTileStore.Pack) {
        if (_state.value.downloading) return
        store.deletePack(pack.id)
        _state.value = _state.value.copy(
            packs = store.packs(),
            cachedBytes = store.cachedBytes(),
            message = "已删除 ${pack.name}",
        )
    }

    fun clearTemporaryCache() {
        if (_state.value.downloading) return
        val freed = store.clearTemporaryCache()
        _state.value = _state.value.copy(
            packs = store.packs(),
            cachedBytes = store.cachedBytes(),
            message = "已清理 ${freed / 1024} KiB 自动缓存",
        )
    }

    fun clearMessage() {
        _state.value = _state.value.copy(message = null)
    }

    companion object {
        /** Above this the download would take hours; ask for a district instead. */
        const val MAX_CITY_TILES = 4_000
    }
}
