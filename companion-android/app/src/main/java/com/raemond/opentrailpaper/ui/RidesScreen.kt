package com.raemond.opentrailpaper.ui

import com.raemond.opentrailpaper.data.UploadLog
import com.raemond.opentrailpaper.data.SyncAccounts
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.RideFile
import com.raemond.opentrailpaper.data.FitDecoder
import com.raemond.opentrailpaper.data.LatLon
import com.raemond.opentrailpaper.data.RideNames
import com.raemond.opentrailpaper.data.RidePreview
import com.raemond.opentrailpaper.data.Units
import com.raemond.opentrailpaper.map.MapSnapshotter
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale
import kotlin.math.cos
import kotlin.math.max

/**
 * Lists recorded rides on the device and downloads a selected .fit file over
 * BLE, then offers the system share sheet (Strava, Files, mail).
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun RidesScreen(ble: BleManager) {
    val previews = remember { mutableStateMapOf<String, RidePreview>() }
    val loading = remember { mutableStateMapOf<String, Boolean>() }
    var detail by remember { mutableStateOf<Pair<java.io.File, RidePreview>?>(null) }
    var failedFile by remember { mutableStateOf<java.io.File?>(null) }
    var cachedNames by remember { mutableStateOf(ble.cachedRides()) }

    // The device's last-known ride list (kept across disconnects and launches)
    // merged with the rides cached on this phone, newest first. Shown the same
    // way connected or not, and while the device is re-checked, so reconnecting
    // never blanks or rebuilds the list — a completed check only adds new rides
    // and drops ones the device no longer has (unless they're cached here).
    val rides: List<RideFile> = remember(ble.rides, cachedNames) {
        mergeRides(ble.rides, cachedNames)
    }
    val checking = ble.state == BleManager.ConnState.CONNECTED && ble.loadingRides

    // Once per link: coming back to the tab doesn't re-ask the device.
    LaunchedEffect(ble.state) {
        if (ble.state == BleManager.ConnState.CONNECTED && !ble.loadingRides &&
            !ble.ridesCheckedThisLink
        ) ble.refreshRides()
    }

    // A finished download is both a new cache entry and, on iOS, the moment the
    // detail sheet opens — decode once and do both.
    LaunchedEffect(ble.downloadedFile) {
        val file = ble.downloadedFile ?: return@LaunchedEffect
        val decoded = withContext(Dispatchers.Default) {
            runCatching { FitDecoder.decode(file.readBytes()) }.getOrNull()
        }
        cachedNames = ble.cachedRides()
        // A queued ride that lands while the rider is looking at an earlier one
        // fills in its row rather than yanking the sheet out from under them.
        val busy = detail != null || failedFile != null
        if (decoded != null) {
            previews[file.name] = decoded
            if (!busy) detail = file to decoded
        } else if (!busy) {
            failedFile = file
        }
    }

    val context = LocalContext.current
    // Read so a ride's tag turns "✓ STRAVA" as soon as its upload lands.
    val uploadsVersion = UploadLog.version

    Column(Modifier.fillMaxSize().background(Palette.paper)) {
        RidesHeader(ble)
        Column(
            Modifier
                .weight(1f)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            if (ble.status.recording) InProgressBanner()
            AnimatedVisibility(checking, enter = fadeIn(), exit = fadeOut()) { CheckingRow() }
            if (rides.isNotEmpty()) WeekSummary(rides, previews, ble.useMiles)

            when {
                // With nothing cached yet, the checking row above stands in for
                // the empty state until the device answers.
                rides.isEmpty() -> if (!checking) Card {
                    Text(
                        when {
                            ble.status.recording -> "No synced rides yet"
                            ble.state == BleManager.ConnState.CONNECTED -> "No rides found"
                            else -> "No cached rides"
                        }.uppercase(),
                        style = TypeScale.title,
                        color = Palette.ink,
                    )
                    Text(
                        when {
                            ble.status.recording ->
                                "The current ride is being recorded to the SD card — it'll appear " +
                                    "here once you finish and stop it."

                            ble.state == BleManager.ConnState.CONNECTED ->
                                "Rides are saved to the SD card and sync here automatically."

                            else ->
                                "Connect to your OpenTrailPaper to download rides. Downloaded rides " +
                                    "stay here for offline viewing."
                        },
                        style = barlow(14.sp),
                        color = Palette.muted,
                    )
                }

                else -> {
                    // Keyed by name so a new ride at the top, or one dropping
                    // out, moves rows instead of recomposing each with its
                    // neighbour's state (and reloading its map).
                    rides.forEachIndexed { index, ride -> key(ride.name) {
                        val cached = ble.isCached(ride.name)
                        // Lazily decode cached rides off the main thread so the
                        // list shows thumbnails and stats without re-parsing
                        // every frame.
                        LaunchedEffect(ride.name, cached) {
                            if (!cached || previews.containsKey(ride.name)) return@LaunchedEffect
                            if (loading.put(ride.name, true) == true) return@LaunchedEffect
                            val decoded = withContext(Dispatchers.Default) {
                                runCatching {
                                    FitDecoder.decode(ble.cachedRideFile(ride.name).readBytes())
                                }.getOrNull()
                            }
                            loading.remove(ride.name)
                            if (decoded != null) previews[ride.name] = decoded
                        }

                        RideRow(
                            ride = ride,
                            featured = index == 0,
                            cached = cached,
                            preview = previews[ride.name],
                            downloading = ble.downloadingName == ride.name,
                            queued = ride.name in ble.queuedDownloads,
                            progress = ble.downloadProgress,
                            uploads = remember(ride.name, uploadsVersion) { UploadLog.providers(context, ride.name) },
                            useMiles = ble.useMiles,
                            onTap = {
                                val file = ble.cachedRideFile(ride.name)
                                val p = previews[ride.name]
                                when {
                                    p != null -> detail = file to p
                                    file.exists() -> failedFile = file
                                    else -> ble.downloadRide(ride.name)
                                }
                            },
                            onDelete = {
                                ble.deleteRide(ride.name)
                                previews.remove(ride.name)
                                cachedNames = ble.cachedRides()
                            },
                        )
                    } }
                    Text(
                        "Tap to preview & share · long-press to delete",
                        style = barlow(12.sp),
                        color = Palette.muted,
                    )
                }
            }
        }
    }

    detail?.let { (file, preview) ->
        RideDetailSheet(file, preview, ble.useMiles) { detail = null }
    }
    failedFile?.let { file ->
        RideParseErrorSheet(
            file = file,
            canRetry = ble.state == BleManager.ConnState.CONNECTED,
            onRetry = {
                // Throw away a cached file that won't parse and fetch it again —
                // the way out for rides left corrupt by an interrupted transfer.
                failedFile = null
                file.delete()
                previews.remove(file.name)
                cachedNames = ble.cachedRides()
                ble.downloadRide(file.name)
            },
        ) { failedFile = null }
    }
}

/**
 * What the Rides tab lists: the device's last-known ride list merged with the
 * rides cached on this phone, one entry per name (the device's entry wins),
 * newest first. A ride the device no longer lists stays while it's cached.
 */
