package com.raemond.opentrailpaper.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.DirectionsBike
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.ChevronLeft
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.FolderOpen
import androidx.compose.material.icons.filled.Route
import androidx.compose.material.icons.outlined.ErrorOutline
import androidx.compose.material.icons.outlined.Lock
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.data.CloudRoute
import com.raemond.opentrailpaper.data.CloudRoutes
import com.raemond.opentrailpaper.data.LatLon
import com.raemond.opentrailpaper.data.RouteImport
import com.raemond.opentrailpaper.data.SyncAccounts
import com.raemond.opentrailpaper.data.Units
import kotlinx.coroutines.launch
import java.util.Locale
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min

/**
 * Where an import comes from: a GPX on the phone, or a connected account's
 * saved routes. Only shown with an account connected; without one the Route
 * screen's button goes straight to the file picker.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun RouteSourceSheet(
    accounts: List<SyncAccounts.Provider>,
    onFile: () -> Unit,
    onAccount: (SyncAccounts.Provider) -> Unit,
    onDismiss: () -> Unit,
) {
    val state = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val scope = rememberCoroutineScope()
    // Slide the sheet away before acting, so the next screen doesn't open
    // underneath one still on its way down.
    fun close(then: () -> Unit = {}) {
        scope.launch { state.hide() }.invokeOnCompletion { onDismiss(); then() }
    }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = state,
        containerColor = Palette.paper,
        shape = RoundedCornerShape(topStart = 26.dp, topEnd = 26.dp),
        dragHandle = {
            Box(
                Modifier
                    .padding(top = 10.dp, bottom = 4.dp)
                    .size(width = 38.dp, height = 5.dp)
                    .background(Palette.hairline, RoundedCornerShape(3.dp)),
            )
        },
    ) {
        Column(
            Modifier.padding(start = 16.dp, end = 16.dp, top = 10.dp, bottom = 24.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            // Title and subtitle sit closer than the sheet's own spacing.
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "Import a route",
                        style = condensed(30.sp, FontWeight.Bold),
                        color = Palette.ink,
                        modifier = Modifier.weight(1f),
                    )
                    RoundIconButton(Icons.Filled.Close, "Close", tint = Palette.muted) { close() }
                }
                Text(
                    "Pick one of your saved routes. It goes to the device like any route you plan here.",
                    style = barlow(15.sp),
                    color = Palette.muted,
                )
            }
            Card(padding = 0.dp) {
                SourceRow(Icons.Filled.FolderOpen, "From a file", "GPX from your phone", connected = false) {
                    close(onFile)
                }
                for (p in accounts) {
                    HorizontalDivider(Modifier.padding(start = 74.dp), color = Palette.hairline)
                    SourceRow(
                        if (p == SyncAccounts.Provider.STRAVA) Icons.Filled.Route
                        else Icons.AutoMirrored.Filled.DirectionsBike,
                        p.title, "Connected · saved routes", connected = true,
                    ) { close { onAccount(p) } }
                }
            }
            // Guidance, not a button: nothing here can open Settings at its
            // Accounts card, and a tab switch that lands elsewhere is worse.
            if (SyncAccounts.routeProviders.any { it !in accounts }) {
                Card(padding = 0.dp) {
                    Row(
                        Modifier.padding(horizontal = 16.dp, vertical = 14.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(14.dp),
                    ) {
                        Box(
                            Modifier.size(44.dp).dashedBorder(12.dp),
                            contentAlignment = Alignment.Center,
                        ) {
                            Icon(Icons.Filled.Add, null, tint = Palette.muted, modifier = Modifier.size(20.dp))
                        }
                        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Text("Connect another account", style = TypeScale.bodyStrong, color = Palette.ink)
                            Text("Settings › Accounts", style = barlow(13.sp), color = Palette.muted)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SourceRow(
    icon: ImageVector,
    title: String,
    detail: String,
    connected: Boolean,
    onClick: () -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(16.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Box(
            Modifier.size(44.dp).background(Palette.accentWash, RoundedCornerShape(12.dp)),
            contentAlignment = Alignment.Center,
        ) {
            Icon(icon, null, tint = Palette.accent, modifier = Modifier.size(22.dp))
        }
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, style = condensed(21.sp, FontWeight.SemiBold), color = Palette.ink)
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                if (connected) Box(Modifier.size(7.dp).background(Palette.good, CircleShape))
                Text(detail, style = barlow(13.sp), color = Palette.muted)
            }
        }
        Icon(Icons.Filled.ChevronRight, null, tint = Palette.muted, modifier = Modifier.size(22.dp))
    }
}

/**
 * The rider's saved routes on Strava or RideWithGPS. Picking one downloads its
 * GPX and hands it to [RouteImport], so from there on it is a file import:
 * same parse, same preview, same upload to the device. [onBack] returns to the
 * source picker; [onDismiss] closes both.
 */
