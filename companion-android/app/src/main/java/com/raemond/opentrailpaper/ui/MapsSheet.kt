package com.raemond.opentrailpaper.ui

import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Cancel
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.data.BoundingBox
import com.raemond.opentrailpaper.data.LatLon
import com.raemond.opentrailpaper.map.EInkTileStore
import com.raemond.opentrailpaper.map.H3Tiles
import com.raemond.opentrailpaper.map.MapTile
import com.raemond.opentrailpaper.map.OutlineHex
import com.raemond.opentrailpaper.map.PoiCache
import com.raemond.opentrailpaper.map.PrebuiltTiles
import com.raemond.opentrailpaper.map.CdnVersions
import com.raemond.opentrailpaper.map.SelectionHex
import com.raemond.opentrailpaper.map.MapDownloadProgress
import com.raemond.opentrailpaper.map.runMapDownload
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.util.Locale
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min

/**
 * Draw a box on the map; the app covers it with H3 hexagon tiles (~5.6 km
 * across), skips the ones already on the device, fetches OSM for the rest, and
 * streams each new tile to the device one at a time. The device stores every tile
 * by its H3 id and renders them straight off the SD card, so map coverage is
 * limited by the card, not memory — and re-selecting an overlapping area only
 * ever sends the genuinely new tiles.
 *
 * [seed] pre-fills the selection instead of asking for a box, which is how the
 * Route screen hands over the hexes a planned route crosses. Everything after
 * the selection — what counts as new, the OSM fetch, the streaming, progress,
 * cancel — is the same code either way; only the way the rider names an area
 * differs.
 *
 * It must be a value that does not change while this sheet is open. Opening the
 * sheet refreshes the tile store and the device's tile list, so a caller that
 * passes a list derived from those recomputes it as a NEW list the moment this
 * sheet appears — and a changing unstable parameter recomposes this whole
 * screen, map and all, in a loop that pins the main thread. Snapshot it at the
 * tap instead.
 */
