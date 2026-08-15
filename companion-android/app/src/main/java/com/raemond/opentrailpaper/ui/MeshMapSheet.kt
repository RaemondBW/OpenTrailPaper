package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.data.BoundingBox
import com.raemond.opentrailpaper.data.LatLon
import kotlin.math.max

/**
 * Every node that has broadcast a position, on a map. Nodes without one are
 * listed underneath instead of being silently dropped — "we have not heard where
 * it is" is information too.
 *
 * A cover with floating chrome rather than [FullScreenSheet]: the map is an
 * Android view and runs full-bleed here, so anything composed before it is drawn
 * over. Same reason the Maps screen is built this way.
 */
@Composable
fun MeshMapSheet(ble: BleManager, onDismiss: () -> Unit) {
    val positioned = ble.meshNodes.filter { it.position != null }
    val unpositioned = ble.meshNodes.filter { it.position == null }

    val pins = remember(ble.meshNodes, ble.lastLocation) {
        positioned.mapNotNull { n ->
            val p = n.position ?: return@mapNotNull null
            MeshNodePin(
                id = n.num,
                label = n.displayName,
                detail = meshPositionLine(n, ble) ?: p.shortText,
                coordinate = p.coordinate,
                imprecise = p.isImprecise,
            )
        }
    }

    var camera by remember { mutableStateOf<MapCamera?>(null) }

    // Opens on everything at once. A map centred on one node with the rest off
    // screen is the wrong first impression when the point is who is out there.
    LaunchedEffect(pins.isNotEmpty()) {
        if (camera == null) camera = frameAll(pins.map { it.coordinate })
    }

    FullScreenCover(onDismiss = onDismiss) {
        Box(Modifier.fillMaxSize()) {
            OsmMap(
                modifier = Modifier.fillMaxSize(),
                meshNodes = pins,
                camera = camera,
                showUserLocation = ble.locationPermission.isGranted,
            )

            // Composed AFTER the map, so it is not drawn over by it.
            Row(
                Modifier
                    .align(Alignment.TopCenter)
                    .fillMaxWidth()
                    .windowInsetsPadding(WindowInsets.statusBars)
                    .padding(horizontal = 16.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Pill("Node map")
                Spacer(Modifier.weight(1f))
                Pill("Done", onClick = onDismiss)
            }

            if (unpositioned.isNotEmpty()) {
                Column(
                    Modifier
                        .align(Alignment.BottomCenter)
                        .padding(16.dp)
                        .background(Palette.surface, RoundedCornerShape(20.dp))
                        .border(1.dp, Palette.hairline, RoundedCornerShape(20.dp))
                        .padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    TrackedLabel("No position reported")
                    Text(
                        unpositioned.joinToString(", ") { it.displayName },
                        style = barlow(14.sp),
                        color = Palette.muted,
                    )
                    Text(
                        "Nodes broadcast their position on their own schedule, and " +
                            "many are configured never to.",
                        style = barlow(13.sp),
                        color = Palette.faint,
                    )
                }
            }
        }
    }
}

@Composable
private fun Pill(label: String, onClick: (() -> Unit)? = null) {
    Text(
        label,
        style = condensed(18.sp, FontWeight.SemiBold),
        color = Palette.accent,
        modifier = Modifier
            .background(Palette.surface, RoundedCornerShape(50))
            .border(1.dp, Palette.hairline, RoundedCornerShape(50))
            .let { if (onClick != null) it.clickable(onClick = onClick) else it }
            .padding(horizontal = 16.dp, vertical = 9.dp),
    )
}

/**
 * A camera that holds every pin, or null when there is nothing to frame. A single
 * node gets a fixed span rather than a zero-sized box.
 */
private fun frameAll(coords: List<LatLon>): MapCamera? {
    if (coords.isEmpty()) return null
    if (coords.size == 1) return MapCamera.region(coords[0], 0.05)
    val south = coords.minOf { it.lat }
    val north = coords.maxOf { it.lat }
    val west = coords.minOf { it.lon }
    val east = coords.maxOf { it.lon }
    // A floor on the padding for the case where every node is in the same street,
    // which would otherwise frame a few metres across and zoom to the roof tiles.
    val padLat = max((north - south) * 0.3, 0.005)
    val padLon = max((east - west) * 0.3, 0.005)
    return MapCamera.box(
        BoundingBox(south - padLat, west - padLon, north + padLat, east + padLon),
    )
}
