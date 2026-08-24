package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Cancel
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.MyLocation
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.data.BoundingBox
import com.raemond.opentrailpaper.data.GpxExporter
import com.raemond.opentrailpaper.data.LatLon
import com.raemond.opentrailpaper.data.Units
import com.raemond.opentrailpaper.map.EInkTileStore
import com.raemond.opentrailpaper.map.H3Tiles
import com.raemond.opentrailpaper.map.OutlineHex
import com.raemond.opentrailpaper.routing.Routing
import kotlinx.coroutines.launch
import java.util.Locale

/**
 * Search a destination, build a cycling route, preview it, and send it to the
 * head unit as GPX over BLE.
 */
@Composable
fun RouteScreen(ble: BleManager) {
    val scope = rememberCoroutineScope()
    var query by remember { mutableStateOf("") }
    var results by remember { mutableStateOf<List<Routing.Place>>(emptyList()) }
    var searching by remember { mutableStateOf(false) }
    var destination by remember { mutableStateOf<Routing.Place?>(null) }
    var route by remember { mutableStateOf<Routing.Route?>(null) }
    var building by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var camera by remember {
        mutableStateOf(MapCamera.region(LatLon(37.7764, -122.4346), 0.08))
    }
    /** Hexes the planned route crosses that NOTHING covers — see below. */
    var gapHexes by remember { mutableStateOf<List<OutlineHex>>(emptyList()) }
    var showSaved by remember { mutableStateOf(false) }
    var didCentre by remember { mutableStateOf(false) }

    val here: LatLon? = ble.lastLocation?.let { LatLon(it.latitude, it.longitude) }

    // Frame the rider as soon as there is a fix, instead of sitting on the
    // fallback region until the first tap of "recenter".
    LaunchedEffect(here) {
        if (!didCentre && here != null) {
            didCentre = true
            camera = MapCamera.region(here, 0.08)
        }
    }

    LaunchedEffect(Unit) { EInkTileStore.refresh() }

    /**
     * The hexes the route crosses that neither the phone nor the device holds.
     *
     * Tested against the H3 cell of each route point rather than the hexes
     * currently on screen: coverage is a property of the whole route, not of the
     * part the camera happens to frame, so panning must not change the answer.
     */
    fun recomputeCoverage() {
        val coords = route?.coordinates
        if (coords.isNullOrEmpty()) {
            gapHexes = emptyList()
            return
        }
        val covered = EInkTileStore.ids + ble.deviceTileIds
        if (covered.isEmpty()) {
            // Nothing downloaded at all. Papering the whole route in hexagons
            // would say only what the empty Maps screen already says.
            gapHexes = emptyList()
            return
        }
        val seen = HashSet<String>()
        gapHexes = coords.mapNotNull { c ->
            val id = H3Tiles.idAt(c) ?: return@mapNotNull null
            if (id in covered || !seen.add(id)) return@mapNotNull null
            val t = H3Tiles.tile(id) ?: return@mapNotNull null
            OutlineHex(id, t.hexagon, t.center, synced = false, missing = true)
        }
    }

    LaunchedEffect(route, EInkTileStore.version, ble.deviceTileIds) { recomputeCoverage() }

    Box(Modifier.fillMaxSize().background(Palette.paper)) {
        // Same map component as the Maps screen, but showing only the GAP hexes —
        // the ground a planned route crosses that nothing covers. That is the
        // question this page is for ("will I ride off my maps?"); ordinary
        // coverage would just be noise over the route.
        OsmMap(
            modifier = Modifier.fillMaxSize(),
            outlines = gapHexes,
            route = route?.coordinates,
            destination = destination?.let { MapDestination(it.name, it.coordinate) },
            camera = camera,
            showUserLocation = ble.locationPermission.isGranted,
        )

        // "Route" title pill, floated top-left.
        Text(
            "Route",
            style = condensed(22.sp, FontWeight.Bold),
            color = Palette.ink,
            modifier = Modifier
                .align(Alignment.TopStart)
                .padding(16.dp)
                .background(Palette.surface, RoundedCornerShape(50))
                .border(1.dp, Palette.hairline, RoundedCornerShape(50))
                .padding(horizontal = 16.dp, vertical = 9.dp),
        )

        Column(
            Modifier
                .align(Alignment.BottomCenter)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                RoundMapButton(Icons.Filled.MyLocation, enabled = true) {
                    camera = here?.let { MapCamera.region(it, 0.05) } ?: MapCamera.followUser()
                }
                Spacer(Modifier.size(10.dp))
                // Map downloads live in Settings — this page is for building and
                // sending a route, and the map tool is device-content management.
                RoundMapButton(
                    Icons.Filled.Folder,
                    enabled = ble.state == BleManager.ConnState.CONNECTED,
                ) {
                    ble.refreshRoutes()
                    showSaved = true
                }
            }

            SearchField(
                value = query,
                onValueChange = { query = it },
                searching = searching,
            ) {
                if (query.isBlank()) return@SearchField
                searching = true
                error = null
                scope.launch {
                    results = Routing.search(query, here)
                    searching = false
                    if (results.isEmpty()) error = "Nothing found for “$query”"
                }
            }

            if (results.isNotEmpty() && route == null) {
                ResultsList(results) { place ->
                    destination = place
                    results = emptyList()
                    ble.routeSent = false
                    ble.routeReceived = false
                    val from = here
                    if (from == null) {
                        error = "Waiting for your location — allow location to build a route"
                        return@ResultsList
                    }
                    building = true
                    scope.launch {
                        val built = Routing.route(from, place.coordinate)
                        building = false
                        if (built == null) {
                            error = "Couldn't build a route there"
                        } else {
                            route = built
                            built.bounds?.let { camera = MapCamera.box(it.paddedForDisplay()) }
                        }
                    }
                }
            }

            if (building) {
                Card {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                    ) {
                        CircularProgressIndicator(Modifier.size(20.dp), color = Palette.accent)
                        Text("Building route…", style = TypeScale.body, color = Palette.muted)
                    }
                }
            }

            error?.let {
                Card { Text(it, style = barlow(13.sp), color = Palette.accent) }
            }

            route?.let { r ->
                RouteSummaryCard(
                    name = destination?.name ?: "Route",
                    mode = r.mode,
                    distanceKm = r.distanceMeters / 1000,
                    minutes = (r.durationSeconds / 60).toInt(),
                    progress = ble.lastUploadProgress,
                    sent = ble.routeSent,
                    received = ble.routeReceived,
                    canSend = ble.state == BleManager.ConnState.CONNECTED,
                    useMiles = ble.useMiles,
                    onSend = {
                        val name = fileName(destination?.name)
                        ble.uploadRoute(
                            name = name,
                            gpx = GpxExporter.make(name, r.coordinates),
                            maneuvers = r.maneuvers,
                        )
                    },
                    onClear = {
                        route = null
                        destination = null
                        error = null
                    },
                )
            }
        }
    }

    if (showSaved) {
        SavedRoutesSheet(ble) { showSaved = false }
    }
}

