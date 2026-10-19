package dev.glimpse.motogps

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.Modifier
import androidx.core.content.ContextCompat
import dev.glimpse.motogps.ui.MotoAppScreen

class MainActivity : ComponentActivity() {

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        if (BuildConfig.DEBUG) {
            val app = application as MotoGpsApplication
            intent?.getStringExtra("motoMac")?.let { app.bleCentral.connect(it) }
            intent?.getStringExtra("motoPrefetch")?.let { value ->
                val parts = value.split(",").mapNotNull { it.trim().toDoubleOrNull() }
                if (parts.size == 2) app.navigationSession.debugPrefetch(parts[0], parts[1])
            }
            intent?.getStringExtra("motoFix")?.let { value ->
                val parts = value.split(",").mapNotNull { it.trim().toDoubleOrNull() }
                if (parts.size == 2) app.navigationSession.debugSimulatedFix(parts[0], parts[1])
            }
            intent?.getStringExtra("motoSearch")?.let { app.navigationSession.debugSearchAndStart(it) }
            intent?.getStringExtra("motoCommand")?.let { value ->
                val parts = value.split(",").mapNotNull { it.trim().toIntOrNull() }
                if (parts.isNotEmpty()) {
                    app.navigationSession.debugRemoteCommand(parts[0], parts.getOrElse(1) { 0 })
                }
            }
            intent?.getStringExtra("motoMedia")?.let { app.navigationSession.debugMediaAction(it) }
        }

        requestMissingPermissions()

        setContent {
            MaterialTheme {
                // iOS uses a single full-screen flow: no bottom tabs. Keep the
                // content inside the safe drawing area so the status bar and the
                // gesture bar never cover the navigation bar or the primary
                // action button (targetSdk 36 draws edge-to-edge).
                Box(
                    Modifier
                        .fillMaxSize()
                        .windowInsetsPadding(WindowInsets.safeDrawing),
                ) {
                    MotoAppScreen()
                }
            }
        }
    }

    /**
     * Location, Bluetooth and notification permissions are requested once on
     * first launch, mirroring the iOS permission prompts. Denying a permission
     * never crashes the app: features degrade and the UI explains the state.
     */
    private fun requestMissingPermissions() {
        val wanted = buildList {
            add(Manifest.permission.ACCESS_FINE_LOCATION)
            add(Manifest.permission.ACCESS_COARSE_LOCATION)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                add(Manifest.permission.BLUETOOTH_SCAN)
                add(Manifest.permission.BLUETOOTH_CONNECT)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                add(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
        val missing = wanted.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) permissionLauncher.launch(missing.toTypedArray())
    }
}