internal fun mergeRides(device: List<RideFile>, cached: List<RideFile>): List<RideFile> {
    val seen = HashSet<String>()
    return (device + cached).filter { seen.add(it.name) }.sortedByDescending { it.name }
}

// Inline status above the list while the device's ride list is being fetched —
// the cached rides stay visible underneath it.
@Composable
private fun CheckingRow() {
    Row(
        Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        BlockSpinner()
        Text("CHECKING FOR NEW RIDES", style = caps(12.sp), color = Palette.ink)
    }
}

/** RIDES title, how many the device holds, and refresh / connect. */
@Composable
private fun RidesHeader(ble: BleManager) {
    val connected = ble.state == BleManager.ConnState.CONNECTED
    ScreenHeader("Rides") {
        Text(
            when {
                !connected -> "OFFLINE"
                ble.rides.isEmpty() -> "SYNCED"
                else -> "${ble.rides.size} ON DEVICE"
            },
            style = caps(12.sp),
            color = Palette.ink,
            maxLines = 1,
        )
        SquareIconButton(
            if (connected) Icons.Filled.Refresh else Icons.Filled.Wifi,
            label = if (connected) "Check for new rides" else "Connect to device",
        ) { if (connected) ble.refreshRides() else ble.startScan() }
    }
}

/** Shown while the device is actively recording, so the tab makes clear a ride is
 *  underway (and why new rides haven't synced yet). */
@Composable
private fun InProgressBanner() {
    Row(
        Modifier
            .fillMaxWidth()
            .einkBorder()
            .padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Tag("● REC", inverted = true)
        Column {
            Text("RIDE IN PROGRESS", style = caps(12.sp), color = Palette.ink)
            Text(
                "Recording to the SD card — it'll sync here when you stop.",
                style = barlow(13.sp),
                color = Palette.ink,
            )
        }
    }
}

