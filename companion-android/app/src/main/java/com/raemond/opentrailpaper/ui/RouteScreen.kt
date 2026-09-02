package com.raemond.opentrailpaper.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.FileOpen
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.MyLocation
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Signpost
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
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.Maneuver
import com.raemond.opentrailpaper.data.BoundingBox
import com.raemond.opentrailpaper.data.DeviceText
import com.raemond.opentrailpaper.data.GpxExporter
import com.raemond.opentrailpaper.data.ImportedRoute
import com.raemond.opentrailpaper.data.LatLon
import com.raemond.opentrailpaper.data.RouteImport
import com.raemond.opentrailpaper.data.Units
import com.raemond.opentrailpaper.map.EInkTileStore
import com.raemond.opentrailpaper.map.H3Tiles
import com.raemond.opentrailpaper.map.MapTile
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
    var preview by remember { mutableStateOf<Preview?>(null) }
    var derivingCues by remember { mutableStateOf(false) }
    var building by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var camera by remember {
        mutableStateOf(MapCamera.region(LatLon(37.7764, -122.4346), 0.08))
    }
    /** Hexes the planned route crosses that NOTHING covers — see below. */
    var gapHexes by remember { mutableStateOf<List<OutlineHex>>(emptyList()) }
    /** The same gaps as tiles, for seeding a download. Kept even when the
     *  overlay above is deliberately empty — see [recomputeCoverage]. */
    var gapTiles by remember { mutableStateOf<List<MapTile>>(emptyList()) }
    var showSaved by remember { mutableStateOf(false) }
    var showMaps by remember { mutableStateOf(false) }
    /**
     * The gaps as they stood when the rider tapped download.
     *
     * A snapshot, not [gapTiles] itself: opening the Maps sheet refreshes the
     * tile store and the device's tile list, both of which rebuild [gapTiles]
     * as a new list — and handing a sheet a parameter that changes identity
     * underneath it recomposes the whole sheet, map included, in a loop.
     */
    var mapsSeed by remember { mutableStateOf<List<MapTile>>(emptyList()) }
    /**
     * Measured height of the floating stack at the bottom. The map runs
     * full-bleed underneath it, so without this a route frames itself to the
     * whole screen and lands half-hidden behind the card.
     */
    var cardHeightPx by remember { mutableStateOf(0) }
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

    // A route imported from a file, whether picked here or opened from another
    // app. Framed the same way a searched route is.
    //
    // Rebuilt from RouteImport rather than taken from it: everything remembered
    // in this composable dies on a tab tap, and a rider who leaves for the Ride
    // tab to connect the head unit has to find their route still here when they
    // come back. RouteImport holds it until they clear it.
    LaunchedEffect(RouteImport.pending, cardHeightPx > 0) {
        val imported = RouteImport.pending ?: return@LaunchedEffect
        results = emptyList()
        destination = null
        error = null
        derivingCues = false
        preview = imported.asPreview(RouteImport.cues)
        if (RouteImport.fresh) {
            RouteImport.markDrawn()
            // Only an arrival invalidates the last upload. A redraw is the same
            // route the rider already sent, and still says so.
            ble.routeSent = false
            ble.routeReceived = false
        }
        // Framed on every redraw, not only on arrival: the camera is remembered
        // here too, so a tab round-trip would otherwise restore the route with
        // the map sitting somewhere else entirely.
        //
        // Also re-runs once the card has been measured, because a route that
        // arrives before first layout would otherwise be framed against a
        // zero inset and sit behind the card. Keyed on the boolean, not the
        // height, so the later shrink when a button disappears doesn't yank
        // the camera out from under someone reading the map.
        BoundingBox.around(imported.points)?.let {
            camera = MapCamera.box(it.paddedForDisplay())
        }
    }

    LaunchedEffect(RouteImport.error) {
        RouteImport.error?.let {
            error = it
            RouteImport.clearError()
        }
    }

    /**
     * The hexes the route crosses that neither the phone nor the device holds.
     *
     * Tested against the H3 cell of each route point rather than the hexes
     * currently on screen: coverage is a property of the whole route, not of the
     * part the camera happens to frame, so panning must not change the answer.
     */
    fun recomputeCoverage() {
        val coords = preview?.points
        if (coords.isNullOrEmpty()) {
            gapTiles = emptyList()
            gapHexes = emptyList()
            return
        }
        val covered = EInkTileStore.ids + ble.deviceTileIds
        val seen = HashSet<String>()
        gapTiles = coords.mapNotNull { c ->
            val id = H3Tiles.idAt(c) ?: return@mapNotNull null
            if (id in covered || !seen.add(id)) return@mapNotNull null
            H3Tiles.tile(id)
        }
        // The OVERLAY stays silent when nothing is downloaded at all: papering
        // the whole route in hexagons would say only what the empty Maps screen
        // already says. The download button deliberately does not follow that
        // rule — a rider carrying no maps is exactly who needs it, and they are
        // the one person the overlay never speaks to.
        gapHexes = if (covered.isEmpty()) {
            emptyList()
        } else {
            gapTiles.map {
                OutlineHex(it.id, it.hexagon, it.center, synced = false, missing = true)
            }
        }
    }

    LaunchedEffect(preview, EInkTileStore.version, ble.deviceTileIds) { recomputeCoverage() }

    val context = LocalContext.current
    // Permissive on the picker where the rider is choosing the file themselves;
    // the manifest filters, which decide what shows up in *other* apps'
    // choosers, are the ones that have to stay narrow.
    val pickGpx = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument(),
    ) { uri ->
        if (uri != null) scope.launch { RouteImport.read(context, uri) }
    }

    Box(Modifier.fillMaxSize().background(Palette.paper)) {
        // Same map component as the Maps screen, but showing only the GAP hexes —
        // the ground a planned route crosses that nothing covers. That is the
        // question this page is for ("will I ride off my maps?"); ordinary
        // coverage would just be noise over the route.
        OsmMap(
            modifier = Modifier.fillMaxSize(),
            outlines = gapHexes,
            route = preview?.points,
            destination = destination?.let { MapDestination(it.name, it.coordinate) },
            camera = camera,
            showUserLocation = ble.locationPermission.isGranted,
            bottomInsetPx = cardHeightPx,
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
                .onSizeChanged { cardHeightPx = it.height }
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                RoundMapButton(Icons.Filled.FileOpen, enabled = true) {
                    pickGpx.launch(
                        arrayOf(
                            "application/gpx+xml",
                            "application/octet-stream",
                            "application/xml",
                            "text/xml",
                        ),
                    )
                }
                Spacer(Modifier.size(10.dp))
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

            // Hidden while an imported route is up. Searching would build a
            // different route and throw this one away, and the rider who just
            // opened a file did not come here to type a destination — the
            // card's clear button is the way back to searching. A route this
            // screen built itself keeps the field: the query that produced it
            // is still the obvious thing to edit.
            if (preview?.imported != true) {
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
            }

            if (results.isNotEmpty() && preview == null) {
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
                        // The file button is live while this is in flight, so an
                        // import can land first. A search tap only starts from an
                        // empty preview: if one exists by now the rider moved on,
                        // and their route must not be overwritten by this one.
                        if (preview != null) return@launch
                        if (built == null) {
                            error = "Couldn't build a route there"
                        } else {
                            preview = built.asPreview(place.name)
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

            preview?.let { p ->
                RouteSummaryCard(
                    name = p.name,
                    mode = p.mode,
                    distanceKm = p.distanceMeters / 1000,
                    minutes = p.minutes,
                    cueCount = if (p.imported) p.maneuvers.size else null,
                    cuesTruncated = p.cuesTruncated,
                    derivingCues = derivingCues,
                    mapGapCount = gapTiles.size,
                    onDownloadMaps = {
                        mapsSeed = gapTiles
                        showMaps = true
                    },
                    progress = ble.lastUploadProgress,
                    sent = ble.routeSent,
                    received = ble.routeReceived,
                    canSend = ble.state == BleManager.ConnState.CONNECTED,
                    useMiles = ble.useMiles,
                    onAddCues = {
                        derivingCues = true
                        error = null
                        scope.launch {
                            val set = Routing.cues(p.points)
                            derivingCues = false
                            // The rider can clear this route, or import another,
                            // while OSRM is still thinking. A slow request must
                            // not resurrect a route they dismissed — nor report
                            // its failure over whatever replaced it.
                            if (preview !== p) return@launch
                            when {
                                set == null ->
                                    error = "Couldn't fetch turn cues — check your connection"
                                // Not a failure: a path with no junctions in it
                                // has nothing to call out, and saying the network
                                // broke would send the rider chasing a fault
                                // that isn't there.
                                set.maneuvers.isEmpty() ->
                                    error = "This route has no turns to call out"
                                else -> {
                                    // Parked so a tab round-trip doesn't quietly
                                    // cost a second request for the same cues.
                                    RouteImport.noteCues(set)
                                    preview = p.copy(
                                        maneuvers = set.maneuvers,
                                        cuesTruncated = set.truncated,
                                    )
                                }
                            }
                        }
                    },
                    onSend = {
                        val name = DeviceText.routeFileName(p.name)
                        ble.uploadRoute(
                            name = name,
                            gpx = GpxExporter.make(name, p.points),
                            maneuvers = p.maneuvers,
                        )
                    },
                    onClear = {
                        // The import outlives this screen, so clearing it here
                        // is not enough — it would come back on the next tab.
                        RouteImport.clear()
                        preview = null
                        destination = null
                        error = null
                        derivingCues = false
                    },
                )
            }
        }
    }

    if (showSaved) {
        SavedRoutesSheet(ble) { showSaved = false }
    }

    // The Maps tool, opened with this route's uncovered hexes already selected
    // so the rider lands on the download rather than on an empty drawing board.
    if (showMaps) {
        MapsSheet(ble = ble, seed = mapsSeed) { showMaps = false }
    }
}

/**
 * What the screen is previewing, however it arrived.
 *
 * A searched route and an imported one differ in exactly two ways — an import
 * has no time estimate and starts with no turn cues — so they share everything
 * below this point rather than growing a parallel set of states.
 */
private data class Preview(
    val name: String,
    val points: List<LatLon>,
    val distanceMeters: Double,
    /** Null for an import: there is no honest time estimate for someone else's route. */
    val minutes: Int?,
    val mode: String,
    val maneuvers: List<Maneuver>,
    val imported: Boolean,
    val cuesTruncated: Boolean = false,
)

private fun Routing.Route.asPreview(name: String) = Preview(
    name = name,
    points = coordinates,
    distanceMeters = distanceMeters,
    minutes = (durationSeconds / 60).toInt(),
    mode = mode,
    maneuvers = maneuvers,
    imported = false,
)

private fun ImportedRoute.asPreview(cues: Routing.CueSet?) = Preview(
    name = name,
    points = points,
    distanceMeters = distanceMeters,
    minutes = null,
    mode = mode,
    maneuvers = cues?.maneuvers.orEmpty(),
    imported = true,
    cuesTruncated = cues?.truncated == true,
)

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
    minutes: Int?,
    cueCount: Int?,
    cuesTruncated: Boolean,
    derivingCues: Boolean,
    mapGapCount: Int,
    onDownloadMaps: () -> Unit,
    progress: Double?,
    sent: Boolean,
    received: Boolean,
    canSend: Boolean,
    useMiles: Boolean,
    onAddCues: () -> Unit,
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
                        buildString {
                            append(
                                String.format(
                                    Locale.US,
                                    "%.1f %s",
                                    Units.distance(distanceKm, useMiles),
                                    Units.distLabel(useMiles),
                                ),
                            )
                            // Only for a route we built: there is no honest
                            // duration for a file somebody else planned.
                            if (minutes != null) append(" · $minutes min")
                            if (cueCount != null && cueCount > 0) {
                                append(" · $cueCount turns")
                                if (cuesTruncated) append(" (capped)")
                            }
                        },
                        style = TypeScale.body,
                        color = Palette.muted,
                    )
                }
            }
            IconButton(onClick = onClear) {
                Icon(Icons.Filled.Cancel, contentDescription = "Clear route", tint = Palette.muted)
            }
        }
        // Imported routes arrive with no turn cues. Fetching them sends the
        // shape of this route to a third party, so it is a thing the rider
        // asks for — and the service is named under the button, in front of
        // them before they tap, rather than buried in a dialog after.
        if (cueCount == 0) {
            Spacer(Modifier.size(12.dp))
            if (derivingCues) {
                Row(
                    Modifier.fillMaxWidth().padding(vertical = 13.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.Center,
                ) {
                    CircularProgressIndicator(Modifier.size(18.dp), color = Palette.accent)
                    Spacer(Modifier.size(12.dp))
                    Text("Fetching turn cues…", style = barlow(13.sp), color = Palette.muted)
                }
            } else {
                SecondaryButton(
                    title = "Add turn cues",
                    icon = Icons.Filled.Signpost,
                    onClick = onAddCues,
                )
                Text(
                    "Fetched from routing.openstreetmap.de",
                    style = barlow(11.sp),
                    color = Palette.faint,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 6.dp),
                    textAlign = TextAlign.Center,
                )
            }
        }
        // The route crosses ground the head unit can't draw. Offering the fix
        // here rather than only in Settings, because this is the screen where a
        // rider finds out — the gap hexagons are already on the map behind this
        // card. The download itself still belongs to the Maps tool; this only
        // opens it with the route's own gaps already selected.
        if (mapGapCount > 0) {
            Spacer(Modifier.size(12.dp))
            SecondaryButton(
                title = "Download $mapGapCount map area${if (mapGapCount == 1) "" else "s"}",
                icon = Icons.Filled.Download,
                onClick = onDownloadMaps,
            )
            Text(
                "This route crosses ground the device has no map for",
                style = barlow(11.sp),
                color = Palette.faint,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 6.dp),
                textAlign = TextAlign.Center,
            )
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
