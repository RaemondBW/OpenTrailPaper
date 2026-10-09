package com.raemond.opentrailpaper.ui

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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Cancel
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
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
import com.raemond.opentrailpaper.map.MapBuilder
import com.raemond.opentrailpaper.map.MapTile
import com.raemond.opentrailpaper.map.OsmData
import com.raemond.opentrailpaper.map.OutlineHex
import com.raemond.opentrailpaper.map.PoiCache
import com.raemond.opentrailpaper.map.Cycling
import com.raemond.opentrailpaper.map.SelectionHex
import com.raemond.opentrailpaper.map.TileCache
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.util.Locale
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.floor
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
    val selectionHexes = remember(tiles, ble.deviceTileIds, ble.deviceSupportsPois, converted, excluded) {
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
    val poiTiles = remember(tiles, ble.devicePoiIds, ble.deviceSupportsPois, excluded) {
        tiles.filter { it.id !in excluded && ble.poiNeedsSend(it.id) }
    }
    val poiOnlyCount = poiTiles.count { t -> newTiles.none { it.id == t.id } }
    val onDeviceTiles = remember(tiles, ble.deviceTileIds) { tiles.filter { ble.tileIsCurrent(it.id) } }
    val onDeviceCount = onDeviceTiles.size
    // On the device but (as far as this phone knows) from before the
    // bike-route data — the hint under the Redownload button.
    val updateCount = remember(onDeviceTiles, ble.deviceSupportsPois) {
        onDeviceTiles.count { ble.tileNeedsUpdate(it.id) }
    }
    var confirmRedownload by remember { mutableStateOf(false) }

    // Ask the store what to draw for the region now on screen. Coalesced along
    // with the device's tile list, which arrives tile-by-tile during an upload:
    // without this a large download rebuilds every overlay several times a second
    // and the page stutters badly once a few hundred hexes are on screen.
    LaunchedEffect(region, EInkTileStore.version, ble.deviceTileIds, ble.devicePoiIds, ble.deviceSupportsPois) {
        delay(250)
        val r = region ?: return@LaunchedEffect
        outlines = EInkTileStore.visibleContent(r, ble.deviceTileIds).map {
            if (it.synced) it.withState(ble.tileNeedsUpdate(it.id), ble.poiMark(it.id)) else it
        }

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
     * [redownload]: rebuild the selected hexes the device already has from fresh
     * Overpass data (skipping the phone's caches) and re-send them with their
     * POIs. Otherwise: new hexes, plus POIs that are missing or stale.
     */
    fun startDownload(redownload: Boolean = false) {
        if (building || ble.tilesUploading) return   // one job at a time
        val missing = if (redownload) onDeviceTiles else newTiles
        val poiWork = when {
            !redownload -> poiTiles
            ble.deviceSupportsPois -> onDeviceTiles
            else -> emptyList()
        }
        if (missing.isEmpty() && poiWork.isEmpty()) return
        building = true
        converted = emptySet()
        failedHexes = emptySet()
        downloadTotal = missing.size + poiWork.size
        status = if (missing.isEmpty()) "Fetching cycling POIs…" else "Fetching map data…"
        ble.startTileStream(resend = redownload)   // begin sending as tiles are produced

        job = scope.launch {
            com.raemond.opentrailpaper.map.DownloadStats.reset()
            try {
                // Cycling POIs: their own small query and cache, started NOW so
                // they arrive while the map batches are still building. A failure
                // here never fails the map download — the map is what matters.
                val poisJob = async {
                    if (poiWork.isEmpty()) return@async true
                    try {
                        downloadPois(poiWork, useCache = !redownload, onStatus = {}, enqueue = { ble.enqueuePois(it) })
                        true
                    } catch (e: CancellationException) {
                        throw e
                    } catch (_: Exception) {
                        false
                    }
                }
                if (missing.isNotEmpty()) {
                    downloadTiles(
                        missing = missing,
                        useCache = !redownload,
                        onStatus = { status = it },
                        onBuilt = { ids ->
                            converted = converted + ids
                            EInkTileStore.noteDownloaded(ids)
                        },
                        onFailed = { ids -> failedHexes = failedHexes + ids },
                        enqueue = { ble.enqueueTiles(it) },
                    )
                }
                val poiNote = if (poisJob.await()) null
                              else "Cycling POIs could not be fetched — try again later."
                com.raemond.opentrailpaper.map.DownloadStats.log(missing.size + poiWork.size)
                building = false
                ble.finishTileStream()                     // let the queue drain
                // The cache has a size ceiling and this is the only thing that
                // grows it, so this is where it gets enforced.
                TileCache.trim()
                status = when {
                    failedHexes.isNotEmpty() ->
                        "${failedHexes.size} hex${if (failedHexes.size == 1) "" else "es"} had " +
                            "no map data — tap Select area and retry them."

                    missing.isEmpty() -> poiNote
                    converted.isEmpty() -> "No roads found in that area."
                    else -> poiNote
                }
            } catch (_: CancellationException) {
                building = false
                ble.cancelTileUpload()
                status = "Canceled"
            } catch (e: Exception) {
                building = false
                ble.finishTileStream()                     // send whatever built first
                status = e.message ?: "Map download failed"
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
                        onRedownload = {
                            if (onDeviceCount > REDOWNLOAD_CONFIRM_OVER) confirmRedownload = true
                            else startDownload(redownload = true)
                        },
                        skipped = excluded.count { id -> tiles.any { it.id == id } },
                        canSend = ble.canUploadMap,
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

    if (confirmRedownload) {
        AlertDialog(
            onDismissRequest = { confirmRedownload = false },
            title = { Text("Redownload $onDeviceCount hexes?") },
            text = {
                Text(
                    "They are rebuilt from fresh OpenStreetMap data and sent to the device " +
                        "again, with bike routes and water stops. This takes a while and uses data.",
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

    inspected?.let { (id, onDevice) ->
        TileInspectorSheet(id, onDevice, onCopy = {
            clipboard.setText(AnnotatedString(id))
        }) { inspected = null }
    }
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
                RoundedCornerShape(50),
            )
            .border(1.dp, Palette.hairline, RoundedCornerShape(50))
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 9.dp),
    )
}

// MARK: the download pipeline

/**
 * Fetch, build and stream every missing tile.
 *
 * Download, vectorization and the BLE send all run in parallel: a batch is
 * enqueued the moment it is built, so the link is busy while the next Overpass
 * request is in flight.
 */
private suspend fun downloadTiles(
    missing: List<MapTile>,
    useCache: Boolean,
    onStatus: (String?) -> Unit,
    onBuilt: (List<String>) -> Unit,
    onFailed: (List<String>) -> Unit,
    enqueue: (List<Pair<String, ByteArray>>) -> Unit,
) {
    // Anything built before goes straight out — no Overpass, no elevation fetch,
    // no re-encoding. This is what makes a retry after a dropped link cheap
    // instead of a full rebuild.
    // (Not on a Redownload: that is fresh data by definition.)
    val cached = if (useCache) TileCache.partition(missing.map { it.id }).first else emptyList()
    if (cached.isNotEmpty()) {
        onStatus("Reusing ${cached.size} cached tile${if (cached.size == 1) "" else "s"}…")
        onBuilt(cached.map { it.first })
        enqueue(cached)
    }
    val cachedIds = cached.map { it.first }.toSet()

    // Group tiles into bounded OSM fetches (~0.08° ≈ 9 km) so each Overpass query
    // stays light — big queries 504 on the busy public servers.
    val batches = missing
        .filter { it.id !in cachedIds }
        .groupBy { t ->
            val clat = (t.south + t.north) / 2
            val clon = (t.west + t.east) / 2
            "${floor(clat / 0.08).toInt()}_${floor(clon / 0.08).toInt()}"
        }
        .values.toList()
    if (batches.isEmpty()) return

    // ONE coastline fetch for the whole selection, padded well past it. Sea fill
    // needs the COAST, and an ocean-only selection does not contain any — the
    // batch bbox for hexes out in open water has no coastline in it, so no rings
    // could be assembled and those tiles came out blank. Padding by ~0.35° (~35 km)
    // reaches the shore from anywhere a rider would sensibly select, and is cheap
    // because it is coastline-only. It runs ALONGSIDE the first map batches; each
    // batch waits for it only after its own fetch, when it needs the sea rings.
    val all = union(missing)
    val pad = 0.35
    coroutineScope {
        val seaRingsAsync = async {
            val coastChains = runCatching {
                val coastOsm = MapBuilder.fetchCoastline(
                    all.south - pad, all.west - pad, all.north + pad, all.east + pad,
                )
                MapBuilder.coastlineChains(coastOsm)
            }.getOrDefault(emptyList())
            // Rings are assembled against the PADDED region, not each batch's
            // bbox, so a batch sitting entirely offshore is still inside a ring.
            withContext(Dispatchers.Default) {
                MapBuilder.regionSeaPolygons(
                    coastChains,
                    all.south - pad, all.west - pad, all.north + pad, all.east + pad,
                )
            }
        }

        // Batches run MapBuilder.CONCURRENT_BATCHES at a time, each starting on
        // its own mirror; each is cached and queued for sending as soon as it is
        // built, so the link stays busy while the next fetch is in flight.
        val gate = Semaphore(MapBuilder.CONCURRENT_BATCHES)
        var done = 0
        onStatus("Fetching ${batches.size} area${if (batches.size == 1) "" else "s"}…")
        batches.mapIndexed { i, batch ->
            launch {
                gate.withPermit {
                    val produced = buildBatch(batch, i, seaRingsAsync)
                    done += 1
                    onStatus("Built $done of ${batches.size} area${if (batches.size == 1) "" else "s"}…")
                    // Mark ONLY what was actually produced. Marking every id in
                    // the batch would fill the map in as downloaded and report
                    // success while nothing had been sent or stored.
                    val producedIds = produced.map { it.first }.toSet()
                    onBuilt(producedIds.toList())
                    val missed = batch.map { it.id }.filter { it !in producedIds }
                    if (missed.isNotEmpty()) onFailed(missed)
                    // Cache BEFORE sending: if the link drops mid-transfer the
                    // expensive work survives and the retry is instant.
                    TileCache.store(produced)
                    enqueue(produced)
                }
            }
        }
    }
}

/**
 * Fetch, encode and finish one batch of hexes: roads (+ way flags), DEM
 * elevation (a few hexes at a time), water and sea fill, parks. Returns the
 * non-empty tiles.
 */
private suspend fun buildBatch(
    batch: List<MapTile>,
    index: Int,
    seaRingsAsync: kotlinx.coroutines.Deferred<List<List<DoubleArray>>>,
): List<Pair<String, ByteArray>> {
    val u = union(batch)
    val osm = MapBuilder.fetchOsm(u.south, u.west, u.north, u.east, startMirror = index)
    val t0 = System.nanoTime()
    val encoded = withContext(Dispatchers.Default) { MapBuilder.encodeTiles(osm, batch) }
    // natural=water and park polygons for this region, resolved once and
    // appended per tile as WTR2 / PRK2 sections (after any ELV1 block).
    val waterWays = withContext(Dispatchers.Default) { MapBuilder.waterWays(osm) }
    val parkWays = withContext(Dispatchers.Default) { MapBuilder.parkWays(osm) }
    com.raemond.opentrailpaper.map.DownloadStats.add("encode", (System.nanoTime() - t0) / 1e9)
    val seaRings = seaRingsAsync.await()
    // Bake a DEM elevation grid into each tile (best-effort) so the device has
    // elevation without GPS altitude or the phone.
    val elevGate = Semaphore(MapBuilder.CONCURRENT_ELEVATION)
    val grids = coroutineScope {
        batch.map { t ->
            async {
                elevGate.withPermit {
                    t.id to runCatching {
                        MapBuilder.fetchElevationGrid(t.south, t.west, t.north, t.east)
                    }.getOrNull()
                }
            }
        }.awaitAll().toMap()
    }
    val t1 = System.nanoTime()
    val produced = ArrayList<Pair<String, ByteArray>>(encoded.size)
    withContext(Dispatchers.Default) {
        for ((id, roads) in encoded) {
            val tile = batch.first { it.id == id }
            val out = ByteArrayOutputStream(roads.size + 4096)
            out.write(roads)
            grids[id]?.let { grid ->
                MapBuilder.appendElevation(
                    out, tile.south, tile.west, tile.north, tile.east, grid, MapBuilder.ELEVATION_GRID,
                )
            }
            MapBuilder.appendWater(out, waterWays, seaRings, tile.south, tile.west, tile.north, tile.east)
            MapBuilder.appendParks(out, parkWays, tile.south, tile.west, tile.north, tile.east)
            val data = out.toByteArray()
            // Decide emptiness only now, with water/parks/sea/elevation already
            // appended — a hex can be pure water and still be worth storing.
            if (MapBuilder.isEmpty(data, tile)) continue
            produced.add(id to data)
        }
    }
    com.raemond.opentrailpaper.map.DownloadStats.add("encode", (System.nanoTime() - t1) / 1e9)
    return produced
}

/**
 * Build (or reuse) and queue the `.poi` files for [work]. One POI query per
 * ~0.25° group of tiles: the query is light, so groups can be far bigger than
 * the map batches. Each POI is stored in the one H3 cell containing it.
 */
private suspend fun downloadPois(
    work: List<MapTile>,
    useCache: Boolean,
    onStatus: (String?) -> Unit,
    enqueue: (List<Pair<String, ByteArray>>) -> Unit,
) {
    val (cached, missingIds) =
        if (useCache) PoiCache.partition(work.map { it.id }) else emptyList<Pair<String, ByteArray>>() to work.map { it.id }
    if (cached.isNotEmpty()) enqueue(cached)
    val need = missingIds.toSet()
    val groups = work.filter { it.id in need }.groupBy { t ->
        val clat = (t.south + t.north) / 2
        val clon = (t.west + t.east) / 2
        "${floor(clat / 0.25).toInt()}_${floor(clon / 0.25).toInt()}"
    }.values.toList()
    for ((i, group) in groups.withIndex()) {
        if (i > 0) delay(1000)   // pace the servers
        onStatus("Fetching cycling POIs ${i + 1}/${groups.size}…")
        val u = union(group)
        val osm: OsmData = MapBuilder.fetchPois(u.south, u.west, u.north, u.east)
        val files = withContext(Dispatchers.Default) {
            // Which cell each POI is in, once (not once per tile).
            val byCell = osm.pois.groupBy { H3Tiles.idAt(LatLon(it.lat, it.lon)) }
            group.map { t ->
                t.id to Cycling.buildPoi(
                    byCell[t.id] ?: emptyList(), t.south, t.west, t.north, t.east, t.id,
                ) { _, _ -> true }
            }
        }
        PoiCache.store(files)
        enqueue(files)
    }
}

/** Hexes above this many ask before a Redownload: each one is an Overpass fetch,
 *  an elevation fetch and a BLE transfer. */
private const val REDOWNLOAD_CONFIRM_OVER = 20

/** Bounding box enclosing a set of tiles, padded slightly so roads at tile edges
 *  are present in the fetch. */
private fun union(ts: List<MapTile>): BoundingBox {
    var s = 90.0; var w = 180.0; var n = -90.0; var e = -180.0
    for (t in ts) {
        s = min(s, t.south); w = min(w, t.west); n = max(n, t.north); e = max(e, t.east)
    }
    val pad = 0.003
    return BoundingBox(s - pad, w - pad, n + pad, e + pad)
}

// MARK: bottom cards

@Composable
private fun FloatingCard(content: @Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit) {
    Column(
        Modifier
            .fillMaxWidth()
            .background(Palette.surface, RoundedCornerShape(20.dp))
            .border(1.dp, Palette.hairline, RoundedCornerShape(20.dp))
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
            CircularProgressIndicator(Modifier.size(20.dp), color = Palette.accent)
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
        LinearProgressIndicator(
            progress = { sent.toFloat() / total },
            modifier = Modifier.fillMaxWidth(),
            color = Palette.good,
            trackColor = Palette.hairline,
        )
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
    skipped: Int,
    canSend: Boolean,
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
            Text(
                "$updateCount hex${if (updateCount == 1) " has" else "es have"} an update (made before " +
                    "bike routes & water stops, or over 90 days old) — redownload to refresh.",
                style = barlow(12.sp),
                color = Palette.ink,
            )
        }
        Spacer(Modifier.size(10.dp))
        PrimaryButton(
            title = when {
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
        if (onDeviceCount > 0) {
            Spacer(Modifier.size(8.dp))
            SecondaryButton(
                title = "Redownload $onDeviceCount hex${if (onDeviceCount == 1) "" else "es"}",
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
                .background(color.copy(alpha = 0.35f), RoundedCornerShape(3.dp))
                .border(1.5.dp, color, RoundedCornerShape(3.dp)),
        )
        Text("$n $label", style = barlow(13.sp, FontWeight.SemiBold), color = Palette.ink)
    }
}

/** Compact key to the hex colours and badges, top-left under the header. */
@Composable
private fun Legend(showNew: Boolean, showPois: Boolean, modifier: Modifier = Modifier) {
    Column(
        modifier
            .background(Palette.surface.copy(alpha = 0.92f), RoundedCornerShape(10.dp))
            .border(1.dp, Palette.hairline, RoundedCornerShape(10.dp))
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
