package dev.glimpse.motogps.location

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager

/**
 * Phone compass based on the rotation-vector sensor. Used for the display
 * heading while stationary, where GNSS course is meaningless (0 or stale).
 * Requires no runtime permission.
 */
class CompassSource(context: Context) {

    fun interface Listener {
        fun onHeading(degrees: Double)
    }

    private val manager = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
    private val sensor: Sensor? =
        manager?.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
            ?: manager?.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD)

    private val rotation = FloatArray(9)
    private val orientation = FloatArray(3)
    private var callback: Listener? = null

    private val listener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            if (event.sensor.type == Sensor.TYPE_ROTATION_VECTOR) {
                SensorManager.getRotationMatrixFromVector(rotation, event.values)
                SensorManager.getOrientation(rotation, orientation)
                val degrees = Math.toDegrees(orientation[0].toDouble())
                callback?.onHeading((degrees + 360.0) % 360.0)
            } else {
                // Fallback: approximate heading from the magnetic field only.
                val x = event.values[0]
                val y = event.values[1]
                val degrees = Math.toDegrees(kotlin.math.atan2(y.toDouble(), x.toDouble()))
                callback?.onHeading((degrees + 360.0) % 360.0)
            }
        }

        override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
    }

    fun start(onHeading: Listener): Boolean {
        val manager = manager ?: return false
        val sensor = sensor ?: return false
        callback = onHeading
        return manager.registerListener(listener, sensor, SensorManager.SENSOR_DELAY_UI)
    }

    fun stop() {
        callback = null
        runCatching { manager?.unregisterListener(listener) }
    }
}