/** Week roll-up from whatever previews are decoded (fills in as rows load). */
@Composable
private fun WeekSummary(
    rides: List<RideFile>,
    previews: Map<String, RidePreview>,
    useMiles: Boolean,
) {
    val recent = rides.filter { daysAgo(it.name) <= 7 }
    val loaded = recent.mapNotNull { previews[it.name] }
    val dist = loaded.sumOf { it.distanceKm }
    val time = loaded.sumOf { it.duration }
    // Separate boxes with gutters, like the device's data cells.
    Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        SummaryCell("This week", "${recent.size}", if (recent.size == 1) "ride" else "rides", Modifier.weight(1f))
        SummaryCell(
            "Distance",
            String.format(Locale.US, "%.0f", Units.distance(dist, useMiles)),
            Units.distLabel(useMiles),
            Modifier.weight(1f),
        )
        SummaryCell("Time", hoursMinutes(time), "hours", Modifier.weight(1f))
    }
}

@Composable
private fun SummaryCell(label: String, value: String, unit: String, modifier: Modifier = Modifier) {
    Column(
        modifier
            .fillMaxHeight()
            .einkBorder()
            .padding(10.dp),
    ) {
        TrackedLabel(label)
        Text(value, style = anton(34.sp), color = Palette.ink, maxLines = 1)
        Text(unit, style = barlow(12.sp), color = Palette.ink, maxLines = 1)
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun RideRow(
    ride: RideFile,
    /** The newest ride: a big card with its map. The rest are compact rows. */
    featured: Boolean,
    cached: Boolean,
    preview: RidePreview?,
    downloading: Boolean,
    queued: Boolean,
    progress: Double,
    uploads: List<SyncAccounts.Provider>,
    useMiles: Boolean,
    onTap: () -> Unit,
    onDelete: () -> Unit,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .background(Palette.paper)
            .einkBorder()
            .combinedClickable(
                onClickLabel = if (cached) "Open the ride" else "Download the ride from the device",
                onClick = onTap,
                onLongClick = onDelete,
            ),
    ) {
        if (featured) {
            // The latest ride: map on top, then name, date and four numbers.
            Box(Modifier.fillMaxWidth().height(150.dp).clipToBounds()) {
                RideBanner(ride, cached, preview, downloading, queued, progress)
            }
            Box(Modifier.fillMaxWidth().height(Rule.border).background(Palette.ink))
            Column(
                Modifier.padding(horizontal = 12.dp, vertical = 10.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Row(verticalAlignment = Alignment.Bottom) {
                    Text(
                        rideName(ride.name), style = barlow(16.sp, FontWeight.Bold), color = Palette.ink,
                        maxLines = 1, modifier = Modifier.weight(1f),
                    )
                    Text(dateSubtitle(ride.name).uppercase(), style = barlow(12.sp, FontWeight.SemiBold), color = Palette.ink)
                }
                if (preview != null) {
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        BigStat(
                            String.format(Locale.US, "%.1f", Units.distance(preview.distanceKm, useMiles)),
                            Units.distLabel(useMiles), Modifier.weight(1f),
                        )
                        BigStat(clock(preview.duration), "Time", Modifier.weight(1f))
                        BigStat(
                            String.format(Locale.US, "%.1f", Units.speed(preview.avgSpeedKmh, useMiles)),
                            Units.speedLabel(useMiles), Modifier.weight(1f),
                        )
                        BigStat(preview.avgPower?.toString() ?: "--", "W avg", Modifier.weight(1f))
                    }
                } else {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.weight(1f)) { StatusLine(ride, preview, downloading, progress, useMiles) }
                        RideTag(cached, downloading, queued, progress, uploads)
                    }
                }
            }
        } else {
            // An older ride: track glyph, name and stats on one line, state tag.
            Row(
                Modifier.padding(horizontal = 12.dp, vertical = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Box(Modifier.size(64.dp, 48.dp).einkBorder(), contentAlignment = Alignment.Center) {
                    val coords = preview?.coordinates
                    if (coords != null && coords.size > 1) {
                        RouteThumbnail(coords, Modifier.fillMaxSize().padding(6.dp))
                    } else {
                        Icon(
                            if (cached) Icons.Filled.Map else Icons.Filled.Download, contentDescription = null,
                            tint = Palette.ink, modifier = Modifier.size(18.dp),
                        )
                    }
                }
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(rideName(ride.name), style = barlow(15.sp, FontWeight.Bold), color = Palette.ink, maxLines = 1)
                    StatusLine(ride, preview, downloading, progress, useMiles)
                }
                RideTag(cached, downloading, queued, progress, uploads)
            }
        }
    }
}