@Composable
fun CloudRoutesSheet(
    provider: SyncAccounts.Provider,
    useMiles: Boolean,
    onBack: () -> Unit,
    onDismiss: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    val routes = remember { mutableStateListOf<CloudRoute>() }
    var nextPage by remember { mutableStateOf<Int?>(1) }
    var loading by remember { mutableStateOf(false) }
    var loadedOnce by remember { mutableStateOf(false) }
    /** A page that didn't load; its message, when the request gave one. */
    var loadFailed by remember { mutableStateOf(false) }
    var loadError by remember { mutableStateOf<String?>(null) }
    /** A route that didn't download — the list itself is fine. */
    var importError by remember { mutableStateOf<String?>(null) }
    var importing by remember { mutableStateOf<String?>(null) }

    fun loadMore() {
        val page = nextPage ?: return
        if (loading) return
        loading = true
        loadFailed = false
        loadError = null
        scope.launch {
            try {
                val got = SyncAccounts.routes(provider, page)
                routes.addAll(got.routes.filter { r -> routes.none { it.id == r.id } })
                nextPage = got.nextPage
            } catch (e: Exception) {
                loadFailed = true
                loadError = e.message
            } finally {
                loading = false
                loadedOnce = true
            }
        }
    }

    LaunchedEffect(provider) { loadMore() }

    FullScreenCover(onDismiss = onBack) {
        Column(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.systemBars)) {
            Column(
                Modifier.padding(start = 16.dp, end = 16.dp, top = 10.dp, bottom = 10.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    RoundIconButton(Icons.Filled.ChevronLeft, "Back to sources", tint = Palette.ink, onClick = onBack)
                    RoundIconButton(Icons.Filled.Close, "Close", tint = Palette.muted, onClick = onDismiss)
                }
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Bottom) {
                    Text(
                        provider.title,
                        style = TypeScale.screenTitle,
                        color = Palette.ink,
                        modifier = Modifier.weight(1f).alignByBaseline(),
                    )
                    val count = when {
                        routes.isNotEmpty() -> "${routes.size} saved route${if (routes.size == 1) "" else "s"}"
                        loadedOnce && !loadFailed && nextPage == null -> "No routes yet"
                        loadFailed -> "Couldn't load"
                        else -> null
                    }
                    count?.let { TrackedLabel(it, Modifier.alignByBaseline()) }
                }
            }
            HorizontalDivider(color = Palette.hairline)
            importError?.let {
                Box(Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp)) {
                    ErrorCard("Couldn't import that route", it, onRetry = null)
                }
            }

            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 40.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                if (provider == SyncAccounts.Provider.STRAVA &&
                    !CloudRoutes.stravaSeesPrivateRoutes(SyncAccounts.tokens[provider]?.scope)
                ) {
                    item(key = "private-note") { PrivateRoutesNote() }
                }

                items(routes, key = { it.id }) { r ->
                    CloudRouteRow(
                        route = r,
                        useMiles = useMiles,
                        importing = importing == r.id,
                        dimmed = importing != null && importing != r.id,
                        enabled = importing == null,
                    ) {
                        importing = r.id
                        importError = null
                        scope.launch {
                            try {
                                val gpx = SyncAccounts.routeGpx(provider, r)
                                // A GPX that will not parse is reported by
                                // RouteImport on the Route screen, like a bad file.
                                RouteImport.importGpx(gpx, r.name, provider.title, r.elevationGainMeters)
                                onDismiss()
                            } catch (e: Exception) {
                                importError = e.message ?: "Couldn't download that route"
                            } finally {
                                importing = null
                            }
                        }
                    }
                }

                item(key = "footer") {
                    when {
                        loading && routes.isEmpty() -> Box(
                            Modifier.fillMaxWidth().padding(vertical = 40.dp),
                            contentAlignment = Alignment.Center,
                        ) { CircularProgressIndicator(color = Palette.accent) }
                        // The LazyColumn only composes this footer once it scrolls
                        // into view, so reaching the bottom loads the next page
                        // (and again straight away if a short page leaves it on screen).
                        loading || (nextPage != null && loadedOnce && !loadFailed) -> {
                            LoadingMore()
                            if (!loading) LaunchedEffect(nextPage) { loadMore() }
                        }
                        loadFailed -> ErrorCard(
                            "Your routes didn't load",
                            loadError ?: "Check your connection, then try again.",
                        ) { loadMore() }
                        loadedOnce && routes.isEmpty() -> EmptyCard(provider.title)
                    }
                }
            }
        }
    }
}

