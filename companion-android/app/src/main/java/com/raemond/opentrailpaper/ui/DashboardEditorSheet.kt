package com.raemond.opentrailpaper.ui

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.AnimationVector2D
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.VectorConverter
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGesturesAfterLongPress
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.DirectionsBike
import androidx.compose.material.icons.filled.DragHandle
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.Remove
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onPlaced
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.layout.positionInParent
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.round
import androidx.compose.ui.unit.sp
import androidx.compose.ui.zIndex
import kotlinx.coroutines.launch
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.data.DashConfig
import com.raemond.opentrailpaper.data.DashField
import com.raemond.opentrailpaper.data.DashItem
import com.raemond.opentrailpaper.data.DashLayout
import com.raemond.opentrailpaper.data.DashSize
import kotlin.math.roundToInt

/**
 * Rearrange the head unit's dashboard from the phone — the page carousel and
 * everything on each page. Port of DashboardEditorView.swift.
 *
 * The preview is the point. A list of field names cannot tell you that four
 * mediums leave everything small, or that a lone `half` field silently spans the
 * row — so the editor draws what the panel will draw, using the same packing
 * rules and height weights as ui_render.cpp.
 */
@Composable
fun DashboardEditorSheet(ble: BleManager, onDismiss: () -> Unit) {
    var config by remember { mutableStateOf(ble.dashConfig ?: DashConfig(emptyList())) }
    var pageIx by remember { mutableIntStateOf(0) }
    var dirty by remember { mutableStateOf(false) }
    var showAdd by remember { mutableStateOf(false) }

    // The device is the source of truth: if it corrects or rejects what we sent,
    // adopt what it actually holds rather than keeping a local fiction on screen.
    // Skipped while the rider has unsent edits, so a stray notify can't wipe work
    // in progress.
    LaunchedEffect(ble.dashConfig) {
        val fromDevice = ble.dashConfig
        if (!dirty && fromDevice != null) {
            config = fromDevice
            if (pageIx >= config.pages.size) pageIx = 0
        }
    }

    fun mutateAt(i: Int, f: (DashLayout) -> DashLayout) {
        val page = config.pages.getOrNull(i) ?: return
        if (page.kind != DashConfig.PageKind.FIELDS) return
        config = config.copy(
            pages = config.pages.toMutableList().also {
                it[i] = page.copy(layout = f(page.layout))
            },
        )
        dirty = true
    }

    FullScreenSheet(
        title = "Dashboard",
        onDismiss = onDismiss,
        trailing = {
            TextButton(
                // Enabled whenever this config is not what the device is holding
                // — a comparison, not a flag someone has to remember to set, so
                // a write that never landed leaves the button live for a retry
                // and it goes quiet by itself once the device echoes a match.
                enabled = config.pages.isNotEmpty() && config != ble.dashConfig,
                onClick = {
                    ble.sendDashConfig(config)
                    dirty = false
                },
            ) {
                Text("Send", style = TypeScale.bodyStrong)
            }
        },
    ) {
        if (ble.dashConfig == null) {
            Box(Modifier.fillMaxWidth().weight(1f), contentAlignment = Alignment.Center) {
                Column(
                    Modifier.padding(32.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text("Connect to edit", style = TypeScale.title, color = Palette.ink)
                    Text(
                        "The layout is read from the device, so it can't be edited until " +
                            "the head unit is connected.",
                        style = TypeScale.body,
                        color = Palette.muted,
                    )
                }
            }
            return@FullScreenSheet
        }

        Column(
            Modifier
                .verticalScroll(rememberScrollState())
                .padding(vertical = 16.dp),
        ) {
            PageCarousel(
                config = config,
                pageIx = pageIx,
                onSelect = { pageIx = it },
                onRemove = { i ->
                    config = config.copy(
                        pages = config.pages.toMutableList().also { it.removeAt(i) },
                    )
                    if (pageIx >= config.pages.size) pageIx = config.pages.size - 1
                    dirty = true
                },
                onMove = { from, to ->
                    val selected = config.pages.getOrNull(pageIx)?.key
                    config = config.copy(
                        pages = config.pages.toMutableList().also {
                            it.add(to, it.removeAt(from))
                        },
                    )
                    selected?.let { sel ->
                        val ix = config.pages.indexOfFirst { it.key == sel }
                        if (ix >= 0) pageIx = ix
                    }
                    dirty = true
                },
                onAdd = { kind ->
                    config = config.copy(
                        pages = config.pages + when (kind) {
                            DashConfig.PageKind.MUSIC -> DashConfig.Page.music()
                            DashConfig.PageKind.WORKOUT -> DashConfig.Page.workout()
                            else -> DashConfig.Page.fields()
                        },
                    )
                    pageIx = config.pages.size - 1
                    dirty = true
                },
            )

            if (config.pages.size > 1) {
                Text(
                    "Tap a page to edit it · hold one, then drag it into place",
                    style = barlow(12.sp),
                    color = Palette.muted,
                    modifier = Modifier.padding(horizontal = 20.dp).padding(bottom = 8.dp),
                )
            }

            val page = config.pages.getOrNull(pageIx)
            Column(
                Modifier.padding(horizontal = 16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                when {
                    page == null -> {}

                    page.isMap -> MapStripSection(config) { slot, id ->
                        config = config.copy(
                            mapFields = config.mapFields.toMutableList().also { it[slot] = id },
                        )
                        dirty = true
                    }

                    page.isMusic -> MusicSection(ble)

                    page.isWorkout -> Card {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Icon(
                                Icons.Filled.DirectionsBike,
                                contentDescription = null,
                                tint = Palette.ink,
                            )
                            Spacer(Modifier.size(8.dp))
                            Text("Workout", style = TypeScale.bodyStrong, color = Palette.ink)
                        }
                        Text(
                            "Runs a structured workout: block countdown, live power " +
                                "against the target, and the whole session as a profile. " +
                                "Load workouts from the Workouts screen or drop .erg/.mrc " +
                                "files in /workouts on the SD card.",
                            style = TypeScale.body,
                            color = Palette.muted,
                        )
                    }

                    else -> {
                        TrackedLabel("Fields")
                        ReorderableItems(
                            items = page.layout.items,
                            onMove = { from, to ->
                                mutateAt(pageIx) { l ->
                                    DashLayout(l.items.toMutableList().also {
                                        it.add(to, it.removeAt(from))
                                    })
                                }
                            },
                            onChange = { index, item ->
                                mutateAt(pageIx) { l ->
                                    DashLayout(l.items.toMutableList().also { it[index] = item })
                                }
                            },
                            onDelete = { index ->
                                mutateAt(pageIx) { l ->
                                    DashLayout(l.items.toMutableList().also { it.removeAt(index) })
                                }
                            },
                        )

                        if (page.layout.items.size < DashLayout.MAX_ITEMS) {
                            TextButton(onClick = { showAdd = true }) {
                                Icon(Icons.Filled.Add, contentDescription = null, tint = Palette.accent)
                                Spacer(Modifier.size(6.dp))
                                Text("Add a field", style = TypeScale.bodyStrong, color = Palette.accent)
                            }
                        }

                        Text(
                            "Long-press the handle to reorder. “Half width” pairs a field with " +
                                "the next half-width field; on its own it spans the row.",
                            style = barlow(12.sp),
                            color = Palette.muted,
                        )
                        Text(
                            "Fields that need a sensor — power, heart rate, cadence — are hidden " +
                                "on the device until it connects, and the rest expand to fill the " +
                                "space. Route left hides when no route is loaded.",
                            style = barlow(12.sp),
                            color = Palette.muted,
                        )

                        HorizontalDivider(color = Palette.hairline)
                        TextButton(onClick = {
                            config = DashConfig.deviceDefault
                            pageIx = 0
                            dirty = true
                        }) {
                            Text(
                                "Reset to the default layout",
                                style = TypeScale.bodyStrong,
                                color = Palette.accent,
                            )
                        }
                    }
                }
            }
        }
    }

    if (showAdd) {
        FieldPickerSheet(
            onDismiss = { showAdd = false },
            onPick = { id ->
                mutateAt(pageIx) { l -> DashLayout(l.items + DashItem(id, DashSize.MEDIUM, true)) }
                showAdd = false
            },
        )
    }
}

/**
 * The page cards: tap to select, hold-and-drag to reorder (live, the array
 * moves under the lifted card as it crosses a neighbour, and the selection
 * follows the page it was on), a remove badge on everything the config can
 * spare, and a dashed "add" card on the right.
 */
@Composable
private fun PageCarousel(
    config: DashConfig,
    pageIx: Int,
    onSelect: (Int) -> Unit,
    onRemove: (Int) -> Unit,
    onMove: (Int, Int) -> Unit,
    onAdd: (DashConfig.PageKind) -> Unit,
) {
    var dragKey by remember { mutableLongStateOf(-1L) }
    var dragOffset by remember { mutableFloatStateOf(0f) }
    var cardWidth by remember { mutableFloatStateOf(1f) }
    val scroll = rememberScrollState()

    // A just-added page scrolls into view (iOS: proxy.scrollTo on the new
    // card). Growth only — a removal should not yank the strip anywhere.
    var lastCount by remember { mutableIntStateOf(config.pages.size) }
    LaunchedEffect(config.pages.size) {
        if (config.pages.size > lastCount) scroll.animateScrollTo(scroll.maxValue)
        lastCount = config.pages.size
    }

    Row(
        Modifier
            .horizontalScroll(scroll)
            .padding(horizontal = 20.dp, vertical = 14.dp),
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        config.pages.forEachIndexed { i, page ->
            val dragging = dragKey == page.key
            Box(
                Modifier
                    .onSizeChanged { if (it.width > 0) cardWidth = it.width.toFloat() }
                    // Neighbours spring to their new slots as the lifted card
                    // crosses them (iOS: withAnimation(.spring) in the drop
                    // delegate). The dragged card itself snaps — its position
                    // is the finger's, and a spring would lag it.
                    .animatePlacement(enabled = !dragging)
                    .offset { IntOffset(if (dragging) dragOffset.roundToInt() else 0, 0) }
                    .zIndex(if (dragging) 1f else 0f)
                    // The lifted card fades so the one in motion reads as THE
                    // card rather than a copy over an unmoved original. Not 0:
                    // if a cancelled gesture ever leaves the flag stale, a
                    // faint card beats a vanished one.
                    .alpha(if (dragging) 0.6f else 1f)
                    .pointerInput(page.key, config.pages.size) {
                        detectDragGesturesAfterLongPress(
                            onDragStart = { dragKey = page.key; dragOffset = 0f },
                            onDragEnd = { dragKey = -1L; dragOffset = 0f },
                            onDragCancel = { dragKey = -1L; dragOffset = 0f },
                            onDrag = { change, amount ->
                                change.consume()
                                dragOffset += amount.x
                                // Live reorder: crossing a neighbour moves the
                                // array under the finger, one slot at a time.
                                val from = config.pages.indexOfFirst { it.key == dragKey }
                                if (from < 0) return@detectDragGesturesAfterLongPress
                                val span = cardWidth + with(this) { 16.dp.toPx() }
                                if (dragOffset > span * 0.6f && from < config.pages.size - 1) {
                                    onMove(from, from + 1)
                                    dragOffset -= span
                                } else if (dragOffset < -span * 0.6f && from > 0) {
                                    onMove(from, from - 1)
                                    dragOffset += span
                                }
                            },
                        )
                    },
            ) {
                PageCard(
                    config = config,
                    page = page,
                    selected = pageIx == i,
                    removable = !page.isMap && config.contentPages > 1,
                    onTap = { onSelect(i) },
                    onRemove = { onRemove(i) },
                )
            }
        }

        if (config.contentPages < DashConfig.MAX_PAGES) {
            AddPageCard(
                hasMusic = config.hasMusicPage,
                hasWorkout = config.hasWorkoutPage,
                onAdd = onAdd,
            )
        }
    }
}

@Composable
private fun PageCard(
    config: DashConfig,
    page: DashConfig.Page,
    selected: Boolean,
    removable: Boolean,
    onTap: () -> Unit,
    onRemove: () -> Unit,
) {
    Box {
        val border by animateFloatAsState(if (selected) 2.5f else 1f, label = "border")
        Box(
            Modifier
                .padding(top = 9.dp, end = 9.dp)
                .size(width = 124.dp, height = 206.dp)
                .clip(RoundedCornerShape(8.dp))
                .border(
                    border.dp,
                    if (selected) Palette.accent else Palette.hairline,
                    RoundedCornerShape(8.dp),
                )
                .clickable(onClick = onTap),
        ) {
            when (page.kind) {
                DashConfig.PageKind.MUSIC -> MusicPreview(
                    Modifier.fillMaxWidth().aspectRatio(540f / 960f),
                )
                DashConfig.PageKind.MAP -> MapPagePreview(
                    config.mapFields,
                    Modifier.fillMaxWidth().aspectRatio(540f / 960f),
                )
                DashConfig.PageKind.WORKOUT -> WorkoutPagePreview(
                    Modifier.fillMaxWidth().aspectRatio(540f / 960f),
                )
                DashConfig.PageKind.FIELDS -> DashPreview(
                    page.layout,
                    Modifier.fillMaxWidth().aspectRatio(DASH_PANEL_ASPECT),
                )
            }
        }
        // The map can't be removed; the last remaining data/music page can't
        // either.
        if (removable) {
            Box(
                Modifier
                    .align(Alignment.TopEnd)
                    .size(24.dp)
                    .clip(CircleShape)
                    .background(Color.White)
                    .clickable(onClick = onRemove),
                contentAlignment = Alignment.Center,
            ) {
                Box(
                    Modifier.size(20.dp).clip(CircleShape).background(Color(0xFFE53935)),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        Icons.Filled.Remove,
                        contentDescription = "Remove this page",
                        tint = Color.White,
                        modifier = Modifier.size(16.dp),
                    )
                }
            }
        }
    }
}