@Composable
fun MapsSheet(
    ble: BleManager,
    seed: List<MapTile> = emptyList(),
    onDismiss: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    val projector = remember { MapProjector() }
    val haptics = LocalHapticFeedback.current
    val clipboard = LocalClipboardManager.current

    var region by remember { mutableStateOf<BoundingBox?>(null) }
    var outlines by remember { mutableStateOf<List<OutlineHex>>(emptyList()) }
    var camera by remember {
        mutableStateOf(MapCamera.region(LatLon(37.7764, -122.4346), 0.25))
    }
    /** The locator's fallback shot has been taken. */
    var didLocate by remember { mutableStateOf(false) }
    /** All the coverage has been framed — the opening shot this screen wants. */
    var didFrameCoverage by remember { mutableStateOf(false) }
    /**
     * Measured height of the floating card, so the map can frame around whatever
     * the card currently is — the hint, a selection summary and a send-progress
     * card are very different heights, and this screen swaps between them.
     */
    var cardHeightPx by remember { mutableStateOf(0) }

    var drawMode by remember { mutableStateOf(false) }
    var dragStart by remember { mutableStateOf<Offset?>(null) }
    var dragEnd by remember { mutableStateOf<Offset?>(null) }
    // Seeded too, not just [tiles]: the card is gated on having a box, so
    // without one a seeded selection draws its hexes on the map and then offers
    // the rider nothing to tap.
    var box by remember { mutableStateOf(BoundingBox.around(seed.flatMap { it.hexagon })) }
    var tiles by remember { mutableStateOf(seed) }
    var excluded by remember { mutableStateOf<Set<String>>(emptySet()) }
    var converted by remember { mutableStateOf<Set<String>>(emptySet()) }
    var failedHexes by remember { mutableStateOf<Set<String>>(emptySet()) }
    var downloadTotal by remember { mutableStateOf(0) }
    var building by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf<String?>(null) }
    var job by remember { mutableStateOf<Job?>(null) }
    var inspected by remember { mutableStateOf<Pair<String, Boolean>?>(null) }

    DisposableEffect(Unit) {
        ble.refreshDeviceMaps()
        ble.refreshDeviceTiles()
        EInkTileStore.refresh()
        onDispose { }
    }

    // A seeded selection frames itself, and claims the framing flag so the
    // frame-all-coverage shot below doesn't pull the camera off it a moment
    // later. Declared before that effect so it wins the flag.
    //
    // Waits for the map to report a region, which is the first moment it has a
    // size. Framing before that runs zoomToBoundingBox against a zero-sized
    // MapView, which spins the main thread until the system reports the app as
    // not responding. The frame-all-coverage effect below is safe for exactly
    // the same reason — it sits behind the same region guard.
    LaunchedEffect(region != null) {
        if (region == null || didFrameCoverage) return@LaunchedEffect
        val bounds = BoundingBox.around(seed.flatMap { it.hexagon }) ?: return@LaunchedEffect
        didFrameCoverage = true
        camera = MapCamera.box(bounds, paddingPx = 32)
    }

    // Hexes in the drawn box that aren't on the device yet, in whichever state the
    // download has reached. Areas already downloaded are NOT in here — they
    // already show as coverage hexagons, which says "you have this" better than a
    // selection tint would.
    // Every selected hex, coloured by what a download would do with it — the
    // same states the coverage uses, drawn heavier: new (accent), update (ochre,
    // hatched), current (green). Tapping still skips/keeps new hexes.
    val cdnRevision = CdnVersions.revision
    // The CDN versions of the selection (cached for an hour; a no-op once known).
    LaunchedEffect(tiles) { CdnVersions.load(tiles.map { it.id }) }
    val selectionHexes = remember(tiles, ble.deviceTileIds, ble.deviceSupportsPois, converted, excluded, cdnRevision) {
        tiles.map { t ->
            SelectionHex(
                t.id,
                t.hexagon,
                when {
                    t.id in converted -> SelectionHex.Kind.DONE
                    ble.tileIsCurrent(t.id) ->
                        if (ble.tileNeedsUpdate(t.id)) SelectionHex.Kind.UPDATE else SelectionHex.Kind.CURRENT
                    t.id in excluded -> SelectionHex.Kind.EXCLUDED
                    else -> SelectionHex.Kind.PENDING
                },
            )
        }
    }

    // Tiles that will actually be sent: not already on the device and not tapped
    // out by the user. Tiles on the device are only rebuilt when the rider asks
    // (Redownload).
    val newTiles = remember(tiles, ble.deviceTileIds, excluded) {
        tiles.filter { !ble.tileIsCurrent(it.id) && it.id !in excluded }
    }
    // Tiles whose cycling POIs go to the device: new ones, plus tiles already
    // there with no POIs or POIs older than PoiCache.MAX_AGE_MS. Empty for
    // firmware without POI support.
    val poiTiles = remember(tiles, ble.devicePoiIds, ble.deviceSupportsPois, excluded, cdnRevision) {
        tiles.filter { it.id !in excluded && ble.poiNeedsSend(it.id) }
    }
    val poiOnlyCount = poiTiles.count { t -> newTiles.none { it.id == t.id } }
    val onDeviceTiles = remember(tiles, ble.deviceTileIds) { tiles.filter { ble.tileIsCurrent(it.id) } }
    val onDeviceCount = onDeviceTiles.size
    // On the device but (as far as this phone knows) from before the
    // bike-route data — the hint under the Redownload button.
    val updateCount = remember(onDeviceTiles, ble.deviceSupportsPois, cdnRevision) {
        onDeviceTiles.count { ble.tileNeedsUpdate(it.id) }
    }
    // Updates known from the CDN index (a different hash than the device's).
    val newerDataCount = remember(onDeviceTiles, cdnRevision) {
        onDeviceTiles.count { ble.tileVersionState(it.id) == PrebuiltTiles.VersionState.UPDATE }
    }
    // On-device hexes a Redownload would send: all but those whose tile (and, on
    // firmware with POIs, .poi) is the CDN's current version. Hexes the CDN does
    // not have are rebuilt from Overpass as before.
    val redownloadCount = remember(onDeviceTiles, ble.deviceSupportsPois, ble.devicePoiIds, cdnRevision) {
        onDeviceTiles.count {
            ble.tileVersionState(it.id) != PrebuiltTiles.VersionState.CURRENT ||
                (ble.deviceSupportsPois && ble.poiVersionState(it.id) != PrebuiltTiles.VersionState.CURRENT)
        }
    }
    // "Map data from Oct 6": the OSM snapshot of the CDN's tiles for the selection.
    val dataDateText = remember(tiles, cdnRevision) {
        CdnVersions.dataDate(tiles.map { it.id })?.let {
            "Map data from " + java.text.SimpleDateFormat(
                android.text.format.DateFormat.getBestDateTimePattern(Locale.getDefault(), "MMMd"),
                Locale.getDefault(),
            ).format(java.util.Date(it))
        }
    }
    var confirmRedownload by remember { mutableStateOf(false) }
    // Redownload tapped with every selected hex already current.
    var confirmUpToDate by remember { mutableStateOf(false) }
    var showLayers by remember { mutableStateOf(false) }

    // Ask the store what to draw for the region now on screen. Coalesced along
    // with the device's tile list, which arrives tile-by-tile during an upload:
    // without this a large download rebuilds every overlay several times a second
    // and the page stutters badly once a few hundred hexes are on screen.
    LaunchedEffect(region, EInkTileStore.version, ble.deviceTileIds, ble.devicePoiIds, ble.deviceSupportsPois, cdnRevision) {
        delay(250)
        val r = region ?: return@LaunchedEffect
        val visible = EInkTileStore.visibleContent(r, ble.deviceTileIds)
        outlines = visible.map {
            if (it.synced) it.withState(ble.tileNeedsUpdate(it.id), ble.poiMark(it.id)) else it
        }
        // Look up the CDN versions of the device's hexes on screen; the revision
        // bump redraws them (cached for an hour; a no-op once known).
        scope.launch { CdnVersions.load(visible.filter { it.synced }.map { it.id }) }

        // Frame ALL the coverage — everything the phone holds plus everything the
        // device holds — the first time this screen has something to frame.
        // Managing downloaded areas starts with seeing them, and a fixed ~22 km
        // box on the rider shows one screenful of a collection that can span a
        // country.
        //
        // Takes precedence over the locate-the-rider fallback below: the tile scan
        // is asynchronous, so it reliably loses a race against a location fix that
        // is often already cached. Neither one ever moves the camera under a box
        // being drawn.
        if (!didFrameCoverage && box == null && !drawMode) {
            val ids = EInkTileStore.ids + ble.deviceTileIds
            val corners = ids.mapNotNull { H3Tiles.tile(it) }.flatMap { it.hexagon }
            val bounds = BoundingBox.around(corners)
            if (bounds != null) {
                didFrameCoverage = true
                // Less padding than a route gets: the hexes ARE the subject here.
                // The card's own share of the map is handled by the map's bottom
                // inset, so it must not be padded for a second time.
                camera = MapCamera.box(bounds, paddingPx = 32)
            }
        }
    }

    // Centre on the rider's first fix, once, at our fixed tile-friendly span —
    // only as the FALLBACK for someone with no coverage yet, and never while a box
    // is being drawn.
    LaunchedEffect(ble.lastLocation) {
        val loc = ble.lastLocation ?: return@LaunchedEffect
        if (didLocate || didFrameCoverage || box != null || drawMode) return@LaunchedEffect
        didLocate = true
        camera = MapCamera.region(LatLon(loc.latitude, loc.longitude), 0.25)
    }

    fun cancelDownload() {
        job?.cancel()
        job = null
        building = false
        ble.cancelTileUpload()
        status = "Canceled"
    }

    /**
     * [redownload]: the selected hexes the device already has, re-fetched and
     * re-sent with their POIs — only those whose CDN hash differs from what the
     * device has (or that the CDN does not have: rebuilt from Overpass as
     * before). [force] re-sends every one (repairing a card). Otherwise: new
     * hexes, plus POIs that are missing or changed.
     */
    fun startDownload(redownload: Boolean = false, force: Boolean = false) {
        if (building || ble.tilesUploading) return   // one job at a time
        val missing0 = if (redownload) onDeviceTiles else newTiles
        val poiWork0 = when {
            !redownload -> poiTiles
            ble.deviceSupportsPois -> onDeviceTiles
            else -> emptyList()
        }
        if (missing0.isEmpty() && poiWork0.isEmpty()) return
        building = true
        converted = emptySet()
        failedHexes = emptySet()
        downloadTotal = missing0.size + poiWork0.size
        status = when {
            redownload -> "Checking for newer map data…"
            missing0.isEmpty() -> "Fetching cycling POIs…"
            else -> "Fetching map data…"
        }
        val tileRecords = ble.tileRecords
        val poiRecords = ble.poiRecords
        val devicePois = ble.devicePoiIds

        job = scope.launch {
            try {
                val result = runMapDownload(
                    ble, missing0, poiWork0, redownload, force,
                    tileRecords, poiRecords, devicePois,
                    MapDownloadProgress(
                        status = { status = it },
                        total = { downloadTotal = it },
                        built = { ids ->
                            converted = converted + ids
                            EInkTileStore.noteDownloaded(ids)
                        },
                        failed = { ids -> failedHexes = failedHexes + ids },
                    ),
                )
                building = false
                status = result
            } catch (_: CancellationException) {
                building = false
                status = "Canceled"
            }
        }
    }

    FullScreenCover(onDismiss = onDismiss) {
        Box(Modifier.fillMaxSize()) {
            OsmMap(
                modifier = Modifier.fillMaxSize(),
                outlines = outlines,
                selection = selectionHexes,
                camera = camera,
                showUserLocation = ble.locationPermission.isGranted,
                bottomInsetPx = cardHeightPx,
                projector = projector,
                // Tap a hex (once an area is drawn) to skip/keep it.
                onTap = { c ->
                    if (box != null && !drawMode) {
                        val id = H3Tiles.idAt(c)
                        if (id != null && tiles.any { it.id == id } && !ble.tileIsCurrent(id)) {
                            excluded = if (id in excluded) excluded - id else excluded + id
                        }
                    }
                },
                // Long-press any hex to read its id. Deliberately NOT gated on an
                // area being drawn — inspecting a hex already on the device is the
                // more useful case of the two.
                onLongPress = { c ->
                    if (!drawMode) {
                        H3Tiles.idAt(c)?.let {
                            inspected = it to (it in ble.deviceTileIds)
                            haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                        }
                    }
                },
                onRegionChange = { region = it },
            )

            // In draw mode a transparent layer captures the drag so the map
            // doesn't pan while you draw a box.
            if (drawMode) {
                Box(
                    Modifier
                        .fillMaxSize()
                        .pointerInput(Unit) {
                            detectDragGestures(
                                onDragStart = { dragStart = it; dragEnd = it },
                                onDragEnd = {
                                    val a = dragStart
                                    val b = dragEnd
                                    if (a != null && b != null) {
                                        val c1 = projector.coordinate(a.x, a.y)
                                        val c2 = projector.coordinate(b.x, b.y)
                                        if (c1 != null && c2 != null) {
                                            val bx = BoundingBox(
                                                min(c1.lat, c2.lat), min(c1.lon, c2.lon),
                                                max(c1.lat, c2.lat), max(c1.lon, c2.lon),
                                            )
                                            box = bx
                                            tiles = H3Tiles.coveringTiles(
                                                bx.south, bx.west, bx.north, bx.east,
                                            )
                                            excluded = emptySet()
                                            converted = emptySet()
                                            status = null
                                        }
                                    }
                                    dragStart = null
                                    dragEnd = null
                                    drawMode = false
                                },
                                onDragCancel = { dragStart = null; dragEnd = null },
                                onDrag = { change, _ -> dragEnd = change.position },
                            )
                        },
                )
            }

            val a = dragStart
            val b = dragEnd
            if (a != null && b != null) {
                androidx.compose.foundation.Canvas(Modifier.fillMaxSize()) {
                    val topLeft = Offset(min(a.x, b.x), min(a.y, b.y))
                    val boxSize = Size(abs(b.x - a.x), abs(b.y - a.y))
                    drawRect(Palette.accent.copy(alpha = 0.18f), topLeft, boxSize)
                    drawRect(Palette.accent, topLeft, boxSize, style = Stroke(width = 2f))
                }
            }

            // The header floats over the map, as it does on iOS — and it has to
            // be composed AFTER the map: the map is an Android view, which the
            // platform draws above any Compose content composed before it.
            Row(
                Modifier
                    .align(Alignment.TopCenter)
                    .fillMaxWidth()
                    .windowInsetsPadding(WindowInsets.statusBars)
                    .padding(horizontal = 16.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                HeaderPill("Maps", filled = false) {}
                Spacer(Modifier.weight(1f))
                // The DEVICE's map layers (what the head unit draws), not this
                // screen's. Hidden for firmware that cannot switch them.
                if (ble.mapLayersSupported) {
                    HeaderPill("Layers", filled = false) { showLayers = true }
                    Spacer(Modifier.size(8.dp))
                }
                HeaderPill(
                    if (drawMode) "Cancel" else "Select area",
                    filled = drawMode,
                ) {
                    box = null
                    tiles = emptyList()
                    excluded = emptySet()
                    converted = emptySet()
                    dragStart = null
                    dragEnd = null
                    drawMode = !drawMode
                }
                Spacer(Modifier.size(8.dp))
                HeaderPill("Done", filled = false, onClick = onDismiss)
            }

            // Key to the hex colours, once there is something to explain.
            if (outlines.isNotEmpty() || box != null) {
                Legend(
                    showNew = box != null,
                    showPois = ble.deviceSupportsPois,
                    modifier = Modifier
                        .align(Alignment.TopStart)
                        .windowInsetsPadding(WindowInsets.statusBars)
                        .padding(start = 16.dp, top = 64.dp),
                )
            }

            // A floating "modal" card at the bottom. Progress states show a
            // spinner + live hex counts; idle/selection states show the controls.
            //
            // Its height is MEASURED rather than assumed, and reported to the map
            // as a bottom inset: the three states it swaps between are very
            // different heights, and "fit all my coverage" must not fit half the
            // hexes behind whichever one is showing.
            Box(
                Modifier
                    .align(Alignment.BottomCenter)
                    .onSizeChanged { cardHeightPx = it.height }
                    .padding(horizontal = 16.dp, vertical = 10.dp),
            ) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (ble.sdCardBlocksMaps) SdCardNotice(ble.status.sdCard)
                    when {
                        building || ble.tilesUploading -> StreamCard(
                            building = building,
                            buildStatus = status,
                            tileMessage = ble.tileMessage,
                            sent = ble.tilesDone,
                            total = max(max(ble.tilesTotal, downloadTotal), 1),
                            built = converted.size,
                            onCancel = { cancelDownload() },
                        )

                        box != null -> SelectionCard(
                            box = box!!,
                            newCount = newTiles.size,
                            poiOnlyCount = poiOnlyCount,
                            onDeviceCount = onDeviceCount,
                            updateCount = updateCount,
                            newerDataCount = newerDataCount,
                            redownloadCount = redownloadCount,
                            dataDateText = dataDateText,
                            onRedownload = {
                                when {
                                    redownloadCount == 0 -> confirmUpToDate = true
                                    redownloadCount > REDOWNLOAD_CONFIRM_OVER -> confirmRedownload = true
                                    else -> startDownload(redownload = true)
                                }
                            },
                            skipped = excluded.count { id -> tiles.any { it.id == id } },
                            canSend = ble.canUploadMap && !ble.sdCardBlocksMaps,
                            needsCard = ble.canUploadMap && ble.sdCardBlocksMaps,
                            status = status,
                            onDownload = { startDownload() },
                            onClear = {
                                box = null
                                tiles = emptyList()
                                excluded = emptySet()
                                converted = emptySet()
                            },
                        )

                        else -> HintCard(drawMode, ble.deviceTileIds.size)
                    }
                }
            }
        }
    }

    if (showLayers) {
        DeviceMapLayersDialog(ble) { showLayers = false }
    }

    if (confirmRedownload) {
        AlertDialog(
            onDismissRequest = { confirmRedownload = false },
            title = { Text("Redownload $redownloadCount hexes?") },
            text = {
                Text(
                    "Hexes with newer map data are fetched and sent to the device again, with " +
                        "bike routes and water stops. Hexes that are already current are skipped. " +
                        "This takes a while and uses data.",
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmRedownload = false
                    startDownload(redownload = true)
                }) { Text("Redownload") }
            },
            dismissButton = { TextButton(onClick = { confirmRedownload = false }) { Text("Cancel") } },
            containerColor = Palette.surface,
        )
    }

    // Nothing newer to fetch. Re-sending the same bytes only helps a damaged
    // card, so it is offered but not the default.
    if (confirmUpToDate) {
        AlertDialog(
            onDismissRequest = { confirmUpToDate = false },
            title = { Text("All $onDeviceCount hexes are up to date") },
            text = {
                Text(
                    "The device already has the newest published map data for each. " +
                        "Re-send only to repair a damaged card.",
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmUpToDate = false
                    startDownload(redownload = true, force = true)
                }) { Text("Re-send anyway") }
            },
            dismissButton = { TextButton(onClick = { confirmUpToDate = false }) { Text("OK") } },
            containerColor = Palette.surface,
        )
    }

    inspected?.let { (id, onDevice) ->
        TileInspectorSheet(id, onDevice, onCopy = {
            clipboard.setText(AnnotatedString(id))
        }) { inspected = null }
    }
}