@Composable
private fun CloudRouteRow(
    route: CloudRoute,
    useMiles: Boolean,
    importing: Boolean,
    dimmed: Boolean,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .alpha(if (dimmed) 0.45f else 1f)
            .background(Palette.surface, RoundedCornerShape(18.dp))
            .border(1.dp, Palette.hairline, RoundedCornerShape(18.dp))
            .clickable(enabled = enabled, onClick = onClick)
            .padding(start = 12.dp, top = 12.dp, bottom = 12.dp, end = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        RouteThumb(route.shape)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(
                route.name,
                style = condensed(20.sp, FontWeight.SemiBold),
                color = Palette.ink,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                route.distanceMeters?.let {
                    Stat(
                        String.format(Locale.US, "%.1f", Units.distance(it / 1000, useMiles)),
                        Units.distLabel(useMiles),
                    )
                }
                route.elevationGainMeters?.let {
                    Stat(
                        String.format(Locale.US, "%,.0f", Units.elevation(it, useMiles)),
                        "${Units.elevLabel(useMiles)} climb",
                    )
                }
            }
        }
        if (importing) {
            CircularProgressIndicator(Modifier.size(18.dp), color = Palette.accent, strokeWidth = 2.dp)
        } else {
            Icon(Icons.Filled.ChevronRight, null, tint = Palette.faint, modifier = Modifier.size(22.dp))
        }
    }
}

/** "58.4 km": the number in the condensed face, the unit small and muted beside it. */
@Composable
private fun Stat(value: String, unit: String) {
    Row(horizontalArrangement = Arrangement.spacedBy(3.dp)) {
        Text(value, style = condensed(18.sp, FontWeight.SemiBold), color = Palette.ink, modifier = Modifier.alignByBaseline())
        Text(unit, style = barlow(12.sp), color = Palette.muted, modifier = Modifier.alignByBaseline())
    }
}

/**
 * The route's outline, fitted to the tile without stretching. Longitude is
 * scaled by cos(latitude) so a square of ground draws square.
 */
@Composable
private fun RouteThumb(shape: List<LatLon>?) {
    Box(
        Modifier
            .size(58.dp)
            .background(Palette.paper, RoundedCornerShape(12.dp))
            .border(1.dp, Palette.hairline, RoundedCornerShape(12.dp)),
        contentAlignment = Alignment.Center,
    ) {
        if (shape == null) {
            Icon(Icons.Filled.Route, null, tint = Palette.faint, modifier = Modifier.size(26.dp))
            return@Box
        }
        val projected = remember(shape) {
            val k = cos(Math.toRadians(shape.sumOf { it.lat } / shape.size))
            shape.map { Offset((it.lon * k).toFloat(), (-it.lat).toFloat()) }
        }
        Canvas(Modifier.size(44.dp)) {
            val stroke = 2.4.dp.toPx()
            val minX = projected.minOf { it.x }
            val minY = projected.minOf { it.y }
            val spanX = projected.maxOf { it.x } - minX
            val spanY = projected.maxOf { it.y } - minY
            val w = size.width - stroke
            val h = size.height - stroke
            val scale = min(w / max(spanX, 1e-9f), h / max(spanY, 1e-9f))
            val dx = (size.width - spanX * scale) / 2
            val dy = (size.height - spanY * scale) / 2
            val path = Path()
            projected.forEachIndexed { i, p ->
                val x = dx + (p.x - minX) * scale
                val y = dy + (p.y - minY) * scale
                if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
            }
            drawPath(
                path, Palette.accent,
                style = Stroke(stroke, cap = StrokeCap.Round, join = StrokeJoin.Round),
            )
        }
    }
}

