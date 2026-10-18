package dev.glimpse.motogps.nav

import android.content.Context
import dev.glimpse.motogps.GatewayConfig

/**
 * Persisted gateway base URL. Mirrors the iOS GatewayConfiguration: empty means
 * "use the bundled default", saving validates https/loopback and normalisation.
 */
class GatewaySettingsStore(context: Context) {
    private val prefs = context.getSharedPreferences("gateway_settings", Context.MODE_PRIVATE)

    fun current(): String {
        val saved = prefs.getString(KEY, null)
        return if (saved.isNullOrEmpty()) GatewayConfig.DEFAULT_BASE_URL else saved
    }

    fun isCustomised(): Boolean = !prefs.getString(KEY, null).isNullOrEmpty()

    /** @return the normalised URL on success, or an error message. */
    fun save(raw: String): String {
        val trimmed = raw.trim()
        require(trimmed.isNotEmpty()) { "请填写网关地址" }
        return when (val result = GatewayConfig.validate(trimmed)) {
            is GatewayConfig.Validation.Ok -> {
                prefs.edit().putString(KEY, result.baseUrl).apply()
                result.baseUrl
            }
            is GatewayConfig.Validation.Invalid ->
                throw IllegalArgumentException("地址无效：${result.reason}（仅支持 https 或本机回环）")
        }
    }

    fun reset() {
        prefs.edit().remove(KEY).apply()
    }

    private companion object {
        const val KEY = "gateway_base_url"
    }
}