/**
 * The switches for what the DEVICE's map draws — bike-route bands, cycleways
 * and lanes, and each POI type. Applied as soon as they change when the device
 * is connected; otherwise shown as last known and sent on reconnect.
 */
@Composable
private fun DeviceMapLayersDialog(ble: BleManager, onDismiss: () -> Unit) {
    val rows = listOf(
        Triple(0x01, "Bike routes", "Grey bands along signed bike routes"),
        Triple(0x02, "Cycleways & bike lanes", "Dashed cycleways, dotted lane edges"),
        Triple(0x04, "Drinking water", "Water drop icons"),
        Triple(0x08, "Toilets", "Restroom icons"),
        Triple(0x10, "Repair stations", "Self-service repair stands"),
        Triple(0x20, "Bike shops", "Bicycle shop icons"),
    )
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Device map layers") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                for ((bit, title, detail) in rows) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(title, style = barlow(16.sp, FontWeight.SemiBold), color = Palette.ink)
                            Text(detail, style = barlow(12.sp), color = Palette.muted)
                        }
                        EinkSwitch(
                            checked = ble.mapLayers and bit != 0,
                            onCheckedChange = { on ->
                                ble.updateMapLayers(if (on) ble.mapLayers or bit else ble.mapLayers and bit.inv())
                            },
                        )
                    }
                }
                Text(
                    if (ble.canUploadMap) {
                        "What the device's map shows. Layers only appear where the downloaded hexes include them."
                    } else {
                        "Not connected — showing the last known setting. Changes are sent the next time the device connects."
                    },
                    style = barlow(12.sp),
                    color = Palette.muted,
                )
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Done") } },
        containerColor = Palette.surface,
    )
}

