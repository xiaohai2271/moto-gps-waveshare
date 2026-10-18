package dev.glimpse.motogps

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class GatewayConfigTest {
    @Test
    fun defaultBaseUrlMatchesIosProjectYml() {
        // Must equal MOTOGPSGatewayBaseURL in platforms/ios/project.yml.
        assertEquals("https://nav.example.com/moto-gps/api/", GatewayConfig.DEFAULT_BASE_URL)
        assertTrue(GatewayConfig.validate(GatewayConfig.DEFAULT_BASE_URL) is GatewayConfig.Validation.Ok)
    }

    @Test
    fun normalizeAddsExactlyOneTrailingSlash() {
        assertEquals("https://a.example/", GatewayConfig.normalize("  https://a.example  "))
        assertEquals("https://a.example/", GatewayConfig.normalize("https://a.example//"))
        assertEquals("", GatewayConfig.normalize("   "))
    }

    @Test
    fun rejectsPlainHttpOnRemoteHosts() {
        val result = GatewayConfig.validate("http://nav.example.com/moto-gps/api/")
        assertTrue(result is GatewayConfig.Validation.Invalid)
    }

    @Test
    fun allowsHttpOnLoopbackOnly() {
        assertTrue(GatewayConfig.validate("http://127.0.0.1:8787/") is GatewayConfig.Validation.Ok)
        assertTrue(GatewayConfig.validate("http://localhost:8787/") is GatewayConfig.Validation.Ok)
    }

    @Test
    fun resolvesEndpointsWithoutDoubleSlashes() {
        val base = GatewayConfig.DEFAULT_BASE_URL
        assertEquals("https://nav.example.com/moto-gps/api/v1/routes", GatewayConfig.resolve(base, "v1/routes"))
        assertEquals("https://nav.example.com/moto-gps/api/v1/places", GatewayConfig.resolve(base, "/v1/places"))
    }
}