/** The far-right card: a new page, data or music. */
@Composable
private fun AddPageCard(
    hasMusic: Boolean,
    hasWorkout: Boolean,
    onAdd: (DashConfig.PageKind) -> Unit,
) {
    var menu by remember { mutableStateOf(false) }
    Box(Modifier.padding(top = 9.dp)) {
        Column(
            Modifier
                .size(width = 124.dp, height = 206.dp)
                .clip(RoundedCornerShape(8.dp))
                .border(1.5.dp, Palette.hairline, RoundedCornerShape(8.dp))
                .clickable { menu = true },
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            Icon(
                Icons.Filled.Add,
                contentDescription = null,
                tint = Palette.muted,
                modifier = Modifier.size(30.dp),
            )
            Spacer(Modifier.height(10.dp))
            Text("Add a page", style = barlow(13.sp), color = Palette.muted)
        }
        DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
            DropdownMenuItem(
                text = { Text("Data page", style = TypeScale.body) },
                onClick = { menu = false; onAdd(DashConfig.PageKind.FIELDS) },
            )
            if (!hasMusic) {
                DropdownMenuItem(
                    text = { Text("Music controls", style = TypeScale.body) },
                    leadingIcon = { Icon(Icons.Filled.MusicNote, contentDescription = null) },
                    onClick = { menu = false; onAdd(DashConfig.PageKind.MUSIC) },
                )
            }
            if (!hasWorkout) {
                DropdownMenuItem(
                    text = { Text("Workout", style = TypeScale.body) },
                    leadingIcon = {
                        Icon(Icons.Filled.DirectionsBike, contentDescription = null)
                    },
                    onClick = { menu = false; onAdd(DashConfig.PageKind.WORKOUT) },
                )
            }
        }
    }
}