@Composable
private fun HeaderPill(label: String, filled: Boolean, onClick: () -> Unit) {
    Text(
        label,
        style = condensed(18.sp, FontWeight.SemiBold),
        color = if (filled) Palette.accentInk else Palette.accent,
        modifier = Modifier
            .background(
                if (filled) Palette.accent else Palette.surface,
                RectangleShape,
            )
            .border(Rule.border, Palette.hairline)
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 9.dp),
    )
}

/** Hexes above this many ask before a Redownload: each one is an Overpass fetch,
 *  an elevation fetch and a BLE transfer. */
private const val REDOWNLOAD_CONFIRM_OVER = 20

// MARK: bottom cards

@Composable
private fun FloatingCard(content: @Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit) {
    Column(
        Modifier
            .fillMaxWidth()
            .background(Palette.surface)
            .border(Rule.border, Palette.hairline)
            .padding(16.dp),
        content = content,
    )
}

@Composable
private fun StreamCard(
    building: Boolean,
    buildStatus: String?,
    tileMessage: String?,
    sent: Int,
    total: Int,
    built: Int,
    onCancel: () -> Unit,
) {
    FloatingCard {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            BlockSpinner()
            Column(Modifier.weight(1f)) {
                Text(
                    if (building) "Downloading maps" else "Sending to device",
                    style = condensed(19.sp, FontWeight.SemiBold),
                    color = Palette.ink,
                )
                Text(
                    if (building) buildStatus ?: "Fetching…" else tileMessage ?: "Uploading hexes…",
                    style = barlow(12.sp),
                    color = Palette.muted,
                    maxLines = 1,
                )
            }
            TextButton(onClick = onCancel) {
                Text("Cancel", style = barlow(14.sp, FontWeight.SemiBold), color = Palette.accent)
            }
        }
        Spacer(Modifier.size(10.dp))
        BlockProgressBar(value = sent.toFloat() / total, modifier = Modifier.fillMaxWidth())
        Text(
            "$sent of $total sent" +
                if (building && built > sent) " · $built built" else "",
            style = barlow(12.sp, FontWeight.SemiBold),
            color = Palette.good,
        )
    }
}

