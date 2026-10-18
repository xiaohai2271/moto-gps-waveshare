package dev.glimpse.motogps

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import dev.glimpse.motogps.ui.BleScreen
import dev.glimpse.motogps.ui.MapScreen
import dev.glimpse.motogps.ui.NavScreen
import dev.glimpse.motogps.ui.SelfCheckScreen

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val fixture = runCatching {
            assets.open(ProtocolSelfCheck.ASSET_NAME)
                .bufferedReader(Charsets.UTF_8).readText()
                .let(ProtocolSelfCheck::parse)
        }.getOrDefault(emptyMap())

        if (BuildConfig.DEBUG) {
            val app = application as MotoGpsApplication
            intent?.getStringExtra("motoMac")?.let { app.bleCentral.connect(it) }
            intent?.getStringExtra("motoSearch")?.let { app.navigationSession.debugSearchAndStart(it) }
        }

        setContent {
            MaterialTheme {
                AppScaffold(fixture)
            }
        }
    }
}

@Composable
private fun AppScaffold(fixture: Map<String, String>) {
    var tab by remember { mutableIntStateOf(0) }
    val titles = listOf("导航", "地图", "圆屏连接", "协议自检")
    Scaffold(
        bottomBar = {
            NavigationBar {
                titles.forEachIndexed { index, title ->
                    NavigationBarItem(
                        selected = tab == index,
                        onClick = { tab = index },
                        icon = { Text(listOf("➤", "◎", "✓")[index]) },
                        label = { Text(title) },
                    )
                }
            }
        },
    ) { padding ->
        when (tab) {
            0 -> NavScreen(Modifier.padding(padding))
            1 -> MapScreen(Modifier.padding(padding))
            2 -> BleScreen(Modifier.padding(padding))
            else -> SelfCheckScreen(fixture, Modifier.padding(padding))
        }
    }
}
