package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.raemond.opentrailpaper.data.BoundingBox
import com.raemond.opentrailpaper.data.LatLon
import com.raemond.opentrailpaper.map.HexOverlay
import com.raemond.opentrailpaper.map.MapMarkers
import com.raemond.opentrailpaper.map.MapStyle
import com.raemond.opentrailpaper.map.MercatorWorld
import com.raemond.opentrailpaper.map.OutlineHex
import com.raemond.opentrailpaper.map.SelectionHex
import org.osmdroid.events.MapEventsReceiver
import org.osmdroid.events.MapListener
import org.osmdroid.events.ScrollEvent
import org.osmdroid.events.ZoomEvent
import org.osmdroid.tileprovider.MapTileProviderBasic
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.CustomZoomButtonsController
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.MapEventsOverlay
import org.osmdroid.views.overlay.Marker
import org.osmdroid.views.overlay.TilesOverlay
import org.osmdroid.views.overlay.Polyline
import org.osmdroid.views.overlay.mylocation.GpsMyLocationProvider
import org.osmdroid.views.overlay.mylocation.MyLocationNewOverlay
import java.util.UUID
import kotlin.math.pow

/**
 * A one-shot camera move. Identity is the token, so re-issuing the same target
 * (tapping "recenter" twice) still moves the map.
 */
class MapCamera private constructor(val target: Target) {
    sealed interface Target {
        data class Region(val center: LatLon, val spanDeg: Double) : Target
        data class Box(val box: BoundingBox, val paddingPx: Int) : Target
        data object FollowUser : Target
    }

    val token: UUID = UUID.randomUUID()

    override fun equals(other: Any?) = other is MapCamera && other.token == token
    override fun hashCode() = token.hashCode()

    companion object {
        fun region(center: LatLon, spanDeg: Double) = MapCamera(Target.Region(center, spanDeg))
        fun box(box: BoundingBox, paddingPx: Int = 48) = MapCamera(Target.Box(box, paddingPx))
        fun followUser() = MapCamera(Target.FollowUser)
    }
}

/**
 * Screen point -> coordinate, for the gestures Compose still owns (the
 * drag-a-box selection on the Maps screen). Handed the live map by [OsmMap].
 */
class MapProjector {
    internal var map: MapView? = null

    fun coordinate(x: Float, y: Float): LatLon? {
        val m = map ?: return null
        if (m.width == 0) return null
        val geo = m.projection.fromPixels(x.toInt(), y.toInt())
        return LatLon(geo.latitude, geo.longitude)
    }
}

/** A pin the Route screen drops on the chosen destination. */
data class MapDestination(val name: String, val coordinate: LatLon)

/**
 * A mesh node to drop on the map. Kept as a flat value rather than passing a
 * MeshNode in, so the map layer stays ignorant of what a mesh is.
 */
data class MeshNodePin(
    val id: Int,
    val label: String,          // shown on the callout
    val detail: String,         // second callout line: signal, age, precision
    val coordinate: LatLon,
    /**
     * The sender deliberately blurred this position. Drawn hollow, because a
     * solid pin on a coordinate the sender rounded off would be a lie.
     */
    val imprecise: Boolean,
)

/**
 * The map both the Route and Maps screens draw on: standard OSM, with the areas
 * this phone or the device holds drawn over it as hexagons — green with a check
 * where the DEVICE has them too, a quiet fill where they are only on the phone.
 *
 * It used to paint each downloaded area in the head unit's own 1-bit style. That
 * is gone; see [com.raemond.opentrailpaper.map.EInkTileStore] for why. The
 * question this map answers is which ground is covered and whether the device has
 * it, and hexagons answer it without decoding a byte of map geometry.
 *
 * A real `MapView` behind `AndroidView`, for the same reason iOS wraps `MKMapView`
 * in a `UIViewRepresentable` rather than using SwiftUI's `Map`: overlays at this
 * count have to be drawn inside the map's own draw pass to stay pinned to the
 * ground while panning.
 */