@Composable
private fun SelectionCard(
    box: BoundingBox,
    newCount: Int,
    poiOnlyCount: Int,
    onDeviceCount: Int,
    updateCount: Int,
    newerDataCount: Int,
    redownloadCount: Int,
    dataDateText: String?,
    skipped: Int,
    canSend: Boolean,
    needsCard: Boolean,
    status: String?,
    onDownload: () -> Unit,
    onRedownload: () -> Unit,
    onClear: () -> Unit,
) {
    FloatingCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                TrackedLabel("Selected area")
                Text(
                    areaText(box),
                    style = barlow(15.sp, FontWeight.SemiBold),
                    color = Palette.ink,
                )
                dataDateText?.let { Text(it, style = barlow(11.sp), color = Palette.muted) }
            }
            IconButton(onClick = onClear) {
                Icon(Icons.Filled.Cancel, contentDescription = "Clear", tint = Palette.muted)
            }
        }
        // The same three states the map draws, in the same colours.
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
            CountChip(newCount, "new", Palette.accent)
            CountChip(updateCount, if (updateCount == 1) "update" else "updates", Palette.update)
            CountChip(onDeviceCount - updateCount, "current", Palette.good)
        }
        Text(
            (if (poiOnlyCount > 0) "$poiOnlyCount need POIs" else "POIs up to date") +
                (if (skipped > 0) " · $skipped skipped" else ""),
            style = barlow(12.sp),
            color = Palette.muted,
        )
        Text(
            "Tap a hex to skip it (or add it back).",
            style = barlow(11.sp),
            color = Palette.faint,
        )
        if (updateCount > 0) {
            // "3 hexes have newer data · 2 hexes were made before bike routes …"
            val old = updateCount - newerDataCount
            val parts = listOfNotNull(
                if (newerDataCount > 0) "$newerDataCount hex${if (newerDataCount == 1) " has" else "es have"} newer data" else null,
                if (old > 0) {
                    "$old hex${if (old == 1) " was" else "es were"} made before bike routes & water stops, or over 90 days ago"
                } else null,
            )
            Text(
                parts.joinToString(" · ") + " — redownload to refresh.",
                style = barlow(12.sp),
                color = Palette.ink,
            )
        }
        Spacer(Modifier.size(10.dp))
        PrimaryButton(
            title = when {
                needsCard -> "Device needs an SD card"
                !canSend -> "Connect device to send"
                newCount > 0 -> "Download $newCount hex${if (newCount == 1) "" else "es"}"
                poiOnlyCount > 0 -> "Send POIs for $poiOnlyCount hex${if (poiOnlyCount == 1) "" else "es"}"
                else -> "Nothing to download"
            },
            icon = Icons.Filled.Download,
            enabled = canSend && (newCount > 0 || poiOnlyCount > 0),
            onClick = onDownload,
        )
        // Rebuild hexes the device already has from fresh map data — the rider's
        // choice, never automatic: it costs a fetch and a transfer per hex.
        // Offered on old firmware too (fresher roads), where it carries no POIs.
        // Only hexes with newer data go out; when every one is current it says so
        // and offers a forced re-send for repairs.
        if (onDeviceCount > 0) {
            Spacer(Modifier.size(8.dp))
            val n = if (redownloadCount > 0) redownloadCount else onDeviceCount
            SecondaryButton(
                title = "Redownload $n hex${if (n == 1) "" else "es"}",
                icon = Icons.Filled.Refresh,
                enabled = canSend,
                onClick = onRedownload,
            )
        }
        status?.let {
            Text(it, style = barlow(12.sp), color = Palette.accent)
        }
    }
}

