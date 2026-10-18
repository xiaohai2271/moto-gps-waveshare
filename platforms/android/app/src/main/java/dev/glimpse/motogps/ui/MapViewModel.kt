package dev.glimpse.motogps.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import dev.glimpse.motogps.MotoGpsApplication
import dev.glimpse.motogps.gateway.CityArea
import dev.glimpse.motogps.gateway.GatewayClient
import dev.glimpse.motogps.map.MapTilePlanner
import dev.glimpse.motogps.map.MapTileStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MapViewModel(application: Application) : AndroidViewModel(application) {
    data class UiState(
        val cities: List<CityArea> = emptyList(),
        val packs: List<MapTileStore.Pack> = emptyList(),
        val message: String = "离线地图只包含背景道路与建筑，不提供离线路线规划。",
        val downloading: Boolean = false,
        val progress: MapTileStore.Progress? = null,
        val cachedBytes: Long = 0,
        val plannedTiles: Int = 0,
    )

    private val gateway = GatewayClient()
    private val store = MapTileStore(application, gateway)
    private val _state = MutableStateFlow(
        UiState(packs = store.packs(), cachedBytes = store.cachedBytes()),
    )
    val state: StateFlow<UiState> = _state.asStateFlow()

    fun searchCities(keywords: String) {
        viewModelScope.launch {
            runCatching { gateway.searchCities(keywords) }
                .onSuccess { cities ->
                    val planned = cities.firstOrNull()?.let { city ->
                        runCatching {
                            MapTilePlanner.boundsWgs84(
                                city.west, city.south, city.east, city.north,
                            ).size
                        }.getOrElse { 0 }
                    } ?: 0
                    _state.value = _state.value.copy(
                        cities = cities,
                        plannedTiles = planned,
                        message = if (cities.isEmpty()) "没有匹配的城市或区县" else "找到 ${cities.size} 个下载范围",
                    )
                }
                .onFailure { _state.value = _state.value.copy(message = "城市搜索失败：${it.message}") }
        }
    }

    fun download(city: CityArea) {
        if (_state.value.downloading) return
        viewModelScope.launch {
            _state.value = _state.value.copy(downloading = true, message = "正在规划 ${city.name} 的瓦片…")
            val tiles = runCatching {
                withContext(Dispatchers.Default) {
                    MapTilePlanner.boundsWgs84(city.west, city.south, city.east, city.north)
                }
            }.getOrElse {
                _state.value = _state.value.copy(
                    downloading = false,
                    message = "范围过大或坐标非法：${it.message}",
                )
                return@launch
            }
            _state.value = _state.value.copy(message = "共 ${tiles.size} 个瓦片，开始下载…")
            runCatching {
                store.download(city.name, city.detail, tiles) { progress ->
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
    }

    fun delete(pack: MapTileStore.Pack) {
        store.deletePack(pack.id)
        _state.value = _state.value.copy(
            packs = store.packs(),
            cachedBytes = store.cachedBytes(),
            message = "已删除 ${pack.name}",
        )
    }
}
