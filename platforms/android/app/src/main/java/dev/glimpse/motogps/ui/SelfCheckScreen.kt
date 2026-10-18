package dev.glimpse.motogps.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import dev.glimpse.motogps.NativeBridge
import dev.glimpse.motogps.ProtocolSelfCheck

@Composable
fun SelfCheckScreen(expected: Map<String, String>, modifier: Modifier = Modifier) {
    var report by remember { mutableStateOf<ProtocolSelfCheck.Report?>(null) }

    Column(
        modifier = modifier.fillMaxSize().padding(20.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("协议黄金字节自检", style = MaterialTheme.typography.headlineSmall)
        Text(
            "用共享 C++ 编解码器重算上游 fixture，逐字节比对；仅证明协议一致，不代表 BLE 已连通。",
            style = MaterialTheme.typography.bodyMedium,
        )
        Button(onClick = {
            report = runCatching { NativeBridge.goldenVectors() }.fold(
                onSuccess = { ProtocolSelfCheck.run(expected, it) },
                onFailure = { ProtocolSelfCheck.Report(false, emptyList()) },
            )
        }) {
            Text("运行协议黄金字节自检")
        }

        report?.let { current ->
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            if (current.passed) "PASS" else "FAIL",
                            color = if (current.passed) Color(0xFF2E7D32) else Color(0xFFC62828),
                            style = MaterialTheme.typography.titleLarge,
                        )
                        Text(
                            "  JNI lib: " + if (current.nativeLoaded) "loaded" else "not loaded",
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                    if (current.lines.isEmpty()) {
                        Text(
                            if (current.nativeLoaded) "未读取到夹具，无法比对（assets 缺失）。"
                            else "C++ 库未加载，无法执行自检（模拟器需匹配 ABI）。",
                        )
                    }
                }
            }
            current.lines.forEach { line ->
                OutlinedCard(Modifier.fillMaxWidth()) {
                    Row(
                        Modifier.padding(12.dp).fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                    ) {
                        Text(
                            line.key,
                            modifier = Modifier.weight(1f),
                            style = MaterialTheme.typography.bodySmall,
                        )
                        Text(
                            if (line.ok) "match" else line.detail,
                            style = MaterialTheme.typography.bodySmall,
                            color = if (line.ok) Color(0xFF2E7D32) else Color(0xFFC62828),
                        )
                    }
                }
            }
        }
    }
}