@Composable
fun OsmMap(
    modifier: Modifier = Modifier,
    outlines: List<OutlineHex> = emptyList(),
    selection: List<SelectionHex> = emptyList(),
    route: List<LatLon>? = null,
    destination: MapDestination? = null,
    meshNodes: List<MeshNodePin> = emptyList(),
    camera: MapCamera? = null,
    showUserLocation: Boolean = false,
    /**
     * How much of the map's bottom edge is covered by floating UI. The map itself
     * runs full-bleed under the card, so this is what keeps it from *framing*
     * full height: a camera move fits its target into the band above the card
     * instead of centring it behind one.
     */
    bottomInsetPx: Int = 0,
    projector: MapProjector? = null,
    onTap: ((LatLon) -> Unit)? = null,
    onLongPress: ((LatLon) -> Unit)? = null,
    onRegionChange: ((BoundingBox) -> Unit)? = null,
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current

    val mapView = remember {
        MapView(context).apply {
            setTileSource(MapStyle.base)
            setMultiTouchControls(true)
            zoomController.setVisibility(CustomZoomButtonsController.Visibility.NEVER)
            // osmdroid repeats the world sideways by default, which puts a second
            // copy of every downloaded hexagon on screen at low zoom.
            isHorizontalMapRepetitionEnabled = false
            isVerticalMapRepetitionEnabled = false
            setBackgroundColor(Palette.paper.toArgb())
            // Tiles that haven't arrived show paper rather than osmdroid's grey
            // graph-paper grid, so a map loading looks like a map, not a fault.
            overlayManager.tilesOverlay.loadingBackgroundColor = Palette.paper.toArgb()
            overlayManager.tilesOverlay.loadingLineColor = Palette.hairline.toArgb()
            controller.setZoom(12.0)
        }
    }

    // Held across recompositions so each update can diff rather than rebuild.
    val state = remember { MapState() }

    // Place names, as their own transparent layer. See MapStyle: this is what
    // keeps a covered area readable once a coverage hexagon is tinted over it.
    val labelsOverlay = remember {
        TilesOverlay(MapTileProviderBasic(context, MapStyle.labels), context).apply {
            loadingBackgroundColor = android.graphics.Color.TRANSPARENT
            loadingLineColor = android.graphics.Color.TRANSPARENT
        }
    }

    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> mapView.onResume()
                Lifecycle.Event.ON_PAUSE -> mapView.onPause()
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            state.locationOverlay?.disableMyLocation()
            labelsOverlay.onDetach(mapView)
            mapView.onDetach()
        }
    }

    Box(modifier) {
    AndroidView(
        modifier = Modifier.matchParentSize(),
        factory = { mapView },
        update = { map ->
            state.onTap = onTap
            state.onLongPress = onLongPress
            state.onRegionChange = onRegionChange
            projector?.map = map

            if (!state.wired) {
                state.wired = true
                map.overlays.add(
                    MapEventsOverlay(object : MapEventsReceiver {
                        override fun singleTapConfirmedHelper(p: GeoPoint?): Boolean {
                            p ?: return false
                            state.onTap?.invoke(LatLon(p.latitude, p.longitude))
                            return state.onTap != null
                        }

                        override fun longPressHelper(p: GeoPoint?): Boolean {
                            p ?: return false
                            state.onLongPress?.invoke(LatLon(p.latitude, p.longitude))
                            return state.onLongPress != null
                        }
                    }),
                )
                map.addMapListener(object : MapListener {
                    override fun onScroll(event: ScrollEvent?): Boolean {
                        state.publishRegion(map)
                        return false
                    }

                    override fun onZoom(event: ZoomEvent?): Boolean {
                        state.publishRegion(map)
                        return false
                    }
                })
            }

            if (showUserLocation && state.locationOverlay == null) {
                val overlay = MyLocationNewOverlay(GpsMyLocationProvider(context), map)
                // osmdroid's stock marker is a large arrow-and-person sprite that
                // belongs to a different decade of map design. The blue dot is
                // what "you are here" looks like on both phones.
                val dot = MapMarkers.locationDot(map.resources.displayMetrics.density)
                overlay.setPersonIcon(dot)
                overlay.setDirectionIcon(dot)
                overlay.setPersonAnchor(0.5f, 0.5f)
                overlay.setDirectionAnchor(0.5f, 0.5f)
                overlay.enableMyLocation()
                state.locationOverlay = overlay
                map.overlays.add(overlay)
            } else if (!showUserLocation) {
                state.locationOverlay?.let {
                    it.disableMyLocation()
                    map.overlays.remove(it)
                }
                state.locationOverlay = null
            }

            // Overlays are expensive to rebuild, and Compose calls this for any
            // state change on the host screen (a progress string, a text field).
            // Only touch the map when what's actually drawn has changed.
            val signature =
                contentSignature(outlines, selection, route, destination, meshNodes)
            if (signature != state.contentKey) {
                state.contentKey = signature
                rebuildContent(
                    map,
                    state,
                    map.resources.displayMetrics.density,
                    labelsOverlay,
                    outlines,
                    selection,
                    route,
                    destination,
                    meshNodes,
                )
            }

            if (camera != null && camera != state.lastCamera) {
                state.lastCamera = camera
                apply(map, state, camera, bottomInsetPx)
            }
            map.invalidate()
        },
    )

    // Required by OpenStreetMap and by CARTO. Bottom-right, which is where a map
    // conventionally puts this and the one corner no screen here floats a
    // control over — top-left put it adrift in the middle of the view, under
    // the screen's title rather than beside it.
    //
    // Lifted by the same inset the camera uses, so it sits just above whatever
    // card is floating over the map instead of behind it.
    Text(
        MapStyle.attribution,
        style = barlow(9.sp),
        color = Palette.muted,
        maxLines = 1,
        modifier = Modifier
            .align(Alignment.BottomEnd)
            .windowInsetsPadding(WindowInsets.navigationBars)
            .padding(
                end = 10.dp,
                bottom = 8.dp + with(LocalDensity.current) { bottomInsetPx.toDp() },
            )
            .background(Palette.paper.copy(alpha = 0.72f), RoundedCornerShape(4.dp))
            .padding(horizontal = 5.dp, vertical = 2.dp),
    )
    }
}