/**
 * Grow the box by a fraction of its own size on each axis, so a route framed by
 * its bounds is not flush against the screen edges or hidden behind the search
 * field and summary overlays. A minimum span keeps very short routes from filling
 * the screen at street level, where the context that makes a route legible is all
 * off-screen.
 */
private fun BoundingBox.paddedForDisplay(
    fraction: Double = 0.25,
    minimumSpanDeg: Double = 0.018,
): BoundingBox {
    val padLat = maxOf((north - south) * fraction, (minimumSpanDeg - (north - south)) / 2, 0.0)
    val padLon = maxOf((east - west) * fraction, (minimumSpanDeg - (east - west)) / 2, 0.0)
    return BoundingBox(south - padLat, west - padLon, north + padLat, east + padLon)
}

private fun fileName(destination: String?): String {
    val base = destination?.takeIf { it.isNotBlank() } ?: "route"
    val safe = base.lowercase(Locale.US)
        .replace(' ', '_')
        .filter { it.isLetterOrDigit() || it == '_' }
    return safe.take(24) + ".gpx"
}

@Composable
private fun RoundMapButton(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    Box(
        Modifier
            .size(42.dp)
            .background(Palette.surface, CircleShape)
            .border(1.dp, Palette.hairline, CircleShape)
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            icon,
            contentDescription = null,
            tint = if (enabled) Palette.accent else Palette.faint,
            modifier = Modifier.size(18.dp),
        )
    }
}

@Composable
private fun SearchField(
    value: String,
    onValueChange: (String) -> Unit,
    searching: Boolean,
    onSubmit: () -> Unit,
) {
    TextField(
        value = value,
        onValueChange = onValueChange,
        modifier = Modifier
            .fillMaxWidth()
            .background(Palette.surface, RoundedCornerShape(14.dp))
            .border(1.dp, Palette.hairline, RoundedCornerShape(14.dp)),
        placeholder = { Text("Search destination", color = Palette.faint) },
        leadingIcon = { Icon(Icons.Filled.Search, contentDescription = null, tint = Palette.muted) },
        trailingIcon = {
            if (searching) CircularProgressIndicator(Modifier.size(18.dp), color = Palette.accent)
        },
        singleLine = true,
        // Searches fire on submit, never per keystroke: Nominatim is a shared
        // free service and a request per character is exactly what its usage
        // policy asks clients not to do.
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
        keyboardActions = KeyboardActions(onSearch = { onSubmit() }),
        colors = TextFieldDefaults.colors(
            focusedContainerColor = Color.Transparent,
            unfocusedContainerColor = Color.Transparent,
            focusedIndicatorColor = Color.Transparent,
            unfocusedIndicatorColor = Color.Transparent,
            cursorColor = Palette.accent,
        ),
    )
}

