package com.raemond.opentrailpaper.ui

import kotlin.math.roundToInt
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.material.icons.filled.Add
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.IntrinsicSize
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
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.MyLocation
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Signpost
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
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
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
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
import com.raemond.opentrailpaper.data.SyncAccounts
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
    /** The import source sheet (a file, or a connected account's saved routes). */
    var importMenu by remember { mutableStateOf(false) }
    var cloudSource by remember { mutableStateOf<SyncAccounts.Provider?>(null) }
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
    //
    // Never over a route, though. The first fix can land a second or two after
    // an import — long enough for the rider to see their route framed and then
    // watch the map slide away to wherever they are standing, which on an
    // imported route is usually a different part of the country.
    LaunchedEffect(here, preview == null) {
        if (!didCentre && here != null && preview == null) {
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
    LaunchedEffect(RouteImport.arrival, cardHeightPx > 0) {
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

    val focusManager = LocalFocusManager.current
    val keyboard = LocalSoftwareKeyboardController.current

    val openFile = {
        pickGpx.launch(
            arrayOf(
                "application/gpx+xml",
                "application/octet-stream",
                "application/xml",
                "text/xml",
            ),
        )
    }
    // With Strava or RideWithGPS connected the button offers their
    // saved routes too; without, it is the file picker as before.
    val accounts = SyncAccounts.routeProviders.filter { SyncAccounts.isConnected(it) }

    val onAddCues: (Preview) -> Unit = { p ->
        derivingCues = true
        error = null
        scope.launch {
            val set = Routing.cues(p.points)
            derivingCues = false
            // The rider can clear this route, or import another, while OSRM
            // is still thinking. A slow request must not resurrect a route
            // they dismissed — nor report its failure over whatever replaced it.
            if (preview !== p) return@launch
            when {
                set == null ->
                    error = "Couldn't fetch turn cues — check your connection"
                // Not a failure: a path with no junctions in it has nothing to
                // call out, and saying the network broke would send the rider
                // chasing a fault that isn't there.
                set.maneuvers.isEmpty() ->
                    error = "This route has no turns to call out"
                else -> {
                    // Parked so a tab round-trip doesn't quietly cost a second
                    // request for the same cues.
                    RouteImport.noteCues(set)
                    preview = p.copy(
                        maneuvers = set.maneuvers,
                        cuesTruncated = set.truncated,
                    )
                }
            }
        }
    }
    val onSend: (Preview) -> Unit = { p ->
        val name = DeviceText.routeFileName(p.name)
        ble.uploadRoute(
            name = name,
            gpx = GpxExporter.make(name, p.points),
            maneuvers = p.maneuvers,
        )
    }
    val onClear: () -> Unit = {
        // The import outlives this screen, so clearing it here is not
        // enough — it would come back on the next tab.
        RouteImport.clear()
        preview = null
        destination = null
        error = null
        derivingCues = false
    }
    val onImport: () -> Unit = { if (accounts.isEmpty()) openFile() else importMenu = true }

    Column(Modifier.fillMaxSize().background(Palette.paper)) {
        // Ruled header: the device's saved routes and import.
        ScreenHeader("Route") {
            SquareIconButton(
                Icons.Filled.Folder, "Routes on the device",
                enabled = ble.state == BleManager.ConnState.CONNECTED,
            ) {
                ble.refreshRoutes()
                showSaved = true
            }
            SquareIconButton(Icons.Filled.Add, "Import a route", onClick = onImport)
        }

        Box(Modifier.weight(1f).fillMaxWidth().clipToBounds()) {
            // Same map component as the Maps screen, but showing only the GAP
            // hexes — the ground a planned route crosses that nothing covers.
            // That is the question this page is for ("will I ride off my
            // maps?"); ordinary coverage would just be noise over the route.
            OsmMap(
                modifier = Modifier
                    .fillMaxSize()
                    // A finger on the map is a finger off the search field:
                    // drop the keyboard. Watched in the initial pass because
                    // the MapView underneath consumes every touch it gets, and
                    // this must not stop it panning — it only listens.
                    .pointerInput(Unit) {
                        awaitEachGesture {
                            awaitFirstDown(requireUnconsumed = false, pass = PointerEventPass.Initial)
                            focusManager.clearFocus()
                            keyboard?.hide()
                        }
                    },
                outlines = gapHexes,
                route = preview?.points,
                destination = destination?.let { MapDestination(it.name, it.coordinate) },
                camera = camera,
                showUserLocation = ble.locationPermission.isGranted,
                showAttribution = false,
            )
            MapAttribution(Modifier.align(Alignment.BottomStart).padding(8.dp))

            Row(
                Modifier.fillMaxWidth().padding(12.dp),
                verticalAlignment = Alignment.Top,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                val p = preview
                if (p != null) {
                    RouteNameBox(p, ble.useMiles, Modifier.weight(1f, fill = false), onClear)
                }
                Spacer(Modifier.weight(1f))
                SquareIconButton(Icons.Filled.MyLocation, "Show my location") {
                    camera = here?.let { MapCamera.region(it, 0.05) } ?: MapCamera.followUser()
                }
            }

            if (results.isNotEmpty() && preview == null) {
                Box(Modifier.align(Alignment.BottomCenter).padding(12.dp)) {
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
                            // The import button is live while this is in flight,
                            // so an import can land first. A search tap only
                            // starts from an empty preview: if one exists by now
                            // the rider moved on, and their route must not be
                            // overwritten by this one.
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
            }
        }

        Box(Modifier.fillMaxWidth().height(Rule.title).background(Palette.ink))

        Column(
            Modifier
                .fillMaxWidth()
                .onSizeChanged { cardHeightPx = it.height }
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            val p = preview
            if (building) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    BlockSpinner()
                    Text("BUILDING ROUTE…", style = caps(12.sp), color = Palette.ink)
                }
            }
            error?.let { WarningBanner("Route", message = it) }
            if (p != null) {
                RouteStatsBox(
                    p, ble.useMiles,
                    tiles = when {
                        (EInkTileStore.ids + ble.deviceTileIds).isEmpty() -> "NONE"
                        gapTiles.isEmpty() -> "READY"
                        else -> "${gapTiles.size} GAPS"
                    },
                )
                RouteExtras(
                    cueCount = if (p.imported) p.maneuvers.size else null,
                    derivingCues = derivingCues,
                    mapGapCount = gapTiles.size,
                    onAddCues = { onAddCues(p) },
                    onDownloadMaps = {
                        mapsSeed = gapTiles
                        showMaps = true
                    },
                )
                SendControls(
                    progress = ble.lastUploadProgress,
                    sent = ble.routeSent,
                    received = ble.routeReceived,
                    canSend = ble.state == BleManager.ConnState.CONNECTED,
                ) { onSend(p) }
            } else {
                // Hidden while a route is up: a search would replace it, and
                // the name box's clear button is the way back.
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
            BlockButton("Import from Strava · RWGPS · GPX", filled = false, expand = true, onClick = onImport)
        }
    }

    if (showSaved) {
        SavedRoutesSheet(ble) { showSaved = false }
    }

    if (importMenu) {
        RouteSourceSheet(
            accounts = accounts,
            onFile = openFile,
            onAccount = { cloudSource = it },
            onDismiss = { importMenu = false },
        )
    }

    cloudSource?.let { p ->
        CloudRoutesSheet(
            p,
            useMiles = ble.useMiles,
            onBack = { cloudSource = null; importMenu = true },
            onDismiss = { cloudSource = null },
        )
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
    /** The account an import came from, and the climb it reported; null for a file. */
    val source: String? = null,
    val climbMeters: Double? = null,
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
    source = source,
    climbMeters = climbMeters,
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
private fun SearchField(
    value: String,
    onValueChange: (String) -> Unit,
    searching: Boolean,
    onSubmit: () -> Unit,
) {
    val keyboard = LocalSoftwareKeyboardController.current
    val focusManager = LocalFocusManager.current
    TextField(
        value = value,
        onValueChange = onValueChange,
        modifier = Modifier
            .fillMaxWidth()
            .background(Palette.surface)
            .einkBorder(),
        placeholder = { Text("Search destination", color = Palette.faint) },
        leadingIcon = { Icon(Icons.Filled.Search, contentDescription = null, tint = Palette.muted) },
        trailingIcon = {
            if (searching) BlockSpinner()
        },
        singleLine = true,
        // Searches fire on submit, never per keystroke: Nominatim is a shared
        // free service and a request per character is exactly what its usage
        // policy asks clients not to do.
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
        // The search runs on its own; the keyboard has nothing left to do and
        // would otherwise cover the results it is about to produce.
        keyboardActions = KeyboardActions(onSearch = {
            keyboard?.hide()
            focusManager.clearFocus()
            onSubmit()
        }),
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
            .background(Palette.surface)
            .border(Rule.border, Palette.hairline),
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

/** The route's name and one line about it, boxed over the map's corner. */
@Composable
private fun RouteNameBox(p: Preview, useMiles: Boolean, modifier: Modifier, onClear: () -> Unit) {
    Row(
        modifier
            .background(Palette.paper)
            .einkBorder()
            .padding(start = 10.dp, top = 6.dp, bottom = 6.dp, end = 2.dp),
        verticalAlignment = Alignment.Top,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Column(Modifier.weight(1f, fill = false), verticalArrangement = Arrangement.spacedBy(1.dp)) {
            Text(
                p.name.uppercase(), style = caps(14.sp), color = Palette.ink,
                maxLines = 2, overflow = TextOverflow.Ellipsis,
            )
            val parts = buildList {
                if (p.source != null) add("from ${p.source}") else if (p.mode.isNotEmpty()) add(p.mode.lowercase())
                if (p.imported && p.mode.isNotEmpty() && p.source != null) add(p.mode.lowercase())
                add(String.format(Locale.US, "%.1f %s", Units.distance(p.distanceMeters / 1000, useMiles), Units.distLabel(useMiles)))
                p.climbMeters?.let { add("${Units.elevation(it, useMiles).roundToInt()} ${Units.elevLabel(useMiles)}") }
                if (p.imported && p.maneuvers.isNotEmpty()) {
                    add("${p.maneuvers.size} turns${if (p.cuesTruncated) " (capped)" else ""}")
                }
            }
            Text(parts.joinToString(" · "), style = barlow(12.sp, FontWeight.SemiBold), color = Palette.ink, maxLines = 1)
        }
        Box(
            Modifier.size(32.dp).clickable(onClickLabel = "Clear route", onClick = onClear),
            contentAlignment = Alignment.Center,
        ) {
            Icon(Icons.Filled.Close, contentDescription = "Clear route", tint = Palette.ink, modifier = Modifier.size(16.dp))
        }
    }
}

/** Distance · climb (or riding time for a planned route) · map tiles. */
@Composable
private fun RouteStatsBox(p: Preview, useMiles: Boolean, tiles: String) {
    Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min).einkBorder()) {
        RouteStat(
            "Distance",
            String.format(Locale.US, "%.1f", Units.distance(p.distanceMeters / 1000, useMiles)),
            Units.distLabel(useMiles), Modifier.weight(1f),
        )
        Box(Modifier.width(Rule.divider).fillMaxHeight().background(Palette.ink))
        if (p.minutes != null) {
            RouteStat("Time", "${p.minutes}", "min", Modifier.weight(1f))
        } else {
            RouteStat(
                "Climb",
                p.climbMeters?.let { "${Units.elevation(it, useMiles).roundToInt()}" } ?: "--",
                Units.elevLabel(useMiles), Modifier.weight(1f),
            )
        }
        Box(Modifier.width(Rule.divider).fillMaxHeight().background(Palette.ink))
        RouteStat("Map tiles", tiles, "", Modifier.weight(1f))
    }
}

@Composable
private fun RouteStat(label: String, value: String, unit: String, modifier: Modifier) {
    Column(modifier.padding(horizontal = 10.dp, vertical = 8.dp)) {
        TrackedLabel(label)
        Row(verticalAlignment = Alignment.Bottom) {
            Text(value, style = anton(26.sp), color = Palette.ink, maxLines = 1, modifier = Modifier.alignByBaseline())
            if (unit.isNotEmpty()) {
                Spacer(Modifier.size(3.dp))
                Text(unit.uppercase(), style = caps(11.sp, 0.sp), color = Palette.ink, modifier = Modifier.alignByBaseline())
            }
        }
    }
}

/**
 * Turn cues for an import (fetching them sends the route's shape to a third
 * party, so it is asked for, and the service named under the button), and
 * the route's map gaps, opened in the Maps tool with those gaps selected.
 */
@Composable
private fun RouteExtras(
    cueCount: Int?,
    derivingCues: Boolean,
    mapGapCount: Int,
    onAddCues: () -> Unit,
    onDownloadMaps: () -> Unit,
) {
    if (cueCount == 0) {
        if (derivingCues) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                BlockSpinner()
                Text("FETCHING TURN CUES…", style = caps(12.sp), color = Palette.ink)
            }
        } else {
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                SecondaryButton(title = "Add turn cues", icon = Icons.Filled.Signpost, onClick = onAddCues)
                Text(
                    "Fetched from routing.openstreetmap.de", style = barlow(11.sp), color = Palette.muted,
                    modifier = Modifier.fillMaxWidth(), textAlign = TextAlign.Center,
                )
            }
        }
    }
    if (mapGapCount > 0) {
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            SecondaryButton(
                title = "Download $mapGapCount map area${if (mapGapCount == 1) "" else "s"}",
                icon = Icons.Filled.Download,
                onClick = onDownloadMaps,
            )
            Text(
                "This route crosses ground the device has no map for", style = barlow(11.sp), color = Palette.muted,
                modifier = Modifier.fillMaxWidth(), textAlign = TextAlign.Center,
            )
        }
    }
}