/**
 * Everything that changes what's drawn, and nothing that doesn't. The route is in
 * here because overlays draw in list order — it has to be re-added on top
 * whenever the hexes underneath are rebuilt, or a coverage fill paints over it.
 */
private fun contentSignature(
    outlines: List<OutlineHex>,
    selection: List<SelectionHex>,
    route: List<LatLon>?,
    destination: MapDestination?,
    meshNodes: List<MeshNodePin>,
): Int {
    // A hash, not a joined string. This runs on every Compose update of the host
    // screen, and building a comma-joined list of several hundred hex ids each
    // time was itself a measurable part of the Maps page's stutter during a
    // large download.
    var h = 17
    for (o in outlines) {
        h = h * 31 + o.id.hashCode() + (if (o.synced) 2 else 0) + (if (o.missing) 4 else 0)
    }
    for (s in selection) h = h * 31 + s.id.hashCode() + s.kind.ordinal
    h = h * 31 + (route?.size ?: 0)
    route?.firstOrNull()?.let { h = h * 31 + it.hashCode() }
    route?.lastOrNull()?.let { h = h * 31 + it.hashCode() }
    h = h * 31 + (destination?.hashCode() ?: 0)
    for (n in meshNodes) h = h * 31 + n.hashCode()
    return h
}

