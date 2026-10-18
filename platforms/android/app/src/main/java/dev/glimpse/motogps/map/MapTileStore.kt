package dev.glimpse.motogps.map

import android.content.Context
import dev.glimpse.motogps.gateway.GatewayClient
import dev.glimpse.motogps.gateway.GatewayJson
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.UUID

/**
 * Disk cache for z15 map tiles plus named download packs.
 *
 * Layout: map/tiles/15-x-y.json, map/packs.json. These packs contain background
 * roads/buildings only; they are not an offline routing engine.
 */
class MapTileStore(
    context: Context,
    private val gateway: GatewayClient = GatewayClient(),
) {
    data class Pack(
        val id: String,
        val name: String,
        val detail: String,
        val tiles: List<MapTilePlanner.TileId>,
        val bytes: Long,
    )

    data class Progress(val done: Int, val total: Int, val failed: Int, val bytes: Long) {
        val fraction: Float get() = if (total == 0) 0f else done.toFloat() / total
    }

    private val root = File(context.filesDir, "map").apply { mkdirs() }
    private val tileDir = File(root, "tiles").apply { mkdirs() }
    private val manifest = File(root, "packs.json")

    fun tileFile(tile: MapTilePlanner.TileId): File =
        File(tileDir, "${tile.z}-${tile.x}-${tile.y}.json")

    fun cachedBytes(): Long = tileDir.listFiles()?.sumOf { it.length() } ?: 0L

    fun packs(): List<Pack> = readManifest()

    /**
     * Downloads [tiles] in batches; already cached tiles are reused, so an
     * interrupted download can resume. Aborts when the pinned budget is hit.
     */
    suspend fun download(
        name: String,
        detail: String,
        tiles: List<MapTilePlanner.TileId>,
        onProgress: (Progress) -> Unit = {},
    ): Pack {
        require(tiles.isNotEmpty()) { "no tiles to download" }
        val gate = Semaphore(4)
        var done = 0
        var failed = 0
        var bytes = 0L
        val downloaded = mutableListOf<MapTilePlanner.TileId>()

        coroutineScope {
            tiles.chunked(4).forEach { batch ->
                val results = batch.map { tile ->
                    async {
                        gate.withPermit {
                            delay(PACING_MS)
                            if (tileFile(tile).exists()) true else fetchWithRetry(tile)
                        } to tile
                    }
                }.awaitAll()
                results.forEach { (ok, tile) ->
                    done += 1
                    if (ok) {
                        bytes += tileFile(tile).length()
                        downloaded += tile
                    } else {
                        failed += 1
                    }
                }
                onProgress(Progress(done, tiles.size, failed, bytes))
                if (cachedBytes() > PINNED_BUDGET_BYTES) {
                    throw IllegalStateException("离线地图超出 512 MiB 预算，请先删除旧离线包")
                }
            }
        }

        val pack = Pack(
            id = UUID.randomUUID().toString(),
            name = name.take(120),
            detail = detail.take(400),
            tiles = downloaded,
            bytes = downloaded.sumOf { tileFile(it).length() },
        )
        writeManifest(readManifest() + pack)
        return pack
    }

    fun deletePack(id: String): Boolean {
        val packs = readManifest()
        val pack = packs.firstOrNull { it.id == id } ?: return false
        val referenced = packs.filter { it.id != id }.flatMap { it.tiles }.toSet()
        pack.tiles.filterNot { it in referenced }.forEach { tileFile(it).delete() }
        writeManifest(packs.filter { it.id != id })
        return true
    }

    private suspend fun fetchWithRetry(tile: MapTilePlanner.TileId): Boolean {
        repeat(MAX_ATTEMPTS) { attempt ->
            val body = runCatching { gateway.fetchTile(tile) }
                .onFailure {
                    android.util.Log.w("MotoGpsMap", "tile $tile attempt ${attempt + 1}: ${it.message}")
                }
                .getOrNull()
            if (body != null && body.toByteArray().size <= MAX_TILE_BYTES) {
                val valid = runCatching {
                    GatewayJson.validateTile(body, tile.z, tile.x, tile.y)
                }.onFailure {
                    android.util.Log.w("MotoGpsMap", "tile $tile invalid: ${it.message}")
                }.isSuccess
                if (valid) {
                    tileFile(tile).writeText(body)
                    return true
                }
            } else if (body != null) {
                android.util.Log.w("MotoGpsMap", "tile $tile too large: ${body.length}")
            }
            delay(1_000L * (attempt + 1))
        }
        return false
    }

    private fun readManifest(): List<Pack> {
        if (!manifest.exists()) return emptyList()
        return runCatching {
            val array = JSONArray(manifest.readText())
            List(array.length()) { i ->
                val o = array.getJSONObject(i)
                val tiles = o.getJSONArray("tiles")
                Pack(
                    id = o.getString("id"),
                    name = o.getString("name"),
                    detail = o.optString("detail", ""),
                    tiles = List(tiles.length()) { j ->
                        val t = tiles.getJSONArray(j)
                        MapTilePlanner.TileId(t.getInt(0), t.getInt(1), t.getInt(2))
                    },
                    bytes = o.optLong("bytes", 0L),
                )
            }
        }.getOrElse {
            runCatching {
                manifest.renameTo(File(root, "packs.corrupt-${System.currentTimeMillis()}.json"))
            }
            emptyList()
        }
    }

    private fun writeManifest(packs: List<Pack>) {
        val array = JSONArray()
        packs.forEach { pack ->
            val o = JSONObject()
            o.put("id", pack.id)
            o.put("name", pack.name)
            o.put("detail", pack.detail)
            o.put("bytes", pack.bytes)
            val tiles = JSONArray()
            pack.tiles.forEach { tiles.put(JSONArray(listOf(it.z, it.x, it.y))) }
            o.put("tiles", tiles)
            array.put(o)
        }
        val tmp = File(root, "packs.json.tmp")
        tmp.writeText(array.toString())
        tmp.renameTo(manifest)
    }

    private companion object {
        const val PACING_MS = 250L
        const val MAX_ATTEMPTS = 3
        const val MAX_TILE_BYTES = 8 * 1024 * 1024
        const val PINNED_BUDGET_BYTES = 512L * 1024 * 1024
    }
}