@Composable
private fun CountChip(n: Int, label: String, color: androidx.compose.ui.graphics.Color) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(5.dp)) {
        Box(
            Modifier
                .size(12.dp)
                .background(color.copy(alpha = 0.35f))
                .border(1.5.dp, color),
        )
        Text("$n $label", style = barlow(13.sp, FontWeight.SemiBold), color = Palette.ink)
    }
}

/** Compact key to the hex colours and badges, top-left under the header. */
@Composable
private fun Legend(showNew: Boolean, showPois: Boolean, modifier: Modifier = Modifier) {
    Column(
        modifier
            .background(Palette.surface.copy(alpha = 0.92f))
            .border(Rule.border, Palette.hairline)
            .padding(8.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        LegendRow(Palette.good, hatched = false, "On device")
        LegendRow(Palette.update, hatched = true, "Update available")
        if (showNew) LegendRow(Palette.accent, hatched = false, "New")
        LegendRow(Palette.muted, hatched = false, "On this phone")
        if (showPois) {
            Text("● / ○ drop: POIs on device / none or old", style = barlow(11.sp), color = Palette.ink)
        }
    }
}

@Composable
private fun LegendRow(color: androidx.compose.ui.graphics.Color, hatched: Boolean, text: String) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        androidx.compose.foundation.Canvas(Modifier.size(12.dp)) {
            drawRect(color.copy(alpha = 0.3f))
            if (hatched) {
                var x = -size.height
                while (x < size.width) {
                    drawLine(color.copy(alpha = 0.7f), Offset(x, size.height), Offset(x + size.height, 0f), 1.5f)
                    x += size.width / 3
                }
            }
            drawRect(color, style = Stroke(width = 1.5.dp.toPx()))
        }
        Text(text, style = barlow(11.sp), color = Palette.ink)
    }
}