private fun rebuildContent(
    map: MapView,
    state: MapState,
    density: Float,
    labelsOverlay: TilesOverlay,
    outlines: List<OutlineHex>,
    selection: List<SelectionHex>,
    route: List<LatLon>?,
    destination: MapDestination?,
    meshNodes: List<MeshNodePin>,
) {
    state.contentOverlays.forEach { map.overlays.remove(it) }
    state.contentOverlays.clear()

    fun add(overlay: org.osmdroid.views.overlay.Overlay) {
        // Below the location overlay, which must stay on top of everything.
        val insertAt = state.locationOverlay?.let { map.overlays.indexOf(it) } ?: -1
        if (insertAt >= 0) map.overlays.add(insertAt, overlay) else map.overlays.add(overlay)
        state.contentOverlays.add(overlay)
    }

    // The green check is a per-area badge and only says anything while the areas
    // are big enough to hold one. Framing a region's worth of coverage puts
    // hundreds on screen, overlapping into a solid mat of green. Past this many,
    // the hexagon's own green edge carries it alone.
    val showChecks = outlines.count { it.synced && !it.missing } <= MAX_CHECKS

    // Selection sits UNDER the coverage hexes: ground already downloaded should
    // read as downloaded, not as a pending selection tint.
    val flats = ArrayList<HexOverlay.Hex>(selection.size + outlines.size)
    for (hex in selection) {
        flats.add(
            HexOverlay.Hex(
                hex.hexagon,
                when (hex.kind) {
                    SelectionHex.Kind.PENDING -> HexOverlay.Style.SELECTION_PENDING
                    SelectionHex.Kind.DONE -> HexOverlay.Style.SELECTION_DONE
                    SelectionHex.Kind.EXCLUDED -> HexOverlay.Style.SELECTION_EXCLUDED
                },
            ),
        )
    }
    for (hex in outlines) {
        flats.add(
            HexOverlay.Hex(
                hex.hexagon,
                when {
                    hex.missing -> HexOverlay.Style.MISSING
                    hex.synced -> HexOverlay.Style.OUTLINE_SYNCED
                    else -> HexOverlay.Style.OUTLINE_PHONE
                },
                check = showChecks && hex.synced && !hex.missing,
            ),
        )
    }
    if (flats.isNotEmpty()) add(HexOverlay(flats, density))

    // Labels last of the ground layers, so street and place names read back
    // through the coverage tint instead of being buried under it.
    add(labelsOverlay)

    // Last, so the route always sits on top of the coverage.
    if (route != null && route.size > 1) {
        val line = Polyline(map)
        line.setPoints(route.map { GeoPoint(it.lat, it.lon) })
        line.outlinePaint.color = Palette.accent.toArgb()
        line.outlinePaint.strokeWidth = 5f * density
        line.outlinePaint.strokeCap = android.graphics.Paint.Cap.ROUND
        line.outlinePaint.strokeJoin = android.graphics.Paint.Join.ROUND
        add(line)
    }

    for (n in meshNodes) {
        val marker = Marker(map)
        marker.position = GeoPoint(n.coordinate.lat, n.coordinate.lon)
        marker.title = n.label
        marker.snippet = n.detail
        marker.icon = android.graphics.drawable.BitmapDrawable(
            map.resources,
            MapMarkers.meshNodePin(density, n.imprecise),
        )
        marker.setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_BOTTOM)
        add(marker)
    }

    if (destination != null) {
        val marker = Marker(map)
        marker.position = GeoPoint(destination.coordinate.lat, destination.coordinate.lon)
        marker.title = destination.name
        marker.icon = android.graphics.drawable.BitmapDrawable(
            map.resources,
            MapMarkers.destinationPin(density),
        )
        marker.setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_BOTTOM)
        add(marker)
    }
}

