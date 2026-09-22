package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.zIndex
import com.raemond.opentrailpaper.data.DashField
import com.raemond.opentrailpaper.data.DashItem
import com.raemond.opentrailpaper.data.DashLayout
import com.raemond.opentrailpaper.data.DashSize
import kotlinx.coroutines.delay
import kotlin.math.hypot
import kotlin.math.roundToInt

/**
 * Edit the panel on the panel.
 *
 * One data page, laid out by touching the picture of it: tap a cell to select,
 * hold and drag to move, drop on the left or right half of a full-width cell to
 * pair the two, tap the size badge to cycle, drag the radar's grab bar for its
 * height. The picture is [DashPreview]'s — the firmware's packer in device
 * pixels — so what the rider arranges is what the device will draw, and the
 * chrome this screen adds (selection outline, badge, drop targets) is the only
 * thing in vermilion; everything inside the panel stays ink on paper.
 *
 * Pushed from [DashboardEditorSheet] when a data-page card is tapped; the
 * carousel, its reorder and the other page kinds stay there. Send is in this
 * toolbar too so a rider can push without backing out.
 *
 * Ported from Dashboard Editor.dc.html (2a) in the design project.
 */
@Composable
fun DashboardPageEditor(
    pageTitle: String,
    layout: DashLayout,
    useMiles: Boolean,
    canSend: Boolean,
    onChange: (DashLayout) -> Unit,
    onSend: () -> Unit,
    onBack: () -> Unit,
) {
    var sel by remember { mutableStateOf<Long?>(null) }
    var tray by remember { mutableStateOf<TrayMode?>(null) }
    var toast by remember { mutableStateOf<String?>(null) }

    // A selection that no longer exists (removed, or replaced by a reset) is
    // dropped rather than left pointing at nothing.
    LaunchedEffect(layout) {
        if (sel != null && layout.items.none { it.key == sel }) sel = null
    }
    LaunchedEffect(toast) {
        if (toast != null) { delay(1800); toast = null }
    }

    fun mutate(f: (List<DashItem>) -> List<DashItem>) = onChange(DashLayout(f(layout.items)))
    fun setItem(key: Long, f: (DashItem) -> DashItem) =
        mutate { items -> items.map { if (it.key == key) f(it) else it } }

    val placement = remember(layout) { place(layout) }
    val selected = placement.cells.firstOrNull { it.item.key == sel }

    FullScreenCover(onDismiss = onBack) {
        Box(Modifier.fillMaxSize().background(Palette.paper)) {
            Column(
                Modifier
                    .fillMaxSize()
                    .windowInsetsPadding(WindowInsets.statusBars)
                    .windowInsetsPadding(WindowInsets.navigationBars),
            ) {
                // ‹ Dashboard · Page 1 · Send
                Box(
                    Modifier.fillMaxWidth().height(52.dp).padding(horizontal = 12.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Row(
                        Modifier
                            .align(Alignment.CenterStart)
                            .clip(RoundedCornerShape(8.dp))
                            .clickable(onClick = onBack)
                            .padding(horizontal = 8.dp, vertical = 6.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text("‹", style = barlow(24.sp, FontWeight.SemiBold), color = Palette.accent,
                            modifier = Modifier.offset(y = (-2).dp))
                        Text("Dashboard", style = barlow(16.sp, FontWeight.SemiBold), color = Palette.accent)
                    }
                    Text(pageTitle, style = TypeScale.title, color = Palette.ink)
                    Box(
                        Modifier
                            .align(Alignment.CenterEnd)
                            .clip(RoundedCornerShape(16.dp))
                            .background(if (canSend) Palette.accent else Palette.faint.copy(alpha = 0.55f))
                            .clickable(enabled = canSend) {
                                onSend()
                                toast = "Sent — waiting for the head unit to echo"
                            }
                            .padding(horizontal = 16.dp, vertical = 7.dp),
                    ) {
                        Text("Send", style = condensed(17.sp, FontWeight.SemiBold), color = Palette.accentInk)
                    }
                }

                // THE PANEL. The device's own shape, as large as the screen
                // allows once the inspector has the height it needs.
                Box(
                    Modifier
                        .weight(1f)
                        .fillMaxWidth()
                        .padding(horizontal = 24.dp, vertical = 6.dp),
                    contentAlignment = Alignment.TopCenter,
                ) {
                    PanelCanvas(
                        layout = layout,
                        placement = placement,
                        useMiles = useMiles,
                        selectedKey = sel,
                        onSelect = { sel = it },
                        onDrop = { key, target -> mutate { applyDrop(it, placement, key, target) } },
                        onCycleSize = { key ->
                            setItem(key) { it.copy(size = DashSize.entries[(it.size.ordinal + 1) % DashSize.entries.size]) }
                            sel = key
                        },
                        onRadarHeight = { key, pct -> setItem(key) { it.copy(heightPercent = pct) } },
                        modifier = Modifier.aspectRatio(DASH_PANEL_ASPECT),
                    )
                    toast?.let {
                        Box(
                            Modifier
                                .align(Alignment.TopCenter)
                                .padding(top = 12.dp, start = 12.dp, end = 12.dp)
                                .background(Palette.ink, RoundedCornerShape(12.dp))
                                .padding(horizontal = 16.dp, vertical = 12.dp),
                        ) {
                            Text(it, style = barlow(14.sp, FontWeight.SemiBold), color = Palette.surface,
                                textAlign = TextAlign.Center)
                        }
                    }
                }

                // THE INSPECTOR. What the selection is, and the two or three
                // things that can be said about it.
                Inspector(
                    layout = layout,
                    placement = placement,
                    selected = selected,
                    onOpenAdd = { tray = TrayMode.ADD },
                    onOpenChange = { tray = TrayMode.CHANGE },
                    onAddRadar = {
                        if (placement.cells.none { it.isRadar }) {
                            val it = DashItem("radar", DashSize.MEDIUM, half = false, vertical = true)
                            mutate { items -> items + it }
                            sel = it.key
                        }
                    },
                    onReset = { onChange(DashLayout.deviceDefault); sel = null },
                    onRemove = { key -> mutate { items -> items.filter { it.key != key } }; sel = null },
                    onDone = { sel = null },
                    onSetItem = ::setItem,
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = 232.dp)
                        .padding(start = 16.dp, end = 16.dp, top = 6.dp, bottom = 16.dp),
                )
            }

            tray?.let { mode ->
                FieldTray(
                    mode = mode,
                    allowRadar = mode == TrayMode.ADD && placement.cells.none { it.isRadar },
                    onDismiss = { tray = null },
                    onPick = { id ->
                        if (mode == TrayMode.CHANGE && sel != null) {
                            setItem(sel!!) { it.copy(field = id) }
                        } else {
                            val it = DashItem(id, DashSize.MEDIUM, half = false)
                            mutate { items -> items + it }
                            sel = it.key
                        }
                        tray = null
                    },
                )
            }
        }
    }
}

private enum class TrayMode { ADD, CHANGE }

// MARK: - the canvas

/** Where a dragged cell would land. Row indices are into [Placement.rows]. */
private sealed class DropTarget(val row: Int) {
    class Before(row: Int) : DropTarget(row)
    class After(row: Int) : DropTarget(row)
    class Left(row: Int) : DropTarget(row)
    class Right(row: Int) : DropTarget(row)
}

private class DragState(val key: Long, var dx: Float = 0f, var dy: Float = 0f, var live: Boolean = false, var target: DropTarget? = null)

/**
 * The rule the pointer is judged by, in device pixels. A full-width row offers
 * its left or right half through its middle band and "before"/"after" at its
 * edges; a paired row only before/after; above and below the grid are the
 * ends. The row the cell came from is not a target, so a wobble does nothing.
 */
private fun targetFor(pl: Placement, dragged: Long, px: Float, py: Float): DropTarget? {
    val rows = pl.rows
    if (rows.isEmpty()) return null
    if (py < rows[0].y) return DropTarget.Before(0)
    val last = rows.last()
    if (py >= last.y + last.h) {
        return if (last.keys.size == 1 && last.keys[0] == dragged) null else DropTarget.After(rows.lastIndex)
    }
    for ((r, row) in rows.withIndex()) {
        if (py < row.y || py >= row.y + row.h + GUTTER) continue
        val rel = (py - row.y) / row.h
        val single = row.keys.size == 1
        if (single && row.keys[0] == dragged) return null
        if (single && rel > 0.22f && rel < 0.78f) {
            return if (px < MARGIN + row.mainW / 2) DropTarget.Left(r) else DropTarget.Right(r)
        }
        return if (rel < 0.5f) DropTarget.Before(r) else DropTarget.After(r)
    }
    return null
}

private fun applyDrop(items: List<DashItem>, pl: Placement, dragged: Long, t: DropTarget): List<DashItem> {
    val d = items.firstOrNull { it.key == dragged } ?: return items
    val rest = items.filter { it.key != dragged }.toMutableList()
    val row = pl.rows.getOrNull(t.row) ?: return items
    when (t) {
        is DropTarget.Left, is DropTarget.Right -> {
            val ti = rest.indexOfFirst { it.key == row.keys[0] }
            if (ti < 0) return items
            rest[ti] = rest[ti].copy(half = true)
            rest.add(if (t is DropTarget.Left) ti else ti + 1, d.copy(half = true))
        }
        is DropTarget.Before, is DropTarget.After -> {
            val anchor = if (t is DropTarget.Before) row.keys.first() else row.keys.last()
            var ai = rest.indexOfFirst { it.key == anchor }
            if (ai < 0) ai = rest.lastIndex
            rest.add(if (t is DropTarget.Before) ai else ai + 1, d.copy(half = false))
        }
    }
    return rest
}

@Composable
private fun PanelCanvas(
    layout: DashLayout,
    placement: Placement,
    useMiles: Boolean,
    selectedKey: Long?,
    onSelect: (Long?) -> Unit,
    onDrop: (Long, DropTarget) -> Unit,
    onCycleSize: (Long) -> Unit,
    onRadarHeight: (Long, Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    var drag by remember { mutableStateOf<DragState?>(null) }
    var railDrag by remember { mutableStateOf(false) }

    BoxWithConstraints(
        modifier
            .background(PanelPaper, RoundedCornerShape(8.dp))
            .border(1.dp, Palette.ink, RoundedCornerShape(8.dp))
            .clip(RoundedCornerShape(8.dp))
            .clipToBounds()
            // A tap on bare paper clears the selection. Cells consume their own
            // taps first, so this only fires between them.
            .pointerInput(Unit) { detectTapGestures { onSelect(null) } },
    ) {
        val k = maxWidth.value / PANEL_W
        val density = LocalDensity.current
        // Device px per screen px, for turning pointer positions into grid space.
        val pxToDevice = 1f / (k * density.density)

        CompositionLocalProvider(
            LocalDensity provides Density(density.density, fontScale = 1f),
            LocalTextStyle provides LocalTextStyle.current.copy(color = Color.Black),
        ) {
            for (p in placement.cells) {
                val key = p.item.key
                val isSel = key == selectedKey
                val dragging = drag?.key == key && drag?.live == true
                val unpaired = placement.isUnpaired(p)
                Box(
                    Modifier
                        .offset(x = (p.x * k).dp, y = ((p.y - STATUS_H) * k).dp)
                        .size(width = (p.w * k).dp, height = (p.h * k).dp)
                        .zIndex(if (dragging) 20f else if (isSel) 5f else 0f)
                        .graphicsLayer {
                            // Read here, not in composition: this lambda runs at
                            // draw time, after a drop may already have cleared it.
                            val d = drag
                            if (d != null && d.key == key && d.live) {
                                translationX = d.dx
                                translationY = d.dy
                                shadowElevation = 12.dp.toPx()
                                alpha = 0.94f
                            }
                        }
                        .pointerInput(key) { detectTapGestures { onSelect(key) } }
                        .pointerInput(key, placement) {
                            if (p.isRadar) return@pointerInput
                            detectDragGestures(
                                onDragStart = { onSelect(key); drag = DragState(key) },
                                onDragEnd = {
                                    drag?.let { d -> if (d.live) d.target?.let { onDrop(key, it) } }
                                    drag = null
                                },
                                onDragCancel = { drag = null },
                                onDrag = { change, amount ->
                                    change.consume()
                                    val d = drag ?: return@detectDragGestures
                                    val dx = d.dx + amount.x
                                    val dy = d.dy + amount.y
                                    val live = d.live || hypot(dx, dy) > 6f * density.density
                                    // Where the pointer is on the device grid.
                                    val px = p.x + (change.position.x + dx - amount.x) * pxToDevice
                                    val py = p.y + (change.position.y + dy - amount.y) * pxToDevice
                                    drag = DragState(key, dx, dy, live, if (live) targetFor(placement, key, px, py) else null)
                                },
                            )
                        },
                ) {
                    Cell(p, k, null, useMiles)

                    if (isSel) {
                        Box(Modifier.fillMaxSize().border(2.5.dp, Palette.accent))
                    }
                    if (unpaired) {
                        Box(
                            Modifier
                                .align(Alignment.TopCenter)
                                .padding(top = 6.dp, bottom = 6.dp)
                                .fillMaxSize()
                                .drawBehind {
                                    drawLine(
                                        Palette.accent,
                                        Offset(size.width / 2, 0f),
                                        Offset(size.width / 2, size.height),
                                        strokeWidth = 1.5.dp.toPx(),
                                        pathEffect = PathEffect.dashPathEffect(floatArrayOf(6f, 6f)),
                                    )
                                },
                        )
                        Text(
                            "½ UNPAIRED · SPANS",
                            style = condensed(10.sp, FontWeight.SemiBold).copy(letterSpacing = 0.8.sp),
                            color = Palette.accent,
                            modifier = Modifier.align(Alignment.TopEnd).padding(top = 6.dp, end = 8.dp),
                        )
                    }
                    if (!p.isRadar) {
                        // Size badge: tap to cycle. A tap here must not also
                        // select-or-drag the cell, so it sits on top and eats it.
                        val zoneBar = p.hero && (p.item.field == "power3s" || p.item.field == "power")
                        Box(
                            Modifier
                                .align(Alignment.BottomEnd)
                                .padding(end = 4.dp, bottom = if (zoneBar) (18 * k + 14).dp else 4.dp)
                                .size(20.dp)
                                .clip(CircleShape)
                                .background(if (isSel) Palette.accent else Palette.surface)
                                .border(1.dp, if (isSel) Palette.accent else Palette.hairline, CircleShape)
                                .pointerInput(key) { detectTapGestures { onCycleSize(key) } },
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(
                                sizeLetter(p.item.size),
                                style = condensed(12.sp, FontWeight.SemiBold),
                                color = if (isSel) Palette.accentInk else Palette.muted,
                            )
                        }
                    } else if (isSel) {
                        // The radar's grab bar: drag it to snap the tile's height
                        // to a half, three-quarters or the whole column. On the
                        // edge that moves — bottom for a top-aligned tile.
                        val bottom = p.item.bottomAligned
                        Box(
                            Modifier
                                .align(if (bottom) Alignment.TopCenter else Alignment.BottomCenter)
                                .fillMaxWidth()
                                .height(22.dp)
                                .background(Palette.accent)
                                .pointerInput(key, placement) {
                                    detectDragGestures(
                                        onDragStart = { railDrag = true },
                                        onDragEnd = { railDrag = false },
                                        onDragCancel = { railDrag = false },
                                        onDrag = { change, _ ->
                                            change.consume()
                                            // Pointer in device space; the bar sits at the tile's moving edge.
                                            val barY = if (bottom) p.y else p.y + p.h - 22f / (k * 1f)
                                            val py = barY + change.position.y * pxToDevice
                                            val top = STATUS_H + MARGIN - STEP
                                            val total = PANEL_H - MARGIN - top
                                            val frac = if (bottom) (PANEL_H - MARGIN - py) / total else (py - top) / total
                                            val snap = if (frac < 0.625f) 50 else if (frac < 0.875f) 75 else 100
                                            if (snap != p.item.heightPercent) onRadarHeight(key, snap)
                                        },
                                    )
                                },
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(
                                "↕ " + when (p.item.heightPercent) { 100 -> "FULL"; 75 -> "¾"; else -> "HALF" },
                                style = condensed(12.sp, FontWeight.SemiBold).copy(letterSpacing = 1.sp),
                                color = Palette.accentInk,
                            )
                        }
                    }
                }
            }

            // Drop indicator: a bar between rows, or a dashed half of the row
            // the cell would pair into.
            drag?.target?.let { t ->
                val row = placement.rows.getOrNull(t.row) ?: return@let
                when (t) {
                    is DropTarget.Before, is DropTarget.After -> {
                        val y = if (t is DropTarget.Before) row.y - GUTTER / 2 else row.y + row.h + GUTTER / 2
                        Box(
                            Modifier
                                .offset(x = (MARGIN * k).dp, y = ((y - STATUS_H) * k).dp - 2.dp)
                                .size(width = (row.mainW * k).dp, height = 4.dp)
                                .zIndex(15f)
                                .background(Palette.accent, RoundedCornerShape(2.dp)),
                        )
                    }
                    is DropTarget.Left, is DropTarget.Right -> {
                        val halfW = ((row.mainW - GUTTER) / 2).toInt().toFloat()
                        val x = if (t is DropTarget.Left) MARGIN else MARGIN + halfW + GUTTER
                        Box(
                            Modifier
                                .offset(x = (x * k).dp, y = ((row.y - STATUS_H) * k).dp)
                                .size(width = (halfW * k).dp, height = (row.h * k).dp)
                                .zIndex(15f)
                                .background(Palette.accent.copy(alpha = 0.12f))
                                .drawBehind {
                                    drawRect(
                                        Palette.accent,
                                        style = Stroke(2.5.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(10f, 8f))),
                                    )
                                },
                        )
                    }
                }
            }
        }
    }
}

private fun sizeLetter(s: DashSize) = when (s) {
    DashSize.SMALL -> "S"
    DashSize.MEDIUM -> "M"
    DashSize.LARGE -> "L"
    DashSize.HERO -> "H"
}

// MARK: - the inspector

@Composable
private fun Inspector(
    layout: DashLayout,
    placement: Placement,
    selected: Placed?,
    onOpenAdd: () -> Unit,
    onOpenChange: () -> Unit,
    onAddRadar: () -> Unit,
    onReset: () -> Unit,
    onRemove: (Long) -> Unit,
    onDone: () -> Unit,
    onSetItem: (Long, (DashItem) -> DashItem) -> Unit,
    modifier: Modifier = Modifier,
) {
    Card(modifier, padding = 14.dp) {
        val scroll = rememberScrollState()
        when {
            selected == null -> {
                Text(
                    "Tap a cell to edit it. Hold and drag to move — drop on the left or " +
                        "right half of a full-width cell to pair them.",
                    style = barlow(14.sp), color = Palette.muted,
                )
                // A fixed gap, not weight(1f): the inspector is not a weighted
                // child of the screen column, so it is measured before the
                // panel with the whole remaining height on offer, and a
                // weighted spacer here took all of it - the panel was left
                // with a sliver and this card filled the screen.
                Spacer(Modifier.height(14.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    Text(
                        "Reset to default",
                        style = barlow(14.sp, FontWeight.SemiBold), color = Palette.muted,
                        modifier = Modifier
                            .clip(RoundedCornerShape(8.dp))
                            .clickable(onClick = onReset)
                            .padding(6.dp),
                    )
                }
                Spacer(Modifier.height(4.dp))
                val hasRadar = placement.cells.any { it.isRadar }
                val canAddField = layout.items.size < DashLayout.MAX_ITEMS
                Row(Modifier.fillMaxWidth().height(44.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Box(
                        Modifier
                            .weight(1f)
                            .fillMaxSize()
                            .clip(RoundedCornerShape(22.dp))
                            .background(if (canAddField) Palette.accent else Palette.faint)
                            .clickable(enabled = canAddField, onClick = onOpenAdd),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text("Add a field", style = condensed(19.sp, FontWeight.SemiBold), color = Palette.accentInk)
                    }
                    Box(
                        Modifier
                            .weight(1f)
                            .fillMaxSize()
                            .clip(RoundedCornerShape(22.dp))
                            .border(
                                if (hasRadar) 1.dp else 1.5.dp,
                                if (hasRadar) Palette.hairline else Palette.ink,
                                RoundedCornerShape(22.dp),
                            )
                            .clickable(enabled = !hasRadar && canAddField, onClick = onAddRadar),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            if (hasRadar) "Radar added" else "Add radar",
                            style = condensed(19.sp, FontWeight.SemiBold),
                            color = if (hasRadar) Palette.faint else Palette.ink,
                        )
                    }
                }
            }

            selected.isRadar -> {
                val item = selected.item
                Column(Modifier.verticalScroll(scroll)) {
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Bottom) {
                        Text("Radar", style = TypeScale.title, color = Palette.ink)
                        Text(
                            "Varia · right column", style = barlow(13.sp, FontWeight.Medium),
                            color = Palette.muted, modifier = Modifier.padding(start = 6.dp, bottom = 3.dp).weight(1f),
                        )
                        HeaderAction("Remove", Palette.accent) { onRemove(item.key) }
                        HeaderAction("Done", Palette.muted, onDone)
                    }
                    Spacer(Modifier.height(10.dp))
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Bottom) {
                        TrackedLabel("Height", Modifier.weight(1f))
                        Text(
                            "${placement.railH.roundToInt()} PX · ${if (item.bottomAligned) "FROM BOTTOM" else "FROM TOP"}",
                            style = TypeScale.label.copy(letterSpacing = 0.6.sp), color = Palette.muted,
                        )
                    }
                    Spacer(Modifier.height(4.dp))
                    val heights = listOf(50, 75, 100)
                    Segmented(
                        options = listOf("Half", "Three-quarter", "Full"),
                        selected = heights.indexOf(item.heightPercent).coerceAtLeast(0),
                    ) { i -> onSetItem(item.key) { it.copy(heightPercent = heights[i]) } }
                    Spacer(Modifier.height(8.dp))
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        Segmented(
                            options = listOf("Top", "Bottom"),
                            selected = if (item.bottomAligned) 1 else 0,
                            enabled = item.heightPercent < 100,
                            modifier = Modifier.width(150.dp),
                        ) { i -> onSetItem(item.key) { it.copy(bottomAligned = i == 1) } }
                        Spacer(Modifier.width(10.dp))
                        Text(
                            if (item.heightPercent == 100) "Full height fills the column either way."
                            else "Bottom lets a full-width hero sit above the tile.",
                            style = barlow(12.5.sp), color = Palette.muted, modifier = Modifier.weight(1f),
                        )
                    }
                }
            }

            else -> {
                val item = selected.item
                val row = placement.rows.firstOrNull { item.key in it.keys }
                val unpaired = placement.isUnpaired(selected)
                Column(Modifier.verticalScroll(scroll)) {
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Bottom) {
                        Row(
                            Modifier
                                .weight(1f)
                                .clip(RoundedCornerShape(8.dp))
                                .clickable(onClick = onOpenChange),
                            verticalAlignment = Alignment.Bottom,
                        ) {
                            Text(item.fieldLabel, style = TypeScale.title, color = Palette.ink)
                            Text(
                                "Change ›", style = barlow(14.sp, FontWeight.SemiBold), color = Palette.accent,
                                modifier = Modifier.padding(start = 6.dp, bottom = 3.dp),
                            )
                        }
                        HeaderAction("Remove", Palette.accent) { onRemove(item.key) }
                        HeaderAction("Done", Palette.muted, onDone)
                    }
                    Spacer(Modifier.height(10.dp))
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Bottom) {
                        TrackedLabel("Size", Modifier.weight(1f))
                        if (row != null) Text(
                            "${row.weight} OF ${placement.totalWeight} SHARES → ${row.h.roundToInt()} PX",
                            style = TypeScale.label.copy(letterSpacing = 0.6.sp), color = Palette.muted,
                        )
                    }
                    Spacer(Modifier.height(4.dp))
                    Segmented(
                        options = DashSize.entries.map { it.label },
                        selected = item.size.ordinal,
                    ) { i -> onSetItem(item.key) { it.copy(size = DashSize.entries[i]) } }
                    Spacer(Modifier.height(8.dp))
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        Segmented(
                            options = listOf("Full row", "Half"),
                            selected = if (item.half) 1 else 0,
                            modifier = Modifier.width(170.dp),
                        ) { i -> onSetItem(item.key) { it.copy(half = i == 1) } }
                        Spacer(Modifier.width(10.dp))
                        Text(
                            when {
                                !item.half -> "Drop another field on this cell’s left or right half to pair them."
                                unpaired -> "Unpaired — it spans the row. Drag another field beside it."
                                else -> "Paired with its neighbour."
                            },
                            style = barlow(12.5.sp),
                            color = if (item.half && unpaired) Palette.accent else Palette.muted,
                            modifier = Modifier.weight(1f),
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun HeaderAction(label: String, color: Color, onClick: () -> Unit) {
    Text(
        label, style = barlow(14.sp, FontWeight.SemiBold), color = color,
        modifier = Modifier
            .clip(RoundedCornerShape(8.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 7.dp, vertical = 4.dp),
    )
}

// MARK: - the field tray

/** A sheet of every field, two to a row, over a dimmed editor. */
@Composable
private fun FieldTray(
    mode: TrayMode,
    allowRadar: Boolean,
    onDismiss: () -> Unit,
    onPick: (String) -> Unit,
) {
    Box(Modifier.fillMaxSize()) {
        Box(
            Modifier
                .fillMaxSize()
                .background(Palette.ink.copy(alpha = 0.35f))
                .pointerInput(Unit) { detectTapGestures { onDismiss() } },
        )
        Column(
            Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .background(Palette.paper, RoundedCornerShape(topStart = 24.dp, topEnd = 24.dp))
                .pointerInput(Unit) {}
                .windowInsetsPadding(WindowInsets.navigationBars),
        ) {
            Box(
                Modifier
                    .align(Alignment.CenterHorizontally)
                    .padding(top = 8.dp)
                    .size(width = 36.dp, height = 5.dp)
                    .background(Palette.hairline, RoundedCornerShape(3.dp)),
            )
            Row(
                Modifier.fillMaxWidth().height(48.dp).padding(horizontal = 20.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    if (mode == TrayMode.CHANGE) "Change field" else "Add a field",
                    style = TypeScale.title, color = Palette.ink, modifier = Modifier.weight(1f),
                )
                Text(
                    "Cancel", style = barlow(16.sp, FontWeight.SemiBold), color = Palette.accent,
                    modifier = Modifier.clip(RoundedCornerShape(8.dp)).clickable(onClick = onDismiss).padding(6.dp),
                )
            }
            LazyVerticalGrid(
                columns = GridCells.Fixed(2),
                modifier = Modifier.fillMaxWidth().height(440.dp).padding(horizontal = 16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                contentPadding = androidx.compose.foundation.layout.PaddingValues(top = 6.dp, bottom = 16.dp),
            ) {
                items(DashField.all.filter { it.id != "radar" || allowRadar }, key = { it.id }) { f ->
                    Column(
                        Modifier
                            .fillMaxWidth()
                            .background(Palette.surface, RoundedCornerShape(12.dp))
                            .border(1.dp, Palette.hairline, RoundedCornerShape(12.dp))
                            .clip(RoundedCornerShape(12.dp))
                            .clickable { onPick(f.id) }
                            .padding(horizontal = 12.dp, vertical = 9.dp),
                    ) {
                        Text(f.label, style = barlow(15.sp, FontWeight.SemiBold), color = Palette.ink, maxLines = 1)
                        Text(f.detail, style = barlow(12.sp), color = Palette.muted, maxLines = 2)
                    }
                }
            }
        }
    }
}
