package com.raemond.opentrailpaper.ui

import android.content.Intent
import android.net.Uri
import android.os.SystemClock
import android.provider.Settings
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.GridView
import androidx.compose.material.icons.filled.Sync
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.DeviceStatus
import com.raemond.opentrailpaper.ble.PermissionState
import com.raemond.opentrailpaper.data.DashConfig
import com.raemond.opentrailpaper.data.DashLayout
import com.raemond.opentrailpaper.data.LatLon
import com.raemond.opentrailpaper.data.Units
import com.raemond.opentrailpaper.link.proto.Feature
import com.raemond.opentrailpaper.link.proto.LiveFrame
import com.raemond.opentrailpaper.link.proto.LiveTopic
import com.raemond.opentrailpaper.link.proto.StopwatchOp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The Ride tab is the head unit's live dashboard on the phone: the same pages,
 * laid out the way the device packs them (DashLayout), filled with what the
 * device streams over the link ([BleManager.liveFrame]), else the status it
 * reports. Fields neither carries, and everything while disconnected, read
 * "--" — the last known layout stays on screen. Port of RideView.swift.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun RideScreen(ble: BleManager) {
    var showDashEditor by remember { mutableStateOf(false) }
    var showWorkouts by remember { mutableStateOf(false) }
    val connected = ble.state == BleManager.ConnState.CONNECTED
    // The rider's layout (unsynced edit, else the device's, cached across
    // disconnects); the device default only before it has ever been read.
    val config = ble.dashTarget ?: DashConfig.deviceDefault
    val pages = config.pages
    val pager = rememberPagerState { pages.size + 1 }
    val scope = rememberCoroutineScope()
    val pageIx = pager.currentPage.coerceAtMost(pages.size)

    // Frames are collected here alone, so no other tab recomposes for them.
    val liveFrame by ble.liveFrame.collectAsState()
    val frame: LiveFrame? = if (connected) liveFrame else null
    // When the frame arrived, to count the stopwatch on between frames.
    var frameAt by remember { mutableLongStateOf(SystemClock.elapsedRealtime()) }
    LaunchedEffect(liveFrame) { frameAt = SystemClock.elapsedRealtime() }

    LaunchedEffect(ble.state) {
        if (connected) ble.refreshSensors()
    }

    // The link streams only what the page in view needs besides RIDE (always
    // on): the stopwatch, the player, the map's position and view, and radar
    // where a tile shows it. Dropped when the tab goes or the app backgrounds.
    val topics = liveTopics(config, pageIx)
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    var started by remember { mutableStateOf(lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) }
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_START -> started = true
                Lifecycle.Event.ON_STOP -> started = false
                else -> Unit
            }
        }
        lifecycle.addObserver(observer)
        onDispose {
            lifecycle.removeObserver(observer)
            ble.setLiveTopics(emptySet())
        }
    }
    LaunchedEffect(started, topics) { ble.setLiveTopics(if (started) topics else emptySet()) }

    // The wall clock the status bar and clock tiles print, ticking on the minute.
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(Unit) {
        while (true) {
            delay(60_000 - System.currentTimeMillis() % 60_000)
            now = System.currentTimeMillis()
        }
    }
    val clock = SimpleDateFormat(if (ble.clock24h) "H:mm" else "h:mm", Locale.getDefault()).format(Date(now))
    val status = panelStatus(ble, frame, clock)
    val feed = LiveFeed(connected, frame, ble.status, ble.useMiles, ble.ftpWatts, clock)

    // The tab jumps to the WORKOUT page when a workout loads.
    val workoutIx = pages.indexOfFirst { it.isWorkout }
    var wasLoaded by remember { mutableStateOf(ble.workoutStatus.loaded) }
    LaunchedEffect(ble.workoutStatus.loaded) {
        val loaded = ble.workoutStatus.loaded
        if (loaded && !wasLoaded && workoutIx >= 0) pager.animateScrollToPage(workoutIx)
        wasLoaded = loaded
    }
    WorkoutFileFetcher(ble, onWorkoutPage = pages.getOrNull(pageIx)?.isWorkout == true, sheetOpen = showWorkouts)

    Column(Modifier.fillMaxSize().background(Palette.paper)) {
        ScreenHeader(
            "Ride",
            subtitle = {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Box(
                        Modifier
                            .size(8.dp)
                            .background(if (connected) Palette.ink else Color.Transparent)
                            .border(1.5.dp, Palette.ink),
                    )
                    Text(statusLine(ble), style = caps(11.sp, 1.5.sp), color = Palette.ink, maxLines = 1)
                }
            },
        ) {
            BlockButton("Workouts", filled = false) { showWorkouts = true }
            // The layout is read from the device: until it has been, an
            // invented default would overwrite the rider's real one.
            BlockButton(
                "Edit", icon = Icons.Filled.Edit, enabled = ble.dashTarget != null,
                modifier = Modifier.semantics { contentDescription = "Edit dashboard" },
            ) { showDashEditor = true }
        }

        Column(
            Modifier.weight(1f).padding(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Warnings(ble)
            PageSelector(pages, pageIx) { i -> scope.launch { pager.animateScrollToPage(i) } }
            HorizontalPager(
                state = pager,
                modifier = Modifier.weight(1f).fillMaxWidth(),
                pageSpacing = 16.dp,
                key = { i -> pages.getOrNull(i)?.key ?: -1L },
            ) { i ->
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    val framed = Modifier.border(Rule.border, Palette.ink).padding(Rule.border)
                    val p = pages.getOrNull(i)
                    if (p == null) {
                        AddNewPage(ble, config, framed) { added ->
                            scope.launch { pager.animateScrollToPage(added) }
                        }
                    } else {
                        DevicePage(ble, config, p, status, feed, frame, frameAt, framed)
                    }
                }
            }
            if (!connected) {
                PrimaryButton(
                    title = when (ble.state) {
                        BleManager.ConnState.PAIRING -> "Pairing…"
                        BleManager.ConnState.SCANNING, BleManager.ConnState.CONNECTING -> "Searching…"
                        else -> "Connect to OpenTrailPaper"
                    },
                    icon = Icons.Filled.Wifi,
                    // An explicit tap also lifts a refusal / failed-pairing hold.
                ) { ble.retryConnection() }
            }
        }
    }

    if (showDashEditor) {
        DashboardEditorSheet(ble) { showDashEditor = false }
    }
    if (showWorkouts) {
        WorkoutsSheet(ble) { showWorkouts = false }
    }
}

