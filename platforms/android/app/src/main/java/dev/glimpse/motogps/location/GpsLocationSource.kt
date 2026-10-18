package dev.glimpse.motogps.location

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Bundle
import android.os.Looper
import androidx.core.content.ContextCompat

/**
 * Multi-provider location source built on the platform LocationManager, so it
 * works on devices without Google Play Services.
 *
 * GPS updates arrive every second; network updates provide an indoor fallback
 * with coarser accuracy. A fix is only delivered when it is fresh (<=15 s) and
 * accurate enough for navigation; consumers see WGS84 coordinates only.
 */
class GpsLocationSource(private val context: Context) {

    data class Fix(
        val longitude: Double,
        val latitude: Double,
        val accuracyM: Double,
        val speedMps: Double,
        val headingDeg: Double,
        val timestampMs: Long,
        val provider: String,
    )

    fun interface Listener {
        fun onFix(fix: Fix)
    }

    private val manager =
        context.getSystemService(Context.LOCATION_SERVICE) as? LocationManager

    private var callback: Listener? = null
    private var failure: ((String) -> Unit)? = null
    private var lastDeliveredAtMs = 0L
    private var lastProvider: String? = null

    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            toFix(location)?.let { fix ->
                // Prefer GPS; do not let a coarse network ping overwrite a recent
                // high-accuracy GPS fix.
                val now = System.currentTimeMillis()
                if (lastProvider == LocationManager.GPS_PROVIDER &&
                    fix.provider != LocationManager.GPS_PROVIDER &&
                    now - lastDeliveredAtMs < 10_000
                ) {
                    return
                }
                lastProvider = fix.provider
                lastDeliveredAtMs = now
                callback?.onFix(fix)
            }
        }

        @Deprecated("Deprecated in Java")
        override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) = Unit
        override fun onProviderEnabled(provider: String) = Unit
        override fun onProviderDisabled(provider: String) {
            if (provider == LocationManager.GPS_PROVIDER) {
                failure?.invoke("GPS 关闭，使用网络定位")
            }
        }
    }

    fun hasPermission(): Boolean =
        ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED

    fun start(onFix: Listener, onFailure: (String) -> Unit): Boolean {
        val manager = manager ?: run {
            onFailure("location service unavailable")
            return false
        }
        if (!hasPermission()) {
            onFailure("location permission missing")
            return false
        }
        callback = onFix
        failure = onFailure

        // Seed with a recent last-known fix so the UI is not blank indoors.
        val known = listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER)
            .mapNotNull { provider ->
                runCatching {
                    if (manager.isProviderEnabled(provider)) manager.getLastKnownLocation(provider) else null
                }.getOrNull()
            }
            .maxByOrNull { it.time }
        known?.let { location -> toFix(location)?.let { fix -> onFix.onFix(fix) } }

        val providers = buildList {
            if (runCatching { manager.isProviderEnabled(LocationManager.GPS_PROVIDER) }.getOrDefault(false)) {
                add(LocationManager.GPS_PROVIDER to 1_000L)
            }
            if (runCatching { manager.isProviderEnabled(LocationManager.NETWORK_PROVIDER) }.getOrDefault(false)) {
                add(LocationManager.NETWORK_PROVIDER to 5_000L)
            }
        }
        if (providers.isEmpty()) {
            onFailure("GPS 与网络定位均不可用")
            return false
        }
        var registered = false
        providers.forEach { (provider, interval) ->
            runCatching {
                manager.requestLocationUpdates(
                    provider, interval, 0f, locationListener, Looper.getMainLooper(),
                )
                registered = true
            }.onFailure {
                onFailure("$provider 注册失败：${it.message}")
            }
        }
        return registered
    }

    fun stop() {
        callback = null
        failure = null
        lastProvider = null
        runCatching { manager?.removeUpdates(locationListener) }
    }

    private fun toFix(location: Location): Fix? {
        if (!location.hasAccuracy()) return null
        val age = System.currentTimeMillis() - location.time
        if (age > MAX_AGE_MS) return null
        val accuracy = location.accuracy.toDouble()
        if (accuracy < 0 || accuracy > MAX_ACCURACY_M) return null
        val speed = if (location.hasSpeed()) location.speed.toDouble() else 0.0
        val heading = if (location.hasBearing()) location.bearing.toDouble() else 0.0
        return Fix(
            longitude = location.longitude,
            latitude = location.latitude,
            accuracyM = accuracy,
            speedMps = speed,
            headingDeg = heading,
            timestampMs = location.time,
            provider = location.provider ?: "unknown",
        )
    }

    private companion object {
        const val MAX_AGE_MS = 15_000L
        const val MAX_ACCURACY_M = 100.0
    }
}
