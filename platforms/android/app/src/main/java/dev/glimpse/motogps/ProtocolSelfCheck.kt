package dev.glimpse.motogps

/**
 * Runs the upstream wire-format contract check on the device: encodes the
 * golden fixtures with the shared C++ implementation (via JNI) and compares
 * them with the fixture shipped in assets.
 */
object ProtocolSelfCheck {
    data class Line(val key: String, val ok: Boolean, val detail: String)

    data class Report(
        val nativeLoaded: Boolean,
        val lines: List<Line>,
    ) {
        val passed: Boolean get() = nativeLoaded && lines.isNotEmpty() && lines.all { it.ok }
    }

    private const val FIXTURE = "ble-navigation-v1.golden.txt"

    fun expectedFixture(): Map<String, String> {
        // Loaded from assets by the caller on Android (see MainActivity).
        return emptyMap()
    }

    fun parse(text: String): Map<String, String> = text.lines()
        .filter { it.contains("=") && !it.trimStart().startsWith("#") }
        .associate {
            val (k, v) = it.split("=", limit = 2)
            k.trim() to v.trim()
        }

    fun run(expected: Map<String, String>, nativeDump: String): Report {
        val nativeLoaded = runCatching { nativeDump.isNotEmpty() }.getOrDefault(false)
        if (!nativeLoaded) {
            return Report(false, emptyList())
        }
        val actual = parse(nativeDump)
        val lines = expected.map { (key, value) ->
            val got = actual[key]
            Line(key, got == value, if (got == null) "missing" else "match")
        }
        return Report(true, lines)
    }

    const val ASSET_NAME = FIXTURE
}