@Composable
private fun BigStat(value: String, label: String, modifier: Modifier = Modifier) {
    Column(modifier) {
        Text(value, style = anton(22.sp), color = Palette.ink, maxLines = 1)
        Text(label.uppercase(), style = caps(11.sp, 1.sp), color = Palette.ink, maxLines = 1)
    }
}

@Composable
private fun StatusLine(ride: RideFile, preview: RidePreview?, downloading: Boolean, progress: Double, useMiles: Boolean) {
    when {
        preview != null -> Text(
            String.format(
                Locale.US, "%.1f %s · %s · %.1f %s",
                Units.distance(preview.distanceKm, useMiles), Units.distLabel(useMiles),
                clock(preview.duration),
                Units.speed(preview.avgSpeedKmh, useMiles), Units.speedLabel(useMiles),
            ),
            style = barlow(13.sp), color = Palette.ink, maxLines = 1,
        )
        downloading -> BlockProgressBar(progress.toFloat(), Modifier.width(120.dp), segments = 8, height = 8.dp)
        else -> Text(
            dateSubtitle(ride.name) + String.format(Locale.US, " · %.0f KB", ride.size / 1024.0),
            style = barlow(13.sp), color = Palette.ink, maxLines = 1,
        )
    }
}

/** ✓ STRAVA once uploaded; else where the ride is, or the transfer state. */
@Composable
private fun RideTag(
    cached: Boolean,
    downloading: Boolean,
    queued: Boolean,
    progress: Double,
    uploads: List<SyncAccounts.Provider>,
) {
    val first = uploads.firstOrNull()
    when {
        downloading -> Tag("${(progress * 100).toInt()}%")
        queued -> Tag("Queued")
        first != null -> Tag(
            "✓ " + UploadLog.shortTitle(first) + if (uploads.size > 1) " +${uploads.size - 1}" else "",
            inverted = true,
        )
        cached -> Tag("On phone")
        else -> Tag("On device")
    }
}

/**
 * Map banner for the big card: a real map snapshot with the route drawn on it
 * once the ride is decoded, otherwise a prompt to download.
 */
@Composable
private fun RideBanner(
    ride: RideFile,
    cached: Boolean,
    preview: RidePreview?,
    downloading: Boolean,
    queued: Boolean,
    progress: Double,
) {
    val coords = preview?.coordinates
    Box(Modifier.fillMaxSize().background(Palette.paper), contentAlignment = Alignment.Center) {
        when {
            coords != null && coords.size > 1 -> RouteMapThumbnail(ride.name, coords)
            downloading -> Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                BlockProgressBar(progress.toFloat(), Modifier.width(180.dp))
                Text("DOWNLOADING…", style = caps(12.sp), color = Palette.ink)
            }

            queued -> Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                BlockSpinner()
                Text(
                    "QUEUED — STARTS AFTER THE CURRENT DOWNLOAD",
                    style = caps(11.sp, 1.5.sp),
                    color = Palette.ink,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.padding(horizontal = 16.dp),
                )
            }

            else -> Box(Modifier.fillMaxSize().hatched(), contentAlignment = Alignment.Center) {
                Column(
                    Modifier.background(Palette.paper).padding(14.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Icon(
                        if (cached) Icons.Filled.Map else Icons.Filled.Download,
                        contentDescription = null,
                        tint = Palette.ink,
                        modifier = Modifier.size(28.dp),
                    )
                    Text(if (cached) "LOADING MAP…" else "TAP TO DOWNLOAD", style = caps(12.sp), color = Palette.ink)
                }
            }
        }
    }
}

/**
 * A real map with the ride's route drawn on top, rendered once and cached by ride
 * name. Falls back to a track-shape glyph while the snapshot loads or if it can't
 * be produced (offline).
 */