/** "CONNECTED · 76% · GPS 9 SATS", or the connection state. */
private fun statusLine(ble: BleManager): String {
    if (ble.state != BleManager.ConnState.CONNECTED) return connectLabel(ble.state).uppercase()
    val s = ble.status
    val parts = mutableListOf("CONNECTED", "${s.battery}%")
    parts += if (s.gpsFix) "GPS ${s.sats} SATS" else "NO GPS FIX · ${s.sats} SATS"
    if (s.recording) parts += "REC"
    return parts.joinToString(" · ")
}

private fun connectLabel(state: BleManager.ConnState) = when (state) {
    BleManager.ConnState.SCANNING -> "Searching…"
    BleManager.ConnState.CONNECTING -> "Connecting…"
    BleManager.ConnState.PAIRING -> "Pairing — type the code on the device"
    BleManager.ConnState.POWERED_OFF -> "Bluetooth off"
    else -> "Not connected"
}

@Composable
private fun Warnings(ble: BleManager) {
    val context = LocalContext.current
    val perm = ble.bluetoothPermission
    if (perm.fixableInSettings || perm == PermissionState.UNAVAILABLE) {
        WarningBanner(
            "Bluetooth not allowed",
            message = "The app can't reach your OpenTrailPaper without it.",
        ) {
            if (perm.fixableInSettings) {
                Text(
                    "SETTINGS", style = caps(11.sp, 1.5.sp), color = Palette.ink,
                    modifier = Modifier
                        .clickable(role = Role.Button) {
                            context.startActivity(
                                Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
                                    .setData(Uri.fromParts("package", context.packageName, null))
                                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                            )
                        }
                        .padding(8.dp),
                )
            }
        }
    } else if (ble.state == BleManager.ConnState.POWERED_OFF) {
        WarningBanner("Bluetooth is off", message = "Turn it on in quick settings to connect.")
    }
    if (ble.dashTarget == null) {
        WarningBanner(
            "Default layout",
            message = "Connect once to load your device's own dashboard pages.",
            icon = Icons.Filled.GridView,
        )
    } else if (ble.pendingDashConfig != null) {
        val connected = ble.state == BleManager.ConnState.CONNECTED
        WarningBanner(
            if (connected) "Syncing layout" else "Layout waiting to sync",
            message = if (connected) null else "Your edits go to the device on the next connect.",
            icon = Icons.Filled.Sync,
        )
    }
}

