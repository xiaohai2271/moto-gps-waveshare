package dev.glimpse.motogps

import java.net.URI

/**
 * Gateway base URL handling shared by every HTTP adapter.
 *
 * Mirrors the iOS behaviour (platforms/ios/App/AppConfiguration.swift +
 * AmapGatewayRouteProvider): the built-in address can be overridden by the
 * user, and only `https` or a loopback host may be used. The bundled default
 * must stay in sync with `MOTOGPSGatewayBaseURL` in platforms/ios/project.yml.
 */
object GatewayConfig {
    /**
     * Keep this value identical to the iOS Info.plist entry
     * `MOTOGPSGatewayBaseURL: https://moto.menghuan.life/` in
     * platforms/ios/project.yml.
     */
    const val DEFAULT_BASE_URL = "https://moto.menghuan.life/"

    sealed interface Validation {
        data class Ok(val baseUrl: String) : Validation
        data class Invalid(val reason: String) : Validation
    }

    /** Trims whitespace and guarantees exactly one trailing slash. */
    fun normalize(raw: String): String {
        val trimmed = raw.trim().trimEnd('/')
        if (trimmed.isEmpty()) return ""
        return "$trimmed/"
    }

    fun validate(raw: String): Validation {
        val candidate = normalize(raw)
        if (candidate.isEmpty()) {
            return Validation.Invalid("empty")
        }
        val uri = runCatching { URI(candidate) }.getOrNull()
            ?: return Validation.Invalid("malformed")
        if (uri.scheme != "https" && uri.host !in LOOPBACK_HOSTS) {
            return Validation.Invalid("https or loopback required")
        }
        if (uri.host.isNullOrBlank()) {
            return Validation.Invalid("missing host")
        }
        return Validation.Ok(candidate)
    }

    /** Resolves an endpoint such as `v1/routes` against a validated base URL. */
    fun resolve(baseUrl: String, path: String): String =
        normalize(baseUrl) + path.trimStart('/')

    private val LOOPBACK_HOSTS = setOf("127.0.0.1", "localhost")
}