@Composable
private fun ResultsList(results: List<Routing.Place>, onPick: (Routing.Place) -> Unit) {
    Column(
        Modifier
            .fillMaxWidth()
            .background(Palette.surface, RoundedCornerShape(14.dp))
            .border(1.dp, Palette.hairline, RoundedCornerShape(14.dp)),
    ) {
        results.take(4).forEachIndexed { i, item ->
            Column(
                Modifier
                    .fillMaxWidth()
                    .clickable { onPick(item) }
                    .padding(horizontal = 14.dp, vertical = 12.dp),
            ) {
                Text(item.name, style = TypeScale.body, color = Palette.ink, maxLines = 1)
                if (item.detail.isNotBlank()) {
                    Text(item.detail, style = barlow(12.sp), color = Palette.muted, maxLines = 1)
                }
            }
            if (i < results.take(4).lastIndex) HorizontalDivider(color = Palette.hairline)
        }
    }
}

@Composable
private fun RouteSummaryCard(
    name: String,
    mode: String,
    distanceKm: Double,
    minutes: Int,
    progress: Double?,
    sent: Boolean,
    received: Boolean,
    canSend: Boolean,
    useMiles: Boolean,
    onSend: () -> Unit,
    onClear: () -> Unit,
) {
    Card {
        Row(verticalAlignment = Alignment.Top) {
            Column(Modifier.weight(1f)) {
                Text(name, style = TypeScale.title, color = Palette.ink, maxLines = 1)
                Row(
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    if (mode.isNotEmpty()) {
                        Text(
                            mode.uppercase(),
                            style = barlow(11.sp, FontWeight.Bold),
                            color = Color.White,
                            modifier = Modifier
                                .background(
                                    if (mode == "Cycling") Palette.good else Palette.muted,
                                    RoundedCornerShape(50),
                                )
                                .padding(horizontal = 7.dp, vertical = 3.dp),
                        )
                    }
                    Text(
                        String.format(
                            Locale.US,
                            "%.1f %s · %d min",
                            Units.distance(distanceKm, useMiles),
                            Units.distLabel(useMiles),
                            minutes,
                        ),
                        style = TypeScale.body,
                        color = Palette.muted,
                    )
                }
            }
            IconButton(onClick = onClear) {
                Icon(Icons.Filled.Cancel, contentDescription = "Clear route", tint = Palette.muted)
            }
        }
        Spacer(Modifier.size(12.dp))
        when {
            progress != null -> {
                Text("Sending to device…", style = barlow(12.sp), color = Palette.muted)
                LinearProgressIndicator(
                    progress = { progress.toFloat() },
                    modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
                    color = Palette.accent,
                    trackColor = Palette.hairline,
                )
            }

            sent -> Row(
                Modifier.fillMaxWidth().padding(vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Icon(Icons.Filled.CheckCircle, contentDescription = null, tint = Palette.good)
                // "Received" once the device confirms (0x23); until then (or on
                // older firmware that never acks) show "Sent".
                Text(
                    if (received) "Received by device" else "Sent to device",
                    style = TypeScale.body,
                    color = Palette.good,
                )
                Spacer(Modifier.weight(1f))
                TextButton(onClick = onSend, enabled = canSend) {
                    Text("Send again", style = barlow(13.sp, FontWeight.SemiBold))
                }
            }

            else -> PrimaryButton(
                title = if (canSend) "Send to device" else "Connect to send",
                icon = Icons.AutoMirrored.Filled.Send,
                enabled = canSend,
                onClick = onSend,
            )
        }
    }
}

/** Saved routes on the device, with delete. */
@Composable
private fun SavedRoutesSheet(ble: BleManager, onDismiss: () -> Unit) {
    FullScreenSheet(title = "Saved routes", onDismiss = onDismiss) {
        when {
            ble.loadingRoutes -> Box(
                Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center,
            ) { CircularProgressIndicator(color = Palette.accent) }

            ble.deviceRoutes.isEmpty() -> Box(
                Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center,
            ) {
                Column(
                    Modifier.padding(32.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text("No saved routes", style = TypeScale.title, color = Palette.ink)
                    Text(
                        "Routes you send to the device appear here.",
                        style = TypeScale.body,
                        color = Palette.muted,
                    )
                }
            }

            else -> Column {
                ble.deviceRoutes.forEach { name ->
                    Row(
                        Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 14.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(name, style = TypeScale.body, color = Palette.ink)
                        Spacer(Modifier.weight(1f))
                        TextButton(onClick = { ble.deleteRoute(name) }) {
                            Text("Delete", color = Palette.accent, style = barlow(13.sp))
                        }
                    }
                    HorizontalDivider(color = Palette.hairline)
                }
            }
        }
    }
}
