package dev.glimpse.motogps

import android.Manifest
import android.content.pm.PackageManager
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import dev.glimpse.motogps.ble.Esp32BleCentral
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

/** Diagnostic: lists devices advertising the moto BLE service. */
@RunWith(AndroidJUnit4::class)
class BleScanProbeTest {
    @Test
    fun scanFindsDisplay() = runBlocking {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        assumeTrue(
            "BLUETOOTH_SCAN not granted",
            instrumentation.targetContext.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) ==
                PackageManager.PERMISSION_GRANTED,
        )
        val app = instrumentation.targetContext.applicationContext as MotoGpsApplication
        val central = app.bleCentral
        instrumentation.runOnMainSync { central.startScan() }
        val found = withTimeoutOrNull(15_000) {
            central.devices.first { it.isNotEmpty() }
        }
        instrumentation.runOnMainSync { central.stopScan() }
        assertTrue("no device advertising the moto service", found != null)
        val summary = found!!.joinToString { "${it.name}@${it.address} rssi=${it.rssi}" }
        org.junit.Assert.fail("found: $summary")
    }
}
