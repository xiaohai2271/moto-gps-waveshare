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
 * GNSS source built on the platform LocationManager so it works on devices
 * without Google Play Services. Coordinates are WGS84; conversion to GCJ-02
 * happens only at the gateway boundary or when comparing with route geometry.
 */
class GpsLocationSource(private val context: Context) {

    data class Fix(
        val longitude: Double,
        val latitude: Double,
        val accuracyM: Double,
        val speedMps: Double,
        val headingDeg: Double,
        val timestampMs: Long,
    )

    fun interface Listener {
        fun onFix(fix: Fix)
    }

    private val manager =
        context.getSystemService(Context.LOCATION_SERVICE) as? LocationManager

    private var callback: Listener? = null
    private var failure: ((String) -> Unit)? = null

    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            toFix(location)?.let { callback?.onFix(it) }
        }

        @Deprecated("Deprecated in Java")
        override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) = Unit
        override fun onProviderEnabled(provider: String) = Unit
        override fun onProviderDisabled(provider: String) {
            failure?.invoke("provider disabled: $provider")
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
        if (!manager.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
            onFailure("GPS provider disabled")
            return false
        }
        callback = onFix
        failure = onFailure
        return runCatching {
            manager.requestLocationUpdates(
                LocationManager.GPS_PROVIDER,
                1_000L,
                0f,
                locationListener,
                Looper.getMainLooper(),
            )
            true
        }.getOrElse {
            onFailure("requestLocationUpdates failed: ${it.message}")
            false
        }
    }

    fun stop() {
        callback = null
        failure = null
        runCatching { manager?.removeUpdates(locationListener) }
    }

    private fun toFix(location: Location): Fix? {
        if (!location.hasAccuracy()) return null
        val age = System.currentTimeMillis() - location.time
        if (age > MAX_AGE_MS) return null
        val accuracy = location.accuracy.toDouble()
        if (accuracy < 0) return null
        val speed = if (location.hasSpeed()) location.speed.toDouble() else 0.0
        val heading = if (location.hasBearing()) location.bearing.toDouble() else 0.0
        return Fix(
            longitude = location.longitude,
            latitude = location.latitude,
            accuracyM = accuracy,
            speedMps = speed,
            headingDeg = heading,
            timestampMs = location.time,
        )
    }

    private companion object {
        const val MAX_AGE_MS = 15_000L
    }
}