@Composable
private fun MapStripSection(config: DashConfig, onPick: (Int, String) -> Unit) {
    TrackedLabel("Map data strip")
    listOf("Left cell", "Middle cell", "Right cell").forEachIndexed { slot, name ->
        var open by remember { mutableStateOf(false) }
        Card {
            Row(
                Modifier.clickable { open = true },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(name, style = TypeScale.body, color = Palette.ink, modifier = Modifier.weight(1f))
                Text(
                    DashField.named(config.mapFields[slot])?.label ?: config.mapFields[slot],
                    style = TypeScale.bodyStrong,
                    color = Palette.accent,
                )
            }
            DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                DashField.all.forEach { f ->
                    DropdownMenuItem(
                        text = { Text(f.label, style = TypeScale.body) },
                        onClick = { open = false; onPick(slot, f.id) },
                    )
                }
            }
        }
    }
    Text(
        "The three cells under the map. While navigating, Route left takes the right cell " +
            "unless one of them already shows it. Drag the map card to choose where it sits " +
            "in the Home-key cycle; it can't be removed.",
        style = barlow(12.sp),
        color = Palette.muted,
    )
}

@Composable
private fun MusicSection(ble: BleManager) {
    val context = LocalContext.current
    // Re-check the grant every time this screen comes back to the foreground —
    // the rider returns HERE from the notification-access switch, and the card
    // below must vanish (and the observers arm) the moment they do.
    val lifecycle = androidx.lifecycle.compose.LocalLifecycleOwner.current
    androidx.compose.runtime.DisposableEffect(lifecycle) {
        val obs = androidx.lifecycle.LifecycleEventObserver { _, event ->
            if (event == androidx.lifecycle.Lifecycle.Event.ON_RESUME) {
                ble.refreshMediaAccess()
            }
        }
        lifecycle.lifecycle.addObserver(obs)
        onDispose { lifecycle.lifecycle.removeObserver(obs) }
    }
    Card {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Filled.MusicNote, contentDescription = null, tint = Palette.ink)
            Spacer(Modifier.size(8.dp))
            Text("Music controls", style = TypeScale.bodyStrong, color = Palette.ink)
        }
        Text(
            "Shows what's playing on the phone — any app — with play/skip and volume. " +
                "Album art comes along when the player provides it.",
            style = TypeScale.body,
            color = Palette.muted,
        )
    }
    // Android gates media sessions behind notification access; only the rider
    // can flip that switch, so say exactly what to do and take them there.
    // (iOS asks its Media Library question by itself; this is our equivalent.)
    if (!ble.mediaAccessGranted) {
        Card {
            Text("Allow media access", style = TypeScale.bodyStrong, color = Palette.ink)
            Text(
                "To see what's playing, Android requires granting this app notification " +
                    "access. Nothing is read from your notifications — the permission is " +
                    "how Android exposes media sessions.",
                style = barlow(12.sp),
                color = Palette.muted,
            )
            TextButton(onClick = { context.startActivity(ble.mediaAccessIntent()) }) {
                Text("Open settings", style = TypeScale.bodyStrong, color = Palette.accent)
            }
        }
    }
}