@Composable
private fun LoadingMore() {
    Row(
        Modifier.fillMaxWidth().padding(vertical = 14.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        CircularProgressIndicator(
            Modifier.size(16.dp), color = Palette.accent, trackColor = Palette.hairline, strokeWidth = 2.5.dp,
        )
        TrackedLabel("Loading more")
    }
}

@Composable
private fun EmptyCard(provider: String) {
    Card(padding = 0.dp) {
        Column(
            Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            DashedRouteGlyph()
            Text(
                "Nothing saved on $provider",
                style = condensed(21.sp, FontWeight.SemiBold),
                color = Palette.ink,
                textAlign = TextAlign.Center,
            )
            Text(
                "Routes you save there show up here.",
                style = barlow(14.sp),
                color = Palette.muted,
                textAlign = TextAlign.Center,
            )
        }
    }
}

/** The mockup's dotted line from a start dot: a route not drawn yet. */
@Composable
private fun DashedRouteGlyph() {
    Canvas(Modifier.size(40.dp)) {
        val u = size.width / 40
        val path = Path().apply {
            moveTo(6 * u, 32 * u)
            cubicTo(12 * u, 20 * u, 16 * u, 26 * u, 22 * u, 16 * u)
            cubicTo(28 * u, 6 * u, 32 * u, 10 * u, 34 * u, 8 * u)
        }
        drawPath(
            path, Palette.faint,
            style = Stroke(
                2.4.dp.toPx(), cap = StrokeCap.Round,
                pathEffect = PathEffect.dashPathEffect(floatArrayOf(4 * u, 5 * u)),
            ),
        )
        drawCircle(Palette.faint, 3 * u, Offset(6 * u, 32 * u))
    }
}

/** A load or download that failed; [onRetry] adds the "Try again" pill. */
@Composable
private fun ErrorCard(title: String, detail: String?, onRetry: (() -> Unit)?) {
    Card {
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Icon(
                Icons.Outlined.ErrorOutline, null, tint = Palette.accentDark,
                modifier = Modifier.padding(top = 1.dp).size(22.dp),
            )
            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(title, style = TypeScale.bodyStrong, color = Palette.ink)
                detail?.let { Text(it, style = barlow(14.sp), color = Palette.muted) }
            }
        }
        if (onRetry != null) {
            SecondaryButton("Try again", Modifier.padding(top = 12.dp), onClick = onRetry)
        }
    }
}

/**
 * Strava without read_all lists public routes only. Reconnecting is done in
 * Settings, which this sheet cannot open at the Accounts card, so it says where.
 */
@Composable
private fun PrivateRoutesNote() {
    Row(
        Modifier
            .fillMaxWidth()
            .background(Palette.accentWash, RoundedCornerShape(18.dp))
            .padding(horizontal = 16.dp, vertical = 14.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Icon(
            Icons.Outlined.Lock, null, tint = Palette.accentDark,
            modifier = Modifier.padding(top = 2.dp).size(20.dp),
        )
        Text(
            "Private routes are hidden. Reconnect Strava in Settings › Accounts to include them.",
            style = barlow(14.sp),
            color = Palette.ink,
        )
    }
}

/** The 44dp round chrome button of the import screens. */
@Composable
private fun RoundIconButton(
    icon: ImageVector,
    contentDescription: String,
    tint: androidx.compose.ui.graphics.Color,
    onClick: () -> Unit,
) {
    Box(
        Modifier
            .size(44.dp)
            .background(Palette.surface, CircleShape)
            .border(1.dp, Palette.hairline, CircleShape)
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription, tint = tint, modifier = Modifier.size(22.dp))
    }
}

/** A dashed hairline-weight outline in [Palette.faint]: a slot not filled yet. */
private fun Modifier.dashedBorder(radius: androidx.compose.ui.unit.Dp) = drawBehind {
    val w = 1.dp.toPx()
    drawRoundRect(
        Palette.faint,
        topLeft = Offset(w / 2, w / 2),
        size = androidx.compose.ui.geometry.Size(size.width - w, size.height - w),
        cornerRadius = CornerRadius(radius.toPx()),
        style = Stroke(w, pathEffect = PathEffect.dashPathEffect(floatArrayOf(3.dp.toPx(), 3.dp.toPx()))),
    )
}