/** "PAGE 1" counts field pages only, as the device's page dots do; the others
 *  are named for what they show. */
private fun pageName(i: Int, pages: List<DashConfig.Page>): String = when (pages[i].kind) {
    DashConfig.PageKind.FIELDS -> "PAGE ${pages.take(i + 1).count { it.kind == DashConfig.PageKind.FIELDS }}"
    DashConfig.PageKind.MAP -> "MAP"
    DashConfig.PageKind.MUSIC -> "MUSIC"
    DashConfig.PageKind.WORKOUT -> "WORKOUT"
    DashConfig.PageKind.STOPWATCH -> "STOPWATCH"
}

/**
 * The page strip. Names keep their full size: they share the width when they
 * fit, and the strip scrolls when they don't. The narrow + cell is the "add
 * new" page that ends the swipe.
 */
@Composable
private fun PageSelector(pages: List<DashConfig.Page>, selected: Int, onSelect: (Int) -> Unit) {
    BoxWithConstraints(Modifier.fillMaxWidth().border(Rule.border, Palette.ink)) {
        val names = pages.indices.map { pageName(it, pages) }
        // Bold 12sp caps tracked 2sp run about 10dp a letter, plus padding.
        val natural = names.sumOf { it.length * 10 + 24 } + 44
        val fill = natural <= maxWidth.value
        Row(
            Modifier
                .fillMaxWidth()
                .height(IntrinsicSize.Min)
                .then(if (fill) Modifier else Modifier.horizontalScroll(rememberScrollState())),
        ) {
            names.forEachIndexed { i, name ->
                if (i > 0) Box(Modifier.width(Rule.border).fillMaxHeight().background(Palette.ink))
                SelectorCell(i == selected, if (fill) Modifier.weight(1f) else Modifier, { onSelect(i) }) {
                    Text(name, style = caps(12.sp), color = if (i == selected) Palette.paper else Palette.ink, maxLines = 1)
                }
            }
            if (names.isNotEmpty()) Box(Modifier.width(Rule.border).fillMaxHeight().background(Palette.ink))
            SelectorCell(selected == pages.size, Modifier.width(44.dp), { onSelect(pages.size) }) {
                Icon(
                    Icons.Filled.Add, contentDescription = "Add a page",
                    tint = if (selected == pages.size) Palette.paper else Palette.ink,
                    modifier = Modifier.size(18.dp),
                )
            }
        }
    }
}

@Composable
private fun SelectorCell(on: Boolean, modifier: Modifier, onClick: () -> Unit, label: @Composable () -> Unit) {
    Box(
        modifier
            .defaultMinSize(minHeight = 34.dp)
            .background(if (on) Palette.ink else Palette.paper)
            .clickable(role = Role.Tab, onClick = onClick)
            .padding(horizontal = 12.dp),
        contentAlignment = Alignment.Center,
    ) { label() }
}