@Composable
private fun HintCard(drawMode: Boolean, deviceHexes: Int) {
    FloatingCard {
        Text(
            if (drawMode) {
                "Drag a box across the area you want."
            } else {
                "Tap “Select area”, then drag a box. Green hexes are on the device; ochre, " +
                    "hatched ones have an update. A filled drop means the device has their " +
                    "water stops and bike shops."
            },
            style = barlow(14.sp),
            color = if (drawMode) Palette.accent else Palette.muted,
        )
        if (deviceHexes > 0) {
            Text(
                "$deviceHexes hexes on the device",
                style = barlow(11.sp),
                color = Palette.muted,
            )
        }
    }
}

private fun areaText(b: BoundingBox): String {
    val latKm = (b.north - b.south) * 111.0
    val lonKm = (b.east - b.west) * 111.0 * cos((b.south + b.north) / 2 * Math.PI / 180)
    return String.format(Locale.US, "%.1f × %.1f km", lonKm, latKm)
}

/**
 * Shows a hex's H3 id and where it lives on the card.
 *
 * The id is the thing every other part of the system names a tile by: the
 * filename on the SD card, the tile list the app and device reconcile, and what a
 * diag log prints. When one specific hex misbehaves — an ocean tile that will not
 * fill, a hex with no roads — being able to read its id off the map turns
 * "somewhere around here" into something greppable.
 */
