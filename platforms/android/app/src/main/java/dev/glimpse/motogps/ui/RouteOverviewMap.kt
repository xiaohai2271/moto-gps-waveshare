package dev.glimpse.motogps.ui

import androidx.compose.foundation.Canvas
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import dev.glimpse.motogps.nav.RouteCandidate
import kotlin.math.max

/**
 * Static route-selection overview, ported from iOS RouteOverviewMap:
 * the selected candidate is drawn blue (7dp), the others gray (5dp), with a
 * blue start marker and a red destination marker, fitted to the view.
 * Like the iOS version this map is not pannable or zoomable.
 * The polyline is GCJ-02 (the gateway output) and is drawn without further
 * coordinate conversion — only relative geometry is shown, and no
 * WGS84/GCJ-02 boundary is crossed here.
 */
@Composable
fun RouteOverviewMap(
    candidates: List<RouteCandidate>,
    selectedId: String?,
    origin: Pair<Double, Double>?,
    destination: Pair<Double, Double>,
    modifier: Modifier = Modifier,
) {
    // Web-Mercator projection of every relevant point (lon, lat).
    data class Projected(val x: Double, val y: Double)

    fun mercator(lon: Double, lat: Double): Projected {
        val clampedLat = max(-85.0, lat.coerceAtMost(85.0))
        val x = Math.toRadians(lon)
        val y = Math.log(Math.tan(Math.PI / 4 + Math.toRadians(clampedLat) / 2))
        return Projected(x, y)
    }

    val points = remember(candidates, selectedId, origin, destination) {
        val all = mutableListOf<Pair<Projected, Boolean>>() // second = selected route
        candidates.forEach { candidate ->
            val selected = candidate.id == selectedId
            candidate.route.polyline.forEach { p ->
                all += mercator(p.longitude, p.latitude) to selected
            }
        }
        all
    }
    val originPoint = origin?.let { mercator(it.first, it.second) }
    val destinationPoint = mercator(destination.first, destination.second)

    val bounds = remember(points, originPoint, destinationPoint) {
        val xs = points.map { it.first.x } + listOfNotNull(originPoint?.x, destinationPoint.x)
        val ys = points.map { it.first.y } + listOfNotNull(originPoint?.y, destinationPoint.y)
        if (xs.isEmpty()) null
        else RectD(minX = xs.min(), maxX = xs.max(), minY = ys.min(), maxY = ys.max())
    }

    val blue = Color(0xFF007AFF)
    val gray = Color(0xFF8E8E93)
    val red = Color(0xFFFF3B30)
    val background = Color(0xFFF2F2F7)

    with(LocalDensity.current) {
        val padLeft = 30.dp.toPx()
        val padTop = 38.dp.toPx()
        val padRight = 30.dp.toPx()
        val padBottom = 42.dp.toPx()
        val selectedWidth = 7.dp.toPx()
        val unselectedWidth = 5.dp.toPx()

        Canvas(modifier) {
            val w = size.width - padLeft - padRight
            val h = size.height - padTop - padBottom
            val b = bounds ?: return@Canvas
            val spanX = max(b.maxX - b.minX, 1e-9)
            val spanY = max(b.maxY - b.minY, 1e-9)
            // Fit like MKMapView setVisibleMapRect: uniform scale, centered.
            val scale = minOf(w / spanX, h / spanY)
            fun project(p: Projected): Offset {
                val cx = (b.minX + b.maxX) / 2
                val cy = (b.minY + b.maxY) / 2
                return Offset(
                    (size.width / 2 + (p.x - cx) * scale).toFloat(),
                    (size.height / 2 - (p.y - cy) * scale).toFloat(),
                )
            }

            drawRect(background)

            // Unselected routes first so the selected one draws on top.
            candidates.filter { it.id != selectedId }.forEach { candidate ->
                if (candidate.route.polyline.size < 2) return@forEach
                val path = Path()
                candidate.route.polyline.forEachIndexed { i, p ->
                    val o = project(mercator(p.longitude, p.latitude))
                    if (i == 0) path.moveTo(o.x, o.y) else path.lineTo(o.x, o.y)
                }
                drawPath(path, gray, style = Stroke(unselectedWidth, cap = StrokeCap.Round, join = StrokeJoin.Round))
            }
            candidates.filter { it.id == selectedId }.forEach { candidate ->
                if (candidate.route.polyline.size < 2) return@forEach
                val path = Path()
                candidate.route.polyline.forEachIndexed { i, p ->
                    val o = project(mercator(p.longitude, p.latitude))
                    if (i == 0) path.moveTo(o.x, o.y) else path.lineTo(o.x, o.y)
                }
                drawPath(path, blue, style = Stroke(selectedWidth, cap = StrokeCap.Round, join = StrokeJoin.Round))
            }

            originPoint?.let {
                val o = project(it)
                drawCircle(blue, radius = selectedWidth * 1.1f, center = o)
                drawCircle(Color.White, radius = selectedWidth * 0.45f, center = o)
            }
            val d = project(destinationPoint)
            drawCircle(red, radius = selectedWidth * 1.1f, center = d)
            drawCircle(Color.White, radius = selectedWidth * 0.45f, center = d)
        }
    }
}

private data class RectD(val minX: Double, val maxX: Double, val minY: Double, val maxY: Double)