/** The page as the device draws it, live. */
@Composable
private fun DevicePage(
    ble: BleManager,
    config: DashConfig,
    p: DashConfig.Page,
    status: PanelStatus,
    feed: LiveFeed,
    frame: LiveFrame?,
    frameAt: Long,
    modifier: Modifier,
) {
    val connected = ble.state == BleManager.ConnState.CONNECTED
    when (p.kind) {
        DashConfig.PageKind.FIELDS -> DevicePanel(modifier, status) { k ->
            DashPreview(
                p.layout,
                Modifier.offset(y = (STATUS_H * k).dp).size((PANEL_W * k).dp, ((PANEL_H - STATUS_H) * k).dp),
                useMiles = ble.useMiles,
                feed = feed,
            )
        }
        DashConfig.PageKind.MAP -> LiveMapPage(ble, config.mapFields, status, feed, frame, modifier)
        DashConfig.PageKind.MUSIC -> LiveMusicPage(ble, status, modifier)
        DashConfig.PageKind.WORKOUT -> WorkoutPagePreview(
            modifier, status, workoutModel(ble),
            if (connected) workoutActions(ble) else null,
        )
        DashConfig.PageKind.STOPWATCH -> LiveStopwatchPage(ble, status, frame, frameAt, modifier)
    }
}

/** What the device should stream for the page in view, besides RIDE. */
private fun liveTopics(config: DashConfig, pageIx: Int): Set<LiveTopic> {
    val p = config.pages.getOrNull(pageIx) ?: return emptySet()
    return when (p.kind) {
        DashConfig.PageKind.STOPWATCH -> setOf(LiveTopic.LIVE_TOPIC_STOPWATCH)
        DashConfig.PageKind.MUSIC -> setOf(LiveTopic.LIVE_TOPIC_MUSIC)
        DashConfig.PageKind.MAP ->
            if ("radar" in config.mapFields) {
                setOf(LiveTopic.LIVE_TOPIC_POSITION, LiveTopic.LIVE_TOPIC_VIEW, LiveTopic.LIVE_TOPIC_RADAR)
            } else {
                setOf(LiveTopic.LIVE_TOPIC_POSITION, LiveTopic.LIVE_TOPIC_VIEW)
            }
        DashConfig.PageKind.FIELDS ->
            if (p.layout.items.any { it.field == "radar" }) setOf(LiveTopic.LIVE_TOPIC_RADAR) else emptySet()
        DashConfig.PageKind.WORKOUT -> emptySet()
    }
}

/**
 * The status bar as the device prints it: its own labels and liveness from
 * the live stream, else a guess from the status it reports.
 */
private fun panelStatus(ble: BleManager, frame: LiveFrame?, clock: String): PanelStatus {
    val connected = ble.state == BleManager.ConnState.CONNECTED
    val s = ble.status
    val labels = mutableListOf<String>()
    if (frame != null && frame.hasRide()) {
        val r = frame.ride
        if (r.hasHrBpm()) labels += "HR"
        if (r.hasPowerW()) labels += "PWR"
        if (r.speedFromWheel) labels += "SPD"
        if (r.radarConnected && !r.autoPaused) labels += if (r.radarLive) "RDR" else "RDR?"
        if (r.autoPaused) labels += "PAUSED"
    } else if (connected) {
        if (s.heartRate != null) labels += "HR"
        if (s.power != null) labels += "PWR"
    }
    return PanelStatus(
        clock = clock,
        satellites = if (connected) s.sats else 0,
        battery = if (connected) s.battery else 0,
        linked = connected,
        sensors = labels.joinToString(" ") { "· $it" },
    )
}

/** Coggan's seven power zones as % of FTP: <55, <75, <90, <105, <120, <150, above. */
private fun powerZone(watts: Int, ftp: Int): Int {
    if (ftp <= 0) return 0
    val pct = watts.toDouble() / ftp * 100
    val bounds = doubleArrayOf(55.0, 75.0, 90.0, 105.0, 120.0, 150.0)
    return (bounds.indexOfFirst { pct < it }.takeIf { it >= 0 } ?: 6) + 1
}

private fun hms(secs: Long) = String.format(Locale.US, "%d:%02d:%02d", secs / 3600, secs / 60 % 60, secs % 60)