@Composable
private fun TileInspectorSheet(
    id: String,
    onDevice: Boolean,
    onCopy: () -> Unit,
    onDismiss: () -> Unit,
) {
    // Matches src/map_store.cpp: /maps/tiles/<first 6>/<rest>.ebm
    val cardPath = if (id.length > 6) {
        "/maps/tiles/${id.take(6)}/${id.drop(6)}.ebm"
    } else {
        "/maps/tiles/$id.ebm"
    }

    FullScreenSheet(title = "Tile", onDismiss = onDismiss) {
        Column(
            Modifier.fillMaxSize().padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Card {
                TrackedLabel("H3 cell")
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        id,
                        style = TypeScale.body.copy(
                            fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace,
                        ),
                        color = Palette.ink,
                        modifier = Modifier.weight(1f),
                    )
                    TextButton(onClick = onCopy) {
                        Text("Copy", color = Palette.accent, style = barlow(13.sp))
                    }
                }
            }
            Card {
                TrackedLabel("On the SD card")
                Text(
                    cardPath,
                    style = barlow(12.sp).copy(
                        fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace,
                    ),
                    color = Palette.muted,
                )
            }
            Card {
                TrackedLabel("Status")
                Text(
                    if (onDevice) "On the device" else "Not on the device",
                    style = TypeScale.body,
                    color = if (onDevice) Palette.good else Palette.muted,
                )
            }
        }
    }
}