/**
 * Springs a card to wherever the Row lays it out next — the Compose analogue
 * of the iOS drop delegate's withAnimation(.spring) reorder. `enabled = false`
 * snaps instead, for the card whose position IS the finger.
 */
private fun Modifier.animatePlacement(enabled: Boolean): Modifier = composed {
    val scope = rememberCoroutineScope()
    var target by remember { mutableStateOf<IntOffset?>(null) }
    var anim by remember { mutableStateOf<Animatable<IntOffset, AnimationVector2D>?>(null) }
    this
        .onPlaced { target = it.positionInParent().round() }
        .offset {
            val t = target ?: return@offset IntOffset.Zero
            val a = anim ?: Animatable(t, IntOffset.VectorConverter).also { anim = it }
            if (a.targetValue != t) {
                scope.launch {
                    if (enabled) a.animateTo(t, spring(stiffness = Spring.StiffnessMediumLow))
                    else a.snapTo(t)
                }
            }
            a.value - t
        }
}

/**
 * The field rows, reorderable by long-pressing the handle.
 *
 * A plain Column rather than a reorderable list library: the layout is capped at
 * 12 items, so there is nothing to virtualise, and the drag maths reduces to
 * "how many row heights has the finger travelled".
 */
@Composable
private fun ReorderableItems(
    items: List<DashItem>,
    onMove: (Int, Int) -> Unit,
    onChange: (Int, DashItem) -> Unit,
    onDelete: (Int) -> Unit,
) {
    var dragIndex by remember { mutableIntStateOf(-1) }
    var dragOffset by remember { mutableFloatStateOf(0f) }
    var rowHeight by remember { mutableFloatStateOf(1f) }

    Column {
        items.forEachIndexed { index, item ->
            val dragging = index == dragIndex
            Box(
                Modifier
                    .onSizeChanged { if (it.height > 0) rowHeight = it.height.toFloat() }
                    .offset { IntOffset(0, if (dragging) dragOffset.roundToInt() else 0) }
                    .alpha(if (dragging) 0.85f else 1f),
            ) {
                DashItemRow(
                    item = item,
                    onChange = { onChange(index, it) },
                    onDelete = { onDelete(index) },
                    dragModifier = Modifier.pointerInput(index, items.size) {
                        detectDragGesturesAfterLongPress(
                            onDragStart = { dragIndex = index; dragOffset = 0f },
                            onDragEnd = {
                                val shift = (dragOffset / rowHeight).roundToInt()
                                val target = (index + shift).coerceIn(0, items.size - 1)
                                if (target != index) onMove(index, target)
                                dragIndex = -1
                                dragOffset = 0f
                            },
                            onDragCancel = { dragIndex = -1; dragOffset = 0f },
                            onDrag = { change, amount ->
                                change.consume()
                                dragOffset += amount.y
                            },
                        )
                    },
                )
            }
        }
    }
}