/**
 * What each field shows right now: the value and its unit, "--" when the link
 * doesn't carry it (or there is no link). The live stream while it runs; the
 * status the device reports otherwise.
 */
private class LiveFeed(
    val connected: Boolean,
    val f: LiveFrame?,
    val s: DeviceStatus,
    val miles: Boolean,
    val ftp: Int,
    val clock: String,
) : DashFeed {
    private val r = f?.takeIf { it.hasRide() }?.ride

    private fun watts(field: String): Int? = when {
        !connected -> null
        r != null -> if (field == "power3s") r.takeIf { it.hasPower3SW() }?.power3SW else r.takeIf { it.hasPowerW() }?.powerW
        else -> s.power
    }

    private fun one(v: Double?) = v?.let { String.format(Locale.US, "%.1f", it) } ?: "--"
    private fun whole(v: Double?) = v?.let { String.format(Locale.US, "%.0f", it) } ?: "--"

    override fun text(field: String): String {
        if (!connected && field != "clock") return "--"
        return when (field) {
            "speed" -> one((if (r != null) r.takeIf { it.hasSpeedDkmh() }?.speedDkmh?.div(10.0) else s.speedKmh)?.let { Units.speed(it, miles) })
            "avgspeed" -> one(s.avgSpeedKmh?.let { Units.speed(it, miles) })
            "power3s", "power" -> watts(field)?.toString() ?: "--"
            "hr" -> (if (r != null) r.takeIf { it.hasHrBpm() }?.hrBpm else s.heartRate)?.toString() ?: "--"
            "cadence" -> (if (r != null) r.takeIf { it.hasCadenceRpm() }?.cadenceRpm else s.cadence)?.toString() ?: "--"
            "distance" -> one(s.distanceM?.let { Units.distance(it / 1000.0, miles) })
            "ridetime" -> s.elapsedSec?.let { hms(it) } ?: "--"
            "movingtime" -> s.movingSec?.let { hms(it) } ?: "--"
            "climb" -> whole(s.climbM?.let { Units.elevation(it.toDouble(), miles) })
            "altitude" -> whole(s.altitudeM?.let { Units.elevation(it.toDouble(), miles) })
            "grade" -> one(s.gradePct)
            "battery" -> "${s.battery}"
            "sats" -> "${s.sats}"
            "clock" -> clock
            "routeleft" -> if (s.hasRoute) one(Units.distance(s.remainingKm, miles)) else "--"
            else -> "--"
        }
    }

    override fun unit(field: String): String? = when (field) {
        "speed", "avgspeed" -> Units.speedLabel(miles).uppercase()
        "power3s", "power" -> "W"
        "hr" -> "BPM"
        "cadence" -> "RPM"
        "distance", "routeleft" -> Units.distLabel(miles).uppercase()
        "climb", "altitude" -> Units.elevLabel(miles).uppercase()
        "grade", "battery" -> "%"
        else -> null
    }

    override fun zone(field: String): Int = watts(field)?.let { powerZone(it, ftp) } ?: 0

    override fun radar(): List<Int> =
        f?.takeIf { it.hasRadar() }?.radar?.distancesMList?.map { it } ?: emptyList()
}

// MARK: - pages

/**
 * The MAP page: the e-ink map (muted, desaturated) around the DEVICE's
 * position from the live stream, with the device's rider marker and compass;
 * without one (older firmware, no fix yet) the phone's location stands in.
 * Zoom is the preview's own: there is no command for the device's map zoom.
 */