private fun apply(map: MapView, state: MapState, camera: MapCamera, bottomInsetPx: Int) {
    // Only break follow mode if it is actually engaged.
    fun stopFollowing() {
        state.locationOverlay?.disableFollowLocation()
    }
    when (val target = camera.target) {
        is MapCamera.Target.Region -> {
            stopFollowing()
            // osmdroid zooms by level, so a degree span becomes the level whose
            // world is that many degrees wide across the view.
            val zoom = zoomForSpan(target.spanDeg, map.width)
            map.controller.setZoom(zoom)
            map.controller.setCenter(liftedCentre(target.center, zoom, bottomInsetPx))
        }

        is MapCamera.Target.Box -> {
            stopFollowing()
            val b = target.box
            if (bottomInsetPx <= 0) {
                map.zoomToBoundingBox(
                    org.osmdroid.util.BoundingBox(b.north, b.east, b.south, b.west),
                    true,
                    target.paddingPx,
                )
            } else {
                // zoomToBoundingBox only takes a uniform border, which would centre
                // the box behind the floating card. Fit it into the band ABOVE the
                // card instead: pick the zoom against the usable height, then lift
                // the centre by half the inset. Animated, like the plain path.
                val zoom = zoomToFit(b, map, target.paddingPx, bottomInsetPx)
                map.controller.animateTo(liftedCentre(b.center, zoom, bottomInsetPx), zoom, null)
            }
        }

        MapCamera.Target.FollowUser -> state.locationOverlay?.enableFollowLocation()
    }
}

/** The zoom level at which [spanDeg] of longitude fills [widthPx]. */
private fun zoomForSpan(spanDeg: Double, widthPx: Int): Double {
    val width = if (widthPx > 0) widthPx else 1080
    val tile = MapStyle.TILE_SIZE.toDouble()
    val z = kotlin.math.ln(width * 360.0 / (tile * spanDeg)) / kotlin.math.ln(2.0)
    return z.coerceIn(2.0, 19.0)
}

/** The largest zoom at which [box] fits the view, less its border and the band
 *  the floating card covers. */
private fun zoomToFit(box: BoundingBox, map: MapView, borderPx: Int, bottomInsetPx: Int): Double {
    val tile = MapStyle.TILE_SIZE.toDouble()
    val w = ((if (map.width > 0) map.width else 1080) - 2 * borderPx).coerceAtLeast(1)
    val h = ((if (map.height > 0) map.height else 1920) - 2 * borderPx - bottomInsetPx)
        .coerceAtLeast(1)
    // The box as a fraction of the whole world, so a zoom is a ratio away.
    val fx = ((box.east - box.west) / 360.0).coerceAtLeast(1e-9)
    val fy = ((MercatorWorld.y(box.south) - MercatorWorld.y(box.north)) / MercatorWorld.WORLD)
        .coerceAtLeast(1e-9)
    val ln2 = kotlin.math.ln(2.0)
    val zx = kotlin.math.ln(w / (tile * fx)) / ln2
    val zy = kotlin.math.ln(h / (tile * fy)) / ln2
    return kotlin.math.min(zx, zy).coerceIn(2.0, 19.0)
}

/**
 * Where the MAP must be centred for [centre] to land in the middle of the band
 * above the floating card — half the inset south of it, so the content rides up.
 */
private fun liftedCentre(centre: LatLon, zoom: Double, bottomInsetPx: Int): GeoPoint {
    if (bottomInsetPx <= 0) return GeoPoint(centre.lat, centre.lon)
    val worldPx = MapStyle.TILE_SIZE * 2.0.pow(zoom)
    val y = MercatorWorld.y(centre.lat) / MercatorWorld.WORLD * worldPx + bottomInsetPx / 2.0
    return GeoPoint(MercatorWorld.latFromY(y / worldPx * MercatorWorld.WORLD), centre.lon)
}

/**
 * Past this many synced areas on screen the checks overlap into a solid mat of
 * green and say less than the hexagons' own edges do.
 */
private const val MAX_CHECKS = 120

private class MapState {
    var wired = false
    var contentKey = 0
    var lastCamera: MapCamera? = null
    var locationOverlay: MyLocationNewOverlay? = null
    val contentOverlays = ArrayList<org.osmdroid.views.overlay.Overlay>()

    var onTap: ((LatLon) -> Unit)? = null
    var onLongPress: ((LatLon) -> Unit)? = null
    var onRegionChange: ((BoundingBox) -> Unit)? = null

    fun publishRegion(map: MapView) {
        val cb = onRegionChange ?: return
        val b = map.boundingBox
        cb(BoundingBox(b.latSouth, b.lonWest, b.latNorth, b.lonEast))
    }
}