/** One configurable row: field name, size, and whether it shares its row. */
@Composable
private fun DashItemRow(
    item: DashItem,
    onChange: (DashItem) -> Unit,
    onDelete: () -> Unit,
    dragModifier: Modifier,
) {
    Card(Modifier.padding(vertical = 4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(
                Icons.Filled.DragHandle,
                contentDescription = "Reorder",
                tint = Palette.faint,
                modifier = dragModifier.padding(end = 10.dp),
            )
            Text(
                item.fieldLabel,
                style = TypeScale.bodyStrong,
                color = Palette.ink,
                modifier = Modifier.weight(1f),
            )
            IconButton(onClick = onDelete) {
                Icon(Icons.Filled.Close, contentDescription = "Remove", tint = Palette.muted)
            }
        }
        Row(
            Modifier.padding(top = 6.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            DashSize.entries.forEach { size ->
                SmallChip(size.label, item.size == size) { onChange(item.copy(size = size)) }
            }
            Spacer(Modifier.weight(1f))
            SmallChip("Half", item.half) { onChange(item.copy(half = !item.half)) }
        }
    }
}

@Composable
private fun SmallChip(label: String, selected: Boolean, onClick: () -> Unit) {
    FilterChip(
        selected = selected,
        onClick = onClick,
        label = { Text(label, style = barlow(12.sp, FontWeight.SemiBold)) },
        shape = RoundedCornerShape(50),
        colors = FilterChipDefaults.filterChipColors(
            containerColor = Palette.paper,
            labelColor = Palette.muted,
            selectedContainerColor = Palette.accentWash,
            selectedLabelColor = Palette.accent,
        ),
    )
}

@Composable
private fun FieldPickerSheet(onDismiss: () -> Unit, onPick: (String) -> Unit) {
    FullScreenSheet(title = "Add a field", onDismiss = onDismiss, confirmLabel = "Cancel") {
        LazyColumn {
            items(DashField.all, key = { it.id }) { field ->
                Column(
                    Modifier
                        .fillMaxWidth()
                        .clickable { onPick(field.id) }
                        .padding(horizontal = 20.dp, vertical = 14.dp),
                ) {
                    Text(field.label, style = TypeScale.body, color = Palette.ink)
                    Text(field.detail, style = barlow(12.sp), color = Palette.muted)
                }
                HorizontalDivider(color = Palette.hairline)
            }
        }
    }
}