@Composable
private fun LiveMapPage(
    ble: BleManager,
    fields: List<String>,
    status: PanelStatus,
    feed: LiveFeed,
    frame: LiveFrame?,
    modifier: Modifier,
) {
    val pos = frame?.takeIf { it.hasPosition() }?.position
    val rider = pos?.let { LatLon(it.latE7 / 1e7, it.lonE7 / 1e7) }
    val heading = pos?.takeIf { it.hasCourseDdeg() }?.courseDdeg?.div(10.0)
    var span by remember { mutableStateOf(0.012) }
    // Rounded, so float noise doesn't move the camera.
    val key = rider?.let { String.format(Locale.US, "%.5f,%.5f", it.lat, it.lon) }
    val camera = remember(key, span) {
        rider?.let { MapCamera.region(it, span) }
            ?: ble.lastLocation?.let { MapCamera.region(LatLon(it.latitude, it.longitude), span) }
            ?: MapCamera.followUser()
    }
    MapPagePreview(
        fields, modifier, status, value = feed::text,
        map = { k ->
            Box(Modifier.fillMaxSize()) {
                OsmMap(
                    modifier = Modifier.fillMaxSize(),
                    camera = camera,
                    showUserLocation = rider == null && ble.locationPermission.isGranted,
                    showAttribution = false,
                )
                // The panel is a picture: swipes go to the pager, not the map.
                Box(Modifier.fillMaxSize().pointerInput(Unit) {
                    awaitPointerEventScope { while (true) awaitPointerEvent() }
                })
                if (rider != null) {
                    RiderMark(heading, k, Modifier.align(Alignment.Center))
                }
                CompassMark(k, Modifier.offset(((540 - 76) * k).dp, (18 * k).dp))
                MapAttribution(Modifier.align(Alignment.BottomStart).padding(4.dp))
            }
        },
        onZoom = { dir -> span = (if (dir > 0) span / 2 else span * 2).coerceIn(0.002, 2.0) },
    )
}

/**
 * The MUSIC page, live: what this phone is playing (the same session the
 * device's page shows), the cover dithered to the panel's tones, and the
 * buttons driving that player.
 */
@Composable
private fun LiveMusicPage(ble: BleManager, status: PanelStatus, modifier: Modifier) {
    val track = ble.phoneTrack
    val art = remember(track?.gray) { track?.gray?.let { panelArt(it, track.artSide) } }
    var tick by remember { mutableLongStateOf(SystemClock.elapsedRealtime()) }
    LaunchedEffect(track?.playing, track?.positionMs) {
        while (track?.playing == true) {
            tick = SystemClock.elapsedRealtime()
            delay(500)
        }
    }
    val progress = track?.takeIf { it.durationMs > 0 }?.let {
        (it.positionAt(tick) / 1000).toInt() to ((it.durationMs + 500) / 1000).toInt()
    }
    MusicPreview(
        modifier, status,
        track = track?.let { PanelTrack(it.title, it.artist, it.album) },
        art = art,
        playing = track?.playing == true,
        progress = progress,
        onTransport = { b -> ble.phoneMediaCommand(if (b == 0) 3 else if (b == 1) 1 else 2) },
        transportEnabled = track != null,
        onVolume = { v -> ble.phoneMediaCommand(if (v == 0) 4 else 5) },
    )
}

/**
 * The STOPWATCH page from the live stream: the device's clock, counted on
 * between frames while it runs, and once a lap is done the device's lap
 * layout. START/PAUSE, RESET and LAP drive the device's own clock, enabled by
 * its rules; the next frame redraws the page.
 */