/** SEND TO DEVICE, its block progress, and the inverted ✓ once it's there. */
@Composable
private fun SendControls(progress: Double?, sent: Boolean, received: Boolean, canSend: Boolean, onSend: () -> Unit) {
    when {
        progress != null -> Column(
            Modifier.fillMaxWidth().heightIn(min = 52.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp, Alignment.CenterVertically),
        ) {
            Text("SENDING TO DEVICE…", style = caps(12.sp), color = Palette.ink)
            BlockProgressBar(progress.toFloat(), Modifier.fillMaxWidth(), segments = 12)
        }

        sent -> Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            // "Received" once the device confirms; until then (or on older
            // firmware that never acks) "Sent".
            Box(
                Modifier.weight(1f).fillMaxHeight().heightIn(min = 52.dp).background(Palette.ink),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    if (received) "✓ RECEIVED BY DEVICE" else "✓ SENT TO DEVICE",
                    style = anton(18.sp).copy(letterSpacing = 1.5.sp), color = Palette.paper, maxLines = 1,
                )
            }
            BlockButton(
                "Again", filled = false, enabled = canSend, onClick = onSend,
                modifier = Modifier.fillMaxHeight().semantics { contentDescription = "Send again" },
            )
        }

        else -> PrimaryButton(
            title = if (canSend) "Send to device" else "Connect to send",
            enabled = canSend,
            onClick = onSend,
        )
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
            ) { BlockSpinner() }

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