@Composable
fun RouteMapThumbnail(name: String, coords: List<LatLon>) {
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val density = LocalDensity.current
        val wPx = with(density) { maxWidth.roundToPx() }
        val hPx = with(density) { maxHeight.roundToPx() }
        // Keyed on the size as well as the ride: a cached bitmap rendered for a
        // different width would be resampled, and a map resampled is a map with
        // unreadable street lines.
        var bitmap by remember(name, wPx, hPx) {
            mutableStateOf(MapSnapshotter.cached(name, wPx, hPx))
        }
        LaunchedEffect(name, wPx, hPx) {
            if (wPx > 0 && hPx > 0) bitmap = MapSnapshotter.snapshot(name, coords, wPx, hPx)
        }
        val img = bitmap
        if (img != null) {
            Image(
                img.asImageBitmap(),
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop,
            )
        } else {
            RouteThumbnail(coords, Modifier.fillMaxSize().padding(10.dp))
        }
    }
}

/**
 * Draws the GPS track as a normalized polyline — a recognizable "shape of the
 * ride" glyph, used as an instant/offline fallback for the map snapshot.
 */
@Composable
fun RouteThumbnail(coords: List<LatLon>, modifier: Modifier = Modifier) {
    Canvas(modifier) {
        if (coords.size < 2) return@Canvas
        val minLat = coords.minOf { it.lat }
        val maxLat = coords.maxOf { it.lat }
        val minLon = coords.minOf { it.lon }
        val maxLon = coords.maxOf { it.lon }
        val meanLat = (minLat + maxLat) / 2
        val cosLat = max(0.01, cos(meanLat * Math.PI / 180))
        val w = (maxLon - minLon) * cosLat
        val h = maxLat - minLat
        if (w <= 0 && h <= 0) return@Canvas
        // Fit the track into the box, preserving aspect ratio, north up.
        val scale = minOf(
            if (w > 0) size.width / w else Double.MAX_VALUE,
            if (h > 0) size.height / h else Double.MAX_VALUE,
        )
        val offX = (size.width - w * scale) / 2
        val offY = (size.height - h * scale) / 2
        fun pt(c: LatLon) = Offset(
            (offX + (c.lon - minLon) * cosLat * scale).toFloat(),
            (size.height - (offY + (c.lat - minLat) * scale)).toFloat(),
        )
        val path = Path()
        path.moveTo(pt(coords[0]).x, pt(coords[0]).y)
        for (c in coords.drop(1)) path.lineTo(pt(c).x, pt(c).y)
        drawPath(
            path,
            Palette.accent,
            style = Stroke(width = 2f, cap = StrokeCap.Round, join = StrokeJoin.Round),
        )
        drawCircle(Palette.good, radius = 2.5f, center = pt(coords[0]))
    }
}

// MARK: naming

/**
 * Filenames look like 20260712-150300.fit — the device names them in the
 * RIDER'S wall-clock time (its timezone setting), so the name already says
 * what a human wants to read. Parse and display in the phone's own zone so no
 * conversion happens and the shown time equals the name, verbatim. (Rides
 * recorded before the local-naming firmware carry UTC names and will display
 * those UTC digits as-is — a one-time historical quirk. Same fix as the iOS
 * app's RidesView.rideDate.)
 */
fun rideDate(name: String): Date? {
    val base = name.removeSuffix(".fit")
    return runCatching {
        SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).parse(base)
    }.getOrNull()
}

private fun daysAgo(name: String): Int {
    val d = rideDate(name) ?: return 9999
    return ((System.currentTimeMillis() - d.time) / 86_400_000L).toInt()
}

/** "1:47": hours and minutes. */
private fun clock(seconds: Double): String {
    val m = (seconds / 60).toInt()
    return String.format(Locale.US, "%d:%02d", m / 60, m % 60)
}

private fun hoursMinutes(seconds: Double) = clock(seconds)

/**
 * The rider's name for it (renamed on upload), else a human name like
 * "Saturday afternoon ride" from the file's timestamp.
 */
private fun rideName(fileName: String): String {
    RideNames.names[fileName]?.let { return it }
    val d = rideDate(fileName) ?: return fileName
    val cal = Calendar.getInstance().apply { time = d }
    val weekday = SimpleDateFormat("EEEE", Locale.getDefault()).format(d)
    val h = cal.get(Calendar.HOUR_OF_DAY)
    val part = when {
        h < 5 -> "night"
        h < 12 -> "morning"
        h < 17 -> "afternoon"
        h < 21 -> "evening"
        else -> "night"
    }
    return "$weekday $part ride"
}

private fun dateSubtitle(fileName: String): String {
    val d = rideDate(fileName) ?: return ""
    return SimpleDateFormat("MMM d · HH:mm", Locale.getDefault()).format(d)
}