@Composable
private fun LiveStopwatchPage(
    ble: BleManager,
    status: PanelStatus,
    frame: LiveFrame?,
    frameAt: Long,
    modifier: Modifier,
) {
    val connected = ble.state == BleManager.ConnState.CONNECTED
    val sw = frame?.takeIf { it.hasStopwatch() }?.stopwatch
    if (sw == null) {
        StopwatchPagePreview(
            modifier, status, time = "--:--", state = "--", running = false,
            note = if (connected && !ble.has(Feature.FEATURE_LIVE)) "Update the device's firmware to see its stopwatch here" else null,
            controlsEnabled = false,
        )
        return
    }
    var tick by remember { mutableLongStateOf(SystemClock.elapsedRealtime()) }
    LaunchedEffect(sw.running, frameAt) {
        tick = SystemClock.elapsedRealtime()
        while (sw.running) {
            delay(250)
            tick = SystemClock.elapsedRealtime()
        }
    }
    val base = sw.elapsedMs.toLong() and 0xFFFF_FFFFL
    val ms = if (sw.running) base + (tick - frameAt).coerceAtLeast(0) else base
    val lapMs = (sw.lapMs.toLong() and 0xFFFF_FFFFL) + (ms - base)
    val laps = sw.laps
    val withLaps = ble.has(Feature.FEATURE_STOPWATCH_LAPS)
    StopwatchPagePreview(
        modifier, status,
        time = stopwatchTimeText(ms),
        state = if (sw.running) "RUNNING" else if (ms == 0L) "READY" else "PAUSED",
        running = sw.running,
        lapPage = if (laps == 0) null else StopwatchLapPage(
            totalMs = ms, running = sw.running, lapNumber = laps + 1, lapMs = lapMs,
            lapSpeedKmh = sw.takeIf { it.hasLapSpeedDkmh() }?.lapSpeedDkmh?.div(10.0),
            lapCadence = sw.takeIf { it.hasLapCadenceRpm() }?.lapCadenceRpm,
            lapPowerW = sw.takeIf { it.hasLapPowerW() }?.lapPowerW,
            showCadence = sw.showsCadence, showPower = sw.showsPower,
            useMiles = ble.useMiles,
            rows = if (!withLaps) null else sw.recentList.map {
                StopwatchLapRow(
                    it.number, it.splitMs.toLong() and 0xFFFF_FFFFL,
                    it.takeIf { l -> l.hasSpeedDkmh() }?.speedDkmh?.div(10.0),
                    it.takeIf { l -> l.hasCadenceRpm() }?.cadenceRpm,
                    it.takeIf { l -> l.hasPowerW() }?.powerW,
                )
            },
        ),
        controlsEnabled = ble.has(Feature.FEATURE_STOPWATCH),
        canReset = !sw.running && sw.elapsedMs > 0,
        canLap = sw.running && laps < 99,
        onControl = { i ->
            ble.stopwatchCommand(
                when (i) {
                    0 -> StopwatchOp.STOPWATCH_OP_TOGGLE
                    1 -> StopwatchOp.STOPWATCH_OP_RESET
                    else -> StopwatchOp.STOPWATCH_OP_LAP
                },
            )
        },
    )
}

/** Workout files read off the device for the WORKOUT page's block profile, by name. */
private object WorkoutFiles {
    val texts = mutableStateMapOf<String, String>()
}

/**
 * Reads the loaded workout's file off the device (once per name) while the
 * WORKOUT page is up, for its block profile. Stands aside while the Workouts
 * sheet is open: that sheet uses the same fetch to open its editor.
 */
@Composable
private fun WorkoutFileFetcher(ble: BleManager, onWorkoutPage: Boolean, sheetOpen: Boolean) {
    val w = ble.workoutStatus
    val want = onWorkoutPage && !sheetOpen && ble.state == BleManager.ConnState.CONNECTED &&
        w.loaded && w.name.isNotEmpty() && w.name !in WorkoutFiles.texts
    LaunchedEffect(want, w.name) { if (want) ble.fetchWorkout(w.name) }
    LaunchedEffect(ble.fetchedWorkout, sheetOpen) {
        val f = ble.fetchedWorkout ?: return@LaunchedEffect
        if (!sheetOpen && f.name == ble.workoutStatus.name) {
            WorkoutFiles.texts[f.name] = f.text
            ble.fetchedWorkout = null
        }
    }
}

/** The WORKOUT page's model: the device's session status, the loaded file's
 *  blocks, live power, and the device's /workouts for the picker. */
