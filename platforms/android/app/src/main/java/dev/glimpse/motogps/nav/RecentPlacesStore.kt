package dev.glimpse.motogps.nav

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

/** Recently selected destinations, persisted like the iOS app (max 8). */
class RecentPlacesStore(context: Context) {
    private val prefs = context.getSharedPreferences("recent_places", Context.MODE_PRIVATE)

    fun load(): List<SavedPlace> = runCatching {
        val raw = prefs.getString(KEY, null) ?: return emptyList()
        val array = JSONArray(raw)
        List(array.length()) { i ->
            val o = array.getJSONObject(i)
            SavedPlace(
                id = o.getString("id"),
                name = o.getString("name"),
                address = o.optString("address", ""),
                city = o.optString("city", ""),
                district = o.optString("district", ""),
                displayArea = o.optString("displayArea", ""),
                longitude = o.getDouble("lon"),
                latitude = o.getDouble("lat"),
            )
        }
    }.getOrElse { emptyList() }

    fun record(place: SavedPlace): List<SavedPlace> {
        val others = load().filterNot { it.identity == place.identity }
        val updated = (listOf(place) + others).take(8)
        save(updated)
        return updated
    }

    fun clear(): List<SavedPlace> {
        prefs.edit().remove(KEY).apply()
        return emptyList()
    }

    private fun save(places: List<SavedPlace>) {
        val array = JSONArray()
        places.forEach { p ->
            array.put(
                JSONObject()
                    .put("id", p.id)
                    .put("name", p.name)
                    .put("address", p.address)
                    .put("city", p.city)
                    .put("district", p.district)
                    .put("displayArea", p.displayArea)
                    .put("lon", p.longitude)
                    .put("lat", p.latitude),
            )
        }
        prefs.edit().putString(KEY, array.toString()).apply()
    }

    private companion object {
        const val KEY = "recent_places_v1"
    }
}

data class SavedPlace(
    val id: String,
    val name: String,
    val address: String,
    val city: String,
    val district: String,
    val displayArea: String,
    val longitude: Double,
    val latitude: Double,
) {
    val identity: String get() = "$id|$name"

    fun subtitle(): String {
        val area = displayArea.ifEmpty { listOf(city, district).filter { it.isNotEmpty() }.joinToString(" · ") }
        return listOf(area, address).filter { it.isNotEmpty() }.joinToString(" · ")
            .ifEmpty { "查看路线" }
    }

    companion object {
        fun fromPlace(place: dev.glimpse.motogps.gateway.Place): SavedPlace = SavedPlace(
            id = place.id,
            name = place.name,
            address = place.address,
            city = place.city,
            district = place.district,
            displayArea = place.displayArea,
            longitude = place.longitude,
            latitude = place.latitude,
        )
    }
}
