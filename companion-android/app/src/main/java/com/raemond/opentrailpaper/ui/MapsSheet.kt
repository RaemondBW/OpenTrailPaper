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
import com.raemond.opentrailpaper.map.OutlineHex
import com.raemond.opentrailpaper.map.SelectionHex
import com.raemond.opentrailpaper.map.TileCache
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
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
 */
@Composable
fun MapsSheet(ble: BleManager, onDismiss: () -> Unit) {
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
    var box by remember { mutableStateOf<BoundingBox?>(null) }
    var tiles by remember { mutableStateOf<List<MapTile>>(emptyList()) }
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

    // Hexes in the drawn box that aren't on the device yet, in whichever state the
    // download has reached. Areas already downloaded are NOT in here — they
    // already show as coverage hexagons, which says "you have this" better than a
    // selection tint would.
    val selectionHexes = remember(tiles, ble.deviceTileIds, converted, excluded) {
        tiles.filter { it.id !in ble.deviceTileIds }.map { t ->
            SelectionHex(
                t.id,
                t.hexagon,
                when {
                    t.id in converted -> SelectionHex.Kind.DONE
                    t.id in excluded -> SelectionHex.Kind.EXCLUDED
                    else -> SelectionHex.Kind.PENDING
                },
            )
        }
    }

    // Tiles that will actually be sent: not already on the device and not tapped
    // out by the user.
    val newTiles = remember(tiles, ble.deviceTileIds, excluded) {
        tiles.filter { it.id !in ble.deviceTileIds && it.id !in excluded }
    }
    val onDeviceCount = tiles.count { it.id in ble.deviceTileIds }

    // Ask the store what to draw for the region now on screen. Coalesced along
    // with the device's tile list, which arrives tile-by-tile during an upload:
    // without this a large download rebuilds every overlay several times a second
    // and the page stutters badly once a few hundred hexes are on screen.
    LaunchedEffect(region, EInkTileStore.version, ble.deviceTileIds) {
        delay(250)
        val r = region ?: return@LaunchedEffect
        outlines = EInkTileStore.visibleContent(r, ble.deviceTileIds)

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

    fun startDownload() {
        val missing = newTiles
        if (missing.isEmpty()) return
        building = true
        converted = emptySet()
        failedHexes = emptySet()
        downloadTotal = missing.size
        status = "Fetching map data…"
        ble.startTileStream()            // begin sending as tiles are produced

        job = scope.launch {
            try {
                downloadTiles(
                    missing = missing,
                    onStatus = { status = it },
                    onBuilt = { ids ->
                        converted = converted + ids
                        EInkTileStore.noteDownloaded(ids)
                    },
                    onFailed = { ids -> failedHexes = failedHexes + ids },
                    enqueue = { ble.enqueueTiles(it) },
                )
                building = false
                ble.finishTileStream()                     // let the queue drain
                // The cache has a size ceiling and this is the only thing that
                // grows it, so this is where it gets enforced.
                TileCache.trim()
                status = when {
                    failedHexes.isNotEmpty() ->
                        "${failedHexes.size} hex${if (failedHexes.size == 1) "" else "es"} had " +
                            "no map data — tap Select area and retry them."

                    converted.isEmpty() -> "No roads found in that area."
                    else -> null
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
                        if (id != null && tiles.any { it.id == id } && id !in ble.deviceTileIds) {
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
                        onDeviceCount = onDeviceCount,
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
    onStatus: (String?) -> Unit,
    onBuilt: (List<String>) -> Unit,
    onFailed: (List<String>) -> Unit,
    enqueue: (List<Pair<String, ByteArray>>) -> Unit,
) {
    // Anything built before goes straight out — no Overpass, no elevation fetch,
    // no re-encoding. This is what makes a retry after a dropped link cheap
    // instead of a full rebuild.
    val (cached, _) = TileCache.partition(missing.map { it.id })
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
    // because it is coastline-only.
    val all = union(missing)
    val pad = 0.35
    onStatus("Fetching coastline…")
    val coastChains = runCatching {
        val coastOsm = MapBuilder.fetchCoastline(
            all.south - pad, all.west - pad, all.north + pad, all.east + pad,
        )
        MapBuilder.coastlineChains(coastOsm)
    }.getOrDefault(emptyList())

    // Rings are assembled against the PADDED region, not each batch's bbox, so a
    // batch sitting entirely offshore is still inside a ring and fills.
    val seaRings = withContext(Dispatchers.Default) {
        MapBuilder.regionSeaPolygons(
            coastChains,
            all.south - pad, all.west - pad, all.north + pad, all.east + pad,
        )
    }

    for ((i, batch) in batches.withIndex()) {
        if (i > 0) delay(1000)   // pace the servers
        val n = batches.size
        onStatus("Fetching area ${i + 1}/$n…")
        val u = union(batch)
        val osm = MapBuilder.fetchOsm(u.south, u.west, u.north, u.east) { m ->
            onStatus("Fetching area ${i + 1}/$n — $m")
        }

        onStatus("Building tiles ${i + 1}/$n…")
        val encoded = withContext(Dispatchers.Default) { MapBuilder.encodeTiles(osm, batch) }
        // natural=water and park polygons for this region, resolved once and
        // appended per tile as WTR2 / PRK2 sections (after any ELV1 block).
        val waterWays = withContext(Dispatchers.Default) { MapBuilder.waterWays(osm) }
        val parkWays = withContext(Dispatchers.Default) { MapBuilder.parkWays(osm) }

        onStatus("Elevation ${i + 1}/$n…")
        val produced = ArrayList<Pair<String, ByteArray>>(encoded.size)
        for ((id, roads) in encoded) {
            val tile = batch.first { it.id == id }
            val out = ByteArrayOutputStream(roads.size + 4096)
            out.write(roads)
            // Bake a DEM elevation grid into each tile (best-effort) so the device
            // has elevation without GPS altitude or the phone.
            runCatching {
                MapBuilder.fetchElevationGrid(tile.south, tile.west, tile.north, tile.east)
            }.getOrNull()?.let { grid ->
                MapBuilder.appendElevation(
                    out, tile.south, tile.west, tile.north, tile.east,
                    grid, MapBuilder.ELEVATION_GRID,
                )
            }
            withContext(Dispatchers.Default) {
                MapBuilder.appendWater(
                    out, waterWays, seaRings,
                    tile.south, tile.west, tile.north, tile.east,
                )
                MapBuilder.appendParks(
                    out, parkWays, tile.south, tile.west, tile.north, tile.east,
                )
            }
            val data = out.toByteArray()
            // Decide emptiness only now, with water/parks/sea/elevation already
            // appended — a hex can be pure water and still be worth storing.
            if (MapBuilder.isEmpty(data, tile)) continue
            produced.add(id to data)
        }

        // Mark ONLY what was actually produced. Marking every id in the batch
        // would fill the map in as downloaded and report success while nothing
        // had been sent or stored — a rider then finds a hole in their coverage
        // with no clue which hex is missing, and it survives reboots because the
        // tile genuinely is not on the card.
        val producedIds = produced.map { it.first }.toSet()
        onBuilt(producedIds.toList())
        val missed = batch.map { it.id }.filter { it !in producedIds }
        if (missed.isNotEmpty()) onFailed(missed)

        // Cache BEFORE sending: if the link drops mid-transfer the expensive work
        // survives and the retry is instant.
        TileCache.store(produced)
        enqueue(produced)             // send in parallel with the next fetch
    }
}

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
            "$sent of $total hexes sent" +
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
    onDeviceCount: Int,
    skipped: Int,
    canSend: Boolean,
    status: String?,
    onDownload: () -> Unit,
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
        Text(
            "$newCount to download · $onDeviceCount on device" +
                if (skipped > 0) " · $skipped skipped" else "",
            style = barlow(12.sp),
            color = Palette.muted,
        )
        Text(
            "Tap a hex to skip it (or add it back).",
            style = barlow(11.sp),
            color = Palette.faint,
        )
        Spacer(Modifier.size(10.dp))
        PrimaryButton(
            title = when {
                !canSend -> "Connect device to send"
                newCount == 0 -> "Nothing to download"
                else -> "Download $newCount hex${if (newCount == 1) "" else "es"}"
            },
            icon = Icons.Filled.Download,
            enabled = canSend && newCount > 0,
            onClick = onDownload,
        )
        status?.let {
            Text(it, style = barlow(12.sp), color = Palette.accent)
        }
    }
}

@Composable
private fun HintCard(drawMode: Boolean, deviceHexes: Int) {
    FloatingCard {
        Text(
            if (drawMode) {
                "Drag a box across the area you want."
            } else {
                "Tap “Select area”, then drag a box. Shaded hexagons are downloaded; " +
                    "a green check means the device has them too."
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