private fun workoutModel(ble: BleManager): WorkoutPageModel {
    val w = ble.workoutStatus
    val connected = ble.state == BleManager.ConnState.CONNECTED
    val ftp = if (w.ftpW > 0) w.ftpW else ble.ftpWatts
    val text = WorkoutFiles.texts[w.name]
    return WorkoutPageModel(
        loaded = connected && w.loaded,
        name = w.name,
        blockIdx = w.blockIndex,
        blockCount = w.blockCount,
        running = w.running,
        paused = w.paused,
        done = w.done,
        elapsed = w.elapsedSec.toInt(),
        total = w.totalSec.toInt(),
        targetW = w.targetW,
        ftpW = ftp,
        power = if (connected) ble.status.power else null,
        blocks = text?.let { WorkoutPageModel.blocks(it, ftp) } ?: emptyList(),
        picker = ble.deviceWorkouts,
        fileUnavailable = text == null && ble.workoutMessage?.startsWith("Failed") == true,
    )
}

/** The WORKOUT page's taps, as the device's own taps run them. */
private fun workoutActions(ble: BleManager) = WorkoutPageActions(
    toggle = {
        val w = ble.workoutStatus
        val started = w.running || w.paused || w.done || w.elapsedSec > 0
        when {
            !started -> ble.workoutStart()
            w.running -> ble.workoutPause()
            else -> ble.workoutResume()
        }
    },
    jump = { ble.workoutJump(it) },
    unload = { ble.workoutUnload() },
    load = { ble.loadWorkout(it) },
    refresh = { ble.refreshWorkouts() },
)

/**
 * The last page: add a page to the device's cycle, which then syncs (now, or
 * on the next connect) like an edit made in the editor.
 */
@Composable
private fun AddNewPage(ble: BleManager, config: DashConfig, modifier: Modifier, onAdded: (Int) -> Unit) {
    var menu by remember { mutableStateOf(false) }
    val full = config.contentPages >= ble.dashMaxPages
    Box(
        modifier
            .aspectRatio(PANEL_W / PANEL_H)
            .background(Palette.paper)
            .clipToBounds()
            .drawBehind {
                val inset = 14.dp.toPx()
                val w = 2.dp.toPx()
                val dash = PathEffect.dashPathEffect(floatArrayOf(8.dp.toPx(), 6.dp.toPx()))
                drawRect(
                    Color.Black,
                    topLeft = Offset(inset, inset),
                    size = androidx.compose.ui.geometry.Size(size.width - 2 * inset, size.height - 2 * inset),
                    style = androidx.compose.ui.graphics.drawscope.Stroke(w, pathEffect = dash),
                )
            },
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.padding(horizontal = 24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Icon(Icons.Filled.Add, contentDescription = null, tint = Palette.ink, modifier = Modifier.size(44.dp))
            Text("ADD NEW", style = barlow(22.sp, FontWeight.Bold).copy(letterSpacing = 3.sp), color = Palette.ink)
            val note = when {
                ble.dashTarget == null -> "Connect once to load your device's pages, then add more here."
                full -> "The device holds ${ble.dashMaxPages} pages besides the map. Remove one in Edit to add another."
                else -> null
            }
            if (note != null) {
                Text(note, style = barlow(13.sp), color = Palette.ink, textAlign = TextAlign.Center)
            } else {
                Box {
                    BlockButton("Choose a page", icon = Icons.Filled.Add) { menu = true }
                    DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                        fun add(p: DashConfig.Page) {
                            menu = false
                            val c = ble.dashTarget ?: return
                            ble.saveDashConfig(c.copy(pages = c.pages + p))
                            onAdded(c.pages.size)
                        }
                        DropdownMenuItem(text = { Text("Data page") }, onClick = {
                            add(DashConfig.Page.fields(DashLayout.deviceDefault))
                        })
                        if (!config.hasMusicPage) {
                            DropdownMenuItem(text = { Text("Music controls") }, onClick = { add(DashConfig.Page.music()) })
                        }
                        if (!config.hasWorkoutPage) {
                            DropdownMenuItem(text = { Text("Workout") }, onClick = { add(DashConfig.Page.workout()) })
                        }
                        if (!config.hasStopwatchPage) {
                            DropdownMenuItem(text = { Text("Stopwatch") }, onClick = { add(DashConfig.Page.stopwatch()) })
                        }
                    }
                }
            }
        }
    }
}
